"""Read, Write, Edit, Glob, Grep: files as Claude Code works with them, on Windows.

Cheaper than FileSystem or PowerShell for the same work: Read numbers its
lines and reads a part of a long file, Edit sends only the text that changes,
Glob and Grep return paths relative to where they looked. A file keeps its
encoding (UTF-8, with or without its BOM, UTF-16, or the ANSI code page: GBK
on a Chinese Windows) and its line ends (CRLF or LF) through Write and Edit.
Nothing is overwritten unseen: Write and Edit need the file to have been
read in this conversation, and not changed on disk since (state.py).
Relative paths resolve against the shared working directory.
"""
from __future__ import annotations

import fnmatch
import io
import json
import locale
import os
import re
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Annotated, Literal

from mcp.types import ToolAnnotations
from pydantic import BaseModel, Field

from windows_mcp_delta import state
from windows_mcp_delta.extend import delta

READ_LINES = 2000           # lines Read gives by default
LINE_MAX = 2000             # a longer line is cut
PDF_PAGES_MAX = 20
IMAGE_SIDE_MAX = 1568       # an image is scaled down to this, as Claude sees it anyway
IMAGE_BYTES_MAX = 1_000_000  # and compressed so what comes back is at most this many bytes (the agent may lower it)
GREP_FILE_MAX = 20 * 1024 * 1024
GREP_SECONDS = 60
SKIPPED_DIRS = {".git", ".hg", ".svn", "node_modules", "__pycache__", ".venv", ".mypy_cache", ".pytest_cache"}
IMAGES = {".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp"}
TYPES = {
    "py": (".py", ".pyi", ".pyw"), "js": (".js", ".mjs", ".cjs", ".jsx"), "ts": (".ts", ".tsx", ".mts", ".cts"),
    "c": (".c", ".h"), "cpp": (".cpp", ".cc", ".cxx", ".hpp", ".hh", ".h"), "cs": (".cs",), "java": (".java",),
    "go": (".go",), "rust": (".rs",), "rb": (".rb",), "php": (".php",), "sh": (".sh", ".bash"),
    "ps": (".ps1", ".psm1", ".psd1"), "md": (".md", ".markdown"), "json": (".json", ".jsonc"), "yaml": (".yml", ".yaml"),
    "toml": (".toml",), "xml": (".xml",), "html": (".html", ".htm"), "css": (".css", ".scss", ".less"),
    "sql": (".sql",), "txt": (".txt",), "bat": (".bat", ".cmd"),
}
_READ_ONLY = ToolAnnotations(readOnlyHint=True, idempotentHint=True, openWorldHint=False)
_WRITES = ToolAnnotations(readOnlyHint=False, destructiveHint=True, idempotentHint=False, openWorldHint=False)


# ------------------------------------------------------------ text files

@dataclass
class TextFile:
    text: str                # its lines joined by \n, whatever its line ends
    encoding: str            # what it is written back in
    newline: str             # "\r\n" or "\n"


def _ansi() -> str:
    return locale.getpreferredencoding(False) or "cp1252"


def decode(data: bytes) -> TextFile:
    """`data` as text; ValueError when it is not text (NUL bytes without a UTF-16 BOM)."""
    if data.startswith(b"\xef\xbb\xbf"):
        text, encoding = data[3:].decode("utf-8", errors="replace"), "utf-8-sig"
    elif data.startswith((b"\xff\xfe", b"\xfe\xff")):
        text, encoding = data.decode("utf-16"), "utf-16"
    elif b"\x00" in data[:8192]:
        raise ValueError("binary")
    else:
        try:
            text, encoding = data.decode("utf-8"), "utf-8"
        except UnicodeDecodeError:
            try:
                text, encoding = data.decode(_ansi()), _ansi()
            except (UnicodeDecodeError, LookupError):
                text, encoding = data.decode("latin-1"), "latin-1"
    crlf = text.count("\r\n")
    newline = "\r\n" if crlf and crlf * 2 >= text.count("\n") else "\n"
    return TextFile(text.replace("\r\n", "\n"), encoding, newline)


def load(path: str) -> TextFile:
    with open(path, "rb") as file:
        return decode(file.read())


