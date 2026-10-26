"""PowerShell, over Windows-MCP's, and Jobs, JobOutput, JobStop.

Windows-MCP's PowerShell always starts in the home folder, gives back only
stdout when there is any (an error's message is lost), and stops a command
after its timeout. This one takes the same command and timeout and adds:
cwd (the shared working directory by default, and a `cd` in the command
moves it for the next call), stdout and stderr together, the exit code of
the last command, output trimmed as the other tools trim it, timeouts up to
ten minutes, and background=true to leave a long command running as a job
(jobs.py) — Wsl, Python and Node take background too.
"""
from __future__ import annotations

import asyncio
import base64
import os
import shutil
import subprocess
from typing import Annotated

from mcp.types import ToolAnnotations
from pydantic import Field

from windows_mcp_delta import jobs, state
from windows_mcp_delta.extend import delta
from windows_mcp_delta.tools import run

MARK = "@@windows-mcp-delta-cwd@@"
WAIT_MAX = 180


def _clean_path(value: str) -> str:
    """PATH with the entries of this server's own uv-run environment taken out, so a bare `python` (or `pip`) in a
    command finds the person's real interpreter, not the ephemeral one uv put us in (which has no pip or deps)."""
    import sys
    drop = []
    prefix = os.path.normcase(sys.prefix)
    base = os.path.normcase(getattr(sys, "base_prefix", sys.prefix))
    if prefix != base:   # a venv (uv's ephemeral one): its own folder, and the Scripts in it
        drop.append(prefix)
    entries = []
    for part in value.split(os.pathsep):
        if not part:
            continue
        low = os.path.normcase(part.rstrip("\\/"))
        if any(low == d or low.startswith(d + os.sep) for d in drop) or "\\uv\\cache\\" in low or "/uv/cache/" in low:
            continue
        entries.append(part)
    return os.pathsep.join(entries)


def _environment() -> dict:
    try:
        from windows_mcp.powershell.service import _prepare_env   # the variables an MCP host leaves out, put back
        env = _prepare_env()
    except Exception:
        env = dict(os.environ)
        env.pop("PYTHONHOME", None)
    env["NO_COLOR"] = "1"
    env["PYTHONUTF8"] = "1"                 # a bare `python` in the command prints UTF-8, whatever the code page
    env["PYTHONIOENCODING"] = "utf-8"
    for key in ("PATH", "Path"):
        if key in env:
            env[key] = _clean_path(env[key])
    return env


def _shell() -> list[str]:
    found = shutil.which("pwsh")
    if found:
        return [found, "-NoProfile", "-NonInteractive"]
    return [shutil.which("powershell") or "powershell", "-NoProfile", "-NonInteractive", "-OutputFormat", "Text"]


def _script(command: str, folder: str, track: bool) -> str:
    """`command` in UTF-8 output, run in `folder`; its last status the exit code; with `track`, the folder it ends
    in printed last (a finally, so that it is printed whatever happens)."""
    quoted = folder.replace("'", "''")
    head = ("$OutputEncoding = [System.Text.Encoding]::UTF8; [Console]::OutputEncoding = [System.Text.Encoding]::UTF8; "
            f"$ProgressPreference = 'SilentlyContinue'; Set-Location -LiteralPath '{quoted}'\n")
    body = f"{command}\n$__deltaOk = $?\n"
    if track:
        body = f"try {{\n{body}}} finally {{ [Console]::Out.WriteLine(\"`n{MARK}\" + (Get-Location).ProviderPath) }}\n"
    return head + body + "if (-not $__deltaOk) { if ($LASTEXITCODE) { exit $LASTEXITCODE } else { exit 1 } }\n"


def argv(command: str, folder: str, track: bool = True) -> list[str]:
    encoded = base64.b64encode(_script(command, folder, track).encode("utf-16-le")).decode("ascii")
    return _shell() + ["-EncodedCommand", encoded]


def _take_cwd(text: str) -> str:
    """The output without the folder line, the working directory moved to that folder."""
    at = text.rfind(MARK)
    if at < 0:
        return text
    line_end = text.find("\n", at)
    folder = text[at + len(MARK):line_end if line_end >= 0 else len(text)].strip()
    if folder and os.path.isdir(folder):
        try:
            state.set_cwd(folder)
        except ValueError:
            pass
    return (text[:at] + (text[line_end + 1:] if line_end >= 0 else "")).rstrip()


