/*
 * The program's dialogs and manager window as theme.c lays them out, with
 * private mock data, in every interface language and at four font and
 * geometry scales (96, 144, 192 and 288: a resource and its font scaled
 * together; the monitor's own DPI is left as it is):
 *  - every visible control inside its dialog and every dialog inside the
 *    monitor's work area, without overlaps, each caption whole (wrapped
 *    labels, buttons, check boxes, links, edits, drop-down lists);
 *  - the manager window: one client size for every language, both views and
 *    every status and version, level with the buttons beside them; its table
 *    columns and profile note; live language changes in the same window and
 *    in its modal dialogs;
 *    its responsive layout from the minimum to wide, tall and maximized
 *    frames, and its native frame messages;
 *  - the sessions view driven by sessions.c on private profiles, entries and
 *    transcripts: resizes, refills and language changes keep its rows, its
 *    selection (a starred row included), its folded folders and the row
 *    on top; its disk watcher; double-clicks;
 *  - the themed message box (Ui_Message), as the program shows it;
 *  - the Settings dialog as the program shows it: each help question shows
 *    its own answer, the language box offers Windows' language then each
 *    of the program's, the one in use chosen, and a language applied with
 *    Apply (enabled by a change only) is followed by the open dialog.
 * Built and run by build.cmd with the program's resources and manifest. Its
 * windows show briefly; the maximized one, the modal profile dialog and the
 * message boxes take the foreground. The log and the queued session changes
 * go to a private folder (Util_SetStateDir), and the manager window's class
 * is one of the tests' own: the program never finds a fixture.
 */
#include "../src/app.h"
#include "../src/resource.h"
#include "../src/sessiontools.h"
#include "../src/vaulttools.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <stdio.h>
#include <stdlib.h>

/* The manager window's layout contract is theme.c's (THEME_MAIN_*, app.h);
 * these bounds are the tests' own, in DIPs. */
#define SESSION_PROFILES_MAX_DIPS 200    /* the sessions' side bar stays narrow */

/* A session snapshot is read on another thread: generous for a loaded machine. */
#define SNAPSHOT_TIMEOUT_MS 10000
/* How long a write the watcher ignores is given to ask for a snapshot anyway. */
#define IGNORED_WRITE_MS    1000

enum { SCALED_FONT_SUBCLASS = 1, FRAME_PROBE_SUBCLASS, MESSAGE_OWNER_SUBCLASS, SETTINGS_OWNER_SUBCLASS };
enum { TRANSCRIPT_STREAM_TIMER = 1 };

/* The first scale is the monitor's own: the checks made at it need it to fit. */
static const int kFontScales[] = { 96, 144, 192, 288 };
static BOOL g_scaleFits[ARRAYSIZE(kFontScales)];

typedef enum LinkStatus { LINKS_NOT_SET_UP, LINKS_ROUTED, CLAUDE_MISSING, LINK_STATUSES } LinkStatus;

typedef struct LayoutFixture {
    int resource, language, fontScale;
    const WCHAR *noteName;              /* the default profile's name; NULL: one MAX_LABEL long */
    const int *hiddenControls;          /* hidden before the dialog is fitted, as their dialog hides them */
    int hiddenCount;
    HFONT font;                         /* the scaled resource font, owned */
    HWND table, tableViewport, tree, treeViewport, profilesViewport;
    BOOL sessions;                      /* the sessions view shows */
    BOOL realSessions;                  /* sessions.c drives that view */
    LinkStatus links;
    MainVersion version;
    /* What sessions.c did through the window. */
    int reloadRequests, openPrompts, searchCompletions, snapshots;
    WPARAM searchGeneration;
    BOOL holdSearch;
    struct { WPARAM generation; LPARAM index; } heldSearch[64];
    int heldSearchCount;
    HWND lastPrompt;
    WCHAR promptText[512];
} LayoutFixture;

static int g_checks, g_failures;

static void Check(const LayoutFixture *fixture, HWND control, const char *name, BOOL ok)
{
    g_checks++;
    if (ok) return;
    g_failures++;
    printf("  FAIL resource=%d mode=%s language=%ls font-scale=%d control=%d: %s\n", fixture->resource,
           fixture->sessions ? "Sessions" : "Profiles", Localize_LanguageCode(fixture->language), fixture->fontScale,
           control ? GetDlgCtrlID(control) : 0, name);
}

static int Language(const WCHAR *code)
{
    return Localize_LanguageForCode(code);
}

/* The thread's pending messages, handled. A window on screen whose thread
 * reads no message for five seconds is "not responding": Windows then
 * ignores ShowWindow on it (a maximized one stays maximized) and puts it back
 * where its ghost was when it hides. Every step that keeps a fixture on
 * screen for long pumps. */
static void PumpMessages(void)
{
    MSG message;
    int handled = 0;
    while (handled++ < 4096 && PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

typedef BOOL (*Condition)(const void *context);

/* Messages handled until `done` holds, at most `timeoutMs`: woken by
 * messages, never spinning. A NULL `done` waits the whole time. */
static BOOL PumpUntil(Condition done, const void *context, DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        ULONGLONG now;
        PumpMessages();
        if (done && done(context)) return TRUE;
        now = GetTickCount64();
        if (now >= deadline) return FALSE;
        MsgWaitForMultipleObjectsEx(0, NULL, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

static BOOL IsClass(HWND window, const WCHAR *name)
{
    WCHAR actual[64];
    return GetClassNameW(window, actual, ARRAYSIZE(actual)) && _wcsicmp(actual, name) == 0;
}

static RECT RelativeRect(HWND dialog, HWND control)
{
    RECT rect = { 0, 0, 0, 0 };
    GetWindowRect(control, &rect);
    MapWindowPoints(NULL, dialog, (POINT *)&rect, 2);
    return rect;
}

static BOOL InsideWorkArea(HWND window)
{
    RECT frame;
    MONITORINFO monitor = { sizeof monitor };
    return GetWindowRect(window, &frame) && GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) &&
           frame.left >= monitor.rcWork.left && frame.top >= monitor.rcWork.top &&
           frame.right <= monitor.rcWork.right && frame.bottom <= monitor.rcWork.bottom;
}

static void LongName(WCHAR *name, size_t cch)
{
    static const WCHAR kPhrase[] = L"Long profile name ";
    size_t i;
    for (i = 0; i < MAX_LABEL && i + 1 < cch; i++) name[i] = kPhrase[i % (ARRAYSIZE(kPhrase) - 1)];
    name[i] = 0;
}

static void FixtureName(const LayoutFixture *fixture, WCHAR *name, size_t cch)
{
    if (fixture->noteName) StringCchCopyW(name, cch, fixture->noteName);
    else LongName(name, cch);
}

/* Font and resource geometry are scaled together; the monitor DPI is unchanged. */
static BOOL ScaleFixture(HWND dialog, LayoutFixture *fixture)
{
    typedef struct Position { HWND window; RECT rect; int dropHeight; } Position;
    Position positions[128];
    HWND child;
    LOGFONTW font;
    RECT frame, client;
    MONITORINFO monitor = { sizeof monitor };
    HFONT native = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    int count = 0, i, width, height;
    if (!native || !GetObjectW(native, sizeof font, &font)) return FALSE;
    GetWindowRect(dialog, &frame);
    GetClientRect(dialog, &client);
    for (child = GetWindow(dialog, GW_CHILD); child && count < (int)ARRAYSIZE(positions); child = GetWindow(child, GW_HWNDNEXT)) {
        RECT dropped;
        positions[count].window = child;
        positions[count].rect = RelativeRect(dialog, child);
        /* A drop-down list's resource height is its list's, as the dialog manager sizes it. */
        positions[count].dropHeight = IsClass(child, WC_COMBOBOXW) && SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&dropped)
                                      ? dropped.bottom - dropped.top : 0;
        count++;
    }
    font.lfHeight = MulDiv(font.lfHeight, fixture->fontScale, 96);
    fixture->font = CreateFontIndirectW(&font);
    if (!fixture->font) return FALSE;
    SendMessageW(dialog, WM_SETFONT, (WPARAM)fixture->font, FALSE);
    width = MulDiv(client.right, fixture->fontScale, 96) + frame.right - frame.left - client.right;
    height = MulDiv(client.bottom, fixture->fontScale, 96) + frame.bottom - frame.top - client.bottom;
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowPos(dialog, NULL, max(monitor.rcWork.left, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2),
                 max(monitor.rcWork.top, monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2),
                 width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    for (i = 0; i < count; i++) {
        RECT rect = positions[i].rect;
        SendMessageW(positions[i].window, WM_SETFONT, (WPARAM)fixture->font, FALSE);
        SetWindowPos(positions[i].window, NULL, MulDiv(rect.left, fixture->fontScale, 96), MulDiv(rect.top, fixture->fontScale, 96),
                     MulDiv(rect.right - rect.left, fixture->fontScale, 96),
                     MulDiv(positions[i].dropHeight ? positions[i].dropHeight : rect.bottom - rect.top, fixture->fontScale, 96),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return TRUE;
}

/* A native template font already has its monitor's size. A synthetic scale
 * supplies that same initial WM_GETFONT result while the theme reads it: the
 * theme's dialog subclass, installed after this one, answers with the font
 * it made from it once it has one. */
static LRESULT CALLBACK ScaledResourceFont(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR font)
{
    (void)id;
    if (message == WM_GETFONT) return (LRESULT)font;
    return DefSubclassProc(window, message, wp, lp);
}

static void CheckInitialFont(HWND dialog, const LayoutFixture *fixture)
{
    LOGFONTW expected = { 0 }, actual = { 0 };
    HFONT font = (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0);
    BOOL available = GetObjectW(fixture->font, sizeof expected, &expected) && GetObjectW(font, sizeof actual, &actual);
    Check(fixture, NULL, "initialized dialog retains its requested native resource font scale", available &&
          expected.lfHeight == actual.lfHeight && expected.lfWeight == actual.lfWeight);
    Check(fixture, NULL, "initialized dialog font covers the selected script", available && wcscmp(actual.lfFaceName, Localize_FontFace()) == 0);
}

/* The profile table's columns and one row (gui.c sizes the columns later). */
static void PopulateProfileTable(HWND table, const WCHAR *name)
{
    LVCOLUMNW column;
    LVITEMW item;
    RECT rect;
    int i;
    GetClientRect(table, &rect);
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    for (i = 0; Theme_ProfileColumnTitle(i); i++) {
        column.pszText = (WCHAR *)TR(Theme_ProfileColumnTitle(i));
        column.cx = rect.right / 4;
        ListView_InsertColumn(table, i, &column);
    }
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT;
    item.pszText = (WCHAR *)name;
    ListView_InsertItem(table, &item);
    ListView_SetItemText(table, 0, 1, (WCHAR *)TR(Theme_ProfileRole(TRUE, TRUE)));
    ListView_SetItemText(table, 0, 2, L"%APPDATA%\\Claude");
    ListView_SetItemText(table, 0, 3, (WCHAR *)TR(Theme_SessionsFolderState(0)));
    ListView_SetItemText(table, 0, PROFILE_COLUMN_STATE, (WCHAR *)TR(Theme_ProfileState(FALSE)));
    Gui_OrderProfileColumns(table);
}

/* The uninstall dialog's profiles to keep, as gui.c makes them: check boxes in
 * a column the view sizes, in a smooth view; with none (`rows` 0) the dialog
 * hides the view (the fixture's hidden controls). */
static void PopulateKeptProfiles(HWND dialog, const WCHAR *name, int rows)
{
    HWND list = GetDlgItem(dialog, IDC_U_LIST);
    WCHAR text[LABEL_CCH + 64];
    LVCOLUMNW column;
    LVITEMW item;
    int i;
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);
    for (i = 0; i < rows; i++) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (%%APPDATA%%\\%s)"), name, L"Claude-Private");
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = text;
        ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, i, TRUE);
    }
    Theme_SmoothView(list);
}

/* The sessions dialog's profiles, as syncui.c makes them: check boxes in a
 * column the view sizes, in a smooth view, one open now. */
static void PopulateSyncProfiles(HWND dialog, const WCHAR *name)
{
    HWND list = GetDlgItem(dialog, IDC_Y_LIST);
    WCHAR text[LABEL_CCH + 128];
    LVCOLUMNW column;
    LVITEMW item;
    int i;
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);
    for (i = 0; i < 3; i++) {
        if (i == 0) StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (open now: gets them once it closes)"), name);
        else StringCchCopyW(text, ARRAYSIZE(text), name);
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = text;
        ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, i, i != 2);
    }
    Theme_SmoothView(list);
}

/* The link dialog's profiles, as router.c makes them: one column the view
 * sizes, in a smooth view, one open now and selected. */
static void PopulateLinkProfiles(HWND dialog, const WCHAR *name)
{
    HWND list = GetDlgItem(dialog, IDC_L_LIST);
    WCHAR text[LABEL_CCH + 64];
    LVCOLUMNW column;
    LVITEMW item;
    int i;
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);
    for (i = 0; i < 3; i++) {
        if (i == 1) StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (open now)"), name);
        else StringCchCopyW(text, ARRAYSIZE(text), name);
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = text;
        ListView_InsertItem(list, &item);
    }
    ListView_SetItemState(list, 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Theme_SmoothView(list);
}

/* What the link dialog says for a sign-in link, its longest form. */
static void LinkCaptions(HWND dialog, const WCHAR *name)
{
    WCHAR text[512];
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"This sign-in was started in \x201C%s\x201D: it finishes only there."), name);
    SetDlgItemTextW(dialog, IDC_L_TEXT, text);
    SetDlgItemTextW(dialog, IDC_L_LINK, L"claude://login/fixture?...");
}

/* The backup dialog's parts, as backup.c lists them: check boxes in one
 * column the view sizes, sign-in left unchecked. */
static CheckList *g_backupParts;   /* the backup dialog's list, as PopulateBackupParts filled it last */
static const char *g_backupLayout;   /* an import's settings, NULL: its sample's */
static size_t g_backupLayoutLength;
static CheckList *g_profileList;   /* the profile dialog's, as FillMock filled it last */

static BOOL g_backupImport;   /* the backup dialog filled as an import, its sessions one by one */

static void PopulateBackupParts(HWND dialog)
{
    HWND list = GetDlgItem(dialog, IDC_B_LIST);
    g_backupParts = g_backupLayout ? Backup_FillLayoutList(list, g_backupLayout, g_backupLayoutLength)
                    : g_backupImport ? Backup_FillImportList(list) : Backup_FillPartsList(list, 1234);
    Theme_SmoothView(list);
}

/* What to sync, as syncui.c shows it: the default, everything, its groups folded. */
static void PopulateSyncItems(HWND dialog)
{
    HWND list = GetDlgItem(dialog, IDC_I_LIST);
    SyncUi_FillItemsList(list, SYNC_ITEMS_DEFAULT);
    Theme_SmoothView(list);
}

/* What the what-to-sync dialog says, for three profiles. */
static void SyncItemsCaptions(HWND dialog, const WCHAR *name)
{
    WCHAR text[512], names[LABEL_CCH * 3 + 32];
    SetWindowTextW(dialog, TR(L"What to sync"));
    StringCchPrintfW(names, ARRAYSIZE(names), L"%s%s%s%s%s", name, TR(L", "), L"Work", TR(L" and "), L"Personal");
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"What %s sync with each other, at each sync:"), names);
    SetDlgItemTextW(dialog, IDC_I_TEXT, text);
}

/* Sync settings as syncui.c fills them: two profiles to sync with, one checked, and every item. */
static void SyncSetupCaptions(HWND dialog, const WCHAR *name, BOOL fill)
{
    static const WCHAR *const kPartners[] = { L"Work", L"Personal" };
    static const BOOL kChecked[] = { TRUE, FALSE };
    WCHAR text[256];
    HWND items;
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"\x201C%s\x201D syncs with:"), name);
    SetDlgItemTextW(dialog, IDC_Z_TEXT, text);
    if (!fill) return;
    SyncUi_PartnerBoxes(dialog, kPartners, kChecked, (int)ARRAYSIZE(kPartners));
    items = GetDlgItem(dialog, IDC_Z_ITEMS);
    /* The items as the what-to-sync dialog shows them. */
    SyncUi_FillItemsList(items, SYNC_ITEMS_DEFAULT);
    Theme_SmoothView(items);
}

/* The conflicts of a sync as syncui.c lists them: a heading, two elements, a column per profile. */
static void ConflictsCaptions(HWND dialog, const WCHAR *name, BOOL fill)
{
    HWND list = GetDlgItem(dialog, IDC_X_LIST);
    WCHAR text[512], names[LABEL_CCH * 2 + 32];
    StringCchPrintfW(names, ARRAYSIZE(names), L"%s%s%s", name, TR(L" and "), L"Personal");
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s changed these each their own way since the last sync. Choose the version to keep of each:"), names);
    SetDlgItemTextW(dialog, IDC_X_TEXT, text);
    if (!fill) return;
    SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)TR(L"The latest of each"));
    SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)name);
    SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)L"Personal");
    SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_SETCURSEL, 0, 0);
    SyncUi_FillConflictsList(list, name);
    Theme_SmoothView(list);
}

/* What the backup dialog says when it restores, its longest form. */
static void BackupCaptions(HWND dialog, const WCHAR *name)
{
    WCHAR text[512];
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Restore \x201C%s\x201D from a backup"), name);
    SetWindowTextW(dialog, text);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D. What it replaces is kept in a backup first."), name);
    SetDlgItemTextW(dialog, IDC_B_TEXT, text);
    SetDlgItemTextW(dialog, IDOK, TR(L"Restore"));
}

/* The recover dialog as syncui.c fills it: its text, the profile, versions in one column. */
static void RestoreCaptions(HWND dialog, const WCHAR *name, BOOL fill)
{
    HWND list = GetDlgItem(dialog, IDC_R_LIST);
    WCHAR text[256];
    LVCOLUMNW column;
    LVITEMW item;
    int i;
    SetDlgItemTextW(dialog, IDC_R_TEXT, TR(L"Choose a saved version, then search and select sessions to recover. Sessions deleted in Claude stay deleted."));
    if (!fill) return;
    SendDlgItemMessageW(dialog, IDC_R_PROFILE, CB_ADDSTRING, 0, (LPARAM)name);
    SendDlgItemMessageW(dialog, IDC_R_PROFILE, CB_SETCURSEL, 0, 0);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);
    for (i = 0; i < 3; i++) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s \x00B7 sessions: %d"), L"2026-10-08  08:30:12", 56 + i);
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = text;
        ListView_InsertItem(list, &item);
    }
    ListView_SetItemState(list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Theme_SmoothView(list);
}

/* The clean-up dialog as syncui.c fills it: its text, conversations checked or not. */
static void PurgeCaptions(HWND dialog, BOOL fill)
{
    HWND list = GetDlgItem(dialog, IDC_C_LIST);
    WCHAR text[256];
    LVCOLUMNW column;
    LVITEMW item;
    int i;
    SetDlgItemTextW(dialog, IDC_C_TEXT, TR(L"Conversations still on this PC that no profile shows any more."));
    if (!fill) return;
    CheckDlgButton(dialog, IDC_C_BACKUP, BST_CHECKED);
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);
    for (i = 0; i < 3; i++) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s  \x00B7  Private conversation  \x00B7  2026-10-08  08:30  \x00B7  1.3 MB",
                         i ? TR(L"In no list") : L"Private profile");
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = text;
        ListView_InsertItem(list, &item);
        ListView_SetCheckState(list, i, i == 0);
    }
    Theme_SmoothView(list);
}

/* What the sessions dialog says when it moves every session, its longest form. */
static void SyncCaptions(HWND dialog)
{
    WCHAR text[1024];
    SetWindowTextW(dialog, TR(L"Move all sessions"));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"The profiles checked get every session %s lists, which then leaves it. "
                                               L"The conversations stay on this PC."), L"Private profile");
    SetDlgItemTextW(dialog, IDC_Y_TEXT, text);
    SetDlgItemTextW(dialog, IDC_Y_TO_LABEL, TR(L"&To these profiles:"));
    SetDlgItemTextW(dialog, IDOK, TR(L"Move"));
}

/* The status line and its button, and the version, as gui.c shows them. */
static void MainCaptions(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR text[2048];
    HWND statusAction = GetDlgItem(dialog, IDC_STATUS_ACTION);
    if (fixture->links == CLAUDE_MISSING) {
        SetDlgItemTextW(dialog, IDC_STATUS, TR(Theme_MainStatus(MAIN_STATUS_NO_CLAUDE)));
        SetWindowTextW(statusAction, TR(Theme_MainCaption(IDC_STATUS_ACTION, 0)));
    } else {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainStatus(fixture->links == LINKS_ROUTED ? MAIN_STATUS_ROUTED : MAIN_STATUS_NO_LINKS)),
                         L"2.16120.0");
        SetDlgItemTextW(dialog, IDC_STATUS, text);
    }
    /* Get Claude; links are fixed from the column. The sessions view has no column. */
    ShowWindow(statusAction, fixture->links == CLAUDE_MISSING && !fixture->sessions ? SW_SHOW : SW_HIDE);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(fixture->version)),
                     fixture->version == MAIN_VERSION_BUILD ? L"2026.12.31" : L"9.8.7");
    SetDlgItemTextW(dialog, IDC_VERSION, text);
    ShowWindow(GetDlgItem(dialog, IDC_UPDATE), fixture->version != MAIN_VERSION_BUILD && !fixture->sessions ? SW_SHOW : SW_HIDE);
}

/* The profile dialog's captions as gui.c sets them for an edit of Claude's
 * own profile while it runs: its title, the folder line at its longest. */
static void ProfileCaptions(HWND dialog)
{
    WCHAR text[512];
    SetWindowTextW(dialog, TR(L"Edit profile"));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Data folder: %%APPDATA%%\\%s%s"), L"Claude", TR(L"\nThe regular Claude icon opens this profile."));
    StringCchCatW(text, ARRAYSIZE(text), L"\n");
    StringCchCatW(text, ARRAYSIZE(text), TR(L"Its Claude is open: close it to change its settings."));
    SetDlgItemTextW(dialog, IDC_P_FOLDER, text);
}

/* The new profile's form as gui.c fills it, at its longest: a long name,
 * every color, the sessions synced with two profiles, every check box. */
static void NewProfileCaptions(HWND dialog, const WCHAR *name)
{
    HWND combo = GetDlgItem(dialog, IDC_P_COLOR);
    WCHAR text[LABEL_CCH * 3 + 64], names[LABEL_CCH * 2 + 8];
    int color;
    SetWindowTextW(dialog, TR(L"New profile"));
    SetDlgItemTextW(dialog, IDC_P_NAME, name);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (color = 0; color < PALETTE_SIZE; color++) SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)TR(g_ColorNames[color]));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Its data will be kept in %%APPDATA%%\\%s."), L"Claude-Private profile");
    SetDlgItemTextW(dialog, IDC_P_FOLDER, text);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Copy &settings from \x201C%s\x201D"), name);
    SetDlgItemTextW(dialog, IDC_P_COPY, text);
    CheckDlgButton(dialog, IDC_P_STARTUP, BST_CHECKED);
    CheckDlgButton(dialog, IDC_P_COPY, BST_CHECKED);
    CheckDlgButton(dialog, IDC_P_OPEN, BST_CHECKED);
    combo = GetDlgItem(dialog, IDC_P_SYNC);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)TR(L"Not synced"));
    StringCchPrintfW(names, ARRAYSIZE(names), L"%s, %s", name, L"Work");
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"With %s"), names);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(combo, CB_SETCURSEL, 1, 0);
}

/* The export's window as transfer.c fills it, at its longest: one of several, stopped by an error. */
static void TransferCaptions(HWND dialog, const WCHAR *name)
{
    WCHAR text[512];
    SetWindowTextW(dialog, TR(L"Export"));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%d of %d: \x201C%s\x201D to %s"), 2, 3, name, L"C:\\Users\\Private\\Desktop\\Exports");
    SetDlgItemTextW(dialog, IDC_TR_TEXT, text);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Stopped: %s What was copied is kept; Retry goes on from there."), L"There is not enough space on the disk.");
    SetDlgItemTextW(dialog, IDC_TR_STATUS, text);
    SetDlgItemTextW(dialog, IDC_TR_PAUSE, TR(L"&Retry"));
}

static void FillMock(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR name[LABEL_CCH], text[4096];
    FixtureName(fixture, name, ARRAYSIZE(name));
    switch (fixture->resource) {
    case IDD_MAIN:
        MainCaptions(dialog, fixture);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainNote()), name);
        SetDlgItemTextW(dialog, IDC_NOTE, text);
        PopulateProfileTable(GetDlgItem(dialog, IDC_LIST), name);
        break;
    case IDD_PROFILE:
        ProfileCaptions(dialog);
        g_profileList = Gui_FillProfileList(GetDlgItem(dialog, IDC_P_LIST));
        Theme_SmoothView(GetDlgItem(dialog, IDC_P_LIST));
        break;
    case IDD_PROFILE_NEW:
        NewProfileCaptions(dialog, name);
        break;
    case IDD_TITLE:
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Title in \x201C%s\x201D:"), name);
        SetDlgItemTextW(dialog, IDC_T_LABEL, text);
        SetDlgItemTextW(dialog, IDC_T_TITLE, L"A private mock session title that remains user data in every language");
        break;
    case IDD_SETTINGS:
        SendDlgItemMessageW(dialog, IDC_G_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search settings"));
        break;
    case IDD_ABOUT: {
        HWND topics = GetDlgItem(dialog, IDC_W_TOPICS);
        WCHAR chosen[CHANGELOG_NAME_CCH] = L"";
        About_Fill(dialog);
        ListView_GetItemText(topics, ListView_GetNextItem(topics, -1, LVNI_FOCUSED), 0, chosen, ARRAYSIZE(chosen));
        Check(fixture, NULL, "About opens on this version, the embedded changelog's newest", wcscmp(chosen, L"v" APP_VERSION_DATE_W) == 0);
        Check(fixture, NULL, "About shows what the newest version changed", GetWindowTextLengthW(GetDlgItem(dialog, IDC_W_TEXT)) > 0);
        Theme_SmoothView(topics);
        break;
    }
    case IDD_PREVIEW:
        SetDlgItemTextW(dialog, IDC_V_TITLE, L"A private mock session title that remains user data in every language");
        SetDlgItemTextW(dialog, IDC_V_TEXT, L"\x25B6  You\r\nPrivate user text\r\n\r\n\x25C6  Claude\r\nPrivate reply\r\n[Bash] Build it");
        break;
    case IDD_SIGNIN:
        SetDlgItemTextW(dialog, IDC_SIGNIN_PROFILE, L"Private profile");
        SendDlgItemMessageW(dialog, IDC_SIGNIN_BROWSER, CB_ADDSTRING, 0, (LPARAM)L"Microsoft Edge");
        SendDlgItemMessageW(dialog, IDC_SIGNIN_BROWSER, CB_SETCURSEL, 0, 0);
        SetDlgItemTextW(dialog, IDC_SIGNIN_URL, L"https://claude.ai/login");
        SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS,
            TR(L"A separate private browser was started. Complete registration or sign-in yourself. If this is a Desktop sign-in, allow the claude:// link and choose this profile."));
        break;
    case IDD_SESSIONTOOLS:
        SetDlgItemTextW(dialog, IDC_ST_NOTE, L"Private fixture session details");
        SendDlgItemMessageW(dialog, IDC_ST_LIST, LB_ADDSTRING, 0, (LPARAM)L"Private fixture metadata");
        SetDlgItemTextW(dialog, IDC_ST_DETAIL, L"Private details\r\nSecond row");
        break;
    case IDD_TEXTVIEW:
        SetDlgItemTextW(dialog, IDC_T_NOTE, TR(L"Only versions and profile status are included. Account names, paths, links and log contents are omitted."));
        SetDlgItemTextW(dialog, IDC_T_TEXT, L"Private fixture details\r\nSecond row");
        break;
    case IDD_MESSAGE:
        StringCchPrintfW(text, ARRAYSIZE(text),
            TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\nThe copy goes on separately."), L"A private conversation", name);
        SetDlgItemTextW(dialog, IDC_M_TEXT, text);
        SetDlgItemTextW(dialog, IDOK, TR(L"Copy"));
        break;
    case IDD_UNINSTALL:
        StringCchPrintfW(text, ARRAYSIZE(text),
            TR(L"Claude Desktop and its \x201C%s\x201D profile (the one the regular Claude icon opens) are not touched."), name);
        SetDlgItemTextW(dialog, IDC_U_KEEP, text);
        PopulateKeptProfiles(dialog, name, fixture->hiddenCount ? 0 : 1);
        break;
    case IDD_SYNC:
        SyncCaptions(dialog);
        PopulateSyncProfiles(dialog, name);
        break;
    case IDD_LINK:
        LinkCaptions(dialog, name);
        PopulateLinkProfiles(dialog, name);
        break;
    case IDD_BACKUP:
        BackupCaptions(dialog, name);
        PopulateBackupParts(dialog);
        break;
    case IDD_RESTORE:
        RestoreCaptions(dialog, name, TRUE);
        break;
    case IDD_PURGE:
        PurgeCaptions(dialog, TRUE);
        break;
    case IDD_SYNC_ITEMS:
        SyncItemsCaptions(dialog, name);
        PopulateSyncItems(dialog);
        break;
    case IDD_CONFLICTS:
        ConflictsCaptions(dialog, name, TRUE);
        break;
    case IDD_SYNC_SETUP:
        SyncSetupCaptions(dialog, name, TRUE);
        break;
    case IDD_PASSWORD:
        SetWindowTextW(dialog, TR(L"Password"));
        SetDlgItemTextW(dialog, IDC_K_TEXT, TR(L"The sign-in was backed up on another PC. Enter the password chosen for it then:"));
        break;
    case IDD_TRANSFER:
        TransferCaptions(dialog, name);
        break;
    }
}

/* Only captions and private mock choices change. Native HWNDs and user text
 * remain in place while the same dialog visits several writing systems. */
static void TransitionCaptions(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR text[512];
    if (fixture->resource == IDD_MAIN) {
        WCHAR name[LABEL_CCH];
        FixtureName(fixture, name, ARRAYSIZE(name));
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainNote()), name);
        SetDlgItemTextW(dialog, IDC_NOTE, text);
        SetDlgItemTextW(dialog, IDC_SESSIONS, TR(Theme_MainCaption(IDC_SESSIONS, fixture->sessions)));
        Theme_SetMainGlyph(dialog, IDC_SESSIONS, fixture->sessions);
        MainCaptions(dialog, fixture);
    } else if (fixture->resource == IDD_PROFILE) {
        ProfileCaptions(dialog);
    } else if (fixture->resource == IDD_PROFILE_NEW) {
        NewProfileCaptions(dialog, L"Private profile");
    } else if (fixture->resource == IDD_TITLE) {
        SetDlgItemTextW(dialog, IDC_T_TITLE, L"Private user text");
    } else if (fixture->resource == IDD_MESSAGE) {
        SetDlgItemTextW(dialog, IDC_M_TEXT, TR(L"Continue?"));
        SetDlgItemTextW(dialog, IDOK, TR(L"Copy"));
    } else if (fixture->resource == IDD_SYNC) {
        SyncCaptions(dialog);
    } else if (fixture->resource == IDD_LINK) {
        LinkCaptions(dialog, L"Private profile");
    } else if (fixture->resource == IDD_BACKUP) {
        BackupCaptions(dialog, L"Private profile");
    } else if (fixture->resource == IDD_RESTORE) {
        RestoreCaptions(dialog, L"Private profile", FALSE);
    } else if (fixture->resource == IDD_PURGE) {
        PurgeCaptions(dialog, FALSE);
    } else if (fixture->resource == IDD_SYNC_ITEMS) {
        SyncItemsCaptions(dialog, L"Private profile");
    } else if (fixture->resource == IDD_CONFLICTS) {
        ConflictsCaptions(dialog, L"Private profile", FALSE);
    } else if (fixture->resource == IDD_SYNC_SETUP) {
        SyncSetupCaptions(dialog, L"Private profile", FALSE);
    }
}

