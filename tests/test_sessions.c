/*
 * Sessions on private profiles, transcripts and Claude Code folders in a
 * temporary fixture: where a profile's sessions are stored and how they are
 * read (sessionstore.c); copies, changes waiting for a running profile,
 * removals and Delete session everywhere, with a stand-in for the Recycle
 * Bin (sessionedit.c); sessions merged, shared to a running profile, moved,
 * exported and imported (sessionsync.c); sessions kept the same in groups,
 * with the sidebar's pins and groups, sessions without a folder and sessions
 * gone on with apart (sessionvault.c); paths past MAX_PATH; reads
 * cancelled or made at once; the sessions view's background snapshots
 * (sessions.c). Built and run by build.cmd; exits non-zero when a check fails.
 */
#include "../src/app.h"
#include <wincrypt.h>
#include "../src/resource.h"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define TRANSCRIPT_READ_MAX (1024u * 1024u)

static int g_checks, g_failures;
static WCHAR g_root[MAX_PATH];
static DWORD g_fixtureTag;
static BOOL FileHas(const WCHAR *path, const char *text);
static const WCHAR *const g_family = L"Fixture_package";
static const WCHAR *const g_scratchId = L"11111111-1111-4111-8111-111111111111";
static const WCHAR *const g_projectId = L"22222222-2222-4222-8222-222222222222";
static const WCHAR *const g_oldId = L"33333333-3333-4333-8333-333333333333";
static const WCHAR *const g_lineageId = L"aaaaaaaa-0000-4000-8000-000000000001";
static const WCHAR *const g_priorId = L"aaaaaaaa-0000-4000-8000-000000000002";
static const WCHAR *const g_preClearId = L"aaaaaaaa-0000-4000-8000-000000000003";
static const WCHAR *const g_claimedId = L"aaaaaaaa-0000-4000-8000-000000000004";
static const WCHAR *const g_duplicateId = L"aaaaaaaa-0000-4000-8000-000000000005";
static const WCHAR *const g_longId = L"bbbbbbbb-0000-4000-8000-000000000001";
static const WCHAR *const g_editIds[3] = {
    L"cccccccc-0000-4000-8000-000000000001", L"cccccccc-0000-4000-8000-000000000002", L"cccccccc-0000-4000-8000-000000000003"
};
static const WCHAR *const g_unlistedId = L"dddddddd-0000-4000-8000-000000000001";
static const WCHAR *const g_accountId = L"eeeeeeee-0000-4000-8000-000000000001";
static const WCHAR *const g_organizationId = L"eeeeeeee-0000-4000-8000-000000000002";
static const WCHAR *const g_deletedId = L"ffffffff-0000-4000-8000-000000000001";
static const WCHAR *const g_startedId = L"ffffffff-0000-4000-8000-000000000002";
static const WCHAR *const g_neighborId = L"ffffffff-0000-4000-8000-000000000003";

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
}

static BOOL Join(const WCHAR *dir, const WCHAR *leaf, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", dir, leaf));
}

/* `text` written as `path`, whole. */
static BOOL WriteWholeFile(const WCHAR *path, const char *text)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0;
    BOOL ok = file != INVALID_HANDLE_VALUE && WriteFile(file, text, (DWORD)strlen(text), &done, NULL) && done == strlen(text);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

static BOOL FixturePath(const WCHAR *leaf, WCHAR *out, size_t cch)
{
    return Join(g_root, leaf, out, cch);
}

/* Inside the fixture root, with no way out of it. */
static BOOL InFixture(const WCHAR *path)
{
    return Core_PathEquals(path, g_root) || (Core_PathUnder(path, g_root) && !wcsstr(path, L"\\..") && !wcschr(path, L'/'));
}

/* There as a folder (or, without `folder`, as a file), at any path length. */
static BOOL PathThere(const WCHAR *path, BOOL folder)
{
    WCHAR extended[LONG_PATH_CCH];
    DWORD attributes;
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return FALSE;
    attributes = GetFileAttributesW(extended);
    return attributes != INVALID_FILE_ATTRIBUTES && ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == folder;
}

static BOOL FileThere(const WCHAR *path)
{
    return PathThere(path, FALSE);
}

static BOOL DirThere(const WCHAR *path)
{
    return PathThere(path, TRUE);
}

/* Every missing folder of `path`, inside the fixture root, at any length. */
static BOOL MakeDir(const WCHAR *path)
{
    WCHAR partial[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    size_t length, i;
    if (!InFixture(path) || FAILED(StringCchCopyW(partial, ARRAYSIZE(partial), path))) return FALSE;
    length = wcslen(partial);
    for (i = wcslen(g_root); i <= length; i++) {
        if (i < length && partial[i] != L'\\') continue;
        partial[i] = 0;
        if (!Util_ExtendedPath(partial, extended, ARRAYSIZE(extended)) ||
            (!CreateDirectoryW(extended, NULL) && GetLastError() != ERROR_ALREADY_EXISTS))
            return FALSE;
        if (i < length) partial[i] = L'\\';
    }
    return DirThere(path);
}

static BOOL Save(const WCHAR *path, const char *text)
{
    WCHAR parent[LONG_PATH_CCH], extended[LONG_PATH_CCH], *slash;
    HANDLE file;
    DWORD bytes = (DWORD)strlen(text), written = 0;
    BOOL ok;
    if (!InFixture(path) || FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), path))) return FALSE;
    slash = wcsrchr(parent, L'\\');
    if (!slash) return FALSE;
    *slash = 0;
    if (!MakeDir(parent) || !Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return FALSE;
    file = CreateFileW(extended, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, text, bytes, &written, NULL) && written == bytes;
    CloseHandle(file);
    return ok;
}

static BOOL SaveIn(const WCHAR *dir, const WCHAR *name, const char *text)
{
    WCHAR path[LONG_PATH_CCH];
    return Join(dir, name, path, ARRAYSIZE(path)) && Save(path, text);
}

/* Member `key` of the JSON file `path`, as Core_JsonMember gives it, to `check`. */
static BOOL CheckMember(const WCHAR *path, const char *key, BOOL (*check)(const char *value, size_t length, void *context), void *context)
{
    DWORD length;
    size_t valueLength;
    const char *value;
    char *json = Util_ReadFile(path, 65536, FALSE, &length);
    BOOL ok = FALSE;
    if (json) {
        ok = Core_JsonMember(json, length, key, &value, &valueLength) && check(value, valueLength, context);
        HeapFree(GetProcessHeap(), 0, json);
    }
    return ok;
}

typedef struct StringOut {
    WCHAR *text;
    size_t cch;
} StringOut;

static BOOL DecodeString(const char *value, size_t length, void *context)
{
    StringOut *out = (StringOut *)context;
    return Core_JsonString(value, length, out->text, out->cch);
}

static BOOL IsTrue(const char *value, size_t length, void *context)
{
    (void)context;
    return Core_JsonTrue(value, length);
}

static BOOL IsAny(const char *value, size_t length, void *context)
{
    (void)value;
    (void)length;
    (void)context;
    return TRUE;
}

static BOOL ReadString(const WCHAR *path, const char *key, WCHAR *out, size_t cch)
{
    StringOut string;
    string.text = out;
    string.cch = cch;
    out[0] = 0;
    return CheckMember(path, key, DecodeString, &string);
}

static BOOL ReadTrue(const WCHAR *path, const char *key)
{
    return CheckMember(path, key, IsTrue, NULL);
}

static BOOL HasMember(const WCHAR *path, const char *key)
{
    return CheckMember(path, key, IsAny, NULL);
}

/* Refuse reparse points and paths outside the unique fixture root. */
static BOOL RemoveFixture(const WCHAR *path)
{
    WCHAR extended[LONG_PATH_CCH], child[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    DWORD attributes;
    HANDLE find;
    BOOL ok = TRUE;
    if (!InFixture(path) || !Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return FALSE;
    attributes = GetFileAttributesW(extended);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return FALSE;
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(extended, attributes & ~FILE_ATTRIBUTE_READONLY);
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(extended);
    find = Util_FindFiles(path, L"*", &found, FALSE);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            if (!Join(path, found.cFileName, child, ARRAYSIZE(child)) || !RemoveFixture(child)) ok = FALSE;
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return ok && RemoveDirectoryW(extended);
}

/* In the temporary folder, by its long names: Claude Code's temporary
 * folder is found by them (the temporary folder can come in 8.3 names). */
static BOOL CreateRoot(void)
{
    WCHAR temp[MAX_PATH], path[MAX_PATH], full[MAX_PATH];
    GUID id;
    DWORD n = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!n || n >= ARRAYSIZE(temp) || FAILED(CoCreateGuid(&id)) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%scdm-session-%08lx-%08lx", temp,
                                GetCurrentProcessId(), id.Data1))) return FALSE;
    n = GetFullPathNameW(path, ARRAYSIZE(full), full, NULL);
    g_fixtureTag = id.Data1;
    if (n == 0 || n >= ARRAYSIZE(full) || !CreateDirectoryW(full, NULL)) return FALSE;
    n = GetLongPathNameW(full, g_root, ARRAYSIZE(g_root));
    return n > 0 && n < ARRAYSIZE(g_root);
}

/* A fake Claude of `profile`: the window its running Claude has. */
static HWND StartFakeClaude(const Profile *profile)
{
    WNDCLASSW cls;
    ZeroMemory(&cls, sizeof cls);
    cls.hInstance = GetModuleHandleW(NULL);
    cls.lpfnWndProc = DefWindowProcW;
    cls.lpszClassName = L"Chrome_MessageWindow";
    RegisterClassW(&cls);
    return CreateWindowExW(0, cls.lpszClassName, profile->dataDir, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, cls.hInstance, NULL);
}

static void StopFakeClaude(HWND window)
{
    if (!window) return;
    DestroyWindow(window);
    UnregisterClassW(L"Chrome_MessageWindow", GetModuleHandleW(NULL));
}

/* -------------------------------------------- sessionedit.c, in the fixture */

#define RECYCLED_MAX 16

/* What the Recycle Bin is asked to take: never the real one. Every path must
 * be in the fixture; those taken (`result` REMOVE_DONE) are deleted, as the
 * bin would move them away. With `startsClaude`, that profile's Claude
 * starts while Windows asks. */
static struct {
    RemoveResult  result;
    const Profile *startsClaude;
    HWND          started;
    BOOL          escaped;
    int           calls, count;
    WCHAR         paths[RECYCLED_MAX][LONG_PATH_CCH];
} g_recycle;

static void StopStartedClaude(void)
{
    StopFakeClaude(g_recycle.started);
    g_recycle.started = NULL;
    g_recycle.startsClaude = NULL;
}

/* What the next calls do; whether a path ever escaped the fixture is kept. */
static void RecycleWill(RemoveResult result, const Profile *startsClaude)
{
    BOOL escaped = g_recycle.escaped;
    StopStartedClaude();
    ZeroMemory(&g_recycle, sizeof g_recycle);
    g_recycle.escaped = escaped;
    g_recycle.result = result;
    g_recycle.startsClaude = startsClaude;
}

static RemoveResult FixtureRecycle(HWND owner, const WCHAR *const *paths, int count)
{
    BOOL escaped = FALSE;
    int i;
    (void)owner;
    g_recycle.calls++;
    for (i = 0; i < count; i++) {
        if (!InFixture(paths[i])) escaped = TRUE;
        if (g_recycle.count < RECYCLED_MAX) StringCchCopyW(g_recycle.paths[g_recycle.count++], LONG_PATH_CCH, paths[i]);
    }
    if (escaped) {
        g_recycle.escaped = TRUE;
        return REMOVE_FAILED;
    }
    for (i = 0; i < count; i++)
        if (!Util_FitsRecycleBin(paths[i])) return REMOVE_FAILED;
    if (g_recycle.startsClaude && !g_recycle.started) g_recycle.started = StartFakeClaude(g_recycle.startsClaude);
    if (g_recycle.result == REMOVE_DONE)
        for (i = 0; i < count; i++)
            if (!RemoveFixture(paths[i])) return REMOVE_FAILED;
    return g_recycle.result;
}

static BOOL Recycled(const WCHAR *path)
{
    int i;
    for (i = 0; i < g_recycle.count; i++)
        if (Core_PathEquals(g_recycle.paths[i], path)) return TRUE;
    return FALSE;
}

/* With `g_cancelCopies`, a folder copy is cancelled in Windows' progress
 * once it has copied everything. */
static BOOL g_cancelCopies;

static int FixtureFileOperation(LPSHFILEOPSTRUCTW operation)
{
    int code = SHFileOperationW(operation);
    if (!g_cancelCopies || operation->wFunc != FO_COPY || code != 0) return code;
    operation->fAnyOperationsAborted = TRUE;
    return ERROR_CANCELLED;
}

#define SessionEdit_Open                 TestedSessionEdit_Open
#define SessionEdit_Change               TestedSessionEdit_Change
#define SessionEdit_Cancel               TestedSessionEdit_Cancel
#define SessionEdit_Forget               TestedSessionEdit_Forget
#define SessionEdit_ApplyPending         TestedSessionEdit_ApplyPending
#define SessionEdit_ApplyPendingAfterRun TestedSessionEdit_ApplyPendingAfterRun
#define SessionEdit_ApplyPendingFor      TestedSessionEdit_ApplyPendingFor
#define SessionEdit_CopyConversation     TestedSessionEdit_CopyConversation
#define SessionEdit_RemoveCopy           TestedSessionEdit_RemoveCopy
#define SessionEdit_CanDelete            TestedSessionEdit_CanDelete
#define SessionEdit_RemovesWorkingFolder TestedSessionEdit_RemovesWorkingFolder
#define SessionEdit_ListFiles            TestedSessionEdit_ListFiles
#define SessionEdit_DeleteEverywhere     TestedSessionEdit_DeleteEverywhere
#define SessionEdit_ListConversation     TestedSessionEdit_ListConversation
#define SessionEdit_CopiedCwd            TestedSessionEdit_CopiedCwd
#define SessionEdit_ListTranscriptFiles  TestedSessionEdit_ListTranscriptFiles
#define SessionEdit_TemporaryDir         TestedSessionEdit_TemporaryDir
#define SessionEdit_Lock                 TestedSessionEdit_Lock
#define SessionEdit_Unlock               TestedSessionEdit_Unlock
#define Util_Recycle                     FixtureRecycle
#define SHFileOperationW                 FixtureFileOperation
#include "../src/sessionedit.c"
#undef Util_Recycle
#undef SHFileOperationW

/* Backup operations use the same private fixtures, without the file chooser. */
#define Backup_Create        TestedBackup_Create
#define Backup_Restore       TestedBackup_Restore
#define Backup_ReadAppKey    TestedBackup_ReadAppKey
#define Backup_WriteAppKey   TestedBackup_WriteAppKey
#define Backup_FillPartsList TestedBackup_FillPartsList
#define Backup_ValidateArchive TestedBackup_ValidateArchive
#define Backup_CheckArchive TestedBackup_CheckArchive
#include "../src/backup.c"

static DWORD g_searchReads;
static char *ReadSearchFixture(const WCHAR *path, DWORD maximum, BOOL tail, DWORD *length)
{
    g_searchReads++;
    return Util_ReadFile(path, maximum, tail, length);
}
#define SessionSearch_Locate     TestedSessionSearch_Locate
#define SessionSearch_Start      TestedSessionSearch_Start
#define SessionSearch_Stop       TestedSessionSearch_Stop
#define SessionSearch_Invalidate TestedSessionSearch_Invalidate
#define Util_ReadFile            ReadSearchFixture
#include "../src/sessionsearch.c"
#undef Util_ReadFile

/* ------------------------------------------------------------------ fixtures */

/* An empty DACL denies enumeration; the owner can always restore it. */
static BOOL DenyDirectory(const WCHAR *dir, BOOL deny)
{
    SECURITY_DESCRIPTOR descriptor;
    ACL acl;
    return InFixture(dir) && InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
           InitializeAcl(&acl, sizeof acl, ACL_REVISION) &&
           SetSecurityDescriptorDacl(&descriptor, TRUE, deny ? &acl : NULL, FALSE) &&
           SetFileSecurityW(dir, DACL_SECURITY_INFORMATION, &descriptor);
}

static BOOL SetProfile(Profile *p, const WCHAR *label, const WCHAR *leaf, BOOL stock)
{
    ZeroMemory(p, sizeof *p);
    p->isStock = stock;
    return SUCCEEDED(StringCchPrintfW(p->folder, ARRAYSIZE(p->folder), L"Fixture-%08lx-%08lx-%s", GetCurrentProcessId(), g_fixtureTag, label)) &&
           SUCCEEDED(StringCchCopyW(p->name, ARRAYSIZE(p->name), label)) &&
           FixturePath(leaf, p->dataDir, ARRAYSIZE(p->dataDir));
}

/* A profile of its own signed in to `account`, whose entries go in `entries`. */
static BOOL PrepareProfile(Profile *p, const WCHAR *label, const WCHAR *leaf, const WCHAR *account, const WCHAR *organization,
                           WCHAR *entries, size_t cch)
{
    char config[256];
    WCHAR sub[MAX_PATH];
    return SetProfile(p, label, leaf, FALSE) && Profiles_ResolveStorage(p, NULL, NULL) &&
           SUCCEEDED(StringCchPrintfA(config, sizeof config, "{\"lastKnownAccountUuid\":\"%ls\"}", account)) &&
           SaveIn(p->storageDir, L"config.json", config) &&
           SUCCEEDED(StringCchPrintfW(sub, ARRAYSIZE(sub), L"claude-code-sessions\\%s\\%s", account, organization)) &&
           Join(p->storageDir, sub, entries, cch) && MakeDir(entries);
}

/* An entry of session `id` (its own id local_<id>) working in `cwd`, with
 * `extra` JSON members. */
static BOOL WriteEntryIn(const WCHAR *entries, const WCHAR *id, const WCHAR *cwd, const char *extra)
{
    WCHAR file[LONG_PATH_CCH];
    char quoted[MAX_PATH * 6 + 4], json[8192];
    return Core_JsonQuote(cwd, quoted, sizeof quoted) &&
           SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,"
                                                         "\"title\":\"Fixture session\",\"lastActivityAt\":123%s}", id, id, quoted, extra)) &&
           SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, id)) && Save(file, json);
}

static BOOL WriteEntry(const WCHAR *entries, const WCHAR *id, const char *extra)
{
    return WriteEntryIn(entries, id, L"C:\\Fixture", extra);
}

static BOOL EntryPath(const WCHAR *entries, const WCHAR *id, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\local_%s.json", entries, id));
}

/* Claude Code's private folder of the fixture: the one holding `projects`. */
static BOOL CodeDir(const WCHAR *projects, WCHAR *out, size_t cch)
{
    WCHAR *slash;
    if (FAILED(StringCchCopyW(out, cch, projects)) || (slash = wcsrchr(out, L'\\')) == NULL) return FALSE;
    *slash = 0;
    return TRUE;
}

/* A session's entry, and its transcript as Claude Code writes it: one line
 * per message (some without a session or a folder), the working folder
 * becoming a subfolder after a cd, and a last line still being written. */
static BOOL WriteSession(const WCHAR *entries, const WCHAR *projects, const WCHAR *id, const WCHAR *cwd)
{
    WCHAR file[LONG_PATH_CCH], subfolder[MAX_PATH];
    char quoted[MAX_PATH * 6 + 4], quotedSubfolder[MAX_PATH * 6 + 4], json[8192];
    if (!Core_JsonQuote(cwd, quoted, sizeof quoted) || !Join(cwd, L"sub", subfolder, ARRAYSIZE(subfolder)) ||
        !Core_JsonQuote(subfolder, quotedSubfolder, sizeof quotedSubfolder) ||
        FAILED(StringCchPrintfA(json, sizeof json,
            "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,"
            "\"title\":\"Fixture session\",\"lastActivityAt\":123}\n", id, id, quoted)) ||
        FAILED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, id)) || !Save(file, json)) return FALSE;
    return SUCCEEDED(StringCchPrintfA(json, sizeof json,
               "{\"sessionId\":\"%ls\",\"cwd\":%s,\"type\":\"user\"}\n"
               "{\"type\":\"summary\",\"summary\":\"Fixture\"}\n"
               "{\"parentUuid\":null,\"cwd\":%s,\"sessionId\":\"%ls\",\"type\":\"assistant\"}\n"
               "{\"cwd\":%s,\"sessionId\":\"%ls\",\"type\":\"user\"}\n"
               "{\"sessionId\":\"%ls\",\"cwd\":%s,\"partial\":",
               id, quoted, quoted, id, quotedSubfolder, id, id, quoted)) &&
           SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\fixture\\%s.jsonl", projects, id)) && Save(file, json);
}

static int CountText(const char *text, const char *part)
{
    int count = 0;
    size_t length = strlen(part);
    for (; (text = strstr(text, part)) != NULL; text += length) count++;
    return count;
}

/* The copy at `path` of a WriteSession transcript: each of its whole lines
 * with the new id and the working folder `copyCwd` (its subfolder too), and
 * not the line that was still being written. */
static void CheckCopiedLines(const char *what, const WCHAR *path, const WCHAR *oldId, const WCHAR *newId,
                             const WCHAR *oldCwd, const WCHAR *copyCwd)
{
    char label[256], member[128], quoted[MAX_PATH * 6 + 4], cwdMember[MAX_PATH * 6 + 16], *text;
    DWORD length = 0;
    BOOL moved = !Core_PathEquals(oldCwd, copyCwd);
    text = Util_ReadFile(path, TRANSCRIPT_READ_MAX, FALSE, &length);
    StringCchPrintfA(label, sizeof label, "%s: the copied transcript can be read", what);
    Check(label, text != NULL);
    if (!text) return;
    StringCchPrintfA(member, sizeof member, "\"sessionId\":\"%ls\"", oldId);
    StringCchPrintfA(label, sizeof label, "%s: no line keeps the original's id", what);
    Check(label, CountText(text, member) == 0);
    StringCchPrintfA(member, sizeof member, "\"sessionId\":\"%ls\"", newId);
    StringCchPrintfA(label, sizeof label, "%s: every whole line has the copy's id", what);
    Check(label, CountText(text, member) == 3);
    StringCchPrintfA(label, sizeof label, "%s: the copy ends after the last whole line", what);
    Check(label, length > 0 && text[length - 1] == '\n' && !strstr(text, "\"partial\""));
    Core_JsonQuote(copyCwd, quoted, sizeof quoted);
    StringCchPrintfA(cwdMember, sizeof cwdMember, "\"cwd\":%s", quoted);
    StringCchPrintfA(label, sizeof label, "%s: every line in the working folder names the copy's", what);
    Check(label, CountText(text, cwdMember) == 2);
    StringCchPrintfA(cwdMember, sizeof cwdMember, "\"cwd\":%.*s\\\\sub\"", (int)strlen(quoted) - 1, quoted);
    StringCchPrintfA(label, sizeof label, "%s: a line in a subfolder names the copy's subfolder", what);
    Check(label, CountText(text, cwdMember) == 1);
    if (moved) {
        Core_JsonQuote(oldCwd, quoted, sizeof quoted);
        StringCchPrintfA(cwdMember, sizeof cwdMember, "\"cwd\":%.*s", (int)strlen(quoted) - 1, quoted);
        StringCchPrintfA(label, sizeof label, "%s: no line keeps the original's working folder", what);
        Check(label, CountText(text, cwdMember) == 0);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

static BOOL PrepareProfiles(ProfileList *profiles, WCHAR *local, WCHAR *projects)
{
    WCHAR physical[MAX_PATH], stockEntries[MAX_PATH], oldEntries[MAX_PATH], secondaryEntries[MAX_PATH];
    WCHAR scratch[MAX_PATH], physicalScratch[MAX_PATH], project[MAX_PATH], code[MAX_PATH];
    Profile *stock = &profiles->items[0], *secondary = &profiles->items[1];
    ZeroMemory(profiles, sizeof *profiles);
    profiles->count = 2;
    if (!SetProfile(stock, L"Stock", L"R\\Claude", TRUE) || !SetProfile(secondary, L"Secondary", L"R\\Claude-Secondary", FALSE) ||
        !FixturePath(L"L", local, MAX_PATH) || !FixturePath(L"code", code, ARRAYSIZE(code)) ||
        !Join(code, L"projects", projects, MAX_PATH) || !MakeDir(secondary->dataDir) ||
        FAILED(StringCchPrintfW(physical, ARRAYSIZE(physical), L"%s\\Packages\\%s\\LocalCache\\Roaming\\Claude", local, g_family)) ||
        !MakeDir(physical)) return FALSE;
    Check("stock absent: virtual storage resolves", Profiles_ResolveStorage(stock, local, g_family));
    Check("stock absent: storage is LocalCache", Core_PathEquals(stock->storageDir, physical));
    Check("stock absent: logical path is preserved", Core_EndsWithI(stock->dataDir, L"\\R\\Claude"));
    Check("stock absent: resolution does not create logical directory", !Util_DirExists(stock->dataDir));
    Check("secondary: storage resolves", Profiles_ResolveStorage(secondary, local, g_family));
    Check("secondary: storage is its real profile directory", Core_PathEquals(secondary->storageDir, secondary->dataDir));
    if (!Core_PathEquals(stock->storageDir, physical) || !Core_PathEquals(secondary->storageDir, secondary->dataDir)) return FALSE;
    if (!Join(physical, L"claude-code-sessions\\account-a\\org-a", stockEntries, ARRAYSIZE(stockEntries)) ||
        !Join(physical, L"claude-code-sessions\\account-old\\org-old", oldEntries, ARRAYSIZE(oldEntries)) ||
        !Join(secondary->storageDir, L"claude-code-sessions\\account-b\\org-b", secondaryEntries, ARRAYSIZE(secondaryEntries)) ||
        !MakeDir(secondaryEntries) ||
        !Join(stock->dataDir, L"scratch-workspaces\\account-a\\org-a\\scratch-original", scratch, ARRAYSIZE(scratch)) ||
        !Join(physical, L"scratch-workspaces\\account-a\\org-a\\scratch-original", physicalScratch, ARRAYSIZE(physicalScratch)) ||
        !FixturePath(L"workspace", project, ARRAYSIZE(project)) || !MakeDir(project) ||
        !SaveIn(physicalScratch, L"sentinel.txt", "scratch fixture\n") ||
        !SaveIn(physical, L"config.json", "{\"lastKnownAccountUuid\":\"account-a\",\"windowSizeWasSignedIn\":true,"
            "\"locale\":\"en-US\",\"userThemeMode\":\"dark\",\"accessToken\":\"must-not-copy\"}") ||
        !SaveIn(physical, L"claude_desktop_config.json", "{\"mcpServers\":{\"fixture\":{\"command\":\"fixture-command\"}},"
            "\"isHardwareAccelerationDisabled\":true,\"preferences\":{\"menuBarEnabled\":false,\"privateValue\":\"must-not-copy\"},"
            "\"credentials\":\"must-not-copy\"}") ||
        !SaveIn(secondary->storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"account-b\"}") ||
        !WriteSession(stockEntries, projects, g_scratchId, scratch) || !WriteSession(stockEntries, projects, g_projectId, project) ||
        !WriteSession(oldEntries, projects, g_oldId, project)) return FALSE;
    return SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", code);
}

/* Claude moved the stock folder out of its package, to %LOCALAPPDATA%\
 * Claude-Data\<family in lower case>: the profile is there from then on, and
 * its folder as it was named stays its former one. */
static void TestMovedStock(void)
{
    Profile p;
    WCHAR local[MAX_PATH], moved[MAX_PATH], roaming[MAX_PATH], logical[MAX_PATH], packaged[MAX_PATH];
    BOOL ready = SetProfile(&p, L"Moved", L"MR\\Claude", TRUE) && FixturePath(L"ML", local, ARRAYSIZE(local)) &&
                 FixturePath(L"MR", roaming, ARRAYSIZE(roaming)) && MakeDir(roaming) &&
                 SUCCEEDED(StringCchCopyW(logical, ARRAYSIZE(logical), p.dataDir)) &&
                 SUCCEEDED(StringCchPrintfW(moved, ARRAYSIZE(moved), L"%s\\Claude-Data\\fixture_package", local)) && MakeDir(moved) &&
                 SUCCEEDED(StringCchPrintfW(packaged, ARRAYSIZE(packaged), L"%s\\Packages\\%s\\LocalCache\\Roaming\\Claude", local, g_family));
    Check("moved stock fixture", ready);
    if (!ready) return;
    Check("moved stock: storage resolves", Profiles_ResolveStorage(&p, local, g_family));
    Check("moved stock: storage is the moved folder", Core_PathEquals(p.storageDir, moved));
    Check("moved stock: Claude names it there, as its window's title does", Core_PathEquals(p.dataDir, moved));
    Check("moved stock: the folder as it was is its former one", Core_PathEquals(p.formerDir, logical));
    Check("moved stock: resolved again, the same", Profiles_ResolveStorage(&p, local, g_family) && Core_PathEquals(p.storageDir, moved) &&
          Core_PathEquals(p.dataDir, moved) && Core_PathEquals(p.formerDir, logical));
    Check("moved stock: wins over a real %APPDATA%\\Claude", MakeDir(logical) && Profiles_ResolveStorage(&p, local, g_family) &&
          Core_PathEquals(p.storageDir, moved));
    Check("moved stock: a secondary profile never moves", SetProfile(&p, L"MovedSecondary", L"MR\\Claude-Secondary", FALSE) &&
          Profiles_ResolveStorage(&p, local, g_family) && Core_PathEquals(p.storageDir, p.dataDir) && !p.formerDir[0]);
    Check("moved stock: gone, the package's copy again", SetProfile(&p, L"Moved", L"MR\\Claude", TRUE) &&
          Profiles_ResolveStorage(&p, local, g_family) && RemoveDirectoryW(moved) && RemoveDirectoryW(logical) &&
          Profiles_ResolveStorage(&p, local, g_family) && Core_PathEquals(p.storageDir, packaged) &&
          Core_PathEquals(p.dataDir, logical) && !p.formerDir[0]);
}

static void TestResolution(const WCHAR *local)
{
    Profile p;
    WCHAR expected[MAX_PATH], mapped[MAX_PATH], ancestor[MAX_PATH], watched[MAX_PATH];
    Check("real stock fixture", SetProfile(&p, L"Real", L"real\\Claude", TRUE) && MakeDir(p.dataDir));
    Check("real stock: storage resolves", Profiles_ResolveStorage(&p, local, g_family));
    Check("real stock: real directory wins over LocalCache", Core_PathEquals(p.storageDir, p.dataDir));
    Check("missing secondary fixture", SetProfile(&p, L"Missing", L"R\\Claude-Missing", FALSE));
    Check("missing secondary: storage resolves", Profiles_ResolveStorage(&p, local, g_family));
    Check("missing secondary: no stock fallback", Core_PathEquals(p.storageDir, p.dataDir));
    Check("missing secondary: no directory created", !Util_DirExists(p.dataDir));
    Check("path mapping: boundary fixture", Join(p.dataDir, L"config.json", expected, ARRAYSIZE(expected)));
    Check("secondary: file path unchanged", Core_ProfileFilePath(&p, expected, mapped, ARRAYSIZE(mapped)) &&
          Core_PathEquals(mapped, expected));
    Check("watch fixture", SetProfile(&p, L"Watch", L"R\\Claude-Watch", FALSE) &&
          Profiles_ResolveStorage(&p, local, g_family) && FixturePath(L"R", ancestor, ARRAYSIZE(ancestor)));
    Check("watch: missing storage uses existing ancestor", SessionStore_WatchDir(&p, watched, ARRAYSIZE(watched)) &&
          Core_PathEquals(watched, ancestor));
    Check("watch: lookup creates no profile directory", !Util_DirExists(p.dataDir));
    Check("watch: storage fixture created", MakeDir(p.storageDir));
    Check("watch: existing storage is watched", SessionStore_WatchDir(&p, watched, ARRAYSIZE(watched)) &&
          Core_PathEquals(watched, p.storageDir));
    Check("watch: sessions fixture created", Join(p.storageDir, L"claude-code-sessions", expected, ARRAYSIZE(expected)) && MakeDir(expected));
    Check("watch: existing sessions directory is watched", SessionStore_WatchDir(&p, watched, ARRAYSIZE(watched)) &&
          Core_PathEquals(watched, expected));
    TestMovedStock();
}

static int FindRow(const SessionSet *s, const WCHAR *id)
{
    int r;
    for (r = 0; r < s->rowCount; r++)
        if (wcscmp(s->rows[r].key, id) == 0) return r;
    return -1;
}

static int ProfileRows(const SessionSet *s, int profile)
{
    int r, n = 0;
    for (r = 0; r < s->rowCount; r++)
        if (s->rows[r].entry[profile] >= 0) n++;
    return n;
}

static BOOL FindCopiedTranscript(const WCHAR *projects, const WCHAR *id, WCHAR *out, size_t cch)
{
    WIN32_FIND_DATAW project;
    HANDLE find;
    BOOL found = FALSE;
    find = Util_FindFiles(projects, L"*", &project, TRUE);
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!(project.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || project.cFileName[0] == L'.') continue;
        if (SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s\\%s.jsonl", projects, project.cFileName, id)) && FileThere(out)) {
            found = TRUE;
            break;
        }
    } while (FindNextFileW(find, &project));
    FindClose(find);
    return found;
}

/* Whether the paths of a deletion list hold `path`. */
static BOOL Listed(WCHAR (*paths)[LONG_PATH_CCH], int count, const WCHAR *path)
{
    int i;
    for (i = 0; i < count; i++)
        if (Core_PathEquals(paths[i], path)) return TRUE;
    return FALSE;
}

static BOOL CheckCopy(SessionSet *s, int row, int target, const WCHAR *projects, WCHAR *id, WCHAR *path, WCHAR *cwd)
{
    WCHAR error[512], physical[MAX_PATH], marker[MAX_PATH];
    WCHAR projectName[CORE_PROJECT_NAME_MAX + 1], expected[LONG_PATH_CCH];
    BOOL ok = SessionEdit_CopyConversation(NULL, s, row, target, id, SESSION_ID_CCH, error, ARRAYSIZE(error)) == COPY_MADE;
    Check("scratch copy: completes", ok);
    if (!ok) {
        wprintf(L"        %s\n", error);
        return FALSE;
    }
    Check("scratch copy: distinct conversation id", Core_IsUuid(id) && wcscmp(id, s->rows[row].key) != 0);
    ok = FindCopiedTranscript(projects, id, path, LONG_PATH_CCH);
    Check("scratch copy: transcript exists", ok);
    if (!ok) return FALSE;
    ok = ReadString(path, "cwd", cwd, MAX_PATH);
    Check("scratch copy: transcript has working directory", ok);
    if (!ok) return FALSE;
    Check("scratch copy: transcript uses target logical path", Core_PathUnder(cwd, s->source[target].scratchDir));
    CheckCopiedLines("scratch copy", path, s->rows[row].key, id, s->rows[row].cwd, cwd);
    if (Core_ProjectDirName(cwd, projectName, ARRAYSIZE(projectName)) &&
        SUCCEEDED(StringCchPrintfW(expected, ARRAYSIZE(expected), L"%s\\%s\\%s.jsonl", projects, projectName, id))) {
        Check("scratch copy: transcript filed in its working folder's project folder", Core_PathEquals(expected, path));
    } else {
        WCHAR *slash;
        StringCchCopyW(expected, ARRAYSIZE(expected), s->rows[row].transcriptPath);
        if ((slash = wcsrchr(expected, L'\\')) != NULL) *slash = 0;
        Check("scratch copy: transcript too long for its project folder stays beside the original",
              Core_PathUnder(path, expected) && wcschr(path + wcslen(expected) + 1, L'\\') == NULL);
    }
    Check("scratch copy: physical working directory resolves", SessionStore_WorkingDir(s, cwd, physical, ARRAYSIZE(physical)));
    Check("scratch copy: scratch content exists in target storage", Join(physical, L"sentinel.txt", marker, ARRAYSIZE(marker)) &&
          Util_FileExists(marker));
    Check("scratch copy: absent stock logical directory stays absent", !Util_DirExists(s->profiles.items[0].dataDir));
    return TRUE;
}

static void TestSessions(const ProfileList *profiles, const WCHAR *projects)
{
    SessionSet s;
    WCHAR logical[MAX_PATH], physical[MAX_PATH], expected[MAX_PATH], outside[MAX_PATH];
    WCHAR id[SESSION_ID_CCH], path[LONG_PATH_CCH], cwd[MAX_PATH], error[512];
    int r, i;
    BOOL loaded = SessionStore_LoadProfiles(&s, profiles);
    Check("session load succeeds", loaded);
    if (!loaded) return;
    Check("virtual stock: signed in", s.source[0].signedIn);
    Check("virtual stock: entries directory found", s.source[0].entriesDir[0] != 0);
    Check("virtual stock: exactly two active-account sessions", ProfileRows(&s, 0) == 2 && s.entryCount == 2);
    Check("virtual stock: old account ignored", FindRow(&s, g_oldId) < 0);
    Check("empty secondary: signed in", s.source[1].signedIn);
    Check("empty secondary: entries directory found", s.source[1].entriesDir[0] != 0);
    Check("empty secondary: zero sessions", ProfileRows(&s, 1) == 0);
    Check("virtual stock: entries directory uses physical storage", Core_PathUnder(s.source[0].entriesDir, profiles->items[0].storageDir));
    Check("virtual stock: scratch directory uses logical storage", Core_PathUnder(s.source[0].scratchDir, profiles->items[0].dataDir));
    for (i = 0; i < s.entryCount; i++)
        Check("virtual stock: entry file uses physical storage", Core_PathUnder(s.entries[i].file, profiles->items[0].storageDir));
    r = FindRow(&s, g_scratchId);
    Check("virtual stock: scratch session loaded", r >= 0);
    if (r >= 0) {
        SessionRow original = s.rows[r];
        int originalOwner = s.groups[original.group].scratchOf;
        Check("virtual stock: scratch session belongs to logical profile", originalOwner == 0);
        Check("virtual stock: scratch transcript found", original.transcript);
        Check("working directory: virtual storage", SessionStore_WorkingDir(&s, original.cwd, physical, ARRAYSIZE(physical)) &&
              Core_PathUnder(physical, profiles->items[0].storageDir) && Util_DirExists(physical));
        if (CheckCopy(&s, r, 1, projects, id, path, cwd)) {
            WCHAR storage[MAX_PATH];
            StringCchCopyW(s.rows[r].key, ARRAYSIZE(s.rows[r].key), id);
            StringCchCopyW(s.rows[r].transcriptPath, ARRAYSIZE(s.rows[r].transcriptPath), path);
            StringCchCopyW(s.rows[r].cwd, ARRAYSIZE(s.rows[r].cwd), cwd);
            s.groups[original.group].scratchOf = 1;
            CheckCopy(&s, r, 0, projects, id, path, cwd);
            StringCchCopyW(storage, ARRAYSIZE(storage), s.profiles.items[0].storageDir);
            s.profiles.items[0].storageDir[0] = 0;
            Check("unresolved target: scratch copy fails safely",
                  SessionEdit_CopyConversation(NULL, &s, r, 0, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_FAILED && error[0]);
            Check("unresolved target: logical stock stays absent", !Util_DirExists(s.profiles.items[0].dataDir));
            StringCchCopyW(s.profiles.items[0].storageDir, ARRAYSIZE(s.profiles.items[0].storageDir), storage);
            s.rows[r] = original;
            s.groups[original.group].scratchOf = originalOwner;
        }
    }
    r = FindRow(&s, g_projectId);
    if (r >= 0 && SessionEdit_CopyConversation(NULL, &s, r, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE) {
        WCHAR beside[LONG_PATH_CCH], *slash;
        StringCchCopyW(beside, ARRAYSIZE(beside), s.rows[r].transcriptPath);
        if ((slash = wcsrchr(beside, L'\\')) != NULL) slash[1] = 0;
        StringCchCatW(beside, ARRAYSIZE(beside), id);
        StringCchCatW(beside, ARRAYSIZE(beside), L".jsonl");
        Check("project copy: filed beside the original", FileThere(beside));
        CheckCopiedLines("project copy", beside, g_projectId, id, s.rows[r].cwd, s.rows[r].cwd);
    } else Check("project copy: completes", FALSE);
    Check("path mapping: fixture", Join(profiles->items[0].dataDir, L"config.json", logical, ARRAYSIZE(logical)) &&
          Join(profiles->items[0].storageDir, L"config.json", expected, ARRAYSIZE(expected)));
    Check("path mapping: profile file uses physical storage", Core_ProfileFilePath(&profiles->items[0], logical, physical, ARRAYSIZE(physical)) &&
          Core_PathEquals(physical, expected));
    Check("path mapping: external fixture", FixturePath(L"workspace", outside, ARRAYSIZE(outside)));
    Check("working directory: external path unchanged", SessionStore_WorkingDir(&s, outside, physical, ARRAYSIZE(physical)) &&
          Core_PathEquals(physical, outside));
    Check("path mapping: neighboring profile fixture", Join(profiles->items[1].dataDir, L"config.json", logical, ARRAYSIZE(logical)));
    Check("path mapping: prefix boundary preserved", Core_ProfileFilePath(&profiles->items[0], logical, physical, ARRAYSIZE(physical)) &&
          Core_PathEquals(physical, logical));
    Check("virtual stock: reading and copies never create logical stock", !Util_DirExists(profiles->items[0].dataDir));
    SessionStore_Free(&s);
}

/* A copy that could not be opened goes again, and only what that copy made;
 * what could not go is named. */
static void TestCopyRemoval(const ProfileList *profiles, const WCHAR *projects)
{
    SessionSet set;
    WCHAR id[SESSION_ID_CCH], error[512], path[LONG_PATH_CCH], folder[LONG_PATH_CCH], original[LONG_PATH_CCH];
    WCHAR cwd[MAX_PATH], physical[MAX_PATH], originalFiles[MAX_PATH], left[LONG_PATH_CCH], *slash;
    HANDLE held;
    int row;
    BOOL ready;
    if (!SessionStore_LoadProfiles(&set, profiles)) {
        Check("copy removal snapshot loads", FALSE);
        return;
    }
    row = FindRow(&set, g_scratchId);
    ready = row >= 0 && SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE &&
            FindCopiedTranscript(projects, id, path, ARRAYSIZE(path)) && ReadString(path, "cwd", cwd, ARRAYSIZE(cwd)) &&
            SessionStore_WorkingDir(&set, cwd, physical, ARRAYSIZE(physical)) && Util_DirExists(physical) &&
            SessionStore_WorkingDir(&set, set.rows[row].cwd, originalFiles, ARRAYSIZE(originalFiles));
    Check("copy removal: a scratch copy is made", ready);
    if (ready) {
        StringCchCopyW(folder, ARRAYSIZE(folder), path);
        if ((slash = wcsrchr(folder, L'\\')) != NULL) *slash = 0;
        StringCchCopyW(original, ARRAYSIZE(original), set.rows[row].transcriptPath);
        if ((slash = wcsrchr(original, L'\\')) != NULL) *slash = 0;
        Check("copy removal: another id removes nothing",
              !SessionEdit_RemoveCopy(&set, row, 1, g_oldId, left, ARRAYSIZE(left)) && !left[0] && FileThere(path));
        Check("copy removal: another target removes nothing",
              !SessionEdit_RemoveCopy(&set, row, 0, id, left, ARRAYSIZE(left)) && !left[0] && FileThere(path));
        Check("copy removal: the copy is removed", SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)) && !left[0]);
        Check("copy removal: its transcript is gone", !FileThere(path));
        Check("copy removal: its copied working folder is gone", !Util_DirExists(physical));
        Check("copy removal: a project folder made for it is gone, the original's stays",
              Core_PathEquals(folder, original) ? DirThere(folder) : !DirThere(folder));
        Check("copy removal: the original's working folder stays", Util_DirExists(originalFiles));
        Check("copy removal: removes once only", !SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)));
    }
    row = FindRow(&set, g_projectId);
    ready = row >= 0 && SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE &&
            FindCopiedTranscript(projects, id, path, ARRAYSIZE(path));
    Check("copy removal: a project copy is made", ready);
    if (ready) {
        Check("copy removal: the project copy is removed", SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)) && !FileThere(path));
        Check("copy removal: the original transcript and its folder stay", FileThere(set.rows[row].transcriptPath));
    }
    ready = row >= 0 && SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE &&
            FindCopiedTranscript(projects, id, path, ARRAYSIZE(path));
    held = ready ? CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    Check("copy removal: a copy held open by another program", held != INVALID_HANDLE_VALUE);
    if (held != INVALID_HANDLE_VALUE) {
        Check("copy removal: what could not be deleted is named",
              !SessionEdit_RemoveCopy(&set, row, 1, id, left, ARRAYSIZE(left)) && Core_PathEquals(left, path) && FileThere(path));
        CloseHandle(held);
        DeleteFileW(path);
    }
    SessionStore_Free(&set);
}

