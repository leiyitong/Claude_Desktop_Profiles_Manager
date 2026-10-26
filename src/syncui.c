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
#include "vaulttools.h"
#include <commctrl.h>
#include <shlobj.h>
#include <windowsx.h>


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
    /* An import: the profiles and the archive's sessions in one list, each one to check. */
    CheckList              checks;
    CheckGroup             groups[2];
    CheckValue            *values;      /* the profiles', then the sessions'; NULL: the profiles alone, as the others list them */
    const ArchivedSession *sessions;
    int                    sessionCount;
    WCHAR                (*names)[LABEL_CCH + 128];   /* each profile's row */
} SyncDialog;

#define IMPORT_PROFILES 0   /* the import list's groups */
#define IMPORT_SESSIONS 1
static const WCHAR kImportGlyphs[] = { 0xE77B, 0xE8BD };
static const ThemeTint kImportTints[] = { THEME_TINT_TEAL, THEME_TINT_PURPLE };

static void FormatWhen(ULONGLONG ms, WCHAR *out, size_t cch);

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
    Profiles_JoinNames(state->profiles, state->sources, names, ARRAYSIZE(names));
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
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"The sessions checked in \x201C%s\x201D will be listed in the profiles checked. "
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

/* An import's list: the profiles that can take the sessions, then the
 * archive's sessions, every one checked, both groups open. */
static void SetupImportList(SyncDialog *state, HWND list)
{
    int p, i, n = 0;
    state->groups[IMPORT_PROFILES].heading = L"To these profiles";
    state->groups[IMPORT_PROFILES].image = 0;
    state->groups[IMPORT_PROFILES].name = L"profiles";
    state->groups[IMPORT_SESSIONS].heading = L"Sessions";
    state->groups[IMPORT_SESSIONS].image = 1;
    state->groups[IMPORT_SESSIONS].name = L"sessions";
    for (p = 0; p < state->profiles->count; p++) {
        const Profile *profile = &state->profiles->items[p];
        CheckValue *value = &state->values[n];
        if (!(state->takers & (1u << p)) || (state->sources & (1u << p))) continue;
        if (Claude_IsRunning(profile)) StringCchPrintfW(state->names[n], ARRAYSIZE(state->names[n]), TR(L"%s   (open now: gets them once it closes)"), profile->name);
        else StringCchCopyW(state->names[n], ARRAYSIZE(state->names[n]), profile->name);
        value->group = IMPORT_PROFILES;
        value->image = -1;
        value->label = state->names[n];
        value->literal = value->boxed = TRUE;
        value->taken = (state->chosen & (1u << p)) != 0;
        value->pick = -1;
        value->data = p;
        n++;
    }
    for (i = 0; i < state->sessionCount; i++, n++) {
        CheckValue *value = &state->values[n];
        value->group = IMPORT_SESSIONS;
        value->image = -1;
        value->label = state->sessions[i].title;
        value->literal = value->boxed = value->taken = TRUE;
        value->pick = -1;
        value->data = i;
        FormatWhen(state->sessions[i].lastActivity, value->text, ARRAYSIZE(value->text));
    }
    ZeroMemory(&state->checks, sizeof state->checks);
    state->checks.groups = state->groups;
    state->checks.groupCount = 2;
    state->checks.values = state->values;
    state->checks.valueCount = n;
    CheckList_Setup(&state->checks, list, kImportGlyphs, kImportTints, 2);
    CheckList_FoldAll(&state->checks, TRUE);
}

static void ReadChosen(HWND dialog, SyncDialog *state)
{
    int i;
    BOOL sessions = TRUE;
    state->chosen = 0;
    if (state->values) {
        sessions = FALSE;
        for (i = 0; i < state->checks.valueCount; i++) {
            const CheckValue *value = &state->values[i];
            if (!value->taken) continue;
            if (value->group == IMPORT_PROFILES) state->chosen |= 1u << value->data;
            else sessions = TRUE;
        }
    } else {
        for (i = 0; i < state->rowCount; i++)
            if (ListView_GetCheckState(state->rows, i)) state->chosen |= 1u << state->rowProfile[i];
    }
    EnableWindow(GetDlgItem(dialog, IDOK), state->chosen != 0 && sessions);
}

static INT_PTR CALLBACK SyncProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SyncDialog *state = (SyncDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
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
        if (state->values) {
            ShowWindow(GetDlgItem(dialog, IDC_Y_TO_LABEL), SW_HIDE);   /* the list's groups say it */
            SetupImportList(state, list);
        } else {
            Theme_CheckRows(list);
            FillRows(state);
        }
        Theme_SmoothView(list);
        ReadChosen(dialog, state);
        return TRUE;

    case WM_NOTIFY:
    case WM_CHECKLIST_BOX:
    case WM_CONTEXTMENU: {
        BOOL toggled;
        if (state && state->values && CheckList_Message(&state->checks, dialog, message, wp, lp, &toggled)) {
            if (toggled) ReadChosen(dialog, state);
            return TRUE;
        }
        if (message == WM_NOTIFY && state && !state->values && Theme_CheckRowChanged((const NMHDR *)lp, state->rows)) {
            if (!state->filling) ReadChosen(dialog, state);
            return TRUE;
        }
        break;
    }

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
static void ShowReport(HWND owner, const ProfileList *profiles, const SyncReport *report, const WCHAR *first)
{
    const size_t reportCch = 32768;
    WCHAR *text = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, reportCch * sizeof(WCHAR));
    WCHAR line[LONG_PATH_CCH + 256], names[NAMES_CCH];
    int d;
    if (!text) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The operation finished, but its results could not be displayed."));
        return;
    }
    text[0] = 0;
    if (first) StringCchCopyW(text, reportCch, first);
    AddCount(text, reportCch, TR(L"Sessions added: %d"), report->added);
    AddCount(text, reportCch, TR(L"Sessions updated: %d"), report->updated);
    AddCount(text, reportCch, TR(L"Sessions removed: %d"), report->removed);
    AddCount(text, reportCch, TR(L"Sessions two profiles went on with apart, now kept as two: %d"), report->forked);
    AddCount(text, reportCch, TR(L"Left as they were (newer or deleted there): %d"), report->skipped);
    if (report->waiting) {
        if (report->profileCount) {
            names[0] = 0;
            for (d = 0; d < report->profileCount; d++)
                if (report->profiles[d].waiting) {
                    int p = Profiles_Find(profiles, report->profiles[d].folder);
                    if (names[0]) StringCchCatW(names, ARRAYSIZE(names), TR(L", "));
                    StringCchCatW(names, ARRAYSIZE(names), p >= 0 ? profiles->items[p].name : report->profiles[d].folder);
                }
        } else Profiles_JoinNames(profiles, report->waiting, names, ARRAYSIZE(names));
        if (names[0]) {
            StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Waiting for %s to close: the rest is done then."), names);
            AddLine(text, reportCch, line);
        }
    }
    if (report->unavailable) {
        StringCchCopyW(line, ARRAYSIZE(line), TR(L"Some selected profiles were left out because they are not signed in to Claude yet."));
        AddLine(text, reportCch, line);
    }
    if (report->failed) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Changes that could not be made: %d. The first:"), report->failed);
        AddLine(text, reportCch, line);
        AddLine(text, reportCch, report->error);
    }
    if (report->backup[0]) {
        AddLine(text, reportCch, TR(L"What was replaced or removed is kept in:"));
        AddLine(text, reportCch, report->backup);
    }
    if (!report->added && !report->updated && !report->removed && !report->skipped && !report->failed && !report->waiting &&
        !report->unavailable && !report->forked)
        AddLine(text, reportCch, TR(L"Nothing needed changing."));
    for (d = 0; d < report->profileCount; d++) {
        const SyncProfileResult *detail = &report->profiles[d];
        int p = Profiles_Find(profiles, detail->folder);
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"%s: added %d, updated %d, removed %d, kept %d, failed %d%s"),
                         p >= 0 ? profiles->items[p].name : detail->folder, detail->added, detail->updated, detail->removed,
                         detail->skipped, detail->failed, detail->waiting ? TR(L"; waiting for this profile to close") : L"");
        AddLine(text, reportCch, line);
        if (detail->unavailable) AddLine(text, reportCch, TR(L"Not signed in"));
        if (detail->error[0]) AddLine(text, reportCch, detail->error);
    }
    Ui_ReviewText(owner, TR(L"Operation results"), TR(L"Results are listed separately for each profile that received changes."), text, NULL);
    HeapFree(GetProcessHeap(), 0, text);
}

