"""Esc stops Claude acting on the desktop.

Windows-MCP keeps the desktop for the person: a call that would act on it is refused while they used the mouse or
keyboard in the last 10 s (USER_CONTROL), and while Claude acts their own input is swallowed until a chord takes it
back. Working beside Claude that way is not possible. windows-mcp-delta lets Claude act while the person works, and
gives them a key to stop it instead: Esc, pressed on the keyboard while Claude is acting (within ACTIVE seconds of
its latest action; Esc pressed at other times is the person's own). Its actions are then refused, with a message
telling it to stop and ask, for HOLD seconds after the Esc or the latest refused try, so that it cannot go on by
trying again; once it has left the desktop alone that long, the person's next request works. The key still
reaches the app, and an Esc that Claude itself sends (injected input) does not count.

The key is read by a low-level keyboard hook that only looks (it never holds a key back), on a thread of its own,
started with Claude's first action.
"""
from __future__ import annotations

import os
import sys
import threading
import time

ACTIVE = 15.0   # seconds after Claude's latest desktop action during which an Esc stops it
HOLD = 15.0     # seconds its actions stay refused, from the Esc or the latest refused try

STOPPED = ("STOPPED_BY_USER: the person pressed Esc to stop you acting on the desktop. The action that was already "
           "running when they pressed it may have finished; only the calls after it are refused. Do not act on the "
           "desktop again now: tell them what you were doing and the last action you know ran, and ask how to go on.")

_lock = threading.Lock()
_last_action = 0.0
_stopped_until = 0.0
_thread: threading.Thread | None = None


def user_gate() -> bool:
    """WINDOWS_MCP_DELTA_USER_CONTROL=1 keeps Windows-MCP's own gate (wait for the mouse to rest) instead."""
    return os.environ.get("WINDOWS_MCP_DELTA_USER_CONTROL", "").strip().lower() in ("1", "true", "yes", "on")


def note_action(now: float | None = None) -> None:
    """Claude starts acting on the desktop."""
    global _last_action
    with _lock:
        _last_action = time.monotonic() if now is None else now


def pressed(now: float | None = None) -> bool:
    """The person pressed Esc: Claude's actions are stopped when it is acting now. TRUE when they are."""
    global _stopped_until
    now = time.monotonic() if now is None else now
    with _lock:
        if _last_action and now - _last_action <= ACTIVE:
            _stopped_until = now + HOLD
            return True
    return False


def refused(now: float | None = None) -> bool:
    """Whether an action now is refused; a try while stopped keeps it stopped."""
    global _stopped_until
    now = time.monotonic() if now is None else now
    with _lock:
        if now < _stopped_until:
            _stopped_until = now + HOLD
            return True
    return False


def reset() -> None:
    global _last_action, _stopped_until
    with _lock:
        _last_action = _stopped_until = 0.0


def ensure_started() -> None:
    """The keyboard hook, started once (Windows; WINDOWS_MCP_DELTA_NO_ESC_HOOK=1 leaves it out, for tests)."""
    global _thread
    if sys.platform != "win32" or os.environ.get("WINDOWS_MCP_DELTA_NO_ESC_HOOK"):
        return
    with _lock:
        if _thread is not None:
            return
        _thread = threading.Thread(target=_watch, name="windows-mcp-delta Esc", daemon=True)
    _thread.start()


def _watch() -> None:
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)   # its own: Windows-MCP sets argtypes on the shared one
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    LRESULT = ctypes.c_ssize_t
    HOOKPROC = ctypes.WINFUNCTYPE(LRESULT, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM)

    class KBDLLHOOKSTRUCT(ctypes.Structure):
        _fields_ = [("vkCode", wintypes.DWORD), ("scanCode", wintypes.DWORD), ("flags", wintypes.DWORD),
                    ("time", wintypes.DWORD), ("dwExtraInfo", ctypes.c_size_t)]

    user32.SetWindowsHookExW.argtypes = [ctypes.c_int, HOOKPROC, wintypes.HINSTANCE, wintypes.DWORD]
    user32.SetWindowsHookExW.restype = ctypes.c_void_p
    user32.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int, wintypes.WPARAM, wintypes.LPARAM]
    user32.CallNextHookEx.restype = LRESULT
    user32.GetMessageW.argtypes = [ctypes.POINTER(wintypes.MSG), wintypes.HWND, wintypes.UINT, wintypes.UINT]
    kernel32.GetModuleHandleW.restype = wintypes.HMODULE

    WH_KEYBOARD_LL, WM_KEYDOWN, WM_SYSKEYDOWN, VK_ESCAPE, LLKHF_INJECTED = 13, 0x100, 0x104, 0x1B, 0x10

    def on_key(code, wparam, lparam):
        try:
            if code == 0 and wparam in (WM_KEYDOWN, WM_SYSKEYDOWN):
                data = ctypes.cast(lparam, ctypes.POINTER(KBDLLHOOKSTRUCT)).contents
                if data.vkCode == VK_ESCAPE and not data.flags & LLKHF_INJECTED:
                    pressed()
        except Exception:
            pass   # the hook must always pass the key on
        return user32.CallNextHookEx(None, code, wparam, lparam)

    callback = HOOKPROC(on_key)   # kept alive for as long as the hook is
    if not user32.SetWindowsHookExW(WH_KEYBOARD_LL, callback, kernel32.GetModuleHandleW(None), 0):
        return
    message = wintypes.MSG()
    while user32.GetMessageW(ctypes.byref(message), None, 0, 0) > 0:
        user32.TranslateMessage(ctypes.byref(message))
        user32.DispatchMessageW(ctypes.byref(message))
