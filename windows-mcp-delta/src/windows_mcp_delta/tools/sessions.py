"""WhoAmI, Profiles, ListSessions, SearchSessions, ReadSession, OpenSession:
the sessions of every account on this PC (claude.py reads them).

Claude Code sessions are on this PC, whichever account ran them: they are
listed, searched in full and read here, and OpenSession goes on with one in
any profile (Claude Desktop's claude://resume link, through Claude Delta).
Chat conversations are kept by claude.ai: those that have called these
tools are listed (caller.py notes them), and OpenSession opens any of them,
or a new chat with a prompt, in its profile. Ids may be shortened to any
start that only one session has.
"""
from __future__ import annotations

import asyncio
import datetime
import time
from typing import Annotated, Literal

from mcp.types import ToolAnnotations
from pydantic import Field

from windows_mcp_delta import caller, claude, cookies, mailbox, online
from windows_mcp_delta.extend import delta

SEARCH_SECONDS = 90     # a search stops after this long, saying how far it went
SHORT = 8               # characters of an id shown


def _ago(seconds_since_1970: float) -> str:
    if not seconds_since_1970:
        return "?"
    gone = max(0, time.time() - seconds_since_1970)
    for size, unit in ((86400, "d"), (3600, "h"), (60, "m")):
        if gone >= size:
            return f"{int(gone // size)}{unit} ago"
    return "just now"


def _when(seconds_since_1970: float) -> str:
    return datetime.datetime.fromtimestamp(seconds_since_1970).strftime("%Y-%m-%d %H:%M") if seconds_since_1970 else "?"