/* ----------------------------------------------------------- confirming */

void SyncUi_Pending(HWND owner, const ProfileList *profiles)
{
    WCHAR *text = SessionSync_PendingText(profiles);
    if (!text) { Ui_Message(owner, MB_ICONERROR, TR(L"Sessions could not be loaded.")); return; }
    Ui_ReviewText(owner, TR(L"Pending changes"),
                  TR(L"Stage 1 sends sessions and settings. Stage 2 applies local edits, with removals last. Newer target changes can prevent replacement. This is a snapshot, not an audit history."),
                  text, NULL);
    HeapFree(GetProcessHeap(), 0, text);
}

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
    if (Core_BitCount(*takers) >= 2) return TRUE;
    Ui_Message(owner, MB_ICONINFORMATION, TR(L"This needs at least two profiles signed in to Claude: "
                                             L"a profile keeps sessions only once it is signed in."));
    return FALSE;
}

void SyncUi_ShowReport(HWND owner, const ProfileList *profiles, const SyncReport *report, const WCHAR *first)
{
    ShowReport(owner, profiles, report, first);
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
    Profiles_JoinNames(profiles, dialog.chosen, names, ARRAYSIZE(names));
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
    dialog.chosen = Core_BitCount(takers) == 1 ? takers : 0;   /* one to choose: it is */
    if (!ChooseProfiles(owner, &dialog)) return FALSE;
    Profiles_JoinNames(profiles, sources, names, ARRAYSIZE(names));
    Profiles_JoinNames(profiles, dialog.chosen, targets, ARRAYSIZE(targets));
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
    dialog.chosen = Core_BitCount(takers) == 1 ? takers : 0;   /* one to choose: it is */
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
    if (Core_BitCount(chosen) == 1)
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
    return SyncUi_ImportFile(owner, profiles, chosen, NULL);
}

/* The sessions left checked in an import's list, by key; none when they all are (every one taken). */
static BOOL PickedSessions(const SyncDialog *state, WCHAR (**keys)[SESSION_ID_CCH], SessionPick *pick)
{
    int i, taken = 0;
    *keys = NULL;
    ZeroMemory(pick, sizeof *pick);
    for (i = 0; i < state->checks.valueCount; i++) taken += state->values[i].group == IMPORT_SESSIONS && state->values[i].taken;
    if (taken == state->sessionCount) return TRUE;
    if ((*keys = (WCHAR (*)[SESSION_ID_CCH])Util_Alloc((size_t)max(taken, 1) * sizeof **keys)) == NULL) return FALSE;
    for (i = 0; i < state->checks.valueCount; i++)
        if (state->values[i].group == IMPORT_SESSIONS && state->values[i].taken)
            StringCchCopyW((*keys)[pick->count++], SESSION_ID_CCH, state->sessions[state->values[i].data].key);
    pick->keys = (const WCHAR (*)[SESSION_ID_CCH])*keys;
    return TRUE;
}

BOOL SyncUi_ImportFile(HWND owner, const ProfileList *profiles, DWORD chosen, const WCHAR *file)
{
    WCHAR path[LONG_PATH_CCH], first[256];
    WCHAR (*keys)[SESSION_ID_CCH] = NULL;
    const WCHAR *name;
    ArchivedSession *listed = NULL;
    SessionPick pick;
    SessionSet set;
    SyncDialog dialog;
    SyncReport report;
    DWORD takers;
    HCURSOR old;
    ZipIn *zip;
    int sessions = 0;
    BOOL ok = FALSE;
    WCHAR lockedPath[LONG_PATH_CCH];
    HANDLE archiveLock = INVALID_HANDLE_VALUE;
    ZeroMemory(&dialog, sizeof dialog);
    if (file ? FAILED(StringCchCopyW(path, ARRAYSIZE(path), file)) : !ChooseArchive(owner, FALSE, path, ARRAYSIZE(path))) return FALSE;
    zip = Zip_Open(path);
    dialog.sessionCount = zip ? SessionSync_ArchiveSessions(zip, &listed) : -1;
    Zip_Free(zip);
    if (dialog.sessionCount < 0) {
        Ui_Message(owner, MB_ICONWARNING, TR(L"This file is not a session archive of " APP_NAME L"."));
        return FALSE;
    }
    if (!LoadSessions(owner, profiles, &set)) goto done;
    takers = SessionSync_Takers(&set);
    SessionStore_Free(&set);
    if (!takers) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"No profile is signed in to Claude yet: a profile keeps sessions only once it is signed in."));
        goto done;
    }
    name = wcsrchr(path, L'\\');
    dialog.kind = SYNC_UI_IMPORT;
    dialog.profiles = profiles;
    dialog.takers = takers;
    dialog.chosen = (chosen & takers) ? chosen & takers : takers;
    dialog.archive = name ? name + 1 : path;
    dialog.sessions = listed;
    if (listed && dialog.sessionCount > 0 &&
        ((dialog.values = (CheckValue *)Util_Alloc((size_t)(MAX_PROFILES + dialog.sessionCount) * sizeof *dialog.values)) == NULL ||
         (dialog.names = (WCHAR (*)[LABEL_CCH + 128])Util_Alloc(MAX_PROFILES * sizeof *dialog.names)) == NULL))
        goto done;
    if (!ChooseProfiles(owner, &dialog) || (dialog.values && !PickedSessions(&dialog, &keys, &pick))) goto done;
    /* Chosen in the window, imported without asking again; the archive held as it was checked until then:
     * other readers are let in, a writer or a replacement is not. */
    archiveLock = Util_ExtendedPath(path, lockedPath, ARRAYSIZE(lockedPath))
                    ? CreateFileW(lockedPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                    : INVALID_HANDLE_VALUE;
    if (archiveLock == INVALID_HANDLE_VALUE) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The archive could not be held open for the import. Close other programs using it and try again."));
        goto done;
    }
    if (!Backup_CheckArchive(owner, path) || !LoadSessions(owner, profiles, &set)) goto done;
    ZeroMemory(&report, sizeof report);
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    ok = SessionSync_ImportPicked(&set, path, dialog.chosen, keys ? &pick : NULL, &sessions, &report);
    SetCursor(old);
    SessionStore_Free(&set);
    if (!ok) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The sessions could not be imported. %s"), report.error);
        goto done;
    }
    StringCchPrintfW(first, ARRAYSIZE(first), TR(L"Sessions in the archive: %d"), sessions);
    ShowReport(owner, profiles, &report, first);
