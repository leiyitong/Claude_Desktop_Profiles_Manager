/* Entirely private files; browsers and confirmation are stand-ins. */
#include "../src/app.h"
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static int checks, failures, questions, opens;
static BOOL allow;
static void Check(const char *name, BOOL ok)
{
    checks++;
    if (!ok) { failures++; printf("FAIL %s\n", name); }
}
static BOOL FixtureAsk(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel)
{
    (void)owner; (void)icon; (void)ok; (void)cancel;
    questions++;
    Check("artifact confirmation defaults to cancel", defaultCancel);
    Check("artifact confirmation names source and target", wcsstr(text, L"source.md") != NULL && wcsstr(text, L"https://example.invalid/artifact") != NULL);
    return allow;
}
static HINSTANCE WINAPI FixtureOpen(HWND owner, LPCWSTR operation, LPCWSTR file, LPCWSTR arguments, LPCWSTR dir, INT show)
{
    (void)owner; (void)operation; (void)file; (void)dir; (void)show;
    opens++;
    Check("artifact launch has no command arguments", arguments == NULL);
    return (HINSTANCE)(INT_PTR)33;
}
#define SessionTools_OpenArtifact TestedSessionTools_OpenArtifact
#define SessionTools_Details TestedSessionTools_Details
#define SessionTools_Catalog TestedSessionTools_Catalog
#define SessionTools_Export TestedSessionTools_Export
#define SessionTools_Context TestedSessionTools_Context
#define SessionTools_ExportFile TestedSessionTools_ExportFile
#define SessionTools_PreviewPage TestedSessionTools_PreviewPage
#define Ui_Ask FixtureAsk
#define ShellExecuteW FixtureOpen
#include "../src/sessiontools.c"
#undef Ui_Ask
#undef ShellExecuteW

static BOOL Save(const WCHAR *path, const char *text)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD bytes = (DWORD)strlen(text), written;
    BOOL ok = file != INVALID_HANDLE_VALUE && WriteFile(file, text, bytes, &written, NULL) && written == bytes;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}
static void FreeTools(ToolDialog *state)
{
    int i;
    for (i = 0; i < state->count; i++) if (state->items[i].transcript) HeapFree(GetProcessHeap(), 0, state->items[i].transcript);
    if (state->items) HeapFree(GetProcessHeap(), 0, state->items);
    ZeroMemory(state, sizeof *state);
}
static void TestRewrittenSearchContext(const WCHAR *source)
{
    ULONGLONG offset = 0;
    size_t end = 0;
    WCHAR *context;
    Check("rewrite fixture initially matches", Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"OriginalNeedle\"}}\n") &&
          SessionSearch_Locate(source, L"OriginalNeedle", FALSE, TRUE, &offset, &end) && offset == 0);
    Check("rewrite fixture changes at the same offset", Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"Different text\"}}\n"));
    context = SessionTools_Context(source, offset, L"OriginalNeedle", FALSE, TRUE);
    Check("context rejects a replaced nonmatching message", !context);
    if (context) HeapFree(GetProcessHeap(), 0, context);
    Check("regex fixture initially matches", Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"Order 123\"}}\n") &&
          SessionSearch_Locate(source, L"Order [0-9]+", TRUE, TRUE, &offset, &end));
    Check("regex fixture changes to nonmatching text", Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"Order ABC\"}}\n"));
    context = SessionTools_Context(source, offset, L"Order [0-9]+", TRUE, TRUE);
    Check("context rechecks regex semantics after rewrite", !context);
    if (context) HeapFree(GetProcessHeap(), 0, context);
    Check("case fixture initially matches", Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"CaseNeedle\"}}\n") &&
          SessionSearch_Locate(source, L"CaseNeedle", FALSE, TRUE, &offset, &end));
    Check("case fixture changes only its case", Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"caseneedle\"}}\n"));
    context = SessionTools_Context(source, offset, L"CaseNeedle", FALSE, TRUE);
    Check("context preserves case-sensitive rejection", !context);
    if (context) HeapFree(GetProcessHeap(), 0, context);
    context = SessionTools_Context(source, offset, L"CaseNeedle", FALSE, FALSE);
    Check("context preserves case-insensitive acceptance", context && wcsstr(context, L"caseneedle"));
    if (context) HeapFree(GetProcessHeap(), 0, context);
    context = SessionTools_Context(source, offset, L"Case[Nn]eedle", TRUE, TRUE);
    Check("regex context preserves case-sensitive rejection", !context);
    if (context) HeapFree(GetProcessHeap(), 0, context);
    context = SessionTools_Context(source, offset, L"Case[Nn]eedle", TRUE, FALSE);
    Check("regex context preserves case-insensitive acceptance", context && wcsstr(context, L"caseneedle"));
    if (context) HeapFree(GetProcessHeap(), 0, context);
    context = SessionTools_Context(source, 1, L"caseneedle", FALSE, TRUE);
    Check("context rejects stale offsets inside a JSON line", !context);
    if (context) HeapFree(GetProcessHeap(), 0, context);
}

