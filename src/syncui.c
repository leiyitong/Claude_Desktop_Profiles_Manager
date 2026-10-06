/*
 * What the user does with sessions sent between profiles (sessionsync.c):
 * merge every profile's, overwrite others with one profile's, share or copy the
 * sessions chosen in the sessions view, export sessions to an archive and
 * import one. The profiles taking part are chosen in one dialog (IDD_SYNC),
 * which says what will happen; what was done is said once it is.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <shlobj.h>

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
