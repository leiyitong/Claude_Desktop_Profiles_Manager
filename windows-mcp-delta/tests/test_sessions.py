import asyncio
import json
import os
import time

import pytest
from fastmcp import Client, FastMCP

from windows_mcp_delta import caller, claude, mailbox
import windows_mcp_delta.extend as extend
from windows_mcp_delta.tools import messages, sessions

A1 = "11111111-1111-4111-8111-111111111111"   # transcripts
A2 = "22222222-2222-4222-8222-222222222222"
A3 = "33333333-3333-4333-8333-333333333333"
OLD = "44444444-4444-4444-8444-444444444444"
CHAT = "55555555-5555-4555-8555-555555555555"


def line(kind, content, **extra):
    return json.dumps({"type": kind, "message": {"role": kind, "content": content}, "timestamp": "2026-10-24T10:00:00Z", **extra})


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def entry(folder, account, org, sid, **fields):
    write(folder / claude.ENTRIES / account / org / f"local_{sid[:8]}.json", json.dumps({"sessionId": f"local_{sid[:8]}", **fields}))


@pytest.fixture
def pc(tmp_path, monkeypatch):
    roaming, local, code = tmp_path / "Roaming", tmp_path / "Local", tmp_path / "dotclaude"
    for name, value in (("APPDATA", roaming), ("LOCALAPPDATA", local), ("CLAUDE_CONFIG_DIR", code)):
        monkeypatch.setenv(name, str(value))
    monkeypatch.delenv("WINDOWS_MCP_DELTA_MANAGER", raising=False)
    mailbox.reset()
    caller.reset()
    mailbox.set_root(tmp_path / "home" / "mailbox")
    mailbox.set_session("Claude")
    stock, work = roaming / "Claude", roaming / "Claude-Work"
    write(stock / "config.json", json.dumps({"lastKnownAccountUuid": "acc-main"}))
    write(work / "Local State", "{}")
    write(work / "config.json", json.dumps({"lastKnownAccountUuid": "acc-work"}))
    (roaming / "Claude-Data").mkdir()            # Claude's own, not a profile
    now = time.time() * 1000
    entry(stock, "acc-main", "org", A1, cliSessionId=A1, title="Fix the build", cwd=r"C:\src\app", lastActivityAt=now - 3600e3, isStarred=True)
    entry(work, "acc-work", "org", A1, cliSessionId=A1, title="Fix the build", cwd=r"C:\src\app", lastActivityAt=now - 7200e3)
    entry(work, "acc-work", "org", A2, cliSessionId=A2, title="Translate the docs", cwd=r"C:\docs", lastActivityAt=now - 60e3,
          priorCliSessionIds=[OLD])
    entry(work, "acc-other", "org", A3, cliSessionId=A3, title="Not shown: another account", lastActivityAt=now)
    project = code / "projects" / "C--src-app"
    write(project / f"{A1}.jsonl", "\n".join([
        line("user", "Please fix the failing build in core.c"),
        line("assistant", [{"type": "text", "text": "Looking at the build."}, {"type": "tool_use", "name": "Bash", "input": {"command": "make test"}}]),
        line("user", [{"type": "tool_result", "content": "ok"}]),
        line("user", "<command-name>/clear</command-name>"),
        line("assistant", [{"type": "thinking", "thinking": "hidden"}]),
        line("assistant", "Side work", isSidechain=True),
        line("assistant", [{"type": "text", "text": "The build passes now: the zlib flag was missing."}]),
    ]))
    write(project / f"{A2}.jsonl", "\n".join([line("user", f"Translate message {i} into Chinese") for i in range(1, 51)]))
    write(project / f"{OLD}.jsonl", line("user", "the earlier part"))
    write(code / "projects" / "C--term" / f"{A3[:-1]}9.jsonl", line("user", "Run on its own in a terminal\nsecond line"))
    return tmp_path


def test_profiles_and_rows(pc):
    every = claude.profiles()
    assert [(p.folder, p.name, p.account) for p in every] == [("Claude", "Main", "acc-main"), ("Claude-Work", "Work", "acc-work")]
    rows = claude.sessions(every)
    by_id = {r.id: r for r in rows}
    assert [r.id for r in rows if r.profiles] == [A2, A1]           # the latest first
    assert sorted(by_id[A1].profiles) == ["Claude", "Claude-Work"] and by_id[A1].starred
    assert by_id[A2].prior == [OLD] and OLD not in by_id          # a transcript it went on from is part of it
    assert A3 not in by_id                                         # another account's entries are not shown
    terminal = [r for r in rows if not r.profiles]
    assert len(terminal) == 1 and terminal[0].title == "Run on its own in a terminal"
    assert claude.find_session(A1[:6], rows).id == A1
    assert claude.find_profile("work", every).folder == "Claude-Work" and claude.find_profile("Main", every).stock


def test_messages_show_what_the_preview_shows(pc):
    path = claude.find_session(A1).path
    said = [(r, t) for r, t, _ in claude.messages(path)]
    assert said == [("User", "Please fix the failing build in core.c"),
                    ("Claude", "Looking at the build.\n[Bash] make test"),
                    ("Claude", "The build passes now: the zlib flag was missing.")]


def test_list_and_profiles_tools(pc):
    out = sessions.list_sessions()
    assert "Code sessions (3)" in out and "\u201cFix the build\u201d · Main, Work" in out and "★" in out
    assert "Translate the docs" in sessions.list_sessions(profile="Work", kind="code")
    assert "Fix the build" not in sessions.list_sessions(query="docs", kind="code")
    assert sessions.list_sessions(profile="Nobody").startswith("Error: no profile")
    listing = sessions.list_profiles()
    assert "Claude · Main · account acc-main" in listing and "Claude-Work · Work" in listing and "this one" in listing


