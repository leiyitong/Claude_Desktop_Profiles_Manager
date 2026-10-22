/* Event-triggered snapshots contain only typed UI geometry and developer flags.
 * Login, MCP configuration, paths and unknown extension fields are excluded. */
#include "profiletools.h"
#include "operationhistory.h"
#include <commctrl.h>
#include <string.h>
#include <stdlib.h>
#define SNAP_MAX 16
#define SNAP_BYTES (1024u * 1024u)
static const WCHAR *const kFiles[] = { L"window-state.json", L"developer_settings.json" };
static const char *const kNames[] = { "window-state.json", "developer_settings.json" };
static const char *const kGeometry[] = { "x", "y", "width", "height", "isMaximized", "isFullScreen" };
static const char *const kFlags[] = { "allowDevTools" };
static LONG g_snapshotBusy, g_snapshotSequence;
static BOOL Join(const WCHAR *a, const WCHAR *b, WCHAR *out, size_t cch) { return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", a, b)); }
static BOOL NumberOrBool(const char *value, size_t n) {
    size_t i = 0;
    if ((n == 4 && !memcmp(value, "true", 4)) || (n == 5 && !memcmp(value, "false", 5))) return TRUE;
    if (!n) return FALSE; if (value[i] == '-') i++; if (i == n) return FALSE;
    for (; i < n; i++) if (value[i] < '0' || value[i] > '9') return FALSE;
    return TRUE;
}
static BOOL IsBoolean(const char *value, size_t n) {
    return (n == 4 && !memcmp(value, "true", 4)) || (n == 5 && !memcmp(value, "false", 5));
}
static BOOL SafeObject(const char *json, size_t length, BOOL developer, char *out, size_t cap) {
    size_t i, n, used = 2; const char *value; const char *const *keys = developer ? kFlags : kGeometry;
    size_t count = developer ? ARRAYSIZE(kFlags) : ARRAYSIZE(kGeometry);
    StringCchCopyA(out, cap, "{}");
    for (i = 0; i < count; i++) if (Core_JsonMember(json, length, keys[i], &value, &n)) {
        char changed[4096], raw[128]; size_t changedLength;
        if (n >= sizeof raw || !NumberOrBool(value, n) || ((developer || i >= 4) ? !IsBoolean(value, n) : IsBoolean(value, n))) return FALSE;
        CopyMemory(raw, value, n); raw[n] = 0;
        if (!Core_JsonSetMember(out, used, keys[i], raw, changed, sizeof changed - 1, &changedLength)) return FALSE;
        changed[changedLength] = 0; if (FAILED(StringCchCopyA(out, cap, changed))) return FALSE;
        used = changedLength;
    }
    if (!developer) {
        static const char *const nested[] = { "displayBounds" };
        for (i = 0; i < ARRAYSIZE(nested); i++) if (Core_JsonMember(json, length, nested[i], &value, &n)) {
            char child[4096], changed[4096]; size_t changedLength;
            if (!n || value[0] != '{') return FALSE;
            /* Geometry children are scalar coordinates, not developer flags. */
            { size_t j, vn, childLength = 2; const char *v; StringCchCopyA(child, sizeof child, "{}");
              for (j = 0; j < 4; j++) if (Core_JsonMember(value, n, kGeometry[j], &v, &vn)) {
                  char raw[128], changedChild[4096]; size_t outLength;
                  if (vn >= sizeof raw || !NumberOrBool(v, vn) || IsBoolean(v, vn)) return FALSE;
                  CopyMemory(raw, v, vn); raw[vn] = 0;
                  if (!Core_JsonSetMember(child, childLength, kGeometry[j], raw, changedChild, sizeof changedChild - 1, &outLength)) return FALSE;
                  changedChild[outLength] = 0; StringCchCopyA(child, sizeof child, changedChild); childLength = outLength;
              }
            }
            if (!Core_JsonSetMember(out, used, nested[i], child, changed, sizeof changed - 1, &changedLength)) return FALSE;
            changed[changedLength] = 0; if (FAILED(StringCchCopyA(out, cap, changed))) return FALSE;
            used = changedLength;
        }
    }
    return TRUE;
}
static BOOL SnapshotDirectoryIn(const Profile *profile, const WCHAR *state, WCHAR *out, size_t cch) {
    size_t i;
    if (!profile || !profile->folder[0] || wcsnlen_s(profile->folder, FOLDER_CCH) == FOLDER_CCH) return FALSE;
    for (i = 0; profile->folder[i]; i++) if (profile->folder[i] < 32 || wcschr(L"\\/:*?\"<>|", profile->folder[i])) return FALSE;
    return state && *state && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\config-snapshots\\%s", state, profile->folder));
}
static BOOL SnapshotDirectory(const Profile *profile, WCHAR *out, size_t cch) {
    WCHAR state[MAX_PATH]; return Util_StateDir(state, ARRAYSIZE(state)) && SnapshotDirectoryIn(profile, state, out, cch);
}
typedef struct SnapshotNames { int count; WCHAR names[SNAP_MAX + 32][128]; } SnapshotNames;
static int __cdecl NameOrder(const void *a, const void *b) { return -wcscmp((const WCHAR *)a, (const WCHAR *)b); }
static BOOL SnapshotName(const WCHAR *name) {
    size_t i; if (wcslen(name) != 41 || name[8] != L'-' || name[15] != L'-' || name[19] != L'-' || name[28] != L'-' || wcscmp(name + 37, L".zip")) return FALSE;
    for (i = 0; i < 37; i++) {
        WCHAR c = name[i]; if (i == 8 || i == 15 || i == 19 || i == 28) continue;
        if (c >= L'0' && c <= L'9') continue;
        if (i >= 20 && c >= L'A' && c <= L'F') continue;
        return FALSE;
    }
    return TRUE;
}
static BOOL SnapshotListIn(const Profile *profile, const WCHAR *state, SnapshotNames *list) {
    WCHAR directory[LONG_PATH_CCH]; HANDLE files; WIN32_FIND_DATAW found;
    ZeroMemory(list, sizeof *list); if (!SnapshotDirectoryIn(profile, state, directory, ARRAYSIZE(directory))) return FALSE;
    files = Util_FindFiles(directory, L"*.zip", &found, FALSE); if (files == INVALID_HANDLE_VALUE) return TRUE;
    do {
        if (found.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (!SnapshotName(found.cFileName)) continue;
        if (list->count < (int)ARRAYSIZE(list->names)) StringCchCopyW(list->names[list->count++], ARRAYSIZE(list->names[0]), found.cFileName);
    } while (FindNextFileW(files, &found)); FindClose(files);
    qsort(list->names, (size_t)list->count, sizeof list->names[0], NameOrder); return TRUE;
}
static BOOL SnapshotList(const Profile *profile, SnapshotNames *list) {
    WCHAR state[MAX_PATH]; return Util_StateDir(state, ARRAYSIZE(state)) && SnapshotListIn(profile, state, list);
}
static BOOL ReadSafe(const Profile *profile, int index, char *out, size_t cch, BOOL *present) {
    WCHAR path[LONG_PATH_CCH]; char *raw; DWORD length, attributes;
    *present = FALSE;
    if (!profile->storageDir[0] || !Join(profile->storageDir, kFiles[index], path, ARRAYSIZE(path))) return FALSE;
    switch (Util_QueryPath(path, &attributes)) {
    case PATH_MISSING: out[0] = 0; return TRUE;
    case PATH_UNREACHABLE: return FALSE;
    case PATH_PRESENT: if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return FALSE;
    }
    raw = Util_ReadFile(path, SNAP_BYTES, FALSE, &length); if (!raw) return FALSE;
    *present = Core_JsonIsValue(raw, length) && SafeObject(raw, length, index != 0, out, cch);
    HeapFree(GetProcessHeap(), 0, raw); return *present;
}
static BOOL SaveSnapshotIn(const Profile *profile, WCHAR *saved, size_t cch, BOOL force, const WCHAR *state) {
    WCHAR directory[LONG_PATH_CCH], path[LONG_PATH_CCH]; char content[2][4096]; BOOL present[2], same = TRUE, ok; size_t i; DWORD error, sequence = (DWORD)InterlockedIncrement(&g_snapshotSequence);
    SnapshotNames list; ZipIn *previous = NULL; ZipOut *zip; SYSTEMTIME now;
    if (!SnapshotDirectoryIn(profile, state, directory, ARRAYSIZE(directory)) || !Util_EnsureDir(directory) ||
        !ReadSafe(profile, 0, content[0], sizeof content[0], &present[0]) || !ReadSafe(profile, 1, content[1], sizeof content[1], &present[1]) ||
        (!force && !present[0] && !present[1])) return FALSE;
    if (!force && SnapshotListIn(profile, state, &list) && list.count && Join(directory, list.names[0], path, ARRAYSIZE(path))) previous = Zip_Open(path);
    if (previous) {
        for (i = 0; i < ARRAYSIZE(kFiles); i++) {
            DWORD length = 0; int entry = Zip_Find(previous, kNames[i]); char *raw = NULL;
            if (entry >= 0) raw = (char *)Zip_Read(previous, entry, 4096, &length);
            if ((present[i] && (!raw || length != strlen(content[i]) || memcmp(raw, content[i], length))) || (!present[i] && entry >= 0)) same = FALSE;
            if (raw) HeapFree(GetProcessHeap(), 0, raw);
        }
        Zip_Free(previous);
        if (same) { if (saved && cch) StringCchCopyW(saved, cch, path); return TRUE; }
    }
    GetSystemTime(&now);
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%04u%02u%02u-%02u%02u%02u-%03u-%08lX-%08lX.zip", directory,
                               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, sequence, GetCurrentProcessId()))) return FALSE;
    zip = Zip_Create(path, &error); if (!zip) return FALSE;
    { static const char marker[] = "Typed UI geometry and allowDevTools only. No login, MCP, unknown fields or paths.\n";
      char presence[160];
      StringCchPrintfA(presence, sizeof presence, "{\"windowStatePresent\":%s,\"developerSettingsPresent\":%s}", present[0] ? "true" : "false", present[1] ? "true" : "false");
      ok = Zip_AddData(zip, "ui-config-snapshot-v1.txt", marker, sizeof marker - 1) && Zip_AddData(zip, "source-presence.json", presence, strlen(presence));
    }
    for (i = 0; ok && i < ARRAYSIZE(kFiles); i++) if (present[i]) ok = Zip_AddData(zip, kNames[i], content[i], strlen(content[i]));
    ok = Zip_Close(zip, ok) && ok; if (!ok) return FALSE;
    if (saved && cch) StringCchCopyW(saved, cch, path);
    if (SnapshotListIn(profile, state, &list)) for (i = SNAP_MAX; i < (size_t)list.count; i++) if (Join(directory, list.names[i], path, ARRAYSIZE(path))) DeleteFileW(path);
    return TRUE;
}
static BOOL SaveSnapshot(const Profile *profile, WCHAR *saved, size_t cch, BOOL force) {
    WCHAR state[MAX_PATH]; return Util_StateDir(state, ARRAYSIZE(state)) && SaveSnapshotIn(profile, saved, cch, force, state);
}
BOOL ProfileTools_SaveSnapshot(const Profile *profile, WCHAR *saved, size_t cch) {
    HANDLE lock = Util_SyncLock(); BOOL ok; if (!lock) return FALSE;
    ok = SaveSnapshot(profile, saved, cch, TRUE); Util_SyncUnlock(lock); return ok;
}
static BOOL WriteAtomic(const WCHAR *path, const char *raw, size_t length) {
    WCHAR temp[LONG_PATH_CCH]; HANDLE file; DWORD written; BOOL ok;
    if (length > MAXDWORD || FAILED(StringCchPrintfW(temp, ARRAYSIZE(temp), L"%s.cdpm-config-new", path))) return FALSE;
    file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, raw, (DWORD)length, &written, NULL) && written == length && FlushFileBuffers(file); CloseHandle(file);
    if (ok) ok = MoveFileExW(temp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); if (!ok) DeleteFileW(temp); return ok;
}
/* Only the known scalar members change; nested geometry also preserves unknown children. */
static char *ApplySafe(const char *base, size_t baseLength, const char *snapshot, size_t snapshotLength, BOOL developer, BOOL geometryChild, size_t *outLength) {
    size_t i, n; const char *value; const char *const *keys = developer ? kFlags : kGeometry; size_t count = developer ? ARRAYSIZE(kFlags) : geometryChild ? 4 : ARRAYSIZE(kGeometry);
    char *current = (char *)HeapAlloc(GetProcessHeap(), 0, baseLength + 1); size_t length = baseLength;
    if (!current) return NULL; CopyMemory(current, base, baseLength); current[baseLength] = 0;
    for (i = 0; i < count; i++) {
        char *changed = (char *)HeapAlloc(GetProcessHeap(), 0, length + 4096); size_t changedLength; BOOL ok;
        if (!changed) goto failed;
        if (Core_JsonMember(snapshot, snapshotLength, keys[i], &value, &n)) {
            char raw[128]; if (n >= sizeof raw) { HeapFree(GetProcessHeap(), 0, changed); goto failed; } CopyMemory(raw, value, n); raw[n] = 0;
            ok = Core_JsonSetMember(current, length, keys[i], raw, changed, length + 4096, &changedLength);
        } else ok = Core_JsonRemoveMember(current, length, keys[i], changed, length + 4096, &changedLength);
        if (!ok) { HeapFree(GetProcessHeap(), 0, changed); goto failed; }
        changed[changedLength] = 0; HeapFree(GetProcessHeap(), 0, current); current = changed; length = changedLength;
    }
    if (!developer && !geometryChild) {
        static const char *const nested[] = { "displayBounds" };
        for (i = 0; i < ARRAYSIZE(nested); i++) if (Core_JsonMember(snapshot, snapshotLength, nested[i], &value, &n)) {
            const char *old = "{}"; size_t oldLength = 2, childLength; char *child, *changed;
            Core_JsonMember(current, length, nested[i], &old, &oldLength);
            if (!oldLength || old[0] != '{') goto failed;
            child = ApplySafe(old, oldLength, value, n, FALSE, TRUE, &childLength); if (!child) goto failed;
            changed = (char *)HeapAlloc(GetProcessHeap(), 0, length + childLength + 4096);
            if (!changed || !Core_JsonSetMember(current, length, nested[i], child, changed, length + childLength + 4096, outLength)) {
                if (changed) HeapFree(GetProcessHeap(), 0, changed); HeapFree(GetProcessHeap(), 0, child); goto failed;
            }
            changed[*outLength] = 0; HeapFree(GetProcessHeap(), 0, child); HeapFree(GetProcessHeap(), 0, current); current = changed; length = *outLength;
        }
    }
    *outLength = length; return current;
failed: HeapFree(GetProcessHeap(), 0, current); return NULL;
}
BOOL ProfileTools_RestoreSnapshot(const Profile *profile, const WCHAR *archive, WCHAR *backup, size_t cch) {
    ZipIn *zip = NULL; HANDLE lock; int i, prepared = 0; BOOL ok = FALSE; char *changed[2] = { NULL, NULL }; size_t lengths[2] = { 0, 0 };
    WCHAR paths[2][LONG_PATH_CCH], validation[256]; DWORD error = ERROR_INVALID_DATA;
    if (!profile || !profile->storageDir[0] || profile->running || Claude_IsRunning(profile)) { SetLastError(ERROR_BUSY); return FALSE; }
    lock = Util_SyncLock(); if (!lock) return FALSE;
    zip = Zip_Open(archive); if (!zip || !Zip_ValidateAll(zip, NULL, validation, ARRAYSIZE(validation))) goto done;
    { DWORD markerLength = 0, presenceLength = 0; char *marker, *presence; const char *flag; size_t flagLength;
      marker = (char *)Zip_Read(zip, Zip_Find(zip, "ui-config-snapshot-v1.txt"), 256, &markerLength);
      presence = (char *)Zip_Read(zip, Zip_Find(zip, "source-presence.json"), 256, &presenceLength);
      ok = marker && markerLength && presence && Core_JsonIsValue(presence, presenceLength) &&
           Core_JsonMember(presence, presenceLength, "windowStatePresent", &flag, &flagLength) && IsBoolean(flag, flagLength) &&
           Core_JsonMember(presence, presenceLength, "developerSettingsPresent", &flag, &flagLength) && IsBoolean(flag, flagLength);
      if (ok) {
          static const char *const flags[] = { "windowStatePresent", "developerSettingsPresent" }; int f;
          for (f = 0; f < 2; f++) {
              if (!Core_JsonMember(presence, presenceLength, flags[f], &flag, &flagLength) ||
                  Core_JsonTrue(flag, flagLength) != (Zip_Find(zip, kNames[f]) >= 0)) ok = FALSE;
          }
      }
      if (marker) HeapFree(GetProcessHeap(), 0, marker); if (presence) HeapFree(GetProcessHeap(), 0, presence);
      if (!ok) goto done; ok = FALSE;
    }
    for (i = 0; i < 2; i++) {
        int entry = Zip_Find(zip, kNames[i]); DWORD length, baseLength; char *snapshot, *base; char safe[4096];
        if (entry < 0) continue;
        snapshot = (char *)Zip_Read(zip, entry, 4096, &length); if (!snapshot) goto done;
        if (!Core_JsonIsValue(snapshot, length) || !SafeObject(snapshot, length, i != 0, safe, sizeof safe) || strlen(safe) != length || memcmp(safe, snapshot, length)) { HeapFree(GetProcessHeap(), 0, snapshot); goto done; }
        if (!Join(profile->storageDir, kFiles[i], paths[i], ARRAYSIZE(paths[i]))) { HeapFree(GetProcessHeap(), 0, snapshot); goto done; }
        base = Util_ReadFile(paths[i], SNAP_BYTES, FALSE, &baseLength);
        if (!base && (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND)) {
            base = (char *)HeapAlloc(GetProcessHeap(), 0, 3); if (base) { CopyMemory(base, "{}", 3); baseLength = 2; }
        }
        if (!base) { HeapFree(GetProcessHeap(), 0, snapshot); goto done; }
        if (Core_JsonIsValue(base, baseLength) && base[0] == '{') changed[i] = ApplySafe(base, baseLength, snapshot, length, i != 0, FALSE, &lengths[i]);
        HeapFree(GetProcessHeap(), 0, snapshot); HeapFree(GetProcessHeap(), 0, base); if (!changed[i]) goto done; prepared++;
    }
    if (!prepared || Claude_IsRunning(profile) || !SaveSnapshot(profile, backup, cch, TRUE)) { error = ERROR_BUSY; goto done; }
    for (i = 0; i < 2; i++) if (changed[i]) {
        if (Claude_IsRunning(profile)) { error = ERROR_BUSY; goto recorded; }
        if (!WriteAtomic(paths[i], changed[i], lengths[i])) { error = GetLastError(); goto recorded; }
    }
    error = ERROR_SUCCESS; ok = TRUE;
recorded:
    OperationHistory_Record(OPERATION_SNAPSHOT, profile->folder, error, backup);
done:
    for (i = 0; i < 2; i++) if (changed[i]) HeapFree(GetProcessHeap(), 0, changed[i]);
    if (zip) Zip_Free(zip); Util_SyncUnlock(lock); if (!ok) SetLastError(error); return ok;
}
typedef struct SnapshotWork { ProfileList profiles; WCHAR state[MAX_PATH]; } SnapshotWork;
static DWORD WINAPI SnapshotThread(void *parameter) {
    SnapshotWork *work = (SnapshotWork *)parameter; HANDLE lock = Util_SyncLock(); int i;
    if (lock) { for (i = 0; i < work->profiles.count; i++) SaveSnapshotIn(&work->profiles.items[i], NULL, 0, FALSE, work->state); Util_SyncUnlock(lock); }
    HeapFree(GetProcessHeap(), 0, work); InterlockedExchange(&g_snapshotBusy, 0); return 0;
}
void ProfileTools_SnapshotEvent(const ProfileList *profiles) {
    SnapshotWork *copy; HANDLE thread;
    if (!profiles || InterlockedCompareExchange(&g_snapshotBusy, 1, 0)) return;
    copy = (SnapshotWork *)HeapAlloc(GetProcessHeap(), 0, sizeof *copy);
    if (!copy) { InterlockedExchange(&g_snapshotBusy, 0); return; } copy->profiles = *profiles;
    if (!Util_StateDir(copy->state, ARRAYSIZE(copy->state))) { HeapFree(GetProcessHeap(), 0, copy); InterlockedExchange(&g_snapshotBusy, 0); return; }
    thread = CreateThread(NULL, 0, SnapshotThread, copy, 0, NULL);
    if (!thread) { HeapFree(GetProcessHeap(), 0, copy); InterlockedExchange(&g_snapshotBusy, 0); } else CloseHandle(thread);
}
typedef struct SnapshotDialog { const Profile *profile; SnapshotNames names; } SnapshotDialog;
static void Fill(HWND dialog, SnapshotDialog *state) {
    int i; SendDlgItemMessageW(dialog, IDC_PT_SNAPSHOT_LIST, LB_RESETCONTENT, 0, 0); SnapshotList(state->profile, &state->names);
    for (i = 0; i < state->names.count; i++) SendDlgItemMessageW(dialog, IDC_PT_SNAPSHOT_LIST, LB_ADDSTRING, 0, (LPARAM)state->names.names[i]);
    SendDlgItemMessageW(dialog, IDC_PT_SNAPSHOT_LIST, LB_SETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDC_PT_SNAPSHOT_RESTORE), state->names.count > 0);
}
static INT_PTR CALLBACK SnapshotProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    SnapshotDialog *state = (SnapshotDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) { state = (SnapshotDialog *)lp; SetWindowLongPtrW(dialog, DWLP_USER, lp); Fill(dialog, state); return TRUE; }
    if (message == WM_COMMAND && state) {
        WCHAR saved[LONG_PATH_CCH] = L"", directory[LONG_PATH_CCH], path[LONG_PATH_CCH];
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (LOWORD(wp) == IDC_PT_SNAPSHOT_CREATE) {
            if (!ProfileTools_SaveSnapshot(state->profile, saved, ARRAYSIZE(saved))) Ui_Message(dialog, MB_ICONERROR, TR(L"No supported configuration could be snapshotted."));
            Fill(dialog, state); return TRUE;
        }
        if (LOWORD(wp) == IDC_PT_SNAPSHOT_RESTORE) {
            int selected = (int)SendDlgItemMessageW(dialog, IDC_PT_SNAPSHOT_LIST, LB_GETCURSEL, 0, 0);
            if (selected < 0 || selected >= state->names.count || !SnapshotDirectory(state->profile, directory, ARRAYSIZE(directory)) ||
                !Join(directory, state->names.names[selected], path, ARRAYSIZE(path))) return TRUE;
            if (!Ui_ReviewText(dialog, TR(L"Restore configuration snapshot"), TR(L"Close this profile first. Only supported UI geometry and developer boolean flags are restored. Other fields remain unchanged. Current supported values are backed up first."), path, TR(L"Restore"))) return TRUE;
            if (!ProfileTools_RestoreSnapshot(state->profile, path, saved, ARRAYSIZE(saved))) Ui_Message(dialog, MB_ICONERROR, TR(L"The configuration snapshot could not be restored (Windows error %lu)."), GetLastError());
            else Ui_Message(dialog, MB_ICONINFORMATION, TR(L"Configuration restored. The pre-change snapshot is listed in recent operation results."));
            Fill(dialog, state); return TRUE;
        }
    }
    return FALSE;
}
void ProfileTools_SnapshotDialog(HWND owner, const Profile *profile) {
    SnapshotDialog state; ZeroMemory(&state, sizeof state); state.profile = profile;
    Ui_Dialog(owner, IDD_PT_SNAPSHOTS, SnapshotProc, (LPARAM)&state);
}