def save(path: str, text: str, encoding: str = "utf-8", newline: str = "\n") -> None:
    """`text` (lines joined by \\n) written whole, in `encoding` with `newline` line ends, through a temporary file."""
    data = (text.replace("\r\n", "\n").replace("\n", newline) if newline == "\r\n" else text).encode(encoding)
    folder = os.path.dirname(path) or "."
    os.makedirs(folder, exist_ok=True)
    temporary = os.path.join(folder, f".{os.path.basename(path)}.{os.getpid()}.tmp")
    with open(temporary, "wb") as file:
        file.write(data)
    try:
        os.replace(temporary, path)
    except OSError:
        os.remove(temporary)
        raise


def _numbered(lines: list[str], first: int) -> str:
    out = []
    for number, line in enumerate(lines, first):
        if len(line) > LINE_MAX:
            line = line[:LINE_MAX] + f" … ({len(line) - LINE_MAX} more characters)"
        out.append(f"{number:>6}\t{line}")
    return "\n".join(out)


# ------------------------------------------------------------------ Read

def _image(path: str, max_side: int = IMAGE_SIDE_MAX, max_bytes: int = IMAGE_BYTES_MAX):
    """An image for the model: scaled to `max_side` on its long edge and compressed so it is at most `max_bytes`
    (a 1080p JPEG is otherwise too large for the client to take). PNG is kept while it fits; a photo that does not
    fit is sent as JPEG, its quality and then its size lowered until it does."""
    from fastmcp.utilities.types import Image
    try:
        from PIL import Image as Pil
    except ImportError:
        return Image(path=path)   # without Pillow the file is handed over as it is
    with Pil.open(path) as picture:
        picture.load()
        fits_disk = os.path.getsize(path) <= max_bytes
        if max(picture.size) <= max_side and fits_disk and path.lower().endswith((".png", ".jpg", ".jpeg")):
            return Image(path=path)
        side = min(max(picture.size), max_side)
        has_alpha = picture.mode in ("RGBA", "LA", "P")
        for _ in range(8):
            scaled = picture.copy()
            scaled.thumbnail((side, side))
            buffer = io.BytesIO()
            scaled.convert("RGBA" if has_alpha else "RGB").save(buffer, format="PNG")
            if buffer.tell() <= max_bytes:
                return Image(data=buffer.getvalue(), format="png")
            for quality in (85, 70, 55, 40):
                buffer = io.BytesIO()
                scaled.convert("RGB").save(buffer, format="JPEG", quality=quality, optimize=True)
                if buffer.tell() <= max_bytes:
                    return Image(data=buffer.getvalue(), format="jpeg")
            if side <= 256:
                return Image(data=buffer.getvalue(), format="jpeg")   # as small as we go: hand over the smallest JPEG
            side = int(side * 0.75)


def _pages(spec: str | None, count: int) -> list[int]:
    if not spec:
        return list(range(1, min(count, PDF_PAGES_MAX) + 1))
    chosen: list[int] = []
    for part in spec.replace(" ", "").split(","):
        if "-" in part:
            low, high = part.split("-", 1)
            chosen += range(int(low), int(high or count) + 1)
        elif part:
            chosen.append(int(part))
    return [p for p in dict.fromkeys(chosen) if 1 <= p <= count][:PDF_PAGES_MAX]


def _pdf(path: str, pages: str | None) -> str:
    from pypdf import PdfReader
    reader = PdfReader(path)
    count = len(reader.pages)
    chosen = _pages(pages, count)
    out = [f"--- page {n} ---\n{(reader.pages[n - 1].extract_text() or '').strip()}" for n in chosen]
    tail = f"\n({len(chosen)} of {count} pages; pages='{chosen[-1] + 1}-{min(count, chosen[-1] + PDF_PAGES_MAX)}' " \
           "for more)" if chosen and chosen[-1] < count else ""
    return "\n".join(out) + tail if out else f"(no such page: the PDF has {count})"


