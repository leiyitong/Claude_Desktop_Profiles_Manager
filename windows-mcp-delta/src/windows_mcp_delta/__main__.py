"""windows-mcp-delta's command line: windows-mcp's own, its server built with our tools.

windows-mcp builds its one server in `_build_mcp`, which every command that
serves calls: ours calls it, then makes the overrides and additions on what
it built. Its scheduled task (`install`) starts this package rather than
windows-mcp. The rest of the command line is windows-mcp's, unchanged.
"""
import sys

from windows_mcp import __main__ as base

from windows_mcp_delta.extend import delta
from windows_mcp_delta import hints
from windows_mcp_delta import tools  # noqa: F401  (its modules register their tools on `delta`)

_base_build_mcp = base._build_mcp


def _build_mcp():
    mcp = _base_build_mcp()
    delta.apply(mcp)
    try:
        hints.apply(mcp)
    except Exception:   # a hint never keeps the server from starting
        pass
    return mcp


base._build_mcp = _build_mcp
if hasattr(base, "_resolve_program"):
    base._resolve_program = lambda: [sys.executable, "-m", "windows_mcp_delta"]

main = base.main

if __name__ == "__main__":
    main()
