import asyncio
import os
import shutil
import sys
import time

import pytest

from windows_mcp_delta import jobs, mailbox, state
from windows_mcp_delta.tools import idle, run, shell


@pytest.fixture(autouse=True)
def clean(tmp_path):
    state.reset()
    mailbox.reset()
    mailbox.set_root(tmp_path / "home" / "mailbox")
    mailbox.set_session("me")
    jobs.reset()
    idle._last.clear()
    yield
    jobs.reset()
    state.reset()


def py(code):
    return [sys.executable, "-u", "-c", code]


def test_quick_job_reports_its_output():
    out = run.start_job(py("print('done quickly')"), None, "quick")
    assert out.startswith("Job j1 ended at once: exit code 0") and "done quickly" in out


def test_long_job_output_and_stop():
    out = run.start_job(py("import time\nprint('ready', flush=True)\nfor i in range(100):\n    print('tick', i, flush=True)\n    time.sleep(0.2)"),
                        None, "ticker")
    assert out.startswith("Started job j1") and "ready" in out
    time.sleep(0.7)
    first = asyncio.run(shell.job_output("j1"))
    assert first.startswith("Job j1: running for") and "tick" in first and "ready" not in first   # only what is new
    time.sleep(0.5)
    second = asyncio.run(shell.job_output("1"))
    assert "tick" in second and second.split("\n")[1] != first.split("\n")[1]
    assert "ready" in asyncio.run(shell.job_output("j1", all=True))
    assert "j1 · running" in shell.list_jobs() and "ticker" in shell.list_jobs()
    stopped = asyncio.run(shell.job_stop("j1"))
    assert stopped.startswith("Job j1: stopped after")
    assert asyncio.run(shell.job_output("j9")).startswith("Error: no job")


def test_wait_and_idle_job_condition():
    run.start_job(py("import time\ntime.sleep(3)\nprint('built')\nraise SystemExit(3)"), None, "build")
    out = asyncio.run(idle.idle({"type": "job", "id": "j1"}, timeout=20))
    assert out.startswith("Woke") and "job j1 ended: exit code 3" in out and "built" in out
    assert "no job" in asyncio.run(idle.idle({"type": "job", "id": "j7"}, timeout=2))
    run.start_job(py("import time\ntime.sleep(2.5)\nprint('ok')"), None, "short")
    out = asyncio.run(shell.job_output("j2", wait=10))
    assert "exit code 0" in out and "ok" in out


def test_python_tool_in_background(tmp_path):
    state.set_cwd(str(tmp_path))
    out = run.python(code="import time\ntime.sleep(3)\nprint('background python')", background=True, interpreter=sys.executable)
    assert out.startswith("Started job j1")
    assert "background python" in asyncio.run(shell.job_output("j1", wait=15))


def test_powershell_script_and_cwd_tracking(tmp_path):
    script = shell._script("Get-ChildItem", r"C:\it's here", track=True)
    assert "Set-Location -LiteralPath 'C:\\it''s here'" in script and shell.MARK in script and "finally" in script
    assert shell.MARK not in shell._script("x", "C:\\", track=False)
    target = tmp_path / "moved"
    target.mkdir()
    state.set_cwd(str(tmp_path))
    text = shell._take_cwd(f"hello\nworld\n\n{shell.MARK}{target}\n")
    assert text == "hello\nworld" and state.cwd() == str(target)


@pytest.mark.skipif(not shutil.which("pwsh"), reason="PowerShell is not installed here")
def test_powershell_runs(tmp_path):
    (tmp_path / "sub").mkdir()
    state.set_cwd(str(tmp_path))
    out = asyncio.run(shell.powershell("cd sub; Write-Output (Get-Location).Path; Write-Error 'oops'"))
    assert "sub" in out and "oops" in out
    assert state.cwd() == str(tmp_path / "sub")                               # a cd moves it for later calls
    assert asyncio.run(shell.powershell("Write-Output ok")).startswith("Exit code 0")
    assert asyncio.run(shell.powershell("exit 4")).startswith("Exit code 4")  # a native exit code is kept
    assert asyncio.run(shell.powershell("Get-Item no-such-file")).startswith("Exit code")   # a failing cmdlet is not Exit code 0
    assert "Exit code 0" not in asyncio.run(shell.powershell("Get-Item no-such-file"))
    assert asyncio.run(shell.powershell("cd (Get-Location).Path")) and state.cwd() == str(tmp_path / "sub")   # still there


def test_clean_path_drops_the_servers_own_venv(monkeypatch):
    # A bare `python` in a command must find the person's interpreter, not uv's ephemeral one we run in.
    monkeypatch.setattr(sys, "prefix", r"C:\uv\cache\builds-v0\abcd\.venv", raising=False)
    monkeypatch.setattr(sys, "base_prefix", r"C:\Program Files\Python314", raising=False)
    path = os.pathsep.join([
        r"C:\uv\cache\builds-v0\abcd\.venv\Scripts",
        r"C:\uv\cache\archive-v0\deadbeef\bin",
        r"C:\Program Files\Python314",
        r"C:\Windows\System32",
    ])
    cleaned = shell._clean_path(path).split(os.pathsep)
    assert r"C:\Program Files\Python314" in cleaned
    assert r"C:\Windows\System32" in cleaned
    assert not any(r"uv\cache" in p.lower() for p in cleaned)


def test_environment_sets_utf8_for_child_python(monkeypatch):
    monkeypatch.setenv("PATH", r"C:\Windows\System32")
    env = shell._environment()
    assert env.get("PYTHONUTF8") == "1" and env.get("PYTHONIOENCODING") == "utf-8"