def _notebook(path: str) -> str:
    notebook = json.loads(load(path).text)
    out = []
    for number, cell in enumerate(notebook.get("cells", []), 1):
        source = "".join(cell.get("source", []))
        out.append(f"# cell {number} [{cell.get('cell_type', '?')}]\n{source}")
        for output in cell.get("outputs", []) or []:
            text = "".join(output.get("text", []) or output.get("data", {}).get("text/plain", []))
            if text:
                out.append("# output\n" + (text[:2000] + " …" if len(text) > 2000 else text))
    return "\n".join(out)


def _listing(path: str) -> str:
    entries = sorted(os.scandir(path), key=lambda e: (not e.is_dir(), e.name.lower()))
    lines = [e.name + ("/" if e.is_dir() else "") for e in entries[:500]]
    more = f"\n... and {len(entries) - 500} more" if len(entries) > 500 else ""
    return f"{path} is a folder ({len(entries)} entries):\n" + "\n".join(lines) + more


@delta.add(
    "Read",
    description=("Reads a file: text with numbered lines (cat -n), by default its first 2000 lines; offset and limit read "
                 "another part of a long file. Images come back as images, scaled and compressed to fit (max_side, "
                 "max_bytes lower it for a big photo); PDFs as text (pages='1-5'), notebooks as their cells, a folder "
                 "as its listing. Keeps the file's encoding (GBK, UTF-16...) in mind for Edit. "
                 "Read a file before you Edit or overwrite it."),
    annotations=_READ_ONLY,
)
def read(
    path: Annotated[str, Field(description="The file: absolute, or relative to the working directory.")],
    offset: Annotated[int | None, Field(description="The first line to read (1 is the first).", ge=1)] = None,
    limit: Annotated[int | None, Field(description="How many lines.", ge=1)] = None,
    pages: Annotated[str | None, Field(description="A PDF's pages, e.g. '3' or '1-5,8' (20 at most).")] = None,
    max_side: Annotated[int, Field(description="An image's longest edge, in pixels (it is scaled down to this).", ge=64)] = IMAGE_SIDE_MAX,
    max_bytes: Annotated[int, Field(description="The most an image may come back as, in bytes (lower it when the result is still too large).", ge=50_000)] = IMAGE_BYTES_MAX,
):
    full = state.resolve(path)
    if os.path.isdir(full):
        return _listing(full)
    if not os.path.isfile(full):
        return f"Error: no such file: {full}"
    extension = os.path.splitext(full)[1].lower()
    if extension in IMAGES:
        return _image(full, max_side, max_bytes)
    if extension == ".pdf":
        return _pdf(full, pages)
    try:
        if extension == ".ipynb":
            text = _notebook(full)
            state.remember_read(full)
            return text
        content = load(full)
    except ValueError:
        return f"Error: {full} is a binary file ({os.path.getsize(full)} bytes)."
    state.remember_read(full)
    if not content.text:
        return f"({full} is empty)"
    lines = content.text.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    first = offset or 1
    count = limit or READ_LINES
    shown = lines[first - 1:first - 1 + count]
    if not shown:
        return f"(no line {first}: {full} has {len(lines)})"
    out = _numbered(shown, first)
    last = first + len(shown) - 1
    if first > 1 or last < len(lines):
        out += f"\n(lines {first}–{last} of {len(lines)}" + (f"; Read(path, offset={last + 1}) for more)" if last < len(lines) else ")")
    return out


# ----------------------------------------------------------- Write, Edit

@delta.add(
    "Write",
    description=("Writes a file whole, creating it and its folders when missing. An existing file must have been Read "
                 "first, and keeps its encoding and line ends. Prefer Edit to change part of a file."),
    annotations=_WRITES,
)
def write(
    path: Annotated[str, Field(description="The file: absolute, or relative to the working directory.")],
    content: Annotated[str, Field(description="Everything the file is to hold.")],
) -> str:
    full = state.resolve(path)
    if os.path.isdir(full):
        return f"Error: {full} is a folder."
    refused = state.unseen_change(full)
    if refused:
        return f"Error: {refused}"
    existed = os.path.exists(full)
    encoding, newline = "utf-8", "\n"
    if existed:
        try:
            old = load(full)
            encoding, newline = old.encoding, old.newline
        except ValueError:
            pass
    text = content.replace("\r\n", "\n") if newline == "\r\n" or existed else content
    try:
        save(full, text, encoding, newline)
    except UnicodeEncodeError:
        save(full, text, "utf-8", newline)   # what the old code page cannot hold: UTF-8 then
        encoding = "utf-8"
    state.remember_read(full)
    lines = text.count("\n") + (0 if text.endswith("\n") or not text else 1)
    kept = [k for k in (encoding if encoding != "utf-8" else "", "CRLF" if newline == "\r\n" else "") if k]
    return f"{'Wrote' if existed else 'Created'} {full} ({lines} lines{''.join(', ' + k for k in kept)})"


