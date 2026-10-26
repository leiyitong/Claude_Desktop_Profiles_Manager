"""Print values of Claude's web storage (Chromium's Local Storage of https://claude.ai), read-only.

    python tools/read-webstore.py <profile folder> [name ...]

<profile folder> is a profile's data folder (%APPDATA%\\Claude-<name>, or Main's in
%LOCALAPPDATA%\\Packages\\Claude_*\\LocalCache\\Roaming\\Claude). Without names it lists the
names stored; with names it prints each one's latest value. The folder is only read:
LevelDB's LOCK is not opened, so it works while that Claude runs (what Claude has not
written to disk yet does not show). The formats are those webstore.c reads (core.c).
"""
from pathlib import Path
import struct
import sys

ORIGIN = b"_https://claude.ai\x00"


def varint(data, at):
    value = shift = 0
    while True:
        byte = data[at]
        at += 1
        value |= (byte & 0x7F) << shift
        if byte < 0x80:
            return value, at
        shift += 7


def snappy(data):
    size, at = varint(data, 0)
    out = bytearray()
    while at < len(data):
        tag = data[at]
        at += 1
        kind = tag & 3
        if kind == 0:
            length = tag >> 2
            if length >= 60:
                extra = length - 59
                length = int.from_bytes(data[at:at + extra], "little")
                at += extra
            length += 1
            out += data[at:at + length]
            at += length
            continue
        if kind == 1:
            length = ((tag >> 2) & 7) + 4
            offset = ((tag >> 5) << 8) | data[at]
            at += 1
        elif kind == 2:
            length = (tag >> 2) + 1
            offset = int.from_bytes(data[at:at + 2], "little")
            at += 2
        else:
            length = (tag >> 2) + 1
            offset = int.from_bytes(data[at:at + 4], "little")
            at += 4
        for _ in range(length):   # the copy may overlap what it makes
            out.append(out[-offset])
    if len(out) != size:
        raise ValueError("snappy: wrong length")
    return bytes(out)


def block(data, offset, size):
    raw = data[offset:offset + size]
    kind = data[offset + size]
    return snappy(raw) if kind == 1 else raw


def entries(data):
    """A table block's entries (prefix-compressed keys), its restart array left out."""
    restarts = struct.unpack_from("<I", data, len(data) - 4)[0]
    end = len(data) - 4 - 4 * restarts
    at, key = 0, b""
    while at < end:
        shared, at = varint(data, at)
        unshared, at = varint(data, at)
        length, at = varint(data, at)
        key = key[:shared] + data[at:at + unshared]
        at += unshared
        yield key, data[at:at + length]
        at += length


def handle(data, at):
    offset, at = varint(data, at)
    size, at = varint(data, at)
    return offset, size


def table(path, found):
    data = path.read_bytes()
    footer = data[-48:]   # the metaindex's handle, the index's, padding, the magic number
    _, at = varint(footer, 0)
    _, at = varint(footer, at)
    index_offset, at = varint(footer, at)
    index_size, at = varint(footer, at)
    for _, value in entries(block(data, index_offset, index_size)):
        offset, size = handle(value, 0)
        for internal, stored in entries(block(data, offset, size)):
            take(found, internal[:-8], struct.unpack("<Q", internal[-8:])[0], stored)


def take(found, key, tag, value):
    sequence, kind = tag >> 8, tag & 0xFF
    if not key.startswith(ORIGIN):
        return
    if key not in found or found[key][0] < sequence:
        found[key] = (sequence, value if kind == 1 else None)


def log(path, found):
    data = path.read_bytes()
    at, record = 0, b""
    while at + 7 <= len(data):
        left = 32768 - at % 32768
        if left < 7:
            at += left
            continue
        length, kind = struct.unpack_from("<HB", data, at + 4)
        chunk = data[at + 7:at + 7 + length]
        at += 7 + length
        if kind == 0 and length == 0:
            break
        if kind in (1, 2):   # full, first
            record = chunk
        elif kind in (3, 4):   # middle, last
            record += chunk
        if kind in (1, 4):
            batch(record, found)
            record = b""


def batch(data, found):
    if len(data) < 12:
        return
    sequence, count = struct.unpack_from("<QI", data, 0)
    at = 12
    for i in range(count):
        kind = data[at]
        at += 1
        length, at = varint(data, at)
        key = data[at:at + length]
        at += length
        value = None
        if kind == 1:
            length, at = varint(data, at)
            value = data[at:at + length]
            at += length
        take(found, key, ((sequence + i) << 8) | kind, value)


def text(stored):
    if stored is None:
        return None
    return stored[1:].decode("latin-1") if stored[:1] == b"\x01" else stored[1:].decode("utf-16-le", "replace")


def main():
    sys.stdout.reconfigure(errors="replace")   # a console's code page may not have every character
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    folder = Path(sys.argv[1]) / "Local Storage" / "leveldb"
    if not folder.is_dir():
        print(f"no web storage in {folder}")
        return 1
    found = {}
    for path in sorted(folder.iterdir()):
        try:
            if path.suffix == ".ldb":
                table(path, found)
            elif path.suffix == ".log":
                log(path, found)
        except (OSError, ValueError, IndexError, struct.error) as error:
            print(f"{path.name}: not read ({error})", file=sys.stderr)
    names = {text(key[len(ORIGIN):]): stored for key, (_, stored) in found.items()}
    if len(sys.argv) == 2:
        for name in sorted(n for n, v in names.items() if v is not None):
            print(name)
        return 0
    for name in sys.argv[2:]:
        print(f"== {name}")
        value = text(names.get(name))
        print(value if value is not None else "(none)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
