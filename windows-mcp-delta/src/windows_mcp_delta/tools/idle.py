"""Idle and Sleep: the session waits — up to three minutes a call — until a
time, or until something it is watching happens, then wakes and goes on in
the same reply. Sleep is Idle with a time alone, named for what people ask
("sleep 10 minutes, then tell me..."): a model that sees only tools' names
finds it, where "Idle" says nothing of waiting a while.

A conversation that keeps calling Idle never really ends: between the calls
it is asleep, costing nothing, and it wakes the moment another session sends
it a message, a file appears, a process ends, and so on. With no condition
Idle is a plain pause that returns after the timeout saying to call it again;
with one or more conditions it returns as soon as any is met, naming what
woke it. `reuse=true` watches the same conditions as the call before, so the
loop that keeps the session alive need not resend them each time.

Conditions (one, or a list — any of them wakes it):
  {"type": "message", "from"?, "contains"?}   another session's message (taken and returned)
  {"type": "file", "path", "state": "exists"|"gone"|"changed"}
  {"type": "process", "name"? | "pid"?, "state": "running"|"gone"}
  {"type": "window", "title", "state": "exists"|"gone"}   (Windows)
  {"type": "command", "command", "contains"?, "wsl"?, "interval"?}   a probe that passes (exit 0, or output contains)
  {"type": "job", "id"}   a background job ends (PowerShell, Wsl, Python, Node with background=true)
A bare string is shorthand for a type with no other fields, e.g. "message".
"""
from __future__ import annotations

import asyncio
import datetime
import os
import re
import subprocess
import sys
import time
from typing import Annotated, Any

import psutil
from mcp.types import ToolAnnotations
from pydantic import Field

from windows_mcp_delta import caller, jobs, mailbox, state
from windows_mcp_delta.extend import delta
from windows_mcp_delta.tools import run

MAX = 180          # three minutes: the longest one Idle call waits before it returns "call me again"
TICK = 0.5         # how often the cheap conditions are checked
_last: dict[str, Any] = {}   # the conditions, timeout and deadline of the call before (reuse)


def _parse_until(until, duration) -> float | None:
    """The absolute deadline (epoch seconds) `until` or `duration` asks for; None for none.

    It is absolute so that the calls of one wait add up to it whatever the agent does between them: the time it
    takes to think and call again comes off what is left. `until` takes epoch seconds, "+90s" / "+5m" / "+2h",
    "HH:MM" (its next occurrence, today or tomorrow) or an ISO 8601 time; `duration` takes seconds from now.
    """
    if duration is not None:
        return time.time() + max(1.0, float(duration))
    if until is None or until == "":
        return None
    if isinstance(until, (int, float)):
        return float(until)
    text = str(until).strip()
    relative = re.fullmatch(r"\+\s*(\d+(?:\.\d+)?)\s*([smhd]?)", text, re.IGNORECASE)
    if relative:
        unit = {"": 1, "s": 1, "m": 60, "h": 3600, "d": 86400}[relative.group(2).lower()]
        return time.time() + float(relative.group(1)) * unit
    clock = re.fullmatch(r"(\d{1,2}):(\d{2})(?::(\d{2}))?", text)
    if clock:
        now = datetime.datetime.now()
        target = now.replace(hour=int(clock.group(1)), minute=int(clock.group(2)), second=int(clock.group(3) or 0),
                             microsecond=0)
        if target <= now:
            target += datetime.timedelta(days=1)
        return target.timestamp()
    try:
        return datetime.datetime.fromisoformat(text).timestamp()
    except ValueError:
        raise ValueError(f"cannot read a time from {until!r}: use epoch seconds, '+5m', 'HH:MM' or ISO 8601") from None


def _human(seconds: float) -> str:
    seconds = int(max(0, round(seconds)))
    days, rest = divmod(seconds, 86400)
    hours, rest = divmod(rest, 3600)
    minutes, secs = divmod(rest, 60)
    if days:
        return f"{days}d {hours}h"
    if hours:
        return f"{hours}h {minutes}m"
    if minutes:
        return f"{minutes}m {secs}s"
    return f"{secs}s"


