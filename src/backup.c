/*
 * A profile backed up to one compressed archive (zip.c), and restored from
 * it into a profile, the same or another one, a part at a time:
 *
 *   sessions  the Code sessions it lists, as a session archive of
 *             sessionsync.c (entries and conversations), and its scheduled
 *             tasks; restored through that archive's import, into the
 *             account the profile is signed in to
 *   cowork    local-agent-mode-sessions (Cowork's sessions; not its VM)
 *   settings  claude_desktop_config.json (MCP servers, preferences), window
 *             and developer settings, and the language and theme of
 *             config.json
 *   signin    what keeps it signed in: cookies and the web storage of the
 *             app, Local State (whose key, DPAPI-protected, opens them only
 *             on this PC for this Windows user) and config.json; whole
 *             folders replaced, so no database mixes old and new files
 *
 * Caches, logs, the Claude Code and Cowork programs Claude downloads again
 * and Cowork's VM image are never in it. A restore needs the profile closed
 * (and every profile sharing its session folder), and keeps what it replaces
 * in backups\<time>\<folder>-restore of the state folder.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdarg.h>
#include <string.h>

#define BACKUP_FORMAT     "claude-desktop-profiles-manager-backup"
#define BACKUP_VERSION    1
#define MANIFEST_NAME     "manifest.json"
#define SESSIONS_ARCHIVE  "sessions/archive.zip"
#define SCHEDULED_TASKS   L"scheduled-tasks.json"
#define COWORK_DIR        L"local-agent-mode-sessions"
#define APPEARANCE_NAME   "settings/appearance.json"   /* config.json's language and theme only */
#define MANIFEST_MAX      (64u * 1024u)
#define JSON_MAX          (16u * 1024u * 1024u)
#define BACKUPS_DIR       L"backups"
#define ARCHIVE_EXTENSION L"zip"

typedef enum BackupPart { PART_SESSIONS, PART_COWORK, PART_SETTINGS, PART_SIGNIN, PARTS } BackupPart;

static const char *const kPartNames[PARTS] = { "sessions", "cowork", "settings", "signin" };

/* The settings files, whole, and what keeps a profile signed in. */
static const WCHAR *const kSettingsFiles[] = { CLAUDE_DESKTOP_SETTINGS, L"window-state.json", L"developer_settings.json" };
static const WCHAR *const kSigninFiles[] = { L"Local State", L"Preferences", CLAUDE_APP_SETTINGS, L"ant-did", L"ant-device-registry.json",
                                             L"buddy-tokens.json", L"ccd-ids.json" };
static const WCHAR *const kSigninFolders[] = { L"Network", L"Local Storage", L"Session Storage", L"IndexedDB", L"WebStorage" };
/* config.json's members a restore of the settings carries over: none tied to an account. */
static const char *const kAppearanceKeys[] = { "locale", "userThemeMode" };

typedef struct BackupDialog {
    const WCHAR *title, *text, *action;
    BOOL         offered[PARTS], chosen[PARTS];
    int          sessions;            /* the sessions the profile lists, -1: not counted */
    HWND         rows;
    int          rowPart[PARTS], rowCount;
    BOOL         filling;
} BackupDialog;

static void *Alloc(size_t bytes) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes); }
static void Free(void *memory) { if (memory) HeapFree(GetProcessHeap(), 0, memory); }

/* `format` written at `used` in `buffer` (cut short when it is full): where the text ends. */
static size_t AppendA(char *buffer, size_t cch, size_t used, const char *format, ...)
{
    va_list arguments;
    if (used >= cch) return used;
    va_start(arguments, format);
    StringCchVPrintfA(buffer + used, cch - used, format, arguments);
    va_end(arguments);
    return used + strlen(buffer + used);
}

static const WCHAR *PartLabel(BackupPart part, int sessions, WCHAR *text, size_t cch)
{
    switch (part) {
    case PART_SESSIONS:
        if (sessions >= 0) StringCchPrintfW(text, cch, TR(L"Code sessions and their conversations (%d)"), sessions);
        else StringCchCopyW(text, cch, TR(L"Code sessions and their conversations"));
        return text;
    case PART_COWORK:   return TR(L"Cowork sessions");
    case PART_SETTINGS: return TR(L"Settings: MCP servers, preferences, language and theme");
    default:            return TR(L"Sign-in");
    }
}