static void TestSettings(const Profile *stock, const WCHAR *local)
{
    Profile target;
    WCHAR config[MAX_PATH], desktop[MAX_PATH], value[64];
    char *json;
    DWORD len;
    BOOL ready = SetProfile(&target, L"Settings", L"R\\Claude-Settings", FALSE) && MakeDir(target.dataDir) &&
                 Profiles_ResolveStorage(&target, local, g_family) && Join(target.storageDir, L"config.json", config, ARRAYSIZE(config)) &&
                 Join(target.storageDir, L"claude_desktop_config.json", desktop, ARRAYSIZE(desktop));
    Check("settings: fixture ready", ready);
    if (!ready) return;
    Profiles_CopySettings(stock, &target);
    Check("settings: app settings copied from virtual storage", ReadString(config, "locale", value, ARRAYSIZE(value)) && wcscmp(value, L"en-US") == 0);
    Check("settings: theme copied", ReadString(config, "userThemeMode", value, ARRAYSIZE(value)) && wcscmp(value, L"dark") == 0);
    Check("settings: account omitted", !HasMember(config, "lastKnownAccountUuid"));
    Check("settings: sign-in state omitted", !HasMember(config, "windowSizeWasSignedIn"));
    Check("settings: token omitted", !HasMember(config, "accessToken"));
    Check("settings: MCP servers copied", HasMember(desktop, "mcpServers"));
    Check("settings: acceleration setting copied", HasMember(desktop, "isHardwareAccelerationDisabled"));
    Check("settings: credentials omitted", !HasMember(desktop, "credentials"));
    json = Util_ReadFile(desktop, 65536, FALSE, &len);
    Check("settings: only allowed preference copied", json && strstr(json, "menuBarEnabled") && !strstr(json, "privateValue"));
    if (json) HeapFree(GetProcessHeap(), 0, json);
    Check("settings: source logical directory stays absent", !Util_DirExists(stock->dataDir));
}

static BOOL SetModified(const WCHAR *path, ULONGLONG ticks)
{
    FILETIME modified;
    HANDLE file = CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    modified.dwLowDateTime = (DWORD)ticks;
    modified.dwHighDateTime = (DWORD)(ticks >> 32);
    ok = SetFileTime(file, NULL, NULL, &modified);
    CloseHandle(file);
    return ok;
}

static void TestNewestAccount(const WCHAR *projects)
{
    ProfileList profiles;
    SessionSet set;
    WCHAR oldEntries[MAX_PATH], newEntries[MAX_PATH], oldFile[MAX_PATH], newFile[MAX_PATH], cwd[MAX_PATH];
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = SetProfile(&profiles.items[0], L"AccountFallback", L"accounts\\profile", FALSE) &&
            Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\account-a-old\\org-old", oldEntries, ARRAYSIZE(oldEntries)) &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\account-z-new\\org-new", newEntries, ARRAYSIZE(newEntries)) &&
            FixturePath(L"workspace", cwd, ARRAYSIZE(cwd)) &&
            WriteSession(oldEntries, projects, g_oldId, cwd) && WriteSession(newEntries, projects, g_projectId, cwd) &&
            SUCCEEDED(StringCchPrintfW(oldFile, ARRAYSIZE(oldFile), L"%s\\local_%s.json", oldEntries, g_oldId)) &&
            SUCCEEDED(StringCchPrintfW(newFile, ARRAYSIZE(newFile), L"%s\\local_%s.json", newEntries, g_projectId)) &&
            SetModified(oldFile, 132000000000000000ULL) && SetModified(newFile, 133000000000000000ULL);
    Check("account fallback fixture created", ready);
    if (!ready) return;
    Check("account fallback snapshot loads", SessionStore_LoadProfiles(&set, &profiles));
    Check("missing config selects the account whose nested entry is newest", Core_PathEquals(set.source[0].entriesDir, newEntries));
    Check("missing config excludes older account sessions", FindRow(&set, g_projectId) >= 0 && FindRow(&set, g_oldId) < 0);
    SessionStore_Free(&set);
}

static int CountFolders(const WCHAR *dir)
{
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(dir, L"*", &found, TRUE);
    int count = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0)
            count++;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return count;
}

/* A copy without the working folder it needs fails with a reason; one
 * cancelled in Windows' progress leaves nothing and no error. */
static void TestScratchCopyFailure(const ProfileList *profiles, const WCHAR *projects)
{
    SessionSet set;
    WCHAR id[SESSION_ID_CCH], error[512], missing[MAX_PATH], original[MAX_PATH], physical[MAX_PATH], marker[MAX_PATH];
    WCHAR area[MAX_PATH], copied[LONG_PATH_CCH];
    CopyResult result;
    int row, before;
    Check("scratch error snapshot loads", SessionStore_LoadProfiles(&set, profiles));
    row = FindRow(&set, g_scratchId);
    if (row >= 0 && FixturePath(L"missing-scratch", missing, ARRAYSIZE(missing))) {
        StringCchCopyW(original, ARRAYSIZE(original), set.rows[row].cwd);
        StringCchCopyW(set.rows[row].cwd, ARRAYSIZE(set.rows[row].cwd), missing);
        Check("missing scratch files do not produce a successful empty copy", SessionEdit_CopyConversation(NULL, &set, row, 1,
              id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_FAILED);
        Check("missing scratch copy has a reason", error[0] != 0);
        StringCchCopyW(set.rows[row].cwd, ARRAYSIZE(set.rows[row].cwd), original);
        if (Core_ProfileFilePath(&set.profiles.items[1], set.source[1].scratchDir, area, ARRAYSIZE(area))) {
            before = CountFolders(area);
            g_cancelCopies = TRUE;
            result = SessionEdit_CopyConversation(NULL, &set, row, 1, id, ARRAYSIZE(id), error, ARRAYSIZE(error));
            g_cancelCopies = FALSE;
            Check("a copy cancelled in Windows' progress is told apart from a failure", result == COPY_CANCELLED && !error[0]);
            Check("a cancelled copy leaves no working folder, not even in part", CountFolders(area) == before);
            Check("a cancelled copy leaves no transcript", !FindCopiedTranscript(projects, id, copied, ARRAYSIZE(copied)));
        } else Check("scratch area of the copy's profile", FALSE);
        if (SessionStore_WorkingDir(&set, original, physical, ARRAYSIZE(physical)) &&
            Join(physical, L"sentinel.txt", marker, ARRAYSIZE(marker)) && Core_PathUnder(marker, g_root) && DeleteFileW(marker)) {
            Check("empty existing scratch directory can be copied", SessionEdit_CopyConversation(NULL, &set, row, 1,
                  id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE);
        } else Check("empty scratch fixture ready", FALSE);
    } else Check("scratch error row found", FALSE);
    SessionStore_Free(&set);
}

static int QueueWorker(const WCHAR *root, const WCHAR *folder, const WCHAR *eventName, int worker)
{
    Profile profile;
    PendingEdit edit;
    HANDLE start;
    WCHAR state[MAX_PATH];
    BOOL waiting;
    int i;
    if (!Join(root, L"state", state, ARRAYSIZE(state))) return 1;
    Util_SetStateDir(state);
    ZeroMemory(&profile, sizeof profile);
    if (FAILED(StringCchCopyW(profile.folder, ARRAYSIZE(profile.folder), folder)) ||
        !Join(root, L"pending-profile", profile.dataDir, ARRAYSIZE(profile.dataDir))) return 1;
    start = OpenEventW(SYNCHRONIZE, FALSE, eventName);
    if (!start) return 1;
    if (WaitForSingleObject(start, 5000) != WAIT_OBJECT_0) { CloseHandle(start); return 1; }
    CloseHandle(start);
    for (i = 0; i < 64; i++) {
        ZeroMemory(&edit, sizeof edit);
        edit.op = PENDING_STAR;
        StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"%08lx-7777-4777-8777-%012lx", (DWORD)worker, (DWORD)i);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
        if (!SessionEdit_Change(NULL, &profile, NULL, &edit, &waiting, NULL, 0) || !waiting) return 1;
    }
    return 0;
}

static void TestPendingProcesses(void)
{
    Profile profile;
    PendingEdit *edits;
    WCHAR path[MAX_PATH], exe[MAX_PATH], command[MAX_PATH * 3], eventName[128];
    HANDLE start, process[2] = { NULL, NULL };
    BOOL ready;
    int worker, count, stars = 0;
    ready = SetProfile(&profile, L"Pending", L"pending-profile", FALSE) &&
            SessionStore_PendingPath(&profile, path, ARRAYSIZE(path)) && Util_SelfExe(exe, ARRAYSIZE(exe));
    Check("pending process fixture prepared", ready);
    if (!ready) return;
    StringCchPrintfW(eventName, ARRAYSIZE(eventName), L"Local\\SessionFixture.%08lx.%08lx", GetCurrentProcessId(), g_fixtureTag);
    start = CreateEventW(NULL, TRUE, FALSE, eventName);
    if (!start) { Check("pending process start event created", FALSE); return; }
    for (worker = 0; worker < 2; worker++) {
        STARTUPINFOW startup;
        PROCESS_INFORMATION child;
        ZeroMemory(&startup, sizeof startup);
        startup.cb = sizeof startup;
        ZeroMemory(&child, sizeof child);
        ready = SUCCEEDED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" --queue-worker \"%s\" \"%s\" \"%s\" %d",
                                           exe, g_root, profile.folder, eventName, worker)) &&
                CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &child);
        Check("parallel queue worker started", ready);
        if (ready) { process[worker] = child.hProcess; CloseHandle(child.hThread); }
    }
    SetEvent(start);
    for (worker = 0; worker < 2; worker++) {
        if (process[worker]) {
            DWORD code = 1;
            ready = WaitForSingleObject(process[worker], 60000) == WAIT_OBJECT_0;
            Check("queue worker completes without write collisions", ready && GetExitCodeProcess(process[worker], &code) && code == 0);
            if (!ready) {
                TerminateProcess(process[worker], 1);
                WaitForSingleObject(process[worker], 5000);
            }
            CloseHandle(process[worker]);
        }
    }
    CloseHandle(start);
    edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    if (edits) {
        count = SessionStore_LoadPending(&profile, edits, SESSION_PENDING_MAX);
        for (worker = 0; worker < count; worker++)
            if (edits[worker].op == PENDING_STAR && edits[worker].value[0] == L'1') stars++;
        Check("parallel processes retain every queued change", count == 128 && stars == 128);
        HeapFree(GetProcessHeap(), 0, edits);
    } else Check("pending process results can be read", FALSE);
    Check("unique pending fixture file removed", DeleteFileW(path));
}

/* Whether a removal waits for `profile` (or its queue cannot be read). */
static BOOL RemovalQueued(const Profile *profile)
{
    PendingEdit *edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    int n = edits ? SessionStore_LoadPending(profile, edits, SESSION_PENDING_MAX) : -1, i;
    BOOL queued = n < 0;
    for (i = 0; i < n && !queued; i++) queued = edits[i].op == PENDING_REMOVE;
    if (edits) HeapFree(GetProcessHeap(), 0, edits);
    return queued;
}

static void TestPendingApplication(const ProfileList *profiles)
{
    const Profile *profile = &profiles->items[0];
    SessionSet set;
    PendingEdit edit, queued[3];
    HWND window;
    WCHAR path[MAX_PATH], title[SESSION_TITLE_CCH];
    BOOL waiting;
    int row;
    Check("pending application snapshot loads", SessionStore_LoadProfiles(&set, profiles));
    row = FindRow(&set, g_projectId);
    if (row < 0 || !SessionStore_PendingPath(profile, path, ARRAYSIZE(path))) {
        Check("pending application entry found", FALSE);
        SessionStore_Free(&set);
        return;
    }
    window = StartFakeClaude(profile);
    Check("running profile message-window fixture created", window != NULL);
    if (!window) { SessionStore_Free(&set); return; }
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_projectId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Queued fixture title");
    Check("running profile change queues successfully",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) && waiting);
    Check("running profile entry stays unchanged", ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) &&
          wcscmp(title, L"Fixture session") == 0);
    Check("running profile does not apply its queue", SessionEdit_ApplyPending(NULL, profile) == 0 && SessionStore_LoadPending(profile, queued, 3) == 1);
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    Check("second pending kind retains queued title", SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) &&
          waiting && SessionStore_LoadPending(profile, queued, 3) == 2);
    Check("cancelling one pending kind preserves another", SessionEdit_Cancel(profile, &edit, 1) && SessionStore_LoadPending(profile, queued, 3) == 1 &&
          queued[0].op == PENDING_TITLE);
    StopFakeClaude(window);
    Check("closed private profile applies the queued title", SessionEdit_ApplyPending(NULL, profile) == 1 &&
          ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, L"Queued fixture title") == 0);
    Check("applied title records user ownership", ReadString(set.entries[set.rows[row].entry[0]].file, "titleSource", title, ARRAYSIZE(title)) &&
          wcscmp(title, L"user") == 0);
    Check("applied queue file removed", !Util_FileExists(path));
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"local_%s", g_projectId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Title queued under the entry's own id");
    Check("a change named by an entry's own id is queued", SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting);
    Check("a change queued under an entry's own id reaches the entry of its session",
          SessionEdit_ApplyPending(NULL, profile) == 1 &&
          ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, edit.value) == 0);
    if (Util_FileExists(path)) DeleteFileW(path);
    SessionStore_Free(&set);
}

static BOOL WrittenAt(const WCHAR *path, FILETIME *written)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return FALSE;
    *written = data.ftLastWriteTime;
    return TRUE;
}

/* The queue file: blank lines are skipped, a file that cannot be read is
 * never rewritten (its changes would be lost), and cancelling what is not
 * there writes nothing. */
static void TestPendingQueueFile(void)
{
    Profile profile, empty;
    PendingEdit edits[4], edit;
    WCHAR path[MAX_PATH], emptyPath[MAX_PATH];
    const WCHAR *unqueued[1];
    FILETIME before, after;
    HANDLE locked;
    BOOL waiting;
    char text[512];
    BOOL ready = SetProfile(&profile, L"Queue", L"queue-profile", FALSE) && SessionStore_PendingPath(&profile, path, ARRAYSIZE(path)) &&
                 SUCCEEDED(StringCchPrintfA(text, sizeof text, "title\t%ls\tFirst\r\n\r\n\nstar\t%ls\t1\n", g_projectId, g_projectId)) &&
                 Save(path, text);
    Check("queue file fixture written", ready);
    if (!ready) return;
    Check("blank lines in the queue are skipped", SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == 2 &&
          edits[0].op == PENDING_TITLE && edits[1].op == PENDING_STAR);
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_oldId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    ready = SetModified(path, 132000000000000000ULL) && WrittenAt(path, &before);
    Check("cancelling a change that is not queued leaves the queue file as it is",
          ready && SessionEdit_Cancel(&profile, &edit, 1) && WrittenAt(path, &after) && CompareFileTime(&before, &after) == 0);
    unqueued[0] = g_oldId;
    SessionEdit_Forget(&profile, unqueued, ARRAYSIZE(unqueued));
    Check("forgetting a session with nothing queued leaves the queue file as it is",
          ready && WrittenAt(path, &after) && CompareFileTime(&before, &after) == 0 &&
          SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == 2);
    Check("cancelling with no queue makes none",
          SetProfile(&empty, L"Unqueued", L"unqueued-profile", FALSE) && SessionStore_PendingPath(&empty, emptyPath, ARRAYSIZE(emptyPath)) &&
          SessionEdit_Cancel(&empty, &edit, 1) && !Util_FileExists(emptyPath));
    locked = CreateFileW(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    Check("queue file locked by another process fixture", locked != INVALID_HANDLE_VALUE);
    if (locked != INVALID_HANDLE_VALUE) {
        Check("an unreadable queue reads as unreadable, not empty", SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == -1);
        Check("a change is refused rather than written over an unreadable queue",
              !SessionEdit_Change(NULL, &profile, NULL, &edit, &waiting, NULL, 0) && !waiting);
        CloseHandle(locked);
        Check("the unreadable queue kept its changes", SessionStore_LoadPending(&profile, edits, ARRAYSIZE(edits)) == 2);
    }
    DeleteFileW(path);
}

/* Both entries of a session listed twice titled `title`. */
static BOOL BothTitled(const WCHAR *older, const WCHAR *newer, const WCHAR *title)
{
    WCHAR read[SESSION_TITLE_CCH];
    return ReadString(older, "title", read, ARRAYSIZE(read)) && wcscmp(read, title) == 0 &&
           ReadString(newer, "title", read, ARRAYSIZE(read)) && wcscmp(read, title) == 0;
}

/* Entry details: every entry kept when a session is listed twice in one
 * profile, and changed under any of its keys; long titles, group names and
 * lookups by key. */
static void TestEntryDetails(void)
{
    ProfileList profiles;
    SessionSet set, shown;
    PendingEdit edit, keep[3];
    WCHAR entries[MAX_PATH], name[MAX_PATH], older[MAX_PATH], newer[MAX_PATH], pending[MAX_PATH], error[512];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    HWND window;
    char json[2048], titleText[301];
    int row, count = 0, head, shownRow;
    BOOL ready, waiting;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    memset(titleText, 'T', 300);
    titleText[300] = 0;
    ready = SetProfile(&profiles.items[0], L"Details", L"details\\profile", FALSE) &&
            Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            SaveIn(profiles.items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"details\"}") &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\details\\organization", entries, ARRAYSIZE(entries)) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_older\",\"cliSessionId\":\"%ls\",\"title\":\"Older\","
                                                          "\"cwd\":\"C:\\\\Details\",\"lastActivityAt\":10}", g_projectId)) &&
            SaveIn(entries, L"local_older.json", json) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_newer\",\"cliSessionId\":\"%ls\",\"title\":\"Newer\","
                                                          "\"cwd\":\"C:\\\\Details\",\"lastActivityAt\":20}", g_projectId)) &&
            SaveIn(entries, L"local_newer.json", json) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_long\",\"cliSessionId\":\"%ls\",\"title\":\"%s\","
                                                          "\"lastActivityAt\":5}", g_oldId, titleText)) &&
            SaveIn(entries, L"local_long.json", json) &&
            Join(entries, L"local_older.json", older, ARRAYSIZE(older)) && Join(entries, L"local_newer.json", newer, ARRAYSIZE(newer));
    Check("entry detail fixtures created", ready);
    if (!ready) return;
    Check("entry detail snapshot loads", SessionStore_LoadProfiles(&set, &profiles));
    row = SessionStore_FindRow(&set, g_projectId);
    Check("rows are found by key through the index", row >= 0 && row == FindRow(&set, g_projectId));
    head = row >= 0 ? set.rows[row].entry[0] : -1;
    Check("a session listed twice in one profile names its latest entry",
          head >= 0 && set.entryCount == 3 && wcscmp(set.entries[head].title, L"Newer") == 0);
    Check("the other entry of that session follows it",
          head >= 0 && set.entries[head].duplicate >= 0 && wcscmp(set.entries[set.entries[head].duplicate].title, L"Older") == 0 &&
          set.entries[set.entries[head].duplicate].duplicate < 0);
    Check("a session listed twice lists both entries for deletion",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) &&
          Listed(paths, count, older) && Listed(paths, count, newer));
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    paths = NULL;
    Check("a row that is not there lists nothing, with a reason",
          !SessionEdit_ListFiles(&set, -1, &paths, &count, error, ARRAYSIZE(error)) && !paths && count == 0 && error[0]);
    if (head >= 0 && SessionStore_PendingPath(&profiles.items[0], pending, ARRAYSIZE(pending))) {
        Profile *profile = &profiles.items[0];
        ZeroMemory(&edit, sizeof edit);
        edit.op = PENDING_TITLE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_projectId);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Renamed twice");
        Check("a rename of a session listed twice is made at once",
              SessionEdit_Change(NULL, profile, &set.entries[head], &edit, &waiting, NULL, 0) && !waiting);
        Check("the rename reaches both of its entries", BothTitled(older, newer, L"Renamed twice"));

        window = StartFakeClaude(profile);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Renamed while running");
        ready = window && SessionEdit_Change(NULL, profile, &set.entries[head], &edit, &waiting, NULL, 0) && waiting;
        StopFakeClaude(window);
        Check("a rename of a session listed twice waits while its profile runs", ready);
        Check("the waiting rename reaches both of its entries once the profile is closed",
              ready && SessionEdit_ApplyPending(NULL, profile) == 1 && BothTitled(older, newer, L"Renamed while running") &&
              !Util_FileExists(pending));

        /* A rename queued under the own id of the older entry, made before the session's first message. */
        window = StartFakeClaude(profile);
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), L"local_older");
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Queued under an own id");
        ready = window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
        StopFakeClaude(window);
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_projectId);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Renamed at once");
        Check("a rename made at once also takes off what waited under another key of the session",
              ready && SessionEdit_Change(NULL, profile, &set.entries[head], &edit, &waiting, NULL, 0) && !waiting &&
              BothTitled(older, newer, L"Renamed at once") && !Util_FileExists(pending));

        /* Keep: a removal queued under the own id of the older entry, shown on the session and cancelled. */
        window = StartFakeClaude(profile);
        edit.op = PENDING_REMOVE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), L"local_older");
        edit.value[0] = 0;
        ready = window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting && RemovalQueued(profile);
        StopFakeClaude(window);
        Check("a removal queued under the own id of a session's other entry", ready);
        if (ready && SessionStore_LoadProfiles(&shown, &profiles)) {
            shownRow = FindRow(&shown, g_projectId);
            Check("a removal queued under another entry's own id shows on the session",
                  shownRow >= 0 && shown.entries[shown.rows[shownRow].entry[0]].pendingRemove);
            SessionStore_Free(&shown);
        } else Check("a removal queued under another entry's own id is read", FALSE);
        ZeroMemory(keep, sizeof keep);
        keep[0].op = keep[1].op = keep[2].op = PENDING_REMOVE;
        StringCchCopyW(keep[0].key, ARRAYSIZE(keep[0].key), g_projectId);
        StringCchCopyW(keep[1].key, ARRAYSIZE(keep[1].key), L"local_newer");
        StringCchCopyW(keep[2].key, ARRAYSIZE(keep[2].key), L"local_older");
        Check("Keep, under every key of the session, cancels it",
              SessionEdit_Cancel(profile, keep, ARRAYSIZE(keep)) && !RemovalQueued(profile) && !Util_FileExists(pending) &&
              FileThere(older) && FileThere(newer));
    } else Check("a session listed twice and its queue", FALSE);
    row = SessionStore_FindRow(&set, g_oldId);
    Check("a title too long to keep whole keeps its beginning",
          row >= 0 && wcslen(set.entries[set.rows[row].entry[0]].title) == SESSION_TITLE_CCH - 1 &&
          wcsspn(set.entries[set.rows[row].entry[0]].title, L"T") == SESSION_TITLE_CCH - 1);
    if (row >= 0) SessionStore_GroupName(&set, set.rows[row].group, name, ARRAYSIZE(name));
    Check("a session without a working folder is shown in the unknown folder", row >= 0 && wcscmp(name, L"Unknown folder") == 0);
    SessionStore_Free(&set);
}

/* Stars made at once and later, what a snapshot shows of waiting
 * changes, changes that cannot be made, the queue's limit and Forget. */
/* A Remote Control session turned into a plain local one (PENDING_UNREMOTE):
 * the members that make Claude bridge it go, the rest stays; at once in a
 * closed profile, else once its Claude closes. */
static void TestTurnLocal(void)
{
    static const WCHAR *const ids[2] = { L"cccccccc-0000-4000-8000-0000000000a1", L"cccccccc-0000-4000-8000-0000000000a2" };
    static const char *const bridged =
        ",\"remoteControlUserEnabled\":true,\"bridgeSessionIds\":[\"session_01Fixture\"],\"steeredByRemoteClient\":false,"
        "\"remoteControlAutoEligible\":false,\"isStarred\":true";
    ProfileList profiles;
    Profile *profile = &profiles.items[0];
    SessionSet set;
    PendingEdit edit;
    WCHAR entries[MAX_PATH], files[2][MAX_PATH], pending[MAX_PATH], title[SESSION_TITLE_CCH];
    HWND window;
    BOOL ready, waiting;
    int i, rows[2];
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(profile, L"Turn local", L"turnlocal\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            SessionStore_PendingPath(profile, pending, ARRAYSIZE(pending));
    for (i = 0; ready && i < 2; i++) ready = WriteEntry(entries, ids[i], bridged) && EntryPath(entries, ids[i], files[i], MAX_PATH);
    ready = ready && SessionStore_LoadProfiles(&set, &profiles);
    Check("turn local: fixtures ready", ready);
    if (!ready) return;
    for (i = 0; i < 2; i++) rows[i] = FindRow(&set, ids[i]);
    if (rows[0] < 0 || rows[1] < 0) {
        Check("turn local: sessions listed", FALSE);
        SessionStore_Free(&set);
        return;
    }
    Check("turn local: listed as Remote Control", set.entries[set.rows[rows[0]].entry[0]].environment == SESSION_ENV_REMOTE);

    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_UNREMOTE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), ids[0]);
    Check("turn local: made at once in a closed profile",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[0]].entry[0]], &edit, &waiting, NULL, 0) && !waiting &&
          !Util_FileExists(pending));
    Check("turn local: Remote Control's members gone",
          !HasMember(files[0], "remoteControlUserEnabled") && !HasMember(files[0], "bridgeSessionIds") &&
          !HasMember(files[0], "steeredByRemoteClient") && !HasMember(files[0], "remoteControlAutoEligible"));
    Check("turn local: the rest kept", ReadTrue(files[0], "isStarred") && ReadString(files[0], "title", title, ARRAYSIZE(title)) &&
          wcscmp(title, L"Fixture session") == 0);

    window = StartFakeClaude(profile);
    Check("turn local: running profile", window != NULL);
    if (window) {
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), ids[1]);
        Check("turn local: waits while the profile runs",
              SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[1]].entry[0]], &edit, &waiting, NULL, 0) && waiting &&
              ReadTrue(files[1], "remoteControlUserEnabled"));
        StopFakeClaude(window);
        Check("turn local: made once its Claude closes", SessionEdit_ApplyPending(NULL, profile) == 1 &&
              !HasMember(files[1], "remoteControlUserEnabled") && !HasMember(files[1], "bridgeSessionIds") && !Util_FileExists(pending));
    }
    SessionStore_Free(&set);
    Check("turn local: listed as local after", SessionStore_LoadProfiles(&set, &profiles) && (rows[0] = FindRow(&set, ids[0])) >= 0 &&
          set.entries[set.rows[rows[0]].entry[0]].environment == SESSION_ENV_LOCAL);
    SessionStore_Free(&set);
}

