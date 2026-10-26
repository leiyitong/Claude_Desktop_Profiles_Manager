"""What windows-mcp-delta's tools share for as long as the server runs.

- The working directory: relative paths of every tool resolve against it,
  and a shell's `cd` (Shell, PowerShell) moves it. It starts at the user's
  Desktop, where Windows-MCP's own FileSystem resolves relative paths.
- The files read, with the time each was read at: Write and Edit refuse to
  replace a file the conversation has not read, or that changed on disk
  since it was read, so that nothing is overwritten unseen.
"""
from __future__ import annotations

import os
import threading
from pathlib import Path

_lock = threading.Lock()
_cwd: str | None = None
_read: dict[str, float] = {}


def _default_cwd() -> str:
    try:
        from platformdirs import user_desktop_dir
        desktop = user_desktop_dir()
        if desktop and os.path.isdir(desktop):
            return desktop
    except Exception:
        pass
    return str(Path.home())


def cwd() -> str:
    global _cwd
    with _lock:
        if _cwd is None:
            _cwd = _default_cwd()
        return _cwd


def set_cwd(path: str) -> str:
    """The working directory moved to `path` (resolved against the current one); ValueError when it is no folder."""
    global _cwd
    target = resolve(path)
    if not os.path.isdir(target):
        raise ValueError(f"Not a folder: {target}")
    with _lock:
        _cwd = target
    return target


def resolve(path: str) -> str:
    """`path` absolute and normalized: `~` expanded, a relative one against the working directory."""
    expanded = os.path.expandvars(os.path.expanduser(path.strip().strip('"')))
    if not os.path.isabs(expanded):
        expanded = os.path.join(cwd(), expanded)
    return os.path.normpath(expanded)


def _key(path: str) -> str:
    return os.path.normcase(os.path.abspath(path))


def remember_read(path: str) -> None:
    """`path` read now, whole or in part."""
    try:
        stamp = os.path.getmtime(path)
    except OSError:
        return
    with _lock:
        _read[_key(path)] = stamp


def unseen_change(path: str) -> str | None:
    """Why `path` may not be replaced: never read, or changed since; None when it may (or does not exist)."""
    if not os.path.exists(path):
        return None
    with _lock:
        stamp = _read.get(_key(path))
    if stamp is None:
        return f"{path} exists and was not read in this conversation: Read it first, so that nothing in it is lost unseen."
    try:
        if os.path.getmtime(path) > stamp + 1e-6:
            return f"{path} changed on disk since it was read: Read it again first."
    except OSError:
        return None
    return None


def reset() -> None:
    """Back to how the server starts (tests)."""
    global _cwd
    with _lock:
        _cwd = None
        _read.clear()
