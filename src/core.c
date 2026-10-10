/*
 * Pure helpers: no registry, no files, no UI, each covered by tests/test_core.c.
 */
#include "app.h"
#include <math.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>

/* ---------------------------------------------------------------- strings */

/* A space of any kind: the ASCII ones and every one Windows classes as a
 * space (no-break, ideographic, U+2000-U+200A, U+202F, U+205F, U+1680,
 * U+0085...). */
static BOOL IsSpace(WCHAR c)
{
    WORD type = 0;
    if (c < 0x80) return c == L' ' || (c >= L'\t' && c <= L'\r');
    return GetStringTypeW(CT_CTYPE1, &c, 1, &type) && (type & C1_SPACE);
}

/* A C0 or C1 control character, DEL included. */
static BOOL IsControl(WCHAR c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F);
}

static size_t TrimmedSpan(const WCHAR *raw, const WCHAR **start)
{
    size_t n;
    while (*raw && IsSpace(*raw)) raw++;
    n = wcslen(raw);
    while (n > 0 && IsSpace(raw[n - 1])) n--;
    *start = raw;
    return n;
}

static BOOL EqualsI(const WCHAR *a, int aLength, const WCHAR *b, int bLength)
{
    return CompareStringOrdinal(a, aLength, b, bLength, TRUE) == CSTR_EQUAL;
}

BOOL Core_EqualsI(const WCHAR *a, const WCHAR *b)
{
    return a && b && EqualsI(a, -1, b, -1);
}

BOOL Core_EndsWithI(const WCHAR *s, const WCHAR *suffix)
{
    size_t length, suffixLength;
    if (!s || !suffix) return FALSE;
    length = wcslen(s);
    suffixLength = wcslen(suffix);
    return suffixLength <= length && EqualsI(s + length - suffixLength, (int)suffixLength, suffix, (int)suffixLength);
}

static const WCHAR *FindI(const WCHAR *s, const WCHAR *needle)
{
    size_t length = wcslen(s), needleLength = wcslen(needle), i;
    if (needleLength == 0 || needleLength > length) return NULL;
    for (i = 0; i + needleLength <= length; i++)
        if (EqualsI(s + i, (int)needleLength, needle, (int)needleLength)) return s + i;
    return NULL;
}

BOOL Core_ContainsI(const WCHAR *s, const WCHAR *needle)
{
    return s && needle && FindI(s, needle) != NULL;
}

/* The length of `path` without its trailing separators; a drive root
 * ("C:\\") keeps its own. */
size_t Core_TrimmedPathLength(const WCHAR *path)
{
    size_t n = wcslen(path);
    while (n > 3 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    return n;
}

/* Like Core_PathEquals, but ordered (an empty path first). */
int Core_PathCompare(const WCHAR *a, const WCHAR *b)
{
    if (!a[0] || !b[0]) return a[0] ? 1 : b[0] ? -1 : 0;
    return CompareStringOrdinal(a, (int)Core_TrimmedPathLength(a), b, (int)Core_TrimmedPathLength(b), TRUE) - CSTR_EQUAL;
}

BOOL Core_PathEquals(const WCHAR *a, const WCHAR *b)
{
    return a && b && *a && *b && Core_PathCompare(a, b) == 0;
}

BOOL Core_IsOurExe(const WCHAR *path)
{
    const WCHAR *name;
    if (!path || !*path) return FALSE;
    name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    return EqualsI(name, -1, APP_EXE, -1);
}

#define HASHED_TEXT_CCH 512

/* FNV-1a of the text in upper case; a text of HASHED_TEXT_CCH characters or
 * more hashes as an empty one (no name hashed is that long). AppUserModelIDs,
 * icon files and watcher mutexes are named after it: it must never change. */
DWORD Core_HashIgnoringCase(const WCHAR *text)
{
    WCHAR upper[HASHED_TEXT_CCH];
    DWORD hash = 2166136261u;
    int length, i;
    length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, text, -1, upper, ARRAYSIZE(upper), NULL, NULL, 0);
    for (i = 0; i < length - 1; i++) {
        hash ^= (DWORD)upper[i];
        hash *= 16777619u;
    }
    return hash;
}

void Core_ProfileAumid(const WCHAR *folder, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, APP_AUMID_PREFIX L"Profile.%08lX", (unsigned long)Core_HashIgnoringCase(folder));
}

ULONGLONG Core_SystemTimeTicks(const SYSTEMTIME *st)
{
    FILETIME fileTime;
    ULARGE_INTEGER ticks;
    if (!SystemTimeToFileTime(st, &fileTime)) return 0;
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return ticks.QuadPart;
}

/* ----------------------------------------------------------- profile names */

static BOOL IsNameChar(WCHAR c)
{
    WORD type = 0;
    if (c == L' ' || c == L'-' || c == L'_' || c == L'.') return TRUE;
    if (IsControl(c) || (c >= 0xD800 && c <= 0xDFFF)) return FALSE;
    if (!GetStringTypeW(CT_CTYPE1, &c, 1, &type)) return FALSE;
    return (type & (C1_ALPHA | C1_DIGIT)) != 0;
}

/* Claude keeps its own siblings next to a profile folder ("<folder>-Data" in
 * %LOCALAPPDATA%, "Claude-3p" in %APPDATA%): a name ending in "-Data" or
 * "-3p" would share them with another profile. */
static BOOL IsReservedSuffix(const WCHAR *s, size_t n)
{
    static const WCHAR *const exact[] = { L"3p", L"Data" };
    static const WCHAR *const tails[] = { L"-3p", L"-Data" };
    size_t i, tailLength;
    for (i = 0; i < ARRAYSIZE(exact); i++)
        if (EqualsI(s, (int)n, exact[i], -1)) return TRUE;
    for (i = 0; i < ARRAYSIZE(tails); i++) {
        tailLength = wcslen(tails[i]);
        if (n >= tailLength && EqualsI(s + n - tailLength, (int)tailLength, tails[i], (int)tailLength)) return TRUE;
    }
    return FALSE;
}

C_ASSERT(MAX_NAME == 32 && MAX_LABEL == 48);   /* the limits the messages below give */

BOOL Core_ValidateNewName(const WCHAR *raw, WCHAR *name, size_t nameCch,
                          WCHAR *folder, size_t folderCch, const WCHAR **error)
{
    const WCHAR *start, *unused;
    size_t n, i;
    if (!error) error = &unused;
    *error = NULL;
    if (!raw) raw = L"";
    n = TrimmedSpan(raw, &start);
    if (n == 0) { *error = L"Enter a name."; return FALSE; }
    if (n > MAX_NAME) { *error = L"Use 32 characters or fewer."; return FALSE; }
    for (i = 0; i < n; i++) {
        if (!IsNameChar(start[i])) {
            *error = L"Use letters, digits, spaces, \x201C-\x201D, \x201C_\x201D or \x201C.\x201D.";
            return FALSE;
        }
    }
    if (start[0] == L'.' || start[n - 1] == L'.') {
        *error = L"The name cannot start or end with a dot.";
        return FALSE;
    }
    if (IsReservedSuffix(start, n)) { *error = L"Claude uses this name itself. Pick another one."; return FALSE; }
    if (FAILED(StringCchCopyNW(name, nameCch, start, n))) { *error = L"Name too long."; return FALSE; }
    if (FAILED(StringCchPrintfW(folder, folderCch, PROFILE_PREFIX L"%s", name))) { *error = L"Name too long."; return FALSE; }
    return TRUE;
}

BOOL Core_ValidateLabel(const WCHAR *raw, WCHAR *label, size_t cch, const WCHAR **error)
{
    const WCHAR *start, *unused;
    size_t n, i;
    if (!error) error = &unused;
    *error = NULL;
    if (!raw) raw = L"";
    n = TrimmedSpan(raw, &start);
    if (n == 0) { *error = L"Enter a name."; return FALSE; }
    if (n > MAX_LABEL) { *error = L"Use 48 characters or fewer."; return FALSE; }
    for (i = 0; i < n; i++) {
        if (IsControl(start[i])) { *error = L"The name contains an invalid character."; return FALSE; }
    }
    if (FAILED(StringCchCopyNW(label, cch, start, n))) { *error = L"Name too long."; return FALSE; }
    return TRUE;
}

BOOL Core_CleanBadge(const WCHAR *raw, WCHAR *badge, size_t cch)
{
    const WCHAR *start;
    size_t n, i, length = 0;
    int characters = 0;
    if (!badge || cch == 0) return FALSE;
    badge[0] = 0;
    if (!raw) return TRUE;
    n = TrimmedSpan(raw, &start);
    for (i = 0; i < n && characters < MAX_BADGE; i++, characters++) {
        BOOL pair = IS_HIGH_SURROGATE(start[i]) && i + 1 < n && IS_LOW_SURROGATE(start[i + 1]);
        if (IsControl(start[i])) {
            badge[0] = 0;
            return FALSE;
        }
        if (length + (pair ? 2 : 1) >= cch) break;
        badge[length++] = start[i];
        if (pair) badge[length++] = start[++i];
    }
    /* "A B" cut after its space. */
    while (length > 0 && IsSpace(badge[length - 1])) length--;
    badge[length] = 0;
    return TRUE;
}

BOOL Core_IsProfileFolder(const WCHAR *folder)
{
    size_t prefixLength = wcslen(PROFILE_PREFIX), n, i;
    if (!folder) return FALSE;
    n = wcslen(folder);
    if (n <= prefixLength || n >= FOLDER_CCH) return FALSE;
    if (!EqualsI(folder, (int)prefixLength, PROFILE_PREFIX, (int)prefixLength)) return FALSE;
    for (i = prefixLength; i < n; i++)
        if (IsControl(folder[i])) return FALSE;
    return !IsReservedSuffix(folder + prefixLength, n - prefixLength);
}

/* ------------------------------------------------------------------- links */

BOOL Core_SanitizeUrl(const WCHAR *in, WCHAR *out, size_t cch)
{
    static const WCHAR hex[] = L"0123456789ABCDEF";
    const WCHAR *start;
    size_t n, i, length = 0;
    if (!in || !out || cch == 0) return FALSE;
    out[0] = 0;
    n = TrimmedSpan(in, &start);
    if (n < 8 || !EqualsI(start, 7, L"claude:", 7)) return FALSE;
    for (i = 0; i < n; i++) {
        WCHAR c = start[i];
        /* The link travels as one quoted argument: a quote, a backslash (it
         * can escape the closing quote) or whitespace must not reach argv. */
        if (c < 0x20 || c == 0x7F || c == L'"' || c == L'\\' || c == L' ') {
            if (length + 3 >= cch) return FALSE;
            out[length++] = L'%';
            out[length++] = hex[(c >> 4) & 0xF];
            out[length++] = hex[c & 0xF];
        } else if (IsSpace(c)) {
            return FALSE;
        } else {
            if (length + 1 >= cch) return FALSE;
            out[length++] = c;
        }
    }
    out[length] = 0;
    return TRUE;
}

/* Looks at the link's path only: words in the query or fragment (a chat
 * prompt, say) never make a link a sign-in. */
BOOL Core_IsSignInUrl(const WCHAR *url)
{
    static const WCHAR *const markers[] = { L"login", L"auth", L"magic-link", L"sso", L"callback" };
    WCHAR path[URL_CCH];
    size_t i;
    if (!url) return FALSE;
    (void)StringCchCopyNW(path, ARRAYSIZE(path), url, wcscspn(url, L"?#"));   /* truncates if longer */
    for (i = 0; i < ARRAYSIZE(markers); i++)
        if (Core_ContainsI(path, markers[i])) return TRUE;
    return FALSE;
}

BOOL Core_BuildLaunchArgs(const WCHAR *dataDir, const WCHAR *url, WCHAR *out, size_t cch)
{
    if (!out || cch == 0) return FALSE;
    out[0] = 0;
    if (dataDir && *dataDir) {
        size_t n = Core_TrimmedPathLength(dataDir);
        if (wcschr(dataDir, L'"')) return FALSE;
        if (FAILED(StringCchPrintfW(out, cch, L"--user-data-dir=\"%.*s\"", (int)n, dataDir))) return FALSE;
    }
    if (url && *url) {
        /* Only a claude: link, never a switch, and nothing that can end the quotes. */
        if (!EqualsI(url, (int)min(wcslen(url), 7), L"claude:", 7) || wcspbrk(url, L"\"\\")) return FALSE;
        if (out[0] && FAILED(StringCchCatW(out, cch, L" "))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, L"\""))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, url))) return FALSE;
        if (FAILED(StringCchCatW(out, cch, L"\""))) return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------ log parsing */

static BOOL IsDigit(char c) { return c >= '0' && c <= '9'; }

static const char kStampShape[] = "dddd-dd-dd dd:dd:dd";
#define STAMP_LENGTH (sizeof kStampShape - 1)

/* The line starts with a "yyyy-mm-dd hh:mm:ss" stamp and goes on after it. */
static BOOL IsStampedLine(const char *line, const char *lineEnd)
{
    size_t i;
    if ((size_t)(lineEnd - line) <= STAMP_LENGTH) return FALSE;
    for (i = 0; i < STAMP_LENGTH; i++) {
        if (kStampShape[i] == 'd' ? !IsDigit(line[i]) : line[i] != kStampShape[i]) return FALSE;
    }
    return TRUE;
}

/* Walks a text line by line: FALSE after the last line. */
static BOOL NextLine(const char **cursor, const char *end, const char **line, const char **lineEnd)
{
    const char *newline;
    if (*cursor >= end) return FALSE;
    newline = (const char *)memchr(*cursor, '\n', (size_t)(end - *cursor));
    *line = *cursor;
    *lineEnd = newline ? newline : end;
    *cursor = newline ? newline + 1 : end;
    return TRUE;
}

static int ParseDigits(const char *p, int count)
{
    int value = 0, i;
    for (i = 0; i < count; i++) value = value * 10 + (p[i] - '0');
    return value;
}

static BOOL LineContains(const char *p, const char *end, const char *needle)
{
    size_t needleLength = strlen(needle);
    for (; (size_t)(end - p) >= needleLength; p++)
        if (memcmp(p, needle, needleLength) == 0) return TRUE;
    return FALSE;
}

/* FALSE for a date or time that does not exist (2026-09-31, 24:00:00). */
static BOOL StampToTime(const char *stamp, SYSTEMTIME *st)
{
    FILETIME unused;
    ZeroMemory(st, sizeof *st);
    st->wYear = (WORD)ParseDigits(stamp, 4);
    st->wMonth = (WORD)ParseDigits(stamp + 5, 2);
    st->wDay = (WORD)ParseDigits(stamp + 8, 2);
    st->wHour = (WORD)ParseDigits(stamp + 11, 2);
    st->wMinute = (WORD)ParseDigits(stamp + 14, 2);
    st->wSecond = (WORD)ParseDigits(stamp + 17, 2);
    return SystemTimeToFileTime(st, &unused);
}

/* Claude logs "<yyyy-mm-dd hh:mm:ss> [info] [Auth] Using system browser for:
 * /login/..." in the main.log of the window that opened the browser. */
BOOL Core_LatestSignInStart(const char *text, size_t len, SYSTEMTIME *latest)
{
    const char *cursor, *line, *lineEnd;
    char best[STAMP_LENGTH];
    SYSTEMTIME when;
    BOOL found = FALSE;
    if (!text || !latest) return FALSE;
    cursor = text;
    while (NextLine(&cursor, text + len, &line, &lineEnd)) {
        if (IsStampedLine(line, lineEnd) && StampToTime(line, &when) &&
            LineContains(line + STAMP_LENGTH, lineEnd, "[Auth] Using system browser for:") &&
            (!found || memcmp(line, best, STAMP_LENGTH) > 0)) {
            memcpy(best, line, STAMP_LENGTH);
            *latest = when;
            found = TRUE;
        }
    }
    return found;
}

/* The last line of a main.log saying why Claude quits. Going down for an
 * update, Claude's updater logs "beforeQuitForUpdate handler fired" and
 * Windows, closing every Claude to replace the package, "Windows session
 * ending (close-app) - quitting the app": that line ends the log of each
 * window. A quit from the window or the tray logs "Quitting app...",
 * "beforeQuit:" and "willQuit:" lines, a Windows shutdown "Windows session
 * ending (shutdown)". A crash logs none: the line found is then an older
 * run's, which the caller tells by its time. */
BOOL Core_LastQuit(const char *text, size_t len, SYSTEMTIME *when, BOOL *forUpdate)
{
    static const char *const update[] = { "beforeQuitForUpdate", "Windows session ending (close-app" };
    static const char *const other[] = { "Windows session ending (", "Quitting app", "beforeQuit:", "willQuit:" };
    const char *cursor, *line, *lineEnd, *last = NULL;
    BOOL lastUpdate = FALSE;
    size_t i;
    if (!text || !when || !forUpdate) return FALSE;
    cursor = text;
    while (NextLine(&cursor, text + len, &line, &lineEnd)) {
        BOOL isUpdate = FALSE, isOther = FALSE;
        if (!IsStampedLine(line, lineEnd)) continue;
        for (i = 0; i < ARRAYSIZE(update) && !isUpdate; i++) isUpdate = LineContains(line + STAMP_LENGTH, lineEnd, update[i]);
        for (i = 0; i < ARRAYSIZE(other) && !isUpdate && !isOther; i++)
            isOther = LineContains(line + STAMP_LENGTH, lineEnd, other[i]);
        if (isUpdate || isOther) {
            last = line;
            lastUpdate = isUpdate;
        }
    }
    if (!last || !StampToTime(last, when)) return FALSE;
    *forUpdate = lastUpdate;
    return TRUE;
}