done:
    if (archiveLock != INVALID_HANDLE_VALUE) CloseHandle(archiveLock);
    Util_Free(dialog.values);
    Util_Free(dialog.names);
    Util_Free(listed);
    Util_Free(keys);
    return ok;
}

/* ------------------------------------------------------------- the vault */

/* A time (ms since 1970) as the user's short date and time. */
static void FormatWhen(ULONGLONG ms, WCHAR *out, size_t cch)
{
    WCHAR date[64], time[64];
    out[0] = 0;
    if (Util_FormatWhen(ms, 0, date, ARRAYSIZE(date), time, ARRAYSIZE(time))) StringCchPrintfW(out, cch, L"%s  %s", date, time);
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
        SetDlgItemTextW(dialog, IDC_R_TEXT, TR(L"Choose a saved version, then search and select sessions to recover. Sessions deleted in Claude stay deleted."));
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
    {
        VaultRestoreItem *items = NULL;
        int count = SessionVault_RestoreItems(profiles, dialog.profile, dialog.list, dialog.versions[dialog.chosen].name, &items), i, n = 0;
        BOOL *checked = count >= 0 ? (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)max(count, 1) * sizeof *checked) : NULL;
        const WCHAR **keys = count >= 0 ? (const WCHAR **)HeapAlloc(GetProcessHeap(), 0, (size_t)max(count, 1) * sizeof *keys) : NULL;
        BOOL accepted = FALSE;
        if (items && checked && keys) {
            accepted = VaultTools_Choose(owner, items, count, p->syncGroup != 0, checked);
            if (accepted) for (i = 0; i < count; i++) if (checked[i] && items[i].status < 3) keys[n++] = items[i].key;
        } else Ui_Message(owner, MB_ICONERROR, TR(L"Sessions could not be loaded."));
        ZeroMemory(&report, sizeof report);
        ok = accepted && n > 0 && SessionVault_RestoreSelected(profiles, dialog.profile, dialog.list, dialog.versions[dialog.chosen].name, keys, n, &report);
        if (items) HeapFree(GetProcessHeap(), 0, items);
        if (keys) HeapFree(GetProcessHeap(), 0, (void *)keys);
        if (checked) HeapFree(GetProcessHeap(), 0, checked);
        if (!accepted || !n) { HeapFree(GetProcessHeap(), 0, dialog.versions); return FALSE; }
    }
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

/* ------------------------------------------------------- what to sync */

/* The images of what syncs, the conflicts' list and the what-to-sync list
 * alike: a conflict's choice, then what each row is, then what a setting's
 * parts are (a session, a folder, a list or an object, anything else). */
enum {
    IMAGE_CHOSEN, IMAGE_CANDIDATE, IMAGE_SESSION, IMAGE_SIDEBAR, IMAGE_DETAILS, IMAGE_APPEARANCE, IMAGE_LANGUAGE, IMAGE_MODEL,
    IMAGE_SETTING, IMAGE_PERMISSION, IMAGE_NEW, IMAGE_TITLE, IMAGE_ARCHIVE, IMAGE_DELETE, IMAGE_STAR, IMAGE_GROUP, IMAGE_ORDER,
    IMAGE_FILTER, IMAGE_WIDTH, IMAGE_PRESET, IMAGE_PANE, IMAGE_UNREAD, IMAGE_COST, IMAGE_CODE, IMAGE_ZOOM, IMAGE_SPELLING,
    IMAGE_COWORK, IMAGE_REMOTE, IMAGE_RECENT, IMAGE_LOCK, IMAGE_TRUSTED, IMAGE_PIN, IMAGE_SECTION, IMAGE_VALUE, IMAGES
};
static const WCHAR kSyncGlyphs[IMAGES] = {
    0xEC61, 0xEA3A, 0xE8BD, 0xE90C, 0xE946, 0xE8D2, 0xE774, 0xE99A,
    0xE713, 0xEA18, 0xE710, 0xE8AC, 0xE7B8, 0xE74D, 0xE734, 0xE8B7, 0xE8CB,
    0xE71C, 0xE740, 0xE9E9, 0xE90D, 0xE715, 0xE8EF, 0xE943, 0xE71E, 0xE82D,
    0xE716, 0xE703, 0xE81C, 0xE72E, 0xED25, 0xE718, 0xE8FD, 0xEA37
};
static const ThemeTint kSyncTints[IMAGES] = {
    THEME_TINT_GREEN, THEME_TINT_NONE, THEME_TINT_PURPLE, THEME_TINT_AMBER, THEME_TINT_TEAL, THEME_TINT_PURPLE, THEME_TINT_PURPLE, THEME_TINT_GOLD,
    THEME_TINT_BLUE, THEME_TINT_RED, THEME_TINT_GREEN, THEME_TINT_BLUE, THEME_TINT_AMBER, THEME_TINT_RED, THEME_TINT_GOLD, THEME_TINT_AMBER, THEME_TINT_TEAL,
    THEME_TINT_TEAL, THEME_TINT_TEAL, THEME_TINT_GOLD, THEME_TINT_TEAL, THEME_TINT_BLUE, THEME_TINT_GREEN, THEME_TINT_BLUE, THEME_TINT_TEAL, THEME_TINT_PURPLE,
    THEME_TINT_TEAL, THEME_TINT_BLUE, THEME_TINT_TEAL, THEME_TINT_RED, THEME_TINT_AMBER, THEME_TINT_TEAL, THEME_TINT_TEAL, THEME_TINT_NONE
};

