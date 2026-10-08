/*
 * What the user does with sessions sent between profiles (sessionsync.c):
 * merge every profile's, overwrite others with one profile's, share or copy the
 * sessions chosen in the sessions view, export sessions to an archive and
 * import one. The profiles taking part are chosen in one dialog (IDD_SYNC),
 * which says what will happen; what was done is said once it is. Also a list
 * of sessions recovered from the vault (sessionvault.c, IDD_RESTORE), the
 * conversations no profile lists cleaned up and Claude Code's folder copied
 * (sessionpurge.c, IDD_PURGE).
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <shlobj.h>

#define UNIX_EPOCH_TICKS      116444736000000000ULL
#define TICKS_PER_MILLISECOND (TICKS_PER_SECOND / 1000)

#define ARCHIVE_EXTENSION L"zip"
#define NAMES_CCH         (MAX_PROFILES * (LABEL_CCH + 16))
#define REPORT_CCH        (4 * LONG_PATH_CCH)

typedef enum SyncKind { SYNC_UI_MERGE, SYNC_UI_OVERWRITE, SYNC_UI_SHARE, SYNC_UI_COPY, SYNC_UI_IMPORT } SyncKind;

typedef struct SyncDialog {
    SyncKind           kind;
    const ProfileList *profiles;
    DWORD              takers;          /* the profiles whose entries can take sessions */
    DWORD              chosen;          /* checked at first; once it closed, the profiles chosen */
    int                source;          /* the profile the sessions come from, never a target; -1 for none */
    BOOL               exact;           /* overwrite: what the source does not list is taken out of the others */
    const WCHAR       *archive;         /* import: the archive's name */
    HWND               rows;            /* the profiles as check boxes (in the view it scrolls in, which has its id) */
    int                rowProfile[MAX_PROFILES];
    int                rowCount;
    int                sourceProfile[MAX_PROFILES];   /* overwrite: each item's profile in the From box */
    BOOL               filling;
} SyncDialog;

static int BitCount(DWORD bits)
{
    int n = 0;
    for (; bits; bits &= bits - 1) n++;
    return n;
}

/* The profiles of `bits` by name: "Personal", "Personal and Work",
 * "Personal, Work and Test". */
static void NamesOf(const ProfileList *profiles, DWORD bits, WCHAR *out, size_t cch)
{
    int p, total = BitCount(bits), seen = 0;
    out[0] = 0;
    for (p = 0; p < profiles->count; p++) {
        if (!(bits & (1u << p))) continue;
        if (seen) StringCchCatW(out, cch, seen == total - 1 ? TR(L" and ") : TR(L", "));
        StringCchCatW(out, cch, profiles->items[p].name);
        seen++;
    }
}

/* ----------------------------------------------------------- the dialog */

static const WCHAR *Title(SyncKind kind)
{
    switch (kind) {
    case SYNC_UI_MERGE:  return TR(L"Merge all sessions");
    case SYNC_UI_OVERWRITE: return TR(L"Overwrite sessions");
    case SYNC_UI_SHARE:  return TR(L"Share sessions");
    case SYNC_UI_COPY:   return TR(L"Copy sessions");
    default:             return TR(L"Import sessions");
    }
}

static const WCHAR *ActionCaption(SyncKind kind)
{
    switch (kind) {
    case SYNC_UI_MERGE:  return TR(L"Merge");
    case SYNC_UI_OVERWRITE: return TR(L"Overwrite");
    case SYNC_UI_SHARE:  return TR(L"Share");
    case SYNC_UI_COPY:   return TR(L"Copy");
    default:             return TR(L"Import");
    }
}

/* What the dialog will do, above the profiles. */
static void Explain(HWND dialog, const SyncDialog *state)
{
    WCHAR text[1024 + MAX_PATH];
    switch (state->kind) {
    case SYNC_UI_MERGE:
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"Each profile checked gets the sessions the others list, in their latest state. "
                                                 L"A session deleted in a profile stays deleted there."));
        break;
    case SYNC_UI_OVERWRITE:
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"The profiles checked get every session of the profile chosen, in its state there, "
                                                 L"even the ones they deleted."));
        break;
    case SYNC_UI_SHARE:
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"The sessions selected will also be listed in the profiles checked: "
                                                 L"the same conversation, which goes on from any of them."));
        break;
    case SYNC_UI_COPY:
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"Each profile checked gets its own copy of the sessions selected: "
                                                 L"a new conversation, which goes on apart from the original."));
        break;
    default:
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"The sessions of \x201C%s\x201D will be listed in the profiles checked. "
                                                   L"Conversations already on this PC are kept as they are."),
                         state->archive ? state->archive : L"");
        break;
    }
    SetDlgItemTextW(dialog, IDC_Y_TEXT, text);
}

