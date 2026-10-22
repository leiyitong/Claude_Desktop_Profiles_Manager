/* Bounded local result history: no conversation text or account credentials. */
#include "operationhistory.h"
#include <string.h>
#define OP_LIMIT 64
#define OP_MAGIC 0x334F4443u
typedef struct OpEntry { FILETIME time; DWORD kind, error; int added, updated, removed, skipped, failed, forked, conflicts; BOOL waiting, unavailable; WCHAR folder[FOLDER_CCH], backup[LONG_PATH_CCH], detail[384]; } OpEntry;
typedef struct OpHistory { DWORD magic, count; OpEntry entries[OP_LIMIT]; } OpHistory;
static BOOL OpPath(WCHAR *out, size_t cch) {
    WCHAR directory[MAX_PATH];
    return Util_StateDir(directory, ARRAYSIZE(directory)) && Util_EnsureDir(directory) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\operation-history.dat", directory));
}
static BOOL OpRead(const WCHAR *path, OpHistory *history) {
    DWORD length = 0, i; char *data = Util_ReadFile(path, sizeof *history, FALSE, &length);
    ZeroMemory(history, sizeof *history); history->magic = OP_MAGIC;
    if (!data) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    if (length != sizeof *history) { HeapFree(GetProcessHeap(), 0, data); return FALSE; }
    CopyMemory(history, data, sizeof *history); HeapFree(GetProcessHeap(), 0, data);
    if (history->magic != OP_MAGIC || history->count > OP_LIMIT) return FALSE;
    for (i = 0; i < history->count; i++)
        if (history->entries[i].kind > OPERATION_SNAPSHOT ||
            wcsnlen_s(history->entries[i].folder, FOLDER_CCH) == FOLDER_CCH ||
            wcsnlen_s(history->entries[i].backup, LONG_PATH_CCH) == LONG_PATH_CCH ||
            wcsnlen_s(history->entries[i].detail, ARRAYSIZE(history->entries[i].detail)) == ARRAYSIZE(history->entries[i].detail)) return FALSE;
    return TRUE;
}
static void OpPush(const OpEntry *entry) {
    WCHAR path[LONG_PATH_CCH], temporary[LONG_PATH_CCH], name[128];
    HANDLE mutex, file; DWORD written; BOOL ok; OpHistory *history;
    if (entry->kind > OPERATION_SNAPSHOT || !OpPath(path, ARRAYSIZE(path)) ||
        FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"Local\\CDPM.OperationHistory.%08lX", Core_HashIgnoringCase(path)))) return;
    mutex = CreateMutexW(NULL, FALSE, name); if (!mutex) return;
    written = WaitForSingleObject(mutex, 5000);
    if (written != WAIT_OBJECT_0 && written != WAIT_ABANDONED) { CloseHandle(mutex); return; }
    history = (OpHistory *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *history);
    if (!history || !OpRead(path, history)) goto done;
    if (history->count < OP_LIMIT) history->count++;
    MoveMemory(history->entries + 1, history->entries, (history->count - 1) * sizeof(OpEntry));
    history->entries[0] = *entry;
    if (FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s.tmp", path))) goto done;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    ok = WriteFile(file, history, sizeof *history, &written, NULL) && written == sizeof *history && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) DeleteFileW(temporary);
done:
    if (history) HeapFree(GetProcessHeap(), 0, history);
    ReleaseMutex(mutex); CloseHandle(mutex);
}
void OperationHistory_RecordDetail(OperationKind kind, const WCHAR *profileFolder, DWORD error, const WCHAR *backupPath, const WCHAR *detail) {
    OpEntry entry; ZeroMemory(&entry, sizeof entry); GetSystemTimeAsFileTime(&entry.time); entry.kind = kind; entry.error = error;
    if (FAILED(StringCchCopyW(entry.folder, FOLDER_CCH, profileFolder ? profileFolder : L"")) ||
        FAILED(StringCchCopyW(entry.backup, LONG_PATH_CCH, backupPath ? backupPath : L""))) return;
    StringCchCopyNW(entry.detail, ARRAYSIZE(entry.detail), detail ? detail : L"", ARRAYSIZE(entry.detail)-1);
    OpPush(&entry);
}
void OperationHistory_Record(OperationKind kind, const WCHAR *profileFolder, DWORD error, const WCHAR *backupPath) {
    OperationHistory_RecordDetail(kind, profileFolder, error, backupPath, NULL);
}
void OperationHistory_RecordReport(OperationKind kind, const ProfileList *profiles, const SyncReport *report) {
    OpEntry entry; int i; (void)profiles; if (!report) return;
    ZeroMemory(&entry, sizeof entry); GetSystemTimeAsFileTime(&entry.time); entry.kind = kind;
    entry.error = report->failed || report->error[0] ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
    entry.added = report->added; entry.updated = report->updated; entry.removed = report->removed; entry.skipped = report->skipped;
    entry.failed = report->failed; entry.forked = report->forked; entry.conflicts = report->conflicts;
    entry.waiting = report->waiting != 0; entry.unavailable = report->unavailable != 0;
    StringCchCopyW(entry.backup, LONG_PATH_CCH, report->backup);
    StringCchCopyNW(entry.detail, ARRAYSIZE(entry.detail), report->error, ARRAYSIZE(entry.detail) - 1); OpPush(&entry);
    for (i = 0; i < report->profileCount && i < MAX_PROFILES; i++) {
        const SyncProfileResult *p = &report->profiles[i]; ZeroMemory(&entry, sizeof entry);
        GetSystemTimeAsFileTime(&entry.time); entry.kind = kind; entry.error = p->failed || p->error[0] ? ERROR_GEN_FAILURE : ERROR_SUCCESS;
        entry.added = p->added; entry.updated = p->updated; entry.removed = p->removed; entry.skipped = p->skipped; entry.failed = p->failed;
        entry.waiting = p->waiting; entry.unavailable = p->unavailable;
        StringCchCopyW(entry.folder, FOLDER_CCH, p->folder); StringCchCopyW(entry.backup, LONG_PATH_CCH, report->backup);
        StringCchCopyNW(entry.detail, ARRAYSIZE(entry.detail), p->error, ARRAYSIZE(entry.detail) - 1); OpPush(&entry);
    }
}
void OperationHistory_Show(HWND owner) {
    WCHAR path[LONG_PATH_CCH], line[2400], stamp[80], *text; OpHistory *history;
    DWORD i; SYSTEMTIME utc, local;
    history = (OpHistory *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *history);
    text = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 128 * 1024 * sizeof(WCHAR));
    if (!history || !text || !OpPath(path, ARRAYSIZE(path))) goto done;
    if (!OpRead(path, history)) { Ui_Message(owner, MB_ICONERROR, TR(L"The operation history could not be read.")); goto done; }
    for (i = 0; i < history->count; i++) {
        OpEntry *entry = &history->entries[i]; const WCHAR *kind = TR(L"Sync");
        if (entry->kind == OPERATION_IMPORT) kind = TR(L"Import");
        else if (entry->kind == OPERATION_RESTORE) kind = TR(L"Restore");
        else if (entry->kind == OPERATION_MIGRATION) kind = TR(L"Manager configuration");
        else if (entry->kind == OPERATION_SNAPSHOT) kind = TR(L"Configuration snapshot");
        StringCchCopyW(stamp, ARRAYSIZE(stamp), TR(L"Unknown"));
        if (FileTimeToSystemTime(&entry->time, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &local))
            StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute, local.wSecond);
        StringCchPrintfW(line, ARRAYSIZE(line), L"%s | %s | %s | %s (%lu)\r\n%s\r\n%s\r\n", stamp, kind,
                         entry->folder[0] ? entry->folder : TR(L"All profiles"), entry->error ? TR(L"Failed") : entry->waiting ? TR(L"Waiting") : TR(L"Completed"), entry->error, entry->detail, entry->backup);
        StringCchCatW(text, 128 * 1024, line);
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Added %d; updated %d; removed %d; skipped %d; failed %d; forked %d; conflicts %d; waiting %d; unavailable %d.\r\n\r\n"), entry->added, entry->updated, entry->removed, entry->skipped, entry->failed, entry->forked, entry->conflicts, entry->waiting, entry->unavailable);
        StringCchCatW(text, 128 * 1024, line);
    }
    if (!history->count) StringCchCopyW(text, 128 * 1024, TR(L"No operations have been recorded yet."));
    Ui_ReviewText(owner, TR(L"Recent operation results"), TR(L"The latest 64 results are kept locally. Backup paths are references, not a promise that recovery will succeed."), text, NULL);
done:
    if (history) HeapFree(GetProcessHeap(), 0, history);
    if (text) HeapFree(GetProcessHeap(), 0, text);
}
