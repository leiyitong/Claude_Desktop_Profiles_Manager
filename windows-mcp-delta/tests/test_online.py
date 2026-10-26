import asyncio
import base64
import hashlib
import json
import os
import sqlite3

import pytest

from windows_mcp_delta import caller, claude, cookies, mailbox, online
from windows_mcp_delta.tools import sessions

KEY = bytes(range(32))
ORG = "aaaaaaaa-1111-4111-8111-aaaaaaaaaaaa"
CHAT1 = "bbbbbbbb-2222-4222-8222-bbbbbbbbbbbb"
CHAT2 = "cccccccc-3333-4333-8333-cccccccccccc"


def seal(value: bytes, key=KEY, prefix=False) -> bytes:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    nonce = b"\x00" * 12
    plain = (hashlib.sha256(b"claude.ai").digest() + value) if prefix else value
    return b"v10" + nonce + AESGCM(key).encrypt(nonce, plain, None)


@pytest.fixture(autouse=True)
def home(tmp_path, monkeypatch):
    mailbox.reset()
    caller.reset()
    mailbox.set_root(tmp_path / "home" / "mailbox")
    mailbox.set_session("Claude")
    for name in ("WINDOWS_MCP_DELTA_ONLINE", "HTTP_PROXY", "HTTPS_PROXY"):
        monkeypatch.delenv(name, raising=False)
    yield tmp_path
    mailbox.reset()


# ----------------------------------------------------------------- gate

def test_enabled_gate(home, monkeypatch):
    assert online.enabled() is False
    (mailbox.root().parent / "online-enabled").write_text("")
    assert online.enabled() is True
    (mailbox.root().parent / "online-enabled").unlink()
    monkeypatch.setenv("WINDOWS_MCP_DELTA_ONLINE", "1")
    assert online.enabled() is True
    monkeypatch.setenv("WINDOWS_MCP_DELTA_ONLINE", "off")
    assert online.enabled() is False      # the variable wins over the flag file
    (mailbox.root().parent / "online-enabled").write_text("")
    assert online.enabled() is False


# -------------------------------------------------------------- cookies

def make_store(tmp_path, session_value=b"sk-ant-sid01-secret", prefix=False, with_key=True):
    storage = tmp_path / "profile"
    (storage / "Network").mkdir(parents=True, exist_ok=True)
    state = {"os_crypt": {"encrypted_key": base64.b64encode(KEY).decode()}} if with_key else {"os_crypt": {}}
    (storage / "Local State").write_text(json.dumps(state))
    db = storage / "Network" / "Cookies"
    db.unlink(missing_ok=True)
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE cookies (host_key TEXT, name TEXT, encrypted_value BLOB, value TEXT)")
    con.execute("INSERT INTO cookies VALUES (?,?,?,?)", (".claude.ai", "sessionKey", seal(session_value, prefix=prefix), ""))
    con.execute("INSERT INTO cookies VALUES (?,?,?,?)", ("claude.ai", "lastActiveOrg", seal(ORG.encode()), ""))
    con.execute("INSERT INTO cookies VALUES (?,?,?,?)", ("example.com", "other", seal(b"ignore"), ""))
    con.commit()
    con.close()
    return storage


def test_decrypt_value_forms():
    assert cookies.decrypt_value(seal(b"sk-ant-hello"), KEY) == "sk-ant-hello"
    assert cookies.decrypt_value(seal(b"sk-ant-hello", prefix=True), KEY) == "sk-ant-hello"   # 32-byte domain hash stripped
    assert cookies.decrypt_value(seal(b"x"), b"\x01" * 32) is None                            # wrong key
    assert cookies.decrypt_value(b"v10" + b"\x00" * 12 + b"junk", None) is None               # no key
    assert cookies.decrypt_value(b"", KEY) == ""


def test_read_cookies_and_account(home):
    storage = make_store(home)
    found = cookies._read_cookies(storage)
    assert found["sessionKey"] == "sk-ant-sid01-secret" and found["lastActiveOrg"] == ORG and "other" not in found

    class P:
        def __init__(self, s):
            self.storage, self.name = s, "Work"
    account = cookies.account_for(P(storage))
    assert account.session_key == "sk-ant-sid01-secret" and account.org == ORG and account.name == "Work"
    assert cookies.account_for(P(None)) is None
    assert cookies.account_for(P(make_store(home / "signedout", session_value=b""))) is None


def test_read_cookies_prefix_and_no_key(home):
    assert cookies._read_cookies(make_store(home / "a", prefix=True))["sessionKey"] == "sk-ant-sid01-secret"
    assert cookies._read_cookies(make_store(home / "b", with_key=False)).get("sessionKey") in (None, "")


def test_read_cookies_while_locked(home, monkeypatch):
    storage = make_store(home)
    real = sqlite3.connect

    def refuse_ro(target, *a, **k):
        if "mode=ro" in str(target):
            raise sqlite3.OperationalError("locked")
        return real(target, *a, **k)

    monkeypatch.setattr(sqlite3, "connect", refuse_ro)
    assert cookies._read_cookies(storage)["sessionKey"] == "sk-ant-sid01-secret"   # copied, then read


# ------------------------------------------------- the claude.ai client

class FakeResponse:
    def __init__(self, payload, status=200):
        self._payload, self.status_code = payload, status

    def json(self):
        return self._payload

    def raise_for_status(self):
        if self.status_code >= 400:
            raise online.requests.HTTPError(str(self.status_code))


def message(sender, text):
    return {"sender": sender, "content": [{"type": "text", "text": text}]}