/* What gui.c does to its window before theming it: the table's styles and
 * smooth view, the sessions view's controls in theirs, "Sessions >"
 * semibold. */
static void PrepareMainWindow(HWND dialog, LayoutFixture *fixture)
{
    fixture->table = GetDlgItem(dialog, IDC_LIST);
    fixture->tree = GetDlgItem(dialog, IDC_S_TREE);
    ListView_SetExtendedListViewStyle(fixture->table, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    fixture->tableViewport = Theme_SmoothView(fixture->table);
    if (fixture->realSessions) SessionsView_Init(dialog);
    fixture->treeViewport = Theme_SmoothView(fixture->tree);
    fixture->profilesViewport = Theme_SmoothView(GetDlgItem(dialog, IDC_S_PROFILES));
    Theme_SetStrong(GetDlgItem(dialog, IDC_SESSIONS));
}

/* The note under "Set as default" names the default profile (gui.c's UpdateNote). */
static void LayoutMockNote(HWND dialog, const LayoutFixture *fixture)
{
    WCHAR name[LABEL_CCH];
    FixtureName(fixture, name, ARRAYSIZE(name));
    Theme_LayoutSidebarNote(GetDlgItem(dialog, IDC_NOTE), TR(Theme_MainNote()), name);
}

/* The sessions view in place of the profiles, as Gui_ShowSessions swaps
 * them, without sessions.c: theme.c places every control of both views. */
static void ShowMockSessions(HWND dialog)
{
    static const int kProfileControls[] = { IDC_PROFILE_SEARCH_LABEL, IDC_PROFILE_SEARCH, IDC_PROFILE_FIND, IDC_LIST, IDC_SYNC, IDC_BACKUP, IDC_RESTORE_BACKUP,
                                            IDC_REPAIR, IDC_WINDOWS_MCP, IDC_SEARXNG, IDC_COWORK, IDC_NOTE };
    static const int kSessionControls[] = { IDC_S_PROFILES, IDC_S_FOLD, IDC_S_SEARCH, IDC_S_FIND, IDC_S_FILTER, IDC_S_TREE };
    static const int kFootControls[] = { IDC_STATUS, IDC_PROGRESS, IDC_STATUS_ACTION, IDC_VERSION, IDC_UPDATE };   /* the view has no column */
    size_t i;
    for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(dialog, kProfileControls[i]), SW_HIDE);
    for (i = 0; i < ARRAYSIZE(kFootControls); i++) ShowWindow(GetDlgItem(dialog, kFootControls[i]), SW_HIDE);
    for (i = 0; i < ARRAYSIZE(kSessionControls); i++) ShowWindow(GetDlgItem(dialog, kSessionControls[i]), SW_SHOW);
    SetDlgItemTextW(dialog, IDC_SESSIONS, TR(Theme_MainCaption(IDC_SESSIONS, 1)));
    Theme_SetMainGlyph(dialog, IDC_SESSIONS, 1);
    SendDlgItemMessageW(dialog, IDC_S_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search this profile's sessions"));
    Theme_LayoutMain(dialog);
}

/* A fixture made as the program makes its dialog, at the fixture's scale:
 * resource labels, then mock data (`fill`), then the theme. */
static void InitializeFixture(HWND dialog, LayoutFixture *fixture, void (*fill)(HWND, const LayoutFixture *))
{
    int hidden;
    Localize_Window(dialog);
    Check(fixture, NULL, "font and geometry scale created", ScaleFixture(dialog, fixture));
    fill(dialog, fixture);
    for (hidden = 0; hidden < fixture->hiddenCount; hidden++) ShowWindow(GetDlgItem(dialog, fixture->hiddenControls[hidden]), SW_HIDE);
    if (fixture->resource == IDD_MAIN) {
        SIZE minimum;
        MINMAXINFO tracking = { 0 };
        Check(fixture, NULL, "main minimum is unavailable before layout initialization", !Theme_MainMinimum(dialog, &minimum));
        Check(fixture, NULL, "native tracking falls back before main initialization",
              !Gui_MainWindowGeometry(dialog, WM_GETMINMAXINFO, 0, (LPARAM)&tracking));
        PrepareMainWindow(dialog, fixture);
    }
    SetWindowSubclass(dialog, ScaledResourceFont, SCALED_FONT_SUBCLASS, (DWORD_PTR)fixture->font);
    if (fixture->resource == IDD_MAIN) {
        Theme_Apply(dialog);
        Theme_RememberLayout(dialog);
        Theme_FitDialog(dialog);
    } else {
        /* Ui_Dialog fits before applying the final control theme. */
        Theme_FitDialog(dialog);
        Theme_Apply(dialog);
    }
    RemoveWindowSubclass(dialog, ScaledResourceFont, SCALED_FONT_SUBCLASS);
    if (fixture->resource == IDD_MAIN) {
        Gui_LayoutProfileColumns(fixture->table);
        LayoutMockNote(dialog, fixture);
    }
    if (fixture->sessions) ShowMockSessions(dialog);
    CheckInitialFont(dialog, fixture);
}

/* A message box of the sessions view (opening a session without Claude asks
 * for it, and never starts it), read and closed once it waits for the user. */
static void ClosePrompt(LayoutFixture *fixture, HWND prompt)
{
    if (!prompt || prompt == fixture->lastPrompt) return;
    fixture->lastPrompt = prompt;
    fixture->openPrompts++;
    GetDlgItemTextW(prompt, IDC_M_TEXT, fixture->promptText, ARRAYSIZE(fixture->promptText));
    PostMessageW(prompt, WM_COMMAND, IDOK, 0);
}

static void DeliverSearchResult(LayoutFixture *fixture, WPARAM generation, LPARAM index)
{
    if (!SessionsView_SearchFound(generation, index)) return;
    fixture->searchGeneration = generation;
    if (index < 0) fixture->searchCompletions++;
}

static INT_PTR CALLBACK FixtureProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    LayoutFixture *fixture = (LayoutFixture *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        fixture = (LayoutFixture *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        InitializeFixture(dialog, fixture, FillMock);
        return TRUE;
    }
    if (!fixture) return FALSE;
    if (message >= WM_CTLCOLORMSGBOX && message <= WM_CTLCOLORSTATIC)
        return Theme_CtlColor(message, wp, lp, fixture->resource == IDD_MAIN ? IDC_VERSION : 0);
    if (fixture->resource == IDD_MAIN && (message == WM_GETMINMAXINFO || message == WM_DPICHANGED))
        return Gui_MainWindowGeometry(dialog, message, wp, lp);
    if (fixture->realSessions) {
        LRESULT result;
        switch (message) {
        case WM_APP_SESSIONS:
            fixture->reloadRequests++;
            SessionsView_Notified();
            return TRUE;
        case WM_APP_SESSIONS_READY:
            SessionsView_Ready(TRUE);
            fixture->snapshots++;
            return TRUE;
        case WM_APP_SESSION_SEARCH:
            if (fixture->holdSearch && fixture->heldSearchCount < (int)ARRAYSIZE(fixture->heldSearch)) {
                fixture->heldSearch[fixture->heldSearchCount].generation = wp;
                fixture->heldSearch[fixture->heldSearchCount++].index = lp;
            } else DeliverSearchResult(fixture, wp, lp);
            return TRUE;
        case WM_NOTIFY:
            if (!SessionsView_Notify((const NMHDR *)lp, &result)) break;
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
            return TRUE;
        case WM_DRAWITEM:
            if (SessionsView_DrawItem((const DRAWITEMSTRUCT *)lp)) return TRUE;
            break;
        case WM_COMMAND:
            if (SessionsView_Command(wp)) return TRUE;
            break;
        case WM_ENTERIDLE:
            if (wp == MSGF_DIALOGBOX) ClosePrompt(fixture, (HWND)lp);
            break;
        }
    }
    if (message == WM_SIZE && wp != SIZE_MINIMIZED && fixture->resource == IDD_MAIN) {
        SIZE minimum;
        if (Theme_MainMinimum(dialog, &minimum)) {
            Theme_LayoutMain(dialog);
            Gui_LayoutProfileColumns(fixture->table);
        }
    }
    if (message == WM_DESTROY) Localize_ForgetWindow(dialog);
    return FALSE;
}

/* IDD_MAIN with a window class of the tests' own: the program finds its
 * manager window by its class (a second launch brings it forward, an install
 * closes it), and must never find a fixture. The class name keeps its
 * length, so the rest of the template keeps its layout. */
static void *g_mainTemplate;
static WCHAR g_fixtureClass[64];

static BOOL PrepareMainTemplate(void)
{
    HRSRC found = FindResourceW(g_hInst, MAKEINTRESOURCEW(IDD_MAIN), (LPCWSTR)RT_DIALOG);
    HGLOBAL loaded = found ? LoadResource(g_hInst, found) : NULL;
    const BYTE *source = loaded ? (const BYTE *)LockResource(loaded) : NULL;
    DWORD size = found ? SizeofResource(g_hInst, found) : 0;
    size_t length = wcslen(APP_WINDOW_CLASS), menuWords;
    WNDCLASSEXW window;
    WCHAR *menu, *windowClass;
    /* DLGTEMPLATEEX: its fixed part, then the menu and the class. */
    if (!source || size < 26 || ((const WORD *)source)[1] != 0xFFFF || length < 4 || length >= ARRAYSIZE(g_fixtureClass)) return FALSE;
    if ((g_mainTemplate = HeapAlloc(GetProcessHeap(), 0, size)) == NULL) return FALSE;
    CopyMemory(g_mainTemplate, source, size);
    menu = (WCHAR *)((BYTE *)g_mainTemplate + 26);
    menuWords = menu[0] == 0 ? 1 : menu[0] == 0xFFFF ? 2 : wcsnlen(menu, (size - 26) / sizeof(WCHAR)) + 1;
    windowClass = menu + menuWords;
    if ((BYTE *)(windowClass + length + 1) > (BYTE *)g_mainTemplate + size || wcsncmp(windowClass, APP_WINDOW_CLASS, length + 1) != 0) return FALSE;
    StringCchCopyW(g_fixtureClass, ARRAYSIZE(g_fixtureClass), APP_WINDOW_CLASS);
    CopyMemory(g_fixtureClass + length - 4, L"Test", 4 * sizeof(WCHAR));
    CopyMemory(windowClass, g_fixtureClass, length * sizeof(WCHAR));
    ZeroMemory(&window, sizeof window);
    window.cbSize = sizeof window;
    window.lpfnWndProc = DefDlgProcW;
    window.cbWndExtra = DLGWINDOWEXTRA;
    window.hInstance = g_hInst;
    window.hCursor = LoadCursorW(NULL, IDC_ARROW);
    window.lpszClassName = g_fixtureClass;
    return RegisterClassExW(&window) != 0;
}

/* One of the program's dialogs, the manager window with the tests' class. */
static HWND CreateResourceDialog(int resource, DLGPROC proc, LPARAM param)
{
    if (resource == IDD_MAIN) return CreateDialogIndirectParamW(g_hInst, (LPCDLGTEMPLATEW)g_mainTemplate, NULL, proc, param);
    return CreateDialogParamW(g_hInst, MAKEINTRESOURCEW(resource), NULL, proc, param);
}

static HWND CreateFixture(LayoutFixture *fixture)
{
    Localize_SetLanguage(fixture->language, FALSE);
    return CreateResourceDialog(fixture->resource, FixtureProc, (LPARAM)fixture);
}

static void DestroyFixture(HWND dialog, LayoutFixture *fixture)
{
    if (dialog) DestroyWindow(dialog);
    if (fixture->font) DeleteObject(fixture->font);
    fixture->font = NULL;
}

static LayoutFixture MainFixture(int language, int fontScale, BOOL sessions)
{
    LayoutFixture fixture;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_MAIN;
    fixture.language = language;
    fixture.fontScale = fontScale;
    fixture.sessions = sessions;
    fixture.noteName = L"Personal";
    fixture.links = LINKS_ROUTED;
    return fixture;
}

/* ------------------------------------------------------------ rendering */

/* What `control` paints in `region` of its client (WM_PRINTCLIENT over the
 * window's face), as 32-bit pixels. */
static BOOL RenderClient(HWND control, const RECT *region, DWORD *pixels)
{
    BITMAPINFO bitmap = { 0 };
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP image;
    HGDIOBJ previous;
    void *surface = NULL;
    RECT area;
    int width = region->right - region->left, height = region->bottom - region->top;
    if (!dc || width <= 0 || height <= 0) {
        if (dc) DeleteDC(dc);
        return FALSE;
    }
    bitmap.bmiHeader.biSize = sizeof bitmap.bmiHeader;
    bitmap.bmiHeader.biWidth = width;
    bitmap.bmiHeader.biHeight = -height;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    image = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &surface, NULL, 0);
    if (!image || !surface) {
        if (image) DeleteObject(image);
        DeleteDC(dc);
        return FALSE;
    }
    previous = SelectObject(dc, image);
    SetRect(&area, 0, 0, width, height);
    FillRect(dc, &area, Theme_Brush(THEME_FACE));
    SetViewportOrgEx(dc, -region->left, -region->top, NULL);
    IntersectClipRect(dc, region->left, region->top, region->right, region->bottom);
    SendMessageW(control, WM_PRINTCLIENT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
    GdiFlush();
    CopyMemory(pixels, surface, (size_t)width * (size_t)height * sizeof *pixels);
    SelectObject(dc, previous);
    DeleteObject(image);
    DeleteDC(dc);
    return TRUE;
}

static DWORD *AllocatePixels(int width, int height)
{
    return (DWORD *)malloc((size_t)max(1, width) * (size_t)max(1, height) * sizeof(DWORD));
}

/* Every Role caption is drawn whole: each pixel of the cell matches the same
 * row drawn in a wider column. */
static void CheckNativeRoles(const LayoutFixture *fixture)
{
    static const struct { BOOL stock, isDefault; } kRoles[] = { { TRUE, TRUE }, { TRUE, FALSE }, { FALSE, TRUE } };
    HWND table = fixture->table;
    RECT actual, wide, client;
    WCHAR original[LABEL_CCH];
    int role, originalWidth = ListView_GetColumnWidth(table, 1);
    ListView_GetItemText(table, 0, 1, original, ARRAYSIZE(original));
    GetClientRect(table, &client);
    for (role = 0; role < (int)ARRAYSIZE(kRoles); role++) {
        DWORD *shown = NULL, *reference = NULL, *blank = NULL;
        size_t pixel, referenceInk = 0;
        int width, referenceWidth, height;
        BOOL painted = FALSE, complete = TRUE;
        ListView_SetItemText(table, 0, 1, (WCHAR *)TR(Theme_ProfileRole(kRoles[role].stock, kRoles[role].isDefault)));
        if (!ListView_GetSubItemRect(table, 0, 1, LVIR_BOUNDS, &actual)) {
            Check(fixture, table, "native Role subitem bounds available", FALSE);
            continue;
        }
        width = actual.right - actual.left;
        height = actual.bottom - actual.top;
        ListView_SetColumnWidth(table, 1, originalWidth + MulDiv(64, fixture->fontScale, 96));
        ListView_GetSubItemRect(table, 0, 1, LVIR_BOUNDS, &wide);
        referenceWidth = wide.right - wide.left;
        shown = AllocatePixels(width, height);
        reference = AllocatePixels(referenceWidth, height);
        blank = AllocatePixels(referenceWidth, height);
        if (shown && reference && blank && width > 0 && height > 0 && referenceWidth > width && wide.right <= client.right) {
            painted = RenderClient(table, &wide, reference);
            ListView_SetItemText(table, 0, 1, L"");
            painted = RenderClient(table, &wide, blank) && painted;
            ListView_SetColumnWidth(table, 1, originalWidth);
            ListView_SetItemText(table, 0, 1, (WCHAR *)TR(Theme_ProfileRole(kRoles[role].stock, kRoles[role].isDefault)));
            painted = RenderClient(table, &actual, shown) && painted;
            if (painted) for (pixel = 0; pixel < (size_t)referenceWidth * (size_t)height; pixel++) {
                size_t x = pixel % (size_t)referenceWidth, y = pixel / (size_t)referenceWidth;
                if ((reference[pixel] & 0xffffff) == (blank[pixel] & 0xffffff)) continue;
                referenceInk++;
                if (x >= (size_t)width || (shown[y * (size_t)width + x] & 0xffffff) != (reference[pixel] & 0xffffff)) complete = FALSE;
            }
        }
        ListView_SetColumnWidth(table, 1, originalWidth);
        Check(fixture, table, "native Role row has a complete wider reference render", painted && referenceInk > 0);
        Check(fixture, table, "every Role glyph matches the wider native row without ellipsis", painted && referenceInk > 0 && complete);
        free(blank);
        free(reference);
        free(shown);
    }
    ListView_SetItemText(table, 0, 1, original);
}

static void CheckNativeNote(HWND dialog, const LayoutFixture *fixture)
{
    HWND note = GetDlgItem(dialog, IDC_NOTE);
    HFONT font = (HFONT)SendMessageW(note, WM_GETFONT, 0, 0);
    const WCHAR *format = TR(Theme_MainNote()), *slot = wcsstr(format, L"%s");
    WCHAR text[1024], expectedName[LABEL_CCH];
    RECT client, position, calculated = { 0, 0, 0, 0 }, area;
    HDC dc;
    TEXTMETRICW metrics = { 0 };
    DWORD *shown = NULL, *empty = NULL, *reference = NULL, *referenceEmpty = NULL;
    size_t shownPixels, referencePixels, pixel, shownInk = 0, completeInk = 0, missingInk = 0;
    int width, height, referenceHeight, lastInkRow = -1;
    BOOL rendered, metricsReady = FALSE, fits;
    GetWindowTextW(note, text, ARRAYSIZE(text));
    FixtureName(fixture, expectedName, ARRAYSIZE(expectedName));
    if (slot) {
        size_t prefix = (size_t)(slot - format), suffix = wcslen(slot + 2), length = wcslen(text);
        BOOL prose = length >= prefix + suffix && wcsncmp(text, format, prefix) == 0 && wcscmp(text + length - suffix, slot + 2) == 0;
        Check(fixture, note, "localized note retains every fixed word around the profile name", prose);
        if (prose) {
            size_t expected = wcslen(expectedName), shownName = length - prefix - suffix;
            BOOL same = shownName == expected && wcsncmp(text + prefix, expectedName, expected) == 0;
            BOOL shortened = shownName >= 2 && shownName - 1 < expected && text[prefix + shownName - 1] == L'\x2026' &&
                             wcsncmp(text + prefix, expectedName, shownName - 1) == 0;
            Check(fixture, note, "profile name is preserved or shortened only by a final ellipsis", same || shortened);
        }
    } else Check(fixture, note, "localized note format contains its profile name argument", FALSE);
    GetClientRect(note, &client);
    position = RelativeRect(dialog, note);
    width = client.right;
    height = client.bottom;
    dc = GetDC(note);
    if (dc && font) {
        HGDIOBJ previous = SelectObject(dc, font);
        metricsReady = GetTextMetricsW(dc, &metrics);
        calculated.right = client.right;
        DrawTextW(dc, text, -1, &calculated, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | Localize_ReadingFlags());
        SelectObject(dc, previous);
    }
    if (dc) ReleaseDC(note, dc);
    Check(fixture, note, "native note font metrics are available", metricsReady && metrics.tmHeight > 0);
    if (!metricsReady || metrics.tmHeight <= 0) return;
    Check(fixture, note, "wrapped note has no horizontally cropped word", calculated.right <= width);
    /* The note drawn taller, by the same native control: every pixel of its
     * text is there, with two clear lines below. */
    referenceHeight = max(height, calculated.bottom) + 2 * metrics.tmHeight;
    if (width <= 0 || height <= 0) {
        Check(fixture, note, "native note has a nonempty drawing area", FALSE);
        return;
    }
    shownPixels = (size_t)width * (size_t)height;
    referencePixels = (size_t)width * (size_t)referenceHeight;
    shown = AllocatePixels(width, height);
    empty = AllocatePixels(width, height);
    reference = AllocatePixels(width, referenceHeight);
    referenceEmpty = AllocatePixels(width, referenceHeight);
    if (!shown || !empty || !reference || !referenceEmpty) {
        Check(fixture, note, "native note raster buffers allocated", FALSE);
        goto done;
    }
    SetRect(&area, 0, 0, width, height);
    rendered = RenderClient(note, &area, shown);
    SetWindowTextW(note, L"");
    rendered = RenderClient(note, &area, empty) && rendered;
    SetWindowPos(note, NULL, 0, 0, width, referenceHeight, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    area.bottom = referenceHeight;
    rendered = RenderClient(note, &area, referenceEmpty) && rendered;
    SetWindowTextW(note, text);
    rendered = RenderClient(note, &area, reference) && rendered;
    SetWindowPos(note, NULL, position.left, position.top, position.right - position.left,
                 position.bottom - position.top, SWP_NOZORDER | SWP_NOACTIVATE);
    Check(fixture, note, "native STATIC caption renders for its visible and reference heights", rendered);
    if (!rendered) goto done;
    fits = TRUE;
    for (pixel = 0; pixel < referencePixels; pixel++) {
        DWORD ink = reference[pixel] & 0xffffff, blank = referenceEmpty[pixel] & 0xffffff;
        if (ink != blank) {
            completeInk++;
            lastInkRow = (int)(pixel / (size_t)width);
            if (pixel >= shownPixels || (shown[pixel] & 0xffffff) != ink || (empty[pixel] & 0xffffff) != blank) {
                missingInk++;
                fits = FALSE;
            }
        }
        if (pixel < shownPixels && (shown[pixel] & 0xffffff) != (empty[pixel] & 0xffffff)) shownInk++;
    }
    Check(fixture, note, "native reference has visible text and two clear font rows below its final line",
          completeInk > 0 && lastInkRow < referenceHeight - 2 * metrics.tmHeight);
    Check(fixture, note, "every localized note glyph remains visible in the native STATIC", fits && shownInk == completeInk);
    if (!fits || shownInk != completeInk)
        printf("        name=%ls area=%dx%d CALCRECT-height=%ld native-final-ink-row=%d font-height=%ld shown-ink=%zu complete-ink=%zu missing-ink=%zu\n",
               fixture->noteName ? fixture->noteName : L"MAX_LABEL", width, height, calculated.bottom, lastInkRow, metrics.tmHeight,
               shownInk, completeInk, missingInk);
done:
    free(referenceEmpty);
    free(reference);
    free(empty);
    free(shown);
}

static void CheckNoteGeometry(HWND dialog, const LayoutFixture *fixture)
{
    HWND note = GetDlgItem(dialog, IDC_NOTE);
    RECT client, table = RelativeRect(dialog, fixture->tableViewport), placed = RelativeRect(dialog, note);
    RECT last = RelativeRect(dialog, GetDlgItem(dialog, IDC_REPAIR)), status = RelativeRect(dialog, GetDlgItem(dialog, IDC_STATUS));
    GetClientRect(dialog, &client);
    Check(fixture, note, "note stays beside the table and inside the dialog",
          placed.left >= table.right && placed.top >= 0 && placed.right <= client.right && placed.bottom <= client.bottom);
    Check(fixture, note, "note fits the table's vertical band", placed.bottom <= table.bottom);
    Check(fixture, note, "the complete note fits between the column's last action and its foot",
          placed.top >= last.bottom && placed.bottom <= status.top);
}

static BOOL IntentionalEllipsis(HWND control)
{
    LONG style = GetWindowLongW(control, GWL_STYLE);
    return IsClass(control, WC_STATICW) && (style & (SS_ENDELLIPSIS | SS_PATHELLIPSIS | SS_WORDELLIPSIS));
}

/* What of a push button `box` of `owner` its caption may take: inside the
 * frame Windows' button theme draws (its content margins). */
static RECT PushButtonTextArea(HWND owner, const RECT *box)
{
    RECT area = *box;
    HTHEME theme = OpenThemeData(owner, L"Button");
    HDC dc = GetDC(owner);
    if (theme && dc) GetThemeBackgroundContentRect(theme, dc, BP_PUSHBUTTON, PBS_NORMAL, box, &area);
    else InflateRect(&area, -GetSystemMetricsForDpi(SM_CXEDGE, GetDpiForWindow(owner)), -GetSystemMetricsForDpi(SM_CYEDGE, GetDpiForWindow(owner)));
    if (dc) ReleaseDC(owner, dc);
    if (theme) CloseThemeData(theme);
    return area;
}

static void CheckText(const LayoutFixture *fixture, HWND control)
{
    WCHAR text[4096];
    RECT client, measured = { 0, 0, 0, 0 };
    BOOL button = IsClass(control, WC_BUTTONW), label = IsClass(control, WC_STATICW);
    BOOL edit = IsClass(control, WC_EDITW), combo = IsClass(control, WC_COMBOBOXW), link = IsClass(control, WC_LINK);
    LONG style = GetWindowLongW(control, GWL_STYLE);
    HDC dc;
    HGDIOBJ previous;
    TEXTMETRICW metrics;
    UINT flags = Localize_ReadingFlags();
    int type = button ? style & BS_TYPEMASK : style & SS_TYPEMASK;
    if (!button && !label && !edit && !combo && !link) return;
    if (label && (type == SS_ICON || type == SS_BITMAP || type == SS_OWNERDRAW)) return;
    GetWindowTextW(control, text, ARRAYSIZE(text));
    GetClientRect(control, &client);
    dc = GetDC(control);
    previous = SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    GetTextMetricsW(dc, &metrics);
    if (link) {
        SIZE ideal = { 0, 0 };
        BOOL available = (BOOL)SendMessageW(control, LM_GETIDEALSIZE, client.right, (LPARAM)&ideal);
        Check(fixture, control, "native link ideal size is available", available && ideal.cy > 0);
        Check(fixture, control, "credit link lines fit their actual height", available && ideal.cy <= client.bottom);
        Check(fixture, control, "credit link lines fit their actual width", available && ideal.cx <= client.right);
    } else if (combo) {
        /* Its choices open in a menu (theme.c): the closed box, a drop-down
         * button showing the choice, is what shows. */
        HFONT font = (HFONT)SendMessageW(control, WM_GETFONT, 0, 0);
        int row, rows = (int)SendMessageW(control, CB_GETCOUNT, 0, 0);
        for (row = 0; row < rows; row++) {
            SendMessageW(control, CB_GETLBTEXT, row, (LPARAM)text);
            Check(fixture, control, "every choice fits the closed drop-down list", Theme_DropDownWidth(control, font, text) <= client.right);
        }
        Check(fixture, control, "closed drop-down list is as tall as its script font", client.bottom >= metrics.tmHeight);
    } else if (edit) {
        RECT textArea;
        SendMessageW(control, EM_GETRECT, 0, (LPARAM)&textArea);
        Check(fixture, control, "horizontally scrolling edit retains full font height", textArea.bottom - textArea.top >= metrics.tmHeight);
    } else if (text[0]) {
        BOOL wrap = label && type == SS_LEFT && !IntentionalEllipsis(control);
        if (button && (type == BS_CHECKBOX || type == BS_AUTOCHECKBOX)) {
            SIZE ideal = { 0, 0 };
            Check(fixture, control, "native checkbox glyph and caption fit the allocated control",
                  Theme_CheckBoxSize(control, &ideal) && ideal.cx <= client.right && ideal.cy <= client.bottom);
        } else {
            BOOL pushButton = button && (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON);
            RECT area = pushButton ? PushButtonTextArea(control, &client) : client;
            if (pushButton && Theme_IsIconButton(control)) {
                /* Its icon alone, its caption in a tip: a square as tall as the font. */
                Check(fixture, control, "an icon button is square and as tall as its script font",
                      client.right == client.bottom && client.bottom >= metrics.tmHeight);
                SelectObject(dc, previous);
                ReleaseDC(control, dc);
                return;
            }
            measured.right = max(1, client.right);
            DrawTextW(dc, text, -1, &measured, DT_CALCRECT | (wrap ? DT_WORDBREAK : DT_SINGLELINE) |
                      ((label && (style & SS_NOPREFIX)) ? DT_NOPREFIX : 0) | flags);
            Check(fixture, control, "caption font and wrapped lines fit actual height", measured.bottom <= client.bottom);
            if (pushButton) Check(fixture, control, "button caption fits inside its frame", measured.right <= area.right - area.left);
            else if (!IntentionalEllipsis(control)) Check(fixture, control, "caption fits actual width", measured.right <= client.right);
        }
    }
    SelectObject(dc, previous);
    ReleaseDC(control, dc);
}

/* The toolbar in one row above the body, left to right, one gap between
 * the buttons of a group and a wider one between groups, its icons the last
 * group; the column beside the list (or the sessions' panes): the view's
 * button level with the body's top, the status above the version, the
 * shield's badge at the end of the version's row, the foot (the version and
 * the badge, or Update) on the body's bottom. */
static void CheckMainBands(HWND dialog, const LayoutFixture *fixture)
{
    static const int kToolbar[] = { IDC_OPEN, IDC_STOP, IDC_RESTART, IDC_NEW, IDC_EDIT, IDC_DELETE, IDC_DEFAULT, IDC_SHORTCUTS, IDC_SETTINGS, IDC_ABOUT };
    static const BOOL kStartsGroup[] = { FALSE, FALSE, FALSE, TRUE, FALSE, FALSE, TRUE, TRUE, FALSE, FALSE };
    HWND update = GetDlgItem(dialog, IDC_UPDATE);
    RECT first = RelativeRect(dialog, GetDlgItem(dialog, kToolbar[0])), view = RelativeRect(dialog, GetDlgItem(dialog, IDC_SESSIONS));
    RECT body = RelativeRect(dialog, fixture->sessions ? fixture->profilesViewport : fixture->tableViewport);
    RECT status = RelativeRect(dialog, GetDlgItem(dialog, IDC_STATUS)), version = RelativeRect(dialog, GetDlgItem(dialog, IDC_VERSION));
    RECT last = (GetWindowLongW(update, GWL_STYLE) & WS_VISIBLE) ? RelativeRect(dialog, update) : version;
    BOOL row = TRUE, order = TRUE, gaps = TRUE;
    LONG within = -1, between = -1;
    size_t i;
    for (i = 1; i < ARRAYSIZE(kToolbar); i++) {
        RECT button = RelativeRect(dialog, GetDlgItem(dialog, kToolbar[i])), before = RelativeRect(dialog, GetDlgItem(dialog, kToolbar[i - 1]));
        LONG *expected = kStartsGroup[i] ? &between : &within;
        row = row && button.top == first.top && button.bottom == first.bottom;
        order = order && button.left > before.right;
        if (*expected < 0) *expected = button.left - before.right;
        gaps = gaps && button.left - before.right == *expected;
    }
    Check(fixture, GetDlgItem(dialog, kToolbar[0]), "the toolbar's buttons share one row", row);
    Check(fixture, GetDlgItem(dialog, kToolbar[0]), "the toolbar's buttons go left to right, apart", order);
    Check(fixture, GetDlgItem(dialog, IDC_SHORTCUTS), "the toolbar has one gap within its groups and a wider one between them, its icons included",
          gaps && between > within);
    Check(fixture, GetDlgItem(dialog, kToolbar[0]), "the toolbar sits above the body, where the content starts",
          first.bottom < body.top && first.left == body.left);
    if (fixture->sessions) {
        RECT tree = RelativeRect(dialog, fixture->treeViewport);
        Check(fixture, GetDlgItem(dialog, IDC_SESSIONS), "the view's button ends the search row, above the tree's right end",
              view.top >= body.top && view.bottom <= tree.top && view.right == tree.right && view.left > body.right);
        Check(fixture, GetDlgItem(dialog, IDC_STATUS), "the sessions view shows no column's foot",
              !IsWindowVisible(GetDlgItem(dialog, IDC_STATUS)) && !IsWindowVisible(GetDlgItem(dialog, IDC_VERSION)) && !IsWindowVisible(update));
        return;
    }
    {
        RECT search = RelativeRect(dialog, GetDlgItem(dialog, IDC_PROFILE_SEARCH));
        RECT label = RelativeRect(dialog, GetDlgItem(dialog, IDC_PROFILE_SEARCH_LABEL));
        if (GetWindowLongW(GetDlgItem(dialog, IDC_PROFILE_SEARCH), GWL_STYLE) & WS_VISIBLE) {
            Check(fixture, GetDlgItem(dialog, IDC_SESSIONS), "the view's button is level with the requested search row",
                  view.top == search.top && search.bottom < body.top && view.left > body.right);
            Check(fixture, GetDlgItem(dialog, IDC_PROFILE_SEARCH), "search label is vertically centered beside the edit",
                  label.left == body.left && label.right < search.left && search.right == body.right &&
                  abs(label.top + label.bottom - search.top - search.bottom) <= 1 && label.bottom < body.top);
        } else Check(fixture, GetDlgItem(dialog, IDC_LIST), "hidden search takes no row above the table",
                     view.top == body.top && !(GetWindowLongW(GetDlgItem(dialog, IDC_PROFILE_SEARCH_LABEL), GWL_STYLE) & WS_VISIBLE));
    }
    {
        RECT shield = RelativeRect(dialog, GetDlgItem(dialog, IDC_SHIELD));
        BOOL updating = (GetWindowLongW(update, GWL_STYLE) & WS_VISIBLE) != 0;
        Check(fixture, GetDlgItem(dialog, IDC_STATUS), "the status sits above the version, in the column",
              status.bottom <= version.top && status.left == view.left && version.left == view.left);
        Check(fixture, GetDlgItem(dialog, IDC_SHIELD), "the shield's badge ends the version's row, beside it",
              version.right < shield.left && shield.right == view.right && shield.top < version.bottom && shield.bottom > version.top);
        Check(fixture, GetDlgItem(dialog, IDC_VERSION), "the column's foot ends on the body's bottom",
              (updating ? last.bottom : max(version.bottom, shield.bottom)) == body.bottom);
    }
}

static void CheckFieldCaption(HWND dialog, const LayoutFixture *fixture, int captionId, int fieldId)
{
    HWND caption = GetDlgItem(dialog, captionId), field = GetDlgItem(dialog, fieldId);
    RECT label, box;
    if (!(GetWindowLongW(caption, GWL_STYLE) & WS_VISIBLE) || !(GetWindowLongW(field, GWL_STYLE) & WS_VISIBLE)) return;
    label = RelativeRect(dialog, caption);
    box = RelativeRect(dialog, field);
    Check(fixture, caption, "caption is vertically centered on its field", abs(label.top + label.bottom - box.top - box.bottom) <= 1);
    if (IsClass(caption, WC_STATICW)) {
        HDC dc = GetDC(caption);
        HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(caption, WM_GETFONT, 0, 0));
        TEXTMETRICW metrics;
        GetTextMetricsW(dc, &metrics);
        Check(fixture, caption, "short field caption has no extra blank text row", label.bottom - label.top <= metrics.tmHeight + 2);
        SelectObject(dc, old);
        ReleaseDC(caption, dc);
    }
}

/* Every visible control inside the client, captioned whole and apart from
 * the others; the dialog inside its monitor's work area. */
static void CheckGeometry(HWND dialog, const LayoutFixture *fixture)
{
    HWND controls[128], child;
    RECT client;
    int count = 0, i, j;
    GetClientRect(dialog, &client);
    Check(fixture, NULL, "dialog stays inside monitor work area", InsideWorkArea(dialog));
    for (child = GetWindow(dialog, GW_CHILD); child && count < (int)ARRAYSIZE(controls); child = GetWindow(child, GW_HWNDNEXT)) {
        RECT rect;
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        if (GetDlgCtrlID(child) == IDC_SHIELD) continue;   /* a floating badge, on top of the corner by design */
        rect = RelativeRect(dialog, child);
        Check(fixture, child, "visible control stays inside dialog client", rect.left >= 0 && rect.top >= 0 &&
              rect.right <= client.right && rect.bottom <= client.bottom);
        CheckText(fixture, child);
        controls[count++] = child;
    }
    for (i = 0; i < count; i++) {
        RECT first = RelativeRect(dialog, controls[i]);
        for (j = i + 1; j < count; j++) {
            RECT second = RelativeRect(dialog, controls[j]), overlap;
            BOOL clear = !IntersectRect(&overlap, &first, &second) || overlap.right - overlap.left <= 1 || overlap.bottom - overlap.top <= 1;
            Check(fixture, controls[i], "visible controls do not overlap", clear);
            if (!clear) printf("        other=%d overlap=%ldx%ld\n", GetDlgCtrlID(controls[j]), overlap.right - overlap.left, overlap.bottom - overlap.top);
        }
    }
    if (fixture->resource == IDD_MAIN) CheckMainBands(dialog, fixture);
    switch (fixture->resource) {
    case IDD_PROFILE_NEW:
        CheckFieldCaption(dialog, fixture, IDC_P_NAME_LABEL, IDC_P_NAME);
        CheckFieldCaption(dialog, fixture, IDC_P_COLOR_LABEL, IDC_P_COLOR);
        CheckFieldCaption(dialog, fixture, IDC_P_BADGE_LABEL, IDC_P_BADGE);
        CheckFieldCaption(dialog, fixture, IDC_P_TITLE_LABEL, IDC_P_TITLE);
        CheckFieldCaption(dialog, fixture, IDC_P_NOTE_LABEL, IDC_P_NOTE);
        CheckFieldCaption(dialog, fixture, IDC_P_SYNC_LABEL, IDC_P_SYNC);
        break;
    case IDD_RESTORE: CheckFieldCaption(dialog, fixture, IDC_R_PROFILE_LABEL, IDC_R_PROFILE); break;
    case IDD_PURGE:
        CheckFieldCaption(dialog, fixture, IDC_C_SHOW_LABEL, IDC_C_PROFILE);
        CheckFieldCaption(dialog, fixture, IDC_C_ALL, IDC_C_PROFILE);
        break;
    case IDD_CONFLICTS: CheckFieldCaption(dialog, fixture, IDC_X_LABEL, IDC_X_PROFILE); break;
        break;
    case IDD_PASSWORD:
        CheckFieldCaption(dialog, fixture, IDC_K_LABEL, IDC_K_PASSWORD);
        CheckFieldCaption(dialog, fixture, IDC_K_AGAIN_LABEL, IDC_K_AGAIN);
        break;
    case IDD_SIGNIN: CheckFieldCaption(dialog, fixture, IDC_SIGNIN_BROWSER_LABEL, IDC_SIGNIN_BROWSER); break;
    }
    if (fixture->resource == IDD_PROFILE_NEW) {
        RECT combo = RelativeRect(dialog, GetDlgItem(dialog, IDC_P_SYNC));
        RECT action = RelativeRect(dialog, GetDlgItem(dialog, IDC_P_SYNC_ITEMS));
        RECT label = RelativeRect(dialog, GetDlgItem(dialog, IDC_P_SYNC_LABEL));
        Check(fixture, GetDlgItem(dialog, IDC_P_SYNC), "sync choice and action have the same top and bottom",
              combo.top == action.top && combo.bottom == action.bottom);
        Check(fixture, GetDlgItem(dialog, IDC_P_SYNC_LABEL), "sync label is vertically centered on its controls",
              abs(label.top + label.bottom - combo.top - combo.bottom) <= 1);
    }
}

static void CheckProfileSearchLayout(void)
{
    size_t scale;
    int language;
    for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) if (g_scaleFits[scale])
        for (language = 0; language < Localize_LanguageCount(); language++) {
            LayoutFixture fixture = MainFixture(language, kFontScales[scale], FALSE);
            HWND dialog = CreateFixture(&fixture);
            Check(&fixture, NULL, "profile search layout fixture created", dialog != NULL);
            if (dialog) {
                Check(&fixture, GetDlgItem(dialog, IDC_PROFILE_SEARCH), "profile search starts hidden",
                      !(GetWindowLongW(GetDlgItem(dialog, IDC_PROFILE_SEARCH), GWL_STYLE) & WS_VISIBLE));
                CheckGeometry(dialog, &fixture);
                ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH_LABEL), SW_SHOW);
                ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH), SW_SHOW);
                Theme_LayoutMain(dialog);
                CheckGeometry(dialog, &fixture);
                ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH_LABEL), SW_HIDE);
                ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH), SW_HIDE);
                Theme_LayoutMain(dialog);
                CheckGeometry(dialog, &fixture);
            }
            DestroyFixture(dialog, &fixture);
        }
}

