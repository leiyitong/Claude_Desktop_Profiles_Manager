/* Local session metadata and reading copies. This module has no network
 * client and never sends an entry to the writable session store. */
#include "app.h"
#include "sessiontools.h"
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <stdlib.h>

#define TOOLS_MAX_ITEMS 4096
#define TOOLS_LINE_MAX (8u * 1024u * 1024u)
#define TOOLS_FILE_MAX (512ull * 1024u * 1024u)
#define CONTEXT_CCH (128u * 1024u)

typedef struct ToolItem {
    WCHAR label[256], detail[3072], url[2048];
    WCHAR source[768];
    WCHAR *transcript;
} ToolItem;
typedef struct ToolDialog {
    ToolItem *items;
    int count, capacity, skipped;
    const WCHAR *title, *note;
    HWND list;
    BOOL historical;
} ToolDialog;

static void StringMember(const char *json, size_t length, const char *key, WCHAR *out, size_t cch)
{
    const char *value;
    size_t n;
    out[0] = 0;
    if (Core_JsonMember(json, length, key, &value, &n)) Core_JsonString(value, n, out, cch);
}

static ToolItem *NewItem(ToolDialog *state, const WCHAR *label)
{
    ToolItem *grown, *item;
    if (state->count >= TOOLS_MAX_ITEMS) { state->skipped++; return NULL; }
    if (state->count == state->capacity) {
        int capacity = state->capacity ? state->capacity * 2 : 32;
        grown = state->items ? (ToolItem *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, state->items, (size_t)capacity * sizeof *grown)
                             : (ToolItem *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)capacity * sizeof *grown);
        if (!grown) { state->skipped++; return NULL; }
        state->items = grown;
        state->capacity = capacity;
    }
    item = &state->items[state->count++];
    StringCchCopyW(item->label, ARRAYSIZE(item->label), label);
    return item;
}

static void AppendDetail(ToolItem *item, const WCHAR *text)
{
    const WCHAR *cut = TR(L"\r\n[Additional metadata omitted]");
    if (FAILED(StringCchCatW(item->detail, ARRAYSIZE(item->detail), text))) {
        size_t at = ARRAYSIZE(item->detail) - wcslen(cut) - 1;
        if (at && IS_LOW_SURROGATE(item->detail[at])) at--;
        item->detail[at] = 0;
        StringCchCatW(item->detail, ARRAYSIZE(item->detail), cut);
    }
}

/* Only selected scalar values leave the entry. Never display raw tool,
 * permission, environment or MCP configuration, which can hold credentials. */
static void Field(ToolItem *item, const char *json, size_t length, const char *key, const WCHAR *label)
{
    const char *value;
    size_t n;
    WCHAR decoded[768], line[850];
    if (!Core_JsonMember(json, length, key, &value, &n)) return;
    decoded[0] = 0;
    if (n && value[0] == '"') {
        if (!Core_JsonString(value, n, decoded, ARRAYSIZE(decoded))) StringCchCopyW(decoded, ARRAYSIZE(decoded), TR(L"[Long value omitted]"));
    } else if (n <= 40 && (value[0] == '-' || (value[0] >= '0' && value[0] <= '9') || value[0] == 't' || value[0] == 'f')) {
        int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, (int)n, decoded, ARRAYSIZE(decoded) - 1);
        if (!written) return;
        decoded[written] = 0;
    } else return;
    StringCchPrintfW(line, ARRAYSIZE(line), L"%s: %s\r\n", label, decoded);
    AppendDetail(item, line);
}

static BOOL AddBranch(void *context, const char *value, size_t length)
{
    ToolDialog *state = (ToolDialog *)context;
    WCHAR parent[SESSION_ID_CCH], child[SESSION_ID_CCH], label[256];
    ToolItem *item;
    StringMember(value, length, "parent", parent, ARRAYSIZE(parent));
    StringMember(value, length, "child", child, ARRAYSIZE(child));
    StringCchPrintfW(label, ARRAYSIZE(label), L"%s \x2192 %s", parent, child);
    item = NewItem(state, label);
    if (item) {
        Field(item, value, length, "id", TR(L"Edge"));
        Field(item, value, length, "parent", TR(L"Parent"));
        Field(item, value, length, "child", TR(L"Child"));
        Field(item, value, length, "forkPoint", TR(L"Fork point"));
        Field(item, value, length, "at", TR(L"Recorded at"));
        Field(item, value, length, "cwd", TR(L"Folder"));
    }
    return TRUE;
}

