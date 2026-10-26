"""TaskAdd, TaskList, TaskUpdate, TaskRemove: the session's own task list (tasks.py).

For work that spans many steps or a long Idle loop: the list is on disk, so
the plan survives the conversation's own context being trimmed and the
server restarting. What they return is short (one line per task) to keep the
tokens down; TaskList(session=...) reads another account's list.
"""
from __future__ import annotations

from typing import Annotated

from mcp.types import ToolAnnotations
from pydantic import BaseModel, Field

from windows_mcp_delta import tasks
from windows_mcp_delta.extend import delta

_MARK = {"pending": "[ ]", "in_progress": "[>]", "completed": "[x]"}


def _line(task: dict) -> str:
    return f"#{task['id']} {_MARK.get(task['status'], '[?]')} {task['subject']}"


class NewTask(BaseModel):
    subject: str = Field(description="What to do, in a few words, in the imperative.")
    description: str | None = Field(default=None, description="Details, when the subject is not enough.")


@delta.add(
    "TaskAdd",
    description=("Adds one or more tasks to this session's task list (kept on disk across restarts and long Idle loops). "
                 "Use it to plan work of three or more steps, then TaskUpdate each as you start and finish it."),
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=False, openWorldHint=False),
)
def task_add(
    items: Annotated[list[NewTask] | None, Field(description="Several tasks at once.")] = None,
    subject: Annotated[str | None, Field(description="One task's subject (instead of items).")] = None,
    description: Annotated[str | None, Field(description="That task's details.")] = None,
) -> str:
    pairs = [(i.subject, i.description) for i in (items or []) if i.subject.strip()]
    if subject and subject.strip():
        pairs.append((subject, description))
    if not pairs:
        return "Error: give a subject, or items."
    made = tasks.add(pairs)
    return "Added:\n" + "\n".join(_line(t) for t in made)


@delta.add(
    "TaskList",
    description=("Lists the task list, one line per task: #id, [ ] pending / [>] in progress / [x] completed, subject. "
                 "Pass session to read another account's list (read-only), status to show only some, details to see "
                 "descriptions."),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False),
)
def task_list(
    status: Annotated[str | None, Field(description="Only tasks with this status: pending, in_progress or completed.")] = None,
    details: Annotated[bool, Field(description="Show each task's description under it.")] = False,
    session: Annotated[str | None, Field(description="Another session's (account's) id, to read its list.")] = None,
) -> str:
    listed = [t for t in tasks.load(session) if not status or t["status"] == status]
    if not listed:
        return "No tasks." if not status else f"No {status} tasks."
    lines = []
    for task in listed:
        lines.append(_line(task))
        if details and task.get("description"):
            lines.append("    " + task["description"].replace("\n", "\n    "))
    done = sum(1 for t in listed if t["status"] == "completed")
    return "\n".join(lines) + f"\n({done}/{len(listed)} done)"


@delta.add(
    "TaskUpdate",
    description="Changes a task: its status (pending, in_progress, completed), subject or description. Mark a task in_progress before starting it and completed once it is done.",
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=False, idempotentHint=True, openWorldHint=False),
)
def task_update(
    id: Annotated[int, Field(description="The task's number.")],
    status: Annotated[str | None, Field(description="pending, in_progress or completed.")] = None,
    subject: Annotated[str | None, Field(description="A new subject.")] = None,
    description: Annotated[str | None, Field(description="A new description.")] = None,
) -> str:
    try:
        return _line(tasks.update(id, status, subject, description))
    except KeyError:
        return f"Error: no task #{id}."
    except ValueError as error:
        return f"Error: {error}."


@delta.add(
    "TaskRemove",
    description="Takes tasks off the list: those of ids, or every completed one (completed=true).",
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=True, openWorldHint=False),
)
def task_remove(
    ids: Annotated[list[int] | None, Field(description="The numbers of the tasks to remove.")] = None,
    completed: Annotated[bool, Field(description="Remove every completed task.")] = False,
) -> str:
    if not ids and not completed:
        return "Error: give ids, or completed=true."
    count = tasks.remove(ids, completed)
    return f"Removed {count} task{'s' if count != 1 else ''}."
