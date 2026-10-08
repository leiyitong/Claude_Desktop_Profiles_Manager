/*
 * Conversations of sessions no profile lists any more ("Recently deleted"),
 * deleted from Claude Code's folder, or put back in the lists that had them
 * (SessionVault_Undelete); and that whole folder copied beside itself.
 *
 * Deleting a session in Claude removes its entry only (sessionedit.c): its
 * conversation stays in Claude Code's folder, which every profile shares, and
 * a restore, Claude's own import of Claude Code's sessions or a script can
 * bring it back. What is offered here is a conversation that no profile lists
 * and no kept list names (sessionvault.c, which also holds the ones lost with a
 * reinstalled Claude): one Claude marked deleted, or the vault knows deleted;
 * or, unchecked, one nothing knows of (made in a terminal, say), unless it was
 * written in the last PURGE_RECENT_HOURS. Nothing running goes, nor what a
 * session listed goes on from. What goes is what Claude's own delete removes
 * for a transcript (SessionEdit_ListTranscriptFiles), to the Recycle Bin,
 * never a working folder; its ids then join the vault's deleted ones.
 *
 * Claude Code's folder holds the conversations of every profile, and only
 * there: once a week, when a Claude closes and none runs, it is copied beside
 * itself as <name>_auto_<date>, and only the two latest such copies stay.
 */
#include "app.h"
#include <objbase.h>
#include <shellapi.h>
#include <stdlib.h>
#include <wchar.h>

#define TRANSCRIPT_EXTENSION  L".jsonl"
#define TOMBSTONE_PREFIX      L"deleted_"
#define TAIL_BYTES            (256u * 1024u)   /* the end of a transcript, where Claude Code writes its title */
#define UNIX_EPOCH_TICKS      116444736000000000ULL
#define TICKS_PER_MILLISECOND (TICKS_PER_SECOND / 1000)
#define HOUR_MS               (3600ULL * 1000ULL)

static void Free(void *block)
{
    if (block) HeapFree(GetProcessHeap(), 0, block);
}

static ULONGLONG FileTimeMs(const FILETIME *time)
{
    ULONGLONG ticks = ((ULONGLONG)time->dwHighDateTime << 32) | time->dwLowDateTime;
    return ticks > UNIX_EPOCH_TICKS ? (ticks - UNIX_EPOCH_TICKS) / TICKS_PER_MILLISECOND : 0;
}

static ULONGLONG NowMs(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return FileTimeMs(&now);
}

BOOL SessionPurge_CodeFolder(WCHAR *out, size_t cch)
{
    WCHAR *slash;
    if (!SessionStore_ProjectsDir(out, cch) || (slash = wcsrchr(out, L'\\')) == NULL) return FALSE;
    *slash = 0;
    return TRUE;
}