/* The table's columns as gui.c sizes them by default, and every Role
 * caption whole. */
static void CheckProfileTable(HWND dialog, const LayoutFixture *fixture)
{
    HWND table = fixture->table;
    RECT client;
    int first = ListView_GetColumnWidth(table, 0), second = ListView_GetColumnWidth(table, 1), third = ListView_GetColumnWidth(table, 2);
    int profileWidth, roleWidth, dataMinimum;
    GetClientRect(table, &client);
    Theme_ProfileColumnWidths(table, &profileWidth, &roleWidth, &dataMinimum, NULL);
    Check(fixture, table, "default Profile column is its fixed width at the window's DPI",
          first == MulDiv(THEME_PROFILE_COLUMN_DIPS, (int)GetDpiForWindow(table), 96));
    Check(fixture, table, "default Data column fits the data folder", third == dataMinimum);
    Check(fixture, table, "default Status column fits its title and every state", ListView_GetColumnWidth(table, PROFILE_COLUMN_STATE) == Theme_ProfileStateWidth(table));
    Check(fixture, table, "default Sessions folder column consumes exactly the remaining client width",
          ListView_GetColumnWidth(table, 3) == client.right - first - second - third - ListView_GetColumnWidth(table, PROFILE_COLUMN_STATE));
    Check(fixture, table, "default columns do not introduce a horizontal scroll bar", !(GetWindowLongW(table, GWL_STYLE) & WS_HSCROLL));
    if (ListView_GetColumnWidth(table, 3) != client.right - first - second - third - ListView_GetColumnWidth(table, PROFILE_COLUMN_STATE)) {
        RECT dialogClient;
        SIZE minimum = { 0, 0 };
        int sessionsMinimum = 0;
        GetClientRect(dialog, &dialogClient);
        Theme_MainMinimum(dialog, &minimum);
        Theme_ProfileColumnWidths(table, &profileWidth, &roleWidth, &dataMinimum, &sessionsMinimum);
        printf("        client=%ld columns=%d,%d,%d,%d budget=%d,%d,%d,%d dialog=%ldx%ld minimum=%ldx%ld\n", client.right, first, second,
               third, ListView_GetColumnWidth(table, 3), profileWidth, roleWidth, dataMinimum, sessionsMinimum, dialogClient.right,
               dialogClient.bottom, minimum.cx, minimum.cy);
    }
    CheckNativeRoles(fixture);
}

static void CheckHiddenProfileNote(HWND dialog, const LayoutFixture *fixture)
{
    struct { HWND window; RECT rect; } controls[128];
    HWND child, note = GetDlgItem(dialog, IDC_NOTE);
    RECT client, frame, actual;
    WCHAR name[LABEL_CCH], expected[1024], text[1024];
    const WCHAR *format = TR(Theme_MainNote());
    BOOL unchanged = TRUE;
    int count = 0, i;
    Check(fixture, note, "the sessions view hides the profile note", !(GetWindowLongW(note, GWL_STYLE) & WS_VISIBLE));
    FixtureName(fixture, name, ARRAYSIZE(name));
    StringCchPrintfW(expected, ARRAYSIZE(expected), format, name);
    GetClientRect(dialog, &client);
    GetWindowRect(dialog, &frame);
    for (child = GetWindow(dialog, GW_CHILD); child && count < (int)ARRAYSIZE(controls); child = GetWindow(child, GW_HWNDNEXT)) {
        controls[count].window = child;
        controls[count++].rect = RelativeRect(dialog, child);
    }
    SetWindowTextW(note, L"Private stale note");
    Theme_LayoutSidebarNote(note, format, name);
    GetClientRect(dialog, &actual);
    unchanged = EqualRect(&client, &actual);
    GetWindowRect(dialog, &actual);
    unchanged = unchanged && EqualRect(&frame, &actual);
    for (i = 0; i < count; i++) {
        actual = RelativeRect(dialog, controls[i].window);
        unchanged = unchanged && EqualRect(&controls[i].rect, &actual);
    }
    Check(fixture, note, "hidden profile note leaves dialog and control geometry unchanged", unchanged);
    GetWindowTextW(note, text, ARRAYSIZE(text));
    Check(fixture, note, "hidden profile note receives its complete current localized caption", wcscmp(text, expected) == 0);
}

/* ------------------------------------------------------- live languages */

typedef struct FontState {
    BOOL available;
    LOGFONTW logical;
    TEXTMETRICW metrics;
} FontState;

typedef struct LayoutState {
    RECT client;
    struct { HWND window; RECT rect; } children[128];
    struct { HWND window; FontState font; } fonts[128];
    FontState dialogFont, roles[THEME_FONTS];
    int childCount, fontCount;
} LayoutState;

static FontState ReadFont(HWND window, HFONT font)
{
    FontState state;
    HDC dc;
    HGDIOBJ previous;
    ZeroMemory(&state, sizeof state);
    if (!font || !GetObjectW(font, sizeof state.logical, &state.logical)) return state;
    dc = GetDC(window);
    if (!dc) return state;
    previous = SelectObject(dc, font);
    state.available = GetTextMetricsW(dc, &state.metrics);
    SelectObject(dc, previous);
    ReleaseDC(window, dc);
    return state;
}

static BOOL SameFont(const FontState *a, const FontState *b)
{
    return a->available && b->available &&
           a->logical.lfHeight == b->logical.lfHeight && a->logical.lfWidth == b->logical.lfWidth &&
           a->logical.lfWeight == b->logical.lfWeight && a->logical.lfCharSet == b->logical.lfCharSet &&
           a->logical.lfItalic == b->logical.lfItalic && a->logical.lfUnderline == b->logical.lfUnderline &&
           a->logical.lfStrikeOut == b->logical.lfStrikeOut &&
           wcscmp(a->logical.lfFaceName, b->logical.lfFaceName) == 0 &&
           a->metrics.tmHeight == b->metrics.tmHeight && a->metrics.tmAscent == b->metrics.tmAscent &&
           a->metrics.tmDescent == b->metrics.tmDescent && a->metrics.tmAveCharWidth == b->metrics.tmAveCharWidth;
}

static BOOL CALLBACK ReadChildFont(HWND child, LPARAM param)
{
    LayoutState *state = (LayoutState *)param;
    FontState font = ReadFont(child, (HFONT)SendMessageW(child, WM_GETFONT, 0, 0));
    if (font.available && state->fontCount < (int)ARRAYSIZE(state->fonts)) {
        state->fonts[state->fontCount].window = child;
        state->fonts[state->fontCount++].font = font;
    }
    return TRUE;
}

static void ReadLayout(HWND dialog, LayoutState *state)
{
    HWND child;
    ThemeFonts roles = { 0 };
    int i;
    ZeroMemory(state, sizeof *state);
    GetClientRect(dialog, &state->client);
    state->dialogFont = ReadFont(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0));
    for (child = GetWindow(dialog, GW_CHILD); child && state->childCount < (int)ARRAYSIZE(state->children);
         child = GetWindow(child, GW_HWNDNEXT)) {
        state->children[state->childCount].window = child;
        state->children[state->childCount++].rect = RelativeRect(dialog, child);
    }
    EnumChildWindows(dialog, ReadChildFont, (LPARAM)state);
    Theme_CreateFonts(dialog, &roles);
    for (i = 0; i < THEME_FONTS; i++) state->roles[i] = ReadFont(dialog, roles.font[i]);
    Theme_FreeFonts(&roles);
}

static void CompareLayout(HWND dialog, const LayoutFixture *fixture, const LayoutState *expected)
{
    LayoutState actual;
    int i;
    ReadLayout(dialog, &actual);
    Check(fixture, NULL, "same-language dialog font returns to its original face, size, weight and metrics",
          SameFont(&expected->dialogFont, &actual.dialogFont));
    for (i = 0; i < expected->fontCount; i++) {
        HWND child = expected->fonts[i].window;
        FontState font = ReadFont(child, (HFONT)SendMessageW(child, WM_GETFONT, 0, 0));
        Check(fixture, child, "same control font survives language round trips without changing metrics",
              IsChild(dialog, child) && SameFont(&expected->fonts[i].font, &font));
    }
    for (i = 0; i < THEME_FONTS; i++)
        Check(fixture, NULL, "derived text roles retain their original size, face and metrics", SameFont(&expected->roles[i], &actual.roles[i]));
    Check(fixture, NULL, "same-language client size is independent of previous languages", EqualRect(&expected->client, &actual.client));
    Check(fixture, NULL, "language changes retain the original direct controls", expected->childCount == actual.childCount);
    for (i = 0; i < expected->childCount; i++) {
        HWND child = expected->children[i].window;
        RECT rect = RelativeRect(dialog, child);
        Check(fixture, child, "same-language control geometry is independent of previous languages",
              GetParent(child) == dialog && EqualRect(&expected->children[i].rect, &rect));
    }
}

static INT_PTR CALLBACK TransitionProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    if (message == WM_INITDIALOG) {
        LayoutFixture *fixture = (LayoutFixture *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        InitializeFixture(dialog, fixture, TransitionCaptions);
        return TRUE;
    }
    return FixtureProc(dialog, message, wp, lp);
}

typedef struct FrameProbe {
    RECT client;
    int changes;
} FrameProbe;

static LRESULT CALLBACK ObserveFrame(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    FrameProbe *probe = (FrameProbe *)data;
    (void)id;
    if (message == WM_WINDOWPOSCHANGED) {
        RECT client;
        GetClientRect(window, &client);
        if (!EqualRect(&client, &probe->client)) probe->changes++;
    }
    return DefSubclassProc(window, message, wp, lp);
}

/* What gui.c does when the user picks another language, on a mock window. */
static void TransitionLanguage(HWND dialog, LayoutFixture *fixture, int language)
{
    FontState before = ReadFont(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0)), after;
    RECT previous, current;
    FrameProbe probe;
    GetClientRect(dialog, &previous);
    probe.client = previous;
    probe.changes = 0;
    SetWindowSubclass(dialog, ObserveFrame, FRAME_PROBE_SUBCLASS, (DWORD_PTR)&probe);
    fixture->language = language;
    Check(fixture, NULL, "live transition language selected for this process only", Localize_SetLanguage(language, FALSE));
    Localize_Window(dialog);
    Theme_Apply(dialog);
    TransitionCaptions(dialog, fixture);
    Theme_FitDialog(dialog);
    if (fixture->resource == IDD_MAIN) {
        Gui_LayoutProfileColumns(fixture->table);
        if (fixture->sessions) ShowMockSessions(dialog);
        else LayoutMockNote(dialog, fixture);
    }
    after = ReadFont(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0));
    GetClientRect(dialog, &current);
    if (fixture->resource == IDD_MAIN) {
        Check(fixture, NULL, "switching languages preserves the common main client size", EqualRect(&previous, &current));
        Check(fixture, NULL, "switching languages causes no intermediate native frame resize", probe.changes == 0);
    }
    RemoveWindowSubclass(dialog, ObserveFrame, FRAME_PROBE_SUBCLASS);
    Check(fixture, NULL, "dialog face covers the selected script", after.available && wcscmp(after.logical.lfFaceName, Localize_FontFace()) == 0);
    Check(fixture, NULL, "changing scripts preserves logical height, width, weight and charset", before.available && after.available &&
          before.logical.lfHeight == after.logical.lfHeight && before.logical.lfWidth == after.logical.lfWidth &&
          before.logical.lfWeight == after.logical.lfWeight && before.logical.lfCharSet == after.logical.lfCharSet);
}

typedef struct ModalCapture {
    LayoutFixture fixture;
    LayoutState *state;
} ModalCapture;

#define WM_MODAL_CAPTURE (WM_APP + 241)

/* The point size of a dialog template's FONT (app.rc): in a DLGTEMPLATEEX,
 * after its fixed part (13 words), its menu, class and title. */
static int ResourceFontPoints(int resource)
{
    HRSRC found = FindResourceW(g_hInst, MAKEINTRESOURCEW(resource), (LPCWSTR)RT_DIALOG);
    HGLOBAL loaded = found ? LoadResource(g_hInst, found) : NULL;
    const WORD *words = loaded ? (const WORD *)LockResource(loaded) : NULL;
    size_t count = found ? SizeofResource(g_hInst, found) / sizeof(WORD) : 0, at = 13, field;
    DWORD style;
    if (!words || count < 16 || words[1] != 0xFFFF) return 0;
    style = (DWORD)words[6] | ((DWORD)words[7] << 16);
    for (field = 0; field < 3 && at < count; field++) {
        if (field < 2 && words[at] == 0xFFFF) at += 2;   /* an ordinal */
        else {
            while (at < count && words[at]) at++;
            at++;
        }
    }
    return (style & DS_SETFONT) && at < count ? (int)words[at] : 0;
}

static INT_PTR CALLBACK ModalCaptureProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ModalCapture *capture = (ModalCapture *)GetWindowLongPtrW(dialog, DWLP_USER);
    (void)wp;
    if (message == WM_INITDIALOG) {
        BOOL queued;
        capture = (ModalCapture *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        TransitionCaptions(dialog, &capture->fixture);
        queued = PostMessageW(dialog, WM_MODAL_CAPTURE, 0, 0);
        Check(&capture->fixture, NULL, "private modal capture queued after shared dialog initialization", queued);
        if (!queued) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_MODAL_CAPTURE && capture) {
        int dpi = (int)GetDpiForWindow(dialog);
        CheckGeometry(dialog, &capture->fixture);
        Theme_Apply(dialog);
        CheckGeometry(dialog, &capture->fixture);
        ReadLayout(dialog, capture->state);
        Theme_FitDialog(dialog);
        Theme_Apply(dialog);
        CompareLayout(dialog, &capture->fixture, capture->state);
        Check(&capture->fixture, NULL, "shared modal shell retains its own native resource font size",
              capture->state->dialogFont.available && ResourceFontPoints(IDD_PROFILE) > 0 &&
              capture->state->dialogFont.logical.lfHeight == -MulDiv(ResourceFontPoints(IDD_PROFILE), dpi, 72));
        Check(&capture->fixture, NULL, "shared modal shell font covers the current script", capture->state->dialogFont.available &&
              wcscmp(capture->state->dialogFont.logical.lfFaceName, Localize_FontFace()) == 0);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    return FALSE;
}

static void ReadPrivateModal(HWND owner, const LayoutFixture *ownerFixture, LayoutState *state)
{
    ModalCapture capture;
    INT_PTR result;
    ZeroMemory(&capture, sizeof capture);
    ZeroMemory(state, sizeof *state);
    capture.fixture.resource = IDD_PROFILE;
    capture.fixture.language = ownerFixture->language;
    capture.fixture.fontScale = 96;
    capture.state = state;
    result = Ui_Dialog(owner, IDD_PROFILE, ModalCaptureProc, (LPARAM)&capture);
    Check(&capture.fixture, NULL, "private modal opens and closes through the shared dialog shell", result == IDOK);
}

/* A modal dialog of the manager uses the current language's script and its
 * own resource font, whatever its owner's scale and earlier languages. */
static void CheckReopenedModal(HWND owner, LayoutFixture *fixture, const LayoutState *french)
{
    LayoutState initial, script, reopened;
    int i, chinese = Language(L"zh-CN");
    ReadPrivateModal(owner, fixture, &initial);
    TransitionLanguage(owner, fixture, chinese);
    ReadPrivateModal(owner, fixture, &script);
    Check(fixture, NULL, "closed modal uses the current script independently of its owner's resource font",
          script.dialogFont.available && wcscmp(script.dialogFont.logical.lfFaceName, Localize_FontFaceAt(chinese)) == 0 &&
          initial.dialogFont.logical.lfHeight == script.dialogFont.logical.lfHeight);
    TransitionLanguage(owner, fixture, Language(L"en"));
    ReadPrivateModal(owner, fixture, &reopened);
    Check(fixture, NULL, "reopened English modal keeps its own original font and metrics", SameFont(&initial.dialogFont, &reopened.dialogFont));
    Check(fixture, NULL, "reopened English modal returns to its original client size", EqualRect(&initial.client, &reopened.client));
    Check(fixture, NULL, "reopened English modal retains the same resource control count", initial.childCount == reopened.childCount);
    if (initial.childCount == reopened.childCount) for (i = 0; i < initial.childCount; i++)
        Check(fixture, NULL, "reopened English modal retains its original resource geometry",
              EqualRect(&initial.children[i].rect, &reopened.children[i].rect));
    CompareLayout(owner, fixture, french);
}

/* The same dialog goes from English to Chinese and back: its fonts, size
 * and controls come back exactly. */
static void CheckLanguageRoundTrips(void)
{
    static const int kResources[] = { IDD_MAIN, IDD_PROFILE, IDD_PROFILE_NEW, IDD_TITLE, IDD_MESSAGE, IDD_UNINSTALL, IDD_SYNC, IDD_LINK, IDD_BACKUP,
                                      IDD_RESTORE, IDD_PURGE, IDD_SYNC_ITEMS, IDD_CONFLICTS, IDD_SYNC_SETUP, IDD_PREVIEW, IDD_SETTINGS,
                                      IDD_ABOUT, IDD_PASSWORD, IDD_SIGNIN, IDD_TEXTVIEW, IDD_SESSIONTOOLS, IDD_SESSIONFILTER, IDD_SESSIONSEARCH, IDD_VAULT_PICK, IDD_PURGE_OPEN,
                                      IDD_TRANSFER, IDD_SHIELD, IDD_SEARXNG, IDD_UPDATE };
    static const WCHAR *const kVisited[] = { L"zh-CN" };
    size_t resource, scale, visited;
    int view, round, french = Language(L"en");
    for (resource = 0; resource < ARRAYSIZE(kResources); resource++) for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++)
        for (view = 0; view < (kResources[resource] == IDD_MAIN ? 2 : 1); view++) {
            LayoutFixture fixture;
            LayoutState frenchLayout;
            HWND dialog;
            if (!g_scaleFits[scale]) continue;
            ZeroMemory(&fixture, sizeof fixture);
            fixture.resource = kResources[resource];
            fixture.language = french;
            fixture.fontScale = kFontScales[scale];
            fixture.sessions = view != 0;
            fixture.links = LINKS_ROUTED;
            Localize_SetLanguage(french, FALSE);
            dialog = CreateResourceDialog(fixture.resource, TransitionProc, (LPARAM)&fixture);
            Check(&fixture, NULL, "live transition private resource dialog created", dialog != NULL);
            if (dialog) {
                ReadLayout(dialog, &frenchLayout);
                for (round = 0; round < 2; round++) for (visited = 0; visited < ARRAYSIZE(kVisited); visited++) {
                    LayoutState translated;
                    TransitionLanguage(dialog, &fixture, Language(kVisited[visited]));
                    ReadLayout(dialog, &translated);
                    TransitionLanguage(dialog, &fixture, Language(kVisited[visited]));
                    CompareLayout(dialog, &fixture, &translated);
                    TransitionLanguage(dialog, &fixture, french);
                    CompareLayout(dialog, &fixture, &frenchLayout);
                }
                if (fixture.resource == IDD_MAIN) CheckReopenedModal(dialog, &fixture, &frenchLayout);
            }
            DestroyFixture(dialog, &fixture);
        }
}

/* ------------------------------------------------------ the main window */

/* Whether the dialogs scaled by each font scale fit this monitor's work
 * area: the manager window, the largest, needs its minimum. A smaller
 * desktop (a hosted build runner) checks the scales that fit, and says so. */
static void MeasureScalesAgainstWorkArea(void)
{
    size_t scale;
    for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
        LayoutFixture fixture = MainFixture(Language(L"en"), kFontScales[scale], FALSE);
        HWND dialog = CreateFixture(&fixture);
        SIZE minimum = { 0, 0 };
        RECT frame;
        MONITORINFO monitor = { sizeof monitor };
        g_scaleFits[scale] = FALSE;
        if (dialog && Theme_MainMinimum(dialog, &minimum) && GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) {
            SetRect(&frame, 0, 0, minimum.cx, minimum.cy);
            AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongW(dialog, GWL_STYLE), GetMenu(dialog) != NULL, (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE),
                                     GetDpiForWindow(dialog));
            g_scaleFits[scale] = frame.right - frame.left <= monitor.rcWork.right - monitor.rcWork.left &&
                                 frame.bottom - frame.top <= monitor.rcWork.bottom - monitor.rcWork.top;
            if (!g_scaleFits[scale])
                printf("  skip  font scale %d: the manager window needs %ldx%ld, more than this monitor's %ldx%ld work area\n",
                       kFontScales[scale], frame.right - frame.left, frame.bottom - frame.top,
                       monitor.rcWork.right - monitor.rcWork.left, monitor.rcWork.bottom - monitor.rcWork.top);
        }
        Check(&fixture, NULL, "main window measured at each font scale", dialog && minimum.cx > 0);
        DestroyFixture(dialog, &fixture);
    }
}

/* The checks made at the monitor's own scale (kFontScales[0]) need it to fit. */
static BOOL NativeScaleFits(const char *what)
{
    if (g_scaleFits[0]) return TRUE;
    printf("  skip  %s: font scale %d does not fit this monitor's work area\n", what, kFontScales[0]);
    return FALSE;
}

/* A dialog's rows hidden before it is fitted. */
typedef struct HiddenRowsCase {
    int resource, rowAbove;   /* the hidden band: below rowAbove's bottom, down to the lowest hidden control's */
    int hidden[3], hiddenCount;
    const char *name;
} HiddenRowsCase;

/* Rows a dialog hides close up, the gap above them kept: the new profile's
 * form without "Copy settings" (no profile to copy), without "Open it now"
 * (no Claude) or without both, the uninstall dialog without its profiles
 * (only Claude's own). Every row below the band moves up by it, the rows
 * above stay; a second fit changes nothing. */