static void TestPendingChanges(void)
{
    ProfileList profiles;
    Profile *profile = &profiles.items[0], full;
    SessionSet set;
    static PendingEdit edits[SESSION_PENDING_MAX];
    PendingEdit edit;
    WCHAR entries[MAX_PATH], files[3][MAX_PATH], pending[MAX_PATH], title[SESSION_TITLE_CCH], temporary[MAX_PATH];
    const WCHAR *forgotten[1];
    HWND window;
    HANDLE locked;
    BOOL ready, waiting, accepted;
    int i, rows[3], row;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(profile, L"Edits", L"edits\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            SessionStore_PendingPath(profile, pending, ARRAYSIZE(pending));
    for (i = 0; ready && i < 3; i++) ready = WriteEntry(entries, g_editIds[i], "") && EntryPath(entries, g_editIds[i], files[i], MAX_PATH);
    ready = ready && SessionStore_LoadProfiles(&set, &profiles);
    Check("change fixtures ready", ready);
    if (!ready) return;
    for (i = 0; i < 3; i++) rows[i] = FindRow(&set, g_editIds[i]);
    if (rows[0] < 0 || rows[1] < 0 || rows[2] < 0) {
        Check("change fixture sessions loaded", FALSE);
        SessionStore_Free(&set);
        return;
    }

    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[1]);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    Check("a star in a closed profile is made at once",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[1]].entry[0]], &edit, &waiting, NULL, 0) &&
          !waiting && ReadTrue(files[1], "isStarred") && !Util_FileExists(pending));

    window = StartFakeClaude(profile);
    Check("change fixture: running profile", window != NULL);
    if (window) {
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"0");
        Check("a star in a running profile waits",
              SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[1]].entry[0]], &edit, &waiting, NULL, 0) &&
              waiting && ReadTrue(files[1], "isStarred"));
        edit.op = PENDING_TITLE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[0]);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Waiting title");
        accepted = SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[0]].entry[0]], &edit, &waiting, NULL, 0) && waiting;
        edit.op = PENDING_REMOVE;
        StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[2]);
        edit.value[0] = 0;
        accepted = accepted && SessionEdit_Change(NULL, profile, &set.entries[set.rows[rows[2]].entry[0]], &edit, &waiting, NULL, 0) && waiting;
        Check("changes to a running profile are queued", accepted);
        SessionStore_Free(&set);
        ready = SessionStore_LoadProfiles(&set, &profiles);
        Check("a snapshot shows what waits", ready && set.source[0].pending == 3);
        if (ready && (row = FindRow(&set, g_editIds[0])) >= 0) {
            const SessionEntry *entry = &set.entries[set.rows[row].entry[0]];
            Check("a waiting title shows on its entry", entry->pending && wcscmp(entry->pendingTitle, L"Waiting title") == 0 &&
                  entry->pendingStar == -1 && !entry->pendingRemove);
        } else Check("a waiting title's session is listed", FALSE);
        if (ready && (row = FindRow(&set, g_editIds[1])) >= 0)
            Check("a waiting star shows on its entry", set.entries[set.rows[row].entry[0]].pendingStar == 0 &&
                  set.entries[set.rows[row].entry[0]].starred);
        else Check("a waiting star's session is listed", FALSE);
        if (ready && (row = FindRow(&set, g_editIds[2])) >= 0)
            Check("a waiting removal shows on its entry", set.entries[set.rows[row].entry[0]].pendingRemove);
        else Check("a waiting removal's session is listed", FALSE);
        Check("Keep cancels the waiting removal", SessionEdit_Cancel(profile, &edit, 1) && !RemovalQueued(profile));
        forgotten[0] = g_editIds[0];
        SessionEdit_Forget(profile, forgotten, ARRAYSIZE(forgotten));
        Check("Forget drops every change to that session only", SessionStore_LoadPending(profile, edits, ARRAYSIZE(edits)) == 1 &&
              wcscmp(edits[0].key, g_editIds[1]) == 0);
        StopFakeClaude(window);
        Check("the waiting star is made once the profile is closed",
              SessionEdit_ApplyPending(NULL, profile) == 1 && !ReadTrue(files[1], "isStarred") && !Util_FileExists(pending));
    }

    /* A change that fails while the profile is closed leaves the queue, logged, and the folder holds no temporary file. */
    StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s.cdm-new", files[2]);
    window = StartFakeClaude(profile);
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_editIds[2]);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Never written");
    row = FindRow(&set, g_editIds[2]);
    ready = window && row >= 0 && SetFileAttributesW(files[2], FILE_ATTRIBUTE_READONLY) &&
            SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    Check("a read-only entry fixture with a waiting title", ready);
    if (ready) {
        Check("a change that cannot be made is dropped", SessionEdit_ApplyPending(NULL, profile) == 1 &&
              !Util_FileExists(pending) && ReadString(files[2], "title", title, ARRAYSIZE(title)) && wcscmp(title, L"Fixture session") == 0);
        Check("a dropped change leaves no temporary file", !Util_FileExists(temporary));
        Check("a dropped change is not tried again", SessionEdit_ApplyPending(NULL, profile) == 0);
    }
    SetFileAttributesW(files[2], FILE_ATTRIBUTE_NORMAL);

    /* Rule 13: an entry gone is never written again. */
    row = FindRow(&set, g_editIds[2]);
    ready = row >= 0 && DeleteFileW(files[2]);
    Check("a deleted entry fixture", ready);
    if (ready) {
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Rule 13");
        Check("a change to an entry gone fails at once",
              !SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) &&
              !waiting && !Util_FileExists(files[2]) && !Util_FileExists(temporary) && !Util_FileExists(pending));
        window = StartFakeClaude(profile);
        ready = window && SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) && waiting;
        StopFakeClaude(window);
        Check("a change waits for an entry that is gone", ready);
        Check("a waiting change never makes a missing entry", SessionEdit_ApplyPending(NULL, profile) == 0 &&
              !Util_FileExists(files[2]) && SessionStore_LoadPending(profile, edits, ARRAYSIZE(edits)) == 1);
        Check("once the profile has run, a change to a session it does not list is dropped",
              SessionEdit_ApplyPendingAfterRun(profile) == 1 && !Util_FileExists(files[2]) && !Util_FileExists(pending));
    }

    /* After a run, an unreadable entry keeps every change waiting: its session may be the one. */
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_unlistedId);
    ready = SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    locked = CreateFileW(files[0], GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    Check("an unlisted session's change with an unreadable entry", ready && locked != INVALID_HANDLE_VALUE);
    if (locked != INVALID_HANDLE_VALUE) {
        Check("an incomplete read drops nothing", SessionEdit_ApplyPendingAfterRun(profile) == 0 &&
              SessionStore_LoadPending(profile, edits, ARRAYSIZE(edits)) == 1);
        CloseHandle(locked);
        Check("a complete read drops it", SessionEdit_ApplyPendingAfterRun(profile) == 1 && !Util_FileExists(pending));
    }
    if (Util_FileExists(pending)) DeleteFileW(pending);
    SessionStore_Free(&set);

    /* The queue takes SESSION_PENDING_MAX changes, and refuses the next one. */
    ready = SetProfile(&full, L"Full", L"full-profile", FALSE) && SessionStore_PendingPath(&full, pending, ARRAYSIZE(pending));
    accepted = TRUE;
    for (i = 0; ready && accepted && i < SESSION_PENDING_MAX; i++) {
        ZeroMemory(&edit, sizeof edit);
        edit.op = PENDING_STAR;
        StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"%08lx-9999-4999-8999-999999999999", (DWORD)i);
        StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
        accepted = SessionEdit_Change(NULL, &full, NULL, &edit, &waiting, NULL, 0) && waiting;
    }
    Check("the queue takes its limit of changes", ready && accepted && SessionStore_LoadPending(&full, edits, ARRAYSIZE(edits)) == SESSION_PENDING_MAX);
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_unlistedId);
    Check("a change past the limit is refused, the queue kept", !SessionEdit_Change(NULL, &full, NULL, &edit, &waiting, NULL, 0) && !waiting &&
          SessionStore_LoadPending(&full, edits, ARRAYSIZE(edits)) == SESSION_PENDING_MAX);
    DeleteFileW(pending);
}

static void TestReadonlyPending(const ProfileList *profiles)
{
    const Profile *profile = &profiles->items[0];
    SessionSet set;
    PendingEdit oldEdit = { 0 }, newer, queued[3];
    WCHAR path[MAX_PATH], original[SESSION_TITLE_CCH], title[SESSION_TITLE_CCH];
    BOOL waiting, ready;
    int row;
    ready = SessionStore_LoadProfiles(&set, profiles);
    Check("readonly pending snapshot loads", ready);
    if (!ready) return;
    row = FindRow(&set, g_projectId);
    if (row < 0 || !SessionStore_PendingPath(profile, path, ARRAYSIZE(path)) ||
        !ReadString(set.entries[set.rows[row].entry[0]].file, "title", original, ARRAYSIZE(original))) {
        Check("readonly pending entry is available", FALSE);
        SessionStore_Free(&set);
        return;
    }
    oldEdit.op = PENDING_TITLE;
    StringCchCopyW(oldEdit.key, ARRAYSIZE(oldEdit.key), g_projectId);
    StringCchCopyW(oldEdit.value, ARRAYSIZE(oldEdit.value), L"Old queued intent");
    ready = SessionEdit_Change(NULL, profile, NULL, &oldEdit, &waiting, NULL, 0) && waiting &&
            SetFileAttributesW(path, FILE_ATTRIBUTE_READONLY);
    Check("readonly pending intent fixture is durable", ready);
    if (ready) {
        WCHAR *preview = SessionSync_PendingText(profiles);
        Check("pending details display intent without applying it", preview && wcsstr(preview, L"Old queued intent") &&
              ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, original) == 0 &&
              SessionStore_LoadPending(profile, queued, ARRAYSIZE(queued)) == 1);
        if (preview) HeapFree(GetProcessHeap(), 0, preview);
        newer = oldEdit;
        StringCchCopyW(newer.value, ARRAYSIZE(newer.value), L"Newest chosen intent");
        Check("a blocked intent commit returns failure before changing the entry",
              !SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &newer, &waiting, NULL, 0) && !waiting &&
              ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, original) == 0);
        Check("a failed intent replacement retains the older durable queue",
              SessionStore_LoadPending(profile, queued, ARRAYSIZE(queued)) == 1 && wcscmp(queued[0].value, oldEdit.value) == 0);
        Check("a blocked queue cannot replay entries or sustain a reload loop",
              SessionEdit_ApplyPending(NULL, profile) == 0 && SessionEdit_ApplyPending(NULL, profile) == 0 &&
              ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, original) == 0);
        Check("pending intent becomes writable", SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL));
        Check("a writable intent commits the latest entry and clears its queue",
              SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &newer, &waiting, NULL, 0) && !waiting &&
              ReadString(set.entries[set.rows[row].entry[0]].file, "title", title, ARRAYSIZE(title)) && wcscmp(title, newer.value) == 0 &&
              !Util_FileExists(path));
    }
    if (Util_FileExists(path)) {
        SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path);
    }
    SessionStore_Free(&set);
}

static void TestDeletionList(const ProfileList *profiles, const WCHAR *projects)
{
    enum { COPIES = 56 };
    SessionSet set;
    WCHAR file[LONG_PATH_CCH], child[LONG_PATH_CCH], error[1024], (*paths)[LONG_PATH_CCH] = NULL;
    BOOL ready = TRUE, intact = TRUE;
    int copy, count = 0, row;
    for (copy = 0; copy < COPIES && ready; copy++) {
        ready = SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\deletion-%02d\\%s.jsonl", projects, copy, g_projectId)) &&
                Save(file, "{}\n") &&
                SUCCEEDED(StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\deletion-%02d\\%s\\sentinel.txt", projects, copy, g_projectId)) &&
                Save(child, "subagent fixture\n");
    }
    Check("many-project deletion fixtures prepared", ready);
    if (!ready) return;
    Check("many-project deletion snapshot loads", SessionStore_LoadProfiles(&set, profiles));
    row = FindRow(&set, g_projectId);
    Check("deletion collection finds conversation", row >= 0);
    if (row >= 0) {
        Check("deletion collection completes", SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)));
        Check("deletion collection retains every path beyond its initial capacity", count == COPIES * 2 + 2);
        for (copy = 0; copy < count; copy++)
            if (!Core_PathUnder(paths[copy], g_root) || (!FileThere(paths[copy]) && !DirThere(paths[copy]))) intact = FALSE;
        Check("deletion preparation leaves all private files intact", intact);
        if (paths) HeapFree(GetProcessHeap(), 0, paths);
        paths = NULL;
        {
            /* A project folder whose transcript paths are longer than MAX_PATH. */
            WCHAR leaf[MAX_PATH], longDir[LONG_PATH_CCH], transcript[LONG_PATH_CCH];
            size_t length = wcslen(projects) + 24 < MAX_PATH ? MAX_PATH - wcslen(projects) - 24 : 0;
            wmemset(leaf, L'x', length);
            leaf[length] = 0;
            ready = length > 0 && Join(projects, leaf, longDir, ARRAYSIZE(longDir)) && MakeDir(longDir);
            Check("long project directory fixture created", ready);
            if (ready) {
                Check("an unrelated long project folder does not stop deletion",
                      SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == COPIES * 2 + 2 && !error[0]);
                if (paths) HeapFree(GetProcessHeap(), 0, paths);
                paths = NULL;
                ready = SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\%s.jsonl", longDir, g_projectId)) &&
                        wcslen(transcript) >= MAX_PATH && Save(transcript, "{}\n");
                Check("a transcript past MAX_PATH fixture", ready);
                Check("a transcript past MAX_PATH is listed for deletion",
                      ready && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == COPIES * 2 + 3 &&
                      Listed(paths, count, transcript));
                if (paths) HeapFree(GetProcessHeap(), 0, paths);
                paths = NULL;
                Check("long project directory fixture removed", RemoveFixture(longDir));
            }
        }
        Check("failed or partial preparation leaves the original entry intact", Util_FileExists(set.entries[set.rows[row].entry[0]].file));
    }
    SessionStore_Free(&set);
}

/* A session's other transcripts (earlier ones, the one before a /clear) and
 * what Claude Code keeps for each, a transcript another session goes on
 * with, and a transcript filed in two project folders. */
static void TestOtherTranscripts(const WCHAR *projects)
{
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], code[MAX_PATH], path[LONG_PATH_CCH], error[1024], entry[MAX_PATH];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    char extra[512];
    int row, count = 0;
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(&profiles.items[0], L"Lineage", L"lineage\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            SUCCEEDED(StringCchPrintfA(extra, sizeof extra, ",\"priorCliSessionIds\":[\"%ls\", \"%ls\",\"not-a-uuid\"],\"preClearCliSessionId\":\"%ls\"",
                                       g_priorId, g_claimedId, g_preClearId)) &&
            WriteEntry(entries, g_lineageId, extra) && WriteEntry(entries, g_claimedId, "") && WriteEntry(entries, g_duplicateId, "") &&
            EntryPath(entries, g_lineageId, entry, ARRAYSIZE(entry)) && CodeDir(projects, code, ARRAYSIZE(code)) &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_lineageId)) && Save(path, "0123456789") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_priorId)) && Save(path, "01234567890123456789") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_preClearId)) &&
            Save(path, "0123456789012345678901234567890123456789") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_claimedId)) && Save(path, "claimed") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\file-history\\%s\\snapshot", code, g_lineageId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\session-env\\%s\\environment", code, g_priorId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\debug\\%s.txt", code, g_lineageId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\file-history\\%s\\snapshot", code, g_claimedId)) && Save(path, "x") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.desktop-released.json", projects, g_lineageId)) && Save(path, "{}") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\dup-small\\%s.jsonl", projects, g_duplicateId)) && Save(path, "12345") &&
            SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\dup-large\\%s.jsonl", projects, g_duplicateId)) &&
            Save(path, "12345678901234567890123456789012345678901234567890");
    ready = ready && SessionStore_LoadProfiles(&set, &profiles);
    Check("other transcript fixtures ready", ready);
    if (!ready) return;
    row = FindRow(&set, g_lineageId);
    Check("a conversation's size counts its other transcripts, not another session's", row >= 0 && set.rows[row].transcriptBytes == 70);
    Check("a session's other transcripts list for deletion with what Claude Code keeps for them",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 8 && Listed(paths, count, entry));
    if (paths) {
        const WCHAR *const expected[] = { L"lineage\\aaaaaaaa-0000-4000-8000-000000000001.jsonl",
                                          L"lineage\\aaaaaaaa-0000-4000-8000-000000000002.jsonl",
                                          L"lineage\\aaaaaaaa-0000-4000-8000-000000000003.jsonl",
                                          L"lineage\\aaaaaaaa-0000-4000-8000-000000000001.desktop-released.json" };
        const WCHAR *const stores[] = { L"file-history\\aaaaaaaa-0000-4000-8000-000000000001",
                                        L"session-env\\aaaaaaaa-0000-4000-8000-000000000002",
                                        L"debug\\aaaaaaaa-0000-4000-8000-000000000001.txt" };
        BOOL all = TRUE;
        int i;
        for (i = 0; i < (int)ARRAYSIZE(expected); i++)
            all = all && Join(projects, expected[i], path, ARRAYSIZE(path)) && Listed(paths, count, path);
        for (i = 0; i < (int)ARRAYSIZE(stores); i++)
            all = all && Join(code, stores[i], path, ARRAYSIZE(path)) && Listed(paths, count, path);
        Check("the transcripts, Claude's marks and Claude Code's folders of the session are listed", all);
        Check("a transcript another session goes on with stays, with its folders",
              SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\lineage\\%s.jsonl", projects, g_claimedId)) && !Listed(paths, count, path) &&
              SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\file-history\\%s", code, g_claimedId)) && !Listed(paths, count, path));
        HeapFree(GetProcessHeap(), 0, paths);
        paths = NULL;
    }
    row = FindRow(&set, g_duplicateId);
    Check("a transcript filed twice is the larger one",
          row >= 0 && set.rows[row].transcriptBytes == 50 && Core_EndsWithI(set.rows[row].transcriptPath, L"\\dup-large\\aaaaaaaa-0000-4000-8000-000000000005.jsonl"));
    Check("both copies of a transcript filed twice are listed for deletion",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 3);
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    SessionStore_Free(&set);
}

/* Entries counted, not listed: remote sessions (moved to the cloud: set,
 * whatever its form), files that are no entry and entries that cannot be
 * read; with either of the last two, the read is not complete. */
static void TestEntryCounts(void)
{
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], locked[MAX_PATH], broken[MAX_PATH];
    HANDLE file;
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(&profiles.items[0], L"Counts", L"counts\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            WriteEntry(entries, g_editIds[0], ",\"movedToCloud\":false") &&
            WriteEntry(entries, g_editIds[1], ",\"sshConfig\":{\"host\":\"fixture\"}") &&
            WriteEntry(entries, g_editIds[2], ",\"cloudSessionId\":\"fixture\"") &&
            WriteEntry(entries, g_priorId, ",\"movedToCloud\":{\"sessionId\":\"fixture\"}") &&
            WriteEntry(entries, g_preClearId, ",\"movedToCloud\":null") &&
            SaveIn(entries, L"local_not-an-entry.json", "{\"title\":\"no session\"}") && Join(entries, L"local_not-an-entry.json", broken, ARRAYSIZE(broken)) &&
            WriteEntry(entries, g_unlistedId, "") && EntryPath(entries, g_unlistedId, locked, ARRAYSIZE(locked));
    Check("entry count fixtures ready", ready);
    if (!ready) return;
    file = CreateFileW(locked, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ready = file != INVALID_HANDLE_VALUE && SessionStore_LoadProfiles(&set, &profiles);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Check("entry count snapshot loads", ready);
    if (!ready) return;
    Check("sessions run elsewhere are counted, not listed", set.source[0].elsewhere == 3);
    Check("a file that is no entry and an entry that cannot be read are counted", set.source[0].unreadable == 2);
    Check("only this PC's readable sessions are listed",
          set.rowCount == 2 && FindRow(&set, g_editIds[0]) >= 0 && FindRow(&set, g_preClearId) >= 0);
    SessionStore_Free(&set);
    Check("a file that is no entry leaves the read incomplete", !SessionStore_LoadEntries(&set, &profiles.items[0]));
    SessionStore_Free(&set);
    Check("a profile's entries all read count as complete",
          DeleteFileW(broken) && SessionStore_LoadEntries(&set, &profiles.items[0]) && set.rowCount == 3);
    SessionStore_Free(&set);
}

/* A record of a running Claude Code for `sessionId`, of process `pid`
 * started at `started` (a FILETIME). */
static BOOL WriteLiveRecord(const WCHAR *code, DWORD pid, const WCHAR *sessionId, ULONGLONG started)
{
    WCHAR path[MAX_PATH];
    char json[256];
    return SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\sessions\\%lu.json", code, pid)) &&
           SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"pid\":%lu,\"sessionId\":\"%ls\",\"procStart\":\"%I64u\"}", pid, sessionId, started)) &&
           Save(path, json);
}

/* Paths past MAX_PATH, an entry's and a transcript's: read, changed, copied,
 * listed; a deletion that would need the Recycle Bin for them is refused, as
 * it is while the session runs. Every file of this session is longer than
 * MAX_PATH, so not even a broken check could move one to the Recycle Bin. */
static void TestLongPaths(const WCHAR *projects)
{
    ProfileList profiles, running;
    SessionSet set;
    PendingEdit edit;
    STARTUPINFOW startup;
    PROCESS_INFORMATION child;
    FILETIME created, exited, kernel, user;
    WCHAR leaf[MAX_PATH], storage[MAX_PATH], entries[LONG_PATH_CCH], entry[LONG_PATH_CCH], folder[LONG_PATH_CCH];
    WCHAR transcript[LONG_PATH_CCH], code[MAX_PATH], exe[MAX_PATH], id[SESSION_ID_CCH], error[2048], title[SESSION_TITLE_CCH];
    WCHAR left[LONG_PATH_CCH];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    HWND window;
    RemoveResult result;
    /* A profile folder of 145 characters: its entries folder (95 more) is
     * shorter than MAX_PATH, an entry in it (48 more) is not. */
    size_t storageLength = MAX_PATH - 115, padding = storageLength > wcslen(g_root) + 1 ? storageLength - wcslen(g_root) - 1 : 0;
    int row, count = 0, i, calls;
    BOOL ready, waiting, outside, loaded;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    wmemset(leaf, L'p', padding);
    leaf[padding] = 0;
    ready = padding > 0 && FixturePath(leaf, storage, ARRAYSIZE(storage)) &&
            SetProfile(&profiles.items[0], L"Long", leaf, FALSE) && Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            SaveIn(storage, L"config.json", "{\"lastKnownAccountUuid\":\"eeeeeeee-0000-4000-8000-000000000001\"}") &&
            SUCCEEDED(StringCchPrintfW(entries, ARRAYSIZE(entries), L"%s\\claude-code-sessions\\%s\\%s", storage, g_accountId, g_organizationId)) &&
            WriteEntry(entries, g_longId, "") && EntryPath(entries, g_longId, entry, ARRAYSIZE(entry)) &&
            wcslen(entries) < MAX_PATH && wcslen(entry) >= MAX_PATH;
    wmemset(leaf, L'q', CORE_PROJECT_NAME_MAX);
    leaf[CORE_PROJECT_NAME_MAX] = 0;
    ready = ready && Join(projects, leaf, folder, ARRAYSIZE(folder)) &&
            SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\%s.jsonl", folder, g_longId)) &&
            Save(transcript, "{\"sessionId\":\"bbbbbbbb-0000-4000-8000-000000000001\",\"cwd\":\"C:\\\\Fixture\"}\n") &&
            wcslen(transcript) >= MAX_PATH && CodeDir(projects, code, ARRAYSIZE(code)) && SessionStore_LoadProfiles(&set, &profiles);
    Check("long path fixtures ready", ready);
    if (!ready) return;
    row = FindRow(&set, g_longId);
    Check("an entry past MAX_PATH is read", row >= 0 && Core_PathEquals(set.entries[set.rows[row].entry[0]].file, entry));
    Check("a transcript past MAX_PATH is found", row >= 0 && set.rows[row].transcript && Core_PathEquals(set.rows[row].transcriptPath, transcript) &&
          set.rows[row].transcriptBytes > 0);
    if (row < 0) {
        SessionStore_Free(&set);
        return;
    }
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_longId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Long title");
    Check("an entry past MAX_PATH is renamed",
          SessionEdit_Change(NULL, &profiles.items[0], &set.entries[set.rows[row].entry[0]], &edit, &waiting, NULL, 0) &&
          !waiting && ReadString(entry, "title", title, ARRAYSIZE(title)) && wcscmp(title, L"Long title") == 0);
    edit.op = PENDING_REMOVE;
    edit.value[0] = 0;
    calls = g_recycle.calls;
    Check("an entry too long for the Recycle Bin is not removed, its path named",
          !SessionEdit_Change(NULL, &profiles.items[0], &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) &&
          !waiting && wcsstr(error, entry) != NULL && FileThere(entry) && g_recycle.calls == calls);
    if (SessionEdit_CopyConversation(NULL, &set, row, 0, id, ARRAYSIZE(id), error, ARRAYSIZE(error)) == COPY_MADE) {
        WCHAR copied[LONG_PATH_CCH];
        Check("a transcript past MAX_PATH is copied beside it with its own id",
              SUCCEEDED(StringCchPrintfW(copied, ARRAYSIZE(copied), L"%s\\%s.jsonl", folder, id)) && FileThere(copied) &&
              ReadString(copied, "sessionId", title, ARRAYSIZE(title)) && wcscmp(title, id) == 0);
        Check("a copy past MAX_PATH that could not be opened is removed",
              SessionEdit_RemoveCopy(&set, row, 0, id, left, ARRAYSIZE(left)) && !FileThere(copied));
    } else Check("a transcript past MAX_PATH is copied", FALSE);
    Check("every file of the session is listed for deletion",
          SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 2 &&
          Listed(paths, count, entry) && Listed(paths, count, transcript));
    for (i = 0; paths && i < count; i++) ready = ready && wcslen(paths[i]) >= MAX_PATH;
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    if (ready) {
        Check("nothing runs the session: it can be deleted", SessionEdit_CanDelete(&set, row, error, ARRAYSIZE(error)) && !error[0]);
        result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
        Check("a deletion needing the Recycle Bin for paths past MAX_PATH is refused with the path",
              result == REMOVE_FAILED && wcsstr(error, entry) != NULL);
        Check("a refused deletion moves nothing", FileThere(entry) && FileThere(transcript) && g_recycle.calls == calls);
    }

    window = StartFakeClaude(&profiles.items[0]);
    if (ready && window && Claude_IsRunning(&profiles.items[0])) {
        Check("a session whose profile runs cannot be deleted, the profile named",
              !SessionEdit_CanDelete(&set, row, error, ARRAYSIZE(error)) && wcsstr(error, profiles.items[0].name) != NULL);
        result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
        Check("deletion is refused while a profile listing the session runs", result == REMOVE_FAILED && error[0] &&
              wcsstr(error, profiles.items[0].name) != NULL && FileThere(entry) && FileThere(transcript));
    } else Check("running profile fixture for deletion", FALSE);
    StopFakeClaude(window);

    /* A Claude Code running the session: a suspended child of this process, recorded as Claude Code records itself. */
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    ZeroMemory(&child, sizeof child);
    ready = Util_SelfExe(exe, ARRAYSIZE(exe)) &&
            CreateProcessW(exe, NULL, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, NULL, &startup, &child);
    if (ready) {
        ready = GetProcessTimes(child.hProcess, &created, &exited, &kernel, &user) &&
                WriteLiveRecord(code, child.dwProcessId, g_longId, ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime) &&
                WriteLiveRecord(code, GetCurrentProcessId(), g_editIds[0], 1);
        Check("running Claude Code fixture", ready);
        running = profiles;
        running.items[0].running = TRUE;
        running.items[0].pid = GetCurrentProcessId();
        SessionStore_Free(&set);
        loaded = ready && SessionStore_LoadProfiles(&set, &running);
        row = loaded ? FindRow(&set, g_longId) : -1;
        if (row >= 0) {
            Check("a session run by a profile's Claude Code is in use there", set.rows[row].live == 1);
            Check("a profile's Claude Code is found running the session", SessionStore_RunningNow(&running, g_longId, &outside) == 1 && !outside);
            Check("a record whose process started at another time does not count", SessionStore_RunningNow(&running, g_editIds[0], &outside) == 0 &&
                  !outside);
        } else Check("in-use snapshot loads", FALSE);
        if (loaded) SessionStore_Free(&set);
        loaded = ready && SessionStore_LoadProfiles(&set, &profiles);
        row = loaded ? FindRow(&set, g_longId) : -1;
        if (row >= 0 && SessionStore_RunningNow(&profiles, g_longId, &outside) == 0 && outside) {
            Check("a session a Claude Code of no profile runs cannot be deleted", !SessionEdit_CanDelete(&set, row, error, ARRAYSIZE(error)) && error[0]);
            result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
            Check("deletion is refused while a Claude Code of no profile runs the session",
                  result == REMOVE_FAILED && error[0] && FileThere(entry) && FileThere(transcript));
        } else Check("Claude Code of no profile fixture", FALSE);
        TerminateProcess(child.hProcess, 0);
        WaitForSingleObject(child.hProcess, 5000);
        CloseHandle(child.hThread);
        CloseHandle(child.hProcess);
    } else Check("suspended Claude Code fixture started", FALSE);
    SessionStore_Free(&set);
    Check("long path fixtures removed", RemoveFixture(folder) && RemoveFixture(storage));
}

static ULONGLONG NowMs(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - 116444736000000000ULL) / 10000;
}

/* The time in ms that Claude's mark of session `id` deleted, beside the
 * entries `entries`, holds; 0 when there is no such mark. */
static ULONGLONG DeletedMark(const WCHAR *entries, const WCHAR *id)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    ULONGLONG ms = 0;
    char *text;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\deleted_%s", entries, id)) ||
        (text = Util_ReadFile(path, 64, FALSE, &length)) == NULL) return 0;
    if (strspn(text, "0123456789") == length) ms = _strtoui64(text, NULL, 10);
    HeapFree(GetProcessHeap(), 0, text);
    return ms;
}

static int CountMarks(const WCHAR *entries)
{
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(entries, L"deleted_*", &found, FALSE);
    int count = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do count++; while (FindNextFileW(find, &found));
    FindClose(find);
    return count;
}

static BOOL RemoveMarks(const WCHAR *entries)
{
    WCHAR path[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(entries, L"deleted_*", &found, FALSE);
    BOOL ok = TRUE;
    if (find == INVALID_HANDLE_VALUE) return TRUE;
    do ok = Join(entries, found.cFileName, path, ARRAYSIZE(path)) && RemoveFixture(path) && ok; while (FindNextFileW(find, &found));
    FindClose(find);
    return ok;
}

static void MakeRemoval(PendingEdit *edit, const WCHAR *key)
{
    ZeroMemory(edit, sizeof *edit);
    edit->op = PENDING_REMOVE;
    StringCchCopyW(edit->key, ARRAYSIZE(edit->key), key);
}

/* Removing a session from one profile: its entries to the Recycle Bin, and
 * Claude's marks that it was deleted there, for each id of the session no
 * other session claims; declined at Windows' question, nothing; with Claude
 * started meanwhile, it waits for that Claude to close. */
static void TestRemovals(void)
{
    ProfileList profiles;
    Profile *profile = &profiles.items[0];
    SessionSet set;
    PendingEdit edit;
    WCHAR entries[MAX_PATH], head[MAX_PATH], other[MAX_PATH], third[MAX_PATH], pending[MAX_PATH], error[512];
    HWND window;
    char extra[512], json[1024];
    ULONGLONG before, after, mark;
    int row;
    BOOL ready, waiting, made;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    /* Session A, listed twice (its other entry under its own id), naming an
     * earlier transcript and one session B goes on with; and session C. */
    ready = PrepareProfile(profile, L"Removals", L"removals\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            SessionStore_PendingPath(profile, pending, ARRAYSIZE(pending)) &&
            SUCCEEDED(StringCchPrintfA(extra, sizeof extra, ",\"priorCliSessionIds\":[\"%ls\",\"%ls\"]", g_priorId, g_claimedId)) &&
            WriteEntry(entries, g_lineageId, extra) && EntryPath(entries, g_lineageId, head, ARRAYSIZE(head)) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\","
                                                          "\"lastActivityAt\":100}", g_editIds[0], g_lineageId)) &&
            EntryPath(entries, g_editIds[0], other, ARRAYSIZE(other)) && Save(other, json) &&
            WriteEntry(entries, g_claimedId, "") && WriteEntry(entries, g_duplicateId, "") && EntryPath(entries, g_duplicateId, third, ARRAYSIZE(third)) &&
            SessionStore_LoadProfiles(&set, &profiles);
    Check("removal fixtures ready", ready);
    if (!ready) return;
    row = FindRow(&set, g_lineageId);
    if (row < 0 || FindRow(&set, g_duplicateId) < 0) {
        Check("removal fixture sessions listed", FALSE);
        SessionStore_Free(&set);
        return;
    }

    /* What waited for A under any of its keys goes with it. */
    window = StartFakeClaude(profile);
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchPrintfW(edit.key, ARRAYSIZE(edit.key), L"local_%s", g_editIds[0]);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Queued under the other entry's own id");
    ready = window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    edit.op = PENDING_STAR;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_lineageId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"1");
    ready = ready && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    Check("changes wait for a session under two of its keys", ready);
    RecycleWill(REMOVE_DONE, NULL);
    MakeRemoval(&edit, g_lineageId);
    before = NowMs();
    made = SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) && !waiting;
    after = NowMs();
    Check("a session is removed from a closed profile", made && !g_recycle.escaped && !FileThere(head) && !FileThere(other));
    Check("both of its entries go to the Recycle Bin together", g_recycle.calls == 1 && g_recycle.count == 2 && Recycled(head) && Recycled(other));
    mark = DeletedMark(entries, g_lineageId);
    Check("Claude's mark that it was deleted holds the time in ms", mark >= before && mark <= after);
    Check("each id of the session is marked once: its entries' own ids and its earlier transcript",
          DeletedMark(entries, g_editIds[0]) == mark && DeletedMark(entries, g_priorId) == mark);
    Check("a transcript another session goes on with is not marked", DeletedMark(entries, g_claimedId) == 0 && CountMarks(entries) == 3);
    Check("nothing waits any more for the removed session, under any of its keys", !Util_FileExists(pending));

    /* C: declined at Windows' question. */
    SessionStore_Free(&set);
    ready = SessionStore_LoadProfiles(&set, &profiles) && (row = FindRow(&set, g_duplicateId)) >= 0 && RemoveMarks(entries);
    Check("removal fixture read again", ready);
    if (!ready) {
        SessionStore_Free(&set);
        return;
    }
    RecycleWill(REMOVE_CANCELLED, NULL);
    MakeRemoval(&edit, g_duplicateId);
    Check("a removal declined at Windows' question is no failure, and does not wait",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) && !waiting &&
          !error[0] && g_recycle.calls == 1);
    Check("a declined removal leaves the entry, marks nothing and queues nothing",
          FileThere(third) && CountMarks(entries) == 0 && !Util_FileExists(pending));

    /* C again: its Claude starts while Windows asks; it then keeps the session in memory. */
    RecycleWill(REMOVE_DONE, profile);
    Check("a removal overtaken by its Claude starting waits for that Claude",
          SessionEdit_Change(NULL, profile, &set.entries[set.rows[row].entry[0]], &edit, &waiting, error, ARRAYSIZE(error)) && waiting &&
          RemovalQueued(profile));
    Check("a removal overtaken marks nothing", CountMarks(entries) == 0);
    StopStartedClaude();
    Check("its Claude wrote the session back as it closed", WriteEntry(entries, g_duplicateId, ""));
    RecycleWill(REMOVE_DONE, NULL);
    before = NowMs();
    Check("the waiting removal is made once the profile is closed",
          SessionEdit_ApplyPending(NULL, profile) == 1 && !FileThere(third) && Recycled(third) && !Util_FileExists(pending));
    mark = DeletedMark(entries, g_duplicateId);
    Check("a waiting removal made writes Claude's mark", mark >= before && mark <= NowMs() && CountMarks(entries) == 1);

    /* C once more, queued while its profile runs; its Claude starts again while Windows asks. */
    ready = RemoveMarks(entries) && WriteEntry(entries, g_duplicateId, "");
    window = StartFakeClaude(profile);
    ready = ready && window && SessionEdit_Change(NULL, profile, NULL, &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    Check("a removal waits while its profile runs", ready);
    RecycleWill(REMOVE_DONE, profile);
    Check("a waiting removal overtaken by its Claude starting waits again, unmarked",
          SessionEdit_ApplyPending(NULL, profile) == 0 && RemovalQueued(profile) && CountMarks(entries) == 0);
    StopStartedClaude();
    Check("once that Claude has run without the session, its removal waits no more",
          SessionEdit_ApplyPendingAfterRun(profile) == 1 && !RemovalQueued(profile) && CountMarks(entries) == 0);
    RecycleWill(REMOVE_DONE, NULL);
    SessionStore_Free(&set);
}

