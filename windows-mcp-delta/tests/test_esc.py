from windows_mcp_delta import esc


def setup_function():
    esc.reset()


def test_esc_while_claude_acts_stops_it():
    esc.note_action(now=100.0)
    assert esc.pressed(now=105.0)
    assert esc.refused(now=110.0)


def test_esc_at_other_times_is_the_persons_own():
    assert not esc.pressed(now=5.0)                       # Claude never acted
    esc.note_action(now=100.0)
    assert not esc.pressed(now=100.0 + esc.ACTIVE + 1)    # it acted, but long ago
    assert not esc.refused(now=200.0)


def test_trying_again_keeps_it_stopped_until_it_lets_go():
    esc.note_action(now=100.0)
    esc.pressed(now=101.0)
    t = 101.0
    for _ in range(5):                                    # each try within HOLD of the last keeps it stopped
        t += esc.HOLD - 1
        assert esc.refused(now=t)
    assert not esc.refused(now=t + esc.HOLD + 0.1)        # left alone that long: the next request works


def test_the_hook_stays_out_of_the_tests(monkeypatch):
    monkeypatch.setenv("WINDOWS_MCP_DELTA_NO_ESC_HOOK", "1")
    esc.ensure_started()
    assert esc._thread is None
