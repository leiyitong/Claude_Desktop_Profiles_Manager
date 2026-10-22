/*
 * About, from the icon at the toolbar's end: the program's name and version,
 * help (the questions someone new asks, each answered in a few lines) and
 * the changelog embedded in the program, one version per day. On the left a
 * list whose headings fold: Help and its questions, then the changelog's
 * months and their versions; on the right what the row chosen says, a
 * heading all of its own: every answer, the whole changelog, a month's
 * versions.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>

#define HELP_TOPICS            7
#define CHANGELOG_VERSIONS_MAX 4096   /* days: more than ten years of them */

/* The headings: Help, the changelog's title (nothing to fold), then its months. */
#define HEADING_HELP       0
#define HEADING_CHANGELOG  1
#define HEADING_MONTH(m)   (2 + (m))
/* The other rows: a question, or a version after them. */
#define TOPIC_ROW(t)       ((LPARAM)(t))
#define VERSION_ROW(v)     ((LPARAM)(HELP_TOPICS + (v)))

static const WCHAR *Question(int topic)
{
    switch (topic) {
    case 0: return TR(L"What is this program for?");
    case 1: return TR(L"Where are my conversations saved?");
    case 2: return TR(L"How do profiles keep the same conversations?");
    case 3: return TR(L"Why does a change wait for Claude to close?");
    case 4: return TR(L"I deleted a conversation by mistake");
    case 5: return TR(L"What is the session vault?");
    default: return TR(L"Before reinstalling Claude");
    }
}

static const WCHAR *Answer(int topic)
{
    switch (topic) {
    case 0:
        return TR(L"It runs several Claude accounts side by side, each in a window of its own: a profile, with its own sign-in, settings "
                  L"and conversations. Claude itself is not changed.");
    case 1:
        return TR(L"Their content is in the .claude folder of your user folder, shared by every profile. Each profile keeps its own list of them: "
                  L"titles, stars, archived ones.");
    case 2:
        return TR(L"Sync settings\x2026 links profiles. Each time one of them closes, what changed in its list goes to the others.");
    case 3:
        return TR(L"An open Claude keeps its list in memory and writes it back, so a change to an open profile is made once it closes.");
    case 4:
        return TR(L"Backup & Restore\x2026, then Clean up sessions\x2026: check it and click Restore.");
    case 5:
        return TR(L"A copy of each profile's list of sessions, kept outside Claude for the days set in Settings. Recover sessions\x2026 puts "
                  L"a version back.");
    default:
        return TR(L"Back up Main first (Backup & Restore\x2026). Once Claude is installed again, open Main here and sign in; "
                  L"Recover sessions\x2026 brings its list back if it is gone.");
    }
}

/* The changelog of the interface's language as the program holds it. */
typedef struct Changelog {
    ChangelogVersion *versions;
    int count;
    WCHAR *text;
} Changelog;

static void FreeChangelog(Changelog *log)
{
    if (log->versions) HeapFree(GetProcessHeap(), 0, log->versions);
    if (log->text) HeapFree(GetProcessHeap(), 0, log->text);
    ZeroMemory(log, sizeof *log);
}

/* The embedded changelog of the interface's language (English when it has
 * none): FALSE when it cannot be read. */
static BOOL ReadChangelog(Changelog *log)
{
    const WCHAR *code = Localize_LanguageCode(Localize_EffectiveLanguage());
    int id = code && wcscmp(code, L"zh-CN") == 0 ? IDR_CHANGELOG_ZH : IDR_CHANGELOG_EN;
    HRSRC found = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL loaded = found ? LoadResource(NULL, found) : NULL;
    const char *markdown = loaded ? (const char *)LockResource(loaded) : NULL;
    DWORD size = found ? SizeofResource(NULL, found) : 0;
    size_t cch = (size_t)size * 3 + 16;   /* a byte gives a character at most, a line a number and a line end more */
    ZeroMemory(log, sizeof *log);
    if (!markdown || !size) return FALSE;
    log->versions = (ChangelogVersion *)HeapAlloc(GetProcessHeap(), 0, CHANGELOG_VERSIONS_MAX * sizeof(ChangelogVersion));
    log->text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cch * sizeof(WCHAR));
    if (log->versions && log->text) log->count = Core_ChangelogRead(markdown, size, log->versions, CHANGELOG_VERSIONS_MAX, log->text, cch);
    if (!log->count) FreeChangelog(log);
    return log->count > 0;
}

/* Version `name`'s month: "2026.10" of "v2026.10.12". */
static void VersionMonth(const WCHAR *name, WCHAR *month, size_t cch)
{
    StringCchCopyNW(month, cch, name + (name[0] == L'v'), 7);
}

/* Questions [first, last), each a heading with its answer under it, in the text box. */
static void ShowHelp(HWND dialog, int first, int last)
{
    WCHAR text[8192] = L"";
    int i;
    for (i = first; i < last; i++) {
        if (i > first) StringCchCatW(text, ARRAYSIZE(text), L"\n\n");
        StringCchCatW(text, ARRAYSIZE(text), L"### ");
        StringCchCatW(text, ARRAYSIZE(text), Question(i));
        StringCchCatW(text, ARRAYSIZE(text), L"\n\n");
        StringCchCatW(text, ARRAYSIZE(text), Answer(i));
    }
    Theme_SetRichText(GetDlgItem(dialog, IDC_W_TEXT), text);
}