/* Delete everywhere: every entry, the conversation and everything Claude's
 * own delete removes with it, in one go; nothing when Windows' question is
 * declined; with a Claude started meanwhile, its entry there goes when it
 * closes. */
static void TestDeleteEverywhere(const WCHAR *projects)
{
    ProfileList profiles;
    SessionSet set;
    PendingEdit edit;
    WCHAR entries[2][MAX_PATH], files[2][MAX_PATH], staged[MAX_PATH], scratch[MAX_PATH], working[MAX_PATH], subfolder[MAX_PATH];
    WCHAR temporary[MAX_PATH], temporaryFolder[MAX_PATH], longTemporary[LONG_PATH_CCH], leaf[MAX_PATH], neighbor[MAX_PATH];
    WCHAR transcript[MAX_PATH], preImport[MAX_PATH], sessionFolder[MAX_PATH], physical[MAX_PATH], pending[MAX_PATH], error[1024];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    const WCHAR *listed[8];
    char quoted[MAX_PATH * 6 + 4], extra[MAX_PATH * 6 + 64];
    HWND window;
    RemoveResult result;
    size_t padding;
    int p, row, count = 0, i;
    BOOL ready = TRUE, all, waiting;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (p = 0; p < 2 && ready; p++)
        ready = PrepareProfile(&profiles.items[p], p ? L"DeleteB" : L"DeleteA", p ? L"delete\\b" : L"delete\\a", g_accountId, g_organizationId,
                               entries[p], MAX_PATH);
    /* D: listed in both profiles, without a folder in A's "no folder" area;
     * the transcript A staged for it; Claude Code's transcript, the one it
     * kept before taking it in, its folder and its temporary folder. */
    ready = ready && SUCCEEDED(StringCchPrintfW(scratch, ARRAYSIZE(scratch), L"%s\\scratch-workspaces\\%s\\%s", profiles.items[0].dataDir,
                                                g_accountId, g_organizationId)) &&
            Join(scratch, L"scratch-delete", working, ARRAYSIZE(working)) && SaveIn(working, L"notes.txt", "scratch\n") &&
            Join(working, L"sub", subfolder, ARRAYSIZE(subfolder)) &&
            SUCCEEDED(StringCchPrintfW(staged, ARRAYSIZE(staged), L"%s\\imported-staging\\%s.jsonl", entries[0], g_deletedId)) && Save(staged, "{}\n") &&
            Core_JsonQuote(staged, quoted, sizeof quoted) &&
            SUCCEEDED(StringCchPrintfA(extra, sizeof extra, ",\"stagedTranscriptPath\":%s", quoted)) &&
            WriteEntryIn(entries[0], g_deletedId, working, extra) && WriteEntryIn(entries[1], g_deletedId, working, "") &&
            EntryPath(entries[0], g_deletedId, files[0], MAX_PATH) && EntryPath(entries[1], g_deletedId, files[1], MAX_PATH) &&
            WriteEntryIn(entries[0], g_neighborId, subfolder, "") && EntryPath(entries[0], g_neighborId, neighbor, ARRAYSIZE(neighbor)) &&
            WriteEntry(entries[0], g_startedId, "") && WriteEntry(entries[1], g_startedId, "") &&
            SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\delete\\%s.jsonl", projects, g_deletedId)) && Save(transcript, "{}\n") &&
            SUCCEEDED(StringCchPrintfW(preImport, ARRAYSIZE(preImport), L"%s.pre-import", transcript)) && Save(preImport, "{}\n") &&
            SUCCEEDED(StringCchPrintfW(sessionFolder, ARRAYSIZE(sessionFolder), L"%s\\delete\\%s", projects, g_deletedId)) &&
            SaveIn(sessionFolder, L"subagent.jsonl", "{}\n") &&
            GetEnvironmentVariableW(L"CLAUDE_CODE_TMPDIR", temporary, ARRAYSIZE(temporary)) > 0 &&
            SUCCEEDED(StringCchPrintfW(temporaryFolder, ARRAYSIZE(temporaryFolder), L"%s\\claude\\C--Fixture\\%s", temporary, g_deletedId)) &&
            SaveIn(temporaryFolder, L"output.txt", "task\n") && SessionStore_PendingPath(&profiles.items[1], pending, ARRAYSIZE(pending));
    /* A temporary folder too long for the Recycle Bin, left to Claude Code. */
    padding = wcslen(temporary) + 40 < MAX_PATH ? MAX_PATH - wcslen(temporary) - 40 : 1;
    wmemset(leaf, L't', padding);
    leaf[padding] = 0;
    ready = ready && SUCCEEDED(StringCchPrintfW(longTemporary, ARRAYSIZE(longTemporary), L"%s\\claude\\%s\\%s", temporary, leaf, g_deletedId)) &&
            wcslen(longTemporary) >= MAX_PATH && MakeDir(longTemporary);
    /* A title waiting for D in B. */
    window = ready ? StartFakeClaude(&profiles.items[1]) : NULL;
    ZeroMemory(&edit, sizeof edit);
    edit.op = PENDING_TITLE;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), g_deletedId);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), L"Waiting title");
    ready = ready && window && SessionEdit_Change(NULL, &profiles.items[1], NULL, &edit, &waiting, NULL, 0) && waiting;
    StopFakeClaude(window);
    ready = ready && SessionStore_LoadProfiles(&set, &profiles);
    Check("delete everywhere fixtures ready", ready);
    listed[0] = files[0];
    listed[1] = files[1];
    listed[2] = staged;
    listed[3] = working;            /* folders: 3, 4 and 7 */
    listed[4] = temporaryFolder;
    listed[5] = transcript;
    listed[6] = preImport;
    listed[7] = sessionFolder;
    if (!ready) return;
    row = FindRow(&set, g_deletedId);
    Check("a session another one works under keeps its working folder",
          row >= 0 && !SessionEdit_RemovesWorkingFolder(&set, row, physical, ARRAYSIZE(physical)));
    Check("so its working folder is not listed for deletion",
          row >= 0 && SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == 7 && !Listed(paths, count, working));
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    paths = NULL;
    SessionStore_Free(&set);
    ready = RemoveFixture(neighbor) && SessionStore_LoadProfiles(&set, &profiles) && (row = FindRow(&set, g_deletedId)) >= 0;
    Check("delete everywhere fixture without the other session", ready);
    if (!ready) {
        SessionStore_Free(&set);
        return;
    }
    Check("a session without a folder takes its working folder with it",
          SessionEdit_RemovesWorkingFolder(&set, row, physical, ARRAYSIZE(physical)) && Core_PathEquals(physical, working));
    all = SessionEdit_ListFiles(&set, row, &paths, &count, error, ARRAYSIZE(error)) && count == (int)ARRAYSIZE(listed);
    for (i = 0; all && i < (int)ARRAYSIZE(listed); i++) all = Listed(paths, count, listed[i]);
    Check("the deletion lists the entries, the staged transcript, the working folder, the transcript kept before, the temporary folder", all);
    Check("a temporary folder too long for the Recycle Bin is not listed", paths && !Listed(paths, count, longTemporary));
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    paths = NULL;

    RecycleWill(REMOVE_CANCELLED, NULL);
    result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
    all = result == REMOVE_CANCELLED && g_recycle.calls == 1 && g_recycle.count == (int)ARRAYSIZE(listed);
    for (i = 0; all && i < (int)ARRAYSIZE(listed); i++) all = Recycled(listed[i]) && PathThere(listed[i], i == 3 || i == 4 || i == 7);
    Check("a deletion declined at Windows' question leaves everything", all);
    Check("a declined deletion marks nothing and keeps what waits",
          CountMarks(entries[0]) == 0 && CountMarks(entries[1]) == 0 && Util_FileExists(pending));

    RecycleWill(REMOVE_DONE, NULL);
    result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
    all = result == REMOVE_DONE && !g_recycle.escaped && g_recycle.calls == 1 && g_recycle.count == (int)ARRAYSIZE(listed);
    for (i = 0; all && i < (int)ARRAYSIZE(listed); i++) all = !PathThere(listed[i], i == 3 || i == 4 || i == 7);
    Check("a session is deleted everywhere in one go", all && !error[0]);
    Check("each profile that listed it gets Claude's mark",
          DeletedMark(entries[0], g_deletedId) > 0 && DeletedMark(entries[1], g_deletedId) > 0 && CountMarks(entries[0]) == 1 && CountMarks(entries[1]) == 1);
    Check("what waited for it is dropped", !Util_FileExists(pending));
    Check("a temporary folder too long for the Recycle Bin is left", DirThere(longTemporary));
    SessionStore_Free(&set);

    /* B's Claude starts while Windows asks. */
    ready = SessionStore_LoadProfiles(&set, &profiles) && (row = FindRow(&set, g_startedId)) >= 0;
    Check("started-during-deletion fixture read", ready);
    if (ready) {
        RecycleWill(REMOVE_DONE, &profiles.items[1]);
        result = SessionEdit_DeleteEverywhere(NULL, &set, row, error, ARRAYSIZE(error));
        Check("a deletion overtaken by a profile starting is made", result == REMOVE_DONE && g_recycle.count == 2);
        Check("the profile that did not start gets Claude's mark", DeletedMark(entries[0], g_startedId) > 0);
        Check("the profile that started waits to remove the entry its Claude keeps, unmarked",
              RemovalQueued(&profiles.items[1]) && DeletedMark(entries[1], g_startedId) == 0);
        StopStartedClaude();
        Check("once that Claude has run without the session, its removal waits no more",
              SessionEdit_ApplyPendingAfterRun(&profiles.items[1]) == 1 && !RemovalQueued(&profiles.items[1]));
    }
    RecycleWill(REMOVE_DONE, NULL);
    SessionStore_Free(&set);
}

typedef struct ConcurrentRead {
    const ProfileList *profiles;
    HANDLE start;
    BOOL valid;
} ConcurrentRead;

static DWORD WINAPI ReadFixtures(void *param)
{
    ConcurrentRead *read = (ConcurrentRead *)param;
    int pass;
    read->valid = TRUE;
    WaitForSingleObject(read->start, INFINITE);
    for (pass = 0; pass < 12; pass++) {
        SessionSet set;
        int r;
        if (!SessionStore_LoadProfiles(&set, read->profiles)) { read->valid = FALSE; break; }
        if (set.rowCount != 2 || set.groupCount != 2 || set.entryCount != 2) read->valid = FALSE;
        for (r = 0; r < set.rowCount; r++)
            if (set.rows[r].group < 0 || set.rows[r].group >= set.groupCount || set.rows[r].entry[0] < 0)
                read->valid = FALSE;
        SessionStore_Free(&set);
    }
    return 0;
}

/* A read cancelled while it goes: the cancel handle is a change
 * notification on the state folder, which the read signals itself when it
 * logs where the first profile's entries are. */
static void TestCancelledRead(void)
{
    enum { SESSIONS = 32 };
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], state[MAX_PATH], id[SESSION_ID_CCH];
    HANDLE change;
    BOOL ready, cancelled;
    int i;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = PrepareProfile(&profiles.items[0], L"Cancel", L"cancel\\profile", g_accountId, g_organizationId, entries, ARRAYSIZE(entries)) &&
            Util_StateDir(state, ARRAYSIZE(state));
    for (i = 0; ready && i < SESSIONS; i++)
        ready = SUCCEEDED(StringCchPrintfW(id, ARRAYSIZE(id), L"%08x-5555-4555-8555-555555555555", i)) && WriteEntry(entries, id, "");
    Check("cancelled read fixtures ready", ready);
    if (!ready) return;
    change = FindFirstChangeNotificationW(state, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    Check("cancel notification created", change != INVALID_HANDLE_VALUE);
    if (change == INVALID_HANDLE_VALUE) return;
    ready = WaitForSingleObject(change, 0) == WAIT_TIMEOUT;
    cancelled = !SessionStore_LoadProfilesCancel(&set, &profiles, change);
    Check("a read cancelled while it goes publishes nothing",
          ready && cancelled && WaitForSingleObject(change, 0) == WAIT_OBJECT_0 && !set.entries && !set.rows && !set.groups && set.rowCount == 0);
    if (!cancelled) SessionStore_Free(&set);
    FindCloseChangeNotification(change);
    Check("the same read not cancelled is complete", SessionStore_LoadProfiles(&set, &profiles) && set.rowCount == SESSIONS);
    SessionStore_Free(&set);
}

static void TestConcurrentReads(const ProfileList *profiles)
{
    HANDLE start = CreateEventW(NULL, TRUE, FALSE, NULL), threads[2] = { NULL, NULL };
    ConcurrentRead read[2];
    SessionSet set;
    int i;
    Check("cancellation event created", start != NULL);
    if (!start) return;
    SetEvent(start);
    Check("a read cancelled before it starts returns no partial data", !SessionStore_LoadProfilesCancel(&set, profiles, start) &&
          !set.entries && !set.rows && !set.groups && set.rowCount == 0);
    ResetEvent(start);
    for (i = 0; i < 2; i++) {
        read[i].profiles = profiles;
        read[i].start = start;
        read[i].valid = FALSE;
        threads[i] = CreateThread(NULL, 0, ReadFixtures, &read[i], 0, NULL);
        Check("concurrent fixture reader created", threads[i] != NULL);
    }
    SetEvent(start);
    for (i = 0; i < 2; i++) {
        if (threads[i]) {
            WaitForSingleObject(threads[i], INFINITE);
            Check("concurrent reader keeps its own rows and groups", read[i].valid);
            CloseHandle(threads[i]);
        }
    }
    CloseHandle(start);
}

/* Wait on the window queue, with a bounded test deadline. */
static BOOL WaitForSnapshot(HWND window, BOOL apply)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    MSG message;
    for (;;) {
        ULONGLONG now;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.hwnd == window && message.message == WM_APP_SESSIONS) {
                SessionsView_Reload();
                continue;
            }
            if (message.hwnd == window && message.message == WM_APP_SESSIONS_READY) {
                SessionsView_Ready(apply);
                return TRUE;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        now = GetTickCount64();
        if (now >= deadline) return FALSE;
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static BOOL WaitForViewChange(HWND window, HWND tree, int expected, BOOL notificationOnly)
{
    ULONGLONG deadline = GetTickCount64() + 5000;
    MSG message;
    for (;;) {
        ULONGLONG now;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            if (message.hwnd == window && message.message == WM_APP_SESSIONS) {
                SessionsView_Reload();
                if (notificationOnly) return TRUE;
                continue;
            }
            if (message.hwnd == window && message.message == WM_APP_SESSIONS_READY) {
                SessionsView_Ready(TRUE);
                if (!notificationOnly && TreeView_GetCount(tree) == (UINT)expected) return TRUE;
                continue;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        now = GetTickCount64();
        if (now >= deadline) return FALSE;
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static void TestBackgroundView(const ProfileList *profiles)
{
    static const int ids[] = { IDC_S_PROFILES, IDC_S_TREE, IDC_S_SEARCH, IDC_S_FIND, IDC_S_FILTER };
    static const WCHAR *const classes[] = { L"LISTBOX", WC_TREEVIEWW, L"EDIT", L"BUTTON", L"BUTTON" };
    HWND window, tree;
    ClaudePackage package;
    ProfileList newer;
    INITCOMMONCONTROLSEX controls = { sizeof controls, ICC_TREEVIEW_CLASSES };
    DWORD handlesBefore = 0, handlesAfter = 0, handlesAgain = 0;
    int i;
    /* The view makes what waits with sessionedit.c itself: never a removal here (the real Recycle Bin). */
    for (i = 0; i < profiles->count; i++)
        if (RemovalQueued(&profiles->items[i])) {
            Check("no removal waits for the view's profiles", FALSE);
            return;
        }
    ZeroMemory(&package, sizeof package);
    g_hInst = GetModuleHandleW(NULL);
    InitCommonControlsEx(&controls);
    window = CreateWindowExW(0, L"STATIC", L"Session test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 480, NULL, NULL, g_hInst, NULL);
    Check("background view fixture window created", window != NULL);
    if (!window) return;
    for (i = 0; i < (int)ARRAYSIZE(ids); i++) {
        DWORD style = WS_CHILD;
        if (ids[i] == IDC_S_PROFILES) style |= LBS_OWNERDRAWFIXED | LBS_NOTIFY;
        CreateWindowExW(0, classes[i], L"", style, i * 100, 0, 100, 300, window, (HMENU)(INT_PTR)ids[i], g_hInst, NULL);
    }
    tree = GetDlgItem(window, IDC_S_TREE);
    SessionsView_Init(window);
    SessionsView_SetProfiles(profiles);
    SessionsView_Warm(profiles);
    Check("background snapshot completes through a window event", WaitForSnapshot(window, TRUE));
    Check("background loading does not fill hidden controls", TreeView_GetCount(tree) == 0);
    SessionsView_Enter(&package, profiles->items[0].folder);
    Check("cached snapshot renders both fixture conversations", TreeView_GetCount(tree) == 4);
    Check("account switch fixture config written", SaveIn(profiles->items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"account-old\"}"));
    Check("config change reloads the active account through a window event", WaitForViewChange(window, tree, 2, FALSE));
    Check("account fixture config switched back", SaveIn(profiles->items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"account-a\"}"));
    Check("restored account reloads without a focus change", WaitForViewChange(window, tree, 4, FALSE));
    {
        WCHAR projects[MAX_PATH], transcript[MAX_PATH];
        HANDLE file;
        DWORD written;
        BOOL ready = SessionStore_ProjectsDir(projects, ARRAYSIZE(projects)) &&
                     SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\fixture\\%s.jsonl", projects, g_scratchId));
        file = ready ? CreateFileW(transcript, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        ready = file != INVALID_HANDLE_VALUE && WriteFile(file, "{}\n", 3, &written, NULL) && written == 3;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Check("existing transcript fixture appended", ready);
        Check("transcript append produces a sessions window event", ready && WaitForViewChange(window, tree, 0, TRUE));
        Check("transcript write snapshot completes", WaitForSnapshot(window, TRUE));
    }
    SessionsView_Leave();
    newer = *profiles;
    newer.count = 0;
    SessionsView_SetProfiles(&newer);
    Check("ready can defer a snapshot during a modal", WaitForSnapshot(window, FALSE));
    Check("deferred snapshot leaves existing control contents", TreeView_GetCount(tree) == 4);
    SessionsView_Ready(TRUE);
    SessionsView_Enter(&package, NULL);
    Check("deferred empty snapshot replaces the cache on return", TreeView_GetCount(tree) == 0);
    SessionsView_SetProfiles(profiles);
    SessionsView_SetProfiles(&newer);
    Check("rapid profile changes publish the latest snapshot", WaitForSnapshot(window, TRUE));
    Check("obsolete background data does not return to the tree", TreeView_GetCount(tree) == 0);
    SessionsView_SetProfiles(profiles);
    GetProcessHandleCount(GetCurrentProcess(), &handlesBefore);
    SessionsView_Destroy();            /* cancel a read while the window is still alive */
    GetProcessHandleCount(GetCurrentProcess(), &handlesAfter);
    /* Its reader and watcher threads, their events and folder notifications. */
    Check("background shutdown closes its thread and event handles", handlesAfter < handlesBefore);
    /* A whole view lifetime again, what it opens first made once already: every handle it opened is closed. */
    SessionsView_Init(window);
    SessionsView_SetProfiles(profiles);
    SessionsView_Warm(profiles);
    Check("second background snapshot completes", WaitForSnapshot(window, TRUE));
    SessionsView_Destroy();
    GetProcessHandleCount(GetCurrentProcess(), &handlesAgain);
    Check("a view's lifetime leaves no handle open", handlesAgain == handlesAfter);
    DestroyWindow(window);
    Check("background fixture does not create logical stock directory", !Util_DirExists(profiles->items[0].dataDir));
}

static void TestIndexedSessions(void)
{
    enum { ROWS = 384 };
    ProfileList profiles;
    SessionSet first, second;
    WCHAR entries[MAX_PATH], id[SESSION_ID_CCH], file[MAX_PATH];
    char json[512];
    int p, r;
    BOOL ready = TRUE, valid = TRUE;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (p = 0; p < profiles.count && ready; p++) {
        Profile *profile = &profiles.items[p];
        ready = SetProfile(profile, p ? L"Index-B" : L"Index-A", p ? L"index\\B" : L"index\\A", FALSE) &&
                Profiles_ResolveStorage(profile, NULL, NULL) &&
                SaveIn(profile->storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"indexed\"}") &&
                Join(profile->storageDir, L"claude-code-sessions\\indexed\\organization", entries, ARRAYSIZE(entries));
        for (r = 0; r < ROWS && ready; r++) {
            StringCchPrintfW(id, ARRAYSIZE(id), p ? L"%08X-4444-4444-8444-444444444444" : L"%08x-4444-4444-8444-444444444444", r);
            ready = SUCCEEDED(StringCchPrintfA(json, sizeof json,
                "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"title\":\"Row %d\","
                "\"cwd\":\"C:\\\\Fixture\\\\Project%d\",\"lastActivityAt\":%d}", id, id, r, r, r)) &&
                SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, id)) && Save(file, json);
        }
        if (ready) ready = SaveIn(entries, L"local_unicode.json",
            p ? "{\"sessionId\":\"local_unicode\",\"cliSessionId\":\"\\u00c4session\",\"cwd\":\"C:\\\\Unicode\"}"
              : "{\"sessionId\":\"local_unicode\",\"cliSessionId\":\"\\u00e4session\",\"cwd\":\"C:\\\\Unicode\"}");
    }
    Check("indexed session fixtures created", ready);
    if (!ready) return;
    Check("indexed session load completes", SessionStore_LoadProfiles(&first, &profiles));
    Check("case variants and Unicode ids merge across profiles", first.rowCount == ROWS + 1 && first.entryCount == (ROWS + 1) * 2);
    Check("indexed sessions retain hundreds of independent project groups", first.groupCount == ROWS + 1);
    for (r = 0; r < first.rowCount; r++)
        if (first.rows[r].entry[0] < 0 || first.rows[r].entry[1] < 0 || first.rows[r].group < 0 ||
            first.rows[r].group >= first.groupCount) valid = FALSE;
    Check("each indexed conversation keeps both profile entries after sorting", valid);
    Check("indexed reload completes", SessionStore_LoadProfiles(&second, &profiles));
    valid = first.rowCount == second.rowCount;
    for (r = 0; valid && r < first.rowCount; r++)
        if (wcscmp(first.rows[r].key, second.rows[r].key) != 0 || first.rows[r].group != second.rows[r].group) valid = FALSE;
    Check("indexed reload preserves deterministic row and group order", valid);
    SessionStore_Free(&first);
    SessionStore_Free(&second);
}

static void TestGroupAliases(void)
{
    static const WCHAR *const directories[] = {
        L"C:\\Workspace\\\x00c9quipe", L"c:\\workspace\\\x00e9quipe\\", L"C:\\Build", L"c:\\build///", L"", L""
    };
    ProfileList profiles;
    SessionSet set;
    WCHAR entries[MAX_PATH], file[MAX_PATH], ids[6][SESSION_ID_CCH];
    char quoted[MAX_PATH * 6 + 4], json[2048];
    BOOL ready, valid = TRUE;
    int item, rows[6];
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 1;
    ready = SetProfile(&profiles.items[0], L"GroupAliases", L"groups\\aliases", FALSE) &&
            Profiles_ResolveStorage(&profiles.items[0], NULL, NULL) &&
            SaveIn(profiles.items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"groups\"}") &&
            Join(profiles.items[0].storageDir, L"claude-code-sessions\\groups\\organization", entries, ARRAYSIZE(entries));
    for (item = 0; ready && item < (int)ARRAYSIZE(directories); item++) {
        StringCchPrintfW(ids[item], ARRAYSIZE(ids[item]), L"%08x-8888-4888-8888-888888888888", item);
        ready = Core_JsonQuote(directories[item], quoted, sizeof quoted) &&
                SUCCEEDED(StringCchPrintfA(json, sizeof json,
                    "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,\"lastActivityAt\":%d}",
                    ids[item], ids[item], quoted, item)) &&
                SUCCEEDED(StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\local_%s.json", entries, ids[item])) && Save(file, json);
    }
    Check("Unicode and separator group fixtures created", ready);
    if (!ready) return;
    Check("group alias snapshot loads", SessionStore_LoadProfiles(&set, &profiles));
    Check("Unicode case, trailing separators and unknown folders retain three distinct groups", set.groupCount == 3 && set.rowCount == 6);
    for (item = 0; item < (int)ARRAYSIZE(rows); item++) {
        rows[item] = FindRow(&set, ids[item]);
        if (rows[item] < 0) valid = FALSE;
    }
    Check("group alias rows retain all session ids", valid);
    if (valid) {
        Check("Unicode folder case variants share their group", set.rows[rows[0]].group == set.rows[rows[1]].group);
        Check("forward and back trailing separators share their group", set.rows[rows[2]].group == set.rows[rows[3]].group);
        Check("empty working folders share the unknown group", set.rows[rows[4]].group == set.rows[rows[5]].group);
        Check("groups retain first-discovery order before activity sorting",
              set.rows[rows[0]].group == 0 && set.rows[rows[2]].group == 1 && set.rows[rows[4]].group == 2);
    }
    SessionStore_Free(&set);
}

/* ------------------------------------------------------ sessionsync.c */

static const WCHAR *const g_syncIds[6] = {
    L"cccccccc-0000-4000-8000-000000000001", L"cccccccc-0000-4000-8000-000000000002",
    L"cccccccc-0000-4000-8000-000000000003", L"cccccccc-0000-4000-8000-000000000004",
    L"cccccccc-0000-4000-8000-000000000005", L"cccccccc-0000-4000-8000-000000000006"
};

/* An entry of session `id` named `title`, last used at `activity`, archived or not. */
static BOOL WriteSyncEntryAs(const WCHAR *entries, const WCHAR *id, const char *title, int activity, BOOL archived)
{
    WCHAR file[MAX_PATH];
    char json[512];
    return SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\","
                                                         "\"title\":\"%s\",\"isArchived\":%s,\"lastActivityAt\":%d}",
                                      id, id, title, archived ? "true" : "false", activity)) &&
           EntryPath(entries, id, file, ARRAYSIZE(file)) && Save(file, json);
}

static BOOL WriteSyncEntry(const WCHAR *entries, const WCHAR *id, const char *title, int activity)
{
    return WriteSyncEntryAs(entries, id, title, activity, FALSE);
}

static BOOL EntryThere(const WCHAR *entries, const WCHAR *id)
{
    WCHAR path[MAX_PATH];
    return EntryPath(entries, id, path, ARRAYSIZE(path)) && FileThere(path);
}

static BOOL TitledAs(const WCHAR *path, const WCHAR *title)
{
    WCHAR found[SESSION_TITLE_CCH];
    return ReadString(path, "title", found, ARRAYSIZE(found)) && wcscmp(found, title) == 0;
}

static BOOL EntryTitled(const WCHAR *entries, const WCHAR *id, const WCHAR *title)
{
    WCHAR path[MAX_PATH];
    return EntryPath(entries, id, path, ARRAYSIZE(path)) && TitledAs(path, title);
}

/* The copy of profile `p`'s entry of `id` that `report` backed up. */
static BOOL BackedUp(const SyncReport *report, const Profile *p, const WCHAR *id, WCHAR *out, size_t cch)
{
    return report->backup[0] && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s\\local_%s.json", report->backup, p->folder, id)) &&
           FileThere(out);
}

static BOOL MarkPath(const WCHAR *entries, const WCHAR *id, WCHAR *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\deleted_%s", entries, id));
}

/* Merging, overwriting, sharing to a running profile, and an archive taken
 * from one profile into another, on four profiles of their own. */
static void TestSessionSync(const WCHAR *projects)
{
    static const WCHAR *const labels[4] = { L"Sync-A", L"Sync-B", L"Sync-C", L"Sync-D" };
    static const WCHAR *const leaves[4] = { L"sync\\A", L"sync\\B", L"sync\\C", L"sync\\D" };
    static const WCHAR *const accounts[4] = { L"sync-a", L"sync-b", L"sync-c", L"sync-d" };
    ProfileList profiles;
    SessionSet set;
    SyncReport report;
    WCHAR entries[4][MAX_PATH], path[MAX_PATH], archive[MAX_PATH], transcript[MAX_PATH], error[LONG_PATH_CCH];
    char *before = NULL, *after = NULL;
    DWORD beforeLength = 0, afterLength = 0;
    HWND window;
    int p, rows[2], exported = 0, sessions = 0;
    BOOL ready = TRUE, loaded;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 4;
    for (p = 0; p < profiles.count && ready; p++)
        ready = PrepareProfile(&profiles.items[p], labels[p], leaves[p], accounts[p], L"sync-organization", entries[p], ARRAYSIZE(entries[p]));
    ready = ready && WriteSession(entries[0], projects, g_syncIds[0], L"C:\\Fixture") &&
            WriteSyncEntry(entries[1], g_syncIds[1], "B two", 200) && WriteSyncEntry(entries[0], g_syncIds[2], "A three", 300) &&
            WriteSyncEntry(entries[1], g_syncIds[2], "B three", 100);
    Check("session sync fixtures created", ready);
    if (!ready) return;

    /* Merge: each profile gets what it lacks and the latest of what it has. */
    loaded = SessionStore_LoadProfiles(&set, &profiles);
    Check("session sync snapshot loads", loaded && SessionSync_Takers(&set) == 0xF);
    ZeroMemory(&report, sizeof report);
    Check("merging three profiles succeeds", loaded && SessionSync_Merge(&set, 0x7, &report));
    Check("merging adds each missing entry", report.added == 5 && report.failed == 0 && report.waiting == 0);
    Check("merging brings an older entry up to date", report.updated == 1 && EntryTitled(entries[1], g_syncIds[2], L"A three"));
    {
        int detail, added = 0, updated = 0;
        BOOL keyed = report.profileCount == 3;
        for (detail = 0; detail < report.profileCount; detail++) {
            const SyncProfileResult *result = &report.profiles[detail];
            keyed = keyed && Profiles_Find(&profiles, result->folder) >= 0 && !result->waiting && !result->failed;
            added += result->added;
            updated += result->updated;
        }
        Check("merge results identify each recipient with matching totals", keyed && added == report.added && updated == report.updated);
    }
    Check("a merged entry replaced is kept in the backup",
          BackedUp(&report, &profiles.items[1], g_syncIds[2], path, ARRAYSIZE(path)) && TitledAs(path, L"B three"));
    SessionStore_Free(&set);
    loaded = SessionStore_LoadProfiles(&set, &profiles);
    Check("merged profiles list every session", loaded && ProfileRows(&set, 0) == 3 && ProfileRows(&set, 1) == 3 && ProfileRows(&set, 2) == 3);
    Check("a profile left out of the merge is unchanged", loaded && ProfileRows(&set, 3) == 0);
    ZeroMemory(&report, sizeof report);
    Check("merging again changes nothing",
          loaded && SessionSync_Merge(&set, 0x7, &report) && report.added == 0 && report.updated == 0 && report.failed == 0);
    SessionStore_Free(&set);

    /* Moved out of a profile: its entry goes to the backup, Claude's marks stay so that it does not take it in again. */
    ready = WriteSyncEntry(entries[1], g_syncIds[3], "B four", 50);
    Check("removal fixtures created", ready);
    if (!ready) return;
    loaded = SessionStore_LoadProfiles(&set, &profiles);
    rows[0] = loaded ? FindRow(&set, g_syncIds[3]) : -1;
    ZeroMemory(&report, sizeof report);
    Check("taking a session out of a profile succeeds", rows[0] >= 0 && SessionSync_Remove(&set, 0x2, rows, 1, &report) && report.failed == 0);
    Check("the session is taken out, kept in the backup", report.removed == 1 && !EntryThere(entries[1], g_syncIds[3]) &&
          BackedUp(&report, &profiles.items[1], g_syncIds[3], path, ARRAYSIZE(path)));
    Check("Claude's mark that it was deleted there is written", MarkPath(entries[1], g_syncIds[3], path, ARRAYSIZE(path)) && FileThere(path));
    if (loaded) SessionStore_Free(&set);

    /* Share to a running profile: it waits for that profile to close. The
     * entry goes whole: an archived session stays archived. */
    ready = WriteSyncEntryAs(entries[0], g_syncIds[5], "A six", 600, TRUE);
    loaded = ready && SessionStore_LoadProfiles(&set, &profiles);
    window = loaded ? StartFakeClaude(&profiles.items[1]) : NULL;
    Check("sharing fixtures created", window != NULL);
    if (window) {
        rows[0] = FindRow(&set, g_syncIds[5]);
        ZeroMemory(&report, sizeof report);
        Check("sharing to a running profile succeeds", rows[0] >= 0 && SessionSync_Share(&set, 0, rows, 1, 0x6, &report));
        Check("a closed profile takes a shared session at once", report.added == 1 && EntryThere(entries[2], g_syncIds[5]));
        Check("a shared session keeps its archived state",
              EntryPath(entries[2], g_syncIds[5], path, ARRAYSIZE(path)) && ReadTrue(path, "isArchived"));
        Check("a running profile's entries are not written", (report.waiting & 0x2) && !EntryThere(entries[1], g_syncIds[5]));
        {
            int detail;
            BOOL waiting = FALSE, applied = FALSE;
            for (detail = 0; detail < report.profileCount; detail++) {
                const SyncProfileResult *result = &report.profiles[detail];
                if (Core_EqualsI(result->folder, profiles.items[1].folder)) waiting = result->waiting && result->added == 0;
                if (Core_EqualsI(result->folder, profiles.items[2].folder)) applied = !result->waiting && result->added == 1;
            }
            Check("per-profile results distinguish pending from applied changes", waiting && applied);
        }
        Check("the session waits in the running profile's plan", SessionSync_PendingCount(&profiles.items[1]) == 1);
        {
            WCHAR blocked[MAX_PATH + 16];
            HANDLE file = INVALID_HANDLE_VALUE;
            ready = SessionSync_PlanPath(&profiles.items[1], path, ARRAYSIZE(path)) &&
                    SUCCEEDED(StringCchPrintfW(blocked, ARRAYSIZE(blocked), L"%s.cdm-new", path)) &&
                    (file = CreateFileW(blocked, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE;
            ZeroMemory(&report, sizeof report);
            Check("a replacement whose plan cannot commit reports failure",
                  ready && SessionSync_Share(&set, 0, rows, 1, 0x2, &report) && report.failed > 0);
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (ready) DeleteFileW(blocked);
            Check("the committed plan survives its failed replacement", SessionSync_PendingCount(&profiles.items[1]) == 1);
        }
        Check("the waiting session is not made while its Claude runs", SessionEdit_ApplyPending(NULL, &profiles.items[1]) == 0);
        StopFakeClaude(window);
        Check("the waiting session is made once its Claude closed",
              SessionEdit_ApplyPending(NULL, &profiles.items[1]) == 1 && EntryTitled(entries[1], g_syncIds[5], L"A six"));
        Check("the plan is gone once made", SessionSync_PendingCount(&profiles.items[1]) == 0 &&
              SessionSync_PlanPath(&profiles.items[1], path, ARRAYSIZE(path)) && !FileThere(path));
    }
    if (loaded) SessionStore_Free(&set);

    /* Export from one profile, import into another: the conversation comes back with it. */
    loaded = SessionStore_LoadProfiles(&set, &profiles);
    rows[0] = loaded ? FindRow(&set, g_syncIds[0]) : -1;
    rows[1] = loaded ? FindRow(&set, g_syncIds[2]) : -1;
    ready = rows[0] >= 0 && rows[1] >= 0 && FixturePath(L"sessions.zip", archive, ARRAYSIZE(archive)) &&
            SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\fixture\\%s.jsonl", projects, g_syncIds[0])) &&
            (before = Util_ReadFile(transcript, TRANSCRIPT_READ_MAX, FALSE, &beforeLength)) != NULL;
    Check("export fixtures ready", ready);
    if (ready) {
        Check("exporting two sessions succeeds",
              SessionSync_Export(&set, 0, rows, 2, archive, &exported, error, ARRAYSIZE(error)) && exported == 2 && !error[0]);
        Check("the export is a session archive", SessionSync_IsArchive(archive));
        Check("another file is not a session archive", !SessionSync_IsArchive(transcript));
        Check("a conversation of the fixture is taken away", DeleteFileW(transcript));
        {
            WCHAR *preview = SessionSync_ImportPreview(&set, archive, 0x9, error, ARRAYSIZE(error));
            Check("import preview distinguishes duplicate and new entries",
                  preview && wcsstr(preview, TR(L"Duplicate; keep existing")) && wcsstr(preview, TR(L"New session")));
            Check("preview does not extract transcripts or create target entries", preview && !FileThere(transcript) &&
                  !EntryThere(entries[3], g_syncIds[0]) && !EntryThere(entries[3], g_syncIds[2]));
            if (preview) HeapFree(GetProcessHeap(), 0, preview);
            set.source[3].complete = FALSE;
            preview = SessionSync_ImportPreview(&set, archive, 0x8, error, ARRAYSIZE(error));
            Check("preview rejects an incomplete target snapshot", !preview && error[0]);
            if (preview) HeapFree(GetProcessHeap(), 0, preview);
            set.source[3].complete = TRUE;
            {
                WCHAR collision[MAX_PATH];
                char json[512];
                BOOL made = EntryPath(entries[3], g_syncIds[0], collision, ARRAYSIZE(collision)) &&
                            SUCCEEDED(StringCchPrintfA(json, sizeof json,
                                "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"lastActivityAt\":1}", g_syncIds[0], g_syncIds[4])) &&
                            Save(collision, json);
                preview = made ? SessionSync_ImportPreview(&set, archive, 0x8, error, ARRAYSIZE(error)) : NULL;
                Check("preview reports an existing destination file as a conflict",
                      preview && wcsstr(preview, TR(L"Conflict; keep existing entry with the same file name")));
                if (preview) HeapFree(GetProcessHeap(), 0, preview);
                if (made) DeleteFileW(collision);
            }
            {
                int at = SessionStore_FindEntry(&set, 0, g_syncIds[2], NULL);
                if (at >= 0) {
                    set.entries[at].pendingRemove = TRUE;
                    ZeroMemory(&report, sizeof report);
                    Check("an import skipped by a pending removal still has a profile result",
                          SessionSync_Import(&set, archive, 0x1, &sessions, &report) && report.skipped > 0 &&
                          report.profileCount == 1 && report.profiles[0].skipped == report.skipped &&
                          Core_EqualsI(report.profiles[0].folder, profiles.items[0].folder));
                    set.entries[at].pendingRemove = FALSE;
                    /* Keep the extraction-failure fixture below after this import. */
                    if (FileThere(transcript)) DeleteFileW(transcript);
                }
            }
        }
        /* No entry may be sent until every conversation was extracted. */
        {
            WCHAR blocked[MAX_PATH + 16];
            HANDLE file = INVALID_HANDLE_VALUE;
            ready = ready && SUCCEEDED(StringCchPrintfW(blocked, ARRAYSIZE(blocked), L"%s.cdm-new", transcript)) &&
                    (file = CreateFileW(blocked, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE;
            ZeroMemory(&report, sizeof report);
            Check("failed conversation extraction prevents importing entries",
                  ready && !SessionSync_Import(&set, archive, 0x8, &sessions, &report) && report.failed > 0 &&
                  !EntryThere(entries[3], g_syncIds[0]) && !EntryThere(entries[3], g_syncIds[2]));
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
            if (ready) DeleteFileW(blocked);
        }
    }
    if (loaded) SessionStore_Free(&set);
    loaded = ready && SessionStore_LoadProfiles(&set, &profiles);
    if (loaded) {
        WCHAR blocked[2][MAX_PATH + 16], wrapped[MAX_PATH];
        HANDLE held[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
        ZipOut *out = NULL;
        ZipIn *zip = NULL;
        DWORD code = 0;
        int i;
        BOOL signedOut = FALSE;
        for (i = 0; i < 2 && ready; i++) {
            ready = EntryPath(entries[3], g_syncIds[i == 0 ? 0 : 2], path, ARRAYSIZE(path)) &&
                    SUCCEEDED(StringCchPrintfW(blocked[i], ARRAYSIZE(blocked[i]), L"%s.cdm-new", path)) &&
                    (held[i] = CreateFileW(blocked[i], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE;
        }
        ZeroMemory(&report, sizeof report);
        Check("an import can return a report containing failed entry writes",
              ready && SessionSync_Import(&set, archive, 0x8, &sessions, &report) && report.failed == 2);
        ready = ready && FixturePath(L"wrapped-backup.zip", wrapped, ARRAYSIZE(wrapped)) && (out = Zip_Create(wrapped, &code)) != NULL;
        if (out) {
            BOOL added = Zip_AddFile(out, SESSIONS_ARCHIVE, archive);
            ready = Zip_Close(out, added) && added && ready;
        }
        if (ready) zip = Zip_Open(wrapped);
        error[0] = 0;
        Check("backup restore rejects an import report with failed entry writes",
              zip && !RestoreSessions(zip, &profiles, 3, g_root, &sessions, &signedOut, error, ARRAYSIZE(error)) && error[0]);
        Zip_Free(zip);
        for (i = 0; i < 2; i++)
            if (held[i] != INVALID_HANDLE_VALUE) {
                CloseHandle(held[i]);
                DeleteFileW(blocked[i]);
            }
        ZeroMemory(&report, sizeof report);
        Check("importing the archive succeeds",
              SessionSync_Import(&set, archive, 0x8, &sessions, &report) && sessions == 2 && report.added == 2 && report.failed == 0);
        after = Util_ReadFile(transcript, TRANSCRIPT_READ_MAX, FALSE, &afterLength);
        Check("importing puts back a missing conversation as it was",
              after && afterLength == beforeLength && memcmp(after, before, beforeLength) == 0);
        Check("the imported entries are the exported ones",
              EntryTitled(entries[3], g_syncIds[2], L"A three") && EntryTitled(entries[3], g_syncIds[0], L"Fixture session"));
        SessionStore_Free(&set);
    }
    if (before) HeapFree(GetProcessHeap(), 0, before);
    if (after) HeapFree(GetProcessHeap(), 0, after);
}

/* ------------------------------------------------------------ sessionundo.c */

static const WCHAR *const g_undoIds[3] = {
    L"cdcdcdcd-0000-4000-8000-000000000001", L"cdcdcdcd-0000-4000-8000-000000000002", L"cdcdcdcd-0000-4000-8000-000000000003"
};

/* The latest action undone (or done again) through a snapshot of `profiles` read now. */
static BOOL UndoLatest(const ProfileList *profiles, BOOL redo)
{
    SessionSet set;
    WCHAR error[LONG_PATH_CCH];
    BOOL ok;
    if (!SessionStore_LoadProfiles(&set, profiles)) return FALSE;
    ok = redo ? SessionUndo_Redo(NULL, &set, error, ARRAYSIZE(error)) : SessionUndo_Undo(NULL, &set, error, ARRAYSIZE(error));
    if (!ok) printf("  (%ls)\n", error);
    SessionStore_Free(&set);
    return ok;
}

/* Profile `p`'s entry of `key` changed as the sessions view changes it, one action kept for undoing. */
static BOOL ChangeAndKeep(const ProfileList *profiles, int p, PendingOp op, const WCHAR *key, const WCHAR *value)
{
    SessionSet set;
    PendingEdit edit;
    const SessionEntry *entry;
    WCHAR before[SESSION_TITLE_CCH];
    char *content = NULL;
    DWORD length = 0;
    int found;
    BOOL waiting, made, starred, beforeAuto;
    if (!SessionStore_LoadProfiles(&set, profiles)) return FALSE;
    if ((found = SessionStore_FindEntry(&set, p, key, NULL)) < 0) {
        SessionStore_Free(&set);
        return FALSE;
    }
    entry = &set.entries[found];
    starred = entry->starred;
    beforeAuto = entry->autoTitle;
    StringCchCopyW(before, ARRAYSIZE(before), entry->title);
    if (op == PENDING_REMOVE) content = Util_ReadFile(entry->file, SESSION_ENTRY_MAX_BYTES, FALSE, &length);
    ZeroMemory(&edit, sizeof edit);
    edit.op = op;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), key);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), value);
    SessionUndo_Begin(&set);
    made = SessionEdit_Change(NULL, &profiles->items[p], entry, &edit, &waiting, NULL, 0) && !waiting;
    if (made && op == PENDING_STAR) SessionUndo_Star(&profiles->items[p], key, starred, value[0] == L'1');
    else if (made && op == PENDING_TITLE) SessionUndo_Title(&profiles->items[p], key, before, beforeAuto, value);
    else if (made) SessionUndo_Listed(&profiles->items[p], key, FALSE, content, length);
    else if (content) HeapFree(GetProcessHeap(), 0, content);
    SessionUndo_End();
    SessionStore_Free(&set);
    return made;
}

static BOOL SourceIs(const WCHAR *path, const WCHAR *expected)
{
    WCHAR source[16];
    return ReadString(path, "titleSource", source, ARRAYSIZE(source)) && wcscmp(source, expected) == 0;
}

static BOOL OnlyQueued(const Profile *p, SyncOpKind kind)
{
    SyncOp *ops = NULL;
    int count = SessionSync_Queued(p, &ops);
    BOOL only = count == 1 && ops[0].kind == kind;
    if (ops) HeapFree(GetProcessHeap(), 0, ops);
    return only;
}

/* Ctrl+Z and Ctrl+Y of the sessions view: a star, a title (Claude's own
 * given back as Claude's, none taken away again), a removal to the Recycle
 * Bin, and a share, also while the other profile runs. */
static void TestUndo(void)
{
    static const WCHAR *const labels[2] = { L"Undo-A", L"Undo-B" };
    static const WCHAR *const leaves[2] = { L"undo\\A", L"undo\\B" };
    static const WCHAR *const accounts[2] = { L"undo-a", L"undo-b" };
    ProfileList profiles;
    SessionSet set;
    SyncReport report;
    WCHAR entries[2][MAX_PATH], path[MAX_PATH], untitled[MAX_PATH], mark[MAX_PATH];
    char json[512];
    HWND window;
    int p, row = -1;
    BOOL ready = TRUE, loaded, autoTitle = FALSE;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (p = 0; p < profiles.count && ready; p++)
        ready = PrepareProfile(&profiles.items[p], labels[p], leaves[p], accounts[p], L"undo-organization", entries[p], ARRAYSIZE(entries[p]));
    ready = ready &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\","
                                                          "\"title\":\"Claude's title\",\"titleSource\":\"auto\",\"lastActivityAt\":500}",
                                       g_undoIds[0], g_undoIds[0])) &&
            EntryPath(entries[0], g_undoIds[0], path, ARRAYSIZE(path)) && Save(path, json) &&
            WriteSyncEntry(entries[0], g_undoIds[1], "Shared", 400) &&
            SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\","
                                                          "\"lastActivityAt\":300}", g_undoIds[2], g_undoIds[2])) &&
            EntryPath(entries[0], g_undoIds[2], untitled, ARRAYSIZE(untitled)) && Save(untitled, json) &&
            MarkPath(entries[0], g_undoIds[0], mark, ARRAYSIZE(mark));
    Check("undo fixtures created", ready);
    if (!ready) return;
    SessionUndo_Clear();
    SessionUndo_Star(&profiles.items[0], g_undoIds[0], FALSE, TRUE);
    Check("a change told outside an action is not kept", !SessionUndo_CanUndo());
    if ((loaded = SessionStore_LoadProfiles(&set, &profiles)) != FALSE) {
        int found = SessionStore_FindEntry(&set, 0, g_undoIds[0], NULL);
        autoTitle = found >= 0 && set.entries[found].autoTitle && !set.entries[found].userTitle;
        SessionStore_Free(&set);
    }
    Check("an entry tells Claude's own title apart", autoTitle);

    /* A star, then a title: each an action, undone from the latest. */
    Check("a star and a title made, each kept",
          ChangeAndKeep(&profiles, 0, PENDING_STAR, g_undoIds[0], L"1") && ChangeAndKeep(&profiles, 0, PENDING_TITLE, g_undoIds[0], L"Mine") &&
          ReadTrue(path, "isStarred") && TitledAs(path, L"Mine") && SourceIs(path, L"user") && SessionUndo_CanUndo() && !SessionUndo_CanRedo());
    Check("undoing the title gives Claude's own back, as Claude's",
          UndoLatest(&profiles, FALSE) && TitledAs(path, L"Claude's title") && SourceIs(path, L"auto") && ReadTrue(path, "isStarred"));
    Check("undoing the star takes it away", UndoLatest(&profiles, FALSE) && !ReadTrue(path, "isStarred") && !SessionUndo_CanUndo() &&
          SessionUndo_CanRedo());
    Check("nothing more to undo", !UndoLatest(&profiles, FALSE));
    Check("redoing makes the star again", UndoLatest(&profiles, TRUE) && ReadTrue(path, "isStarred"));
    Check("... then the title, as someone's", UndoLatest(&profiles, TRUE) && TitledAs(path, L"Mine") && SourceIs(path, L"user") &&
          !SessionUndo_CanRedo());
    Check("a session without a title renamed", ChangeAndKeep(&profiles, 0, PENDING_TITLE, g_undoIds[2], L"Named") && TitledAs(untitled, L"Named"));
    Check("undone, it has no title again", UndoLatest(&profiles, FALSE) && !HasMember(untitled, "title") && !HasMember(untitled, "titleSource"));

    /* Removed as Remove removes it: undoing puts the entry back without Claude's marks; redoing takes it out again. */
    RecycleWill(REMOVE_DONE, NULL);
    Check("a session removed to the Recycle Bin, marked", ChangeAndKeep(&profiles, 0, PENDING_REMOVE, g_undoIds[0], L"") && Recycled(path) &&
          !FileThere(path) && FileThere(mark));
    Check("undoing the removal puts its entry back, unmarked", UndoLatest(&profiles, FALSE) && TitledAs(path, L"Mine") && !FileThere(mark));
    Check("redoing it takes it out again, marked", UndoLatest(&profiles, TRUE) && !FileThere(path) && FileThere(mark));
    Check("... and undoing that brings it back", UndoLatest(&profiles, FALSE) && TitledAs(path, L"Mine") && ReadTrue(path, "isStarred"));

    /* Shared with the other profile: undoing takes it out of that one only. */
    if ((loaded = SessionStore_LoadProfiles(&set, &profiles)) != FALSE) {
        row = FindRow(&set, g_undoIds[1]);
        ZeroMemory(&report, sizeof report);
        SessionUndo_Begin(&set);
        ready = row >= 0 && SessionSync_Share(&set, 0, &row, 1, 0x2, &report) && report.added == 1;
        SessionUndo_End();
        SessionStore_Free(&set);
    }
    Check("a share is kept as it is made", loaded && ready && EntryThere(entries[1], g_undoIds[1]) && SessionUndo_CanUndo());
    Check("undoing the share takes it out of the other profile only",
          UndoLatest(&profiles, FALSE) && !EntryThere(entries[1], g_undoIds[1]) && EntryThere(entries[0], g_undoIds[1]));
    Check("redoing shares it again", UndoLatest(&profiles, TRUE) && EntryTitled(entries[1], g_undoIds[1], L"Shared"));

    /* The other profile running: the share waits in its plan, and undoing it takes its place. */
    Check("the share undone again", UndoLatest(&profiles, FALSE) && !EntryThere(entries[1], g_undoIds[1]));
    window = StartFakeClaude(&profiles.items[1]);
    Check("redone while the other profile runs, it waits", window && UndoLatest(&profiles, TRUE) && !EntryThere(entries[1], g_undoIds[1]) &&
          OnlyQueued(&profiles.items[1], SYNC_PUT));
    Check("undone while it runs, a removal waits in its place", UndoLatest(&profiles, FALSE) && OnlyQueued(&profiles.items[1], SYNC_REMOVE));
    StopFakeClaude(window);
    SessionEdit_ApplyPending(NULL, &profiles.items[1]);
    Check("once it closed, it does not list the session", !EntryThere(entries[1], g_undoIds[1]) &&
          SessionSync_PendingCount(&profiles.items[1]) == 0);

    SessionUndo_Forget(g_undoIds[1]);
    Check("what names a session deleted everywhere is forgotten", !SessionUndo_CanRedo() && SessionUndo_CanUndo());
    SessionUndo_Clear();
    Check("cleared, nothing is undone", !SessionUndo_CanUndo() && !SessionUndo_CanRedo());
}

/* The headings left open, kept in settings.ini of the (private) state folder: in order, an item with "=" whole. */
static void TestSettingList(void)
{
    WCHAR items[3][MAX_PATH] = { L"*starred", L"C:\\Work\\a=b", L"*archived|*date:1" }, back[4][MAX_PATH];
    int count;
    Check("a list kept in settings.ini", Util_SetSettingList(L"TestList", (const WCHAR (*)[MAX_PATH])items, 3));
    count = Util_GetSettingList(L"TestList", back, 4);
    Check("... reads back whole, in order", count == 3 && wcscmp(back[0], items[0]) == 0 && wcscmp(back[1], items[1]) == 0 &&
                                            wcscmp(back[2], items[2]) == 0);
    Check("... at most as many as asked", Util_GetSettingList(L"TestList", back, 2) == 2);
    Check("an empty list takes the section away", Util_SetSettingList(L"TestList", NULL, 0) && Util_GetSettingList(L"TestList", back, 4) == 0);
    Check("a list never kept is empty", Util_GetSettingList(L"NoSuchList", back, 4) == 0);
}

/* ------------------------------------------------------------ what sessions sync */

static const WCHAR *const g_itemIds[3] = {
    L"dededede-0000-4000-8000-000000000001", L"dededede-0000-4000-8000-000000000002", L"dededede-0000-4000-8000-000000000003"
};

static BOOL WriteItemEntry(const WCHAR *entries, const WCHAR *id, const char *title, int activity, BOOL archived)
{
    WCHAR file[MAX_PATH];
    char json[512];
    return SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\","
                                                         "\"title\":\"%s\",\"titleSource\":\"user\",\"isArchived\":%s,\"lastActivityAt\":%d}",
                                      id, id, title, archived ? "true" : "false", activity)) &&
           EntryPath(entries, id, file, ARRAYSIZE(file)) && Save(file, json);
}

