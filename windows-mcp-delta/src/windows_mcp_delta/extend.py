"""Tools over Windows-MCP's, as a subclass's methods over its base class's.

    from windows_mcp_delta.extend import delta

    @delta.override("PowerShell")
    def powershell(command: str, timeout: int = 30, cwd: str | None = None) -> str:
        ...
        return delta.base("PowerShell")(command=command, timeout=timeout)   # super

    @delta.add("Grep", description="...")
    def grep(pattern: str, path: str | None = None) -> str:
        ...

An override takes the tool's name, and its description and annotations
unless it gives its own; its parameters should be the base tool's, with
optional ones added, so that what calls the tool goes on working. An add
whose name the base already has is an error, an override of a name it lacks
too: both say so when the server is built, not when a tool is called.
"""
from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
from typing import Any, Callable

# Claude Desktop sends a tool the id of the conversation calling it only when the tool asks, by declaring the kinds
# of session id it wants in its _meta; "conversation" is the chat, "cse" the client session. (Found in the app.)
SESSION_ID_KINDS = "anthropic/sessionIdKinds"


@dataclass
class _Entry:
    name: str
    fn: Callable[..., Any]
    override: bool
    options: dict[str, Any] = field(default_factory=dict)


DESKTOP_CATEGORIES = ("direct", "possible", "other")


class Delta:
    def __init__(self) -> None:
        self._entries: list[_Entry] = []
        self._bases: dict[str, Callable[..., Any]] = {}
        self._middleware: list[Any] = []
        self._desktop: dict[str, str] = {}
        self.applied = False

    def desktop(self, category: str, *names: str) -> None:
        """How tools `names` touch the desktop, for Windows-MCP's ownership gate: "direct" (they click or type),
        "possible" (code they run might), "other" (never). A tool the gate does not know counts as direct."""
        if category not in DESKTOP_CATEGORIES:
            raise ValueError(f"unknown desktop category {category!r}")
        for name in names:
            self._desktop[name] = category

    def desktop_categories(self) -> dict[str, str]:
        return dict(self._desktop)

    def add(self, name: str, **options: Any) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
        """A new tool, `options` as FastMCP's `tool()` takes them (description, annotations, ...)."""
        def register(fn: Callable[..., Any]) -> Callable[..., Any]:
            self._entries.append(_Entry(name, fn, False, options))
            return fn
        return register

    def override(self, name: str, **options: Any) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
        """One of Windows-MCP's tools, replaced; `base(name)` calls the one replaced. `description_suffix` is added to
        the base's description, which the override keeps unless it gives its own."""
        def register(fn: Callable[..., Any]) -> Callable[..., Any]:
            self._entries.append(_Entry(name, fn, True, options))
            return fn
        return register

    def use(self, middleware: Any) -> None:
        """A FastMCP middleware added to the server with the tools (one that sees every call, Windows-MCP's too)."""
        self._middleware.append(middleware)

    def base(self, name: str) -> Callable[..., Any]:
        """The function of Windows-MCP's tool `name` that an override replaced (its `super`)."""
        try:
            return self._bases[name]
        except KeyError:
            raise LookupError(f"{name} was not overridden, or the server is not built yet") from None

    def names(self) -> list[tuple[str, bool]]:
        """Each tool's name, and whether it overrides one of Windows-MCP's."""
        return [(e.name, e.override) for e in self._entries]

    def apply(self, mcp: Any) -> None:
        """The overrides and additions made on Windows-MCP's server `mcp`, once."""
        if self.applied:
            return
        for entry in self._entries:
            existing = _get_tool(mcp, entry.name)
            options = dict(entry.options)
            if entry.override:
                if existing is None:
                    raise RuntimeError(f"windows-mcp-delta overrides {entry.name}, which this windows-mcp does not have")
                self._bases[entry.name] = getattr(existing, "fn", None) or existing
                suffix = options.pop("description_suffix", None)
                options.setdefault("description", getattr(existing, "description", None))
                if suffix:   # the base's description, and what the override adds to it
                    options["description"] = (options.get("description") or "") + suffix
                if getattr(existing, "annotations", None) is not None:
                    options.setdefault("annotations", existing.annotations)
                _remove_tool(mcp, entry.name)
            elif existing is not None:
                raise RuntimeError(f"windows-mcp already has {entry.name}: windows-mcp-delta should override it")
            # Ask Claude Desktop to send which conversation calls the tool (its own browser tools do the same):
            # a tool that declares anthropic/sessionIdKinds gets the conversation id in each call's _meta (caller.py).
            if entry.options.get("meta") is None:
                options["meta"] = {SESSION_ID_KINDS: ["cse", "conversation"]}
            mcp.tool(name=entry.name, **{k: v for k, v in options.items() if v is not None})(entry.fn)
        register_desktop(self._desktop)
        let_through(self._desktop)
        for middleware in self._middleware:
            mcp.add_middleware(middleware)
        self.applied = True


