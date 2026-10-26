"""Click, Type and the other actions answer "ok"; Snapshot answers a table of the elements to act on.

Every answer is read by the model in full on each call, so each says what it needs and no more. An action's
answer repeated what it was given (the text typed, the coordinates); it is "ok" now, and verify=true adds what
changed on screen after it, as a Snapshot diff. Snapshot's UI tree drew every level of every window with tree
lines and key names, often larger than the screenshot itself; by default it is now the elements one can act on
(interactive, then scrollable), one row each, `id|type|name|x,y|state`, at most `limit` rows, a window's rows
under its title. The id is the label Click(label=...) and the others take. diff=true answers only the rows that
changed since the previous Snapshot; mode="tree" keeps Windows-MCP's tree, `depth` levels deep.
"""
from __future__ import annotations

import asyncio
from dataclasses import dataclass
from typing import Annotated, Any, Literal

from pydantic import Field

from windows_mcp_delta.extend import delta

LIMIT = 150          # rows a table shows at most, by default
NAME_MAX = 60        # characters of an element's name a row keeps
VALUE_MAX = 40       # characters of its value
SETTLE = 0.4         # seconds an action's effect is given to show before verify reads the screen
WINDOWS_SHOWN = 8    # opened windows the header names

VERIFY = (" Returns ok. Set verify=true to get what changed on screen afterwards (a Snapshot diff: the rows added, "
          "removed or changed), instead of calling Snapshot again.")


@dataclass(frozen=True)
class Row:
    id: int
    window: str
    type: str
    name: str
    x: int
    y: int
    state: str

    def key(self) -> tuple[str, str, str]:
        return self.window, self.type, self.name

    def line(self) -> str:
        return f"{self.id}|{self.type}|{self.name}|{self.x},{self.y}|{self.state}"


_previous: list[Row] | None = None   # the rows of the latest Snapshot, for diff


def _desktop() -> Any:
    try:
        from windows_mcp import __main__ as base
    except ImportError:
        return None
    return getattr(base, "desktop", None)


def _clean(text: Any, limit: int) -> str:
    """One line, no column separator, at most `limit` characters."""
    text = " ".join(str(text or "").replace("|", "/").split())
    return text if len(text) <= limit else text[: limit - 1] + "…"


def _state(metadata: dict, scrollable: bool) -> str:
    parts = []
    if metadata.get("has_focused"):
        parts.append("focused")
    if scrollable:
        if metadata.get("vertical_scrollable"):
            parts.append(f"v:{metadata.get('vertical_scroll_percent', 0)}%")
        if metadata.get("horizontal_scrollable"):
            parts.append(f"h:{metadata.get('horizontal_scroll_percent', 0)}%")
        return " ".join(parts)
    if metadata.get("is_password"):
        parts.append("password")
    value = metadata.get("value")
    if value is not None and value not in ("", "(empty)"):
        parts.append(f'value:"{_clean(value, VALUE_MAX)}"')
    if metadata.get("min") is not None and metadata.get("max") is not None:
        parts.append(f"range:{metadata['min']}-{metadata['max']}")
    toggle = metadata.get("toggle_state")
    if toggle:
        parts.append(str(toggle).lower())
    return " ".join(parts)


def rows_of(tree_state: Any) -> list[Row]:
    """The elements one can act on, numbered as labels are: the interactive ones, then the scrollable ones."""
    rows: list[Row] = []
    interactive = list(getattr(tree_state, "interactive_nodes", None) or [])
    scrollable = list(getattr(tree_state, "scrollable_nodes", None) or [])
    for index, node in enumerate(interactive + scrollable):
        center = getattr(node, "center", None)
        rows.append(Row(
            id=index,
            window=_clean(getattr(node, "window_name", ""), NAME_MAX),
            type=_clean(getattr(node, "control_type", ""), 24).lower(),
            name=_clean(getattr(node, "name", ""), NAME_MAX),
            x=int(getattr(center, "x", 0) or 0),
            y=int(getattr(center, "y", 0) or 0),
            state=_state(getattr(node, "metadata", None) or {}, index >= len(interactive)),
        ))
    return rows


def _grouped(rows: list[Row]) -> list[str]:
    lines, window = [], None
    for row in rows:
        if row.window != window:
            window = row.window
            lines.append(f"# {window or '(no window)'}")
        lines.append(row.line())
    return lines


def table(rows: list[Row], limit: int, window: str | None = None) -> str:
    """The rows as text: at most `limit` of them, those of windows whose title holds `window` when given."""
    if window:
        rows = [r for r in rows if window.lower() in r.window.lower()]
    if not rows:
        return "No elements to act on" + (f" in a window matching {window!r}." if window else ".")
    shown = rows[: max(1, limit)]
    lines = ["id|type|name|x,y|state", *_grouped(shown)]
    if len(rows) > len(shown):
        lines.append(f"(+{len(rows) - len(shown)} more: raise limit, or pass window=... to narrow)")
    return "\n".join(lines)