/* Claude's marks of deleted sessions, in every profile's entries folder. */
static void AddMarks(const SessionSet *set, VaultIds *deleted)
{
    WIN32_FIND_DATAW found;
    HANDLE find;
    int p;
    for (p = 0; p < set->profiles.count; p++) {
        if (!set->source[p].entriesDir[0] ||
            (find = Util_FindFiles(set->source[p].entriesDir, TOMBSTONE_PREFIX L"*", &found, FALSE)) == INVALID_HANDLE_VALUE)
            continue;
        do {
            const WCHAR *id = found.cFileName + ARRAYSIZE(TOMBSTONE_PREFIX) - 1;
            WCHAR (*grown)[SESSION_ID_CCH];
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !Core_IsUuid(id) || SessionVault_HasId(deleted, id)) continue;
            if (deleted->count == deleted->capacity) {
                int larger = deleted->capacity ? deleted->capacity * 2 : 64;
                grown = deleted->ids ? (WCHAR (*)[SESSION_ID_CCH])HeapReAlloc(GetProcessHeap(), 0, deleted->ids, (size_t)larger * sizeof *grown)
                                     : (WCHAR (*)[SESSION_ID_CCH])HeapAlloc(GetProcessHeap(), 0, (size_t)larger * sizeof *grown);
                if (!grown) break;
                deleted->ids = grown;
                deleted->capacity = larger;
            }
            StringCchCopyW(deleted->ids[deleted->count++], SESSION_ID_CCH, id);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
}

/* A member of one transcript line, decoded; FALSE when it has none. */
static BOOL LineMember(const char *line, size_t length, const char *key, WCHAR *out, size_t cch)
{
    const char *value;
    size_t valueLength;
    return Core_JsonMember(line, length, key, &value, &valueLength) && Core_JsonString(value, valueLength, out, cch) && out[0];
}

/* What the end of a transcript says of it: the title it was given, else its
 * summary, else the last prompt; the folder it worked in. */
static void Describe(PurgeItem *item)
{
    WCHAR prompt[SESSION_TITLE_CCH] = L"", summary[SESSION_TITLE_CCH] = L"", custom[SESSION_TITLE_CCH] = L"", text[SESSION_TITLE_CCH];
    DWORD length = 0;
    char *tail = Util_ReadFile(item->path, TAIL_BYTES, TRUE, &length), *line, *end;
    if (!tail) return;
    for (line = tail; line < tail + length; line = end + 1) {
        size_t size;
        const char *kind;
        size_t kindLength;
        end = (char *)memchr(line, '\n', (size_t)(tail + length - line));
        if (!end) end = tail + length;
        size = (size_t)(end - line);
        if (size < 2 || line[0] != '{') continue;   /* the first one is cut where the read began */
        if (Core_JsonMember(line, size, "type", &kind, &kindLength)) {
            if (kindLength == sizeof "\"custom-title\"" - 1 && memcmp(kind, "\"custom-title\"", kindLength) == 0 &&
                LineMember(line, size, "customTitle", text, ARRAYSIZE(text)))
                StringCchCopyW(custom, ARRAYSIZE(custom), text);
            else if (kindLength == sizeof "\"summary\"" - 1 && memcmp(kind, "\"summary\"", kindLength) == 0 &&
                     LineMember(line, size, "summary", text, ARRAYSIZE(text)))
                StringCchCopyW(summary, ARRAYSIZE(summary), text);
            else if (kindLength == sizeof "\"last-prompt\"" - 1 && memcmp(kind, "\"last-prompt\"", kindLength) == 0 &&
                     LineMember(line, size, "lastPrompt", text, ARRAYSIZE(text)))
                StringCchCopyW(prompt, ARRAYSIZE(prompt), text);
        }
        LineMember(line, size, "cwd", item->project, ARRAYSIZE(item->project));
    }
    Free(tail);
    StringCchCopyW(item->title, ARRAYSIZE(item->title), custom[0] ? custom : summary[0] ? summary : prompt);
}

static int __cdecl CompareWritten(const void *a, const void *b)
{
    const PurgeItem *x = (const PurgeItem *)a, *y = (const PurgeItem *)b;
    if (x->written != y->written) return x->written > y->written ? -1 : 1;
    return CompareStringOrdinal(x->id, -1, y->id, -1, TRUE) - CSTR_EQUAL;
}

static int FindItem(const PurgeItem *items, int count, const WCHAR *id)
{
    int i;
    for (i = 0; i < count; i++)
        if (Core_EqualsI(items[i].id, id)) return i;
    return -1;
}

int SessionPurge_List(const ProfileList *profiles, PurgeItem **items, WCHAR *error, size_t errorCch)
{
    WCHAR projects[LONG_PATH_CCH], folder[LONG_PATH_CCH];
    WIN32_FIND_DATAW project, file;
    HANDLE folders, files;
    SessionSet set;
    VaultIds listed, deleted;
    ProfileList now = *profiles;
    ULONGLONG recent = NowMs() - PURGE_RECENT_HOURS * HOUR_MS;
    int count = 0, capacity = 0, i;
    *items = NULL;
    error[0] = 0;
    if (!SessionStore_LoadProfiles(&set, profiles)) {
        StringCchCopyW(error, errorCch, TR(L"Sessions could not be loaded."));
        return -1;
    }
    SessionVault_Ids(&listed, &deleted);
    AddMarks(&set, &deleted);
    Claude_UpdateRunning(&now);
    if (SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) &&
        (folders = Util_FindFiles(projects, L"*", &project, TRUE)) != INVALID_HANDLE_VALUE) {
        do {
            if (!(project.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || project.cFileName[0] == L'.' ||
                FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", projects, project.cFileName)) ||
                (files = Util_FindFiles(folder, L"*" TRANSCRIPT_EXTENSION, &file, FALSE)) == INVALID_HANDLE_VALUE)
                continue;
            do {
                WCHAR id[SESSION_ID_CCH];
                size_t idLength = wcslen(file.cFileName) - (ARRAYSIZE(TRANSCRIPT_EXTENSION) - 1);
                ULONGLONG written = FileTimeMs(&file.ftLastWriteTime), bytes = ((ULONGLONG)file.nFileSizeHigh << 32) | file.nFileSizeLow;
                PurgeItem *item;
                BOOL outside = FALSE;
                int at;
                if ((file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !Core_EndsWithI(file.cFileName, TRANSCRIPT_EXTENSION) ||
                    idLength >= ARRAYSIZE(id))
                    continue;
                StringCchCopyNW(id, ARRAYSIZE(id), file.cFileName, idLength);
                /* Listed by a profile, the one a listed session goes on from, or kept in the vault to be restored. */
                if (!Core_IsUuid(id) || SessionStore_ClaimedByOther(&set, -1, -1, id) || SessionVault_HasId(&listed, id)) continue;
                if ((at = FindItem(*items, count, id)) >= 0) {   /* continued from another folder: the larger one */
                    if (bytes > (*items)[at].bytes) {
                        (*items)[at].bytes = bytes;
                        StringCchPrintfW((*items)[at].path, LONG_PATH_CCH, L"%s\\%s", folder, file.cFileName);
                    }
                    continue;
                }
                if (!SessionVault_HasId(&deleted, id) && written > recent) continue;
                if (SessionStore_RunningNow(&now, id, &outside) || outside) continue;
                if (count == capacity) {
                    int larger = capacity ? capacity * 2 : 32;
                    PurgeItem *grown = *items ? (PurgeItem *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, *items, (size_t)larger * sizeof *grown)
                                              : (PurgeItem *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)larger * sizeof *grown);
                    if (!grown) break;
                    *items = grown;
                    capacity = larger;
                }
                item = &(*items)[count++];
                ZeroMemory(item, sizeof *item);
                StringCchCopyW(item->id, ARRAYSIZE(item->id), id);
                StringCchPrintfW(item->path, ARRAYSIZE(item->path), L"%s\\%s", folder, file.cFileName);
                item->kind = SessionVault_HasId(&deleted, id) ? PURGE_DELETED : PURGE_UNKNOWN;
                item->bytes = bytes;
                item->written = written;
            } while (FindNextFileW(files, &file));
            FindClose(files);
        } while (FindNextFileW(folders, &project));
        FindClose(folders);
    }
    for (i = 0; i < count; i++) Describe(&(*items)[i]);
    if (count > 1) qsort(*items, (size_t)count, sizeof **items, CompareWritten);
    SessionVault_FreeIds(&listed);
    SessionVault_FreeIds(&deleted);
    if (count > 0 && SessionVault_EverListed(&listed)) {
        for (i = 0; i < count; i++) (*items)[i].restorable = SessionVault_HasId(&listed, (*items)[i].id);
        SessionVault_FreeIds(&listed);
    }
    SessionStore_Free(&set);
    Util_Log(L"conversations no list names: %d", count);
    return count;
}

/* Only what lies in Claude Code's folder or its temporary folder. */
static BOOL InCodeFolders(const WCHAR *path)
{
    WCHAR code[LONG_PATH_CCH], temporary[MAX_PATH];
    return (SessionPurge_CodeFolder(code, ARRAYSIZE(code)) && Core_PathUnder(path, code) && !Core_PathEquals(path, code)) ||
           (SessionEdit_TemporaryDir(temporary, ARRAYSIZE(temporary)) && Core_PathUnder(path, temporary) && !Core_PathEquals(path, temporary));
}

RemoveResult SessionPurge_Delete(HWND owner, const ProfileList *profiles, const PurgeItem *items, const int *chosen, int count,
                                 int *deleted, WCHAR *error, size_t errorCch)
{
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    const WCHAR **ids, **list = NULL;
    ProfileList now = *profiles;
    RemoveResult result = REMOVE_FAILED;
    int i, idCount = 0, pathCount = 0;
    *deleted = 0;
    error[0] = 0;
    if (count <= 0) return REMOVE_DONE;
    if ((ids = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *ids)) == NULL) return REMOVE_FAILED;
    Claude_UpdateRunning(&now);
    for (i = 0; i < count; i++) {
        BOOL outside = FALSE;
        const PurgeItem *item = &items[chosen[i]];
        /* Read again: one may have started since the list was made. */
        if (SessionStore_RunningNow(&now, item->id, &outside) || outside) {
            Util_Log(L"conversation %s kept: it runs now", item->id);
            continue;
        }
        ids[idCount++] = item->id;
    }
    if (!idCount) {
        result = REMOVE_DONE;
        goto done;
    }
    if (!SessionEdit_ListTranscriptFiles(ids, idCount, &paths, &pathCount, error, errorCch)) goto done;
    if ((list = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)max(pathCount, 1) * sizeof *list)) == NULL) goto done;
    for (i = 0; i < pathCount; i++) {
        if (!InCodeFolders(paths[i])) {
            StringCchPrintfW(error, errorCch, TR(L"%s could not be removed (error %lu)."), paths[i], (DWORD)ERROR_ACCESS_DENIED);
            Util_Log(L"conversation cleanup refused %s: outside Claude Code's folders", paths[i]);
            goto done;
        }
        if (!Util_FitsRecycleBin(paths[i])) {
            StringCchPrintfW(error, errorCch, TR(L"This file's path is too long for the Recycle Bin: %s"), paths[i]);
            goto done;
        }
        list[i] = paths[i];
    }
    result = pathCount ? Util_Recycle(owner, list, pathCount) : REMOVE_DONE;
    Util_Log(L"conversations cleaned up: %d (%d item(s)) %s", idCount, pathCount,
             result == REMOVE_DONE ? L"done" : result == REMOVE_CANCELLED ? L"cancelled" : L"FAILED");
    if (result == REMOVE_FAILED && !error[0]) StringCchCopyW(error, errorCch, TR(L"Some of its files could not be moved to the Recycle Bin."));
    if (result == REMOVE_DONE) {
        *deleted = idCount;
        SessionVault_AddDeleted(ids, idCount);
    }
