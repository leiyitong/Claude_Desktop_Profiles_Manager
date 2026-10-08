/*
 * The session vault, and the profiles whose sessions are kept the same.
 *
 * Claude keeps a profile's list of Code sessions in the profile
 * (sessionstore.c): Main's inside Claude's package when %APPDATA%\Claude does
 * not exist, so uninstalling Claude takes that list away, while the
 * conversations stay in Claude Code's folder. The vault keeps every list out
 * of Claude's reach, in our state folder: vault\lists\<list>\<time>.txt names
 * each session a version lists and its entry, kept once in
 * vault\objects\<hash>.json; vault\deleted.txt names the sessions deleted,
 * which nothing here brings back. A version is written only when the list
 * changed, and every version stays.
 *
 * The profiles whose sessions are kept the same (Profile.syncSessions) share
 * one list, VAULT_GROUP_LIST. Whenever one of them closes (its watcher), opens
 * through us, or the manager opens, each session is resolved against the
 * group's last version (Core_MirrorResolve): a change made in one profile
 * since then, the latest first, goes to the others; a session deleted in one
 * goes from the others; a profile that never had the list, or lost it (its
 * entries folder is new: Claude reinstalled, another account), gets it. The
 * entries go through sessionsync.c as Claude writes its own, at once where a
 * profile is closed, else once it closes, each backed up first; the version
 * is written only once every change was made or waits. A profile alone keeps
 * a list named after its folder, which only follows it: what it lost stays
 * listed there, to be restored (SessionVault_Restore).
 */
#include "app.h"
#include <stdlib.h>
#include <wchar.h>

#define VAULT_DIR             L"vault"
#define OBJECTS_DIR           L"objects"
#define LISTS_DIR             L"lists"
#define DELETED_FILE          L"deleted.txt"
#define VERSION_EXTENSION     L".txt"
#define TEMPORARY_SUFFIX      L".cdm-new"
#define TOMBSTONE_PREFIX      L"deleted_"
#define LOCAL_PREFIX          L"local_"
#define ARCHIVED_INDEX        L"archived-sessions.idx"   /* beside the entries: Claude's list of the archived ones */
#define VERSION_HEADER        L"claude-desktop-profiles-manager-vault 1"
#define TEXT_MAX_BYTES        (64u * 1024u * 1024u)
#define CONTENT_MAX_BYTES     (16u * 1024u * 1024u)
#define UNIX_EPOCH_TICKS      116444736000000000ULL
#define TICKS_PER_MILLISECOND (TICKS_PER_SECOND / 1000)
#define LINE_CCH              (FOLDER_CCH + SESSION_ID_CCH + 96)

typedef struct VaultItem {          /* a session a version lists */
    WCHAR     key[SESSION_ID_CCH];
    ULONGLONG hash;                 /* its entry: vault\objects\<hash>.json */
    ULONGLONG activity;             /* its last activity */
} VaultItem;

typedef struct VaultMember {        /* a profile that had the version's list */
    WCHAR     folder[FOLDER_CCH];
    ULONGLONG entries;              /* its entries folder (its path hashed) */
    ULONGLONG created;              /* ... and when that folder was made */
} VaultMember;

typedef struct VaultList {
    BOOL        found;
    ULONGLONG   index;              /* Claude's list of archived sessions; 0 for none */
    VaultMember members[MAX_PROFILES];
    int         memberCount;
    VaultItem  *items;
    int         count, capacity;
} VaultList;

typedef struct Dated {              /* an id deleted at `time` (ms) */
    WCHAR     id[SESSION_ID_CCH];
    ULONGLONG time;
} Dated;

typedef struct DatedSet {
    Dated *items;
    int    count, capacity;
    BOOL   changed;
} DatedSet;

/* How a member's list stands against the base. */
typedef enum MemberState { MEMBER_NEW, MEMBER_LOST, MEMBER_KEPT } MemberState;

/* --------------------------------------------------------------- memory */

static void *Grow(void *items, int *capacity, int need, size_t size)
{
    void *bigger;
    int n;
    if (need <= *capacity) return items;
    if (*capacity > 0x3FFFFFFF) return NULL;
    n = *capacity ? *capacity * 2 : 32;
    while (n < need) n *= 2;
    bigger = items ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, items, (size_t)n * size)
                   : HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)n * size);
    if (bigger) *capacity = n;
    return bigger;
}

static void Free(void *block)
{
    if (block) HeapFree(GetProcessHeap(), 0, block);
}

static ULONGLONG NowMs(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - UNIX_EPOCH_TICKS) / TICKS_PER_MILLISECOND;
}

static ULONGLONG FileTimeMs(const FILETIME *time)
{
    ULONGLONG ticks = ((ULONGLONG)time->dwHighDateTime << 32) | time->dwLowDateTime;
    return ticks > UNIX_EPOCH_TICKS ? (ticks - UNIX_EPOCH_TICKS) / TICKS_PER_MILLISECOND : 0;
}

/* When `path` was last written and made (ms); FALSE when it is not there. */
static BOOL FileTimes(const WCHAR *path, ULONGLONG *written, ULONGLONG *created)
{
    WCHAR extended[LONG_PATH_CCH];
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended)) || !GetFileAttributesExW(extended, GetFileExInfoStandard, &data))
        return FALSE;
    if (written) *written = FileTimeMs(&data.ftLastWriteTime);
    if (created) *created = ((ULONGLONG)data.ftCreationTime.dwHighDateTime << 32) | data.ftCreationTime.dwLowDateTime;
    return TRUE;
}

/* ---------------------------------------------------------------- files */

static BOOL VaultPath(const WCHAR *sub, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    if (!Util_StateDir(state, ARRAYSIZE(state))) return FALSE;
    return sub ? SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" VAULT_DIR L"\\%s", state, sub))
               : SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" VAULT_DIR, state));
}

/* `data` written to `path` through a file beside it put in place at once;
 * one there is replaced only with `replace`. */
