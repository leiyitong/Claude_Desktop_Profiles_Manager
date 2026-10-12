"""The changelog: CHANGELOG.md (English) and CHANGELOG.zh-CN.md (Simplified Chinese).

A day's commits make one version, named after that day: src/version.h's
APP_VERSION_DATE ("2026.10.12"), which the manager shows as its version.
Each file holds one heading per version, newest first, "## v<day>", and
under it, as a release page has it: what the version is about in a sentence
or two, then its changes in sections, in this order and each only when it
has some, "### New features", "### Improvements", "### Bug fixes",
"### Breaking changes", "### Upgrade guide" (新增功能, 优化改进, Bug 修复,
破坏性变更, 升级指南), each change a line numbered from 1 in its section.
The two files have the same versions, sections and numbers of lines. The
program embeds both (src/app.rc) and shows the one of its interface language
in About (Core_ChangelogRead), so they keep this form.

  python tools/changelog.py --stamp     before a commit that changes the program: APP_VERSION_DATE becomes
                                        today, and today's version goes on top of both files when it is not
                                        there yet (its lines are numbered again); the commit's lines go under it
  python tools/changelog.py --check     checks both files (build.cmd runs it)
  python tools/changelog.py --notes latest|<day>
                                        a version's release notes in both languages (main's pre-releases)
  python tools/changelog.py --release   the notes of every version since the latest v* tag (a signed release)

The check: both files in this form and in step; days unique, newest first;
the newest src/version.h's; and, where git has the commit before, that the
latest commit, when it changed the program (src/), wrote both files and is
the version of the day it was made.
"""
from datetime import datetime
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
REPO = "https://github.com/JustLikeCheese/Claude_Desktop_Profiles_Manager"
FILES = {"en": ROOT / "CHANGELOG.md", "zh-CN": ROOT / "CHANGELOG.zh-CN.md"}
VERSION_H = ROOT / "src" / "version.h"
SECTIONS = {"en": ["New features", "Improvements", "Bug fixes", "Breaking changes", "Upgrade guide"],
            "zh-CN": ["新增功能", "优化改进", "Bug 修复", "破坏性变更", "升级指南"]}
TAGLINE = {"en": "Run several Claude Desktop accounts side by side on Windows, each with its own sign-in, history and settings.",
           "zh-CN": "在 Windows 上同时运行多个 Claude Desktop 账号，每个账号有自己的登录、历史和设置。"}
FOOTER = """---

## 📥 Installation / 安装

Download `ClaudeDesktopProfilesManager.exe` below and double-click it: it installs itself for the current user, no admin rights needed, and opens. To update, run the new version the same way.

下载下面的 `ClaudeDesktopProfilesManager.exe` 并双击：它会为当前用户安装自己（不需要管理员权限）并打开。更新时用同样的方法运行新版本。

## 📚 Documentation / 文档

- [GitHub Repository](%(repo)s)
- [README](%(repo)s/blob/main/README.md)
- [How it works](%(repo)s/blob/main/docs/HOW-IT-WORKS.md)
- [Changelog](%(repo)s/blob/main/CHANGELOG.md) · [更新日志](%(repo)s/blob/main/CHANGELOG.zh-CN.md)
""" % {"repo": REPO}
DAY_FORMAT = "%Y.%m.%d"
VERSION_LINE = re.compile(r'^## v(\d{4}\.\d{2}\.\d{2})$')
SECTION_LINE = re.compile(r'^### (.+)$')
ENTRY_LINE = re.compile(r'^(\d+)\. (\S.*)$')
DATE_DEFINE = re.compile(r'(#define APP_VERSION_DATE\s+")([^"]*)(")')
LINE_BYTES_MAX = 4000   # the program reads a line of up to 4096 characters (Core_ChangelogRead)


def read(path):
    return path.read_text(encoding="utf-8").replace("\r\n", "\n")


def write(path, text):
    with open(str(path), "w", encoding="utf-8", newline="\n") as out:
        out.write(text)


