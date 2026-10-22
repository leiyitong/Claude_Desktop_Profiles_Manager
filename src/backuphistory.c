/* Recent completed profile ZIP saves. Presence and size are checked on demand;
 * neither a save record nor a file on disk proves an archive's integrity. */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <shellapi.h>
#include <string.h>
#include <wchar.h>

#define HISTORY_LIMIT 64
#define HISTORY_MAGIC 0x48424443u
#define HISTORY_VERSION 1u
#define HISTORY_LOCK_MS 5000u

typedef struct HistoryEntry {
    FILETIME saved;
    WCHAR profile[LABEL_CCH];
    WCHAR path[LONG_PATH_CCH];
} HistoryEntry;

typedef struct History {
    DWORD magic, version, count;
    HistoryEntry entries[HISTORY_LIMIT];
} History;

typedef struct HistoryDialog {
    History *history;
    HWND rows;
} HistoryDialog;

static BOOL HistoryPaths(WCHAR *directory, size_t directoryCch, WCHAR *path, size_t pathCch)
{
    if (Util_StateDir(directory, directoryCch) &&
        SUCCEEDED(StringCchPrintfW(path, pathCch, L"%s\\backup-history.dat", directory))) return TRUE;
    SetLastError(ERROR_FILENAME_EXCED_RANGE);
    return FALSE;
}

/* Only absolute disk paths: a corrupted history must never turn Open folder
 * into a request to execute a URL or a relative program. */
static BOOL HistoryDiskPath(const WCHAR *path)
{
    size_t i, n = wcsnlen_s(path, LONG_PATH_CCH);
    if (!n || n == LONG_PATH_CCH || wcschr(path, L'"') || wcschr(path, L'/')) return FALSE;
    if (!(n >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' && path[2] == L'\\') &&
        !(n > 4 && path[0] == L'\\' && path[1] == L'\\')) return FALSE;
    for (i = 0; i < n; i++)
        if (path[i] < 0x20 || (path[i] >= 0x7f && path[i] <= 0x9f)) return FALSE;
    return TRUE;
}

static DWORD HistoryBytes(DWORD count)
{
    return (DWORD)(FIELD_OFFSET(History, entries) + count * sizeof(HistoryEntry));
}

/* A missing file alone is an empty history. An unreadable or malformed file
 * stays untouched, including when Record is called after a successful backup. */
static BOOL ReadHistory(const WCHAR *path, History *history)
{
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read = 0, error = ERROR_SUCCESS, i;
    BOOL ok;
    ZeroMemory(history, sizeof *history);
    history->magic = HISTORY_MAGIC;
    history->version = HISTORY_VERSION;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    ok = GetFileSizeEx(file, &size);
    if (!ok) error = GetLastError();
    else if (size.QuadPart < HistoryBytes(0) || size.QuadPart > (LONGLONG)sizeof *history) ok = FALSE, error = ERROR_INVALID_DATA;
    else if (!ReadFile(file, history, (DWORD)size.QuadPart, &read, NULL)) ok = FALSE, error = GetLastError();
    else if (read != (DWORD)size.QuadPart || history->magic != HISTORY_MAGIC || history->version != HISTORY_VERSION ||
             history->count > HISTORY_LIMIT || read != HistoryBytes(history->count)) ok = FALSE, error = ERROR_INVALID_DATA;
    for (i = 0; ok && i < history->count; i++)
        if (wcsnlen_s(history->entries[i].profile, LABEL_CCH) == LABEL_CCH || !HistoryDiskPath(history->entries[i].path))
            ok = FALSE, error = ERROR_INVALID_DATA;
    CloseHandle(file);
    if (!ok) SetLastError(error);
    return ok;
}

static HANDLE HistoryLock(const WCHAR *directory)
{
    WCHAR name[100];
    HANDLE mutex;
    DWORD waited, error;
    if (FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"Local\\ClaudeDesktopProfilesManager.BackupHistory.%08lX", Core_HashIgnoringCase(directory)))) return NULL;
    mutex = CreateMutexW(NULL, FALSE, name);
    if (!mutex) return NULL;
    waited = WaitForSingleObject(mutex, HISTORY_LOCK_MS);
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) return mutex;
    error = waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    CloseHandle(mutex);
    SetLastError(error);
    return NULL;
}