def _when(deadline: float) -> str:
    try:
        moment = datetime.datetime.fromtimestamp(deadline)
    except (OSError, OverflowError, ValueError):
        return "the time set"
    return moment.strftime("%H:%M:%S") if moment.date() == datetime.date.today() else moment.strftime("%Y-%m-%d %H:%M:%S")


def _normalize(condition) -> list[dict]:
    if condition is None:
        return []
    if isinstance(condition, str):
        return [{"type": condition}]
    if isinstance(condition, dict):
        return [condition]
    if isinstance(condition, (list, tuple)):
        out = []
        for item in condition:
            if isinstance(item, str):
                out.append({"type": item})
            elif isinstance(item, dict):
                out.append(item)
        return out
    return []


def _describe(conds: list[dict]) -> str:
    parts = []
    for c in conds:
        t = c.get("type")
        if t == "message":
            parts.append("message" + (f" from {c['from']}" if c.get("from") else ""))
        elif t == "file":
            parts.append(f"file {c.get('state', 'exists')} {c.get('path')}")
        elif t == "process":
            parts.append(f"process {c.get('name') or c.get('pid')} {c.get('state', 'gone')}")
        elif t == "window":
            parts.append(f"window '{c.get('title')}' {c.get('state', 'exists')}")
        elif t == "command":
            parts.append(f"command '{c.get('command')}'")
        elif t == "job":
            parts.append(f"job {c.get('id')} ends")
        else:
            parts.append(str(t))
    return ", ".join(parts) if parts else "nothing (a plain pause)"


def _process_running(name, pid) -> bool:
    if pid is not None:
        try:
            proc = psutil.Process(int(pid))
            return proc.is_running() and proc.status() != psutil.STATUS_ZOMBIE   # a just-exited child lingers as a zombie
        except (psutil.Error, ValueError, TypeError):
            return False
    if name:
        low = str(name).lower()
        for proc in psutil.process_iter(["name"]):
            try:
                if (proc.info["name"] or "").lower() == low:
                    return True
            except psutil.Error:
                pass
    return False


def _window_titles() -> list[str]:
    if sys.platform != "win32":
        return []
    import ctypes
    from ctypes import wintypes
    titles: list[str] = []
    user32 = ctypes.windll.user32

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def collect(hwnd, _):
        if user32.IsWindowVisible(hwnd):
            length = user32.GetWindowTextLengthW(hwnd)
            if length:
                buffer = ctypes.create_unicode_buffer(length + 1)
                user32.GetWindowTextW(hwnd, buffer, length + 1)
                titles.append(buffer.value)
        return True

    user32.EnumWindows(collect, 0)
    return titles


def _probe(command: str, wsl: bool, distro, cwd):
    """A probe command run once (no shell quoting worries for the agent); its exit code and output."""
    if wsl:
        folder = cwd if cwd and str(cwd).startswith(("/", "~")) else (state.resolve(cwd) if cwd else state.cwd())
        argv, use_shell = run.wsl_argv(command, distro, folder), False
    elif sys.platform == "win32":
        argv, use_shell = ["powershell", "-NoProfile", "-NonInteractive", "-Command", command], False
    else:
        argv, use_shell = ["/bin/sh", "-c", command], False
    try:
        done = subprocess.run(argv, capture_output=True, timeout=30,
                              cwd=(state.resolve(cwd) if cwd and not str(cwd).startswith(("/", "~")) and not wsl else None),
                              creationflags=run._NO_WINDOW)
    except (subprocess.TimeoutExpired, OSError):
        return 1, ""
    return done.returncode, run._decode(done.stdout) + run._decode(done.stderr)


