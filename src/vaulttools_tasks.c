/* Measured Desktop 2.42304.2 task references: Code stores SKILL.md below
 * CLAUDE_CONFIG_DIR/scheduled-tasks; Cowork below its configured Scheduled
 * root (cowork-managed when migrated). Definition-aware restores keep tasks
 * disabled and never replay grants or approvals into another account. */
#include "vaulttools_tasks.h"
#include <shlobj.h>
#include <string.h>
#include <wchar.h>
#define TASK_MAX (8u * 1024u * 1024u)
#define REGISTRY_MAX (16u * 1024u * 1024u)
static void Free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static char *Copy(const char *text, size_t n)
{
    char *out = (char *)HeapAlloc(GetProcessHeap(), 0, n + 1);
    if (out) { memcpy(out, text, n); out[n] = 0; } return out;
}
static BOOL Text(const char *json, size_t n, const char *key, WCHAR *out, size_t cch)
{
    const char *value; size_t length;
    return Core_JsonMember(json, n, key, &value, &length) && Core_JsonString(value, length, out, cch);
}
static BOOL Set(char **json, size_t *length, const char *key, const char *value)
{
    const char *keys[1] = { key }; size_t n = 0;
    char *out = Core_JsonSetNested(*json, *length, keys, 1, value, &n);
    if (!out) return FALSE;
    Free(*json); *json = out; *length = n; return TRUE;
}
static BOOL SetText(char **json, size_t *length, const char *key, const WCHAR *text)
{
    char *quoted = (char *)HeapAlloc(GetProcessHeap(), 0, wcslen(text) * 6 + 3); BOOL ok;
    if (!quoted) return FALSE;
    ok = Core_JsonQuote(text, quoted, wcslen(text) * 6 + 3) && Set(json, length, key, quoted);
    Free(quoted); return ok;
}
static BOOL SafeId(const WCHAR *id)
{
    size_t i, n = wcslen(id);
    if (!n || n > 128 || id[0] == L'.') return FALSE;
    for (i = 0; i < n; i++) if (!((id[i] >= L'a' && id[i] <= L'z') || (id[i] >= L'A' && id[i] <= L'Z') ||
         (id[i] >= L'0' && id[i] <= L'9') || id[i] == L'-' || id[i] == L'_')) return FALSE;
    return TRUE;
}
/* No directory component may redirect a referenced prompt outside its path. */
static BOOL PlainPath(const WCHAR *path, BOOL missingOK)
{
    WCHAR part[LONG_PATH_CCH]; size_t i, n = wcslen(path);
    if (n < 3 || path[1] != L':' || path[2] != L'\\' || wcschr(path, L'/') || wcsstr(path, L"\\..") ||
        wcsstr(path + 2, L":") || FAILED(StringCchCopyW(part, ARRAYSIZE(part), path))) return FALSE;
    for (i = 3; i <= n; i++) if (i == n || part[i] == L'\\') {
        DWORD attributes; WCHAR save = part[i]; part[i] = 0; attributes = GetFileAttributesW(part); part[i] = save;
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (missingOK && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) continue;
            return FALSE;
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return FALSE;
        if (i < n && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return FALSE;
    }
    return TRUE;
}
static BOOL TaskPath(const Profile *profile, const WCHAR *id, const WCHAR *raw, BOOL code, WCHAR *out, size_t cch)
{
    WCHAR copy[LONG_PATH_CCH], *slash;
    if (!SafeId(id) || !Core_ProfileFilePath(profile, raw, out, cch) || !PlainPath(out, FALSE) ||
        FAILED(StringCchCopyW(copy, ARRAYSIZE(copy), out)) || !(slash = wcsrchr(copy, L'\\')) || !Core_EqualsI(slash + 1, L"SKILL.md")) return FALSE;
    *slash = 0;
    if (!(slash = wcsrchr(copy, L'\\')) || !Core_EqualsI(slash + 1, id)) return FALSE;
    *slash = 0;
    if (code) {
        WCHAR root[LONG_PATH_CCH];
        return SessionPurge_CodeFolder(root, ARRAYSIZE(root)) && SUCCEEDED(StringCchCatW(root, ARRAYSIZE(root), L"\\scheduled-tasks")) && Core_PathEquals(root, copy);
    }
    slash = wcsrchr(copy, L'\\');
    return slash && (Core_EqualsI(slash + 1, L"Scheduled") || _wcsnicmp(slash + 1, L"Scheduled (inactive", ARRAYSIZE(L"Scheduled (inactive") - 1) == 0);
}
static BOOL Prefix(const char *member, char *out, size_t cch)
{
    return SUCCEEDED(StringCchPrintfA(out, cch, "task-definitions/%016I64x", Core_HashBytes(CORE_HASH_START, member, strlen(member))));
}
typedef struct AddTasks { ZipOut *zip; const Profile *profile; const char *prefix; char *coverage; size_t used; BOOL ok, code; int saved, missing, remote; } AddTasks;
static BOOL AddTask(void *context, const char *json, size_t n)
{
    AddTasks *a = (AddTasks *)context;
    WCHAR id[129], raw[LONG_PATH_CCH], path[LONG_PATH_CCH]; char idA[129], name[256];
    const char *place, *existing; size_t placeLength, existingLength;
    const char *status = "missing";
    if (!Core_JsonIsValue(json, n) || !Text(json, n, "id", id, ARRAYSIZE(id)) || !SafeId(id) ||
        !WideCharToMultiByte(CP_UTF8, 0, id, -1, idA, sizeof idA, NULL, NULL) ||
        Core_JsonMember(a->coverage, a->used, idA, &existing, &existingLength)) return a->ok = FALSE;
    if (Core_JsonMember(json, n, "place", &place, &placeLength) && !(placeLength == 4 && memcmp(place, "null", 4) == 0)) { status = "remote"; a->remote++; }
    else if (Text(json, n, "filePath", raw, ARRAYSIZE(raw)) && TaskPath(a->profile, id, raw, a->code, path, ARRAYSIZE(path))) {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &data) && !data.nFileSizeHigh && data.nFileSizeLow <= TASK_MAX &&
            !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (FAILED(StringCchPrintfA(name, sizeof name, "%s/%s/SKILL.md", a->prefix, idA)) || !Zip_AddFile(a->zip, name, path)) return a->ok = FALSE;
            status = "saved"; a->saved++;
        } else a->missing++;
    } else a->missing++;
    {
        char rawStatus[32]; StringCchPrintfA(rawStatus, sizeof rawStatus, "\"%s\"", status);
        return a->ok = Set(&a->coverage, &a->used, idA, rawStatus);
    }
}
BOOL VaultTasks_AddRegistry(ZipOut *zip, const Profile *profile, const WCHAR *registryPath, const char *registryMember, BOOL code, WCHAR *error, size_t cch)
{
    DWORD n = 0; const char *tasks; size_t taskLength;
    char *json = Util_ReadFile(registryPath, REGISTRY_MAX, FALSE, &n), prefix[96], name[160], *coverage = NULL;
    size_t coverageLength = 2; AddTasks a; BOOL ok = FALSE;
    if (!json || !Core_JsonIsValue(json, n) || !Core_JsonMember(json, n, "scheduledTasks", &tasks, &taskLength) || !Prefix(registryMember, prefix, sizeof prefix)) goto done;
    ZeroMemory(&a, sizeof a); a.zip = zip; a.profile = profile; a.prefix = prefix; a.code = code; a.coverage = Copy("{}", 2); a.used = 2; a.ok = a.coverage != NULL;
    if (!a.ok || !Core_JsonEachElement(tasks, taskLength, AddTask, &a) || !a.ok) { Free(a.coverage); goto done; }
    coverage = Copy("{}", 2);
    if (!coverage || !Set(&coverage, &coverageLength, "definitions", a.coverage)) { Free(a.coverage); goto done; }
    Free(a.coverage);
    {
        char counts[32]; WCHAR member[1024];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, registryMember, -1, member, ARRAYSIZE(member)) ||
            !SetText(&coverage, &coverageLength, "registry", member)) goto done;
        StringCchPrintfA(counts, sizeof counts, "%d", a.saved); if (!Set(&coverage, &coverageLength, "saved", counts)) goto done;
        StringCchPrintfA(counts, sizeof counts, "%d", a.missing); if (!Set(&coverage, &coverageLength, "missing", counts)) goto done;
        StringCchPrintfA(counts, sizeof counts, "%d", a.remote); if (!Set(&coverage, &coverageLength, "remote", counts)) goto done;
    }
    if (!code && Core_PathUnder(registryPath, profile->storageDir)) {
        WCHAR root[LONG_PATH_CCH], grants[LONG_PATH_CCH], relative[LONG_PATH_CCH], *last;
        if (SUCCEEDED(StringCchPrintfW(root, ARRAYSIZE(root), L"%s\\local-agent-mode-sessions\\", profile->storageDir)) &&
            _wcsnicmp(registryPath, root, wcslen(root)) == 0 && SUCCEEDED(StringCchCopyW(relative, ARRAYSIZE(relative), registryPath + wcslen(root))) &&
            (last = wcsrchr(relative, L'\\')) != NULL) {
            *last = 0;
            if (SUCCEEDED(StringCchPrintfW(grants, ARRAYSIZE(grants), L"%s\\scheduled-task-grants\\%s\\artifact-publish-grants.json", profile->storageDir, relative)) &&
                PlainPath(grants, FALSE)) {
                StringCchPrintfA(name, sizeof name, "%s/artifact-publish-grants.json", prefix);
                if (!Zip_AddFile(zip, name, grants) || !Set(&coverage, &coverageLength, "grantsIncluded", "true")) goto done;
            }
        }
    }
    if (!Set(&coverage, &coverageLength, "grantsRestored", "false") || !Set(&coverage, &coverageLength, "version", "1")) goto done;
    StringCchPrintfA(name, sizeof name, "%s/coverage.json", prefix);
    ok = Zip_AddData(zip, name, coverage, coverageLength);