def fake_api(monkeypatch, chats, bodies, record=None):
    def get(self, url, params=None, timeout=None):
        if record is not None:
            record.append((url, params))
        if url.endswith("/api/organizations"):
            return FakeResponse([{"uuid": ORG, "capabilities": ["chat", "claude_pro"]}])
        if url.endswith("/chat_conversations"):
            offset = (params or {}).get("offset", 0)
            return FakeResponse(chats[offset:offset + (params or {}).get("limit", 100)])
        for uuid, body in bodies.items():
            if url.endswith(uuid):
                return FakeResponse(body)
        return FakeResponse({}, 404)
    monkeypatch.setattr(online.requests.Session, "get", get)


def test_client_list_and_read(home, monkeypatch):
    chats = [{"uuid": CHAT1, "name": "Trip plan", "updated_at": "2026-10-20T10:00:00Z"},
             {"uuid": CHAT2, "name": "Budget", "updated_at": "2026-10-21T10:00:00Z"}]
    bodies = {CHAT1: {"uuid": CHAT1, "name": "Trip plan", "chat_messages": [
        message("human", "Plan a trip to Kyoto"), message("assistant", "Here is a 5-day plan."),
        {"sender": "assistant", "content": [{"type": "thinking", "thinking": "hmm"}, {"type": "tool_use", "name": "Search", "input": {"query": "kyoto"}}]}]}}
    record = []
    fake_api(monkeypatch, chats, bodies, record)
    client = online.Client(online.Account("sk", ""))
    listed = client.list_chats()
    assert [c.name for c in listed] == ["Trip plan", "Budget"] and client.account.org == ORG
    info, messages = client.read_chat(CHAT1)
    assert info.name == "Trip plan"
    assert [(m.sender, m.text) for m in messages] == [("User", "Plan a trip to Kyoto"), ("Claude", "Here is a 5-day plan."),
                                                      ("Claude", "[Search] kyoto")]
    assert any("sessionKey=sk" == client.session.headers["Cookie"] for _ in [0])


def test_client_unauthorized(home, monkeypatch):
    monkeypatch.setattr(online.requests.Session, "get", lambda self, url, **k: FakeResponse({}, 401))
    with pytest.raises(PermissionError):
        online.Client(online.Account("bad", ORG)).list_chats()


def test_search_chats(home, monkeypatch):
    chats = [{"uuid": CHAT1, "name": "Trip plan", "updated_at": "2026-10-20T10:00:00Z"},
             {"uuid": CHAT2, "name": "Budget", "updated_at": "2026-10-21T10:00:00Z"}]
    bodies = {CHAT1: {"uuid": CHAT1, "name": "Trip plan", "chat_messages": [message("assistant", "The zlib flag was missing.")]},
              CHAT2: {"uuid": CHAT2, "name": "Budget", "chat_messages": [message("human", "unrelated")]}}
    fake_api(monkeypatch, chats, bodies)
    client = online.Client(online.Account("sk", ORG))
    found = list(online.search_chats(client, "zlib", False, False, 10, 40))
    assert len(found) == 1 and found[0][0].uuid == CHAT1 and found[0][1] == 1


# --------------------------------------------------- the session tools

def install(home, monkeypatch, enabled=True):
    storage = make_store(home)

    def account_for(profile):
        return online.Account("sk", ORG, getattr(profile, "name", ""))

    monkeypatch.setattr(sessions.cookies, "account_for", account_for)
    monkeypatch.setattr(sessions.claude, "profiles", lambda: [claude.Profile("Claude", "Main", home, storage, True, "acc")])
    flag = mailbox.root().parent / "online-enabled"
    if enabled:
        flag.write_text("")
    else:
        flag.unlink(missing_ok=True)


def test_readsession_chat_off_then_on(home, monkeypatch):
    install(home, monkeypatch, enabled=False)
    out = asyncio.run(sessions.read_session(CHAT1))
    assert out == online.OFF
    install(home, monkeypatch, enabled=True)
    fake_api(monkeypatch, [], {CHAT1: {"uuid": CHAT1, "name": "Trip", "chat_messages": [
        message("human", "hi"), message("assistant", "hello")]}})
    out = asyncio.run(sessions.read_session(CHAT1, count=5))
    assert "Trip" in out and "(chat on claude.ai)" in out and "[1] User: hi" in out and "[2] Claude: hello" in out


def test_listsessions_chat_online(home, monkeypatch):
    install(home, monkeypatch)
    fake_api(monkeypatch, [{"uuid": CHAT1, "name": "Trip plan", "updated_at": "2026-10-20T10:00:00Z"}], {})
    out = sessions.list_sessions(kind="chat")
    assert "Chat conversations of Main (1" in out and "Trip plan" in out and CHAT1[:8] in out


def test_listsessions_chat_off_lists_seen(home, monkeypatch):
    install(home, monkeypatch, enabled=False)
    caller._remember(CHAT1, "Seen one")
    out = sessions.list_sessions(kind="chat")
    assert "turn on online access" in out and "Seen one" in out


def test_searchsessions_chats(home, monkeypatch):
    install(home, monkeypatch)
    chats = [{"uuid": CHAT1, "name": "Trip", "updated_at": "2026-10-20T10:00:00Z"}]
    fake_api(monkeypatch, chats, {CHAT1: {"uuid": CHAT1, "name": "Trip", "chat_messages": [message("assistant", "the kyoto itinerary")]}})
    out = asyncio.run(sessions.search_sessions("kyoto", chats=True))
    assert "Found in 1 chat of Main" in out and "the kyoto itinerary" in out
    install(home, monkeypatch, enabled=False)
    assert online.OFF in asyncio.run(sessions.search_sessions("kyoto", chats=True))
