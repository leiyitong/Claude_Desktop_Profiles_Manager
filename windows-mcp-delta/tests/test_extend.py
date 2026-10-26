import asyncio

import pytest
from fastmcp import Client, FastMCP

from windows_mcp_delta.extend import Delta


def build(delta):
    mcp = FastMCP(name="windows-mcp")

    @mcp.tool(name="PowerShell", description="base description")
    def powershell_tool(command: str, timeout: int = 30) -> str:
        return f"base:{command}:{timeout}"

    delta.apply(mcp)
    return mcp


def call(mcp, name, args):
    async def run():
        async with Client(mcp) as client:
            result = await client.call_tool(name, args)
            return result.content[0].text
    return asyncio.run(run())


def tools(mcp):
    async def run():
        async with Client(mcp) as client:
            return {t.name: t for t in await client.list_tools()}
    return asyncio.run(run())


def test_override_replaces_and_calls_super():
    delta = Delta()

    @delta.override("PowerShell")
    def powershell(command: str, timeout: int = 30, cwd: str | None = None) -> str:
        return "delta+" + delta.base("PowerShell")(command=command, timeout=timeout) + f":{cwd}"

    mcp = build(delta)
    listed = tools(mcp)
    assert list(listed) == ["PowerShell"]
    assert listed["PowerShell"].description == "base description"   # kept from the base
    assert "cwd" in listed["PowerShell"].input_schema["properties"]     # an option added
    assert call(mcp, "PowerShell", {"command": "dir", "cwd": "C:\\"}) == "delta+base:dir:30:C:\\"


def test_add_and_conflicts():
    delta = Delta()

    @delta.add("Grep", description="search")
    def grep(pattern: str) -> str:
        return f"grep:{pattern}"

    mcp = build(delta)
    assert set(tools(mcp)) == {"PowerShell", "Grep"}
    assert call(mcp, "Grep", {"pattern": "x"}) == "grep:x"

    clash = Delta()
    clash.add("PowerShell")(lambda command: command)
    with pytest.raises(RuntimeError, match="should override"):
        build(clash)
    missing = Delta()
    missing.override("Nope")(lambda: "")
    with pytest.raises(RuntimeError, match="does not have"):
        build(missing)


def test_command_line_builds_with_delta():
    from windows_mcp import __main__ as base
    import windows_mcp_delta.__main__ as entry

    mcp = base._build_mcp()   # as `serve` calls it: the hook
    assert entry.delta.applied
    assert base._resolve_program()[-1] == "windows_mcp_delta"
    assert set(tools(mcp)) >= {"PowerShell", "FileSystem"}


def test_desktop_categories_join_the_ownership_table():
    from types import SimpleNamespace
    from windows_mcp_delta.extend import register_desktop

    policy = SimpleNamespace(DIRECT=frozenset({"Click"}), POSSIBLE=frozenset({"PowerShell"}), OTHER=frozenset({"Wait"}),
                             MODES={"App": {}}, CLASSIFIED_TOOLS=frozenset())
    delta = Delta()
    delta.desktop("other", "Idle", "Read")
    delta.desktop("possible", "Python")
    with pytest.raises(ValueError):
        delta.desktop("sometimes", "Nope")
    assert register_desktop(delta.desktop_categories(), policy)
    assert {"Idle", "Read", "Wait"} <= policy.OTHER and "Python" in policy.POSSIBLE and "Click" in policy.DIRECT
    assert {"Idle", "Python", "App", "ControlStatus"} <= policy.CLASSIFIED_TOOLS
    delta.desktop("direct", "Idle")   # a tool moves: it is in one set only
    register_desktop(delta.desktop_categories(), policy)
    assert "Idle" in policy.DIRECT and "Idle" not in policy.OTHER
    assert not register_desktop({"Idle": "other"}, SimpleNamespace())   # a Windows-MCP without the table


def test_every_delta_tool_says_how_it_touches_the_desktop():
    """A tool missing from the table would wait while the user moves the mouse (USER_CONTROL), as Idle did."""
    from windows_mcp_delta import tools  # noqa: F401  (registers them)
    from windows_mcp_delta.extend import delta

    added = {name for name, override in delta.names() if not override}
    assert added - set(delta.desktop_categories()) == set()