static BOOL AddArtifact(void *context, const char *value, size_t length)
{
    ToolDialog *state = (ToolDialog *)context;
    WCHAR title[256];
    ToolItem *item;
    StringMember(value, length, "title", title, ARRAYSIZE(title));
    item = NewItem(state, title[0] ? title : TR(L"Untitled artifact"));
    if (item) {
        StringMember(value, length, "url", item->url, ARRAYSIZE(item->url));
        Field(item, value, length, "title", TR(L"Title"));
        StringMember(value, length, "sourcePath", item->source, ARRAYSIZE(item->source));
        Field(item, value, length, "sourcePath", TR(L"Source path"));
        Field(item, value, length, "updatedAt", TR(L"Updated at"));
        Field(item, value, length, "url", TR(L"URL"));
        if (!Core_SafeHttpsUrl(item->url)) item->url[0] = 0;
    }
    return TRUE;
}

static void LoadDetails(ToolDialog *state, const WCHAR *file, int page)
{
    DWORD length;
    char *json = Util_ReadFile(file, SESSION_ENTRY_MAX_BYTES, FALSE, &length);
    const char *value;
    size_t n;
    ToolItem *item;
    if (!json) { state->skipped++; return; }
    if (!Core_JsonIsValue(json, length)) { state->skipped++; HeapFree(GetProcessHeap(), 0, json); return; }
    if (page == 1) {
        item = NewItem(state, TR(L"Session lineage"));
        if (item) {
            Field(item, json, length, "cliSessionId", TR(L"Transcript"));
            Field(item, json, length, "forkedFromSessionId", TR(L"Forked from"));
            Field(item, json, length, "preClearCliSessionId", TR(L"Before clear"));
            Field(item, json, length, "unarchivedCliSessionId", TR(L"Unarchived transcript"));
            if (Core_JsonMember(json, length, "priorCliSessionIds", &value, &n)) {
                WCHAR *ids = Core_JsonStringList(value, n), *at;
                for (at = ids; at && *at; at += wcslen(at) + 1) {
                    AppendDetail(item, TR(L"Prior transcript: "));
                    AppendDetail(item, at);
                    AppendDetail(item, L"\r\n");
                }
                if (ids) HeapFree(GetProcessHeap(), 0, ids);
            }
        }
        if (Core_JsonMember(json, length, "rewindEdges", &value, &n)) Core_JsonEachElement(value, n, AddBranch, state);
    } else if (page == 2) {
        if (Core_JsonMember(json, length, "publishedArtifacts", &value, &n)) Core_JsonEachElement(value, n, AddArtifact, state);
    } else {
        item = NewItem(state, TR(L"Saved execution settings"));
        if (item) {
            Field(item, json, length, "title", TR(L"Title"));
            Field(item, json, length, "model", TR(L"Model"));
            Field(item, json, length, "effort", TR(L"Effort"));
            Field(item, json, length, "permissionMode", TR(L"Permission mode"));
            Field(item, json, length, "cliSessionId", TR(L"Transcript"));
            Field(item, json, length, "cwd", TR(L"Folder"));
            Field(item, json, length, "createdAt", TR(L"Created at"));
            Field(item, json, length, "lastActivityAt", TR(L"Last activity"));
            if (Core_JsonMember(json, length, "sessionSettings", &value, &n)) {
                Field(item, value, n, "outputStyle", TR(L"Output style"));
                Field(item, value, n, "ultracode", TR(L"Ultracode"));
            }
            /* A count is enough to identify the saved surface without
             * exposing a server definition, tool prompt or secret. */
            if (Core_JsonMember(json, length, "toolSurfaceSnapshot", &value, &n) && n > 2 && value[0] != 'n')
                AppendDetail(item, TR(L"A tool surface snapshot is saved. Its contents are private.\r\n"));
            if (Core_JsonMember(json, length, "remoteMcpServersConfig", &value, &n) && n > 2 && value[0] != 'n')
                AppendDetail(item, TR(L"MCP configuration is saved. Its contents are private.\r\n"));
        }
    }
    HeapFree(GetProcessHeap(), 0, json);
}

static void ExportReading(HWND owner, const WCHAR *source);

