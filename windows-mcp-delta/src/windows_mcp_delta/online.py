"""Reading and searching a profile's chat conversations on claude.ai.

A Chat conversation lives on claude.ai, not on this PC, so reading or
searching one means talking to claude.ai as that profile is signed in — with
its session (cookies.py reads it from the profile's own cookie store). This
reaches the whole account, so it is off until the user turns it on: `enabled`
is false unless Claude Delta has set WINDOWS_MCP_DELTA_ONLINE or the user has
made the flag file, and a tool that needs it returns `OFF` for the agent to
show instead. The endpoints are claude.ai's web API (/api/organizations/.../
chat_conversations); nothing is written, only read.
"""
from __future__ import annotations

import time
from dataclasses import dataclass, field
from typing import Iterator

import requests

from windows_mcp_delta import mailbox

BASE = "https://claude.ai"
TIMEOUT = 30
LIST_PAGE = 100
AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
         "Chrome/130.0.0.0 Safari/537.36")
OFF = ("Online access to claude.ai is off, so chat conversations cannot be read or searched (only listed and opened). "
       "Ask the user to turn it on in Claude Delta → Settings → Windows MCP → “Read chats online” "
       "(it reads the signed-in account's chats through its own claude.ai session; nothing is written).")


def enabled() -> bool:
    """Whether reading claude.ai is allowed: Claude Delta set the variable, or the user made the flag file."""
    value = (_env() or "").strip().lower()
    if value in ("1", "true", "yes", "on"):
        return True
    if value in ("0", "false", "no", "off"):
        return False
    return (mailbox.root().parent / "online-enabled").exists()


def _env() -> str | None:
    import os
    return os.environ.get("WINDOWS_MCP_DELTA_ONLINE")


@dataclass
class Chat:
    uuid: str
    name: str = ""
    summary: str = ""
    created: float = 0.0
    updated: float = 0.0
    model: str = ""
    org: str = ""


@dataclass
class ChatMessage:
    sender: str                 # "User" or "Claude"
    text: str
    created: str = ""


@dataclass
class Account:
    """A profile's claude.ai session, enough to talk to it."""
    session_key: str
    org: str = ""
    name: str = ""              # the profile this is for, for messages


def _when(value) -> float:
    if isinstance(value, (int, float)):
        return float(value)
    if isinstance(value, str) and value:
        try:
            import datetime
            return datetime.datetime.fromisoformat(value.replace("Z", "+00:00")).timestamp()
        except ValueError:
            return 0.0
    return 0.0


class Client:
    """claude.ai as one profile is signed in. Read only."""

    def __init__(self, account: Account) -> None:
        self.account = account
        self.session = requests.Session()
        self.session.headers.update({
            "User-Agent": AGENT,
            "Accept": "application/json",
            "Cookie": f"sessionKey={account.session_key}",
            "anthropic-client-platform": "web_claude_ai",
        })

    def _get(self, path: str, **params):
        response = self.session.get(BASE + path, params=params or None, timeout=TIMEOUT)
        if response.status_code in (401, 403):
            raise PermissionError("claude.ai did not accept the session (signed out, or it expired): sign in again "
                                  "in that profile.")
        response.raise_for_status()
        return response.json()

    def organization(self) -> str:
        """The organization to use: the one the session names, else the first personal account."""
        if self.account.org:
            return self.account.org
        orgs = self._get("/api/organizations")
        chosen = ""
        for org in orgs if isinstance(orgs, list) else []:
            uuid = org.get("uuid")
            if not uuid:
                continue
            capabilities = org.get("capabilities") or []
            if "chat" in capabilities and ("claude_pro" in capabilities or not chosen):
                chosen = uuid
            chosen = chosen or uuid
        if not chosen:
            raise RuntimeError("claude.ai returned no organization for this account.")
        self.account.org = chosen
        return chosen

    def list_chats(self, limit: int = LIST_PAGE) -> list[Chat]:
        org = self.organization()
        out: list[Chat] = []
        offset = 0
        while len(out) < limit:
            page = self._get(f"/api/organizations/{org}/chat_conversations", limit=min(LIST_PAGE, limit - len(out)), offset=offset)
            rows = page if isinstance(page, list) else page.get("conversations", [])
            if not rows:
                break
            for row in rows:
                out.append(Chat(row.get("uuid", ""), (row.get("name") or "").strip(), (row.get("summary") or "").strip(),
                                _when(row.get("created_at")), _when(row.get("updated_at")),
                                (row.get("model") or ""), org))
            if len(rows) < LIST_PAGE:
                break
            offset += len(rows)
        return out

    def read_chat(self, uuid: str) -> tuple[Chat, list[ChatMessage]]:
        org = self.organization()
        data = self._get(f"/api/organizations/{org}/chat_conversations/{uuid}", tree="True", rendering_mode="messages")
        chat = Chat(data.get("uuid", uuid), (data.get("name") or "").strip(), (data.get("summary") or "").strip(),
                    _when(data.get("created_at")), _when(data.get("updated_at")), (data.get("model") or ""), org)
        messages = [m for m in (_message(raw) for raw in data.get("chat_messages", []) or []) if m]
        return chat, messages


def _message(raw: dict) -> ChatMessage | None:
    if not isinstance(raw, dict):
        return None
    who = "User" if raw.get("sender") == "human" else "Claude"
    parts: list[str] = []
    content = raw.get("content")
    if isinstance(content, list):
        for block in content:
            if not isinstance(block, dict):
                continue
            kind = block.get("type")
            if kind == "text" and block.get("text"):
                parts.append(block["text"].strip())
            elif kind == "tool_use":
                name = block.get("name", "tool")
                given = block.get("input") if isinstance(block.get("input"), dict) else {}
                hint = next((str(v) for v in given.values() if isinstance(v, str) and v.strip()), "")
                parts.append(f"[{name}] {hint}".strip())
            elif kind == "thinking":
                continue
    if not parts and isinstance(raw.get("text"), str):
        parts.append(raw["text"].strip())
    text = "\n".join(p for p in parts if p)
    return ChatMessage(who, text, raw.get("created_at") or "") if text else None


def search_chats(client: Client, text: str, regex: bool, match_case: bool, limit: int, budget: int) -> Iterator[tuple[Chat, int, list[tuple[int, str, str]]]]:
    """Chats whose messages hold `text`, read one by one until `limit` are found or `budget` are read (claude.ai has
    no search of one's own chats, so each is fetched and scanned). Each: (chat, how many messages, first excerpts)."""
    import re
    pattern = re.compile(text if regex else re.escape(text), 0 if match_case else re.IGNORECASE)
    found = 0
    for read, chat in enumerate(client.list_chats(budget)):
        if found >= limit or read >= budget:
            return
        quick = pattern.search(chat.name) or pattern.search(chat.summary)
        try:
            _, messages = client.read_chat(chat.uuid)
        except (requests.RequestException, PermissionError, RuntimeError):
            continue
        hits, excerpts = 0, []
        for number, message in enumerate(messages, 1):
            match = pattern.search(message.text)
            if not match:
                continue
            hits += 1
            if len(excerpts) < 2:
                start, end = max(0, match.start() - 70), min(len(message.text), match.end() + 70)
                excerpt = " ".join(message.text[start:end].split())
                excerpts.append((number, message.sender, ("…" if start else "") + excerpt + ("…" if end < len(message.text) else "")))
        if hits or quick:
            found += 1
            yield chat, hits, excerpts