/* The profiles that can be chosen: those that take sessions, the source
 * apart; one open now gets them once it closes. */
static void FillRows(SyncDialog *state)
{
    WCHAR text[LABEL_CCH + 128];
    LVITEMW item;
    int p;
    state->filling = TRUE;
    ListView_DeleteAllItems(state->rows);
    state->rowCount = 0;
    for (p = 0; p < state->profiles->count; p++) {
        const Profile *profile = &state->profiles->items[p];
        if (!(state->takers & (1u << p)) || p == state->source) continue;
        if (Claude_IsRunning(profile)) StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (open now: gets them once it closes)"), profile->name);
        else StringCchCopyW(text, ARRAYSIZE(text), profile->name);
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = state->rowCount;
        item.pszText = text;
        ListView_InsertItem(state->rows, &item);
        ListView_SetCheckState(state->rows, state->rowCount, (state->chosen & (1u << p)) != 0);
        state->rowProfile[state->rowCount++] = p;
    }
    state->filling = FALSE;
}

static void ReadChosen(HWND dialog, SyncDialog *state)
{
    int i;
    state->chosen = 0;
    for (i = 0; i < state->rowCount; i++)
        if (ListView_GetCheckState(state->rows, i)) state->chosen |= 1u << state->rowProfile[i];
    EnableWindow(GetDlgItem(dialog, IDOK), state->chosen != 0);
}

/* Overwrite: the profiles in the From box, the source selected. */
static void FillSources(HWND dialog, SyncDialog *state)
{
    HWND box = GetDlgItem(dialog, IDC_Y_FROM);
    int p, count = 0;
    for (p = 0; p < state->profiles->count; p++) {
        if (!(state->takers & (1u << p))) continue;
        SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)state->profiles->items[p].name);
        if (p == state->source) SendMessageW(box, CB_SETCURSEL, (WPARAM)count, 0);
        state->sourceProfile[count++] = p;
    }
}