def register_desktop(categories: dict[str, str], policy: Any = None) -> bool:
    """Tools' desktop categories added to Windows-MCP's ownership table (windows_mcp.tools.control_policy).

    Its gate holds back every call that would act on the desktop while the user is using it (input in the last
    10 s: USER_CONTROL, with the seconds left to wait), and a tool missing from its table counts as one that does:
    an Idle or a Read would wait for the mouse to rest. `requires_control` reads the module's sets when called, so
    adding names to them is enough. FALSE when this Windows-MCP has no such table (an older one: nothing to gate).
    """
    if policy is None:
        try:
            from windows_mcp.tools import control_policy as policy
        except ImportError:
            return False
    sets = {"direct": "DIRECT", "possible": "POSSIBLE", "other": "OTHER"}
    if not all(hasattr(policy, attr) for attr in sets.values()):
        return False
    for name, category in categories.items():
        for attr in sets.values():   # a tool in one set only
            setattr(policy, attr, frozenset(getattr(policy, attr)) - {name})
        setattr(policy, sets[category], frozenset(getattr(policy, sets[category])) | {name})
    if hasattr(policy, "CLASSIFIED_TOOLS") and hasattr(policy, "MODES"):
        policy.CLASSIFIED_TOOLS = policy.DIRECT | policy.POSSIBLE | policy.OTHER | policy.MODES.keys() | {"ControlStatus"}
    return True


def let_through(categories: dict[str, str], gate: Any = None) -> bool:
    """Windows-MCP's ownership gate (ControlToolGate) replaced by Esc (esc.py).

    The gate refuses a call that would act on the desktop while the person used the mouse or keyboard in the last
    10 s (USER_CONTROL), and while Claude acts it swallows their input; Windows-MCP 0.8.8 as published gates every
    tool but ControlStatus. Its call handler now lets every call through, so Claude acts while the person works and
    takes no hold on their input. Only a tool that acts on the screen and steals focus (the "direct" desktop-input
    tools) is refused once the person pressed Esc while Claude was acting, and runs one at a time under the gate's
    own lock; running a command or script ("possible") and the read-only and file tools ("other") are not stopped by
    Esc. The session is still noted, for the gate's notifications. WINDOWS_MCP_DELTA_USER_CONTROL=1 keeps
    Windows-MCP's gate for those tools. FALSE when this Windows-MCP has no gate.
    """
    from windows_mcp_delta import esc

    if gate is None:
        try:
            from windows_mcp.tools.control_notifications import ControlToolGate as gate
        except ImportError:
            return False
    original = getattr(gate, "on_call_tool", None)
    if original is None:
        return False
    known = getattr(original, "delta_categories", None)
    if known is not None:   # wrapped already: the categories brought up to date
        known.update(categories)
        return True

    async def on_call_tool(self, context, call_next):
        name = getattr(context.message, "name", None)
        notifier = getattr(self, "notifier", None)
        if notifier is not None and hasattr(notifier, "remember"):
            notifier.remember(context)
        category = on_call_tool.delta_categories.get(name)
        # Esc only stops tools that act on the screen and steal focus (the "direct" desktop-input tools). Running a
        # command, a script or a job ("possible") does not move the mouse or keyboard, so Esc leaves it alone, as do
        # the read-only and file tools ("other").
        if name == "ControlStatus" or category in ("other", "possible"):
            return await call_next(context)
        if esc.user_gate():
            return await original(self, context, call_next)
        if esc.refused():
            from fastmcp.exceptions import ToolError
            raise ToolError(esc.STOPPED + (f" (The call just refused was {name}.)" if name else ""))
        esc.ensure_started()
        esc.note_action()
        lock = getattr(self, "_call_lock", None)
        if lock is None:
            return await call_next(context)
        async with lock:
            return await call_next(context)

    on_call_tool.delta_categories = dict(categories)
    gate.on_call_tool = on_call_tool
    return True


def _get_tool(mcp: Any, name: str) -> Any:
    """The tool `name` of `mcp`, None when it has none (before the server runs: no loop yet)."""
    try:
        return asyncio.run(mcp.get_tool(name))
    except Exception:
        return None


def _remove_tool(mcp: Any, name: str) -> None:
    provider = getattr(mcp, "local_provider", None)
    if provider is not None and hasattr(provider, "remove_tool"):
        provider.remove_tool(name)
        return
    tools = getattr(getattr(mcp, "_tool_manager", None), "_tools", None)   # FastMCP before its providers
    if tools is not None:
        tools.pop(name, None)


delta = Delta()