def _size(count: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if count < 1024 or unit == "GB":
            return f"{count:.0f} {unit}" if unit == "B" else f"{count:.1f} {unit}"
        count /= 1024
    return ""


def _names(folders: list[str], every: list[claude.Profile]) -> str:
    by_folder = {p.folder: p.name for p in every}
    return ", ".join(by_folder.get(f, f) for f in folders) or "no profile (Claude Code on its own)"


def _session_line(row: claude.Session, every: list[claude.Profile]) -> str:
    marks = (" ★" if row.starred else "") + (" · archived" if row.archived else "") + \
        (f" · running (pid {row.running})" if row.running else "") + ("" if row.path else " · not on this PC")
    folder = f" · {row.cwd}" if row.cwd else ""
    return f"{row.id[:SHORT]} “{row.title or '(untitled)'}” · {_names(row.profiles, every)}{folder} · " \
           f"{_ago(row.last)} · {_size(row.bytes)}{marks}"


def _filter(rows: list[claude.Session], profile: claude.Profile | None, archived: bool) -> list[claude.Session]:
    return [r for r in rows if (archived or not r.archived) and (profile is None or profile.folder in r.profiles)]


def _profile_arg(name: str | None, every: list[claude.Profile]) -> claude.Profile | None | str:
    """The profile `name` names; None for every one; an error's text when it names none."""
    if not name or name.strip().lower() in ("all", "*"):
        return None
    found = claude.find_profile(name, every)
    return found or f"Error: no profile {name!r}. Profiles: {', '.join(f'{p.name} ({p.folder})' for p in every)}."


def _find_chat(key: str) -> tuple[str, dict] | None:
    key = key.strip().lower()
    chats = caller.seen()
    if key in chats:
        return key, chats[key]
    starts = [k for k in chats if k.lower().startswith(key)] if len(key) >= 4 else []
    return (starts[0], chats[starts[0]]) if len(starts) == 1 else None


def _client(profile: claude.Profile | None, every: list[claude.Profile]) -> tuple[online.Client | None, str]:
    """A claude.ai client for `profile` (the current one when None), and why there is none when there is not."""
    target = profile or claude.find_profile(mailbox.session_id(), every)
    if target is None:
        return None, "which profile (account) to read is not known: name one."
    account = cookies.account_for(target)
    if account is None:
        return None, f"{target.name} is not signed in to claude.ai on this PC (no session cookie), so its chats cannot be read."
    return online.Client(account), ""


def _page(said: list, start: int | None, count: int, message_chars: int, max_chars: int, header: list[str],
          reader: str = "ReadSession") -> str:
    """`said` (role, text) windowed like ReadSession: numbered, the window chosen by start/count, kept under max_chars."""
    total = len(said)
    first = (total - count + 1) if start is None else (start if start > 0 else total + start + 1)
    first = max(1, min(first, max(total, 1)))
    lines, used, shown_last = [], sum(len(h) + 1 for h in header), first - 1
    for number, (role, text) in enumerate(said[first - 1:first - 1 + count], first):
        if len(text) > message_chars:
            text = text[:message_chars] + f" … ({len(text) - message_chars} more characters)"
        line = f"[{number}] {role}: {text}"
        if used + len(line) > max_chars and lines:
            break
        lines.append(line)
        used += len(line) + 1
        shown_last = number
    footer = f"Messages {first}–{shown_last} of {total}."
    if first > 1:
        footer += f" Earlier: {reader}(id, start={max(1, first - count)})."
    if shown_last < total:
        footer += f" Later: {reader}(id, start={shown_last + 1})."
    return "\n".join(header + lines + [footer])


async def _read_chat(id, chat, start, count, message_chars, max_chars, every) -> str:
    """A chat conversation read from claude.ai (online.py), when online access is on."""
    if not online.enabled():
        return online.OFF
    full = chat[0] if chat else id.strip()
    if not claude.UUID.fullmatch(full):
        return f"Error: {id!r} is not a conversation id, or that start matches no chat seen."
    profile = claude.find_profile(chat[1].get("profile", ""), every) if chat else None
    client, why = _client(profile, every)
    if client is None:
        return f"Error: {why}"
    try:
        info, messages = await asyncio.to_thread(client.read_chat, full)
    except PermissionError as error:
        return f"Error: {error}"
    except Exception as error:   # requests / shape
        return f"Error: could not read the chat from claude.ai: {error}"
    header = [f"“{info.name or '(untitled)'}” — {info.uuid} (chat on claude.ai)",
              f"Account: {client.account.name}" + (f" · model {info.model}" if info.model else "") +
              (f" · last {_when(info.updated)}" if info.updated else "") + f" · {len(messages)} messages"]
    return _page([(m.sender, m.text) for m in messages], start, count, message_chars, max_chars, header)


@delta.add(
    "WhoAmI",
    description=("Says who and where this conversation is: the Claude Desktop profile (account) it runs in, its "
                 "conversation id and name (a chat) or Claude Code session id, its mailbox address (what SendMessage "
                 "takes to reach it), and the other profiles on this PC."),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False),
)
def who_am_i() -> str:
    every = claude.profiles()
    me = mailbox.session_id()
    mine = claude.find_profile(me, every)
    who = caller.current()
    lines = [f"Profile: {mine.name} ({mine.folder})" + (f" · account {mine.account}" if mine.account else " · signed out")
             if mine else f"Profile: {me} (not a Claude Desktop profile Claude Delta knows)",
             f"Mailbox address: {me}"]
    if who["conversation"]:
        lines.append(f"Conversation: {who['conversation']}" + (f" “{who['name']}”" if who["name"] else "") +
                     " (a chat; SendMessage(to=this id) reaches this conversation alone)")
    code = claude.code_session_of()
    if code:
        lines.append(f"Claude Code session: {code} (ReadSession and OpenSession take it)")
    if not who["conversation"] and not code:
        lines.append("Conversation: not known (the app did not say which)")
    if who["client"]:
        lines.append(f"Client: {who['client']}")
    others = [f"{p.name} ({p.folder})" for p in every if p.folder != me]
    if others:
        lines.append("Other profiles: " + ", ".join(others))
    lines.append("Online access to claude.ai: " + ("on (chats can be read and searched)" if online.enabled()
                 else "off (chats are listed and opened only)"))
    return "\n".join(lines)


@delta.add(
    "Profiles",
    description=("Lists the Claude Desktop profiles on this PC (one account each, as Claude Delta makes them): folder "
                 "(the mailbox address), name, signed in or not, open now, how many Code sessions each lists, and "
                 "messages waiting in its inbox."),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False),
)
def list_profiles() -> str:
    every = claude.profiles()
    if not every:
        return "No Claude Desktop profiles found."
    rows = claude.sessions(every)
    open_now = claude.open_profiles()
    me = mailbox.session_id()
    lines = []
    for p in every:
        count = sum(1 for r in rows if p.folder in r.profiles and not r.archived)
        waiting = len(mailbox.peek(of=p.folder))
        parts = [f"{p.folder} · {p.name}", f"account {p.account[:SHORT]}…" if p.account else "signed out",
                 "open" if p.folder in open_now else "closed", f"{count} Code session{'s' if count != 1 else ''}"]
        if waiting:
            parts.append(f"{waiting} message{'s' if waiting != 1 else ''} waiting")
        if p.folder == me:
            parts.append("this one")
        lines.append(" · ".join(parts))
    return "\n".join(lines)