/* Each group of what syncs, in Core_SyncGroup's order (its SYNC_ITEM_* mask):
 * its heading, its image, and its name as settings.ini keeps it opened. */
static const CheckGroup kSyncKinds[SYNC_GROUP_COUNT] = {
    { SYNC_ITEM_SESSIONS, L"Sessions", IMAGE_SESSION, L"sessions" },
    { SYNC_ITEM_SIDEBAR, L"Sidebar", IMAGE_SIDEBAR, L"sidebar" },
    { SYNC_ITEM_DETAILS, L"Session details", IMAGE_DETAILS, L"details" },
    { SYNC_ITEM_APPEARANCE, L"Appearance", IMAGE_APPEARANCE, L"appearance" },
    { SYNC_ITEM_LANGUAGE, L"Interface language", IMAGE_LANGUAGE, L"language" },
    { SYNC_ITEM_MODEL, L"Default model", IMAGE_MODEL, L"model" },
    { SYNC_ITEM_SETTINGS, L"Settings", IMAGE_SETTING, L"settings" },
    { SYNC_ITEM_PERMISSIONS, L"Permissions", IMAGE_PERMISSION, L"permissions" },
};
#define ITEMS_OPENED_SECTION L"SyncItemsOpened"

/* Each item under its group, as the person sees it; a group of one item is a heading alone. */
static const CheckItem kSyncItemRows[] = {
    { SYNC_ITEM_SESSIONS_NEW, L"New sessions", IMAGE_NEW },
    { SYNC_ITEM_SESSIONS_TITLES, L"Titles", IMAGE_TITLE },
    { SYNC_ITEM_SESSIONS_ARCHIVED, L"Archived or not", IMAGE_ARCHIVE },
    { SYNC_ITEM_SESSIONS_DELETED, L"Deleted sessions", IMAGE_DELETE },
    { SYNC_ITEM_SIDEBAR_PINS, L"Stars and pins", IMAGE_STAR },
    { SYNC_ITEM_SIDEBAR_GROUPS, L"Groups and sections", IMAGE_GROUP },
    { SYNC_ITEM_SIDEBAR_ORDER, L"Project order", IMAGE_ORDER },
    { SYNC_ITEM_SIDEBAR_FILTERS, L"Filters, grouping and sorting", IMAGE_FILTER },
    { SYNC_ITEM_SIDEBAR_LAYOUT, L"Width and the place of Routines", IMAGE_WIDTH },
    { SYNC_ITEM_DETAILS_MODEL, L"Model, effort and session presets", IMAGE_PRESET },
    { SYNC_ITEM_DETAILS_PANE, L"Side pane", IMAGE_PANE },
    { SYNC_ITEM_DETAILS_UNREAD, L"Unread marks", IMAGE_UNREAD },
    { SYNC_ITEM_DETAILS_COST, L"Cost", IMAGE_COST },
    { SYNC_ITEM_APPEARANCE_FONTS, L"Fonts", IMAGE_APPEARANCE },
    { SYNC_ITEM_APPEARANCE_EDITOR, L"Editor", IMAGE_CODE },
    { SYNC_ITEM_APPEARANCE_ZOOM, L"Zoom", IMAGE_ZOOM },
    { SYNC_ITEM_APPEARANCE_SPELLING, L"Spelling", IMAGE_SPELLING },
    { SYNC_ITEM_SETTINGS_ARCHIVE, L"Archive when the PR closes", IMAGE_ARCHIVE },
    { SYNC_ITEM_SETTINGS_COWORK, L"Cowork", IMAGE_COWORK },
    { SYNC_ITEM_SETTINGS_REMOTE, L"Remote Control", IMAGE_REMOTE },
    { SYNC_ITEM_SETTINGS_RECENTS, L"Cowork's recent folders", IMAGE_RECENT },
    { SYNC_ITEM_SETTINGS_CODE, L"Code: scheduled tasks, links in previews, going on after a usage limit", IMAGE_CODE },
    { SYNC_ITEM_PERMISSIONS_MODES, L"Folders' permission modes", IMAGE_LOCK },
    { SYNC_ITEM_PERMISSIONS_TRUSTED, L"Cowork's trusted folders", IMAGE_TRUSTED },
};

/* What to sync, as a checked list whose groups fold (checklist.c): `items`
 * checked, the groups opened before open. */
static void SetupItemsList(CheckList *checks, HWND list, DWORD items)
{
    ZeroMemory(checks, sizeof *checks);
    checks->groups = kSyncKinds;
    checks->groupCount = SYNC_GROUP_COUNT;
    checks->items = kSyncItemRows;
    checks->itemCount = (int)ARRAYSIZE(kSyncItemRows);
    checks->openedSection = ITEMS_OPENED_SECTION;
    checks->offered = SYNC_ITEMS_ALL;
    checks->checked = items;
    CheckList_Setup(checks, list, kSyncGlyphs, kSyncTints, IMAGES);
}

void SyncUi_FillItemsList(HWND list, DWORD items)
{
    static CheckList shown;   /* a list alone, as the tests show it */
    SetupItemsList(&shown, list, items);
}

/* The what-to-sync groups and items, and the glyphs they draw with, shared
 * so a backup's list shows the same ones (backup.c builds its own list from
 * these plus its content parts). */
const CheckGroup *SyncUi_Groups(int *count)
{
    if (count) *count = SYNC_GROUP_COUNT;
    return kSyncKinds;
}

const CheckItem *SyncUi_Items(int *count)
{
    if (count) *count = (int)ARRAYSIZE(kSyncItemRows);
    return kSyncItemRows;
}

const WCHAR *SyncUi_Glyphs(const ThemeTint **tints, int *images)
{
    if (tints) *tints = kSyncTints;
    if (images) *images = IMAGES;
    return kSyncGlyphs;
}

static BOOL HasItemRow(const CheckList *checks, DWORD bit)
{
    int i;
    for (i = 0; i < checks->itemCount; i++)
        if (checks->items[i].bit == bit) return TRUE;
    return FALSE;
}

static BOOL GroupHasItemRows(const CheckList *checks, int g)
{
    int i;
    for (i = 0; i < checks->itemCount; i++)
        if ((checks->items[i].bit & checks->groups[g].mask) && (checks->items[i].bit & checks->offered)) return TRUE;
    return FALSE;
}

#define LAYOUT_PART_DEPTH 3   /* the levels of a setting's parts shown one by one */

