"""Wsl, Python and Node: a command in WSL, or a script run by Python or Node, on Windows or inside WSL.

They save Claude the PowerShell around them (quoting, encodings, finding the
interpreter, moving to a folder) and keep what comes back short: the exit
code, then the output with its terminal colors taken out, its middle left out
when it is long. Code is handed over in a temporary file, so no quoting
mangles it and no command-line length limits it.

Which interpreter: on Windows the person's own (the `py` launcher, else
`python` / `node` on PATH), not the Python windows-mcp-delta itself runs in,
which has none of their packages; inside WSL, `python3` and `node`.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from typing import Annotated

import psutil
from mcp.types import ToolAnnotations
from pydantic import Field

from windows_mcp_delta import jobs, state
from windows_mcp_delta.extend import delta

TIMEOUT_MAX = 600
STARTUP = 2.0               # a background command over this soon is reported as a finished one
OUTPUT_MAX = 30000          # characters back at most: the start and the end, the middle left out
_ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]|\x1b\][^\x07]*(\x07|\x1b\\)")
_NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
_WINDOWS = sys.platform == "win32"

_ANNOTATIONS = ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=True)


# ------------------------------------------------------------------ helpers

def _decode(data: bytes) -> str:
    """Program output as text: UTF-8 (UTF-16 when it has that shape: wsl.exe's own messages), else the ANSI code page."""
    if not data:
        return ""
    if data.startswith(b"\xff\xfe") or (len(data) >= 4 and data[1:2] == b"\x00" and data[3:4] == b"\x00"):
        try:
            return data.decode("utf-16-le").lstrip("\ufeff")
        except UnicodeDecodeError:
            pass
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        import locale
        return data.decode(locale.getpreferredencoding(False) or "latin-1", errors="replace")


def _trim(text: str, limit: int = OUTPUT_MAX) -> str:
    """Without terminal colors, carriage-return redraws or trailing blank lines; the middle left out past `limit`."""
    text = _ANSI.sub("", text).replace("\r\n", "\n")
    # A progress bar redraws its line with \r: only what it showed last stays.
    text = "\n".join(line.rsplit("\r", 1)[-1] for line in text.split("\n")).rstrip()
    if len(text) <= limit:
        return text
    head = text[: limit // 2]
    tail = text[-(limit // 2):]
    return f"{head}\n[... {len(text) - len(head) - len(tail)} characters left out ...]\n{tail}"


def _kill_tree(pid: int) -> None:
    try:
        parent = psutil.Process(pid)
        for child in parent.children(recursive=True):
            child.kill()
        parent.kill()
    except psutil.Error:
        pass


def _run(argv: list[str], cwd: str | None, timeout: int, stdin: str | None = None, env: dict | None = None) -> str:
    """`argv` run, without a shell and without a console window: its exit code and its output, trimmed."""
    timeout = max(1, min(int(timeout), TIMEOUT_MAX))
    try:
        process = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, creationflags=_NO_WINDOW)
    except FileNotFoundError:
        return f"Error: {argv[0]} was not found."
    except OSError as error:
        return f"Error: {argv[0]} could not start ({error})."
    try:
        out, _ = process.communicate(stdin.encode("utf-8") if stdin is not None else None, timeout=timeout)
    except subprocess.TimeoutExpired:
        _kill_tree(process.pid)
        out, _ = process.communicate()
        text = _trim(_decode(out))
        return f"Timed out after {timeout}s (stopped).\n{text}" if text else f"Timed out after {timeout}s (stopped)."
    text = _trim(_decode(out))
    return f"Exit code {process.returncode}" + (f"\n{text}" if text else " (no output)")


def start_job(job_argv: list[str], cwd: str | None, label: str, env: dict | None = None, stdin: str | None = None) -> str:
    """`job_argv` started as a job: what to say of it (its output when it is over within STARTUP seconds)."""
    try:
        job = jobs.start(job_argv, cwd, label, env, stdin)
    except OSError as error:
        return f"Error: {job_argv[0]} could not start ({error})."
    if jobs.wait(job, STARTUP):
        text = _trim(_decode(job.read()))
        return f"Job {job.id} ended at once: {job.status()}" + (f"\n{text}" if text else " (no output)")
    early = _trim(_decode(job.read()), 4000)
    return (f"Started job {job.id} (pid {job.process.pid}). JobOutput('{job.id}') reads its new output; Idle with "
            f"{{\"type\": \"job\", \"id\": \"{job.id}\"}} waits for it to end; JobStop ends it." +
            (f"\nSo far:\n{early}" if early else ""))


def to_wsl_path(path: str) -> str:
    """A Windows path as WSL sees it under /mnt (C:\\x\\y -> /mnt/c/x/y); a Linux path ("/...", "~...") as it is."""
    if path.startswith(("/", "~")):
        return path
    full = os.path.abspath(path) if _WINDOWS else path
    match = re.match(r"^([A-Za-z]):[\\/]?(.*)$", full)
    if not match:
        return full.replace("\\", "/")
    rest = match.group(2).replace("\\", "/")
    return f"/mnt/{match.group(1).lower()}/{rest}".rstrip("/") if rest else f"/mnt/{match.group(1).lower()}"


def _wsl_env() -> dict:
    """wsl.exe's own messages in UTF-8 rather than UTF-16 (WSL_UTF8), like the output of what it runs."""
    env = dict(os.environ)
    env["WSL_UTF8"] = "1"
    return env


def wsl_argv(command: str, distro: str | None, cwd: str | None) -> list[str]:
    """wsl.exe running `command` in a login bash of `distro` (its default when None), in `cwd` (WSL's home when None)."""
    argv = ["wsl.exe"]
    if distro:
        argv += ["-d", distro]
    if cwd:
        argv += ["--cd", to_wsl_path(cwd)]
    return argv + ["--", "bash", "-lc", command]


def _windows_cwd(cwd: str | None) -> str:
    target = state.resolve(cwd) if cwd else state.cwd()
    if not os.path.isdir(target):
        raise ValueError(f"Not a folder: {target}")
    return target


def _interpreter(kind: str, chosen: str | None) -> list[str] | None:
    """The person's own Python or Node on Windows: `chosen`, else the py launcher / python / node on PATH."""
    if chosen:
        return [chosen]
    if kind == "python":
        if _WINDOWS and shutil.which("py"):
            return ["py", "-3"]
        for name in ("python", "python3"):
            found = shutil.which(name)
            # Windows' "python.exe" in WindowsApps opens the Store when no Python is installed.
            if found and "WindowsApps" not in found:
                return [found]
        return None
    found = shutil.which("node")
    return [found] if found else None


def _script_run(kind: str, code: str | None, script: str | None, args: list[str] | None, cwd: str | None,
                wsl: bool, distro: str | None, timeout: int, stdin: str | None, interpreter: str | None,
                background: bool = False) -> str:
    if bool(code) == bool(script):
        return "Error: give either code or script (a file to run), not both."
    args = [str(a) for a in (args or [])]
    suffix = ".py" if kind == "python" else ".js"
    temporary = None
    try:
        if code:
            # A file the code is in: nothing to quote, no length limit. Python reads it as UTF-8.
            handle, temporary = tempfile.mkstemp(prefix="delta-", suffix=suffix)
            with os.fdopen(handle, "w", encoding="utf-8", newline="\n") as file:
                file.write(code)
            path = temporary
        else:
            path = script if (wsl and script.startswith(("/", "~"))) else state.resolve(script)
            if not (wsl and script.startswith(("/", "~"))) and not os.path.isfile(path):
                return f"Error: no such file: {path}"
        if wsl:
            runner = "python3" if kind == "python" else "node"
            command = " ".join(_sh_quote(part) for part in [runner, to_wsl_path(path), *args])
            if background:
                temporary = None   # the job reads it after we return: left in the temporary folder
                return start_job(wsl_argv(command, distro, cwd or state.cwd()), None, f"{runner} {script or '(code)'}",
                                 _wsl_env(), stdin)
            return _run(wsl_argv(command, distro, cwd or state.cwd()), None, timeout, stdin, _wsl_env())
        try:
            folder = _windows_cwd(cwd)
        except ValueError as error:
            return f"Error: {error}"
        runner = _interpreter(kind, interpreter)
        if not runner:
            return f"Error: no {'Python' if kind == 'python' else 'Node.js'} found on this PC (give its path as interpreter)."
        env = dict(os.environ)
        if kind == "python":
            env.update(PYTHONIOENCODING="utf-8", PYTHONUTF8="1")   # its output in UTF-8 whatever the console's code page
        if background:
            temporary = None
            return start_job(runner + [path, *args], folder, f"{kind} {script or '(code)'}", env, stdin)
        return _run(runner + [path, *args], folder, timeout, stdin, env)
    finally:
        if temporary:
            try:
                os.remove(temporary)
            except OSError:
                pass


def _sh_quote(text: str) -> str:
    return "'" + text.replace("'", "'\"'\"'") + "'"


# -------------------------------------------------------------------- tools

@delta.add(
    "Wsl",
    description=("Runs a bash command inside WSL (Windows Subsystem for Linux) and returns its exit code and output "
                 "(colors removed, the middle of a very long output left out). Use it for Linux tools, builds and scripts "
                 "rather than wrapping wsl.exe in PowerShell. cwd may be a Windows path (mapped to /mnt/...) or a Linux path; "
                 "by default it is the shared working directory. A login shell runs the command, so the distro's PATH applies."),
    annotations=_ANNOTATIONS,
)
def wsl(
    command: Annotated[str, Field(description="The bash command line, e.g. 'make -j8' or 'ls -la | head'.")],
    cwd: Annotated[str, Field(description="Folder to run in: a Windows path or a Linux one ('/home/me/app'); empty: the shared working directory.")] = "",
    distro: Annotated[str | None, Field(description="WSL distribution name; the default one when omitted.")] = None,
    timeout: Annotated[int, Field(description=f"Seconds before it is stopped (at most {TIMEOUT_MAX}).")] = 120,
    stdin: Annotated[str | None, Field(description="Text given to the command's standard input.")] = None,
    background: Annotated[bool, Field(description="Leave it running as a job and return at once (JobOutput reads it).")] = False,
) -> str:
    if not command.strip():
        return "Error: command is empty."
    folder = cwd if cwd and cwd.startswith(("/", "~")) else state.resolve(cwd) if cwd else state.cwd()
    if background:
        return start_job(wsl_argv(command, distro, folder), None, f"wsl: {command}", _wsl_env(), stdin)
    return _run(wsl_argv(command, distro, folder), None, timeout, stdin, _wsl_env())


@delta.add(
    "Python",
    description=("Runs Python code (code) or a .py file (script) with arguments, on Windows with the person's own Python "
                 "(the py launcher, or python on PATH) or inside WSL (wsl=true, python3), and returns its exit code and output "
                 "(UTF-8, trimmed). Prefer it to PowerShell for any Python: no quoting, any length of code. "
                 "cwd defaults to the shared working directory."),
    annotations=_ANNOTATIONS,
)
def python(
    code: Annotated[str | None, Field(description="Python source to run.")] = None,
    script: Annotated[str | None, Field(description="A .py file to run instead of code (a Linux path is allowed with wsl).")] = None,
    args: Annotated[list[str] | None, Field(description="Command-line arguments (sys.argv[1:]).")] = None,
    cwd: Annotated[str, Field(description="Folder to run in, an absolute path; empty: the shared working directory.")] = "",
    wsl: Annotated[bool, Field(description="Run inside WSL with python3 rather than on Windows.")] = False,
    distro: Annotated[str | None, Field(description="WSL distribution, with wsl=true.")] = None,
    timeout: Annotated[int, Field(description=f"Seconds before it is stopped (at most {TIMEOUT_MAX}).")] = 120,
    stdin: Annotated[str | None, Field(description="Text given to its standard input.")] = None,
    interpreter: Annotated[str | None, Field(description="A particular python.exe to use on Windows (a venv's, say).")] = None,
    background: Annotated[bool, Field(description="Leave it running as a job and return at once (JobOutput reads it).")] = False,
) -> str:
    return _script_run("python", code, script, args, cwd, wsl, distro, timeout, stdin, interpreter, background)


@delta.add(
    "Node",
    description=("Runs JavaScript (code) or a .js/.mjs file (script) with arguments in Node.js, on Windows or inside WSL "
                 "(wsl=true), and returns its exit code and output (trimmed). Prefer it to PowerShell for any Node: no quoting, "
                 "any length of code. cwd defaults to the shared working directory."),
    annotations=_ANNOTATIONS,
)
def node(
    code: Annotated[str | None, Field(description="JavaScript source to run (CommonJS; use script for an .mjs file).")] = None,
    script: Annotated[str | None, Field(description="A .js/.mjs file to run instead of code.")] = None,
    args: Annotated[list[str] | None, Field(description="Command-line arguments (process.argv.slice(2)).")] = None,
    cwd: Annotated[str, Field(description="Folder to run in, an absolute path; empty: the shared working directory.")] = "",
    wsl: Annotated[bool, Field(description="Run inside WSL rather than on Windows.")] = False,
    distro: Annotated[str | None, Field(description="WSL distribution, with wsl=true.")] = None,
    timeout: Annotated[int, Field(description=f"Seconds before it is stopped (at most {TIMEOUT_MAX}).")] = 120,
    stdin: Annotated[str | None, Field(description="Text given to its standard input.")] = None,
    interpreter: Annotated[str | None, Field(description="A particular node.exe to use on Windows.")] = None,
    background: Annotated[bool, Field(description="Leave it running as a job and return at once (JobOutput reads it).")] = False,
) -> str:
    return _script_run("node", code, script, args, cwd, wsl, distro, timeout, stdin, interpreter, background)