static BOOL WriteWhole(const WCHAR *path, const char *data, size_t length, BOOL replace)
{
    WCHAR temporary[MAX_PATH + 16];
    HANDLE file;
    DWORD written = 0;
    BOOL ok;
    if (FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s" TEMPORARY_SUFFIX, path)) || length > MAXDWORD) return FALSE;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = (length == 0 || (WriteFile(file, data, (DWORD)length, &written, NULL) && written == length)) && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary, path, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) ||
                 (!replace && GetLastError() == ERROR_ALREADY_EXISTS);
    if (!ok) {
        DWORD error = GetLastError();
        DeleteFileW(temporary);
        SetLastError(error);
    }
    return ok;
}

/* `text` as UTF-8, written to `path`. */
static BOOL WriteText(const WCHAR *path, const WCHAR *text, BOOL replace)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *utf8;
    BOOL ok;
    if (bytes <= 0 || (utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)bytes)) == NULL) return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL);
    ok = WriteWhole(path, utf8, (size_t)bytes - 1, replace);
    Free(utf8);
    return ok;
}

/* A UTF-8 file of ours as text (a heap block, NULL when it is not there). */
static WCHAR *ReadText(const WCHAR *path)
{
    DWORD length = 0;
    char *raw = Util_ReadFile(path, TEXT_MAX_BYTES, FALSE, &length);
    WCHAR *text = NULL;
    int wide;
    if (!raw) return NULL;
    wide = MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, NULL, 0);
    if (wide >= 0 && (text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)wide + 1) * sizeof(WCHAR))) != NULL) {
        MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, text, wide);
        text[wide] = 0;
    }
    Free(raw);
    return text;
}

/* The next line of `*text`, cut out of it; NULL at the end. */
static WCHAR *NextLine(WCHAR **text)
{
    WCHAR *line = *text, *end;
    size_t length;
    if (!line || !*line) return NULL;
    end = wcschr(line, L'\n');
    if (end) {
        *end = 0;
        *text = end + 1;
    } else {
        *text = line + wcslen(line);
    }
    length = wcslen(line);
    if (length && line[length - 1] == L'\r') line[length - 1] = 0;
    return line;
}

/* Up to `max` tab-separated fields of `line`, cut in place; how many. */
static int Fields(WCHAR *line, WCHAR **fields, int max)
{
    int n = 0;
    while (n < max) {
        WCHAR *tab = wcschr(line, L'\t');
        fields[n++] = line;
        if (!tab) break;
        *tab = 0;
        line = tab + 1;
    }
    return n;
}

static BOOL HexNumber(const WCHAR *text, ULONGLONG *value)
{
    WCHAR *end;
    if (!text[0]) return FALSE;
    *value = _wcstoui64(text, &end, 16);
    return *end == 0;
}

static BOOL DecimalNumber(const WCHAR *text, ULONGLONG *value)
{
    WCHAR *end;
    if (!text[0]) return FALSE;
    *value = _wcstoui64(text, &end, 10);
    return *end == 0;
}

/* ------------------------------------------------------------- entries */

/* An entry's hash, without its own id: the same session's entry in two
 * profiles differs only there. Never 0, which stands for none. */
static ULONGLONG EntryHash(const char *json, size_t length)
{
    char *without = (char *)HeapAlloc(GetProcessHeap(), 0, length + 1);
    size_t used = 0;
    ULONGLONG hash;
    if (without && Core_JsonRemoveMember(json, length, "sessionId", without, length + 1, &used)) hash = Core_HashBytes(CORE_HASH_START, without, used);
    else hash = Core_HashBytes(CORE_HASH_START, json, length);
    Free(without);
    return hash ? hash : 1;
}

static ULONGLONG EntryActivity(const char *json, size_t length)
{
    const char *value;
    size_t valueLength;
    ULONGLONG number = 0;
    if (Core_JsonMember(json, length, "lastActivityAt", &value, &valueLength)) Core_JsonNumber(value, valueLength, &number);
    return number;
}

static BOOL ObjectPath(ULONGLONG hash, WCHAR *out, size_t cch)
{
    WCHAR name[32];
    return SUCCEEDED(StringCchPrintfW(name, ARRAYSIZE(name), OBJECTS_DIR L"\\%016I64x.json", hash)) && VaultPath(name, out, cch);
}

static BOOL SaveObject(ULONGLONG hash, const char *content, size_t length)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH];
    if (!ObjectPath(hash, path, ARRAYSIZE(path)) || !VaultPath(OBJECTS_DIR, dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir)) return FALSE;
    if (Util_FileExists(path)) return TRUE;
    return WriteWhole(path, content, length, FALSE);
}

static char *LoadObject(ULONGLONG hash, DWORD *length)
{
    WCHAR path[MAX_PATH];
    *length = 0;
    return ObjectPath(hash, path, ARRAYSIZE(path)) ? Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, length) : NULL;
}

/* The ids a session's entry goes by, as Claude marks a deleted one: its own
 * (without local_) and those of its transcripts. `add` gets each. */
static void EntryIds(const char *json, size_t length, void (*add)(void *context, const WCHAR *id), void *context)
{
    static const char *const single[] = { "cliSessionId", "preClearCliSessionId", "unarchivedCliSessionId" };
    WCHAR id[SESSION_ID_CCH];
    const char *value;
    size_t valueLength, i, start;
    for (i = 0; i < ARRAYSIZE(single); i++)
        if (Core_JsonMember(json, length, single[i], &value, &valueLength) && Core_JsonString(value, valueLength, id, ARRAYSIZE(id)) &&
            Core_IsUuid(id))
            add(context, id);
    if (Core_JsonMember(json, length, "sessionId", &value, &valueLength) && Core_JsonString(value, valueLength, id, ARRAYSIZE(id))) {
        size_t prefix = ARRAYSIZE(LOCAL_PREFIX) - 1;
        const WCHAR *own = wcsncmp(id, LOCAL_PREFIX, prefix) == 0 ? id + prefix : id;
        if (Core_IsUuid(own)) add(context, own);
    }
    if (!Core_JsonMember(json, length, "priorCliSessionIds", &value, &valueLength) || valueLength < 2 || value[0] != '[') return;
    for (i = 1; i < valueLength; i++) {
        if (value[i] != '"') continue;
        for (start = i++; i < valueLength && value[i] != '"'; i++)
            if (value[i] == '\\') i++;
        if (i < valueLength && Core_JsonString(value + start, i + 1 - start, id, ARRAYSIZE(id)) && Core_IsUuid(id)) add(context, id);
    }
}