def test_search_across_accounts(pc):
    out = asyncio.run(sessions.search_sessions("ZLIB flag"))
    assert "Found in 1 Code session" in out and "#3 Claude: " in out and "zlib flag" in out
    out = asyncio.run(sessions.search_sessions(r"message \d+ into", regex=True, limit=5))
    assert "Translate the docs" in out and "50 messages" in out
    assert asyncio.run(sessions.search_sessions("make test")).count("#2 Claude: ") == 1   # tool calls are searched
    assert asyncio.run(sessions.search_sessions("nowhere at all")).startswith("Not found in 3 Code sessions")
    assert asyncio.run(sessions.search_sessions("(", regex=True)).startswith("Error")


def test_read_pages(pc):
    out = asyncio.run(sessions.read_session(A2[:8], count=5))
    assert "[46] User: Translate message 46" in out and "[50]" in out and "[45]" not in out
    assert "Messages 46–50 of 50. Earlier: ReadSession(id, start=41)." in out and "Goes on from: " + OLD in out
    out = asyncio.run(sessions.read_session(A2, start=1, count=2))
    assert "[1] User" in out and "Later: ReadSession(id, start=3)" in out
    out = asyncio.run(sessions.read_session(A2, start=-3, count=2))
    assert "[48]" in out and "[49]" in out and "[50]" not in out
    assert asyncio.run(sessions.read_session("zzzz")).startswith("Error: no Code session")


def test_open_session_links(pc, monkeypatch):
    opened = []
    monkeypatch.setattr(claude, "open_link", lambda url, profile: opened.append((url, profile and profile.folder)) or "in X")
    assert sessions.open_session(id=A2[:8]).startswith("Opened Code session")
    assert opened[-1] == (f"claude://resume?session={A2}", "Claude")           # goes on here by default
    sessions.open_session(id=A1, profile="Work")
    assert opened[-1] == (f"claude://resume?session={A1}", "Claude-Work")
    caller._remember(CHAT, "Plan the trip")                                     # a chat seen in Claude (this profile)
    sessions.open_session(conversation=CHAT[:8])
    assert opened[-1] == (f"claude://claude.ai/chat/{CHAT}", "Claude")
    sessions.open_session(new_chat="hello & bye", profile="Work")
    assert opened[-1] == ("claude://claude.ai/new?q=hello%20%26%20bye", "Claude-Work")
    assert sessions.open_session().startswith("Error: give one")


def test_running_session_is_not_opened_twice(pc, monkeypatch):
    sessions_dir = pc / "dotclaude" / "sessions"
    write(sessions_dir / f"{os.getpid()}.json", json.dumps({"sessionId": A1}))
    monkeypatch.setattr(claude, "open_link", lambda url, profile: "x")
    assert "runs now" in sessions.open_session(id=A1)
    assert sessions.open_session(id=A1, force=True).startswith("Opened")
    assert "running (pid" in sessions.list_sessions(running=True)


def test_launch_through_claude_delta(pc, monkeypatch, tmp_path):
    program = tmp_path / "ClaudeDelta.exe"
    program.write_text("")
    monkeypatch.setenv("WINDOWS_MCP_DELTA_MANAGER", str(program))
    started = []
    monkeypatch.setattr(claude.subprocess, "Popen", lambda args, **kw: started.append(args))
    work = claude.find_profile("Work")
    assert claude.open_link("claude://resume?session=" + A1, work) == "in Work"
    assert started[-1] == [str(program), "--launch", "Claude-Work", "claude://resume?session=" + A1]
    with pytest.raises(ValueError):
        claude.open_link("https://evil.example", work)


def test_messages_between_profiles_and_conversations(pc):
    caller._remember(CHAT, "Plan the trip")        # noted by Claude's server
    assert "Sent to Claude-Work." == messages.send_message("Work", "build is green")
    mailbox.set_session("Claude-Work")
    assert "build is green" in messages.inbox(keep=True) and "[from Claude" in messages.inbox()
    assert messages.inbox() == "No messages."
    # a reply to one conversation: only it takes it
    assert messages.send_message(CHAT[:8], "for the trip chat").startswith("Sent to conversation 55555555 of Claude")
    mailbox.set_session("Claude")
    caller.note({caller.CONVERSATION: "66666666-6666-4666-8666-666666666666"})
    assert messages.inbox() == "No messages."      # another conversation leaves it
    caller.note({caller.CONVERSATION: CHAT})
    assert "for the trip chat" in messages.inbox()
    assert messages.send_message("all", "hi").startswith("Sent to 1 profiles")
    assert messages.send_message("nobody", "x").startswith("Error")


def test_caller_middleware_sees_the_conversation(pc):
    mcp = FastMCP(name="windows-mcp")
    fresh = extend.Delta()
    fresh._entries = [e for e in extend.delta._entries if e.name in ("WhoAmI", "ListSessions")]
    fresh.use(caller.Caller())
    fresh.apply(mcp)

    async def go():
        async with Client(mcp) as client:
            who = await client.call_tool("WhoAmI", {}, meta={caller.CONVERSATION: CHAT, caller.CONVERSATION_NAME: "Plan the trip"})
            chats = await client.call_tool("ListSessions", {"kind": "chat"})
            return who.content[0].text, chats.content[0].text

    who, chats = asyncio.run(go())
    assert f"Conversation: {CHAT} \u201cPlan the trip\u201d" in who and "Profile: Main (Claude) · account acc-main" in who
    assert "Other profiles: Work (Claude-Work)" in who
    assert "55555555 \u201cPlan the trip\u201d · Main" in chats