/* ------------------------------------------------------------ the dialog */

static void ReadChosen(HWND dialog, BackupDialog *state)
{
    BOOL any = FALSE;
    int i;
    for (i = 0; i < PARTS; i++) state->chosen[i] = FALSE;
    for (i = 0; i < state->rowCount; i++)
        if (ListView_GetCheckState(state->rows, i)) any = state->chosen[state->rowPart[i]] = TRUE;
    EnableWindow(GetDlgItem(dialog, IDOK), any);
}

static INT_PTR CALLBACK BackupProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    BackupDialog *state = (BackupDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        LVCOLUMNW column;
        LVITEMW item;
        WCHAR text[256];
        int part;
        state = (BackupDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, state->title);
        SetDlgItemTextW(dialog, IDC_B_TEXT, state->text);
        SetDlgItemTextW(dialog, IDOK, state->action);
        if (!state->offered[PART_SIGNIN]) ShowWindow(GetDlgItem(dialog, IDC_B_NOTE), SW_HIDE);   /* its warning is about sign-in */
        state->rows = GetDlgItem(dialog, IDC_B_LIST);
        ListView_SetExtendedListViewStyle(state->rows, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(state->rows, 0, &column);   /* the view it scrolls in gives it the list's width */
        state->filling = TRUE;
        for (part = 0; part < PARTS; part++) {
            if (!state->offered[part]) continue;
            ZeroMemory(&item, sizeof item);
            item.mask = LVIF_TEXT;
            item.iItem = state->rowCount;
            item.pszText = (LPWSTR)PartLabel((BackupPart)part, state->sessions, text, ARRAYSIZE(text));
            ListView_InsertItem(state->rows, &item);
            ListView_SetCheckState(state->rows, state->rowCount, state->chosen[part]);
            state->rowPart[state->rowCount++] = part;
        }
        state->filling = FALSE;
        Theme_SmoothView(state->rows);
        ReadChosen(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_B_NOTE);

    case WM_NOTIFY:
        if (state && ((const NMHDR *)lp)->hwndFrom == state->rows && ((const NMHDR *)lp)->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            if (!state->filling && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK))
                ReadChosen(dialog, state);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wp) == IDOK) {
            ReadChosen(dialog, state);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* The archive the user chose to write (`save`, named after the profile) or read. */
static BOOL ChooseArchive(HWND owner, BOOL save, const WCHAR *title, const WCHAR *profileName, WCHAR *path, size_t cch)
{
    IFileDialog *dialog = NULL;
    IShellItem *result = NULL;
    COMDLG_FILTERSPEC filter;
    FILEOPENDIALOGOPTIONS options = 0;
    PWSTR chosen = NULL;
    BOOL ok = FALSE;
    HRESULT hr = CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog,
                                  (void **)&dialog);
    if (FAILED(hr)) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The file could not be chosen (error 0x%08lX)."), (unsigned long)hr);
        return FALSE;
    }
    filter.pszName = TR(L"Profile backup (*.zip)");
    filter.pszSpec = L"*." ARCHIVE_EXTENSION;
    IFileDialog_SetTitle(dialog, title);
    IFileDialog_SetFileTypes(dialog, 1, &filter);
    IFileDialog_SetDefaultExtension(dialog, ARCHIVE_EXTENSION);
    if (save) {
        WCHAR name[MAX_PATH];
        SYSTEMTIME now;
        size_t i;
        GetLocalTime(&now);
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s %04u-%02u-%02u." ARCHIVE_EXTENSION, profileName, now.wYear, now.wMonth, now.wDay);
        for (i = 0; name[i]; i++)
            if (wcschr(L"\\/:*?\"<>|", name[i])) name[i] = L'_';
        IFileDialog_SetFileName(dialog, name);
    }
    if (SUCCEEDED(IFileDialog_GetOptions(dialog, &options)))
        IFileDialog_SetOptions(dialog, options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    hr = IFileDialog_Show(dialog, owner);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IFileDialog_GetResult(dialog, &result))) {
        if (SUCCEEDED(hr = IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &chosen))) {
            ok = SUCCEEDED(StringCchCopyW(path, cch, chosen)) &&
                 (!save || Core_EndsWithI(path, L"." ARCHIVE_EXTENSION) || SUCCEEDED(StringCchCatW(path, cch, L"." ARCHIVE_EXTENSION)));
            if (!ok) Ui_Message(owner, MB_ICONERROR, TR(L"The path is too long."));
            CoTaskMemFree(chosen);
        }
        IShellItem_Release(result);
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        Ui_Message(owner, MB_ICONERROR, TR(L"The file could not be chosen (error 0x%08lX)."), (unsigned long)hr);
    IFileDialog_Release(dialog);
    return ok;
}