async def _check(cond: dict, key: int, baselines: dict, last_cmd: dict) -> str | None:
    t = cond.get("type")
    if t == "message":
        messages = await asyncio.to_thread(mailbox.take, cond.get("from"), cond.get("kind"), cond.get("contains"),
                                           caller.conversation())
        return "message(s) arrived:\n" + mailbox.describe(messages) if messages else None
    if t == "job":
        job = jobs.get(str(cond.get("id", "")))
        if job is None:
            return f"there is no job {cond.get('id')!r} (Jobs lists them)"
        if not job.poll():
            return None
        last = run._trim(run._decode(job.tail(3000)), 3000)
        job.read_to = job.size()
        return f"job {job.id} ended: {job.status()}" + (f"\n{last}" if last else "")
    if t == "file":
        path = state.resolve(cond["path"])
        want = cond.get("state", "exists")
        exists = os.path.exists(path)
        if want == "exists":
            return f"file appeared: {path}" if exists else None
        if want == "gone":
            return None if exists else f"file removed: {path}"
        if want == "changed":
            current = os.path.getmtime(path) if exists else None
            if key not in baselines:
                baselines[key] = current
                return None
            return f"file changed: {path}" if current != baselines[key] else None
        return None
    if t == "process":
        running = await asyncio.to_thread(_process_running, cond.get("name"), cond.get("pid"))
        who = cond.get("name") or cond.get("pid")
        if cond.get("state", "gone") == "running":
            return f"process running: {who}" if running else None
        return None if running else f"process ended: {who}"
    if t == "window":
        if sys.platform != "win32":
            return None
        want = cond.get("state", "exists")
        title = (cond.get("title") or "").lower()
        hit = any(title in name.lower() for name in await asyncio.to_thread(_window_titles))
        if want == "exists":
            return f"window appeared: {cond.get('title')}" if hit else None
        return None if hit else f"window gone: {cond.get('title')}"
    if t == "command":
        interval = max(2, int(cond.get("interval", 10)))
        now = time.monotonic()
        if key in last_cmd and now - last_cmd[key] < interval:
            return None
        last_cmd[key] = now
        code, out = await asyncio.to_thread(_probe, cond.get("command", ""), bool(cond.get("wsl")),
                                            cond.get("distro"), cond.get("cwd"))
        expect = cond.get("contains")
        if expect is not None:
            return f"command output contains {expect!r}" if expect in out else None
        return f"command succeeded (exit 0): {cond.get('command')}" if code == 0 else None
    return None   # an unknown type never wakes it


@delta.add(
    "Idle",
    description=(
        "Sleep, wait or delay, then go on: pauses for a duration or until a time, or until something it watches "
        "happens, and returns so you carry on in the same reply. Use it whenever you are asked to do something later "
        "('in 10 minutes tell me...', 'sleep 30m then...', 'at 15:00 check...', 'remind me...') instead of saying you "
        "cannot wait or cannot message later: your reply stays open while it waits, then you do the task and answer. "
        "Pass until ('15:00', '+10m', ISO time) or duration (seconds); one call waits three minutes at most, so call "
        "Idle(reuse=true) each time it returns until it says 'Scheduled time reached' (the deadline holds across calls, "
        "so your own time between them counts). For a timed wait alone, Sleep does the same with fewer arguments. "
        "Pass a condition (or a list, any of them wakes it) to return as soon as it is met; it also keeps a session "
        "alive at no cost between events. Conditions: {'type':'message','from'?,'contains'?} a message from another "
        "session (taken and returned); {'type':'file','path','state':'exists'|'gone'|'changed'}; "
        "{'type':'process','name'?|'pid'?,'state':'running'|'gone'}; {'type':'window','title','state':'exists'|'gone'}; "
        "{'type':'command','command','contains'?,'wsl'?,'interval'?}; {'type':'job','id'} a background job ends. "
        "A bare string like 'message' is shorthand. "
        "With neither a condition nor a time it is a plain pause."
    ),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=False, openWorldHint=True),
)
async def idle(
    condition: Annotated[Any, Field(description="A condition dict, a list of them (wake on any), or a bare type string; omit for none.")] = None,
    until: Annotated[Any, Field(description="Wake at this time: 'HH:MM', '+90s'/'+5m'/'+2h', ISO 8601, or epoch seconds. Kept across calls.")] = None,
    duration: Annotated[float | None, Field(description="Wake after this many seconds in all (across the calls that go on waiting).")] = None,
    timeout: Annotated[int, Field(description=f"Seconds this one call waits at most before telling you what is left (1 to {MAX}).")] = MAX,
    reuse: Annotated[bool, Field(description="Go on with the previous Idle call's conditions and deadline, without resending them.")] = False,
) -> str:
    if reuse and condition is None and until is None and duration is None:
        if not _last.get("set"):
            return "Nothing to reuse: call Idle with a condition or a time first (or omit reuse for a plain pause)."
        if timeout == MAX:
            timeout = _last["timeout"]
        return await _wait(_last["conds"], _last.get("deadline"), timeout, "Idle")
    return await _start(condition, until, duration, timeout, "Idle")