/* ------------------------------------------------------------ id sets */

static int FindDated(const DatedSet *set, const WCHAR *id)
{
    int i;
    for (i = 0; i < set->count; i++)
        if (Core_EqualsI(set->items[i].id, id)) return i;
    return -1;
}

static BOOL AddDated(DatedSet *set, const WCHAR *id, ULONGLONG time)
{
    Dated *grown;
    int at = FindDated(set, id);
    if (at >= 0) {
        if (time > set->items[at].time) set->items[at].time = time;
        return TRUE;
    }
    if (!id[0] || (grown = (Dated *)Grow(set->items, &set->capacity, set->count + 1, sizeof *set->items)) == NULL) return FALSE;
    set->items = grown;
    StringCchCopyW(set->items[set->count].id, SESSION_ID_CCH, id);
    set->items[set->count++].time = time;
    set->changed = TRUE;
    return TRUE;
}

static void RemoveDated(DatedSet *set, const WCHAR *id)
{
    int at = FindDated(set, id);
    if (at < 0) return;
    set->items[at] = set->items[--set->count];
    set->changed = TRUE;
}

/* The time a profile's marks give `key`, or its own id without local_; 0 when none. */
static ULONGLONG MarkTime(const DatedSet *marks, const WCHAR *key)
{
    size_t prefix = ARRAYSIZE(LOCAL_PREFIX) - 1;
    int at = FindDated(marks, key);
    if (at < 0 && wcsncmp(key, LOCAL_PREFIX, prefix) == 0) at = FindDated(marks, key + prefix);
    return at >= 0 ? max(marks->items[at].time, 1) : 0;
}

/* Claude's marks of the sessions deleted from an entries folder. */
static void ReadMarks(const WCHAR *dir, DatedSet *marks)
{
    WCHAR path[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    if (!dir[0] || (find = Util_FindFiles(dir, TOMBSTONE_PREFIX L"*", &found, FALSE)) == INVALID_HANDLE_VALUE) return;
    do {
        const WCHAR *id = found.cFileName + ARRAYSIZE(TOMBSTONE_PREFIX) - 1;
        ULONGLONG time = 0;
        DWORD length = 0;
        char *text;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !Core_IsUuid(id)) continue;
        if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName)) &&
            (text = Util_ReadFile(path, 64, FALSE, &length)) != NULL) {
            Core_JsonNumber(text, length, &time);
            Free(text);
        }
        if (!time) time = FileTimeMs(&found.ftLastWriteTime);
        AddDated(marks, id, time);
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

/* The ledger of sessions deleted, which nothing here brings back. */
static void LoadLedger(DatedSet *ledger)
{
    WCHAR path[MAX_PATH], *text, *rest, *line, *fields[2];
    ULONGLONG time;
    ZeroMemory(ledger, sizeof *ledger);
    if (!VaultPath(DELETED_FILE, path, ARRAYSIZE(path)) || (text = ReadText(path)) == NULL) return;
    for (rest = text; (line = NextLine(&rest)) != NULL;)
        if (Fields(line, fields, 2) == 2 && Core_IsUuid(fields[0]) && DecimalNumber(fields[1], &time)) AddDated(ledger, fields[0], time);
    Free(text);
    ledger->changed = FALSE;
}

static BOOL SaveLedger(const DatedSet *ledger)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH], line[LINE_CCH], *text;
    size_t cch = (size_t)ledger->count * LINE_CCH + 1;
    int i;
    BOOL ok;
    if (!ledger->changed) return TRUE;
    if (!VaultPath(NULL, dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir) || !VaultPath(DELETED_FILE, path, ARRAYSIZE(path)) ||
        (text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cch * sizeof(WCHAR))) == NULL)
        return FALSE;
    text[0] = 0;
    for (i = 0; i < ledger->count; i++) {
        StringCchPrintfW(line, ARRAYSIZE(line), L"%s\t%I64u\n", ledger->items[i].id, ledger->items[i].time);
        StringCchCatW(text, cch, line);
    }
    ok = WriteText(path, text, TRUE);
    if (!ok) Util_Log(L"could not write %s (error %lu)", path, GetLastError());
    Free(text);
    return ok;
}

/* ------------------------------------------------------------ versions */

static void FreeList(VaultList *list)
{
    Free(list->items);
    ZeroMemory(list, sizeof *list);
}

static int FindItem(const VaultList *list, const WCHAR *key)
{
    int i;
    for (i = 0; i < list->count; i++)
        if (Core_EqualsI(list->items[i].key, key)) return i;
    return -1;
}

static BOOL AddItem(VaultList *list, const WCHAR *key, ULONGLONG hash, ULONGLONG activity)
{
    VaultItem *grown = (VaultItem *)Grow(list->items, &list->capacity, list->count + 1, sizeof *list->items);
    if (!grown) return FALSE;
    list->items = grown;
    StringCchCopyW(list->items[list->count].key, SESSION_ID_CCH, key);
    list->items[list->count].hash = hash;
    list->items[list->count++].activity = activity;
    return TRUE;
}

static BOOL ListDir(const WCHAR *listName, WCHAR *out, size_t cch)
{
    WCHAR sub[MAX_PATH];
    return listName[0] && !wcschr(listName, L'\\') && !wcschr(listName, L'/') && wcscmp(listName, L".") != 0 &&
           wcscmp(listName, L"..") != 0 && SUCCEEDED(StringCchPrintfW(sub, ARRAYSIZE(sub), LISTS_DIR L"\\%s", listName)) &&
           VaultPath(sub, out, cch);
}

static int __cdecl CompareNamesDescending(const void *a, const void *b)
{
    return -wcscmp((const WCHAR *)a, (const WCHAR *)b);
}

/* The version names of a list, newest first (a heap array of names without
 * their extension; NULL with none). */