BOOL SyncUi_SessionName(void *context, const WCHAR *text, WCHAR *name, size_t cch)
{
    const SessionNames *names = (const SessionNames *)context;
    const WCHAR *id = text;
    int i;
    if (!names) return FALSE;
    if (wcsncmp(id, L"code:", 5) == 0) id += 5;   /* a pin's: the kind of session, then its id */
    for (i = 0; i < names->count; i++) {
        const ArchivedSession *session = &names->sessions[i];
        if ((session->own[0] && Core_EqualsI(session->own, id)) || Core_EqualsI(session->key, id) ||
            (wcsncmp(id, L"local_", 6) == 0 && Core_EqualsI(session->key, id + 6)))
            return session->title[0] && SUCCEEDED(StringCchCopyW(name, cch, session->title));
    }
    return FALSE;
}

/* The image of a part that belongs to item `bit`: its item's, else its group's (`g`). */
static int PartImage(const CheckList *checks, DWORD bit, int g)
{
    int i;
    for (i = 0; i < checks->itemCount; i++)
        if (checks->items[i].bit == bit) return checks->items[i].image;
    return checks->groups[g].image;
}

static BOOL ValueHasParts(const CheckList *checks, int v)
{
    return v + 1 < checks->valueCount && checks->values[v + 1].parent == v + 1;
}

void SyncUi_AddLayoutValues(CheckList *checks, int cap, const char *layout, size_t length, DWORD items, BOOL boxed, const SessionNames *names)
{
    LayoutPart part;
    CheckParts parts;
    int first = checks->valueCount, p, g, i, j;
    ZeroMemory(&parts, sizeof parts);
    parts.cap = cap;
    parts.depth = LAYOUT_PART_DEPTH;
    parts.name = names ? SyncUi_SessionName : NULL;
    parts.context = (void *)names;
    parts.named = IMAGE_SESSION;
    parts.folder = IMAGE_GROUP;
    parts.list = IMAGE_SECTION;
    parts.leaf = IMAGE_VALUE;
    for (p = 0; layout && checks->values && checks->valueCount < cap && SessionSync_LayoutPart(p, &part); p++) {
        int at = checks->valueCount;
        CheckValue *value = &checks->values[at];
        const char *raw;
        size_t rawLength;
        if (!(part.item & items) || !part.label || !Core_JsonMember(layout, length, part.name, &raw, &rawLength)) continue;
        ZeroMemory(value, sizeof *value);
        if (!CheckList_FromJson(value, raw, rawLength)) continue;
        for (g = 0; g < checks->groupCount && !(checks->groups[g].mask & part.item); g++)
            ;
        if (g == checks->groupCount) continue;
        value->group = g;
        value->item = HasItemRow(checks, part.item) ? part.item : 0;
        value->image = PartImage(checks, part.item, g);
        value->label = part.label;
        value->pick = -1;
        value->data = p;
        value->boxed = boxed;
        value->taken = boxed && (checks->checked & part.item) != 0;
        checks->valueCount++;
        CheckList_AddParts(checks, at, &parts);
    }
    /* An item's only value on its row; a group's only one, without item rows, on its heading: one without parts. */
    for (i = first; i < checks->valueCount; i++) {
        const CheckValue *value = &checks->values[i];
        int same = 0;
        if (value->parent) continue;
        for (j = first; j < checks->valueCount; j++)
            same += !checks->values[j].parent &&
                    (value->item ? checks->values[j].item == value->item : checks->values[j].group == value->group && !checks->values[j].item);
        checks->values[i].merged = same == 1 && (value->item || !GroupHasItemRows(checks, value->group)) && !ValueHasParts(checks, i);
    }
}

char *SyncUi_LayoutChanges(const CheckList *checks, int first, const char *layout, size_t length, size_t *outLength)
{
    char *changes = NULL;
    size_t used = 0;
    int i;
    *outLength = 0;
    for (i = first; checks->values && layout && i < checks->valueCount; i++) {
        LayoutPart part;
        const char *was, *now;
        char *made = NULL;
        size_t wasLength, nowLength = 0;
        if (checks->values[i].parent || !SessionSync_LayoutPart((int)checks->values[i].data, &part) ||
            !Core_JsonMember(layout, length, part.name, &was, &wasLength))
            continue;
        if (CheckList_IsLeftOut(checks, i)) {
            now = "null";   /* left out: not in the layout written */
            nowLength = 4;
        } else if ((made = CheckList_JsonOf(checks, i, &nowLength)) != NULL) {
            now = made;
        } else {
            continue;
        }
        if (!changes && (changes = Util_CopyBytes("{}", 2)) != NULL) used = 2;
        if (changes) changes = SessionSync_SetMember(changes, &used, part.name, now, nowLength);
        Util_Free(made);
        if (!changes) return NULL;
    }
    *outLength = used;
    return changes;
}

typedef struct ItemsDialog {
    const WCHAR *text;
    DWORD        items;
    CheckList    rows;
} ItemsDialog;

/* What the boxes checked say; OK only with one at least. */
static void ReadItems(HWND dialog, ItemsDialog *state)
{
    state->items = state->rows.checked;
    EnableWindow(GetDlgItem(dialog, IDOK), state->items != 0);
}

static INT_PTR CALLBACK ItemsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ItemsDialog *state = (ItemsDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG:
        state = (ItemsDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_I_TEXT, state->text);
        SetupItemsList(&state->rows, GetDlgItem(dialog, IDC_I_LIST), state->items);
        Theme_SmoothView(state->rows.list);
        ReadItems(dialog, state);
        return TRUE;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_I_NOTE);

    case WM_NOTIFY:
    case WM_CHECKLIST_BOX:
    case WM_CONTEXTMENU: {
        BOOL toggled;
        if (state && CheckList_Message(&state->rows, dialog, message, wp, lp, &toggled)) {
            if (toggled) ReadItems(dialog, state);
            return TRUE;
        }
        break;
    }

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
    Profiles_JoinNames(profiles, members, names, ARRAYSIZE(names));
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
    CheckList          items;
    int                boxProfile[MAX_PROFILES];   /* each check box's profile */
    int                boxCount;
} SetupDialog;

#define PARTNER_GAP_DIPS 16   /* between two profiles' check boxes in their row */

typedef struct BelowRow {
    HWND dialog;
    int  from, by;   /* the controls whose top is at `from` or lower move down `by` */
} BelowRow;