done:
    Free(paths);
    Free((void *)list);
    Free((void *)ids);
    return result;
}

/* ------------------------------------------------------------ the copy */

BOOL SessionPurge_BackupName(WCHAR *out, size_t cch)
{
    WCHAR code[MAX_PATH], parent[MAX_PATH], name[MAX_PATH], *slash;
    const WCHAR *leaf;
    SYSTEMTIME today;
    int copy;
    if (!SessionPurge_CodeFolder(code, ARRAYSIZE(code)) || FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), code)) ||
        (slash = wcsrchr(parent, L'\\')) == NULL)
        return FALSE;
    *slash = 0;
    leaf = code + (slash + 1 - parent);
    GetLocalTime(&today);
    for (copy = 1; copy < 1000; copy++) {
        if (!Core_DatedCopyName(leaf, &today, copy, name, ARRAYSIZE(name)) ||
            FAILED(StringCchPrintfW(out, cch, L"%s\\%s", parent, name)))
            return FALSE;
        if (Util_QueryPath(out, NULL) == PATH_MISSING) return TRUE;
    }
    return FALSE;
}

CopyResult SessionPurge_BackUp(HWND owner, const WCHAR *to, DWORD *error)
{
    WCHAR code[MAX_PATH], source[MAX_PATH + 3], target[MAX_PATH + 1];   /* double zero-terminated lists */
    SHFILEOPSTRUCTW operation;
    int result;
    *error = 0;
    ZeroMemory(source, sizeof source);
    ZeroMemory(target, sizeof target);
    if (!SessionPurge_CodeFolder(code, ARRAYSIZE(code)) || !Util_DirExists(code) ||
        FAILED(StringCchPrintfW(source, ARRAYSIZE(source) - 1, L"%s\\*", code)) || FAILED(StringCchCopyW(target, ARRAYSIZE(target) - 1, to))) {
        *error = ERROR_PATH_NOT_FOUND;
        return COPY_FAILED;
    }
    if (!CreateDirectoryW(to, NULL)) {
        *error = GetLastError();
        return COPY_FAILED;
    }
    /* With an owner, Windows shows the copy's progress, and asks about a
     * file in use; without one, nothing is shown. */
    ZeroMemory(&operation, sizeof operation);
    operation.hwnd = owner;
    operation.wFunc = FO_COPY;
    operation.pFrom = source;
    operation.pTo = target;
    operation.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | (owner ? 0 : FOF_SILENT | FOF_NOERRORUI);
    result = SHFileOperationW(&operation);
    if (result == 0 && !operation.fAnyOperationsAborted) {
        Util_Log(L"%s copied to %s", code, to);
        return COPY_MADE;
    }
    *error = (DWORD)result;
    Util_Log(L"%s not copied whole to %s (code %d%s)", code, to, result, operation.fAnyOperationsAborted ? L", cancelled" : L"");
    return operation.fAnyOperationsAborted || result == ERROR_CANCELLED ? COPY_CANCELLED : COPY_FAILED;
}