static void PreviewHistorical(HWND owner, const WCHAR *path)
{
    DWORD length;
    char *content = Util_ReadFile(path, 8u * 1024u * 1024u, TRUE, &length), *line, *end;
    WCHAR *text = NULL, *message = NULL;
    if (!content) { Ui_Message(owner, MB_ICONWARNING, TR(L"The conversation could not be read: %s"), path); return; }
    text = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1024u * 1024u * sizeof(WCHAR));
    message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, 65536u * sizeof(WCHAR));
    if (!text || !message) goto done;
    line = content;
    /* A tail read may start inside a JSON record. */
    if (length == 8u * 1024u * 1024u) {
        line = strchr(content, '\n');
        if (!line) line = content + length;
        else line++;
    }
    for (; *line; line = *end ? end + 1 : end) {
        TranscriptRole role;
        end = strchr(line, '\n');
        if (!end) end = content + length;
        role = Core_TranscriptLine(line, (size_t)(end - line), message, 65536);
        if (role == TRANSCRIPT_NONE) continue;
        if (FAILED(StringCchCatW(text, 1024u * 1024u, role == TRANSCRIPT_USER ? TR(L"You\r\n") : L"Claude\r\n")) ||
            FAILED(StringCchCatW(text, 1024u * 1024u, message)) || FAILED(StringCchCatW(text, 1024u * 1024u, L"\r\n\r\n"))) break;
    }
    Ui_ReviewText(owner, TR(L"Historical local conversation"),
                  TR(L"Read-only preview: up to the last 8 MiB, 64K characters per message and 1M characters total. Reading export has a separate, larger limit."), text, NULL);
done:
    if (text) HeapFree(GetProcessHeap(), 0, text);
    if (message) HeapFree(GetProcessHeap(), 0, message);
    HeapFree(GetProcessHeap(), 0, content);
}

static void SelectTool(HWND dialog, ToolDialog *state)
{
    LRESULT selected = SendMessageW(state->list, LB_GETCURSEL, 0, 0);
    LRESULT index = selected >= 0 ? SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)selected, 0) : -1;
    const ToolItem *item = index >= 0 && index < state->count ? &state->items[index] : NULL;
    SetDlgItemTextW(dialog, IDC_ST_DETAIL, item ? item->detail : L"");
    EnableWindow(GetDlgItem(dialog, IDC_ST_OPEN), item && item->url[0]);
    EnableWindow(GetDlgItem(dialog, IDC_ST_PREVIEW), item && item->transcript);
    EnableWindow(GetDlgItem(dialog, IDC_ST_EXPORT), item && item->transcript);
}