static void CheckHiddenRows(void)
{
    static const HiddenRowsCase kCases[] = {
        { IDD_PROFILE_NEW, IDC_P_STARTUP, { IDC_P_COPY, IDC_P_OPEN, 0 }, 2, "the new profile's form without its last check boxes" },
        { IDD_PROFILE_NEW, IDC_P_STARTUP, { IDC_P_COPY, 0, 0 }, 1, "the new profile's form without Copy settings" },
        { IDD_PROFILE_NEW, IDC_P_COPY, { IDC_P_OPEN, 0, 0 }, 1, "the new profile's form without Open it now" },
        { IDD_UNINSTALL, IDC_U_KEEP, { IDC_U_LABEL, IDC_U_LIST, IDC_U_HINT }, 3, "the uninstall dialog without its profiles" },
    };
    size_t i, scale;
    for (i = 0; i < ARRAYSIZE(kCases); i++) for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
        LayoutFixture whole, closed;
        HWND wholeDialog, closedDialog;
        char name[200];
        if (!g_scaleFits[scale]) continue;
        ZeroMemory(&whole, sizeof whole);
        whole.resource = kCases[i].resource;
        whole.language = Language(L"en");
        whole.fontScale = kFontScales[scale];
        closed = whole;
        closed.hiddenControls = kCases[i].hidden;
        closed.hiddenCount = kCases[i].hiddenCount;
        wholeDialog = CreateFixture(&whole);
        closedDialog = CreateFixture(&closed);
        Check(&closed, NULL, "hidden rows fixtures created", wholeDialog && closedDialog);
        if (wholeDialog && closedDialog) {
            RECT aboveWhole = RelativeRect(wholeDialog, GetDlgItem(wholeDialog, kCases[i].rowAbove));
            RECT clientWhole, clientClosed, clientAgain;
            LayoutState before, after;
            HWND child, twin;
            int lowest = aboveWhole.bottom, band, moved = 0, kept = 0, childIndex;
            BOOL rowsFollow = TRUE, same = TRUE;
            for (childIndex = 0; childIndex < kCases[i].hiddenCount; childIndex++)
                lowest = max(lowest, RelativeRect(wholeDialog, GetDlgItem(wholeDialog, kCases[i].hidden[childIndex])).bottom);
            band = lowest - aboveWhole.bottom;
            GetClientRect(wholeDialog, &clientWhole);
            GetClientRect(closedDialog, &clientClosed);
            /* The same template: the same controls, in the same order (labels share an id). */
            for (child = GetWindow(closedDialog, GW_CHILD), twin = GetWindow(wholeDialog, GW_CHILD); child && twin;
                 child = GetWindow(child, GW_HWNDNEXT), twin = GetWindow(twin, GW_HWNDNEXT)) {
                RECT closedRect, wholeRect;
                if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
                closedRect = RelativeRect(closedDialog, child);
                wholeRect = RelativeRect(wholeDialog, twin);
                if (wholeRect.top >= lowest) {
                    moved++;
                    if (closedRect.top != wholeRect.top - band) rowsFollow = FALSE;
                } else {
                    kept++;
                    if (closedRect.top != wholeRect.top) rowsFollow = FALSE;
                }
            }
            StringCchPrintfA(name, ARRAYSIZE(name), "%s moves the rows below them up by their band, from the row above them", kCases[i].name);
            Check(&closed, NULL, name, band > 0 && moved > 0 && kept > 0 && rowsFollow);
            StringCchPrintfA(name, ARRAYSIZE(name), "%s is shorter by that band", kCases[i].name);
            Check(&closed, NULL, name, clientWhole.bottom - clientClosed.bottom == band);
            if (!rowsFollow || clientWhole.bottom - clientClosed.bottom != band)
                printf("        hidden-row band=%d client-delta=%ld above-bottom=%ld lowest=%d\n", band,
                       clientWhole.bottom - clientClosed.bottom, aboveWhole.bottom, lowest);
            ReadLayout(closedDialog, &before);
            Theme_FitDialog(closedDialog);
            ReadLayout(closedDialog, &after);
            GetClientRect(closedDialog, &clientAgain);
            for (childIndex = 0; childIndex < before.childCount && childIndex < after.childCount; childIndex++)
                same = same && EqualRect(&before.children[childIndex].rect, &after.children[childIndex].rect);
            StringCchPrintfA(name, ARRAYSIZE(name), "%s fitted again keeps its layout", kCases[i].name);
            Check(&closed, NULL, name, same && before.childCount == after.childCount && EqualRect(&clientClosed, &clientAgain));
            CheckGeometry(closedDialog, &closed);
        }
        DestroyFixture(wholeDialog, &whole);
        DestroyFixture(closedDialog, &closed);
    }
}

#define SHARED_ROWS_MAX 64

/* A list's rows as the group each sits under (-1 for a heading itself) and its text. */
static int ReadGroupedRows(HWND list, int *group, WCHAR (*text)[128])
{
    int row, count = ListView_GetItemCount(list), g = -1;
    for (row = 0; row < count && row < SHARED_ROWS_MAX; row++) {
        LVITEMW item;
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = row;
        item.pszText = text[row];
        item.cchTextMax = 128;
        text[row][0] = 0;
        ListView_GetItem(list, &item);
        if (item.lParam < 0) g++;
        group[row] = item.lParam < 0 ? -1 : g;
    }
    return min(count, SHARED_ROWS_MAX);
}

/* What a backup keeps and what profiles sync are one list: the backup's is
 * built on what to sync's groups and items (syncui.c), its own content added.
 * Every group opened, the same headings come in the same order (Account and
 * Profile the backup's own, last), and under the settings side's groups the same items,
 * the Settings group's MCP servers added at its end. */
static void CheckSharedItemLists(void)
{
    static const WCHAR opened[10][MAX_PATH] = { L"sessions", L"sidebar", L"details", L"appearance", L"language", L"model", L"settings", L"permissions",
                                                L"account", L"profile" };
    static WCHAR syncText[SHARED_ROWS_MAX][128], backupText[SHARED_ROWS_MAX][128];
    int syncGroup[SHARED_ROWS_MAX], backupGroup[SHARED_ROWS_MAX], syncRows, backupRows, s, b, g, syncHeads = 0, backupHeads = 0;
    BOOL headingsSame = TRUE, itemsSame = TRUE;
    LayoutFixture fixture;
    HWND parent, sync, backup;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_BACKUP;
    fixture.language = Language(L"en");
    Localize_SetLanguage(fixture.language, FALSE);
    Util_SetSettingList(L"SyncItemsOpened", opened, 8);
    Util_SetSettingList(L"BackupPartsOpened", opened, 10);
    parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 400, 400, NULL, NULL, g_hInst, NULL);
    sync = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 0, 0, 300, 300, parent, NULL, g_hInst, NULL);
    backup = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL, 0, 0, 300, 300, parent, NULL, g_hInst, NULL);
    Check(&fixture, NULL, "shared lists: made", parent && sync && backup);
    if (sync && backup) {
        SyncUi_FillItemsList(sync, SYNC_ITEMS_DEFAULT);
        Backup_FillPartsList(backup, 3);
        syncRows = ReadGroupedRows(sync, syncGroup, syncText);
        backupRows = ReadGroupedRows(backup, backupGroup, backupText);
        /* The headings, in order. */
        for (s = 0, b = 0; s < syncRows; s++) {
            if (syncGroup[s] != -1) continue;
            syncHeads++;
            while (b < backupRows && backupGroup[b] != -1) b++;
            if (b >= backupRows || wcscmp(syncText[s], backupText[b]) != 0) headingsSame = FALSE;
            b++;
        }
        for (b = 0; b < backupRows; b++)
            if (backupGroup[b] == -1) backupHeads++;
        Check(&fixture, NULL, "shared lists: the same headings in the same order, Account and Profile added last",
              headingsSame && backupHeads == syncHeads + 2 && syncHeads == 8);
        /* The settings side's groups (all but Sessions): the same items, the backup's MCP servers last in Settings. */
        for (g = 1; g < syncHeads; g++) {
            int syncItems = 0, backupItems = 0;
            for (s = 0; s < syncRows; s++) syncItems += syncGroup[s] == g;
            for (b = 0; b < backupRows; b++) backupItems += backupGroup[b] == g;
            for (s = 0, b = 0; s < syncRows; s++) {
                if (syncGroup[s] != g) continue;
                while (b < backupRows && backupGroup[b] != g) b++;
                if (b >= backupRows || wcscmp(syncText[s], backupText[b]) != 0) itemsSame = FALSE;
                b++;
            }
            if (backupItems != syncItems + (g == 6 ? 1 : 0)) itemsSame = FALSE;
        }
        Check(&fixture, NULL, "shared lists: the settings side's items are what to sync's, MCP servers added to Settings", itemsSame);
        Check(&fixture, NULL, "shared lists: Sessions holds the backup's own content",
              backupRows > 2 && backupGroup[1] == 0 && backupGroup[2] == 0 && wcsstr(backupText[2], L"Cowork") != NULL);
    }
    if (parent) DestroyWindow(parent);
    Util_SetSettingList(L"SyncItemsOpened", NULL, 0);
    Util_SetSettingList(L"BackupPartsOpened", NULL, 0);
}

/* The row whose lParam is `param`, -1 for none. */
static int RowOfParam(HWND list, LPARAM param)
{
    LVFINDINFOW find;
    ZeroMemory(&find, sizeof find);
    find.flags = LVFI_PARAM;
    find.lParam = param;
    return ListView_FindItem(list, -1, &find);
}

/* Row `row` selected, as a click selects it: the list's owner told. */
static void SelectValueRow(CheckList *checks, HWND owner, int row)
{
    NMLISTVIEW change;
    LRESULT result;
    ListView_SetItemState(checks->list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(checks->list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ZeroMemory(&change, sizeof change);
    change.hdr.hwndFrom = checks->list;
    change.hdr.code = LVN_ITEMCHANGED;
    change.iItem = row;
    change.uChanged = LVIF_STATE;
    change.uNewState = LVIS_SELECTED;
    CheckList_Notify(checks, owner, &change.hdr, &result);
}

/* A property list (the profile's own in a backup's list): its values in the
 * second column; the selected one's editor over that column, as tall as its
 * row, writing the value as it changes; a choice's drop-down holding its
 * choices; a switch with no editor; the group's box taking the values too. */
static void CheckValueRows(void)
{
    static const WCHAR opened[1][MAX_PATH] = { L"profile" };
    LayoutFixture fixture;
    CheckList *checks;
    HWND parent, list;
    RECT cell, editor;
    WCHAR text[64];
    int name, color, picture, heading;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_BACKUP;
    fixture.language = Language(L"en");
    Localize_SetLanguage(fixture.language, FALSE);
    Util_SetSettingList(L"BackupPartsOpened", opened, 1);
    parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 500, 500, NULL, NULL, g_hInst, NULL);
    list = parent ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                    0, 0, 400, 400, parent, NULL, g_hInst, NULL)
                  : NULL;
    checks = list ? Backup_FillPartsList(list, 3) : NULL;
    Check(&fixture, NULL, "value rows: made", checks != NULL);
    if (checks) {
        name = RowOfParam(list, CHECK_VALUE_BASE + 0);
        color = RowOfParam(list, CHECK_VALUE_BASE + 2);
        picture = RowOfParam(list, CHECK_VALUE_BASE + 5);
        text[0] = 0;
        if (name >= 0) ListView_GetItemText(list, name, 1, text, ARRAYSIZE(text));
        Check(&fixture, NULL, "value rows: the profile's own below its heading, values in the second column",
              name > 0 && color > name && picture > color && wcscmp(text, L"Work") == 0 &&
              (ListView_GetItemState(list, name, LVIS_STATEIMAGEMASK) >> 12) == 2);
        if (name > 0) SelectValueRow(checks, parent, name);
        Check(&fixture, NULL, "value rows: a text's editor is an edit over its value, as tall as its row",
              checks->editor && checks->editing == 0 && GetParent(checks->editor) == list && (GetWindowLongW(checks->editor, GWL_STYLE) & WS_VISIBLE) &&
              ListView_GetSubItemRect(list, name, 1, LVIR_BOUNDS, &cell) && GetWindowRect(checks->editor, &editor) &&
              MapWindowPoints(NULL, list, (POINT *)&editor, 2) >= 0 && editor.top == cell.top && editor.bottom == cell.bottom &&
              editor.left >= cell.left && editor.right <= cell.right && GetWindowTextW(checks->editor, text, ARRAYSIZE(text)) && wcscmp(text, L"Work") == 0);
        if (checks->editor) SetWindowTextW(checks->editor, L"Home");
        text[0] = 0;
        if (name > 0) ListView_GetItemText(list, name, 1, text, ARRAYSIZE(text));
        Check(&fixture, NULL, "value rows: what is typed is the value and its row's", wcscmp(checks->values[0].text, L"Home") == 0 && wcscmp(text, L"Home") == 0);
        if (color > 0) SelectValueRow(checks, parent, color);
        Check(&fixture, NULL, "value rows: a choice's editor is a drop-down of its choices, the value chosen",
              checks->editor && checks->editing == 2 && SendMessageW(checks->editor, CB_GETCOUNT, 0, 0) == PALETTE_SIZE &&
              SendMessageW(checks->editor, CB_GETCURSEL, 0, 0) == 2 && ListView_GetSubItemRect(list, color, 1, LVIR_BOUNDS, &cell) &&
              GetWindowRect(checks->editor, &editor) && editor.bottom - editor.top == cell.bottom - cell.top);
        if (picture > 0) SelectValueRow(checks, parent, picture);
        Check(&fixture, NULL, "value rows: a box alone has no editor", !checks->editor && checks->editing == -1);
        heading = name > 0 ? name - 1 : -1;
        CheckList_Toggle(checks, heading);
        Check(&fixture, NULL, "value rows: the group's box takes its values",
              heading >= 0 && !checks->values[0].taken && !checks->values[5].taken &&
              (ListView_GetItemState(list, heading, LVIS_STATEIMAGEMASK) >> 12) == 1 &&
              (ListView_GetItemState(list, name, LVIS_STATEIMAGEMASK) >> 12) == 1);
    }
    if (parent) DestroyWindow(parent);
    /* An import's: its sessions one by one under their item, each with when it was last used. */
    {
        static const WCHAR sessionsOpened[1][MAX_PATH] = { L"sessions" };
        int first = -1, row, rows, dated = 0;
        Util_SetSettingList(L"BackupPartsOpened", sessionsOpened, 1);
        parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 500, 500, NULL, NULL, g_hInst, NULL);
        list = parent ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                        0, 0, 400, 400, parent, NULL, g_hInst, NULL)
                      : NULL;
        checks = list ? Backup_FillImportList(list) : NULL;
        rows = list ? ListView_GetItemCount(list) : 0;
        for (row = 0; row < rows; row++) {
            WCHAR label[64] = L"", when[64] = L"";
            ListView_GetItemText(list, row, 0, label, ARRAYSIZE(label));
            ListView_GetItemText(list, row, 1, when, ARRAYSIZE(when));
            if (wcscmp(label, L"Fix the sign-in page") == 0) first = row;
            if (Theme_RowParam(list, row) >= CHECK_VALUE_BASE && when[0]) dated++;
        }
        Check(&fixture, NULL, "value rows: an import's sessions one by one below their item, each with when it was last used",
              checks && first > 1 && dated == 3 && (ListView_GetItemState(list, first, LVIS_STATEIMAGEMASK) >> 12) == 2 &&
              (ListView_GetItemState(list, first - 1, LVIS_STATEIMAGEMASK) >> 12) == 2);
        if (checks && first > 1) CheckList_Toggle(checks, first);
        Check(&fixture, NULL, "value rows: a session left out leaves its item mixed",
              checks && first > 1 && (ListView_GetItemState(list, first - 1, LVIS_STATEIMAGEMASK) >> 12) == 3);
        if (parent) DestroyWindow(parent);
    }
    Util_SetSettingList(L"BackupPartsOpened", NULL, 0);
}

/* What a property list's owner does: its notifications to the list, a box
 * turned over through it, and each value change counted. */
static int g_valueChanges;

static LRESULT CALLBACK PropertyHost(HWND host, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    CheckList *checks = (CheckList *)ref;
    LRESULT result;
    BOOL toggled;
    (void)id;
    if (message == WM_NOTIFY && lp && CheckList_Notify(checks, host, (const NMHDR *)lp, &result)) return result;
    if (CheckList_Message(checks, host, message, wp, lp, &toggled)) return 0;
    if (message == WM_CHECKLIST_VALUE) g_valueChanges++;
    return DefSubclassProc(host, message, wp, lp);
}

/* A click on cell (`row`, `column`), as the list tells its owner. */
static void ClickCell(CheckList *checks, HWND owner, int row, int column)
{
    NMITEMACTIVATE click;
    RECT cell;
    LRESULT result;
    ZeroMemory(&click, sizeof click);
    if (!ListView_GetSubItemRect(checks->list, row, column, LVIR_BOUNDS, &cell)) return;
    if (!column) cell.right = cell.left + ListView_GetColumnWidth(checks->list, 0);
    click.hdr.hwndFrom = checks->list;
    click.hdr.code = NM_CLICK;
    click.iItem = row;
    click.iSubItem = column;
    click.ptAction.x = cell.right - 4;
    click.ptAction.y = (cell.top + cell.bottom) / 2;
    CheckList_Notify(checks, owner, &click.hdr, &result);
}

/* A key on the list's focused row, as the list tells its owner. */
static void ListKey(CheckList *checks, HWND owner, WORD key)
{
    NMLVKEYDOWN down;
    LRESULT result;
    ZeroMemory(&down, sizeof down);
    down.hdr.hwndFrom = checks->list;
    down.hdr.code = LVN_KEYDOWN;
    down.wVKey = key;
    CheckList_Notify(checks, owner, &down.hdr, &result);
}

static int CellImage(HWND list, int row, int column)
{
    LVITEMW cell;
    ZeroMemory(&cell, sizeof cell);
    cell.mask = LVIF_IMAGE;
    cell.iItem = row;
    cell.iSubItem = column;
    cell.iImage = I_IMAGENONE;
    ListView_GetItem(list, &cell);
    return cell.iImage;
}

static int RowBox(HWND list, int row)
{
    return (int)((ListView_GetItemState(list, row, LVIS_STATEIMAGEMASK) & LVIS_STATEIMAGEMASK) >> 12);
}

/* The pane beside a property list (checklist.c), as the dialogs have it. */
static void AddPane(HWND parent)
{
    CreateWindowExW(0, L"STATIC", L"Preview pane", WS_CHILD | WS_VISIBLE, 510, 0, 200, 20, parent, (HMENU)(INT_PTR)IDC_PANE_LABEL, g_hInst, NULL);
    CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 510, 20, 200, 20, parent, (HMENU)(INT_PTR)IDC_PANE_TITLE, g_hInst, NULL);
    CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 510, 40, 180, 20, parent, (HMENU)(INT_PTR)IDC_PANE_SEARCH, g_hInst, NULL);
    CreateWindowExW(0, L"BUTTON", L"Search options", WS_CHILD | WS_VISIBLE, 690, 40, 20, 20, parent, (HMENU)(INT_PTR)IDC_PANE_FIND, g_hInst, NULL);
    CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS, 510, 64, 200, 500,
                    parent, (HMENU)(INT_PTR)IDC_PANE_LIST, g_hInst, NULL);
}

/* The indent of `row` in `list`. */
static int RowIndent(HWND list, int row)
{
    LVITEMW item;
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_INDENT;
    item.iItem = row;
    item.iIndent = -1;
    ListView_GetItem(list, &item);
    return item.iIndent;
}

/* The first value with parts on a row of its own, -1 for none. */
static int ValueWithParts(const CheckList *checks)
{
    int v;
    for (v = 0; v + 1 < checks->valueCount; v++)
        if (checks->values[v + 1].parent == v + 1 && !checks->values[v].merged) return v;
    return -1;
}

/* The edit dialog's list (gui.c): no boxes; the profile's own under Profile,
 * a text in place, a switch turned over by Space, the picture an action that
 * tells the dialog; Sync's choice on its heading; the settings one by one
 * under their groups, folded at first, one group's only value on its
 * heading. The pane beside it shows what the row selected holds, as the list
 * would below it, further out: a value alone itself, changed there, the list
 * following, and the other way round; a heading its values; a setting its
 * parts; a value's parts folding apart from the list's. */
static void CheckProfileList(void)
{
    LayoutFixture fixture;
    CheckList *checks, *pane = NULL;
    HWND parent, list;
    WCHAR text[64] = L"", sync[64] = L"", language[64] = L"", paneTitle[64] = L"";
    int name, startup, picture, row, rows, settings = -1, languageRow = -1, changes, parts, at;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_PROFILE;
    fixture.language = Language(L"en");
    Localize_SetLanguage(fixture.language, FALSE);
    Util_SetSettingList(L"ProfileOpened", NULL, 0);
    parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 720, 700, NULL, NULL, g_hInst, NULL);
    list = parent ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                    0, 0, 500, 600, parent, NULL, g_hInst, NULL)
                  : NULL;
    if (list) AddPane(parent);
    checks = list ? Gui_FillProfileList(list) : NULL;
    if (checks) SetWindowSubclass(parent, PropertyHost, 1, (DWORD_PTR)checks);
    if (checks) pane = checks->pane;
    Check(&fixture, NULL, "profile list: made, its pane beside it", checks != NULL && pane && pane->twin == checks && pane->list);
    if (checks) {
        name = RowOfParam(list, CHECK_VALUE_BASE + 0);
        picture = RowOfParam(list, CHECK_VALUE_BASE + 3);
        startup = RowOfParam(list, CHECK_VALUE_BASE + 6);
        if (name >= 0) ListView_GetItemText(list, name, 1, text, ARRAYSIZE(text));
        row = RowOfParam(list, THEME_HEADING_ROW(1));
        if (row >= 0) ListView_GetItemText(list, row, 1, sync, ARRAYSIZE(sync));
        rows = ListView_GetItemCount(list);
        for (row = 0; row < rows; row++) {
            WCHAR label[64] = L"";
            ListView_GetItemText(list, row, 0, label, ARRAYSIZE(label));
            if (wcscmp(label, L"Settings") == 0) settings = row;
            if (wcscmp(label, L"Interface language") == 0) languageRow = row;
        }
        if (languageRow >= 0) ListView_GetItemText(list, languageRow, 1, language, ARRAYSIZE(language));
        Check(&fixture, NULL, "profile list: no boxes, the profile's own open below its heading with their values",
              name == 1 && picture > name && startup > picture && wcscmp(text, L"Work") == 0 && RowBox(list, name) == 0 && RowBox(list, 0) == 0);
        Check(&fixture, NULL, "profile list: Sync's choice on its heading", wcscmp(sync, TR(L"Not synced")) == 0);
        Check(&fixture, NULL, "profile list: the settings under their groups, folded at first, a group's only value on its heading",
              settings > startup && RowOfParam(list, CHECK_VALUE_BASE + 9) < 0 && wcscmp(language, L"zh-CN") == 0);
        if (name > 0) SelectValueRow(checks, parent, name);
        Check(&fixture, NULL, "profile list: a text is changed in place",
              checks->editor && checks->editing == 0 && GetWindowTextW(checks->editor, text, ARRAYSIZE(text)) && wcscmp(text, L"Work") == 0);
        GetDlgItemTextW(parent, IDC_PANE_TITLE, paneTitle, ARRAYSIZE(paneTitle));
        Check(&fixture, NULL, "pane: a value alone, itself, its editor open",
              pane && wcscmp(paneTitle, L"Name") == 0 && ListView_GetItemCount(pane->list) == 1 && RowOfParam(pane->list, CHECK_VALUE_BASE + 0) == 0 &&
              RowIndent(pane->list, 0) == 1 && pane->editor && pane->editing == 0 && GetWindowTextW(pane->editor, text, ARRAYSIZE(text)) &&
              wcscmp(text, L"Work") == 0);
        changes = g_valueChanges;
        if (pane && pane->editor) SetWindowTextW(pane->editor, L"Home");
        text[0] = 0;
        if (name >= 0) ListView_GetItemText(list, name, 1, text, ARRAYSIZE(text));
        Check(&fixture, NULL, "pane: typed there, the value, its row and the list's editor follow, the dialog told",
              wcscmp(checks->values[0].text, L"Home") == 0 && wcscmp(text, L"Home") == 0 && g_valueChanges > changes && checks->editor &&
              GetWindowTextW(checks->editor, text, ARRAYSIZE(text)) && wcscmp(text, L"Home") == 0);
        if (checks->editor) SetWindowTextW(checks->editor, L"Home office");
        text[0] = 0;
        if (pane && pane->editor) GetWindowTextW(pane->editor, text, ARRAYSIZE(text));
        Check(&fixture, NULL, "pane: typed in the list, the pane's editor follows", wcscmp(text, L"Home office") == 0);
        if (startup > 0) SelectValueRow(checks, parent, startup);
        ListKey(checks, parent, VK_SPACE);
        Check(&fixture, NULL, "profile list: Space turns a switch over", startup > 0 && !checks->values[6].on && !checks->editor);
        Check(&fixture, NULL, "pane: a switch, as the list has it", pane && CellImage(pane->list, 0, 1) == THEME_CELL_BOX);
        changes = g_valueChanges;
        if (pane) ClickCell(pane, parent, 0, 1);
        Check(&fixture, NULL, "pane: turned over there, the list follows", checks->values[6].on && g_valueChanges == changes + 1 &&
              CellImage(list, startup, 1) == (THEME_CELL_BOX + THEME_CELL_BOX_CHECKED));
        SelectValueRow(checks, parent, 0);
        GetDlgItemTextW(parent, IDC_PANE_TITLE, paneTitle, ARRAYSIZE(paneTitle));
        text[0] = 0;
        if (pane) ListView_GetItemText(pane->list, 0, 1, text, ARRAYSIZE(text));
        Check(&fixture, NULL, "pane: a heading's values, further out, to change there",
              pane && wcscmp(paneTitle, L"Profile") == 0 && RowOfParam(pane->list, CHECK_VALUE_BASE + 0) == 0 &&
              RowOfParam(pane->list, CHECK_VALUE_BASE + 6) > 0 && RowIndent(pane->list, 0) == 1 && wcscmp(text, L"Home office") == 0 &&
              ListView_GetItemCount(pane->list) == startup);
        at = ValueWithParts(checks);
        parts = 0;
        for (row = at + 1; at >= 0 && row < checks->valueCount && checks->values[row].parent; row++) parts += checks->values[row].parent == at + 1;
        if (at >= 0) CheckList_SelectValue(checks, at, FALSE);
        row = pane && at >= 0 ? RowOfParam(pane->list, CHECK_VALUE_BASE + at) : -1;
        {
            WCHAR held[64] = L"", said[64] = L"";
            int itemRow = at >= 0 && checks->values[at].item ? RowOfParam(list, (LPARAM)checks->values[at].item) : -1, n = 0, w;
            for (w = 0; itemRow >= 0 && w < checks->valueCount; w++)
                n += checks->values[w].item == checks->values[at].item && !checks->values[w].merged && !checks->values[w].parent;
            if (n) StringCchPrintfW(held, ARRAYSIZE(held), TR(L"%d items"), n);
            if (itemRow >= 0) ListView_GetItemText(list, itemRow, 1, said, ARRAYSIZE(said));
            Check(&fixture, NULL, "overview: a setting's item says how many values it holds, its values not below it; selected, the pane shows them",
                  pane && at >= 0 && parts > 0 && itemRow >= 0 && wcscmp(said, held) == 0 && RowOfParam(list, CHECK_VALUE_BASE + at) < 0 &&
                  ListView_GetNextItem(list, -1, LVNI_SELECTED) == itemRow && row >= 0 && ListView_GetNextItem(pane->list, -1, LVNI_SELECTED) == row &&
                  RowIndent(pane->list, row) == 1 && RowOfParam(pane->list, CHECK_VALUE_BASE + at + 1) < 0);
        }
        rows = pane ? ListView_GetItemCount(pane->list) : 0;
        if (row >= 0) ListKey(pane, parent, VK_RIGHT);
        Check(&fixture, NULL, "pane: a value's parts opened there, not in the list",
              row >= 0 && ListView_GetItemCount(pane->list) == rows + parts && RowOfParam(pane->list, CHECK_VALUE_BASE + at + 1) == row + 1 &&
              checks->values[at].paneOpen && !checks->values[at].open);
        if (pane) SetWindowTextW(GetDlgItem(parent, IDC_PANE_SEARCH), checks->values[at + 1].label);
        text[0] = 0;
        if (pane && ListView_GetItemCount(pane->list) > 0) ListView_GetItemText(pane->list, ListView_GetItemCount(pane->list) - 1, 0, text, ARRAYSIZE(text));
        Check(&fixture, NULL, "pane: its search keeps what matches and what holds it",
              pane && pane->search && RowOfParam(pane->list, CHECK_VALUE_BASE + at) == 0 && RowOfParam(pane->list, CHECK_VALUE_BASE + at + 1) == 1 &&
              wcscmp(text, checks->values[at + 1].label) == 0 && ListView_GetItemCount(pane->list) < rows + parts);
        if (pane) SetWindowTextW(GetDlgItem(parent, IDC_PANE_SEARCH), L"");
        Check(&fixture, NULL, "pane: an empty search keeps every row", pane && ListView_GetItemCount(pane->list) == rows + parts);
        changes = g_valueChanges;
        if (picture > 0) ClickCell(checks, parent, picture, 1);
        Check(&fixture, NULL, "profile list: a click on the picture's action tells the dialog", picture > 0 && g_valueChanges == changes + 1);
        CheckList_SelectValue(checks, 9, FALSE);
        Check(&fixture, NULL, "profile list: a setting selected opens its group", RowOfParam(list, CHECK_VALUE_BASE + 9) > settings ||
              RowOfParam(list, (LPARAM)checks->values[9].item) > 0);
    }
    if (parent) DestroyWindow(parent);
    Util_SetSettingList(L"ProfileOpened", NULL, 0);
}

/* The dialog's font as it opens, its height into lParam's int; it closes at once, unseen. */
static INT_PTR CALLBACK ZoomProbeProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    LOGFONTW font;
    (void)wp;
    if (message != WM_INITDIALOG) return FALSE;
    ZeroMemory(&font, sizeof font);
    GetObjectW((HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0), sizeof font, &font);
    *(int *)lp = font.lfHeight;
    EndDialog(dialog, IDOK);
    return TRUE;
}

/* The windows' size (Settings' Zoom): a dialog opens with its font that
 * much larger, its layout with it; an unknown size is 100%. */
static void CheckZoom(void)
{
    LayoutFixture fixture;
    int normal = 0, larger = 0, unknown = 0;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_SETTINGS;
    fixture.language = Language(L"en");
    Util_SetSetting(SETTING_ZOOM, 100);
    Theme_DialogBox(IDD_SETTINGS, NULL, ZoomProbeProc, (LPARAM)&normal);
    Util_SetSetting(SETTING_ZOOM, 200);
    Check(&fixture, NULL, "zoom: 200% read back", Theme_Zoom() == 200);
    Theme_DialogBox(IDD_SETTINGS, NULL, ZoomProbeProc, (LPARAM)&larger);
    Util_SetSetting(SETTING_ZOOM, 1000);
    Theme_DialogBox(IDD_SETTINGS, NULL, ZoomProbeProc, (LPARAM)&unknown);
    Util_SetSetting(SETTING_ZOOM, 100);
    Check(&fixture, NULL, "zoom: a dialog's font twice as large at 200%", normal < 0 && abs(larger - 2 * normal) <= 1);
    Check(&fixture, NULL, "zoom: a size out of range is 100%", unknown == normal && Theme_Zoom() == 100);
}

/* The conflicts dialog's list (syncui.c): read-only, a heading per kind of
 * what syncs, open; each element below it with a profile's version in each
 * column, the latest kept; a click or Right keeps another. */
