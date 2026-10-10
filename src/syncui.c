/*
 * What the user does with sessions sent between profiles (sessionsync.c):
 * merge every profile's, copy or move every session of some profiles to
 * others, share or copy the sessions chosen in the sessions view, export
 * sessions to an archive and import one. The profiles taking part are chosen in one dialog (IDD_SYNC),
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

typedef enum SyncKind { SYNC_UI_MERGE, SYNC_UI_COPY_ALL, SYNC_UI_MOVE_ALL, SYNC_UI_SHARE, SYNC_UI_COPY, SYNC_UI_IMPORT } SyncKind;

typedef struct SyncDialog {
    SyncKind           kind;
    const ProfileList *profiles;
    DWORD              takers;          /* the profiles whose entries can take sessions */
    DWORD              chosen;          /* checked at first; once it closed, the profiles chosen */
    DWORD              sources;         /* the profiles the sessions come from, never targets */
    const WCHAR       *archive;         /* import: the archive's name */
    HWND               rows;            /* the profiles as check boxes (in the view it scrolls in, which has its id) */
    int                rowProfile[MAX_PROFILES];
    int                rowCount;
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
    case SYNC_UI_COPY_ALL: return TR(L"Copy all sessions");
    case SYNC_UI_MOVE_ALL: return TR(L"Move all sessions");
    case SYNC_UI_SHARE:  return TR(L"Share sessions");
    case SYNC_UI_COPY:   return TR(L"Copy sessions");
    default:             return TR(L"Import sessions");
    }
}

static const WCHAR *ActionCaption(SyncKind kind)
{
    switch (kind) {
    case SYNC_UI_MERGE:  return TR(L"Merge");
    case SYNC_UI_COPY_ALL: return TR(L"Copy");
    case SYNC_UI_MOVE_ALL: return TR(L"Move");
    case SYNC_UI_SHARE:  return TR(L"Share");
    case SYNC_UI_COPY:   return TR(L"Copy");
    default:             return TR(L"Import");
    }
}

/* What the dialog will do, above the profiles. */
static void Explain(HWND dialog, const SyncDialog *state)
{
    WCHAR text[1024 + MAX_PATH + NAMES_CCH], names[NAMES_CCH];
    NamesOf(state->profiles, state->sources, names, ARRAYSIZE(names));
    switch (state->kind) {
    case SYNC_UI_MERGE:
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"Each profile checked gets the sessions the others list, in their latest state. "
                                                 L"A session deleted in a profile stays deleted there."));
        break;
    case SYNC_UI_COPY_ALL:
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"The profiles checked get every session %s lists: the same conversations, "
                                                   L"which go on from any of them."), names);
        break;
    case SYNC_UI_MOVE_ALL:
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"The profiles checked get every session %s lists, which then leaves it. "
                                                   L"The conversations stay on this PC."), names);
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
        if (!(state->takers & (1u << p)) || (state->sources & (1u << p))) continue;
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
        case IDOK:
            ReadChosen(dialog, state);
            if (!state->chosen) return TRUE;
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
void SyncUi_ShowReport(HWND owner, const ProfileList *profiles, const SyncReport *report, const WCHAR *first)
{
    WCHAR text[REPORT_CCH], line[LONG_PATH_CCH + 256], names[NAMES_CCH];
    text[0] = 0;
    if (first) StringCchCopyW(text, ARRAYSIZE(text), first);
    AddCount(text, ARRAYSIZE(text), TR(L"Sessions added: %d"), report->added);
    AddCount(text, ARRAYSIZE(text), TR(L"Sessions updated: %d"), report->updated);
    AddCount(text, ARRAYSIZE(text), TR(L"Sessions removed: %d"), report->removed);
    AddCount(text, ARRAYSIZE(text), TR(L"Sessions two profiles went on with apart, now kept as two: %d"), report->forked);
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
        !report->unavailable && !report->forked)
        AddLine(text, ARRAYSIZE(text), TR(L"Nothing needed changing."));
    Ui_Message(owner, report->failed ? MB_ICONWARNING : MB_ICONINFORMATION, L"%s", text);
}

/* ----------------------------------------------------------- confirming */

BOOL SyncUi_Confirm(HWND owner, LPCWSTR icon, const WCHAR *question, BOOL backedUp, const WCHAR *button)
{
    WCHAR text[2048];
    StringCchCopyW(text, ARRAYSIZE(text), question);
    if (backedUp) {
        StringCchCatW(text, ARRAYSIZE(text), L"\n\n");
        StringCchCatW(text, ARRAYSIZE(text), TR(L"What it replaces is backed up first."));
    }
    return Ui_Ask(owner, icon ? icon : IDI_QUESTION, text, button, TR(L"Cancel"), FALSE);
}

BOOL SyncUi_ConfirmSessions(HWND owner, const WCHAR *question, const WCHAR *button)
{
    return SyncUi_Confirm(owner, NULL, question, TRUE, button);
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
    WCHAR names[NAMES_CCH], question[NAMES_CCH + 128];
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
    if (!ChooseProfiles(owner, &dialog)) return FALSE;
    NamesOf(profiles, dialog.chosen, names, ARRAYSIZE(names));
    StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Merge the sessions of %s?"), names);
    if (!SyncUi_ConfirmSessions(owner, question, TR(L"Merge"))) return FALSE;
    /* Read again: the dialog may have been open a while. */
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    ZeroMemory(&report, sizeof report);
    SessionSync_Merge(&set, dialog.chosen, &report);
    SessionStore_Free(&set);
    SyncUi_ShowReport(owner, profiles, &report, NULL);
    return TRUE;
}