static WCHAR (*VersionNames(const WCHAR *listName, int *count))[MAX_PATH]
{
    WCHAR dir[MAX_PATH], (*names)[MAX_PATH] = NULL;
    WIN32_FIND_DATAW found;
    HANDLE find;
    int capacity = 0;
    *count = 0;
    if (!ListDir(listName, dir, ARRAYSIZE(dir)) || (find = Util_FindFiles(dir, L"*" VERSION_EXTENSION, &found, FALSE)) == INVALID_HANDLE_VALUE)
        return NULL;
    do {
        WCHAR (*grown)[MAX_PATH];
        size_t length = wcslen(found.cFileName);
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !Core_EndsWithI(found.cFileName, VERSION_EXTENSION)) continue;
        if ((grown = (WCHAR (*)[MAX_PATH])Grow(names, &capacity, *count + 1, sizeof *names)) == NULL) break;
        names = grown;
        StringCchCopyNW(names[*count], MAX_PATH, found.cFileName, length - (ARRAYSIZE(VERSION_EXTENSION) - 1));
        (*count)++;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (*count > 1) qsort(names, (size_t)*count, sizeof *names, CompareNamesDescending);
    return names;
}

static BOOL VersionPath(const WCHAR *listName, const WCHAR *version, WCHAR *out, size_t cch)
{
    WCHAR dir[MAX_PATH];
    return ListDir(listName, dir, ARRAYSIZE(dir)) && !wcschr(version, L'\\') && !wcschr(version, L'/') &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s" VERSION_EXTENSION, dir, version));
}

static BOOL ParseVersion(WCHAR *text, VaultList *list)
{
    WCHAR *rest = text, *line, *fields[4];
    ULONGLONG a, b;
    int n;
    ZeroMemory(list, sizeof *list);
    if ((line = NextLine(&rest)) == NULL || wcscmp(line, VERSION_HEADER) != 0) return FALSE;
    while ((line = NextLine(&rest)) != NULL) {
        n = Fields(line, fields, 4);
        if (n == 4 && wcscmp(fields[0], L"listed") == 0 && fields[1][0] && wcslen(fields[1]) < SESSION_ID_CCH &&
            HexNumber(fields[2], &a) && DecimalNumber(fields[3], &b)) {
            if (!AddItem(list, fields[1], a, b)) return FALSE;
        } else if (n == 4 && wcscmp(fields[0], L"member") == 0 && list->memberCount < MAX_PROFILES && HexNumber(fields[2], &a) &&
                   DecimalNumber(fields[3], &b)) {
            VaultMember *member = &list->members[list->memberCount++];
            StringCchCopyW(member->folder, ARRAYSIZE(member->folder), fields[1]);
            member->entries = a;
            member->created = b;
        } else if (n == 2 && wcscmp(fields[0], L"index") == 0 && HexNumber(fields[1], &a)) {
            list->index = a;
        }
    }
    list->found = TRUE;
    return TRUE;
}

static BOOL LoadVersion(const WCHAR *listName, const WCHAR *version, VaultList *list)
{
    WCHAR path[MAX_PATH], *text;
    BOOL ok;
    ZeroMemory(list, sizeof *list);
    if (!VersionPath(listName, version, path, ARRAYSIZE(path)) || (text = ReadText(path)) == NULL) return FALSE;
    ok = ParseVersion(text, list);
    if (!ok) {
        FreeList(list);
        Util_Log(L"session vault: %s cannot be read", path);
    }
    Free(text);
    return ok;
}

/* The newest version of a list that reads; none: an empty list, not found. */
static void LoadLatest(const WCHAR *listName, VaultList *list, WCHAR *name, size_t cch)
{
    WCHAR (*names)[MAX_PATH];
    int count, i;
    ZeroMemory(list, sizeof *list);
    if (cch) name[0] = 0;
    names = VersionNames(listName, &count);
    for (i = 0; i < count; i++)
        if (LoadVersion(listName, names[i], list)) {
            if (cch) StringCchCopyW(name, cch, names[i]);
            break;
        }
    Free(names);
}

static int __cdecl CompareItems(const void *a, const void *b)
{
    return CompareStringOrdinal(((const VaultItem *)a)->key, -1, ((const VaultItem *)b)->key, -1, TRUE) - CSTR_EQUAL;
}

/* The version's text, its sessions in order: two versions alike read alike. */
static WCHAR *VersionText(VaultList *list)
{
    size_t cch = (size_t)(list->count + list->memberCount + 4) * LINE_CCH;
    WCHAR *text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cch * sizeof(WCHAR)), line[LINE_CCH];
    int i;
    if (!text) return NULL;
    if (list->count > 1) qsort(list->items, (size_t)list->count, sizeof *list->items, CompareItems);
    StringCchCopyW(text, cch, VERSION_HEADER L"\n");
    if (list->index) {
        StringCchPrintfW(line, ARRAYSIZE(line), L"index\t%016I64x\n", list->index);
        StringCchCatW(text, cch, line);
    }
    for (i = 0; i < list->memberCount; i++) {
        StringCchPrintfW(line, ARRAYSIZE(line), L"member\t%s\t%016I64x\t%I64u\n", list->members[i].folder, list->members[i].entries,
                         list->members[i].created);
        StringCchCatW(text, cch, line);
    }
    for (i = 0; i < list->count; i++) {
        StringCchPrintfW(line, ARRAYSIZE(line), L"listed\t%s\t%016I64x\t%I64u\n", list->items[i].key, list->items[i].hash,
                         list->items[i].activity);
        StringCchCatW(text, cch, line);
    }
    return text;
}