static void FillTools(HWND dialog, ToolDialog *state)
{
    WCHAR filter[256];
    int i;
    GetDlgItemTextW(dialog, IDC_ST_FILTER, filter, ARRAYSIZE(filter));
    SendMessageW(state->list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(state->list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < state->count; i++) {
        LRESULT at;
        if (filter[0] && !Core_ContainsI(state->items[i].label, filter) && !Core_ContainsI(state->items[i].detail, filter)) continue;
        at = SendMessageW(state->list, LB_ADDSTRING, 0, (LPARAM)state->items[i].label);
        if (at >= 0) SendMessageW(state->list, LB_SETITEMDATA, (WPARAM)at, i);
    }
    SendMessageW(state->list, LB_SETCURSEL, 0, 0);
    SendMessageW(state->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->list, NULL, TRUE);
    SelectTool(dialog, state);
}

/* Every launch path goes through this decision. Cancel is the default. */
BOOL SessionTools_OpenArtifact(HWND owner, const WCHAR *url, const WCHAR *title, const WCHAR *source)
{
    WCHAR question[3400];
    INT_PTR result;
    if (!Core_SafeHttpsUrl(url)) return FALSE;
    if (FAILED(StringCchPrintfW(question, ARRAYSIZE(question),
        TR(L"Open this cloud artifact in your browser?\n\nTitle: %s\nSource: %s\nTarget: %s\n\nYour browser will connect to this external website. It may use your signed-in account."),
        title ? title : L"", source ? source : L"", url))) return FALSE;
    if (!Ui_Ask(owner, IDI_QUESTION, question, TR(L"Open link"), TR(L"Cancel"), TRUE)) return FALSE;
    result = (INT_PTR)ShellExecuteW(owner, L"open", url, NULL, NULL, SW_SHOWNORMAL);
    if (result <= 32) Ui_Message(owner, MB_ICONWARNING, TR(L"The link could not be opened (error %ld)."), (long)result);
    return result > 32;
}

static INT_PTR CALLBACK ToolsProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    ToolDialog *state = (ToolDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (msg == WM_INITDIALOG) {
        WCHAR note[1024];
        state = (ToolDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        state->list = GetDlgItem(dialog, IDC_ST_LIST);
        SetWindowTextW(dialog, state->title);
        StringCchPrintfW(note, ARRAYSIZE(note), TR(L"%s  Entries: %d. Unreadable or omitted: %d."), state->note, state->count, state->skipped);
        SetDlgItemTextW(dialog, IDC_ST_NOTE, note);
        SetDlgItemTextW(dialog, IDC_ST_OPEN, TR(L"Open link\x2026"));
        SetDlgItemTextW(dialog, IDCANCEL, TR(L"Close"));
        SetDlgItemTextW(dialog, IDC_ST_PREVIEW, TR(L"Preview local conversation"));
        SetDlgItemTextW(dialog, IDC_ST_EXPORT, TR(L"Export reading copy\x2026"));
        ShowWindow(GetDlgItem(dialog, IDC_ST_PREVIEW), state->historical ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(dialog, IDC_ST_EXPORT), state->historical ? SW_SHOW : SW_HIDE);
        SendDlgItemMessageW(dialog, IDC_ST_FILTER, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Filter local metadata"));
        SendDlgItemMessageW(dialog, IDC_ST_DETAIL, EM_SETLIMITTEXT, 0, 0);
        Theme_SmoothView(state->list);
        FillTools(dialog, state);
        return TRUE;
    }
    if (!state) return FALSE;
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (LOWORD(wp) == IDC_ST_FILTER && HIWORD(wp) == EN_CHANGE) { FillTools(dialog, state); return TRUE; }
        if (LOWORD(wp) == IDC_ST_LIST && HIWORD(wp) == LBN_SELCHANGE) { SelectTool(dialog, state); return TRUE; }
        if (LOWORD(wp) == IDC_ST_PREVIEW || LOWORD(wp) == IDC_ST_EXPORT) {
            LRESULT at = SendMessageW(state->list, LB_GETCURSEL, 0, 0), index = at >= 0 ? SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)at, 0) : -1;
            if (index >= 0 && index < state->count && state->items[index].transcript) {
                if (LOWORD(wp) == IDC_ST_PREVIEW) PreviewHistorical(dialog, state->items[index].transcript);
                else ExportReading(dialog, state->items[index].transcript);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDC_ST_OPEN) {
            LRESULT at = SendMessageW(state->list, LB_GETCURSEL, 0, 0), index = at >= 0 ? SendMessageW(state->list, LB_GETITEMDATA, (WPARAM)at, 0) : -1;
            if (index >= 0 && index < state->count && Core_SafeHttpsUrl(state->items[index].url)) {
                SessionTools_OpenArtifact(dialog, state->items[index].url, state->items[index].label, state->items[index].source);
            }
            return TRUE;
        }
    }
    return FALSE;
}

static void ShowTools(HWND owner, ToolDialog *state)
{
    int i;
    Ui_Dialog(owner, IDD_SESSIONTOOLS, ToolsProc, (LPARAM)state);
    for (i = 0; i < state->count; i++) if (state->items[i].transcript) HeapFree(GetProcessHeap(), 0, state->items[i].transcript);
    if (state->items) HeapFree(GetProcessHeap(), 0, state->items);
}

void SessionTools_Details(HWND owner, const WCHAR *file, int page)
{
    ToolDialog state;
    ZeroMemory(&state, sizeof state);
    state.title = page == 1 ? TR(L"Rewind and fork relationships") : page == 2 ? TR(L"Session artifacts") : TR(L"Saved execution settings");
    state.note = TR(L"Read-only local metadata. Values describe saved history, not live state.");
    LoadDetails(&state, file, page);
    ShowTools(owner, &state);
}

/* A UUID and an encoded project component stay under the shared projects
 * folder. Reparse points are not used as transcript files. */
static WCHAR *HistoricalTranscript(const char *json, size_t length)
{
    WCHAR root[LONG_PATH_CCH], cwd[LONG_PATH_CCH], project[LONG_PATH_CCH], path[LONG_PATH_CCH], extended[LONG_PATH_CCH], id[SESSION_ID_CCH];
    static const char *const directories[] = { "originCwd", "cwd" };
    size_t i;
    StringMember(json, length, "cliSessionId", id, ARRAYSIZE(id));
    if (!Core_IsUuid(id) || !SessionStore_ProjectsDir(root, ARRAYSIZE(root)) || Util_IsDirectoryLink(root)) return NULL;
    for (i = 0; i < ARRAYSIZE(directories); i++) {
        DWORD attributes;
        WCHAR *copy;
        StringMember(json, length, directories[i], cwd, ARRAYSIZE(cwd));
        if (!Core_ProjectDirName(cwd, project, ARRAYSIZE(project)) ||
            FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", root, project)) || Util_IsDirectoryLink(path) ||
            FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s\\%s.jsonl", root, project, id)) ||
            !Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) continue;
        attributes = GetFileAttributesW(extended);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) continue;
        copy = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (wcslen(path) + 1) * sizeof(WCHAR));
        if (copy) StringCchCopyW(copy, wcslen(path) + 1, path);
        return copy;
    }
    return NULL;
}

