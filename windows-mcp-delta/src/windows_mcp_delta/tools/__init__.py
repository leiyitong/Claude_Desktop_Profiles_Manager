"""The tools windows-mcp-delta overrides or adds, one module per group; importing them registers them on `delta`."""
from windows_mcp_delta import caller
from windows_mcp_delta.extend import delta

delta.use(caller.Caller())   # notes who calls each tool (the conversation), before any of them runs

from windows_mcp_delta.tools import run  # noqa: E402,F401  Wsl, Python, Node
from windows_mcp_delta.tools import idle  # noqa: E402,F401  Idle, Sleep
from windows_mcp_delta.tools import tasklist  # noqa: E402,F401  TaskAdd, TaskList, TaskUpdate, TaskRemove
from windows_mcp_delta.tools import sessions  # noqa: E402,F401  WhoAmI, Profiles, ListSessions, SearchSessions, ReadSession, OpenSession
from windows_mcp_delta.tools import messages  # noqa: E402,F401  SendMessage, Inbox
from windows_mcp_delta.tools import files  # noqa: E402,F401  Read, Write, Edit, Glob, Grep
from windows_mcp_delta.tools import shell  # noqa: E402,F401  PowerShell (over Windows-MCP's), Jobs, JobOutput, JobStop
from windows_mcp_delta.tools import web  # noqa: E402,F401  WebFetch, WebSearch, Download
from windows_mcp_delta.tools import desktop  # noqa: E402,F401  Click, Type, Scroll, Move, Shortcut, MultiSelect, MultiEdit, Snapshot (over Windows-MCP's)

# How each tool touches the desktop, for Windows-MCP's ownership gate (Delta.desktop): none of these clicks or
# types, so none waits while the user is using the mouse or keyboard. Code that Wsl, Python and Node run, a job
# JobStop ends and the window OpenSession opens might, as PowerShell's might ("possible", Windows-MCP's own class).
delta.desktop("other", "Idle", "Sleep", "Inbox", "SendMessage", "WhoAmI", "Profiles", "ListSessions", "SearchSessions",
              "ReadSession", "TaskAdd", "TaskList", "TaskUpdate", "TaskRemove", "Read", "Write", "Edit", "Glob", "Grep",
              "Jobs", "JobOutput", "WebFetch", "WebSearch", "Download")
delta.desktop("possible", "Wsl", "Python", "Node", "JobStop", "OpenSession")
# Windows-MCP's own tools that only read, as its later table classes them: its 0.8.8 gates them all, so a Snapshot
# failed with USER_CONTROL whenever the mouse had moved.
delta.desktop("other", "Snapshot", "Screenshot", "Scrape", "FileSystem", "Registry", "Wait", "WaitFor", "DisplayInventory")