/* ------------------------------------------------------- the weekly copy */

#define WEEKLY_COPIES_KEPT 2
#define WEEKLY_COPY_MUTEX  L"Local\\ClaudeDesktopProfilesManager.WeeklyCopy"   /* one watcher copies at a time */
#define PARTIAL_SUFFIX     L".partial"

typedef struct WeeklyCopy {
    WCHAR      name[MAX_PATH];
    SYSTEMTIME day;
} WeeklyCopy;

static int __cdecl CompareCopiesNewestFirst(const void *a, const void *b)
{
    return -wcscmp(((const WeeklyCopy *)a)->name, ((const WeeklyCopy *)b)->name);   /* the same prefix, then the date */
}

/* The copies beside Claude Code's folder `leaf` in `parent`: the latest day
 * of any (`latest`, wYear 0 for none), and the weekly ones, newest first. */
static int ReadCopies(const WCHAR *parent, const WCHAR *leaf, SYSTEMTIME *latest, WeeklyCopy *weekly, int capacity)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    int count = 0;
    ZeroMemory(latest, sizeof *latest);
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s_*", leaf)) ||
        (find = Util_FindFiles(parent, pattern, &found, TRUE)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        SYSTEMTIME day;
        BOOL isWeekly;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !Core_CopyDay(leaf, found.cFileName, &day, &isWeekly)) continue;
        if (day.wYear > latest->wYear || (day.wYear == latest->wYear && (day.wMonth > latest->wMonth ||
            (day.wMonth == latest->wMonth && day.wDay > latest->wDay))))
            *latest = day;
        if (isWeekly && count < capacity) {
            StringCchCopyW(weekly[count].name, ARRAYSIZE(weekly[count].name), found.cFileName);
            weekly[count++].day = day;
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (count > 1) qsort(weekly, (size_t)count, sizeof *weekly, CompareCopiesNewestFirst);
    return count;
}