class Change(BaseModel):
    old_string: str = Field(description="The exact text to replace (with enough around it to be unique).")
    new_string: str = Field(description="What it becomes.")
    replace_all: bool = Field(default=False, description="Replace every place it is found.")


def _apply(text: str, old: str, new: str, replace_all: bool) -> tuple[str, int, int] | str:
    """`text` with `old` replaced; (the new text, how many, the first place's offset), or why not."""
    old, new = old.replace("\r\n", "\n"), new.replace("\r\n", "\n")
    if old == new:
        return "old_string and new_string are the same."
    count = text.count(old)
    if count == 0:
        squeezed = re.sub(r"\s+", " ", old.strip())
        near = [i + 1 for i, line in enumerate(text.split("\n")) if squeezed[:40] and squeezed[:40] in re.sub(r"\s+", " ", line)]
        hint = f" Something like it is on line {', '.join(map(str, near[:5]))}: Read it and copy the text exactly." if near else \
            " Read the file again and copy the text exactly, spaces and tabs included."
        return "old_string was not found." + hint
    if count > 1 and not replace_all:
        lines = [text.count("\n", 0, m.start()) + 1 for m in re.finditer(re.escape(old), text)]
        return f"old_string is found {count} times (lines {', '.join(map(str, lines[:10]))}): give more of the text around " \
               "it, or replace_all=true."
    first = text.index(old)
    return (text.replace(old, new) if replace_all else text.replace(old, new, 1)), (count if replace_all else 1), first


@delta.add(
    "Edit",
    description=("Changes part of a file: old_string, which must be found exactly once (or replace_all=true), becomes "
                 "new_string. Several changes at once with edits (applied in order, all or none). The file must have been "
                 "Read first; it keeps its encoding and line ends. An empty old_string on a missing file creates it. "
                 "Returns the lines around the change."),
    annotations=_WRITES,
)
def edit(
    path: Annotated[str, Field(description="The file: absolute, or relative to the working directory.")],
    old_string: Annotated[str | None, Field(description="The exact text to replace.")] = None,
    new_string: Annotated[str | None, Field(description="What it becomes.")] = None,
    replace_all: Annotated[bool, Field(description="Replace every place old_string is found.")] = False,
    edits: Annotated[list[Change] | None, Field(description="Several changes, in order (instead of old_string/new_string).")] = None,
) -> str:
    full = state.resolve(path)
    changes = [c if isinstance(c, Change) else Change(**c) for c in edits or []]
    if old_string is not None or new_string is not None:
        changes.insert(0, Change(old_string=old_string or "", new_string=new_string or "", replace_all=replace_all))
    if not changes:
        return "Error: give old_string and new_string, or edits."
    if not os.path.exists(full):
        if len(changes) == 1 and changes[0].old_string == "":
            return write(path, changes[0].new_string)
        return f"Error: no such file: {full}"
    refused = state.unseen_change(full)
    if refused:
        return f"Error: {refused}"
    try:
        content = load(full)
    except ValueError:
        return f"Error: {full} is a binary file."
    text, total, first = content.text, 0, None
    for number, change in enumerate(changes, 1):
        if change.old_string == "":
            return "Error: old_string is empty (only a new file is made from an empty one)."
        result = _apply(text, change.old_string, change.new_string, change.replace_all)
        if isinstance(result, str):
            return f"Error: {'' if len(changes) == 1 else f'change {number}: '}{result} Nothing was changed."
        text, count, at = result
        total += count
        first = at if first is None else min(first, at)
    try:
        save(full, text, content.encoding, content.newline)
    except UnicodeEncodeError:
        return f"Error: the new text has characters {content.encoding} cannot hold. Nothing was changed."
    state.remember_read(full)
    lines = text.split("\n")
    start_line = text.count("\n", 0, first) + 1
    span = changes[0].new_string.count("\n") + 1 if len(changes) == 1 else 3
    low, high = max(1, start_line - 2), min(len(lines), start_line + span + 1, start_line + 10)
    return f"Edited {full}: {total} replacement{'s' if total != 1 else ''}.\n" + _numbered(lines[low - 1:high], low)