static void CheckConflictsList(void)
{
    LayoutFixture fixture;
    CheckList *checks;
    HWND parent, list;
    WCHAR first[64] = L"", second[64] = L"";
    int heading, row = -1;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_CONFLICTS;
    fixture.language = Language(L"en");
    Localize_SetLanguage(fixture.language, FALSE);
    parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 600, 500, NULL, NULL, g_hInst, NULL);
    list = parent ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                    0, 0, 500, 300, parent, NULL, g_hInst, NULL)
                  : NULL;
    checks = list ? SyncUi_FillConflictsList(list, L"Work") : NULL;
    if (checks) SetWindowSubclass(parent, PropertyHost, 1, (DWORD_PTR)checks);
    Check(&fixture, NULL, "conflicts list: made", checks != NULL);
    if (checks) {
        heading = RowOfParam(list, THEME_HEADING_ROW(1));
        row = RowOfParam(list, CHECK_VALUE_BASE + 0);
        if (row >= 0) ListView_GetItemText(list, row, 1, first, ARRAYSIZE(first));
        if (row >= 0) ListView_GetItemText(list, row, 2, second, ARRAYSIZE(second));
        Check(&fixture, NULL, "conflicts list: no boxes, the elements open below their kind, a version per profile",
              heading == 0 && row == 1 && RowOfParam(list, CHECK_VALUE_BASE + 1) == 2 && RowBox(list, row) == 0 &&
              wcscmp(first, L"Work") == 0 && wcscmp(second, L"Personal work") == 0);
        Check(&fixture, NULL, "conflicts list: the latest kept, the first element selected",
              checks->values[0].pick == 0 && checks->values[1].pick == 1 && CellImage(list, row, 1) == checks->pickedImage &&
              CellImage(list, row, 2) == checks->candidateImage && ListView_GetNextItem(list, -1, LVNI_SELECTED) == row);
        ListKey(checks, parent, VK_RIGHT);
        Check(&fixture, NULL, "conflicts list: Right keeps the next version", checks->values[0].pick == 1 && CellImage(list, row, 2) == checks->pickedImage);
        ClickCell(checks, parent, row, 1);
        Check(&fixture, NULL, "conflicts list: a click keeps a version", checks->values[0].pick == 0 && !checks->editor);
    }
    if (parent) DestroyWindow(parent);
}

/* The property list's own rules, on a list of its own: an item's only value
 * on its row; values below their item, their switches check boxes in their
 * cells, turned over by a click and by Space, and still while their item is
 * not checked; an item whose values have boxes mixed when some are taken,
 * all or none by its box; a pick list's versions side by side, the one kept
 * clicked or stepped to, those it lacks skipped. Every change tells the
 * owner. */
static void CheckPropertyList(void)
{
    static const WCHAR opened[1][MAX_PATH] = { L"property-test" };
    static const CheckGroup kGroups[] = { { 0x7, L"Settings", 0, L"property-test" } };
    static const CheckItem kItems[] = { { 0x1, L"Archive when the PR closes", 0 }, { 0x2, L"Cowork", 0 }, { 0x4, L"New sessions", 0 } };
    static const WCHAR kGlyphs[] = { 0xE713, 0xEC61, 0xEA3A };
    static const ThemeTint kTints[] = { THEME_TINT_BLUE, THEME_TINT_GREEN, THEME_TINT_NONE };
    static const WCHAR *const kBrowsers[] = { L"Chrome", L"Edge" };
    static const WCHAR *const kTitles[] = { L"Changed", L"Main", L"Work", L"Test" };
    static const WCHAR *const kFirst[] = { L"on", L"off", NULL }, *const kSecond[] = { L"a", NULL, L"c" };
    LayoutFixture fixture;
    CheckList checks, picks;
    CheckValue values[5], versions[2];
    HWND parent, list, pickList;
    int merged, browser, search, sessions, a, b, i, changes;
    ZeroMemory(&fixture, sizeof fixture);
    fixture.resource = IDD_BACKUP;
    fixture.language = Language(L"en");
    Localize_SetLanguage(fixture.language, FALSE);
    Util_SetSettingList(L"PropertyTestOpened", opened, 1);
    ZeroMemory(&checks, sizeof checks);
    ZeroMemory(values, sizeof values);
    for (i = 0; i < (int)ARRAYSIZE(values); i++) {
        values[i].group = 0;
        values[i].image = -1;
        values[i].pick = -1;
    }
    values[0].item = 0x1, values[0].kind = CHECK_VALUE_SWITCH, values[0].merged = TRUE, values[0].on = TRUE;
    values[1].item = 0x2, values[1].kind = CHECK_VALUE_SWITCH, values[1].label = L"Cowork web search";
    values[2].item = 0x2, values[2].kind = CHECK_VALUE_CHOICE, values[2].label = L"Cowork's browser", values[2].choices = kBrowsers, values[2].choiceCount = 2;
    values[3].item = 0x4, values[3].boxed = TRUE, values[3].taken = TRUE, values[3].literal = TRUE, values[3].label = L"Session A";
    StringCchCopyW(values[3].text, ARRAYSIZE(values[3].text), L"today");
    values[4].item = 0x4, values[4].boxed = TRUE, values[4].literal = TRUE, values[4].label = L"Session B";
    checks.groups = kGroups;
    checks.groupCount = 1;
    checks.items = kItems;
    checks.itemCount = 3;
    checks.values = values;
    checks.valueCount = 5;
    checks.openedSection = L"PropertyTestOpened";
    checks.offered = 0x7;
    checks.checked = 0x7;
    parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 600, 500, NULL, NULL, g_hInst, NULL);
    list = parent ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                    0, 0, 500, 400, parent, NULL, g_hInst, NULL)
                  : NULL;
    if (list) SetWindowSubclass(parent, PropertyHost, 1, (DWORD_PTR)&checks);
    if (list) CheckList_Setup(&checks, list, kGlyphs, kTints, 3);
    Check(&fixture, NULL, "property list: made", list != NULL);
    if (list) {
        merged = RowOfParam(list, 0x1);
        browser = RowOfParam(list, CHECK_VALUE_BASE + 2);
        search = RowOfParam(list, CHECK_VALUE_BASE + 1);
        sessions = RowOfParam(list, 0x4);
        a = RowOfParam(list, CHECK_VALUE_BASE + 3);
        b = RowOfParam(list, CHECK_VALUE_BASE + 4);
        Check(&fixture, NULL, "property list: an item's only value shows on its row, its switch a check box in its cell",
              merged > 0 && RowOfParam(list, CHECK_VALUE_BASE + 0) < 0 && CellImage(list, merged, 1) == (THEME_CELL_BOX + THEME_CELL_BOX_CHECKED));
        Check(&fixture, NULL, "property list: values below their item, in its order",
              search > merged && browser == search + 1 && sessions > browser && a == sessions + 1 && b == a + 1 &&
              CellImage(list, search, 1) == THEME_CELL_BOX);
        Check(&fixture, NULL, "property list: an item whose values have boxes is mixed when some are taken, checked when one is",
              RowBox(list, sessions) == 3 && RowBox(list, a) == 2 && RowBox(list, b) == 1 && (checks.checked & 0x4));
        CheckList_Toggle(&checks, sessions);
        Check(&fixture, NULL, "property list: its box takes all its values", values[3].taken && values[4].taken && RowBox(list, sessions) == 2);
        CheckList_Toggle(&checks, sessions);
        Check(&fixture, NULL, "property list: then none, the item unchecked", !values[3].taken && !values[4].taken && RowBox(list, sessions) == 1 &&
              !(checks.checked & 0x4));
        CheckList_Toggle(&checks, a);
        Check(&fixture, NULL, "property list: a value's box checks its item again", values[3].taken && (checks.checked & 0x4) && RowBox(list, sessions) == 3);
        changes = g_valueChanges;
        ClickCell(&checks, parent, search, 1);
        Check(&fixture, NULL, "property list: a click on a switch turns it over and tells the owner",
              values[1].on && g_valueChanges == changes + 1 && CellImage(list, search, 1) == (THEME_CELL_BOX + THEME_CELL_BOX_CHECKED));
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list, search, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
        SendMessageW(list, WM_CHAR, L' ', 0);
        SendMessageW(list, WM_KEYUP, VK_SPACE, 0xC0000001);
        PumpMessages();
        Check(&fixture, NULL, "property list: Space turns a switch without a box over, once", !values[1].on && g_valueChanges == changes + 2);
        CheckList_Toggle(&checks, RowOfParam(list, 0x2));
        changes = g_valueChanges;
        ClickCell(&checks, parent, search, 1);
        Check(&fixture, NULL, "property list: the values of an item not checked stay as they are, shown so",
              !(checks.checked & 0x2) && !values[1].on && g_valueChanges == changes &&
              CellImage(list, search, 1) == (THEME_CELL_BOX + THEME_CELL_BOX_DISABLED));
        CheckList_Toggle(&checks, RowOfParam(list, 0x2));
        CheckList_CheckAll(&checks, CHECKLIST_NONE);
        Check(&fixture, NULL, "property list: Select none unchecks every box, items and values",
              !(checks.checked & 0x7) && !values[3].taken && !values[4].taken && RowBox(list, sessions) == 1 && RowBox(list, merged) == 1);
        CheckList_Toggle(&checks, a);
        CheckList_CheckAll(&checks, CHECKLIST_INVERT);
        Check(&fixture, NULL, "property list: Invert turns each over: an item without boxed values its own box, values one by one",
              (checks.checked & 0x7) == 0x7 && !values[3].taken && values[4].taken && RowBox(list, sessions) == 3 && RowBox(list, merged) == 2);
        CheckList_CheckAll(&checks, CHECKLIST_ALL);
        Check(&fixture, NULL, "property list: Select all checks every box",
              (checks.checked & 0x7) == 0x7 && values[3].taken && values[4].taken && RowBox(list, sessions) == 2);
    }
    if (parent) {
        RemoveWindowSubclass(parent, PropertyHost, 1);
        DestroyWindow(parent);
    }
    /* A pick list: versions side by side, the one kept checked. */
    ZeroMemory(&picks, sizeof picks);
    ZeroMemory(versions, sizeof versions);
    for (i = 0; i < 2; i++) {
        versions[i].group = -1;
        versions[i].kind = CHECK_VALUE_PICK;
        versions[i].image = 0;
        versions[i].literal = TRUE;
    }
    versions[0].label = L"Archive when the PR closes", versions[0].versions = kFirst, versions[0].pick = 0;
    versions[1].label = L"Default model", versions[1].versions = kSecond, versions[1].pick = 2;
    picks.values = versions;
    picks.valueCount = 2;
    picks.columns = kTitles;
    picks.columnCount = 4;
    picks.plain = TRUE;
    picks.pickedImage = 1;
    picks.candidateImage = 2;
    parent = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 600, 500, NULL, NULL, g_hInst, NULL);
    pickList = parent ? CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                                        0, 0, 500, 300, parent, NULL, g_hInst, NULL)
                      : NULL;
    if (pickList) SetWindowSubclass(parent, PropertyHost, 1, (DWORD_PTR)&picks);
    if (pickList) CheckList_Setup(&picks, pickList, kGlyphs, kTints, 3);
    Check(&fixture, NULL, "pick list: made", pickList != NULL);
    if (pickList) {
        WCHAR text[32] = L"";
        int first = RowOfParam(pickList, CHECK_VALUE_BASE + 0), second = RowOfParam(pickList, CHECK_VALUE_BASE + 1);
        ListView_GetItemText(pickList, first, 3, text, ARRAYSIZE(text));
        Check(&fixture, NULL, "pick list: a column per version, the one kept checked, a missing one a dash",
              first == 0 && second == 1 && Header_GetItemCount(ListView_GetHeader(pickList)) == 4 && CellImage(pickList, first, 1) == 1 &&
              CellImage(pickList, first, 2) == 2 && CellImage(pickList, first, 3) == I_IMAGENONE && wcscmp(text, L"\x2014") == 0 &&
              !(ListView_GetExtendedListViewStyle(pickList) & LVS_EX_CHECKBOXES));
        changes = g_valueChanges;
        ClickCell(&picks, parent, first, 2);
        Check(&fixture, NULL, "pick list: a click keeps another version and tells the owner",
              versions[0].pick == 1 && CellImage(pickList, first, 2) == 1 && CellImage(pickList, first, 1) == 2 && g_valueChanges == changes + 1);
        ClickCell(&picks, parent, first, 3);
        Check(&fixture, NULL, "pick list: a version it lacks cannot be kept", versions[0].pick == 1);
        ListView_SetItemState(pickList, second, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListKey(&picks, parent, VK_LEFT);
        Check(&fixture, NULL, "pick list: Left keeps the version before, past those it lacks", versions[1].pick == 0);
        ListKey(&picks, parent, VK_RIGHT);
        Check(&fixture, NULL, "pick list: Right the one after", versions[1].pick == 2);
        Check(&fixture, NULL, "pick list: the labels keep two fifths of the width at least",
              ListView_GetColumnWidth(pickList, 0) * 5 >= 500 * 2);
    }
    if (parent) {
        RemoveWindowSubclass(parent, PropertyHost, 1);
        DestroyWindow(parent);
    }
    Util_SetSettingList(L"PropertyTestOpened", NULL, 0);
}

/* The dialogs other than the manager window, in every language and scale. */
static void CheckDialogs(void)
{
    static const int kResources[] = { IDD_PROFILE, IDD_PROFILE_NEW, IDD_TITLE, IDD_MESSAGE, IDD_UNINSTALL, IDD_SYNC, IDD_LINK, IDD_BACKUP, IDD_RESTORE, IDD_PURGE,
                                      IDD_SYNC_ITEMS, IDD_CONFLICTS, IDD_SYNC_SETUP, IDD_PREVIEW, IDD_SETTINGS, IDD_ABOUT, IDD_PASSWORD,
                                      IDD_SIGNIN, IDD_TEXTVIEW, IDD_SESSIONTOOLS, IDD_SESSIONFILTER, IDD_SESSIONSEARCH, IDD_VAULT_PICK, IDD_PURGE_OPEN,
                                      IDD_TRANSFER, IDD_SHIELD, IDD_SEARXNG, IDD_UPDATE };
    size_t resource, scale;
    int language;
    for (language = 0; language < Localize_LanguageCount(); language++)
        for (resource = 0; resource < ARRAYSIZE(kResources); resource++) for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
            LayoutFixture fixture;
            HWND dialog;
            if (!g_scaleFits[scale]) continue;
            ZeroMemory(&fixture, sizeof fixture);
            fixture.resource = kResources[resource];
            fixture.language = language;
            fixture.fontScale = kFontScales[scale];
            dialog = CreateFixture(&fixture);
            Check(&fixture, NULL, "resource fixture dialog created", dialog != NULL);
            if (dialog) CheckGeometry(dialog, &fixture);
            DestroyFixture(dialog, &fixture);
        }
}

/* A second fit of a fitted window changes nothing: its frame stays. */
static void CheckSecondFitKeepsFrame(HWND dialog, const LayoutFixture *fixture)
{
    RECT frame, after;
    GetWindowRect(dialog, &frame);
    Theme_FitDialog(dialog);
    GetWindowRect(dialog, &after);
    Check(fixture, NULL, "a second fit keeps the native frame", EqualRect(&frame, &after));
}

typedef struct MessageCheck { LayoutFixture fixture; BOOL single; HWND seen; int checked; } MessageCheck;

/* The message box's owner: each box, once it waits for the user, is checked
 * and closed. */
static LRESULT CALLBACK CheckWaitingMessage(HWND owner, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    MessageCheck *check = (MessageCheck *)reference;
    (void)id;
    if (message == WM_ENTERIDLE && wp == MSGF_DIALOGBOX && (HWND)lp != check->seen) {
        HWND box = (HWND)lp, ok = GetDlgItem(box, IDOK), cancel = GetDlgItem(box, IDCANCEL);
        RECT okPlace = RelativeRect(box, ok), cancelPlace = RelativeRect(box, cancel);
        check->seen = box;
        check->checked++;
        CheckGeometry(box, &check->fixture);
        if (check->single)
            Check(&check->fixture, ok, "a one-button message box shows its button alone, in the second one's place",
                  IsWindowVisible(ok) && !IsWindowVisible(cancel) && okPlace.right == cancelPlace.right && okPlace.top == cancelPlace.top);
        else Check(&check->fixture, cancel, "a two-button message box shows both buttons", IsWindowVisible(ok) && IsWindowVisible(cancel));
        Check(&check->fixture, GetDlgItem(box, IDC_M_ICON), "a message box shows its icon",
              SendDlgItemMessageW(box, IDC_M_ICON, STM_GETICON, 0, 0) != 0 && IsWindowVisible(GetDlgItem(box, IDC_M_ICON)));
        PostMessageW(box, WM_COMMAND, IDOK, 0);
    }
    return DefSubclassProc(owner, message, wp, lp);
}

/* The themed message box as the program shows it (Ui_Message, at the
 * monitor's scale), with one button and with two, in every language. */
static void CheckMessageBoxes(void)
{
    HWND owner;
    int language, variant;
    if (!NativeScaleFits("the message boxes")) return;
    owner = CreateWindowExW(WS_EX_TOOLWINDOW, WC_STATICW, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_hInst, NULL);
    if (!owner) {
        printf("  FAIL  message box owner created\n");
        g_checks++;
        g_failures++;
        return;
    }
    for (language = 0; language < Localize_LanguageCount(); language++) for (variant = 0; variant < 2; variant++) {
        MessageCheck check;
        WCHAR text[1024];
        ZeroMemory(&check, sizeof check);
        check.fixture.resource = IDD_MESSAGE;
        check.fixture.language = language;
        check.fixture.fontScale = kFontScales[0];
        check.single = variant == 0;
        Localize_SetLanguage(language, FALSE);
        StringCchPrintfW(text, ARRAYSIZE(text),
            TR(L"Copy \x201C%s\x201D to \x201C%s\x201D?\n\nThe copy goes on separately."), L"A private conversation", L"Personal");
        SetWindowSubclass(owner, CheckWaitingMessage, MESSAGE_OWNER_SUBCLASS, (DWORD_PTR)&check);
        Ui_Message(owner, check.single ? MB_OK | MB_ICONINFORMATION : MB_YESNO | MB_ICONQUESTION, L"%s", text);
        RemoveWindowSubclass(owner, CheckWaitingMessage, MESSAGE_OWNER_SUBCLASS);
        Check(&check.fixture, NULL, "the message box waited for the user once, and was checked", check.checked == 1);
    }
    DestroyWindow(owner);
}

#define SETTINGS_ANSWERS_MAX 16
#define SETTINGS_ANSWER_CCH  1024

typedef struct SettingsCheck { LayoutFixture fixture; int chosen; HWND seen; int checked; } SettingsCheck;

static int g_settingsApplied, g_settingsLanguage;

/* `key` pressed alone: the thread's Ctrl, Shift and Alt are up meanwhile,
 * whatever the real keyboard holds. */
static void SendPlainKey(HWND control, WPARAM key)
{
    static const int kModifiers[] = { VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_MENU, VK_LMENU, VK_RMENU };
    BYTE state[256], plain[256];
    size_t i;
    GetKeyboardState(state);
    memcpy(plain, state, sizeof plain);
    for (i = 0; i < ARRAYSIZE(kModifiers); i++) plain[kModifiers[i]] &= (BYTE)~0x80;
    SetKeyboardState(plain);
    SendMessageW(control, WM_KEYDOWN, key, 0x00500001);
    SendMessageW(control, WM_KEYUP, key, 0xC0500001);
    SetKeyboardState(state);
    PumpMessages();
}

/* What gui.c's DoLanguage does for Settings, for this process only. */
static void ApplySettingsLanguage(int language)
{
    g_settingsApplied++;
    g_settingsLanguage = language;
    Localize_SetLanguage(language, FALSE);
}

/* The page's first setting is the language, showing `name`. */
static BOOL PageLanguageIs(HWND page, const WCHAR *name)
{
    WCHAR title[128], shown[128];
    return page && Settings_PageRow(page, 0, title, ARRAYSIZE(title), shown, ARRAYSIZE(shown)) && wcscmp(title, TR(L"Language")) == 0 &&
           wcscmp(shown, name) == 0;
}

/* Another language chosen in the Settings dialog, its row stepped with the
 * keyboard, and applied: Apply waits for the change, gives the language to
 * the program, and waits again; the dialog speaks the new language, in its
 * script's face, still laid out whole. */
static void CheckSettingsApply(HWND dialog, SettingsCheck *check)
{
    HWND apply = GetDlgItem(dialog, IDC_G_APPLY), page = GetWindow(GetDlgItem(dialog, IDC_G_PAGE), GW_CHILD);   /* in its view */
    int target = Localize_EffectiveLanguage() == 0 ? 1 : 0, steps;
    WCHAR caption[64], title[128] = L"", name[128];
    FontState font;
    StringCchCopyW(name, ARRAYSIZE(name), Localize_LanguageName(target));
    Check(&check->fixture, apply, "Apply waits for a change", !IsWindowEnabled(apply));
    if (page) {
        SetFocus(page);
        SendPlainKey(page, VK_HOME);
        for (steps = 0; steps <= Localize_LanguageCount() && !PageLanguageIs(page, name); steps++) SendPlainKey(page, VK_RIGHT);
        for (steps = 0; steps <= Localize_LanguageCount() && !PageLanguageIs(page, name); steps++) SendPlainKey(page, VK_LEFT);
    }
    Check(&check->fixture, page, "the language's row takes another language from the keyboard, and Apply waits no more",
          PageLanguageIs(page, name) && IsWindowEnabled(apply));
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDC_G_APPLY, BN_CLICKED), (LPARAM)apply);
    PumpMessages();
    check->fixture.language = target;
    GetWindowTextW(dialog, caption, ARRAYSIZE(caption));
    if (page) Settings_PageRow(page, 0, title, ARRAYSIZE(title), NULL, 0);
    font = ReadFont(dialog, (HFONT)SendMessageW(dialog, WM_GETFONT, 0, 0));
    Check(&check->fixture, apply, "Apply gives the language chosen to the program", g_settingsApplied == 1 && g_settingsLanguage == target);
    Check(&check->fixture, NULL, "the settings dialog follows the language applied: caption, the page's rows",
          wcscmp(caption, TR(L"Settings")) == 0 && wcscmp(title, TR(L"Language")) == 0);
    Check(&check->fixture, NULL, "the settings dialog's face covers the language applied",
          font.available && wcscmp(font.logical.lfFaceName, Localize_FontFace()) == 0);
    Check(&check->fixture, page, "the language's row shows the language applied", PageLanguageIs(page, name));
    Check(&check->fixture, apply, "Apply waits again for a change once applied", !IsWindowEnabled(apply));
    CheckGeometry(dialog, &check->fixture);
}

/* The settings' kinds on the left, the page of the one chosen on the right:
 * General's first, the language its first setting; another kind chosen,
 * that kind's page; a search, the settings that match, from every kind. */
static void CheckSettingsList(HWND dialog, SettingsCheck *check)
{
    HWND kinds = GetDlgItem(dialog, IDC_G_LIST), page = GetWindow(GetDlgItem(dialog, IDC_G_PAGE), GW_CHILD);   /* the page in its view */
    WCHAR title[128] = L"";
    int matches = 0;
    if (page) Settings_PageRow(page, 0, title, ARRAYSIZE(title), NULL, 0);
    Check(&check->fixture, kinds, "Settings show their kinds, and the page General's settings, the language first",
          kinds && page && wcscmp(title, TR(L"Language")) == 0);
    if (kinds) {
        SendPlainKey(kinds, VK_DOWN);   /* New profiles */
        SendPlainKey(kinds, VK_DOWN);   /* Claude Code */
    }
    title[0] = 0;
    if (page) Settings_PageRow(page, 0, title, ARRAYSIZE(title), NULL, 0);
    Check(&check->fixture, page, "another kind chosen shows its page", wcscmp(title, TR(L"Days to keep conversations")) == 0);
    SetDlgItemTextW(dialog, IDC_G_SEARCH, L"Cowork");
    PumpMessages();
    while (page && Settings_PageRow(page, matches, NULL, 0, NULL, 0)) matches++;
    Check(&check->fixture, page, "a search shows the settings that match, from every kind", matches > 1 && matches < 20);
    SetDlgItemTextW(dialog, IDC_G_SEARCH, L"");
    if (kinds) SendPlainKey(kinds, VK_HOME);
    PumpMessages();
    CheckGeometry(dialog, &check->fixture);
}

/* The Settings dialog's owner: once the dialog waits for the user, its
 * language box is read; another language is chosen and applied, which the
 * dialog follows, Apply waiting for a change before and after; then Cancel
 * closes it. The vault's days stay as they are: nothing is written. */
static LRESULT CALLBACK CheckWaitingSettings(HWND owner, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    SettingsCheck *check = (SettingsCheck *)reference;
    (void)id;
    if (message == WM_ENTERIDLE && wp == MSGF_DIALOGBOX && (HWND)lp != check->seen) {
        HWND dialog = (HWND)lp, page = GetWindow(GetDlgItem(dialog, IDC_G_PAGE), GW_CHILD);   /* in its view */
        check->seen = dialog;
        check->checked++;
        CheckGeometry(dialog, &check->fixture);
        Check(&check->fixture, page, "the language's row shows the language in use, Windows' as its own choice",
              PageLanguageIs(page, Localize_LanguageName(check->chosen)));
        CheckSettingsList(dialog, check);
        CheckSettingsApply(dialog, check);
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    }
    return DefSubclassProc(owner, message, wp, lp);
}

/* The About dialog's owner: once the dialog waits for the user, it shows
 * this version, the changelog's newest, and what it changed; help's
 * questions, walked with the keyboard from their heading, each show an
 * answer of their own; the changelog's heading shows every version; then
 * Close closes it. */
static LRESULT CALLBACK CheckWaitingAbout(HWND owner, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    static WCHAR answers[SETTINGS_ANSWERS_MAX][SETTINGS_ANSWER_CCH];
    SettingsCheck *check = (SettingsCheck *)reference;
    (void)id;
    if (message == WM_ENTERIDLE && wp == MSGF_DIALOGBOX && (HWND)lp != check->seen) {
        HWND dialog = (HWND)lp, list = GetWindow(GetDlgItem(dialog, IDC_W_TOPICS), GW_CHILD);
        WCHAR chosen[CHANGELOG_NAME_CCH] = L"", version[64] = L"", expected[64];
        int row, other, questions = 0;
        BOOL own = list != NULL;
        check->seen = dialog;
        check->checked++;
        CheckGeometry(dialog, &check->fixture);
        if (list) ListView_GetItemText(list, ListView_GetNextItem(list, -1, LVNI_FOCUSED), 0, chosen, ARRAYSIZE(chosen));
        GetDlgItemTextW(dialog, IDC_W_VERSION, version, ARRAYSIZE(version));
        StringCchPrintfW(expected, ARRAYSIZE(expected), TR(L"Version %s"), APP_VERSION_DATE_W);
        GetDlgItemTextW(dialog, IDC_W_TEXT, answers[0], SETTINGS_ANSWER_CCH);
        Check(&check->fixture, list, "About shows this version, and opens on it in the changelog with what it changed",
              wcscmp(version, expected) == 0 && wcscmp(chosen, L"v" APP_VERSION_DATE_W) == 0 &&
              wcsncmp(answers[0], L"v" APP_VERSION_DATE_W, ARRAYSIZE(L"v" APP_VERSION_DATE_W) - 1) == 0 &&
              (answers[0][ARRAYSIZE(L"v" APP_VERSION_DATE_W) - 1] == L'\r' || answers[0][ARRAYSIZE(L"v" APP_VERSION_DATE_W) - 1] == L'\n') &&
              answers[0][ARRAYSIZE(L"v" APP_VERSION_DATE_W)]);
        if (list) SendPlainKey(list, VK_HOME);
        for (row = 1; own && row < ListView_GetItemCount(list) && Theme_RowParam(list, row) >= 0 && questions < SETTINGS_ANSWERS_MAX; row++, questions++) {
            SendPlainKey(list, VK_DOWN);
            GetDlgItemTextW(dialog, IDC_W_TEXT, answers[questions], SETTINGS_ANSWER_CCH);
            own = ListView_GetNextItem(list, -1, LVNI_SELECTED) == row && answers[questions][0];
            for (other = 0; own && other < questions; other++) own = wcscmp(answers[questions], answers[other]) != 0;
        }
        Check(&check->fixture, list, "each help question, reached with the keyboard, shows its own answer", own && questions > 1);
        for (row = 0; list && row < ListView_GetItemCount(list); row++) {
            WCHAR name[64] = L"";
            ListView_GetItemText(list, row, 0, name, ARRAYSIZE(name));
            if (wcscmp(name, TR(L"Changelog")) == 0) break;
        }
        if (list && row < ListView_GetItemCount(list)) {
            int length;
            WCHAR *whole;
            Theme_FocusRow(list, row);
            PumpMessages();
            length = GetWindowTextLengthW(GetDlgItem(dialog, IDC_W_TEXT));
            whole = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((size_t)length + 1) * sizeof(WCHAR));
            if (whole) GetDlgItemTextW(dialog, IDC_W_TEXT, whole, length + 1);
            Check(&check->fixture, list, "the changelog's heading shows every version, the newest first",
                  whole && wcsncmp(whole, L"v" APP_VERSION_DATE_W, ARRAYSIZE(L"v" APP_VERSION_DATE_W) - 1) == 0 &&
                  (wcsstr(whole, L"\rv20") != NULL || wcsstr(whole, L"\nv20") != NULL));
            if (whole) HeapFree(GetProcessHeap(), 0, whole);
        } else {
            Check(&check->fixture, list, "the changelog's heading is listed", FALSE);
        }
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    }
    return DefSubclassProc(owner, message, wp, lp);
}

/* The Settings dialog as the program shows it (Settings_Show, at the
 * monitor's scale), following Windows' language and in each of the program's. */
static void CheckSettingsDialog(void)
{
    HWND owner;
    int language;
    if (!NativeScaleFits("the settings dialog")) return;
    owner = CreateWindowExW(WS_EX_TOOLWINDOW, WC_STATICW, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_hInst, NULL);
    if (!owner) {
        printf("  FAIL  settings dialog owner created\n");
        g_checks++;
        g_failures++;
        return;
    }
    for (language = -1; language < Localize_LanguageCount(); language++) {
        SettingsCheck check;
        ZeroMemory(&check, sizeof check);
        Localize_SetLanguage(language, FALSE);
        check.fixture.resource = IDD_SETTINGS;
        check.fixture.language = Localize_EffectiveLanguage();
        check.fixture.fontScale = kFontScales[0];
        check.chosen = language;
        g_settingsApplied = 0;
        SetWindowSubclass(owner, CheckWaitingSettings, SETTINGS_OWNER_SUBCLASS, (DWORD_PTR)&check);
        Settings_Show(owner, ApplySettingsLanguage, NULL);
        RemoveWindowSubclass(owner, CheckWaitingSettings, SETTINGS_OWNER_SUBCLASS);
        Check(&check.fixture, NULL, "the settings dialog waited for the user once, and was checked", check.checked == 1);
        Check(&check.fixture, NULL, "Cancel after Apply gives the program nothing more", g_settingsApplied == 1);
    }
    DestroyWindow(owner);
}

/* The About dialog as the program shows it (About_Show, at the monitor's
 * scale), following Windows' language and in each of the program's. */
