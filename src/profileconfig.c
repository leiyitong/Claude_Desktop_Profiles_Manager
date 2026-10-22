/* Versioned manager-only migration. Stable folders are explicitly mapped,
 * never moved, and numeric sync group IDs are rebuilt on the destination. */
#include "profiletools.h"
#include "operationhistory.h"
#include <commdlg.h>
#include <string.h>
#define CONFIG_MAGIC 0x314D4443u
#define PNG_MAX (4u * 1024u * 1024u)
typedef struct ProfileMeta {
    WCHAR folder[FOLDER_CCH], name[LABEL_CCH], note[256], badge[BADGE_CCH], title[WINDOW_TITLE_CCH];
    int color, group; DWORD items, picture;
} ProfileMeta;
typedef struct ManagerConfig { DWORD magic, version, count; int defaultIndex; ProfileMeta profiles[MAX_PROFILES]; } ManagerConfig;
typedef struct ConfigSource { ManagerConfig config; DWORD *pictures[MAX_PROFILES]; } ConfigSource;
static BOOL Join(const WCHAR *dir, const WCHAR *leaf, WCHAR *out, size_t cch) { return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", dir, leaf)); }
static BOOL FolderSafe(const WCHAR *folder) {
    size_t i, n = wcsnlen_s(folder, FOLDER_CCH);
    if (!n || n >= FOLDER_CCH || folder[n - 1] == '.' || folder[n - 1] == ' ' ||
        (!Core_EqualsI(folder, STOCK_FOLDER) && !Core_IsProfileFolder(folder))) return FALSE;
    for (i = 0; i < n; i++) if (folder[i] < 32 || wcschr(L"\\/:*?\"<>|", folder[i])) return FALSE;
    return TRUE;
}
static BOOL ConfigValid(const ManagerConfig *config) {
    DWORD i, j; WCHAR clean[LABEL_CCH], badge[BADGE_CCH];
    if (config->magic != CONFIG_MAGIC || config->version != 1 || config->count > MAX_PROFILES ||
        config->defaultIndex < -1 || config->defaultIndex >= (int)config->count) return FALSE;
    for (i = 0; i < config->count; i++) {
        const ProfileMeta *p = &config->profiles[i];
        if (!FolderSafe(p->folder) || wcsnlen_s(p->name, LABEL_CCH) == LABEL_CCH ||
            wcsnlen_s(p->note, ARRAYSIZE(p->note)) == ARRAYSIZE(p->note) || wcsnlen_s(p->title, WINDOW_TITLE_CCH) == WINDOW_TITLE_CCH ||
            wcsnlen_s(p->badge, BADGE_CCH) == BADGE_CCH || !Core_ValidateLabel(p->name, clean, ARRAYSIZE(clean), NULL) ||
            !Core_CleanBadge(p->badge, badge, ARRAYSIZE(badge)) || p->color < -1 || p->color >= PALETTE_SIZE ||
            p->group < 0 || p->group > MAX_PROFILES || (p->items & ~SYNC_ITEMS_ALL) || p->picture > 1) return FALSE;
        for (j = 0; j < i; j++) if (Core_EqualsI(p->folder, config->profiles[j].folder)) return FALSE;
    }
    return TRUE;
}
BOOL ProfileTools_Export(const ProfileList *profiles, const WCHAR *path, DWORD *error) {
    ManagerConfig *config; ZipOut *zip; int i; BOOL ok = TRUE; WCHAR state[MAX_PATH], picture[LONG_PATH_CCH]; char entry[40];
    if (!profiles || profiles->count < 0 || profiles->count > MAX_PROFILES || !path || !error) return FALSE;
    config = (ManagerConfig *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *config);
    if (!config) { *error = ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
    config->magic = CONFIG_MAGIC; config->version = 1; config->count = profiles->count; config->defaultIndex = -1;
    for (i = 0; i < profiles->count; i++) {
        const Profile *p = &profiles->items[i]; ProfileMeta *m = &config->profiles[i];
        StringCchCopyW(m->folder, FOLDER_CCH, p->folder); StringCchCopyW(m->name, LABEL_CCH, p->name);
        StringCchCopyW(m->note, ARRAYSIZE(m->note), p->note); StringCchCopyW(m->badge, BADGE_CCH, p->badge);
        StringCchCopyW(m->title, WINDOW_TITLE_CCH, p->windowTitle); m->color = p->color; m->group = p->syncGroup; m->items = p->syncItems;
        m->picture = p->picture != 0; if (Core_EqualsI(profiles->defaultFolder, p->folder)) config->defaultIndex = i;
    }
    if (!ConfigValid(config)) { *error = ERROR_INVALID_DATA; HeapFree(GetProcessHeap(), 0, config); return FALSE; }
    zip = Zip_Create(path, error);
    if (!zip) { HeapFree(GetProcessHeap(), 0, config); return FALSE; }
    ok = Zip_AddData(zip, "manager-config-v1.dat", config, sizeof *config);
    for (i = 0; ok && i < profiles->count; i++) if (config->profiles[i].picture) {
        StringCchPrintfA(entry, ARRAYSIZE(entry), "pictures/%02d.png", i);
        ok = Util_StateDir(state, ARRAYSIZE(state)) && SUCCEEDED(StringCchPrintfW(picture, ARRAYSIZE(picture), L"%s\\pictures\\%s.png", state, profiles->items[i].folder)) &&
             Zip_AddFile(zip, entry, picture);
    }
    *error = ok ? ERROR_SUCCESS : Zip_Error(zip);
    ok = Zip_Close(zip, ok) && ok; if (!ok && !*error) *error = ERROR_WRITE_FAULT;
    HeapFree(GetProcessHeap(), 0, config); return ok;
}
static void SourceFree(ConfigSource *source) { DWORD i; for (i = 0; i < MAX_PROFILES; i++) if (source->pictures[i]) { HeapFree(GetProcessHeap(), 0, source->pictures[i]); source->pictures[i] = NULL; } }
static BOOL SourceRead(const WCHAR *path, ConfigSource *source) {
    ZipIn *zip = Zip_Open(path); DWORD length, i, written; void *raw; BOOL ok = FALSE; WCHAR state[MAX_PATH], temp[LONG_PATH_CCH], validation[256]; char entry[40];
    ZeroMemory(source, sizeof *source); if (!zip) return FALSE;
    if (!Zip_ValidateAll(zip, NULL, validation, ARRAYSIZE(validation))) { Zip_Free(zip); SetLastError(ERROR_INVALID_DATA); return FALSE; }
    raw = Zip_Read(zip, Zip_Find(zip, "manager-config-v1.dat"), sizeof source->config, &length);
    if (!raw) goto done;
    if (length == sizeof source->config) CopyMemory(&source->config, raw, length);
    HeapFree(GetProcessHeap(), 0, raw); if (length != sizeof source->config || !ConfigValid(&source->config)) goto done;
    for (i = 0; i < source->config.count; i++) if (source->config.profiles[i].picture) {
        HANDLE file; HRESULT hr;
        StringCchPrintfA(entry, ARRAYSIZE(entry), "pictures/%02lu.png", i);
        ok = FALSE; raw = Zip_Read(zip, Zip_Find(zip, entry), PNG_MAX, &length); if (!raw) goto done;
        if (!Util_StateDir(state, ARRAYSIZE(state)) || !Util_EnsureDir(state) ||
            FAILED(StringCchPrintfW(temp, ARRAYSIZE(temp), L"%s\\migration-picture-%lu-%lu.png", state, GetCurrentProcessId(), i))) { HeapFree(GetProcessHeap(), 0, raw); goto done; }
        file = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (file == INVALID_HANDLE_VALUE) { HeapFree(GetProcessHeap(), 0, raw); goto done; }
        ok = WriteFile(file, raw, length, &written, NULL) && written == length; CloseHandle(file); HeapFree(GetProcessHeap(), 0, raw);
        if (ok) source->pictures[i] = Icons_ReadPicture(temp, &hr);
        DeleteFileW(temp); if (!source->pictures[i]) goto done;
    }
    ok = TRUE;
done:
    Zip_Free(zip); if (!ok) SourceFree(source); return ok;
}
static BOOL BackupPath(const WCHAR *prefix, WCHAR *out, size_t cch) {
    WCHAR state[MAX_PATH]; FILETIME time; ULARGE_INTEGER ticks;
    GetSystemTimeAsFileTime(&time); ticks.LowPart = time.dwLowDateTime; ticks.HighPart = time.dwHighDateTime;
    return Util_StateDir(state, ARRAYSIZE(state)) && Util_EnsureDir(state) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s-%llu.zip", state, prefix, ticks.QuadPart));
}
/* Preserve exact known registry values, including absence, for automatic rollback.
 * Unknown values are never deleted. PNG bytes are restored only if changed. */
#define UNDO_VALUES 9
#define UNDO_VALUE_BYTES 2048
static const WCHAR *const kUndoValues[UNDO_VALUES] = { L"Name", L"Note", L"Color", L"Badge", L"WindowTitle", L"Picture", L"SyncSessions", L"SyncParts", L"SyncItems" };
typedef struct UndoValue { BOOL present; DWORD type, length; BYTE bytes[UNDO_VALUE_BYTES]; } UndoValue;
typedef struct UndoTarget { UndoValue values[UNDO_VALUES]; BOOL picturePresent; DWORD pictureLength; char *picture; WCHAR picturePath[LONG_PATH_CCH]; } UndoTarget;
typedef struct MigrationUndo { UndoTarget targets[MAX_PROFILES]; UndoValue defaultValue; } MigrationUndo;
static BOOL CaptureValue(HKEY key, const WCHAR *name, UndoValue *value) {
    LSTATUS status; ZeroMemory(value, sizeof *value); if (!key) return TRUE;
    value->length = sizeof value->bytes;
    status = RegQueryValueExW(key, name, NULL, &value->type, value->bytes, &value->length);
    if (status == ERROR_FILE_NOT_FOUND) { value->length = 0; return TRUE; }
    if (status != ERROR_SUCCESS) { SetLastError((DWORD)status); return FALSE; }
    value->present = TRUE; return TRUE;
}
static BOOL RestoreValue(HKEY key, const WCHAR *name, const UndoValue *value) {
    LSTATUS status = value->present ? RegSetValueExW(key, name, 0, value->type, value->bytes, value->length) : RegDeleteValueW(key, name);
    return status == ERROR_SUCCESS || (!value->present && status == ERROR_FILE_NOT_FOUND);
}
static BOOL CaptureUndo(const ProfileList *profiles, DWORD targets, MigrationUndo *undo) {
    int i, v; WCHAR state[MAX_PATH], keyName[MAX_PATH]; HKEY key = NULL; LSTATUS status;
    if (!Util_StateDir(state, ARRAYSIZE(state))) return FALSE;
    status = RegOpenKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, KEY_QUERY_VALUE, &key);
    if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return FALSE;
    if (!CaptureValue(key, L"DefaultProfile", &undo->defaultValue)) { if (key) RegCloseKey(key); return FALSE; }
    if (key) RegCloseKey(key);
    for (i = 0; i < profiles->count; i++) if (targets & (1u << i)) {
        UndoTarget *target = &undo->targets[i]; DWORD attributes; key = NULL;
        StringCchPrintfW(keyName, ARRAYSIZE(keyName), REG_PROFILES L"\\%s", profiles->items[i].folder);
        status = RegOpenKeyExW(HKEY_CURRENT_USER, keyName, 0, KEY_QUERY_VALUE, &key);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) return FALSE;
        for (v = 0; v < UNDO_VALUES; v++) if (!CaptureValue(key, kUndoValues[v], &target->values[v])) { if (key) RegCloseKey(key); return FALSE; }
        if (key) RegCloseKey(key);
        if (FAILED(StringCchPrintfW(target->picturePath, ARRAYSIZE(target->picturePath), L"%s\\pictures\\%s.png", state, profiles->items[i].folder))) return FALSE;
        switch (Util_QueryPath(target->picturePath, &attributes)) {
        case PATH_UNREACHABLE: return FALSE;
        case PATH_MISSING: break;
        case PATH_PRESENT:
            if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return FALSE;
            target->picture = Util_ReadFile(target->picturePath, PNG_MAX, FALSE, &target->pictureLength);
            if (!target->picture) return FALSE; target->picturePresent = TRUE; break;
        }
    }
    return TRUE;
}
static BOOL RestorePicture(const UndoTarget *target) {
    char *current; DWORD length = 0, written; WCHAR temporary[LONG_PATH_CCH]; HANDLE file; BOOL ok;
    if (!target->picturePresent) return DeleteFileW(target->picturePath) || GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    current = Util_ReadFile(target->picturePath, PNG_MAX, FALSE, &length);
    ok = current && length == target->pictureLength && !memcmp(current, target->picture, length);
    if (current) HeapFree(GetProcessHeap(), 0, current); if (ok) return TRUE;
    if (FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s.cdpm-undo", target->picturePath))) return FALSE;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, target->picture, target->pictureLength, &written, NULL) && written == target->pictureLength && FlushFileBuffers(file); CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary, target->picturePath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(temporary); return ok;
}
static BOOL RestoreUndo(const ProfileList *profiles, DWORD changed, BOOL defaultAttempted, const MigrationUndo *undo) {
    BOOL ok = TRUE; int i, v; HKEY key; WCHAR keyName[MAX_PATH];
    for (i = 0; i < profiles->count; i++) if (changed & (1u << i)) {
        const UndoTarget *target = &undo->targets[i];
        if (!RestorePicture(target)) ok = FALSE;
        StringCchPrintfW(keyName, ARRAYSIZE(keyName), REG_PROFILES L"\\%s", profiles->items[i].folder);
        if (RegCreateKeyExW(HKEY_CURRENT_USER, keyName, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) { ok = FALSE; continue; }
        for (v = 0; v < UNDO_VALUES; v++) if (!RestoreValue(key, kUndoValues[v], &target->values[v])) ok = FALSE;
        RegCloseKey(key);
    }
    if (defaultAttempted) {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_ROOT, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL) != ERROR_SUCCESS) ok = FALSE;
        else { if (!RestoreValue(key, L"DefaultProfile", &undo->defaultValue)) ok = FALSE; RegCloseKey(key); }
    }
    return ok;
}
static void FreeUndo(MigrationUndo *undo) {
    int i; if (!undo) return;
    for (i = 0; i < MAX_PROFILES; i++) if (undo->targets[i].picture) HeapFree(GetProcessHeap(), 0, undo->targets[i].picture);
    HeapFree(GetProcessHeap(), 0, undo);
}
BOOL ProfileTools_Import(const WCHAR *path, const ProfileList *profiles, const int *mapping, int mappingCount, DWORD *error) {
    ConfigSource *source; MigrationUndo *undo = NULL; DWORD i, used = 0, targets = 0, changed = 0; int groups[MAX_PROFILES + 1], target; BOOL ok = FALSE;
    WCHAR backup[LONG_PATH_CCH], detail[384] = L""; HANDLE lock; BOOL defaultAttempted = FALSE;
    if (!profiles || !mapping || !error) return FALSE; *error = ERROR_INVALID_DATA;
    source = (ConfigSource *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *source); if (!source) { *error = ERROR_NOT_ENOUGH_MEMORY; return FALSE; }
    lock = Util_SyncLock(); if (!lock) { HeapFree(GetProcessHeap(), 0, source); *error = GetLastError(); return FALSE; }
    if (!SourceRead(path, source) || mappingCount != (int)source->config.count) goto done;
    ZeroMemory(groups, sizeof groups);
    for (i = 0; i < source->config.count; i++) {
        target = mapping[i]; if (target == -1) continue;
        if (target < 0 || target >= profiles->count || !FolderSafe(profiles->items[target].folder) || (targets & (1u << target))) goto done;
        targets |= 1u << target;
        if (profiles->items[target].running || Claude_IsRunning(&profiles->items[target])) { *error = ERROR_BUSY; goto done; }
    }
    if (!targets) { *error = ERROR_CANCELLED; goto done; }
    for (i = 0; i < (DWORD)profiles->count; i++) if (!(targets & (1u << i)) && profiles->items[i].syncGroup > 0)
        used |= 1u << (profiles->items[i].syncGroup - 1);
    for (i = 0; i < source->config.count; i++) if (mapping[i] >= 0 && source->config.profiles[i].group) {
        int group = source->config.profiles[i].group, next;
        if (groups[group]) continue;
        for (next = 1; next <= MAX_PROFILES; next++) if (!(used & (1u << (next - 1)))) break;
        if (next > MAX_PROFILES) { *error = ERROR_TOO_MANY_NAMES; goto done; }
        groups[group] = next; used |= 1u << (next - 1);
    }
    undo = (MigrationUndo *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *undo);
    if (!undo || !CaptureUndo(profiles, targets, undo)) { *error = GetLastError(); if (!*error) *error = ERROR_NOT_ENOUGH_MEMORY; goto done; }
    if (!BackupPath(L"manager-config-before", backup, ARRAYSIZE(backup)) || !ProfileTools_Export(profiles, backup, error)) goto done;
    for (i = 0; i < source->config.count; i++) if ((target = mapping[i]) >= 0) {
        const ProfileMeta *m = &source->config.profiles[i]; const WCHAR *folder = profiles->items[target].folder; DWORD stamp = 0;
        if (Claude_IsRunning(&profiles->items[target])) { *error = ERROR_BUSY; goto recorded; }
        changed |= 1u << target;
        if (m->picture && !Icons_SavePicture(folder, source->pictures[i], &stamp)) { *error = GetLastError(); goto recorded; }
        if (!Profiles_Update(folder, m->name, m->color, m->badge, stamp) || !Profiles_SetNote(folder, m->note) ||
            !Profiles_SetWindowTitle(folder, m->title) || !Profiles_SetSyncGroup(folder, groups[m->group]) || !Profiles_SetSyncItems(folder, m->items)) {
            *error = GetLastError(); if (!*error) *error = ERROR_WRITE_FAULT; goto recorded;
        }
        if (!m->picture) Icons_DeletePicture(folder);
    }
    target = source->config.defaultIndex;
    if (target >= 0 && mapping[target] >= 0) {
        defaultAttempted = TRUE;
        if (!Profiles_SetDefault(profiles->items[mapping[target]].folder)) { *error = GetLastError(); goto recorded; }
    }
    *error = ERROR_SUCCESS; ok = TRUE;
recorded:
    if (!ok && changed) {
        DWORD original = *error ? *error : ERROR_GEN_FAILURE; BOOL restored = RestoreUndo(profiles, changed, defaultAttempted, undo);
        *error = restored ? ERROR_REQUEST_ABORTED : ERROR_PARTIAL_COPY;
        StringCchPrintfW(detail, ARRAYSIZE(detail), restored ? TR(L"Import failed (Windows error %lu). All attempted manager changes were rolled back.") : TR(L"Import failed (Windows error %lu). Rollback was incomplete; use the saved configuration backup after resolving the file or registry error."), original);
    }
    OperationHistory_RecordDetail(OPERATION_MIGRATION, L"", *error, backup, detail);
done:
    FreeUndo(undo); SourceFree(source); HeapFree(GetProcessHeap(), 0, source); Util_SyncUnlock(lock); return ok;
}
typedef struct MappingDialog { const ProfileList *profiles; const ProfileMeta *source; int target; } MappingDialog;
static INT_PTR CALLBACK MappingProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    MappingDialog *map = (MappingDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        WCHAR note[600]; int i; map = (MappingDialog *)lp; SetWindowLongPtrW(dialog, DWLP_USER, lp);
        StringCchPrintfW(note, ARRAYSIZE(note), TR(L"Source profile: %s (%s)\r\nChoose an existing destination. Its data directory will not be renamed or moved. Skip leaves it unmapped."), map->source->name, map->source->folder);
        SetDlgItemTextW(dialog, IDC_PT_MAP_NOTE, note); SendDlgItemMessageW(dialog, IDC_PT_MAP_TARGET, CB_ADDSTRING, 0, (LPARAM)TR(L"Skip"));
        for (i = 0; i < map->profiles->count; i++) SendDlgItemMessageW(dialog, IDC_PT_MAP_TARGET, CB_ADDSTRING, 0, (LPARAM)map->profiles->items[i].name);
        SendDlgItemMessageW(dialog, IDC_PT_MAP_TARGET, CB_SETCURSEL, map->target + 1, 0); return TRUE;
    }
    if (message == WM_COMMAND && map) {
        if (LOWORD(wp) == IDOK) { map->target = (int)SendDlgItemMessageW(dialog, IDC_PT_MAP_TARGET, CB_GETCURSEL, 0, 0) - 1; EndDialog(dialog, IDOK); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    }
    return FALSE;
}
void ProfileTools_MigrationDialog(HWND owner, const ProfileList *profiles, BOOL importing) {
    WCHAR path[LONG_PATH_CCH] = L"", summary[8192], line[400]; OPENFILENAMEW choice; DWORD error = 0, i; ConfigSource *source; BOOL ok;
    ZeroMemory(&choice, sizeof choice); choice.lStructSize = sizeof choice; choice.hwndOwner = owner; choice.lpstrFile = path; choice.nMaxFile = ARRAYSIZE(path);
    choice.lpstrFilter = L"ZIP (*.zip)\0*.zip\0\0"; choice.lpstrDefExt = L"zip"; choice.Flags = OFN_EXPLORER | OFN_NOCHANGEDIR | (importing ? OFN_FILEMUSTEXIST : OFN_OVERWRITEPROMPT);
    if (!(importing ? GetOpenFileNameW(&choice) : GetSaveFileNameW(&choice))) return;
    if (!importing) { ok = ProfileTools_Export(profiles, path, &error); }
    else {
        int mapping[MAX_PROFILES]; source = (ConfigSource *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *source); if (!source) return;
        if (!SourceRead(path, source)) { HeapFree(GetProcessHeap(), 0, source); Ui_Message(owner, MB_ICONERROR, TR(L"This is not a readable manager configuration package.")); return; }
        summary[0] = 0;
        for (i = 0; i < source->config.count; i++) {
            MappingDialog map; int j; map.profiles = profiles; map.source = &source->config.profiles[i]; map.target = -1;
            for (j = 0; j < profiles->count; j++) if (Core_EqualsI(map.source->folder, profiles->items[j].folder)) map.target = j;
            if (Ui_Dialog(owner, IDD_PT_MAPPING, MappingProc, (LPARAM)&map) != IDOK) { SourceFree(source); HeapFree(GetProcessHeap(), 0, source); return; }
            mapping[i] = map.target;
            StringCchPrintfW(line, ARRAYSIZE(line), TR(L"%s (%s) -> %s; imported group %d; sync items 0x%08lX\r\n"), map.source->name, map.source->folder, map.target < 0 ? TR(L"Skip") : profiles->items[map.target].name, map.source->group, map.source->items);
            StringCchCatW(summary, ARRAYSIZE(summary), line);
        }
        SourceFree(source); HeapFree(GetProcessHeap(), 0, source);
        if (!Ui_ReviewText(owner, TR(L"Import manager configuration"), TR(L"Review the mapping. Destinations must be closed. Manager labels, images, default and sync memberships are replaced; current configuration is backed up first."), summary, TR(L"Import"))) return;
        ok = ProfileTools_Import(path, profiles, mapping, (int)i, &error);
    }
    if (ok) Ui_Message(owner, MB_ICONINFORMATION, importing ? TR(L"Manager configuration imported. Reopen this tools window to refresh its profile list.") : TR(L"Manager configuration exported."));
    else if (error == ERROR_REQUEST_ABORTED) Ui_Message(owner, MB_ICONERROR, TR(L"The import failed. All attempted manager changes were rolled back. The original error and pre-change backup are listed in recent operation results."));
    else if (error == ERROR_PARTIAL_COPY) Ui_Message(owner, MB_ICONERROR, TR(L"The import failed and some manager changes could not be rolled back. Resolve the file or registry error, then use the pre-change backup listed in recent operation results."));
    else Ui_Message(owner, MB_ICONERROR, TR(L"The configuration operation failed before completion (Windows error %lu). Any pre-change backup is listed in recent operation results."), error);
}
