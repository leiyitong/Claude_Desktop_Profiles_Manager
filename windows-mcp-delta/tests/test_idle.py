import asyncio
import subprocess
import sys
import threading
import time

from fastmcp import Client, FastMCP

from windows_mcp_delta import mailbox, state
import windows_mcp_delta.extend as extend
from windows_mcp_delta.tools import idle


def setup_function(tmp_path=None):
    state.reset()
    mailbox.reset()
    idle._last.clear()


def use_mailbox(tmp_path, who="me"):
    mailbox.set_root(tmp_path)
    mailbox.set_session(who)


def test_message_wakes_and_is_taken(tmp_path):
    use_mailbox(tmp_path)
    threading.Timer(0.3, lambda: mailbox.post("me", "build done", sender="PRO2")).start()
    out = asyncio.run(idle.idle(condition="message", timeout=5))
    assert out.startswith("Woke") and "build done" in out and "PRO2" in out
    assert mailbox.peek() == []   # consumed


def test_message_already_waiting_and_filtered(tmp_path):
    use_mailbox(tmp_path)
    mailbox.post("me", "from A", sender="A")
    mailbox.post("me", "from B", sender="B")
    out = asyncio.run(idle.idle({"type": "message", "from": "B"}, timeout=2))
    assert "from B" in out and "from A" not in out
    assert [m["from"] for m in mailbox.peek()] == ["A"]   # A's still there


def test_timeout_says_call_again(tmp_path):
    use_mailbox(tmp_path)
    out = asyncio.run(idle.idle({"type": "message", "from": "nobody"}, timeout=1))
    assert "Call Idle again" in out and "message from nobody" in out


def test_plain_pause(tmp_path):
    use_mailbox(tmp_path)
    t0 = time.monotonic()
    out = asyncio.run(idle.idle(timeout=1))
    assert time.monotonic() - t0 >= 0.9 and "Call Idle again" in out and "plain pause" in out


def test_file_appears(tmp_path):
    flag = tmp_path / "flag.txt"
    threading.Timer(0.3, lambda: flag.write_text("x")).start()
    out = asyncio.run(idle.idle({"type": "file", "path": str(flag), "state": "exists"}, timeout=5))
    assert "file appeared" in out


def test_file_changed(tmp_path):
    flag = tmp_path / "flag.txt"
    flag.write_text("one")
    def bump():
        time.sleep(0.3)
        flag.write_text("two")
        import os
        os.utime(flag, (time.time() + 2, time.time() + 2))
    threading.Thread(target=bump).start()
    out = asyncio.run(idle.idle({"type": "file", "path": str(flag), "state": "changed"}, timeout=5))
    assert "file changed" in out


def test_process_gone(tmp_path):
    proc = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(0.4)"])
    out = asyncio.run(idle.idle({"type": "process", "pid": proc.pid, "state": "gone"}, timeout=5))
    assert "process ended" in out


def test_command_probe(tmp_path):
    out = asyncio.run(idle.idle({"type": "command", "command": "echo hello123", "contains": "hello123", "interval": 2}, timeout=4))
    assert "contains" in out


def test_any_of_several(tmp_path):
    use_mailbox(tmp_path)
    flag = tmp_path / "f"
    threading.Timer(0.3, lambda: flag.write_text("x")).start()
    out = asyncio.run(idle.idle([{"type": "message"}, {"type": "file", "path": str(flag)}], timeout=5))
    assert "file appeared" in out


def test_reuse(tmp_path):
    use_mailbox(tmp_path)
    asyncio.run(idle.idle({"type": "message", "from": "Z"}, timeout=1))   # stores the condition
    mailbox.post("me", "yo", sender="Z")
    out = asyncio.run(idle.idle(reuse=True, timeout=3))
    assert "yo" in out
    idle._last.clear()
    assert "Nothing to reuse" in asyncio.run(idle.idle(reuse=True))


def test_registered_and_callable(tmp_path):
    use_mailbox(tmp_path)
    mailbox.post("me", "ping", sender="X")
    mcp = FastMCP(name="windows-mcp")
    fresh = extend.Delta()
    fresh._entries = [e for e in extend.delta._entries if e.name == "Idle"]
    fresh.apply(mcp)

    async def go():
        async with Client(mcp) as client:
            names = {t.name for t in await client.list_tools()}
            result = await client.call_tool("Idle", {"condition": "message", "timeout": 2})
            return names, result.content[0].text

    names, text = asyncio.run(go())
    assert "Idle" in names and "ping" in text


# ---------------------------------------------------------------- scheduled time

