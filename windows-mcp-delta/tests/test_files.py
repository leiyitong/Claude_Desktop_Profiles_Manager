import os
import time

import pytest

from windows_mcp_delta import state
from windows_mcp_delta.tools import files


@pytest.fixture(autouse=True)
def here(tmp_path):
    state.reset()
    state.set_cwd(str(tmp_path))
    yield tmp_path
    state.reset()


def test_read_numbers_lines_and_pages(here):
    (here / "a.txt").write_text("\n".join(f"line {i}" for i in range(1, 11)) + "\n")
    out = files.read("a.txt")
    assert out.startswith("     1\tline 1") and "    10\tline 10" in out and "of 10" not in out
    out = files.read("a.txt", offset=4, limit=3)
    assert out.splitlines()[0] == "     4\tline 4" and "(lines 4–6 of 10; Read(path, offset=7) for more)" in out
    assert files.read("missing.txt").startswith("Error: no such file")
    (here / "bin.dat").write_bytes(b"\x00\x01\x02")
    assert "binary" in files.read("bin.dat")
    (here / "empty.txt").write_text("")
    assert "is empty" in files.read("empty.txt")
    assert "a.txt" in files.read(".")                                        # a folder: its listing


def test_write_needs_a_read_and_keeps_encoding_and_line_ends(here):
    assert files.write("new/b.txt", "one\ntwo\n").startswith("Created")
    target = here / "gbk.txt"
    target.write_bytes("第一行\r\n第二行\r\n".encode("gbk"))
    assert "was not read" in files.write("gbk.txt", "x")
    files.read("gbk.txt")
    if files._ansi().lower().replace("-", "") in ("gbk", "cp936"):
        assert files.write("gbk.txt", "第三行\n第四行\n").endswith("CRLF)")
        assert target.read_bytes() == "第三行\r\n第四行\r\n".encode("gbk")
    utf16 = here / "u16.txt"
    utf16.write_bytes("hello\r\nworld\r\n".encode("utf-16"))
    files.read("u16.txt")
    assert "utf-16, CRLF" in files.write("u16.txt", "bye\nnow\n")
    assert utf16.read_bytes() == "bye\r\nnow\r\n".encode("utf-16")
    time.sleep(0.05)
    utf16.write_bytes("changed".encode("utf-16"))
    os.utime(utf16, (time.time() + 5, time.time() + 5))
    assert "changed on disk" in files.write("u16.txt", "again")


def test_edit(here):
    target = here / "c.py"
    target.write_bytes(b"def f():\r\n    return 1\r\n\r\ndef g():\r\n    return 1\r\n")
    assert "was not read" in files.edit("c.py", "return 1", "return 2")
    files.read("c.py")
    out = files.edit("c.py", "return 1", "return 2")
    assert "found 2 times (lines 2, 5)" in out and "Nothing was changed" in out
    out = files.edit("c.py", "def g():\n    return 1", "def g():\n    return 3")
    assert out.startswith("Edited") and "     5\t    return 3" in out
    assert target.read_bytes() == b"def f():\r\n    return 1\r\n\r\ndef g():\r\n    return 3\r\n"   # CRLF kept
    assert "1 replacement" in out
    out = files.edit("c.py", edits=[{"old_string": "f()", "new_string": "first()"}, {"old_string": "g()", "new_string": "second()"}])
    assert "2 replacements" in out and b"def first()" in target.read_bytes() and b"def second()" in target.read_bytes()
    out = files.edit("c.py", edits=[{"old_string": "first", "new_string": "x"}, {"old_string": "nowhere", "new_string": "y"}])
    assert out.startswith("Error: change 2: old_string was not found") and b"def first()" in target.read_bytes()   # all or none
    assert "same" in files.edit("c.py", "first", "first")
    assert files.edit("c.py", "return", "yield", replace_all=True).startswith("Edited")
    assert target.read_bytes().count(b"yield") == 2
    out = files.edit("c.py", "def  second():", "z")
    assert "line 4" in out                                                   # a hint where something like it is
    assert files.edit("made.txt", "", "fresh\n").startswith("Created")