def parse(lang, errors):
    """The lines before the first version, and its versions in the file's
    order: {"day", "summary": [lines], "sections": [(title, [lines])]}."""
    head, versions = [], []
    for number, line in enumerate(read(FILES[lang]).split("\n"), 1):
        where = "%s:%d" % (FILES[lang].name, number)
        if len(line.encode("utf-8")) >= LINE_BYTES_MAX:
            errors.append("%s: a line is %d bytes at most" % (where, LINE_BYTES_MAX - 1))
        version = VERSION_LINE.match(line)
        if version:
            versions.append({"day": version.group(1), "summary": [], "sections": []})
            continue
        if not versions:
            head.append(line)
            continue
        if not line.strip():
            continue
        current = versions[-1]
        section = SECTION_LINE.match(line)
        if section:
            current["sections"].append((section.group(1), []))
            continue
        if line.startswith("#"):
            errors.append(where + ": a version's heading is \"## v<yyyy.mm.dd>\", a section's \"### <section>\"")
            continue
        if not current["sections"]:
            current["summary"].append(line)
            continue
        entry = ENTRY_LINE.match(line)
        lines = current["sections"][-1][1]
        if not entry:
            errors.append(where + ": a section holds numbered lines, one per change (\"1. ...\")")
        elif int(entry.group(1)) != len(lines) + 1:
            errors.append(where + ": a section's lines are numbered 1, 2, 3...: run python tools/changelog.py --stamp")
        lines.append(entry.group(2) if entry else line)
    while head and head[-1] == "":
        head.pop()
    return head, versions


def serialize(head, versions):
    out = list(head)
    for version in versions:
        out += ["", "## v" + version["day"]]
        for line in version["summary"]:
            out += ["", line]
        for title, lines in version["sections"]:
            out += ["", "### " + title, ""]
            out += ["%d. %s" % (n, line) for n, line in enumerate(lines, 1)]
    return "\n".join(out) + "\n"


def version_date(text):
    match = DATE_DEFINE.search(text or "")
    return match.group(2) if match else None