/* A new version of the list, unless it reads as the latest one. */
static BOOL SaveVersion(const WCHAR *listName, VaultList *list, const WCHAR *latest)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH], name[64], *text, *previous = NULL;
    SYSTEMTIME now;
    BOOL ok = FALSE;
    int copy;
    if ((text = VersionText(list)) == NULL) return FALSE;
    if (latest && latest[0] && VersionPath(listName, latest, path, ARRAYSIZE(path)) && (previous = ReadText(path)) != NULL &&
        wcscmp(previous, text) == 0) {
        ok = TRUE;
        goto done;
    }
    if (!ListDir(listName, dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir)) goto done;
    GetSystemTime(&now);
    for (copy = 1; copy < 100 && !ok; copy++) {
        if (copy == 1)
            StringCchPrintfW(name, ARRAYSIZE(name), L"%04u%02u%02u-%02u%02u%02u-%03u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                             now.wSecond, now.wMilliseconds);
        else
            StringCchPrintfW(name, ARRAYSIZE(name), L"%04u%02u%02u-%02u%02u%02u-%03u-%02d", now.wYear, now.wMonth, now.wDay, now.wHour,
                             now.wMinute, now.wSecond, now.wMilliseconds, copy);
        if (!VersionPath(listName, name, path, ARRAYSIZE(path))) break;
        if (Util_FileExists(path)) continue;
        ok = WriteText(path, text, FALSE);
        if (!ok) break;
    }
    if (ok) Util_Log(L"session vault: %s kept, %d session(s)", listName, list->count);
    else Util_Log(L"session vault: %s could not be kept (error %lu)", listName, GetLastError());
done:
    Free(previous);
    Free(text);
    return ok;
}

/* ---------------------------------------------------------------- keeping */

/* What the sync reads of one profile. */
typedef struct Member {
    MemberState state;
    BOOL        taker;               /* it has an entries folder, read whole */
    ULONGLONG   entries, created;
    DatedSet    marks;
} Member;

/* `profiles` of `list`, in order. */
static int Subset(const ProfileList *list, DWORD profiles, ProfileList *out)
{
    int i;
    ZeroMemory(out, sizeof *out);
    StringCchCopyW(out->defaultFolder, ARRAYSIZE(out->defaultFolder), list->defaultFolder);
    for (i = 0; i < list->count; i++)
        if (profiles & (1u << i)) out->items[out->count++] = list->items[i];
    return out->count;
}

static void ReadMember(const SessionSet *set, int m, const VaultList *base, Member *member)
{
    const SessionSource *source = &set->source[m];
    int i;
    ZeroMemory(member, sizeof *member);
    member->taker = source->entriesDir[0] != 0;
    if (source->entriesDir[0]) {
        member->entries = Core_HashText(CORE_HASH_START, source->entriesDir);
        FileTimes(source->entriesDir, NULL, &member->created);
        ReadMarks(source->entriesDir, &member->marks);
    }
    member->state = MEMBER_NEW;
    for (i = 0; i < base->memberCount; i++) {
        const VaultMember *had = &base->members[i];
        if (!Core_EqualsI(had->folder, set->profiles.items[m].folder)) continue;
        member->state = had->entries == member->entries && had->created == member->created ? MEMBER_KEPT : MEMBER_LOST;
    }
    /* Sessions it could not read are not taken for gone. */
    if (source->unreadable && member->state == MEMBER_KEPT) member->state = MEMBER_LOST;
}

/* A change for the sync's send, its content kept until then. */
typedef struct Changes {
    SyncSend *items;
    char    **owned;
    int       count, capacity, ownedCount, ownedCapacity;
} Changes;

static char *Own(Changes *changes, char *content)
{
    char **grown;
    if (!content) return NULL;
    if ((grown = (char **)Grow(changes->owned, &changes->ownedCapacity, changes->ownedCount + 1, sizeof *grown)) == NULL) {
        Free(content);
        return NULL;
    }
    changes->owned = grown;
    return changes->owned[changes->ownedCount++] = content;
}

static BOOL AddChange(Changes *changes, int profile, SyncOpKind kind, DWORD flags, const WCHAR *key, ULONGLONG time, ULONGLONG seen,
                      const char *content, size_t length)
{
    SyncSend *grown = (SyncSend *)Grow(changes->items, &changes->capacity, changes->count + 1, sizeof *grown), *change;
    if (!grown) return FALSE;
    changes->items = grown;
    change = &changes->items[changes->count++];
    ZeroMemory(change, sizeof *change);
    change->profile = profile;
    change->op.kind = kind;
    change->op.flags = flags;
    change->op.time = time;
    change->op.seen = seen;
    StringCchCopyW(change->op.key, ARRAYSIZE(change->op.key), key);
    change->content = content;
    change->length = length;
    return TRUE;
}

static void FreeChanges(Changes *changes)
{
    int i;
    for (i = 0; i < changes->ownedCount; i++) Free(changes->owned[i]);
    Free(changes->owned);
    Free(changes->items);
    ZeroMemory(changes, sizeof *changes);
}

typedef struct LedgerAdd {
    DatedSet *ledger;
    ULONGLONG time;
} LedgerAdd;

static void AddToLedger(void *context, const WCHAR *id)
{
    LedgerAdd *add = (LedgerAdd *)context;
    AddDated(add->ledger, id, add->time);
}

/* An entry's file: its content (a heap block) and when it was written. */
static char *ReadEntry(const WCHAR *file, DWORD *length, ULONGLONG *written)
{
    char *content = Util_ReadFile(file, SESSION_ENTRY_MAX_BYTES, FALSE, length);
    if (content && !FileTimes(file, written, NULL)) *written = 0;
    return content;
}

/* One session made the same in every member: `row` of `set` (-1: listed by
 * none, only by the base, as `key`). */
