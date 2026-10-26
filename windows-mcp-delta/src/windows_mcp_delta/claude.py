"""What Claude Desktop and Claude Code keep on this PC, read only, as Claude
Delta's sessions view reads it (sessionstore.c, profiles.c):

- The profiles: %APPDATA%\\Claude, the stock one (Main), whose files Claude
  may keep in %LOCALAPPDATA%\\Claude-Data\\<package family> or the package's
  LocalCache, and each %APPDATA%\\Claude-<name>. Named as Claude Delta names
  them (the registry), each signed in to the account its config.json names
  (lastKnownAccountUuid).
- A Claude Code session: its transcript, in the Claude Code folder every
  profile shares (~\\.claude\\projects\\<project>\\<id>.jsonl, or under
  CLAUDE_CONFIG_DIR), and an entry in each profile that lists it
  (claude-code-sessions\\<account>\\<organization>\\local_<id>.json: title,
  folder, star, last activity). A profile shows its account's entries of
  the organization with the latest one. A transcript no profile lists is a
  session Claude Code ran on its own (in a terminal).
- Which run now: each running Claude Code writes ~\\.claude\\sessions\\<pid>.json.
- The profile a process belongs to: the Claude Desktop among its parents,
  whose --user-data-dir names the profile's folder (none: the stock one).
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

import psutil

STOCK = "Claude"
PREFIX = "Claude-"
ENTRIES = "claude-code-sessions"
REG_PROFILES = r"Software\Claude Desktop Profiles Manager\Profiles"
REG_HANDLER = r"Software\Classes\ClaudeDesktopProfilesManager.Url\shell\open\command"
CONFIG_MAX = 16 * 1024 * 1024
ENTRY_MAX = 1024 * 1024
HEAD_MAX = 256 * 1024          # what is read of a transcript for its title
_TOOL_INPUTS = ("description", "command", "file_path", "path", "pattern", "url", "query", "prompt")
_FAMILY = re.compile(r"Claude_[A-Za-z0-9]+")
UUID = re.compile(r"[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")


@dataclass
class Profile:
    folder: str              # Claude, Claude-Work: its id (mailbox addresses and --launch use it)
    name: str                # as Claude Delta shows it: Main, Work
    data_dir: Path           # what --user-data-dir names (the stock one: none is passed)
    storage: Path | None     # where its files are; None when it has none yet
    stock: bool
    account: str = ""        # the account signed in; "" signed out


@dataclass
class Session:
    id: str                  # the transcript's (Claude Code's session id)
    path: Path | None        # its transcript; None: not on this PC (yet)
    title: str = ""
    cwd: str = ""
    last: float = 0.0        # last activity, seconds since 1970
    created: float = 0.0
    bytes: int = 0
    starred: bool = False
    archived: bool = False
    model: str = ""
    profiles: list[str] = field(default_factory=list)   # the folders of the profiles that list it
    prior: list[str] = field(default_factory=list)      # the transcripts it went on from
    running: int = 0         # the pid of the Claude Code running it; 0: none


# ------------------------------------------------------------------ places

def _env_dir(name: str) -> Path | None:
    value = os.environ.get(name)
    return Path(value) if value else None


def code_dir() -> Path:
    return _env_dir("CLAUDE_CONFIG_DIR") or Path.home() / ".claude"


def _reg(key: str, value: str | None) -> str | None:
    if sys.platform != "win32":
        return None
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key) as handle:
            data, _ = winreg.QueryValueEx(handle, value or "")
            return data if isinstance(data, str) else None
    except OSError:
        return None


def _reg_exists(key: str) -> bool:
    if sys.platform != "win32":
        return False
    try:
        import winreg
        winreg.CloseKey(winreg.OpenKey(winreg.HKEY_CURRENT_USER, key))
        return True
    except OSError:
        return False


def _families(local: Path) -> list[str]:
    try:
        return sorted(p.name for p in (local / "Packages").iterdir() if _FAMILY.fullmatch(p.name) and p.is_dir())
    except OSError:
        return []


def _stock_storage(data_dir: Path) -> Path | None:
    """Where the stock profile's files are: moved out of the package, else %APPDATA%\\Claude, else the package's own."""
    local = _env_dir("LOCALAPPDATA")
    families = _families(local) if local else []
    for family in families:
        moved = local / "Claude-Data" / family.lower()
        if moved.is_dir():
            return moved
    if data_dir.is_dir():
        return data_dir
    for family in families:
        cached = local / "Packages" / family / "LocalCache" / "Roaming" / STOCK
        if cached.is_dir():
            return cached
    return None


def _is_profile_folder(name: str) -> bool:
    rest = name[len(PREFIX):]
    if not name.lower().startswith(PREFIX.lower()) or not rest or len(name) >= 64:
        return False
    low = rest.lower()
    return low not in ("3p", "data") and not low.endswith(("-3p", "-data")) and all(ord(c) >= 32 for c in rest)


def _read_json(path: Path, limit: int):
    try:
        with open(path, "rb") as file:
            data = file.read(limit + 1)
        return None if len(data) > limit else json.loads(data.decode("utf-8-sig"))
    except (OSError, ValueError):
        return None


def _make(folder: str, data_dir: Path, storage: Path | None, stock: bool) -> Profile:
    name = _reg(f"{REG_PROFILES}\\{folder}", "Name") or ("Main" if stock else folder[len(PREFIX):])
    config = _read_json(storage / "config.json", CONFIG_MAX) if storage else None
    account = config.get("lastKnownAccountUuid") if isinstance(config, dict) else None
    return Profile(folder, name, data_dir, storage, stock, account if isinstance(account, str) else "")


def profiles() -> list[Profile]:
    """Every profile on this PC: the stock one first, then the others by folder."""
    root = _env_dir("APPDATA")
    if not root:
        return []
    stock = root / STOCK
    out = [_make(STOCK, stock, _stock_storage(stock), True)]
    try:
        folders = sorted(p.name for p in root.iterdir() if _is_profile_folder(p.name) and p.is_dir())
    except OSError:
        folders = []
    for folder in folders:
        data_dir = root / folder
        if (data_dir / "Local State").is_file() or _reg_exists(f"{REG_PROFILES}\\{folder}"):
            out.append(_make(folder, data_dir, data_dir, False))
    return out


def find_profile(name: str, among: list[Profile] | None = None) -> Profile | None:
    """The profile `name` names: its folder (Claude-Work), its name (Work), or Main/Claude for the stock one."""
    key = name.strip().casefold()
    for profile in among if among is not None else profiles():
        if key in (profile.folder.casefold(), profile.name.casefold()) or (profile.stock and key in ("main", "stock")) \
                or key == (PREFIX + profile.name).casefold():
            return profile
    return None


# ------------------------------------------------------- processes and who

def _user_data_dir(cmdline: list[str]) -> str | None:
    for i, arg in enumerate(cmdline):
        if arg.startswith("--user-data-dir="):
            return arg.split("=", 1)[1].strip('"')
        if arg == "--user-data-dir" and i + 1 < len(cmdline):
            return cmdline[i + 1]
    return None


def _code_session_of(pid: int) -> str | None:
    """The session of the Claude Code that runs as `pid`: its sessions\\<pid>.json; None when it is none."""
    data = _read_json(code_dir() / "sessions" / f"{pid}.json", ENTRY_MAX)
    session = data.get("sessionId") if isinstance(data, dict) else None
    return session if isinstance(session, str) and session else None


def _chain(process: psutil.Process | None = None) -> list[psutil.Process]:
    try:
        process = process or psutil.Process()
        return [process] + process.parents()
    except psutil.Error:
        return []


def profile_of(process: psutil.Process | None = None) -> str | None:
    """The folder of the profile whose Claude Desktop started `process` (this one by default); None when none did.
    Claude Code is a claude.exe too: one that has a sessions\\<pid>.json is passed over."""
    for p in _chain(process):
        try:
            if p.name().lower() != "claude.exe" or _code_session_of(p.pid):
                continue
            data = _user_data_dir(p.cmdline())
        except psutil.Error:
            continue
        return re.split(r"[\\/]", data.rstrip("\\/"))[-1] if data else STOCK
    return None


def code_session_of(process: psutil.Process | None = None) -> str | None:
    """The Claude Code session among `process`'s parents (this one's by default): the session that called us."""
    for p in _chain(process)[1:]:
        session = _code_session_of(p.pid)
        if session:
            return session
    return None


def open_profiles() -> set[str]:
    """The folders of the profiles whose Claude Desktop runs now (its main process: no --type= of Electron's helpers)."""
    out = set()
    for p in psutil.process_iter(["name", "cmdline"]):
        try:
            if (p.info["name"] or "").lower() != "claude.exe":
                continue
            cmdline = p.info["cmdline"] or []
            if any(a.startswith("--type=") for a in cmdline) or _code_session_of(p.pid):
                continue
            data = _user_data_dir(cmdline)
            out.add(re.split(r"[\\/]", data.rstrip("\\/"))[-1] if data else STOCK)
        except (psutil.Error, TypeError):
            continue
    return out


def running() -> dict[str, int]:
    """The Claude Code sessions running now: id (lower case) to pid."""
    out = {}
    try:
        files = list((code_dir() / "sessions").glob("*.json"))
    except OSError:
        return out
    for file in files:
        if not file.stem.isdigit() or not psutil.pid_exists(int(file.stem)):
            continue
        session = _code_session_of(int(file.stem))
        if session:
            out[session.lower()] = int(file.stem)
    return out


# -------------------------------------------------------------- sessions

def transcripts() -> dict[str, Path]:
    """Every transcript in Claude Code's folder: id (lower case) to path (subagents' are in folders of their own, left out)."""
    found: dict[str, Path] = {}
    try:
        projects = list(os.scandir(code_dir() / "projects"))
    except OSError:
        return found
    for project in projects:
        try:
            if not project.is_dir():
                continue
            for file in os.scandir(project.path):
                if file.name.endswith(".jsonl") and file.is_file():
                    found[file.name[:-6].lower()] = Path(file.path)
        except OSError:
            continue
    return found


def _newest_json(folder: Path) -> float:
    try:
        return max((f.stat().st_mtime for f in folder.glob("*.json")), default=-1.0)
    except OSError:
        return -1.0


def entries_dir(profile: Profile) -> Path | None:
    """The folder of the entries `profile` shows: its account's organization with the latest one."""
    if not profile.storage:
        return None
    root = profile.storage / ENTRIES
    try:
        if profile.account:
            candidates = [d for d in (root / profile.account).iterdir() if d.is_dir()]
        else:
            candidates = [o for a in root.iterdir() if a.is_dir() for o in a.iterdir() if o.is_dir()]
    except OSError:
        return None
    return max(candidates, key=_newest_json, default=None)


def entries(profile: Profile) -> list[dict]:
    folder = entries_dir(profile)
    out = []
    if folder:
        for file in sorted(folder.glob("local_*.json")):
            data = _read_json(file, ENTRY_MAX)
            if isinstance(data, dict) and isinstance(data.get("sessionId"), str):
                out.append(data)
    return out


def _seconds(value) -> float:
    return value / 1000.0 if isinstance(value, (int, float)) and value > 0 else 0.0


_titles: dict[str, tuple[float, int, str]] = {}
_titles_lock = threading.Lock()


def transcript_title(path: Path) -> str:
    """A title for a transcript no profile names: its summary, else its first message's first line."""
    try:
        stat = path.stat()
    except OSError:
        return ""
    with _titles_lock:
        cached = _titles.get(str(path))
    if cached and cached[:2] == (stat.st_mtime, stat.st_size):
        return cached[2]
    title = ""
    try:
        with open(path, "rb") as file:
            head = file.read(HEAD_MAX)
        for line in head.splitlines():
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            if not isinstance(obj, dict):
                continue
            if obj.get("type") in ("summary", "custom-title") and isinstance(obj.get("summary") or obj.get("customTitle"), str):
                title = (obj.get("customTitle") or obj.get("summary")).strip()
                break
            shown = message(obj)
            if shown and shown[0] == "User" and not title:
                title = shown[1].strip().splitlines()[0][:100]
                break
    except OSError:
        pass
    with _titles_lock:
        _titles[str(path)] = (stat.st_mtime, stat.st_size, title)
    return title


def sessions(all_profiles: list[Profile] | None = None) -> list[Session]:
    """Every Code session on this PC, the latest first: those the profiles list, then those none does."""
    paths = transcripts()
    live = running()
    rows: dict[str, Session] = {}
    priors: set[str] = set()
    for profile in all_profiles if all_profiles is not None else profiles():
        for entry in entries(profile):
            key = entry.get("cliSessionId") if isinstance(entry.get("cliSessionId"), str) and entry.get("cliSessionId") else entry["sessionId"]
            row = rows.get(key.lower())
            if row is None:
                row = rows[key.lower()] = Session(key, paths.get(key.lower()))
            last = _seconds(entry.get("lastActivityAt"))
            if last >= row.last:
                row.last = last
                row.title = entry.get("title") if isinstance(entry.get("title"), str) else row.title
                row.cwd = entry.get("cwd") if isinstance(entry.get("cwd"), str) else row.cwd
                row.model = entry.get("model") if isinstance(entry.get("model"), str) else row.model
                row.created = _seconds(entry.get("createdAt")) or row.created
            row.starred |= entry.get("isStarred") is True
            row.archived = row.archived or entry.get("isArchived") is True
            if profile.folder not in row.profiles:
                row.profiles.append(profile.folder)
            for prior in entry.get("priorCliSessionIds") or []:
                if isinstance(prior, str) and prior.lower() not in (p.lower() for p in row.prior):
                    row.prior.append(prior)
                    priors.add(prior.lower())
    for key, path in paths.items():
        if key not in rows and key not in priors:
            rows[key] = Session(path.stem, path)
    for key, row in rows.items():
        if row.path:
            try:
                stat = row.path.stat()
                row.bytes = stat.st_size
                row.last = row.last or stat.st_mtime
            except OSError:
                pass
            if not row.title:
                row.title = transcript_title(row.path)
        row.running = live.get(key, 0)
    return sorted(rows.values(), key=lambda s: s.last, reverse=True)


def find_session(key: str, among: list[Session] | None = None) -> Session | None:
    """The session `key` names: its id, or the start of it (at least 4 characters, and only one session's)."""
    key = key.strip().lower()
    rows = among if among is not None else sessions()
    exact = [s for s in rows if s.id.lower() == key]
    if exact:
        return exact[0]
    if len(key) >= 4:
        starts = [s for s in rows if s.id.lower().startswith(key)]
        if len(starts) == 1:
            return starts[0]
    return None


# ------------------------------------------------------------- messages

def _tool(block: dict) -> str:
    name = block.get("name") if isinstance(block.get("name"), str) else "?"
    given = block.get("input") if isinstance(block.get("input"), dict) else {}
    for key in _TOOL_INPUTS:
        what = given.get(key)
        if isinstance(what, str) and what.strip():
            return f"[{name}] {what.strip()}"
    return f"[{name}]"


def message(obj: dict) -> tuple[str, str] | None:
    """One transcript line as Claude Delta's preview shows it: ("User" or "Claude", what was said, each tool call
    on a line of its own as "[name] what"); None for what shows nothing (tool results, thinking, meta, side chains,
    the plumbing of commands)."""
    kind = obj.get("type")
    if kind not in ("user", "assistant") or obj.get("isMeta") is True or obj.get("isSidechain") is True:
        return None
    body = obj.get("message")
    content = body.get("content") if isinstance(body, dict) else None
    parts: list[str] = []
    if isinstance(content, str):
        if kind == "user" and content.startswith("<"):
            return None
        parts.append(content.strip())
    elif isinstance(content, list):
        for block in content:
            if not isinstance(block, dict):
                continue
            if block.get("type") == "text" and isinstance(block.get("text"), str):
                if not (kind == "user" and block["text"].startswith("<")):
                    parts.append(block["text"].strip())
            elif block.get("type") == "tool_use":
                parts.append(_tool(block))
    text = "\n".join(p for p in parts if p)
    return ("User" if kind == "user" else "Claude", text) if text else None


def messages(path: Path) -> Iterator[tuple[str, str, str]]:
    """The messages of a transcript, in order: (role, text, timestamp as written)."""
    try:
        file = open(path, "rb")
    except OSError:
        return
    with file:
        for line in file:
            if b'"user"' not in line and b'"assistant"' not in line:
                continue
            try:
                obj = json.loads(line)
            except ValueError:
                continue
            shown = message(obj) if isinstance(obj, dict) else None
            if shown:
                stamp = obj.get("timestamp")
                yield shown[0], shown[1], stamp if isinstance(stamp, str) else ""


def _byte_prefilter(text: str, match_case: bool) -> bytes | None:
    """What a transcript's bytes must hold for `text` to be in it; None when the bytes cannot tell
    (JSON escapes it, or a case-blind search of letters other than ASCII's)."""
    if any(c in text for c in '"\\') or any(ord(c) < 32 for c in text):
        return None
    if not match_case and any(c.lower() != c.upper() and ord(c) > 127 for c in text):
        return None
    data = text.encode("utf-8")
    return data if match_case else data.lower()


def search(rows: list[Session], text: str, regex: bool = False, match_case: bool = False,
           hits_per_session: int = 2, context: int = 70) -> Iterator[tuple[Session, int, list[tuple[int, str, str]]]]:
    """The sessions whose messages hold `text`, as found: (session, how many messages hold it, the first ones:
    (message number from 1, role, an excerpt around it)). ValueError for a bad regular expression."""
    flags = 0 if match_case else re.IGNORECASE
    pattern = re.compile(text if regex else re.escape(text), flags)
    needle = None if regex else _byte_prefilter(text, match_case)
    for row in rows:
        if not row.path:
            continue
        if needle is not None:
            try:
                data = row.path.read_bytes()
            except OSError:
                continue
            if needle not in (data if match_case else data.lower()):
                continue
        found, count = [], 0
        for number, (role, said, _) in enumerate(messages(row.path), 1):
            hit = pattern.search(said)
            if not hit:
                continue
            count += 1
            if len(found) < hits_per_session:
                start, end = max(0, hit.start() - context), min(len(said), hit.end() + context)
                excerpt = " ".join(said[start:end].split())
                found.append((number, role, ("…" if start else "") + excerpt + ("…" if end < len(said) else "")))
        if count:
            yield row, count, found


# ------------------------------------------------------------------ links

def manager() -> str | None:
    """Claude Delta's program: $WINDOWS_MCP_DELTA_MANAGER, else the one registered for claude:// links."""
    named = os.environ.get("WINDOWS_MCP_DELTA_MANAGER")
    if named and os.path.isfile(named):
        return named
    command = _reg(REG_HANDLER, None)
    if command:
        match = re.match(r'\s*"([^"]+)"', command) or re.match(r"\s*(\S+)", command)
        if match and os.path.isfile(match.group(1)):
            return match.group(1)
    return None


def open_link(url: str, profile: Profile | None) -> str:
    """`url` (a claude:// link) opened in `profile`'s Claude Desktop, started when it is closed; how it was."""
    if not url.startswith("claude:"):
        raise ValueError("only claude:// links are opened")
    program = manager()
    flags = getattr(subprocess, "DETACHED_PROCESS", 0) | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
    if program:
        args = [program, "--launch", profile.folder, url] if profile else [program, "--url", url]
        subprocess.Popen(args, close_fds=True, creationflags=flags)
        return f"in {profile.name}" if profile else "through Claude Delta, which asks which profile when several could take it"
    if sys.platform == "win32" and profile is None:
        os.startfile(url)  # noqa: S606  the app registered for claude:// links
        return "with the app registered for claude:// links"
    raise RuntimeError("Claude Delta is not installed (it opens a link in a chosen profile)")


def resume_link(session_id: str) -> str:
    return f"claude://resume?session={session_id}"


def chat_link(conversation: str) -> str:
    return f"claude://claude.ai/chat/{conversation}"


def new_chat_link(prompt: str | None, cowork: bool = False) -> str:
    from urllib.parse import quote
    base = "claude://cowork/new" if cowork else "claude://claude.ai/new"
    return f"{base}?q={quote(prompt, safe='')}" if prompt else base
