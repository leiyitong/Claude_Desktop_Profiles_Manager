/* On-demand local reports. No network calls or database-engine opens. */
#include "profiletools.h"
#include "operationhistory.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#define REPORT_CCH (1024u * 1024u)
#define USAGE_MAX (16u * 1024u * 1024u)
#define WM_PT_DONE (WM_APP + 73)
#define WM_PT_SNAPSHOT (WM_APP + 74)
static BOOL Append(WCHAR *out, size_t cch, const WCHAR *format, ...) {
    va_list args; HRESULT hr; size_t used = wcslen(out);
    va_start(args, format); hr = StringCchVPrintfW(out + used, cch - used, format, args); va_end(args);
    return SUCCEEDED(hr);
}
static BOOL Cancelled(HANDLE cancel) { return cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0; }
static BOOL Join(const WCHAR *dir, const WCHAR *leaf, WCHAR *out, size_t cch) {
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", dir, leaf));
}
typedef struct UsageReport { WCHAR *out; size_t cch; DWORD count, missing; } UsageReport;
static BOOL UsageNumber(const char *json, size_t len, const char *key, WCHAR *out, size_t cch) {
    const char *value; size_t n; char small[64], *end; double number;
    if (!Core_JsonMember(json, len, key, &value, &n) || !n || n >= sizeof small ||
        !((value[0] >= '0' && value[0] <= '9') || value[0] == '-')) return FALSE;
    CopyMemory(small, value, n); small[n] = 0; number = strtod(small, &end);
    if (end != small + n || number < 0 || number > 1e12) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%.4g", number));
}
typedef struct ExtraUsage { WCHAR *out; size_t cch; BOOL first; } ExtraUsage;
static BOOL UsageExtra(void *context, const char *key, size_t keyLength, const char *value, size_t length) {
    ExtraUsage *extra = (ExtraUsage *)context; WCHAR name[128], number[64]; char raw[64], *end; double d; int wide;
    if ((keyLength == 2 && !memcmp(key, "fh", 2)) || (keyLength == 2 && !memcmp(key, "sd", 2))) return TRUE;
    if (!keyLength || keyLength > 100 || !length || length >= sizeof raw) return TRUE;
    wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, key, (int)keyLength, name, ARRAYSIZE(name)-1);
    if (!wide) return TRUE;
    name[wide] = 0; CopyMemory(raw, value, length); raw[length] = 0; d = strtod(raw, &end);
    if (end != raw + length || d < 0 || d > 1e12) return TRUE;
    StringCchPrintfW(number, ARRAYSIZE(number), L"%.4g", d);
    if (!Append(extra->out, extra->cch, extra->first ? L" | %s=%s" : L", %s=%s", name, number)) return FALSE;
    extra->first = FALSE; return TRUE;
}
static BOOL UsageSample(void *context, const char *json, size_t len) {
    UsageReport *report = (UsageReport *)context;
    const char *value, *usage; size_t n, usageLength; WCHAR org[96], stamp[80], fh[64], sd[64];
    ULONGLONG ms = 0; ULARGE_INTEGER ticks; FILETIME ft; SYSTEMTIME utc, local;
    StringCchCopyW(org, ARRAYSIZE(org), TR(L"Unknown"));
    if (Core_JsonMember(json, len, "org", &value, &n)) Core_JsonString(value, n, org, ARRAYSIZE(org));
    StringCchCopyW(stamp, ARRAYSIZE(stamp), TR(L"Unknown"));
    if (Core_JsonMember(json, len, "t", &value, &n) && Core_JsonNumber(value, n, &ms) && ms < 20000000000000ULL) {
        ticks.QuadPart = ms * 10000ULL + 116444736000000000ULL; ft.dwLowDateTime = ticks.LowPart; ft.dwHighDateTime = ticks.HighPart;
        if (FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &local))
            StringCchPrintfW(stamp, ARRAYSIZE(stamp), L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay,
                             local.wHour, local.wMinute, local.wSecond);
    }
    StringCchCopyW(fh, ARRAYSIZE(fh), TR(L"Not recorded")); StringCchCopyW(sd, ARRAYSIZE(sd), TR(L"Not recorded"));
    if (Core_JsonMember(json, len, "u", &usage, &usageLength)) {
        BOOL a = UsageNumber(usage, usageLength, "fh", fh, ARRAYSIZE(fh));
        BOOL b = UsageNumber(usage, usageLength, "sd", sd, ARRAYSIZE(sd));
        if (!a && !b) report->missing++;
    } else report->missing++;
    report->count++;
    if (!Append(report->out, report->cch, L"%s | %s | %s | %s", stamp, org, fh, sd)) return FALSE;
    if (Core_JsonMember(json, len, "u", &usage, &usageLength) && usageLength > 1 && usage[0] == '{') {
        ExtraUsage extra = { report->out, report->cch, TRUE };
        if (!Core_JsonEachMember(usage, usageLength, UsageExtra, &extra)) return FALSE;
    }
    return Append(report->out, report->cch, L"\r\n");
}
BOOL ProfileTools_UsageText(const char *json, size_t length, WCHAR *out, size_t cch) {
    const char *samples, *version; size_t n, vn; ULONGLONG v; UsageReport report;
    if (!out || !cch) return FALSE; out[0] = 0;
    if (!json || !Core_JsonIsValue(json, length) || !Core_JsonMember(json, length, "version", &version, &vn) ||
        !Core_JsonNumber(version, vn, &v) || v != 2 || !Core_JsonMember(json, length, "samples", &samples, &n)) return FALSE;
    if (!Append(out, cch, TR(L"Local sample time | Organization ID | Five-hour recorded usage (%%) | Seven-day recorded usage (%%)\r\n"))) return FALSE;
    report.out = out; report.cch = cch; report.count = report.missing = 0;
    if (!Core_JsonEachElement(samples, n, UsageSample, &report)) return FALSE;
    return Append(out, cch, TR(L"\r\nSamples: %lu; samples without usage: %lu. Missing data is not zero. Values are the numbers saved by Claude; no live quota or reset time is requested.\r\n"), report.count, report.missing);
}
static BOOL Scan(const WCHAR *path, HANDLE cancel, ProfileSpace *space, unsigned depth, BOOL cache) {
    WIN32_FIND_DATAW found; HANDLE search; DWORD attributes, error; WCHAR child[LONG_PATH_CCH];
    if (Cancelled(cancel)) { SetLastError(ERROR_CANCELLED); return FALSE; }
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) { space->complete = FALSE; space->skipped++; return TRUE; }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) { space->skipped++; return TRUE; }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || depth >= 64) { space->complete = FALSE; space->skipped++; return TRUE; }
    search = Util_FindFiles(path, L"*", &found, FALSE);
    if (search == INVALID_HANDLE_VALUE) {
        error = GetLastError(); if (error != ERROR_FILE_NOT_FOUND) { space->complete = FALSE; space->skipped++; } return TRUE;
    }
    do {
        BOOL isCache;
        if (Cancelled(cancel)) { FindClose(search); SetLastError(ERROR_CANCELLED); return FALSE; }
        if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || Core_EqualsI(found.cFileName, L"LOCK") ||
            Core_EqualsI(found.cFileName, L"lockfile") || Core_EqualsI(found.cFileName, L"SingletonLock")) { space->skipped++; continue; }
        if (!Join(path, found.cFileName, child, ARRAYSIZE(child))) { space->skipped++; space->complete = FALSE; continue; }
        isCache = cache || Core_EqualsI(found.cFileName, L"Cache") || Core_EqualsI(found.cFileName, L"Code Cache") ||
                  Core_EqualsI(found.cFileName, L"GPUCache");
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!Scan(child, cancel, space, depth + 1, isCache)) { FindClose(search); return FALSE; }
        } else {
            ULONGLONG bytes = ((ULONGLONG)found.nFileSizeHigh << 32) | found.nFileSizeLow;
            if (MAXULONGLONG - space->bytes < bytes) { FindClose(search); SetLastError(ERROR_ARITHMETIC_OVERFLOW); return FALSE; }
            space->bytes += bytes; space->files++; if (isCache) space->cacheBytes += bytes;
        }
    } while (FindNextFileW(search, &found));
    error = GetLastError(); FindClose(search);
    if (error != ERROR_NO_MORE_FILES) { space->complete = FALSE; space->skipped++; }
    return TRUE;
}
BOOL ProfileTools_ScanSpace(const WCHAR *path, HANDLE cancel, ProfileSpace *space) {
    if (!path || !space) return FALSE; ZeroMemory(space, sizeof *space); space->complete = TRUE;
    return Scan(path, cancel, space, 0, FALSE);
}
typedef struct ArchiveIndexCheck { int count, invalid, missing; const SessionSet *set; int profile; } ArchiveIndexCheck;
static BOOL ArchiveId(void *context, const char *raw, size_t length) {
    ArchiveIndexCheck *check = (ArchiveIndexCheck *)context; WCHAR id[SESSION_ID_CCH]; int i; BOOL found = FALSE;
    check->count++;
    if (!Core_JsonString(raw, length, id, ARRAYSIZE(id)) || wcsncmp(id, L"local_", 6) || !Core_IsUuid(id + 6)) { check->invalid++; return TRUE; }
    for (i = 0; i < check->set->rowCount; i++) {
        int e = check->set->rows[i].entry[check->profile];
        if (e >= 0 && !wcscmp(check->set->entries[e].localId, id)) { found = TRUE; break; }
    }
    if (!found) check->missing++; return TRUE;
}
static BOOL ArchiveIndex(const char *raw, size_t length, ArchiveIndexCheck *check) {
    const char *version, *archived; size_t vn, an; ULONGLONG v;
    return raw && length && raw[0] == '{' && Core_JsonIsValue(raw, length) &&
           Core_JsonMember(raw, length, "v", &version, &vn) && Core_JsonNumber(version, vn, &v) && v == 1 &&
           Core_JsonMember(raw, length, "archived", &archived, &an) && Core_JsonEachElement(archived, an, ArchiveId, check) && !check->invalid;
}
BOOL ProfileTools_HealthText(const ProfileList *profiles, HANDLE cancel, WCHAR *out, size_t cch) {
    SessionSet *set; int p, r; BOOL ok; char *layouts[MAX_PROFILES] = { 0 }; size_t layoutLengths[MAX_PROFILES] = { 0 };
    if (!profiles || !out || !cch) return FALSE; out[0] = 0;
    set = (SessionSet *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *set); if (!set) return FALSE;
    ok = SessionStore_LoadProfilesCancel(set, profiles, cancel);
    if (Cancelled(cancel)) { SessionStore_Free(set); HeapFree(GetProcessHeap(), 0, set); return FALSE; }
    if (!Append(out, cch, TR(L"Current local session inventory. Different bytes are differences, not proof of conflict or data loss. Unknown JSON fields are not treated as missing.\r\n\r\n"))) ok = FALSE;
    for (p = 0; p < profiles->count; p++) {
        SessionSource *source = &set->source[p]; int missing = 0, damaged = 0, listed = 0, duplicate = 0;
        WCHAR index[LONG_PATH_CCH]; DWORD bytes; char *raw;
        const WCHAR *account = source->entriesDir[0] ? source->entriesDir : TR(L"No current account or organization resolved");
        for (r = 0; r < set->rowCount; r++) if (set->rows[r].entry[p] >= 0) {
            SessionEntry *entry = &set->entries[set->rows[r].entry[p]];
            listed++; if (!set->rows[r].transcript) missing++; if (entry->damaged) damaged++; if (entry->duplicate >= 0) duplicate++;
        }
        if (!Append(out, cch, TR(L"Profile: %s (%s)\r\nCurrent account / organization directory: %s\r\nEntries: %d; unreadable: %d; remote entries: %d; missing local transcripts: %d; Claude marked unavailable: %d; duplicate entries: %d; complete read: %d.\r\n"),
                    profiles->items[p].name, profiles->items[p].folder, account, listed, source->unreadable, source->elsewhere, missing,
                    damaged, duplicate, source->complete)) ok = FALSE;
        if (source->entriesDir[0]) layouts[p] = SessionSync_ReadLayout(&profiles->items[p], source->entriesDir, &layoutLengths[p], NULL);
        if (source->entriesDir[0] && Join(source->entriesDir, L"archived-sessions.idx", index, ARRAYSIZE(index)) && Util_FileExists(index)) {
            raw = Util_ReadFile(index, SESSION_ENTRY_MAX_BYTES, FALSE, &bytes);
            { ArchiveIndexCheck check; BOOL valid; ZeroMemory(&check, sizeof check); check.set = set; check.profile = p;
              valid = ArchiveIndex(raw, bytes, &check);
              if (!Append(out, cch, TR(L"Archive index v1 structure valid: %d; IDs %d; invalid IDs %d; IDs not in the current local list %d (may refer to remote or unavailable entries).\r\n"), valid, check.count, check.invalid, check.missing)) ok = FALSE;
            }
            if (raw) HeapFree(GetProcessHeap(), 0, raw);
        }
        if (!Append(out, cch, L"\r\n")) ok = FALSE;
    }
    for (p = 0; p < profiles->count; p++) { int q;
        for (q = p + 1; q < profiles->count; q++) {
            int partIndex;
            LayoutPart part;
            if (!Append(out, cch, TR(L"Current configuration differences: %s / %s\r\n"), profiles->items[p].name, profiles->items[q].name)) ok = FALSE;
            if (!layouts[p] || !layouts[q]) {
                if (!Append(out, cch, TR(L"A current layout could not be read or is not stored; absence is not treated as equality.\r\n"))) ok = FALSE;
            } else for (partIndex = 0; SessionSync_LayoutPart(partIndex, &part); partIndex++) {
                const char *a = NULL, *b = NULL; size_t an = 0, bn = 0; WCHAR name[160]; const WCHAR *status;
                BOOL hasA = Core_JsonMember(layouts[p], layoutLengths[p], part.name, &a, &an);
                BOOL hasB = Core_JsonMember(layouts[q], layoutLengths[q], part.name, &b, &bn);
                if (!hasA && !hasB) continue;
                if (part.label) StringCchCopyW(name, ARRAYSIZE(name), TR(part.label));
                else if (!MultiByteToWideChar(CP_UTF8, 0, part.name, -1, name, ARRAYSIZE(name))) StringCchCopyW(name, ARRAYSIZE(name), TR(L"Unknown"));
                status = !hasA || !hasB ? TR(L"Present in one profile") : an == bn && !memcmp(a, b, an) ? TR(L"Identical saved value") : TR(L"Different saved values");
                if (!Append(out, cch, L"  %s: %s\r\n", name, status)) ok = FALSE;
            }
            {
            int shared = 0, same = 0, changed = 0, oneOnly = 0, unreadable = 0;
            for (r = 0; r < set->rowCount; r++) {
                int a = set->rows[r].entry[p], b = set->rows[r].entry[q]; DWORD an = 0, bn = 0; char *av, *bv;
                if (Cancelled(cancel)) { ok = FALSE; break; }
                if (a < 0 && b < 0) continue; if (a < 0 || b < 0) { oneOnly++; continue; } shared++;
                av = Util_ReadFile(set->entries[a].file, SESSION_ENTRY_MAX_BYTES, FALSE, &an);
                bv = Util_ReadFile(set->entries[b].file, SESSION_ENTRY_MAX_BYTES, FALSE, &bn);
                if (!av || !bv) unreadable++; else if (an == bn && !memcmp(av, bv, an)) same++; else changed++;
                if (av) HeapFree(GetProcessHeap(), 0, av); if (bv) HeapFree(GetProcessHeap(), 0, bv);
            }
            if (!Append(out, cch, TR(L"%s / %s: shared=%d, identical bytes=%d, different bytes=%d, listed by one=%d, unreadable comparisons=%d.\r\n"),
                        profiles->items[p].name, profiles->items[q].name, shared, same, changed, oneOnly, unreadable)) ok = FALSE;
            }
        }
    }
    for (p = 0; p < profiles->count; p++) if (layouts[p]) HeapFree(GetProcessHeap(), 0, layouts[p]);
    SessionStore_Free(set); HeapFree(GetProcessHeap(), 0, set); return ok;
}
typedef struct ReportWork { HWND dialog; HANDLE cancel, thread; ProfileList profiles; int kind; WCHAR *text; BOOL ok; } ReportWork;
static BOOL SpaceText(ReportWork *work) {
    int i; WCHAR shared[LONG_PATH_CCH], state[MAX_PATH]; ULONGLONG total = 0; ProfileSpace space;
    work->text[0] = 0;
    if (!Append(work->text, REPORT_CCH, TR(L"Logical file sizes; sparse files, compression and shared blocks can use a different amount on disk. Reparse points and lock files are skipped. Cache is included in each profile total.\r\n\r\n"))) return FALSE;
    for (i = 0; i < work->profiles.count; i++) {
        const Profile *profile = &work->profiles.items[i]; ULARGE_INTEGER freeBytes; ULONGLONG estimate;
        const WCHAR *path = profile->storageDir[0] ? profile->storageDir : profile->dataDir;
        if (!ProfileTools_ScanSpace(path, work->cancel, &space)) return FALSE;
        if (!Append(work->text, REPORT_CCH, TR(L"%s: %llu bytes; cache %llu; files %llu; skipped %llu; complete %d.\r\n"),
                    profile->name, space.bytes, space.cacheBytes, space.files, space.skipped, space.complete)) return FALSE;
        total += space.bytes;
        estimate = space.bytes > (MAXULONGLONG - 16 * 1024 * 1024) / 2 ? MAXULONGLONG : space.bytes * 2 + 16 * 1024 * 1024;
        if (space.files > (MAXULONGLONG - estimate) / 4096) estimate = MAXULONGLONG; else estimate += space.files * 4096;
        if (GetDiskFreeSpaceExW(path, &freeBytes, NULL, NULL) &&
            !Append(work->text, REPORT_CCH, TR(L"Free on this volume: %llu bytes. Profile files only, ZIP plus temporary space estimate: %llu bytes; shared transcripts are excluded; this is an estimate, not a reservation.\r\n"),
                    freeBytes.QuadPart, estimate)) return FALSE;
    }
    if (SessionStore_ClaudeCodePath(L"", shared, ARRAYSIZE(shared)) && Util_DirExists(shared)) {
        if (!ProfileTools_ScanSpace(shared, work->cancel, &space)) return FALSE;
        if (!Append(work->text, REPORT_CCH, TR(L"\r\nShared .claude (counted once): %llu bytes; skipped %llu; complete %d.\r\n"), space.bytes, space.skipped, space.complete)) return FALSE;
    }
    if (Util_StateDir(state, ARRAYSIZE(state)) && Util_DirExists(state)) {
        if (!ProfileTools_ScanSpace(state, work->cancel, &space)) return FALSE;
        if (!Append(work->text, REPORT_CCH, TR(L"Manager state, including vault and managed backups: %llu bytes; skipped %llu; complete %d.\r\n"), space.bytes, space.skipped, space.complete)) return FALSE;
    }
    if (Util_StateDir(state, ARRAYSIZE(state))) {
        static const WCHAR *const folders[] = { L"vault", L"backups", L"config-snapshots" }; size_t f;
        for (f = 0; f < ARRAYSIZE(folders); f++) {
            WCHAR path[LONG_PATH_CCH];
            if (Join(state, folders[f], path, ARRAYSIZE(path)) && Util_DirExists(path)) {
                if (!ProfileTools_ScanSpace(path, work->cancel, &space) || !Append(work->text, REPORT_CCH, TR(L"Manager %s: %llu bytes; skipped %llu; complete %d.\r\n"), folders[f], space.bytes, space.skipped, space.complete)) return FALSE;
            }
        }
    }
    if (SessionStore_ClaudeCodePath(L"", shared, ARRAYSIZE(shared))) {
        WCHAR parent[LONG_PATH_CCH], *slash; WIN32_FIND_DATAW found; HANDLE files;
        StringCchCopyW(parent, ARRAYSIZE(parent), shared); parent[Core_TrimmedPathLength(parent)] = 0; slash = wcsrchr(parent, L'\\');
        if (slash) {
            WCHAR leaf[MAX_PATH]; StringCchCopyW(leaf, ARRAYSIZE(leaf), slash + 1); *slash = 0;
            files = Util_FindFiles(parent, L"*", &found, TRUE);
            if (files != INVALID_HANDLE_VALUE) {
                do {
                    SYSTEMTIME day; BOOL weekly; WCHAR path[LONG_PATH_CCH];
                    if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || !Core_CopyDay(leaf, found.cFileName, &day, &weekly)) continue;
                    if (!Join(parent, found.cFileName, path, ARRAYSIZE(path)) || !ProfileTools_ScanSpace(path, work->cancel, &space)) { FindClose(files); return FALSE; }
                    if (!Append(work->text, REPORT_CCH, TR(L"Code copy %s: %llu bytes; weekly %d; skipped %llu; complete %d.\r\n"), found.cFileName, space.bytes, weekly, space.skipped, space.complete)) { FindClose(files); return FALSE; }
                } while (FindNextFileW(files, &found));
                FindClose(files);
            }
        }
    }
    return Append(work->text, REPORT_CCH, TR(L"\r\nAll profile totals: %llu bytes. Exported backups outside manager state are not included. Known dated and weekly copies beside .claude are listed separately. Choose the backup destination before comparing its actual free space.\r\n"), total);
}
static DWORD WINAPI ReportThread(void *parameter) {
    ReportWork *work = (ReportWork *)parameter;
    work->ok = work->kind == IDC_PT_HEALTH ? ProfileTools_HealthText(&work->profiles, work->cancel, work->text, REPORT_CCH) : SpaceText(work);
    PostMessageW(work->dialog, WM_PT_DONE, 0, 0); return 0;
}
static INT_PTR CALLBACK ProgressProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    ReportWork *work = (ReportWork *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        work = (ReportWork *)lp; SetWindowLongPtrW(dialog, DWLP_USER, lp); work->dialog = dialog;
        work->thread = CreateThread(NULL, 0, ReportThread, work, 0, NULL);
        if (!work->thread) EndDialog(dialog, IDCANCEL); return TRUE;
    }
    if (message == WM_PT_DONE && work) { WaitForSingleObject(work->thread, INFINITE); EndDialog(dialog, work->ok && !Cancelled(work->cancel) ? IDOK : IDCANCEL); return TRUE; }
    if ((message == WM_COMMAND && LOWORD(wp) == IDCANCEL) || message == WM_CLOSE) {
        if (work) { SetEvent(work->cancel); EnableWindow(GetDlgItem(dialog, IDCANCEL), FALSE); } return TRUE;
    }
    return FALSE;
}
static void ShowReport(HWND owner, const ProfileList *profiles, int kind) {
    ReportWork *work = (ReportWork *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *work); INT_PTR result;
    if (!work) return; work->profiles = *profiles; work->kind = kind;
    work->text = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, REPORT_CCH * sizeof(WCHAR));
    work->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!work->text || !work->cancel) goto done;
    result = Ui_Dialog(owner, IDD_PT_PROGRESS, ProgressProc, (LPARAM)work);
    if (result == IDOK) Ui_ReviewText(owner, kind == IDC_PT_HEALTH ? TR(L"Data health and current differences") : TR(L"Storage and backup estimate"),
                                    TR(L"Local report. This view can contain profile names, account IDs and paths; review it before sharing."), work->text, NULL);
    else if (!Cancelled(work->cancel)) Ui_Message(owner, MB_ICONERROR, TR(L"The local report could not be completed."));