BOOL SyncUi_CopyAll(HWND owner, const ProfileList *profiles, DWORD sources, BOOL move)
{
    WCHAR names[NAMES_CCH], targets[NAMES_CCH], question[3 * NAMES_CCH + 512];
    SessionSet set;
    SyncDialog dialog;
    SyncReport report;
    DWORD takers;
    int *rows, r, p, count = 0;
    sources &= profiles->count >= 32 ? (DWORD)-1 : (1u << profiles->count) - 1;
    if (!sources) return FALSE;
    for (p = 0; move && p < profiles->count; p++)
        if ((sources & (1u << p)) && profiles->items[p].syncGroup) {
            Ui_Message(owner, MB_ICONINFORMATION,
                       TR(L"The sessions of \x201C%s\x201D are kept the same as other profiles': taking them out of it would take them out "
                          L"of those too. Stop keeping its sessions the same first."),
                       profiles->items[p].name);
            return FALSE;
        }
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    takers = SessionSync_Takers(&set) & ~sources;
    SessionStore_Free(&set);
    if (!takers) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"This needs at least two profiles signed in to Claude: "
                                                 L"a profile keeps sessions only once it is signed in."));
        return FALSE;
    }
    ZeroMemory(&dialog, sizeof dialog);
    dialog.kind = move ? SYNC_UI_MOVE_ALL : SYNC_UI_COPY_ALL;
    dialog.profiles = profiles;
    dialog.takers = takers;
    dialog.sources = sources;
    dialog.chosen = BitCount(takers) == 1 ? takers : 0;   /* one to choose: it is */
    if (!ChooseProfiles(owner, &dialog)) return FALSE;
    NamesOf(profiles, sources, names, ARRAYSIZE(names));
    NamesOf(profiles, dialog.chosen, targets, ARRAYSIZE(targets));
    if (move)
        StringCchPrintfW(question, ARRAYSIZE(question),
                         TR(L"Move every session of %s to %s?\n\nThey are then taken out of %s."), names, targets, names);
    else
        StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Copy every session of %s to %s?"), names, targets);
    if (!SyncUi_ConfirmSessions(owner, question, move ? TR(L"Move") : TR(L"Copy")))
        return FALSE;
    if (!LoadSessions(owner, profiles, &set)) return FALSE;
    rows = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)max(set.rowCount, 1) * sizeof *rows);
    if (!rows) {
        SessionStore_Free(&set);
        Ui_Message(owner, MB_ICONERROR, TR(L"Sessions could not be loaded."));
        return FALSE;
    }
    for (r = 0; r < set.rowCount; r++)
        for (p = 0; p < set.profiles.count; p++)
            if ((sources & (1u << p)) && set.rows[r].entry[p] >= 0 && !set.entries[set.rows[r].entry[p]].pendingRemove) {
                rows[count++] = r;
                break;
            }
    ZeroMemory(&report, sizeof report);
    if (count) {
        HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        SessionSync_Share(&set, -1, rows, count, dialog.chosen, &report);
        if (move && !report.failed) SessionSync_Remove(&set, sources, rows, count, &report);
        SetCursor(old);
    }
    HeapFree(GetProcessHeap(), 0, rows);
    SessionStore_Free(&set);
    if (!count) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"%s lists no session yet."), names);
        return FALSE;
    }
    SyncUi_ShowReport(owner, profiles, &report, NULL);
    return TRUE;
}

BOOL SyncUi_ShareOrCopy(HWND owner, const SessionSet *set, int from, const int *rows, int rowCount, BOOL copy, int to)
{
    SyncDialog dialog;
    SyncReport report;
    DWORD takers = SessionSync_Takers(set) & ~(from >= 0 ? 1u << from : 0);
    if (!rowCount) return FALSE;
    if (to >= 0 && !(takers & (1u << to))) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"\x201C%s\x201D is not signed in to Claude yet: sign in there first."), set->profiles.items[to].name);
        return FALSE;
    }
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
    dialog.sources = from >= 0 ? 1u << from : 0;
    if (to >= 0) {
        /* Pasted into one profile, or one picked in a menu: a question instead of the choice. */
        WCHAR question[SESSION_TITLE_CCH + LABEL_CCH + 128];
        const WCHAR *verb = copy ? TR(L"Copy") : TR(L"Share");
        const SessionRow *row = &set->rows[rows[0]];
        const SessionEntry *entry = from >= 0 && row->entry[from] >= 0 ? &set->entries[row->entry[from]] : NULL;
        int p;
        for (p = 0; !entry && p < set->profiles.count; p++)
            if (row->entry[p] >= 0) entry = &set->entries[row->entry[p]];
        const WCHAR *title = entry && entry->title[0] ? entry->title : row->key, *name = set->profiles.items[to].name;
        if (rowCount == 1 && copy)
            StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\nThe copy goes on separately."), title, name);
        else if (rowCount == 1)
            StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Share \x201C%s\x201D with \x201C%s\x201D?\n\nBoth go on with the same conversation."),
                             title, name);
        else if (copy)
            StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Copy %d sessions to \x201C%s\x201D?\n\nThe copies go on separately."), rowCount, name);
        else
            StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Share %d sessions with \x201C%s\x201D?"), rowCount, name);
        if (!Ui_Ask(owner, IDI_QUESTION, question, verb, TR(L"Cancel"), FALSE)) return FALSE;
        dialog.chosen = 1u << to;
    } else if (!ChooseProfiles(owner, &dialog)) {
        return FALSE;
    }
    ZeroMemory(&report, sizeof report);
    if (copy) {
        if (SessionSync_Copy(owner, set, from, rows, rowCount, dialog.chosen, &report) == COPY_CANCELLED && !report.added && !report.waiting)
            return FALSE;   /* cancelled in Windows' progress: nothing to say */
    } else {
        SessionSync_Share(set, from, rows, rowCount, dialog.chosen, &report);
    }
    SyncUi_ShowReport(owner, &set->profiles, &report, NULL);
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
    WCHAR path[LONG_PATH_CCH], first[256], names[NAMES_CCH], question[NAMES_CCH + LONG_PATH_CCH + 128];
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
    dialog.archive = name ? name + 1 : path;
    if (!ChooseProfiles(owner, &dialog)) return FALSE;
    NamesOf(profiles, dialog.chosen, names, ARRAYSIZE(names));
    StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Import the sessions of %s into %s?"), dialog.archive, names);
    if (!SyncUi_ConfirmSessions(owner, question, TR(L"Import")) || !LoadSessions(owner, profiles, &set))
        return FALSE;
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
    SyncUi_ShowReport(owner, profiles, &report, first);
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
    WCHAR when[64], question[LABEL_CCH + 256];
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
    FormatWhen(dialog.versions[dialog.chosen].time, when, ARRAYSIZE(when));
    StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Recover the sessions of %s in \x201C%s\x201D?\n\nSessions deleted since stay deleted."),
                     when, p->name);
    if (!SyncUi_ConfirmSessions(owner, question, TR(L"Recover"))) {
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
    SyncUi_ShowReport(owner, profiles, &report, NULL);
    return TRUE;
}