@delta.add(
    "ListSessions",
    description=("Lists the sessions of every account on this PC, the latest first: Claude Code sessions (id, title, "
                 "the profiles that list it, folder, last activity, size, running now) and the chat conversations "
                 "that have used these tools. Filter by profile, kind, or a text in the title or folder. Ids are "
                 "shortened; any unique start works in ReadSession and OpenSession."),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False),
)
def list_sessions(
    profile: Annotated[str | None, Field(description="Only this profile's (its name or folder); all by default.")] = None,
    kind: Annotated[Literal["all", "code", "chat"], Field(description="code, chat, or all.")] = "all",
    query: Annotated[str | None, Field(description="Only those whose title or folder holds this (any case).")] = None,
    limit: Annotated[int, Field(description="At most this many of each kind.", ge=1, le=500)] = 20,
    archived: Annotated[bool, Field(description="Include archived Code sessions.")] = False,
    running: Annotated[bool, Field(description="Only Code sessions running now.")] = False,
) -> str:
    every = claude.profiles()
    chosen = _profile_arg(profile, every)
    if isinstance(chosen, str):
        return chosen
    needle = (query or "").casefold()
    out = []
    if kind in ("all", "code"):
        rows = [r for r in _filter(claude.sessions(every), chosen, archived)
                if (not needle or needle in r.title.casefold() or needle in r.cwd.casefold()) and (not running or r.running)]
        out.append(f"Code sessions ({len(rows)}" + (f", showing {limit}" if len(rows) > limit else "") + "):")
        out += [_session_line(r, every) for r in rows[:limit]] or ["(none)"]
    if kind in ("all", "chat"):
        by_folder = {p.folder: p.name for p in every}
        if online.enabled():
            client, why = _client(chosen, every)
            if client is None:
                out.append(f"Chat conversations: {why}")
            else:
                try:
                    chats = [c for c in client.list_chats(limit * 2)
                             if not needle or needle in (c.name or "").casefold() or needle in (c.summary or "").casefold()]
                    out.append(f"Chat conversations of {client.account.name} ({len(chats)}" +
                               (f", showing {limit}" if len(chats) > limit else "") + "):")
                    out += [f"{c.uuid[:SHORT]} “{c.name or '(untitled)'}” · {_ago(c.updated)}" for c in chats[:limit]] or ["(none)"]
                except PermissionError as error:
                    out.append(f"Chat conversations: {error}")
                except Exception as error:
                    out.append(f"Chat conversations: could not list from claude.ai ({error})")
        else:
            seen = sorted(((k, v) for k, v in caller.seen().items()
                           if (chosen is None or v.get("profile") == chosen.folder) and
                           (not needle or needle in (v.get("name") or "").casefold())),
                          key=lambda kv: kv[1].get("last", 0), reverse=True)
            out.append(f"Chat conversations seen ({len(seen)}" + (f", showing {limit}" if len(seen) > limit else "") +
                       "; turn on online access to list every chat):")
            out += [f"{k[:SHORT]} “{v.get('name') or '(untitled)'}” · {by_folder.get(v.get('profile'), v.get('profile'))}"
                    f" · last used {_ago(v.get('last', 0))}" for k, v in seen[:limit]] or ["(none)"]
    return "\n".join(out)