static void CheckAboutDialog(void)
{
    HWND owner;
    int language;
    if (!NativeScaleFits("the About dialog")) return;
    owner = CreateWindowExW(WS_EX_TOOLWINDOW, WC_STATICW, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, g_hInst, NULL);
    if (!owner) {
        printf("  FAIL  About dialog owner created\n");
        g_checks++;
        g_failures++;
        return;
    }
    for (language = -1; language < Localize_LanguageCount(); language++) {
        SettingsCheck check;
        ZeroMemory(&check, sizeof check);
        Localize_SetLanguage(language, FALSE);
        check.fixture.resource = IDD_ABOUT;
        check.fixture.language = Localize_EffectiveLanguage();
        check.fixture.fontScale = kFontScales[0];
        SetWindowSubclass(owner, CheckWaitingAbout, SETTINGS_OWNER_SUBCLASS, (DWORD_PTR)&check);
        About_Show(owner);
        RemoveWindowSubclass(owner, CheckWaitingAbout, SETTINGS_OWNER_SUBCLASS);
        Check(&check.fixture, NULL, "the About dialog waited for the user once, and was checked", check.checked == 1);
    }
    DestroyWindow(owner);
}

/* The manager window in every language, view and scale, through every link
 * status and version: one client size, every control in place. Its table,
 * note and details are checked once per window. */
static void CheckMainStates(void)
{
    size_t scale;
    int language, view, links, version;
    for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) {
        RECT common = { 0, 0, 0, 0 };
        int commonRole = -1;
        if (!g_scaleFits[scale]) continue;
        for (language = 0; language < Localize_LanguageCount(); language++) for (view = 0; view < 2; view++) {
            LayoutFixture fixture = MainFixture(language, kFontScales[scale], view != 0);
            HWND dialog;
            RECT client;
            fixture.links = LINKS_NOT_SET_UP;
            dialog = CreateFixture(&fixture);
            Check(&fixture, NULL, "private main fixture created", dialog != NULL);
            if (!dialog) {
                DestroyFixture(dialog, &fixture);
                continue;
            }
            GetClientRect(dialog, &client);
            if (IsRectEmpty(&common)) common = client;
            Check(&fixture, NULL, "main size is identical across all languages and both views", EqualRect(&common, &client));
            if (!fixture.sessions) {
                if (commonRole < 0) commonRole = ListView_GetColumnWidth(fixture.table, 1);
                Check(&fixture, fixture.table, "default Role column has one width for every language",
                      ListView_GetColumnWidth(fixture.table, 1) == commonRole);
                CheckProfileTable(dialog, &fixture);
                CheckNoteGeometry(dialog, &fixture);
                CheckNativeNote(dialog, &fixture);
                CheckSecondFitKeepsFrame(dialog, &fixture);
            } else {
                CheckHiddenProfileNote(dialog, &fixture);
            }
            for (links = 0; links < LINK_STATUSES; links++) for (version = 0; version < MAIN_VERSIONS; version++) {
                fixture.links = (LinkStatus)links;
                fixture.version = (MainVersion)version;
                MainCaptions(dialog, &fixture);
                Theme_LayoutMain(dialog);
                GetClientRect(dialog, &client);
                Check(&fixture, NULL, "status and version changes keep the main client size", EqualRect(&common, &client));
                CheckGeometry(dialog, &fixture);
            }
            DestroyFixture(dialog, &fixture);
        }
    }
}

/* A note naming profiles short and long (spaced or not), in a fresh window
 * per language and then in one window through every language. */
static void CheckProfileNotes(void)
{
    WCHAR longName[LABEL_CCH], unbroken[LABEL_CCH];
    const WCHAR *names[] = { L"Personal", L"Work", longName, unbroken };
    size_t scale, name, character;
    int language, round;
    LongName(longName, ARRAYSIZE(longName));
    for (character = 0; character < MAX_LABEL; character++) unbroken[character] = L'W';
    unbroken[character] = 0;
    for (scale = 0; scale < ARRAYSIZE(kFontScales); scale++) for (name = 0; name < ARRAYSIZE(names); name++) {
        LayoutFixture fixture;
        HWND dialog;
        if (!g_scaleFits[scale]) continue;
        for (language = 0; language < Localize_LanguageCount(); language++) {
            fixture = MainFixture(language, kFontScales[scale], FALSE);
            fixture.noteName = names[name];
            dialog = CreateFixture(&fixture);
            Check(&fixture, NULL, "fresh-language private profile note fixture created", dialog != NULL);
            if (dialog) {
                CheckNoteGeometry(dialog, &fixture);
                CheckNativeNote(dialog, &fixture);
            }
            DestroyFixture(dialog, &fixture);
        }
        fixture = MainFixture(Language(L"en"), kFontScales[scale], FALSE);
        fixture.noteName = names[name];
        dialog = CreateFixture(&fixture);
        Check(&fixture, NULL, "private profile note fixture created", dialog != NULL);
        if (dialog) for (round = 0; round < 2; round++) for (language = 0; language < Localize_LanguageCount(); language++) {
            TransitionLanguage(dialog, &fixture, language);
            CheckNoteGeometry(dialog, &fixture);
            CheckNativeNote(dialog, &fixture);
        }
        DestroyFixture(dialog, &fixture);
    }
}

/* A column sized as the user sizes it: its header divider dragged with the
 * native header's tracking (the left button held in this thread's keyboard
 * state only) and released at `width`. TRUE when the column has it. */
static BOOL DragDivider(HWND table, int column, int width)
{
    HWND header = ListView_GetHeader(table);
    RECT edge;
    BYTE saved[256], keys[256];
    int distance = width - ListView_GetColumnWidth(table, column), y;
    BOOL ok;
    if (!Header_GetItemRect(header, column, &edge) || !GetKeyboardState(saved)) return FALSE;
    CopyMemory(keys, saved, sizeof keys);
    keys[VK_LBUTTON] = 0x80;
    ok = SetKeyboardState(keys);
    y = (edge.top + edge.bottom) / 2;
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(edge.right - 1, y));
    SendMessageW(header, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(edge.right - 1 + distance, y));
    SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(edge.right - 1 + distance, y));
    if (GetCapture() == header) ReleaseCapture();
    ok = SetKeyboardState(saved) && ok;
    return ok && ListView_GetColumnWidth(table, column) == width;
}

/* Columns the user sized keep their widths through layouts and every
 * language; the sessions folder column follows the room left until it is
 * sized too. */
static void CheckColumnInteractions(HWND dialog, LayoutFixture *fixture)
{
    HWND table = fixture->table;
    int profile = ListView_GetColumnWidth(table, 0), role = ListView_GetColumnWidth(table, 1);
    int data = ListView_GetColumnWidth(table, 2), sessions = ListView_GetColumnWidth(table, 3), language;
    WCHAR name[LABEL_CCH];
    LongName(name, ARRAYSIZE(name));
    ListView_SetItemText(table, 0, 0, name);
    ListView_SetItemState(table, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Gui_LayoutProfileColumns(table);
    Check(fixture, table, "name and selection changes retain the default fixed columns",
          ListView_GetColumnWidth(table, 0) == profile && ListView_GetColumnWidth(table, 1) == role && ListView_GetColumnWidth(table, 2) == data);
    Check(fixture, table, "the profile column's divider dragged by the user sizes it", DragDivider(table, 0, profile + 20));
    Check(fixture, table, "the role column's divider dragged by the user sizes it", DragDivider(table, 1, role + 10));
    Gui_LayoutProfileColumns(table);
    {
        int defaultProfile, defaultRole, dataMinimum, sessionsMinimum;
        Theme_ProfileColumnWidths(table, &defaultProfile, &defaultRole, &dataMinimum, &sessionsMinimum);
        Check(fixture, table, "manual first-column widths survive layout and the sessions column follows the remainder, down to its minimum",
              ListView_GetColumnWidth(table, 0) == profile + 20 && ListView_GetColumnWidth(table, 1) == role + 10 &&
              ListView_GetColumnWidth(table, 2) == data && ListView_GetColumnWidth(table, 3) == max(sessionsMinimum, sessions - 30));
    }
    for (language = 0; language < Localize_LanguageCount(); language++) {
        TransitionLanguage(dialog, fixture, language);
        Check(fixture, table, "manual fixed-column adjustments survive every language change",
              ListView_GetColumnWidth(table, 0) == profile + 20 && ListView_GetColumnWidth(table, 1) == role + 10);
    }
    Check(fixture, table, "the data column's divider dragged by the user sizes it", DragDivider(table, 2, data - 40));
    Gui_LayoutProfileColumns(table);
    Check(fixture, table, "an explicit data-column adjustment is retained", ListView_GetColumnWidth(table, 2) == data - 40);
    Check(fixture, table, "the sessions folder's divider dragged by the user sizes it", DragDivider(table, 3, ListView_GetColumnWidth(table, 3) - 20));
}

/* The window frame of a `width` x `height` client, at the work area's corner. */
static void ResizeMainFixture(HWND dialog, LayoutFixture *fixture, int width, int height)
{
    RECT frame, actual;
    MONITORINFO monitor = { sizeof monitor };
    SetRect(&frame, 0, 0, width, height);
    Check(fixture, NULL, "main resize frame computed from native styles",
          AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongW(dialog, GWL_STYLE), GetMenu(dialog) != NULL,
                                   (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE), GetDpiForWindow(dialog)));
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    SetWindowPos(dialog, NULL, monitor.rcWork.left, monitor.rcWork.top, frame.right - frame.left, frame.bottom - frame.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    GetClientRect(dialog, &actual);
    Check(fixture, NULL, "native resize reaches the requested client size", actual.right == width && actual.bottom == height);
    if (actual.right != width || actual.bottom != height) printf("        requested=%dx%d actual=%ldx%ld\n", width, height, actual.right, actual.bottom);
}

/* A client size of `widthDips` x `heightDips`, at least the minimum and at
 * most what the work area holds. */
static SIZE ClientWithin(HWND dialog, SIZE minimum, int widthDips, int heightDips)
{
    RECT frame = { 0, 0, 0, 0 }, client;
    MONITORINFO monitor = { sizeof monitor };
    int dpi = (int)GetDpiForWindow(dialog);
    SIZE size;
    GetWindowRect(dialog, &frame);
    GetClientRect(dialog, &client);
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    size.cx = min(max(minimum.cx, MulDiv(widthDips, dpi, 96)), monitor.rcWork.right - monitor.rcWork.left - (frame.right - frame.left - client.right));
    size.cy = min(max(minimum.cy, MulDiv(heightDips, dpi, 96)), monitor.rcWork.bottom - monitor.rcWork.top - (frame.bottom - frame.top - client.bottom));
    return size;
}

static void DescribeFrame(const char *what, HWND dialog, const RECT *expected)
{
    RECT actual;
    WINDOWPLACEMENT placement = { sizeof placement };
    MONITORINFO monitor = { sizeof monitor };
    GetWindowRect(dialog, &actual);
    GetWindowPlacement(dialog, &placement);
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    printf("        %s: zoomed=%d hung=%d frame=%ld,%ld,%ld,%ld expected=%ld,%ld,%ld,%ld normal=%ld,%ld,%ld,%ld work=%ld,%ld,%ld,%ld\n", what,
           IsZoomed(dialog), IsHungAppWindow(dialog), actual.left, actual.top, actual.right, actual.bottom,
           expected->left, expected->top, expected->right, expected->bottom, placement.rcNormalPosition.left, placement.rcNormalPosition.top,
           placement.rcNormalPosition.right, placement.rcNormalPosition.bottom, monitor.rcWork.left, monitor.rcWork.top,
           monitor.rcWork.right, monitor.rcWork.bottom);
}

/* The content has a bounded reading width (THEME_MAIN_READING_WIDTH_DIPS, or
 * the minimum when wider): a client up to it is filled within the margins,
 * a wider one keeps that width, centered. It spans from the toolbar's first
 * button to the column's right edge. */
static void CheckReadingWidth(HWND dialog, const LayoutFixture *fixture, SIZE minimum, const char *name)
{
    HWND row = GetDlgItem(dialog, IDC_OPEN);
    RECT client, group = RelativeRect(dialog, row), last = RelativeRect(dialog, GetDlgItem(dialog, IDC_SESSIONS));
    int dpi = (int)GetDpiForWindow(dialog), margin = MulDiv(THEME_MAIN_MARGIN_DIPS, dpi, 96);
    int reading = max(minimum.cx, MulDiv(THEME_MAIN_READING_WIDTH_DIPS, dpi, 96)), left, right;
    BOOL bounded;
    GetClientRect(dialog, &client);
    group.right = last.right;
    left = group.left;
    right = client.right - group.right;
    if (client.right <= reading) bounded = left == margin && right == margin;
    else bounded = group.right - group.left == reading - 2 * margin && abs(left - right) <= 1;
    Check(fixture, row, name, bounded);
    if (!bounded) printf("        client %ld, content %ld..%ld, reading width %d, margin %d\n", client.right, group.left, group.right, reading, margin);
}

/* WM_DPICHANGED's suggested frames, then a native maximize, language changes
 * while maximized and a native restore, on a window of its own. The window
 * is on screen meanwhile: messages are handled between steps (PumpMessages). */
static void CheckMainFrameMessages(void)
{
    static const int kActions[] = { IDC_OPEN, IDC_STOP, IDC_RESTART, IDC_NEW, IDC_EDIT, IDC_DELETE, IDC_DEFAULT, IDC_SYNC, IDC_BACKUP,
                                    IDC_RESTORE_BACKUP, IDC_REPAIR, IDC_SHORTCUTS, IDC_SETTINGS, IDC_ABOUT };
    LayoutFixture fixture = MainFixture(Language(L"en"), 96, FALSE);
    HWND dialog, child;
    RECT saved, requested, actual, client, actionRects[ARRAYSIZE(kActions)];
    SIZE minimum, size;
    MONITORINFO monitor = { sizeof monitor };
    HMENU system;
    int dpi, i;
    if (!NativeScaleFits("the manager window's frame messages")) return;
    dialog = CreateFixture(&fixture);
    Check(&fixture, NULL, "frame message private fixture created", dialog != NULL);
    if (!dialog || !Theme_MainMinimum(dialog, &minimum)) {
        DestroyFixture(dialog, &fixture);
        return;
    }
    size = ClientWithin(dialog, minimum, THEME_MAIN_READING_WIDTH_DIPS, 0);
    ResizeMainFixture(dialog, &fixture, size.cx, size.cy);
    dpi = (int)GetDpiForWindow(dialog);
    system = GetSystemMenu(dialog, FALSE);
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    GetWindowRect(dialog, &saved);
    for (i = 0; i < (int)ARRAYSIZE(kActions); i++) actionRects[i] = RelativeRect(dialog, GetDlgItem(dialog, kActions[i]));
    Check(&fixture, NULL, "native system menu offers Size and Maximize",
          system && GetMenuState(system, SC_SIZE, MF_BYCOMMAND) != (UINT)-1 && GetMenuState(system, SC_MAXIMIZE, MF_BYCOMMAND) != (UINT)-1);
    requested = saved;
    Check(&fixture, NULL, "current-DPI suggested frame message is handled by production geometry",
          Gui_MainWindowGeometry(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&requested));
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "a valid suggested frame preserves its size and position", EqualRect(&requested, &actual));
    SetRect(&requested, monitor.rcWork.right - 40, monitor.rcWork.bottom - 40, monitor.rcWork.right - 38, monitor.rcWork.bottom - 38);
    Check(&fixture, NULL, "undersized suggested frame is handled by production geometry",
          Gui_MainWindowGeometry(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&requested));
    GetClientRect(dialog, &client);
    Check(&fixture, NULL, "suggested frame clamps to the common client minimum and current work area",
          client.right == minimum.cx && client.bottom == minimum.cy && InsideWorkArea(dialog));
    requested = saved;
    Gui_MainWindowGeometry(dialog, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&requested);
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "the chosen normal frame is back before maximizing", EqualRect(&saved, &actual));
    ShowWindow(dialog, SW_MAXIMIZE);
    PumpMessages();
    Check(&fixture, NULL, "native maximize changes the main window state", IsZoomed(dialog));
    GetWindowRect(dialog, &requested);
    GetClientRect(dialog, &client);
    Theme_FitDialog(dialog);
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "fitting preserves the maximized native frame", IsZoomed(dialog) && EqualRect(&actual, &requested));
    CheckReadingWidth(dialog, &fixture, minimum, "maximized content keeps its reading width, centered");
    for (i = 0; i < (int)ARRAYSIZE(kActions); i++) {
        RECT action = RelativeRect(dialog, GetDlgItem(dialog, kActions[i]));
        Check(&fixture, GetDlgItem(dialog, kActions[i]), "maximized actions keep their height and vertical grouping",
              action.top == actionRects[i].top && action.bottom == actionRects[i].bottom);
    }
    for (child = GetWindow(dialog, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT placed;
        if (!(GetWindowLongW(child, GWL_STYLE) & WS_VISIBLE)) continue;
        placed = RelativeRect(dialog, child);
        Check(&fixture, child, "maximized visible control stays inside the client",
              placed.left >= 0 && placed.top >= 0 && placed.right <= client.right && placed.bottom <= client.bottom);
    }
    CheckNativeNote(dialog, &fixture);
    PumpMessages();
    TransitionLanguage(dialog, &fixture, Language(L"zh-CN"));
    PumpMessages();
    TransitionLanguage(dialog, &fixture, Language(L"en"));
    PumpMessages();
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "language round trip preserves maximized state and frame", IsZoomed(dialog) && EqualRect(&actual, &requested));
    ShowWindow(dialog, SW_RESTORE);
    PumpMessages();
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "native restore returns to the chosen normal frame", !IsZoomed(dialog) && EqualRect(&saved, &actual));
    if (IsZoomed(dialog) || !EqualRect(&saved, &actual)) DescribeFrame("after restore", dialog, &saved);
    ShowWindow(dialog, SW_HIDE);
    PumpMessages();
    GetWindowRect(dialog, &actual);
    Check(&fixture, NULL, "hiding keeps the restored frame", EqualRect(&saved, &actual));
    DestroyFixture(dialog, &fixture);
}

static BOOL SaveFixtureFile(const WCHAR *root, const WCHAR *path, const char *text, BOOL replace)
{
    HANDLE file;
    DWORD length = (DWORD)strlen(text), written = 0;
    BOOL ok;
    if (!Core_PathUnder(path, root)) return FALSE;
    file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, replace ? CREATE_ALWAYS : CREATE_NEW,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(file, text, length, &written, NULL) && written == length;
    CloseHandle(file);
    return ok;
}