def diff_rows(before: list[Row] | None, after: list[Row], limit: int) -> str:
    """What changed between two tables: rows added (+), changed (~, same element elsewhere or in another state)
    and removed (-, by its old description: its id is gone). An element is its window, type and name, counted
    in order where several share them. Ids are the new table's, the ones labels now resolve to."""
    if before is None:
        return "No earlier Snapshot to compare with: the whole table.\n" + table(after, limit)

    def keyed(rows: list[Row]) -> dict[tuple, Row]:
        seen: dict[tuple, int] = {}
        out = {}
        for row in rows:
            n = seen.get(row.key(), 0)
            seen[row.key()] = n + 1
            out[(*row.key(), n)] = row
        return out

    old, new = keyed(before), keyed(after)
    added = [r for k, r in new.items() if k not in old]
    changed = [r for k, r in new.items() if k in old and (old[k].x, old[k].y, old[k].state) != (r.x, r.y, r.state)]
    removed = [r for k, r in old.items() if k not in new]
    if not (added or changed or removed):
        return "No change since the previous Snapshot."
    lines = [f"diff: +{len(added)} ~{len(changed)} -{len(removed)}"]
    budget = max(1, limit)
    for mark, rows in (("+", added), ("~", changed)):
        for row in rows[:budget]:
            lines.append(mark + row.line())
        budget -= min(budget, len(rows))
    for row in removed[: max(0, budget)]:
        lines.append(f"-{row.type}|{row.name}|{row.window}")
    total = len(added) + len(changed) + len(removed)
    if total > limit:
        lines.append(f"(+{total - limit} more changes: raise limit)")
    return "\n".join(lines)


def header(state: Any) -> str:
    cursor = getattr(state, "cursor_position", None)
    active = getattr(getattr(state, "active_window", None), "name", "") or "none"
    names = [_clean(getattr(w, "name", ""), 40) for w in (getattr(state, "windows", None) or [])]
    names = [n for n in names if n]
    shown = ", ".join(names[:WINDOWS_SHOWN]) + (f" (+{len(names) - WINDOWS_SHOWN})" if len(names) > WINDOWS_SHOWN else "")
    parts = []
    if cursor:
        parts.append(f"cursor {cursor[0]},{cursor[1]}")
    parts.append(f"focused: {_clean(active, 60)}")
    if shown:
        parts.append(f"windows: {shown}")
    return " · ".join(parts)


def _tree(state: Any, depth: int | None) -> str | None:
    """Windows-MCP's UI tree, `depth` levels below the desktop at most; None when it cannot be drawn here."""
    try:
        from windows_mcp.tree.views import _format_semantic_node
    except ImportError:
        return None
    root = getattr(getattr(state, "tree_state", None), "semantic_tree_root", None)
    if root is None:
        return "No elements"
    lines: list[str] = []

    def walk(node, prefix: str, last: bool, level: int) -> None:
        if node.element_type == "desktop":
            lines.append("desktop")
        else:
            lines.append(f"{prefix}{'└── ' if last else '├── '}{_format_semantic_node(node)}")
        children = node.children or []
        if not children:
            return
        if depth is not None and level >= depth:
            lines.append(f"{prefix}{'    ' if last else '│   '}… {len(children)} below (raise depth)")
            return
        inner = prefix + ("    " if last or node.element_type == "desktop" else "│   ")
        for i, child in enumerate(children):
            walk(child, inner, i == len(children) - 1, level + 1)

    walk(root, "", True, 0)
    return "\n".join(lines)


async def _snapshot_rows() -> list[Row] | None:
    """A fresh UI tree read, its rows; None when there is no desktop to read."""
    base = delta.base("Snapshot")
    result = await base(use_vision=False, use_ui_tree=True)
    state = getattr(_desktop(), "desktop_state", None)
    if state is None or getattr(state, "tree_state", None) is None:
        return None
    if isinstance(result, list) and result and isinstance(result[0], str) and result[0].startswith("Error"):
        return None
    return rows_of(state.tree_state)


async def _done(verify: bool) -> str:
    """An action's answer: ok, and what changed on screen when asked."""
    global _previous
    if not verify:
        return "ok"
    await asyncio.sleep(SETTLE)
    rows = await _snapshot_rows()
    if rows is None:
        return "ok (the screen could not be read to verify)"
    text = diff_rows(_previous, rows, LIMIT)
    _previous = rows
    return "ok\n" + text


Verify = Annotated[bool, Field(description="Also return what changed on screen afterwards (a Snapshot diff).")]


@delta.override("Click", description_suffix=VERIFY)
async def click(loc: list[int] | str | None = None, label: int | None = None,
                button: Literal["left", "right", "middle"] = "left", clicks: int = 1, verify: Verify = False) -> str:
    await delta.base("Click")(loc=loc, label=label, button=button, clicks=clicks)
    return await _done(verify)