static BOOL ResolveSession(const SessionSet *set, int row, const WCHAR *key, const Member *members, const VaultList *base,
                           DatedSet *ledger, BOOL same, Changes *changes, VaultList *next)
{
    MirrorSide sides[MAX_PROFILES], baseSide;
    char *contents[MAX_PROFILES], *content = NULL;
    DWORD lengths[MAX_PROFILES], length = 0;
    int n = set->profiles.count, m, at = FindItem(base, key), deleted = FindDated(ledger, key), winner;
    BOOL ok = TRUE;
    ZeroMemory(contents, sizeof contents);
    ZeroMemory(lengths, sizeof lengths);
    for (m = 0; m < n; m++) {
        int entry = row >= 0 ? set->rows[row].entry[m] : -1;
        ULONGLONG marked;
        ZeroMemory(&sides[m], sizeof sides[m]);
        if (entry >= 0 && (contents[m] = ReadEntry(set->entries[entry].file, &lengths[m], &sides[m].time)) != NULL) {
            sides[m].state = MIRROR_LISTED;
            sides[m].hash = EntryHash(contents[m], lengths[m]);
        } else if (entry >= 0) {
            sides[m].state = MIRROR_MISSING;   /* listed, but cut short or being written: it says nothing */
        } else if ((marked = MarkTime(&members[m].marks, key)) != 0) {
            sides[m].state = MIRROR_DELETED;
            sides[m].time = marked;
        } else {
            sides[m].state = members[m].state == MEMBER_KEPT && at >= 0 ? MIRROR_REMOVED : MIRROR_MISSING;
        }
    }
    ZeroMemory(&baseSide, sizeof baseSide);
    baseSide.state = at >= 0 ? MIRROR_LISTED : MIRROR_DELETED;
    if (at >= 0) baseSide.hash = base->items[at].hash;
    winner = Core_MirrorResolve(sides, n, at >= 0 || deleted >= 0 ? &baseSide : NULL);
    if (winner >= 0) {
        ULONGLONG hash = winner < n ? sides[winner].hash : baseSide.hash;
        if (winner < n) {
            length = lengths[winner];
            content = Own(changes, contents[winner]);   /* sent: kept until then */
            contents[winner] = NULL;
            ok = content && SaveObject(hash, content, length);
        } else if ((content = Own(changes, LoadObject(hash, &length))) == NULL) {
            /* The base's entry is gone from the vault: nothing to give, the list kept as it was. */
            Util_Log(L"session vault: the entry of %s is missing", key);
            ok = AddItem(next, key, base->items[at].hash, base->items[at].activity);
            goto done;
        }
        for (m = 0; ok && same && m < n; m++) {
            int entry = row >= 0 ? set->rows[row].entry[m] : -1;
            if (!members[m].taker || (sides[m].state == MIRROR_LISTED && sides[m].hash == hash)) continue;
            ok = AddChange(changes, m, SYNC_PUT, SYNC_UNDELETE | SYNC_REPLACE, key, EntryActivity(content, length),
                           entry >= 0 ? set->entries[entry].lastActivity : 0, content, length);
        }
        if (ok) ok = AddItem(next, key, hash, EntryActivity(content, length));
        RemoveDated(ledger, key);
    } else if (winner == CORE_MIRROR_DELETED) {
        LedgerAdd add;
        add.ledger = ledger;
        add.time = deleted >= 0 ? max(ledger->items[deleted].time, 1) : 1;
        for (m = 0; m < n; m++)
            if (sides[m].state == MIRROR_DELETED && sides[m].time > add.time) add.time = sides[m].time;
        AddDated(ledger, key, add.time);
        for (m = 0; m < n; m++) {
            int entry = row >= 0 ? set->rows[row].entry[m] : -1;
            if (contents[m]) EntryIds(contents[m], lengths[m], AddToLedger, &add);
            if (ok && same && members[m].taker && entry >= 0 && sides[m].state == MIRROR_LISTED)
                ok = AddChange(changes, m, SYNC_REMOVE, 0, key, 0, set->entries[entry].lastActivity, NULL, 0);
        }
        if (at >= 0 && (content = LoadObject(base->items[at].hash, &length)) != NULL) {
            EntryIds(content, length, AddToLedger, &add);
            Free(content);
        }
    }
done:
    for (m = 0; m < n; m++) Free(contents[m]);
    return ok;
}

/* Claude's list of archived sessions, the same in every member: the latest
 * changed since the base. One a member that had the list lost is no
 * change; one Claude took away is not taken from the others. */
static BOOL ResolveIndex(const SessionSet *set, const Member *members, const VaultList *base, BOOL same, Changes *changes, VaultList *next)
{
    MirrorSide sides[MAX_PROFILES], baseSide;
    char *contents[MAX_PROFILES], *content = NULL;
    DWORD lengths[MAX_PROFILES], length = 0;
    WCHAR path[LONG_PATH_CCH];
    int n = set->profiles.count, m, winner;
    ULONGLONG hash;
    BOOL ok = TRUE;
    ZeroMemory(contents, sizeof contents);
    ZeroMemory(lengths, sizeof lengths);
    ZeroMemory(sides, sizeof sides);
    for (m = 0; m < n; m++) {
        sides[m].state = members[m].state == MEMBER_KEPT && base->index ? MIRROR_REMOVED : MIRROR_MISSING;
        if (!members[m].taker || FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" ARCHIVED_INDEX, set->source[m].entriesDir)) ||
            (contents[m] = ReadEntry(path, &lengths[m], &sides[m].time)) == NULL)
            continue;
        sides[m].state = MIRROR_LISTED;
        sides[m].hash = Core_HashBytes(CORE_HASH_START, contents[m], lengths[m]);
        if (!sides[m].hash) sides[m].hash = 1;
    }
    ZeroMemory(&baseSide, sizeof baseSide);
    baseSide.state = MIRROR_LISTED;
    baseSide.hash = base->index;
    winner = Core_MirrorResolve(sides, n, base->index ? &baseSide : NULL);
    if (winner < 0) goto done;
    hash = winner < n ? sides[winner].hash : base->index;
    if (winner < n) {
        length = lengths[winner];
        content = Own(changes, contents[winner]);
        contents[winner] = NULL;
        ok = content && SaveObject(hash, content, length);
    } else if ((content = Own(changes, LoadObject(hash, &length))) == NULL) {
        goto done;   /* the base's list gone from the vault: none given */
    }
    if (ok) next->index = hash;
    for (m = 0; ok && same && m < n; m++)
        if (members[m].taker && !(sides[m].state == MIRROR_LISTED && sides[m].hash == hash))
            ok = AddChange(changes, m, SYNC_INDEX, 0, ARCHIVED_INDEX, 0, 0, content, length);
done:
    for (m = 0; m < n; m++) Free(contents[m]);
    return ok;
}