/* ------------------------------------------------------- what to sync */

/* Each row of the dialog: what it syncs (SYNC_ITEM_*), as the list shows it. */
static const struct { DWORD item; const WCHAR *label; } kSyncItems[SYNC_ITEM_COUNT] = {
    { SYNC_ITEM_SESSIONS, L"Sessions: new ones, titles, archived and deleted ones" },
    { SYNC_ITEM_SIDEBAR, L"Sidebar: pins, groups, project order, filters" },
    { SYNC_ITEM_DETAILS, L"Each session's model, effort, side pane, unread mark and cost" },
    { SYNC_ITEM_APPEARANCE, L"Appearance: fonts, editor, zoom, spelling" },
    { SYNC_ITEM_LANGUAGE, L"Interface language" },
    { SYNC_ITEM_MODEL, L"Default model" },
    { SYNC_ITEM_SETTINGS, L"Settings: auto-archive, Cowork, Remote Control, recent folders" },
    { SYNC_ITEM_PERMISSIONS, L"Permissions: folders' permission modes, Cowork's trusted folders" },
};

typedef struct ItemsDialog {
    const WCHAR *text;
    DWORD        items;
    HWND         rows;
    BOOL         filling;
} ItemsDialog;

/* What the rows checked say; OK only with one at least. */
static void ReadItems(HWND dialog, ItemsDialog *state)
{
    int row;
    state->items = 0;
    for (row = 0; row < SYNC_ITEM_COUNT; row++)
        if (ListView_GetCheckState(state->rows, row)) state->items |= kSyncItems[row].item;
    EnableWindow(GetDlgItem(dialog, IDOK), state->items != 0);
}

static INT_PTR CALLBACK ItemsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ItemsDialog *state = (ItemsDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        LVCOLUMNW column;
        LVITEMW item;
        int row;
        state = (ItemsDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_I_TEXT, state->text);
        state->rows = GetDlgItem(dialog, IDC_I_LIST);
        ListView_SetExtendedListViewStyle(state->rows, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(state->rows, 0, &column);   /* the view it scrolls in gives it the list's width */
        state->filling = TRUE;
        for (row = 0; row < SYNC_ITEM_COUNT; row++) {
            ZeroMemory(&item, sizeof item);
            item.mask = LVIF_TEXT;
            item.iItem = row;
            item.pszText = (LPWSTR)TR(kSyncItems[row].label);
            ListView_InsertItem(state->rows, &item);
            ListView_SetCheckState(state->rows, row, (state->items & kSyncItems[row].item) != 0);
        }
        state->filling = FALSE;
        Theme_SmoothView(state->rows);
        ReadItems(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_I_NOTE);

    case WM_NOTIFY:
        if (state && ((const NMHDR *)lp)->hwndFrom == state->rows && ((const NMHDR *)lp)->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            if (!state->filling && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK))
                ReadItems(dialog, state);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wp) == IDOK) {
            ReadItems(dialog, state);
            if (state->items) EndDialog(dialog, IDOK);
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

BOOL SyncUi_ChooseItems(HWND owner, const ProfileList *profiles, DWORD members, DWORD *items)
{
    WCHAR names[NAMES_CCH], text[NAMES_CCH + 128];
    ItemsDialog dialog;
    NamesOf(profiles, members, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"What %s sync with each other, at each sync:"), names);
    ZeroMemory(&dialog, sizeof dialog);
    dialog.text = text;
    dialog.items = *items & SYNC_ITEMS_ALL;
    if (Ui_Dialog(owner, IDD_SYNC_ITEMS, ItemsProc, (LPARAM)&dialog) != IDOK || !dialog.items) return FALSE;
    *items = dialog.items;
    return TRUE;
}

/* ------------------------------------------------------- sync settings */

typedef struct SetupDialog {
    const ProfileList *profiles;
    SyncSetup         *setup;
    HWND               partners, items;
    int                rowProfile[MAX_PROFILES];
    int                rowCount;
    BOOL               filling;
} SetupDialog;

/* What the rows checked say: what to sync only once it syncs with one at least, and OK with something to sync. */
static void ReadSetup(HWND dialog, SetupDialog *state)
{
    int row;
    state->setup->partners = 0;
    for (row = 0; row < state->rowCount; row++)
        if (ListView_GetCheckState(state->partners, row)) state->setup->partners |= 1u << state->rowProfile[row];
    state->setup->items = 0;
    for (row = 0; row < SYNC_ITEM_COUNT; row++)
        if (ListView_GetCheckState(state->items, row)) state->setup->items |= kSyncItems[row].item;
    EnableWindow(GetDlgItem(dialog, IDC_Z_ITEMS_LABEL), state->setup->partners != 0);
    EnableWindow(state->items, state->setup->partners != 0);
    EnableWindow(GetDlgItem(dialog, IDC_Z_SYNC_NOW), state->setup->partners && state->setup->items);
    EnableWindow(GetDlgItem(dialog, IDOK), !state->setup->partners || state->setup->items);
}

/* A list of rows with check boxes, in one column as wide as the list. */
static void CheckRows(HWND list)
{
    LVCOLUMNW column;
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);   /* the view it scrolls in gives it the list's width */
}

static void AddCheckRow(HWND list, int row, const WCHAR *text, BOOL checked)
{
    LVITEMW item;
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = (LPWSTR)text;
    ListView_InsertItem(list, &item);
    ListView_SetCheckState(list, row, checked);
}

