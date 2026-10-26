"""Each session's task list, kept on disk so that it outlives a conversation's
Idle loop and the server's restarts.

One JSON file per session (the mailbox's session id) beside the mailbox:
tasks/<session>.json, a list of tasks with a small number for an id, a
subject, an optional description, a status (pending, in_progress, completed)
and when each was made and last changed. Written whole through a temporary
file, so a reader never sees half of it; another session's list can be read,
not written.
"""
from __future__ import annotations

import json
import os
import tempfile
import threading
import time
from pathlib import Path

from windows_mcp_delta import mailbox

STATUSES = ("pending", "in_progress", "completed")
_lock = threading.Lock()


def _file(session: str | None = None) -> Path:
    folder = mailbox.root().parent / "tasks"
    folder.mkdir(parents=True, exist_ok=True)
    return folder / f"{mailbox._safe(session) if session else mailbox.session_id()}.json"


def load(session: str | None = None) -> list[dict]:
    try:
        data = json.loads(_file(session).read_text(encoding="utf-8"))
        return data if isinstance(data, list) else []
    except (OSError, ValueError):
        return []


def _save(tasks: list[dict]) -> None:
    path = _file()
    handle, temporary = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
    with os.fdopen(handle, "w", encoding="utf-8") as file:
        json.dump(tasks, file, ensure_ascii=False, indent=1)
    os.replace(temporary, path)


def add(subjects: list[tuple[str, str | None]]) -> list[dict]:
    """Tasks made from (subject, description) pairs, pending; the ones made."""
    with _lock:
        tasks = load()
        next_id = max((t["id"] for t in tasks), default=0) + 1
        made = []
        for subject, description in subjects:
            task = {"id": next_id, "subject": subject.strip(), "status": "pending", "created": time.time(), "updated": time.time()}
            if description:
                task["description"] = description.strip()
            tasks.append(task)
            made.append(task)
            next_id += 1
        _save(tasks)
        return made


def update(task_id: int, status: str | None = None, subject: str | None = None, description: str | None = None) -> dict:
    """Task `task_id` changed; KeyError when there is none, ValueError for an unknown status."""
    if status is not None and status not in STATUSES:
        raise ValueError(f"status must be one of {', '.join(STATUSES)}")
    with _lock:
        tasks = load()
        for task in tasks:
            if task["id"] == task_id:
                if status is not None:
                    task["status"] = status
                if subject:
                    task["subject"] = subject.strip()
                if description is not None:
                    task["description"] = description.strip()
                task["updated"] = time.time()
                _save(tasks)
                return task
        raise KeyError(task_id)


def remove(task_ids: list[int] | None = None, completed: bool = False) -> int:
    """Tasks taken off the list: those of `task_ids`, or every completed one; how many."""
    with _lock:
        tasks = load()
        keep = [t for t in tasks if not ((task_ids and t["id"] in task_ids) or (completed and t["status"] == "completed"))]
        removed = len(tasks) - len(keep)
        if removed:
            _save(keep)
        return removed
