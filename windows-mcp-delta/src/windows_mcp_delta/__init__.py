"""Windows-MCP-Delta: Windows-MCP as a base class.

Every tool and command of Windows-MCP is here as it is (`windows-mcp-delta
serve`, `--transport`, `--tools`, `auth`, `install` ...); a tool of its can be
overridden (same name, the same parameters and maybe more; the one it
replaces stays callable, as super) and tools are added (extend.py).
"""

__version__ = "0.1.0"