/* --------------------------------------------------------------- routing */

int Core_SuggestTarget(int count, const ULONGLONG *signInTicks, ULONGLONG nowTicks, ULONGLONG signInMaxAgeTicks,
                       BOOL signInUrl, int lastUsed, int defaultIndex, RouteReason *reason)
{
    /* A stamp up to a minute ahead still counts: the clock may have been set
     * back since Claude wrote it. */
    const ULONGLONG clockSkewTicks = 60 * TICKS_PER_SECOND;
    RouteReason chosen;
    int suggested = -1, i;

    if (count <= 0) {
        chosen = ROUTE_NOTHING_RUNNING;
    } else {
        if (signInUrl) {
            for (i = 0; i < count; i++) {
                ULONGLONG signInTime = signInTicks ? signInTicks[i] : 0;
                if (!signInTime || signInTime > nowTicks + clockSkewTicks) continue;
                if (nowTicks > signInTime && nowTicks - signInTime > signInMaxAgeTicks) continue;
                if (suggested < 0 || signInTime > signInTicks[suggested]) suggested = i;
            }
        }
        if (suggested >= 0) {
            chosen = ROUTE_SIGNIN;
        } else if (count == 1) {
            suggested = 0;
            chosen = ROUTE_ONLY_ONE;
        } else if (lastUsed >= 0 && lastUsed < count) {
            suggested = lastUsed;
            chosen = ROUTE_LAST_USED;
        } else if (defaultIndex >= 0 && defaultIndex < count) {
            suggested = defaultIndex;
            chosen = ROUTE_DEFAULT;
        } else {
            suggested = 0;
            chosen = ROUTE_FIRST;
        }
    }
    if (reason) *reason = chosen;
    return suggested;
}

int Core_NotificationTarget(const BOOL *running, int count, int stock, int topmost, BOOL *ask)
{
    int i, others = 0, first = -1;
    *ask = FALSE;
    for (i = 0; i < count; i++) {
        if (i == stock || !running[i]) continue;
        if (first < 0) first = i;
        others++;
    }
    if (others == 0) return -1;
    if (others == 1) return first;
    *ask = TRUE;
    return topmost >= 0 && topmost < count && topmost != stock && running[topmost] ? topmost : first;
}

/* ---------------------------------------------------------------- shortcuts */

static const WCHAR *NextToken(const WCHAR *p, WCHAR *token, size_t cch)
{
    size_t length = 0;
    BOOL quoted = FALSE;
    while (*p && IsSpace(*p)) p++;
    if (!*p) return NULL;
    while (*p && (quoted || !IsSpace(*p))) {
        if (*p == L'"') { quoted = !quoted; p++; continue; }
        if (length + 1 < cch) token[length++] = *p;
        p++;
    }
    token[length] = 0;
    return p;
}

BOOL Core_ArgsSelectProfile(const WCHAR *args, const WCHAR *folder)
{
    WCHAR token[256];
    BOOL next = FALSE;
    const WCHAR *p = args;
    if (!args || !folder || !*folder) return FALSE;
    while ((p = NextToken(p, token, ARRAYSIZE(token))) != NULL) {
        if (next) return EqualsI(token, -1, folder, -1);
        next = EqualsI(token, -1, L"--launch", -1);
    }
    return FALSE;
}

BOOL Core_ArgsReferenceDir(const WCHAR *args, const WCHAR *dir)
{
    size_t dirLength;
    const WCHAR *p;
    WCHAR bare[MAX_PATH];
    if (!args || !dir || !*dir) return FALSE;
    dirLength = Core_TrimmedPathLength(dir);
    if (dirLength < 4 || FAILED(StringCchCopyNW(bare, ARRAYSIZE(bare), dir, dirLength))) return FALSE;
    for (p = FindI(args, bare); p; p = FindI(p + 1, bare)) {
        WCHAR before = p == args ? L' ' : p[-1];
        WCHAR after = p[dirLength];
        BOOL okBefore = before == L'"' || before == L'=' || IsSpace(before);
        BOOL okAfter = after == 0 || after == L'"' || IsSpace(after) ||
                       (after == L'\\' && (p[dirLength + 1] == 0 || p[dirLength + 1] == L'"' || IsSpace(p[dirLength + 1])));
        if (okBefore && okAfter) return TRUE;
    }
    return FALSE;
}

/* `text` (a package name, or a shell parsing name holding one) names a
 * package called Claude, whatever its publisher: "Claude_" starts it or
 * follows a backslash or a dot (AnthropicPBC.Claude_...). */
BOOL Core_NamesClaudePackage(const WCHAR *text)
{
    static const WCHAR family[] = L"Claude_";
    const WCHAR *p;
    if (!text) return FALSE;
    for (p = FindI(text, family); p; p = FindI(p + 1, family))
        if (p == text || p[-1] == L'\\' || p[-1] == L'.') return TRUE;
    return FALSE;
}

BOOL Core_LinkOpensProfile(const LinkInfo *link, const WCHAR *folder, const WCHAR *dataDir, BOOL isStock)
{
    if (!link) return FALSE;
    if (Core_IsOurExe(link->target)) return Core_ArgsSelectProfile(link->args, folder);
    if (Core_ArgsReferenceDir(link->args, dataDir)) return TRUE;
    if (isStock) {
        if (Core_EndsWithI(link->target, L"\\app\\Claude.exe") && !Core_ContainsI(link->args, L"--user-data-dir"))
            return TRUE;
        if (!link->target[0] && Core_EndsWithI(link->parsing, L"!Claude") && Core_NamesClaudePackage(link->parsing))
            return TRUE;
    }
    return FALSE;
}

/* `path` is `dir` itself or inside it (case-insensitive, whole folder names). */
BOOL Core_PathUnder(const WCHAR *path, const WCHAR *dir)
{
    size_t dirLength;
    if (!path || !dir) return FALSE;
    dirLength = Core_TrimmedPathLength(dir);
    if (dirLength == 0 || wcslen(path) < dirLength || !EqualsI(path, (int)dirLength, dir, (int)dirLength)) return FALSE;
    return dir[dirLength - 1] == L'\\' || dir[dirLength - 1] == L'/' || path[dirLength] == 0 || path[dirLength] == L'\\' ||
           path[dirLength] == L'/';
}

/* Where Claude's package keeps what it writes to AppData for itself:
 * %LOCALAPPDATA%\Packages\<family>\LocalCache\<area>\<name> (area: Local or Roaming). */
BOOL Core_PackageCachePath(const WCHAR *localAppData, const WCHAR *family, const WCHAR *area, const WCHAR *name,
                           WCHAR *out, size_t cch)
{
    if (!localAppData || !*localAppData || !family || !*family) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\Packages\\%s\\LocalCache\\%s\\%s", localAppData, family, area, name));
}

/* Stored working directories use the profile's logical path. File access
 * outside its package uses the same suffix below the resolved storage. */
BOOL Core_ProfileFilePath(const Profile *p, const WCHAR *path, WCHAR *out, size_t cch)
{
    WCHAR resolved[MAX_PATH];
    const WCHAR *result = path;
    size_t logical, physical;
    if (!out || !cch) return FALSE;
    if (!p || !path || !*path) {
        out[0] = 0;
        return FALSE;
    }
    if (Core_PathUnder(path, p->dataDir)) {
        if (!p->storageDir[0]) {
            out[0] = 0;
            return FALSE;
        }
        logical = Core_TrimmedPathLength(p->dataDir);
        physical = Core_TrimmedPathLength(p->storageDir);
        if (FAILED(StringCchPrintfW(resolved, ARRAYSIZE(resolved), L"%.*s%s",
                                    (int)physical, p->storageDir, path + logical))) {
            out[0] = 0;
            return FALSE;
        }
        result = resolved;
    }
    if (result == out) {
        if (wcslen(result) < cch) return TRUE;
    } else if (SUCCEEDED(StringCchCopyW(out, cch, result))) {
        return TRUE;
    }
    out[0] = 0;
    return FALSE;
}

/* The two times are the same to the shell (FAT date and time, 2-second steps). */
BOOL Core_SameFatTime(const FILETIME *a, const FILETIME *b)
{
    WORD dateA, timeA, dateB, timeB;
    if (!FileTimeToDosDateTime(a, &dateA, &timeA) || !FileTimeToDosDateTime(b, &dateB, &timeB)) return FALSE;
    return dateA == dateB && timeA == timeB;
}

void Core_ShortcutFileName(const WCHAR *label, int copyNumber, WCHAR *out, size_t cch)
{
    WCHAR clean[LABEL_CCH];
    size_t i;
    if (FAILED(StringCchCopyW(clean, ARRAYSIZE(clean), label ? label : L""))) clean[0] = 0;
    for (i = 0; clean[i]; i++) {
        if (IsControl(clean[i]) || wcschr(L"\\/:*?\"<>|", clean[i])) clean[i] = L'_';
    }
    if (copyNumber > 1)
        StringCchPrintfW(out, cch, L"Claude (%s) (%d).lnk", clean, copyNumber);
    else
        StringCchPrintfW(out, cch, L"Claude (%s).lnk", clean);
}

/* ------------------------------------------------------------------- JSON */

static BOOL IsJsonSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static size_t SkipSpace(const char *s, size_t len, size_t i)
{
    while (i < len && IsJsonSpace(s[i])) i++;
    return i;
}

/* The end of the JSON string that starts at s[i] (a quote), 0 when unterminated. */
static size_t StringEnd(const char *s, size_t len, size_t i)
{
    for (i++; i < len; i++) {
        if (s[i] == '\\') i++;
        else if (s[i] == '"') return i + 1;
    }
    return 0;
}