/* Catalogs never call AddEntry or create writable rows. Only two directory
 * levels are visited; reparse points are not followed. */
static void CatalogOrganization(ToolDialog *state, const WCHAR *dir, const WCHAR *account, const WCHAR *organization, BOOL historical)
{
    WIN32_FIND_DATAW found;
    HANDLE files = Util_FindFiles(dir, L"*.json", &found, FALSE);
    if (files == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR path[LONG_PATH_CCH], title[256];
        DWORD length;
        char *json;
        SessionEntryKind kind;
        ToolItem *item;
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName))) { state->skipped++; continue; }
        json = Util_ReadFile(path, SESSION_ENTRY_MAX_BYTES, FALSE, &length);
        if (!json) { state->skipped++; continue; }
        kind = Core_SessionEntryKind(json, length);
        if (kind == ENTRY_NOT_SESSION || (!historical && kind != ENTRY_ELSEWHERE)) { HeapFree(GetProcessHeap(), 0, json); continue; }
        StringMember(json, length, "title", title, ARRAYSIZE(title));
        item = NewItem(state, title[0] ? title : TR(L"Untitled session"));
        if (item) {
            StringCchPrintfW(item->detail, ARRAYSIZE(item->detail), L"%s: %s\r\n%s: %s\r\n%s: %s\r\n", TR(L"Account folder"), account,
                             TR(L"Organization folder"), organization, TR(L"Location"), kind == ENTRY_ELSEWHERE ? TR(L"Remote metadata only") : TR(L"Historical local entry"));
            if (historical && kind == ENTRY_LOCAL) {
                item->transcript = HistoricalTranscript(json, length);
                if (!item->transcript) AppendDetail(item, TR(L"No local transcript was found for preview or reading export.\r\n"));
            }
            Field(item, json, length, "sessionId", TR(L"Session"));
            Field(item, json, length, "cliSessionId", TR(L"Transcript"));
            Field(item, json, length, "model", TR(L"Model"));
            Field(item, json, length, "effort", TR(L"Effort"));
            Field(item, json, length, "cwd", TR(L"Folder"));
            Field(item, json, length, "createdAt", TR(L"Created at"));
            Field(item, json, length, "lastActivityAt", TR(L"Last activity"));
            {
                const char *remote;
                size_t remoteLength;
                if (Core_JsonMember(json, length, "sshConfig", &remote, &remoteLength) && remoteLength > 4)
                    AppendDetail(item, TR(L"SSH configuration is recorded.\r\n"));
                if (Core_JsonMember(json, length, "wslConfig", &remote, &remoteLength) && remoteLength > 4)
                    AppendDetail(item, TR(L"WSL configuration is recorded.\r\n"));
                Field(item, json, length, "cloudSessionId", TR(L"Cloud session"));
            }
        }
        /* Do not expose SSH/WSL objects: these can contain private commands. */
        HeapFree(GetProcessHeap(), 0, json);
    } while (FindNextFileW(files, &found));
    FindClose(files);
}