BOOL SessionVault_Keep(const ProfileList *list, DWORD profiles, const WCHAR *listName, BOOL same, SyncReport *report)
{
    ProfileList members;
    SessionSet set;
    VaultList base, next;
    DatedSet ledger;
    Member member[MAX_PROFILES];
    Changes changes;
    WCHAR latest[MAX_PATH];
    int n, m, r, i;
    BOOL ok = TRUE;

    if ((n = Subset(list, profiles, &members)) == 0) return TRUE;
    /* What waits for a closed member first: the sync reads what it will hold. */
    for (m = 0; m < n; m++)
        if (!Claude_IsRunning(&members.items[m])) SessionEdit_ApplyPending(NULL, &members.items[m]);
    if (!SessionStore_LoadProfiles(&set, &members)) return FALSE;
    LoadLatest(listName, &base, latest, ARRAYSIZE(latest));
    LoadLedger(&ledger);
    ZeroMemory(&next, sizeof next);
    ZeroMemory(&changes, sizeof changes);
    for (m = 0; m < n; m++) ReadMember(&set, m, &base, &member[m]);

    for (r = 0; r < set.rowCount && ok; r++) ok = ResolveSession(&set, r, set.rows[r].key, member, &base, &ledger, same, &changes, &next);
    for (i = 0; i < base.count && ok; i++)
        if (SessionStore_FindRow(&set, base.items[i].key) < 0)
            ok = ResolveSession(&set, -1, base.items[i].key, member, &base, &ledger, same, &changes, &next);
    if (ok) ok = ResolveIndex(&set, member, &base, same, &changes, &next);
    /* Sessions Claude marked deleted that no list names: deleted before the vault knew them. */
    for (m = 0; m < n && ok; m++)
        for (i = 0; i < member[m].marks.count; i++)
            if (FindItem(&next, member[m].marks.items[i].id) < 0) AddDated(&ledger, member[m].marks.items[i].id, member[m].marks.items[i].time);

    if (ok && changes.count) ok = SessionSync_Send(&set, changes.items, changes.count, report);
    if (ok && !report->failed) {
        for (m = 0; m < n; m++) {
            if (!member[m].taker) continue;   /* not signed in yet: it gets the list once it is */
            StringCchCopyW(next.members[next.memberCount].folder, FOLDER_CCH, members.items[m].folder);
            next.members[next.memberCount].entries = member[m].entries;
            next.members[next.memberCount++].created = member[m].created;
        }
        ok = SaveVersion(listName, &next, latest) && SaveLedger(&ledger);
    } else if (report->failed) {
        Util_Log(L"session vault: %s not kept, %d change(s) could not be made", listName, report->failed);
        ok = FALSE;
    }
    if (changes.count) Util_Log(L"session vault: %s made the same, %d change(s) sent", listName, changes.count);
    for (m = 0; m < n; m++) Free(member[m].marks.items);
    Free(ledger.items);
    FreeChanges(&changes);
    FreeList(&base);
    FreeList(&next);
    SessionStore_Free(&set);
    return ok;
}

DWORD SessionVault_Group(const ProfileList *list)
{
    DWORD group = 0;
    int i;
    for (i = 0; i < list->count; i++)
        if (list->items[i].syncSessions) group |= 1u << i;
    return group;
}

BOOL SessionVault_ListName(const ProfileList *list, int index, WCHAR *out, size_t cch)
{
    if (index < 0 || index >= list->count) return FALSE;
    return SUCCEEDED(StringCchCopyW(out, cch, list->items[index].syncSessions ? VAULT_GROUP_LIST : list->items[index].folder));
}

/* The list profile `index` belongs to kept: the group's made the same. */
static BOOL KeepFor(const ProfileList *list, int index)
{
    SyncReport report;
    ZeroMemory(&report, sizeof report);
    if (index < 0 || index >= list->count) return FALSE;
    if (list->items[index].syncSessions) return SessionVault_Keep(list, SessionVault_Group(list), VAULT_GROUP_LIST, TRUE, &report);
    return SessionVault_Keep(list, 1u << index, list->items[index].folder, FALSE, &report);
}

void SessionVault_KeepAll(const ProfileList *list)
{
    int i;
    BOOL groupKept = FALSE;
    for (i = 0; i < list->count; i++) {
        if (list->items[i].syncSessions) {
            if (!groupKept) KeepFor(list, i);
            groupKept = TRUE;
        } else if (!Claude_IsRunning(&list->items[i])) {
            KeepFor(list, i);
        }
    }
}

void SessionVault_BeforeOpen(const ProfileList *list, int index)
{
    KeepFor(list, index);
}

void SessionVault_AfterClose(const WCHAR *folder)
{
    ProfileList list;
    int i;
    SessionEdit_ApplyPendingFor(folder);
    Profiles_Load(&list, NULL);
    if ((i = Profiles_Find(&list, folder)) >= 0) KeepFor(&list, i);
}

/* ---------------------------------------------------------------- restoring */

int SessionVault_Versions(const WCHAR *listName, VaultVersion *out, int capacity)
{
    WCHAR (*names)[MAX_PATH], path[MAX_PATH], *text;
    int total = 0, count = 0, i;
    names = VersionNames(listName, &total);
    for (i = 0; i < total && count < capacity; i++) {
        VaultList version;
        SYSTEMTIME at;
        FILETIME time;
        if (!VersionPath(listName, names[i], path, ARRAYSIZE(path)) || (text = ReadText(path)) == NULL) continue;
        if (ParseVersion(text, &version)) {
            ZeroMemory(&out[count], sizeof out[count]);
            ZeroMemory(&at, sizeof at);
            StringCchCopyW(out[count].name, ARRAYSIZE(out[count].name), names[i]);
            out[count].sessions = version.count;
            if (swscanf_s(names[i], L"%4hu%2hu%2hu-%2hu%2hu%2hu-%3hu", &at.wYear, &at.wMonth, &at.wDay, &at.wHour, &at.wMinute,
                          &at.wSecond, &at.wMilliseconds) == 7 && SystemTimeToFileTime(&at, &time))
                out[count].time = FileTimeMs(&time);
            count++;
        }
        FreeList(&version);
        Free(text);
    }
    Free(names);
    return count;
}