/* Versions [first, last) in the text box, as a release page shows them: each
 * its name as a heading, then what it says. */
static void ShowVersions(HWND dialog, const Changelog *log, int first, int last)
{
    size_t cch = 1, used = 0, length;
    WCHAR *text;
    int i;
    for (i = first; i < last; i++) cch += log->versions[i].to - log->versions[i].from + CHANGELOG_NAME_CCH + 8;
    if ((text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, cch * sizeof(WCHAR))) == NULL) return;
    text[0] = 0;
    for (i = first; i < last; i++) {
        if (i > first) StringCchCatW(text + used, cch - used, L"\n\n");
        StringCchCatW(text + used, cch - used, L"## ");
        StringCchCatW(text + used, cch - used, log->versions[i].name);
        StringCchCatW(text + used, cch - used, L"\n\n");
        used += wcslen(text + used);
        length = log->versions[i].to - log->versions[i].from;
        memcpy(text + used, log->text + log->versions[i].from, length * sizeof(WCHAR));
        used += length;
        text[used] = 0;
    }
    Theme_SetRichText(GetDlgItem(dialog, IDC_W_TEXT), text);
    HeapFree(GetProcessHeap(), 0, text);
}

/* What row `param` says, in the text box: a question's answer, or a
 * version; Help every answer, Changelog every version, a month its own. */
static void ShowRow(HWND dialog, LPARAM param)
{
    Changelog log;
    WCHAR month[16], last[16] = L"";
    int i, first = -1, end, months = -1, heading = THEME_HEADING_OF(param);
    if (param >= 0 && param < HELP_TOPICS) {
        ShowHelp(dialog, (int)param, (int)param + 1);
        return;
    }
    if (param < 0 && heading == HEADING_HELP) {
        ShowHelp(dialog, 0, HELP_TOPICS);
        return;
    }
    if (!ReadChangelog(&log)) return;
    if (param >= HELP_TOPICS) {
        first = (int)param - HELP_TOPICS;
        end = first + 1;
    } else if (heading == HEADING_CHANGELOG) {
        first = 0;
        end = log.count;
    } else {
        for (i = 0, end = log.count; i < log.count; i++) {   /* the month's versions follow each other */
            VersionMonth(log.versions[i].name, month, ARRAYSIZE(month));
            if (wcscmp(month, last) != 0) {
                StringCchCopyW(last, ARRAYSIZE(last), month);
                if (++months == heading - HEADING_MONTH(0)) first = i;
                else if (first >= 0) {
                    end = i;
                    break;
                }
            }
        }
    }
    if (first >= 0 && first < end && end <= log.count) ShowVersions(dialog, &log, first, end);
    FreeChangelog(&log);
}

/* Heading `row` opens, its questions or its month's versions under it, or folds. */
static void FoldHeading(HWND list, int row, BOOL open)
{
    LPARAM param = Theme_RowParam(list, row);
    Changelog log;
    WCHAR month[16], of[16];
    int i, at = row + 1;
    if (row < 0 || param >= 0 || THEME_HEADING_OF(param) == HEADING_CHANGELOG) return;
    Theme_DeleteFoldedRows(list, row);
    Theme_SetFold(list, row, open ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED);
    if (!open) return;
    if (THEME_HEADING_OF(param) == HEADING_HELP) {
        for (i = 0; i < HELP_TOPICS; i++) Theme_InsertFoldingRow(list, at++, Question(i), I_IMAGENONE, TOPIC_ROW(i), 0);
        return;
    }
    if (!ReadChangelog(&log)) return;
    ListView_GetItemText(list, row, 0, month, ARRAYSIZE(month));
    for (i = 0; i < log.count; i++) {
        VersionMonth(log.versions[i].name, of, ARRAYSIZE(of));
        if (wcscmp(of, month) == 0) Theme_InsertFoldingRow(list, at++, log.versions[i].name, I_IMAGENONE, VERSION_ROW(i), 0);
    }
    FreeChangelog(&log);
}

static BOOL HeadingOpen(HWND list, int row)
{
    return (ListView_GetItemState(list, row, LVIS_OVERLAYMASK) & LVIS_OVERLAYMASK) == INDEXTOOVERLAYMASK(THEME_FOLD_OPEN);
}

/* Every heading opened, or every one folded; the keyboard's row stays on its
 * heading when its own row folds. */
static void FoldAll(HWND list, BOOL open)
{
    int row, focus = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
    while (focus > 0 && !open && Theme_RowParam(list, focus) >= 0) focus--;
    for (row = ListView_GetItemCount(list) - 1; row >= 0; row--)
        if (Theme_RowParam(list, row) < 0) FoldHeading(list, row, open);
    if (!open && focus >= 0) Theme_FocusRow(list, focus);
}