/* ------------------------------------------------------------- writing */

/* `name` (a path inside the archive, "/" between folders) in UTF-8. */
static BOOL Utf8Name(const WCHAR *name, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, out, cap, NULL, NULL);
    int i;
    if (n <= 0) return FALSE;
    for (i = 0; out[i]; i++)
        if (out[i] == '\\') out[i] = '/';
    return TRUE;
}

/* Every file of `dir` (links not followed) under `prefix` in the archive. */
static BOOL AddTree(ZipOut *zip, const WCHAR *dir, const WCHAR *prefix)
{
    WCHAR path[LONG_PATH_CCH], name[LONG_PATH_CCH];
    char utf8[3 * LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(dir, L"*", &found, FALSE);
    BOOL ok = TRUE;
    if (find == INVALID_HANDLE_VALUE) return TRUE;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0 || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName)) ||
            FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s/%s", prefix, found.cFileName)))
            continue;
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = AddTree(zip, path, name) && ok;
        else if (Utf8Name(name, utf8, sizeof utf8)) ok = Zip_AddFile(zip, utf8, path) && ok;
    } while (ok && FindNextFileW(find, &found));
    FindClose(find);
    return ok;
}

/* `storage`\`file` under `prefix`/`file`, when it is there. */
static BOOL AddIfThere(ZipOut *zip, const WCHAR *storage, const WCHAR *file, const WCHAR *prefix)
{
    WCHAR path[LONG_PATH_CCH], name[MAX_PATH];
    char utf8[3 * MAX_PATH];
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", storage, file)) || !Util_FileExists(path)) return TRUE;
    return SUCCEEDED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s/%s", prefix, file)) && Utf8Name(name, utf8, sizeof utf8) &&
           Zip_AddFile(zip, utf8, path);
}

/* The language and theme of config.json, as a JSON object of their own. */
static BOOL AddAppearance(ZipOut *zip, const WCHAR *storage)
{
    WCHAR path[LONG_PATH_CCH];
    char out[1024];
    size_t used = 0, i;
    DWORD length = 0;
    char *json;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_APP_SETTINGS, storage)) ||
        (json = Util_ReadFile(path, JSON_MAX, FALSE, &length)) == NULL)
        return TRUE;
    out[used++] = '{';
    for (i = 0; i < ARRAYSIZE(kAppearanceKeys); i++) {
        const char *value;
        size_t valueLength;
        if (!Core_JsonMember(json, length, kAppearanceKeys[i], &value, &valueLength) || used + strlen(kAppearanceKeys[i]) + valueLength + 8 > sizeof out)
            continue;
        if (used > 1) out[used++] = ',';
        used = AppendA(out, sizeof out, used, "\"%s\":", kAppearanceKeys[i]);
        memcpy(out + used, value, valueLength);
        used += valueLength;
    }
    out[used++] = '}';
    HeapFree(GetProcessHeap(), 0, json);
    return Zip_AddData(zip, APPEARANCE_NAME, out, used);
}