def test_the_gate_lets_delta_tools_through_while_the_user_works():
    try:
        from windows_mcp.tools import control_policy
    except ImportError:
        pytest.skip("this Windows-MCP has no ownership gate")
    from windows_mcp_delta import tools  # noqa: F401
    from windows_mcp_delta.extend import delta, register_desktop

    register_desktop(delta.desktop_categories())
    for name in ("Idle", "Read", "Grep", "Inbox", "WebFetch"):
        assert not control_policy.requires_control("relaxed", name, {})
        assert not control_policy.requires_control("balanced", name, {})
    assert not control_policy.requires_control("relaxed", "Python", {})
    assert control_policy.requires_control("balanced", "Python", {})
    assert control_policy.requires_control("relaxed", "Click", {})


def test_the_gate_lets_everything_through_and_esc_stops_actions(monkeypatch):
    """Claude acts while the person works; Esc pressed while it acts refuses its actions, never its reads."""
    from types import SimpleNamespace
    from fastmcp.exceptions import ToolError
    from windows_mcp_delta import esc
    from windows_mcp_delta.extend import let_through

    class Gate:
        async def on_call_tool(self, context, call_next):
            raise RuntimeError("USER_CONTROL")

    remembered = []
    me = SimpleNamespace(notifier=SimpleNamespace(remember=remembered.append))

    async def call_next(context):
        return f"ran {context.message.name}"

    def through(name):
        context = SimpleNamespace(message=SimpleNamespace(name=name, arguments={}))
        return asyncio.run(Gate.on_call_tool(me, context, call_next))

    esc.reset()
    monkeypatch.delenv("WINDOWS_MCP_DELTA_USER_CONTROL", raising=False)
    assert let_through({"Idle": "other", "Python": "possible"}, Gate)
    assert through("Click") == "ran Click" and through("Python") == "ran Python" and through("Idle") == "ran Idle"
    assert len(remembered) == 3
    assert esc.pressed()                              # Esc right after an action: Claude is stopped
    for acting in ("Click", "Type"):                  # only the desktop-input tools are stopped
        with pytest.raises(ToolError, match="STOPPED_BY_USER"):
            through(acting)
    assert through("Python") == "ran Python"          # a command is not a screen action: Esc leaves it alone
    assert through("Idle") == "ran Idle"              # reading goes on
    monkeypatch.setenv("WINDOWS_MCP_DELTA_USER_CONTROL", "1")   # Windows-MCP's own gate, kept on request
    esc.reset()
    with pytest.raises(RuntimeError, match="USER_CONTROL"):
        through("Click")
    assert through("Idle") == "ran Idle"
    assert let_through({"Read": "other"}, Gate)        # again: the same wrapper, its table grown
    assert through("Read") == "ran Read"
    assert not let_through({"Idle": "other"}, SimpleNamespace())   # a Windows-MCP without the gate
    esc.reset()


def test_the_real_gate_lets_delta_tools_through():
    try:
        from windows_mcp.tools.control_notifications import ControlToolGate
    except ImportError:
        pytest.skip("this Windows-MCP has no ownership gate")
    from types import SimpleNamespace
    from windows_mcp_delta import tools  # noqa: F401
    from windows_mcp_delta.extend import delta, let_through

    class Busy:   # a controller that says the user has the desktop
        def __getattr__(self, name):
            raise AssertionError(f"the gate asked the controller ({name}) for a tool it should let through")

    me = SimpleNamespace(notifier=SimpleNamespace(remember=lambda context: None), controller=Busy())

    async def call_next(context):
        return "ran"

    import os
    from windows_mcp_delta import esc
    os.environ.pop("WINDOWS_MCP_DELTA_USER_CONTROL", None)
    esc.reset()
    assert let_through(delta.desktop_categories())
    for name in ("Idle", "Sleep", "Read", "TaskList", "WebFetch", "Snapshot", "Screenshot", "Click", "Type"):
        context = SimpleNamespace(message=SimpleNamespace(name=name, arguments={}))
        assert asyncio.run(ControlToolGate.on_call_tool(me, context, call_next)) == "ran"