def git(*args):
    try:
        result = subprocess.run(["git"] + list(args), cwd=str(ROOT), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError:
        return None
    return result.stdout.decode("utf-8", "replace") if result.returncode == 0 else None


def check():
    errors = []
    parsed = {}
    for lang in FILES:
        if not FILES[lang].exists():
            errors.append(FILES[lang].name + " is missing")
            continue
        _, versions = parse(lang, errors)
        parsed[lang] = versions
        if not versions:
            errors.append(FILES[lang].name + ": no version")
        for version in versions:
            name = "%s: v%s" % (FILES[lang].name, version["day"])
            try:
                datetime.strptime(version["day"], DAY_FORMAT)
            except ValueError:
                errors.append(name + " is no day")
            if not version["summary"]:
                errors.append(name + " says in a sentence or two what it is about, before its sections")
            if not version["sections"]:
                errors.append(name + " has no change")
            titles = [t for t, _ in version["sections"]]
            if [t for t in titles if t not in SECTIONS[lang]] or titles != [t for t in SECTIONS[lang] if t in titles]:
                errors.append("%s: its sections are %s, in this order, each once" % (name, ", ".join(SECTIONS[lang])))
            for title, lines in version["sections"]:
                if not lines:
                    errors.append("%s: %s has no line" % (name, title))
        days = [v["day"] for v in versions]
        if days != sorted(set(days), reverse=True):
            errors.append(FILES[lang].name + ": the versions are not one per day, newest first")
    if len(parsed) == len(FILES):
        en, zh = parsed["en"], parsed["zh-CN"]
        if [v["day"] for v in en] != [v["day"] for v in zh]:
            errors.append("the two changelogs list different versions")
        else:
            for a, b in zip(en, zh):
                shape = lambda v, lang: [(SECTIONS[lang].index(t) if t in SECTIONS[lang] else t, len(lines)) for t, lines in v["sections"]]
                if shape(a, "en") != shape(b, "zh-CN") or len(a["summary"]) != len(b["summary"]):
                    errors.append("v%s differs between the two changelogs: its summary, sections or numbers of lines" % a["day"])
        current = version_date(read(VERSION_H))
        if not current:
            errors.append("src/version.h has no APP_VERSION_DATE")
        elif en and en[0]["day"] != current:
            errors.append("the newest version in the changelogs is not src/version.h's %s: run python tools/changelog.py --stamp" % current)
        # The latest commit: when it changed the program, it said what, under the day it was made.
        if git("rev-parse", "--verify", "--quiet", "HEAD^") is not None and (git("rev-parse", "--is-shallow-repository") or "").strip() == "false":
            written = (git("diff", "--name-only", "HEAD^", "HEAD") or "").split()
            if [name for name in written if name.startswith("src/")]:
                for lang in FILES:
                    if FILES[lang].name not in written:
                        errors.append("the latest commit changed the program but not %s: every such commit says what it changes" % FILES[lang].name)
                committed = version_date(git("show", "HEAD:src/version.h"))
                made = (git("log", "-1", "--format=%cd", "--date=format-local:" + DAY_FORMAT, "HEAD") or "").strip()
                if committed and made and committed != made:
                    errors.append("the latest commit, made on %s, is version %s: run --stamp before a commit" % (made, committed))
    for error in errors:
        print("changelog: " + error)
    print("changelog: %d errors" % len(errors))
    return 0 if not errors else 1


def stamp():
    today = datetime.now().strftime(DAY_FORMAT)
    committed = version_date(git("show", "HEAD:src/version.h"))
    for lang in FILES:
        errors = []
        head, versions = parse(lang, errors)
        if [e for e in errors if "numbered 1, 2, 3" not in e]:
            sys.exit("\n".join(errors))
        if not versions or versions[0]["day"] != today:
            if versions and committed and versions[0]["day"] > committed:
                versions[0]["day"] = today   # a version not committed yet, begun on another day: today's
            else:
                versions.insert(0, {"day": today, "summary": [], "sections": []})
        write(FILES[lang], serialize(head, versions))   # numbered again
    source = read(VERSION_H)
    write(VERSION_H, DATE_DEFINE.sub(lambda m: m.group(1) + today + m.group(3), source, count=1))
    print("changelog: this version is v%s; write what the commit changes under it in both files" % today)


def versions_of(lang, wanted):
    errors = []
    _, versions = parse(lang, errors)
    if errors:
        sys.exit("\n".join(errors))
    return [v for v in versions if wanted(v["day"])]


def print_notes(chosen, several):
    """Release notes as a release page has them: the program in a line,
    each version's summary and its sections, in English then Chinese, then
    how to install it and where its documents are."""
    out = []
    for lang in FILES:
        if out:
            out += ["", "---", ""]
        out += ["> " + TAGLINE[lang], ""]
        for version in chosen(lang):
            if several:
                out += ["## v" + version["day"], ""]
            out += version["summary"] + [""]
            for title, lines in version["sections"]:
                out += [("### " if several else "## ") + title, ""]
                out += ["%d. %s" % (n, line) for n, line in enumerate(lines, 1)] + [""]
    out += ["", FOOTER]
    sys.stdout.buffer.write(("\n".join(out).strip("\n") + "\n").encode("utf-8"))


def notes(which):
    versions = versions_of("en", lambda d: True)
    if not versions:
        sys.exit("changelog: no version")
    wanted = versions[0]["day"] if which == "latest" else which.lstrip("v")
    if wanted not in [v["day"] for v in versions]:
        sys.exit("changelog: there is no v%s" % wanted)
    print_notes(lambda lang: versions_of(lang, lambda d: d == wanted), False)


def release():
    tags = [t for t in (git("tag", "--list", "v*", "--sort=-v:refname") or "").split() if re.match(r'^v\d+\.\d+\.\d+$', t)]
    since = None
    if tags:
        since = version_date(git("show", tags[0] + ":src/version.h"))
        if not since:   # a release made before versions were days: its commit's day
            since = (git("log", "-1", "--format=%cd", "--date=format-local:" + DAY_FORMAT, tags[0]) or "").strip() or None
    print_notes(lambda lang: versions_of(lang, lambda d: since is None or d > since), True)


def main(args):
    if args == ["--check"]:
        return check()
    if args == ["--stamp"]:
        stamp()
        return 0
    if len(args) == 2 and args[0] == "--notes":
        notes(args[1])
        return 0
    if args == ["--release"]:
        release()
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