static BOOL WriteHistory(const WCHAR *path, const History *history)
{
    WCHAR temporary[LONG_PATH_CCH];
    HANDLE file;
    DWORD written = 0, bytes = HistoryBytes(history->count), error;
    BOOL ok;
    if (FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s.tmp", path))) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, history, bytes, &written, NULL) && written == bytes && FlushFileBuffers(file);
    error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (ok && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) ok = FALSE, error = GetLastError();
    if (!ok) {
        DeleteFileW(temporary);
        SetLastError(error ? error : ERROR_WRITE_FAULT);
    }
    return ok;
}

void BackupHistory_Record(const Profile *profile, const WCHAR *path)
{
    WCHAR directory[MAX_PATH], historyPath[LONG_PATH_CCH];
    History *history;
    HistoryEntry entry;
    HANDLE mutex;
    DWORD i, at;
    if (!profile || !path || !HistoryDiskPath(path) ||
        !HistoryPaths(directory, ARRAYSIZE(directory), historyPath, ARRAYSIZE(historyPath)) || !Util_EnsureDir(directory)) return;
    mutex = HistoryLock(directory);
    if (!mutex) {
        Util_Log(L"backup history: record lock unavailable (error %lu)", GetLastError());
        return;
    }
    history = (History *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *history);
    if (!history) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    if (!history || !ReadHistory(historyPath, history)) {
        Util_Log(L"backup history: existing history could not be read; kept unchanged (error %lu)", GetLastError());
    } else {
        ZeroMemory(&entry, sizeof entry);
        StringCchCopyW(entry.profile, ARRAYSIZE(entry.profile), profile->name);
        StringCchCopyW(entry.path, ARRAYSIZE(entry.path), path);
        GetSystemTimeAsFileTime(&entry.saved);
        /* One location has one current archive; a later overwrite replaces its
         * old record rather than suggesting that two versions remain there. */
        at = 0;
        for (i = 0; i < history->count; i++)
            if (!Core_PathEquals(history->entries[i].path, path)) history->entries[at++] = history->entries[i];
        at = min(at, HISTORY_LIMIT - 1);
        memmove(&history->entries[1], &history->entries[0], at * sizeof(HistoryEntry));
        history->count = at + 1;
        history->entries[0] = entry;
        if (!WriteHistory(historyPath, history)) Util_Log(L"backup history: record could not be saved (error %lu)", GetLastError());
    }
    if (history) HeapFree(GetProcessHeap(), 0, history);
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}

static void HistoryTime(const FILETIME *saved, WCHAR *out, size_t cch)
{
    FILETIME local;
    SYSTEMTIME when;
    if (FileTimeToLocalFileTime(saved, &local) && FileTimeToSystemTime(&local, &when))
        StringCchPrintfW(out, cch, L"%04u-%02u-%02u %02u:%02u", when.wYear, when.wMonth, when.wDay, when.wHour, when.wMinute);
    else StringCchCopyW(out, cch, L"\x2014");
}

static void HistoryFile(const WCHAR *path, WCHAR *size, size_t sizeCch, WCHAR *status, size_t statusCch)
{
    WCHAR extended[LONG_PATH_CCH];
    WIN32_FILE_ATTRIBUTE_DATA data;
    DWORD error;
    StringCchCopyW(size, sizeCch, L"\x2014");
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) {
        StringCchCopyW(status, statusCch, TR(L"Unavailable"));
        return;
    }
    if (GetFileAttributesExW(extended, GetFileExInfoStandard, &data)) {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            ULONGLONG bytes = ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            StringCchPrintfW(size, sizeCch, TR(L"%I64u bytes"), bytes);
            StringCchCopyW(status, statusCch, TR(L"Available"));
            return;
        }
        error = ERROR_DIRECTORY;
    } else error = GetLastError();
    StringCchCopyW(status, statusCch, error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? TR(L"Missing") : TR(L"Unavailable"));
}

static void HistorySelection(HWND dialog, const HistoryDialog *state)
{
    int selected = ListView_GetNextItem(state->rows, -1, LVNI_SELECTED);
    EnableWindow(GetDlgItem(dialog, IDC_HISTORY_OPEN), state->history && selected >= 0 && (DWORD)selected < state->history->count);
    EnableWindow(GetDlgItem(dialog, IDC_HISTORY_VALIDATE), state->history && selected >= 0 && (DWORD)selected < state->history->count);
}