static BOOL EntryActivityIs(const WCHAR *entries, const WCHAR *id, ULONGLONG expected)
{
    WCHAR path[MAX_PATH];
    DWORD length = 0;
    char *json;
    const char *value;
    size_t valueLength;
    ULONGLONG activity = 0;
    BOOL same;
    if (!EntryPath(entries, id, path, ARRAYSIZE(path)) || (json = Util_ReadFile(path, 65536, FALSE, &length)) == NULL) return FALSE;
    same = Core_JsonMember(json, length, "lastActivityAt", &value, &valueLength) && Core_JsonNumber(value, valueLength, &activity) &&
           activity == expected;
    HeapFree(GetProcessHeap(), 0, json);
    return same;
}

static BOOL KeepItemsGroup(const ProfileList *profiles, DWORD items)
{
    WCHAR listName[FOLDER_CCH];
    SyncReport report;
    int p;
    for (p = 0; p < profiles->count; p++) ((ProfileList *)profiles)->items[p].syncItems = items;
    ZeroMemory(&report, sizeof report);
    return SessionVault_GroupListName(7, listName, ARRAYSIZE(listName)) && SessionVault_Keep(profiles, 0x3, listName, TRUE, &report) &&
           report.failed == 0;
}

/* Sessions synced by part: new ones, titles, archived states and deletions,
 * each left out alone, in a group of two profiles of their own. */
static void TestSessionItems(void)
{
    static const WCHAR *const labels[2] = { L"Parts-A", L"Parts-B" };
    static const WCHAR *const leaves[2] = { L"parts\\A", L"parts\\B" };
    static const WCHAR *const accounts[2] = { L"parts-a", L"parts-b" };
    ProfileList profiles;
    WCHAR entries[2][MAX_PATH], path[MAX_PATH], mark[MAX_PATH];
    char time[32];
    int p;
    BOOL ready = TRUE;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (p = 0; p < profiles.count && ready; p++)
        ready = PrepareProfile(&profiles.items[p], labels[p], leaves[p], accounts[p], L"parts-organization", entries[p], ARRAYSIZE(entries[p]));
    profiles.items[0].syncGroup = profiles.items[1].syncGroup = 7;
    ready = ready && WriteItemEntry(entries[0], g_itemIds[0], "Planning", 100, FALSE);
    Check("sync parts: fixtures created", ready);
    if (!ready) return;
    Check("sync parts: a new session goes across", KeepItemsGroup(&profiles, SYNC_ITEMS_ALL) && EntryTitled(entries[1], g_itemIds[0], L"Planning"));

    /* Titles and archived states left out: each keeps its own; what else changed goes across. */
    ready = WriteItemEntry(entries[1], g_itemIds[0], "Mine", 100, FALSE);
    Check("sync parts: a title changed in one profile stays there",
          ready && KeepItemsGroup(&profiles, SYNC_ITEMS_ALL & ~(SYNC_ITEM_SESSIONS_TITLES | SYNC_ITEM_SESSIONS_ARCHIVED)) &&
          EntryTitled(entries[0], g_itemIds[0], L"Planning") && EntryTitled(entries[1], g_itemIds[0], L"Mine"));
    ready = WriteItemEntry(entries[0], g_itemIds[0], "Planning", 300, TRUE);
    Check("sync parts: going on goes across, the title and the archived state do not",
          ready && KeepItemsGroup(&profiles, SYNC_ITEMS_ALL & ~(SYNC_ITEM_SESSIONS_TITLES | SYNC_ITEM_SESSIONS_ARCHIVED)) &&
          EntryActivityIs(entries[1], g_itemIds[0], 300) && EntryTitled(entries[1], g_itemIds[0], L"Mine") &&
          EntryPath(entries[1], g_itemIds[0], path, ARRAYSIZE(path)) && !ReadTrue(path, "isArchived"));
    Check("sync parts: syncing again changes nothing",
          KeepItemsGroup(&profiles, SYNC_ITEMS_ALL & ~(SYNC_ITEM_SESSIONS_TITLES | SYNC_ITEM_SESSIONS_ARCHIVED)) &&
          EntryTitled(entries[1], g_itemIds[0], L"Mine") && EntryTitled(entries[0], g_itemIds[0], L"Planning"));

    /* New sessions left out: one made in a profile stays there; one both list still follows. */
    ready = WriteItemEntry(entries[0], g_itemIds[1], "Only here", 500, FALSE) && WriteItemEntry(entries[0], g_itemIds[0], "Planning", 600, TRUE);
    Check("sync parts: without new sessions, one made in a profile stays there; updates still go across",
          ready && KeepItemsGroup(&profiles, SYNC_ITEMS_ALL & ~(SYNC_ITEM_SESSIONS_NEW | SYNC_ITEM_SESSIONS_TITLES)) &&
          !EntryThere(entries[1], g_itemIds[1]) && EntryActivityIs(entries[1], g_itemIds[0], 600));

    /* Deletions left out: deleted in one profile, the other keeps it, and it does not come back. */
    ready = EntryPath(entries[0], g_itemIds[0], path, ARRAYSIZE(path)) && DeleteFileW(path) && MarkPath(entries[0], g_itemIds[0], mark, ARRAYSIZE(mark)) &&
            SUCCEEDED(StringCchPrintfA(time, sizeof time, "%I64u", NowMs())) && Save(mark, time);
    Check("sync parts: without deletions, the other profile keeps a session deleted in one",
          ready && KeepItemsGroup(&profiles, SYNC_ITEMS_ALL & ~SYNC_ITEM_SESSIONS_DELETED) && EntryThere(entries[1], g_itemIds[0]) &&
          !EntryThere(entries[0], g_itemIds[0]));
    Check("sync parts: ... and it does not come back where it was deleted",
          KeepItemsGroup(&profiles, SYNC_ITEMS_ALL & ~SYNC_ITEM_SESSIONS_DELETED) && !EntryThere(entries[0], g_itemIds[0]) &&
          EntryThere(entries[1], g_itemIds[0]));
    Check("sync parts: with deletions again, it goes everywhere",
          KeepItemsGroup(&profiles, SYNC_ITEMS_ALL) && !EntryThere(entries[1], g_itemIds[0]) && !EntryThere(entries[0], g_itemIds[0]));
}

/* ------------------------------------------------- sessionvault.c, sessionpurge.c */

static const WCHAR *const g_vaultIds[6] = {
    L"abababab-0000-4000-8000-000000000001", L"abababab-0000-4000-8000-000000000002", L"abababab-0000-4000-8000-000000000003",
    L"abababab-0000-4000-8000-000000000004", L"abababab-0000-4000-8000-000000000005", L"abababab-0000-4000-8000-000000000006"
};

/* An entry of session `id` named `title`, starred or not, last used at `activity`. */
static BOOL WriteVaultEntry(const WCHAR *entries, const WCHAR *id, const char *title, int activity, BOOL starred)
{
    WCHAR file[MAX_PATH];
    char json[512];
    return SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\","
                                                         "\"title\":\"%s\",\"isStarred\":%s,\"lastActivityAt\":%d}",
                                      id, id, title, starred ? "true" : "false", activity)) &&
           EntryPath(entries, id, file, ARRAYSIZE(file)) && Save(file, json);
}

static BOOL KeepGroup(const ProfileList *profiles, DWORD members, SyncReport *report)
{
    ZeroMemory(report, sizeof *report);
    return SessionVault_Keep(profiles, members, VAULT_GROUP_LIST, TRUE, report) && report->failed == 0;
}

static BOOL RemoveEntry(const WCHAR *entries, const WCHAR *id)
{
    WCHAR path[MAX_PATH];
    return EntryPath(entries, id, path, ARRAYSIZE(path)) && DeleteFileW(path);
}