BOOL SessionVault_Restore(const ProfileList *list, int index, const WCHAR *listName, const WCHAR *version, SyncReport *report)
{
    ProfileList single;
    SessionSet set;
    VaultList kept;
    DatedSet ledger;
    Changes changes;
    WCHAR path[LONG_PATH_CCH];
    int i;
    BOOL ok = TRUE;
    if (index < 0 || index >= list->count || !LoadVersion(listName, version, &kept)) return FALSE;
    Subset(list, 1u << index, &single);
    if (!SessionStore_LoadProfiles(&set, &single)) {
        FreeList(&kept);
        return FALSE;
    }
    if (!set.source[0].entriesDir[0]) {
        report->unavailable |= 1;
        SessionStore_Free(&set);
        FreeList(&kept);
        return FALSE;
    }
    LoadLedger(&ledger);
    ZeroMemory(&changes, sizeof changes);
    for (i = 0; i < kept.count && ok; i++) {
        int entry = SessionStore_FindEntry(&set, 0, kept.items[i].key, NULL);
        ULONGLONG written;
        DWORD length = 0, currentLength = 0;
        char *content, *current = NULL;
        if (FindDated(&ledger, kept.items[i].key) >= 0) continue;   /* deleted since: it stays deleted */
        if (entry >= 0 && (current = ReadEntry(set.entries[entry].file, &currentLength, &written)) != NULL &&
            EntryHash(current, currentLength) == kept.items[i].hash) {
            Free(current);
            continue;
        }
        Free(current);
        if ((content = Own(&changes, LoadObject(kept.items[i].hash, &length))) == NULL) {
            report->failed++;
            if (!report->error[0]) StringCchPrintfW(report->error, ARRAYSIZE(report->error), TR(L"%s could not be read (error %lu)."), kept.items[i].key, (DWORD)ERROR_FILE_NOT_FOUND);
            continue;
        }
        ok = AddChange(&changes, 0, SYNC_PUT, SYNC_UNDELETE | SYNC_REPLACE, kept.items[i].key, EntryActivity(content, length),
                       entry >= 0 ? set.entries[entry].lastActivity : 0, content, length);
    }
    if (ok && kept.index && SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" ARCHIVED_INDEX, set.source[0].entriesDir))) {
        DWORD length = 0, currentLength = 0;
        char *current = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &currentLength), *content;
        ULONGLONG hash = current ? Core_HashBytes(CORE_HASH_START, current, currentLength) : 0;
        Free(current);
        if (hash != kept.index && (content = Own(&changes, LoadObject(kept.index, &length))) != NULL)
            ok = AddChange(&changes, 0, SYNC_INDEX, 0, ARCHIVED_INDEX, 0, 0, content, length);
    }
    /* Claude's marks of the sessions deleted: its own import of Claude Code's sessions leaves them out. */
    for (i = 0; i < ledger.count && ok; i++)
        if (FindItem(&kept, ledger.items[i].id) < 0)
            ok = AddChange(&changes, 0, SYNC_MARK, 0, ledger.items[i].id, ledger.items[i].time, 0, NULL, 0);
    if (ok && changes.count) ok = SessionSync_Send(&set, changes.items, changes.count, report);
    Util_Log(L"session vault: %s version %s restored to %s%s", listName, version, single.items[0].folder, ok ? L"" : L" FAILED");
    Free(ledger.items);
    FreeChanges(&changes);
    FreeList(&kept);
    SessionStore_Free(&set);
    /* A member restored to an older version: the others follow. */
    if (ok && list->items[index].syncSessions && !Claude_IsRunning(&list->items[index])) KeepFor(list, index);
    return ok;
}

/* ------------------------------------------------------------ known sessions */

static BOOL AddId(VaultIds *ids, const WCHAR *id)
{
    WCHAR (*grown)[SESSION_ID_CCH];
    if (!id[0] || SessionVault_HasId(ids, id)) return TRUE;
    if ((grown = (WCHAR (*)[SESSION_ID_CCH])Grow(ids->ids, &ids->capacity, ids->count + 1, sizeof *ids->ids)) == NULL) return FALSE;
    ids->ids = grown;
    StringCchCopyW(ids->ids[ids->count++], SESSION_ID_CCH, id);
    return TRUE;
}

static void AddIdTo(void *context, const WCHAR *id)
{
    AddId((VaultIds *)context, id);
}

BOOL SessionVault_HasId(const VaultIds *ids, const WCHAR *id)
{
    int i;
    for (i = 0; i < ids->count; i++)
        if (Core_EqualsI(ids->ids[i], id)) return TRUE;
    return FALSE;
}

void SessionVault_FreeIds(VaultIds *ids)
{
    Free(ids->ids);
    ZeroMemory(ids, sizeof *ids);
}

BOOL SessionVault_Ids(VaultIds *listed, VaultIds *deleted)
{
    WCHAR dir[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    DatedSet ledger;
    int i;
    ZeroMemory(listed, sizeof *listed);
    ZeroMemory(deleted, sizeof *deleted);
    LoadLedger(&ledger);
    for (i = 0; i < ledger.count; i++) AddId(deleted, ledger.items[i].id);
    Free(ledger.items);
    if (!VaultPath(LISTS_DIR, dir, ARRAYSIZE(dir)) || (find = Util_FindFiles(dir, L"*", &found, TRUE)) == INVALID_HANDLE_VALUE) return TRUE;
    do {
        VaultList latest;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        LoadLatest(found.cFileName, &latest, NULL, 0);
        for (i = 0; i < latest.count; i++) {
            DWORD length = 0;
            char *content = LoadObject(latest.items[i].hash, &length);
            AddId(listed, latest.items[i].key);
            if (content) EntryIds(content, length, AddIdTo, listed);
            Free(content);
        }
        FreeList(&latest);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return TRUE;
}

BOOL SessionVault_AddDeleted(const WCHAR *const *ids, int count)
{
    DatedSet ledger;
    ULONGLONG now = NowMs();
    BOOL ok;
    int i;
    LoadLedger(&ledger);
    for (i = 0; i < count; i++) AddDated(&ledger, ids[i], now);
    ok = SaveLedger(&ledger);
    Free(ledger.items);
    return ok;
}