@delta.add(
    "SearchSessions",
    description=("Searches the messages of every Claude Code session on this PC, whichever account ran it, for a text "
                 "(or a regular expression): each session found with how many of its messages hold it and excerpts "
                 "with their message numbers (ReadSession(id, start=number) reads from there). What tools returned "
                 "is not searched; what was said and each tool call's command or path is. chats=true also searches a "
                 "profile's claude.ai chats when online access is on (slower: each chat is fetched)."),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=True),
)
async def search_sessions(
    text: Annotated[str, Field(description="What to look for.")],
    regex: Annotated[bool, Field(description="text is a regular expression (Python's).")] = False,
    match_case: Annotated[bool, Field(description="Upper and lower case differ.")] = False,
    profile: Annotated[str | None, Field(description="Only sessions this profile lists.")] = None,
    limit: Annotated[int, Field(description="At most this many sessions.", ge=1, le=200)] = 10,
    archived: Annotated[bool, Field(description="Search archived sessions too.")] = True,
    chats: Annotated[bool, Field(description="Also search this profile's claude.ai chats (needs online access).")] = False,
) -> str:
    if not text:
        return "Error: give a text to look for."
    every = claude.profiles()
    chosen = _profile_arg(profile, every)
    if isinstance(chosen, str):
        return chosen
    chat_part = await _search_chats(text, regex, match_case, chosen, limit, every) if chats else ""
    rows = _filter(claude.sessions(every), chosen, archived)

    def run():
        found, searched, deadline = [], 0, time.monotonic() + SEARCH_SECONDS
        for row, count, hits in claude.search(rows, text, regex, match_case):
            found.append((row, count, hits))
            if len(found) >= limit or time.monotonic() > deadline:
                break
        else:
            return found, None
        searched = rows.index(found[-1][0]) + 1
        return found, searched

    try:
        found, stopped_at = await asyncio.to_thread(run)
    except Exception as error:   # re.error for a bad expression
        return f"Error: {error}"
    if not found:
        return (f"Not found in {len(rows)} Code sessions." + (f"\n\n{chat_part}" if chat_part else "")) \
            if (rows or not chat_part) else chat_part
    head = f"Found in {len(found)} Code session{'s' if len(found) != 1 else ''}"
    head += f" (stopped after {stopped_at} of {len(rows)}; search again with a smaller profile or a narrower text for more)" \
        if stopped_at and stopped_at < len(rows) else f" (of {len(rows)} searched)"
    lines = [head + ":"]
    for row, count, hits in found:
        lines.append(f"{_session_line(row, every)} — {count} message{'s' if count != 1 else ''}")
        lines += [f"   #{number} {role}: {excerpt}" for number, role, excerpt in hits]
    return "\n".join(lines) + (f"\n\n{chat_part}" if chat_part else "")


async def _search_chats(text, regex, match_case, chosen, limit, every) -> str:
    """The claude.ai chat part of a search: OFF when online access is off, else the chats found."""
    if not online.enabled():
        return online.OFF
    client, why = _client(chosen, every)
    if client is None:
        return f"Chats not searched: {why}"
    budget = max(limit * 5, 40)

    def run():
        return list(online.search_chats(client, text, regex, match_case, limit, budget))

    try:
        found = await asyncio.to_thread(run)
    except Exception as error:   # PermissionError (signed out) or a request/shape error
        return f"Chats not searched: {error}"
    if not found:
        return f"No chat of {client.account.name} (of the {budget} most recent read) holds it."
    lines = [f"Found in {len(found)} chat{'s' if len(found) != 1 else ''} of {client.account.name}:"]
    for chat, hits, excerpts in found:
        lines.append(f"{chat.uuid[:SHORT]} “{chat.name or '(untitled)'}” · {_ago(chat.updated)} — "
                     f"{hits} message{'s' if hits != 1 else ''}")
        lines += [f"   #{number} {role}: {excerpt}" for number, role, excerpt in excerpts]
    return "\n".join(lines)