/* A folder made anew, as when Claude is reinstalled: another creation time. */
static BOOL SetCreated(const WCHAR *dir, ULONGLONG ticks)
{
    FILETIME created;
    HANDLE handle = CreateFileW(dir, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BOOL ok;
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    created.dwLowDateTime = (DWORD)ticks;
    created.dwHighDateTime = (DWORD)(ticks >> 32);
    ok = SetFileTime(handle, &created, NULL, NULL);
    CloseHandle(handle);
    return ok;
}

static const PurgeItem *FindPurged(const PurgeItem *items, int count, const WCHAR *id)
{
    int i;
    for (i = 0; i < count; i++)
        if (Core_EqualsI(items[i].id, id)) return &items[i];
    return NULL;
}

/* Two profiles keeping the same sessions, and a third one outside: changes
 * made in either, a deletion with Claude's mark or without, a list lost and
 * given back, a profile running meanwhile; a version restored; the
 * conversations no list names; Claude Code's folder copied. */
static void TestVault(const WCHAR *projects)
{
    static const WCHAR *const labels[3] = { L"Vault-A", L"Vault-B", L"Vault-C" };
    static const WCHAR *const leaves[3] = { L"vault\\A", L"vault\\B", L"vault\\C" };
    static const WCHAR *const accounts[3] = { L"vault-a", L"vault-b", L"vault-c" };
    ProfileList profiles;
    SyncReport report;
    VaultVersion versions[16];
    VaultIds listed, deleted;
    PurgeItem *items = NULL;
    const PurgeItem *gone, *unknown;
    WCHAR entries[3][MAX_PATH], path[MAX_PATH], error[LONG_PATH_CCH], transcripts[4][MAX_PATH], code[MAX_PATH], copy[MAX_PATH];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    const WCHAR *ids[1];
    DWORD copyError = 0;
    HWND window;
    int p, count, pathCount = 0;
    BOOL ready = TRUE;

    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 3;
    for (p = 0; p < profiles.count && ready; p++)
        ready = PrepareProfile(&profiles.items[p], labels[p], leaves[p], accounts[p], L"vault-organization", entries[p], ARRAYSIZE(entries[p]));
    profiles.items[0].syncGroup = profiles.items[1].syncGroup = 1;
    ready = ready && WriteVaultEntry(entries[0], g_vaultIds[0], "A one", 100, FALSE) && WriteVaultEntry(entries[1], g_vaultIds[1], "B two", 200, FALSE);
    Check("vault fixtures created", ready);
    if (!ready) return;

    /* The first sync: each one gets the other's. */
    Check("the group is the profiles that keep the same sessions", SessionVault_Group(&profiles, 1) == 0x3 && SessionVault_Group(&profiles, 2) == 0);
    Check("a first sync of the group succeeds", KeepGroup(&profiles, 0x3, &report));
    Check("a first sync gives each one the other's sessions", report.added == 2 && EntryTitled(entries[1], g_vaultIds[0], L"A one") &&
          EntryTitled(entries[0], g_vaultIds[1], L"B two"));
    Check("a profile outside the group is left alone", !EntryThere(entries[2], g_vaultIds[0]) && !EntryThere(entries[2], g_vaultIds[1]));
    Check("a version of the group's list is kept",
          SessionVault_Versions(VAULT_GROUP_LIST, versions, ARRAYSIZE(versions)) == 1 && versions[0].sessions == 2 && versions[0].time > 0);
    {
        WCHAR *preview = SessionVault_RestorePreview(&profiles, 2, VAULT_GROUP_LIST, versions[0].name);
        Check("recovery preview lists new sessions without writing entries", preview && wcsstr(preview, L"A one") &&
              !EntryThere(entries[2], g_vaultIds[0]) && !EntryThere(entries[2], g_vaultIds[1]));
        if (preview) HeapFree(GetProcessHeap(), 0, preview);
    }
    Check("syncing again changes nothing and keeps no other version",
          KeepGroup(&profiles, 0x3, &report) && report.added + report.updated + report.removed == 0 &&
          SessionVault_Versions(VAULT_GROUP_LIST, versions, ARRAYSIZE(versions)) == 1);
    {
        static const char index[] = "{\"v\":1,\"archived\":[\"local_11111111-1111-4111-8111-111111111111\"]}";
        Sleep(20);
        ready = SaveIn(entries[0], L"archived-sessions.idx", index);
        Check("the archived index is synced without requiring a sessionId member",
              ready && KeepGroup(&profiles, 0x3, &report) && Join(entries[1], L"archived-sessions.idx", path, ARRAYSIZE(path)) &&
              FileHas(path, "\"archived\":[\"local_11111111"));
        count = SessionVault_Versions(VAULT_GROUP_LIST, versions, ARRAYSIZE(versions));
        Check("a kept version restores the archived index",
              count > 0 && SessionVault_Restore(&profiles, 2, VAULT_GROUP_LIST, versions[0].name, &report) &&
              Join(entries[2], L"archived-sessions.idx", path, ARRAYSIZE(path)) && FileHas(path, "\"archived\":[\"local_11111111"));
        /* The later checks start with the outside profile empty. */
        RemoveEntry(entries[2], g_vaultIds[0]);
        RemoveEntry(entries[2], g_vaultIds[1]);
    }

    /* A rename in one, a star in the other, no message sent: both go across. */
    Sleep(20);
    ready = WriteVaultEntry(entries[0], g_vaultIds[0], "A one renamed", 100, FALSE) && WriteVaultEntry(entries[1], g_vaultIds[1], "B two", 200, TRUE);
    Check("a rename and a star made in two profiles", ready);
    Check("they are made the same", KeepGroup(&profiles, 0x3, &report) && report.updated == 2);
    Check("the rename went across", EntryTitled(entries[1], g_vaultIds[0], L"A one renamed"));
    Check("the star went across", EntryPath(entries[0], g_vaultIds[1], path, ARRAYSIZE(path)) && ReadTrue(path, "isStarred"));
    Check("what a sync replaces is kept in the backup", BackedUp(&report, &profiles.items[1], g_vaultIds[0], path, ARRAYSIZE(path)) &&
          TitledAs(path, L"A one"));

    /* Deleted in one profile, Claude's mark beside: it goes from the other, and the vault knows it deleted. */
    ready = RemoveEntry(entries[0], g_vaultIds[1]) && MarkPath(entries[0], g_vaultIds[1], path, ARRAYSIZE(path)) && Save(path, "777");
    Check("a session deleted in one profile", ready);
    Check("the deletion is made the same", KeepGroup(&profiles, 0x3, &report) && report.removed == 1 && !EntryThere(entries[1], g_vaultIds[1]));
    SessionVault_Ids(&listed, &deleted);
    Check("the vault names the deleted session deleted, the others listed",
          SessionVault_HasId(&deleted, g_vaultIds[1]) && !SessionVault_HasId(&listed, g_vaultIds[1]) && SessionVault_HasId(&listed, g_vaultIds[0]));
    SessionVault_FreeIds(&listed);
    SessionVault_FreeIds(&deleted);

    /* Taken away by Claude without a mark: it goes from the other too. */
    ready = WriteVaultEntry(entries[1], g_vaultIds[2], "B three", 300, FALSE) && KeepGroup(&profiles, 0x3, &report) &&
            EntryThere(entries[0], g_vaultIds[2]) && RemoveEntry(entries[1], g_vaultIds[2]);
    Check("a new session went across, then Claude took it away in one profile", ready);
    Check("a session taken away goes from the others", KeepGroup(&profiles, 0x3, &report) && report.removed == 1 && !EntryThere(entries[0], g_vaultIds[2]));

    /* A profile whose whole list is lost (a new entries folder: Claude reinstalled) gets it back; nothing goes elsewhere. */
    ready = WriteVaultEntry(entries[0], g_vaultIds[3], "A four", 400, FALSE) && KeepGroup(&profiles, 0x3, &report) &&
            EntryThere(entries[1], g_vaultIds[3]) && RemoveEntry(entries[1], g_vaultIds[0]) && RemoveEntry(entries[1], g_vaultIds[3]) &&
            SetCreated(entries[1], 133000000000000000ULL);
    Check("a profile loses its list", ready);
    Check("its list comes back", KeepGroup(&profiles, 0x3, &report) && report.added == 2 && report.removed == 0 &&
          EntryTitled(entries[1], g_vaultIds[0], L"A one renamed") && EntryThere(entries[1], g_vaultIds[3]));
    Check("the other one keeps its sessions", EntryThere(entries[0], g_vaultIds[0]) && EntryThere(entries[0], g_vaultIds[3]));

    /* A running profile gets its part once it closes. */
    Sleep(20);
    ready = WriteVaultEntry(entries[0], g_vaultIds[0], "A one again", 500, FALSE);
    window = ready ? StartFakeClaude(&profiles.items[1]) : NULL;
    Check("a change made while the other profile runs", window != NULL);
    if (window) {
        Check("the change waits for the running profile", KeepGroup(&profiles, 0x3, &report) && (report.waiting & 0x2) &&
              EntryTitled(entries[1], g_vaultIds[0], L"A one renamed"));
        Check("kept again while it runs, what it still shows is no change", KeepGroup(&profiles, 0x3, &report) &&
              EntryTitled(entries[0], g_vaultIds[0], L"A one again") && EntryTitled(entries[1], g_vaultIds[0], L"A one renamed"));
        StopFakeClaude(window);
        Check("it is made once that profile closed",
              SessionEdit_ApplyPending(NULL, &profiles.items[1]) > 0 && EntryTitled(entries[1], g_vaultIds[0], L"A one again"));
        Check("then nothing is left to change", KeepGroup(&profiles, 0x3, &report) && report.added + report.updated + report.removed == 0);
    }

    /* A profile outside the group gets the group's latest list: the deleted sessions only as Claude's marks. */
    count = SessionVault_Versions(VAULT_GROUP_LIST, versions, ARRAYSIZE(versions));
    Check("every change kept a version, the newest first", count >= 6 && versions[0].time >= versions[count - 1].time &&
          wcscmp(versions[0].name, versions[count - 1].name) > 0);
    ZeroMemory(&report, sizeof report);
    Check("a version is restored to another profile",
          count > 0 && SessionVault_Restore(&profiles, 2, VAULT_GROUP_LIST, versions[0].name, &report) && report.failed == 0);
    Check("it lists the version's sessions", EntryTitled(entries[2], g_vaultIds[0], L"A one again") && EntryThere(entries[2], g_vaultIds[3]));
    Check("a deleted session is not restored, Claude's mark is written for it",
          !EntryThere(entries[2], g_vaultIds[1]) && MarkPath(entries[2], g_vaultIds[1], path, ARRAYSIZE(path)) && FileThere(path));
    Check("a profile alone keeps a list of its own", SessionVault_Keep(&profiles, 0x4, profiles.items[2].folder, FALSE, &report) &&
          SessionVault_Versions(profiles.items[2].folder, versions, ARRAYSIZE(versions)) == 1 && versions[0].sessions == 2);

    /* The conversations no list names: a deleted one (any age), an old unknown one; a new unknown one is left. */
    ready = TRUE;
    for (p = 0; p < 4 && ready; p++)
        ready = SUCCEEDED(StringCchPrintfW(transcripts[p], ARRAYSIZE(transcripts[p]), L"%s\\fixture\\%s.jsonl", projects,
                                           g_vaultIds[p == 0 ? 1 : p == 1 ? 4 : p == 2 ? 5 : 0]));
    ready = ready && Save(transcripts[0], "{\"type\":\"user\",\"cwd\":\"C:\\\\Gone\"}\n{\"type\":\"custom-title\",\"customTitle\":\"Gone one\"}\n") &&
            Save(transcripts[1], "{\"type\":\"user\",\"cwd\":\"C:\\\\Old\"}\n{\"type\":\"last-prompt\",\"lastPrompt\":\"Old prompt\"}\n") &&
            SetModified(transcripts[1], 132000000000000000ULL) && Save(transcripts[2], "{\"type\":\"user\"}\n") &&
            Save(transcripts[3], "{\"type\":\"user\"}\n") && SetModified(transcripts[3], 132000000000000000ULL);
    Check("conversation fixtures created", ready);
    count = ready ? SessionPurge_List(&profiles, &items, error, ARRAYSIZE(error)) : -1;
    gone = count > 0 ? FindPurged(items, count, g_vaultIds[1]) : NULL;
    unknown = count > 0 ? FindPurged(items, count, g_vaultIds[4]) : NULL;
    Check("the conversations no list names are found", count >= 2);
    Check("a session deleted in Claude is offered as deleted, with its title",
          gone && gone->kind == PURGE_DELETED && wcscmp(gone->title, L"Gone one") == 0 && Core_PathEquals(gone->project, L"C:\\Gone"));
    Check("an old conversation no list ever had is offered as unknown, with its last prompt",
          unknown && unknown->kind == PURGE_UNKNOWN && wcscmp(unknown->title, L"Old prompt") == 0);
    Check("a new conversation no list knows is left alone", count > 0 && !FindPurged(items, count, g_vaultIds[5]));
    Check("a listed session's conversation is never offered", count > 0 && !FindPurged(items, count, g_vaultIds[0]));
    Check("a deleted session a kept list had can be restored, one no list had cannot",
          gone && gone->restorable && unknown && !unknown->restorable);
    Check("it shows under the profiles whose lists had it, the other in no list", gone && gone->profiles != 0 && unknown && unknown->profiles == 0);
    /* Restored: back in each profile of the group, as the list last had it, Claude's mark gone, no longer deleted. */
    ids[0] = g_vaultIds[1];
    ZeroMemory(&report, sizeof report);
    Check("a deleted session is restored", SessionVault_Undelete(&profiles, ids, 1, &report) == 1 && report.failed == 0);
    Check("it is back in both profiles of the group, as it was",
          EntryTitled(entries[0], g_vaultIds[1], L"B two") && EntryTitled(entries[1], g_vaultIds[1], L"B two"));
    if (gone) {
        int chosen = (int)(gone - items), removed = -1;
        Check("cleanup rechecks a session restored while the old list was open",
              SessionPurge_Delete(NULL, &profiles, items, &chosen, 1, &removed, error, ARRAYSIZE(error)) == REMOVE_DONE &&
              removed == 0 && FileThere(transcripts[0]));
    }
    if (items) HeapFree(GetProcessHeap(), 0, items);
    Check("Claude's mark of its deletion is gone", MarkPath(entries[0], g_vaultIds[1], path, ARRAYSIZE(path)) && !FileThere(path));
    Check("a profile whose list never had it does not get it, but loses Claude's mark of it",
          !EntryThere(entries[2], g_vaultIds[1]) && MarkPath(entries[2], g_vaultIds[1], path, ARRAYSIZE(path)) && !FileThere(path));
    SessionVault_Ids(&listed, &deleted);
    Check("the vault no longer names it deleted, and lists it again",
          !SessionVault_HasId(&deleted, g_vaultIds[1]) && SessionVault_HasId(&listed, g_vaultIds[1]));
    SessionVault_FreeIds(&listed);
    SessionVault_FreeIds(&deleted);
    Check("the next sync keeps it", KeepGroup(&profiles, 0x3, &report) && report.removed == 0 && EntryThere(entries[0], g_vaultIds[1]));
    ids[0] = g_vaultIds[4];
    ZeroMemory(&report, sizeof report);
    Check("a conversation no list had is not restored", SessionVault_Undelete(&profiles, ids, 1, &report) == 0 &&
          !EntryThere(entries[0], g_vaultIds[4]));
    Check("the restored session is deleted again for the clean-up below",
          RemoveEntry(entries[0], g_vaultIds[1]) && MarkPath(entries[0], g_vaultIds[1], path, ARRAYSIZE(path)) && Save(path, "888") &&
          KeepGroup(&profiles, 0x3, &report) && report.removed == 1 && !EntryThere(entries[1], g_vaultIds[1]));
    ids[0] = g_vaultIds[1];
    Check("what goes of a conversation is listed, its transcript first",
          TestedSessionEdit_ListTranscriptFiles(ids, 1, &paths, &pathCount, error, ARRAYSIZE(error)) && pathCount >= 1 &&
          Listed(paths, pathCount, transcripts[0]));
    if (paths) HeapFree(GetProcessHeap(), 0, paths);
    Check("a conversation cleaned up joins the deleted ones", SessionVault_AddDeleted(&ids[0], 1));

    /* The weekly copy: none for a week, so one is made; the oldest weekly copies go, two stay. */
    {
        WCHAR oldest[MAX_PATH], older[MAX_PATH], weekly[MAX_PATH], partial[MAX_PATH + 16], name[MAX_PATH];
        SYSTEMTIME today;
        GetLocalTime(&today);
        ready = SessionPurge_CodeFolder(code, ARRAYSIZE(code)) && InFixture(code) &&
                SUCCEEDED(StringCchPrintfW(oldest, ARRAYSIZE(oldest), L"%s_auto_20200101", code)) &&
                SUCCEEDED(StringCchPrintfW(older, ARRAYSIZE(older), L"%s_auto_20200108", code)) &&
                Core_WeeklyCopyName(wcsrchr(code, L'\\') + 1, &today, name, ARRAYSIZE(name)) &&
                SUCCEEDED(StringCchCopyW(weekly, ARRAYSIZE(weekly), code)) && wcsrchr(weekly, L'\\') &&
                SUCCEEDED(StringCchCopyW(wcsrchr(weekly, L'\\') + 1, ARRAYSIZE(weekly) - (size_t)(wcsrchr(weekly, L'\\') + 1 - weekly), name)) &&
                SUCCEEDED(StringCchPrintfW(partial, ARRAYSIZE(partial), L"%s.partial", weekly)) &&
                CreateDirectoryW(oldest, NULL) && CreateDirectoryW(older, NULL);
        Check("two old weekly copies", ready);
        if (ready) SessionPurge_WeeklyBackUp(&profiles);
        Check("a weekly copy is made when none is a week old",
              ready && SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\projects\\fixture\\%s.jsonl", weekly, g_vaultIds[1])) && FileThere(path));
        Check("it is named after the folder and today, and no partial copy is left", ready && Util_DirExists(weekly) && !Util_DirExists(partial));
        Check("only the two latest weekly copies stay", ready && !Util_DirExists(oldest) && Util_DirExists(older));
        if (ready) {
            DeleteFileW(path);
            SessionPurge_WeeklyBackUp(&profiles);
        }
        Check("none is made again within the week", ready && !FileThere(path) && Util_DirExists(older));
    }

    /* The vault does not grow without end: old versions beyond the latest 20 go, and the entries no version names. */
    {
        static VaultVersion many[64];
        WCHAR state[MAX_PATH], dir[MAX_PATH], latest[MAX_PATH], old[MAX_PATH], stray[MAX_PATH];
        DWORD length = 0;
        char *text = NULL;
        int made = 0, before, after, oldLeft = 0;
        before = SessionVault_Versions(VAULT_GROUP_LIST, many, ARRAYSIZE(many));
        ready = before > 0 && before < 20 && Util_StateDir(state, ARRAYSIZE(state)) &&
                SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\vault\\lists\\" VAULT_GROUP_LIST, state)) &&
                SUCCEEDED(StringCchPrintfW(latest, ARRAYSIZE(latest), L"%s\\%s.txt", dir, many[0].name)) &&
                (text = Util_ReadFile(latest, 1024 * 1024, FALSE, &length)) != NULL &&
                SUCCEEDED(StringCchPrintfW(stray, ARRAYSIZE(stray), L"%s\\vault\\objects\\00000000000abcde.json", state)) &&
                Save(stray, "{}") && SetModified(stray, 132000000000000000ULL);
        for (p = 1; ready && p <= 25; p++) {
            HANDLE file = INVALID_HANDLE_VALUE;
            DWORD written = 0;
            ready = SUCCEEDED(StringCchPrintfW(old, ARRAYSIZE(old), L"%s\\202001%02d-000000-000.txt", dir, p)) &&
                    (file = CreateFileW(old, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE;
            if (!ready) break;
            ready = WriteFile(file, text, length, &written, NULL) && written == length;
            CloseHandle(file);
            made++;
        }
        if (text) HeapFree(GetProcessHeap(), 0, text);
        Check("old versions and a stray entry in the vault", ready && made == 25);
        Sleep(20);
        ready = ready && WriteVaultEntry(entries[0], g_vaultIds[0], "A one pruned", 600, FALSE) && KeepGroup(&profiles, 0x3, &report);
        after = SessionVault_Versions(VAULT_GROUP_LIST, many, ARRAYSIZE(many));
        for (p = 0; p < after; p++)
            if (wcsncmp(many[p].name, L"2020", 4) == 0) oldLeft++;
        Check("a new version keeps the latest 20, the older ones go", ready && after == 20 && oldLeft == 20 - (before + 1) &&
              wcsncmp(many[0].name, L"2020", 4) != 0);
        Check("an entry no version names, written long ago, goes", ready && !FileThere(stray));
        Check("the entries the versions name stay", ready && EntryTitled(entries[1], g_vaultIds[0], L"A one pruned") &&
              SessionVault_Restore(&profiles, 2, VAULT_GROUP_LIST, many[0].name, &report) && report.failed == 0);
        {
            WCHAR blocked[MAX_PATH], version[MAX_PATH];
            BOOL denied;
            ready = SUCCEEDED(StringCchPrintfW(blocked, ARRAYSIZE(blocked), L"%s\\vault\\lists\\blocked", state)) && MakeDir(blocked) &&
                    Join(blocked, L"20200101-000000-000.txt", version, ARRAYSIZE(version)) &&
                    Save(version, "claude-desktop-profiles-manager-vault 1\nlisted\tabababab-0000-4000-8000-000000000001\t00000000000abcde\t1\n") &&
                    Save(stray, "{}") && SetModified(stray, 132000000000000000ULL);
            denied = ready && DenyDirectory(blocked, TRUE);
            Sleep(20);
            Check("vault collection stops when another list cannot be enumerated",
                  denied && WriteVaultEntry(entries[0], g_vaultIds[0], "A one guarded", 700, FALSE) &&
                  KeepGroup(&profiles, 0x3, &report) && FileThere(stray));
            if (denied) Check("the private list's access is restored", DenyDirectory(blocked, FALSE));
            if (ready) {
                DeleteFileW(version);
                RemoveDirectoryW(blocked);
            }
            Sleep(20);
            Check("vault collection resumes once every list can be read",
                  ready && WriteVaultEntry(entries[0], g_vaultIds[0], "A one recovered", 800, FALSE) &&
                  KeepGroup(&profiles, 0x3, &report) && !FileThere(stray));
        }
    }

    /* Claude Code's folder copied beside it, as <name>_<date>. */
    ready = SessionPurge_CodeFolder(code, ARRAYSIZE(code)) && SessionPurge_BackupName(copy, ARRAYSIZE(copy)) && InFixture(copy);
    Check("the copy of Claude Code's folder is named after it and today", ready && wcsncmp(copy, code, wcslen(code)) == 0 && copy[wcslen(code)] == L'_');
    Check("the folder is copied whole", ready && SessionPurge_BackUp(NULL, copy, &copyError) == COPY_MADE &&
          SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\projects\\fixture\\%s.jsonl", copy, g_vaultIds[1])) && FileThere(path));
    Check("the next copy that day gets another name", SessionPurge_BackupName(path, ARRAYSIZE(path)) && !Core_PathEquals(path, copy));
}

/* ------------------------------------------------ groups, layouts, forks */

static const WCHAR *const g_keptIds[4] = {
    L"cdcdcdcd-0000-4000-8000-000000000001", L"cdcdcdcd-0000-4000-8000-000000000002", L"cdcdcdcd-0000-4000-8000-000000000003",
    L"cdcdcdcd-0000-4000-8000-000000000004"
};

/* 2026-10-08T10:00:<second>Z in ms since 1970. */
static ULONGLONG AtSecond(int second)
{
    SYSTEMTIME at;
    FILETIME time;
    ZeroMemory(&at, sizeof at);
    at.wYear = 2026;
    at.wMonth = 10;
    at.wDay = 8;
    at.wHour = 10;
    at.wSecond = (WORD)second;
    SystemTimeToFileTime(&at, &time);
    return ((((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime) - 116444736000000000ULL) / 10000;
}

/* An entry of session `id` named `title`, working in `cwd`, last used at `activity` (ms). */
static BOOL WriteKeptEntry(const WCHAR *entries, const WCHAR *id, const char *title, const WCHAR *cwd, ULONGLONG activity)
{
    WCHAR file[MAX_PATH];
    char quoted[MAX_PATH * 6 + 4], json[2048];
    return Core_JsonQuote(cwd, quoted, sizeof quoted) &&
           SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":%s,\"originCwd\":%s,"
                                                         "\"title\":\"%s\",\"lastActivityAt\":%I64u}", id, id, quoted, quoted, title, activity)) &&
           EntryPath(entries, id, file, ARRAYSIZE(file)) && Save(file, json);
}

static void AddLine(char *text, size_t cap, const WCHAR *session, const char *type, const char *uuid, const char *parent, int second)
{
    char line[512];
    if (parent)
        StringCchPrintfA(line, sizeof line, "{\"type\":\"%s\",\"uuid\":\"%s\",\"parentUuid\":\"%s\",\"timestamp\":\"2026-10-08T10:00:%02d.000Z\","
                         "\"sessionId\":\"%ls\"}\n", type, uuid, parent, second, session);
    else
        StringCchPrintfA(line, sizeof line, "{\"type\":\"%s\",\"uuid\":\"%s\",\"parentUuid\":null,\"timestamp\":\"2026-10-08T10:00:%02d.000Z\","
                         "\"sessionId\":\"%ls\"}\n", type, uuid, second, session);
    StringCchCatA(text, cap, line);
}

static BOOL FileHas(const WCHAR *path, const char *part)
{
    DWORD length = 0;
    char *text = Util_ReadFile(path, TRANSCRIPT_READ_MAX, FALSE, &length);
    BOOL has = text && strstr(text, part) != NULL;
    if (text) HeapFree(GetProcessHeap(), 0, text);
    return has;
}

/* The entry of `entries` whose title is `title` (a session kept as two): its transcript's id. */
static BOOL FindTitled(const WCHAR *entries, const WCHAR *title, WCHAR *id, size_t cch)
{
    WCHAR path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(entries, L"local_*.json", &found, FALSE);
    BOOL seen = FALSE;
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (Join(entries, found.cFileName, path, ARRAYSIZE(path)) && TitledAs(path, title) && ReadString(path, "cliSessionId", id, cch)) seen = TRUE;
    } while (!seen && FindNextFileW(find, &found));
    FindClose(find);
    return seen;
}

static int g_steps, g_lastTotal;

static void CountSteps(void *context, int done, int total)
{
    (void)context;
    g_steps++;
    g_lastTotal = total;
    if (done > total) g_lastTotal = -1;
}

/* Two groups apart; the pins and groups of Claude's sidebar kept the same,
 * in each profile's own account; a session without a folder working in each
 * profile's own area; a session two profiles went on with apart, kept as two. */
static void TestKeptGroups(const WCHAR *projects)
{
    static const WCHAR *const labels[4] = { L"Kept-A", L"Kept-B", L"Kept-C", L"Kept-D" };
    static const WCHAR *const leaves[4] = { L"kept\\A", L"kept\\B", L"kept\\C", L"kept\\D" };
    static const WCHAR *const accounts[4] = { L"kept-a", L"kept-b", L"kept-c", L"kept-d" };
    ProfileList profiles;
    SyncReport report;
    WCHAR entries[4][MAX_PATH], configs[4][MAX_PATH], scratch[2][MAX_PATH], path[MAX_PATH], list[FOLDER_CCH], forkId[SESSION_ID_CCH];
    WCHAR transcript[MAX_PATH], forked[MAX_PATH], cwd[MAX_PATH];
    char layout[2048], text[4096], scope[128];
    HWND window;
    int i;
    BOOL ready = TRUE;

    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 4;
    for (i = 0; i < profiles.count && ready; i++)
        ready = PrepareProfile(&profiles.items[i], labels[i], leaves[i], accounts[i], L"kept-organization", entries[i], ARRAYSIZE(entries[i])) &&
                Join(profiles.items[i].storageDir, L"claude_desktop_config.json", configs[i], ARRAYSIZE(configs[i]));
    profiles.items[0].syncGroup = profiles.items[1].syncGroup = 2;
    profiles.items[2].syncGroup = profiles.items[3].syncGroup = 3;
    ready = ready && WriteKeptEntry(entries[0], g_keptIds[0], "A one", L"C:\\Fixture", AtSecond(2)) &&
            WriteKeptEntry(entries[2], g_keptIds[1], "C one", L"C:\\Fixture", AtSecond(2)) &&
            SessionVault_GroupListName(2, list, ARRAYSIZE(list)) && wcscmp(list, L"group-2") == 0;
    Check("kept groups: fixtures created", ready);
    if (!ready) return;
    Check("kept groups: each group is its profiles", SessionVault_Group(&profiles, 2) == 0x3 && SessionVault_Group(&profiles, 3) == 0xC &&
          SessionVault_NewGroup(&profiles) == 1);

    /* Both groups kept at once, each apart. */
    ZeroMemory(&report, sizeof report);
    g_steps = g_lastTotal = 0;
    Check("kept groups: every group kept", SessionVault_KeepGroups(&profiles, 0xF, CountSteps, NULL, &report) && report.failed == 0);
    Check("kept groups: each group got its own sessions", EntryThere(entries[1], g_keptIds[0]) && EntryThere(entries[3], g_keptIds[1]));
    Check("kept groups: nothing crossed between groups", !EntryThere(entries[2], g_keptIds[0]) && !EntryThere(entries[0], g_keptIds[1]));
    Check("kept groups: the progress went forward to its end", g_steps > 0 && g_lastTotal > 0);

    /* The sidebar's pins, project order and filters of A, for its account, go to B for B's; its groups, which Claude's web
     * storage keeps, and what else B's settings hold stay. */
    StringCchPrintfA(layout, sizeof layout,
        "{\"preferences\":{\"epitaxyPrefs\":{\"starred-local-code-sessions\":[\"local_%ls\"],"
        "\"dframe-local-slice\":{\"pinnedOrder\":[\"code:local_%ls\"]},"
        "\"dframe-group-scopes\":{\"%ls/kept-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"Lunwen\"}],"
        "\"assignments\":{\"code:local_%ls\":\"cg-1\"}},\"other/org\":{\"groups\":[]}},"
        "\"code-projects-order.%ls\":[\"C:\\\\Work\"],\"code-sessions-status-filter.%ls\":\"all\","
        "\"ccd-sessions-filter\":{\"state\":{\"selectedProjects\":[\"C:\\\\Work\"]},\"version\":0},\"unrelated\":1}},\"window\":2}",
        g_keptIds[0], g_keptIds[0], accounts[0], g_keptIds[0], accounts[0], accounts[0]);
    ready = Save(configs[0], layout) && Save(configs[1], "{\"preferences\":{\"keep\":1},\"other\":true}");
    Check("kept layouts: fixtures created", ready);
    ZeroMemory(&report, sizeof report);
    Check("kept layouts: the group is kept", ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0);
    StringCchPrintfA(scope, sizeof scope, "\"%ls/kept-organization\":{\"groups\":[{\"id\":\"cg-1\"", accounts[1]);
    Check("kept layouts: the pins went across", FileHas(configs[1], "\"starred-local-code-sessions\":[\"local_cdcdcdcd-0000-4000-8000-000000000001\"]") &&
          FileHas(configs[1], "\"pinnedOrder\":[\"code:local_cdcdcdcd-0000-4000-8000-000000000001\"]"));
    Check("kept layouts: the sidebar's filters went across, for the other profile's account",
          FileHas(configs[1], "\"code-sessions-status-filter.kept-b\":\"all\"") &&
          FileHas(configs[1], "\"ccd-sessions-filter\":{\"state\":{\"selectedProjects\":[\"C:\\\\Work\"]},\"version\":0}"));
    Check("kept layouts: the groups stay where they are, for Claude's web storage keeps them",
          !FileHas(configs[1], "dframe-group-scopes") && !FileHas(configs[1], scope));
    Check("kept layouts: the project order went across, for the other profile's account",
          FileHas(configs[1], "\"code-projects-order.kept-b\":[\"C:\\\\Work\"]"));
    Check("kept layouts: what else the settings hold stays", FileHas(configs[1], "\"keep\":1") && FileHas(configs[1], "\"other\":true") &&
          !FileHas(configs[1], "\"unrelated\"") && !FileHas(configs[1], "other/org"));
    ZeroMemory(&report, sizeof report);
    Check("kept layouts: kept again, nothing changes",
          SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && report.added + report.updated == 0);
    Sleep(30);
    ready = Save(configs[1], "{\"preferences\":{\"keep\":1,\"epitaxyPrefs\":{\"starred-local-code-sessions\":[]}},\"other\":true}");
    ZeroMemory(&report, sizeof report);
    Check("kept layouts: a pin taken away in one goes from the other",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && FileHas(configs[0], "\"starred-local-code-sessions\":[]") &&
          FileHas(configs[0], "\"window\":2"));

    /* Each part on its own: a pin added in one, then a filter set in the other: both kept. */
    Sleep(30);
    StringCchPrintfA(layout, sizeof layout, "{\"preferences\":{\"epitaxyPrefs\":{\"starred-local-code-sessions\":[\"local_%ls\"]}},\"window\":2}",
                     g_keptIds[0]);
    ready = Save(configs[0], layout);
    Sleep(30);
    StringCchPrintfA(layout, sizeof layout,
        "{\"preferences\":{\"epitaxyPrefs\":{\"code-sessions-show-pr-status.%ls\":false}},\"other\":true}", accounts[1]);
    ready = ready && Save(configs[1], layout);
    ZeroMemory(&report, sizeof report);
    Check("kept layouts: parts changed in two profiles are kept", ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0);
    Check("kept layouts: the pin of one went to the other, set later",
          FileHas(configs[1], "\"starred-local-code-sessions\":[\"local_cdcdcdcd-0000-4000-8000-000000000001\"]"));
    Check("kept layouts: the filter of the other went to the first, for its account",
          FileHas(configs[0], "\"code-sessions-show-pr-status.kept-a\":false"));
    Check("kept layouts: a part neither changed is given back where it was missing",
          FileHas(configs[0], "\"code-projects-order.kept-a\":[\"C:\\\\Work\"]") &&
          FileHas(configs[1], "\"code-projects-order.kept-b\":[\"C:\\\\Work\"]"));

    /* A change sent to a running profile, which Claude rewrites there before it closes: what it still shows is no change. */
    window = StartFakeClaude(&profiles.items[1]);
    Sleep(30);
    ready = window && Save(configs[0], "{\"preferences\":{\"epitaxyPrefs\":{\"starred-local-code-sessions\":[]}},\"window\":2}");
    ZeroMemory(&report, sizeof report);
    Check("kept layouts: a pin taken away waits for the running profile",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && (report.waiting & 0x2) &&
          FileHas(configs[1], "\"starred-local-code-sessions\":[\"local_cdcdcdcd-0000-4000-8000-000000000001\"]"));
    Sleep(30);
    StringCchPrintfA(layout, sizeof layout,
        "{\"preferences\":{\"epitaxyPrefs\":{\"starred-local-code-sessions\":[\"local_%ls\"],"
        "\"code-sessions-show-pr-status.%ls\":true}},\"other\":true}",
        g_keptIds[0], accounts[1]);
    ready = window && Save(configs[1], layout);
    ZeroMemory(&report, sizeof report);
    Check("kept layouts: kept again while it runs, its old pins are no change",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 &&
          FileHas(configs[0], "\"starred-local-code-sessions\":[]"));
    StopFakeClaude(window);
    Check("kept layouts: it gets the pins once it closes",
          SessionEdit_ApplyPending(NULL, &profiles.items[1]) > 0 && FileHas(configs[1], "\"starred-local-code-sessions\":[]"));

    /* A session without a folder: in the other profile, it works in that one's own area, its files with it. */
    ready = Core_ScratchDirFor(profiles.items[0].dataDir, entries[0], scratch[0], ARRAYSIZE(scratch[0])) &&
            Core_ScratchDirFor(profiles.items[1].dataDir, entries[1], scratch[1], ARRAYSIZE(scratch[1])) &&
            Join(scratch[0], L"scratch-2026-10-07-abcdef", cwd, ARRAYSIZE(cwd)) && MakeDir(cwd) && SaveIn(cwd, L"notes.txt", "kept") &&
            WriteKeptEntry(entries[0], g_keptIds[2], "Scratch one", cwd, AtSecond(3));
    Check("kept scratch: fixtures created", ready);
    ZeroMemory(&report, sizeof report);
    Check("kept scratch: the group is kept", ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0);
    Check("kept scratch: the other profile's entry works in its own area",
          EntryPath(entries[1], g_keptIds[2], path, ARRAYSIZE(path)) && ReadString(path, "cwd", forked, ARRAYSIZE(forked)) &&
          Join(scratch[1], L"scratch-2026-10-07-abcdef", cwd, ARRAYSIZE(cwd)) && Core_PathEquals(forked, cwd) &&
          ReadString(path, "originCwd", forked, ARRAYSIZE(forked)) && Core_PathEquals(forked, cwd));
    Check("kept scratch: its working folder is there, with the files", Join(cwd, L"notes.txt", path, ARRAYSIZE(path)) && FileThere(path));
    ZeroMemory(&report, sizeof report);
    Check("kept scratch: the two entries count as the same", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) &&
          report.added + report.updated == 0);
    /* Sent by an earlier version as it was, the same entry in both: the other profile's moves to its own area. */
    ready = Join(scratch[0], L"scratch-2026-10-07-fedcba", cwd, ARRAYSIZE(cwd)) && MakeDir(cwd) &&
            WriteKeptEntry(entries[0], g_keptIds[1], "Scratch two", cwd, AtSecond(4)) &&
            WriteKeptEntry(entries[1], g_keptIds[1], "Scratch two", cwd, AtSecond(4));
    ZeroMemory(&report, sizeof report);
    Check("kept scratch: an entry working in the other's area moves to its own",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.updated == 1 &&
          EntryPath(entries[1], g_keptIds[1], path, ARRAYSIZE(path)) && ReadString(path, "cwd", forked, ARRAYSIZE(forked)) &&
          Join(scratch[1], L"scratch-2026-10-07-fedcba", cwd, ARRAYSIZE(cwd)) && Core_PathEquals(forked, cwd) && DirThere(cwd));
    ZeroMemory(&report, sizeof report);
    Check("kept scratch: then each works in its own, nothing changes", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) &&
          report.added + report.updated == 0);

    /* One conversation, gone on with in both profiles apart since the last sync: the branch of the one that went on first is kept as its own session. */
    text[0] = 0;
    AddLine(text, sizeof text, g_keptIds[3], "user", "a", NULL, 1);
    AddLine(text, sizeof text, g_keptIds[3], "assistant", "b", "a", 2);
    ready = SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\fixture\\%s.jsonl", projects, g_keptIds[3])) && Save(transcript, text) &&
            WriteKeptEntry(entries[0], g_keptIds[3], "Shared", L"C:\\Fixture", AtSecond(2));
    ZeroMemory(&report, sizeof report);
    ready = ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && EntryThere(entries[1], g_keptIds[3]);
    AddLine(text, sizeof text, g_keptIds[3], "user", "c1", "b", 10);
    AddLine(text, sizeof text, g_keptIds[3], "user", "c2", "b", 11);
    AddLine(text, sizeof text, g_keptIds[3], "assistant", "d1", "c1", 13);
    Sleep(30);
    ready = ready && Save(transcript, text) && WriteKeptEntry(entries[0], g_keptIds[3], "Shared", L"C:\\Fixture", AtSecond(13)) &&
            WriteKeptEntry(entries[1], g_keptIds[3], "Shared", L"C:\\Fixture", AtSecond(11));
    Check("kept forks: fixtures created", ready);
    ZeroMemory(&report, sizeof report);
    Check("kept forks: the group is kept", ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0);
    Check("kept forks: the session is kept as two", report.forked == 1);
    Check("kept forks: both profiles list the other branch, titled with its profile",
          FindTitled(entries[0], L"Shared (Kept-B)", forkId, ARRAYSIZE(forkId)) && Core_IsUuid(forkId) &&
          FindTitled(entries[1], L"Shared (Kept-B)", path, ARRAYSIZE(path)) && wcscmp(path, forkId) == 0);
    Check("kept forks: the original goes on, in both", EntryTitled(entries[0], g_keptIds[3], L"Shared") && EntryTitled(entries[1], g_keptIds[3], L"Shared"));
    ready = SUCCEEDED(StringCchPrintfW(forked, ARRAYSIZE(forked), L"%s\\fixture\\%s.jsonl", projects, forkId)) && FileThere(forked);
    Check("kept forks: the other branch has a transcript of its own", ready);
    Check("kept forks: it holds that branch, under its own id", ready && FileHas(forked, "\"uuid\":\"c2\"") && FileHas(forked, "\"uuid\":\"b\"") &&
          !FileHas(forked, "\"uuid\":\"c1\"") && !FileHas(forked, "\"uuid\":\"d1\"") && !FileHas(forked, "cdcdcdcd-0000-4000-8000-000000000004"));
    Check("kept forks: the original transcript is left as it was", FileHas(transcript, "\"uuid\":\"c1\"") && FileHas(transcript, "\"uuid\":\"c2\""));
    ZeroMemory(&report, sizeof report);
    Check("kept forks: kept again, no other fork", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.forked == 0);

    /* A conversation that ran out of room goes on in the same entry with a new transcript, naming the earlier ones: they
     * are no deletion, and the other profile gets the entry with the new transcript, each time, from either profile. */
    {
        static const WCHAR *const goesOn[3] = { L"cdcdcdcd-0000-4000-8000-000000000011", L"cdcdcdcd-0000-4000-8000-000000000012",
                                                L"cdcdcdcd-0000-4000-8000-000000000013" };
        char json[1024], priors[256];
        WCHAR file[MAX_PATH], cli[SESSION_ID_CCH];
        int step, in;
        ready = WriteKeptEntry(entries[0], goesOn[0], "Long one", L"C:\\Fixture", AtSecond(20));
        ZeroMemory(&report, sizeof report);
        Check("kept continued: fixtures kept", ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && EntryThere(entries[1], goesOn[0]));
        priors[0] = 0;
        for (step = 1; step < 3; step++) {
            in = step - 1;   /* it goes on in A, then in B */
            StringCchPrintfA(json, sizeof json, "%s\"%ls\"", priors[0] ? "," : "", goesOn[step - 1]);
            StringCchCatA(priors, sizeof priors, json);
            StringCchPrintfA(json, sizeof json,
                "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"priorCliSessionIds\":[%s],\"cwd\":\"C:\\\\Fixture\","
                "\"originCwd\":\"C:\\\\Fixture\",\"title\":\"Long one\",\"lastActivityAt\":%I64u}", goesOn[0], goesOn[step], priors, AtSecond(20 + step));
            Sleep(30);
            ready = EntryPath(entries[in], goesOn[0], file, ARRAYSIZE(file)) && Save(file, json) &&
                    EntryPath(entries[1 - in], goesOn[0], file, ARRAYSIZE(file));
            ZeroMemory(&report, sizeof report);
            Check(step == 1 ? "kept continued: gone on in one, the other gets the entry with the new transcript"
                            : "kept continued: gone on again in the other, the first gets it in turn",
                  ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && report.removed == 0 &&
                  ReadString(file, "cliSessionId", cli, ARRAYSIZE(cli)) && wcscmp(cli, goesOn[step]) == 0);
        }
        ZeroMemory(&report, sizeof report);
        Check("kept continued: kept again, both keep the entry, nothing removed",
              SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && report.removed == 0 &&
              EntryThere(entries[0], goesOn[0]) && EntryThere(entries[1], goesOn[0]));
    }

    /* A profile new to the group takes its layout: what it showed before, newer as it is, changes none. */
    Sleep(30);
    StringCchPrintfA(layout, sizeof layout, "{\"preferences\":{\"epitaxyPrefs\":{\"code-sessions-status-filter.%ls\":\"archived\"}}}", accounts[2]);
    ready = Save(configs[2], layout);
    ZeroMemory(&report, sizeof report);
    Check("kept layouts: a profile new to the group takes its filter, and changes none",
          ready && SessionVault_Keep(&profiles, 0x7, list, TRUE, &report) && report.failed == 0 &&
          FileHas(configs[0], "\"code-sessions-status-filter.kept-a\":\"all\"") &&
          FileHas(configs[2], "\"code-sessions-status-filter.kept-c\":\"all\""));
}

/* ------------------------------------------- the sidebar in web storage */

#define UNSORTED_UTF8 "\xE6\x9C\xAA\xE5\x88\x86\xE7\xB1\xBB"   /* a group's name */

static BOOL SaveBytes(const WCHAR *path, const BYTE *data, size_t length)
{
    HANDLE file;
    DWORD written = 0;
    BOOL ok;
    if (!InFixture(path)) return FALSE;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, data, (DWORD)length, &written, NULL) && written == length;
    CloseHandle(file);
    return ok;
}

/* Claude's web storage in `storageDir` (Local Storage\leveldb), as LevelDB
 * leaves it: CURRENT, a manifest naming log 3, and that log, one write batch
 * of the claude.ai values `names` (`values`, UTF-8). */
static BOOL MakeWebStorage(const WCHAR *storageDir, const WCHAR *const *names, const char *const *values, int count)
{
    static const char kComparator[] = "leveldb.BytewiseComparator";
    static BYTE keys[8][128], data[8][4096], batch[32768], record[40000];
    WCHAR dir[MAX_PATH], path[MAX_PATH], text[2048];
    BYTE edit[64];
    CoreLevelOp ops[8];
    size_t editLength = 0, length;
    int i, chars;
    if (count > 8 || !Join(storageDir, L"Local Storage\\leveldb", dir, ARRAYSIZE(dir)) || !MakeDir(dir)) return FALSE;
    ZeroMemory(ops, sizeof ops);
    for (i = 0; i < count; i++) {
        if ((chars = MultiByteToWideChar(CP_UTF8, 0, values[i], -1, text, ARRAYSIZE(text)) - 1) < 0) return FALSE;
        ops[i].put = TRUE;
        ops[i].key = keys[i];
        ops[i].keyLength = Core_WebStorageKey("https://claude.ai", names[i], keys[i], sizeof keys[i]);
        ops[i].value = data[i];
        ops[i].valueLength = Core_WebStorageValue(text, (size_t)chars, data[i], sizeof data[i]);
        if (!ops[i].keyLength || !ops[i].valueLength) return FALSE;
    }
    /* The manifest's edit: the comparator, log 3, next file 4, last sequence 0. */
    edit[editLength++] = 1;
    edit[editLength++] = (BYTE)(sizeof kComparator - 1);
    memcpy(edit + editLength, kComparator, sizeof kComparator - 1);
    editLength += sizeof kComparator - 1;
    edit[editLength++] = 2;
    edit[editLength++] = 3;
    edit[editLength++] = 3;
    edit[editLength++] = 4;
    edit[editLength++] = 4;
    edit[editLength++] = 0;
    if ((length = Core_LevelLogAppend(0, edit, editLength, record, sizeof record)) == 0 || !Join(dir, L"MANIFEST-000002", path, ARRAYSIZE(path)) ||
        !SaveBytes(path, record, length) || !SaveIn(dir, L"CURRENT", "MANIFEST-000002\n") || !SaveIn(dir, L"LOCK", ""))
        return FALSE;
    return (length = Core_LevelBatchWrite(1, ops, count, batch, sizeof batch)) != 0 &&
           (length = Core_LevelLogAppend(0, batch, length, record, sizeof record)) != 0 && Join(dir, L"000003.log", path, ARRAYSIZE(path)) &&
           SaveBytes(path, record, length);
}

/* Conflict `part` of `item` is among `conflicts`, between the profiles of `members`. */
/* A conflict over `part`, which belongs to `item` (an item, or a group of them). */
static BOOL HasConflict(const VaultConflict *conflicts, int count, const char *part, DWORD item, DWORD members)
{
    int c;
    for (c = 0; c < count; c++)
        if (strcmp(conflicts[c].part, part) == 0 && (conflicts[c].item & item) && conflicts[c].members == members) return TRUE;
    return FALSE;
}

/* Every conflict of `item` in `group` settled for the version of `profile`. */
static BOOL DecideItem(const ProfileList *profiles, int group, DWORD item, int profile)
{
    static VaultConflict conflicts[64];
    int chosen[64], count = SessionVault_Conflicts(profiles, group, conflicts, 64), c, n = 0;
    for (c = 0; c < count; c++)
        if (conflicts[c].item & item) {
            if (n != c) conflicts[n] = conflicts[c];
            chosen[n++] = profile;
        }
    return n > 0 && SessionVault_Decide(profiles, group, conflicts, chosen, n);
}

/* Web storage value `name` of `p` holds `part`. */
static BOOL WebHas(const Profile *p, const WCHAR *name, const char *part)
{
    WebStore *store = WebStore_Open(p);
    size_t length = 0;
    char *value = store ? WebStore_Get(store, name, &length) : NULL;
    BOOL has = value && strstr(value, part) != NULL;
    if (value) HeapFree(GetProcessHeap(), 0, value);
    WebStore_Free(store);
    return has;
}

/* The groups of `p` for `scope` made `groups`, as Claude would: its web storage written. */
static BOOL SetWebGroups(const Profile *p, const char *scope, const char *groups, const WCHAR *backup)
{
    WebStore *store = WebStore_Open(p);
    const char *keys[3] = { "state", "customGroupsByScope", scope };
    size_t length = 0, setLength = 0;
    char *value = store ? WebStore_Get(store, L"dframe-store", &length) : NULL, *set = NULL;
    BOOL ok = value && (set = Core_JsonSetNested(value, length, keys, 3, groups, &setLength)) != NULL &&
              WebStore_Set(store, L"dframe-store", set, setLength) && WebStore_Commit(store, backup);
    if (value) HeapFree(GetProcessHeap(), 0, value);
    if (set) HeapFree(GetProcessHeap(), 0, set);
    WebStore_Free(store);
    return ok;
}

static ULONGLONG SizeOf(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) return 0;
    return ((ULONGLONG)attributes.nFileSizeHigh << 32) | attributes.nFileSizeLow;
}

