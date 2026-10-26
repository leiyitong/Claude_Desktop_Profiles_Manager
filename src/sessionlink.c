/*
 * A profile's session entries kept in a folder it shares through a link:
 * another profile's, or any folder of the user's, as earlier versions made
 * them. Claude keeps a profile's entries in claude-code-sessions\<account>\
 * <organization> (sessionstore.c), and that folder was a directory junction
 * (or a symbolic link) to the shared one.
 *
 * Claude no longer writes its entries through a link: it refuses an entries
 * folder that is a reparse point ("symlink/file plant", see test_claude.c),
 * so a linked profile reads the shared sessions but every change it makes is
 * lost once it closes. Links are only read and taken away here: unlinking
 * gives the profile a folder of its own again, with the entries the shared
 * one holds then; profiles keep the same sessions through sessionvault.c.
 *
 * Claude keeps its sessions in memory and writes them back, so the entries
 * of a folder some profile shares are written only while every profile that
 * shares it is closed (SessionLink_Busy).
 */
#include "app.h"
#include <stdarg.h>

/* `path` without the \\?\ (or \\?\UNC\) that GetFinalPathNameByHandle puts before it. */
static void PlainPath(WCHAR *path)
{
    if (wcsncmp(path, L"\\\\?\\UNC\\", 8) == 0) {
        memmove(path + 2, path + 8, (wcslen(path + 8) + 1) * sizeof(WCHAR));
    } else if (wcsncmp(path, L"\\\\?\\", 4) == 0) {
        memmove(path, path + 4, (wcslen(path + 4) + 1) * sizeof(WCHAR));
    }
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
    PlainPath(out);
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
 * Chromium message window: the stock folder through its storage, also once
 * Claude moved it out of its package. */
static void RunningProfile(const WCHAR *dataDir, const ClaudePackage *pkg, Profile *p)
{
    WCHAR appData[MAX_PATH], localAppData[MAX_PATH], stock[MAX_PATH], moved[MAX_PATH];
    const WCHAR *leaf = wcsrchr(dataDir, L'\\');
    BOOL local = pkg->found && Util_LocalAppData(localAppData, ARRAYSIZE(localAppData));
    ZeroMemory(p, sizeof *p);
    StringCchCopyW(p->dataDir, ARRAYSIZE(p->dataDir), dataDir);
    StringCchCopyW(p->folder, ARRAYSIZE(p->folder), leaf ? leaf + 1 : dataDir);
    if (Util_AppData(appData, ARRAYSIZE(appData)) && SUCCEEDED(StringCchPrintfW(stock, ARRAYSIZE(stock), L"%s\\" STOCK_FOLDER, appData)) &&
        (Core_PathEquals(stock, dataDir) ||
         (local && Core_MovedStockDir(localAppData, pkg->family, moved, ARRAYSIZE(moved)) && Core_PathEquals(moved, dataDir)))) {
        p->isStock = TRUE;
        StringCchCopyW(p->dataDir, ARRAYSIZE(p->dataDir), stock);
        StringCchCopyW(p->folder, ARRAYSIZE(p->folder), STOCK_FOLDER);
    }
    if (p->isStock && local)
        Profiles_ResolveStorage(p, localAppData, pkg->family);
    else
        Profiles_ResolveStorage(p, NULL, NULL);
}

/* Within a batch of writes (SessionLink_BeginBatch), each profile's entries
 * folder is resolved once: resolving reads its config.json and lists its
 * entries, and SessionLink_Busy is asked before and after every write. */
typedef struct LinkCacheItem {
    WCHAR dataDir[MAX_PATH];
    BOOL  ok;
    WCHAR final[LONG_PATH_CCH];
} LinkCacheItem;
typedef struct LinkCache {
    int           depth, count;
    BOOL          packageRead;
    ClaudePackage pkg;
    LinkCacheItem items[2 * MAX_PROFILES];
} LinkCache;
static __declspec(thread) LinkCache *g_cache;

void SessionLink_BeginBatch(void)
{
    if (g_cache) {
        g_cache->depth++;
        return;
    }
    if ((g_cache = (LinkCache *)Util_Alloc(sizeof *g_cache)) != NULL) {
        ZeroMemory(g_cache, sizeof *g_cache);
        g_cache->depth = 1;
    }
}

void SessionLink_EndBatch(void)
{
    if (g_cache && --g_cache->depth == 0) {
        Util_Free(g_cache);
        g_cache = NULL;
    }
}

/* Where the entries folder of `p` leads (EntriesFolders), from the batch's cache when there is one. */
static BOOL FinalEntries(const Profile *p, WCHAR *final, size_t cch)
{
    WCHAR dir[LONG_PATH_CCH];
    BOOL ok;
    int i;
    for (i = 0; g_cache && i < g_cache->count; i++)
        if (Core_PathEquals(g_cache->items[i].dataDir, p->dataDir))
            return g_cache->items[i].ok && SUCCEEDED(StringCchCopyW(final, cch, g_cache->items[i].final));
    ok = EntriesFolders(p, dir, ARRAYSIZE(dir), final, cch, NULL);
    if (g_cache && g_cache->count < (int)ARRAYSIZE(g_cache->items)) {
        LinkCacheItem *item = &g_cache->items[g_cache->count++];
        StringCchCopyW(item->dataDir, ARRAYSIZE(item->dataDir), p->dataDir);
        item->ok = ok && SUCCEEDED(StringCchCopyW(item->final, ARRAYSIZE(item->final), final));
    }
    return ok;
}

/* A running Chromium's data folder is a Claude profile's: directly in
 * %APPDATA% and named as one (rule 3), or the stock folder Claude moved. */
static BOOL ClaudeFolder(const WCHAR *title, const ClaudePackage *pkg)
{
    WCHAR appData[MAX_PATH], local[MAX_PATH], moved[MAX_PATH];
    const WCHAR *leaf = wcsrchr(title, L'\\');
    if (leaf && Util_AppData(appData, ARRAYSIZE(appData)) && (size_t)(leaf - title) == Core_TrimmedPathLength(appData) &&
        CompareStringOrdinal(title, (int)(leaf - title), appData, (int)(leaf - title), TRUE) == CSTR_EQUAL && (Core_EqualsI(leaf + 1, STOCK_FOLDER) || Core_IsProfileFolder(leaf + 1)))
        return TRUE;
    return pkg->found && Util_LocalAppData(local, ARRAYSIZE(local)) && Core_MovedStockDir(local, pkg->family, moved, ARRAYSIZE(moved)) &&
           Core_PathEquals(title, moved);
}

BOOL SessionLink_Busy(const Profile *p)
{
    WCHAR mine[LONG_PATH_CCH], title[MAX_PATH], theirs[LONG_PATH_CCH];
    ClaudePackage own, *pkg = g_cache ? &g_cache->pkg : &own;
    BOOL *packageRead = g_cache ? &g_cache->packageRead : NULL, read = FALSE;
    HWND window = NULL;
    if (!packageRead) packageRead = &read;
    if (Claude_IsRunning(p)) return TRUE;
    if (!FinalEntries(p, mine, ARRAYSIZE(mine))) return FALSE;
    /* Another running Claude whose entries folder is this one, through a link either way. */
    while ((window = Claude_NextInstanceWindow(window, title, ARRAYSIZE(title))) != NULL) {
        Profile running;
        if (Core_PathEquals(title, p->dataDir)) continue;
        if (!*packageRead) {
            Claude_FindPackage(pkg);
            *packageRead = TRUE;
        }
        if (!ClaudeFolder(title, pkg)) continue;
        RunningProfile(title, pkg, &running);
        if (FinalEntries(&running, theirs, ARRAYSIZE(theirs)) && Core_PathEquals(theirs, mine)) return TRUE;
    }
    return FALSE;
}

static void Fail(WCHAR *error, size_t cch, const WCHAR *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    StringCchVPrintfW(error, cch, format, arguments);
    va_end(arguments);
}

BOOL SessionLink_Remove(const ProfileList *list, int index, WCHAR *error, size_t errorCch)
{
    const Profile *p = &list->items[index];
    WCHAR dir[LONG_PATH_CCH], final[LONG_PATH_CCH], extended[LONG_PATH_CCH], copy[LONG_PATH_CCH], extendedCopy[LONG_PATH_CCH];
    DWORD code = 0, ignored;
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
    /* A folder of its own again, with the sessions it listed while linked:
     * copied beside the link first, which goes only once they all are. */
    if (FAILED(StringCchPrintfW(copy, ARRAYSIZE(copy), L"%s" UTIL_TEMPORARY_SUFFIX, dir)) || !Util_ExtendedPath(dir, extended, ARRAYSIZE(extended)) ||
        !Util_ExtendedPath(copy, extendedCopy, ARRAYSIZE(extendedCopy))) {
        Fail(error, errorCch, TR(L"The path is too long."));
        return FALSE;
    }
    Util_DeleteTree(copy, &ignored);   /* left by a try that stopped */
    if (!Util_EnsureDir(copy) || (reachable && !Util_CopyTree(final, copy, FALSE, &code))) {
        Fail(error, errorCch, TR(L"The sessions could not be copied back from %s (error %lu)."), reachable ? final : dir,
             code ? code : GetLastError());
        Util_DeleteTree(copy, &ignored);
        return FALSE;
    }
    if (!RemoveDirectoryW(extended)) {
        Fail(error, errorCch, TR(L"%s could not be removed (error %lu)."), dir, GetLastError());
        Util_DeleteTree(copy, &ignored);
        return FALSE;
    }
    if (!MoveFileExW(extendedCopy, extended, 0)) {
        code = GetLastError();
        Fail(error, errorCch, TR(L"The sessions are in %s, which could not be moved to %s (error %lu)."), copy, dir, code);
        Util_Log(L"session folder of %s: the link is gone, the sessions are in %s (error %lu)", p->folder, copy, code);
        return FALSE;
    }
    Util_Log(L"session folder of %s unlinked from %s", p->folder, reachable ? final : L"a folder that is gone");
    return TRUE;
}
