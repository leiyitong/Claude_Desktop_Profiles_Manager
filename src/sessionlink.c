/*
 * A profile's session entries kept in a folder it shares: another profile's
 * (two accounts list the same sessions) or any folder of the user's.
 *
 * Claude keeps a profile's entries in claude-code-sessions\<account>\
 * <organization> (sessionstore.c), so the link is made there: that folder
 * becomes a directory junction (no administrator rights; any local drive)
 * to the shared one, or a directory symbolic link for a network share (which
 * Windows allows without rights only in Developer Mode). Claude reads and
 * writes through it as through its own folder. The entries a profile had
 * are copied into the shared folder first (the ones it holds already stay),
 * and kept in a backup; unlinking gives the profile a folder of its own
 * again, with the entries the shared one holds then.
 *
 * Claude keeps its sessions in memory and writes them back, so the entries
 * of a folder some profile shares are written only while every profile that
 * shares it is closed (SessionLink_Busy): a link to a running profile's
 * folder, or from a running profile, is refused.
 */
#include "app.h"
#include <winioctl.h>
#include <stdarg.h>
#include <shellapi.h>

#define BACKUPS_DIR L"backups"

/* A mount point's reparse data (ntifs.h's REPARSE_DATA_BUFFER, its mount point part). */
typedef struct MountPoint {
    DWORD tag;
    WORD  dataLength;
    WORD  reserved;
    WORD  substituteOffset, substituteLength, printOffset, printLength;
    WCHAR paths[2 * LONG_PATH_CCH + 8];
} MountPoint;

/* `path` without the \\?\ (or \\?\UNC\) that GetFinalPathNameByHandle puts before it. */
static void PlainPath(WCHAR *path, size_t cch)
{
    if (wcsncmp(path, L"\\\\?\\UNC\\", 8) == 0) {
        memmove(path + 2, path + 8, (wcslen(path + 8) + 1) * sizeof(WCHAR));
    } else if (wcsncmp(path, L"\\\\?\\", 4) == 0) {
        memmove(path, path + 4, (wcslen(path + 4) + 1) * sizeof(WCHAR));
    }
    (void)cch;
}