/* Every session the profile lists, as a session archive in the backup. */
static BOOL AddSessions(ZipOut *zip, const ProfileList *list, int index, int *count, WCHAR *error, size_t errorCch)
{
    WCHAR temporary[MAX_PATH], dir[LONG_PATH_CCH], tasks[LONG_PATH_CCH];
    SessionSet set;
    int *rows, r, n = 0;
    BOOL ok;
    *count = 0;
    if (!SessionStore_LoadProfiles(&set, list)) {
        StringCchCopyW(error, errorCch, TR(L"Sessions could not be loaded."));
        return FALSE;
    }
    rows = (int *)Alloc((size_t)max(set.rowCount, 1) * sizeof *rows);
    if (!rows) {
        SessionStore_Free(&set);
        return FALSE;
    }
    for (r = 0; r < set.rowCount; r++)
        if (set.rows[r].entry[index] >= 0 && !set.entries[set.rows[r].entry[index]].pendingRemove) rows[n++] = r;
    ok = TRUE;
    if (n) {
        WCHAR folder[MAX_PATH];
        ok = GetTempPathW(ARRAYSIZE(folder), folder) && GetTempFileNameW(folder, L"cdm", 0, temporary) &&
             SessionSync_Export(&set, index, rows, n, temporary, count, error, errorCch) && Zip_AddFile(zip, SESSIONS_ARCHIVE, temporary);
        DeleteFileW(temporary);
    }
    if (ok && SessionStore_EntriesDir(&list->items[index], dir, ARRAYSIZE(dir), NULL) &&
        SUCCEEDED(StringCchPrintfW(tasks, ARRAYSIZE(tasks), L"%s\\" SCHEDULED_TASKS, dir)) && Util_FileExists(tasks))
        ok = Zip_AddFile(zip, "sessions/" "scheduled-tasks.json", tasks);
    Free(rows);
    SessionStore_Free(&set);
    return ok;
}

static int CountSessions(const ProfileList *list, int index)
{
    SessionSet set;
    int r, n = 0;
    if (!SessionStore_LoadProfiles(&set, list)) return -1;
    for (r = 0; r < set.rowCount; r++)
        if (set.rows[r].entry[index] >= 0 && !set.entries[set.rows[r].entry[index]].pendingRemove) n++;
    SessionStore_Free(&set);
    return n;
}

BOOL Backup_Create(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index)
{
    const Profile *p = &list->items[index];
    WCHAR title[LABEL_CCH + 64], path[LONG_PATH_CCH], error[LONG_PATH_CCH + 256], text[LONG_PATH_CCH + 512], cowork[LONG_PATH_CCH];
    char manifest[1024], quoted[3 * LABEL_CCH + 8];
    BackupDialog dialog;
    ZipOut *zip;
    HCURSOR old;
    DWORD code = 0;
    int part, sessions = 0;
    size_t used;
    BOOL ok = TRUE;
    (void)pkg;
    if (!p->storageDir[0]) return FALSE;
    StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Back up \x201C%s\x201D"), p->name);
    ZeroMemory(&dialog, sizeof dialog);
    dialog.title = title;
    dialog.text = TR(L"Choose what the backup holds:");
    dialog.action = TR(L"Back up");
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    dialog.sessions = CountSessions(list, index);
    SetCursor(old);
    StringCchPrintfW(cowork, ARRAYSIZE(cowork), L"%s\\" COWORK_DIR, p->storageDir);
    dialog.offered[PART_SESSIONS] = dialog.chosen[PART_SESSIONS] = dialog.sessions != 0;
    dialog.offered[PART_COWORK] = dialog.chosen[PART_COWORK] = Util_DirExists(cowork);
    dialog.offered[PART_SETTINGS] = dialog.chosen[PART_SETTINGS] = TRUE;
    dialog.offered[PART_SIGNIN] = TRUE;   /* left unchecked: the user asks for it */
    if (Ui_Dialog(owner, IDD_BACKUP, BackupProc, (LPARAM)&dialog) != IDOK) return FALSE;
    if (dialog.chosen[PART_SIGNIN] && Claude_IsRunning(p)) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first: its sign-in files are in use."), p->name);
        return FALSE;
    }
    if (!ChooseArchive(owner, TRUE, title, p->name, path, ARRAYSIZE(path))) return FALSE;
    if ((zip = Zip_Create(path, &code)) == NULL) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The backup could not be written (error %lu)."), code);
        return FALSE;
    }
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    error[0] = 0;
    if (!Core_JsonQuote(p->name, quoted, sizeof quoted)) StringCchCopyA(quoted, ARRAYSIZE(quoted), "\"\"");
    used = AppendA(manifest, sizeof manifest, 0, "{\"format\":\"" BACKUP_FORMAT "\",\"version\":%d,\"profile\":%s,\"parts\":[", BACKUP_VERSION, quoted);
    for (part = 0; part < PARTS; part++) {
        if (!dialog.chosen[part]) continue;
        used = AppendA(manifest, sizeof manifest, used, "%s\"%s\"", manifest[used - 1] == '[' ? "" : ",", kPartNames[part]);
    }
    used = AppendA(manifest, sizeof manifest, used, "]}");
    ok = Zip_AddData(zip, MANIFEST_NAME, manifest, used);
    if (ok && dialog.chosen[PART_SESSIONS]) ok = AddSessions(zip, list, index, &sessions, error, ARRAYSIZE(error));
    if (ok && dialog.chosen[PART_COWORK]) ok = AddTree(zip, cowork, L"cowork");
    if (ok && dialog.chosen[PART_SETTINGS]) {
        size_t i;
        for (i = 0; ok && i < ARRAYSIZE(kSettingsFiles); i++) ok = AddIfThere(zip, p->storageDir, kSettingsFiles[i], L"settings");
        ok = ok && AddAppearance(zip, p->storageDir);
    }
    if (ok && dialog.chosen[PART_SIGNIN]) {
        size_t i;
        WCHAR dir[LONG_PATH_CCH], prefix[MAX_PATH];
        for (i = 0; ok && i < ARRAYSIZE(kSigninFiles); i++) ok = AddIfThere(zip, p->storageDir, kSigninFiles[i], L"signin");
        for (i = 0; ok && i < ARRAYSIZE(kSigninFolders); i++) {
            if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", p->storageDir, kSigninFolders[i])) || !Util_DirExists(dir)) continue;
            StringCchPrintfW(prefix, ARRAYSIZE(prefix), L"signin/%s", kSigninFolders[i]);
            ok = AddTree(zip, dir, prefix);
        }
    }
    code = Zip_Error(zip);
    ok = Zip_Close(zip, ok) && ok;
    SetCursor(old);
    if (!ok) {
        if (error[0]) Ui_Message(owner, MB_ICONERROR, L"%s", error);
        else Ui_Message(owner, MB_ICONERROR, TR(L"The backup could not be written (error %lu)."), code ? code : GetLastError());
        return FALSE;
    }
    Util_Log(L"backed up %s to %s", p->folder, path);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Backup saved:\n%s"), path);
    Ui_Message(owner, MB_ICONINFORMATION, L"%s", text);
    return TRUE;
}