static INT_PTR CALLBACK SetupProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SetupDialog *state = (SetupDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        WCHAR text[LABEL_CCH + 64];
        int p, row;
        state = (SetupDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"\x201C%s\x201D syncs with:"), state->profiles->items[state->setup->profile].name);
        SetDlgItemTextW(dialog, IDC_Z_TEXT, text);
        state->partners = GetDlgItem(dialog, IDC_Z_LIST);
        state->items = GetDlgItem(dialog, IDC_Z_ITEMS);
        CheckRows(state->partners);
        CheckRows(state->items);
        state->filling = TRUE;
        for (p = 0; p < state->profiles->count; p++) {
            if (p == state->setup->profile) continue;
            state->rowProfile[state->rowCount] = p;
            AddCheckRow(state->partners, state->rowCount, state->profiles->items[p].name, (state->setup->partners & (1u << p)) != 0);
            state->rowCount++;
        }
        for (row = 0; row < SYNC_ITEM_COUNT; row++)
            AddCheckRow(state->items, row, TR(kSyncItems[row].label), (state->setup->items & kSyncItems[row].item) != 0);
        state->filling = FALSE;
        Theme_SmoothView(state->partners);
        Theme_SmoothView(state->items);
        ReadSetup(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_Z_NOTE);

    case WM_NOTIFY:
        if (state && (((const NMHDR *)lp)->hwndFrom == state->partners || ((const NMHDR *)lp)->hwndFrom == state->items) &&
            ((const NMHDR *)lp)->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            if (!state->filling && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK))
                ReadSetup(dialog, state);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDOK:
        case IDC_Z_SYNC_NOW:
            ReadSetup(dialog, state);
            if (state->setup->partners && !state->setup->items) return TRUE;
            state->setup->syncNow = LOWORD(wp) == IDC_Z_SYNC_NOW;
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

BOOL SyncUi_Setup(HWND owner, const ProfileList *profiles, SyncSetup *setup)
{
    SetupDialog dialog;
    const Profile *p;
    if (setup->profile < 0 || setup->profile >= profiles->count) return FALSE;
    p = &profiles->items[setup->profile];
    setup->partners = p->syncGroup ? SessionVault_Group(profiles, p->syncGroup) & ~(1u << setup->profile) : 0;
    setup->items = setup->partners ? SessionVault_GroupItems(profiles, setup->partners | (1u << setup->profile)) : SYNC_ITEMS_DEFAULT;
    setup->syncNow = FALSE;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.profiles = profiles;
    dialog.setup = setup;
    return Ui_Dialog(owner, IDD_SYNC_SETUP, SetupProc, (LPARAM)&dialog) == IDOK;
}

/* ------------------------------------------------------ conflicts settled */

/* The images of the conflicts' list: the choice, then what each row is. */
enum {
    IMAGE_CHOSEN, IMAGE_CANDIDATE, IMAGE_SESSION, IMAGE_GROUP, IMAGE_SECTION, IMAGE_PIN, IMAGE_SETTING, IMAGE_APPEARANCE,
    IMAGE_LANGUAGE, IMAGE_MODEL, IMAGE_LAYOUT, IMAGE_PERMISSION, IMAGE_STAR, IMAGES
};
static const WCHAR kConflictGlyphs[IMAGES] = { 0xEC61, 0xEA3A, 0xE8BD, 0xE8B7, 0xE8FD, 0xE718, 0xE713, 0xE8D2, 0xE774, 0xE945, 0xE80A, 0xE72E, 0xE734 };
static const ThemeTint kConflictTints[IMAGES] = {
    THEME_TINT_GREEN, THEME_TINT_NONE, THEME_TINT_PURPLE, THEME_TINT_AMBER, THEME_TINT_TEAL, THEME_TINT_TEAL, THEME_TINT_BLUE, THEME_TINT_PURPLE,
    THEME_TINT_PURPLE, THEME_TINT_GOLD, THEME_TINT_TEAL, THEME_TINT_RED, THEME_TINT_GOLD
};

/* Each kind of setting a conflict belongs to (SYNC_ITEM_*): its heading, its image. */
static const struct { DWORD item; const WCHAR *heading; int image; } kConflictKinds[] = {
    { SYNC_ITEM_SESSIONS, L"Sessions", IMAGE_SESSION },
    { SYNC_ITEM_SIDEBAR, L"Sidebar", IMAGE_GROUP },
    { SYNC_ITEM_DETAILS, L"Session details", IMAGE_LAYOUT },
    { SYNC_ITEM_APPEARANCE, L"Appearance", IMAGE_APPEARANCE },
    { SYNC_ITEM_LANGUAGE, L"Interface language", IMAGE_LANGUAGE },
    { SYNC_ITEM_MODEL, L"Default model", IMAGE_MODEL },
    { SYNC_ITEM_SETTINGS, L"Settings", IMAGE_SETTING },
    { SYNC_ITEM_PERMISSIONS, L"Permissions", IMAGE_PERMISSION },
};

#define CONFLICTS_SHOWN 256   /* the elements the dialog lists at most */
#define ROW_HEADING     (-1)  /* a row's lParam: a kind's heading */

typedef struct ConflictsDialog {
    const ProfileList *profiles;
    VaultConflict     *conflicts;
    int                count;
    int               *chosen;            /* by conflict: the profile whose version it keeps */
    int                columns[MAX_PROFILES];   /* the profile of each column after the first */
    int                columnCount;
    const WCHAR       *text;
    HWND               list;
    HICON              icon;
} ConflictsDialog;

/* The image a conflict's row shows: what its element is. */
static int ConflictImage(const VaultConflict *conflict)
{
    size_t k;
    if (strcmp(conflict->part, "groups") == 0) return strncmp(conflict->path, "/assignments", 12) == 0 ? IMAGE_SESSION : IMAGE_GROUP;
    if (strcmp(conflict->part, "pills") == 0) return IMAGE_SESSION;
    if (strcmp(conflict->part, "sections") == 0) return IMAGE_SECTION;
    if (strcmp(conflict->part, "slice") == 0 || strcmp(conflict->part, "navPins") == 0) return IMAGE_PIN;
    if (strncmp(conflict->part, "starred", 7) == 0) return IMAGE_STAR;
    for (k = 0; k < ARRAYSIZE(kConflictKinds); k++)
        if (kConflictKinds[k].item == conflict->item) return kConflictKinds[k].image;
    return IMAGE_SETTING;
}