/* The end of the JSON value that starts at s[i], 0 when malformed. */
static size_t ValueEnd(const char *s, size_t len, size_t i)
{
    int depth = 0;
    if (i >= len) return 0;
    if (s[i] == '"') return StringEnd(s, len, i);
    if (s[i] != '{' && s[i] != '[') {
        while (i < len && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' && s[i] != '\r' && s[i] != '\n' && s[i] != '\t') i++;
        return i;
    }
    for (; i < len; i++) {
        if (s[i] == '"') {
            i = StringEnd(s, len, i);
            if (!i) return 0;
            i--;
        } else if (s[i] == '{' || s[i] == '[') {
            depth++;
        } else if (s[i] == '}' || s[i] == ']') {
            if (--depth == 0) return i + 1;
        }
    }
    return 0;
}

/* Where the JSON text starts, after a UTF-8 byte order mark. */
static size_t SkipByteOrderMark(const char *json, size_t len)
{
    return len >= 3 && (unsigned char)json[0] == 0xEF && (unsigned char)json[1] == 0xBB && (unsigned char)json[2] == 0xBF ? 3 : 0;
}

/* The raw text of member `key` of the JSON object `json` (not of a nested
 * one), as it is written: quotes, braces and all. */
BOOL Core_JsonMember(const char *json, size_t len, const char *key, const char **value, size_t *valueLen)
{
    size_t i, keyStart, keyEnd, end, keyLen = strlen(key);
    i = SkipSpace(json, len, SkipByteOrderMark(json, len));
    if (i >= len || json[i] != '{') return FALSE;
    i = SkipSpace(json, len, i + 1);
    while (i < len && json[i] == '"') {
        keyStart = i + 1;
        keyEnd = StringEnd(json, len, i);
        if (!keyEnd) return FALSE;
        i = SkipSpace(json, len, keyEnd);
        if (i >= len || json[i] != ':') return FALSE;
        i = SkipSpace(json, len, i + 1);
        end = ValueEnd(json, len, i);
        if (!end || end == i) return FALSE;
        if (keyEnd - 1 - keyStart == keyLen && memcmp(json + keyStart, key, keyLen) == 0) {
            *value = json + i;
            *valueLen = end - i;
            return TRUE;
        }
        i = SkipSpace(json, len, end);
        if (i >= len || json[i] != ',') return FALSE;
        i = SkipSpace(json, len, i + 1);
    }
    return FALSE;
}

BOOL Core_JsonEachMember(const char *json, size_t len, CoreJsonMember each, void *context)
{
    size_t i, keyEnd, end;
    i = SkipSpace(json, len, SkipByteOrderMark(json, len));
    if (i >= len || json[i] != '{') return FALSE;
    i = SkipSpace(json, len, i + 1);
    if (i < len && json[i] == '}') return TRUE;
    while (i < len && json[i] == '"') {
        const char *key = json + i + 1;
        if ((keyEnd = StringEnd(json, len, i)) == 0) return FALSE;
        i = SkipSpace(json, len, keyEnd);
        if (i >= len || json[i] != ':') return FALSE;
        i = SkipSpace(json, len, i + 1);
        if ((end = ValueEnd(json, len, i)) == 0 || end == i) return FALSE;
        if (!each(context, key, (size_t)(json + keyEnd - 1 - key), json + i, end - i)) return FALSE;
        i = SkipSpace(json, len, end);
        if (i < len && json[i] == '}') return TRUE;
        if (i >= len || json[i] != ',') return FALSE;
        i = SkipSpace(json, len, i + 1);
    }
    return FALSE;
}

BOOL Core_JsonIsValue(const char *text, size_t len)
{
    size_t start = SkipSpace(text, len, 0), end;
    if (start >= len || (end = ValueEnd(text, len, start)) == 0 || end == start) return FALSE;
    return SkipSpace(text, len, end) == len;
}

/* Appends `codePoint` in UTF-8 at out[*length], leaving room for a
 * terminating zero in the `cap` bytes: FALSE, nothing appended, when it does
 * not fit. */
static BOOL PutUtf8(char *out, size_t cap, size_t *length, DWORD codePoint)
{
    unsigned char bytes[4];
    size_t count, i;
    if (codePoint < 0x80) {
        bytes[0] = (unsigned char)codePoint;
        count = 1;
    } else if (codePoint < 0x800) {
        bytes[0] = (unsigned char)(0xC0 | (codePoint >> 6));
        bytes[1] = (unsigned char)(0x80 | (codePoint & 0x3F));
        count = 2;
    } else if (codePoint < 0x10000) {
        bytes[0] = (unsigned char)(0xE0 | (codePoint >> 12));
        bytes[1] = (unsigned char)(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (codePoint & 0x3F));
        count = 3;
    } else {
        bytes[0] = (unsigned char)(0xF0 | (codePoint >> 18));
        bytes[1] = (unsigned char)(0x80 | ((codePoint >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((codePoint >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (codePoint & 0x3F));
        count = 4;
    }
    if (*length + count >= cap) return FALSE;
    for (i = 0; i < count; i++) out[(*length)++] = (char)bytes[i];
    return TRUE;
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static BOOL Hex4(const char *p, DWORD *value)
{
    int i, digit;
    *value = 0;
    for (i = 0; i < 4; i++) {
        if ((digit = HexDigit(p[i])) < 0) return FALSE;
        *value = (*value << 4) | (DWORD)digit;
    }
    return TRUE;
}

static BOOL JsonUtf16(const char *utf8, size_t length, WCHAR *out, size_t cch)
{
    int n;
    if (length == 0) return TRUE;
    if (cch <= 1 || length > INT_MAX || cch - 1 > INT_MAX) return FALSE;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, (int)length, out, (int)(cch - 1));
    if (n <= 0) { out[0] = 0; return FALSE; }
    out[n] = 0;
    return TRUE;
}

/* A JSON string as Core_JsonMember gives it (quotes included), decoded to
 * UTF-16. FALSE when it is not a string, is malformed or does not fit. */
BOOL Core_JsonString(const char *raw, size_t len, WCHAR *out, size_t cch)
{
    char *utf8;
    size_t i, length = 0;
    BOOL ok;
    if (!raw || !out || cch == 0 || len < 2 || raw[0] != '"' || raw[len - 1] != '"') return FALSE;
    out[0] = 0;
    for (i = 1; i + 1 < len; i++) {
        if ((unsigned char)raw[i] < 0x20 || raw[i] == '"') return FALSE;
        if (raw[i] == '\\') break;
    }
    if (i + 1 == len) return JsonUtf16(raw + 1, len - 2, out, cch);
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, len);
    if (!utf8) return FALSE;
    for (i = 1; i + 1 < len; i++) {
        char c = raw[i];
        DWORD cp, low;
        if (c != '\\') {
            if ((unsigned char)c < 0x20 || c == '"') goto bad;
            utf8[length++] = c;
            continue;
        }
        if (++i + 1 >= len) goto bad;
        switch (raw[i]) {
        case '"': case '\\': case '/': utf8[length++] = raw[i]; break;
        case 'b': utf8[length++] = '\b'; break;
        case 'f': utf8[length++] = '\f'; break;
        case 'n': utf8[length++] = '\n'; break;
        case 'r': utf8[length++] = '\r'; break;
        case 't': utf8[length++] = '\t'; break;
        case 'u':
            if (i + 5 > len - 1 || !Hex4(raw + i + 1, &cp)) goto bad;
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 7 <= len - 1 && raw[i + 1] == '\\' && raw[i + 2] == 'u' &&
                Hex4(raw + i + 3, &low) && low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 6;
            } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                cp = 0xFFFD;   /* a lone surrogate */
            }
            if (!PutUtf8(utf8, len, &length, cp)) goto bad;
            break;
        default:
            goto bad;
        }
    }
    ok = JsonUtf16(utf8, length, out, cch);
    HeapFree(GetProcessHeap(), 0, utf8);
    return ok;
bad:
    HeapFree(GetProcessHeap(), 0, utf8);
    out[0] = 0;
    return FALSE;
}

/* A JSON number without fraction or sign (a count, a time in ms). */
BOOL Core_JsonNumber(const char *raw, size_t len, ULONGLONG *value)
{
    size_t i;
    *value = 0;
    if (!raw || len == 0) return FALSE;
    for (i = 0; i < len; i++) {
        if (!IsDigit(raw[i]) || *value > (~0ULL - (ULONGLONG)(raw[i] - '0')) / 10) return FALSE;
        *value = *value * 10 + (ULONGLONG)(raw[i] - '0');
    }
    return TRUE;
}

BOOL Core_JsonTrue(const char *raw, size_t len)
{
    return raw && len == 4 && memcmp(raw, "true", 4) == 0;
}

/* The member is there, and neither null nor false. */
static BOOL JsonIsSet(const char *json, size_t len, const char *key)
{
    const char *value;
    size_t valueLength;
    return Core_JsonMember(json, len, key, &value, &valueLength) && !(valueLength == 4 && memcmp(value, "null", 4) == 0) &&
           !(valueLength == 5 && memcmp(value, "false", 5) == 0);
}

/* What a Code session entry (claude-code-sessions\...\local_*.json) is: a
 * session of this PC, one run over SSH, in WSL or in the cloud (its
 * conversation is elsewhere), or not an entry (no sessionId). Claude only
 * checks that these members are set: movedToCloud is true in some versions,
 * an object in others. */
SessionEntryKind Core_SessionEntryKind(const char *json, size_t len)
{
    const char *value;
    size_t valueLength;
    if (!json || !Core_JsonMember(json, len, "sessionId", &value, &valueLength) || valueLength < 3 || value[0] != '"')
        return ENTRY_NOT_SESSION;
    if (JsonIsSet(json, len, "sshConfig") || JsonIsSet(json, len, "wslConfig") || JsonIsSet(json, len, "cloudSessionId") ||
        JsonIsSet(json, len, "movedToCloud"))
        return ENTRY_ELSEWHERE;
    return ENTRY_LOCAL;
}

/* ------------------------------------------------------------ transcripts */

#define TRANSCRIPT_CUT L"\x2026"

typedef struct TranscriptText {        /* what a transcript line shows, as it is put together */
    WCHAR *out;
    size_t cch, used;
    BOOL   full;
} TranscriptText;

/* `text` added, each line break as CRLF (an edit control's), carriage
 * returns of its own left out; what does not fit cut with an ellipsis. */
static void TranscriptPut(TranscriptText *t, const WCHAR *text, size_t length)
{
    size_t i;
    for (i = 0; i < length && !t->full; i++) {
        size_t need = text[i] == L'\n' ? 2 : 1;
        if (text[i] == L'\r') continue;
        if (t->used + need + ARRAYSIZE(TRANSCRIPT_CUT) > t->cch) {
            StringCchCopyW(t->out + t->used, t->cch - t->used, TRANSCRIPT_CUT);
            t->used += ARRAYSIZE(TRANSCRIPT_CUT) - 1;
            t->full = TRUE;
            break;
        }
        if (text[i] == L'\n') t->out[t->used++] = L'\r';
        t->out[t->used++] = text[i];
        t->out[t->used] = 0;
    }
}

/* A JSON string added, blanks around it left out; a block after another on a line of its own. */
static void TranscriptPutString(TranscriptText *t, const char *raw, size_t len, const WCHAR *before)
{
    WCHAR *text;
    size_t start = 0, end;
    if (len < 2 || raw[0] != '"' || t->full) return;
    if ((text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (len + 1) * sizeof(WCHAR))) == NULL) return;
    if (Core_JsonString(raw, len, text, len + 1)) {
        end = wcslen(text);
        while (start < end && iswspace(text[start])) start++;
        while (end > start && iswspace(text[end - 1])) end--;
        if (end > start) {
            if (t->used) TranscriptPut(t, L"\n", 1);
            if (before) TranscriptPut(t, before, wcslen(before));
            TranscriptPut(t, text + start, end - start);
        }
    }
    HeapFree(GetProcessHeap(), 0, text);
}

/* A tool call: "[name] what it works on", from the first of its inputs that names it. */
static void TranscriptPutTool(TranscriptText *t, const char *block, size_t len)
{
    static const char *const kInputs[] = { "description", "command", "file_path", "path", "pattern", "url", "query", "prompt" };
    WCHAR name[64], head[ARRAYSIZE(name) + 4];
    const char *value, *input, *what;
    size_t valueLength, inputLength, whatLength, i;
    if (!Core_JsonMember(block, len, "name", &value, &valueLength) || !Core_JsonString(value, valueLength, name, ARRAYSIZE(name))) return;
    StringCchPrintfW(head, ARRAYSIZE(head), L"[%s] ", name);
    if (Core_JsonMember(block, len, "input", &input, &inputLength))
        for (i = 0; i < ARRAYSIZE(kInputs); i++)
            if (Core_JsonMember(input, inputLength, kInputs[i], &what, &whatLength) && whatLength > 2 && what[0] == '"') {
                size_t before = t->used;
                TranscriptPutString(t, what, whatLength, head);
                if (t->used != before) return;
            }
    if (t->used) TranscriptPut(t, L"\n", 1);
    TranscriptPut(t, head, wcslen(head) - 1);
}

/* One line of a Claude Code transcript (.jsonl) as the preview shows it:
 * who speaks, and what: the text of the message, each tool call on a line
 * of its own as "[name] what". Tool results, thinking, meta lines, side
 * chains and the plumbing of commands (a user's text starting with a tag)
 * show nothing. `out` is cut with an ellipsis past `cch`. */
TranscriptRole Core_TranscriptLine(const char *line, size_t len, WCHAR *out, size_t cch)
{
    TranscriptText t;
    WCHAR type[16];
    const char *value, *message, *content;
    size_t valueLength, messageLength, contentLength, i, end;
    TranscriptRole role;
    if (!out || cch < ARRAYSIZE(TRANSCRIPT_CUT) + 1) return TRANSCRIPT_NONE;
    out[0] = 0;
    if (!line || !Core_JsonMember(line, len, "type", &value, &valueLength) || !Core_JsonString(value, valueLength, type, ARRAYSIZE(type)))
        return TRANSCRIPT_NONE;
    if (wcscmp(type, L"user") == 0) role = TRANSCRIPT_USER;
    else if (wcscmp(type, L"assistant") == 0) role = TRANSCRIPT_CLAUDE;
    else return TRANSCRIPT_NONE;
    if ((Core_JsonMember(line, len, "isMeta", &value, &valueLength) && Core_JsonTrue(value, valueLength)) ||
        (Core_JsonMember(line, len, "isSidechain", &value, &valueLength) && Core_JsonTrue(value, valueLength)) ||
        !Core_JsonMember(line, len, "message", &message, &messageLength) ||
        !Core_JsonMember(message, messageLength, "content", &content, &contentLength))
        return TRANSCRIPT_NONE;
    ZeroMemory(&t, sizeof t);
    t.out = out;
    t.cch = cch;
    if (content[0] == '"') {
        if (role == TRANSCRIPT_USER && contentLength > 1 && content[1] == '<') return TRANSCRIPT_NONE;
        TranscriptPutString(&t, content, contentLength, NULL);
    } else if (content[0] == '[') {
        i = SkipSpace(content, contentLength, 1);
        while (i < contentLength && content[i] != ']' && !t.full) {
            const char *block = content + i;
            WCHAR kind[16];
            if ((end = ValueEnd(content, contentLength, i)) == 0 || end == i) break;
            if (Core_JsonMember(block, end - i, "type", &value, &valueLength) && Core_JsonString(value, valueLength, kind, ARRAYSIZE(kind))) {
                if (wcscmp(kind, L"text") == 0 && Core_JsonMember(block, end - i, "text", &value, &valueLength)) {
                    if (!(role == TRANSCRIPT_USER && valueLength > 1 && value[1] == '<')) TranscriptPutString(&t, value, valueLength, NULL);
                } else if (wcscmp(kind, L"tool_use") == 0) {
                    TranscriptPutTool(&t, block, end - i);
                }
            }
            i = SkipSpace(content, contentLength, end);
            if (i < contentLength && content[i] == ',') i = SkipSpace(content, contentLength, i + 1);
        }
    }
    return t.used ? role : TRANSCRIPT_NONE;
}

/* ---------------------------------------------------------- session edits */

/* `json` with its top-level member `key` set to `raw` (a JSON value as
 * text): replaced where it is, else added last in the object, the rest kept
 * byte for byte. FALSE when `json` is not an object or `out` is too small. */
BOOL Core_JsonSetMember(const char *json, size_t len, const char *key, const char *raw, char *out, size_t cap, size_t *outLen)
{
    const char *member;
    size_t memberLength, written, rawLen = strlen(raw), keyLen = strlen(key), head, tail, last, need, start;
    BOOL empty;
    *outLen = 0;
    start = SkipSpace(json, len, SkipByteOrderMark(json, len));
    if (start >= len || json[start] != '{') return FALSE;
    if (Core_JsonMember(json, len, key, &member, &memberLength)) {
        head = (size_t)(member - json);
        tail = len - head - memberLength;
        if (head + rawLen + tail > cap) return FALSE;
        memcpy(out, json, head);
        memcpy(out + head, raw, rawLen);
        memcpy(out + head + rawLen, member + memberLength, tail);
        *outLen = head + rawLen + tail;
        return TRUE;
    }
    /* Not there: added after the last value. */
    for (last = len; last > 0 && IsJsonSpace(json[last - 1]); last--) {}
    if (last == 0 || json[last - 1] != '}') return FALSE;
    for (head = last - 1; head > start && IsJsonSpace(json[head - 1]); head--) {}
    empty = head == start + 1;
    need = head + (empty ? 0 : 1) + 1 + keyLen + 2 + rawLen + (len - head);
    if (need > cap) return FALSE;
    memcpy(out, json, head);
    written = head;
    if (!empty) out[written++] = ',';
    out[written++] = '"';
    memcpy(out + written, key, keyLen);
    written += keyLen;
    out[written++] = '"';
    out[written++] = ':';
    memcpy(out + written, raw, rawLen);
    written += rawLen;
    memcpy(out + written, json + head, len - head);
    *outLen = written + (len - head);
    return TRUE;
}

/* `json` without its top-level member `key` and the comma that joined it,
 * the rest kept byte for byte; unchanged when it has no such member. `out`
 * may be `json` itself. FALSE when `json` is not an object or `out` is too
 * small. */
BOOL Core_JsonRemoveMember(const char *json, size_t len, const char *key, char *out, size_t cap, size_t *outLen)
{
    size_t i, keyStart, keyEnd, end, next, keyLen = strlen(key), previousEnd, memberStart, from, to;
    *outLen = 0;
    i = SkipSpace(json, len, SkipByteOrderMark(json, len));
    if (i >= len || json[i] != '{') return FALSE;
    previousEnd = i + 1;
    i = SkipSpace(json, len, i + 1);
    while (i < len && json[i] == '"') {
        memberStart = i;
        keyStart = i + 1;
        keyEnd = StringEnd(json, len, i);
        if (!keyEnd) return FALSE;
        i = SkipSpace(json, len, keyEnd);
        if (i >= len || json[i] != ':') return FALSE;
        i = SkipSpace(json, len, i + 1);
        end = ValueEnd(json, len, i);
        if (!end || end == i) return FALSE;
        next = SkipSpace(json, len, end);
        if (keyEnd - 1 - keyStart == keyLen && memcmp(json + keyStart, key, keyLen) == 0) {
            /* The comma after it goes with it; the last member's goes before it. */
            if (next < len && json[next] == ',') {
                from = memberStart;
                to = SkipSpace(json, len, next + 1);
            } else {
                from = previousEnd;
                to = end;
            }
            if (len - (to - from) > cap) return FALSE;
            memmove(out, json, from);
            memmove(out + from, json + to, len - to);
            *outLen = len - (to - from);
            return TRUE;
        }
        if (next >= len || json[next] != ',') break;
        previousEnd = end;
        i = SkipSpace(json, len, next + 1);
    }
    if (len > cap) return FALSE;
    memmove(out, json, len);
    *outLen = len;
    return TRUE;
}

/* `text` as a JSON string, quotes included, in UTF-8. */
BOOL Core_JsonQuote(const WCHAR *text, char *out, size_t cap)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = 0;
    const WCHAR *p;
#define PUT(c) do { if (!PutUtf8(out, cap, &n, (DWORD)(c))) return FALSE; } while (0)
    PUT('"');
    for (p = text; *p; p++) {
        DWORD c = *p;
        if (c == '"' || c == '\\') {
            PUT('\\');
            PUT(c);
        } else if (c < 0x20) {
            PUT('\\'); PUT('u'); PUT('0'); PUT('0'); PUT(hex[c >> 4]); PUT(hex[c & 15]);
        } else if (c >= 0xD800 && c <= 0xDBFF && p[1] >= 0xDC00 && p[1] <= 0xDFFF) {
            PUT(0x10000 + ((c - 0xD800) << 10) + (p[1] - 0xDC00));
            p++;
        } else if (c >= 0xD800 && c <= 0xDFFF) {
            PUT(0xFFFD);   /* a lone surrogate */
        } else {
            PUT(c);
        }
    }
    PUT('"');
#undef PUT
    out[n] = 0;
    return TRUE;
}

/* The folder Claude Code keeps a working folder's transcripts in, under
 * ~\.claude\projects: every character but an ASCII letter or digit becomes
 * '-'. Past CORE_PROJECT_NAME_MAX characters Claude Code adds a hash of its
 * own: FALSE then. */
BOOL Core_ProjectDirName(const WCHAR *cwd, WCHAR *out, size_t cch)
{
    size_t i, len = wcslen(cwd);
    if (len == 0 || len > CORE_PROJECT_NAME_MAX || len >= cch) return FALSE;
    for (i = 0; i < len; i++) {
        WCHAR c = cwd[i];
        out[i] = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') ? c : L'-';
    }
    out[len] = 0;
    return TRUE;
}

/* A UUID as Claude Code writes session ids: 8-4-4-4-12 hex digits. */
BOOL Core_IsUuid(const WCHAR *id)
{
    int i;
    if (!id || wcslen(id) != 36) return FALSE;
    for (i = 0; i < 36; i++) {
        WCHAR c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != L'-') return FALSE;
        } else if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F'))) {
            return FALSE;
        }
    }
    return TRUE;
}

/* The link Claude Desktop opens a Claude Code session with (adding it to
 * the profile's list when it is not there yet). */
BOOL Core_ResumeLink(const WCHAR *sessionId, WCHAR *out, size_t cch)
{
    return Core_IsUuid(sessionId) && SUCCEEDED(StringCchPrintfW(out, cch, L"claude://resume?session=%s", sessionId));
}

/* A new "no folder" working folder's name, as Claude Desktop names them:
 * scratch-<date>-<6 hex digits>. */
void Core_ScratchName(const SYSTEMTIME *day, DWORD random, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, L"scratch-%04u-%02u-%02u-%06lx", day->wYear, day->wMonth, day->wDay, (unsigned long)(random & 0xFFFFFF));
}

/* The "no folder" area of an entries folder (...\claude-code-sessions\
 * <account>\<organization>): <dataDir>\scratch-workspaces\<account>\<organization>. */
BOOL Core_ScratchDirFor(const WCHAR *dataDir, const WCHAR *entriesDir, WCHAR *out, size_t cch)
{
    const WCHAR *organization = wcsrchr(entriesDir, L'\\'), *account;
    if (!organization || organization == entriesDir || !organization[1]) return FALSE;
    for (account = organization - 1; account > entriesDir && *account != L'\\'; account--) {}
    if (*account != L'\\' || account + 1 == organization) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\scratch-workspaces%s", dataDir, account));
}

/* Copies `in` to `out` with every `swaps[i].from` replaced by its `to`, the
 * first swap that matches at a place winning, for a file read in chunks: when
 * a swap could start in the last bytes and this is not the `last` chunk, they
 * are left for the next one (`*used` says how much of `in` was taken). A swap
 * held there keeps the later ones from matching at that place, so where the
 * file is cut never changes the result. Returns the bytes written; `out` must
 * hold at least len + (len / shortest from + 1) * longest to bytes. */
