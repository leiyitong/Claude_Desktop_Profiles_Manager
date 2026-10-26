import asyncio

from fastmcp import Client, FastMCP

from windows_mcp_delta import mailbox, tasks
import windows_mcp_delta.extend as extend
from windows_mcp_delta.tools import tasklist


def setup(tmp_path, who="me"):
    mailbox.reset()
    mailbox.set_root(tmp_path / "mailbox")
    mailbox.set_session(who)


def test_add_list_update_remove(tmp_path):
    setup(tmp_path)
    out = tasklist.task_add(items=[tasklist.NewTask(subject="Build the EXE"), tasklist.NewTask(subject="Run tests", description="core and localize")])
    assert "#1 [ ] Build the EXE" in out and "#2 [ ] Run tests" in out
    assert "#3" in tasklist.task_add(subject="Ship it")
    assert tasklist.task_update(1, status="in_progress") == "#1 [>] Build the EXE"
    assert tasklist.task_update(1, status="completed") == "#1 [x] Build the EXE"
    listing = tasklist.task_list(details=True)
    assert "#1 [x] Build the EXE" in listing and "    core and localize" in listing and "(1/3 done)" in listing
    assert tasklist.task_list(status="pending").count("\n") == 2   # two tasks and the count line
    assert tasklist.task_update(9, status="completed") == "Error: no task #9."
    assert tasklist.task_update(2, status="finished").startswith("Error: status must be")
    assert tasklist.task_remove(completed=True) == "Removed 1 task."
    assert tasklist.task_remove(ids=[2, 3]) == "Removed 2 tasks."
    assert tasklist.task_list() == "No tasks."
    assert tasklist.task_add().startswith("Error")


def test_survives_and_other_sessions_are_readable(tmp_path):
    setup(tmp_path, "PRO1")
    tasklist.task_add(subject="Translate the changelog")
    setup(tmp_path, "PRO2")
    assert tasklist.task_list() == "No tasks."                       # each session has its own
    assert "Translate the changelog" in tasklist.task_list(session="PRO1")
    setup(tmp_path, "PRO1")                                          # a restart: read back from disk
    assert tasks.load()[0]["subject"] == "Translate the changelog"
    assert tasklist.task_add(subject="Next")  .startswith("Added:\n#2")   # ids go on


def test_registered(tmp_path):
    setup(tmp_path)
    mcp = FastMCP(name="windows-mcp")
    fresh = extend.Delta()
    fresh._entries = [e for e in extend.delta._entries if e.name.startswith("Task")]
    fresh.apply(mcp)

    async def go():
        async with Client(mcp) as client:
            names = {t.name for t in await client.list_tools()}
            await client.call_tool("TaskAdd", {"items": [{"subject": "A"}, {"subject": "B"}]})
            result = await client.call_tool("TaskList", {})
            return names, result.content[0].text

    names, text = asyncio.run(go())
    assert {"TaskAdd", "TaskList", "TaskUpdate", "TaskRemove"} <= names
    assert "#1 [ ] A" in text and "#2 [ ] B" in text