# ------------------------------------------------------------ Glob, Grep

def _braces(pattern: str) -> list[str]:
    """'*.{ts,tsx}' as ['*.ts', '*.tsx']."""
    match = re.search(r"\{([^{}]*)\}", pattern)
    if not match:
        return [pattern]
    return [p for option in match.group(1).split(",") for p in _braces(pattern[:match.start()] + option + pattern[match.end():])]


def _base(path: str | None) -> str:
    base = state.resolve(path) if path else state.cwd()
    if not os.path.exists(base):
        raise ValueError(f"no such folder: {base}")
    return base


def _shown(path: str, base: str) -> str:
    try:
        relative = os.path.relpath(path, base)
        return path if relative.startswith("..") else relative
    except ValueError:   # another drive
        return path


@delta.add(
    "Glob",
    description=("Finds files by name pattern ('**/*.py', 'src/**/*.{ts,tsx}', '*.md'), the most recently changed first, "
                 "as paths relative to where it looked (path, else the working directory). .git, node_modules and "
                 "caches are not looked in."),
    annotations=_READ_ONLY,
)
def glob(
    pattern: Annotated[str, Field(description="The pattern; ** is any depth.")],
    path: Annotated[str | None, Field(description="The folder to look in.")] = None,
    limit: Annotated[int, Field(description="At most this many paths.", ge=1, le=5000)] = 200,
) -> str:
    pattern = pattern.replace("\\", "/")
    if os.path.isabs(pattern) or re.match(r"^[A-Za-z]:/", pattern):
        # An absolute pattern: its folder part before the first wildcard is where to look.
        parts = pattern.split("/")
        fixed = next((i for i, part in enumerate(parts) if any(c in part for c in "*?[{")), len(parts) - 1)
        path, pattern = "/".join(parts[:fixed]) or "/", "/".join(parts[fixed:])
    try:
        base = _base(path)
    except ValueError as error:
        return f"Error: {error}"
    found: dict[str, float] = {}
    for one in _braces(pattern):
        for match in Path(base).glob(one):
            parts = set(match.relative_to(base).parts[:-1]) if match.is_relative_to(base) else set()
            if parts & SKIPPED_DIRS or not match.is_file():
                continue
            try:
                found[str(match)] = match.stat().st_mtime
            except OSError:
                continue
    if not found:
        return f"No files match {pattern} in {base}."
    ordered = sorted(found, key=found.get, reverse=True)
    out = "\n".join(_shown(p, base) for p in ordered[:limit])
    return out + (f"\n({len(ordered)} files; the {limit} most recent shown)" if len(ordered) > limit else "")


def _files(base: str, globs: list[str], types: tuple[str, ...]):
    if os.path.isfile(base):
        yield base
        return
    for folder, dirs, names in os.walk(base):
        dirs[:] = sorted(d for d in dirs if d not in SKIPPED_DIRS)
        for name in sorted(names):
            full = os.path.join(folder, name)
            if types and not name.lower().endswith(types):
                continue
            if globs:
                relative = os.path.relpath(full, base).replace("\\", "/")
                if not any(fnmatch.fnmatch(name, g) or fnmatch.fnmatch(relative, g) for g in globs):
                    continue
            yield full