static void TestHistoricalCatalog(const WCHAR *root)
{
    WCHAR code[MAX_PATH], projects[MAX_PATH], encoded[MAX_PATH], project[MAX_PATH], transcript[MAX_PATH], entries[MAX_PATH], sessions[MAX_PATH], account[MAX_PATH], local[MAX_PATH], remote[MAX_PATH];
    WCHAR previous[32768];
    DWORD oldLength = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", previous, ARRAYSIZE(previous));
    const WCHAR *id = L"11111111-2222-4333-8444-555555555555";
    ToolDialog state;
    SessionSet set;
    Profile profile;
    WCHAR *path;
    int i;
    BOOL ready;
    const char *localJson = "{\"sessionId\":\"local_11111111-2222-4333-8444-555555555555\",\"cliSessionId\":\"11111111-2222-4333-8444-555555555555\",\"cwd\":\"C:\\\\Historical\",\"title\":\"Historical title\",\"previousTitles\":[\"First old title\",\"Second old title\"],\"model\":\"future-model\",\"effort\":\"high\"}";
    ZeroMemory(&state, sizeof state);
    ZeroMemory(&set, sizeof set);
    ZeroMemory(&profile, sizeof profile);
    StringCchPrintfW(code, ARRAYSIZE(code), L"%s\\code", root);
    StringCchPrintfW(projects, ARRAYSIZE(projects), L"%s\\projects", code);
    Core_ProjectDirName(L"C:\\Historical", encoded, ARRAYSIZE(encoded));
    StringCchPrintfW(project, ARRAYSIZE(project), L"%s\\%s", projects, encoded);
    StringCchPrintfW(transcript, ARRAYSIZE(transcript), L"%s\\%s.jsonl", project, id);
    StringCchPrintfW(sessions, ARRAYSIZE(sessions), L"%s\\claude-code-sessions", root);
    StringCchPrintfW(account, ARRAYSIZE(account), L"%s\\old-account", sessions);
    StringCchPrintfW(entries, ARRAYSIZE(entries), L"%s\\old-organization", account);
    StringCchPrintfW(local, ARRAYSIZE(local), L"%s\\local_fixture.json", entries);
    StringCchPrintfW(remote, ARRAYSIZE(remote), L"%s\\local_remote.json", entries);
    ready = CreateDirectoryW(code, NULL) && CreateDirectoryW(projects, NULL) && CreateDirectoryW(project, NULL) && CreateDirectoryW(sessions, NULL) && CreateDirectoryW(account, NULL) && CreateDirectoryW(entries, NULL) &&
            Save(transcript, "{\"type\":\"user\",\"message\":{\"content\":\"Private history\"}}\n") && Save(local, localJson) &&
            Save(remote, "{\"sessionId\":\"ssh_fixture\",\"title\":\"Remote title\",\"sshConfig\":{\"token\":\"PRIVATE_REMOTE_SECRET\"},\"cliSessionId\":\"11111111-2222-4333-8444-555555555555\",\"cwd\":\"C:\\\\Historical\"}") &&
            SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", code);
    Check("private historical catalog fixture", ready);
    if (ready) {
        StringCchCopyW(profile.storageDir, ARRAYSIZE(profile.storageDir), root);
        StringCchCopyW(profile.dataDir, ARRAYSIZE(profile.dataDir), root);
        Check("session store retains old titles and dynamic execution settings", SessionStore_LoadEntries(&set, &profile) && set.entryCount == 1 &&
              set.entries[0].previousTitles && wcscmp(set.entries[0].previousTitles, L"First old title") == 0 &&
              wcscmp(set.entries[0].model, L"future-model") == 0 && wcscmp(set.entries[0].effort, L"high") == 0 && set.source[0].elsewhere == 1);
        SessionStore_Free(&set);
        path = HistoricalTranscript(localJson, strlen(localJson));
        Check("local historical UUID resolves only under private projects", path && Core_PathEquals(path, transcript));
        if (path) HeapFree(GetProcessHeap(), 0, path);
        path = HistoricalTranscript("{\"cliSessionId\":\"../outside\",\"cwd\":\"C:\\\\Historical\"}", 57);
        Check("historical transcript rejects path-injection IDs", !path);
        if (path) HeapFree(GetProcessHeap(), 0, path);
        CatalogOrganization(&state, entries, L"old-account", L"old-organization", TRUE);
        Check("history lists local and remote without writable SessionSet", state.count == 2 && set.entryCount == 0 && set.rowCount == 0);
        for (i = 0; i < state.count; i++) {
            if (wcscmp(state.items[i].label, L"Historical title") == 0) Check("historical local entry offers read-only transcript", state.items[i].transcript != NULL);
            if (wcscmp(state.items[i].label, L"Remote title") == 0) Check("remote entry never exposes a local action or secret", !state.items[i].transcript && !wcsstr(state.items[i].detail, L"PRIVATE_REMOTE_SECRET"));
        }
        FreeTools(&state);
        CatalogOrganization(&state, entries, L"old-account", L"old-organization", FALSE);
        Check("remote catalog excludes local sessions", state.count == 1 && !state.items[0].transcript && wcscmp(state.items[0].label, L"Remote title") == 0);
        FreeTools(&state);
    }
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", oldLength ? previous : NULL);
    DeleteFileW(transcript); DeleteFileW(local); DeleteFileW(remote);
    RemoveDirectoryW(project); RemoveDirectoryW(projects); RemoveDirectoryW(code); RemoveDirectoryW(entries); RemoveDirectoryW(account); RemoveDirectoryW(sessions);
}