done:
    if (!ok && error && cch) StringCchCopyW(error, cch, TR(L"Scheduled task definitions could not be collected. The backup was not saved."));
    Free(json); Free(coverage); return ok;
}
static BOOL HasFiles(const WCHAR *root)
{
    WIN32_FIND_DATAW data; HANDLE find = Util_FindFiles(root, L"*", &data, FALSE); BOOL any = FALSE;
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do { if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L"..")) { any = TRUE; break; } } while (FindNextFileW(find, &data));
    FindClose(find); return any;
}
static BOOL TaskRoot(const Profile *p, BOOL code, WCHAR *out, size_t cch)
{
    WCHAR config[LONG_PATH_CCH], path[LONG_PATH_CCH], value[LONG_PATH_CCH];
    DWORD n = 0; char *json; BOOL found = FALSE;
    if (code) return SessionPurge_CodeFolder(out, cch) && SUCCEEDED(StringCchCatW(out, cch, L"\\scheduled-tasks")) && PlainPath(out, TRUE);
    if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\cowork-managed\\.migrated.json", p->storageDir)) && Util_FileExists(path))
        return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\cowork-managed\\Scheduled", p->storageDir)) && PlainPath(out, TRUE);
    if (FAILED(StringCchPrintfW(config, ARRAYSIZE(config), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir))) return FALSE;
    json = Util_ReadFile(config, REGISTRY_MAX, FALSE, &n);
    if (json && Text(json, n, "coworkUserFilesPath", value, ARRAYSIZE(value))) found = Core_ProfileFilePath(p, value, out, cch);
    Free(json);
    if (!found) {
        PWSTR home = NULL, documents = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Profile, KF_FLAG_DONT_VERIFY, NULL, &home))) {
            StringCchPrintfW(out, cch, L"%s\\Claude", home); found = TRUE;
            if (!HasFiles(out) && SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Documents, KF_FLAG_DONT_VERIFY, NULL, &documents)) &&
                SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\Claude", documents)) && HasFiles(path)) StringCchCopyW(out, cch, path);
        }
        CoTaskMemFree(home); CoTaskMemFree(documents);
    }
    return found && SUCCEEDED(StringCchCatW(out, cch, L"\\Scheduled")) && PlainPath(out, TRUE);
}
static BOOL WriteNew(const WCHAR *path, const void *bytes, DWORD n)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0; BOOL ok = file != INVALID_HANDLE_VALUE && (!n || (WriteFile(file, bytes, n, &done, NULL) && done == n)) && FlushFileBuffers(file);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (!ok && file != INVALID_HANDLE_VALUE) DeleteFileW(path);
    return ok;
}
typedef struct RestoreTasksState { const ZipIn *zip; const Profile *profile; const char *prefix; WCHAR root[LONG_PATH_CCH]; char *array; size_t used; BOOL ok; DWORD error; } RestoreTasksState;
static BOOL AppendTask(RestoreTasksState *r, const char *value, size_t length)
{
    char *next = (char *)HeapAlloc(GetProcessHeap(), 0, r->used + length + 3); size_t n = r->used;
    if (!next) return FALSE;
    memcpy(next, r->array, n); if (n > 1) next[n++] = ','; memcpy(next + n, value, length); n += length;
    next[n] = 0; Free(r->array); r->array = next; r->used = n; return TRUE;
}
static BOOL RestoreTask(void *context, const char *value, size_t length)
{
    static const char *const approvals[] = { "approvedPermissions", "permissionModeCarried", "chromePermissionMode", "chromeAllowedDomains",
        "userSelectedFolders", "userSelectedFiles", "userSelectedProjectUuids", "spaceId", "notifySessionId", "dispatchSubscribed", "observedToolUse", "watcher", "lastScheduledFor", "missedRunScanFloor" };
    RestoreTasksState *r = (RestoreTasksState *)context;
    WCHAR id[129], chosen[129], path[LONG_PATH_CCH], dir[LONG_PATH_CCH]; char idA[129], name[256];
    char *task = Copy(value, length); size_t used = length, i;
    DWORD bytes = 0; void *definition = NULL; int at;
    const char *place; size_t placeLength;
    if (SessionLink_Busy(r->profile)) { r->error = ERROR_BUSY; Free(task); return r->ok = FALSE; }
    if (!task || !Text(task, used, "id", id, ARRAYSIZE(id)) || !SafeId(id) || !Set(&task, &used, "enabled", "false")) goto failed;
    for (i = 0; i < ARRAYSIZE(approvals); i++) if (!Core_JsonRemoveMember(task, used, approvals[i], task, used + 1, &used)) goto failed;
    task[used] = 0;
    if (!SetText(&task, &used, "permissionMode", L"default")) goto failed;
    if (!(Core_JsonMember(task, used, "place", &place, &placeLength) && !(placeLength == 4 && memcmp(place, "null", 4) == 0)) && r->root[0]) {
        if (!WideCharToMultiByte(CP_UTF8, 0, id, -1, idA, sizeof idA, NULL, NULL) ||
            FAILED(StringCchPrintfA(name, sizeof name, "%s/%s/SKILL.md", r->prefix, idA))) goto failed;
        at = Zip_Find(r->zip, name);
        if (at >= 0) {
            int collision;
            definition = Zip_Read(r->zip, at, TASK_MAX, &bytes); if (!definition) goto failed;
            for (collision = 0; collision < 1024; collision++) {
                DWORD currentLength = 0; char *current;
                if (!collision) StringCchCopyW(chosen, ARRAYSIZE(chosen), id);
                else StringCchPrintfW(chosen, ARRAYSIZE(chosen), L"cdm-%016I64x-%d", Core_HashBytes(CORE_HASH_START, definition, bytes), collision);
                if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", r->root, chosen)) ||
                    FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\SKILL.md", dir)) || !PlainPath(path, TRUE)) goto failed;
                current = Util_ReadFile(path, TASK_MAX, FALSE, &currentLength);
                if (current) {
                    BOOL same = currentLength == bytes && memcmp(current, definition, bytes) == 0;
                    Free(current); if (same) break; else continue;
                }
                if (Util_QueryPath(path, NULL) != PATH_MISSING || !Util_EnsureDir(dir) || !PlainPath(dir, FALSE) || !WriteNew(path, definition, bytes)) goto failed;
                break;
            }
            if (collision == 1024 || !SetText(&task, &used, "id", chosen) || !SetText(&task, &used, "filePath", path) ||
                !Set(&task, &used, "ownFile", "true")) goto failed;
        }
    }
    r->ok = AppendTask(r, task, used); Free(task); Free(definition); return r->ok;
failed:
    r->error = GetLastError(); if (!r->error) r->error = ERROR_INVALID_DATA;
    Free(task); Free(definition); return r->ok = FALSE;
}
char *VaultTasks_RestoreRegistry(const ZipIn *zip, const Profile *profile, const char *registryMember, const char *json, size_t length,
                                BOOL code, BOOL *aware, size_t *outLength, DWORD *error)
{
    char prefix[96], manifest[160], *out = NULL, *array;
    const char *tasks; size_t taskLength; RestoreTasksState r;
    *aware = FALSE; *outLength = 0;
    if (!Prefix(registryMember, prefix, sizeof prefix)) return NULL;
    StringCchPrintfA(manifest, sizeof manifest, "%s/coverage.json", prefix);
    if (Zip_Find(zip, manifest) < 0) return NULL;
    *aware = TRUE;
    if (!VaultTasks_ValidateCoverage(zip, manifest)) { *error = ERROR_INVALID_DATA; return NULL; }
    ZeroMemory(&r, sizeof r); r.zip = zip; r.profile = profile; r.prefix = prefix; r.array = Copy("[", 1); r.used = 1; r.ok = r.array != NULL;
    if (!TaskRoot(profile, code, r.root, ARRAYSIZE(r.root))) { *error = ERROR_BAD_PATHNAME; Free(r.array); return NULL; }
    if (!r.ok || !Core_JsonMember(json, length, "scheduledTasks", &tasks, &taskLength) ||
        !Core_JsonEachElement(tasks, taskLength, RestoreTask, &r) || !r.ok) goto done;
    array = (char *)HeapReAlloc(GetProcessHeap(), 0, r.array, r.used + 2);
    if (!array) goto done;
    r.array = array; r.array[r.used++] = ']'; r.array[r.used] = 0;
    out = Copy(json, length); *outLength = length;
    if (!out || !Set(&out, outLength, "scheduledTasks", r.array)) { Free(out); out = NULL; }
done:
    if (!out) *error = r.error ? r.error : ERROR_INVALID_DATA;
    Free(r.array); return out;
}
BOOL VaultTasks_IsCoverage(const char *name)
{
    size_t n = strlen(name); const char suffix[] = "/coverage.json";
    return strncmp(name, "task-definitions/", 17) == 0 && n >= sizeof suffix - 1 && strcmp(name + n - (sizeof suffix - 1), suffix) == 0;
}
typedef struct ValidateTasks { const ZipIn *zip; const char *prefix, *definitions; size_t definitionsLength; int count, saved, missing, remote; BOOL ok; } ValidateTasks;
static BOOL ValidateTask(void *context, const char *value, size_t length)
{
    ValidateTasks *v = (ValidateTasks *)context;
    WCHAR id[129], path[LONG_PATH_CCH], status[32]; char idA[129], member[256];
    const char *raw; size_t n; int at;
    if (!Text(value, length, "id", id, ARRAYSIZE(id)) || !SafeId(id) || !Text(value, length, "filePath", path, ARRAYSIZE(path)) ||
        !WideCharToMultiByte(CP_UTF8, 0, id, -1, idA, sizeof idA, NULL, NULL) ||
        !Core_JsonMember(v->definitions, v->definitionsLength, idA, &raw, &n) || !Core_JsonString(raw, n, status, ARRAYSIZE(status)) ||
        FAILED(StringCchPrintfA(member, sizeof member, "%s/%s/SKILL.md", v->prefix, idA))) return v->ok = FALSE;
    at = Zip_Find(v->zip, member); v->count++;
    if (wcscmp(status, L"saved") == 0) { v->saved++; return v->ok = at >= 0 && Zip_Size(v->zip, at) <= TASK_MAX; }
    if (at >= 0) return v->ok = FALSE;
    if (wcscmp(status, L"missing") == 0) { v->missing++; return TRUE; }
    if (wcscmp(status, L"remote") == 0) { v->remote++; return TRUE; }
    return v->ok = FALSE;
}
static BOOL CountDefinition(void *context, const char *key, size_t keyLength, const char *value, size_t length)
{
    int *count = (int *)context; (void)key; (void)keyLength; (void)value; (void)length; (*count)++; return TRUE;
}
BOOL VaultTasks_ValidateCoverage(const ZipIn *zip, const char *name)
{
    DWORD n = 0, registryLength = 0; char *coverage = NULL, *registry = NULL, prefix[96], expected[160], member[1024];
    WCHAR memberW[1024]; const char *raw, *tasks; size_t k, tasksLength; ULONGLONG version = 0, counts[3] = { 0, 0, 0 };
    static const char *const keys[] = { "saved", "missing", "remote" };
    ValidateTasks v; BOOL ok = FALSE; int at = Zip_Find(zip, name), definitions = 0; size_t i;
    ZeroMemory(&v, sizeof v); v.zip = zip; v.prefix = prefix; v.ok = TRUE;
    if (at < 0 || !(coverage = (char *)Zip_Read(zip, at, REGISTRY_MAX, &n)) || !Core_JsonIsValue(coverage, n) ||
        !Core_JsonMember(coverage, n, "version", &raw, &k) || !Core_JsonNumber(raw, k, &version) || version != 1 ||
        !Text(coverage, n, "registry", memberW, ARRAYSIZE(memberW)) ||
        !WideCharToMultiByte(CP_UTF8, 0, memberW, -1, member, sizeof member, NULL, NULL) || !Core_ArchiveNameSafe(member, strlen(member)) ||
        !Prefix(member, prefix, sizeof prefix) || FAILED(StringCchPrintfA(expected, sizeof expected, "%s/coverage.json", prefix)) || strcmp(expected, name) != 0 ||
        (strcmp(member, "sessions/scheduled-tasks.json") != 0 && !(strncmp(member, "cowork/", 7) == 0 && Core_EndsWithI(memberW, L"/scheduled-tasks.json"))) ||
        !Core_JsonMember(coverage, n, "definitions", &v.definitions, &v.definitionsLength)) goto done;
    for (i = 0; i < ARRAYSIZE(keys); i++) if (!Core_JsonMember(coverage, n, keys[i], &raw, &k) || !Core_JsonNumber(raw, k, &counts[i])) goto done;
    at = Zip_Find(zip, member);
    if (at < 0 || !(registry = (char *)Zip_Read(zip, at, REGISTRY_MAX, &registryLength)) || !Core_JsonIsValue(registry, registryLength) ||
        !Core_JsonMember(registry, registryLength, "scheduledTasks", &tasks, &tasksLength) ||
        !Core_JsonEachElement(tasks, tasksLength, ValidateTask, &v) || !v.ok ||
        !Core_JsonEachMember(v.definitions, v.definitionsLength, CountDefinition, &definitions)) goto done;
    ok = definitions == v.count && (ULONGLONG)v.saved == counts[0] && (ULONGLONG)v.missing == counts[1] && (ULONGLONG)v.remote == counts[2];
done:
    Free(coverage); Free(registry); return ok;
}