static INT_PTR CALLBACK SyncProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SyncDialog *state = (SyncDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    LVCOLUMNW column;
    HWND list;

    switch (message) {
    case WM_INITDIALOG:
        state = (SyncDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, Title(state->kind));
        Explain(dialog, state);
        SetDlgItemTextW(dialog, IDOK, ActionCaption(state->kind));
        SetDlgItemTextW(dialog, IDC_Y_TO_LABEL, state->kind == SYNC_UI_MERGE ? TR(L"&Merge these profiles:") : TR(L"&To these profiles:"));
        /* What the dialog does not offer leaves no empty row (Theme_FitDialog closes it). */
        if (state->kind == SYNC_UI_OVERWRITE) {
            FillSources(dialog, state);
        } else {
            ShowWindow(GetDlgItem(dialog, IDC_Y_FROM_LABEL), SW_HIDE);
            ShowWindow(GetDlgItem(dialog, IDC_Y_FROM), SW_HIDE);
            ShowWindow(GetDlgItem(dialog, IDC_Y_EXACT), SW_HIDE);
        }
        list = state->rows = GetDlgItem(dialog, IDC_Y_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(list, 0, &column);   /* the view it scrolls in gives it the list's width */
        FillRows(state);
        Theme_SmoothView(list);
        ReadChosen(dialog, state);
        return TRUE;

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
        switch (LOWORD(wp)) {
        case IDC_Y_FROM:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT i = SendDlgItemMessageW(dialog, IDC_Y_FROM, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < MAX_PROFILES) {
                    ReadChosen(dialog, state);
                    /* The source chosen before takes part again; the new one gives. */
                    if (state->source >= 0) state->chosen |= 1u << state->source;
                    state->source = state->sourceProfile[i];
                    state->chosen &= ~(1u << state->source);
                    FillRows(state);
                    ReadChosen(dialog, state);
                }
            }
            return TRUE;
        case IDOK:
            ReadChosen(dialog, state);
            if (!state->chosen) return TRUE;
            state->exact = state->kind == SYNC_UI_OVERWRITE && IsDlgButtonChecked(dialog, IDC_Y_EXACT) == BST_CHECKED;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* The dialog of `kind`; FALSE when it was cancelled. */
static BOOL ChooseProfiles(HWND owner, SyncDialog *state)
{
    return Ui_Dialog(owner, IDD_SYNC, SyncProc, (LPARAM)state) == IDOK && state->chosen;
}

/* ------------------------------------------------------------- reports */

static void AddLine(WCHAR *text, size_t cch, const WCHAR *line)
{
    if (text[0]) StringCchCatW(text, cch, L"\n");
    StringCchCatW(text, cch, line);
}

static void AddCount(WCHAR *text, size_t cch, const WCHAR *format, int count)
{
    WCHAR line[256];
    if (!count) return;
    StringCchPrintfW(line, ARRAYSIZE(line), format, count);
    AddLine(text, cch, line);
}

/* What was done, and what was not: added, updated, removed, left as it
 * was, waiting for its profile to close, left out, failed; `first`, when
 * given, opens it. */
static void ShowReport(HWND owner, const ProfileList *profiles, const SyncReport *report, const WCHAR *first)
{
    WCHAR text[REPORT_CCH], line[LONG_PATH_CCH + 256], names[NAMES_CCH];
    text[0] = 0;
    if (first) StringCchCopyW(text, ARRAYSIZE(text), first);
    AddCount(text, ARRAYSIZE(text), TR(L"Sessions added: %d"), report->added);
    AddCount(text, ARRAYSIZE(text), TR(L"Sessions updated: %d"), report->updated);
    AddCount(text, ARRAYSIZE(text), TR(L"Sessions removed: %d"), report->removed);
    AddCount(text, ARRAYSIZE(text), TR(L"Left as they were (newer or deleted there): %d"), report->skipped);
    if (report->waiting) {
        NamesOf(profiles, report->waiting, names, ARRAYSIZE(names));
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Waiting for %s to close: the rest is done then."), names);
        AddLine(text, ARRAYSIZE(text), line);
    }
    if (report->unavailable) {
        NamesOf(profiles, report->unavailable, names, ARRAYSIZE(names));
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Left out, not signed in to Claude yet: %s"), names);
        AddLine(text, ARRAYSIZE(text), line);
    }
    if (report->failed) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Changes that could not be made: %d. The first:"), report->failed);
        AddLine(text, ARRAYSIZE(text), line);
        AddLine(text, ARRAYSIZE(text), report->error);
    }
    if (report->backup[0]) {
        AddLine(text, ARRAYSIZE(text), TR(L"What was replaced or removed is kept in:"));
        AddLine(text, ARRAYSIZE(text), report->backup);
    }
    if (!report->added && !report->updated && !report->removed && !report->skipped && !report->failed && !report->waiting &&
        !report->unavailable)
        AddLine(text, ARRAYSIZE(text), TR(L"Nothing needed changing."));
    Ui_Message(owner, report->failed ? MB_ICONWARNING : MB_ICONINFORMATION, L"%s", text);
}

/* ------------------------------------------------------------- sessions */

/* Every profile's sessions read now, the user told when they cannot be. */
static BOOL LoadSessions(HWND owner, const ProfileList *profiles, SessionSet *set)
{
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    BOOL ok = SessionStore_LoadProfiles(set, profiles);
    SetCursor(old);
    if (!ok) Ui_Message(owner, MB_ICONERROR, TR(L"Sessions could not be loaded."));
    return ok;
}

/* The profiles that can take sessions, two at least; else the user told. */
static BOOL EnoughTakers(HWND owner, const SessionSet *set, DWORD *takers)
{
    *takers = SessionSync_Takers(set);
    if (BitCount(*takers) >= 2) return TRUE;
    Ui_Message(owner, MB_ICONINFORMATION, TR(L"This needs at least two profiles signed in to Claude: "
                                             L"a profile keeps sessions only once it is signed in."));
    return FALSE;
}

BOOL SyncUi_Merge(HWND owner, const ProfileList *profiles)
{
    SessionSet set;
    SyncDialog dialog;
    SyncReport report;
    DWORD takers;
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    if (!EnoughTakers(owner, &set, &takers)) {
        SessionStore_Free(&set);
        return FALSE;
    }
    SessionStore_Free(&set);
    ZeroMemory(&dialog, sizeof dialog);
    dialog.kind = SYNC_UI_MERGE;
    dialog.profiles = profiles;
    dialog.takers = dialog.chosen = takers;
    dialog.source = -1;
    if (!ChooseProfiles(owner, &dialog)) return FALSE;
    /* Read again: the dialog may have been open a while. */
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    ZeroMemory(&report, sizeof report);
    SessionSync_Merge(&set, dialog.chosen, &report);
    SessionStore_Free(&set);
    ShowReport(owner, profiles, &report, NULL);
    return TRUE;
}

BOOL SyncUi_Overwrite(HWND owner, const ProfileList *profiles, const WCHAR *selected)
{
    SessionSet set;
    SyncDialog dialog;
    SyncReport report;
    DWORD takers;
    int p;
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    if (!EnoughTakers(owner, &set, &takers)) {
        SessionStore_Free(&set);
        return FALSE;
    }
    SessionStore_Free(&set);
    ZeroMemory(&dialog, sizeof dialog);
    dialog.kind = SYNC_UI_OVERWRITE;
    dialog.profiles = profiles;
    dialog.takers = takers;
    dialog.source = -1;
    p = selected ? Profiles_Find(profiles, selected) : -1;
    if (p >= 0 && (takers & (1u << p))) dialog.source = p;
    for (p = 0; p < profiles->count && dialog.source < 0; p++)
        if (takers & (1u << p)) dialog.source = p;
    dialog.chosen = takers & ~(1u << dialog.source);
    if (!ChooseProfiles(owner, &dialog)) return FALSE;
    if (dialog.exact && !Ui_Ask(owner, IDI_WARNING,
                                TR(L"The sessions the source does not list will be taken out of the profiles checked. "
                                   L"Their entries are kept in a backup first, and their conversations stay on this PC.\n\nOverwrite anyway?"),
                                TR(L"Overwrite"), TR(L"Cancel"), TRUE))
        return FALSE;
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    ZeroMemory(&report, sizeof report);
    if (!SessionSync_Overwrite(&set, dialog.source, dialog.chosen, dialog.exact, &report) && !report.failed) {
        report.failed++;
        StringCchPrintfW(report.error, ARRAYSIZE(report.error), TR(L"\x201C%s\x201D is not signed in to Claude yet: sign in there first."),
                         profiles->items[dialog.source].name);
    }
    SessionStore_Free(&set);
    ShowReport(owner, profiles, &report, NULL);
    return TRUE;
}

BOOL SyncUi_ShareOrCopy(HWND owner, const SessionSet *set, int from, const int *rows, int rowCount, BOOL copy)
{
    SyncDialog dialog;
    SyncReport report;
    DWORD takers = SessionSync_Takers(set) & ~(from >= 0 ? 1u << from : 0);
    if (!rowCount) return FALSE;
    if (!takers) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"This needs at least two profiles signed in to Claude: "
                                                 L"a profile keeps sessions only once it is signed in."));
        return FALSE;
    }
    ZeroMemory(&dialog, sizeof dialog);
    dialog.kind = copy ? SYNC_UI_COPY : SYNC_UI_SHARE;
    dialog.profiles = &set->profiles;
    dialog.takers = takers;
    dialog.chosen = BitCount(takers) == 1 ? takers : 0;   /* one to choose: it is */
    dialog.source = from;
    if (!ChooseProfiles(owner, &dialog)) return FALSE;
    ZeroMemory(&report, sizeof report);
    if (copy) {
        if (SessionSync_Copy(owner, set, from, rows, rowCount, dialog.chosen, &report) == COPY_CANCELLED && !report.added && !report.waiting)
            return FALSE;   /* cancelled in Windows' progress: nothing to say */
    } else {
        SessionSync_Share(set, from, rows, rowCount, dialog.chosen, &report);
    }
    ShowReport(owner, &set->profiles, &report, NULL);
    return TRUE;
}