static void RefreshHistory(HWND dialog, HistoryDialog *state)
{
    WCHAR directory[MAX_PATH], path[LONG_PATH_CCH];
    History *next = (History *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *next);
    DWORD i;
    if (!next || !HistoryPaths(directory, ARRAYSIZE(directory), path, ARRAYSIZE(path)) || !ReadHistory(path, next)) {
        if (next) HeapFree(GetProcessHeap(), 0, next);
        SetDlgItemTextW(dialog, IDC_HISTORY_NOTE, TR(L"The backup history could not be read. Existing records have been kept."));
        return;
    }
    SendMessageW(state->rows, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state->rows);
    if (state->history) HeapFree(GetProcessHeap(), 0, state->history);
    state->history = next;
    for (i = 0; i < next->count; i++) {
        WCHAR time[40], size[64], status[64];
        LVITEMW item;
        HistoryTime(&next->entries[i].saved, time, ARRAYSIZE(time));
        HistoryFile(next->entries[i].path, size, ARRAYSIZE(size), status, ARRAYSIZE(status));
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.pszText = time;
        ListView_InsertItem(state->rows, &item);
        ListView_SetItemText(state->rows, (int)i, 1, next->entries[i].profile);
        ListView_SetItemText(state->rows, (int)i, 2, (LPWSTR)TR(L"Profile ZIP"));
        ListView_SetItemText(state->rows, (int)i, 3, size);
        ListView_SetItemText(state->rows, (int)i, 4, status);
        ListView_SetItemText(state->rows, (int)i, 5, next->entries[i].path);
    }
    SendMessageW(state->rows, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->rows, NULL, TRUE);
    SetDlgItemTextW(dialog, IDC_HISTORY_NOTE, TR(L"The latest 64 profile ZIP locations saved by this version. Available means the file exists; archive contents have not been checked."));
    HistorySelection(dialog, state);
}

static void OpenHistoryFolder(HWND dialog, const HistoryDialog *state)
{
    WCHAR folder[LONG_PATH_CCH], *slash;
    int selected = ListView_GetNextItem(state->rows, -1, LVNI_SELECTED);
    if (!state->history || selected < 0 || (DWORD)selected >= state->history->count) return;
    StringCchCopyW(folder, ARRAYSIZE(folder), state->history->entries[selected].path);
    slash = wcsrchr(folder, L'\\');
    if (!slash) return;
    if (slash == folder + 2 && folder[1] == L':') slash[1] = 0;
    else *slash = 0;
    if (!HistoryDiskPath(folder) || !Util_DirExists(folder) || (INT_PTR)ShellExecuteW(dialog, L"open", folder, NULL, NULL, SW_SHOWNORMAL) <= 32)
        Ui_Message(dialog, MB_ICONWARNING, TR(L"The backup folder could not be opened. It may have been moved or removed."));
}

static INT_PTR CALLBACK HistoryProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    HistoryDialog *state = (HistoryDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        const WCHAR *titles[] = { TR(L"Saved"), TR(L"Profile"), TR(L"Type"), TR(L"Size"), TR(L"Status"), TR(L"Path") };
        static const int widths[] = { 124, 110, 82, 110, 92, 390 };
        LVCOLUMNW column;
        size_t i;
        state = (HistoryDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        state->rows = GetDlgItem(dialog, IDC_HISTORY_LIST);
        ListView_SetExtendedListViewStyle(state->rows, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        for (i = 0; i < ARRAYSIZE(titles); i++) {
            ZeroMemory(&column, sizeof column);
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = (LPWSTR)titles[i];
            column.cx = MulDiv(widths[i], (int)GetDpiForWindow(dialog), 96);
            ListView_InsertColumn(state->rows, (int)i, &column);
        }
        Theme_SmoothView(state->rows);
        RefreshHistory(dialog, state);
        return TRUE;
    }
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_HISTORY_REFRESH: RefreshHistory(dialog, state); return TRUE;
        case IDC_HISTORY_OPEN: OpenHistoryFolder(dialog, state); return TRUE;
        case IDC_HISTORY_VALIDATE: {
            int selected = ListView_GetNextItem(state->rows, -1, LVNI_SELECTED);
            if (state->history && selected >= 0 && (DWORD)selected < state->history->count)
                Backup_ValidateArchive(dialog, state->history->entries[selected].path);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (state && ((const NMHDR *)lp)->hwndFrom == state->rows) {
            if (((const NMHDR *)lp)->code == LVN_ITEMCHANGED) HistorySelection(dialog, state);
            else if (((const NMHDR *)lp)->code == NM_DBLCLK) OpenHistoryFolder(dialog, state);
        }
        break;
    }
    return FALSE;
}

void BackupHistory_Show(HWND owner)
{
    HistoryDialog state;
    ZeroMemory(&state, sizeof state);
    Ui_Dialog(owner, IDD_HISTORY, HistoryProc, (LPARAM)&state);
    if (state.history) HeapFree(GetProcessHeap(), 0, state.history);
}
