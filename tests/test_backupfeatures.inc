/* Included by test_sessions.c: all paths and state belong to its fixture. */
#include "../src/vaulttools.h"
#include "../src/vaulttools_tasks.h"
#include "../src/presets.h"

static BOOL FeatureCorrupt(const WCHAR *archive, const char *marker)
{
    DWORD n = 0, wrote = 0; size_t i, k = strlen(marker); LARGE_INTEGER at;
    char *bytes = Util_ReadFile(archive, 1024u * 1024u, FALSE, &n);
    HANDLE file; BOOL ok = FALSE;
    if (!bytes) return FALSE;
    for (i = 0; i + k <= n; i++) if (memcmp(bytes + i, marker, k) == 0) break;
    if (i + k <= n) {
        file = CreateFileW(archive, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        at.QuadPart = (LONGLONG)i;
        if (file != INVALID_HANDLE_VALUE) {
            BYTE changed = (BYTE)(bytes[i] ^ 1);
            ok = SetFilePointerEx(file, at, NULL, FILE_BEGIN) && WriteFile(file, &changed, 1, &wrote, NULL) && wrote == 1;
            CloseHandle(file);
        }
    }
    HeapFree(GetProcessHeap(), 0, bytes); return ok;
}
typedef struct FeatureElement { const char *value; size_t length; } FeatureElement;
static BOOL FeatureFirst(void *context, const char *value, size_t length)
{
    FeatureElement *first = (FeatureElement *)context;
    if (!first->value) { first->value = value; first->length = length; } return TRUE;
}
static BOOL FeatureTitled(const Profile *profile, const WCHAR *key, const WCHAR *title)
{
    SessionSet set; int at; BOOL ok;
    if (!SessionStore_LoadEntries(&set, profile)) return FALSE;
    at = SessionStore_FindEntry(&set, 0, key, NULL);
    ok = at >= 0 && TitledAs(set.entries[at].file, title);
    SessionStore_Free(&set); return ok;
}
static void TestBackupFeatures(const WCHAR *projects)
{
    static const WCHAR *const a = L"abcdef01-0000-4000-8000-000000000001";
    static const WCHAR *const b = L"abcdef01-0000-4000-8000-000000000002";
    static const WCHAR *const c = L"abcdef01-0000-4000-8000-000000000003";
    ProfileList profiles; WCHAR entries[2][MAX_PATH], path[LONG_PATH_CCH], archive[MAX_PATH], error[2048];
    SyncReport report; VaultVersion version[4]; VaultRestoreItem *choices = NULL; int count;
    char *out, *again; size_t used = 0, againLength = 0; BOOL ready;
    const WCHAR *selected[1] = { a };
    static const char index[] = "{\"v\":1,\"future\":9,\"archived\":[\"local_abcdef01-0000-4000-8000-000000000002\",\"local_abcdef01-0000-4000-8000-000000000003\"]}";
    out = VaultTools_ArchiveSet(index, sizeof index - 1, L"local_abcdef01-0000-4000-8000-000000000001", TRUE, &used);
    Check("selected archive bits retain unrelated entries and unknown fields", out && strstr(out, "\"future\":9") && strstr(out, "000000000001") && strstr(out, "000000000002") && strstr(out, "000000000003"));
    again = out ? VaultTools_ArchiveSet(out, used, L"local_abcdef01-0000-4000-8000-000000000001", FALSE, &againLength) : NULL;
    Check("unarchiving one selection preserves the others", again && !strstr(again, "000000000001") && strstr(again, "000000000002") && strstr(again, "000000000003"));
    if (out) HeapFree(GetProcessHeap(), 0, out); if (again) HeapFree(GetProcessHeap(), 0, again);
    out = VaultTools_ArchiveSet("{\"archived\":{}}", 15, a, TRUE, &used);
    Check("a malformed archived index is never replaced silently", !out);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    ZeroMemory(&profiles, sizeof profiles); profiles.count = 2;
    ready = PrepareProfile(&profiles.items[0], L"Features source", L"features\\source", L"feature-account-a", L"organization", entries[0], ARRAYSIZE(entries[0])) &&
            PrepareProfile(&profiles.items[1], L"Features target", L"features\\target", L"feature-account-b", L"organization", entries[1], ARRAYSIZE(entries[1])) &&
            WriteSyncEntryAs(entries[0], a, "Historical A", 100, TRUE) && WriteSyncEntryAs(entries[0], b, "Historical B", 100, FALSE) &&
            SaveIn(entries[0], L"archived-sessions.idx", "{\"v\":1,\"archived\":[\"local_abcdef01-0000-4000-8000-000000000001\"]}") &&
            SaveIn(profiles.items[0].storageDir, L"claude_desktop_config.json", "{\"preferences\":{\"ccAutoArchiveOnPrClose\":true}}") &&
            WriteSyncEntry(entries[1], a, "Current A", 300) && WriteSyncEntry(entries[1], b, "Current B", 300) &&
            WriteSyncEntry(entries[1], c, "Outside version", 400) && SaveIn(entries[1], L"archived-sessions.idx", index) &&
            SaveIn(profiles.items[1].storageDir, L"claude_desktop_config.json", "{\"preferences\":{\"ccAutoArchiveOnPrClose\":false},\"keep\":true}");
    ready = ready && RemoveEntry(entries[1], a) && SaveIn(entries[1], L"local_mapped-a.json", "{\"sessionId\":\"local_mapped-a\",\"cliSessionId\":\"abcdef01-0000-4000-8000-000000000001\",\"cwd\":\"C:\\\\Fixture\",\"title\":\"Current A\",\"isArchived\":false,\"lastActivityAt\":300}");
    Check("selected recovery fixtures created", ready);
    ZeroMemory(&report, sizeof report);
    Check("historical list kept for selected recovery", ready && SessionVault_Keep(&profiles, 1u, L"features-select", FALSE, &report));
    count = SessionVault_Versions(L"features-select", version, ARRAYSIZE(version));
    Check("historical entries can be listed without changing target", count > 0 && SessionVault_RestoreItems(&profiles, 1, L"features-select", version[0].name, &choices) == 2 && FeatureTitled(&profiles.items[1], a, L"Current A"));
    if (choices) HeapFree(GetProcessHeap(), 0, choices);
    ZeroMemory(&report, sizeof report);
    Check("selected recovery writes only the chosen entry", count > 0 && SessionVault_RestoreSelected(&profiles, 1, L"features-select", version[0].name, selected, 1, &report) && FeatureTitled(&profiles.items[1], a, L"Historical A") && EntryTitled(entries[1], b, L"Current B") && EntryTitled(entries[1], c, L"Outside version"));
    Check("selected recovery merges archived bits rather than replacing index", Join(entries[1], L"archived-sessions.idx", path, ARRAYSIZE(path)) && FileHas(path, "local_mapped-a") && !FileHas(path, "000000000001") && FileHas(path, "000000000002") && FileHas(path, "000000000003") && FileHas(path, "\"future\":9"));
    Check("selected recovery never applies the old layout", Join(profiles.items[1].storageDir, L"claude_desktop_config.json", path, ARRAYSIZE(path)) && FileHas(path, "\"ccAutoArchiveOnPrClose\":false") && FileHas(path, "\"keep\":true"));
    {
        const WCHAR *onlyB[1] = { b };
        WCHAR mark[MAX_PATH], indexPath[MAX_PATH]; DWORD beforeLength = 0, afterLength = 0;
        char *before, *after; int i, listedCount; BOOL deleted = FALSE;
        VaultIds listed, ledger;
        SessionVault_Ids(&listed, &ledger);
        Check("raw deletion fixture has no global ledger mark", !SessionVault_HasId(&ledger, b));
        SessionVault_FreeIds(&listed); SessionVault_FreeIds(&ledger);
        ready = RemoveEntry(entries[1], b) && MarkPath(entries[1], b, mark, ARRAYSIZE(mark)) && Save(mark, "888");
        Join(entries[1], L"archived-sessions.idx", indexPath, ARRAYSIZE(indexPath));
        before = Util_ReadFile(indexPath, 1024u * 1024u, FALSE, &beforeLength);
        choices = NULL; listedCount = SessionVault_RestoreItems(&profiles, 1, L"features-select", version[0].name, &choices);
        for (i = 0; i < listedCount; i++) if (Core_EqualsI(choices[i].key, b) && choices[i].status == 3) deleted = TRUE;
        Check("selection preview respects a raw Claude deletion before the ledger sees it", ready && deleted);
        if (choices) HeapFree(GetProcessHeap(), 0, choices); choices = NULL;
        ZeroMemory(&report, sizeof report);
        Check("selection recovery does not resurrect a raw Claude deletion", ready && SessionVault_RestoreSelected(&profiles, 1, L"features-select", version[0].name, onlyB, 1, &report) && !EntryThere(entries[1], b) && FileThere(mark));
        after = Util_ReadFile(indexPath, 1024u * 1024u, FALSE, &afterLength);
        Check("skipped raw deletions never change archive bits", before && after && beforeLength == afterLength && memcmp(before, after, beforeLength) == 0);
        if (after) HeapFree(GetProcessHeap(), 0, after); after = NULL;
        {
            static const WCHAR *const own = L"abcdef01-0000-4000-8000-000000000088";
            static const char historical[] = "{\"sessionId\":\"local_abcdef01-0000-4000-8000-000000000088\",\"cliSessionId\":\"abcdef01-0000-4000-8000-000000000002\",\"title\":\"Historical own-id entry\",\"isArchived\":false,\"lastActivityAt\":100}";
            static const char current[] = "{\"sessionId\":\"local_abcdef01-0000-4000-8000-000000000088\",\"cliSessionId\":\"abcdef01-0000-4000-8000-000000000002\",\"title\":\"Current own-id entry\",\"isArchived\":true,\"lastActivityAt\":300}";
            VaultVersion ownVersion[2]; WCHAR ownEntry[MAX_PATH]; int ownCount;
            ready = RemoveFixture(mark) && RemoveEntry(entries[0], b) && EntryPath(entries[0], own, ownEntry, ARRAYSIZE(ownEntry)) && Save(ownEntry, historical) &&
                    SessionVault_Keep(&profiles, 1u, L"features-own-mark", FALSE, &report);
            ownCount = SessionVault_Versions(L"features-own-mark", ownVersion, ARRAYSIZE(ownVersion));
            ready = ready && ownCount > 0 && MarkPath(entries[1], own, mark, ARRAYSIZE(mark)) && Save(mark, "889");
            choices = NULL; deleted = FALSE;
            listedCount = ownCount > 0 ? SessionVault_RestoreItems(&profiles, 1, L"features-own-mark", ownVersion[0].name, &choices) : -1;
            for (i = 0; i < listedCount; i++) if (Core_EqualsI(choices[i].key, b) && choices[i].status == 3) deleted = TRUE;
            Check("selection preview recognizes the historical entry own-id marker", ready && deleted);
            if (choices) HeapFree(GetProcessHeap(), 0, choices); choices = NULL;
            Check("historical own-id markers cannot be cleared by selection recovery", ready && SessionVault_RestoreSelected(&profiles, 1, L"features-own-mark", ownVersion[0].name, onlyB, 1, &report) && !EntryThere(entries[1], b) && !EntryThere(entries[1], own) && FileThere(mark));
            ready = ready && EntryPath(entries[1], own, ownEntry, ARRAYSIZE(ownEntry)) && Save(ownEntry, current);
            choices = NULL; deleted = FALSE;
            listedCount = SessionVault_RestoreItems(&profiles, 1, L"features-select", version[0].name, &choices);
            for (i = 0; i < listedCount; i++) if (Core_EqualsI(choices[i].key, b) && choices[i].status == 3) deleted = TRUE;
            Check("selection preview recognizes a different target own-id marker", ready && deleted);
            if (choices) HeapFree(GetProcessHeap(), 0, choices); choices = NULL;
            Check("a stale target entry with its own deletion marker stays untouched", ready && SessionVault_RestoreSelected(&profiles, 1, L"features-select", version[0].name, onlyB, 1, &report) && FeatureTitled(&profiles.items[1], b, L"Current own-id entry") && FileThere(mark));
            after = Util_ReadFile(indexPath, 1024u * 1024u, FALSE, &afterLength);
            Check("both own-id deletion paths preserve the complete archive index", before && after && beforeLength == afterLength && memcmp(before, after, beforeLength) == 0);
            if (after) HeapFree(GetProcessHeap(), 0, after); after = NULL;
            Check("skipping a deleted selection does not create an absent archive index", RemoveFixture(indexPath) && SessionVault_RestoreSelected(&profiles, 1, L"features-select", version[0].name, onlyB, 1, &report) && !FileThere(indexPath));
            Check("raw marker fixtures restore their private archive index", before && Save(indexPath, before) && RemoveFixture(ownEntry) && RemoveFixture(mark));
        }
        if (before) HeapFree(GetProcessHeap(), 0, before);
    }
    Check("selected recovery respects deletion ledger", Join(entries[1], L"local_mapped-a.json", path, ARRAYSIZE(path)) && RemoveFixture(path) && SessionVault_AddDeleted(selected, 1) && SessionVault_RestoreSelected(&profiles, 1, L"features-select", version[0].name, selected, 1, &report) && !EntryThere(entries[1], a) && !FileThere(path));
    {
        HWND fake = StartFakeClaude(&profiles.items[1]);
        Check("selected recovery rejects a running target", fake && !SessionVault_RestoreSelected(&profiles, 1, L"features-select", version[0].name, selected, 1, &report));
        StopFakeClaude(fake);
    }
    {
        static const char presets[] = "{\"byAccount\":{\"feature-account-a\":{\"presets\":[{\"id\":\"one\",\"name\":\"Preset\",\"target\":{\"kind\":\"local\",\"cwd\":\"C:\\\\Work\"}}],\"builtIns\":{\"local\":{\"settings\":{\"effort\":\"high\"}}}}}}";
        char *layout, *filtered; size_t n = 0, m = 0;
        ready = SaveIn(profiles.items[0].storageDir, SESSION_PRESETS_FILE, presets) && SaveIn(profiles.items[1].storageDir, SESSION_PRESETS_FILE, "{\"futureRoot\":3,\"byAccount\":{\"untouched\":{\"future\":7}}}");
        layout = ready ? SessionSync_ReadLayout(&profiles.items[0], entries[0], &n, NULL) : NULL;
        filtered = layout ? SessionSync_FilterLayout(layout, n, SYNC_ITEM_DETAILS_MODEL, &m) : NULL;
        Check("presets are included by the model and effort scope", filtered && strstr(filtered, "\"sessionPresets\"") && !strstr(filtered, "feature-account-a"));
        ZeroMemory(&report, sizeof report);
        Check("preset restore maps the destination account and preserves others", filtered && SessionSync_RestoreLayout(&profiles.items[1], entries[1], filtered, m, &report) && Join(profiles.items[1].storageDir, SESSION_PRESETS_FILE, path, ARRAYSIZE(path)) && FileHas(path, "\"feature-account-b\"") && !FileHas(path, "feature-account-a") && FileHas(path, "\"untouched\"") && FileHas(path, "\"futureRoot\":3"));
        if (layout) HeapFree(GetProcessHeap(), 0, layout); if (filtered) HeapFree(GetProcessHeap(), 0, filtered);
    }
    {
        static const char manifest[] = "{\"format\":\"claude-desktop-profiles-manager-sessions\",\"version\":1,\"sessions\":1}";
        static const char entry[] = "{\"sessionId\":\"local_abcdef01-0000-4000-8000-000000000099\",\"cliSessionId\":\"abcdef01-0000-4000-8000-000000000099\",\"title\":\"Never imported\"}";
        static const char marker[] = "unsafe-tail-CRC-78xJqP09";
        ZipOut *zip; DWORD code = 0; int imported = 0; SessionSet set;
        FixturePath(L"feature-corrupt-late.zip", archive, ARRAYSIZE(archive)); zip = Zip_Create(archive, &code);
        ready = zip && Zip_AddData(zip, "manifest.json", manifest, sizeof manifest - 1) && Zip_AddData(zip, "entries/new.json", entry, sizeof entry - 1) &&
                Zip_AddData(zip, "files/projects/late/abcdef01-0000-4000-8000-000000000099.jsonl", marker, sizeof marker - 1);
        if (zip) ready = Zip_Close(zip, ready) && ready;
        ready = ready && FeatureCorrupt(archive, marker) && SessionStore_LoadProfiles(&set, &profiles);
        ZeroMemory(&report, sizeof report);
        Check("corrupt late payload stops import before target entries or transcripts", ready && !SessionSync_Import(&set, archive, 2u, &imported, &report) && !EntryThere(entries[1], L"abcdef01-0000-4000-8000-000000000099") && Join(projects, L"late\\abcdef01-0000-4000-8000-000000000099.jsonl", path, ARRAYSIZE(path)) && !FileThere(path));
        if (ready) SessionStore_Free(&set);
    }
    {
        WCHAR prompt[LONG_PATH_CCH], taskRegistry[LONG_PATH_CCH], taskRoot[LONG_PATH_CCH], targetPath[LONG_PATH_CCH];
        char json[8192], quoted[LONG_PATH_CCH * 3], coverageName[160];
        ZipOut *outZip; ZipIn *zip; DWORD code = 0; BOOL aware = FALSE; int files = 0;
        char *mapped; size_t n = 0; const char *tasks, *v; size_t tasksLength, valueLength; FeatureElement first; ZeroMemory(&first, sizeof first);
        ready = SessionPurge_CodeFolder(taskRoot, ARRAYSIZE(taskRoot)) && Join(taskRoot, L"scheduled-tasks\\feature-task\\SKILL.md", prompt, ARRAYSIZE(prompt)) && Save(prompt, "# Historical prompt\n") &&
                Join(entries[0], L"scheduled-tasks.json", taskRegistry, ARRAYSIZE(taskRegistry)) && Core_JsonQuote(prompt, quoted, sizeof quoted);
        if (ready) StringCchPrintfA(json, sizeof json, "{\"future\":9,\"scheduledTasks\":[{\"id\":\"feature-task\",\"filePath\":%s,\"enabled\":true,\"createdAt\":1,\"future\":7,\"permissionMode\":\"bypassPermissions\",\"approvedPermissions\":[\"secret\"],\"notifySessionId\":\"old-account-session\"},{\"id\":\"missing-task\",\"enabled\":true,\"filePath\":\"C:\\\\not-here\\\\SKILL.md\"}]}", quoted);
        ready = ready && Save(taskRegistry, json) && FixturePath(L"feature-tasks.zip", archive, ARRAYSIZE(archive));
        outZip = ready ? Zip_Create(archive, &code) : NULL;
        ready = outZip && Zip_AddData(outZip, "manifest.json", "{\"format\":\"claude-desktop-profiles-manager-backup\",\"version\":2,\"items\":67108864}", sizeof "{\"format\":\"claude-desktop-profiles-manager-backup\",\"version\":2,\"items\":67108864}" - 1) && Zip_AddFile(outZip, "sessions/scheduled-tasks.json", taskRegistry) && VaultTasks_AddRegistry(outZip, &profiles.items[0], taskRegistry, "sessions/scheduled-tasks.json", TRUE, error, ARRAYSIZE(error));
        if (outZip) ready = Zip_Close(outZip, ready) && ready;
        zip = ready ? Zip_Open(archive) : NULL;
        StringCchPrintfA(coverageName, sizeof coverageName, "task-definitions/%016I64x/coverage.json", Core_HashBytes(CORE_HASH_START, "sessions/scheduled-tasks.json", strlen("sessions/scheduled-tasks.json")));
        Check("task definitions and missing coverage are included", zip && VaultTasks_Describe(zip, coverageName, error, ARRAYSIZE(error)) && wcsstr(error, L"missing-task") && wcsstr(error, L"missing=1"));
        Check("task bundle passes full outer verification", zip && Zip_Validate(archive, NULL, &files, error, ARRAYSIZE(error)));
        Check("target task collision fixture created", Save(prompt, "# Destination prompt\n"));
        mapped = zip ? VaultTasks_RestoreRegistry(zip, &profiles.items[1], "sessions/scheduled-tasks.json", json, strlen(json), TRUE, &aware, &n, &code) : NULL;
        Check("definition-aware restore disables tasks and clears approvals", aware && mapped && !strstr(mapped, "approvedPermissions") && !strstr(mapped, "notifySessionId") && !strstr(mapped, "bypassPermissions") && strstr(mapped, "\"enabled\":false") && strstr(mapped, "\"future\":7") && strstr(mapped, "\"future\":9"));
        ready = mapped && Core_JsonMember(mapped, n, "scheduledTasks", &tasks, &tasksLength) && Core_JsonEachElement(tasks, tasksLength, FeatureFirst, &first);
        Check("task file collisions preserve the destination and map a distinct prompt", ready && first.value && Core_JsonMember(first.value, first.length, "filePath", &v, &valueLength) && Core_JsonString(v, valueLength, targetPath, ARRAYSIZE(targetPath)) && !Core_PathEquals(targetPath, prompt) && FileHas(targetPath, "Historical prompt") && FileHas(prompt, "Destination prompt"));
        if (mapped) HeapFree(GetProcessHeap(), 0, mapped);
        Zip_Free(zip);
    }
    {
        WCHAR prompt[LONG_PATH_CCH], registryPath[LONG_PATH_CCH], grants[LONG_PATH_CCH], sourceRoot[MAX_PATH], targetRoot[MAX_PATH], mappedPath[LONG_PATH_CCH], broken[MAX_PATH];
        char json[8192], quoted[LONG_PATH_CCH * 3], config[MAX_PATH * 6 + 64], targetQuoted[MAX_PATH * 6 + 4], prefix[96], coverage[160], grantMember[180];
        static const char member[] = "cowork/account/org/scheduled-tasks.json";
        static const char manifest[] = "{\"format\":\"claude-desktop-profiles-manager-backup\",\"version\":2,\"items\":134217728}";
        ZipOut *output; ZipIn *zip = NULL; DWORD code = 0; BOOL aware = FALSE; int files = 0, z; char *mapped; size_t n = 0;
        const char *tasks, *v; size_t tasksLength, valueLength; FeatureElement first; ZeroMemory(&first, sizeof first);
        ready = FixturePath(L"cowork-feature-source", sourceRoot, ARRAYSIZE(sourceRoot)) && FixturePath(L"cowork-feature-target", targetRoot, ARRAYSIZE(targetRoot)) &&
                Join(sourceRoot, L"Scheduled\\cowork-task\\SKILL.md", prompt, ARRAYSIZE(prompt)) && Save(prompt, "Cowork instructions\n") &&
                Join(profiles.items[0].storageDir, L"local-agent-mode-sessions\\account\\org\\scheduled-tasks.json", registryPath, ARRAYSIZE(registryPath)) &&
                Join(profiles.items[0].storageDir, L"scheduled-task-grants\\account\\org\\artifact-publish-grants.json", grants, ARRAYSIZE(grants)) && Save(grants, "{\"private-grant\":true}") &&
                Core_JsonQuote(prompt, quoted, sizeof quoted) && Core_JsonQuote(targetRoot, targetQuoted, sizeof targetQuoted);
        if (ready) {
            StringCchPrintfA(json, sizeof json, "{\"scheduledTasks\":[{\"id\":\"cowork-task\",\"filePath\":%s,\"createdAt\":1,\"enabled\":true,\"userSelectedFolders\":[\"old-account-permission\"],\"approvedPermissions\":[\"grant\"]}]}", quoted);
            StringCchPrintfA(config, sizeof config, "{\"coworkUserFilesPath\":%s}", targetQuoted);
        }
        ready = ready && Save(registryPath, json) && SaveIn(profiles.items[1].storageDir, CLAUDE_DESKTOP_SETTINGS, config) && FixturePath(L"feature-cowork-tasks.zip", archive, ARRAYSIZE(archive));
        output = ready ? Zip_Create(archive, &code) : NULL;
        ready = output && Zip_AddData(output, "manifest.json", manifest, sizeof manifest - 1) && Zip_AddFile(output, member, registryPath) &&
                VaultTasks_AddRegistry(output, &profiles.items[0], registryPath, member, FALSE, error, ARRAYSIZE(error));
        if (output) ready = Zip_Close(output, ready) && ready;
        zip = ready ? Zip_Open(archive) : NULL;
        StringCchPrintfA(prefix, sizeof prefix, "task-definitions/%016I64x", Core_HashBytes(CORE_HASH_START, member, strlen(member)));
        StringCchPrintfA(coverage, sizeof coverage, "%s/coverage.json", prefix); StringCchPrintfA(grantMember, sizeof grantMember, "%s/artifact-publish-grants.json", prefix);
        Check("Cowork definitions include scoped grants for backup only", zip && Zip_Find(zip, grantMember) >= 0 && VaultTasks_Describe(zip, coverage, error, ARRAYSIZE(error)) && wcsstr(error, L"saved=1") && Zip_Validate(archive, NULL, &files, error, ARRAYSIZE(error)));
        mapped = zip ? VaultTasks_RestoreRegistry(zip, &profiles.items[1], member, json, strlen(json), FALSE, &aware, &n, &code) : NULL;
        ready = mapped && Core_JsonMember(mapped, n, "scheduledTasks", &tasks, &tasksLength) && Core_JsonEachElement(tasks, tasksLength, FeatureFirst, &first) && first.value && Core_JsonMember(first.value, first.length, "filePath", &v, &valueLength) && Core_JsonString(v, valueLength, mappedPath, ARRAYSIZE(mappedPath));
        Check("Cowork definition restore uses target configured root", aware && ready && Core_PathUnder(mappedPath, targetRoot) && FileHas(mappedPath, "Cowork instructions"));
        Check("Cowork authorization is not replayed into another account", mapped && !strstr(mapped, "approvedPermissions") && !strstr(mapped, "userSelectedFolders") && Join(profiles.items[1].storageDir, L"scheduled-task-grants\\account\\org\\artifact-publish-grants.json", grants, ARRAYSIZE(grants)) && !FileThere(grants));
        if (mapped) HeapFree(GetProcessHeap(), 0, mapped);
        FixturePath(L"feature-missing-definition.zip", broken, ARRAYSIZE(broken)); output = Zip_Create(broken, &code); ready = output != NULL && zip != NULL;
        for (z = 0; zip && z < Zip_Count(zip) && ready; z++) {
            const char *name = Zip_Name(zip, z); DWORD bytes = 0; void *value;
            if (strstr(name, "/SKILL.md")) continue;
            value = Zip_Read(zip, z, 16u * 1024u * 1024u, &bytes);
            ready = value && Zip_AddData(output, name, value, bytes); if (value) HeapFree(GetProcessHeap(), 0, value);
        }
        if (output) ready = Zip_Close(output, ready) && ready;
        Check("coverage claiming a missing task payload fails before restoration", ready && !Zip_Validate(broken, NULL, &files, error, ARRAYSIZE(error)));
        Zip_Free(zip); zip = ready ? Zip_Open(broken) : NULL; aware = FALSE;
        mapped = zip ? VaultTasks_RestoreRegistry(zip, &profiles.items[1], member, json, strlen(json), FALSE, &aware, &n, &code) : NULL;
        Check("direct definition restore also rejects inconsistent coverage", aware && !mapped && code == ERROR_INVALID_DATA);
        if (mapped) HeapFree(GetProcessHeap(), 0, mapped); Zip_Free(zip);
    }
    {
        static const char *const names[][3] = { { "File.json", "file.json", NULL }, { "parent", "parent-sibling", "parent/child" }, { "NUL.txt", NULL, NULL } };
        int scenario; DWORD code = 0; ZipOut *writer; ZipIn *zip; HANDLE cancel = CreateEventW(NULL, TRUE, TRUE, NULL);
        FixturePath(L"feature-generic.zip", archive, ARRAYSIZE(archive));
        writer = Zip_Create(archive, &code); ready = writer && Zip_AddData(writer, "manager.json", "{}", 2);
        if (writer) ready = Zip_Close(writer, ready) && ready; zip = ready ? Zip_Open(archive) : NULL;
        Check("generic validation accepts a non-session migration ZIP on its open handle", zip && Zip_ValidateAll(zip, NULL, error, ARRAYSIZE(error)));
        Check("generic validation cancellation never requires a session manifest", zip && cancel && !Zip_ValidateAll(zip, cancel, error, ARRAYSIZE(error)));
        Zip_Free(zip); if (cancel) CloseHandle(cancel);
        for (scenario = 0; scenario < 3; scenario++) {
            int z; writer = Zip_Create(archive, &code); ready = writer != NULL;
            for (z = 0; z < 3 && names[scenario][z] && ready; z++) ready = Zip_AddData(writer, names[scenario][z], "{}", 2);
            if (writer) ready = Zip_Close(writer, ready) && ready; zip = ready ? Zip_Open(archive) : NULL;
            Check(scenario == 0 ? "generic validation rejects Windows case aliases" : scenario == 1 ? "generic validation rejects nonadjacent file parent collisions" : "generic validation rejects Windows device names", zip && !Zip_ValidateAll(zip, NULL, error, ARRAYSIZE(error)));
            Zip_Free(zip);
        }
    }
    {
        PurgeItem item; ZeroMemory(&item, sizeof item); item.kind = PURGE_UNKNOWN;
        StringCchCopyW(item.id, ARRAYSIZE(item.id), L"abcdef01-0000-4000-8000-000000000077");
        ready = Join(projects, L"feature-orphan\\abcdef01-0000-4000-8000-000000000077.jsonl", item.path, ARRAYSIZE(item.path)) && Save(item.path, "{}\n");
        Check("unlisted local transcript can be resumed without creating an entry", ready && SessionPurge_CanOpen(&item));
        item.restorable = TRUE; Check("known deleted conversations use restore rather than orphan Resume", !SessionPurge_CanOpen(&item));
        item.restorable = FALSE; StringCchCopyW(item.id, ARRAYSIZE(item.id), L"invalid-id"); Check("orphan Resume rejects invalid session IDs", !SessionPurge_CanOpen(&item));
    }
}