/* --------------------------------------------------------------- archives */

/* The archive the user chose to write (`save`) or read, in `path`; FALSE
 * when none was (an error said). */
static BOOL ChooseArchive(HWND owner, BOOL save, WCHAR *path, size_t cch)
{
    IFileDialog *dialog = NULL;
    IShellItem *result = NULL;
    COMDLG_FILTERSPEC filter = { TR(L"Session archive (*.zip)"), L"*." ARCHIVE_EXTENSION };
    FILEOPENDIALOGOPTIONS options = 0;
    PWSTR chosen = NULL;
    BOOL ok = FALSE;
    HRESULT hr = CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog,
                                  (void **)&dialog);
    if (FAILED(hr)) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The file could not be chosen (error 0x%08lX)."), (unsigned long)hr);
        return FALSE;
    }
    IFileDialog_SetTitle(dialog, save ? TR(L"Export sessions") : TR(L"Import sessions"));
    IFileDialog_SetFileTypes(dialog, 1, &filter);
    IFileDialog_SetDefaultExtension(dialog, ARCHIVE_EXTENSION);
    if (save) {
        WCHAR name[MAX_PATH];
        SYSTEMTIME now;
        GetLocalTime(&now);
        StringCchPrintfW(name, ARRAYSIZE(name), L"%s %04u-%02u-%02u." ARCHIVE_EXTENSION, TR(L"Claude sessions"), now.wYear, now.wMonth, now.wDay);
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

BOOL SyncUi_Export(HWND owner, const SessionSet *set, int profile, const int *rows, int rowCount)
{
    WCHAR path[LONG_PATH_CCH], error[LONG_PATH_CCH], text[LONG_PATH_CCH + 256];
    HCURSOR old;
    int exported = 0;
    BOOL ok;
    if (!rowCount || !ChooseArchive(owner, TRUE, path, ARRAYSIZE(path))) return FALSE;
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    ok = SessionSync_Export(set, profile, rows, rowCount, path, &exported, error, ARRAYSIZE(error));
    SetCursor(old);
    if (!ok) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The sessions could not be exported. %s"), error);
        return FALSE;
    }
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Sessions exported: %d"), exported);
    AddLine(text, ARRAYSIZE(text), path);
    Ui_Message(owner, MB_ICONINFORMATION, L"%s", text);
    return TRUE;
}