static BOOL CALLBACK MoveBelow(HWND child, LPARAM param)
{
    const BelowRow *below = (const BelowRow *)param;
    RECT rc;
    int id = GetDlgCtrlID(child);
    if (GetParent(child) != below->dialog || (id >= IDC_Z_PROFILE && id < IDC_Z_PROFILE + MAX_PROFILES) || !GetWindowRect(child, &rc))
        return TRUE;
    MapWindowPoints(NULL, below->dialog, (POINT *)&rc, 2);
    if (rc.top >= below->from) SetWindowPos(child, NULL, rc.left, rc.top + below->by, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    return TRUE;
}

void SyncUi_PartnerBoxes(HWND dialog, const WCHAR *const *names, const BOOL *checked, int count)
{
    HWND place = GetDlgItem(dialog, IDC_Z_LIST), previous = place, box;
    HFONT font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    WCHAR text[LABEL_CCH * 2];
    RECT area, frame;
    SIZE need;
    BelowRow below;
    int i, x, y, width, rowHeight, gap = MulDiv(PARTNER_GAP_DIPS, (int)GetDpiForWindow(dialog), 96);
    if (!place || !GetWindowRect(place, &area)) return;
    MapWindowPoints(NULL, dialog, (POINT *)&area, 2);
    rowHeight = area.bottom - area.top;
    x = area.left;
    y = area.top;
    for (i = 0; i < count && i < MAX_PROFILES; i++) {
        const WCHAR *ampersand, *from = names[i];
        /* A name's own ampersands are no access keys. */
        text[0] = 0;
        while ((ampersand = wcschr(from, L'&')) != NULL) {
            StringCchCatNW(text, ARRAYSIZE(text), from, (size_t)(ampersand - from) + 1);
            StringCchCatW(text, ARRAYSIZE(text), L"&");
            from = ampersand + 1;
        }
        StringCchCatW(text, ARRAYSIZE(text), from);
        box = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, x, y, area.right - area.left,
                              rowHeight, dialog, (HMENU)(INT_PTR)(IDC_Z_PROFILE + i), g_hInst, NULL);
        if (!box) continue;
        SendMessageW(box, WM_SETFONT, (WPARAM)font, FALSE);
        width = Theme_CheckBoxSize(box, &need) ? min(need.cx + 2, area.right - area.left) : area.right - area.left;
        if (x > area.left && x + width > area.right) {   /* another row */
            x = area.left;
            y += rowHeight;
        }
        SetWindowPos(box, previous, x, y, width, rowHeight, SWP_NOACTIVATE);   /* the tab order: after the one before */
        CheckDlgButton(dialog, IDC_Z_PROFILE + i, checked[i] ? BST_CHECKED : BST_UNCHECKED);
        previous = box;
        x += width + gap;
    }
    /* More rows than the one the template keeps: what is below moves down, the dialog grows. */
    if (y > area.top && GetWindowRect(dialog, &frame)) {
        below.dialog = dialog;
        below.from = area.bottom;
        below.by = y - area.top;
        EnumChildWindows(dialog, MoveBelow, (LPARAM)&below);
        SetWindowPos(dialog, NULL, 0, 0, frame.right - frame.left, frame.bottom - frame.top + below.by, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    ShowWindow(place, SW_HIDE);   /* the row is the boxes' now: its empty place lies under none of them */
}

/* What the boxes checked say: what to sync only once it syncs with one at least, and OK with something to sync. */
static void ReadSetup(HWND dialog, SetupDialog *state)
{
    int box;
    state->setup->partners = 0;
    for (box = 0; box < state->boxCount; box++)
        if (IsDlgButtonChecked(dialog, IDC_Z_PROFILE + box) == BST_CHECKED) state->setup->partners |= 1u << state->boxProfile[box];
    state->setup->items = state->items.checked;
    EnableWindow(GetDlgItem(dialog, IDC_Z_ITEMS_LABEL), state->setup->partners != 0);
    EnableWindow(state->items.list, state->setup->partners != 0);
    EnableWindow(GetDlgItem(dialog, IDC_Z_SYNC_NOW), state->setup->partners && state->setup->items);
    EnableWindow(GetDlgItem(dialog, IDOK), !state->setup->partners || state->setup->items);
}

/* Sync settings, laid out as an export's: the profiles to sync with, a
 * check box each in a row at the top, then what they sync, the list on the
 * left and its Preview pane on the right (checklist.c). */
static INT_PTR CALLBACK SetupProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SetupDialog *state = (SetupDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        WCHAR text[LABEL_CCH + 64];
        const WCHAR *names[MAX_PROFILES];
        BOOL checked[MAX_PROFILES];
        int p;
        state = (SetupDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"\x201C%s\x201D syncs with:"), state->profiles->items[state->setup->profile].name);
        SetDlgItemTextW(dialog, IDC_Z_TEXT, text);
        for (p = 0; p < state->profiles->count; p++) {
            if (p == state->setup->profile) continue;
            state->boxProfile[state->boxCount] = p;
            names[state->boxCount] = state->profiles->items[p].name;
            checked[state->boxCount++] = (state->setup->partners & (1u << p)) != 0;
        }
        SyncUi_PartnerBoxes(dialog, names, checked, state->boxCount);
        SetupItemsList(&state->items, GetDlgItem(dialog, IDC_Z_ITEMS), state->setup->items);
        Theme_SmoothView(state->items.list);
        ReadSetup(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_Z_NOTE);

    case WM_NOTIFY:
    case WM_CHECKLIST_BOX:
    case WM_CONTEXTMENU: {
        BOOL toggled;
        if (state && CheckList_Message(&state->items, dialog, message, wp, lp, &toggled)) {
            if (toggled) ReadSetup(dialog, state);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wp) >= IDC_Z_PROFILE && LOWORD(wp) < IDC_Z_PROFILE + state->boxCount) {
            if (HIWORD(wp) == BN_CLICKED) ReadSetup(dialog, state);
            return TRUE;
        }
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

#define CONFLICTS_SHOWN 256   /* the elements the dialog lists at most */

/* The conflicts are the property list, read-only (checklist.c): a heading
 * per kind of what syncs, open, each element below it with a version per
 * profile in a column of its own, the one kept checked. */
typedef struct ConflictsDialog {
    const ProfileList *profiles;
    VaultConflict     *conflicts;
    int                count;
    int               *chosen;            /* by conflict: the profile whose version it keeps */
    int                columns[MAX_PROFILES];   /* the profile of each column after the first */
    int                columnCount;
    const WCHAR       *text;
    CheckList          list;
    CheckValue        *values;            /* a conflict each, a heap block (FreeConflictsList) */
    const WCHAR      **versions;          /* each conflict's version in each column, NULL where it has none (a heap block) */
    const WCHAR       *titles[1 + MAX_PROFILES];
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
    for (k = 0; k < ARRAYSIZE(kSyncKinds); k++)
        if (kSyncKinds[k].mask & conflict->item) return kSyncKinds[k].image;
    return IMAGE_SETTING;
}

static int KindOf(const VaultConflict *conflict)
{
    int k;
    for (k = 0; k < (int)ARRAYSIZE(kSyncKinds); k++)
        if (kSyncKinds[k].mask & conflict->item) return k;
    return (int)ARRAYSIZE(kSyncKinds) - 1;
}

static void FreeConflictsList(ConflictsDialog *state)
{
    Util_Free(state->values);
    Util_Free((void *)state->versions);
    state->values = NULL;
    state->versions = NULL;
}

/* The list in `list`: the kinds' headings open, each conflict a pick of the
 * versions its profiles have, the one chosen kept; the first selected, so
 * that the arrows choose from the start. */
static void FillConflicts(HWND list, ConflictsDialog *state)
{
    int c, i, k;
    ZeroMemory(&state->list, sizeof state->list);
    state->values = (CheckValue *)Util_Alloc((size_t)max(state->count, 1) * sizeof *state->values);
    state->versions = (const WCHAR **)Util_Alloc((size_t)max(state->count * state->columnCount, 1) * sizeof *state->versions);
    state->titles[0] = TR(L"Changed");
    for (i = 0; i < state->columnCount; i++) state->titles[i + 1] = state->profiles->items[state->columns[i]].name;
    state->list.groups = kSyncKinds;
    state->list.groupCount = SYNC_GROUP_COUNT;
    state->list.columns = state->titles;
    state->list.columnCount = 1 + state->columnCount;
    state->list.plain = TRUE;
    state->list.pickedImage = IMAGE_CHOSEN;
    state->list.candidateImage = IMAGE_CANDIDATE;
    state->list.openAtFirst = (DWORD)-1;
    state->list.values = state->values;
    for (c = 0; state->values && state->versions && c < state->count; c++) {
        CheckValue *value = &state->values[c];
        const WCHAR **versions = &state->versions[c * state->columnCount];
        value->group = KindOf(&state->conflicts[c]);
        value->kind = CHECK_VALUE_PICK;
        value->image = ConflictImage(&state->conflicts[c]);
        value->label = state->conflicts[c].label;
        value->literal = TRUE;
        value->pick = -1;
        for (i = 0; i < state->columnCount; i++) {
            int p = state->columns[i];
            versions[i] = (state->conflicts[c].members & (1u << p)) ? state->conflicts[c].shown[p] : NULL;
            if (versions[i] && state->chosen[c] == p) value->pick = i;
        }
        value->versions = versions;
        state->list.valueCount++;
    }
    CheckList_Setup(&state->list, list, kSyncGlyphs, kSyncTints, IMAGES);
    for (k = 0; k < SYNC_GROUP_COUNT; k++)
        for (c = 0; c < state->list.valueCount; c++)
            if (state->values[c].group == k) {
                CheckList_SelectValue(&state->list, c, FALSE);
                return;
            }
}

/* Every element: the latest version of each (`choice` 0), or the one of the
 * profile of column `choice` - 1 where it has one. */
static void ChooseAll(ConflictsDialog *state, LRESULT choice)
{
    int c, i;
    for (c = 0; c < state->list.valueCount; c++) {
        CheckValue *value = &state->values[c];
        int p = choice <= 0 ? state->conflicts[c].latest : choice - 1 < state->columnCount ? state->columns[choice - 1] : -1;
        for (i = 0; i < state->columnCount; i++)
            if (state->columns[i] == p && value->versions[i]) value->pick = i;
    }
    CheckList_Refresh(&state->list);
}

/* The versions kept, by conflict. */
static void ReadChoices(ConflictsDialog *state)
{
    int c;
    for (c = 0; c < state->list.valueCount; c++)
        if (state->values[c].pick >= 0 && state->values[c].pick < state->columnCount) state->chosen[c] = state->columns[state->values[c].pick];
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
        if ((state->icon = Theme_LoadIcon(NULL, IDI_INFORMATION, GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(dialog)),
                                          GetSystemMetricsForDpi(SM_CYICON, GetDpiForWindow(dialog)))) != NULL)
            SendDlgItemMessageW(dialog, IDC_X_ICON, STM_SETICON, (WPARAM)state->icon, 0);
        Theme_SetGlyph(GetDlgItem(dialog, IDOK), 0xE73E, THEME_TINT_GREEN);
        Theme_SetGlyph(GetDlgItem(dialog, IDCANCEL), 0xE823, THEME_TINT_NONE);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)TR(L"The latest of each"));
        for (i = 0; i < state->columnCount; i++)
            SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)state->profiles->items[state->columns[i]].name);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_SETCURSEL, 0, 0);
        FillConflicts(GetDlgItem(dialog, IDC_X_LIST), state);
        Theme_SmoothView(state->list.list);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_X_NOTE);

    case WM_NOTIFY:
    case WM_CONTEXTMENU: {
        BOOL toggled;
        if (state && state->values && CheckList_Message(&state->list, dialog, message, wp, lp, &toggled)) return TRUE;
        break;
    }

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_X_PROFILE:
            if (HIWORD(wp) == CBN_SELCHANGE && state->values) ChooseAll(state, SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_GETCURSEL, 0, 0));
            return TRUE;
        case IDOK:
            if (state->values) ReadChoices(state);
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
    Profiles_JoinNames(profiles, members, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s changed these each their own way since the last sync. Choose the version to keep of each:"), names);
    dialog.text = text;
    if (Ui_Dialog(owner, IDD_CONFLICTS, ConflictsProc, (LPARAM)&dialog) != IDOK) goto done;
    ok = SessionVault_Decide(profiles, group, dialog.conflicts, chosen, dialog.count);
    if (!ok) Ui_Message(owner, MB_ICONERROR, TR(L"The choice could not be saved: the profiles keep their own until the next sync asks again."));
done:
    FreeConflictsList(&dialog);
    if (dialog.conflicts) HeapFree(GetProcessHeap(), 0, dialog.conflicts);
    if (chosen) HeapFree(GetProcessHeap(), 0, chosen);
    return ok;
}