/* ------------------------------------------------------------ restoring */

/* A path inside the archive that stays inside the folder it is restored to. */
static BOOL SafeRelative(const char *name, const char *prefix, WCHAR *out, size_t cch)
{
    size_t length = strlen(prefix);
    const char *rest;
    int n;
    size_t i;
    if (strncmp(name, prefix, length) != 0 || name[length] != '/') return FALSE;
    rest = name + length + 1;
    if (!*rest || strstr(rest, "..") || strchr(rest, ':') || rest[0] == '/' || rest[0] == '\\') return FALSE;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rest, -1, out, (int)cch);
    if (n <= 0) return FALSE;
    for (i = 0; out[i]; i++) {
        if (out[i] == L'/') out[i] = L'\\';
        if (out[i] < 0x20 || wcschr(L"*?\"<>|", out[i])) return FALSE;
    }
    return TRUE;
}

/* Every item under `prefix` extracted into `dir`, folders made. */
static BOOL ExtractTree(const ZipIn *zip, const char *prefix, const WCHAR *dir, DWORD *code)
{
    WCHAR relative[LONG_PATH_CCH], path[LONG_PATH_CCH], parent[LONG_PATH_CCH];
    int i;
    BOOL ok = TRUE;
    for (i = 0; i < Zip_Count(zip); i++) {
        WCHAR *slash;
        if (!SafeRelative(Zip_Name(zip, i), prefix, relative, ARRAYSIZE(relative))) continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, relative)) || FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), path))) {
            ok = FALSE;
            continue;
        }
        if ((slash = wcsrchr(parent, L'\\')) != NULL) *slash = 0;
        if (!Util_EnsureDir(parent) || !Zip_Extract(zip, i, path)) {
            *code = GetLastError();
            ok = FALSE;
        }
    }
    return ok;
}