@delta.add(
    "Grep",
    description=("Searches file contents for a regular expression (Python's), in a file or a folder and the folders in "
                 "it (.git, node_modules and caches left out, binary files skipped). output_mode: files_with_matches "
                 "(default: the files, most recently changed first), content (the lines, with context lines around "
                 "them when asked), count. Filter files with glob ('*.{ts,tsx}') or type ('py', 'js', 'c'...). "
                 "head_limit and offset page through a long result."),
    annotations=_READ_ONLY,
)
def grep(
    pattern: Annotated[str, Field(description="The regular expression.")],
    path: Annotated[str | None, Field(description="A file or folder to search; the working directory by default.")] = None,
    glob: Annotated[str | None, Field(description="Only files whose name (or relative path) matches, e.g. '*.py'.")] = None,
    type: Annotated[str | None, Field(description=f"Only files of a type: {', '.join(sorted(TYPES))}.")] = None,
    output_mode: Annotated[Literal["files_with_matches", "content", "count"], Field(description="What to return.")] = "files_with_matches",
    case_insensitive: Annotated[bool, Field(description="Ignore case.")] = False,
    context: Annotated[int, Field(description="Lines shown before and after each match (content).", ge=0, le=50)] = 0,
    before: Annotated[int, Field(description="Lines shown before each match (content).", ge=0, le=50)] = 0,
    after: Annotated[int, Field(description="Lines shown after each match (content).", ge=0, le=50)] = 0,
    multiline: Annotated[bool, Field(description="The pattern may span lines (. matches a line end).")] = False,
    head_limit: Annotated[int, Field(description="At most this many lines of result.", ge=1)] = 250,
    offset: Annotated[int, Field(description="Skip this many lines of result first.", ge=0)] = 0,
) -> str:
    flags = (re.IGNORECASE if case_insensitive else 0) | (re.DOTALL | re.MULTILINE if multiline else 0)
    try:
        regex = re.compile(pattern, flags)
    except re.error as error:
        return f"Error: bad regular expression: {error}"
    if type and type.lower() not in TYPES:
        return f"Error: unknown type {type!r}: {', '.join(sorted(TYPES))}."
    try:
        base = _base(path)
    except ValueError as error:
        return f"Error: {error}"
    before, after = max(before, context), max(after, context)
    globs = [g.replace("\\", "/") for g in _braces(glob)] if glob else []
    deadline = time.monotonic() + GREP_SECONDS
    results: list[tuple[str, float, list[str], int]] = []
    stopped = False
    for full in _files(base, globs, TYPES.get(type.lower(), ()) if type else ()):
        if time.monotonic() > deadline:
            stopped = True
            break
        try:
            if os.path.getsize(full) > GREP_FILE_MAX:
                continue
            text = load(full).text
            stamp = os.path.getmtime(full)
        except (OSError, ValueError):
            continue
        shown_name = _shown(full, base) if os.path.isdir(base) else full
        if multiline:
            starts = [text.count("\n", 0, m.start()) for m in regex.finditer(text)]
            lines = text.split("\n") if starts and output_mode == "content" else []
            hits = sorted(set(starts))
        else:
            lines = text.split("\n")
            hits = [i for i, line in enumerate(lines) if regex.search(line)]
        if not hits:
            continue
        out: list[str] = []
        if output_mode == "content":
            shown_until, matched = -1, set(hits)
            for hit in hits:
                low, high = max(0, hit - before), min(len(lines) - 1, hit + after)
                if out and low > shown_until + 1 and (before or after):
                    out.append("--")
                for i in range(max(low, shown_until + 1), high + 1):
                    line = lines[i] if len(lines[i]) <= LINE_MAX else lines[i][:LINE_MAX] + " …"
                    mark = ":" if i in matched else "-"     # as ripgrep: a match's line, or one around it
                    out.append(f"{shown_name}{mark}{i + 1}{mark}{line}")
                shown_until = max(shown_until, high)
        results.append((shown_name, stamp, out, len(hits)))
    if not results:
        return f"No matches for {pattern}" + (" (stopped: the time limit)" if stopped else "") + "."
    if output_mode == "files_with_matches":
        flat = [r[0] for r in sorted(results, key=lambda r: r[1], reverse=True)]
    elif output_mode == "count":
        flat = [f"{name}:{count}" for name, _, _, count in results]
    else:
        flat = [line for _, _, out, _ in results for line in out]
    page = flat[offset:offset + head_limit]
    note = []
    if offset + head_limit < len(flat):
        note.append(f"{len(flat)} lines in all; offset={offset + head_limit} for the next ones")
    if stopped:
        note.append(f"stopped after {GREP_SECONDS}s: narrow path, glob or type")
    return "\n".join(page) + (f"\n({'; '.join(note)})" if note else "")
