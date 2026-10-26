import asyncio
import os

from fastmcp import Client, FastMCP

from windows_mcp_delta import state
from windows_mcp_delta.tools import run


def setup_function():
    state.reset()


def test_wsl_paths_and_command():
    assert run.to_wsl_path("C:\\Users\\Me\\My App") == "/mnt/c/Users/Me/My App"
    assert run.to_wsl_path("D:\\") == "/mnt/d"
    assert run.to_wsl_path("/home/me/app") == "/home/me/app"
    assert run.to_wsl_path("~/src") == "~/src"
    assert run.wsl_argv("make -j8", "Ubuntu", "C:\\src\\proj") == ["wsl.exe", "-d", "Ubuntu", "--cd", "/mnt/c/src/proj", "--", "bash", "-lc", "make -j8"]
    assert run.wsl_argv("ls", None, None) == ["wsl.exe", "--", "bash", "-lc", "ls"]


def test_output_kept_short():
    assert run._trim("\x1b[31mred\x1b[0m\r\n") == "red"
    assert run._trim("10%\r50%\r100%\ndone") == "100%\ndone"
    long = "a" * 50000
    trimmed = run._trim(long, 1000)
    assert len(trimmed) < 1100 and "characters left out" in trimmed
    assert run._decode("wsl: 错误".encode("utf-16-le")) == "wsl: 错误"
    assert run._decode("中文 ok".encode("utf-8")) == "中文 ok"


def test_python_code_script_args_cwd_stdin(tmp_path):
    out = run.python(code="import sys, os\nprint('你好', sys.argv[1:], os.getcwd())\nprint(input())", args=["a b", "c"],
                     cwd=str(tmp_path), stdin="from stdin\n")
    assert out.startswith("Exit code 0"), out
    assert "你好 ['a b', 'c']" in out and str(tmp_path) in out and "from stdin" in out
    script = tmp_path / "s p a c e.py"
    script.write_text("import sys\nsys.exit(3)", encoding="utf-8")
    assert run.python(script=str(script)).startswith("Exit code 3")
    assert "both" in run.python(code="1", script=str(script))
    assert "no such file" in run.python(script=str(tmp_path / "missing.py"))
    assert "Not a folder" in run.python(code="1", cwd=str(tmp_path / "nope"))


def test_python_timeout_stops_it():
    out = run.python(code="import time\nprint('started', flush=True)\ntime.sleep(30)", timeout=1)
    assert out.startswith("Timed out after 1s") and "started" in out


def test_node(tmp_path):
    out = run.node(code="console.log('node', process.argv.slice(2).join(','), process.cwd())", args=["x", "y"], cwd=str(tmp_path))
    assert out.startswith("Exit code 0") and "node x,y" in out and str(tmp_path) in out


def test_in_wsl_hands_the_file_over(monkeypatch):
    seen = {}

    def fake_run(argv, cwd, timeout, stdin=None, env=None):
        seen["argv"] = argv
        seen["env"] = env
        return "Exit code 0"

    monkeypatch.setattr(run, "_run", fake_run)
    monkeypatch.setattr(run, "to_wsl_path", lambda p: p if p.startswith(("/", "~")) else "/mnt/c/x" + os.path.basename(p))
    assert run.python(code="print(1)", args=["it's"], wsl=True, distro="Ubuntu", cwd="/home/me") == "Exit code 0"
    argv = seen["argv"]
    assert seen["env"]["WSL_UTF8"] == "1"
    assert argv[:6] == ["wsl.exe", "-d", "Ubuntu", "--cd", "/home/me", "--"]
    assert argv[-1].startswith("'python3' '") and argv[-1].endswith(".py' 'it'\"'\"'s'")


def test_registered_as_tools():
    from windows_mcp_delta.extend import Delta
    import windows_mcp_delta.extend as extend

    mcp = FastMCP(name="windows-mcp")
    fresh = Delta()
    fresh._entries = [e for e in extend.delta._entries if e.name in ("Wsl", "Python", "Node")]
    fresh.apply(mcp)

    async def go():
        async with Client(mcp) as client:
            names = {t.name for t in await client.list_tools()}
            result = await client.call_tool("Python", {"code": "print(6*7)"})
            return names, result.content[0].text
    names, text = asyncio.run(go())
    assert {"Wsl", "Python", "Node"} <= names
    assert text.startswith("Exit code 0") and "42" in text