typedef struct DescribeState { WCHAR *out; size_t cch; } DescribeState;
static BOOL DescribeMissing(void *context, const char *key, size_t keyLength, const char *value, size_t length)
{
    DescribeState *d = (DescribeState *)context; WCHAR id[129], status[32], line[256];
    int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, key, (int)keyLength, id, ARRAYSIZE(id) - 1);
    if (wide <= 0 || !Core_JsonString(value, length, status, ARRAYSIZE(status))) return FALSE;
    id[wide] = 0;
    if (wcscmp(status, L"saved") == 0) return TRUE;
    StringCchPrintfW(line, ARRAYSIZE(line), L"  %s: %s\r\n", id, wcscmp(status, L"remote") == 0 ? TR(L"Remote definition was not collected") : TR(L"Task definition is missing or unsafe to read"));
    if (wcslen(d->out) + wcslen(line) + 1 < d->cch) StringCchCatW(d->out, d->cch, line);
    return TRUE;
}

BOOL VaultTasks_Describe(const ZipIn *zip, const char *name, WCHAR *out, size_t cch)
{
    DWORD n = 0; char *json; const char *v; size_t k; ULONGLONG saved = 0, missing = 0, remote = 0;
    WCHAR registry[1024]; BOOL ok;
    int at = Zip_Find(zip, name);
    if (at < 0 || !(json = (char *)Zip_Read(zip, at, REGISTRY_MAX, &n))) return FALSE;
    ok = Text(json, n, "registry", registry, ARRAYSIZE(registry));
    if (Core_JsonMember(json, n, "saved", &v, &k)) Core_JsonNumber(v, k, &saved);
    if (Core_JsonMember(json, n, "missing", &v, &k)) Core_JsonNumber(v, k, &missing);
    if (Core_JsonMember(json, n, "remote", &v, &k)) Core_JsonNumber(v, k, &remote);
    if (ok) ok = SUCCEEDED(StringCchPrintfW(out, cch, TR(L"%s: task definitions saved=%I64u, missing=%I64u, remote=%I64u. Restored tasks stay disabled and require permission review in Claude; saved grants are not applied.\r\n"), registry, saved, missing, remote));
    if (ok && Core_JsonMember(json, n, "definitions", &v, &k)) {
        DescribeState d; d.out = out; d.cch = cch;
        ok = Core_JsonEachMember(v, k, DescribeMissing, &d);
    }
    Free(json); return ok;
}