BOOL SyncUi_ExportProfiles(HWND owner, const ProfileList *profiles, DWORD chosen)
{
    SessionSet set;
    int *rows, r, p, count = 0, only = -1;
    BOOL exported = FALSE;
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    if (BitCount(chosen) == 1)
        for (p = 0; p < set.profiles.count; p++)
            if (chosen & (1u << p)) only = p;
    rows = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)max(set.rowCount, 1) * sizeof *rows);
    if (!rows) {
        SessionStore_Free(&set);
        Ui_Message(owner, MB_ICONERROR, TR(L"Sessions could not be loaded."));
        return FALSE;
    }
    for (r = 0; r < set.rowCount; r++)
        for (p = 0; p < set.profiles.count; p++)
            if ((chosen & (1u << p)) && set.rows[r].entry[p] >= 0 && !set.entries[set.rows[r].entry[p]].pendingRemove) {
                rows[count++] = r;
                break;
            }
    if (count) exported = SyncUi_Export(owner, &set, only, rows, count);
    else Ui_Message(owner, MB_ICONINFORMATION, TR(L"These profiles list no session to export."));
    HeapFree(GetProcessHeap(), 0, rows);
    SessionStore_Free(&set);
    return exported;
}

BOOL SyncUi_Import(HWND owner, const ProfileList *profiles, DWORD chosen)
{
    WCHAR path[LONG_PATH_CCH], first[256];
    const WCHAR *name;
    SessionSet set;
    SyncDialog dialog;
    SyncReport report;
    DWORD takers;
    HCURSOR old;
    int sessions = 0;
    BOOL ok;
    if (!ChooseArchive(owner, FALSE, path, ARRAYSIZE(path))) return FALSE;
    if (!SessionSync_IsArchive(path)) {
        Ui_Message(owner, MB_ICONWARNING, TR(L"This file is not a session archive of " APP_NAME L"."));
        return FALSE;
    }
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    takers = SessionSync_Takers(&set);
    SessionStore_Free(&set);
    if (!takers) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"No profile is signed in to Claude yet: a profile keeps sessions only once it is signed in."));
        return FALSE;
    }
    name = wcsrchr(path, L'\\');
    ZeroMemory(&dialog, sizeof dialog);
    dialog.kind = SYNC_UI_IMPORT;
    dialog.profiles = profiles;
    dialog.takers = takers;
    dialog.chosen = (chosen & takers) ? chosen & takers : takers;
    dialog.source = -1;
    dialog.archive = name ? name + 1 : path;
    if (!ChooseProfiles(owner, &dialog) || !LoadSessions(owner, profiles, &set)) return FALSE;
    ZeroMemory(&report, sizeof report);
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    ok = SessionSync_Import(&set, path, dialog.chosen, &sessions, &report);
    SetCursor(old);
    SessionStore_Free(&set);
    if (!ok) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The sessions could not be imported. %s"), report.error);
        return FALSE;
    }
    StringCchPrintfW(first, ARRAYSIZE(first), TR(L"Sessions in the archive: %d"), sessions);
    ShowReport(owner, profiles, &report, first);
    return TRUE;
}

/* ------------------------------------------------------------- the vault */