/* The folder `path` leads to, links followed; FALSE when it cannot be opened. */
static BOOL FinalPath(const WCHAR *path, WCHAR *out, size_t cch)
{
    WCHAR extended[LONG_PATH_CCH];
    HANDLE dir;
    DWORD length;
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return FALSE;
    dir = CreateFileW(extended, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (dir == INVALID_HANDLE_VALUE) return FALSE;
    length = GetFinalPathNameByHandleW(dir, out, (DWORD)cch, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(dir);
    if (!length || length >= cch) return FALSE;
    PlainPath(out, cch);
    return TRUE;
}

static BOOL IsLink(const WCHAR *path)
{
    DWORD attributes = 0;
    return Util_QueryPath(path, &attributes) == PATH_PRESENT && (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

/* The entries folder of `p` as Claude finds it, and where it leads. */
static BOOL EntriesFolders(const Profile *p, WCHAR *dir, size_t dirCch, WCHAR *final, size_t finalCch, BOOL *signedIn)
{
    BOOL ignored;
    if (!signedIn) signedIn = &ignored;
    return SessionStore_EntriesDir(p, dir, dirCch, signedIn) && FinalPath(dir, final, finalCch);
}

void SessionLink_Read(const ProfileList *list, int index, LinkState *state)
{
    WCHAR final[LONG_PATH_CCH], other[LONG_PATH_CCH], otherFinal[LONG_PATH_CCH];
    BOOL signedIn = FALSE;
    int i;
    ZeroMemory(state, sizeof *state);
    state->profile = -1;
    if (index < 0 || index >= list->count) return;
    if (!SessionStore_EntriesDir(&list->items[index], state->dir, ARRAYSIZE(state->dir), &signedIn)) {
        state->kind = signedIn ? LINK_NO_SESSIONS : LINK_NOT_SIGNED_IN;
        state->dir[0] = 0;
        return;
    }
    if (!IsLink(state->dir)) {
        state->kind = LINK_OWN;
        return;
    }
    state->kind = LINK_FOLDER;
    if (!FinalPath(state->dir, final, ARRAYSIZE(final))) {
        state->kind = LINK_BROKEN;   /* it leads to a folder that is gone */
        return;
    }
    StringCchCopyW(state->target, ARRAYSIZE(state->target), final);
    for (i = 0; i < list->count; i++) {
        if (i == index || !SessionStore_EntriesDir(&list->items[i], other, ARRAYSIZE(other), &signedIn) || IsLink(other) ||
            !FinalPath(other, otherFinal, ARRAYSIZE(otherFinal)) || !Core_PathEquals(otherFinal, final))
            continue;
        state->kind = LINK_PROFILE;
        state->profile = i;
        return;
    }
}

/* A profile's Claude, from its data folder as a running one titles its
 * Chromium message window: the stock folder through its storage. */
static void RunningProfile(const WCHAR *dataDir, const ClaudePackage *pkg, Profile *p)
{
    WCHAR appData[MAX_PATH], localAppData[MAX_PATH], stock[MAX_PATH];
    const WCHAR *leaf = wcsrchr(dataDir, L'\\');
    ZeroMemory(p, sizeof *p);
    StringCchCopyW(p->dataDir, ARRAYSIZE(p->dataDir), dataDir);
    StringCchCopyW(p->folder, ARRAYSIZE(p->folder), leaf ? leaf + 1 : dataDir);
    p->isStock = Util_AppData(appData, ARRAYSIZE(appData)) &&
                 SUCCEEDED(StringCchPrintfW(stock, ARRAYSIZE(stock), L"%s\\" STOCK_FOLDER, appData)) && Core_PathEquals(stock, dataDir);
    if (p->isStock && pkg->found && Util_LocalAppData(localAppData, ARRAYSIZE(localAppData)))
        Profiles_ResolveStorage(p, localAppData, pkg->family);
    else
        Profiles_ResolveStorage(p, NULL, NULL);
}

BOOL SessionLink_Busy(const Profile *p)
{
    WCHAR dir[LONG_PATH_CCH], mine[LONG_PATH_CCH], title[MAX_PATH], theirs[LONG_PATH_CCH], theirDir[LONG_PATH_CCH];
    ClaudePackage pkg;
    HWND window = NULL;
    BOOL packageRead = FALSE;
    if (Claude_IsRunning(p)) return TRUE;
    if (!EntriesFolders(p, dir, ARRAYSIZE(dir), mine, ARRAYSIZE(mine), NULL)) return FALSE;
    /* Another running Claude whose entries folder is this one, through a link either way. */
    while ((window = FindWindowExW(HWND_MESSAGE, window, L"Chrome_MessageWindow", NULL)) != NULL) {
        Profile running;
        if (GetWindowTextW(window, title, ARRAYSIZE(title)) <= 0 || Core_PathEquals(title, p->dataDir)) continue;
        if (!packageRead) {
            Claude_FindPackage(&pkg);
            packageRead = TRUE;
        }
        RunningProfile(title, &pkg, &running);
        if (EntriesFolders(&running, theirDir, ARRAYSIZE(theirDir), theirs, ARRAYSIZE(theirs), NULL) && Core_PathEquals(theirs, mine))
            return TRUE;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- copies */

/* backups\<time>\<folder>-sessions in the state folder, made. */
static BOOL BackupDir(const Profile *p, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    SYSTEMTIME now;
    GetLocalTime(&now);
    return Util_StateDir(state, ARRAYSIZE(state)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" BACKUPS_DIR L"\\%04u%02u%02u-%02u%02u%02u\\%s-sessions", state, now.wYear, now.wMonth,
                                      now.wDay, now.wHour, now.wMinute, now.wSecond, p->folder)) &&
           Util_EnsureDir(out);
}

/* ------------------------------------------------------------------ links */

static BOOL MakeJunction(const WCHAR *at, const WCHAR *target, DWORD *error)
{
    MountPoint point;
    WCHAR substitute[LONG_PATH_CCH + 8], extended[LONG_PATH_CCH];
    size_t substituteBytes, printBytes;
    HANDLE dir;
    DWORD returned = 0;
    BOOL ok;
    if (FAILED(StringCchPrintfW(substitute, ARRAYSIZE(substitute), L"\\??\\%s", target))) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    substituteBytes = wcslen(substitute) * sizeof(WCHAR);
    printBytes = wcslen(target) * sizeof(WCHAR);
    if (substituteBytes + printBytes + 2 * sizeof(WCHAR) > sizeof point.paths) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    if (!Util_ExtendedPath(at, extended, ARRAYSIZE(extended)) || (!CreateDirectoryW(extended, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) {
        *error = GetLastError();
        return FALSE;
    }
    ZeroMemory(&point, sizeof point);
    point.tag = IO_REPARSE_TAG_MOUNT_POINT;
    point.substituteOffset = 0;
    point.substituteLength = (WORD)substituteBytes;
    point.printOffset = (WORD)(substituteBytes + sizeof(WCHAR));
    point.printLength = (WORD)printBytes;
    memcpy(point.paths, substitute, substituteBytes);
    memcpy((BYTE *)point.paths + point.printOffset, target, printBytes);
    point.dataLength = (WORD)(4 * sizeof(WORD) + substituteBytes + printBytes + 2 * sizeof(WCHAR));
    dir = CreateFileW(extended, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (dir == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        RemoveDirectoryW(extended);
        return FALSE;
    }
    ok = DeviceIoControl(dir, FSCTL_SET_REPARSE_POINT, &point, (DWORD)(8 + point.dataLength), NULL, 0, &returned, NULL);
    if (!ok) *error = GetLastError();
    CloseHandle(dir);
    if (!ok) RemoveDirectoryW(extended);
    return ok;
}

static BOOL MakeLink(const WCHAR *at, const WCHAR *target, DWORD *error)
{
    WCHAR extended[LONG_PATH_CCH];
    if (!(target[0] == L'\\' && target[1] == L'\\')) return MakeJunction(at, target, error);
    /* A junction cannot lead to a network share: a symbolic link can, in Developer Mode. */
    if (!Util_ExtendedPath(at, extended, ARRAYSIZE(extended))) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    if (CreateSymbolicLinkW(extended, target, SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) return TRUE;
    *error = GetLastError();
    return FALSE;
}

static void Fail(WCHAR *error, size_t cch, const WCHAR *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    StringCchVPrintfW(error, cch, format, arguments);
    va_end(arguments);
}

BOOL SessionLink_Create(HWND owner, const ProfileList *list, int index, const WCHAR *target, WCHAR *error, size_t errorCch)
{
    const Profile *p = &list->items[index];
    WCHAR dir[LONG_PATH_CCH], final[LONG_PATH_CCH], mine[LONG_PATH_CCH], backup[LONG_PATH_CCH];
    const WCHAR *paths[1];
    DWORD code = 0;
    BOOL signedIn = FALSE, linked;
    int i;
    error[0] = 0;
    if (!SessionStore_EntriesDir(p, dir, ARRAYSIZE(dir), &signedIn)) {
        Fail(error, errorCch, signedIn ? TR(L"\x201C%s\x201D has no session folder yet: open a Code session there once, then link it.")
                                       : TR(L"\x201C%s\x201D is not signed in to Claude yet: sign in there first."),
             p->name);
        return FALSE;
    }
    if (SessionLink_Busy(p)) {
        Fail(error, errorCch, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
        return FALSE;
    }
    if (!Util_EnsureDir(target) || !FinalPath(target, final, ARRAYSIZE(final))) {
        Fail(error, errorCch, TR(L"%s could not be opened (error %lu)."), target, GetLastError());
        return FALSE;
    }
    linked = IsLink(dir);
    if (!linked && FinalPath(dir, mine, ARRAYSIZE(mine)) && Core_PathEquals(mine, final)) {
        Fail(error, errorCch, TR(L"This is already the session folder of \x201C%s\x201D."), p->name);
        return FALSE;
    }
    for (i = 0; i < list->count; i++) {
        WCHAR other[LONG_PATH_CCH], otherFinal[LONG_PATH_CCH];
        if (i != index && Claude_IsRunning(&list->items[i]) &&
            EntriesFolders(&list->items[i], other, ARRAYSIZE(other), otherFinal, ARRAYSIZE(otherFinal), NULL) &&
            Core_PathEquals(otherFinal, final)) {
            Fail(error, errorCch, TR(L"Close \x201C%s\x201D first: it uses that session folder."), list->items[i].name);
            return FALSE;
        }
    }
    if (linked) {
        /* Linked elsewhere already: the old link goes, the folder it led to stays. */
        if (!RemoveDirectoryW(dir)) {
            Fail(error, errorCch, TR(L"%s could not be removed (error %lu)."), dir, GetLastError());
            return FALSE;
        }
    } else {
        /* Its entries join the shared folder (the ones there stay), and stay in a backup. */
        if (!BackupDir(p, backup, ARRAYSIZE(backup)) || !Util_CopyTree(dir, backup, FALSE, &code)) {
            Fail(error, errorCch, TR(L"The session entries could not be backed up (error %lu): nothing was changed."), code ? code : GetLastError());
            return FALSE;
        }
        if (!Util_CopyTree(dir, final, FALSE, &code)) {
            Fail(error, errorCch, TR(L"The session entries could not be copied to %s (error %lu). They are kept in %s."), final, code, backup);
            return FALSE;
        }
        paths[0] = dir;
        if (Util_Recycle(owner, paths, 1) != REMOVE_DONE || Util_DirExists(dir)) {
            Fail(error, errorCch, TR(L"%s could not be moved to the Recycle Bin: the profile keeps its own session folder."), dir);
            return FALSE;
        }
    }
    if (!MakeLink(dir, final, &code)) {
        /* Without its folder, Claude would make a new empty one: it gets its entries back. */
        if (!linked) Util_CopyTree(backup, dir, FALSE, &code);
        Fail(error, errorCch, final[0] == L'\\' ? TR(L"The link could not be made (error %lu). A network folder needs Windows' Developer Mode.")
                                                : TR(L"The link could not be made (error %lu)."), code);
        return FALSE;
    }
    Util_Log(L"session folder of %s linked to %s", p->folder, final);
    return TRUE;
}

BOOL SessionLink_Remove(const ProfileList *list, int index, WCHAR *error, size_t errorCch)
{
    const Profile *p = &list->items[index];
    WCHAR dir[LONG_PATH_CCH], final[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    DWORD code = 0;
    BOOL signedIn = FALSE, reachable;
    error[0] = 0;
    if (!SessionStore_EntriesDir(p, dir, ARRAYSIZE(dir), &signedIn) || !IsLink(dir)) {
        Fail(error, errorCch, TR(L"The session folder of \x201C%s\x201D is its own already."), p->name);
        return FALSE;
    }
    if (SessionLink_Busy(p)) {
        Fail(error, errorCch, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
        return FALSE;
    }
    reachable = FinalPath(dir, final, ARRAYSIZE(final));
    if (!Util_ExtendedPath(dir, extended, ARRAYSIZE(extended)) || !RemoveDirectoryW(extended)) {
        Fail(error, errorCch, TR(L"%s could not be removed (error %lu)."), dir, GetLastError());
        return FALSE;
    }
    /* A folder of its own again, with the sessions it listed while linked. */
    if (!Util_EnsureDir(dir) || (reachable && !Util_CopyTree(final, dir, FALSE, &code))) {
        Fail(error, errorCch, TR(L"The sessions could not be copied back from %s (error %lu)."), reachable ? final : dir,
             code ? code : GetLastError());
        return FALSE;
    }
    Util_Log(L"session folder of %s unlinked from %s", p->folder, reachable ? final : L"a folder that is gone");
    return TRUE;
}
