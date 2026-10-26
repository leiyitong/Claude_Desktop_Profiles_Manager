"""Who calls a tool, and the conversations that have.

Claude Desktop sends each tool call with its conversation in the request's
_meta (anthropic/conversationUuid, anthropic/conversationName); Claude Code
sends claudecode/toolUseId, and its session is the one among this server's
parents (claude.py). A middleware notes the caller of every call
(`current()` while the call runs), and keeps each conversation seen in this
profile's file, conversations/<profile>.json beside the mailbox: one file
per profile, as one server writes each, so that every profile can list the
others' conversations and open them by id.
"""
from __future__ import annotations

import contextvars
import json
import os
import tempfile
import threading
import time

from fastmcp.server.middleware import Middleware

from windows_mcp_delta import mailbox

CONVERSATION = "anthropic/conversationUuid"
CONVERSATION_NAME = "anthropic/conversationName"
CODE_TOOL_USE = "claudecode/toolUseId"
NOTE_EVERY = 60.0      # a conversation's last use is written again at most this often

_current: contextvars.ContextVar[dict | None] = contextvars.ContextVar("caller", default=None)
_lock = threading.Lock()
_written: dict[str, tuple[str, float]] = {}


def current() -> dict:
    """The caller of the tool call under way: {"conversation", "name", "client", "code"}, each "" when not known."""
    return _current.get() or {"conversation": "", "name": "", "client": "", "code": ""}


def conversation() -> str:
    return current()["conversation"]


def _folder():
    folder = mailbox.root().parent / "conversations"
    folder.mkdir(parents=True, exist_ok=True)
    return folder


def seen(profile: str | None = None) -> dict[str, dict]:
    """The conversations seen: id to {"profile", "name", "first", "last"}; of one profile's folder, or of every one."""
    out: dict[str, dict] = {}
    files = [_folder() / f"{mailbox._safe(profile)}.json"] if profile else sorted(_folder().glob("*.json"))
    for file in files:
        try:
            data = json.loads(file.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        if isinstance(data, dict):
            for key, value in data.items():
                if isinstance(value, dict) and (key not in out or value.get("last", 0) > out[key].get("last", 0)):
                    out[key] = {**value, "profile": value.get("profile") or file.stem}
    return out


def note(meta: dict | None, client: str = "") -> dict:
    """The caller `meta` (a request's _meta) names, kept as the current one and its conversation noted."""
    meta = meta if isinstance(meta, dict) else {}
    conversation = meta.get(CONVERSATION)
    name = meta.get(CONVERSATION_NAME)
    who = {
        "conversation": conversation if isinstance(conversation, str) else "",
        "name": name if isinstance(name, str) else "",
        "client": client,
        "code": "yes" if isinstance(meta.get(CODE_TOOL_USE), str) else "",
    }
    _current.set(who)
    if who["conversation"]:
        _remember(who["conversation"], who["name"])
    return who


def _remember(conversation: str, name: str) -> None:
    now = time.time()
    with _lock:
        last = _written.get(conversation)
        if last and last[0] == name and now - last[1] < NOTE_EVERY:
            return
        me = mailbox.session_id()
        path = _folder() / f"{me}.json"
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            data = data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            data = {}
        entry = data.get(conversation) if isinstance(data.get(conversation), dict) else {"first": now}
        entry.update({"profile": me, "last": now})
        if name:
            entry["name"] = name
        data[conversation] = entry
        handle, temporary = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
        with os.fdopen(handle, "w", encoding="utf-8") as file:
            json.dump(data, file, ensure_ascii=False, indent=1)
        os.replace(temporary, path)
        _written[conversation] = (name, now)


def reset() -> None:
    """Back to the defaults (tests)."""
    with _lock:
        _written.clear()
    _current.set(None)


class Caller(Middleware):
    """Each tool call's caller noted before the tool runs."""

    async def on_call_tool(self, context, call_next):
        meta, client = None, ""
        try:
            request = context.fastmcp_context.request_context if context.fastmcp_context else None
            meta = getattr(request, "meta", None)
            info = meta.get("io.modelcontextprotocol/clientInfo") if isinstance(meta, dict) else None
            client = info.get("name", "") if isinstance(info, dict) else ""
            if not client:
                params = getattr(getattr(context.fastmcp_context, "session", None), "client_params", None)
                client = getattr(getattr(params, "clientInfo", None), "name", "") or ""
        except Exception:
            pass
        try:
            note(meta, client)
        except OSError:
            pass
        return await call_next(context)