/* A time (ms since 1970) as the user's short date and time. */
static void FormatWhen(ULONGLONG ms, WCHAR *out, size_t cch)
{
    ULONGLONG ticks = ms * TICKS_PER_MILLISECOND + UNIX_EPOCH_TICKS;
    FILETIME utc;
    SYSTEMTIME universal, local;
    WCHAR date[64], time[64];
    utc.dwLowDateTime = (DWORD)ticks;
    utc.dwHighDateTime = (DWORD)(ticks >> 32);
    out[0] = 0;
    if (!FileTimeToSystemTime(&utc, &universal) || !SystemTimeToTzSpecificLocalTime(NULL, &universal, &local) ||
        !GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, ARRAYSIZE(date), NULL) ||
        !GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, NULL, time, ARRAYSIZE(time)))
        return;
    StringCchPrintfW(out, cch, L"%s  %s", date, time);
}

typedef struct RestoreDialog {
    const ProfileList *profiles;
    int                profile;          /* the one chosen */
    WCHAR              list[FOLDER_CCH];   /* its list in the vault */
    VaultVersion      *versions;
    int                count, chosen;
    HWND               rows;             /* the versions (in the view it scrolls in, which has its id) */
} RestoreDialog;

/* The versions of the list of the profile chosen, newest first and selected. */
static void FillVersions(HWND dialog, RestoreDialog *state)
{
    WCHAR when[128], text[256];
    LVITEMW item;
    int i;
    ListView_DeleteAllItems(state->rows);
    state->count = SessionVault_ListName(state->profiles, state->profile, state->list, ARRAYSIZE(state->list))
                       ? SessionVault_Versions(state->list, state->versions, VAULT_VERSIONS_SHOWN) : 0;
    state->chosen = state->count ? 0 : -1;
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    if (!state->count) {
        item.pszText = (LPWSTR)TR(L"No session list kept yet");
        ListView_InsertItem(state->rows, &item);
    }
    for (i = 0; i < state->count; i++) {
        FormatWhen(state->versions[i].time, when, ARRAYSIZE(when));
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s \x00B7 sessions: %d"), when[0] ? when : state->versions[i].name, state->versions[i].sessions);
        item.iItem = i;
        item.pszText = text;
        ListView_InsertItem(state->rows, &item);
    }
    if (state->count) ListView_SetItemState(state->rows, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    EnableWindow(GetDlgItem(dialog, IDOK), state->count > 0);
}

static INT_PTR CALLBACK RestoreProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    RestoreDialog *state = (RestoreDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    LVCOLUMNW column;
    int p;
    switch (message) {
    case WM_INITDIALOG:
        state = (RestoreDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_R_TEXT, TR(L"The sessions this version lists come back in the profile, as they were then. "
                                               L"Sessions deleted in Claude stay deleted."));
        for (p = 0; p < state->profiles->count; p++) {
            SendDlgItemMessageW(dialog, IDC_R_PROFILE, CB_ADDSTRING, 0, (LPARAM)state->profiles->items[p].name);
            if (p == state->profile) SendDlgItemMessageW(dialog, IDC_R_PROFILE, CB_SETCURSEL, (WPARAM)p, 0);
        }
        state->rows = GetDlgItem(dialog, IDC_R_LIST);
        ListView_SetExtendedListViewStyle(state->rows, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(state->rows, 0, &column);   /* the view it scrolls in gives it the list's width */
        FillVersions(dialog, state);
        Theme_SmoothView(state->rows);
        return TRUE;

    case WM_NOTIFY:
        if (state && ((const NMHDR *)lp)->hwndFrom == state->rows) {
            const NMHDR *header = (const NMHDR *)lp;
            int row = ListView_GetNextItem(state->rows, -1, LVNI_SELECTED);
            if (header->code == LVN_ITEMCHANGED && row >= 0 && row < state->count) state->chosen = row;
            if (header->code == NM_DBLCLK && ((const NMITEMACTIVATE *)lp)->iItem >= 0 && state->count) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_R_PROFILE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT chosen = SendDlgItemMessageW(dialog, IDC_R_PROFILE, CB_GETCURSEL, 0, 0);
                if (chosen >= 0 && chosen < state->profiles->count) {
                    state->profile = (int)chosen;
                    FillVersions(dialog, state);
                }
            }
            return TRUE;
        case IDOK:
            if (state->chosen < 0 || state->chosen >= state->count) return TRUE;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL SyncUi_Restore(HWND owner, const ProfileList *profiles, const WCHAR *selected)
{
    RestoreDialog dialog;
    SyncReport report;
    const Profile *p;
    BOOL ok;
    if (!profiles->count) return FALSE;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.profiles = profiles;
    dialog.profile = selected ? Profiles_Find(profiles, selected) : -1;
    if (dialog.profile < 0) dialog.profile = max(Profiles_Find(profiles, STOCK_FOLDER), 0);
    if ((dialog.versions = (VaultVersion *)HeapAlloc(GetProcessHeap(), 0, VAULT_VERSIONS_SHOWN * sizeof *dialog.versions)) == NULL) return FALSE;
    if (Ui_Dialog(owner, IDD_RESTORE, RestoreProc, (LPARAM)&dialog) != IDOK || dialog.chosen < 0) {
        HeapFree(GetProcessHeap(), 0, dialog.versions);
        return FALSE;
    }
    p = &profiles->items[dialog.profile];
    if (Claude_IsRunning(p)) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first: while it runs, Claude keeps its list of sessions and would write it back."),
                   p->name);
        HeapFree(GetProcessHeap(), 0, dialog.versions);
        return FALSE;
    }
    ZeroMemory(&report, sizeof report);
    ok = SessionVault_Restore(profiles, dialog.profile, dialog.list, dialog.versions[dialog.chosen].name, &report);
    HeapFree(GetProcessHeap(), 0, dialog.versions);
    if (!ok && report.unavailable) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"\x201C%s\x201D is not signed in to Claude yet: sign in there first."), p->name);
        return FALSE;
    }
    if (!ok && !report.failed) {
        Ui_Message(owner, MB_ICONERROR, TR(L"Sessions could not be loaded."));
        return FALSE;
    }
    ShowReport(owner, profiles, &report, NULL);
    return TRUE;
}

