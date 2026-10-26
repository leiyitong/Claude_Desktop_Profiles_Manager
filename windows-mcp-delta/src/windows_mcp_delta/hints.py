"""What windows-mcp-delta tells Claude beyond its tools, read from the profile it serves.

The profile's Claude config is read once, as the server starts: Claude starts its servers again at its next start,
which is when a change to its config takes effect anyway. When the profile has the searxng server Claude Delta
sets up (mcp-searxng), Claude is told where the instance is, and how to query it by hand without garbling a
Chinese query: the query goes in UTF-8, and Git Bash's own curl sends the console's code page (GBK) instead.
"""
from __future__ import annotations

import json
import os
from typing import Any

from windows_mcp_delta import claude

CONFIG = "claude_desktop_config.json"
CONFIG_MAX = 4 * 1024 * 1024
PACKAGE = "mcp-searxng"


def _runs_searxng(server: Any) -> bool:
    args = server.get("args") if isinstance(server, dict) else None
    return isinstance(args, list) and any(
        isinstance(arg, str) and (arg.lower() == PACKAGE or arg.lower().startswith(PACKAGE + "@")) for arg in args)


def searxng_url(config: Any) -> str | None:
    """The address of the SearXNG instance the config's mcp-searxng server searches; None when it has none."""
    servers = config.get("mcpServers") if isinstance(config, dict) else None
    if not isinstance(servers, dict):
        return None
    for server in servers.values():
        if _runs_searxng(server):
            env = server.get("env")
            url = env.get("SEARXNG_URL") if isinstance(env, dict) else None
            if isinstance(url, str) and url.strip():
                return url.strip().rstrip("/")
    return None


def own_config() -> dict | None:
    """The Claude config of the profile this server runs for (`$WINDOWS_MCP_DELTA_SESSION` names its folder)."""
    folder = os.environ.get("WINDOWS_MCP_DELTA_SESSION")
    if not folder:
        return None
    profile = claude.find_profile(folder)
    if profile is None or profile.storage is None:
        return None
    path = profile.storage / CONFIG
    try:
        if path.stat().st_size > CONFIG_MAX:
            return None
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return config if isinstance(config, dict) else None


def searxng_hint(url: str) -> str:
    return (f"SearXNG: this profile searches the web with the searxng server (mcp-searxng) on {url}; prefer its "
            "tools (searxng_web_search to search, web_url_read to read a page). When you query "
            f"{url}/search yourself, ask for format=json (if the instance allows it) and "
            "send the query in UTF-8. From Python, urllib.parse.urlencode or requests' params= encode it so; set "
            "PYTHONIOENCODING=utf-8 so that Chinese results print (this server's Python tool does). Git Bash's own "
            "curl (/mingw64/bin/curl) sends a Chinese query in the console's code page (GBK), which SearXNG reads "
            "as nonsense: there, use C:\\Windows\\System32\\curl.exe, or curl --data-urlencode q@file with the text "
            "in a UTF-8 file.")


def apply(mcp: Any) -> None:
    """The hints for this profile added to the server's instructions."""
    url = searxng_url(own_config())
    if not url:
        return
    current = (getattr(mcp, "instructions", None) or "").rstrip()
    mcp.instructions = (current + "\n\n" if current else "") + searxng_hint(url)
