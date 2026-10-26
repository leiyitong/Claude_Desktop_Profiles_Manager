"""A filesystem mailbox so the Claude Desktop sessions on this PC — one
windows-mcp-delta each — can leave messages for one another.

Each profile has an id (its folder, Claude-Work, as the Claude Desktop that
started us names it, or `$WINDOWS_MCP_DELTA_SESSION`) and an inbox
directory. Another one drops a small JSON file into it; `Idle` watches the
inbox and takes the messages when it wakes, and `Inbox` reads it. One
process reads its own inbox, so a message is delivered (read, then removed)
exactly once. A message can be for one conversation of the profile only
(its "conversation"): the others leave it. SendMessage writes through here.
"""
from __future__ import annotations

import getpass
import json
import os
import socket
import tempfile
import time
import uuid
from pathlib import Path

_root_override: Path | None = None
_session_override: str | None = None


def _safe(name: str) -> str:
    keep = "".join(c if (c.isalnum() or c in "-_.@") else "_" for c in name)
    return keep[:64] or "session"


def _home() -> Path:
    base = os.environ.get("WINDOWS_MCP_DELTA_HOME")
    return Path(base) if base else Path(os.path.expanduser("~")) / ".windows-mcp-delta"


def root() -> Path:
    r = _root_override or (_home() / "mailbox")
    r.mkdir(parents=True, exist_ok=True)
    return r


def set_root(path) -> None:
    """The mailbox folder for tests, or for Claude Delta to point at its state folder; None restores the default."""
    global _root_override
    _root_override = Path(path) if path else None


_detected: str | None = None


def session_id() -> str:
    """This profile's own id: `$WINDOWS_MCP_DELTA_SESSION`, else the folder of the Claude Desktop profile that
    started us, else user@host."""
    global _detected
    if _session_override:
        return _session_override
    env = os.environ.get("WINDOWS_MCP_DELTA_SESSION")
    if env:
        return _safe(env)
    if _detected is None:
        try:
            from windows_mcp_delta import claude
            folder = claude.profile_of()
        except Exception:
            folder = None
        try:
            _detected = _safe(folder or f"{getpass.getuser()}@{socket.gethostname()}")
        except Exception:
            _detected = "session"
    return _detected


def set_session(name) -> None:
    """Override this session's id (tests, or Claude Delta naming the profile); None restores the default."""
    global _session_override
    _session_override = _safe(name) if name else None


def inbox(of: str | None = None) -> Path:
    box = root() / (_safe(of) if of else session_id())
    box.mkdir(parents=True, exist_ok=True)
    return box


def post(target: str, text: str, sender: str | None = None, kind: str = "message", data=None,
         conversation: str | None = None, from_conversation: str | None = None) -> str:
    """A message left in `target`'s inbox (for its `conversation` only, when given); its id. Written whole (a temp
    file renamed in), so a reader never sees half."""
    message = {
        "id": uuid.uuid4().hex,
        "at": time.time(),
        "from": _safe(sender) if sender else session_id(),
        "to": _safe(target),
        "kind": kind,
        "text": text,
    }
    if conversation:
        message["conversation"] = conversation
    if from_conversation:
        message["from_conversation"] = from_conversation
    if data is not None:
        message["data"] = data
    box = inbox(target)
    handle, temporary = tempfile.mkstemp(dir=box, suffix=".tmp")
    with os.fdopen(handle, "w", encoding="utf-8") as file:
        json.dump(message, file)
    os.replace(temporary, box / f"{message['at']:017.6f}-{message['id']}.json")
    return message["id"]


def _files(of=None):
    """This inbox's message files (or `of`'s), oldest first (the time is in the name); an unreadable one is dropped."""
    out = []
    for path in sorted(inbox(of).glob("*.json")):
        try:
            out.append((path, json.loads(path.read_text(encoding="utf-8"))))
        except (OSError, ValueError):
            try:
                path.unlink()
            except OSError:
                pass
    return out


def _matches(message, from_, kind, contains, conversation=None) -> bool:
    for_one = message.get("conversation")
    if for_one and conversation and for_one != conversation:
        return False   # for another conversation of this profile
    if from_ and _safe(from_) != message.get("from"):
        return False
    if kind and kind != message.get("kind"):
        return False
    if contains and contains not in (message.get("text") or ""):
        return False
    return True


def peek(from_=None, kind=None, contains=None, conversation=None, of=None):
    """The matching messages waiting (in `of`'s inbox: another profile's), without removing them. `conversation`:
    the reader's, which leaves out those for another conversation."""
    return [m for _, m in _files(of) if _matches(m, from_, kind, contains, conversation)]


def take(from_=None, kind=None, contains=None, conversation=None):
    """The matching messages, removed as they are returned (delivered once)."""
    taken = []
    for path, message in _files():
        if not _matches(message, from_, kind, contains, conversation):
            continue
        try:
            path.unlink()
        except OSError:
            continue   # another reader took it
        taken.append(message)
    return taken


def describe(messages: list[dict], limit: int = 20, text_max: int = 4000) -> str:
    """Messages as a tool returns them: who from (and which conversation, to reply to), how long ago, the text."""
    lines = []
    for m in messages[:limit]:
        age = int(time.time() - m.get("at", time.time()))
        sender = m.get("from") or "?"
        if m.get("from_conversation"):
            sender += f" · conversation {m['from_conversation']}"
        text = m.get("text") or ""
        if len(text) > text_max:
            text = text[:text_max] + f" … ({len(text) - text_max} more characters)"
        kind = "" if m.get("kind", "message") == "message" else f" · {m['kind']}"
        lines.append(f"[from {sender}{kind} · {age}s ago] {text}")
    if len(messages) > limit:
        lines.append(f"... and {len(messages) - limit} more")
    return "\n".join(lines)


def reset() -> None:
    """Back to the defaults (tests)."""
    global _root_override, _session_override, _detected
    _root_override = None
    _session_override = None
    _detected = None