void SessionTools_Catalog(HWND owner, const Profile *profile, BOOL historical)
{
    ToolDialog state;
    WCHAR root[LONG_PATH_CCH], active[LONG_PATH_CCH], account[LONG_PATH_CCH], organization[LONG_PATH_CCH];
    WIN32_FIND_DATAW a, o;
    HANDLE accounts, organizations;
    BOOL signedIn;
    ZeroMemory(&state, sizeof state);
    state.historical = historical;
    state.title = historical ? TR(L"Historical account and organization sessions") : TR(L"SSH, WSL and cloud session metadata");
    state.note = historical ? TR(L"Read only. Available local conversations can be previewed or exported. Account selection and session entries are never changed.")
                            : TR(L"Read only. These records cannot be opened, edited, synchronized or deleted here.");
    active[0] = 0;
    SessionStore_EntriesDir(profile, active, ARRAYSIZE(active), &signedIn);
    if (SessionStore_SessionsDir(profile, root, ARRAYSIZE(root)) && !Util_IsDirectoryLink(root) && (accounts = Util_FindFiles(root, L"*", &a, TRUE)) != INVALID_HANDLE_VALUE) {
        do {
            if (!(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || a.cFileName[0] == L'.') continue;
            if (FAILED(StringCchPrintfW(account, ARRAYSIZE(account), L"%s\\%s", root, a.cFileName))) continue;
            organizations = Util_FindFiles(account, L"*", &o, TRUE);
            if (organizations == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(o.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (o.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || o.cFileName[0] == L'.') continue;
                if (FAILED(StringCchPrintfW(organization, ARRAYSIZE(organization), L"%s\\%s", account, o.cFileName))) continue;
                if (!historical || !Core_PathEquals(organization, active)) CatalogOrganization(&state, organization, a.cFileName, o.cFileName, historical);
            } while (FindNextFileW(organizations, &o));
            FindClose(organizations);
        } while (FindNextFileW(accounts, &a));
        FindClose(accounts);
    }
    ShowTools(owner, &state);
}

/* Verify against the same decoded snapshot the context will display. A
 * cached line/character position can become stale between the two reads. */
static BOOL ContextMatchEnd(const WCHAR *text, const WCHAR *query, BOOL regex, BOOL matchCase, size_t *messageEnd)
{
    size_t at, length = wcslen(text), n;
    CoreRegex compiled;
    if (!query || !query[0]) return FALSE;
    n = wcslen(query);
    if (regex) return Core_RegexCompile(&compiled, query, !matchCase) && Core_RegexSearchEnd(&compiled, text, length, messageEnd);
    for (at = 0; at + n <= length; at++)
        if (matchCase ? wcsncmp(text + at, query, n) == 0 :
            CompareStringOrdinal(text + at, (int)n, query, (int)n, TRUE) == CSTR_EQUAL) {
            *messageEnd = at + n;
            return TRUE;
        }
    return FALSE;
}

/* Context is centered on the decoded match, including a message whose hit
 * lies after the preview's old 4K/64K boundaries or before its last 8 MiB. */
WCHAR *SessionTools_Context(const WCHAR *path, ULONGLONG offset, const WCHAR *query, BOOL regex, BOOL matchCase)
{
    DWORD length;
    char *content = Util_ReadFile(path, 256u * 1024u * 1024u, FALSE, &length), *line, *end, *previous = NULL, *next;
    WCHAR *result = NULL, *message = NULL;
    size_t capacity, start, n, messageEnd = 0;
    if (!content || offset >= length || (offset && content[(size_t)offset - 1] != '\n')) goto done;
    line = content + (size_t)offset;
    if (line > content) {
        previous = line - 1;
        if (*previous == '\n' && previous > content) previous--;
        while (previous > content && previous[-1] != '\n') previous--;
    }
    end = strchr(line, '\n');
    if (!end) end = content + length;
    capacity = (size_t)(end - line) + 8;
    message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, max(capacity, (size_t)8192) * sizeof(WCHAR));
    result = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, CONTEXT_CCH * sizeof(WCHAR));
    if (!message || !result) {
        if (result) HeapFree(GetProcessHeap(), 0, result);
        result = NULL;
        goto done;
    }
    StringCchCopyW(result, CONTEXT_CCH, TR(L"First matching message and nearby context. Long context is excerpted.\r\n\r\n"));
    if (previous && Core_TranscriptLine(previous, (size_t)(line - previous), message, 4096) != TRANSCRIPT_NONE) {
        StringCchCatW(result, CONTEXT_CCH, message);
        StringCchCatW(result, CONTEXT_CCH, L"\r\n\r\n");
    }
    if (!Core_JsonIsValue(line, (size_t)(end - line)) ||
        Core_TranscriptLine(line, (size_t)(end - line), message, max(capacity, (size_t)8192)) == TRANSCRIPT_NONE ||
        !ContextMatchEnd(message, query, regex, matchCase, &messageEnd)) {
        HeapFree(GetProcessHeap(), 0, result);
        result = NULL;
        goto done;
    }
    n = wcslen(message);
    start = messageEnd > 48000 ? messageEnd - 48000 : 0;
    if (start > n) start = 0;
    if (start && IS_LOW_SURROGATE(message[start])) start++;
    StringCchCatW(result, CONTEXT_CCH, TR(L"[Matching message]\r\n"));
    if (start) StringCchCatW(result, CONTEXT_CCH, L"\x2026");
    StringCchCatNW(result, CONTEXT_CCH, message + start, min(n - start, (size_t)96000));
    if (n - start > 96000) StringCchCatW(result, CONTEXT_CCH, L"\x2026");
    next = *end ? end + 1 : end;
    while (*next) {
        char *last = strchr(next, '\n');
        if (!last) last = content + length;
        if (Core_TranscriptLine(next, (size_t)(last - next), message, 4096) != TRANSCRIPT_NONE) {
            StringCchCatW(result, CONTEXT_CCH, L"\r\n\r\n");
            StringCchCatW(result, CONTEXT_CCH, message);
            break;
        }
        next = *last ? last + 1 : last;
    }
done:
    if (message) HeapFree(GetProcessHeap(), 0, message);
    if (content) HeapFree(GetProcessHeap(), 0, content);
    return result;
}