/* The sidebar's groups, sections and settings, which Claude's web UI keeps in
 * its web storage: kept the same there, for each profile's own account and
 * organization, with Claude's copies of them alike; never while it runs. */
static void TestWebLayout(void)
{
    static const WCHAR *const labels[2] = { L"Web-A", L"Web-B" };
    static const WCHAR *const leaves[2] = { L"web\\A", L"web\\B" };
    static const WCHAR *const accounts[2] = { L"web-a", L"web-b" };
    static const WCHAR *const namesA[3] = { L"dframe-store", L"LSS-persisted.dframe-group-scopes", L"epitaxy-editor-prefs" };
    static const WCHAR *const namesB[2] = { L"dframe-store", L"LSS-persisted.dframe-group-scopes" };
    ProfileList profiles;
    SyncReport report;
    WCHAR entries[2][MAX_PATH], configs[2][MAX_PATH], logs[2][MAX_PATH], list[FOLDER_CCH], backup[MAX_PATH], copied[MAX_PATH];
    char storeA[2048], copyA[1024], groups[512];
    const char *valuesA[3], *valuesB[2];
    ULONGLONG sizes[2];
    static VaultConflict conflicts[32];
    HWND window;
    int i, count;
    BOOL ready = TRUE;

    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (i = 0; i < profiles.count && ready; i++)
        ready = PrepareProfile(&profiles.items[i], labels[i], leaves[i], accounts[i], L"web-organization", entries[i], ARRAYSIZE(entries[i])) &&
                Join(profiles.items[i].storageDir, L"claude_desktop_config.json", configs[i], ARRAYSIZE(configs[i])) &&
                Join(profiles.items[i].storageDir, L"Local Storage\\leveldb\\000003.log", logs[i], ARRAYSIZE(logs[i]));
    profiles.items[0].syncGroup = profiles.items[1].syncGroup = 4;
    StringCchPrintfA(storeA, sizeof storeA,
        "{\"state\":{\"collapsed\":false,\"customGroupsByScope\":{\"web-a/web-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"" UNSORTED_UTF8 "\"}],"
        "\"assignments\":{\"code:local_%ls\":\"cg-1\"},\"order\":{}}},"
        "\"codeSidebarByScope\":{\"web-a/web-organization\":{\"sections\":[{\"id\":\"cg-1\",\"kind\":\"manual\",\"members\":[\"code:local_%ls\"]}]}},"
        "\"navPinnedIds\":[\"projects\",\"artifacts\"],\"groupByByMode\":{\"code\":\"custom\"},\"sidebarRowCountsByScope\":{\"a\":1}},\"version\":1}",
        g_keptIds[0], g_keptIds[0]);
    StringCchPrintfA(copyA, sizeof copyA,
        "{\"value\":{\"web-a/web-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"" UNSORTED_UTF8 "\"}],"
        "\"assignments\":{\"code:local_%ls\":\"cg-1\"},\"order\":{}}},\"tabId\":\"\",\"timestamp\":1}", g_keptIds[0]);
    valuesA[0] = storeA;
    valuesA[1] = copyA;
    valuesA[2] = "{\"state\":{\"editorFont\":\"Consolas\",\"chatTextSize\":\"l\"},\"version\":0}";
    valuesB[0] = "{\"state\":{\"collapsed\":true,\"customGroupsByScope\":{\"web-b/web-organization\":{\"groups\":[],\"assignments\":{},\"order\":{}}},"
                 "\"navPinnedIds\":null,\"groupByByMode\":{},\"sidebarRowCountsByScope\":{\"b\":2}},\"version\":1}";
    valuesB[1] = "{\"value\":{},\"tabId\":\"\",\"timestamp\":5}";
    ready = ready && WriteKeptEntry(entries[0], g_keptIds[0], "Web one", L"C:\\Fixture", AtSecond(2)) &&
            MakeWebStorage(profiles.items[0].storageDir, namesA, valuesA, 3) && MakeWebStorage(profiles.items[1].storageDir, namesB, valuesB, 2) &&
            Save(configs[1], "{\"preferences\":{\"epitaxyPrefs\":{\"dframe-group-scopes\":{}}},\"other\":true}") &&
            SessionVault_GroupListName(4, list, ARRAYSIZE(list)) && Join(g_root, L"web-backup", backup, ARRAYSIZE(backup));
    Check("web layouts: fixtures created", ready);
    if (!ready) return;
    Check("web layouts: the web storage is read", WebHas(&profiles.items[0], L"dframe-store", "\"name\":\"" UNSORTED_UTF8 "\"") &&
          WebHas(&profiles.items[1], L"dframe-store", "\"collapsed\":true"));

    /* The first time, what the two hold each their own way is left to the person; the one they choose then goes across. */
    ZeroMemory(&report, sizeof report);
    ready = SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0;
    count = SessionVault_Conflicts(&profiles, 4, conflicts, ARRAYSIZE(conflicts));
    Check("web layouts: the first time, groups one held go across at once (merged element by element)",
          ready && report.conflicts == count && !HasConflict(conflicts, count, "groups", SYNC_ITEM_SIDEBAR, 0x3) &&
          WebHas(&profiles.items[1], L"dframe-store", "\"web-b/web-organization\":{\"groups\":[{\"id\":\"cg-1\""));
    Check("web layouts: a value held two ways is left to the person, each keeping its own",
          HasConflict(conflicts, count, "navPins", SYNC_ITEM_SIDEBAR, 0x3) && WebHas(&profiles.items[1], L"dframe-store", "\"navPinnedIds\":null"));
    Check("web layouts: the person chooses the first profile's", DecideItem(&profiles, 4, SYNC_ITEMS_ALL, 0) &&
          SessionVault_Conflicts(&profiles, 4, conflicts, ARRAYSIZE(conflicts)) == 0);
    ZeroMemory(&report, sizeof report);
    Check("web layouts: the group is kept", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && report.conflicts == 0);
    StringCchPrintfA(groups, sizeof groups, "\"customGroupsByScope\":{\"web-b/web-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"" UNSORTED_UTF8 "\"}],"
                     "\"assignments\":{\"code:local_%ls\":\"cg-1\"}", g_keptIds[0]);
    Check("web layouts: the groups went across, for the other profile's account", WebHas(&profiles.items[1], L"dframe-store", groups) &&
          !WebHas(&profiles.items[1], L"dframe-store", "web-a/web-organization"));
    Check("web layouts: the sections went across",
          WebHas(&profiles.items[1], L"dframe-store", "\"codeSidebarByScope\":{\"web-b/web-organization\":{\"sections\":[{\"id\":\"cg-1\""));
    Check("web layouts: the Edit sidebar choices and the grouping went across",
          WebHas(&profiles.items[1], L"dframe-store", "\"navPinnedIds\":[\"projects\",\"artifacts\"]") &&
          WebHas(&profiles.items[1], L"dframe-store", "\"groupByByMode\":{\"code\":\"custom\"}"));
    Check("web layouts: what else the web UI keeps stays", WebHas(&profiles.items[1], L"dframe-store", "\"collapsed\":true") &&
          WebHas(&profiles.items[1], L"dframe-store", "\"sidebarRowCountsByScope\":{\"b\":2}") &&
          WebHas(&profiles.items[0], L"dframe-store", "\"sidebarRowCountsByScope\":{\"a\":1}"));
    Check("web layouts: the web UI's copies are alike, with a new time",
          WebHas(&profiles.items[1], L"LSS-persisted.dframe-group-scopes", "{\"value\":{\"web-b/web-organization\":{\"groups\":[{\"id\":\"cg-1\"") &&
          !WebHas(&profiles.items[1], L"LSS-persisted.dframe-group-scopes", "\"timestamp\":5}") &&
          WebHas(&profiles.items[1], L"LSS-persisted.dframe-code-sections", "{\"value\":{\"web-b/web-organization\":{\"sections\""));
    Check("web layouts: the copies in the settings too",
          FileHas(configs[1], "\"dframe-group-scopes\":{\"web-b/web-organization\":{\"groups\":[{\"id\":\"cg-1\"") && FileHas(configs[1], "\"other\":true"));
    Check("web layouts: the editor's settings went across", WebHas(&profiles.items[1], L"epitaxy-editor-prefs", "\"editorFont\":\"Consolas\""));
    Check("web layouts: the web storage was backed up first",
          SUCCEEDED(StringCchPrintfW(copied, ARRAYSIZE(copied), L"%s\\%s\\Local Storage\\leveldb\\000003.log", report.backup, profiles.items[1].folder)) &&
          FileThere(copied) && SizeOf(copied) < SizeOf(logs[1]));

    sizes[0] = SizeOf(logs[0]);
    sizes[1] = SizeOf(logs[1]);
    ZeroMemory(&report, sizeof report);
    Check("web layouts: kept again, nothing is written", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 &&
          SizeOf(logs[0]) == sizes[0] && SizeOf(logs[1]) == sizes[1]);

    /* A group renamed in the other profile goes to the first. */
    Sleep(30);
    ready = SetWebGroups(&profiles.items[1], "web-b/web-organization", "{\"groups\":[{\"id\":\"cg-1\",\"name\":\"Renamed\"}],\"assignments\":{},\"order\":{}}",
                         backup);
    ZeroMemory(&report, sizeof report);
    Check("web layouts: a group renamed in one is renamed in the other",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 &&
          WebHas(&profiles.items[0], L"dframe-store", "\"web-a/web-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"Renamed\"}]") &&
          WebHas(&profiles.items[0], L"LSS-persisted.dframe-group-scopes", "\"name\":\"Renamed\"") &&
          FileHas(configs[0], "\"web-a/web-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"Renamed\"}]"));

    /* While the other profile's Claude runs, its web storage is not written: the change waits until it closes. */
    window = StartFakeClaude(&profiles.items[1]);
    Sleep(30);
    ready = window && SetWebGroups(&profiles.items[0], "web-a/web-organization",
                                   "{\"groups\":[{\"id\":\"cg-2\",\"name\":\"Later\"}],\"assignments\":{},\"order\":{}}", backup);
    sizes[1] = SizeOf(logs[1]);
    ZeroMemory(&report, sizeof report);
    Check("web layouts: a running profile's web storage is not written",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && (report.waiting & 0x2) && SizeOf(logs[1]) == sizes[1] &&
          !WebHas(&profiles.items[1], L"dframe-store", "\"name\":\"Later\""));
    StopFakeClaude(window);
    Check("web layouts: it gets the change once it closes", SessionEdit_ApplyPending(NULL, &profiles.items[1]) > 0 &&
          WebHas(&profiles.items[1], L"dframe-store", "\"web-b/web-organization\":{\"groups\":[{\"id\":\"cg-2\",\"name\":\"Later\"}]"));
}

/* Value `name` of the web storage of `p` with the member reached through
 * `keys` set to `raw`, as Claude would write it. */
static BOOL SetWebMember(const Profile *p, const WCHAR *name, const char *const *keys, int depth, const char *raw, const WCHAR *backup)
{
    WebStore *store = WebStore_Open(p);
    size_t length = 0, setLength = 0;
    char *value = store ? WebStore_Get(store, name, &length) : NULL, *set = NULL;
    BOOL ok = value && (set = Core_JsonSetNested(value, length, keys, depth, raw, &setLength)) != NULL &&
              WebStore_Set(store, name, set, setLength) && WebStore_Commit(store, backup);
    if (value) HeapFree(GetProcessHeap(), 0, value);
    if (set) HeapFree(GetProcessHeap(), 0, set);
    WebStore_Free(store);
    return ok;
}

/* What profiles sync, chosen item by item: Claude's settings and its window's
 * copies of them, plain text values, its own files (Chromium's zoom under
 * each profile's own key), each session's own values made the same session
 * by session; what is not chosen stays, sessions too. */
static void TestSyncItems(void)
{
    static const WCHAR *const labels[2] = { L"Items-A", L"Items-B" };
    static const WCHAR *const leaves[2] = { L"items\\A", L"items\\B" };
    static const WCHAR *const accounts[2] = { L"items-a", L"items-b" };
    static const WCHAR *const namesA[4] = { L"spa:locale", L"default-model", L"epitaxy.sidePaneStore.v1", L"epitaxy-session-result:local_cdcdcdcd-0000-4000-8000-000000000001" };
    static const WCHAR *const namesB[3] = { L"spa:locale", L"default-model", L"epitaxy.sidePaneStore.v1" };
    static const WCHAR *const copyName[1] = { L"LSS-persisted.ccd-sessions-filter" };
    ProfileList profiles;
    SyncReport report;
    WCHAR entries[2][MAX_PATH], configs[2][MAX_PATH], apps[2][MAX_PATH], chromium[2][MAX_PATH], logs[2][MAX_PATH];
    WCHAR list[FOLDER_CCH], backup[MAX_PATH];
    char paneA[1024], paneB[1024], part[256];
    const char *valuesA[4], *valuesB[3], *copyValue[1];
    const char *pane[3] = { "state", "tileLayoutBySession", NULL };
    char id0[64], id1[64];
    ULONGLONG sizes[2];
    static VaultConflict conflicts[32];
    int count;
    HWND window;
    int i;
    BOOL ready = TRUE;

    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (i = 0; i < profiles.count && ready; i++)
        ready = PrepareProfile(&profiles.items[i], labels[i], leaves[i], accounts[i], L"items-organization", entries[i], ARRAYSIZE(entries[i])) &&
                Join(profiles.items[i].storageDir, L"claude_desktop_config.json", configs[i], ARRAYSIZE(configs[i])) &&
                Join(profiles.items[i].storageDir, L"config.json", apps[i], ARRAYSIZE(apps[i])) &&
                Join(profiles.items[i].storageDir, L"Preferences", chromium[i], ARRAYSIZE(chromium[i])) &&
                Join(profiles.items[i].storageDir, L"Local Storage\\leveldb\\000003.log", logs[i], ARRAYSIZE(logs[i]));
    profiles.items[0].syncGroup = profiles.items[1].syncGroup = 5;
    profiles.items[0].syncItems = profiles.items[1].syncItems = SYNC_ITEMS_ALL & ~SYNC_ITEM_MODEL;
    StringCchCopyW(profiles.defaultFolder, ARRAYSIZE(profiles.defaultFolder), profiles.items[0].folder);
    StringCchPrintfA(id0, sizeof id0, "local_%ls", g_keptIds[0]);
    StringCchPrintfA(id1, sizeof id1, "local_%ls", g_keptIds[1]);
    StringCchPrintfA(paneA, sizeof paneA,
        "{\"state\":{\"tileLayout\":{\"root\":\"a\"},\"tileLayoutBySession\":{\"%s\":{\"root\":\"s0\"},"
        "\"local_ffffffff-0000-4000-8000-000000000009\":{\"root\":\"x\"}},\"currentSessionId\":\"%s\"},\"version\":6}", id0, id0);
    StringCchPrintfA(paneB, sizeof paneB,
        "{\"state\":{\"tileLayout\":{\"root\":\"b\"},\"tileLayoutBySession\":{\"%s\":{\"root\":\"s1\"}},\"currentSessionId\":\"%s\"},\"version\":6}",
        id1, id1);
    valuesA[0] = "zh-CN";
    valuesA[1] = "claude-opus-5-5";
    valuesA[2] = paneA;
    valuesA[3] = "{\"v\":2,\"costUSD\":1.5}";
    valuesB[0] = "en-US";
    valuesB[1] = "claude-sonnet-5-5";
    valuesB[2] = paneB;
    copyValue[0] = "{\"value\":{\"state\":{\"selectedProjects\":[]},\"version\":0},\"tabId\":\"\",\"timestamp\":5}";
    ready = ready && WriteKeptEntry(entries[0], g_keptIds[0], "Items one", L"C:\\Fixture", AtSecond(2)) &&
            WriteKeptEntry(entries[1], g_keptIds[1], "Items two", L"C:\\Fixture", AtSecond(3)) &&
            MakeWebStorage(profiles.items[0].storageDir, namesA, valuesA, 4) && MakeWebStorage(profiles.items[1].storageDir, namesB, valuesB, 3) &&
            Save(configs[0], "{\"preferences\":{\"ccAutoArchiveOnPrClose\":true,\"ccdScheduledTasksEnabled\":true,\"localAgentModeTrustedFolders\":[\"C:\\\\Trusted\"],"
                             "\"epitaxyPrefs\":{\"ccd-sessions-filter\":{\"state\":{\"selectedProjects\":[\"C:\\\\A\"]},\"version\":0}}}}") &&
            Save(configs[1], "{\"preferences\":{\"keep\":1,\"ccdScheduledTasksEnabled\":false}}") &&
            Save(apps[0], "{\"lastKnownAccountUuid\":\"items-a\",\"locale\":\"zh-CN\",\"windowControlsZoomFactor\":1.25}") &&
            Save(apps[1], "{\"lastKnownAccountUuid\":\"items-b\",\"locale\":\"en-US\"}") &&
            Save(chromium[0], "{\"partition\":{\"per_host_zoom_levels\":{\"111\":{\"claude.ai\":0.75}}},\"spellcheck\":{\"dictionaries\":[\"en-US\",\"fr\"]}}") &&
            Save(chromium[1], "{\"partition\":{\"per_host_zoom_levels\":{\"222\":{\"claude.ai\":0.5}}},\"spellcheck\":{\"dictionaries\":[\"en-US\"]}}") &&
            SessionVault_GroupListName(5, list, ARRAYSIZE(list)) && Join(g_root, L"items-backup", backup, ARRAYSIZE(backup));
    /* B's window keeps its own copy of a setting: a value LevelDB adds with a later batch. */
    if (ready) {
        WebStore *store = WebStore_Open(&profiles.items[1]);
        ready = store && WebStore_Set(store, copyName[0], copyValue[0], strlen(copyValue[0])) && WebStore_Commit(store, backup);
        WebStore_Free(store);
    }
    Check("sync items: fixtures created", ready);
    if (!ready) return;

    ZeroMemory(&report, sizeof report);
    ready = SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0;
    count = SessionVault_Conflicts(&profiles, 5, conflicts, ARRAYSIZE(conflicts));
    Check("sync items: the first time, values held two ways are left to the person, each keeping its own",
          ready && HasConflict(conflicts, count, "codeSchedules", SYNC_ITEM_SETTINGS, 0x3) && HasConflict(conflicts, count, "locale", SYNC_ITEM_LANGUAGE, 0x3) &&
          HasConflict(conflicts, count, "paneLayout", SYNC_ITEM_DETAILS, 0x3) && FileHas(configs[1], "\"ccdScheduledTasksEnabled\":false") &&
          WebHas(&profiles.items[1], L"spa:locale", "en-US"));
    Check("sync items: a part only one holds goes across at once", FileHas(configs[1], "\"ccAutoArchiveOnPrClose\":true"));
    Check("sync items: the person chooses the first profile's", DecideItem(&profiles, 5, SYNC_ITEMS_ALL, 0));
    ZeroMemory(&report, sizeof report);
    Check("sync items: the group is kept", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && report.conflicts == 0);
    Check("sync items: the settings went across, a window's to its own copy too",
          FileHas(configs[1], "\"ccAutoArchiveOnPrClose\":true") && FileHas(configs[1], "\"keep\":1") && FileHas(configs[1], "\"selectedProjects\":[\"C:\\\\A\"]") &&
          WebHas(&profiles.items[1], copyName[0], "\"selectedProjects\":[\"C:\\\\A\"]") && !WebHas(&profiles.items[1], copyName[0], "\"timestamp\":5}"));
    Check("sync items: the person's choice went across, shorter as it is",
          FileHas(configs[1], "\"ccdScheduledTasksEnabled\":true") && FileHas(configs[0], "\"ccdScheduledTasksEnabled\":true"));
    Check("sync items: permissions chosen, the trusted folders went across", FileHas(configs[1], "\"localAgentModeTrustedFolders\":[\"C:\\\\Trusted\"]"));
    Check("sync items: the language went across, as plain text and in config.json",
          WebHas(&profiles.items[1], L"spa:locale", "zh-CN") && FileHas(apps[1], "\"locale\":\"zh-CN\"") &&
          FileHas(apps[1], "\"windowControlsZoomFactor\":1.25") && FileHas(apps[1], "\"lastKnownAccountUuid\":\"items-b\""));
    Check("sync items: the default model, not chosen, stays", WebHas(&profiles.items[1], L"default-model", "claude-sonnet-5-5") &&
          WebHas(&profiles.items[0], L"default-model", "claude-opus-5-5"));
    Check("sync items: Chromium's zoom under the profile's own key, its spelling languages",
          FileHas(chromium[1], "\"222\":{\"claude.ai\":0.75}") && !FileHas(chromium[1], "\"111\"") &&
          FileHas(chromium[1], "\"dictionaries\":[\"en-US\",\"fr\"]"));
    StringCchPrintfA(part, sizeof part, "\"%s\":{\"root\":\"s0\"}", id0);
    Check("sync items: a session's pane goes to the profile without it", WebHas(&profiles.items[1], L"epitaxy.sidePaneStore.v1", part));
    StringCchPrintfA(part, sizeof part, "\"%s\":{\"root\":\"s1\"}", id1);
    Check("sync items: ...both ways, each session's own", WebHas(&profiles.items[0], L"epitaxy.sidePaneStore.v1", part) &&
          WebHas(&profiles.items[1], L"epitaxy.sidePaneStore.v1", part));
    StringCchPrintfA(part, sizeof part, "\"currentSessionId\":\"%s\"", id1);
    Check("sync items: a session no profile lists, and what is not synced, stay where they are",
          !WebHas(&profiles.items[1], L"epitaxy.sidePaneStore.v1", "ffffffff") && WebHas(&profiles.items[0], L"epitaxy.sidePaneStore.v1", "ffffffff") &&
          WebHas(&profiles.items[1], L"epitaxy.sidePaneStore.v1", part) && WebHas(&profiles.items[1], L"epitaxy.sidePaneStore.v1", "\"tileLayout\":{\"root\":\"a\"}"));
    Check("sync items: a session's cost, a value of its own", WebHas(&profiles.items[1], L"epitaxy-session-result:local_cdcdcdcd-0000-4000-8000-000000000001", "\"costUSD\":1.5"));

    sizes[0] = SizeOf(logs[0]);
    sizes[1] = SizeOf(logs[1]);
    ZeroMemory(&report, sizeof report);
    Check("sync items: kept again, nothing is written", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 &&
          SizeOf(logs[0]) == sizes[0] && SizeOf(logs[1]) == sizes[1]);

    /* Two sessions' panes changed at once, one in each profile: both changes kept. */
    Sleep(30);
    pane[2] = id0;
    ready = SetWebMember(&profiles.items[1], L"epitaxy.sidePaneStore.v1", pane, 3, "{\"root\":\"s0b\"}", backup);
    pane[2] = id1;
    ready = ready && SetWebMember(&profiles.items[0], L"epitaxy.sidePaneStore.v1", pane, 3, "{\"root\":\"s1a\"}", backup);
    ZeroMemory(&report, sizeof report);
    ready = ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0;
    for (i = 0; i < 2 && ready; i++) {
        StringCchPrintfA(part, sizeof part, "\"%s\":{\"root\":\"s0b\"}", id0);
        ready = WebHas(&profiles.items[i], L"epitaxy.sidePaneStore.v1", part);
        StringCchPrintfA(part, sizeof part, "\"%s\":{\"root\":\"s1a\"}", id1);
        ready = ready && WebHas(&profiles.items[i], L"epitaxy.sidePaneStore.v1", part);
    }
    Check("sync items: each session's change goes across, though the other profile changed another's", ready);

    /* Sessions not chosen: a new one stays in its profile, and goes once they are again. */
    profiles.items[0].syncItems = profiles.items[1].syncItems = SYNC_ITEM_SIDEBAR;
    ready = WriteKeptEntry(entries[0], g_keptIds[2], "Items three", L"C:\\Fixture", AtSecond(4));
    ZeroMemory(&report, sizeof report);
    Check("sync items: sessions not chosen stay in their profile",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && !EntryThere(entries[1], g_keptIds[2]) &&
          EntryThere(entries[1], g_keptIds[0]) && EntryThere(entries[0], g_keptIds[1]));
    /* The default profile open, a change waiting for it: what it shows of a part synced for the first time still counts. */
    window = StartFakeClaude(&profiles.items[0]);
    Sleep(30);
    ready = window && Save(configs[1], "{\"preferences\":{\"keep\":1,\"ccdScheduledTasksEnabled\":true,"
                                       "\"epitaxyPrefs\":{\"ccd-sessions-filter\":{\"state\":{\"selectedProjects\":[\"C:\\\\B\"]},\"version\":0}}}}");
    ZeroMemory(&report, sizeof report);
    ready = ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && (report.waiting & 0x1);
    profiles.items[0].syncItems = profiles.items[1].syncItems = 0;
    ZeroMemory(&report, sizeof report);
    Check("sync items: chosen again, the new session goes across",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && EntryThere(entries[1], g_keptIds[2]));
    count = SessionVault_Conflicts(&profiles, 5, conflicts, ARRAYSIZE(conflicts));
    Check("sync items: a part kept for the first time, held two ways, is left to the person though one of them is open",
          HasConflict(conflicts, count, "defaultModel", SYNC_ITEM_MODEL, 0x3) && WebHas(&profiles.items[1], L"default-model", "claude-sonnet-5-5"));
    ready = DecideItem(&profiles, 5, SYNC_ITEM_MODEL, 0);
    ZeroMemory(&report, sizeof report);
    Check("sync items: the open profile's version, chosen, goes to the other",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 &&
          WebHas(&profiles.items[1], L"default-model", "claude-opus-5-5") && WebHas(&profiles.items[0], L"default-model", "claude-opus-5-5"));
    StopFakeClaude(window);
    Check("sync items: the waiting change is made once it closes", SessionEdit_ApplyPending(NULL, &profiles.items[0]) > 0 &&
          FileHas(configs[0], "\"selectedProjects\":[\"C:\\\\B\"]") && WebHas(&profiles.items[0], L"default-model", "claude-opus-5-5"));
    Check("sync items: unchosen, a group syncs everything", SessionVault_GroupItems(&profiles, 0x3) == SYNC_ITEMS_ALL &&
          SYNC_ITEMS_DEFAULT == SYNC_ITEMS_ALL);
}

/* --------------------------------------------- one sync at a time, conflicts */

typedef struct LockProbe {
    volatile LONG taken;
} LockProbe;

static DWORD WINAPI TakeSyncLock(void *context)
{
    LockProbe *probe = (LockProbe *)context;
    HANDLE lock = Util_SyncLock();
    InterlockedExchange(&probe->taken, lock ? 1 : -1);
    Util_SyncUnlock(lock);
    return 0;
}

/* The local_*.json files of an entries folder. */
static int CountEntries(const WCHAR *entries)
{
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(entries, L"local_*.json", &found, FALSE);
    int count = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do count++; while (FindNextFileW(find, &found));
    FindClose(find);
    return count;
}

/* One sync at a time; a profile's view a change never reached is no change;
 * two changes each their own way left to the person; the sidebar marked for
 * the account's server; a profile wiped deletes nothing; an entry Claude's
 * startup damaged is healed from a sound one. */