static BOOL RemoveFixtureTree(const WCHAR *root, const WCHAR *path)
{
    WCHAR pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE files;
    BOOL ok = TRUE;
    if ((!Core_PathEquals(root, path) && !Core_PathUnder(path, root)) ||
        FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", path))) return FALSE;
    files = FindFirstFileW(pattern, &found);
    if (files != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                FAILED(StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\%s", path, found.cFileName))) { ok = FALSE; break; }
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = RemoveFixtureTree(root, child) && ok;
            else ok = DeleteFileW(child) && ok;
        } while (FindNextFileW(files, &found));
        FindClose(files);
    }
    return ok && RemoveDirectoryW(path);
}

/* A private root folder; CLAUDE_CONFIG_DIR (Claude Code's transcripts)
 * points into it until RestoreConfigDir. */
typedef struct PrivateRoot {
    WCHAR path[MAX_PATH], savedConfigDir[MAX_PATH];
    BOOL hadConfigDir;
} PrivateRoot;

static BOOL OpenPrivateRoot(PrivateRoot *root)
{
    WCHAR temp[MAX_PATH];
    DWORD saved;
    ZeroMemory(root, sizeof *root);
    saved = GetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", root->savedConfigDir, ARRAYSIZE(root->savedConfigDir));
    root->hadConfigDir = saved > 0;
    return saved < ARRAYSIZE(root->savedConfigDir) && GetTempPathW(ARRAYSIZE(temp), temp) &&
           GetTempFileNameW(temp, L"cdm", 0, root->path) && DeleteFileW(root->path) && CreateDirectoryW(root->path, NULL);
}

static void ClosePrivateRoot(PrivateRoot *root, const LayoutFixture *fixture)
{
    Check(fixture, NULL, "private transcript environment restored",
          SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", root->hadConfigDir ? root->savedConfigDir : NULL));
    Check(fixture, NULL, "private session folders removed without the Recycle Bin", RemoveFixtureTree(root->path, root->path));
}

/* The real sessions view (sessions.c) with no profile, shown and left in a
 * window of its own: the shared client size and every control in place. */
static void CheckRealSessionsTransition(int language)
{
    LayoutFixture fixture = MainFixture(language, 96, FALSE);
    PrivateRoot root;
    WCHAR projects[MAX_PATH];
    ProfileList profiles;
    ClaudePackage package;
    RECT initial, returned;
    HWND dialog = NULL;
    BOOL ready;
    ZeroMemory(&profiles, sizeof profiles);
    ZeroMemory(&package, sizeof package);
    fixture.realSessions = TRUE;
    ready = OpenPrivateRoot(&root);
    ready = ready && SUCCEEDED(StringCchPrintfW(projects, ARRAYSIZE(projects), L"%s\\projects", root.path)) && CreateDirectoryW(projects, NULL);
    Check(&fixture, NULL, "real sessions transition private transcript root prepared", ready);
    if (ready) {
        SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", root.path);
        dialog = CreateFixture(&fixture);
        Check(&fixture, NULL, "real sessions transition fixture created", dialog != NULL);
    }
    if (dialog) {
        GetClientRect(dialog, &initial);
        SessionsView_SetProfiles(&profiles);
        Gui_ShowSessions(dialog, &package, TRUE, NULL);
        fixture.sessions = TRUE;
        Check(&fixture, fixture.tableViewport, "real Sessions transition hides the profile table's viewport",
              !(GetWindowLongW(fixture.tableViewport, GWL_STYLE) & WS_VISIBLE));
        CheckGeometry(dialog, &fixture);
        Theme_FitDialog(dialog);
        SessionsView_Relayout();
        CheckGeometry(dialog, &fixture);
        GetClientRect(dialog, &returned);
        Check(&fixture, NULL, "real Sessions rendering and fitting retain the shared client size", EqualRect(&initial, &returned));
        Gui_ShowSessions(dialog, &package, FALSE, NULL);
        fixture.sessions = FALSE;
        Theme_FitDialog(dialog);
        LayoutMockNote(dialog, &fixture);
        CheckGeometry(dialog, &fixture);
        GetClientRect(dialog, &returned);
        Check(&fixture, NULL, "real Profiles return preserves the same client size", EqualRect(&initial, &returned));
        Check(&fixture, fixture.tableViewport, "real Profiles return shows the same original viewport",
              (GetWindowLongW(fixture.tableViewport, GWL_STYLE) & WS_VISIBLE) != 0);
        SessionsView_Destroy();
    }
    DestroyFixture(dialog, &fixture);
    if (root.path[0]) ClosePrivateRoot(&root, &fixture);
}

/* The manager window resized natively from its minimum to medium, wide,
 * ultrawide, tall and short-wide clients, in every language and view. */
static void CheckResponsiveMain(void)
{
    static const int kSizes[][2] = { { 0, 0 }, { 1000, 650 }, { 1400, 700 }, { 1900, 850 }, { 1000, 1100 }, { 1200, 0 } };
    int language, view, size;
    SIZE commonMinimum = { 0, 0 };
    if (!NativeScaleFits("the manager window's responsive layout")) return;
    for (language = 0; language < Localize_LanguageCount(); language++) for (view = 0; view < 2; view++) {
        LayoutFixture fixture = MainFixture(language, 96, view != 0);
        HWND dialog = CreateFixture(&fixture);
        SIZE minimum = { 0, 0 };
        RECT initialNote, initialDefault;
        int dpi, role, version;
        Check(&fixture, NULL, "responsive private fixture created", dialog != NULL);
        if (!dialog) {
            DestroyFixture(dialog, &fixture);
            continue;
        }
        Check(&fixture, NULL, "main exposes its intrinsic minimum", Theme_MainMinimum(dialog, &minimum));
        if (!language && !view) commonMinimum = minimum;
        Check(&fixture, NULL, "minimum is independent of language and view", minimum.cx == commonMinimum.cx && minimum.cy == commonMinimum.cy);
        Check(&fixture, NULL, "main native style supports resizing and maximizing",
              (GetWindowLongW(dialog, GWL_STYLE) & (WS_THICKFRAME | WS_MAXIMIZEBOX)) == (WS_THICKFRAME | WS_MAXIMIZEBOX));
        Check(&fixture, NULL, "the central main layout owns DPI geometry", (GetDialogDpiChangeBehavior(dialog) & DDC_DISABLE_ALL) != 0);
        {
            MINMAXINFO tracking = { 0 };
            RECT native = { 0, 0, 0, 0 };
            native.right = minimum.cx;
            native.bottom = minimum.cy;
            AdjustWindowRectExForDpi(&native, (DWORD)GetWindowLongW(dialog, GWL_STYLE), GetMenu(dialog) != NULL,
                                     (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE), GetDpiForWindow(dialog));
            SendMessageW(dialog, WM_GETMINMAXINFO, 0, (LPARAM)&tracking);
            Check(&fixture, NULL, "native tracking enforces the shared client minimum including its frame",
                  tracking.ptMinTrackSize.x == native.right - native.left && tracking.ptMinTrackSize.y == native.bottom - native.top);
        }
        initialNote = RelativeRect(dialog, GetDlgItem(dialog, IDC_NOTE));
        initialDefault = RelativeRect(dialog, GetDlgItem(dialog, IDC_DEFAULT));
        role = ListView_GetColumnWidth(fixture.table, 1);
        dpi = (int)GetDpiForWindow(dialog);
        for (size = 0; size < (int)ARRAYSIZE(kSizes); size++) {
            RECT before, after, table, group, note, setDefault, sessionProfiles, tree, tableClient, search, find, filter, back;
            SIZE client = ClientWithin(dialog, minimum, kSizes[size][0], kSizes[size][1]);

            ResizeMainFixture(dialog, &fixture, client.cx, client.cy);
            GetWindowRect(dialog, &before);
            Theme_FitDialog(dialog);
            LayoutMockNote(dialog, &fixture);
            GetWindowRect(dialog, &after);
            Check(&fixture, NULL, "fitting and note updates retain the user's frame and position", EqualRect(&before, &after));

            table = RelativeRect(dialog, fixture.tableViewport);
            group = RelativeRect(dialog, GetDlgItem(dialog, IDC_OPEN));
            note = RelativeRect(dialog, GetDlgItem(dialog, IDC_NOTE));
            setDefault = RelativeRect(dialog, GetDlgItem(dialog, IDC_DEFAULT));
            sessionProfiles = RelativeRect(dialog, fixture.profilesViewport);
            tree = RelativeRect(dialog, fixture.treeViewport);
            search = RelativeRect(dialog, GetDlgItem(dialog, IDC_S_SEARCH));
            find = RelativeRect(dialog, GetDlgItem(dialog, IDC_S_FIND));
            filter = RelativeRect(dialog, GetDlgItem(dialog, IDC_S_FILTER));
            back = RelativeRect(dialog, GetDlgItem(dialog, IDC_SESSIONS));
            Check(&fixture, fixture.tableViewport, "table starts where the content does", table.left == group.left);
            CheckReadingWidth(dialog, &fixture, minimum, "content fills the client within its margins up to its reading width, then stays centered");
            Check(&fixture, GetDlgItem(dialog, IDC_NOTE), "sidebar prose stays grouped with its actions on tall windows",
                  note.top == initialNote.top && note.bottom - note.top == initialNote.bottom - initialNote.top &&
                  setDefault.top == initialDefault.top && setDefault.bottom - setDefault.top == initialDefault.bottom - initialDefault.top);
            Check(&fixture, fixture.profilesViewport, "session sidebar keeps its intrinsic width",
                  sessionProfiles.right - sessionProfiles.left <= MulDiv(SESSION_PROFILES_MAX_DIPS, dpi, 96));
            if (fixture.sessions) {
                Check(&fixture, fixture.treeViewport, "the session tree takes the width beside the side bar, to the content's edge",
                      tree.left > sessionProfiles.right && tree.right == back.right);
                Check(&fixture, GetDlgItem(dialog, IDC_S_FILTER), "the search box, its options, the filter and the way back share one row above the tree",
                      search.right < find.left && find.right < filter.left && filter.right < back.left && back.bottom <= tree.top &&
                      find.right - find.left == find.bottom - find.top && filter.right - filter.left == filter.bottom - filter.top);
            }
            Check(&fixture, fixture.table, "fixed table columns keep their widths at every window size",
                  ListView_GetColumnWidth(fixture.table, 0) == MulDiv(THEME_PROFILE_COLUMN_DIPS, dpi, 96) && ListView_GetColumnWidth(fixture.table, 1) == role);
            GetClientRect(fixture.table, &tableClient);
            Check(&fixture, fixture.table, "sessions folder column is the exact table remainder",
                  ListView_GetColumnWidth(fixture.table, 0) + ListView_GetColumnWidth(fixture.table, 1) +
                  ListView_GetColumnWidth(fixture.table, 2) + ListView_GetColumnWidth(fixture.table, 3) +
                  ListView_GetColumnWidth(fixture.table, PROFILE_COLUMN_STATE) == tableClient.right);
            CheckGeometry(dialog, &fixture);
            if (!fixture.sessions) {
                CheckNoteGeometry(dialog, &fixture);
                CheckNativeNote(dialog, &fixture);
            }
            for (version = 0; version < MAIN_VERSIONS; version++) {
                fixture.version = (MainVersion)version;
                MainCaptions(dialog, &fixture);
                Theme_FitDialog(dialog);
                CheckText(&fixture, GetDlgItem(dialog, IDC_VERSION));
                GetWindowRect(dialog, &after);
                Check(&fixture, NULL, "version variants retain the resized frame", EqualRect(&before, &after));
            }
            fixture.version = MAIN_VERSION_BUILD;
            MainCaptions(dialog, &fixture);
            Theme_LayoutMain(dialog);
        }
        {
            RECT resized, after;
            int original = fixture.language, other = original == Language(L"zh-CN") ? Language(L"en") : Language(L"zh-CN");
            GetWindowRect(dialog, &resized);
            TransitionLanguage(dialog, &fixture, other);
            TransitionLanguage(dialog, &fixture, original);
            GetWindowRect(dialog, &after);
            Check(&fixture, NULL, "language round trip retains user position and size", EqualRect(&resized, &after));
        }
        if (!language && !view) {
            HWND header = ListView_GetHeader(fixture.table);
            HFONT original = (HFONT)SendMessageW(header, WM_GETFONT, 0, 0), larger;
            LOGFONTW font;
            SIZE altered, returned;
            DWORD gdiBefore, gdiAfter;
            int repeat, widths[2][4];
            GetObjectW(original, sizeof font, &font);
            font.lfHeight *= 2;
            larger = CreateFontIndirectW(&font);
            Check(&fixture, header, "header-only larger test font created", larger != NULL);
            if (larger) {
                Theme_ProfileColumnWidths(fixture.table, &widths[0][0], &widths[0][1], &widths[0][2], &widths[0][3]);
                SendMessageW(header, WM_SETFONT, (WPARAM)larger, FALSE);
                Check(&fixture, header, "header-only font change invalidates the intrinsic budget", Theme_MainMinimum(dialog, &altered));
                Theme_ProfileColumnWidths(fixture.table, &widths[1][0], &widths[1][1], &widths[1][2], &widths[1][3]);
                /* A column is as wide as its title or its widest value, whichever is wider: one of them grows. */
                Check(&fixture, header, "the columns' titles are measured again in the header's new font",
                      (widths[1][1] > widths[0][1] || widths[1][2] > widths[0][2] || widths[1][3] > widths[0][3]) &&
                      altered.cx >= minimum.cx && altered.cy >= minimum.cy);
                SendMessageW(header, WM_SETFONT, (WPARAM)original, FALSE);
                Check(&fixture, header, "header font round trip restores the common minimum",
                      Theme_MainMinimum(dialog, &returned) && returned.cx == minimum.cx && returned.cy == minimum.cy);
                DeleteObject(larger);
            }
            gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            for (repeat = 0; repeat < 32; repeat++) Theme_MainMinimum(dialog, &returned);
            gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
            Check(&fixture, NULL, "repeated minimum queries retain no new GDI resources", gdiBefore == gdiAfter);
        }
        if (!view && language == Language(L"en")) CheckColumnInteractions(dialog, &fixture);
        DestroyFixture(dialog, &fixture);
    }
}

/* ------------------------------------------------- the real sessions view */

#define SESSION_ROWS     80    /* half in each of two folders */
#define STARRED_ROW      7
#define UNKNOWN_ROW      SESSION_ROWS   /* one more session, with no folder recorded */
#define FIXTURE_TIME     1700000000000ULL
#define FIXTURE_MORE_FIRST 5    /* a heading's sessions shown at first, as sessions.c shows them */
#define FIXTURE_MORE_NEXT  10   /* those Show more adds */

typedef struct SessionsFixture {
    LayoutFixture layout;
    PrivateRoot root;
    HWND dialog, tree;
    ProfileList profiles;
    ClaudePackage package;             /* not found: opening a session asks for Claude, never starts it */
    WCHAR entries[MAX_PATH], transcripts[MAX_PATH], folders[3][MAX_PATH];
    int streamWrites;
} SessionsFixture;

static void SessionId(int index, WCHAR *id, size_t cch)
{
    StringCchPrintfW(id, cch, L"00000001-0000-4000-8000-%012d", index);
}

/* An entry of the first profile and its transcript; `folder` NULL: none recorded. */
static BOOL WriteSession(SessionsFixture *sessions, int index, const WCHAR *folder, const WCHAR *title, ULONGLONG lastActivity, BOOL starred)
{
    WCHAR id[SESSION_ID_CCH], path[MAX_PATH];
    char json[4096], quoted[MAX_PATH * 6 + 4], cwd[MAX_PATH * 6 + 16] = "";
    SessionId(index, id, ARRAYSIZE(id));
    if (folder) {
        if (!Core_JsonQuote(folder, quoted, sizeof quoted)) return FALSE;
        StringCchPrintfA(cwd, sizeof cwd, ",\"cwd\":%s", quoted);
    }
    StringCchPrintfA(json, sizeof json,
        "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\"%s,\"title\":\"%ls\",\"lastActivityAt\":%llu%s}",
        id, id, cwd, title, lastActivity, starred ? ",\"isStarred\":true" : "");
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\local_%s.json", sessions->entries, id);
    if (!SaveFixtureFile(sessions->root.path, path, json, FALSE)) return FALSE;
    StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"%ls\"%s,\"type\":\"user\"}\n", id, cwd);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s.jsonl", sessions->transcripts, id);
    return SaveFixtureFile(sessions->root.path, path, json, FALSE);
}

static void RowTitle(int index, WCHAR *title, size_t cch)
{
    StringCchPrintfW(title, cch, L"Private conversation %02d", index);
}

/* Two private profiles: the first lists SESSION_ROWS sessions in two
 * folders, one of them starred, and one with no folder; the second has a
 * config.json but no entries folder yet. Shown in a real manager layout. */
static BOOL OpenSessionsFixture(SessionsFixture *sessions, int language)
{
    WCHAR title[64], path[MAX_PATH], code[MAX_PATH], projects[MAX_PATH];
    LayoutFixture *fixture = &sessions->layout;
    BOOL ready;
    int i, p;
    ZeroMemory(sessions, sizeof *sessions);
    *fixture = MainFixture(language, 96, FALSE);
    fixture->realSessions = TRUE;
    ready = OpenPrivateRoot(&sessions->root);
    Check(fixture, NULL, "populated Sessions private root created", ready);
    if (!ready) return FALSE;
    sessions->profiles.count = 2;
    for (p = 0; p < sessions->profiles.count; p++) {
        Profile *profile = &sessions->profiles.items[p];
        StringCchPrintfW(profile->folder, ARRAYSIZE(profile->folder), L"Fixture-layout-%08lx-%d", GetCurrentProcessId(), p);
        StringCchCopyW(profile->name, ARRAYSIZE(profile->name), p ? L"Second profile" : L"Private profile");
        StringCchPrintfW(profile->dataDir, ARRAYSIZE(profile->dataDir), L"%s\\profile%d", sessions->root.path, p);
        StringCchCopyW(profile->storageDir, ARRAYSIZE(profile->storageDir), profile->dataDir);
        profile->color = p;
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\config.json", profile->storageDir);
        ready = ready && Util_EnsureDir(profile->storageDir) &&
                SaveFixtureFile(sessions->root.path, path, "{\"lastKnownAccountUuid\":\"account-ui\"}", FALSE);
    }
    StringCchCopyW(sessions->profiles.defaultFolder, ARRAYSIZE(sessions->profiles.defaultFolder), sessions->profiles.items[0].folder);
    StringCchPrintfW(sessions->entries, ARRAYSIZE(sessions->entries), L"%s\\claude-code-sessions\\account-ui\\organization-ui",
                     sessions->profiles.items[0].storageDir);
    StringCchPrintfW(code, ARRAYSIZE(code), L"%s\\code", sessions->root.path);
    StringCchPrintfW(projects, ARRAYSIZE(projects), L"%s\\projects", code);
    StringCchPrintfW(sessions->transcripts, ARRAYSIZE(sessions->transcripts), L"%s\\fixture", projects);
    StringCchPrintfW(sessions->folders[0], ARRAYSIZE(sessions->folders[0]), L"%s\\Workspace Alpha", sessions->root.path);
    StringCchPrintfW(sessions->folders[1], ARRAYSIZE(sessions->folders[1]), L"%s\\Workspace Beta", sessions->root.path);
    StringCchPrintfW(sessions->folders[2], ARRAYSIZE(sessions->folders[2]), L"%s\\Workspace Gamma", sessions->root.path);
    ready = ready && Util_EnsureDir(sessions->entries) && Util_EnsureDir(sessions->transcripts);
    for (i = 0; i < 3; i++) ready = ready && Util_EnsureDir(sessions->folders[i]);
    /* Newer first: the first folder above the second, rows in index order. */
    for (i = 0; ready && i <= UNKNOWN_ROW; i++) {
        RowTitle(i, title, ARRAYSIZE(title));
        ready = WriteSession(sessions, i, i == UNKNOWN_ROW ? NULL : sessions->folders[i < SESSION_ROWS / 2 ? 0 : 1], title,
                             FIXTURE_TIME - (ULONGLONG)i * 1000, i == STARRED_ROW);
    }
    Check(fixture, NULL, "private profiles, entries and transcripts prepared", ready);
    if (!ready) return FALSE;
    SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", code);
    {
        SessionSet set = { 0 };
        Check(fixture, NULL, "private fixture reader resolves every row and folder",
              SessionStore_LoadProfiles(&set, &sessions->profiles) && set.rowCount == SESSION_ROWS + 1 && set.groupCount == 3);
        SessionStore_Free(&set);
    }
    sessions->dialog = CreateFixture(fixture);
    Check(fixture, NULL, "populated Sessions native dialog created", sessions->dialog != NULL);
    if (!sessions->dialog) return FALSE;
    sessions->tree = fixture->tree;
    SessionsView_SetProfiles(&sessions->profiles);
    Gui_ShowSessions(sessions->dialog, &sessions->package, TRUE, sessions->profiles.items[0].folder);
    fixture->sessions = TRUE;
    ShowWindow(sessions->dialog, SW_SHOWNOACTIVATE);
    return TRUE;
}

static void CloseSessionsFixture(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    if (sessions->dialog) {
        Gui_ShowSessions(sessions->dialog, &sessions->package, FALSE, NULL);
        fixture->sessions = FALSE;
        SessionsView_Destroy();
        fixture->realSessions = FALSE;
        ShowWindow(sessions->dialog, SW_HIDE);
    }
    DestroyFixture(sessions->dialog, fixture);
    sessions->dialog = NULL;
    if (sessions->root.path[0]) ClosePrivateRoot(&sessions->root, fixture);
}

typedef struct TreeWait { HWND tree; UINT count; const WCHAR *title; } TreeWait;

static HTREEITEM FindTitle(HWND tree, HTREEITEM item, const WCHAR *title)
{
    for (; item; item = TreeView_GetNextSibling(tree, item)) {
        WCHAR text[256];
        TVITEMW query = { 0 };
        HTREEITEM found;
        query.mask = TVIF_TEXT;
        query.hItem = item;
        query.pszText = text;
        query.cchTextMax = ARRAYSIZE(text);
        if (TreeView_GetItem(tree, &query) && wcscmp(text, title) == 0) return item;
        if ((found = FindTitle(tree, TreeView_GetChild(tree, item), title)) != NULL) return found;
    }
    return NULL;
}

static BOOL TreeShows(const void *context)
{
    const TreeWait *wait = (const TreeWait *)context;
    if (wait->title) return FindTitle(wait->tree, TreeView_GetRoot(wait->tree), wait->title) != NULL;
    return TreeView_GetCount(wait->tree) >= wait->count;
}

/* The tree once a snapshot showing `title` is in it. */
static BOOL AwaitTitle(SessionsFixture *sessions, const WCHAR *title)
{
    TreeWait wait;
    wait.tree = sessions->tree;
    wait.count = 0;
    wait.title = title;
    return PumpUntil(TreeShows, &wait, SNAPSHOT_TIMEOUT_MS);
}

/* Every snapshot the fixture's writes so far asked for has been read: a
 * session written now (`title`, the `marker`th, in the first folder) shows in
 * the tree, read after all of them. A read still to come reads the same
 * folders: the watcher has nothing new to watch, and asks for nothing. */
static BOOL AwaitWritesRead(SessionsFixture *sessions, int marker, const WCHAR *title)
{
    return WriteSession(sessions, UNKNOWN_ROW + 10 + marker, sessions->folders[0], title, FIXTURE_TIME - 1000000 - (ULONGLONG)marker * 1000, FALSE) &&
           AwaitTitle(sessions, title);
}

typedef struct CountWait { const int *counter; int above; } CountWait;

static BOOL CountAbove(const void *context)
{
    const CountWait *wait = (const CountWait *)context;
    return *wait->counter > wait->above;
}

static BOOL AwaitReloadRequest(const LayoutFixture *fixture, int seen, DWORD timeoutMs)
{
    CountWait wait;
    wait.counter = &fixture->reloadRequests;
    wait.above = seen;
    return PumpUntil(CountAbove, &wait, timeoutMs);
}

static HTREEITEM NthChild(HWND tree, HTREEITEM parent, int n)
{
    HTREEITEM item = parent ? TreeView_GetChild(tree, parent) : TreeView_GetRoot(tree);
    while (item && n-- > 0) item = TreeView_GetNextSibling(tree, item);
    return item;
}

static BOOL Folded(HWND tree, HTREEITEM folder)
{
    return folder && !(TreeView_GetItemState(tree, folder, TVIS_EXPANDED) & TVIS_EXPANDED);
}

/* Folded as the user folds it, with the keyboard: the tree notifies. */
static void FoldFolder(HWND tree, HTREEITEM folder)
{
    TreeView_SelectItem(tree, folder);
    SendMessageW(tree, WM_KEYDOWN, VK_LEFT, 0);
    SendMessageW(tree, WM_KEYUP, VK_LEFT, 0);
}

/* Opened as the user opens it, with the keyboard: the tree notifies, and the view keeps it open. */
static void OpenFolder(HWND tree, HTREEITEM folder)
{
    TreeView_SelectItem(tree, folder);
    SendMessageW(tree, WM_KEYDOWN, VK_RIGHT, 0);
    SendMessageW(tree, WM_KEYUP, VK_RIGHT, 0);
}

static BOOL ItemTitle(HWND tree, HTREEITEM item, WCHAR *text, int cch)
{
    TVITEMW query = { 0 };
    text[0] = 0;
    query.mask = TVIF_TEXT;
    query.hItem = item;
    query.pszText = text;
    query.cchTextMax = cch;
    return item && TreeView_GetItem(tree, &query);
}

/* The roots of the fixture's tree: Pinned (its node value), the two
 * folders by name, and the sessions with no folder (named in the current
 * language). */
typedef struct FixtureRoots { HTREEITEM starred, alpha, beta, unknown; } FixtureRoots;

static FixtureRoots ReadRoots(HWND tree)
{
    FixtureRoots roots;
    HTREEITEM item;
    ZeroMemory(&roots, sizeof roots);
    for (item = TreeView_GetRoot(tree); item; item = TreeView_GetNextSibling(tree, item)) {
        WCHAR text[256];
        TVITEMW query = { 0 };
        query.mask = TVIF_TEXT | TVIF_PARAM;
        query.hItem = item;
        query.pszText = text;
        query.cchTextMax = ARRAYSIZE(text);
        if (!TreeView_GetItem(tree, &query)) continue;
        if (query.lParam == -1) roots.starred = item;
        else if (wcscmp(text, L"Workspace Alpha") == 0) roots.alpha = item;
        else if (wcscmp(text, L"Workspace Beta") == 0) roots.beta = item;
        else if (wcscmp(text, TR(L"Unknown folder")) == 0) roots.unknown = item;
    }
    return roots;
}

/* A heading's Show more row: its node is sessions.c's NODE_MORE range. */
static BOOL IsMoreRow(HWND tree, HTREEITEM item)
{
    TVITEMW query = { 0 };
    query.mask = TVIF_PARAM;
    query.hItem = item;
    return item && TreeView_GetItem(tree, &query) && query.lParam <= -0x40000000 && query.lParam > -0x50000000;
}

static int ChildCount(HWND tree, HTREEITEM parent, HTREEITEM *last)
{
    HTREEITEM item;
    int count = 0;
    *last = NULL;
    for (item = TreeView_GetChild(tree, parent); item; item = TreeView_GetNextSibling(tree, item)) {
        *last = item;
        count++;
    }
    return count;
}

/* Folder `which` (0 Workspace Alpha, 1 Workspace Beta), read again: a fill makes new rows. */
static HTREEITEM FixtureFolder(HWND tree, int which)
{
    FixtureRoots roots = ReadRoots(tree);
    return which ? roots.beta : roots.alpha;
}

/* A click on Show more row `more`, as the user's; FALSE when it is none. */
static BOOL ClickMoreRow(HWND tree, HTREEITEM more)
{
    RECT rc;
    if (!IsMoreRow(tree, more) || !TreeView_GetItemRect(tree, more, &rc, FALSE)) return FALSE;
    SendMessageW(tree, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2));
    SendMessageW(tree, WM_LBUTTONUP, 0, MAKELPARAM((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2));
    PumpMessages();
    return TRUE;
}

/* A click on heading `heading`'s Show more row; FALSE when it has none. */
static BOOL ClickShowMore(HWND tree, HTREEITEM heading)
{
    HTREEITEM last;
    ChildCount(tree, heading, &last);
    return ClickMoreRow(tree, last);
}

/* The first Show more row from `item` on, its rows' rows included; NULL for none. */
static HTREEITEM FindMoreRow(HWND tree, HTREEITEM item)
{
    for (; item; item = TreeView_GetNextSibling(tree, item)) {
        HTREEITEM found;
        if (IsMoreRow(tree, item)) return item;
        if ((found = FindMoreRow(tree, TreeView_GetChild(tree, item))) != NULL) return found;
    }
    return NULL;
}

/* Each folder's sessions as Claude's sidebar shows a group's: five at
 * first and Show more below them, ten more at a click, then Show all, which
 * shows the rest. A click on Show more, as the user's, chooses nothing. */
static void CheckShowMore(SessionsFixture *sessions, BOOL report)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND tree = sessions->tree;
    WCHAR text[64];
    int which, step;
    for (which = 0; which < 2; which++) {
        BOOL shown[3];
        for (step = 0; step < 3; step++) {
            HTREEITEM last, heading = FixtureFolder(tree, which);
            int count = ChildCount(tree, heading, &last);
            BOOL more = IsMoreRow(tree, last) && ItemTitle(tree, last, text, ARRAYSIZE(text));
            shown[step] = step == 0 ? count == FIXTURE_MORE_FIRST + 1 && more && wcscmp(text, TR(L"Show more")) == 0
                        : step == 1 ? count == FIXTURE_MORE_FIRST + FIXTURE_MORE_NEXT + 1 && more && wcscmp(text, TR(L"Show all")) == 0
                                    : count == SESSION_ROWS / 2 && !more;
            if (step < 2) ClickShowMore(tree, heading);
        }
        if (report) {
            Check(fixture, tree, "a folder shows its first five sessions and Show more, as Claude's sidebar does", shown[0]);
            Check(fixture, tree, "Show more shows ten more, and then says Show all", shown[1]);
            Check(fixture, tree, "Show all shows the rest, and goes", shown[2]);
        }
    }
    Check(fixture, tree, "every session shows once each folder is shown whole", TreeView_GetCount(tree) == SESSION_ROWS + 1 + 1 + 4);
}

/* Every heading opened and shown whole once the tree shows the sessions,
 * whatever they are grouped and sorted by, as Expand all does: a session
 * written below the others shows too. The pinned row shows first: Pinned
 * holds all its sessions. Expand all is the fold button's while every
 * heading is folded; one already open makes it Collapse all first. */
static BOOL ShowEveryHeadingWhole(SessionsFixture *sessions)
{
    WCHAR title[64];
    int clicks;
    RowTitle(STARRED_ROW, title, ARRAYSIZE(title));
    if (!AwaitTitle(sessions, title)) return FALSE;
    for (clicks = 0; clicks < 2 && FindMoreRow(sessions->tree, TreeView_GetRoot(sessions->tree)); clicks++) {
        SessionsView_Command(MAKEWPARAM(IDC_S_FOLD, BN_CLICKED));
        PumpMessages();
    }
    return FindMoreRow(sessions->tree, TreeView_GetRoot(sessions->tree)) == NULL;
}

/* A posted mouse message sets GetMessagePos to its own coordinates: the
 * tree's double-click notification reads them as the screen point (sessions.c).
 * The double-click itself is sent, in client coordinates. FALSE when this
 * Windows does not report them so. */
static BOOL DoubleClickTree(HWND dialog, LayoutFixture *fixture, HWND tree, POINT point)
{
    POINT screen = point;
    MSG message;
    DWORD position;
    ClientToScreen(tree, &screen);
    PumpMessages();
    fixture->lastPrompt = NULL;   /* a new message box can reuse a closed one's handle */
    PostMessageW(dialog, WM_MOUSEMOVE, 0, MAKELPARAM(screen.x, screen.y));
    if (!PeekMessageW(&message, dialog, WM_MOUSEMOVE, WM_MOUSEMOVE, PM_REMOVE)) return FALSE;
    position = GetMessagePos();
    if ((short)LOWORD(position) != screen.x || (short)HIWORD(position) != screen.y) return FALSE;
    SendMessageW(tree, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    SendMessageW(tree, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
    PumpMessages();
    return TRUE;
}

typedef struct PrivateTreeItem {
    HTREEITEM item;
    LPARAM value;
    UINT expanded;
} PrivateTreeItem;

static void ReadPrivateTree(HWND tree, HTREEITEM item, PrivateTreeItem *items, int *count, int capacity)
{
    for (; item && *count < capacity; item = TreeView_GetNextSibling(tree, item)) {
        TVITEMW value = { 0 };
        value.hItem = item;
        value.mask = TVIF_PARAM | TVIF_STATE;
        value.stateMask = TVIS_EXPANDED;
        TreeView_GetItem(tree, &value);
        items[*count].item = item;
        items[*count].value = value.lParam;
        items[*count].expanded = value.state & TVIS_EXPANDED;
        (*count)++;
        ReadPrivateTree(tree, TreeView_GetChild(tree, item), items, count, capacity);
    }
}

/* Native resizes keep the populated tree: its items, expansion, selection
 * and scroll position. */
static void CheckSessionsResize(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, tree = sessions->tree;
    PrivateTreeItem items[128];
    FixtureRoots roots = ReadRoots(tree);
    HTREEITEM selected;
    SCROLLINFO scroll = { sizeof scroll, SIF_ALL };
    SIZE minimum = { 0, 0 }, sizes[3];
    int count = 0, size, i;
    Theme_MainMinimum(dialog, &minimum);
    sizes[0] = ClientWithin(dialog, minimum, 1000, 650);
    sizes[1] = ClientWithin(dialog, minimum, 1900, 650);
    sizes[2] = minimum;
    FoldFolder(tree, roots.beta);
    selected = NthChild(tree, roots.alpha, 2);
    TreeView_SelectItem(tree, selected);
    PumpMessages();
    ReadPrivateTree(tree, TreeView_GetRoot(tree), items, &count, ARRAYSIZE(items));
    SendMessageW(fixture->treeViewport, WM_VSCROLL, MAKEWPARAM(SB_LINEDOWN, 0), 0);
    GetScrollInfo(fixture->treeViewport, SB_VERT, &scroll);
    Check(fixture, fixture->treeViewport, "private session tree has a genuine nonzero scroll position", scroll.nPos > 0);
    for (size = 0; size < (int)ARRAYSIZE(sizes); size++) {
        SCROLLINFO after = { sizeof after, SIF_ALL };
        ResizeMainFixture(dialog, fixture, sizes[size].cx, sizes[size].cy);
        for (i = 0; i < count; i++) {
            TVITEMW item = { 0 };
            item.hItem = items[i].item;
            item.mask = TVIF_PARAM | TVIF_STATE;
            item.stateMask = TVIS_EXPANDED;
            Check(fixture, tree, "resize retains each native tree item and its expansion",
                  TreeView_GetItem(tree, &item) && item.lParam == items[i].value && (item.state & TVIS_EXPANDED) == items[i].expanded);
        }
        Check(fixture, tree, "resize retains the selected native item", TreeView_GetSelection(tree) == selected);
        GetScrollInfo(fixture->treeViewport, SB_VERT, &after);
        Check(fixture, fixture->treeViewport, "resize retains the native scroll position while content still exceeds the page",
              after.nPos == scroll.nPos);
        CheckGeometry(dialog, fixture);
    }
}

/* A snapshot that changes the tree refills it: folded folders stay folded
 * and the selected row stays selected, a starred row under Starred
 * included; a language change too (folders are remembered by path, not by
 * their translated names). */
static void CheckSessionsRefillKeepsState(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, tree = sessions->tree;
    WCHAR title[64], starred[64], shown[256];
    FixtureRoots roots = ReadRoots(tree);
    HTREEITEM starredItem;
    int original = fixture->language, other = original == Language(L"zh-CN") ? Language(L"en") : Language(L"zh-CN"), pass;
    RowTitle(STARRED_ROW, starred, ARRAYSIZE(starred));
    if (!Folded(tree, roots.beta)) FoldFolder(tree, roots.beta);
    FoldFolder(tree, roots.unknown);
    starredItem = NthChild(tree, roots.starred, 0);
    TreeView_SelectItem(tree, starredItem);
    PumpMessages();
    Check(fixture, tree, "the starred row under Starred is selected", ItemTitle(tree, TreeView_GetSelection(tree), shown, ARRAYSIZE(shown)) &&
          wcscmp(shown, starred) == 0 && TreeView_GetParent(tree, TreeView_GetSelection(tree)) == roots.starred);
    StringCchCopyW(title, ARRAYSIZE(title), L"Refill conversation");
    Check(fixture, NULL, "a newer session written", WriteSession(sessions, UNKNOWN_ROW + 1, sessions->folders[0], title, FIXTURE_TIME + 1000, FALSE));
    Check(fixture, tree, "a snapshot with a new session refills the tree", AwaitTitle(sessions, title));
    for (pass = 0; pass < 3; pass++) {
        const char *after = pass == 0 ? "a refill" : pass == 1 ? "a language change" : "the language's return";
        char name[160];
        if (pass) {
            int language = pass == 1 ? other : original;
            fixture->language = language;
            Localize_SetLanguage(language, FALSE);
            Localize_Window(dialog);
            Theme_Apply(dialog);
            Theme_FitDialog(dialog);
            SessionsView_Relayout();
            PumpMessages();
        }
        roots = ReadRoots(tree);
        StringCchPrintfA(name, ARRAYSIZE(name), "a folded folder stays folded after %s", after);
        Check(fixture, tree, name, Folded(tree, roots.beta));
        StringCchPrintfA(name, ARRAYSIZE(name), "the folder of sessions with no folder stays folded after %s", after);
        Check(fixture, tree, name, Folded(tree, roots.unknown) && TreeView_GetChild(tree, roots.unknown) != NULL);
        StringCchPrintfA(name, ARRAYSIZE(name), "the starred row under Starred stays selected after %s", after);
        Check(fixture, tree, name, ItemTitle(tree, TreeView_GetSelection(tree), shown, ARRAYSIZE(shown)) && wcscmp(shown, starred) == 0 &&
              TreeView_GetParent(tree, TreeView_GetSelection(tree)) == roots.starred);
    }
}

/* The row on top of the view stays there, as far above the view's top,
 * when a snapshot adds rows above it (a newer folder, a newer session). */
static void CheckSessionsRefillKeepsTopRow(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND tree = sessions->tree, viewport = fixture->treeViewport;
    WCHAR topTitle[64], title[64];
    FixtureRoots roots = ReadRoots(tree);
    HTREEITEM top = NthChild(tree, roots.alpha, 20), below;
    RECT row;
    SCROLLINFO scroll = { sizeof scroll, SIF_POS };
    int offset = 3, before;
    if (!ItemTitle(tree, top, topTitle, ARRAYSIZE(topTitle)) || !TreeView_GetItemRect(tree, top, &row, FALSE)) {
        Check(fixture, tree, "a row to keep on top found", FALSE);
        return;
    }
    SendMessageW(viewport, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, row.top + offset), 0);
    below = TreeView_GetNextSibling(tree, TreeView_GetNextSibling(tree, top));
    TreeView_SelectItem(tree, below);   /* in sight: the view has no reason to move */
    PumpMessages();
    GetScrollInfo(viewport, SB_VERT, &scroll);
    before = scroll.nPos;
    Check(fixture, viewport, "the view starts scrolled to the chosen row", before == row.top + offset);
    StringCchCopyW(title, ARRAYSIZE(title), L"Newer folder conversation");
    Check(fixture, NULL, "sessions written above the row on top",
          WriteSession(sessions, UNKNOWN_ROW + 2, sessions->folders[2], title, FIXTURE_TIME + 3000, FALSE) &&
          WriteSession(sessions, UNKNOWN_ROW + 3, sessions->folders[0], L"Newer conversation", FIXTURE_TIME + 2000, FALSE));
    Check(fixture, tree, "a snapshot adds rows above the row on top", AwaitTitle(sessions, title) && AwaitTitle(sessions, L"Newer conversation"));
    PumpMessages();
    top = FindTitle(tree, TreeView_GetRoot(tree), topTitle);
    GetScrollInfo(viewport, SB_VERT, &scroll);
    Check(fixture, viewport, "the row on top stays on top, as far above the view's top",
          top && TreeView_GetItemRect(tree, top, &row, FALSE) && scroll.nPos == row.top + offset);
    if (!top || scroll.nPos != row.top + offset) printf("        view=%d row=%ld offset=%d\n", scroll.nPos, row.top, offset);
}

/* The watcher reads a profile's own folder for its config.json and for its
 * entries folder appearing: Chromium's other writes there ask for nothing. */
static void CheckSessionsWatcher(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    WCHAR path[MAX_PATH];
    int seen, write;
    BOOL written = TRUE;
    Check(fixture, NULL, "every write so far is read before the profile's other files change", AwaitWritesRead(sessions, 0, L"Settled conversation A"));
    seen = fixture->reloadRequests;
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\Preferences", sessions->profiles.items[0].storageDir);
    for (write = 0; write < 3; write++) written = SaveFixtureFile(sessions->root.path, path, write % 2 ? "{}" : "{\"a\":1}", TRUE) && written;
    PumpUntil(NULL, NULL, IGNORED_WRITE_MS);
    Check(fixture, NULL, "other writes in a profile's folder ask for no snapshot", written && fixture->reloadRequests == seen);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\config.json", sessions->profiles.items[0].storageDir);
    written = SaveFixtureFile(sessions->root.path, path, "{\"lastKnownAccountUuid\":\"account-ui\",\"fixture\":1}", TRUE);
    Check(fixture, NULL, "a changed config.json asks for a snapshot", written && AwaitReloadRequest(fixture, seen, SNAPSHOT_TIMEOUT_MS));
    Check(fixture, NULL, "every write so far is read again", AwaitWritesRead(sessions, 1, L"Settled conversation B"));
    seen = fixture->reloadRequests;
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\claude-code-sessions", sessions->profiles.items[1].storageDir);
    Check(fixture, NULL, "a profile's entries folder appearing asks for a snapshot",
          CreateDirectoryW(path, NULL) && AwaitReloadRequest(fixture, seen, SNAPSHOT_TIMEOUT_MS));
}

typedef struct TranscriptStream { SessionsFixture *sessions; WCHAR path[MAX_PATH]; } TranscriptStream;
static TranscriptStream g_stream;

/* One more line in a transcript: what a running Claude Code does. */
static void CALLBACK StreamTranscript(HWND window, UINT message, UINT_PTR id, DWORD time)
{
    HANDLE file;
    DWORD written;
    static const char kLine[] = "{\"type\":\"assistant\"}\n";
    (void)window; (void)message; (void)id; (void)time;
    file = CreateFileW(g_stream.path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    if (WriteFile(file, kLine, (DWORD)strlen(kLine), &written, NULL)) g_stream.sessions->streamWrites++;
    CloseHandle(file);
}

/* Notifications that never stop (a transcript written all the time) still
 * let snapshots through: a session added meanwhile shows. */
static void CheckSessionsUnderStream(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    WCHAR id[SESSION_ID_CCH];
    UINT_PTR timer;
    BOOL shown;
    int writes;
    g_stream.sessions = sessions;
    SessionId(0, id, ARRAYSIZE(id));
    StringCchPrintfW(g_stream.path, ARRAYSIZE(g_stream.path), L"%s\\%s.jsonl", sessions->transcripts, id);
    timer = SetTimer(sessions->dialog, TRANSCRIPT_STREAM_TIMER, USER_TIMER_MINIMUM, StreamTranscript);
    Check(fixture, NULL, "a transcript stream started", timer != 0);
    if (!timer) return;
    PumpUntil(NULL, NULL, 200);
    writes = sessions->streamWrites;
    Check(fixture, NULL, "a session added during the stream written",
          WriteSession(sessions, UNKNOWN_ROW + 4, sessions->folders[0], L"Streamed conversation", FIXTURE_TIME + 4000, FALSE));
    shown = AwaitTitle(sessions, L"Streamed conversation");
    Check(fixture, sessions->tree, "a snapshot shows while transcripts change continuously", shown && sessions->streamWrites > writes);
    KillTimer(sessions->dialog, TRANSCRIPT_STREAM_TIMER);
    Check(fixture, NULL, "every write of the stream is read", AwaitWritesRead(sessions, 2, L"Settled conversation C"));
}

/* A double-click opens no session, on its row or below the last one: Enter
 * and the menu's Open in do. Without Claude, an open would ask for it (a
 * message box the fixture counts and closes). */
static void CheckSessionsDoubleClick(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, tree = sessions->tree, search = GetDlgItem(dialog, IDC_S_SEARCH);
    WCHAR title[64], shown[256];
    HTREEITEM row;
    RECT rowRect = { 0, 0, 0, 0 }, client;
    POINT point;
    int prompts;
    RowTitle(5, title, ARRAYSIZE(title));
    SetWindowTextW(search, L"conversation 05");
    PumpMessages();
    row = FindTitle(tree, TreeView_GetRoot(tree), title);
    if (row) TreeView_SelectItem(tree, row);
    PumpMessages();
    GetClientRect(tree, &client);
    Check(fixture, tree, "a search leaves one session, selected, with room below it",
          row && TreeView_GetSelection(tree) == row && ItemTitle(tree, row, shown, ARRAYSIZE(shown)) &&
          TreeView_GetItemRect(tree, row, &rowRect, FALSE) && rowRect.bottom + 20 < client.bottom);
    if (!row || TreeView_GetSelection(tree) != row) {
        SetWindowTextW(search, L"");
        return;
    }
    prompts = fixture->openPrompts;
    point.x = (rowRect.left + rowRect.right) / 2;
    point.y = (rowRect.top + rowRect.bottom) / 2;
    if (!DoubleClickTree(dialog, fixture, tree, point)) {
        printf("  skip  sessions double-click: this Windows reports a posted mouse message's position otherwise\n");
        SetWindowTextW(search, L"");
        return;
    }
    Check(fixture, tree, "a double-click on the selected session's row opens nothing", fixture->openPrompts == prompts && TreeView_GetSelection(tree) == row);
    point.y = (rowRect.bottom + client.bottom) / 2;
    DoubleClickTree(dialog, fixture, tree, point);
    Check(fixture, tree, "a double-click below the last row opens nothing", fixture->openPrompts == prompts && TreeView_GetSelection(tree) == row);
    SetWindowTextW(search, L"");
    PumpMessages();
}

/* `key` pressed with Ctrl held, sent to `control` (the thread's keyboard
 * state says Ctrl, as GetKeyState reads it). */
static void SendCtrlKey(HWND control, WPARAM key)
{
    BYTE state[256], held[256];
    GetKeyboardState(state);
    memcpy(held, state, sizeof held);
    held[VK_CONTROL] |= 0x80;
    held[VK_LCONTROL] |= 0x80;
    SetKeyboardState(held);
    SendMessageW(control, WM_KEYDOWN, key, 0);
    SendMessageW(control, WM_KEYUP, key, 0xC0000001);
    SetKeyboardState(state);
    PumpMessages();
}

/* Ctrl+C on a folder's row takes its sessions; Ctrl+V in the side bar, on
 * the other profile picked there, asks about them. */
static void CheckSessionsCopyPaste(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, tree = sessions->tree, list = GetWindow(fixture->profilesViewport, GW_CHILD);
    FixtureRoots roots = ReadRoots(tree);
    int prompts;
    if (!roots.alpha || !list) {
        Check(fixture, tree, "a folder's row to copy found", FALSE);
        return;
    }
    TreeView_SelectItem(tree, roots.alpha);
    SetFocus(tree);
    PumpMessages();
    SendCtrlKey(tree, 'C');
    SendMessageW(list, LB_SETCURSEL, 1, 0);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDC_S_PROFILES, LBN_SELCHANGE), (LPARAM)list);
    PumpMessages();
    SetFocus(list);
    prompts = fixture->openPrompts;
    fixture->promptText[0] = 0;
    SendCtrlKey(list, 'V');
    Check(fixture, list, "Ctrl+C on a folder, then Ctrl+V on another profile, asks about its sessions",
          fixture->openPrompts > prompts && fixture->promptText[0]);
    prompts = fixture->openPrompts;
    SetFocus(tree);
    SendCtrlKey(tree, 'V');
    Check(fixture, tree, "Ctrl+V in the tree of the other profile asks about them too", fixture->openPrompts > prompts);
    SendMessageW(list, LB_SETCURSEL, 0, 0);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDC_S_PROFILES, LBN_SELCHANGE), (LPARAM)list);
    PumpMessages();
}