static int KindOf(const VaultConflict *conflict)
{
    int k;
    for (k = 0; k < (int)ARRAYSIZE(kConflictKinds); k++)
        if (kConflictKinds[k].item == conflict->item) return k;
    return (int)ARRAYSIZE(kConflictKinds) - 1;
}

/* Each cell of conflict row `row` (the conflict `c`): the chosen version checked, the others ringed. */
static void ShowChoice(const ConflictsDialog *state, int row, int c)
{
    int column;
    for (column = 0; column < state->columnCount; column++) {
        int p = state->columns[column];
        LVITEMW item;
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_IMAGE;
        item.iItem = row;
        item.iSubItem = column + 1;
        item.iImage = !(state->conflicts[c].members & (1u << p)) ? I_IMAGENONE : state->chosen[c] == p ? IMAGE_CHOSEN : IMAGE_CANDIDATE;
        ListView_SetItem(state->list, &item);
    }
}

static void ShowAllChoices(const ConflictsDialog *state)
{
    int row, rows = ListView_GetItemCount(state->list);
    for (row = 0; row < rows; row++) {
        LVITEMW item;
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(state->list, &item) && item.lParam != ROW_HEADING) ShowChoice(state, row, (int)item.lParam);
    }
}

/* The columns across `width`: each profile's as wide as its longest version,
 * the elements' the rest, at least two fifths (the versions then share the
 * other three). */
static void SizeConflictColumns(const ConflictsDialog *state, int width)
{
    int wanted[MAX_PROFILES], total = 0, i, row, rows = ListView_GetItemCount(state->list), dpi = GetDpiForWindow(state->list), first, least;
    int image = GetSystemMetricsForDpi(SM_CXSMICON, dpi), room = MulDiv(20, dpi, 96);
    if (!state->columnCount) return;
    for (i = 0; i < state->columnCount; i++) {
        wanted[i] = ListView_GetStringWidth(state->list, state->profiles->items[state->columns[i]].name) + room;
        for (row = 0; row < rows; row++) {
            WCHAR text[VAULT_SHOWN_CCH];
            ListView_GetItemText(state->list, row, i + 1, text, ARRAYSIZE(text));
            wanted[i] = max(wanted[i], ListView_GetStringWidth(state->list, text) + image + room);
        }
        total += wanted[i];
    }
    least = width * 2 / 5;
    if (total > width - least) {
        int shared = 0;
        for (i = 0; i < state->columnCount; i++) shared += wanted[i] = MulDiv(wanted[i], width - least, total);
        total = shared;
    }
    first = width - total;
    ListView_SetColumnWidth(state->list, 0, first);
    for (i = 0; i < state->columnCount; i++) ListView_SetColumnWidth(state->list, i + 1, wanted[i]);
}

/* The list: a heading per kind, then a row per conflict, a column per profile. */
static void FillConflicts(HWND dialog, ConflictsDialog *state)
{
    LVCOLUMNW column;
    LVITEMW item;
    RECT client;
    int k, c, i, row = 0, width, first;
    state->list = GetDlgItem(dialog, IDC_X_LIST);
    ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_SUBITEMIMAGES);
    ListView_SetImageList(state->list, Theme_GlyphImages(state->list, kConflictGlyphs, kConflictTints, IMAGES), LVSIL_SMALL);
    GetClientRect(state->list, &client);
    width = client.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(state->list));
    first = state->columnCount ? width * 2 / 5 : width;
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = (LPWSTR)TR(L"Changed");
    column.cx = first;
    ListView_InsertColumn(state->list, 0, &column);
    for (i = 0; i < state->columnCount; i++) {
        column.pszText = (LPWSTR)state->profiles->items[state->columns[i]].name;
        column.cx = (width - first) / state->columnCount;
        ListView_InsertColumn(state->list, i + 1, &column);
    }
    for (k = 0; k < (int)ARRAYSIZE(kConflictKinds); k++) {
        BOOL headed = FALSE;
        for (c = 0; c < state->count; c++) {
            if (KindOf(&state->conflicts[c]) != k) continue;
            if (!headed) {
                ZeroMemory(&item, sizeof item);
                item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
                item.iItem = row++;
                item.iImage = kConflictKinds[k].image;
                item.lParam = ROW_HEADING;
                item.pszText = (LPWSTR)TR(kConflictKinds[k].heading);
                ListView_InsertItem(state->list, &item);
                headed = TRUE;
            }
            ZeroMemory(&item, sizeof item);
            item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM | LVIF_INDENT;
            item.iItem = row;
            item.iImage = ConflictImage(&state->conflicts[c]);
            item.iIndent = 1;
            item.lParam = c;
            item.pszText = state->conflicts[c].label;
            ListView_InsertItem(state->list, &item);
            for (i = 0; i < state->columnCount; i++) {
                int p = state->columns[i];
                ListView_SetItemText(state->list, row, i + 1,
                                     (state->conflicts[c].members & (1u << p)) ? state->conflicts[c].shown[p] : (LPWSTR)L"\x2014");
            }
            ShowChoice(state, row, c);
            row++;
        }
    }
    SizeConflictColumns(state, width);
    Theme_SetHeadingRows(state->list);
    Theme_SmoothView(state->list);
    /* The first conflict selected: the arrows choose from the start. */
    for (i = 0; i < row; i++) {
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (ListView_GetItem(state->list, &item) && item.lParam != ROW_HEADING) {
            ListView_SetItemState(state->list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            break;
        }
    }
}

/* Row `row`'s conflict, -1 for a heading. */
static int ConflictOfRow(const ConflictsDialog *state, int row)
{
    LVITEMW item;
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_PARAM;
    item.iItem = row;
    return row >= 0 && ListView_GetItem(state->list, &item) && item.lParam != ROW_HEADING ? (int)item.lParam : -1;
}