static void TestReliableSync(void)
{
    static const WCHAR *const labels[2] = { L"Rel-A", L"Rel-B" };
    static const WCHAR *const leaves[2] = { L"rel\\A", L"rel\\B" };
    static const WCHAR *const accounts[2] = { L"rel-a", L"rel-b" };
    static const WCHAR *const names[1] = { L"dframe-store" };
    ProfileList profiles;
    SyncReport report;
    WCHAR entries[2][MAX_PATH], list[FOLDER_CCH], backup[MAX_PATH], path[MAX_PATH], moved[MAX_PATH], id[SESSION_ID_CCH], plan[MAX_PATH];
    char store[2][1024], json[1024];
    const char *values[2][1];
    static VaultConflict conflicts[16];
    LockProbe probe;
    HANDLE lock, thread;
    HWND window;
    int i, count;
    BOOL ready = TRUE;

    /* One sync at a time: a second waits for the first. */
    ZeroMemory(&probe, sizeof probe);
    lock = Util_SyncLock();
    thread = lock ? CreateThread(NULL, 0, TakeSyncLock, &probe, 0, NULL) : NULL;
    if (thread) Sleep(300);
    Check("one sync at a time: a second sync waits while one runs", lock && thread && probe.taken == 0);
    Util_SyncUnlock(lock);
    if (thread) {
        WaitForSingleObject(thread, 10000);
        CloseHandle(thread);
    }
    Check("one sync at a time: it goes on once the first is done", probe.taken == 1);

    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    for (i = 0; i < profiles.count && ready; i++)
        ready = PrepareProfile(&profiles.items[i], labels[i], leaves[i], accounts[i], L"rel-organization", entries[i], ARRAYSIZE(entries[i]));
    profiles.items[0].syncGroup = profiles.items[1].syncGroup = 6;
    StringCchCopyW(profiles.defaultFolder, ARRAYSIZE(profiles.defaultFolder), profiles.items[0].folder);
    for (i = 0; i < 2; i++) {
        StringCchPrintfA(store[i], sizeof store[i],
            "{\"state\":{\"customGroupsByScope\":{\"%ls/rel-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"%s\"}],\"assignments\":{},\"order\":{}}}},"
            "\"version\":1}", accounts[i], i == 0 ? "Work" : "Home");
        values[i][0] = store[i];
    }
    ready = ready && WriteKeptEntry(entries[0], g_keptIds[0], "Rel one", L"C:\\Fixture", AtSecond(2)) &&
            WriteKeptEntry(entries[0], g_keptIds[1], "Rel two", L"C:\\Fixture", AtSecond(3)) &&
            MakeWebStorage(profiles.items[0].storageDir, names, values[0], 1) && MakeWebStorage(profiles.items[1].storageDir, names, values[1], 1) &&
            SessionVault_GroupListName(6, list, ARRAYSIZE(list)) && Join(g_root, L"rel-backup", backup, ARRAYSIZE(backup));
    profiles.items[0].syncItems = profiles.items[1].syncItems = SYNC_ITEM_SESSIONS | SYNC_ITEM_SIDEBAR;
    Check("reliable sync: fixtures created", ready);
    if (!ready) return;

    /* The first time the groups differ: the person chooses the first profile's. */
    ZeroMemory(&report, sizeof report);
    ready = SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.conflicts == 1 &&
            DecideItem(&profiles, 6, SYNC_ITEM_SIDEBAR, 0);
    ZeroMemory(&report, sizeof report);
    ready = ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && report.conflicts == 0;
    Check("reliable sync: the groups chosen went across", ready && WebHas(&profiles.items[1], L"dframe-store", "\"rel-b/rel-organization\":{\"groups\":[{\"id\":\"cg-1\",\"name\":\"Work\"}]"));
    Check("reliable sync: Claude is told to send them to the account's server at its next start",
          WebHas(&profiles.items[1], L"ccd-sync-pending:ccd/dframe-store", "rel-b/rel-organization"));

    /* A change sent to a profile open at the time, then lost before it closed: what it still shows is no change. */
    window = StartFakeClaude(&profiles.items[1]);
    Sleep(30);
    ready = window && SetWebGroups(&profiles.items[0], "rel-a/rel-organization", "{\"groups\":[{\"id\":\"cg-1\",\"name\":\"Work 2\"}],\"assignments\":{},\"order\":{}}",
                                   backup);
    ZeroMemory(&report, sizeof report);
    ready = ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && (report.waiting & 0x2) && SessionSync_PlanPath(&profiles.items[1], plan, ARRAYSIZE(plan));
    ready = ready && DeleteFileW(plan);   /* what waited for it is gone */
    Sleep(30);
    ready = ready && SetWebGroups(&profiles.items[0], "rel-a/rel-organization", "{\"groups\":[{\"id\":\"cg-1\",\"name\":\"Work 3\"}],\"assignments\":{},\"order\":{}}",
                                  backup);
    ZeroMemory(&report, sizeof report);
    Check("reliable sync: what an open profile shows of a change that never reached it is no change",
          ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.conflicts == 0 &&
          WebHas(&profiles.items[0], L"dframe-store", "\"name\":\"Work 3\""));
    StopFakeClaude(window);
    SessionEdit_ApplyPending(NULL, &profiles.items[1]);
    Check("reliable sync: it gets the latest once it closes", WebHas(&profiles.items[1], L"dframe-store", "\"name\":\"Work 3\""));

    /* Both change it each their own way: each keeps its own until the person chooses. */
    Sleep(30);
    ready = SetWebGroups(&profiles.items[0], "rel-a/rel-organization", "{\"groups\":[{\"id\":\"cg-1\",\"name\":\"From A\"}],\"assignments\":{},\"order\":{}}", backup);
    Sleep(30);   /* B's change written later than A's: the same clock tick would leave them equal */
    ready = ready && SetWebGroups(&profiles.items[1], "rel-b/rel-organization", "{\"groups\":[{\"id\":\"cg-1\",\"name\":\"From B\"}],\"assignments\":{},\"order\":{}}", backup);
    ZeroMemory(&report, sizeof report);
    ready = ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report);
    count = SessionVault_Conflicts(&profiles, 6, conflicts, ARRAYSIZE(conflicts));
    Check("reliable sync: two changes each their own way are left to the person, each keeping its own",
          ready && count == 1 && HasConflict(conflicts, count, "groups", SYNC_ITEM_SIDEBAR, 0x3) &&
          WebHas(&profiles.items[0], L"dframe-store", "\"name\":\"From A\"") && WebHas(&profiles.items[1], L"dframe-store", "\"name\":\"From B\""));
    Check("reliable sync: the conflict is the group's name alone, named as at the last sync, each version shown, the later one marked",
          count == 1 && strcmp(conflicts[0].path, "/groups/#cg-1/name") == 0 && wcsstr(conflicts[0].label, L"Work 3") != NULL &&
          wcscmp(conflicts[0].shown[0], L"From A") == 0 && wcscmp(conflicts[0].shown[1], L"From B") == 0 && conflicts[0].latest == 1);
    ZeroMemory(&report, sizeof report);
    Check("reliable sync: kept again, they stay left to the person", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.conflicts == 1);
    ZeroMemory(&report, sizeof report);
    Check("reliable sync: the version chosen goes across", DecideItem(&profiles, 6, SYNC_ITEM_SIDEBAR, 1) &&
          SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.conflicts == 0 &&
          WebHas(&profiles.items[0], L"dframe-store", "\"name\":\"From B\"") && SessionVault_Conflicts(&profiles, 6, conflicts, ARRAYSIZE(conflicts)) == 0);

    /* A profile whose sessions are all gone (its storage wiped): nothing is deleted elsewhere, and it gets them back. */
    ready = Join(g_root, L"rel-moved", moved, ARRAYSIZE(moved)) && MakeDir(moved) && EntryPath(entries[1], g_keptIds[0], path, ARRAYSIZE(path));
    for (i = 0; ready && i < 2; i++) {
        WCHAR to[MAX_PATH];
        ready = EntryPath(entries[1], g_keptIds[i], path, ARRAYSIZE(path)) && Join(moved, g_keptIds[i], to, ARRAYSIZE(to)) && MoveFileW(path, to);
    }
    ZeroMemory(&report, sizeof report);
    Check("reliable sync: a profile that lists nothing any more deletes nothing elsewhere, and gets its sessions back",
          ready && CountEntries(entries[1]) == 0 && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 && report.removed == 0 &&
          EntryThere(entries[0], g_keptIds[0]) && EntryThere(entries[0], g_keptIds[1]) && EntryThere(entries[1], g_keptIds[0]) &&
          EntryThere(entries[1], g_keptIds[1]));

    /* Claude's startup damaged an entry: its session's id stripped, its transcript said to be gone, and written last. */
    StringCchPrintfW(id, ARRAYSIZE(id), L"local_%s", g_keptIds[0]);
    StringCchPrintfA(json, sizeof json,
        "{\"sessionId\":\"local_%ls\",\"cwd\":\"C:\\\\Fixture\",\"title\":\"Rel one\",\"transcriptUnavailable\":true,\"lastActivityAt\":%I64u}",
        g_keptIds[0], AtSecond(50));
    Sleep(30);
    ready = EntryPath(entries[1], g_keptIds[0], path, ARRAYSIZE(path)) && Save(path, json);
    ZeroMemory(&report, sizeof report);
    ready = ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0;
    Check("reliable sync: a damaged entry never goes to the other profile", ready && EntryPath(entries[0], g_keptIds[0], path, ARRAYSIZE(path)) &&
          !FileHas(path, "transcriptUnavailable") && FileHas(path, "\"cliSessionId\""));
    Check("reliable sync: it is healed from the sound one, under its own id, with no second entry",
          EntryPath(entries[1], g_keptIds[0], path, ARRAYSIZE(path)) && !FileHas(path, "transcriptUnavailable") && FileHas(path, "\"cliSessionId\"") &&
          CountEntries(entries[1]) == 2);
    ZeroMemory(&report, sizeof report);
    Check("reliable sync: kept again, nothing changes", SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.failed == 0 &&
          report.added + report.updated + report.removed == 0);
    {
        DWORD length = 0;
        char *whole = NULL;
        SessionSet partial;
        ZeroMemory(&partial, sizeof partial);
        ready = EntryPath(entries[1], g_keptIds[0], path, ARRAYSIZE(path)) &&
                (whole = Util_ReadFile(path, SESSION_ENTRY_MAX_BYTES, FALSE, &length)) != NULL &&
                Save(path, "{\"sessionId\":\"local_partial\",\"cliSessionId\":\"unfinished");
        Check("an interrupted entry is recorded as an unreadable snapshot",
              ready && SessionStore_LoadProfiles(&partial, &profiles) && partial.source[1].unreadable == 1 && !partial.source[1].complete);
        SessionStore_Free(&partial);
        ZeroMemory(&report, sizeof report);
        Check("an interrupted entry cannot delete the sound copy in another profile",
              ready && SessionVault_Keep(&profiles, 0x3, list, TRUE, &report) && report.removed == 0 && EntryThere(entries[0], g_keptIds[0]));
        if (whole) {
            Check("the complete private entry is put back", WriteWholeFile(path, whole));
            HeapFree(GetProcessHeap(), 0, whole);
        }
    }
}

typedef struct LedgerProbe {
    HANDLE entered, done;
    const WCHAR *id;
    BOOL undelete;
    int result;
} LedgerProbe;

static DWORD WINAPI ChangeLedger(void *context)
{
    LedgerProbe *probe = (LedgerProbe *)context;
    ProfileList profiles;
    SyncReport report;
    ZeroMemory(&profiles, sizeof profiles);
    ZeroMemory(&report, sizeof report);
    SetEvent(probe->entered);
    probe->result = probe->undelete ? SessionVault_Undelete(&profiles, &probe->id, 1, &report)
                                   : (int)SessionVault_AddDeleted(&probe->id, 1);
    SetEvent(probe->done);
    return 0;
}

static void TestLedgerTransactions(void)
{
    static const WCHAR *const first = L"abababab-0000-4000-8000-00000000f001";
    LedgerProbe probe;
    HANDLE lock, thread;
    VaultIds listed, deleted;
    int i;
    ZeroMemory(&probe, sizeof probe);
    probe.id = L"abababab-0000-4000-8000-00000000f002";
    probe.entered = CreateEventW(NULL, TRUE, FALSE, NULL);
    probe.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    for (i = 0; i < 2 && probe.entered && probe.done; i++) {
        probe.undelete = i == 1;
        ResetEvent(probe.entered);
        ResetEvent(probe.done);
        lock = Util_SyncLock();
        thread = lock ? CreateThread(NULL, 0, ChangeLedger, &probe, 0, NULL) : NULL;
        if (thread) WaitForSingleObject(probe.entered, 10000);
        Sleep(300);
        Check("a deleted-ledger transaction waits for another sync's commit",
              lock && thread && WaitForSingleObject(probe.done, 0) == WAIT_TIMEOUT);
        Check("the first ledger change commits under the held sync lock", lock && SessionVault_AddDeleted(&first, 1));
        Util_SyncUnlock(lock);
        if (thread) {
            WaitForSingleObject(thread, 10000);
            CloseHandle(thread);
        }
        Check("the waiting ledger transaction finishes after the lock is released", probe.result == (i == 0 ? 1 : 0));
    }
    Check("serialized deleted-ledger changes preserve both ids", SessionVault_Ids(&listed, &deleted) &&
          SessionVault_HasId(&deleted, first) && SessionVault_HasId(&deleted, probe.id));
    SessionVault_FreeIds(&listed);
    SessionVault_FreeIds(&deleted);
    if (probe.entered) CloseHandle(probe.entered);
    if (probe.done) CloseHandle(probe.done);
}

/* Environment variable `name` kept to be put back: `*kept` NULL when it is
 * not set. FALSE when it could not be kept. */
static BOOL KeepVariable(const WCHAR *name, WCHAR **kept)
{
    DWORD cch = GetEnvironmentVariableW(name, NULL, 0);
    *kept = NULL;
    if (!cch) return TRUE;
    *kept = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (size_t)cch * sizeof(WCHAR));
    return *kept && GetEnvironmentVariableW(name, *kept, cch) < cch;
}

/* A profile's sign-in key, as a backup for any PC takes it (Backup_ReadAppKey)
 * and a restore on another PC writes it (Backup_WriteAppKey): Local State's
 * os_crypt key, "DPAPI" and its DPAPI blob in base64, opened for this Windows
 * user; written protected again, Local State's other members kept. */
static void TestAccountKey(void)
{
    static const BYTE kKey[32] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
    WCHAR storage[MAX_PATH], localState[MAX_PATH];
    DATA_BLOB in, out;
    BYTE blob[512], read[64], other[32];
    char encoded[1024], json[1400];
    size_t length = 0, i;
    DWORD size = 0;
    char *after;
    BOOL ready;
    ready = FixturePath(L"account-key", storage, ARRAYSIZE(storage)) && Util_EnsureDir(storage) && Join(storage, L"Local State", localState, ARRAYSIZE(localState));
    ZeroMemory(&out, sizeof out);
    in.pbData = (BYTE *)kKey;
    in.cbData = sizeof kKey;
    ready = ready && CryptProtectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (ready) {
        memcpy(blob, "DPAPI", 5);
        memcpy(blob + 5, out.pbData, out.cbData);
        ready = Core_Base64Encode(blob, out.cbData + 5, encoded, sizeof encoded) > 0;
        LocalFree(out.pbData);
    }
    ready = ready && SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"browser\":{\"enabled_labs_experiments\":[]},\"os_crypt\":{\"audit_enabled\":true,\"encrypted_key\":\"%s\"},\"profile\":{}}", encoded)) &&
            WriteWholeFile(localState, json);
    Check("account key: fixture written", ready);
    if (!ready) return;
    Check("account key: Local State's key, opened for this Windows user", Backup_ReadAppKey(storage, read, sizeof read, &length) &&
          length == sizeof kKey && memcmp(read, kKey, sizeof kKey) == 0);
    for (i = 0; i < sizeof other; i++) other[i] = (BYTE)(200 + i);
    Check("account key: another written, protected again", Backup_WriteAppKey(storage, other, sizeof other) &&
          Backup_ReadAppKey(storage, read, sizeof read, &length) && length == sizeof other && memcmp(read, other, sizeof other) == 0);
    after = Util_ReadFile(localState, 64 * 1024, FALSE, &size);
    Check("account key: Local State's other members kept", after && strstr(after, "\"enabled_labs_experiments\":[]") && strstr(after, "\"audit_enabled\":true") &&
          strstr(after, "\"profile\":{}") && !strstr(after, encoded));
    if (after) HeapFree(GetProcessHeap(), 0, after);
    Check("account key: a key this Windows user cannot open is none",
          WriteWholeFile(localState, "{\"os_crypt\":{\"encrypted_key\":\"RFBBUElub3QgYSBibG9i\"}}") && !Backup_ReadAppKey(storage, read, sizeof read, &length) && length == 0);
    Check("account key: no Local State, none", DeleteFileW(localState) && !Backup_ReadAppKey(storage, read, sizeof read, &length));
}

static void TestCleanupReferences(const WCHAR *projects)
{
    static const WCHAR *const id = L"abababab-0000-4000-8000-00000000c001";
    ProfileList profiles;
    SessionSet set;
    SyncSend change;
    SyncReport report;
    WCHAR entries[2][MAX_PATH], transcript[MAX_PATH], account[MAX_PATH], error[LONG_PATH_CCH];
    PurgeItem *items = NULL;
    const PurgeItem *item;
    char json[512];
    int count, chosen, removed = -1;
    BOOL ready, denied;
    HWND window;
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 3;
    ready = PrepareProfile(&profiles.items[0], L"Purge-A", L"purge\\A", L"purge-a", L"organization", entries[0], ARRAYSIZE(entries[0])) &&
            PrepareProfile(&profiles.items[1], L"Purge-B", L"purge\\B", L"purge-b", L"organization", entries[1], ARRAYSIZE(entries[1])) &&
            SetProfile(&profiles.items[2], L"Uninitialized", L"purge\\unused", TRUE) &&
            SUCCEEDED(StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\fixture\\%s.jsonl", projects, id)) &&
            Save(transcript, "{\"type\":\"user\"}\n") && SetModified(transcript, 132000000000000000ULL);
    count = ready ? SessionPurge_List(&profiles, &items, error, ARRAYSIZE(error)) : -1;
    ZeroMemory(&set, sizeof set);
    Check("an absent entries directory is not a completed edit snapshot",
          ready && !SessionStore_LoadEntries(&set, &profiles.items[2]) && set.source[0].complete);
    SessionStore_Free(&set);
    item = count > 0 ? FindPurged(items, count, id) : NULL;
    Check("an uninitialized stock profile does not prevent listing unused conversations", item != NULL);
    if (!item) {
        if (items) HeapFree(GetProcessHeap(), 0, items);
        return;
    }
    chosen = (int)(item - items);
    ready = WriteSyncEntry(entries[0], id, "Restored", 1) && SUCCEEDED(StringCchCopyW(account, ARRAYSIZE(account), entries[0]));
    if (ready) *wcsrchr(account, L'\\') = 0;
    denied = ready && DenyDirectory(account, TRUE);
    Check("cleanup stops when an account directory can no longer be resolved",
          denied && SessionPurge_Delete(NULL, &profiles, items, &chosen, 1, &removed, error, ARRAYSIZE(error)) == REMOVE_FAILED &&
          removed == 0 && FileThere(transcript));
    {
        WCHAR old[MAX_PATH], config[MAX_PATH];
        HANDLE held = INVALID_HANDLE_VALUE;
        BOOL prepared = Join(profiles.items[0].storageDir, L"claude-code-sessions\\old\\organization", old, ARRAYSIZE(old)) && MakeDir(old) &&
                        Join(profiles.items[0].storageDir, L"config.json", config, ARRAYSIZE(config)) &&
                        (held = CreateFileW(config, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)) != INVALID_HANDLE_VALUE;
        Check("a visible empty old account cannot hide failed reads of the real account",
              denied && prepared && !SessionStore_LoadEntries(&set, &profiles.items[0]) && !set.source[0].complete && set.source[0].entriesDir[0]);
        SessionStore_Free(&set);
        if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    }
    if (denied) Check("the private account's access is restored", DenyDirectory(account, FALSE));
    if (ready) RemoveEntry(entries[0], id);

    ready = SessionStore_LoadProfiles(&set, &profiles);
    window = ready ? StartFakeClaude(&profiles.items[1]) : NULL;
    ZeroMemory(&change, sizeof change);
    change.profile = 1;
    change.op.kind = SYNC_PUT;
    change.op.flags = SYNC_UNDELETE;
    StringCchCopyW(change.op.key, ARRAYSIZE(change.op.key), id);
    StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"cwd\":\"C:\\\\Fixture\",\"title\":\"Waiting\"}", id, id);
    change.content = json;
    change.length = strlen(json);
    ZeroMemory(&report, sizeof report);
    Check("a session restored to a running profile waits in its plan", window && SessionSync_Send(&set, &change, 1, &report) && report.waiting == 0x2);
    Check("cleanup protects transcripts of queued session entries",
          window && SessionPurge_Delete(NULL, &profiles, items, &chosen, 1, &removed, error, ARRAYSIZE(error)) == REMOVE_FAILED &&
          removed == 0 && FileThere(transcript));
    StopFakeClaude(window);
    if (ready) SessionStore_Free(&set);
    Check("the queued entry can still be applied with its conversation intact",
          SessionEdit_ApplyPending(NULL, &profiles.items[1]) == 1 && EntryThere(entries[1], id) && FileThere(transcript));
    HeapFree(GetProcessHeap(), 0, items);
}

static void TestSearchCache(void)
{
    SessionSearch search;
    WCHAR path[MAX_PATH], *message;
    WIN32_FILE_ATTRIBUTE_DATA stamp;
    HANDLE file;
    volatile LONG stop = 0;
    BOOL ready;
    ULONGLONG hitOffset = 0;
    size_t hitEnd = 0;
    ZeroMemory(&search, sizeof search);
    search.stop = &stop;
    search.matchCase = TRUE;
    StringCchCopyW(search.text, ARRAYSIZE(search.text), L"Alpha");
    message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, SEARCH_LINE_CCH * sizeof(WCHAR));
    ready = message && FixturePath(L"search-cache.jsonl", path, ARRAYSIZE(path)) &&
            Save(path, "{\"type\":\"user\",\"message\":{\"content\":\"Alpha\"}}\n");
    Check("search cache fixture created", ready);
    if (!ready) { if (message) HeapFree(GetProcessHeap(), 0, message); return; }
    SessionSearch_Invalidate();
    g_searchReads = 0;
    Check("the initial search reads decoded conversation text", TranscriptMatches(&search, path, message, &hitOffset, &hitEnd) && g_searchReads == 1);
    Check("an unchanged file reuses its search result", TranscriptMatches(&search, path, message, &hitOffset, &hitEnd) && g_searchReads == 1);
    Check("a different query has its own cache entry", StringCchCopyW(search.text, ARRAYSIZE(search.text), L"Beta") == S_OK &&
          !TranscriptMatches(&search, path, message, &hitOffset, &hitEnd) && g_searchReads == 2);
    ready = Save(path, "{\"type\":\"user\",\"message\":{\"content\":\"Beta Beta\"}}\n");
    Check("changing file size invalidates cached results", ready && TranscriptMatches(&search, path, message, &hitOffset, &hitEnd) && g_searchReads == 3);
    ready = FileStamp(path, &stamp) && Save(path, "{\"type\":\"user\",\"message\":{\"content\":\"Zeta Zeta\"}}\n");
    file = ready ? CreateFileW(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) {
        ready = SetFileTime(file, NULL, NULL, &stamp.ftLastWriteTime);
        CloseHandle(file);
    } else ready = FALSE;
    SessionSearch_Invalidate();
    Check("a file notification invalidates even equal-size equal-time replacements",
          ready && !TranscriptMatches(&search, path, message, &hitOffset, &hitEnd) && g_searchReads == 4);
    StringCchCopyW(search.text, ARRAYSIZE(search.text), L"zeta");
    search.matchCase = TRUE;
    Check("case-sensitive searches remain separate", !TranscriptMatches(&search, path, message, &hitOffset, &hitEnd));
    search.matchCase = FALSE;
    Check("case-insensitive searches remain separate", TranscriptMatches(&search, path, message, &hitOffset, &hitEnd));
    search.regex = TRUE;
    StringCchCopyW(search.text, ARRAYSIZE(search.text), L"Zeta.*Zeta");
    Check("regular-expression searches retain message semantics", Core_RegexCompile(&search.compiled, search.text, FALSE) &&
          TranscriptMatches(&search, path, message, &hitOffset, &hitEnd));
    HeapFree(GetProcessHeap(), 0, message);
}

static void TestPrivateDiagnostics(void)
{
    ProfileList profiles;
    ClaudePackage pkg;
    WCHAR text[2048];
    ZeroMemory(&profiles, sizeof profiles);
    ZeroMemory(&pkg, sizeof pkg);
    profiles.count = 1;
    profiles.items[0].running = TRUE;
    StringCchCopyW(profiles.items[0].name, ARRAYSIZE(profiles.items[0].name), L"private@example.test");
    StringCchCopyW(profiles.items[0].folder, ARRAYSIZE(profiles.items[0].folder), L"Claude-SecretAccount");
    StringCchCopyW(profiles.items[0].storageDir, ARRAYSIZE(profiles.items[0].storageDir), L"C:\\Users\\SecretName\\Claude");
    StringCchCopyW(profiles.items[0].note, ARRAYSIZE(profiles.items[0].note), L"secret note");
    pkg.found = TRUE;
    StringCchCopyW(pkg.version, ARRAYSIZE(pkg.version), L"2.12345.0");
    Check("diagnostics include the installed version and numeric profile state",
          Diagnostics_Text(&pkg, &profiles, text, ARRAYSIZE(text)) && wcsstr(text, L"2.12345.0") && wcsstr(text, L"running=1"));
    Check("shared diagnostics exclude names, paths, notes and account identifiers",
          !wcsstr(text, L"private@example") && !wcsstr(text, L"Secret") && !wcsstr(text, L"secret note"));
    Check("diagnostics reject an undersized output buffer", !Diagnostics_Text(&pkg, &profiles, text, 4));
}

static void TestBackupFailures(void)
{
    static const WCHAR *const id = L"abababab-0000-4000-8000-00000000b001";
    ProfileList profiles;
    WCHAR entries[2][MAX_PATH], targetEntries[MAX_PATH], archive[MAX_PATH], settings[MAX_PATH], path[MAX_PATH], to[MAX_PATH];
    WCHAR error[LONG_PATH_CCH];
    ZipOut *out = NULL;
    ZipIn *zip = NULL, *appearance = NULL;
    DWORD code = 0;
    int sessions = 0;
    BOOL signedOut = FALSE, ready, denied;
    RestoreSource sources[2];
    RestoreResult result;
    int from[PICK_UNITS];
    ZeroMemory(&profiles, sizeof profiles);
    profiles.count = 2;
    ready = PrepareProfile(&profiles.items[0], L"Backup-A", L"backup\\A", L"backup-a", L"organization", entries[0], ARRAYSIZE(entries[0])) &&
            PrepareProfile(&profiles.items[1], L"Backup-B", L"backup\\B", L"backup-b", L"organization", entries[1], ARRAYSIZE(entries[1])) &&
            WriteSyncEntry(entries[0], id, "Backed up", 123) &&
            SaveIn(profiles.items[0].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"backup-a\",\"userThemeMode\":\"dark\"}") &&
            Join(profiles.items[1].storageDir, L"claude-code-sessions\\backup-a\\organization", targetEntries, ARRAYSIZE(targetEntries)) &&
            MakeDir(targetEntries) && FixturePath(L"account-backup.zip", archive, ARRAYSIZE(archive)) &&
            WriteBackup(&profiles, 0, BK_SIGNIN | BK_CONVERSATIONS, NULL, archive, error, ARRAYSIZE(error));
    Check("cross-account backup fixtures created", ready);
    if (ready) zip = Zip_Open(archive);
    ready = ready && FixturePath(L"appearance-backup.zip", settings, ARRAYSIZE(settings)) && (out = Zip_Create(settings, &code)) != NULL;
    if (ready) {
        static const char json[] = "{\"userThemeMode\":\"light\"}";
        BOOL added = Zip_AddData(out, APPEARANCE_NAME, json, sizeof json - 1);
        ready = Zip_Close(out, added) && added;
        if (ready) appearance = Zip_Open(settings);
    }
    ZeroMemory(sources, sizeof sources);
    sources[0].zip = zip;
    sources[0].offered = BK_SIGNIN | BK_CONVERSATIONS;
    sources[0].version = 2;
    sources[1].zip = appearance;
    sources[1].offered = SYNC_ITEM_APPEARANCE;
    sources[1].version = 2;
    from[PICK_UNIT_ACCOUNT] = 0;
    from[PICK_UNIT_SETTINGS] = 1;
    Check("account restore selects that account before importing its sessions",
          zip && appearance && RestoreChosen(NULL, &profiles, 1, sources, 2, BK_SIGNIN | BK_CONVERSATIONS | SYNC_ITEM_APPEARANCE,
                                             from, g_root, L"Fixture restore", &result) && result.sessions == 1 &&
          EntryThere(targetEntries, id) && !EntryThere(entries[1], id));
    Check("settings from the separately chosen backup survive the sign-in restore",
          appearance &&
          Join(profiles.items[1].storageDir, L"config.json", path, ARRAYSIZE(path)) && FileHas(path, "\"backup-a\"") &&
          FileHas(path, "\"light\""));
    Check("an account without an initialized folder cannot restore into the old account",
          zip && SaveIn(profiles.items[1].storageDir, L"config.json", "{\"lastKnownAccountUuid\":\"not-initialized\"}") &&
          !RestoreSessions(zip, &profiles, 1, g_root, &sessions, &signedOut, error, ARRAYSIZE(error)) && error[0] &&
          !EntryThere(entries[1], id));
    Zip_Free(appearance);
    Zip_Free(zip);

    /* Both backup paths must distinguish an unreadable folder from an empty one. */
    ready = FixturePath(L"denied-backup", path, ARRAYSIZE(path)) && MakeDir(path) && SaveIn(path, L"content.txt", "kept") &&
            FixturePath(L"denied-copy", to, ARRAYSIZE(to)) && FixturePath(L"denied-backup.zip", archive, ARRAYSIZE(archive));
    denied = ready && DenyDirectory(path, TRUE);
    Check("copying an unreadable tree fails", denied && !Util_CopyTree(path, to, FALSE, &code) && code == ERROR_ACCESS_DENIED);
    out = ready ? Zip_Create(archive, &code) : NULL;
    Check("archiving an unreadable tree fails", denied && out && !AddTree(out, path, L"data"));
    if (out) Zip_Close(out, FALSE);
    if (denied) Check("the private backup folder's access is restored", DenyDirectory(path, FALSE));
}

static void TestArchiveValidation(void)
{
    static const char manifest[] = "{\"format\":\"claude-desktop-profiles-manager-sessions\",\"version\":1,\"sessions\":1}";
    static const char entry[] = "{\"sessionId\":\"local_11111111-1111-4111-8111-111111111111\",\"cliSessionId\":\"11111111-1111-4111-8111-111111111111\",\"title\":\"Fixture\"}";
    static const char outer[] = "{\"format\":\"claude-desktop-profiles-manager-backup\",\"version\":2,\"items\":67108864}";
    WCHAR archive[MAX_PATH], wrapped[MAX_PATH], error[1024];
    ZipOut *zip;
    DWORD code = 0;
    int files = 0;
    BOOL ok;
    HANDLE cancel = CreateEventW(NULL, TRUE, TRUE, NULL), file;
    FixturePath(L"validation-session.zip", archive, ARRAYSIZE(archive));
    FixturePath(L"validation-profile.zip", wrapped, ARRAYSIZE(wrapped));
    zip = Zip_Create(archive, &code);
    ok = zip && Zip_AddData(zip, "manifest.json", manifest, sizeof manifest - 1) && Zip_AddData(zip, "entries/1.json", entry, sizeof entry - 1);
    if (zip) ok = Zip_Close(zip, ok) && ok;
    Check("archive validation reads all session items", ok && Zip_Validate(archive, NULL, &files, error, ARRAYSIZE(error)) && files == 2);
    Check("archive validation can be cancelled", cancel && !Zip_Validate(archive, cancel, &files, error, ARRAYSIZE(error)) && error[0]);
    if (cancel) CloseHandle(cancel);
    zip = Zip_Create(wrapped, &code);
    ok = zip && Zip_AddData(zip, "manifest.json", outer, sizeof outer - 1) && Zip_AddFile(zip, "sessions/archive.zip", archive);
    if (zip) ok = Zip_Close(zip, ok) && ok;
    Check("archive validation descends into the session archive", ok && Zip_Validate(wrapped, NULL, &files, error, ARRAYSIZE(error)) && files == 4);
    file = CreateFileW(archive, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        BYTE broken = 0;
        DWORD wrote = 0;
        WriteFile(file, &broken, 1, &wrote, NULL);
        CloseHandle(file);
    }
    zip = Zip_Create(wrapped, &code);
    ok = zip && Zip_AddData(zip, "manifest.json", outer, sizeof outer - 1) && Zip_AddFile(zip, "sessions/archive.zip", archive);
    if (zip) ok = Zip_Close(zip, ok) && ok;
    Check("valid outer CRC does not hide a damaged nested archive", ok && !Zip_Validate(wrapped, NULL, &files, error, ARRAYSIZE(error)));
    zip = Zip_Create(archive, &code);
    ok = zip && Zip_AddData(zip, "manifest.json", manifest, sizeof manifest - 1);
    if (zip) ok = Zip_Close(zip, ok) && ok;
    Check("a missing declared session fails validation", ok && !Zip_Validate(archive, NULL, &files, error, ARRAYSIZE(error)));
    zip = Zip_Create(archive, &code);
    ok = zip && Zip_AddData(zip, "manifest.json", manifest, sizeof manifest - 1) && Zip_AddData(zip, "manifest.json", manifest, sizeof manifest - 1);
    if (zip) ok = Zip_Close(zip, ok) && ok;
    Check("duplicate archive paths fail validation", ok && !Zip_Validate(archive, NULL, &files, error, ARRAYSIZE(error)));
    {
        static const char truncated[] = "{\"format\":\"claude-desktop-profiles-manager-sessions\",\"version\":1,\"sessions\":0,";
        zip = Zip_Create(archive, &code);
        ok = zip && Zip_AddData(zip, "manifest.json", truncated, sizeof truncated - 1);
        if (zip) ok = Zip_Close(zip, ok) && ok;
        Check("a truncated manifest with valid CRC fails validation", ok && !Zip_Validate(archive, NULL, &files, error, ARRAYSIZE(error)));
    }
}

#include "test_backupfeatures.inc"

int wmain(int argc, WCHAR **argv)
{
    ProfileList profiles;
    WCHAR local[MAX_PATH], projects[MAX_PATH], temporary[MAX_PATH];
    WCHAR *previousConfig = NULL, *previousTemporary = NULL;
    BOOL ready = FALSE, configChanged = FALSE, temporaryChanged = FALSE, kept;
    /* TestPendingProcesses starts copies of this exe as queue workers. */
    if (argc == 6 && wcscmp(argv[1], L"--queue-worker") == 0) return QueueWorker(argv[2], argv[3], argv[4], _wtoi(argv[5]));
    kept = KeepVariable(L"CLAUDE_CONFIG_DIR", &previousConfig);
    kept = KeepVariable(L"CLAUDE_CODE_TMPDIR", &previousTemporary) && kept;
    if (!kept) {
        if (previousConfig) HeapFree(GetProcessHeap(), 0, previousConfig);
        if (previousTemporary) HeapFree(GetProcessHeap(), 0, previousTemporary);
        printf("Session tests: could not preserve CLAUDE_CONFIG_DIR and CLAUDE_CODE_TMPDIR.\n");
        return 1;
    }
    RecycleWill(REMOVE_DONE, NULL);
    Check("fixture root created", CreateRoot());
    if (!g_failures) {
        WCHAR state[MAX_PATH];
        /* The log, the queued changes and Claude Code's temporary folder stay
         * in the fixture, not in the user's folders. */
        Check("private state folder created", FixturePath(L"state", state, ARRAYSIZE(state)) && MakeDir(state));
        Util_SetStateDir(state);
        temporaryChanged = FixturePath(L"tmp", temporary, ARRAYSIZE(temporary)) && MakeDir(temporary) &&
                           SetEnvironmentVariableW(L"CLAUDE_CODE_TMPDIR", temporary);
        Check("private Claude Code temporary folder set", temporaryChanged);
        ready = temporaryChanged && PrepareProfiles(&profiles, local, projects);
        configChanged = ready;
        Check("session fixtures ready", ready);
        if (ready) {
            TestResolution(local);
            TestSessions(&profiles, projects);
            TestCopyRemoval(&profiles, projects);
            TestSettings(&profiles.items[0], local);
            TestNewestAccount(projects);
            TestScratchCopyFailure(&profiles, projects);
            TestPendingProcesses();
            TestPendingApplication(&profiles);
            TestPendingQueueFile();
            TestEntryDetails();
            TestPendingChanges();
            TestTurnLocal();
            TestReadonlyPending(&profiles);
            TestRemovals();
            TestDeletionList(&profiles, projects);
            TestOtherTranscripts(projects);
            TestDeleteEverywhere(projects);
            TestEntryCounts();
            TestLongPaths(projects);
            TestCancelledRead();
            TestConcurrentReads(&profiles);
            TestBackgroundView(&profiles);
            TestIndexedSessions();
            TestGroupAliases();
            TestSessionSync(projects);
            TestUndo();
            TestSessionItems();
            TestSettingList();
            TestVault(projects);
            TestKeptGroups(projects);
            TestWebLayout();
            TestSyncItems();
            TestReliableSync();
            TestAccountKey();
            TestSearchCache();
            TestPrivateDiagnostics();
            TestBackupFailures();
            TestArchiveValidation();
            TestBackupFeatures(projects);
            TestCleanupReferences(projects);
            TestLedgerTransactions();
        }
        Check("nothing outside the fixture was given to the Recycle Bin", !g_recycle.escaped);
        StopStartedClaude();
        if (configChanged) Check("process environment restored", SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", previousConfig));
        if (temporaryChanged) Check("Claude Code's temporary folder restored", SetEnvironmentVariableW(L"CLAUDE_CODE_TMPDIR", previousTemporary));
        Check("fixture tree removed", RemoveFixture(g_root));
    }
    if (previousConfig) HeapFree(GetProcessHeap(), 0, previousConfig);
    if (previousTemporary) HeapFree(GetProcessHeap(), 0, previousTemporary);
    printf("Session tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