/* A file or folder of the profile, copied into the backup of a restore. */
static BOOL KeepCopy(const WCHAR *storage, const WCHAR *item, const WCHAR *backup, BOOL folder, DWORD *code)
{
    WCHAR from[LONG_PATH_CCH], to[LONG_PATH_CCH], extendedFrom[LONG_PATH_CCH], extendedTo[LONG_PATH_CCH];
    if (FAILED(StringCchPrintfW(from, ARRAYSIZE(from), L"%s\\%s", storage, item)) || FAILED(StringCchPrintfW(to, ARRAYSIZE(to), L"%s\\%s", backup, item)) ||
        !Util_ExtendedPath(from, extendedFrom, ARRAYSIZE(extendedFrom)) || !Util_ExtendedPath(to, extendedTo, ARRAYSIZE(extendedTo))) {
        *code = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    if (folder ? !Util_DirExists(from) : !Util_FileExists(from)) return TRUE;
    if (folder) return Util_CopyTree(from, to, TRUE, code);
    if (Util_EnsureDir(backup) && CopyFileW(extendedFrom, extendedTo, FALSE)) return TRUE;
    *code = GetLastError();
    return FALSE;
}

/* A folder of the profile gone (its copy is in the backup), so that a
 * restored one holds only the restored files. */
static BOOL RemoveFolder(const WCHAR *storage, const WCHAR *item, DWORD *code)
{
    WCHAR folder[LONG_PATH_CCH];
    if (FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", storage, item))) {
        *code = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    return Util_DeleteTree(folder, code);
}

static BOOL RestoreBackupDir(const Profile *p, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    SYSTEMTIME now;
    GetLocalTime(&now);
    return Util_StateDir(state, ARRAYSIZE(state)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" BACKUPS_DIR L"\\%04u%02u%02u-%02u%02u%02u\\%s-restore", state, now.wYear, now.wMonth,
                                      now.wDay, now.wHour, now.wMinute, now.wSecond, p->folder)) &&
           Util_EnsureDir(out);
}

/* config.json given the language and theme of the backup's appearance. */
static BOOL RestoreAppearance(const ZipIn *zip, const WCHAR *storage)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0, appearanceLength = 0;
    int item = Zip_Find(zip, APPEARANCE_NAME);
    char *json, *appearance, *out = NULL;
    size_t i, outLength = 0;
    BOOL ok = TRUE;
    if (item < 0 || (appearance = (char *)Zip_Read(zip, item, MANIFEST_MAX, &appearanceLength)) == NULL) return TRUE;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_APP_SETTINGS, storage))) {
        Free(appearance);
        return FALSE;
    }
    json = Util_ReadFile(path, JSON_MAX, FALSE, &length);
    if (!json) {
        /* No config.json yet: the appearance becomes it. */
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD done = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, appearance, appearanceLength, &done, NULL) && done == appearanceLength;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Free(appearance);
        return ok;
    }
    for (i = 0; ok && i < ARRAYSIZE(kAppearanceKeys); i++) {
        const char *value;
        size_t valueLength, cap;
        char raw[512], *next;
        if (!Core_JsonMember(appearance, appearanceLength, kAppearanceKeys[i], &value, &valueLength) || valueLength >= sizeof raw) continue;
        memcpy(raw, value, valueLength);
        raw[valueLength] = 0;
        cap = (out ? outLength : length) + valueLength + 64;
        if ((next = (char *)Alloc(cap)) == NULL) {
            ok = FALSE;
            break;
        }
        ok = Core_JsonSetMember(out ? out : json, out ? outLength : length, kAppearanceKeys[i], raw, next, cap, &outLength);
        Free(out);
        out = next;
    }
    if (ok && out) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD done = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, out, (DWORD)outLength, &done, NULL) && done == outLength;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    Free(out);
    Free(json);
    Free(appearance);
    return ok;
}