size_t Core_ReplaceChunk(const char *in, size_t len, const CoreSwap *swaps, int count, BOOL last, char *out, size_t *used)
{
    size_t i = 0, n = 0;
    int swap;
    while (i < len) {
        BOOL held = FALSE, swapped = FALSE;
        for (swap = 0; swap < count && !swapped && !held; swap++) {
            const char *from = swaps[swap].from;
            size_t matched = 0;
            /* Compared as far as it matches, so most bytes cost one comparison. */
            while (from[matched] && i + matched < len && in[i + matched] == from[matched]) matched++;
            if (matched == 0) continue;
            if (!from[matched]) {
                size_t toLength = strlen(swaps[swap].to);
                memcpy(out + n, swaps[swap].to, toLength);
                n += toLength;
                i += matched;
                swapped = TRUE;
            } else if (i + matched == len && !last) {
                held = TRUE;
            }
        }
        if (swapped) continue;
        if (held) break;
        out[n++] = in[i++];
    }
    *used = i;
    return n;
}

/* A change to a session entry waiting for its profile to close, one per
 * line: "<op>\t<session id>\t<value>". */
static const WCHAR *const kPendingOps[] = { L"title", L"star", L"remove" };
C_ASSERT(ARRAYSIZE(kPendingOps) == PENDING_REMOVE + 1);

BOOL Core_PendingFormat(const PendingEdit *edit, WCHAR *out, size_t cch)
{
    WCHAR value[SESSION_TITLE_CCH];
    size_t i;
    if ((int)edit->op < 0 || (int)edit->op >= (int)ARRAYSIZE(kPendingOps) || !edit->key[0] || wcspbrk(edit->key, L"\t\r\n"))
        return FALSE;
    StringCchCopyW(value, ARRAYSIZE(value), edit->value);
    for (i = 0; value[i]; i++)
        if (value[i] == L'\t' || value[i] == L'\r' || value[i] == L'\n') value[i] = L' ';
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\t%s\t%s", kPendingOps[edit->op], edit->key, value));
}

BOOL Core_PendingParse(const WCHAR *line, PendingEdit *edit)
{
    const WCHAR *tab1 = wcschr(line, L'\t'), *tab2;
    size_t op;
    ZeroMemory(edit, sizeof *edit);
    if (!tab1 || (tab2 = wcschr(tab1 + 1, L'\t')) == NULL || tab2 == tab1 + 1) return FALSE;
    for (op = 0; op < ARRAYSIZE(kPendingOps); op++)
        if ((size_t)(tab1 - line) == wcslen(kPendingOps[op]) && wcsncmp(line, kPendingOps[op], (size_t)(tab1 - line)) == 0) break;
    if (op == ARRAYSIZE(kPendingOps)) return FALSE;
    edit->op = (PendingOp)op;
    if (FAILED(StringCchCopyNW(edit->key, ARRAYSIZE(edit->key), tab1 + 1, (size_t)(tab2 - tab1 - 1)))) return FALSE;
    StringCchCopyW(edit->value, ARRAYSIZE(edit->value), tab2 + 1);
    return TRUE;
}

/* A change queued after `queued` replaces it: one of the same kind to the
 * same session, and a removal replaces them all (and is replaced by any). */
BOOL Core_PendingReplaces(const PendingEdit *queued, const PendingEdit *added)
{
    return EqualsI(queued->key, -1, added->key, -1) &&
           (queued->op == added->op || queued->op == PENDING_REMOVE || added->op == PENDING_REMOVE);
}

/* ------------------------------------------------------------- sync plans */

/* A profile's plan of changes sent to it (sessionsync.c): UTF-8, one change
 * a line, "<kind>\t<flags>\t<time>\t<seen>\t<key>\t<content>". */
static const WCHAR *const kSyncOps[] = { L"put", L"remove", L"mark", L"unmark", L"index", L"layout" };
C_ASSERT(ARRAYSIZE(kSyncOps) == SYNC_LAYOUT + 1);

/* A name of a file in a folder of ours: no path, no dots of its own. */
static BOOL IsPlainContentName(const WCHAR *name)
{
    const WCHAR *c;
    if (!name[0] || name[0] == L'.') return FALSE;
    for (c = name; *c; c++)
        if (!((*c >= L'0' && *c <= L'9') || (*c >= L'a' && *c <= L'z') || *c == L'.' || *c == L'-')) return FALSE;
    return TRUE;
}

BOOL Core_SyncOpFormat(const SyncOp *op, WCHAR *out, size_t cch)
{
    if ((int)op->kind < 0 || (int)op->kind >= (int)ARRAYSIZE(kSyncOps) || !op->key[0] || wcspbrk(op->key, L"\t\r\n") ||
        (op->content[0] && !IsPlainContentName(op->content)))
        return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\t%lu\t%I64u\t%I64u\t%s\t%s", kSyncOps[op->kind], op->flags, op->time, op->seen,
                                      op->key, op->content));
}

/* A decimal number of a plan line up to `end`. */
static BOOL ParsePlanNumber(const WCHAR *start, const WCHAR *end, ULONGLONG limit, ULONGLONG *value)
{
    ULONGLONG n = 0;
    const WCHAR *c;
    if (start == end) return FALSE;
    for (c = start; c < end; c++) {
        if (*c < L'0' || *c > L'9' || n > (limit - (ULONGLONG)(*c - L'0')) / 10) return FALSE;
        n = n * 10 + (ULONGLONG)(*c - L'0');
    }
    *value = n;
    return TRUE;
}

BOOL Core_SyncOpParse(const WCHAR *line, SyncOp *op)
{
    const WCHAR *field[6], *end[6], *at = line;
    ULONGLONG flags;
    size_t kind;
    int i;
    ZeroMemory(op, sizeof *op);
    for (i = 0; i < 6; i++) {
        field[i] = at;
        end[i] = i < 5 ? wcschr(at, L'\t') : at + wcslen(at);
        if (!end[i]) return FALSE;
        at = end[i] + 1;
    }
    for (kind = 0; kind < ARRAYSIZE(kSyncOps); kind++)
        if ((size_t)(end[0] - field[0]) == wcslen(kSyncOps[kind]) && wcsncmp(field[0], kSyncOps[kind], (size_t)(end[0] - field[0])) == 0) break;
    if (kind == ARRAYSIZE(kSyncOps) || !ParsePlanNumber(field[1], end[1], MAXDWORD, &flags) ||
        !ParsePlanNumber(field[2], end[2], _UI64_MAX, &op->time) || !ParsePlanNumber(field[3], end[3], _UI64_MAX, &op->seen) ||
        end[4] == field[4] || FAILED(StringCchCopyNW(op->key, ARRAYSIZE(op->key), field[4], (size_t)(end[4] - field[4]))) ||
        FAILED(StringCchCopyNW(op->content, ARRAYSIZE(op->content), field[5], (size_t)(end[5] - field[5]))) ||
        (op->content[0] && !IsPlainContentName(op->content)))
        return FALSE;
    op->kind = (SyncOpKind)kind;
    op->flags = (DWORD)flags;
    return TRUE;
}

static int SyncOpFamily(SyncOpKind kind)
{
    switch (kind) {
    case SYNC_PUT: case SYNC_REMOVE: return 0;
    case SYNC_MARK: case SYNC_UNMARK: return 1;
    case SYNC_INDEX: return 2;
    default: return 3;
    }
}

/* A change sent after `queued` replaces it: a put or a removal of the same
 * session, a mark or its removal for the same id, a list of archived
 * sessions, the sessions' pins and groups. */
BOOL Core_SyncOpReplaces(const SyncOp *queued, const SyncOp *added)
{
    return SyncOpFamily(queued->kind) == SyncOpFamily(added->kind) && EqualsI(queued->key, -1, added->key, -1);
}

/* What every profile of a group keeping the same sessions gets of one
 * session, from how each one has it (`sides`) and how the group's list had it
 * at the last sync (`base`, NULL when it never had it): the index of the side
 * whose entry they all take (`count` for the base's own), CORE_MIRROR_DELETED
 * when it goes everywhere, CORE_MIRROR_NOWHERE when no profile has it. A side
 * changed since the base wins, the latest written first, unless a deletion
 * came later; a profile that only lacks it (never had it, or lost its whole
 * list) gets it back. */
int Core_MirrorResolve(const MirrorSide *sides, int count, const MirrorSide *base)
{
    BOOL baseListed = base && base->state == MIRROR_LISTED, deleted = FALSE;
    ULONGLONG deletedAt = 0;
    int i, winner = -1;
    for (i = 0; i < count; i++) {
        const MirrorSide *side = &sides[i];
        if (side->state == MIRROR_LISTED) {
            if (baseListed && side->hash == base->hash) continue;
            if (winner < 0 || side->time > sides[winner].time) winner = i;
        } else if (side->state == MIRROR_DELETED && (!base || baseListed)) {
            deleted = TRUE;
            deletedAt = max(deletedAt, side->time);
        } else if (side->state == MIRROR_REMOVED && baseListed) {
            deleted = TRUE;   /* taken away by Claude, its time unknown: any change made elsewhere wins */
        }
    }
    if (winner >= 0) return deleted && deletedAt > sides[winner].time ? CORE_MIRROR_DELETED : winner;
    if (deleted) return CORE_MIRROR_DELETED;
    if (baseListed) {
        for (i = 0; i < count; i++)
            if (sides[i].state == MIRROR_LISTED) return i;
        return count;
    }
    return base ? CORE_MIRROR_DELETED : CORE_MIRROR_NOWHERE;
}

/* The name of a copy of folder `name` made on `day`: "<name>_yyyymmdd", then
 * "_2", "_3"... for the `copy`th of that day. */
BOOL Core_DatedCopyName(const WCHAR *name, const SYSTEMTIME *day, int copy, WCHAR *out, size_t cch)
{
    if (!name[0] || copy < 1) return FALSE;
    if (copy == 1) return SUCCEEDED(StringCchPrintfW(out, cch, L"%s_%04u%02u%02u", name, day->wYear, day->wMonth, day->wDay));
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s_%04u%02u%02u_%d", name, day->wYear, day->wMonth, day->wDay, copy));
}

#define WEEKLY_COPY_TAG L"_auto_"

BOOL Core_WeeklyCopyName(const WCHAR *name, const SYSTEMTIME *day, WCHAR *out, size_t cch)
{
    return name[0] && SUCCEEDED(StringCchPrintfW(out, cch, L"%s" WEEKLY_COPY_TAG L"%04u%02u%02u", name, day->wYear, day->wMonth, day->wDay));
}

/* Eight digits that make a real day. */
static BOOL ReadDay(const WCHAR *digits, SYSTEMTIME *day)
{
    FILETIME unused;
    int i, value[8];
    for (i = 0; i < 8; i++) {
        if (digits[i] < L'0' || digits[i] > L'9') return FALSE;
        value[i] = digits[i] - L'0';
    }
    ZeroMemory(day, sizeof *day);
    day->wYear = (WORD)(value[0] * 1000 + value[1] * 100 + value[2] * 10 + value[3]);
    day->wMonth = (WORD)(value[4] * 10 + value[5]);
    day->wDay = (WORD)(value[6] * 10 + value[7]);
    return SystemTimeToFileTime(day, &unused);
}

BOOL Core_CopyDay(const WCHAR *name, const WCHAR *copy, SYSTEMTIME *day, BOOL *weekly)
{
    size_t length = wcslen(name), tag = ARRAYSIZE(WEEKLY_COPY_TAG) - 1;
    const WCHAR *rest, *end;
    if (!length || _wcsnicmp(copy, name, length) != 0 || copy[length] != L'_') return FALSE;
    rest = copy + length;
    *weekly = _wcsnicmp(rest, WEEKLY_COPY_TAG, tag) == 0;
    rest += *weekly ? tag : 1;
    if (wcslen(rest) < 8 || !ReadDay(rest, day)) return FALSE;
    end = rest + 8;
    if (!*end) return TRUE;
    if (*weekly || end[0] != L'_' || !end[1]) return FALSE;   /* "_2", "_3"... of a day */
    for (end++; *end; end++)
        if (*end < L'0' || *end > L'9') return FALSE;
    return TRUE;
}