int wmain(void)
{
    WCHAR root[MAX_PATH], temporary[MAX_PATH], source[MAX_PATH], output[MAX_PATH], entry[MAX_PATH];
    DWORD messages, omitted, length;
    char *text, *longMessage;
    BOOL ready;
    ULONGLONG offset, cachedOffset;
    size_t end, cachedEnd;
    WCHAR *context;
    ToolDialog state;
    static const char prefix[] = "{\"type\":\"user\",\"message\":{\"content\":\"";
    static const char suffix[] = "NEEDLE_AFTER_64K\"}}\n";
    static const char json[] = "{\"sessionId\":\"local_fixture\",\"title\":\"Fixture\",\"model\":\"future-model-name\",\"effort\":\"custom\",\"permissionMode\":\"default\",\"remoteMcpServersConfig\":{\"token\":\"PRIVATE_SECRET\"},\"rewindEdges\":[{\"id\":\"e\",\"parent\":\"parent\",\"child\":\"child\",\"forkPoint\":\"point\"}],\"publishedArtifacts\":[{\"title\":\"Artifact\",\"url\":\"https://example.invalid/artifact\",\"sourcePath\":\"source.md\"}]}";
    ZeroMemory(&state, sizeof state);
    ready = GetTempPathW(ARRAYSIZE(temporary), temporary) != 0 &&
            SUCCEEDED(StringCchPrintfW(root, ARRAYSIZE(root), L"%sCDPM-session-tools-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64())) &&
            CreateDirectoryW(root, NULL) && SUCCEEDED(StringCchPrintfW(source, ARRAYSIZE(source), L"%s\\conversation.jsonl", root)) &&
            SUCCEEDED(StringCchPrintfW(output, ARRAYSIZE(output), L"%s\\reading.md", root)) &&
            SUCCEEDED(StringCchPrintfW(entry, ARRAYSIZE(entry), L"%s\\entry.json", root));
    Check("private fixture created", ready);
    if (!ready) return 1;
    Util_SetStateDir(root);
    Localize_SetLanguage(Localize_LanguageForCode(L"en"), FALSE);
    Check("artifact cancel does not open browser", !SessionTools_OpenArtifact(NULL, L"https://example.invalid/artifact", L"Fixture", L"source.md") && questions == 1 && opens == 0);
    allow = TRUE;
    Check("invalid scheme never confirms or opens", !SessionTools_OpenArtifact(NULL, L"file:///C:/a", L"Fixture", L"source.md") && questions == 1 && opens == 0);
    Check("explicit confirmation opens once", SessionTools_OpenArtifact(NULL, L"https://example.invalid/artifact", L"Fixture", L"source.md") && questions == 2 && opens == 1);
    Check("metadata fixture saved", Save(entry, json));
    LoadDetails(&state, entry, 0);
    Check("arbitrary model displayed without credential dump", state.count == 1 && wcsstr(state.items[0].detail, L"future-model-name") && !wcsstr(state.items[0].detail, L"PRIVATE_SECRET"));
    FreeTools(&state);
    LoadDetails(&state, entry, 1);
    Check("rewind metadata includes edge", state.count == 2 && wcsstr(state.items[1].detail, L"parent") && wcsstr(state.items[1].detail, L"point"));
    FreeTools(&state);
    LoadDetails(&state, entry, 2);
    Check("artifact directory preserves safe URL", state.count == 1 && wcscmp(state.items[0].url, L"https://example.invalid/artifact") == 0);
    FreeTools(&state);
    Check("reading fixture saved", Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"Hello\"}}\n{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"World\"}]}}\n{\"type\":\"progress\"}\n"));
    Check("stream reading export writes messages and counts exclusions", SessionTools_ExportFile(source, output, TRUE, &messages, &omitted) && messages == 2 && omitted == 1);
    text = Util_ReadFile(output, 1024 * 1024, FALSE, &length);
    Check("reading export names scope and contains role headings", text && strstr(text, "current transcript only") && strstr(text, "## You") && strstr(text, "World"));
    if (text) HeapFree(GetProcessHeap(), 0, text);
    DeleteFileW(output);
    Check("an HTML reading copy is a page, the message's markup escaped",
          Save(source, "{\"type\":\"user\",\"message\":{\"content\":\"Hi <b>there</b> & bye\"}}\n") &&
          SessionTools_ExportFile(source, output, READING_HTML, &messages, &omitted) && messages == 1 &&
          (text = Util_ReadFile(output, 1024 * 1024, FALSE, &length)) != NULL && strstr(text, "<!doctype html>") &&
          strstr(text, "Hi &lt;b&gt;there&lt;/b&gt; &amp; bye") && !strstr(text, "<b>there"));
    if (text) HeapFree(GetProcessHeap(), 0, text);
    text = NULL;
    DeleteFileW(output);
    Check("a JSON reading copy holds the messages with their roles, and its counts",
          SessionTools_ExportFile(source, output, READING_JSON, &messages, &omitted) && messages == 1 &&
          (text = Util_ReadFile(output, 1024 * 1024, FALSE, &length)) != NULL && Core_JsonIsValue(text, length) &&
          strstr(text, "{\"role\":\"user\",\"text\":\"Hi <b>there</b> & bye\"}") && strstr(text, "\"exported\":1"));
    if (text) HeapFree(GetProcessHeap(), 0, text);
    text = NULL;
    Check("export refuses source overwrite", !SessionTools_ExportFile(source, source, FALSE, &messages, &omitted));
    DeleteFileW(output);
    Check("private hard-link fixture created", CreateHardLinkW(output, source, NULL));
    Check("export refuses source aliases", !SessionTools_ExportFile(source, output, FALSE, &messages, &omitted));
    DeleteFileW(output);
    longMessage = (char *)HeapAlloc(GetProcessHeap(), 0, sizeof prefix + 90000 + sizeof suffix);
    if (longMessage) {
        memcpy(longMessage, prefix, sizeof prefix - 1);
        memset(longMessage + sizeof prefix - 1, 'x', 90000);
        memcpy(longMessage + sizeof prefix - 1 + 90000, suffix, sizeof suffix);
    }
    Check("long message fixture saved", longMessage && Save(source, longMessage));
    if (longMessage) HeapFree(GetProcessHeap(), 0, longMessage);
    Check("search finds content after old 64K message limit", SessionSearch_Locate(source, L"NEEDLE_AFTER_64K", FALSE, TRUE, &offset, &end) && end > 90000);
    Check("cached search preserves context position", SessionSearch_Locate(source, L"NEEDLE_AFTER_64K", FALSE, TRUE, &cachedOffset, &cachedEnd) && cachedOffset == offset && cachedEnd == end);
    context = SessionTools_Context(source, offset, L"NEEDLE_AFTER_64K", FALSE, TRUE);
    Check("context includes the hit beyond old preview limit", context && wcsstr(context, L"NEEDLE_AFTER_64K"));
    if (context) HeapFree(GetProcessHeap(), 0, context);
    Check("reading export does not truncate long message to preview size", SessionTools_ExportFile(source, output, FALSE, &messages, &omitted) && messages == 1);
    text = Util_ReadFile(output, 1024 * 1024, FALSE, &length);
    Check("reading copy includes text after 64K", text && strstr(text, "NEEDLE_AFTER_64K"));
    if (text) HeapFree(GetProcessHeap(), 0, text);
    {
        HANDLE append = CreateFileW(source, FILE_APPEND_DATA, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        char *padding = (char *)HeapAlloc(GetProcessHeap(), 0, 1024 * 1024);
        DWORD written;
        int block;
        BOOL appended = append != INVALID_HANDLE_VALUE && padding;
        if (appended) {
            memset(padding, 'x', 1024 * 1024);
            appended = WriteFile(append, "{\"type\":\"progress\",\"data\":\"", 27, &written, NULL);
            for (block = 0; appended && block < 9; block++) appended = WriteFile(append, padding, 1024 * 1024, &written, NULL) && written == 1024 * 1024;
            appended = appended && WriteFile(append, "\"}\n", 3, &written, NULL);
        }
        if (append != INVALID_HANDLE_VALUE) CloseHandle(append);
        if (padding) HeapFree(GetProcessHeap(), 0, padding);
        Check("conversation exceeds old 8MiB preview tail", appended);
        Check("early match remains locatable before old tail", SessionSearch_Locate(source, L"NEEDLE_AFTER_64K", FALSE, TRUE, &offset, &end));
        context = SessionTools_Context(source, offset, L"NEEDLE_AFTER_64K", FALSE, TRUE);
        Check("early match context is independent from preview tail", context && wcsstr(context, L"NEEDLE_AFTER_64K"));
        if (context) HeapFree(GetProcessHeap(), 0, context);
        Check("oversize export records are explicitly counted", SessionTools_ExportFile(source, output, TRUE, &messages, &omitted) && messages == 1 && omitted == 1);
    }
    TestRewrittenSearchContext(source);
    TestHistoricalCatalog(root);
    DeleteFileW(source); DeleteFileW(output); DeleteFileW(entry);
    StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s\\claude-desktop-profiles-manager.log", root);
    DeleteFileW(temporary);
    RemoveDirectoryW(root);
    printf("Session tools: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}
