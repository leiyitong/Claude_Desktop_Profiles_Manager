import json
from types import SimpleNamespace

from windows_mcp_delta import claude, hints


def config(url="https://searx.example.com", args=("-y", "mcp-searxng")):
    return {"mcpServers": {"windows": {"command": "uvx"},
                           "searxng": {"command": "npx.cmd", "args": list(args), "env": {"SEARXNG_URL": url}}}}


def test_searxng_url_from_the_entry():
    assert hints.searxng_url(config()) == "https://searx.example.com"
    assert hints.searxng_url(config("https://s.example.com/ ")) == "https://s.example.com"
    assert hints.searxng_url(config(args=("-y", "mcp-searxng@0.9.1"))) == "https://searx.example.com"


def test_no_searxng_no_url():
    assert hints.searxng_url(None) is None
    assert hints.searxng_url({}) is None
    assert hints.searxng_url(config(args=("-y", "mcp-searxng-fork"))) is None
    assert hints.searxng_url(config(url="")) is None


def test_the_hint_names_the_instance_and_the_encoding():
    text = hints.searxng_hint("https://searx.example.com")
    assert "https://searx.example.com/search" in text
    assert "UTF-8" in text and "PYTHONIOENCODING=utf-8" in text and "format=json" in text
    assert "C:\\Windows\\System32\\curl.exe" in text


def test_apply_reads_its_own_profile(tmp_path, monkeypatch):
    (tmp_path / hints.CONFIG).write_text(json.dumps(config()), encoding="utf-8")
    profile = claude.Profile("Claude-Work", "Work", tmp_path, tmp_path, False)
    monkeypatch.setenv("WINDOWS_MCP_DELTA_SESSION", "Claude-Work")
    monkeypatch.setattr(claude, "find_profile", lambda name: profile if name == "Claude-Work" else None)
    mcp = SimpleNamespace(instructions="Windows MCP server provides tools.")
    hints.apply(mcp)
    assert mcp.instructions.startswith("Windows MCP server provides tools.\n\nSearXNG: ")
    assert "https://searx.example.com" in mcp.instructions


def test_apply_leaves_a_profile_without_searxng(tmp_path, monkeypatch):
    (tmp_path / hints.CONFIG).write_text(json.dumps({"mcpServers": {}}), encoding="utf-8")
    profile = claude.Profile("Claude-Work", "Work", tmp_path, tmp_path, False)
    monkeypatch.setenv("WINDOWS_MCP_DELTA_SESSION", "Claude-Work")
    monkeypatch.setattr(claude, "find_profile", lambda name: profile)
    mcp = SimpleNamespace(instructions="as it was")
    hints.apply(mcp)
    assert mcp.instructions == "as it was"


def test_no_profile_named_no_hint(monkeypatch):
    monkeypatch.delenv("WINDOWS_MCP_DELTA_SESSION", raising=False)
    mcp = SimpleNamespace(instructions=None)
    hints.apply(mcp)
    assert mcp.instructions is None