/* The backup's Code sessions sent to the profile through their archive's import. */
static BOOL RestoreSessions(const ZipIn *zip, const ProfileList *list, int index, int *sessions, BOOL *signedOut, WCHAR *error, size_t errorCch)
{
    WCHAR folder[MAX_PATH], temporary[MAX_PATH], dir[LONG_PATH_CCH], tasks[LONG_PATH_CCH];
    SessionSet set;
    SyncReport report;
    int item = Zip_Find(zip, SESSIONS_ARCHIVE), taskItem = Zip_Find(zip, "sessions/" "scheduled-tasks.json");
    BOOL ok = TRUE, signedIn = FALSE;
    *sessions = 0;
    *signedOut = !SessionStore_EntriesDir(&list->items[index], dir, ARRAYSIZE(dir), &signedIn);
    if (*signedOut) {
        *signedOut = TRUE;
        return TRUE;
    }
    if (item >= 0) {
        ok = GetTempPathW(ARRAYSIZE(folder), folder) && GetTempFileNameW(folder, L"cdm", 0, temporary) && Zip_Extract(zip, item, temporary);
        if (ok && SessionStore_LoadProfiles(&set, list)) {
            ZeroMemory(&report, sizeof report);
            ok = SessionSync_Import(&set, temporary, 1u << index, sessions, &report);
            if (!ok) StringCchCopyW(error, errorCch, report.error);
            SessionStore_Free(&set);
        } else if (ok) {
            StringCchCopyW(error, errorCch, TR(L"Sessions could not be loaded."));
            ok = FALSE;
        }
        DeleteFileW(temporary);
    }
    if (ok && taskItem >= 0 && SUCCEEDED(StringCchPrintfW(tasks, ARRAYSIZE(tasks), L"%s\\" SCHEDULED_TASKS, dir)))
        ok = Zip_Extract(zip, taskItem, tasks);
    return ok;
}

