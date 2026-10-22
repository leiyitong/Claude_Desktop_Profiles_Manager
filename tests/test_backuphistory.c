/* Isolated data-layer checks, with no backup archives, dialogs or user state.
 * Link with the application's objects and normal system libraries, as for the
 * other test programs; the included module has distinct public entry points. */
#include "../src/app.h"
#include <objbase.h>
#include <stdio.h>
#define BackupHistory_Record Fixture_BackupHistory_Record
#define BackupHistory_Show Fixture_BackupHistory_Show
#include "../src/backuphistory.c"
#undef BackupHistory_Record
#undef BackupHistory_Show

static int failures, checks;
static WCHAR fixture[MAX_PATH];

static void Check(const char *label, BOOL passed)
{
    checks++;
    if (!passed) { failures++; printf("FAIL: %s\n", label); }
}

static BOOL Whole(const WCHAR *path, const char *bytes, DWORD length)
{
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    BOOL ok = file != INVALID_HANDLE_VALUE && WriteFile(file, bytes, length, &written, NULL) && written == length;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    return ok;
}

typedef struct RecordWork { Profile profile; WCHAR path[MAX_PATH]; } RecordWork;

static DWORD WINAPI RecordWorker(void *parameter)
{
    RecordWork *work = (RecordWork *)parameter;
    Fixture_BackupHistory_Record(&work->profile, work->path);
    return 0;
}

int wmain(void)
{
    WCHAR temporary[MAX_PATH], id[40], directory[MAX_PATH], historyPath[LONG_PATH_CCH], blocked[LONG_PATH_CCH], path[MAX_PATH], log[MAX_PATH];
    GUID guid;
    Profile profile;
    History *history = (History *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *history);
    HANDLE locked, threads[2];
    RecordWork work[2];
    DWORD i, bytes;
    char *raw;
    if (!history || !GetTempPathW(ARRAYSIZE(temporary), temporary) || FAILED(CoCreateGuid(&guid)) ||
        !StringFromGUID2(&guid, id, ARRAYSIZE(id)) ||
        FAILED(StringCchPrintfW(fixture, ARRAYSIZE(fixture), L"%sCDPM-history-test-%s", temporary, id)) ||
        !CreateDirectoryW(fixture, NULL)) return 2;
    Util_SetStateDir(fixture);
    ZeroMemory(&profile, sizeof profile);
    StringCchCopyW(profile.name, ARRAYSIZE(profile.name), L"\x6D4B\x8BD5 Work");
    Check("fixture state path", HistoryPaths(directory, ARRAYSIZE(directory), historyPath, ARRAYSIZE(historyPath)) && Core_PathEquals(directory, fixture));
    Check("missing history is empty", ReadHistory(historyPath, history) && !history->count);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\\x5907\x4EFD.zip", fixture);
    Fixture_BackupHistory_Record(&profile, path);
    Check("Unicode names and paths round-trip", ReadHistory(historyPath, history) && history->count == 1 &&
          !wcscmp(history->entries[0].profile, profile.name) && !wcscmp(history->entries[0].path, path));
    StringCchCopyW(profile.name, ARRAYSIZE(profile.name), L"Renamed");
    Fixture_BackupHistory_Record(&profile, path);
    Check("overwriting one location replaces its record", ReadHistory(historyPath, history) && history->count == 1 &&
          !wcscmp(history->entries[0].profile, L"Renamed"));
    for (i = 0; i < 65; i++) {
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\backup-%02lu.zip", fixture, i);
        Fixture_BackupHistory_Record(&profile, path);
    }
    Check("bounded latest locations", ReadHistory(historyPath, history) && history->count == 64 && Core_EndsWithI(history->entries[0].path, L"backup-64.zip") &&
          Core_EndsWithI(history->entries[63].path, L"backup-01.zip"));
    locked = CreateFileW(historyPath, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    Check("history fixture exclusively locked", locked != INVALID_HANDLE_VALUE);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\must-not-replace.zip", fixture);
    Fixture_BackupHistory_Record(&profile, path);
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    Check("unreadable history is never treated as empty", ReadHistory(historyPath, history) && history->count == 64 && Core_EndsWithI(history->entries[0].path, L"backup-64.zip"));
    StringCchPrintfW(blocked, ARRAYSIZE(blocked), L"%s.tmp", historyPath);
    Check("create a temporary-write obstruction", CreateDirectoryW(blocked, NULL));
    Fixture_BackupHistory_Record(&profile, path);
    Check("failed replacement keeps the old history", ReadHistory(historyPath, history) && history->count == 64 && Core_EndsWithI(history->entries[0].path, L"backup-64.zip"));
    RemoveDirectoryW(blocked);
    Check("corrupt history fixture", Whole(historyPath, "broken", 6));
    Fixture_BackupHistory_Record(&profile, path);
    raw = Util_ReadFile(historyPath, 64, FALSE, &bytes);
    Check("malformed history stays byte-for-byte unchanged", raw && bytes == 6 && !memcmp(raw, "broken", 6));
    if (raw) HeapFree(GetProcessHeap(), 0, raw);
    DeleteFileW(historyPath);
    ZeroMemory(work, sizeof work);
    ZeroMemory(threads, sizeof threads);
    for (i = 0; i < ARRAYSIZE(work); i++) {
        work[i].profile = profile;
        StringCchPrintfW(work[i].path, ARRAYSIZE(work[i].path), L"%s\\parallel-%lu.zip", fixture, i);
        threads[i] = CreateThread(NULL, 0, RecordWorker, &work[i], 0, NULL);
        Check("parallel writer started", threads[i] != NULL);
    }
    for (i = 0; i < ARRAYSIZE(threads); i++)
        if (threads[i]) {
            DWORD waited = WaitForSingleObject(threads[i], 15000);
            CloseHandle(threads[i]);
            Check("parallel writer finished", waited == WAIT_OBJECT_0);
            if (waited != WAIT_OBJECT_0) return 2;   /* do not reclaim a fixture still in use */
        }
    Check("parallel writers keep both locations", ReadHistory(historyPath, history) && history->count == 2 &&
          !Core_PathEquals(history->entries[0].path, history->entries[1].path));
    HeapFree(GetProcessHeap(), 0, history);
    /* Only our known fixture files are removed, never a recursively computed path. */
    DeleteFileW(historyPath);
    DeleteFileW(blocked);
    StringCchPrintfW(log, ARRAYSIZE(log), L"%s\\claude-desktop-profiles-manager.log", fixture);
    DeleteFileW(log);
    Util_SetStateDir(NULL);
    RemoveDirectoryW(fixture);
    printf("Backup history: %d checks, %d failures.\n", checks, failures);
    return failures ? 1 : 0;
}
