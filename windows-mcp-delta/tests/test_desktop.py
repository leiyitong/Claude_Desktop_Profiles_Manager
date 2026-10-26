import asyncio
from types import SimpleNamespace

from windows_mcp_delta.tools import desktop


def node(name, kind="Button", window="Notepad", x=10, y=20, **metadata):
    return SimpleNamespace(name=name, control_type=kind, window_name=window, center=SimpleNamespace(x=x, y=y), metadata=metadata)


def tree(interactive, scrollable=()):
    return SimpleNamespace(interactive_nodes=list(interactive), scrollable_nodes=list(scrollable))


def test_rows_are_numbered_as_labels_are():
    rows = desktop.rows_of(tree([node("Save"), node("Text", "Edit", has_focused=True, value="hello")],
                                [node("Page", "Pane", vertical_scrollable=True, vertical_scroll_percent=30)]))
    assert [r.id for r in rows] == [0, 1, 2]   # interactive first, then scrollable: get_coordinates_from_label's order
    assert rows[0].line() == "0|button|Save|10,20|"
    assert rows[1].line() == '1|edit|Text|10,20|focused value:"hello"'
    assert rows[2].line() == "2|pane|Page|10,20|v:30%"


def test_a_row_is_one_line_and_short():
    rows = desktop.rows_of(tree([node("a|b\nc" + "x" * 100, value="v" * 100)]))
    line = rows[0].line()
    assert "\n" not in line and line.count("|") == 4
    assert len(rows[0].name) <= desktop.NAME_MAX and "…" in rows[0].name


def test_the_table_groups_by_window_limits_and_filters():
    rows = desktop.rows_of(tree([node(f"b{i}", window="A" if i < 3 else "B") for i in range(6)]))
    text = desktop.table(rows, 4)
    assert text.splitlines()[0] == "id|type|name|x,y|state"
    assert "# A" in text and "# B" in text and "3|button|b3" in text and "b4" not in text
    assert "(+2 more" in text
    only_b = desktop.table(rows, 50, window="b")
    assert "# A" not in only_b and "5|button|b5" in only_b
    assert "No elements" in desktop.table(rows, 50, window="nothing")


def test_diff_says_what_changed():
    before = desktop.rows_of(tree([node("Save"), node("Cancel"), node("Text", "Edit", value="a")]))
    after = desktop.rows_of(tree([node("Save"), node("Text", "Edit", value="ab"), node("OK", x=50)]))
    text = desktop.diff_rows(before, after, 50)
    lines = text.splitlines()
    assert lines[0] == "diff: +1 ~1 -1"
    assert "+2|button|OK|50,20|" in lines        # the new table's id
    assert '~1|edit|Text|10,20|value:"ab"' in lines
    assert "-button|Cancel|Notepad" in lines
    assert "Save" not in text                    # unchanged rows stay out
    assert desktop.diff_rows(after, after, 50) == "No change since the previous Snapshot."
    assert desktop.diff_rows(None, after, 50).startswith("No earlier Snapshot")


def test_the_header_is_one_line():
    state = SimpleNamespace(cursor_position=(5, 6), active_window=SimpleNamespace(name="Notepad"),
                            windows=[SimpleNamespace(name=f"W{i}") for i in range(10)])
    text = desktop.header(state)
    assert text == "cursor 5,6 · focused: Notepad · windows: W0, W1, W2, W3, W4, W5, W6, W7 (+2)"


def test_actions_answer_ok_and_verify_adds_the_diff(monkeypatch):
    calls = []

    def base(name):
        async def run(**kwargs):
            calls.append((name, kwargs))
            return "a long answer that repeats everything"
        return run

    monkeypatch.setattr(desktop.delta, "base", base)
    out = asyncio.run(desktop.type_(text="hello world", loc=[1, 2]))
    assert out == "ok" and calls[-1] == ("Type", {"text": "hello world", "loc": [1, 2], "label": None, "clear": False,
                                                   "caret_position": "idle", "press_enter": False})
    assert asyncio.run(desktop.click(label=3)) == "ok"

    monkeypatch.setattr(desktop, "SETTLE", 0)
    rows = desktop.rows_of(tree([node("Save"), node("OK")]))

    async def fresh():
        return rows
    monkeypatch.setattr(desktop, "_snapshot_rows", fresh)
    monkeypatch.setattr(desktop, "_previous", desktop.rows_of(tree([node("Save")])))
    out = asyncio.run(desktop.shortcut(shortcut="enter", verify=True))
    assert out.startswith("ok\ndiff: +1 ~0 -0") and "+1|button|OK" in out
    assert desktop._previous == rows             # the next diff compares with this screen