/* Conflict row `row` moved to the next version left or right (`step`). */
static void StepChoice(ConflictsDialog *state, int row, int step)
{
    int c = ConflictOfRow(state, row), column, at = -1;
    if (c < 0) return;
    for (column = 0; column < state->columnCount; column++)
        if (state->columns[column] == state->chosen[c]) at = column;
    for (column = at + step; column >= 0 && column < state->columnCount; column += step)
        if (state->conflicts[c].members & (1u << state->columns[column])) {
            state->chosen[c] = state->columns[column];
            ShowChoice(state, row, c);
            return;
        }
}

static INT_PTR CALLBACK ConflictsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ConflictsDialog *state = (ConflictsDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        int i;
        state = (ConflictsDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_X_TEXT, state->text);
        if (SUCCEEDED(LoadIconWithScaleDown(NULL, IDI_INFORMATION, GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(dialog)),
                                            GetSystemMetricsForDpi(SM_CYICON, GetDpiForWindow(dialog)), &state->icon)))
            SendDlgItemMessageW(dialog, IDC_X_ICON, STM_SETICON, (WPARAM)state->icon, 0);
        Theme_SetGlyph(GetDlgItem(dialog, IDOK), 0xE73E, THEME_TINT_GREEN);
        Theme_SetGlyph(GetDlgItem(dialog, IDCANCEL), 0xE823, THEME_TINT_NONE);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)TR(L"The latest of each"));
        for (i = 0; i < state->columnCount; i++)
            SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)state->profiles->items[state->columns[i]].name);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_SETCURSEL, 0, 0);
        FillConflicts(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_X_NOTE);

    case WM_NOTIFY:
        if (state && ((const NMHDR *)lp)->hwndFrom == state->list) {
            const NMHDR *header = (const NMHDR *)lp;
            if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
                /* A click on a profile's version keeps it. */
                LVHITTESTINFO hit;
                DWORD position = GetMessagePos();
                int c;
                ZeroMemory(&hit, sizeof hit);
                hit.pt.x = (short)LOWORD(position);
                hit.pt.y = (short)HIWORD(position);
                ScreenToClient(state->list, &hit.pt);
                if (ListView_SubItemHitTest(state->list, &hit) >= 0 && hit.iSubItem > 0 && hit.iSubItem <= state->columnCount &&
                    (c = ConflictOfRow(state, hit.iItem)) >= 0 && (state->conflicts[c].members & (1u << state->columns[hit.iSubItem - 1]))) {
                    state->chosen[c] = state->columns[hit.iSubItem - 1];
                    ShowChoice(state, hit.iItem, c);
                }
                return TRUE;
            }
            if (header->code == LVN_KEYDOWN) {
                WORD key = ((const NMLVKEYDOWN *)lp)->wVKey;
                int row = ListView_GetNextItem(state->list, -1, LVNI_FOCUSED);
                if (key == VK_LEFT || key == VK_RIGHT) {
                    StepChoice(state, row, key == VK_LEFT ? -1 : 1);
                    SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
                }
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_X_PROFILE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                /* Every element: the latest version of each, or the one of the profile chosen where it has one. */
                LRESULT choice = SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_GETCURSEL, 0, 0);
                int c;
                for (c = 0; c < state->count; c++) {
                    if (choice <= 0) state->chosen[c] = state->conflicts[c].latest;
                    else if (choice - 1 < state->columnCount && (state->conflicts[c].members & (1u << state->columns[choice - 1])))
                        state->chosen[c] = state->columns[choice - 1];
                }
                ShowAllChoices(state);
            }
            return TRUE;
        case IDOK:
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_DESTROY:
        if (state && state->icon) DestroyIcon(state->icon);
        break;
    }
    return FALSE;
}