@delta.add(
    "Sleep",
    description=(
        "Sleep for a while, or until a time, then go on in the same reply: 'in 10 minutes tell me the gold price' is "
        "Sleep(duration=600), then look it up and answer. Use it instead of saying you cannot wait or cannot message "
        "later. duration: seconds; until: '+10m', '15:00' or an ISO time. One call sleeps three minutes at most and "
        "says how long is left: call Sleep() with no arguments to sleep on, until it says 'Scheduled time reached'; "
        "then do what was asked. (Idle does the same, and can also wake on a message, a file, a process or a job.)"
    ),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=False, openWorldHint=False),
)
async def sleep(
    duration: Annotated[float | None, Field(description="Seconds to sleep in all (across the calls that sleep on).")] = None,
    until: Annotated[Any, Field(description="Wake at this time: '+10m', '+2h', 'HH:MM', ISO 8601, or epoch seconds.")] = None,
) -> str:
    if duration is None and (until is None or until == ""):
        if _last.get("by") != "Sleep" or not _last.get("deadline"):
            return "Nothing to sleep on: call Sleep with duration (seconds) or until ('+10m', '15:00') first."
        return await _wait([], _last["deadline"], MAX, "Sleep")
    return await _start(None, until, duration, MAX, "Sleep")


async def _start(condition, until, duration, timeout, by: str) -> str:
    """A new wait: its conditions and deadline kept for the calls that go on with it."""
    global _last
    try:
        deadline = _parse_until(until, duration)
    except ValueError as error:
        return f"Error: {error}"
    conds = _normalize(condition)
    _last = {"set": True, "conds": conds, "timeout": timeout, "deadline": deadline, "by": by}
    return await _wait(conds, deadline, timeout, by)


async def _wait(conds: list[dict], deadline: float | None, timeout, by: str) -> str:
    """Waits for `conds` or `deadline`, `timeout` seconds at most; `by` names the tool, to say how to go on."""
    again = "Sleep()" if by == "Sleep" else "Idle(reuse=true)"
    timeout = max(1, min(int(timeout), MAX))
    started = time.monotonic()
    baselines: dict = {}
    last_cmd: dict = {}
    notes: set[str] = set()

    def left() -> str:
        return f" {_human(deadline - time.time())} left until {_when(deadline)}." if deadline else ""

    while True:
        for key, cond in enumerate(conds):
            try:
                reason = await _check(cond, key, baselines, last_cmd)
            except Exception as error:   # a bad condition must not end the wait
                notes.add(f"{cond.get('type')}: {error}")
                reason = None
            if reason:
                return f"Woke after {int(time.monotonic() - started)}s \u2014 {reason}" + (
                    f"\n(The schedule still holds:{left()} Call {again} to go on waiting.)" if deadline else "")
        if deadline is not None and time.time() >= deadline:
            _last["deadline"] = None   # reached: a later reuse does not fire it again
            return f"Scheduled time reached ({_when(deadline)}). Do what was scheduled."
        remaining = timeout - (time.monotonic() - started)
        if remaining <= 0:
            tail = f" (note: {'; '.join(sorted(notes))})" if notes else ""
            if deadline is not None:
                if by == "Sleep":
                    return f"Still sleeping:{left()} Call Sleep() now to sleep on (time spent between calls counts)."
                return (f"Still waiting:{left()} Call Idle(reuse=true) now to go on (time spent between calls "
                        f"counts). Watching: {_describe(conds)}.{tail}")
            return (f"Idle {int(time.monotonic() - started)}s, nothing matched. Call Idle again (reuse=true) to keep "
                    f"waiting. Watching: {_describe(conds)}.{tail}")
        nap = min(TICK, remaining)
        if deadline is not None:
            nap = min(nap, max(0.0, deadline - time.time()))
        await asyncio.sleep(nap)