@delta.override(
    "PowerShell",
    description=("Runs PowerShell commands (pwsh when installed, else Windows PowerShell) and returns the exit code and "
                 "the output, errors included (colors removed, a very long output's middle left out). Give the folder "
                 "as cwd rather than starting the command with cd; without it, it runs where the last call left "
                 "off (a cwd, or a cd in the command, moves that for later calls). "
                 "timeout is in seconds, up to 600; background=true leaves a long command (a server, a build, a watch) "
                 "running as a job: see JobOutput, JobStop, Jobs, and Idle's job condition. Prefer Read, Grep, Glob, "
                 "Edit for files, and Python, Node, Wsl for scripts."),
)
async def powershell(
    command: Annotated[str, Field(description="The PowerShell commands.")],
    timeout: Annotated[int, Field(description="Seconds before it is stopped (at most 600). Raise it for slow work like a large file copy, or use background=true.")] = 120,
    cwd: Annotated[str, Field(description="The folder to run in, an absolute path (C:\\Projects\\app); empty: where the last call left off.")] = "",
    background: Annotated[bool, Field(description="Leave it running as a job and return at once.")] = False,
) -> str:
    if not command.strip():
        return "Error: command is empty."
    try:
        folder = run._windows_cwd(cwd)
    except ValueError as error:
        return f"Error: {error}"
    if background:
        return run.start_job(argv(command, folder, track=False), folder, command, _environment())
    result = await asyncio.to_thread(run._run, argv(command, folder), None, timeout, None, _environment())
    result = _take_cwd(result)
    if result.startswith("Timed out"):
        result += "\n(Run it with background=true to let it go on.)"
    return result


@delta.add(
    "Jobs",
    description="Lists the background jobs: id, running or how it ended, what was run, where, how much output.",
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False),
)
def list_jobs() -> str:
    every = jobs.all_jobs()
    if not every:
        return "No jobs."
    lines = []
    for job in every:
        label = job.label if len(job.label) <= 120 else job.label[:117] + "..."
        unread = job.size() - job.read_to
        lines.append(f"{job.id} · {job.status()} · {label}" + (f" · in {job.cwd}" if job.cwd else "") +
                     (f" · {unread} bytes unread" if unread > 0 else ""))
    return "\n".join(lines)


@delta.add(
    "JobOutput",
    description=("A background job's output: what is new since the last call (all=true: from the start; tail=N: the "
                 "last N characters), with whether it still runs. wait=N first waits up to N seconds (180 at most) "
                 "for it to end."),
    annotations=ToolAnnotations(readOnlyHint=True, idempotentHint=False, openWorldHint=False),
)
async def job_output(
    id: Annotated[str, Field(description="The job's id, e.g. j1.")],
    wait: Annotated[int, Field(description="Seconds to wait for it to end first.", ge=0)] = 0,
    all: Annotated[bool, Field(description="Everything from the start, not only what is new.")] = False,
    tail: Annotated[int | None, Field(description="Only the last this many characters.", ge=100)] = None,
) -> str:
    job = jobs.get(id)
    if job is None:
        return f"Error: no job {id!r}. Jobs lists them."
    if wait:
        await asyncio.to_thread(jobs.wait, job, min(wait, WAIT_MAX))
    if tail:
        text = run._decode(job.tail(tail * 4))
        text = run._trim(text)[-tail:]
        job.read_to = job.size()
    else:
        text = run._trim(run._decode(job.read(0 if all else None)))
    return f"Job {job.id}: {job.status()}\n" + (text if text else "(no new output)")


@delta.add(
    "JobStop",
    description="Ends a background job and every process it started; returns its last output.",
    annotations=ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=True, openWorldHint=False),
)
async def job_stop(
    id: Annotated[str, Field(description="The job's id, e.g. j1.")],
) -> str:
    job = jobs.get(id)
    if job is None:
        return f"Error: no job {id!r}. Jobs lists them."
    await asyncio.to_thread(jobs.stop, job)
    text = run._trim(run._decode(job.read()), 4000)
    return f"Job {job.id}: {job.status()}" + (f"\n{text}" if text else "")