CheckList *SyncUi_FillConflictsList(HWND list, const WCHAR *name)
{
    static ConflictsDialog shown;
    static ProfileList profiles;
    static VaultConflict conflicts[2];
    static int chosen[2];
    int c;
    FreeConflictsList(&shown);
    ZeroMemory(&shown, sizeof shown);
    ZeroMemory(&profiles, sizeof profiles);
    ZeroMemory(conflicts, sizeof conflicts);
    profiles.count = 2;
    StringCchCopyW(profiles.items[0].name, ARRAYSIZE(profiles.items[0].name), name);
    StringCchCopyW(profiles.items[1].name, ARRAYSIZE(profiles.items[1].name), L"Personal");
    for (c = 0; c < 2; c++) {
        StringCchCopyA(conflicts[c].part, ARRAYSIZE(conflicts[c].part), "groups");
        conflicts[c].item = SYNC_ITEM_SIDEBAR_GROUPS;
        conflicts[c].members = 0x3;
        conflicts[c].latest = c;
        chosen[c] = c;
        StringCchPrintfW(conflicts[c].label, ARRAYSIZE(conflicts[c].label), c ? TR(L"Group of \x201C%s\x201D") : TR(L"Name of group \x201C%s\x201D"),
                         L"A private conversation");
        StringCchCopyW(conflicts[c].shown[0], ARRAYSIZE(conflicts[c].shown[0]), L"Work");
        StringCchCopyW(conflicts[c].shown[1], ARRAYSIZE(conflicts[c].shown[1]), L"Personal work");
    }
    shown.profiles = &profiles;
    shown.conflicts = conflicts;
    shown.count = 2;
    shown.chosen = chosen;
    shown.columns[0] = 0;
    shown.columns[1] = 1;
    shown.columnCount = 2;
    FillConflicts(list, &shown);
    return &shown.list;
}