/* The profiles' sessions made the same right away, and what that did said. */
BOOL SyncUi_KeepSame(HWND owner, const ProfileList *profiles)
{
    WCHAR names[NAMES_CCH], first[NAMES_CCH + 128];
    SyncReport report;
    DWORD group = SessionVault_Group(profiles);
    HCURSOR old;
    if (!group) return FALSE;
    ZeroMemory(&report, sizeof report);
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    SessionVault_Keep(profiles, group, VAULT_GROUP_LIST, TRUE, &report);
    SetCursor(old);
    NamesOf(profiles, group, names, ARRAYSIZE(names));
    StringCchPrintfW(first, ARRAYSIZE(first), TR(L"The sessions of %s are kept the same from now on."), names);
    ShowReport(owner, profiles, &report, first);
    return TRUE;
}

/* ---------------------------------------------------------- cleaning up */

/* Claude Code's folder copied beside it, as <name>_<date>; `ask` first, and
 * say where it went. FALSE when no copy was made whole. */
static BOOL CopyCodeFolder(HWND owner, BOOL ask)
{
    WCHAR code[MAX_PATH], target[MAX_PATH], text[2 * MAX_PATH + 128];
    DWORD error = 0;
    CopyResult result;
    if (!SessionPurge_CodeFolder(code, ARRAYSIZE(code)) || !Util_DirExists(code)) {
        Ui_Message(owner, MB_ICONWARNING, TR(L"%s could not be opened (error %lu)."), code, (DWORD)ERROR_PATH_NOT_FOUND);
        return FALSE;
    }
    if (!SessionPurge_BackupName(target, ARRAYSIZE(target))) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The path is too long."));
        return FALSE;
    }
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Copy %s\nto %s?"), code, target);
    if (ask && !Ui_Ask(owner, IDI_QUESTION, text, TR(L"Copy"), TR(L"Cancel"), FALSE)) return FALSE;
    result = SessionPurge_BackUp(owner, target, &error);
    if (result == COPY_MADE) {
        if (ask) Ui_Message(owner, MB_ICONINFORMATION, TR(L"Copied to:\n%s"), target);
        return TRUE;
    }
    if (result == COPY_FAILED) Ui_Message(owner, MB_ICONWARNING, TR(L"%s could not be copied whole (error %lu)."), code, error);
    return FALSE;
}

BOOL SyncUi_BackUpCode(HWND owner)
{
    return CopyCodeFolder(owner, TRUE);
}

typedef struct PurgeDialog {
    const PurgeItem *items;
    int              count;
    BOOL            *checked;
    BOOL             backUp;
    HWND             rows;           /* in the view it scrolls in, which has its id */
    BOOL             filling;
} PurgeDialog;

static void ReadChecked(HWND dialog, PurgeDialog *state)
{
    int i, chosen = 0;
    for (i = 0; i < state->count; i++)
        if ((state->checked[i] = ListView_GetCheckState(state->rows, i)) != FALSE) chosen++;
    EnableWindow(GetDlgItem(dialog, IDOK), chosen > 0);
}