def test_parse_until_forms():
    now = time.time()
    assert abs(idle._parse_until("+90s", None) - (now + 90)) < 2
    assert abs(idle._parse_until("+5m", None) - (now + 300)) < 2
    assert abs(idle._parse_until("+2h", None) - (now + 7200)) < 2
    assert abs(idle._parse_until(None, 30) - (now + 30)) < 2
    assert idle._parse_until(1234.5, None) == 1234.5
    clock = idle._parse_until("00:00", None)
    assert now < clock <= now + 86400   # its next occurrence
    assert idle._parse_until(None, None) is None
    import pytest
    with pytest.raises(ValueError):
        idle._parse_until("next tuesday", None)
    assert idle._human(3725) == "1h 2m" and idle._human(65) == "1m 5s" and idle._human(4) == "4s"


def test_duration_reaches_the_schedule(tmp_path):
    use_mailbox(tmp_path)
    t0 = time.monotonic()
    out = asyncio.run(idle.idle(duration=1.5))
    assert out.startswith("Scheduled time reached") and 1.3 <= time.monotonic() - t0 < 3


def test_each_call_tells_what_is_left_and_thinking_time_counts(tmp_path):
    use_mailbox(tmp_path)
    first = asyncio.run(idle.idle(until="+6s", timeout=1))
    assert first.startswith("Still waiting:") and "left until" in first and "reuse=true" in first
    time.sleep(2)                                    # the agent thinking between calls
    second = asyncio.run(idle.idle(reuse=True, timeout=1))
    assert second.startswith("Still waiting:")
    left = lambda text: int(text.split("Still waiting: ")[1].split("s left")[0])
    assert left(second) <= left(first) - 2           # the time between the calls came off
    t0 = time.monotonic()
    while True:
        out = asyncio.run(idle.idle(reuse=True, timeout=1))
        if out.startswith("Scheduled time reached"):
            break
        assert time.monotonic() - t0 < 8
    # reached once: a later reuse is a plain wait again, not the schedule firing twice
    assert not asyncio.run(idle.idle(reuse=True, timeout=1)).startswith("Scheduled time reached")


def test_message_before_the_schedule_wakes_and_keeps_it(tmp_path):
    use_mailbox(tmp_path)
    threading.Timer(0.3, lambda: mailbox.post("me", "early", sender="PRO2")).start()
    out = asyncio.run(idle.idle(condition="message", until="+60s", timeout=5))
    assert out.startswith("Woke") and "early" in out and "schedule still holds" in out
    assert idle._last["deadline"] is not None


def test_bad_time_is_an_error(tmp_path):
    use_mailbox(tmp_path)
    assert asyncio.run(idle.idle(until="someday")).startswith("Error:")


# ---------------------------------------------------------------- Sleep

def test_sleep_wakes_at_its_time(tmp_path):
    t0 = time.monotonic()
    out = asyncio.run(idle.sleep(duration=1.2))
    assert time.monotonic() - t0 >= 1.0 and out.startswith("Scheduled time reached")


def test_sleep_on_across_calls(tmp_path, monkeypatch):
    monkeypatch.setattr(idle, "MAX", 1)   # one call sleeps a second at most here
    first = asyncio.run(idle.sleep(until="+4s"))
    assert first.startswith("Still sleeping:") and "Call Sleep()" in first and "left until" in first
    for _ in range(8):
        out = asyncio.run(idle.sleep())
        if out.startswith("Scheduled time reached"):
            break
        assert out.startswith("Still sleeping:")
    assert out.startswith("Scheduled time reached")
    assert "Nothing to sleep on" in asyncio.run(idle.sleep())   # reached once: not again


def test_sleep_on_needs_a_sleep_first(tmp_path):
    use_mailbox(tmp_path)
    assert "Nothing to sleep on" in asyncio.run(idle.sleep())
    asyncio.run(idle.idle(condition="message", until="+1h", timeout=1))   # an Idle's wait is not Sleep's to go on with
    assert "Nothing to sleep on" in asyncio.run(idle.sleep())
    assert asyncio.run(idle.sleep(until="next tuesday")).startswith("Error:")


def test_descriptions_say_when_to_wait():
    """A model asked "in 10 minutes tell me..." must find a tool for it, by name or by description."""
    from windows_mcp_delta.extend import delta
    entries = {e.name: e.options.get("description", "") for e in delta._entries}
    for name in ("Idle", "Sleep"):
        text = entries[name].lower()
        assert "in 10 minutes" in text and "instead of saying you cannot wait" in text
    assert "sleep" in entries["Idle"].lower() and "delay" in entries["Idle"].lower()