static BOOL WriteBytes(HANDLE file, const void *data, DWORD bytes)
{
    DWORD written;
    return WriteFile(file, data, bytes, &written, NULL) && written == bytes;
}

static BOOL WriteText(HANDLE file, const WCHAR *text)
{
    int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
    char *utf8;
    BOOL ok;
    if (bytes <= 0) return FALSE;
    utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)bytes);
    if (!utf8) return FALSE;
    ok = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, bytes, NULL, NULL) == bytes && WriteBytes(file, utf8, (DWORD)bytes - 1);
    HeapFree(GetProcessHeap(), 0, utf8);
    return ok;
}

static BOOL ExportLine(HANDLE output, const char *line, size_t length, BOOL markdown, DWORD *messages, DWORD *omitted)
{
    WCHAR *message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (length + 8) * sizeof(WCHAR));
    TranscriptRole role;
    BOOL ok = TRUE;
    if (!message) return FALSE;
    role = Core_TranscriptLine(line, length, message, length + 8);
    if (role == TRANSCRIPT_NONE) (*omitted)++;
    else {
        const WCHAR *heading = role == TRANSCRIPT_USER ? TR(L"You") : L"Claude";
        ok = (!markdown || WriteText(output, L"## ")) && WriteText(output, heading) && WriteText(output, L"\r\n\r\n") &&
             WriteText(output, message) && WriteText(output, L"\r\n\r\n");
        if (ok) (*messages)++;
    }
    HeapFree(GetProcessHeap(), 0, message);
    return ok;
}

