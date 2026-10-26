"""A profile's claude.ai session, read from its own cookie store.

Claude Desktop is Chromium: it keeps claude.ai's cookies in the profile's
`Network\\Cookies` (a SQLite database), each value encrypted with a key kept
in `Local State` and sealed to the user by Windows (DPAPI), exactly as
Chrome and Edge do. The session cookie (`sessionKey`) and the active
organization (`lastActiveOrg`) are read here, never written, so a signed-in
profile can talk to claude.ai as itself (online.py). The database is opened
read-only and copied first when it is locked, so a running Claude is not
disturbed.
"""
from __future__ import annotations

import base64
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

from windows_mcp_delta import online

CLAUDE_HOSTS = ("claude.ai", ".claude.ai")
_UUID = re.compile(r"[0-9a-fA-F-]{8,40}")


def _app_key(storage: Path) -> bytes | None:
    """The AES key Chromium sealed in `Local State` (DPAPI-unwrapped); None when there is none to read."""
    try:
        state = json.loads((storage / "Local State").read_text(encoding="utf-8"))
        sealed = base64.b64decode(state["os_crypt"]["encrypted_key"])
    except (OSError, ValueError, KeyError):
        return None
    if sealed[:5] == b"DPAPI":
        return _dpapi_unprotect(sealed[5:])
    return sealed


def _dpapi_unprotect(blob: bytes) -> bytes | None:
    """CryptUnprotectData on `blob` (Windows only); None off Windows or when it cannot be unsealed."""
    if sys.platform != "win32":
        return None
    import ctypes
    import ctypes.wintypes

    class Blob(ctypes.Structure):
        _fields_ = [("cbData", ctypes.wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

    source = Blob(len(blob), ctypes.cast(ctypes.c_char_p(blob), ctypes.POINTER(ctypes.c_char)))
    out = Blob()
    if not ctypes.windll.crypt32.CryptUnprotectData(ctypes.byref(source), None, None, None, None, 0, ctypes.byref(out)):
        return None
    try:
        return ctypes.string_at(out.pbData, out.cbData)
    finally:
        ctypes.windll.kernel32.LocalFree(out.pbData)


def decrypt_value(encrypted: bytes, key: bytes | None) -> str | None:
    """A cookie's value: an AES-GCM one (v10/v11) with `key`, a DPAPI one, or a plain one; None when it cannot be read."""
    if not encrypted:
        return ""
    if encrypted[:3] in (b"v10", b"v11"):
        if not key:
            return None
        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
        nonce, body = encrypted[3:15], encrypted[15:]
        try:
            plain = AESGCM(key).decrypt(nonce, body, None)
        except Exception:
            return None
        # Chromium 122+ prefixes the plaintext with a 32-byte domain hash; the value itself is printable.
        text = _as_text(plain)
        return text if text is not None else _as_text(plain[32:])
    unsealed = _dpapi_unprotect(encrypted)        # DPAPI-only (older Chromium), or not ours
    if unsealed is not None:
        return unsealed.decode("utf-8", errors="replace")
    return _as_text(encrypted)


def _as_text(data: bytes) -> str | None:
    """`data` as text when it is printable UTF-8 (a cookie value is), else None."""
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError:
        return None
    return text if all(c.isprintable() or c in "\t\r\n" for c in text) else None


def _read_cookies(storage: Path) -> dict[str, str]:
    """claude.ai's cookies from the profile: name to value."""
    database = storage / "Network" / "Cookies"
    if not database.is_file():
        return {}
    key = _app_key(storage)
    temporary = None
    try:
        try:
            connection = sqlite3.connect(f"file:{database}?mode=ro&immutable=1", uri=True)
        except sqlite3.OperationalError:
            handle, temporary = tempfile.mkstemp(suffix=".sqlite")
            os.close(handle)
            shutil.copy2(database, temporary)
            connection = sqlite3.connect(temporary)
        out: dict[str, str] = {}
        try:
            for host, name, encrypted, plain in connection.execute(
                    "SELECT host_key, name, encrypted_value, value FROM cookies"):
                if host not in CLAUDE_HOSTS or name in out:
                    continue
                value = decrypt_value(bytes(encrypted) if isinstance(encrypted, (bytes, bytearray)) else b"", key)
                out[name] = value if value else (plain or "")
        finally:
            connection.close()
        return out
    except sqlite3.Error:
        return {}
    finally:
        if temporary:
            try:
                os.remove(temporary)
            except OSError:
                pass


def account_for(profile) -> online.Account | None:
    """The claude.ai session of `profile` (claude.py's Profile): its sessionKey and active organization, or None."""
    if not getattr(profile, "storage", None):
        return None
    cookies = _read_cookies(Path(profile.storage))
    session_key = cookies.get("sessionKey")
    if not session_key:
        return None
    org = cookies.get("lastActiveOrg", "")
    return online.Account(session_key, org if _UUID.fullmatch(org or "") else "", getattr(profile, "name", ""))