@delta.add(
    "ReadSession",
    description=("Reads a session of any account on this PC: a Claude Code session from its transcript, or (when online "
                 "access is on) a claude.ai chat conversation by its id, from claude.ai. Who said what, numbered, each "
                 "tool call as [tool] what. By default the last 30 messages; start picks where to begin (1 the first, "
                 "-50 the 50th from the end). Use it to load another session's work into this one and go on with it "
                 "here, or OpenSession to go on with it in Claude Desktop."),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False),
)
async def read_session(
    id: Annotated[str, Field(description="The session's id, or a unique start of it.")],
    start: Annotated[int | None, Field(description="The first message to show: 1 is the first, -N the Nth from the end.")] = None,
    count: Annotated[int, Field(description="How many messages.", ge=1, le=500)] = 30,
    message_chars: Annotated[int, Field(description="A longer message is cut to this many characters.", ge=100)] = 2000,
    max_chars: Annotated[int, Field(description="The whole answer's limit.", ge=1000)] = 30000,
) -> str:
    every = claude.profiles()
    row = claude.find_session(id, claude.sessions(every))
    if row is None:
        chat = _find_chat(id)
        if chat or claude.UUID.fullmatch(id.strip()):
            return await _read_chat(id, chat, start, count, message_chars, max_chars, every)
        return f"Error: no Code session {id!r} (or that start is not unique). ListSessions or SearchSessions finds ids."
    if not row.path:
        return f"{row.id} is not on this PC: it ran elsewhere (SSH, WSL, the cloud) or has no message yet."
    said = await asyncio.to_thread(lambda: list(claude.messages(row.path)))
    total = len(said)
    first = (total - count + 1) if start is None else (start if start > 0 else total + start + 1)
    first = max(1, min(first, max(total, 1)))
    window = said[first - 1:first - 1 + count]
    header = [f"“{row.title or '(untitled)'}” — {row.id}",
              f"Profiles: {_names(row.profiles, every)}" + (f" · folder {row.cwd}" if row.cwd else "") +
              f" · last activity {_when(row.last)}" + (f" · running (pid {row.running})" if row.running else "") +
              f" · {total} messages"]
    if row.prior:
        header.append("Goes on from: " + ", ".join(row.prior))
    lines, used = [], sum(len(h) + 1 for h in header)
    shown_last = first - 1
    for number, (role, text, _) in enumerate(window, first):
        if len(text) > message_chars:
            text = text[:message_chars] + f" … ({len(text) - message_chars} more characters)"
        line = f"[{number}] {role}: {text}"
        if used + len(line) > max_chars and lines:
            break
        lines.append(line)
        used += len(line) + 1
        shown_last = number
    footer = f"Messages {first}–{shown_last} of {total}."
    if first > 1:
        footer += f" Earlier: ReadSession(id, start={max(1, first - count)})."
    if shown_last < total:
        footer += f" Later: ReadSession(id, start={shown_last + 1})."
    return "\n".join(header + lines + [footer])


@delta.add(
    "OpenSession",
    description=("Opens a session in a Claude Desktop profile (started if it is closed), through Claude Delta: a Claude "
                 "Code session of any account (id) to go on with it there — this profile by default; a chat "
                 "conversation (conversation) in the profile it belongs to; or a new chat with a prompt (new_chat), "
                 "in Cowork with cowork=true. The session then goes on in that window, not in this conversation."),
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=False, openWorldHint=False),
)
def open_session(
    id: Annotated[str | None, Field(description="A Claude Code session's id (or a unique start of it).")] = None,
    conversation: Annotated[str | None, Field(description="A chat conversation's id (ListSessions kind=chat).")] = None,
    new_chat: Annotated[str | None, Field(description="Start a new chat with this prompt.")] = None,
    profile: Annotated[str | None, Field(description="The profile to open it in (name or folder).")] = None,
    cowork: Annotated[bool, Field(description="new_chat starts a Cowork task instead.")] = False,
    force: Annotated[bool, Field(description="Open a Code session even while it runs elsewhere.")] = False,
) -> str:
    if sum(x is not None for x in (id, conversation, new_chat)) != 1:
        return "Error: give one of id, conversation or new_chat."
    every = claude.profiles()
    target = None
    if profile:
        target = claude.find_profile(profile, every)
        if target is None:
            return f"Error: no profile {profile!r}."
    here = claude.find_profile(mailbox.session_id(), every)
    if id is not None:
        row = claude.find_session(id, claude.sessions(every))
        if row is None:
            return f"Error: no Code session {id!r} (or that start is not unique)."
        if not claude.UUID.fullmatch(row.id):
            return f"Error: {row.id} has no message yet, so it cannot be resumed."
        if row.running and not force:
            return (f"{row.id[:SHORT]} runs now (pid {row.running}): a second Claude Code on it would mix up its "
                    "transcript. Wait for it to end (Idle with a process condition), or force=true.")
        target = target or here
        url = claude.resume_link(row.id)
        what = f"Code session “{row.title or row.id}”"
    elif conversation is not None:
        found = _find_chat(conversation)
        full = found[0] if found else conversation.strip()
        if not claude.UUID.fullmatch(full):
            return f"Error: {conversation!r} is not a conversation id (or that start is not unique)."
        if target is None and found:
            target = claude.find_profile(found[1].get("profile", ""), every)
        target = target or here
        url = claude.chat_link(full)
        what = f"chat “{found[1].get('name') if found else full}”"
    else:
        target = target or here
        url = claude.new_chat_link(new_chat, cowork)
        what = "a new Cowork task" if cowork else "a new chat"
    try:
        how = claude.open_link(url, target)
    except (OSError, RuntimeError, ValueError) as error:
        return f"Error: {error}. The link: {url}"
    return f"Opened {what} {how}."