static BOOL SameOutputFile(HANDLE source, const WCHAR *target)
{
    HANDLE file = CreateFileW(target, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    BY_HANDLE_FILE_INFORMATION a, b;
    BOOL same = file != INVALID_HANDLE_VALUE && GetFileInformationByHandle(source, &a) && GetFileInformationByHandle(file, &b) &&
                a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return same;
}

/* Stream one current transcript into a staged UTF-8 reading copy. Oversize
 * records are counted, never exported as apparently complete messages. */
BOOL SessionTools_ExportFile(const WCHAR *source, const WCHAR *target, BOOL markdown, DWORD *messages, DWORD *omitted)
{
    WCHAR inputPath[LONG_PATH_CCH], outputPath[LONG_PATH_CCH], staged[LONG_PATH_CCH], stagePath[LONG_PATH_CCH];
    HANDLE input = INVALID_HANDLE_VALUE, output = INVALID_HANDLE_VALUE;
    BYTE block[65536];
    char *line = NULL;
    DWORD bytes, i, error = ERROR_SUCCESS;
    size_t used = 0;
    ULONGLONG total = 0;
    BOOL skipping = FALSE, ok = FALSE, stageCreated = FALSE;
    BY_HANDLE_FILE_INFORMATION before, after;
    *messages = *omitted = 0;
    if (Core_PathEquals(source, target) || !Util_ExtendedPath(source, inputPath, ARRAYSIZE(inputPath)) ||
        !Util_ExtendedPath(target, outputPath, ARRAYSIZE(outputPath)) ||
        FAILED(StringCchPrintfW(staged, ARRAYSIZE(staged), L"%s.tmp-%lu-%llu", target, GetCurrentProcessId(), GetTickCount64())) ||
        !Util_ExtendedPath(staged, stagePath, ARRAYSIZE(stagePath))) { SetLastError(ERROR_INVALID_NAME); return FALSE; }
    input = CreateFileW(inputPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (input == INVALID_HANDLE_VALUE) goto done;
    if (SameOutputFile(input, outputPath)) { SetLastError(ERROR_INVALID_NAME); goto done; }
    if (!GetFileInformationByHandle(input, &before) || before.nFileSizeHigh || before.nFileSizeLow > TOOLS_FILE_MAX) { SetLastError(ERROR_FILE_TOO_LARGE); goto done; }
    output = CreateFileW(stagePath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (output == INVALID_HANDLE_VALUE) goto done;
    stageCreated = TRUE;
    line = (char *)HeapAlloc(GetProcessHeap(), 0, TOOLS_LINE_MAX);
    if (!line) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); goto done; }
    if (!WriteText(output, TR(L"Reading copy of the current transcript only. Other transcripts and branches, thinking, tool results, metadata and side chains are excluded. Tool calls are summaries. Records over 8 MiB are omitted and counted below.\r\n\r\n"))) goto done;
    for (;;) {
        if (!ReadFile(input, block, sizeof block, &bytes, NULL)) goto done;
        if (!bytes) break;
        total += bytes;
        if (total > TOOLS_FILE_MAX) { SetLastError(ERROR_FILE_TOO_LARGE); goto done; }
        for (i = 0; i < bytes; i++) {
            if (block[i] == '\n') {
                if (skipping) (*omitted)++;
                else if (used && !ExportLine(output, line, used, markdown, messages, omitted)) goto done;
                used = 0;
                skipping = FALSE;
            } else if (!skipping) {
                if (used < TOOLS_LINE_MAX) line[used++] = (char)block[i];
                else { skipping = TRUE; used = 0; }
            }
        }
    }
    if (skipping) (*omitted)++;
    else if (used && !ExportLine(output, line, used, markdown, messages, omitted)) goto done;
    {
        WCHAR footer[256];
        StringCchPrintfW(footer, ARRAYSIZE(footer), TR(L"\r\nExported messages: %lu. Omitted records: %lu.\r\n"), *messages, *omitted);
        if (!WriteText(output, footer) || !FlushFileBuffers(output)) goto done;
    }
    if (!GetFileInformationByHandle(input, &after) || before.nFileSizeLow != after.nFileSizeLow || before.nFileSizeHigh != after.nFileSizeHigh ||
        CompareFileTime(&before.ftLastWriteTime, &after.ftLastWriteTime)) { SetLastError(ERROR_RETRY); goto done; }
    CloseHandle(output);
    output = INVALID_HANDLE_VALUE;
    ok = MoveFileExW(stagePath, outputPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
done:
    if (!ok) { error = GetLastError(); if (!error) error = ERROR_GEN_FAILURE; }
    if (line) HeapFree(GetProcessHeap(), 0, line);
    if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
    if (!ok && stageCreated) DeleteFileW(stagePath);
    SetLastError(error);
    return ok;
}

static void ExportReading(HWND owner, const WCHAR *source)
{
    IFileDialog *dialog = NULL;
    IShellItem *item = NULL;
    PWSTR chosen = NULL;
    COMDLG_FILTERSPEC filters[] = { { TR(L"Markdown (*.md)"), L"*.md" }, { TR(L"Plain text (*.txt)"), L"*.txt" } };
    HRESULT hr;
    UINT type = 1;
    DWORD options, messages, omitted;
    BOOL ok = FALSE;
    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr)) return;
    IFileDialog_SetTitle(dialog, TR(L"Export reading copy"));
    IFileDialog_SetFileTypes(dialog, ARRAYSIZE(filters), filters);
    IFileDialog_SetDefaultExtension(dialog, L"md");
    IFileDialog_SetFileName(dialog, TR(L"Claude conversation.md"));
    if (SUCCEEDED(IFileDialog_GetOptions(dialog, &options))) IFileDialog_SetOptions(dialog, options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_OVERWRITEPROMPT);
    hr = IFileDialog_Show(dialog, owner);
    if (SUCCEEDED(hr) && SUCCEEDED(IFileDialog_GetResult(dialog, &item)) && SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &chosen))) {
        IFileDialog_GetFileTypeIndex(dialog, &type);
        ok = SessionTools_ExportFile(source, chosen, type == 1, &messages, &omitted);
        if (!ok) {
            DWORD error = GetLastError();
            if (error == ERROR_FILE_TOO_LARGE) Ui_Message(owner, MB_ICONWARNING, TR(L"Reading export supports transcripts up to 512 MiB. No existing output was replaced."));
            else Ui_Message(owner, MB_ICONWARNING, TR(L"The reading copy could not be saved (error %lu). No existing output was replaced."), error);
        } else Ui_Message(owner, MB_ICONINFORMATION, TR(L"Reading copy saved. Messages: %lu. Omitted records: %lu. Other transcripts and branches are excluded."), messages, omitted);
    }
    if (chosen) CoTaskMemFree(chosen);
    if (item) IShellItem_Release(item);
    IFileDialog_Release(dialog);
}

void SessionTools_Export(HWND owner, const SessionSet *set, int row)
{
    if (row >= 0 && row < set->rowCount && set->rows[row].transcript) ExportReading(owner, set->rows[row].transcriptPath);
}