@delta.override("Type", description_suffix=VERIFY)
async def type_(text: str, loc: list[int] | str | None = None, label: int | None = None, clear: bool | str = False,
                caret_position: Literal["start", "idle", "end"] = "idle", press_enter: bool | str = False,
                verify: Verify = False) -> str:
    await delta.base("Type")(text=text, loc=loc, label=label, clear=clear, caret_position=caret_position, press_enter=press_enter)
    return await _done(verify)


@delta.override("Scroll", description_suffix=VERIFY)
async def scroll(loc: list[int] | str | None = None, label: int | None = None,
                 type: Literal["horizontal", "vertical"] = "vertical",
                 direction: Literal["up", "down", "left", "right"] = "down", wheel_times: int = 1,
                 verify: Verify = False) -> str:
    await delta.base("Scroll")(loc=loc, label=label, type=type, direction=direction, wheel_times=wheel_times)
    return await _done(verify)


@delta.override("Move", description_suffix=VERIFY)
async def move(loc: list[int] | str | None = None, label: int | None = None, drag: bool | str = False,
               from_loc: list[int] | str | None = None, duration: float | int | str | None = None,
               verify: Verify = False) -> str:
    await delta.base("Move")(loc=loc, label=label, drag=drag, from_loc=from_loc, duration=duration)
    return await _done(verify)


@delta.override("Shortcut", description_suffix=VERIFY)
async def shortcut(shortcut: str, verify: Verify = False) -> str:
    await delta.base("Shortcut")(shortcut=shortcut)
    return await _done(verify)


@delta.override("MultiSelect", description_suffix=VERIFY)
async def multi_select(locs: list[list[int]] | str | None = None, labels: list[int] | str | None = None,
                       press_ctrl: bool | str = True, verify: Verify = False) -> str:
    await delta.base("MultiSelect")(locs=locs, labels=labels, press_ctrl=press_ctrl)
    return await _done(verify)


@delta.override("MultiEdit", description_suffix=VERIFY)
async def multi_edit(locs: list[list] | str | None = None, labels: list[list] | str | None = None,
                     verify: Verify = False) -> str:
    await delta.base("MultiEdit")(locs=locs, labels=labels)
    return await _done(verify)


SNAPSHOT = (
    " By default the UI part is a table of the elements to act on, one row each: id|type|name|x,y|state, under "
    "'# <window>' lines; the id is the label Click, Type, Scroll and Move take. At most `limit` rows (150); "
    "window=... keeps the windows whose title holds it. diff=true returns only the rows added (+), changed (~) "
    "or removed (-) since the previous Snapshot. mode='tree' returns Windows-MCP's full tree instead, `depth` "
    "levels deep."
)


@delta.override("Snapshot", description_suffix=SNAPSHOT)
async def snapshot(
    use_vision: bool | str = False,
    use_dom: bool | str = False,
    use_annotation: bool | str = True,
    use_ui_tree: bool | str = True,
    width_reference_line: int | None = None,
    height_reference_line: int | None = None,
    display: list[int] | None = None,
    region: list[int] | str | None = None,
    mode: Annotated[Literal["table", "tree"], Field(description="table: the elements to act on (default); tree: Windows-MCP's full tree.")] = "table",
    limit: Annotated[int, Field(description="Rows at most, in the table or the diff.")] = LIMIT,
    window: Annotated[str | None, Field(description="Only the windows whose title holds this text.")] = None,
    diff: Annotated[bool, Field(description="Only what changed since the previous Snapshot.")] = False,
    depth: Annotated[int | None, Field(description="mode='tree': levels below the desktop at most.")] = None,
) -> Any:
    global _previous
    result = await delta.base("Snapshot")(
        use_vision=use_vision, use_dom=use_dom, use_annotation=use_annotation, use_ui_tree=use_ui_tree,
        width_reference_line=width_reference_line, height_reference_line=height_reference_line,
        display=display, region=region)
    parts = result if isinstance(result, list) else [result]
    text = parts[0] if parts and isinstance(parts[0], str) else ""
    state = getattr(_desktop(), "desktop_state", None)
    wants_tree = str(use_ui_tree).lower() not in ("false", "0", "no")
    if text.startswith("Error") or state is None or getattr(state, "tree_state", None) is None or not wants_tree:
        return result   # an error, or no UI tree asked: Windows-MCP's answer as it is
    images = [p for p in parts[1:] if not isinstance(p, str)]
    rows = rows_of(state.tree_state)
    if mode == "tree":
        body = _tree(state, depth)
        if body is None:
            return result
    elif diff:
        body = diff_rows(_previous, rows, limit)
    else:
        body = table(rows, limit, window)
    _previous = rows
    return [header(state) + "\n" + body, *images]
