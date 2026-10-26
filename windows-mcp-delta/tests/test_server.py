import asyncio

from fastmcp import Client


def test_the_whole_server_builds(tmp_path):
    from windows_mcp_delta import mailbox
    mailbox.reset()
    mailbox.set_root(tmp_path / "mailbox")
    mailbox.set_session("me")
    from windows_mcp_delta import __main__ as entry
    from windows_mcp import __main__ as base
    mcp = base._build_mcp()

    async def go():
        async with Client(mcp) as client:
            tools = {t.name: t for t in await client.list_tools()}
            ran = await client.call_tool("TaskList", {})
            return tools, ran.content[0].text

    tools, ran = asyncio.run(go())
    expected = {"PowerShell", "FileSystem", "Wsl", "Python", "Node", "Idle", "TaskAdd", "TaskList", "TaskUpdate", "TaskRemove",
                "WhoAmI", "Profiles", "ListSessions", "SearchSessions", "ReadSession", "OpenSession", "SendMessage", "Inbox",
                "Read", "Write", "Edit", "Glob", "Grep", "Jobs", "JobOutput", "JobStop", "WebFetch", "WebSearch", "Download"}
    assert expected <= set(tools), expected - set(tools)
    assert "background" in tools["PowerShell"].input_schema["properties"]     # ours, over the base's
    assert {"command", "timeout"} <= set(tools["PowerShell"].input_schema["properties"])   # what called the base still works
    assert ran
    assert entry.delta.applied
    # each added tool asks Claude Desktop for the conversation id (anthropic/sessionIdKinds)
    assert (tools["WhoAmI"].meta or {}).get("anthropic/sessionIdKinds") == ["cse", "conversation"]
    assert (tools["SendMessage"].meta or {}).get("anthropic/sessionIdKinds") == ["cse", "conversation"]
    mailbox.reset()
