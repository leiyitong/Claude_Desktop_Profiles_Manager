"""windows-mcp's __main__ as windows-mcp-delta relies on it: one server, built once by _build_mcp, a click group."""
import sys

import click
from fastmcp import FastMCP

_mcp = None


def _build_mcp():
    global _mcp
    if _mcp is not None:
        return _mcp
    _mcp = FastMCP(name="windows-mcp")

    @_mcp.tool(name="PowerShell", description="Run a PowerShell command (stand-in).")
    def powershell_tool(command: str, timeout: int = 30) -> str:
        return f"base ran {command!r} within {timeout}s"

    @_mcp.tool(name="FileSystem", description="Files (stand-in).")
    def file_system_tool(mode: str, path: str) -> str:
        return f"base {mode} {path}"

    # The actions and Snapshot windows-mcp-delta overrides (stand-ins: they act on nothing).
    for name in ("Click", "Type", "Scroll", "Move", "Shortcut", "MultiSelect", "MultiEdit", "Snapshot"):
        _mcp.tool(name=name, description=f"{name} (stand-in).")(lambda **kwargs: "base")

    return _mcp


def _resolve_program():
    return [sys.executable, "-m", "windows_mcp"]


@click.group()
def main():
    pass


@main.command()
@click.option("--transport", default="stdio")
def serve(transport):
    _build_mcp().run(transport=transport, show_banner=False)