def test_glob(here):
    (here / "src" / "deep").mkdir(parents=True)
    (here / "node_modules").mkdir()
    for name in ("src/a.ts", "src/deep/b.tsx", "src/c.js", "node_modules/x.ts", "top.ts"):
        (here / name).write_text("x")
    os.utime(here / "src/a.ts", (time.time() + 10, time.time() + 10))
    out = files.glob("**/*.{ts,tsx}").splitlines()
    assert out[0] == os.path.join("src", "a.ts") and sorted(out) == sorted([os.path.join("src", "a.ts"),
                                                                            os.path.join("src", "deep", "b.tsx"), "top.ts"])
    assert files.glob("*.md").startswith("No files")
    absolute = files.glob(str(here / "src") + "/*.js")
    assert absolute == "c.js"
    assert "3 files; the 1 most recent shown" in files.glob("**/*.ts*", limit=1)


def test_grep(here):
    (here / "pkg").mkdir()
    (here / "pkg" / "one.py").write_text("import os\n\ndef main():\n    print('Hello')\n    return 0\n")
    (here / "pkg" / "two.c").write_text("int main(void) { return 0; }\n")
    (here / "pkg" / "three.py").write_bytes("# 中文注释\nprint('你好')\n".encode("gbk") if files._ansi().lower() in ("gbk", "cp936") else
                                             "# 中文注释\nprint('你好')\n".encode("utf-8"))
    (here / "blob.bin").write_bytes(b"\x00main\x00")
    assert set(files.grep("main").splitlines()) == {os.path.join("pkg", "one.py"), os.path.join("pkg", "two.c")}
    assert files.grep("main", type="py") == os.path.join("pkg", "one.py")
    assert files.grep("main", glob="*.c") == os.path.join("pkg", "two.c")
    out = files.grep("print", path="pkg/one.py", output_mode="content", before=1)
    assert out.splitlines() == [f"{here / 'pkg' / 'one.py'}-3-def main():", f"{here / 'pkg' / 'one.py'}:4:    print('Hello')"]
    assert files.grep("hello", output_mode="count", case_insensitive=True) == os.path.join("pkg", "one.py") + ":1"
    assert files.grep("你好", output_mode="count").endswith("three.py:1")
    assert files.grep(r"def main\(\):\n    print", multiline=True, output_mode="count").endswith("one.py:1")
    many = files.grep(".", output_mode="content", head_limit=2)
    assert len(many.splitlines()) == 3 and "offset=2 for the next ones" in many
    assert files.grep("(").startswith("Error: bad regular expression")
    assert files.grep("zzz_nothing").startswith("No matches")
    assert files.grep("x", type="cobol").startswith("Error: unknown type")


def test_pdf_and_image(here):
    from PIL import Image as Pil
    big = here / "big.png"
    Pil.new("RGB", (3000, 1000), "red").save(big)
    result = files.read("big.png")
    from fastmcp.utilities.types import Image
    assert isinstance(result, Image)
    from pypdf import PdfWriter
    writer = PdfWriter()
    for _ in range(3):
        writer.add_blank_page(width=200, height=200)
    with open(here / "doc.pdf", "wb") as f:
        writer.write(f)
    out = files.read("doc.pdf", pages="2-3")
    assert "--- page 2 ---" in out and "--- page 3 ---" in out and "--- page 1 ---" not in out


def test_read_image_scaled_and_compressed_under_max_bytes(here):
    pil = pytest.importorskip("PIL.Image")
    import random
    big = pil.new("RGB", (1920, 1080))
    rng = random.Random(0)
    big.putdata([(rng.randint(0, 255), rng.randint(0, 255), rng.randint(0, 255)) for _ in range(1920 * 1080)])
    path = here / "photo(3).jpg"   # parens and a 1080p photo, as the report had
    big.save(str(path), format="JPEG", quality=95)
    image = files.read("photo(3).jpg", max_bytes=300_000)
    data = getattr(image, "data", None)
    assert data is not None and len(data) <= 300_000   # compressed to fit, not handed over whole