BOOL Backup_Restore(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index)
{
    const Profile *p = &list->items[index];
    WCHAR title[LABEL_CCH + 64], text[LABEL_CCH + 256], path[LONG_PATH_CCH], backup[LONG_PATH_CCH], error[LONG_PATH_CCH + 256];
    WCHAR report[4 * LONG_PATH_CCH], line[LONG_PATH_CCH + 256];
    BackupDialog dialog;
    const char *value;
    size_t valueLength, i;
    DWORD length = 0, code = 0;
    char *manifest;
    ZipIn *zip;
    HCURSOR old;
    int part, item, sessions = 0;
    BOOL ok = TRUE, signedOut = FALSE;
    (void)pkg;
    if (!p->storageDir[0]) return FALSE;
    if (SessionLink_Busy(p)) {   /* said before the choices, checked again after them */
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
        return FALSE;
    }
    StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Restore \x201C%s\x201D from a backup"), p->name);
    if (!ChooseArchive(owner, FALSE, title, p->name, path, ARRAYSIZE(path))) return FALSE;
    zip = Zip_Open(path);
    item = zip ? Zip_Find(zip, MANIFEST_NAME) : -1;
    manifest = item >= 0 ? (char *)Zip_Read(zip, item, MANIFEST_MAX, &length) : NULL;
    if (!manifest || !Core_JsonMember(manifest, length, "format", &value, &valueLength) || valueLength != sizeof(BACKUP_FORMAT) + 1 ||
        memcmp(value + 1, BACKUP_FORMAT, sizeof(BACKUP_FORMAT) - 1) != 0) {
        Free(manifest);
        Zip_Free(zip);
        Ui_Message(owner, MB_ICONWARNING, TR(L"This file is not a backup of " APP_NAME L"."));
        return FALSE;
    }
    ZeroMemory(&dialog, sizeof dialog);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D. What it replaces is kept in a backup first."), p->name);
    dialog.title = title;
    dialog.text = text;
    dialog.action = TR(L"Restore");
    dialog.sessions = -1;
    if (Core_JsonMember(manifest, length, "parts", &value, &valueLength))
        for (part = 0; part < PARTS; part++) {
            char needle[32];
            StringCchPrintfA(needle, ARRAYSIZE(needle), "\"%s\"", kPartNames[part]);
            for (i = 0; i + strlen(needle) <= valueLength; i++)
                if (memcmp(value + i, needle, strlen(needle)) == 0) dialog.offered[part] = TRUE;
            dialog.chosen[part] = dialog.offered[part] && part != PART_SIGNIN;
        }
    Free(manifest);
    if (Ui_Dialog(owner, IDD_BACKUP, BackupProc, (LPARAM)&dialog) != IDOK) {
        Zip_Free(zip);
        return FALSE;
    }
    if (SessionLink_Busy(p)) {
        Zip_Free(zip);
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
        return FALSE;
    }
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    error[0] = 0;
    ok = RestoreBackupDir(p, backup, ARRAYSIZE(backup));
    /* What the restore replaces, kept first. */
    if (!ok) code = GetLastError();
    if (ok && dialog.chosen[PART_COWORK]) ok = KeepCopy(p->storageDir, COWORK_DIR, backup, TRUE, &code);
    if (ok && dialog.chosen[PART_SETTINGS]) {
        for (i = 0; ok && i < ARRAYSIZE(kSettingsFiles); i++) ok = KeepCopy(p->storageDir, kSettingsFiles[i], backup, FALSE, &code);
        ok = ok && KeepCopy(p->storageDir, CLAUDE_APP_SETTINGS, backup, FALSE, &code);
    }
    if (ok && dialog.chosen[PART_SIGNIN]) {
        for (i = 0; ok && i < ARRAYSIZE(kSigninFiles); i++) ok = KeepCopy(p->storageDir, kSigninFiles[i], backup, FALSE, &code);
        for (i = 0; ok && i < ARRAYSIZE(kSigninFolders); i++) ok = KeepCopy(p->storageDir, kSigninFolders[i], backup, TRUE, &code);
    }
    if (!ok) {
        SetCursor(old);
        Zip_Free(zip);
        Ui_Message(owner, MB_ICONERROR, TR(L"What the restore would replace could not be backed up (error %lu): nothing was changed."), code);
        return FALSE;
    }
    if (dialog.chosen[PART_SESSIONS]) ok = RestoreSessions(zip, list, index, &sessions, &signedOut, error, ARRAYSIZE(error)) && ok;
    if (dialog.chosen[PART_COWORK]) {
        WCHAR cowork[LONG_PATH_CCH];
        ok = SUCCEEDED(StringCchPrintfW(cowork, ARRAYSIZE(cowork), L"%s\\" COWORK_DIR, p->storageDir)) && ExtractTree(zip, "cowork", cowork, &code) && ok;
    }
    if (dialog.chosen[PART_SETTINGS]) {
        for (i = 0; i < ARRAYSIZE(kSettingsFiles); i++) {
            char name[MAX_PATH];
            WCHAR target[LONG_PATH_CCH];
            int found;
            WideCharToMultiByte(CP_UTF8, 0, kSettingsFiles[i], -1, name + 9, sizeof name - 9, NULL, NULL);
            memcpy(name, "settings/", 9);
            if ((found = Zip_Find(zip, name)) < 0 || FAILED(StringCchPrintfW(target, ARRAYSIZE(target), L"%s\\%s", p->storageDir, kSettingsFiles[i])))
                continue;
            if (!Zip_Extract(zip, found, target)) {
                code = GetLastError();
                ok = FALSE;
            }
        }
        ok = RestoreAppearance(zip, p->storageDir) && ok;
    }
    if (dialog.chosen[PART_SIGNIN]) {
        /* Whole folders: a database never mixes the files of two times. */
        for (i = 0; i < ARRAYSIZE(kSigninFolders); i++) ok = RemoveFolder(p->storageDir, kSigninFolders[i], &code) && ok;
        ok = ExtractTree(zip, "signin", p->storageDir, &code) && ok;
    }
    Zip_Free(zip);
    SetCursor(old);
    Util_Log(L"restored %s from %s%s", p->folder, path, ok ? L"" : L" (partly)");
    report[0] = 0;
    if (ok) StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Restored into \x201C%s\x201D."), p->name);
    else if (error[0]) StringCchCopyW(report, ARRAYSIZE(report), error);
    else StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Some files could not be restored (error %lu)."), code);
    if (dialog.chosen[PART_SESSIONS] && !signedOut) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Sessions in the archive: %d"), sessions);
        StringCchCatW(report, ARRAYSIZE(report), L"\n");
        StringCchCatW(report, ARRAYSIZE(report), line);
    }
    if (signedOut) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"The Code sessions were left out: \x201C%s\x201D is not signed in to Claude yet."), p->name);
        StringCchCatW(report, ARRAYSIZE(report), L"\n");
        StringCchCatW(report, ARRAYSIZE(report), line);
    }
    StringCchCatW(report, ARRAYSIZE(report), L"\n");
    StringCchCatW(report, ARRAYSIZE(report), TR(L"What was replaced or removed is kept in:"));
    StringCchCatW(report, ARRAYSIZE(report), L"\n");
    StringCchCatW(report, ARRAYSIZE(report), backup);
    Ui_Message(owner, ok ? MB_ICONINFORMATION : MB_ICONWARNING, L"%s", report);
    return ok;
}