/* A conversation's row: what it is, its title (its id without one), when it
 * was last written, its size. */
static void PurgeRow(const PurgeItem *item, WCHAR *out, size_t cch)
{
    WCHAR when[128];
    FormatWhen(item->written, when, ARRAYSIZE(when));
    StringCchPrintfW(out, cch, L"%s  \x00B7  %s  \x00B7  %s  \x00B7  %.1f MB", item->kind == PURGE_DELETED ? TR(L"Deleted in Claude") : TR(L"In no list"),
                     item->title[0] ? item->title : item->id, when, (double)item->bytes / (1024.0 * 1024.0));
}

static INT_PTR CALLBACK PurgeProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    PurgeDialog *state = (PurgeDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    WCHAR text[SESSION_TITLE_CCH + MAX_PATH];
    LVCOLUMNW column;
    LVITEMW item;
    int i;
    switch (message) {
    case WM_INITDIALOG:
        state = (PurgeDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_C_TEXT, TR(L"These conversations are on this PC, but no profile lists them, so a restore could bring them back. "
                                               L"Deleting them makes sure nothing does.\nChecked: deleted in Claude. Unchecked: in no list, "
                                               L"made in a terminal for example."));
        CheckDlgButton(dialog, IDC_C_BACKUP, BST_CHECKED);
        state->rows = GetDlgItem(dialog, IDC_C_LIST);
        ListView_SetExtendedListViewStyle(state->rows, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(state->rows, 0, &column);
        state->filling = TRUE;
        for (i = 0; i < state->count; i++) {
            PurgeRow(&state->items[i], text, ARRAYSIZE(text));
            ZeroMemory(&item, sizeof item);
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = text;
            ListView_InsertItem(state->rows, &item);
            ListView_SetCheckState(state->rows, i, state->items[i].kind == PURGE_DELETED);
        }
        state->filling = FALSE;
        Theme_SmoothView(state->rows);
        ReadChecked(dialog, state);
        return TRUE;

    case WM_NOTIFY:
        if (state && ((const NMHDR *)lp)->hwndFrom == state->rows && ((const NMHDR *)lp)->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            if (!state->filling && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK))
                ReadChecked(dialog, state);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDOK:
            ReadChecked(dialog, state);
            state->backUp = IsDlgButtonChecked(dialog, IDC_C_BACKUP) == BST_CHECKED;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL SyncUi_Purge(HWND owner, const ProfileList *profiles)
{
    WCHAR error[LONG_PATH_CCH];
    PurgeDialog dialog;
    PurgeItem *items = NULL;
    RemoveResult result;
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    int count = SessionPurge_List(profiles, &items, error, ARRAYSIZE(error)), *chosen = NULL, n = 0, deleted = 0, i;
    SetCursor(old);
    if (count < 0) {
        Ui_Message(owner, MB_ICONERROR, L"%s", error);
        return FALSE;
    }
    if (count == 0) {
        Ui_Message(owner, MB_ICONINFORMATION,
                   TR(L"No conversation to clean up: each one on this PC is listed by a profile, kept to be recovered, in use, or new."));
        HeapFree(GetProcessHeap(), 0, items);
        return FALSE;
    }
    ZeroMemory(&dialog, sizeof dialog);
    dialog.items = items;
    dialog.count = count;
    dialog.checked = (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count * sizeof *dialog.checked);
    chosen = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *chosen);
    if (dialog.checked && chosen && Ui_Dialog(owner, IDD_PURGE, PurgeProc, (LPARAM)&dialog) == IDOK) {
        for (i = 0; i < count; i++)
            if (dialog.checked[i]) chosen[n++] = i;
        if (n && (!dialog.backUp || CopyCodeFolder(owner, FALSE))) {
            result = SessionPurge_Delete(owner, profiles, items, chosen, n, &deleted, error, ARRAYSIZE(error));
            if (result == REMOVE_DONE) Ui_Message(owner, MB_ICONINFORMATION, TR(L"Conversations moved to the Recycle Bin: %d"), deleted);
            else if (result == REMOVE_FAILED) Ui_Message(owner, MB_ICONWARNING, TR(L"The conversations could not be deleted. %s"), error);
        }
    }
    if (dialog.checked) HeapFree(GetProcessHeap(), 0, dialog.checked);
    if (chosen) HeapFree(GetProcessHeap(), 0, chosen);
    HeapFree(GetProcessHeap(), 0, items);
    return deleted > 0;
}