BOOL Core_WeeklyCopyDue(const SYSTEMTIME *latest, const SYSTEMTIME *today)
{
    SYSTEMTIME from = *latest, to = *today;
    FILETIME a, b;
    if (!latest->wYear) return TRUE;
    from.wHour = from.wMinute = from.wSecond = from.wMilliseconds = 0;
    to.wHour = to.wMinute = to.wSecond = to.wMilliseconds = 0;
    if (!SystemTimeToFileTime(&from, &a) || !SystemTimeToFileTime(&to, &b)) return TRUE;
    return (((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime) >=
           (((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime) + (ULONGLONG)CORE_WEEKLY_COPY_DAYS * 24 * 3600 * TICKS_PER_SECOND;
}

/* The version the manager shows, the time this copy was built: the
 * compiler's __DATE__ ("Oct  8 2026") and __TIME__ ("17:20:33") as
 * "2026.10.08 17:20". */
static int Digits(const char *text, int count)
{
    int value = 0, i;
    for (i = 0; i < count; i++) {
        if (text[i] == ' ' && value == 0) continue;
        if (text[i] < '0' || text[i] > '9') return -1;
        value = value * 10 + (text[i] - '0');
    }
    return value;
}

BOOL Core_BuildStamp(const char *date, const char *time, WCHAR *out, size_t cch)
{
    static const char kMonths[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    int month, day, year, hour, minute;
    if (cch) out[0] = 0;
    if (!date || !time || strlen(date) != 11 || strlen(time) != 8 || date[3] != ' ' || date[6] != ' ' || time[2] != ':' || time[5] != ':')
        return FALSE;
    for (month = 0; month < 12 && memcmp(kMonths + 3 * month, date, 3) != 0; month++) {}
    day = Digits(date + 4, 2);
    year = Digits(date + 7, 4);
    hour = Digits(time, 2);
    minute = Digits(time + 3, 2);
    if (month == 12 || day < 1 || day > 31 || year < 0 || hour < 0 || hour > 23 || minute < 0 || minute > 59) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%04d.%02d.%02d %02d:%02d", year, month + 1, day, hour, minute));
}

/* The name of the "no folder" working folder `cwd` is, in whichever
 * profile's area it is: the folder after scratch-workspaces\<account>\
 * <organization>\. FALSE when `cwd` is in no such area. */
BOOL Core_ScratchFolderName(const WCHAR *cwd, WCHAR *out, size_t cch)
{
    static const WCHAR kArea[] = L"\\scratch-workspaces\\";
    const size_t areaLength = ARRAYSIZE(kArea) - 1;
    const WCHAR *at, *name = NULL, *end;
    int skipped;
    if (cch) out[0] = 0;
    for (at = cwd; *at && !name; at++)
        if (_wcsnicmp(at, kArea, areaLength) == 0) name = at + areaLength;
    if (!name) return FALSE;
    for (skipped = 0; skipped < 2; skipped++) {
        const WCHAR *slash = wcschr(name, L'\\');
        if (!slash || slash == name) return FALSE;
        name = slash + 1;
    }
    end = wcschr(name, L'\\');
    if (!end) end = name + wcslen(name);
    if (end == name || (end - name == 1 && name[0] == L'.') || (end - name == 2 && name[0] == L'.' && name[1] == L'.')) return FALSE;
    return SUCCEEDED(StringCchCopyNW(out, cch, name, (size_t)(end - name)));
}

/* `json` with the member reached through `keys` (an object at each level,
 * one made where it is missing) set to `raw` (NUL-terminated): a heap block
 * with a NUL after it (HeapFree it); NULL when `json` holds something else
 * than an object on the way, or without memory. */
char *Core_JsonSetNested(const char *json, size_t len, const char *const *keys, int depth, const char *raw, size_t *outLen)
{
    const char *inner;
    size_t innerLength, setLength = 0, cap;
    char *set = NULL, *out;
    BOOL ok;
    *outLen = 0;
    if (depth < 1) return NULL;
    if (depth == 1) {
        set = (char *)raw;
        setLength = strlen(raw);
    } else {
        if (!Core_JsonMember(json, len, keys[0], &inner, &innerLength)) {
            inner = "{}";
            innerLength = 2;
        }
        if ((set = Core_JsonSetNested(inner, innerLength, keys + 1, depth - 1, raw, &setLength)) == NULL) return NULL;
    }
    cap = len + setLength + strlen(keys[0]) + 8;
    out = (char *)HeapAlloc(GetProcessHeap(), 0, cap + 1);
    ok = out && Core_JsonSetMember(json, len, keys[0], set, out, cap, outLen);
    if (depth > 1) HeapFree(GetProcessHeap(), 0, set);
    if (!ok) {
        if (out) HeapFree(GetProcessHeap(), 0, out);
        *outLen = 0;
        return NULL;
    }
    out[*outLen] = 0;
    return out;
}

/* `json` with the text of each of its strings, keys too, that `map`
 * replaces: `map` gets a string's raw text (between its quotes) and writes
 * its replacement into `out` (`cap` bytes), returning its length, or
 * returns (size_t)-1 to keep it. A heap block with a NUL after it (HeapFree
 * it), NULL without memory or for a string left open. */
char *Core_JsonMapStrings(const char *json, size_t len, CoreStringMap map, void *context, size_t *outLen)
{
    char replacement[512], *out, *grown;
    size_t cap = len + 64, n = 0, i = 0;
    *outLen = 0;
    if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, cap + 1)) == NULL) return NULL;
    while (i < len) {
        size_t piece, end, mapped;
        const char *from;
        if (json[i] == '"') {
            if ((end = StringEnd(json, len, i)) == 0) {
                HeapFree(GetProcessHeap(), 0, out);
                return NULL;
            }
            mapped = map(context, json + i + 1, end - i - 2, replacement, sizeof replacement);
            if (mapped != (size_t)-1 && mapped <= sizeof replacement) {
                if (n + mapped + 2 > cap) goto grow;
                out[n++] = '"';
                memcpy(out + n, replacement, mapped);
                n += mapped;
                out[n++] = '"';
                i = end;
                continue;
            }
            from = json + i;
            piece = end - i;
        } else {
            from = json + i;
            piece = 1;
        }
        if (n + piece > cap) goto grow;
        memcpy(out + n, from, piece);
        n += piece;
        i += piece;
        continue;
grow:
        if (cap > ((size_t)-1 - 1) / 2 || (grown = (char *)HeapReAlloc(GetProcessHeap(), 0, out, cap * 2 + 1)) == NULL) {
            HeapFree(GetProcessHeap(), 0, out);
            return NULL;
        }
        out = grown;
        cap *= 2;
    }
    out[n] = 0;
    *outLen = n;
    return out;
}

/* A time in ms since 1970 as transcripts write theirs: ISO 8601 in UTC with
 * milliseconds, "2026-10-08T08:55:12.345Z", which sorts as the time does. */
BOOL Core_IsoTime(ULONGLONG ms, char *out, size_t cap)
{
    ULONGLONG ticks = ms * 10000ULL + 116444736000000000ULL;
    FILETIME time;
    SYSTEMTIME utc;
    time.dwLowDateTime = (DWORD)ticks;
    time.dwHighDateTime = (DWORD)(ticks >> 32);
    return FileTimeToSystemTime(&time, &utc) &&
           SUCCEEDED(StringCchPrintfA(out, cap, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
                                      utc.wMinute, utc.wSecond, utc.wMilliseconds));
}

/* One line of a transcript, as the branch analysis reads it. */
typedef struct TranscriptLine {
    size_t start, length;   /* in the text, without its line break */
    char   uuid[40], parent[40], time[32];
    BOOL   message;         /* a user or assistant message of the main conversation */
    int    parentLine;      /* -1: none found */
} TranscriptLine;

static void LineString(const char *line, size_t length, const char *key, char *out, size_t cap)
{
    const char *value;
    size_t valueLength;
    out[0] = 0;
    if (Core_JsonMember(line, length, key, &value, &valueLength) && valueLength >= 2 && value[0] == '"' && valueLength - 2 < cap &&
        !memchr(value + 1, '\\', valueLength - 2)) {
        memcpy(out, value + 1, valueLength - 2);
        out[valueLength - 2] = 0;
    }
}

/* The last message of the main conversation written at `until` (with a
 * second's slack) or before, and after `since`: -1 for none. */
static int LastMessageBy(const TranscriptLine *lines, int count, const char *since, const char *until)
{
    int i, best = -1;
    for (i = 0; i < count; i++) {
        if (!lines[i].message || !lines[i].time[0] || strcmp(lines[i].time, since) <= 0 || strcmp(lines[i].time, until) > 0) continue;
        if (best < 0 || strcmp(lines[i].time, lines[best].time) >= 0) best = i;
    }
    return best;
}

/* Marks `line` and every line it goes on from. */
static void MarkChain(const TranscriptLine *lines, int count, int line, BOOL *marked)
{
    int steps;
    for (steps = 0; line >= 0 && line < count && !marked[line] && steps <= count; steps++) {
        marked[line] = TRUE;
        line = lines[line].parentLine;
    }
}

/* A transcript two profiles both went on with, apart: `keepTime` and
 * `dropTime` (ms since 1970) are when each one last used it, `since` when
 * the profiles last had it alike. TRUE when it holds two branches written
 * after `since`, the one each went on with: `excluded` (a heap array, one
 * flag per line, HeapFree it) then marks the lines a copy going on from the
 * `drop` branch leaves out, the ones of the `keep` branch alone and what
 * follows them; `lineCount` gets the lines. FALSE for one line of
 * conversation (one went on from the other), or no line new since then. */
BOOL Core_TranscriptBranches(const char *text, size_t len, ULONGLONG keepTime, ULONGLONG dropTime, ULONGLONG since, BOOL **excluded,
                             int *lineCount)
{
    TranscriptLine *lines;
    BOOL *keepChain = NULL, *dropChain = NULL, *out = NULL, ok = FALSE;
    char sinceText[32], keepText[32], dropText[32];
    size_t at = 0;
    int count = 0, capacity = 0, i, j, keep, drop;
    *excluded = NULL;
    *lineCount = 0;
    if (!Core_IsoTime(since, sinceText, sizeof sinceText) || !Core_IsoTime(keepTime + 1000, keepText, sizeof keepText) ||
        !Core_IsoTime(dropTime + 1000, dropText, sizeof dropText))
        return FALSE;
    for (i = 0; (size_t)i < len; i++)
        if (text[i] == '\n') capacity++;
    capacity++;
    if ((lines = (TranscriptLine *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)capacity * sizeof *lines)) == NULL) return FALSE;
    while (at < len && count < capacity) {
        const char *end = (const char *)memchr(text + at, '\n', len - at), *value;
        TranscriptLine *line = &lines[count++];
        char type[24], sidechain[8];
        size_t valueLength;
        line->start = at;
        line->length = end ? (size_t)(end - (text + at)) : len - at;
        if (line->length && text[at + line->length - 1] == '\r') line->length--;
        LineString(text + at, line->length, "uuid", line->uuid, sizeof line->uuid);
        LineString(text + at, line->length, "parentUuid", line->parent, sizeof line->parent);
        if (!line->parent[0]) LineString(text + at, line->length, "logicalParentUuid", line->parent, sizeof line->parent);
        LineString(text + at, line->length, "timestamp", line->time, sizeof line->time);
        LineString(text + at, line->length, "type", type, sizeof type);
        sidechain[0] = 0;
        if (Core_JsonMember(text + at, line->length, "isSidechain", &value, &valueLength) && Core_JsonTrue(value, valueLength))
            StringCchCopyA(sidechain, sizeof sidechain, "true");
        line->message = line->uuid[0] && !sidechain[0] && (strcmp(type, "user") == 0 || strcmp(type, "assistant") == 0);
        line->parentLine = -1;
        at = end ? (size_t)(end - text) + 1 : len;
    }
    /* Each line's parent, found among the lines before it (a transcript is written in order). */
    for (i = 0; i < count; i++) {
        if (!lines[i].parent[0]) continue;
        for (j = i - 1; j >= 0; j--)
            if (lines[j].uuid[0] && strcmp(lines[j].uuid, lines[i].parent) == 0) {
                lines[i].parentLine = j;
                break;
            }
    }
    keep = LastMessageBy(lines, count, sinceText, keepText);
    drop = LastMessageBy(lines, count, sinceText, dropText);
    if (keep < 0 || drop < 0 || keep == drop) goto done;
    keepChain = (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count * sizeof *keepChain);
    dropChain = (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count * sizeof *dropChain);
    out = (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count * sizeof *out);
    if (!keepChain || !dropChain || !out) goto done;
    MarkChain(lines, count, keep, keepChain);
    MarkChain(lines, count, drop, dropChain);
    /* One went on from the other: a single line of conversation. */
    if (keepChain[drop] || dropChain[keep]) goto done;
    for (i = 0; i < count; i++)
        out[i] = (keepChain[i] && !dropChain[i]) || (lines[i].parentLine >= 0 && out[lines[i].parentLine] && !dropChain[i]);
    *excluded = out;
    *lineCount = count;
    out = NULL;
    ok = TRUE;
done:
    if (keepChain) HeapFree(GetProcessHeap(), 0, keepChain);
    if (dropChain) HeapFree(GetProcessHeap(), 0, dropChain);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    HeapFree(GetProcessHeap(), 0, lines);
    return ok;
}

/* ---------------------------------------------------------------- archives */

/* CRC-32 (ISO 3309, as ZIP files check their content), going on from `crc`
 * (0 to start). */
DWORD Core_Crc32(DWORD crc, const void *data, size_t size)
{
    static DWORD table[256];
    static volatile LONG made;
    const unsigned char *p = (const unsigned char *)data;
    if (!made) {
        DWORD n, k, c;
        for (n = 0; n < 256; n++) {
            for (c = n, k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        InterlockedExchange(&made, 1);
    }
    crc = ~crc;
    while (size--) crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* An archive's file name (UTF-8, '/' between folders) that stays inside the
 * folder it is extracted to and names one file there: relative, no empty,
 * "." or ".." part, no backslash, drive, stream or character Windows
 * refuses, no part ending in a dot or a space (Windows would drop it). */
BOOL Core_ArchiveNameSafe(const char *name, size_t length)
{
    size_t i, partStart = 0;
    if (length == 0 || name[0] == '/') return FALSE;
    for (i = 0; i <= length; i++) {
        unsigned char c = i < length ? (unsigned char)name[i] : '/';
        if (c == '/') {
            size_t part = i - partStart;
            if (part == 0 || (part == 1 && name[partStart] == '.') || (part == 2 && name[partStart] == '.' && name[partStart + 1] == '.') ||
                name[i - 1] == '.' || name[i - 1] == ' ')
                return FALSE;
            partStart = i + 1;
        } else if (c < 0x20 || c == 0x7F || strchr("\\:*?\"<>|", c)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* A path under Claude Code's folder (`relative`, '\\' between folders) that
 * holds part of a conversation, as an archive's files may: in a project
 * folder, a transcript (<id>.jsonl), its folder (<id>\\...) or its state
 * before compacting (<id>.precompact.json); in file-history, uploads, tasks
 * or image-cache, the folder of a transcript (<store>\\<id>\\...). Nothing
 * else of Claude Code's (its settings, hooks, memory) comes from an archive. */
BOOL Core_ConversationFileName(const WCHAR *relative)
{
    static const WCHAR *const kStores[] = { L"file-history", L"uploads", L"tasks", L"image-cache" };
    WCHAR id[UUID_TEXT_CCH];
    const WCHAR *slash = wcschr(relative, L'\\'), *part, *rest;
    size_t i, storeLength;
    BOOL project;
    if (!slash) return FALSE;
    storeLength = (size_t)(slash - relative);
    project = storeLength == 8 && _wcsnicmp(relative, L"projects", 8) == 0;
    part = slash + 1;
    if (project && (part = wcschr(part, L'\\')) != NULL) part++;   /* past the project's folder */
    if (!part || wcslen(part) < UUID_TEXT_CCH - 1 || FAILED(StringCchCopyNW(id, ARRAYSIZE(id), part, UUID_TEXT_CCH - 1)) ||
        !Core_IsUuid(id))
        return FALSE;
    rest = part + UUID_TEXT_CCH - 1;
    if (*rest == L'\\' && rest[1]) {
        if (project) return TRUE;
        for (i = 0; i < ARRAYSIZE(kStores); i++)
            if (wcslen(kStores[i]) == storeLength && _wcsnicmp(relative, kStores[i], storeLength) == 0) return TRUE;
        return FALSE;
    }
    return project && (_wcsicmp(rest, L".jsonl") == 0 || _wcsicmp(rest, L".precompact.json") == 0);
}

/* ------------------------------------------------------------ drawing math */

/* One frame of a smooth wheel scroll: how many px of the `pending` ones it
 * covers, `elapsedMs` after the last frame. What is left shrinks by a factor
 * of e every CORE_SCROLL_EASE_MS, whatever the frame rate (a late frame covers
 * more, so the pace holds), and by at least a pixel, so it ends; never past it. */
int Core_ScrollStep(int pending, int elapsedMs)
{
    double part;
    int px;
    if (pending == 0) return 0;
    part = pending * (1.0 - exp(-(double)max(elapsedMs, 1) / CORE_SCROLL_EASE_MS));
    px = (int)(part + (part >= 0 ? 0.5 : -0.5));
    if (px == 0) px = pending > 0 ? 1 : -1;
    return px;
}

/* FNV-1a, 64 bits: `hash` goes on with `size` more bytes (start from CORE_HASH_START). */
ULONGLONG Core_HashBytes(ULONGLONG hash, const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= p[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* Core_HashBytes going on with `text` and its terminator, so texts hashed one
 * after another never run together. Case counts. */
ULONGLONG Core_HashText(ULONGLONG hash, const WCHAR *text)
{
    return Core_HashBytes(hash, text, (wcslen(text) + 1) * sizeof(WCHAR));
}

/* A `width` x `height` rectangle centered on `on`, then moved inside `work`
 * (its top left corner inside when it is bigger). */
void Core_CenterRect(const RECT *on, int width, int height, const RECT *work, RECT *out)
{
    int x = on->left + ((on->right - on->left) - width) / 2, y = on->top + ((on->bottom - on->top) - height) / 2;
    if (x + width > work->right) x = work->right - width;
    if (y + height > work->bottom) y = work->bottom - height;
    if (x < work->left) x = work->left;
    if (y < work->top) y = work->top;
    out->left = x;
    out->top = y;
    out->right = x + width;
    out->bottom = y + height;
}

/* ---------------------------------------------------------------- versions */

/* "v1.2.3" or "1.2" as up to four numbers; FALSE when it does not start with
 * one or a number is above 65535. */
BOOL Core_ParseVersion(const WCHAR *text, DWORD parts[4])
{
    int n = 0;
    ZeroMemory(parts, 4 * sizeof(DWORD));
    if (!text) return FALSE;
    if (*text == L'v' || *text == L'V') text++;
    while (n < 4 && *text >= L'0' && *text <= L'9') {
        DWORD number = 0;
        while (*text >= L'0' && *text <= L'9') {
            number = number * 10 + (DWORD)(*text - L'0');
            if (number > 65535) return FALSE;
            text++;
        }
        parts[n++] = number;
        if (*text != L'.') break;
        text++;
    }
    return n > 0;
}

/* -1, 0 or 1 as version a is older than, the same as or newer than b. */
int Core_CompareVersions(const DWORD a[4], const DWORD b[4])
{
    int i;
    for (i = 0; i < 4; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* ---------------------------------------------------------------- deflate */

/* RFC 1951. Compression: one block of the fixed Huffman codes, LZ77 matches
 * found through hash chains over a 32 KB window; text (what a backup holds
 * most) shrinks to a third or less. Decompression reads every kind of block,
 * so an archive another tool made opens too. */

#define DEFLATE_WINDOW     32768
#define DEFLATE_HASH_BITS  15
#define DEFLATE_MIN_MATCH  3
#define DEFLATE_MAX_MATCH  258
#define DEFLATE_MAX_CHAIN  48     /* earlier places tried for a match: speed against size */
#define DEFLATE_GOOD_MATCH 64     /* a match this long ends the search */

static const WORD kLengthBase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                      131, 163, 195, 227, 258 };
static const BYTE kLengthExtra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const WORD kDistanceBase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                                        2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const BYTE kDistanceExtra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

typedef struct BitWriter {
    BYTE  *out;
    size_t capacity, used;
    DWORD  bits;
    int    count;
    BOOL   full;
} BitWriter;

static void PutBits(BitWriter *writer, DWORD value, int count)
{
    writer->bits |= value << writer->count;
    writer->count += count;
    while (writer->count >= 8) {
        if (writer->used < writer->capacity) writer->out[writer->used++] = (BYTE)writer->bits;
        else writer->full = TRUE;
        writer->bits >>= 8;
        writer->count -= 8;
    }
}

/* A Huffman code goes out from its first bit, the stream from its lowest. */
static void PutCode(BitWriter *writer, DWORD code, int length)
{
    DWORD reversed = 0;
    int i;
    for (i = 0; i < length; i++) reversed |= ((code >> i) & 1) << (length - 1 - i);
    PutBits(writer, reversed, length);
}

static void PutLiteral(BitWriter *writer, int symbol)
{
    if (symbol < 144) PutCode(writer, 0x30 + (DWORD)symbol, 8);
    else if (symbol < 256) PutCode(writer, 0x190 + (DWORD)(symbol - 144), 9);
    else if (symbol < 280) PutCode(writer, (DWORD)(symbol - 256), 7);
    else PutCode(writer, 0xC0 + (DWORD)(symbol - 280), 8);
}

static void PutMatch(BitWriter *writer, int length, int distance)
{
    int code = 28, d = 29;
    while (code > 0 && kLengthBase[code] > length) code--;
    PutLiteral(writer, 257 + code);
    if (kLengthExtra[code]) PutBits(writer, (DWORD)(length - kLengthBase[code]), kLengthExtra[code]);
    while (d > 0 && kDistanceBase[d] > distance) d--;
    PutCode(writer, (DWORD)d, 5);
    if (kDistanceExtra[d]) PutBits(writer, (DWORD)(distance - kDistanceBase[d]), kDistanceExtra[d]);
}

static DWORD HashAt(const BYTE *p)
{
    return ((DWORD)p[0] << 10 ^ (DWORD)p[1] << 5 ^ p[2]) & ((1u << DEFLATE_HASH_BITS) - 1);
}

size_t Core_DeflateBound(size_t size)
{
    /* At worst every byte a 9-bit literal, plus the block's header and end. */
    return size + size / 8 + 16;
}

BOOL Core_Deflate(const void *input, size_t size, void *output, size_t capacity, size_t *written)
{
    const BYTE *in = (const BYTE *)input;
    int *head = NULL, *chain = NULL;
    BitWriter writer;
    size_t i = 0;
    BOOL ok;
    *written = 0;
    head = (int *)HeapAlloc(GetProcessHeap(), 0, ((size_t)1 << DEFLATE_HASH_BITS) * sizeof(int));
    chain = (int *)HeapAlloc(GetProcessHeap(), 0, DEFLATE_WINDOW * sizeof(int));
    if (!head || !chain) {
        if (head) HeapFree(GetProcessHeap(), 0, head);
        if (chain) HeapFree(GetProcessHeap(), 0, chain);
        return FALSE;
    }
    for (i = 0; i < ((size_t)1 << DEFLATE_HASH_BITS); i++) head[i] = -1;
    ZeroMemory(&writer, sizeof writer);
    writer.out = (BYTE *)output;
    writer.capacity = capacity;
    PutBits(&writer, 1, 1);   /* the last block */
    PutBits(&writer, 1, 2);   /* fixed codes */
    i = 0;
    while (i < size && !writer.full) {
        int best = 0, bestDistance = 0;
        if (i + DEFLATE_MIN_MATCH <= size) {
            DWORD hash = HashAt(in + i);
            int candidate = head[hash], tries = DEFLATE_MAX_CHAIN;
            size_t limit = min(size - i, (size_t)DEFLATE_MAX_MATCH);
            while (candidate >= 0 && tries-- > 0 && i - (size_t)candidate <= DEFLATE_WINDOW) {
                const BYTE *a = in + candidate, *b = in + i;
                int length = 0;
                if (a[best] == b[best]) {   /* a longer match must agree there */
                    while ((size_t)length < limit && a[length] == b[length]) length++;
                    if (length > best) {
                        best = length;
                        bestDistance = (int)(i - (size_t)candidate);
                        if (best >= DEFLATE_GOOD_MATCH || (size_t)best == limit) break;
                    }
                }
                {
                    int next = chain[candidate % DEFLATE_WINDOW];
                    if (next >= candidate) break;   /* the slot was reused: the chain ends */
                    candidate = next;
                }
            }
        }
        if (best >= DEFLATE_MIN_MATCH) {
            size_t end = i + (size_t)best;
            PutMatch(&writer, best, bestDistance);
            for (; i < end; i++) {
                if (i + DEFLATE_MIN_MATCH <= size) {
                    DWORD hash = HashAt(in + i);
                    chain[i % DEFLATE_WINDOW] = head[hash];
                    head[hash] = (int)i;
                }
            }
        } else {
            if (i + DEFLATE_MIN_MATCH <= size) {
                DWORD hash = HashAt(in + i);
                chain[i % DEFLATE_WINDOW] = head[hash];
                head[hash] = (int)i;
            }
            PutLiteral(&writer, in[i]);
            i++;
        }
    }
    PutLiteral(&writer, 256);
    if (writer.count) PutBits(&writer, 0, 8 - writer.count);
    ok = !writer.full;
    if (ok) *written = writer.used;
    HeapFree(GetProcessHeap(), 0, head);
    HeapFree(GetProcessHeap(), 0, chain);
    return ok;
}

/* Decompression, after zlib's puff: canonical codes decoded a bit at a time. */
typedef struct BitReader {
    const BYTE *in;
    size_t      size, at;
    DWORD       bits;
    int         count;
    BYTE       *out;
    size_t      capacity, used;
    BOOL        broken;
} BitReader;

typedef struct Huffman {
    short count[16];    /* codes of each length */
    short symbol[320];  /* the symbols, by code */
} Huffman;

static int GetBits(BitReader *reader, int need)
{
    DWORD value = reader->bits;
    while (reader->count < need) {
        if (reader->at >= reader->size) {
            reader->broken = TRUE;
            return 0;
        }
        value |= (DWORD)reader->in[reader->at++] << reader->count;
        reader->count += 8;
    }
    reader->bits = value >> need;
    reader->count -= need;
    return (int)(value & ((1u << need) - 1));
}

static int Decode(BitReader *reader, const Huffman *huffman)
{
    int code = 0, first = 0, index = 0, length;
    for (length = 1; length < 16; length++) {
        int count;
        code |= GetBits(reader, 1);
        if (reader->broken) return -1;
        count = huffman->count[length];
        if (code - count < first) return huffman->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    reader->broken = TRUE;
    return -1;
}

/* A canonical code from each symbol's code length; FALSE for an over-full code. */
static BOOL BuildHuffman(Huffman *huffman, const short *lengths, int n)
{
    short offsets[16];
    int symbol, length, left = 1;
    ZeroMemory(huffman->count, sizeof huffman->count);
    for (symbol = 0; symbol < n; symbol++) huffman->count[lengths[symbol]]++;
    if (huffman->count[0] == n) return TRUE;
    for (length = 1; length < 16; length++) {
        left <<= 1;
        left -= huffman->count[length];
        if (left < 0) return FALSE;
    }
    offsets[1] = 0;
    for (length = 1; length < 15; length++) offsets[length + 1] = (short)(offsets[length] + huffman->count[length]);
    for (symbol = 0; symbol < n; symbol++)
        if (lengths[symbol]) huffman->symbol[offsets[lengths[symbol]]++] = (short)symbol;
    return TRUE;
}

static BOOL InflateCodes(BitReader *reader, const Huffman *lengths, const Huffman *distances)
{
    for (;;) {
        int symbol = Decode(reader, lengths);
        if (symbol < 0) return FALSE;
        if (symbol < 256) {
            if (reader->used >= reader->capacity) return FALSE;
            reader->out[reader->used++] = (BYTE)symbol;
        } else if (symbol == 256) {
            return TRUE;
        } else {
            int length, distance;
            symbol -= 257;
            if (symbol >= 29) return FALSE;
            length = kLengthBase[symbol] + GetBits(reader, kLengthExtra[symbol]);
            symbol = Decode(reader, distances);
            if (symbol < 0 || symbol >= 30) return FALSE;
            distance = kDistanceBase[symbol] + GetBits(reader, kDistanceExtra[symbol]);
            if (reader->broken || (size_t)distance > reader->used || reader->used + (size_t)length > reader->capacity) return FALSE;
            while (length--) {
                reader->out[reader->used] = reader->out[reader->used - (size_t)distance];
                reader->used++;
            }
        }
    }
}

static BOOL InflateStored(BitReader *reader)
{
    size_t length;
    reader->bits = 0;
    reader->count = 0;
    if (reader->at + 4 > reader->size) return FALSE;
    length = reader->in[reader->at] | (size_t)reader->in[reader->at + 1] << 8;
    if ((size_t)(reader->in[reader->at + 2] | reader->in[reader->at + 3] << 8) != (~length & 0xFFFF)) return FALSE;
    reader->at += 4;
    if (reader->at + length > reader->size || reader->used + length > reader->capacity) return FALSE;
    memcpy(reader->out + reader->used, reader->in + reader->at, length);
    reader->at += length;
    reader->used += length;
    return TRUE;
}

static BOOL InflateFixed(BitReader *reader)
{
    Huffman lengths, distances;
    short code[288];
    int symbol;
    for (symbol = 0; symbol < 144; symbol++) code[symbol] = 8;
    for (; symbol < 256; symbol++) code[symbol] = 9;
    for (; symbol < 280; symbol++) code[symbol] = 7;
    for (; symbol < 288; symbol++) code[symbol] = 8;
    BuildHuffman(&lengths, code, 288);
    for (symbol = 0; symbol < 30; symbol++) code[symbol] = 5;
    BuildHuffman(&distances, code, 30);
    return InflateCodes(reader, &lengths, &distances);
}

static BOOL InflateDynamic(BitReader *reader)
{
    static const BYTE kOrder[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    Huffman lengths, distances;
    short code[320];
    int literals = GetBits(reader, 5) + 257, distanceCount = GetBits(reader, 5) + 1, codes = GetBits(reader, 4) + 4, index;
    if (reader->broken || literals > 286 || distanceCount > 30) return FALSE;
    ZeroMemory(code, sizeof code);
    for (index = 0; index < codes; index++) code[kOrder[index]] = (short)GetBits(reader, 3);
    if (reader->broken || !BuildHuffman(&lengths, code, 19)) return FALSE;
    index = 0;
    while (index < literals + distanceCount) {
        int symbol = Decode(reader, &lengths), repeat, value = 0;
        if (symbol < 0) return FALSE;
        if (symbol < 16) {
            code[index++] = (short)symbol;
            continue;
        }
        if (symbol == 16) {
            if (index == 0) return FALSE;
            value = code[index - 1];
            repeat = 3 + GetBits(reader, 2);
        } else if (symbol == 17) {
            repeat = 3 + GetBits(reader, 3);
        } else {
            repeat = 11 + GetBits(reader, 7);
        }
        if (reader->broken || index + repeat > literals + distanceCount) return FALSE;
        while (repeat--) code[index++] = (short)value;
    }
    if (code[256] == 0) return FALSE;
    if (!BuildHuffman(&lengths, code, literals) || !BuildHuffman(&distances, code + literals, distanceCount)) return FALSE;
    return InflateCodes(reader, &lengths, &distances);
}

BOOL Core_Inflate(const void *input, size_t size, void *output, size_t capacity, size_t *written)
{
    BitReader reader;
    int last;
    *written = 0;
    ZeroMemory(&reader, sizeof reader);
    reader.in = (const BYTE *)input;
    reader.size = size;
    reader.out = (BYTE *)output;
    reader.capacity = capacity;
    do {
        int type;
        BOOL ok;
        last = GetBits(&reader, 1);
        type = GetBits(&reader, 2);
        if (reader.broken) return FALSE;
        if (type == 0) ok = InflateStored(&reader);
        else if (type == 1) ok = InflateFixed(&reader);
        else if (type == 2) ok = InflateDynamic(&reader);
        else ok = FALSE;
        if (!ok || reader.broken) return FALSE;
    } while (!last);
    *written = reader.used;
    return TRUE;
}

int Core_ProcessDescendants(const CoreProcess *processes, int count, int root, BOOL *chosen)
{
    int i, j, marked = 0;
    BOOL grew = TRUE;
    for (i = 0; i < count; i++) chosen[i] = FALSE;
    if (root < 0 || root >= count) return 0;
    /* Each pass takes the children of the processes taken so far: as many passes as the tree is deep. */
    while (grew) {
        grew = FALSE;
        for (i = 0; i < count; i++) {
            if (i == root || chosen[i] || !processes[i].started || processes[i].pid == processes[i].parent) continue;
            for (j = 0; j < count; j++) {
                if ((j == root || chosen[j]) && processes[j].pid == processes[i].parent && processes[j].started &&
                    processes[i].started >= processes[j].started) {
                    chosen[i] = grew = TRUE;
                    marked++;
                    break;
                }
            }
        }
    }
    return marked;
}

/* ---------------------------------------------------------------- LevelDB */

/* What Chromium keeps a window's web storage in (Claude's Local Storage):
 * the formats of LevelDB 1.x as its doc/ describes them. A log holds write
 * batches in 32 KiB blocks, each record behind a header (a masked CRC32C of
 * its type and data, its length, its type); a table holds sorted entries in
 * blocks (Snappy-compressed or not), found through an index block its footer
 * points to; the manifest is a log of version edits (which tables and log
 * are live, the last sequence number). Only what reading every live entry,
 * and adding one write batch to the log, need. */

#define LEVEL_BLOCK         32768u   /* a log's block */
#define LEVEL_HEADER        7u       /* a log record's header: checksum, length, type */
#define LEVEL_FOOTER        48u      /* a table's footer */
#define LEVEL_MAGIC         0xdb4775248b80fb57ULL
#define LEVEL_MASK_DELTA    0xa282ead8u

enum { LEVEL_FULL = 1, LEVEL_FIRST = 2, LEVEL_MIDDLE = 3, LEVEL_LAST = 4 };

static DWORD g_crc32cTable[256];

DWORD Core_Crc32c(DWORD crc, const void *data, size_t size)
{
    const BYTE *bytes = (const BYTE *)data;
    size_t i;
    if (!g_crc32cTable[1]) {
        DWORD n, k, c;
        for (n = 0; n < 256; n++) {
            for (c = n, k = 0; k < 8; k++) c = (c & 1) ? 0x82f63b78u ^ (c >> 1) : c >> 1;
            g_crc32cTable[n] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < size; i++) crc = g_crc32cTable[(crc ^ bytes[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* LevelDB stores a CRC that covers data holding CRCs rotated and offset. */
DWORD Core_LevelMask(DWORD crc)
{
    return ((crc >> 15) | (crc << 17)) + LEVEL_MASK_DELTA;
}

/* A varint at `*at`, before `end`; FALSE when it runs past it or past 64 bits. */
static BOOL LevelVarint(const BYTE *data, size_t end, size_t *at, ULONGLONG *value)
{
    int shift;
    *value = 0;
    for (shift = 0; shift < 64 && *at < end; shift += 7) {
        BYTE b = data[(*at)++];
        *value |= (ULONGLONG)(b & 0x7F) << shift;
        if (!(b & 0x80)) return TRUE;
    }
    return FALSE;
}

static size_t LevelPutVarint(BYTE *out, ULONGLONG value)
{
    size_t n = 0;
    while (value >= 0x80) {
        out[n++] = (BYTE)(value | 0x80);
        value >>= 7;
    }
    out[n++] = (BYTE)value;
    return n;
}

static ULONGLONG LevelFixed64(const BYTE *p)
{
    ULONGLONG value = 0;
    int i;
    for (i = 7; i >= 0; i--) value = (value << 8) | p[i];
    return value;
}

static DWORD LevelFixed32(const BYTE *p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) | ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

BOOL Core_SnappyLength(const BYTE *in, size_t length, size_t *outLength)
{
    size_t at = 0;
    ULONGLONG value;
    if (!LevelVarint(in, length, &at, &value) || value > (ULONGLONG)SIZE_MAX / 2) return FALSE;
    *outLength = (size_t)value;
    return TRUE;
}

/* Snappy's raw format: the length, then literals and copies of what came
 * before. FALSE when `in` is not that, or does not fill `out` exactly. */
BOOL Core_SnappyDecode(const BYTE *in, size_t length, BYTE *out, size_t outLength)
{
    size_t at = 0, written = 0, n, offset, i;
    ULONGLONG total;
    if (!LevelVarint(in, length, &at, &total) || total != outLength) return FALSE;
    while (at < length) {
        BYTE tag = in[at++];
        switch (tag & 3) {
        case 0:
            n = tag >> 2;
            if (n >= 60) {
                size_t bytes = n - 59, k;
                if (at + bytes > length) return FALSE;
                for (n = 0, k = 0; k < bytes; k++) n |= (size_t)in[at + k] << (8 * k);
                at += bytes;
            }
            n += 1;
            if (n > length - at || n > outLength - written) return FALSE;
            memcpy(out + written, in + at, n);
            at += n;
            written += n;
            continue;
        case 1:
            if (at >= length) return FALSE;
            n = ((tag >> 2) & 7) + 4;
            offset = ((size_t)(tag >> 5) << 8) | in[at++];
            break;
        case 2:
            if (at + 2 > length) return FALSE;
            n = (size_t)(tag >> 2) + 1;
            offset = (size_t)in[at] | ((size_t)in[at + 1] << 8);
            at += 2;
            break;
        default:
            if (at + 4 > length) return FALSE;
            n = (size_t)(tag >> 2) + 1;
            offset = LevelFixed32(in + at);
            at += 4;
            break;
        }
        if (offset == 0 || offset > written || n > outLength - written) return FALSE;
        for (i = 0; i < n; i++, written++) out[written] = out[written - offset];
    }
    return written == outLength;
}

BOOL Core_LevelLogRecords(const BYTE *log, size_t length, CoreLevelRecord each, void *context, size_t *cleanEnd)
{
    BYTE *whole = NULL;
    size_t at = 0, used = 0, capacity = 0;
    BOOL inRecord = FALSE, ok = TRUE;
    *cleanEnd = 0;
    while (at + LEVEL_HEADER <= length) {
        size_t left = LEVEL_BLOCK - at % LEVEL_BLOCK, size;
        DWORD stored;
        BYTE type;
        if (left < LEVEL_HEADER) {   /* a block's trailer */
            at += left;
            continue;
        }
        stored = LevelFixed32(log + at);
        size = (size_t)log[at + 4] | ((size_t)log[at + 5] << 8);
        type = log[at + 6];
        if (type == 0 && size == 0) {   /* zeros to the end of the block */
            at += left;
            continue;
        }
        if (size > left - LEVEL_HEADER || at + LEVEL_HEADER + size > length ||
            Core_LevelMask(Core_Crc32c(0, log + at + 6, 1 + size)) != stored)
            break;   /* a record cut short, or garbage: what follows is not read */
        if (type == LEVEL_FULL) {
            if (!each(context, log + at + LEVEL_HEADER, size)) ok = FALSE;
            inRecord = FALSE;
        } else if (type == LEVEL_FIRST || ((type == LEVEL_MIDDLE || type == LEVEL_LAST) && inRecord)) {
            if (type == LEVEL_FIRST) used = 0;
            if (used + size > capacity) {
                size_t grown = max(capacity * 2, used + size + 4096);
                BYTE *bigger = whole ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, whole, grown) : (BYTE *)HeapAlloc(GetProcessHeap(), 0, grown);
                if (!bigger) {
                    ok = FALSE;
                    break;
                }
                whole = bigger;
                capacity = grown;
            }
            memcpy(whole + used, log + at + LEVEL_HEADER, size);
            used += size;
            inRecord = type != LEVEL_LAST;
            if (type == LEVEL_LAST && !each(context, whole, used)) ok = FALSE;
        }
        at += LEVEL_HEADER + size;
        if (!inRecord) *cleanEnd = at;
    }
    if (whole) HeapFree(GetProcessHeap(), 0, whole);
    return ok;
}

size_t Core_LevelLogAppend(size_t fileLength, const BYTE *record, size_t length, BYTE *out, size_t capacity)
{
    size_t written = 0, offset = fileLength % LEVEL_BLOCK, done = 0;
    BOOL first = TRUE;
    do {
        size_t left = LEVEL_BLOCK - offset, size;
        DWORD crc;
        BYTE type;
        if (left < LEVEL_HEADER) {
            if (written + left > capacity) return 0;
            memset(out + written, 0, left);
            written += left;
            offset = 0;
            continue;
        }
        size = min(length - done, left - LEVEL_HEADER);
        type = (BYTE)(first && done + size == length ? LEVEL_FULL : first ? LEVEL_FIRST : done + size == length ? LEVEL_LAST : LEVEL_MIDDLE);
        if (written + LEVEL_HEADER + size > capacity) return 0;
        out[written + 4] = (BYTE)size;
        out[written + 5] = (BYTE)(size >> 8);
        out[written + 6] = type;
        memcpy(out + written + LEVEL_HEADER, record + done, size);
        crc = Core_LevelMask(Core_Crc32c(0, out + written + 6, 1 + size));
        out[written] = (BYTE)crc;
        out[written + 1] = (BYTE)(crc >> 8);
        out[written + 2] = (BYTE)(crc >> 16);
        out[written + 3] = (BYTE)(crc >> 24);
        written += LEVEL_HEADER + size;
        offset = (offset + LEVEL_HEADER + size) % LEVEL_BLOCK;
        done += size;
        first = FALSE;
    } while (done < length);
    return written;
}

BOOL Core_LevelBatchRead(const BYTE *batch, size_t length, CoreLevelEntry each, void *context)
{
    ULONGLONG sequence, count, i, keyLength, valueLength;
    size_t at = 12;
    if (length < 12) return FALSE;
    sequence = LevelFixed64(batch);
    count = LevelFixed32(batch + 8);
    for (i = 0; i < count; i++) {
        CoreLevelOp op;
        BYTE type;
        if (at >= length) return FALSE;
        type = batch[at++];
        if ((type != 0 && type != 1) || !LevelVarint(batch, length, &at, &keyLength) || keyLength > length - at) return FALSE;
        ZeroMemory(&op, sizeof op);
        op.put = type == 1;
        op.key = batch + at;
        op.keyLength = (size_t)keyLength;
        at += (size_t)keyLength;
        if (op.put) {
            if (!LevelVarint(batch, length, &at, &valueLength) || valueLength > length - at) return FALSE;
            op.value = batch + at;
            op.valueLength = (size_t)valueLength;
            at += (size_t)valueLength;
        }
        if (!each(context, sequence + i, &op)) return FALSE;
    }
    return at == length;
}

size_t Core_LevelBatchWrite(ULONGLONG sequence, const CoreLevelOp *ops, int count, BYTE *out, size_t capacity)
{
    size_t at = 12;
    int i, b;
    if (capacity < 12) return 0;
    for (b = 0; b < 8; b++) out[b] = (BYTE)(sequence >> (8 * b));
    for (b = 0; b < 4; b++) out[8 + b] = (BYTE)((DWORD)count >> (8 * b));
    for (i = 0; i < count; i++) {
        size_t need = 1 + 10 + ops[i].keyLength + (ops[i].put ? 10 + ops[i].valueLength : 0);
        if (need > capacity - at) return 0;
        out[at++] = (BYTE)(ops[i].put ? 1 : 0);
        at += LevelPutVarint(out + at, ops[i].keyLength);
        memcpy(out + at, ops[i].key, ops[i].keyLength);
        at += ops[i].keyLength;
        if (ops[i].put) {
            at += LevelPutVarint(out + at, ops[i].valueLength);
            memcpy(out + at, ops[i].value, ops[i].valueLength);
            at += ops[i].valueLength;
        }
    }
    return at;
}

/* A block of a table (`handle`: its offset and size), as it is or Snappy-
 * decompressed (a heap block, freed by the caller); NULL when it cannot be. */
static BYTE *LevelTableBlock(const BYTE *table, size_t length, ULONGLONG offset, ULONGLONG size, size_t *blockLength)
{
    BYTE *block = NULL;
    size_t inflated;
    if (offset > length || size > length - offset || length - offset - size < 5) return NULL;
    if (table[offset + size] == 0) {
        if ((block = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)max(size, 1))) != NULL) memcpy(block, table + offset, (size_t)size);
        *blockLength = (size_t)size;
        return block;
    }
    if (table[offset + size] != 1 || !Core_SnappyLength(table + offset, (size_t)size, &inflated) || inflated > (64u << 20) ||
        (block = (BYTE *)HeapAlloc(GetProcessHeap(), 0, max(inflated, 1))) == NULL)
        return NULL;
    if (!Core_SnappyDecode(table + offset, (size_t)size, block, inflated)) {
        HeapFree(GetProcessHeap(), 0, block);
        return NULL;
    }
    *blockLength = inflated;
    return block;
}

typedef BOOL (*LevelBlockEntry)(void *context, const BYTE *key, size_t keyLength, const BYTE *value, size_t valueLength);

/* Each entry of a table's block: keys share a prefix with the one before. */
static BOOL LevelBlockEntries(const BYTE *block, size_t length, LevelBlockEntry each, void *context)
{
    BYTE *key = NULL;
    size_t end, at = 0, keyLength = 0, capacity = 0;
    DWORD restarts;
    BOOL ok = TRUE;
    if (length < 4) return FALSE;
    restarts = LevelFixed32(block + length - 4);
    if (restarts > (length - 4) / 4) return FALSE;
    end = length - 4 - 4 * (size_t)restarts;
    while (ok && at < end) {
        ULONGLONG shared, unshared, valueLength;
        if (!LevelVarint(block, end, &at, &shared) || !LevelVarint(block, end, &at, &unshared) || !LevelVarint(block, end, &at, &valueLength) ||
            shared > keyLength || unshared > end - at || valueLength > end - at - unshared) {
            ok = FALSE;
            break;
        }
        if ((size_t)(shared + unshared) > capacity) {
            size_t grown = (size_t)(shared + unshared) + 256;
            BYTE *bigger = key ? (BYTE *)HeapReAlloc(GetProcessHeap(), 0, key, grown) : (BYTE *)HeapAlloc(GetProcessHeap(), 0, grown);
            if (!bigger) {
                ok = FALSE;
                break;
            }
            key = bigger;
            capacity = grown;
        }
        memcpy(key + shared, block + at, (size_t)unshared);
        keyLength = (size_t)(shared + unshared);
        at += (size_t)unshared;
        ok = each(context, key, keyLength, block + at, (size_t)valueLength);
        at += (size_t)valueLength;
    }
    if (key) HeapFree(GetProcessHeap(), 0, key);
    return ok;
}

typedef struct LevelTableRead {
    const BYTE    *table;
    size_t         length;
    CoreLevelEntry each;
    void          *context;
} LevelTableRead;

/* A data block's entry: its internal key is the user key, then the sequence
 * and the type (0: deleted, 1: a value) in eight bytes. */
static BOOL LevelDataEntry(void *context, const BYTE *key, size_t keyLength, const BYTE *value, size_t valueLength)
{
    LevelTableRead *read = (LevelTableRead *)context;
    CoreLevelOp op;
    ULONGLONG trailer;
    if (keyLength < 8) return FALSE;
    trailer = LevelFixed64(key + keyLength - 8);
    ZeroMemory(&op, sizeof op);
    op.put = (trailer & 0xFF) == 1;
    op.key = key;
    op.keyLength = keyLength - 8;
    op.value = op.put ? value : NULL;
    op.valueLength = op.put ? valueLength : 0;
    return read->each(read->context, trailer >> 8, &op);
}

/* An index block's entry: the data block it points to. */
static BOOL LevelIndexEntry(void *context, const BYTE *key, size_t keyLength, const BYTE *value, size_t valueLength)
{
    LevelTableRead *read = (LevelTableRead *)context;
    ULONGLONG offset, size;
    size_t at = 0, blockLength = 0;
    BYTE *block;
    BOOL ok;
    (void)key;
    (void)keyLength;
    if (!LevelVarint(value, valueLength, &at, &offset) || !LevelVarint(value, valueLength, &at, &size) ||
        (block = LevelTableBlock(read->table, read->length, offset, size, &blockLength)) == NULL)
        return FALSE;
    ok = LevelBlockEntries(block, blockLength, LevelDataEntry, read);
    HeapFree(GetProcessHeap(), 0, block);
    return ok;
}

BOOL Core_LevelTableRead(const BYTE *table, size_t length, CoreLevelEntry each, void *context)
{
    LevelTableRead read;
    const BYTE *footer;
    ULONGLONG ignored, offset, size;
    size_t at = 0, indexLength = 0;
    BYTE *index;
    BOOL ok;
    if (length < LEVEL_FOOTER) return FALSE;
    footer = table + length - LEVEL_FOOTER;
    if (LevelFixed64(footer + LEVEL_FOOTER - 8) != LEVEL_MAGIC || !LevelVarint(footer, LEVEL_FOOTER - 8, &at, &ignored) ||
        !LevelVarint(footer, LEVEL_FOOTER - 8, &at, &ignored) || !LevelVarint(footer, LEVEL_FOOTER - 8, &at, &offset) ||
        !LevelVarint(footer, LEVEL_FOOTER - 8, &at, &size) || (index = LevelTableBlock(table, length, offset, size, &indexLength)) == NULL)
        return FALSE;
    read.table = table;
    read.length = length;
    read.each = each;
    read.context = context;
    ok = LevelBlockEntries(index, indexLength, LevelIndexEntry, &read);
    HeapFree(GetProcessHeap(), 0, index);
    return ok;
}

BOOL Core_LevelManifestEdit(const BYTE *edit, size_t length, CoreLevelManifest *state)
{
    size_t at = 0;
    while (at < length) {
        ULONGLONG tag, a, b, size;
        if (!LevelVarint(edit, length, &at, &tag)) return FALSE;
        switch (tag) {
        case 1:   /* the comparator's name */
            if (!LevelVarint(edit, length, &at, &size) || size > length - at) return FALSE;
            at += (size_t)size;
            break;
        case 2:
            if (!LevelVarint(edit, length, &at, &state->logNumber)) return FALSE;
            state->hasLog = TRUE;
            break;
        case 3:
            if (!LevelVarint(edit, length, &at, &state->nextFile)) return FALSE;
            break;
        case 4:
            if (!LevelVarint(edit, length, &at, &a)) return FALSE;
            state->lastSequence = max(state->lastSequence, a);
            break;
        case 5:   /* a compaction's place: a level, an internal key */
            if (!LevelVarint(edit, length, &at, &a) || !LevelVarint(edit, length, &at, &size) || size > length - at) return FALSE;
            at += (size_t)size;
            break;
        case 6:   /* a table gone: its level and number */
            if (!LevelVarint(edit, length, &at, &a) || !LevelVarint(edit, length, &at, &b)) return FALSE;
            if (state->onTable) state->onTable(state->context, b, FALSE);
            break;
        case 7: { /* a new table: level, number, size, smallest and largest keys */
            ULONGLONG number;
            if (!LevelVarint(edit, length, &at, &a) || !LevelVarint(edit, length, &at, &number) || !LevelVarint(edit, length, &at, &b))
                return FALSE;
            if (!LevelVarint(edit, length, &at, &size) || size > length - at) return FALSE;
            at += (size_t)size;
            if (!LevelVarint(edit, length, &at, &size) || size > length - at) return FALSE;
            at += (size_t)size;
            if (state->onTable) state->onTable(state->context, number, TRUE);
            break;
        }
        case 9:
            if (!LevelVarint(edit, length, &at, &state->prevLogNumber)) return FALSE;
            break;
        default:
            return FALSE;
        }
    }
    return TRUE;
}

/* Chromium's Local Storage, in that database: a key of origin `origin` is
 * "_<origin>", a zero, then the script's key: 1 and its Latin-1 bytes, or 0
 * and its UTF-16LE; a value is 1 and Latin-1, or 0 and UTF-16LE, Latin-1
 * when every character fits. */
static size_t WebStorageString(const WCHAR *text, size_t length, BYTE *out, size_t capacity)
{
    size_t i, at = 1;
    BOOL latin = TRUE;
    for (i = 0; i < length; i++)
        if (text[i] > 0xFF) latin = FALSE;
    if (capacity < 1 + length * (latin ? 1 : 2)) return 0;
    out[0] = (BYTE)(latin ? 1 : 0);
    for (i = 0; i < length; i++) {
        out[at++] = (BYTE)text[i];
        if (!latin) out[at++] = (BYTE)(text[i] >> 8);
    }
    return at;
}

size_t Core_WebStorageKey(const char *origin, const WCHAR *name, BYTE *out, size_t capacity)
{
    size_t prefix = strlen(origin), written;
    if (capacity < prefix + 2) return 0;
    out[0] = '_';
    memcpy(out + 1, origin, prefix);
    out[prefix + 1] = 0;
    written = WebStorageString(name, wcslen(name), out + prefix + 2, capacity - prefix - 2);
    return written ? prefix + 2 + written : 0;
}

size_t Core_WebStorageValue(const WCHAR *text, size_t length, BYTE *out, size_t capacity)
{
    return WebStorageString(text, length, out, capacity);
}

BOOL Core_WebStorageText(const BYTE *value, size_t length, WCHAR *out, size_t cch, size_t *outLength)
{
    size_t i, n;
    if (length < 1 || (value[0] != 0 && value[0] != 1)) return FALSE;
    n = value[0] == 1 ? length - 1 : (length - 1) / 2;
    if (value[0] == 0 && (length - 1) % 2) return FALSE;
    if (n + 1 > cch) return FALSE;
    for (i = 0; i < n; i++) out[i] = value[0] == 1 ? (WCHAR)value[1 + i] : (WCHAR)(value[1 + 2 * i] | (value[2 + 2 * i] << 8));
    out[n] = 0;
    *outLength = n;
    return TRUE;
}

/* ------------------------------------------------------- three-way merge */

/* Merging the values several profiles hold of one part against the value
 * they shared at the last sync (Core_JsonMerge). An element changed by one
 * goes to all; elements changed by several each their own way are merged
 * deeper where they are objects or lists (by key, "id" or a first string),
 * and lists of plain values as sets; what is left is a conflict at its
 * path. */
typedef struct MergeOut {
    char  *data;
    size_t len, cap;
    BOOL   failed;
} MergeOut;

typedef struct MergeJob {
    int                n;
    const BOOL        *say;
    const ULONGLONG   *written;
    CoreMergeDecide    decide;
    CoreMergeReport    report;
    void              *context;
    int                unresolved;
    MergeOut           out;
} MergeJob;

typedef struct MergeNode {
    const char *base;                        /* NULL: absent */
    size_t      baseLen;
    const char *value[MAX_PROFILES];         /* NULL: absent */
    size_t      valueLen[MAX_PROFILES];
    BOOL        changed[MAX_PROFILES];       /* the member's view counts and differs from the base */
} MergeNode;

static void MergePut(MergeOut *out, const char *text, size_t len)
{
    if (out->failed) return;
    if (out->len + len + 1 > out->cap) {
        size_t cap = out->cap ? out->cap : 256;
        char *grown;
        while (cap < out->len + len + 1) cap *= 2;
        grown = out->data ? (char *)HeapReAlloc(GetProcessHeap(), 0, out->data, cap) : (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        if (!grown) {
            out->failed = TRUE;
            return;
        }
        out->data = grown;
        out->cap = cap;
    }
    memcpy(out->data + out->len, text, len);
    out->len += len;
    out->data[out->len] = 0;
}

/* Two JSON texts the same but for spaces outside their strings. */
static BOOL SameJson(const char *a, size_t aLen, const char *b, size_t bLen)
{
    size_t i = 0, j = 0;
    BOOL inString = FALSE;
    if (!a || !b) return a == b;
    for (;;) {
        if (!inString) {
            while (i < aLen && IsJsonSpace(a[i])) i++;
            while (j < bLen && IsJsonSpace(b[j])) j++;
        }
        if (i >= aLen || j >= bLen) return i >= aLen && j >= bLen;
        if (a[i] != b[j]) return FALSE;
        if (a[i] == '\\' && inString) {
            if (i + 1 >= aLen || j + 1 >= bLen || a[i + 1] != b[j + 1]) return FALSE;
            i += 2;
            j += 2;
            continue;
        }
        if (a[i] == '"') inString = !inString;
        i++;
        j++;
    }
}

/* The next member of an object from `*i` (just after '{' or a comma): its
 * key's raw text and its value's. FALSE at the object's end. */
static BOOL MergeNextMember(const char *s, size_t len, size_t *i, const char **key, size_t *keyLen, const char **value, size_t *valueLen)
{
    size_t at = SkipSpace(s, len, *i), keyEnd, end;
    if (at < len && s[at] == ',') at = SkipSpace(s, len, at + 1);
    if (at >= len || s[at] != '"' || (keyEnd = StringEnd(s, len, at)) == 0) return FALSE;
    *key = s + at + 1;
    *keyLen = keyEnd - at - 2;
    at = SkipSpace(s, len, keyEnd);
    if (at >= len || s[at] != ':') return FALSE;
    at = SkipSpace(s, len, at + 1);
    if ((end = ValueEnd(s, len, at)) == 0 || end == at) return FALSE;
    *value = s + at;
    *valueLen = end - at;
    *i = end;
    return TRUE;
}

/* The next item of an array from `*i` (just after '[' or a comma). */
static BOOL MergeNextItem(const char *s, size_t len, size_t *i, const char **value, size_t *valueLen)
{
    size_t at = SkipSpace(s, len, *i), end;
    if (at < len && s[at] == ',') at = SkipSpace(s, len, at + 1);
    if (at >= len || s[at] == ']' || (end = ValueEnd(s, len, at)) == 0 || end == at) return FALSE;
    *value = s + at;
    *valueLen = end - at;
    *i = end;
    return TRUE;
}

static char MergeKind(const char *s, size_t len)
{
    size_t at = SkipSpace(s, len, 0);
    return at < len ? s[at] : 0;
}

static size_t MergeOpen(const char *s, size_t len)
{
    return SkipSpace(s, len, 0) + 1;   /* just after '{' or '[' */
}

/* How a list's items are told apart: by their "id", by their first string,
 * or as plain values (a set). */
typedef enum MergeScheme { SCHEME_NONE, SCHEME_ID, SCHEME_FIRST, SCHEME_PLAIN, SCHEME_MIXED } MergeScheme;

/* The key of item `item` in `scheme`: the raw text inside the quotes of its id or first string. */
static BOOL MergeItemKey(const char *item, size_t len, MergeScheme scheme, const char **key, size_t *keyLen)
{
    const char *value;
    size_t valueLen, i;
    if (scheme == SCHEME_ID) {
        if (MergeKind(item, len) != '{') return FALSE;
        i = MergeOpen(item, len);
        while (MergeNextMember(item, len, &i, key, keyLen, &value, &valueLen))
            if (*keyLen == 2 && memcmp(*key, "id", 2) == 0) {
                if (MergeKind(value, valueLen) != '"' || valueLen < 2) return FALSE;
                *key = value + 1;
                *keyLen = valueLen - 2;
                return TRUE;
            }
        return FALSE;
    }
    if (scheme == SCHEME_FIRST) {
        if (MergeKind(item, len) != '[') return FALSE;
        i = MergeOpen(item, len);
        if (!MergeNextItem(item, len, &i, &value, &valueLen) || MergeKind(value, valueLen) != '"' || valueLen < 2) return FALSE;
        *key = value + 1;
        *keyLen = valueLen - 2;
        return TRUE;
    }
    *key = item;
    *keyLen = len;
    return TRUE;
}

static MergeScheme MergeListScheme(const char *s, size_t len)
{
    MergeScheme scheme = SCHEME_NONE, one;
    const char *item, *key;
    size_t itemLen, keyLen, i = MergeOpen(s, len);
    while (MergeNextItem(s, len, &i, &item, &itemLen)) {
        char kind = MergeKind(item, itemLen);
        if (kind == '{') one = MergeItemKey(item, itemLen, SCHEME_ID, &key, &keyLen) ? SCHEME_ID : SCHEME_MIXED;
        else if (kind == '[') one = MergeItemKey(item, itemLen, SCHEME_FIRST, &key, &keyLen) ? SCHEME_FIRST : SCHEME_MIXED;
        else one = SCHEME_PLAIN;
        if (scheme == SCHEME_NONE) scheme = one;
        else if (scheme != one) return SCHEME_MIXED;
        if (scheme == SCHEME_MIXED) return SCHEME_MIXED;
    }
    return scheme;
}

/* The element of key `key` in the object or list `s` (`scheme`: how the
 * list's items are told apart; SCHEME_NONE for an object). */
static BOOL MergeFind(const char *s, size_t len, MergeScheme scheme, const char *key, size_t keyLen, const char **value, size_t *valueLen)
{
    const char *k;
    size_t kLen, i;
    if (!s) return FALSE;
    i = MergeOpen(s, len);
    if (scheme == SCHEME_NONE) {
        while (MergeNextMember(s, len, &i, &k, &kLen, value, valueLen))
            if (kLen == keyLen && memcmp(k, key, keyLen) == 0) return TRUE;
        return FALSE;
    }
    while (MergeNextItem(s, len, &i, value, valueLen))
        if (MergeItemKey(*value, *valueLen, scheme, &k, &kLen) &&
            (scheme == SCHEME_PLAIN ? SameJson(k, kLen, key, keyLen) : kLen == keyLen && memcmp(k, key, keyLen) == 0))
            return TRUE;
    return FALSE;
}

/* The keys of the elements, each once, in the order of `order` first (the
 * member that changed last), then of the base and the others. */
typedef struct MergeKeys {
    const char **key;
    size_t      *keyLen;
    int          count, capacity;
} MergeKeys;

static void MergeAddKeys(MergeKeys *keys, const char *s, size_t len, MergeScheme scheme)
{
    const char *key, *value;
    size_t keyLen, valueLen, i;
    int k;
    if (!s) return;
    i = MergeOpen(s, len);
    for (;;) {
        if (scheme == SCHEME_NONE) {
            if (!MergeNextMember(s, len, &i, &key, &keyLen, &value, &valueLen)) break;
        } else {
            if (!MergeNextItem(s, len, &i, &value, &valueLen)) break;
            if (!MergeItemKey(value, valueLen, scheme, &key, &keyLen)) continue;
        }
        for (k = 0; k < keys->count; k++)
            if (scheme == SCHEME_PLAIN ? SameJson(keys->key[k], keys->keyLen[k], key, keyLen)
                                       : keys->keyLen[k] == keyLen && memcmp(keys->key[k], key, keyLen) == 0)
                break;
        if (k < keys->count) continue;
        if (keys->count == keys->capacity) {
            int capacity = keys->capacity ? keys->capacity * 2 : 32;
            const char **grownKey = (const char **)(keys->key ? HeapReAlloc(GetProcessHeap(), 0, (void *)keys->key, (size_t)capacity * sizeof *keys->key)
                                                              : HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof *keys->key));
            size_t *grownLen;
            if (!grownKey) return;
            keys->key = grownKey;
            grownLen = (size_t *)(keys->keyLen ? HeapReAlloc(GetProcessHeap(), 0, keys->keyLen, (size_t)capacity * sizeof *keys->keyLen)
                                               : HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof *keys->keyLen));
            if (!grownLen) return;
            keys->keyLen = grownLen;
            keys->capacity = capacity;
        }
        keys->key[keys->count] = key;
        keys->keyLen[keys->count++] = keyLen;
    }
}

static void MergeFreeKeys(MergeKeys *keys)
{
    if (keys->key) HeapFree(GetProcessHeap(), 0, (void *)keys->key);
    if (keys->keyLen) HeapFree(GetProcessHeap(), 0, keys->keyLen);
}

/* `path` with one more step: "/" and `key`, '~' and '/' written "~0" and "~1"; "#" before a list item's key. */
static BOOL MergeStep(const char *path, const char *key, size_t keyLen, BOOL item, char *out, size_t cap)
{
    size_t n = strlen(path), i;
    if (n + 3 > cap) return FALSE;
    memcpy(out, path, n);
    out[n++] = '/';
    if (item) out[n++] = '#';
    for (i = 0; i < keyLen; i++) {
        if (n + 3 > cap) return FALSE;
        if (key[i] == '~' || key[i] == '/') {
            out[n++] = '~';
            out[n++] = key[i] == '~' ? '0' : '1';
        } else {
            out[n++] = key[i];
        }
    }
    out[n] = 0;
    return TRUE;
}

/* The member of `changed` that changed last: the order and the default come from it. */
static int MergeLatest(const MergeJob *job, const BOOL *changed)
{
    int m, latest = -1;
    for (m = 0; m < job->n; m++)
        if (changed[m] && (latest < 0 || job->written[m] > job->written[latest])) latest = m;
    return latest;
}

static BOOL MergeValue(MergeJob *job, const MergeNode *node, const char *path, int depth);

/* An object or a keyed list merged element by element; a set of plain values by membership. */
static void MergeElements(MergeJob *job, const MergeNode *node, const char *path, int depth, MergeScheme scheme, BOOL list)
{
    MergeKeys keys;
    int latest = MergeLatest(job, node->changed), m, k, written = 0;
    char step[CORE_MERGE_PATH_CCH];
    ZeroMemory(&keys, sizeof keys);
    if (latest >= 0) MergeAddKeys(&keys, node->value[latest], node->valueLen[latest], scheme);
    MergeAddKeys(&keys, node->base, node->baseLen, scheme);
    for (m = 0; m < job->n; m++)
        if (node->changed[m]) MergeAddKeys(&keys, node->value[m], node->valueLen[m], scheme);
    MergePut(&job->out, list ? "[" : "{", 1);
    for (k = 0; k < keys.count; k++) {
        MergeNode child;
        BOOL inBase, keep = FALSE;
        size_t before = job->out.len;
        ZeroMemory(&child, sizeof child);
        inBase = MergeFind(node->base, node->baseLen, scheme, keys.key[k], keys.keyLen[k], &child.base, &child.baseLen);
        if (!inBase) child.base = NULL;
        for (m = 0; m < job->n; m++) {
            if (!node->changed[m]) continue;
            if (!MergeFind(node->value[m], node->valueLen[m], scheme, keys.key[k], keys.keyLen[k], &child.value[m], &child.valueLen[m]))
                child.value[m] = NULL;
            child.changed[m] = !SameJson(child.value[m], child.valueLen[m], child.base, child.baseLen);
        }
        if (scheme == SCHEME_PLAIN) {
            /* A plain value: in the result when no member took it out, or one put it in. */
            BOOL added = FALSE, removed = FALSE;
            for (m = 0; m < job->n; m++) {
                if (!child.changed[m]) continue;
                if (child.value[m]) added = TRUE;
                else removed = TRUE;
            }
            keep = inBase ? !removed : added;
            if (keep) {
                if (written++) MergePut(&job->out, ",", 1);
                MergePut(&job->out, keys.key[k], keys.keyLen[k]);
            }
            continue;
        }
        if (written) MergePut(&job->out, ",", 1);
        if (!list) {
            MergePut(&job->out, "\"", 1);
            MergePut(&job->out, keys.key[k], keys.keyLen[k]);
            MergePut(&job->out, "\":", 2);
        }
        if (!MergeStep(path, keys.key[k], keys.keyLen[k], list, step, sizeof step)) {
            job->out.failed = TRUE;
            break;
        }
        /* An item told apart by its first string is a tuple: whole, never merged inside. */
        if (MergeValue(job, &child, step, scheme == SCHEME_FIRST ? 0 : depth - 1)) written++;
        else job->out.len = before;   /* gone from the result: its key and comma too */
        if (!job->out.failed && job->out.data) job->out.data[job->out.len] = 0;
    }
    MergePut(&job->out, list ? "]" : "}", 1);
    MergeFreeKeys(&keys);
}

/* The merged value of `node` appended to the output; FALSE when it is absent from the result. */
static BOOL MergeValue(MergeJob *job, const MergeNode *node, const char *path, int depth)
{
    const char *one = NULL;
    size_t oneLen = 0;
    int m, ways = 0, latest = -1, chosen;
    DWORD members = 0;
    BOOL oneSet = FALSE, deeper = depth > 0;
    char kind = 0;
    MergeScheme scheme = SCHEME_NONE;
    for (m = 0; m < job->n; m++) {
        if (!node->changed[m]) continue;
        members |= 1u << m;
        if (!oneSet) {
            one = node->value[m];
            oneLen = node->valueLen[m];
            oneSet = TRUE;
            ways = 1;
        } else if (ways == 1 && !SameJson(one, oneLen, node->value[m], node->valueLen[m])) {
            ways = 2;
        }
    }
    if (ways == 0) {
        if (!node->base) return FALSE;
        MergePut(&job->out, node->base, node->baseLen);
        return TRUE;
    }
    if (ways == 1) {
        if (!one) return FALSE;
        MergePut(&job->out, one, oneLen);
        return TRUE;
    }
    /* Changed several ways: deeper, where every one of them (and the base) is an object, or a list told apart the same way. */
    for (m = 0; deeper && m < job->n; m++) {
        char k;
        if (!node->changed[m]) continue;
        if (!node->value[m]) {
            deeper = FALSE;   /* taken out by one, changed by another */
            break;
        }
        k = MergeKind(node->value[m], node->valueLen[m]);
        if ((k != '{' && k != '[') || (kind && k != kind)) deeper = FALSE;
        kind = k;
    }
    if (deeper && node->base && MergeKind(node->base, node->baseLen) != kind) deeper = FALSE;
    if (deeper && kind == '[') {
        MergeScheme one2;
        for (m = -1; deeper && m < job->n; m++) {
            const char *s = m < 0 ? node->base : node->changed[m] ? node->value[m] : NULL;
            size_t len = m < 0 ? node->baseLen : node->changed[m] ? node->valueLen[m] : 0;
            if (!s) continue;
            one2 = MergeListScheme(s, len);
            if (one2 == SCHEME_MIXED) deeper = FALSE;
            else if (one2 != SCHEME_NONE) {
                if (scheme == SCHEME_NONE) scheme = one2;
                else if (scheme != one2) deeper = FALSE;
            }
        }
        if (scheme == SCHEME_NONE) scheme = SCHEME_PLAIN;
    }
    if (deeper) {
        MergeElements(job, node, path, depth, kind == '[' ? scheme : SCHEME_NONE, kind == '[');
        return TRUE;
    }
    /* A conflict at `path`: the person's choice, else the one changed last until then. */
    latest = MergeLatest(job, node->changed);
    chosen = job->decide ? job->decide(job->context, path, members) : -1;
    if (chosen < 0 || chosen >= job->n || !(members & (1u << chosen))) {
        if (job->report) job->report(job->context, path, members, latest, node->value, node->valueLen);
        job->unresolved++;
        chosen = latest;
    }
    if (!node->value[chosen]) return FALSE;
    MergePut(&job->out, node->value[chosen], node->valueLen[chosen]);
    return TRUE;
}

char *Core_JsonMerge(const char *base, size_t baseLen, const char *const *values, const size_t *lengths, const BOOL *say,
                     const ULONGLONG *written, int n, int depth, CoreMergeDecide decide, CoreMergeReport report, void *context,
                     size_t *outLen, int *unresolved)
{
    MergeJob job;
    MergeNode node;
    int m;
    BOOL present;
    *outLen = 0;
    *unresolved = 0;
    if (n < 1 || n > MAX_PROFILES) return NULL;
    ZeroMemory(&job, sizeof job);
    ZeroMemory(&node, sizeof node);
    job.n = n;
    job.say = say;
    job.written = written;
    job.decide = decide;
    job.report = report;
    job.context = context;
    node.base = base;
    node.baseLen = base ? baseLen : 0;
    for (m = 0; m < n; m++) {
        node.value[m] = values[m];
        node.valueLen[m] = values[m] ? lengths[m] : 0;
        node.changed[m] = say[m] && !SameJson(node.value[m], node.valueLen[m], node.base, node.baseLen);
    }
    present = MergeValue(&job, &node, "", depth);
    *unresolved = job.unresolved;
    if (job.out.failed) {
        if (job.out.data) HeapFree(GetProcessHeap(), 0, job.out.data);
        return NULL;
    }
    if (!present) {
        if (job.out.data) HeapFree(GetProcessHeap(), 0, job.out.data);
        if ((job.out.data = (char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1)) == NULL) return NULL;
        *outLen = 0;   /* absent: an empty text */
        return job.out.data;
    }
    *outLen = job.out.len;
    return job.out.data;
}
