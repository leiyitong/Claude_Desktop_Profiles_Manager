# windows-mcp-delta

[Windows-MCP](https://github.com/CursorTouch/Windows-MCP), extended the way a subclass extends its base class: every Windows-MCP tool is still there under its name, some are overridden to do more, and new ones are added. Claude Delta sets it up for a profile in one click; it can also be run by hand:

```
uvx --from <this folder> windows-mcp-delta serve --transport stdio
```

## What it adds

| Group | Tools | What for |
| --- | --- | --- |
| Files | `Read`, `Write`, `Edit`, `Glob`, `Grep` | Files as Claude Code works with them: numbered lines, part of a long file, edits that send only what changes; encodings (UTF-8, UTF-16, GBK...) and line ends kept; nothing overwritten unread |
| Desktop | `Click`, `Type`, `Scroll`, `Move`, `Shortcut`, `MultiSelect`, `MultiEdit`, `Snapshot` (overridden) | Actions answer `ok`, with `verify=true` adding what changed on screen; Snapshot's UI part is a table of the elements to act on, `id\|type\|name\|x,y\|state` (the id is the label actions take), at most `limit` rows, `window=` to narrow, `diff=true` for only what changed, `mode="tree"` for the full tree `depth` levels deep |
| Shell | `PowerShell` (overridden), `Jobs`, `JobOutput`, `JobStop` | A working directory that `cd` moves, errors kept, timeouts up to ten minutes, long commands left running as background jobs |
| Run | `Wsl`, `Python`, `Node` | A command in WSL, or code run by Python or Node on Windows or in WSL, without quoting; in the background too |
| Web | `WebFetch`, `WebSearch`, `Download` | A page's main text as Markdown, a part at a time or only the passages wanted; a search; a file saved without passing through the conversation |
| Waiting | `Idle`, `Sleep` | Waits up to three minutes at a time for a message, a file, a process, a window, a command, a job, or a time (`until`, `duration`), so a conversation can stay alive and do scheduled work; `Sleep` is the timed wait alone, for "in 10 minutes, tell me..." |
| Plans | `TaskAdd`, `TaskList`, `TaskUpdate`, `TaskRemove` | A task list per profile, kept on disk across restarts |
| Sessions | `WhoAmI`, `Profiles`, `ListSessions`, `SearchSessions`, `ReadSession`, `OpenSession` | Every account's Claude Code sessions on this PC: listed, searched in full, read, and continued in any profile. Chats are listed and opened; with online access on (off by default), they are also read and searched from claude.ai through the profile's own session |
| Messages | `SendMessage`, `Inbox` | Messages between the conversations of this PC's profiles, to a profile or to one conversation; `Idle` wakes on them |

## Working beside Claude

Windows-MCP keeps the desktop for you: it refuses a call that would act on it while you used the mouse or keyboard
in the last 10 seconds (`USER_CONTROL`), and swallows your input while Claude acts. windows-mcp-delta lets Claude act
while you work instead (no wait, nothing held, no overlay), and gives you **Esc** to stop it: pressed while Claude is
acting, its clicks, typing and code are refused with `STOPPED_BY_USER`, telling it to stop and ask you, until it has
left the desktop alone for 15 seconds; its reads (Snapshot, files, Idle...) go on. The key still reaches the app, and
an Esc Claude sends itself does not count (`esc.py`, a keyboard hook that only looks).
`WINDOWS_MCP_DELTA_USER_CONTROL=1` in the server's environment brings Windows-MCP's own rule back.

## Writing a tool

```python
from windows_mcp_delta.extend import delta

@delta.add("Grep", description="...")              # a new tool
def grep(pattern: str, path: str | None = None) -> str: ...

@delta.override("PowerShell")                       # one of Windows-MCP's, done more
async def powershell(command: str, timeout: int = 30, cwd: str | None = None) -> str:
    ...
    return delta.base("PowerShell")(command=command, timeout=timeout)   # its super
```

An override keeps the base tool's parameters (it may add optional ones), so that whatever called the tool goes on working. Adding a name the base already has, or overriding one it lacks, fails when the server is built.

## State

Kept in `~/.windows-mcp-delta` (or `$WINDOWS_MCP_DELTA_HOME`): `mailbox/<profile>/` messages, `tasks/<profile>.json` task lists, `conversations/<profile>.json` the chats that used the tools, `jobs/<profile>/` background jobs' output. A profile is known by the folder of the Claude Desktop that started the server (`Claude`, `Claude-Work`), or `$WINDOWS_MCP_DELTA_SESSION`.

**Online access** (reading and searching claude.ai chats) is off until the user turns it on: Claude Delta sets `WINDOWS_MCP_DELTA_ONLINE=1`, or the user makes the file `~/.windows-mcp-delta/online-enabled`. It reads the profile's own claude.ai session from its cookie store (`sessionKey`), so it reaches only the account already signed in; nothing is written.

## Tests

```
python -m pytest tests
```

They run on Linux too, against a stand-in for Windows-MCP (`tests/stub`) when the real one is not installed.