/* The side bar's list keeps the profiles' names (screen readers read them). */
static void CheckSessionsProfileNames(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND list = GetWindow(fixture->profilesViewport, GW_CHILD);
    BOOL named = list && (int)SendMessageW(list, LB_GETCOUNT, 0, 0) == sessions->profiles.count;
    int p;
    for (p = 0; named && p < sessions->profiles.count; p++) {
        WCHAR text[LABEL_CCH];
        named = SendMessageW(list, LB_GETTEXTLEN, (WPARAM)p, 0) < (LRESULT)ARRAYSIZE(text) &&
                SendMessageW(list, LB_GETTEXT, (WPARAM)p, (LPARAM)text) != LB_ERR && wcscmp(text, sessions->profiles.items[p].name) == 0;
    }
    Check(fixture, list, "the side bar's list gives each profile's name", named);
}

/* gui.c gives the shown view the focus; sessions.c entering its view
 * moves none. */
static void CheckSessionsFocus(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    HWND dialog = sessions->dialog, button = GetDlgItem(dialog, IDC_SESSIONS);
    const WCHAR *folder = sessions->profiles.items[0].folder;
    Gui_ShowSessions(dialog, &sessions->package, FALSE, NULL);
    fixture->sessions = FALSE;
    SetFocus(button);
    if (GetFocus() == button) {
        SessionsView_Enter(&sessions->package, folder);
        Check(fixture, button, "entering the sessions view leaves the focus where it is", GetFocus() == button);
        SessionsView_Leave();
    } else printf("  skip  sessions focus: the focus could not be taken here\n");
    Gui_ShowSessions(dialog, &sessions->package, TRUE, folder);
    fixture->sessions = TRUE;
    Check(fixture, sessions->tree, "showing the sessions view gives its tree the focus", GetFocus() == sessions->tree);
    PumpMessages();
}

/* The sessions view of a real manager layout, filled by sessions.c from
 * private profiles: resizes in three languages, and in one of them its
 * refills, watcher and double-clicks. */
typedef struct SearchTreeWait {
    SessionsFixture *sessions;
    const WCHAR *present, *absent;
    int after;
} SearchTreeWait;

static BOOL SearchTreeShows(const void *context)
{
    const SearchTreeWait *wait = (const SearchTreeWait *)context;
    HWND tree = wait->sessions->tree;
    return wait->sessions->layout.searchCompletions > wait->after &&
           (!wait->present || FindTitle(tree, TreeView_GetRoot(tree), wait->present)) &&
           (!wait->absent || !FindTitle(tree, TreeView_GetRoot(tree), wait->absent));
}

static BOOL WriteSearchTranscript(SessionsFixture *sessions, int index, const char *content)
{
    WCHAR id[SESSION_ID_CCH], path[MAX_PATH];
    char line[2048], cwd[MAX_PATH * 6 + 4];
    SessionId(index, id, ARRAYSIZE(id));
    if (!Core_JsonQuote(sessions->folders[0], cwd, sizeof cwd)) return FALSE;
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s.jsonl", sessions->transcripts, id);
    StringCchPrintfA(line, sizeof line, "{\"sessionId\":\"%ls\",\"cwd\":%s,\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"%s\"}}\n",
                     id, cwd, content);
    return SaveFixtureFile(sessions->root.path, path, line, TRUE);
}

static BOOL HeldSearchDone(const void *context)
{
    const LayoutFixture *fixture = (const LayoutFixture *)context;
    int i;
    for (i = 0; i < fixture->heldSearchCount; i++)
        if (fixture->heldSearch[i].index < 0) return TRUE;
    return FALSE;
}

static void ReleaseHeldSearch(LayoutFixture *fixture)
{
    int i, count = fixture->heldSearchCount;
    fixture->heldSearchCount = 0;
    for (i = 0; i < count; i++)
        DeliverSearchResult(fixture, fixture->heldSearch[i].generation, fixture->heldSearch[i].index);
}

/* Holding worker messages makes the UI's search stay in flight while each
 * notification publishes a new snapshot. Release its late hit and completion
 * together, before the 150 ms refill, then hold the following pass as well. */
static void CheckPendingSearchPasses(SessionsFixture *sessions)
{
    LayoutFixture *fixture = &sessions->layout;
    WCHAR title[64];
    int pass;
    PumpUntil(NULL, NULL, 700);   /* settle the previous query's file notifications */
    fixture->holdSearch = TRUE;
    fixture->heldSearchCount = 0;
    RowTitle(0, title, ARRAYSIZE(title));
    Check(fixture, NULL, "a gated search transcript is prepared", WriteSearchTranscript(sessions, 0, "gated-search-needle"));
    SetDlgItemTextW(sessions->dialog, IDC_S_SEARCH, L"gated-search-needle");
    for (pass = 0; pass < 3; pass++) {
        CountWait snapshot;
        int notified, completed;
        char content[64];
        BOOL held = PumpUntil(HeldSearchDone, fixture, SNAPSHOT_TIMEOUT_MS);
        Check(fixture, sessions->tree, "a search pass is held in flight until its late hit and completion", held);
        if (!held) break;
        if (pass) Check(fixture, sessions->tree, "a slow following pass retains the completed pass on screen",
                        FindTitle(sessions->tree, TreeView_GetRoot(sessions->tree), title) != NULL);
        notified = fixture->reloadRequests;
        snapshot.counter = &fixture->snapshots;
        snapshot.above = fixture->snapshots;
        StringCchPrintfA(content, sizeof content, "gated-search-needle-%d", pass);
        Check(fixture, NULL, "another transcript write arrives during the held search", WriteSearchTranscript(sessions, 0, content));
        Check(fixture, sessions->tree, "a snapshot arrives while that search is still in flight",
              AwaitReloadRequest(fixture, notified, SNAPSHOT_TIMEOUT_MS) && PumpUntil(CountAbove, &snapshot, SNAPSHOT_TIMEOUT_MS));
        completed = fixture->searchCompletions;
        ReleaseHeldSearch(fixture);
        Check(fixture, sessions->tree, "a pending refresh presents every completed pass without waiting for the refill timer",
              fixture->searchCompletions > completed && FindTitle(sessions->tree, TreeView_GetRoot(sessions->tree), title) != NULL);
    }
    fixture->holdSearch = FALSE;
    ReleaseHeldSearch(fixture);
    SetDlgItemTextW(sessions->dialog, IDC_S_SEARCH, L"");
}

/* Only transcript bodies change: entries and the tree's content hash stay
 * the same. The query must follow notifications even after zero matches. */
typedef struct CrossSearchCheck { LayoutFixture *fixture; BOOL checked; } CrossSearchCheck;

static LRESULT CALLBACK CheckCrossSearch(HWND owner, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR reference)
{
    CrossSearchCheck *check = (CrossSearchCheck *)reference;
    (void)id;
    if (message == WM_ENTERIDLE && wp == MSGF_DIALOGBOX && !check->checked) {
        HWND dialog = (HWND)lp, list = GetWindow(GetDlgItem(dialog, IDC_X_LIST), GW_CHILD), profiles = GetDlgItem(dialog, IDC_X_PROFILE);
        WCHAR title[256];
        int i, shared = -1;
        if (!list) return DefSubclassProc(owner, message, wp, lp);
        check->checked = TRUE;
        if (ListView_GetItemCount(list) != 2) {
            GetDlgItemTextW(dialog, IDC_X_TEXT, title, ARRAYSIZE(title));
            printf("  cross-search query=%ls count=%d\n", title, ListView_GetItemCount(list));
        }
        Check(check->fixture, list, "cross-profile search deduplicates shared sessions and finds another profile's unique session", ListView_GetItemCount(list) == 2);
        for (i = 0; i < ListView_GetItemCount(list); i++) {
            ListView_GetItemText(list, i, 0, title, ARRAYSIZE(title));
            if (wcscmp(title, L"cross-needle shared") == 0) shared = i;
        }
        Check(check->fixture, list, "cross-profile results show the matching profile's title", shared >= 0);
        if (shared >= 0) {
            NMLISTVIEW notification;
            ListView_SetItemState(list, shared, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            Check(check->fixture, profiles, "a shared result offers both opening profiles", SendMessageW(profiles, CB_GETCOUNT, 0, 0) == 2);
            SendMessageW(profiles, CB_SETCURSEL, 1, 0);
            ZeroMemory(&notification, sizeof notification);
            notification.hdr.hwndFrom = list;
            notification.hdr.idFrom = IDC_X_LIST;
            notification.hdr.code = LVN_ITEMCHANGED;
            notification.iItem = shared;
            notification.uChanged = LVIF_TEXT;
            SendMessageW(dialog, WM_NOTIFY, IDC_X_LIST, (LPARAM)&notification);
            Check(check->fixture, profiles, "result text changes preserve the chosen opening profile", SendMessageW(profiles, CB_GETCURSEL, 0, 0) == 1);
        }
        PostMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    }
    return DefSubclassProc(owner, message, wp, lp);
}

static void CheckCrossProfileSearch(void)
{
    SessionsFixture sessions;
    DWORD previous = Util_GetSetting(L"SessionsSearch", 1), resultFilter = Util_GetSetting(L"SessionsResult", 0);
    if (!NativeScaleFits("the cross-profile search")) return;
    Util_SetSetting(L"SessionsSearch", 1);
    Util_SetSetting(L"SessionsResult", 0);
    if (OpenSessionsFixture(&sessions, Language(L"en"))) {
        WCHAR entries[MAX_PATH], path[MAX_PATH], shared[SESSION_ID_CCH], unique[SESSION_ID_CCH];
        char json[1024];
        CrossSearchCheck check = { &sessions.layout, FALSE };
        BOOL ready = ShowEveryHeadingWhole(&sessions);   /* the marker written below the sessions shows */
        SessionId(0, shared, ARRAYSIZE(shared));
        SessionId(UNKNOWN_ROW + 30, unique, ARRAYSIZE(unique));
        StringCchPrintfW(entries, ARRAYSIZE(entries), L"%s\\claude-code-sessions\\account-ui\\organization-ui", sessions.profiles.items[1].storageDir);
        ready = ready && Util_EnsureDir(entries);
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\local_%s.json", entries, shared);
        StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"title\":\"cross-needle shared\"}", shared, shared);
        ready = ready && SaveFixtureFile(sessions.root.path, path, json, FALSE);
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\local_%s.json", entries, unique);
        StringCchPrintfA(json, sizeof json, "{\"sessionId\":\"local_%ls\",\"cliSessionId\":\"%ls\",\"title\":\"cross-needle unique\"}", unique, unique);
        ready = ready && SaveFixtureFile(sessions.root.path, path, json, FALSE) && AwaitWritesRead(&sessions, 45, L"Cross search refresh marker");
        Check(&sessions.layout, NULL, "cross-profile fixtures are ready", ready);
        if (ready) {
            SessionSet set;
            int row, listed = 0;
            BOOL loaded = SessionStore_LoadProfiles(&set, &sessions.profiles);
            for (row = 0; loaded && row < set.rowCount; row++) if (set.rows[row].entry[1] >= 0) listed++;
            Check(&sessions.layout, NULL, "the second profile has both cross-profile test entries", loaded && listed == 2);
            SessionStore_Free(&set);
        }
        if (ready) {
            SetDlgItemTextW(sessions.dialog, IDC_S_SEARCH, L"cross-needle");
            SetWindowSubclass(sessions.dialog, CheckCrossSearch, 0x7123, (DWORD_PTR)&check);
            SessionsView_SearchAll();
            RemoveWindowSubclass(sessions.dialog, CheckCrossSearch, 0x7123);
            Check(&sessions.layout, NULL, "cross-profile result dialog was reviewed", check.checked);
        }
    }
    CloseSessionsFixture(&sessions);
    Util_SetSetting(L"SessionsSearch", previous);
    Util_SetSetting(L"SessionsResult", resultFilter);
}

static void CheckLiveConversationSearch(void)
{
    SessionsFixture sessions;
    DWORD previous = Util_GetSetting(L"SessionsSearch", 1);
    if (!NativeScaleFits("the live conversation search")) return;
    Util_SetSetting(L"SessionsSearch", 2);   /* SEARCH_TEXT only, in the private state folder */
    if (OpenSessionsFixture(&sessions, Language(L"en"))) {
        LayoutFixture *fixture = &sessions.layout;
        WCHAR first[64], second[64];
        SearchTreeWait wait = { &sessions, first, second, 0 };
        WPARAM oldGeneration;
        int notifications;
        RowTitle(0, first, ARRAYSIZE(first));
        RowTitle(1, second, ARRAYSIZE(second));
        Check(fixture, NULL, "search transcripts prepared", WriteSearchTranscript(&sessions, 0, "watcher-needle") &&
              WriteSearchTranscript(&sessions, 1, "other-message"));
        wait.after = fixture->searchCompletions;
        SetDlgItemTextW(sessions.dialog, IDC_S_SEARCH, L"watcher-needle");
        Check(fixture, sessions.tree, "a message query finds its first transcript", PumpUntil(SearchTreeShows, &wait, SNAPSHOT_TIMEOUT_MS));
        oldGeneration = fixture->searchGeneration;
        notifications = fixture->reloadRequests;
        wait.after = fixture->searchCompletions;
        wait.present = second;
        wait.absent = first;
        Check(fixture, NULL, "matching text moves between transcripts", WriteSearchTranscript(&sessions, 0, "other-message") &&
              WriteSearchTranscript(&sessions, 1, "watcher-needle"));
        Check(fixture, sessions.tree, "unchanged query follows transcript notifications and drops the old match",
              AwaitReloadRequest(fixture, notifications, SNAPSHOT_TIMEOUT_MS) && PumpUntil(SearchTreeShows, &wait, SNAPSHOT_TIMEOUT_MS));
        SendMessageW(sessions.dialog, WM_APP_SESSION_SEARCH, oldGeneration, 0);
        PumpUntil(NULL, NULL, 200);
        Check(fixture, sessions.tree, "an old search generation cannot restore its stale match", SearchTreeShows(&wait));
        notifications = fixture->searchCompletions;
        SendMessageW(sessions.dialog, WM_APP_SESSION_SEARCH, oldGeneration, -1);
        Check(fixture, sessions.tree, "a stale completion cannot satisfy a current search wait", fixture->searchCompletions == notifications);
        wait.after = fixture->searchCompletions;
        wait.present = NULL;
        wait.absent = second;
        SetDlgItemTextW(sessions.dialog, IDC_S_SEARCH, L"previously-absent-needle");
        Check(fixture, sessions.tree, "a message query can finish without a match", PumpUntil(SearchTreeShows, &wait, SNAPSHOT_TIMEOUT_MS));
        wait.after = fixture->searchCompletions;
        wait.present = first;
        Check(fixture, NULL, "a previously absent message arrives", WriteSearchTranscript(&sessions, 0, "previously-absent-needle"));
        Check(fixture, sessions.tree, "a zero-match query gains a match from a transcript event",
              PumpUntil(SearchTreeShows, &wait, SNAPSHOT_TIMEOUT_MS));
        CheckPendingSearchPasses(&sessions);
    }
    CloseSessionsFixture(&sessions);
    Util_SetSetting(L"SessionsSearch", previous);
}

typedef struct FilterMenuCheck { const LayoutFixture *fixture; BOOL seen; } FilterMenuCheck;

static void CALLBACK CancelFilterMenu(HWND window, UINT message, UINT_PTR id, DWORD time)
{
    (void)window; (void)message; (void)id; (void)time;
    EndMenu();
}

static LRESULT CALLBACK InspectFilterMenu(HWND window, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    FilterMenuCheck *check = (FilterMenuCheck *)data;
    (void)id;
    if (message == WM_INITMENUPOPUP && !check->seen) {
        HMENU menu = (HMENU)wp, group = GetSubMenu(menu, 0), sort = GetSubMenu(menu, 1), show = GetSubMenu(menu, 2);
        WCHAR text[160];
        check->seen = TRUE;
        Check(check->fixture, NULL, "filter menu starts as Claude's: grouping, sorting and Show submenus", group && sort && show && GetMenuItemCount(menu) == 10);
        GetMenuStringW(group, 2, text, ARRAYSIZE(text), MF_BYPOSITION);
        Check(check->fixture, NULL, "environment grouping is visible and selected in the real menu",
              wcscmp(text, TR(L"Environment")) == 0 && (GetMenuState(group, 2, MF_BYPOSITION) & MF_CHECKED));
        GetMenuStringW(sort, 0, text, ARRAYSIZE(text), MF_BYPOSITION);
        Check(check->fixture, NULL, "last updated leads the sorting choices", wcscmp(text, TR(L"Last updated")) == 0);
        Check(check->fixture, NULL, "sorting direction has its own radio choice",
              (GetMenuState(sort, 5, MF_BYPOSITION) & MF_CHECKED) && !(GetMenuState(sort, 4, MF_BYPOSITION) & MF_CHECKED));
        GetMenuStringW(show, 0, text, ARRAYSIZE(text), MF_BYPOSITION);
        Check(check->fixture, NULL, "metadata is explicitly labeled as PR status", wcscmp(text, TR(L"Meta&data (PR status)")) == 0);
        Check(check->fixture, NULL, "empty environment groups can be requested", !(GetMenuState(show, 2, MF_BYPOSITION) & MF_GRAYED));
        Check(check->fixture, NULL, "reset display defaults is available independently of clear filters",
              !(GetMenuState(menu, 9, MF_BYPOSITION) & MF_GRAYED) && (GetMenuState(menu, 8, MF_BYPOSITION) & MF_GRAYED));
        PostMessageW(window, WM_CANCELMODE, 0, 0);
    }
    return DefSubclassProc(window, message, wp, lp);
}

/* FIXTURE_TIME's day as the sessions view heads it in English: a date of another year. */
static BOOL FixtureDay(WCHAR *text, int cch)
{
    FILETIME utc = Core_MsFileTime(FIXTURE_TIME), local;
    SYSTEMTIME day;
    return FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &day) &&
           GetDateFormatEx(L"en", 0, &day, L"MMM d, yyyy", text, cch, NULL) > 0;
}

#define DAY_HEADING (-2)   /* a case whose day heads the sessions */

/* Persisted display choices are exercised through the real reader and tree. */
static void CheckSessionDisplayChoices(void)
{
    static const WCHAR *const kSettings[] = { L"SessionsGrouping", L"SessionsSortBy", L"SessionsSortDescending", L"SessionsEmptyGroups",
                                              L"SessionsEnvironments", L"SessionsStatus", L"SessionsResult" };
    static const struct { DWORD group, sort, descending, empty, environments; int first; } kCases[] = {
        { 5, 2, 0, 0, 0, UNKNOWN_ROW }, { 5, 2, 1, 0, 0, 0 },
        { 4, 0, 0, 0, 0, 0 }, { 5, 2, 1, 0, SESSION_ENV_CLOUD, -1 }, { 5, 2, 1, 1, SESSION_ENV_CLOUD, -1 },
        { 0, 2, 1, 0, 0, DAY_HEADING }
    };
    DWORD saved[ARRAYSIZE(kSettings)];
    size_t i, setting;
    if (!NativeScaleFits("session display choices")) return;
    for (setting = 0; setting < ARRAYSIZE(kSettings); setting++) saved[setting] = Util_GetSetting(kSettings[setting], setting == 2 ? 1 : 0);
    for (i = 0; i < ARRAYSIZE(kCases); i++) {
        SessionsFixture sessions;
        Util_SetSetting(kSettings[0], kCases[i].group);
        Util_SetSetting(kSettings[1], kCases[i].sort);
        Util_SetSetting(kSettings[2], kCases[i].descending);
        Util_SetSetting(kSettings[3], kCases[i].empty);
        Util_SetSetting(kSettings[4], kCases[i].environments);
        Util_SetSetting(kSettings[5], 0);
        Util_SetSetting(kSettings[6], 0);
        if (OpenSessionsFixture(&sessions, Language(L"en"))) {
            LayoutFixture *fixture = &sessions.layout;
            CountWait wait = { &fixture->snapshots, 0 };
            HTREEITEM group, first;
            WCHAR actual[128], expected[128];
            Check(fixture, sessions.tree, "display-choice snapshot arrives", PumpUntil(CountAbove, &wait, SNAPSHOT_TIMEOUT_MS));
            if (i == 0) {
                FilterMenuCheck menu = { fixture, FALSE };
                UINT_PTR timer = SetTimer(sessions.dialog, 0x7145, 2000, CancelFilterMenu);
                Check(fixture, NULL, "private filter menu has a bounded close", timer != 0);
                if (timer) {
                    SetWindowSubclass(sessions.dialog, InspectFilterMenu, 0x7145, (DWORD_PTR)&menu);
                    SessionsView_Command(MAKEWPARAM(IDC_S_FILTER, BN_CLICKED));
                    RemoveWindowSubclass(sessions.dialog, InspectFilterMenu, 0x7145);
                    KillTimer(sessions.dialog, timer);
                    Check(fixture, NULL, "real filter menu was inspected", menu.seen);
                }
            }
            group = FindTitle(sessions.tree, TreeView_GetRoot(sessions.tree), TR(L"Local"));
            if (kCases[i].first == DAY_HEADING) {
                HTREEITEM day = NthChild(sessions.tree, NULL, 1);
                Check(fixture, sessions.tree, "grouped by date, Pinned comes first, as Claude names it",
                      ItemTitle(sessions.tree, TreeView_GetRoot(sessions.tree), actual, ARRAYSIZE(actual)) && wcscmp(actual, TR(L"Pinned")) == 0);
                Check(fixture, sessions.tree, "a day of another year heads its sessions by its date and year",
                      FixtureDay(expected, ARRAYSIZE(expected)) && ItemTitle(sessions.tree, day, actual, ARRAYSIZE(actual)) &&
                      wcscmp(actual, expected) == 0);
                RowTitle(0, expected, ARRAYSIZE(expected));
                Check(fixture, sessions.tree, "the day's newest session comes first",
                      ItemTitle(sessions.tree, TreeView_GetChild(sessions.tree, day), actual, ARRAYSIZE(actual)) && wcscmp(actual, expected) == 0);
            } else if (kCases[i].first >= 0) {
                first = kCases[i].group == 5 ? TreeView_GetChild(sessions.tree, group) : NthChild(sessions.tree, NULL, 1);
                RowTitle(kCases[i].first, expected, ARRAYSIZE(expected));
                Check(fixture, sessions.tree, "saved grouping and sorting direction determine the first session",
                      (kCases[i].group != 5 || group) && ItemTitle(sessions.tree, first, actual, ARRAYSIZE(actual)) && wcscmp(actual, expected) == 0);
            } else if (kCases[i].empty) {
                Check(fixture, sessions.tree, "show empty groups retains a known environment excluded by the filter",
                      group && TreeView_GetChild(sessions.tree, group) == NULL && TreeView_GetCount(sessions.tree) == 1);
            } else Check(fixture, sessions.tree, "empty groups stay hidden unless requested", !group && TreeView_GetCount(sessions.tree) == 0);
        }
        CloseSessionsFixture(&sessions);
    }
    for (setting = 0; setting < ARRAYSIZE(kSettings); setting++) Util_SetSetting(kSettings[setting], saved[setting]);
}

static void CheckPopulatedSessions(void)
{
    static const WCHAR *const kLanguages[] = { L"en", L"zh-CN" };
    size_t language;
    DWORD grouping;
    if (!NativeScaleFits("the populated sessions view")) return;
    grouping = Util_GetSetting(L"SessionsGrouping", 0);
    Util_SetSetting(L"SessionsGrouping", 1);   /* by folder: the fixture's sessions are told apart by theirs */
    for (language = 0; language < ARRAYSIZE(kLanguages); language++) {
        SessionsFixture sessions;
        TreeWait wait;
        if (OpenSessionsFixture(&sessions, Language(kLanguages[language]))) {
            LayoutFixture *fixture = &sessions.layout;
            FixtureRoots roots;
            wait.tree = sessions.tree;
            wait.count = 4 + 1 + 2 * (FIXTURE_MORE_FIRST + 1) + 1;   /* four roots, the pinned row, each folder's first sessions and Show more, the session with no folder */
            wait.title = NULL;
            Check(fixture, sessions.tree, "event-driven private snapshot populates the native tree", PumpUntil(TreeShows, &wait, SNAPSHOT_TIMEOUT_MS));
            roots = ReadRoots(sessions.tree);
            Check(fixture, sessions.tree, "native tree holds Pinned, two folders and the sessions with no folder",
                  roots.starred && roots.alpha && roots.beta && roots.unknown && roots.unknown != roots.beta &&
                  TreeView_GetCount(sessions.tree) == wait.count);
            if (language == 0)
                Check(fixture, sessions.tree, "headings start folded, until opened", Folded(sessions.tree, roots.alpha) && Folded(sessions.tree, roots.beta));
            OpenFolder(sessions.tree, roots.alpha);
            OpenFolder(sessions.tree, roots.beta);
            CheckShowMore(&sessions, language == 0);
            CheckSessionsResize(&sessions);
            if (language == 0) {
                CheckSessionsProfileNames(&sessions);
                CheckSessionsRefillKeepsState(&sessions);
                CheckSessionsRefillKeepsTopRow(&sessions);
                CheckSessionsWatcher(&sessions);
                CheckSessionsUnderStream(&sessions);
                CheckSessionsDoubleClick(&sessions);
                CheckSessionsCopyPaste(&sessions);
                CheckSessionsFocus(&sessions);
            }
        }
        CloseSessionsFixture(&sessions);
    }
    Util_SetSetting(L"SessionsGrouping", grouping);
}

int wmain(void)
{
    INITCOMMONCONTROLSEX controls = { sizeof controls, ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    WCHAR temp[MAX_PATH], state[MAX_PATH];
    LayoutFixture none;
    int language;
    setvbuf(stdout, NULL, _IONBF, 0);
    g_hInst = GetModuleHandleW(NULL);
    /* As the program runs (app.res): common controls 6 and per-monitor DPI. */
    if (!InitCommonControlsEx(&controls) ||
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        printf("test_layout must be linked with the program's manifest (app.res, /MANIFEST:NO).\n");
        return 1;
    }
    /* The log and the queued session changes, in a private folder before anything is logged. */
    if (!GetTempPathW(ARRAYSIZE(temp), temp) || !GetTempFileNameW(temp, L"cdl", 0, state) || !DeleteFileW(state) || !CreateDirectoryW(state, NULL)) {
        printf("Layout test private state folder could not be made.\n");
        return 1;
    }
    Util_SetStateDir(state);
    Theme_Init();
    if (!Theme_RichTextReady()) printf("  FAIL  the rich edit (msftedit.dll) is loaded\n");
    if (!PrepareMainTemplate()) {
        printf("Layout test class could not be registered.\n");
        RemoveFixtureTree(state, state);
        return 1;
    }
    MeasureScalesAgainstWorkArea();
    CheckDialogs();
    CheckMessageBoxes();
    CheckSettingsDialog();
    CheckSharedItemLists();
    CheckValueRows();
    CheckPropertyList();
    CheckProfileList();
    CheckConflictsList();
    CheckZoom();
    CheckAboutDialog();
    CheckHiddenRows();
    CheckMainStates();
    CheckProfileNotes();
    CheckProfileSearchLayout();
    CheckLanguageRoundTrips();
    CheckResponsiveMain();
    CheckMainFrameMessages();
    if (NativeScaleFits("the real sessions view's transitions")) for (language = 0; language < Localize_LanguageCount(); language++) {
        const WCHAR *code = Localize_LanguageCode(language);
        (void)code;
        CheckRealSessionsTransition(language);
    }
    CheckPopulatedSessions();
    CheckSessionDisplayChoices();
    CheckLiveConversationSearch();
    CheckCrossProfileSearch();
    UnregisterClassW(g_fixtureClass, g_hInst);
    HeapFree(GetProcessHeap(), 0, g_mainTemplate);
    ZeroMemory(&none, sizeof none);
    Check(&none, NULL, "private state folder removed without the Recycle Bin", RemoveFixtureTree(state, state));
    printf("Layout tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