void SessionPurge_WeeklyBackUp(const ProfileList *profiles)
{
    WCHAR code[MAX_PATH], parent[MAX_PATH], name[MAX_PATH], target[MAX_PATH], partial[MAX_PATH + 16], old[MAX_PATH], *slash;
    const WCHAR *leaf;
    WeeklyCopy weekly[16];
    SYSTEMTIME latest, today;
    HANDLE mutex;
    DWORD error = 0;
    BOOL whole;
    int count, i;
    for (i = 0; i < profiles->count; i++)
        if (Claude_IsRunning(&profiles->items[i])) return;   /* its Claude Code writes there: the next close copies */
    if (!SessionPurge_CodeFolder(code, ARRAYSIZE(code)) || !Util_DirExists(code) || FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), code)) ||
        (slash = wcsrchr(parent, L'\\')) == NULL)
        return;
    *slash = 0;
    leaf = code + (slash + 1 - parent);
    if ((mutex = CreateMutexW(NULL, FALSE, WEEKLY_COPY_MUTEX)) == NULL) return;
    if (WaitForSingleObject(mutex, 0) != WAIT_OBJECT_0) {
        CloseHandle(mutex);
        return;
    }
    GetLocalTime(&today);
    ReadCopies(parent, leaf, &latest, weekly, ARRAYSIZE(weekly));
    if (!Core_WeeklyCopyDue(&latest, &today) || !Core_WeeklyCopyName(leaf, &today, name, ARRAYSIZE(name)) ||
        FAILED(StringCchPrintfW(target, ARRAYSIZE(target), L"%s\\%s", parent, name)) ||
        FAILED(StringCchPrintfW(partial, ARRAYSIZE(partial), L"%s" PARTIAL_SUFFIX, target)) || Util_QueryPath(target, NULL) != PATH_MISSING)
        goto done;
    /* Copied under another name first: a copy cut short is never taken for one. */
    if (!Util_DeleteTree(partial, &error)) {
        Util_Log(L"weekly copy: %s cannot be replaced (error %lu)", partial, error);
        goto done;
    }
    whole = Util_CopyTree(code, partial, FALSE, &error);
    if (!MoveFileW(partial, target)) {
        Util_Log(L"weekly copy: %s cannot be named %s (error %lu)", partial, target, GetLastError());
        goto done;
    }
    if (!whole) {
        /* Kept, as a copy of what could be read; the older copies stay too. */
        Util_Log(L"weekly copy: %s copied to %s, not whole (error %lu)", code, target, error);
        goto done;
    }
    Util_Log(L"weekly copy: %s copied to %s", code, target);
    count = ReadCopies(parent, leaf, &latest, weekly, ARRAYSIZE(weekly));
    for (i = WEEKLY_COPIES_KEPT; i < count; i++) {
        if (FAILED(StringCchPrintfW(old, ARRAYSIZE(old), L"%s\\%s", parent, weekly[i].name))) continue;
        if (Util_DeleteTree(old, &error)) Util_Log(L"weekly copy: %s removed, the latest %d stay", old, WEEKLY_COPIES_KEPT);
        else Util_Log(L"weekly copy: %s could not be removed (error %lu)", old, error);
    }
done:
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}