void About_Fill(HWND dialog)
{
    HWND list = GetDlgItem(dialog, IDC_W_TOPICS);
    Changelog log;
    LVCOLUMNW column;
    WCHAR text[64], month[16], last[16] = L"";
    int i, row, months = 0, size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(dialog));
    HIMAGELIST indent = ImageList_Create(size, size, ILC_COLOR32, 0, 1);   /* no image: its width is the indent's */
    Theme_SetStrong(GetDlgItem(dialog, IDC_W_NAME));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Version %s"), APP_VERSION_DATE_W);
    SetDlgItemTextW(dialog, IDC_W_VERSION, text);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    if (indent) ListView_SetImageList(list, indent, LVSIL_SMALL);   /* the list destroys it */
    Theme_SetFoldingRows(list);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);
    Theme_InsertFoldingRow(list, 0, TR(L"Help"), I_IMAGENONE, THEME_HEADING_ROW(HEADING_HELP), THEME_FOLD_CLOSED);
    FoldHeading(list, 0, TRUE);
    if (!ReadChangelog(&log)) {
        Util_Log(L"about: the embedded changelog could not be read");
        Theme_FocusRow(list, 1);
        ShowRow(dialog, TOPIC_ROW(0));
        return;
    }
    /* The changelog's months, newest first, folded but the newest one, whose newest version shows. */
    Theme_InsertFoldingRow(list, ListView_GetItemCount(list), TR(L"Changelog"), I_IMAGENONE, THEME_HEADING_ROW(HEADING_CHANGELOG), THEME_FOLD_NONE);
    row = ListView_GetItemCount(list);
    for (i = 0; i < log.count; i++) {
        VersionMonth(log.versions[i].name, month, ARRAYSIZE(month));
        if (wcscmp(month, last) == 0) continue;
        Theme_InsertFoldingRow(list, ListView_GetItemCount(list), month, I_IMAGENONE, THEME_HEADING_ROW(HEADING_MONTH(months++)), THEME_FOLD_CLOSED);
        StringCchCopyW(last, ARRAYSIZE(last), month);
    }
    FreeChangelog(&log);
    FoldHeading(list, row, TRUE);
    Theme_FocusRow(list, row + 1);
    ShowRow(dialog, VERSION_ROW(0));
}

static INT_PTR CALLBACK AboutProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND list = (HWND)GetWindowLongPtrW(dialog, DWLP_USER);   /* the topics, in their smooth view */
    switch (msg) {
    case WM_INITDIALOG:
        list = GetDlgItem(dialog, IDC_W_TOPICS);
        SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR)list);
        About_Fill(dialog);
        Theme_SmoothView(list);
        SetFocus(list);
        return FALSE;
    case WM_CONTEXTMENU: {
        UINT cmd = list ? Theme_FoldMenu(dialog, list, (HWND)wp, lp) : 0;
        if (cmd) FoldAll(list, cmd == THEME_FOLD_MENU_EXPAND);
        return cmd != 0 || (list && ((HWND)wp == list || (HWND)wp == GetParent(list)));
    }
    case WM_NOTIFY: {
        /* By its id: the list is in its smooth view, which took its place under that id. */
        const NMHDR *header = (const NMHDR *)lp;
        if (header->idFrom != IDC_W_TOPICS || !list) break;
        if (header->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED))
                ShowRow(dialog, Theme_RowParam(list, change->iItem));
        } else if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
            /* The arrow folds or opens a heading; so does a double-click on it. */
            LVHITTESTINFO hit;
            ZeroMemory(&hit, sizeof hit);
            hit.pt = ((const NMITEMACTIVATE *)lp)->ptAction;
            if (ListView_SubItemHitTest(list, &hit) >= 0 && Theme_RowParam(list, hit.iItem) < 0 &&
                (Theme_OnFoldArrow(list, hit.iItem, hit.pt) || header->code == NM_DBLCLK))
                FoldHeading(list, hit.iItem, !HeadingOpen(list, hit.iItem));
        } else if (header->code == LVN_KEYDOWN) {
            /* Right opens a heading, Left folds it, or goes from a row up to its heading. */
            WORD key = ((const NMLVKEYDOWN *)lp)->wVKey;
            int row = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
            if (row >= 0 && (key == VK_LEFT || key == VK_RIGHT)) {
                if (Theme_RowParam(list, row) < 0) {
                    FoldHeading(list, row, key == VK_RIGHT);
                } else if (key == VK_LEFT) {
                    while (row > 0 && Theme_RowParam(list, row) >= 0) row--;
                    Theme_FocusRow(list, row);
                }
            }
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_W_LOGS) {
            Diagnostics_OpenLogs(dialog);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_W_DIAGNOSTICS) {
            Diagnostics_Show(dialog);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) EndDialog(dialog, LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}

void About_Show(HWND owner)
{
    if (!Theme_RichTextReady()) {
        Util_Log(L"about: the rich edit could not be loaded (%lu)", GetLastError());
        return;
    }
    Ui_Dialog(owner, IDD_ABOUT, AboutProc, 0);
}