done:
    if (work->thread) CloseHandle(work->thread); if (work->cancel) CloseHandle(work->cancel);
    if (work->text) HeapFree(GetProcessHeap(), 0, work->text); HeapFree(GetProcessHeap(), 0, work);
}
/* The migration and snapshot dialogs share the same profile selection. */
void ProfileTools_MigrationDialog(HWND owner, const ProfileList *profiles, BOOL importing);
void ProfileTools_SnapshotDialog(HWND owner, const Profile *profile);
typedef struct ToolsDialog { const ProfileList *profiles; const WCHAR *selected; } ToolsDialog;
static INT_PTR CALLBACK ToolsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp) {
    ToolsDialog *tools = (ToolsDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        int i, selected = 0; tools = (ToolsDialog *)lp; SetWindowLongPtrW(dialog, DWLP_USER, lp);
        for (i = 0; i < tools->profiles->count; i++) {
            SendDlgItemMessageW(dialog, IDC_PT_PROFILE, CB_ADDSTRING, 0, (LPARAM)tools->profiles->items[i].name);
            if (tools->selected && Core_EqualsI(tools->selected, tools->profiles->items[i].folder)) selected = i;
        }
        SendDlgItemMessageW(dialog, IDC_PT_PROFILE, CB_SETCURSEL, selected, 0); return TRUE;
    }
    if (message == WM_COMMAND && tools) {
        int command = LOWORD(wp); int selected = (int)SendDlgItemMessageW(dialog, IDC_PT_PROFILE, CB_GETCURSEL, 0, 0);
        const Profile *profile = selected >= 0 && selected < tools->profiles->count ? &tools->profiles->items[selected] : NULL;
        if (command == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
        if (command == IDC_PT_HEALTH || command == IDC_PT_SPACE) { ShowReport(dialog, tools->profiles, command); return TRUE; }
        if (command == IDC_PT_HISTORY) { OperationHistory_Show(dialog); return TRUE; }
        if (command == IDC_PT_EXPORT || command == IDC_PT_IMPORT) { ProfileTools_MigrationDialog(dialog, tools->profiles, command == IDC_PT_IMPORT); if (command == IDC_PT_IMPORT) EndDialog(dialog, IDOK); return TRUE; }
        if (command == IDC_PT_SNAPSHOTS && profile) { ProfileTools_SnapshotDialog(dialog, profile); return TRUE; }
        if (command == IDC_PT_USAGE && profile) {
            WCHAR path[LONG_PATH_CCH], *text; char *raw; DWORD length = 0;
            text = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, REPORT_CCH * sizeof(WCHAR));
            raw = profile->storageDir[0] && Join(profile->storageDir, L"plan-usage-history.json", path, ARRAYSIZE(path)) ? Util_ReadFile(path, USAGE_MAX, FALSE, &length) : NULL;
            if (text && raw && ProfileTools_UsageText(raw, length, text, REPORT_CCH))
                Ui_ReviewText(dialog, TR(L"Local usage history"), TR(L"The selected profile is the file scope; each sample names an organization. Historical account ownership is not inferred. Values are local records, not a live allowance."), text, NULL);
            else Ui_Message(dialog, MB_ICONINFORMATION, TR(L"No readable version 2 local usage history was found for this profile."));
            if (raw) HeapFree(GetProcessHeap(), 0, raw); if (text) HeapFree(GetProcessHeap(), 0, text); return TRUE;
        }
    }
    return FALSE;
}
void ProfileTools_Show(HWND owner, const ProfileList *profiles, const WCHAR *selectedFolder) {
    ToolsDialog tools; ProfileList *copy = (ProfileList *)HeapAlloc(GetProcessHeap(), 0, sizeof *copy);
    if (!copy || !profiles) { if (copy) HeapFree(GetProcessHeap(), 0, copy); return; }
    *copy = *profiles; tools.profiles = copy; tools.selected = selectedFolder;
    Ui_Dialog(owner, IDD_PROFILE_TOOLS, ToolsProc, (LPARAM)&tools); HeapFree(GetProcessHeap(), 0, copy);
}