/* Group `group`'s conflicts settled: FALSE when there were none, or the person left them for later. */
static BOOL SettleGroup(HWND owner, const ProfileList *profiles, int group)
{
    WCHAR names[NAMES_CCH], text[NAMES_CCH + 256];
    ConflictsDialog dialog;
    DWORD members = 0;
    int c, p, *chosen = NULL;
    BOOL ok = FALSE;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.profiles = profiles;
    if ((dialog.conflicts = (VaultConflict *)HeapAlloc(GetProcessHeap(), 0, CONFLICTS_SHOWN * sizeof *dialog.conflicts)) == NULL ||
        (chosen = (int *)HeapAlloc(GetProcessHeap(), 0, CONFLICTS_SHOWN * sizeof *chosen)) == NULL ||
        (dialog.count = SessionVault_Conflicts(profiles, group, dialog.conflicts, CONFLICTS_SHOWN)) <= 0)
        goto done;
    dialog.chosen = chosen;
    for (c = 0; c < dialog.count; c++) {
        members |= dialog.conflicts[c].members;
        /* At first, the version of the one that changed it last. */
        if (dialog.conflicts[c].latest < 0 || !(dialog.conflicts[c].members & (1u << dialog.conflicts[c].latest)))
            for (p = 0; p < profiles->count; p++)
                if (dialog.conflicts[c].members & (1u << p)) {
                    dialog.conflicts[c].latest = p;
                    break;
                }
        chosen[c] = dialog.conflicts[c].latest;
    }
    for (p = 0; p < profiles->count && dialog.columnCount < MAX_PROFILES; p++)
        if (members & (1u << p)) dialog.columns[dialog.columnCount++] = p;
    NamesOf(profiles, members, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s changed these each their own way since the last sync. Choose the version to keep of each:"), names);
    dialog.text = text;
    if (Ui_Dialog(owner, IDD_CONFLICTS, ConflictsProc, (LPARAM)&dialog) != IDOK) goto done;
    ok = SessionVault_Decide(profiles, group, dialog.conflicts, chosen, dialog.count);
    if (!ok) Ui_Message(owner, MB_ICONERROR, TR(L"The choice could not be saved: the profiles keep their own until the next sync asks again."));
done:
    if (dialog.conflicts) HeapFree(GetProcessHeap(), 0, dialog.conflicts);
    if (chosen) HeapFree(GetProcessHeap(), 0, chosen);
    return ok;
}

DWORD SyncUi_SettleConflicts(HWND owner, const ProfileList *profiles)
{
    DWORD settled = 0;
    int group;
    for (group = 1; group <= MAX_PROFILES; group++) {
        DWORD members = SessionVault_Group(profiles, group);
        if (BitCount(members) >= 2 && SettleGroup(owner, profiles, group)) settled |= members;
    }
    return settled;
}

/* The sessions of group `group` made the same right away, and what that did said. */
BOOL SyncUi_KeepSame(HWND owner, const ProfileList *profiles, int group)
{
    WCHAR names[NAMES_CCH], first[NAMES_CCH + 128], listName[FOLDER_CCH];
    SyncReport report;
    DWORD members = SessionVault_Group(profiles, group);
    HCURSOR old;
    if (!members || !SessionVault_GroupListName(group, listName, ARRAYSIZE(listName))) return FALSE;
    ZeroMemory(&report, sizeof report);
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    SessionVault_Keep(profiles, members, listName, TRUE, &report);
    SetCursor(old);
    NamesOf(profiles, members, names, ARRAYSIZE(names));
    StringCchPrintfW(first, ARRAYSIZE(first), TR(L"The sessions of %s are kept the same from now on."), names);
    SyncUi_ShowReport(owner, profiles, &report, first);
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
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Copy .claude to %s?"), wcsrchr(target, L'\\') ? wcsrchr(target, L'\\') + 1 : target);
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
    const PurgeItem   *items;
    const ProfileList *profiles;
    int                count;
    BOOL              *checked;        /* by item; only the rows shown can be checked */
    BOOL               backUp;
    int                shown;          /* PURGE_SHOW_ALL, PURGE_SHOW_UNLISTED, or a profile's index */
    HWND               rows;           /* in the view it scrolls in, which has its id */
    BOOL               filling;
} PurgeDialog;

#define PURGE_SHOW_ALL      (-1)
#define PURGE_SHOW_UNLISTED (-2)       /* in no profile's kept list: made in a terminal, say */

static BOOL PurgeShown(const PurgeDialog *state, const PurgeItem *item)
{
    if (state->shown == PURGE_SHOW_ALL) return TRUE;
    if (state->shown == PURGE_SHOW_UNLISTED) return item->profiles == 0;
    return (item->profiles & (1u << state->shown)) != 0;
}

/* Delete takes any conversation checked; Restore, one a kept list had.
 * Select all is checked while every row shown is. */
static void ReadChecked(HWND dialog, PurgeDialog *state)
{
    int i, rows = ListView_GetItemCount(state->rows), chosen = 0, restorable = 0;
    for (i = 0; i < rows; i++) {
        LVITEMW item;
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (!ListView_GetItem(state->rows, &item) || item.lParam < 0 || item.lParam >= state->count) continue;
        if ((state->checked[item.lParam] = ListView_GetCheckState(state->rows, i)) != FALSE) {
            chosen++;
            if (state->items[item.lParam].restorable) restorable++;
        }
    }
    EnableWindow(GetDlgItem(dialog, IDOK), chosen > 0);
    EnableWindow(GetDlgItem(dialog, IDC_C_RESTORE), restorable > 0);
    EnableWindow(GetDlgItem(dialog, IDC_C_ALL), rows > 0);
    CheckDlgButton(dialog, IDC_C_ALL, rows > 0 && chosen == rows ? BST_CHECKED : BST_UNCHECKED);
}

/* A conversation's row: the profiles that had it (or in no list), its
 * title (its id without one), when it was last written, its size. */
static void PurgeRow(const PurgeDialog *state, const PurgeItem *item, WCHAR *out, size_t cch)
{
    WCHAR when[128], names[MAX_PROFILES * (LABEL_CCH + 2)];
    int p;
    names[0] = 0;
    for (p = 0; p < state->profiles->count; p++)
        if (item->profiles & (1u << p)) {
            if (names[0]) StringCchCatW(names, ARRAYSIZE(names), TR(L", "));
            StringCchCatW(names, ARRAYSIZE(names), state->profiles->items[p].name);
        }
    FormatWhen(item->written, when, ARRAYSIZE(when));
    StringCchPrintfW(out, cch, L"%s  \x00B7  %s  \x00B7  %s  \x00B7  %.1f MB", names[0] ? names : TR(L"In no list"),
                     item->title[0] ? item->title : item->id, when, (double)item->bytes / (1024.0 * 1024.0));
}

/* The rows of what Show picks, none checked: what is acted on is what shows. */
static void FillPurgeRows(HWND dialog, PurgeDialog *state)
{
    WCHAR text[SESSION_TITLE_CCH + MAX_PATH + MAX_PROFILES * (LABEL_CCH + 2)];
    LVITEMW item;
    int i, row = 0;
    state->filling = TRUE;
    SendMessageW(state->rows, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(state->rows);
    for (i = 0; i < state->count; i++) {
        state->checked[i] = FALSE;
        if (!PurgeShown(state, &state->items[i])) continue;
        PurgeRow(state, &state->items[i], text, ARRAYSIZE(text));
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row++;
        item.pszText = text;
        item.lParam = i;
        ListView_InsertItem(state->rows, &item);
    }
    SendMessageW(state->rows, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(state->rows, NULL, TRUE);
    state->filling = FALSE;
    ReadChecked(dialog, state);
}

static INT_PTR CALLBACK PurgeProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    PurgeDialog *state = (PurgeDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    LVCOLUMNW column;
    int i;
    switch (message) {
    case WM_INITDIALOG: {
        HWND show = GetDlgItem(dialog, IDC_C_PROFILE);
        state = (PurgeDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_C_TEXT, TR(L"Conversations still on this PC that no profile shows any more."));
        CheckDlgButton(dialog, IDC_C_BACKUP, BST_CHECKED);
        /* Show: every one, each profile's, those in no list; the item data is what PurgeDialog.shown takes. */
        SendMessageW(show, CB_SETITEMDATA, (WPARAM)SendMessageW(show, CB_ADDSTRING, 0, (LPARAM)TR(L"All profiles")), (LPARAM)PURGE_SHOW_ALL);
        for (i = 0; i < state->profiles->count; i++)
            SendMessageW(show, CB_SETITEMDATA, (WPARAM)SendMessageW(show, CB_ADDSTRING, 0, (LPARAM)state->profiles->items[i].name), (LPARAM)i);
        SendMessageW(show, CB_SETITEMDATA, (WPARAM)SendMessageW(show, CB_ADDSTRING, 0, (LPARAM)TR(L"In no list")), (LPARAM)PURGE_SHOW_UNLISTED);
        SendMessageW(show, CB_SETCURSEL, 0, 0);
        state->shown = PURGE_SHOW_ALL;
        state->rows = GetDlgItem(dialog, IDC_C_LIST);
        ListView_SetExtendedListViewStyle(state->rows, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(state->rows, 0, &column);
        FillPurgeRows(dialog, state);
        Theme_SmoothView(state->rows);
        return TRUE;
    }

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
        case IDC_C_PROFILE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT at = SendDlgItemMessageW(dialog, IDC_C_PROFILE, CB_GETCURSEL, 0, 0);
                if (at != CB_ERR) state->shown = (int)SendDlgItemMessageW(dialog, IDC_C_PROFILE, CB_GETITEMDATA, (WPARAM)at, 0);
                FillPurgeRows(dialog, state);
            }
            return TRUE;
        case IDC_C_ALL:
            if (HIWORD(wp) == BN_CLICKED) {
                BOOL all = IsDlgButtonChecked(dialog, IDC_C_ALL) == BST_CHECKED;
                state->filling = TRUE;
                for (i = 0; i < ListView_GetItemCount(state->rows); i++) ListView_SetCheckState(state->rows, i, all);
                state->filling = FALSE;
                ReadChecked(dialog, state);
            }
            return TRUE;
        case IDOK:
        case IDC_C_RESTORE:
            ReadChecked(dialog, state);
            state->backUp = IsDlgButtonChecked(dialog, IDC_C_BACKUP) == BST_CHECKED;
            EndDialog(dialog, LOWORD(wp));
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* The conversations checked that a kept list had, put back in its profiles; what that did said. */
static BOOL RestoreChosen(HWND owner, const ProfileList *profiles, const PurgeItem *items, const int *chosen, int count)
{
    WCHAR first[512], line[256];
    const WCHAR **keys = (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)max(count, 1) * sizeof *keys);
    SyncReport report;
    HCURSOR old;
    int i, n = 0, missing = 0, restored;
    if (!keys) return FALSE;
    for (i = 0; i < count; i++)
        if (items[chosen[i]].restorable) keys[n++] = items[chosen[i]].id;
        else missing++;
    ZeroMemory(&report, sizeof report);
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    restored = n ? SessionVault_Undelete(profiles, keys, n, &report) : 0;
    SetCursor(old);
    HeapFree(GetProcessHeap(), 0, (void *)keys);
    if (restored < 0) {
        Ui_Message(owner, MB_ICONWARNING, TR(L"The conversations could not be restored. %s"), report.error);
        return FALSE;
    }
    StringCchPrintfW(first, ARRAYSIZE(first), TR(L"Conversations restored: %d"), restored);
    if (missing) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Not restored, in no list: %d"), missing);
        StringCchCatW(first, ARRAYSIZE(first), L"\n");
        StringCchCatW(first, ARRAYSIZE(first), line);
    }
    SyncUi_ShowReport(owner, profiles, &report, first);
    return restored > 0;
}

/* The clean-up, asked first: how many conversations, and that .claude is
 * copied first when that was chosen. */
static BOOL ConfirmPurge(HWND owner, int n, BOOL backUp)
{
    WCHAR question[256];
    StringCchPrintfW(question, ARRAYSIZE(question),
                     backUp ? TR(L"Move %d conversations to the Recycle Bin?\n\n.claude is copied first.") : TR(L"Move %d conversations to the Recycle Bin?"),
                     n);
    return SyncUi_Confirm(owner, IDI_WARNING, question, FALSE, TR(L"Move to the Recycle Bin"));
}

BOOL SyncUi_Purge(HWND owner, const ProfileList *profiles)
{
    WCHAR error[LONG_PATH_CCH];
    PurgeDialog dialog;
    PurgeItem *items = NULL;
    RemoveResult result;
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    INT_PTR action = IDCANCEL;
    BOOL changed = FALSE;
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
    dialog.profiles = profiles;
    dialog.count = count;
    dialog.checked = (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count * sizeof *dialog.checked);
    chosen = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *chosen);
    if (dialog.checked && chosen) action = Ui_Dialog(owner, IDD_PURGE, PurgeProc, (LPARAM)&dialog);
    if (action == IDOK || action == IDC_C_RESTORE)
        for (i = 0; i < count; i++)
            if (dialog.checked[i]) chosen[n++] = i;
    if (action == IDC_C_RESTORE && n) {
        changed = RestoreChosen(owner, profiles, items, chosen, n);
    } else if (action == IDOK && n && ConfirmPurge(owner, n, dialog.backUp) && (!dialog.backUp || CopyCodeFolder(owner, FALSE))) {
        result = SessionPurge_Delete(owner, profiles, items, chosen, n, &deleted, error, ARRAYSIZE(error));
        if (result == REMOVE_DONE) Ui_Message(owner, MB_ICONINFORMATION, TR(L"Conversations moved to the Recycle Bin: %d"), deleted);
        else if (result == REMOVE_FAILED) Ui_Message(owner, MB_ICONWARNING, TR(L"The conversations could not be deleted. %s"), error);
        changed = deleted > 0;
    }
    if (dialog.checked) HeapFree(GetProcessHeap(), 0, dialog.checked);
    if (chosen) HeapFree(GetProcessHeap(), 0, chosen);
    HeapFree(GetProcessHeap(), 0, items);
    return changed;
}