DWORD SyncUi_SettleConflicts(HWND owner, const ProfileList *profiles)
{
    DWORD settled = 0;
    int group;
    for (group = 1; group <= MAX_PROFILES; group++) {
        DWORD members = SessionVault_Group(profiles, group);
        if (Core_BitCount(members) >= 2 && SettleGroup(owner, profiles, group)) settled |= members;
    }
    return settled;
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
    int i, rows = ListView_GetItemCount(state->rows), chosen = 0, restorable = 0, openable = 0;
    for (i = 0; i < rows; i++) {
        LVITEMW item;
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_PARAM;
        item.iItem = i;
        if (!ListView_GetItem(state->rows, &item) || item.lParam < 0 || item.lParam >= state->count) continue;
        if ((state->checked[item.lParam] = ListView_GetCheckState(state->rows, i)) != FALSE) {
            chosen++;
            if (state->items[item.lParam].restorable) restorable++;
            if (SessionPurge_CanOpen(&state->items[item.lParam])) openable++;
        }
    }
    EnableWindow(GetDlgItem(dialog, IDOK), chosen > 0);
    EnableWindow(GetDlgItem(dialog, IDC_C_RESTORE), restorable > 0);
    EnableWindow(GetDlgItem(dialog, IDC_PURGE_OPEN), openable > 0 && openable == chosen);
    EnableWindow(GetDlgItem(dialog, IDC_C_ALL), rows > 0);
    CheckDlgButton(dialog, IDC_C_ALL, rows > 0 && chosen == rows ? BST_CHECKED : BST_UNCHECKED);
}

/* A conversation's row: the profiles that had it (or in no list), its
 * title (its id without one), when it was last written, its size. */
static void PurgeRow(const PurgeDialog *state, const PurgeItem *item, WCHAR *out, size_t cch)
{
    WCHAR when[128], size[32], names[MAX_PROFILES * (LABEL_CCH + 2)];
    Profiles_JoinNames(state->profiles, item->profiles, names, ARRAYSIZE(names));
    FormatWhen(item->written, when, ARRAYSIZE(when));
    Core_FormatSize(item->bytes, size, ARRAYSIZE(size));
    StringCchPrintfW(out, cch, L"%s  \x00B7  %s  \x00B7  %s  \x00B7  %s", names[0] ? names : TR(L"In no list"),
                     item->title[0] ? item->title : item->id, when, size);
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
        Theme_CheckRows(state->rows);
        FillPurgeRows(dialog, state);
        Theme_SmoothView(state->rows);
        return TRUE;
    }

    case WM_NOTIFY:
        if (state && Theme_CheckRowChanged((const NMHDR *)lp, state->rows)) {
            if (!state->filling) ReadChecked(dialog, state);
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
        case IDC_PURGE_OPEN:
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
    ShowReport(owner, profiles, &report, first);
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

typedef struct OpenOrphanDialog { const ProfileList *profiles; int chosen, count; int map[MAX_PROFILES]; } OpenOrphanDialog;
static INT_PTR CALLBACK OpenOrphanProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    OpenOrphanDialog *state = (OpenOrphanDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        int i; WCHAR entries[LONG_PATH_CCH];
        state = (OpenOrphanDialog *)lp; SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_PURGE_EXPLAIN, TR(L"Claude opens the checked local conversations through Resume and adds them to its own list. Claude may connect to its service. No entry is written by the manager."));
        for (i = 0; i < state->profiles->count; i++) if (SessionStore_EntriesDir(&state->profiles->items[i], entries, ARRAYSIZE(entries), NULL)) {
            SendDlgItemMessageW(dialog, IDC_PURGE_TARGET, CB_ADDSTRING, 0, (LPARAM)state->profiles->items[i].name);
            state->map[state->count++] = i;
        }
        SendDlgItemMessageW(dialog, IDC_PURGE_TARGET, CB_SETCURSEL, 0, 0);
        EnableWindow(GetDlgItem(dialog, IDOK), state->count > 0); return TRUE;
    }
    if (state && message == WM_COMMAND && LOWORD(wp) == IDOK) {
        LRESULT at = SendDlgItemMessageW(dialog, IDC_PURGE_TARGET, CB_GETCURSEL, 0, 0);
        if (at >= 0 && at < state->count) { state->chosen = state->map[at]; EndDialog(dialog, IDOK); } return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
static BOOL OpenOrphans(HWND owner, const ProfileList *profiles, const PurgeItem *items, const int *chosen, int count)
{
    OpenOrphanDialog dialog; ClaudePackage package;
    int i, opened = 0; HRESULT hr = S_OK;
    ZeroMemory(&dialog, sizeof dialog); dialog.profiles = profiles; dialog.chosen = -1;
    if (!Claude_FindPackage(&package)) { Ui_Message(owner, MB_ICONWARNING, TR(L"Claude could not be found.")); return FALSE; }
    if (Ui_Dialog(owner, IDD_PURGE_OPEN, OpenOrphanProc, (LPARAM)&dialog) != IDOK || dialog.chosen < 0) return FALSE;
    for (i = 0; i < count; i++) {
        if (!SessionPurge_CanOpen(&items[chosen[i]])) { hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND); break; }
        hr = SessionEdit_Open(&package, &profiles->items[dialog.chosen], items[chosen[i]].id);
        if (FAILED(hr)) break;
        opened++;
    }
    if (FAILED(hr)) Ui_Message(owner, MB_ICONWARNING, TR(L"Some conversations could not be opened (error 0x%08lX)."), (DWORD)hr);
    return opened > 0;
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
    if (action == IDOK || action == IDC_C_RESTORE || action == IDC_PURGE_OPEN)
        for (i = 0; i < count; i++)
            if (dialog.checked[i]) chosen[n++] = i;
    if (action == IDC_PURGE_OPEN && n) {
        changed = OpenOrphans(owner, profiles, items, chosen, n);
    } else if (action == IDC_C_RESTORE && n) {
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
