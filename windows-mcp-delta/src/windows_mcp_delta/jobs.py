"""Background jobs: commands left running while the conversation goes on (a
dev server, a long build, a watch), their output kept in a log file.

A job is started by any tool that runs something (PowerShell, Wsl, Python,
Node) with background=true. Its output goes straight from the process to its
log (jobs/<profile>/<id>.log beside the mailbox), so nothing is lost while
no one reads; JobOutput reads what is new since the last read, Idle can wait
for a job to end ({"type": "job", "id": ...}), JobStop ends it with its
children. The jobs are this server's: they are listed until it exits (their
processes are not ended with it). Logs older than a week are removed.
"""
from __future__ import annotations

import itertools
import os
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

import psutil

from windows_mcp_delta import mailbox

LOG_DAYS = 7
_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
_GROUP = getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)


@dataclass
class Job:
    id: str
    label: str                     # what was run, as asked
    cwd: str
    process: subprocess.Popen
    log: Path
    started: float = field(default_factory=time.time)
    ended: float | None = None
    code: int | None = None
    read_to: int = 0               # the log's bytes JobOutput has given
    stopped: bool = False

    def poll(self) -> bool:
        """Whether it has ended (its end noted the first time it is seen)."""
        if self.ended is None and self.process.poll() is not None:
            self.ended, self.code = time.time(), self.process.returncode
        return self.ended is not None

    def status(self) -> str:
        if not self.poll():
            return f"running for {human(time.time() - self.started)} (pid {self.process.pid})"
        how = "stopped" if self.stopped else f"exit code {self.code}"
        return f"{how} after {human(self.ended - self.started)}"

    def size(self) -> int:
        try:
            return self.log.stat().st_size
        except OSError:
            return 0

    def read(self, start: int | None = None) -> bytes:
        """The log from `start` (where the last read ended by default), and the read moved to its end."""
        start = self.read_to if start is None else max(0, start)
        try:
            with open(self.log, "rb") as file:
                file.seek(start)
                data = file.read()
        except OSError:
            return b""
        self.read_to = start + len(data)
        return data

    def tail(self, count: int) -> bytes:
        """The last `count` bytes of the log, the read not moved."""
        size = self.size()
        try:
            with open(self.log, "rb") as file:
                file.seek(max(0, size - count))
                return file.read()
        except OSError:
            return b""


_jobs: dict[str, Job] = {}
_lock = threading.Lock()
_numbers = itertools.count(1)
_cleaned = False


def human(seconds: float) -> str:
    seconds = int(seconds)
    if seconds >= 3600:
        return f"{seconds // 3600}h {seconds % 3600 // 60}m"
    if seconds >= 60:
        return f"{seconds // 60}m {seconds % 60}s"
    return f"{seconds}s"


def _folder() -> Path:
    global _cleaned
    folder = mailbox.root().parent / "jobs" / mailbox.session_id()
    folder.mkdir(parents=True, exist_ok=True)
    if not _cleaned:
        _cleaned = True
        old = time.time() - LOG_DAYS * 86400
        for log in folder.glob("*.log"):
            try:
                if log.stat().st_mtime < old:
                    log.unlink()
            except OSError:
                pass
    return folder


def start(argv: list[str], cwd: str | None, label: str, env: dict | None = None, stdin: str | None = None) -> Job:
    """`argv` started in the background, its output (stdout and stderr) into a new log; OSError when it cannot start."""
    number = next(_numbers)
    log = _folder() / f"{int(time.time())}-{os.getpid()}-{number}.log"
    with open(log, "wb") as out:
        process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=out, stderr=subprocess.STDOUT,
                                   stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                                   creationflags=_NO_WINDOW | _GROUP)
    if stdin is not None:
        try:
            process.stdin.write(stdin.encode("utf-8"))
            process.stdin.close()
        except OSError:
            pass
    job = Job(f"j{number}", label, cwd or "", process, log)
    with _lock:
        _jobs[job.id] = job
    return job


def get(job_id: str) -> Job | None:
    key = job_id.strip().lower()
    with _lock:
        return _jobs.get(key) or _jobs.get(f"j{key}")


def all_jobs() -> list[Job]:
    with _lock:
        return list(_jobs.values())


def stop(job: Job) -> None:
    """`job` ended with every process it started."""
    if job.poll():
        return
    job.stopped = True
    try:
        parent = psutil.Process(job.process.pid)
        for child in parent.children(recursive=True):
            child.kill()
        parent.kill()
    except psutil.Error:
        pass
    try:
        job.process.wait(5)
    except subprocess.TimeoutExpired:
        pass
    job.poll()


def wait(job: Job, seconds: float) -> bool:
    """Whether `job` ended within `seconds`."""
    deadline = time.monotonic() + seconds
    while not job.poll():
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.2)
    return True


def reset() -> None:
    """Every job stopped and forgotten (tests)."""
    global _numbers
    for job in all_jobs():
        stop(job)
    with _lock:
        _jobs.clear()
    _numbers = itertools.count(1)
