/*
 * The manager window: the profile list with its actions beside it (Open,
 * Quit, Restart, New, Edit, Delete, Sync sessions, Repair, Set as default);
 * in its header the program's, the sessions' and the shortcuts' menus, the
 * status (or the progress of a sync), the version and Sessions view, which
 * turns the same window to the sessions view (sessions.c). Several profiles
 * can be selected (Shift, Ctrl, Ctrl+A): every action that can takes them
 * all, the others a profile selected alone. Open, Quit, Restart and Sync run
 * on a thread of their own, the sync's progress in the header. Also the
 * profile and uninstall dialogs. Everything it shows follows events (see
 * WatchOutside and WatchShortcutFolders); nothing polls.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>

#define WM_APP_UNINSTALL       (WM_APP + 1)
#define WM_APP_RELAYOUT        (WM_APP + 2)
#define WM_APP_RESTORE_FOCUS   (WM_APP + 3)   /* back to the window: the focus may be on a control the refresh hid */
#define WM_APP_FIT_NAMES       (WM_APP + 4)   /* a dialog was fitted (opened, or moved to another scale): cut names to it */
#define WM_APP_SHOW            (WM_APP + 5)   /* a second launch asked for the manager itself */
#define WM_APP_SETUPLINKS      (WM_APP + 6)   /* just installed: offer to set up claude:// links */
#define WM_APP_UPDATE          (WM_APP + 7)   /* the latest release is known */
#define WM_APP_DOWNLOADED      (WM_APP + 8)   /* wParam: how the download went (UpdateResult), lParam: Windows' error */
#define WM_APP_SELECTION       (WM_APP + 9)   /* the list selection changed */
#define WM_APP_SHORTCUTS       (WM_APP + 10)  /* the desktop, Start menu or taskbar pins folder changed */
#define WM_APP_OUTSIDE         (WM_APP + 11)  /* something the window shows changed outside it (g_outside) */
#define WM_APP_SHORTCUTS_CHECK (WM_APP + 14)  /* once for a burst of WM_APP_SHORTCUTS (12 and 13 are the sessions view's) */
#define WM_APP_KEEP_SESSIONS   (WM_APP + 16)  /* opened: the session lists kept, a linked session folder offered its own */
#define WM_APP_JOB_DONE        (WM_APP + 17)  /* an Open, Quit, Restart or Sync is done: lParam its Job */

#define ROW_ICON_DIPS         24   /* a taskbar button's icon size, so the initial shows in the row's badge */
#define PREVIEW_ICON_DIPS     32   /* the profile dialog's icon */
#define JOB_END_WAIT_MS       60000   /* closing waits this long at most for a sync under way */
#define FIRST_COLUMN_WIDTH    60   /* any: Gui_LayoutProfileColumns sizes the columns */

/* Set once the manager's window exists: a second launch waits for it while
 * the first one still repairs its install. */
#define MANAGER_READY_EVENT      L"Local\\ClaudeDesktopProfilesManager.ManagerReady"
#define MANAGER_READY_TIMEOUT_MS 15000
#define SHELL_WORK_WAIT_MS       60000   /* the language's shortcuts and pins take about a second */

/* What changed outside the window, told by the thread that waits for it. */
enum { CHANGE_PROFILES = 1, CHANGE_LINKS = 2, CHANGE_PACKAGES = 4, CHANGE_PINS = 8 };

/* What the button below the status does (IDC_STATUS_ACTION). */
typedef enum StatusAction { STATUS_ACTION_NONE, STATUS_ACTION_GET_CLAUDE } StatusAction;

typedef struct MainState {
    HWND         dlg;
    HWND         list;
    ProfileList  profiles;
    ClaudePackage pkg;
    StatusAction statusAction;
    WCHAR        shortcutStateFolder[FOLDER_CCH]; /* profile whose shortcut and pin state is cached */
    BOOL         onDesktop;
    BOOL         pinned;
    BOOL         inStartMenu;
    ULONG        shortcutsNotify;  /* SHChangeNotifyRegister on the folders the shortcuts menu reads */
    HANDLE       job;              /* the thread of the Open, Quit, Restart or Sync under way */
    BOOL         progressShown;    /* the column's foot shows a sync's progress in place of the status */
    int          progressDone, progressTotal;
    WCHAR        progressText[128];
    WCHAR        build[32];        /* this copy's version: when it was built */
    HANDLE       outsideThread;    /* waits for outside changes (WatchOutside) */
    HANDLE       outsideStop;
    HANDLE       readyEvent;       /* MANAGER_READY_EVENT */
    HICON        bigIcon;
    HICON        smallIcon;
    BOOL         uninstalled;      /* nothing may touch the registry any more */
    BOOL         uninstallInProgress; /* its dialog or the uninstall itself: no refresh */
    BOOL         closing;          /* asked to close (installer update) */
    BOOL         pendingUninstall; /* uninstall asked while a dialog was open */
    BOOL         pendingSetUpLinks; /* links setup asked while a dialog was open */
    BOOL         pendingConflicts;  /* a sync left conflicts to the person, not asked yet */
    BOOL         uninstallOnly;    /* started from Settings to uninstall: no manager window */
    BOOL         updating;         /* the newer release is downloading or installing */
    BOOL         installing;
    BOOL         selectionPending; /* WM_APP_SELECTION posted */
    BOOL         shortcutsCheckPending; /* WM_APP_SHORTCUTS_CHECK posted */
    BOOL         keepAsked;        /* a linked session folder's offer waits for its profiles to close (AskKeepAgain) */
    BOOL         layoutReady, layingOut;
    HANDLE       shellWork;        /* the thread writing shortcuts and pins in a new language */
    HMENU        programMenu;      /* the menu bar's menus, filled as each opens (WM_INITMENUPOPUP) */
    HMENU        sessionsMenu;
    HMENU        shortcutsMenu;
    HMENU        helpMenu;
} MainState;

static MainState g_manager;
static volatile LONG g_outside;   /* CHANGE_* not handled yet: set by the thread, taken by the window */

/* While uninstalling or closing, nothing may reload state or write the
 * registry. */
static BOOL StateChangesBlocked(void)
{
    return g_manager.uninstalled || g_manager.uninstallInProgress || g_manager.closing;
}

/* The profile selected alone: what Edit, Set as default and the shortcuts
 * act on; NULL when none or several are. */
static const Profile *SelectedProfile(void)
{
    int i;
    if (ListView_GetSelectedCount(g_manager.list) != 1) return NULL;
    i = ListView_GetNextItem(g_manager.list, -1, LVNI_SELECTED);
    return (i >= 0 && i < g_manager.profiles.count) ? &g_manager.profiles.items[i] : NULL;
}

/* Of the profiles selected, the one with the keyboard's focus, else the first. */
static const Profile *FocusedProfile(void)
{
    int i = ListView_GetNextItem(g_manager.list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (i < 0) i = ListView_GetNextItem(g_manager.list, -1, LVNI_SELECTED);
    return (i >= 0 && i < g_manager.profiles.count) ? &g_manager.profiles.items[i] : NULL;
}

/* The profiles selected, one bit each (MAX_PROFILES <= 32). Each row found
 * must come after the last: a list not made yet answers 0 every time. */
static DWORD SelectedProfiles(void)
{
    DWORD bits = 0;
    int i = -1, next;
    while ((next = ListView_GetNextItem(g_manager.list, i, LVNI_SELECTED)) > i && next < g_manager.profiles.count) {
        bits |= 1u << next;
        i = next;
    }
    return bits;
}

/* The profiles of `bits` that can be deleted: all but Claude's own. */
static DWORD Deletable(DWORD bits)
{
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if (g_manager.profiles.items[i].isStock) bits &= ~(1u << i);
    return bits;
}

static const WCHAR *StockProfileName(void)
{
    int i = Profiles_Find(&g_manager.profiles, STOCK_FOLDER);
    return i >= 0 ? g_manager.profiles.items[i].name : STOCK_DEFAULT_NAME;
}

/* A control turned off while it has the focus gives it to the next one. */
static void EnableControl(int id, BOOL enabled)
{
    HWND control = GetDlgItem(g_manager.dlg, id);
    if (!enabled && GetFocus() == control) SendMessageW(g_manager.dlg, WM_NEXTDLGCTL, 0, FALSE);
    EnableWindow(control, enabled);
}

static void LayoutMainControls(void);

/* The status and the version take the height their text needs: the window
 * is laid out again when one of them changes. */
static void SetTextIfChanged(int id, const WCHAR *text)
{
    WCHAR current[512];
    GetDlgItemTextW(g_manager.dlg, id, current, ARRAYSIZE(current));
    if (wcscmp(current, text) == 0) return;
    SetDlgItemTextW(g_manager.dlg, id, text);
    if (id == IDC_STATUS || id == IDC_VERSION) LayoutMainControls();
}

/* Posts `message` unless it already waits in the queue: a burst of events
 * is handled once. */
static void PostOnce(BOOL *pending, UINT message)
{
    if (*pending) return;
    *pending = TRUE;
    PostMessageW(g_manager.dlg, message, 0, 0);
}

static void JoinNames(DWORD bits, WCHAR *out, size_t cch);

/* The status action and Update buttons take room only while they show: the
 * window is laid out again when one of them appears or goes. */
static void ShowOptionalButton(int id, BOOL shown)
{
    HWND button = GetDlgItem(g_manager.dlg, id);
    if (((GetWindowLongW(button, GWL_STYLE) & WS_VISIBLE) != 0) == shown) return;
    ShowWindow(button, shown ? SW_SHOW : SW_HIDE);
    LayoutMainControls();
}

/* The focus and the default button of the view shown: the profiles list and
 * Open, or the sessions tree and IDOK (SessionsView_Command opens the
 * session). */
static void FocusView(HWND dialog, BOOL sessions)
{
    SendMessageW(dialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dialog, sessions ? IDC_S_TREE : IDC_LIST), TRUE);
    SendMessageW(dialog, DM_SETDEFID, sessions ? IDOK : IDC_OPEN, 0);
}

/* ------------------------------------------------------------------ list */

/* Where a profile's session entries are: its own folder, another profile's,
 * a folder of the user's, or none yet (sessionlink.c); with its own, the
 * other profiles it syncs with, named. */
static const WCHAR *SessionsFolderText(int i, WCHAR *text, size_t cch)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)];
    LinkState state;
    int group = g_manager.profiles.items[i].syncGroup;
    SessionLink_Read(&g_manager.profiles, i, &state);
    switch (state.kind) {
    case LINK_OWN:
        if (group) {
            JoinNames(SessionVault_Group(&g_manager.profiles, group) & ~(1u << i), names, ARRAYSIZE(names));
            StringCchPrintfW(text, cch, TR(L"\x21C4 Synced with %s"), names);
            return text;
        }
        return TR(Theme_SessionsFolderState(0));
    case LINK_NOT_SIGNED_IN: return TR(Theme_SessionsFolderState(1));
    case LINK_NO_SESSIONS:   return TR(Theme_SessionsFolderState(2));
    case LINK_BROKEN:        return TR(Theme_SessionsFolderState(3));
    case LINK_PROFILE:
        StringCchPrintfW(text, cch, L"\x2192 %s", g_manager.profiles.items[state.profile].name);
        return text;
    default:
        StringCchPrintfW(text, cch, L"\x2192 %s", state.target);
        return text;
    }
}

static void UpdateRow(int i)
{
    const Profile *p = &g_manager.profiles.items[i];
    WCHAR text[MAX_PATH];
    ListView_SetItemText(g_manager.list, i, 0, (LPWSTR)p->name);
    ListView_SetItemText(g_manager.list, i, 1,
                         (LPWSTR)TR(Theme_ProfileRole(p->isStock, Core_EqualsI(p->folder, g_manager.profiles.defaultFolder))));
    StringCchPrintfW(text, ARRAYSIZE(text), L"%%APPDATA%%\\%s", p->folder);
    ListView_SetItemText(g_manager.list, i, 2, text);
    ListView_SetItemText(g_manager.list, i, 3, (LPWSTR)SessionsFolderText(i, text, ARRAYSIZE(text)));
}

static int IconPixels(void)
{
    return MulDiv(ROW_ICON_DIPS, (int)GetDpiForWindow(g_manager.dlg), 96);
}

/* Each row's image, I_IMAGENONE for a profile whose icon could not be made. */
static void RebuildImages(int *rowImages)
{
    int pixels = IconPixels(), i;
    HIMAGELIST images = ImageList_Create(pixels, pixels, ILC_COLOR32, g_manager.profiles.count, 1), previous;
    for (i = 0; i < g_manager.profiles.count; i++) rowImages[i] = I_IMAGENONE;
    if (!images) return;
    for (i = 0; i < g_manager.profiles.count; i++) {
        HICON icon = Icons_Create(&g_manager.pkg, &g_manager.profiles.items[i], pixels);
        if (!icon) icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, pixels, pixels, 0);
        if (icon) {
            int added = ImageList_AddIcon(images, icon);
            if (added >= 0) rowImages[i] = added;
            DestroyIcon(icon);
        }
    }
    previous = ListView_SetImageList(g_manager.list, images, LVSIL_SMALL);
    if (previous) ImageList_Destroy(previous);
}

/* Default widths are independent of profile names and the selected language.
 * A user's adjustments persist; the data column follows the available space
 * (the window's minimum size keeps it at least its own minimum). */
void Gui_LayoutProfileColumns(HWND list)
{
    WCHAR text[MAX_PATH];
    int profileWidth, roleWidth, dataMinimum, sessionsMinimum, dataWidth, padding, i, count = ListView_GetItemCount(list);
    int actualProfile = ListView_GetColumnWidth(list, 0), actualRole = ListView_GetColumnWidth(list, 1);
    int actualData = ListView_GetColumnWidth(list, 2);
    Theme_ProfileColumnWidths(list, &profileWidth, &roleWidth, &dataMinimum, &sessionsMinimum);
    if (Theme_ColumnResizeIsManual(list, 0)) profileWidth = actualProfile;
    if (Theme_ColumnResizeIsManual(list, 1)) roleWidth = actualRole;
    /* The data column: every row's folder whole, with the room the stock folder gets around its text. */
    dataWidth = dataMinimum;
    padding = dataMinimum - ListView_GetStringWidth(list, L"%APPDATA%\\" STOCK_FOLDER);
    for (i = 0; i < count; i++) {
        text[0] = 0;
        ListView_GetItemText(list, i, 2, text, ARRAYSIZE(text));
        dataWidth = max(dataWidth, ListView_GetStringWidth(list, text) + padding);
    }
    if (Theme_ColumnResizeIsManual(list, 2)) dataWidth = actualData;
    else if (!Theme_ColumnResizeIsManual(list, 3)) {
        /* Narrower, down to the stock folder's width, when the sessions folder would not fit otherwise: a folder cut short shows whole in its tip. */
        RECT client;
        GetClientRect(list, &client);
        dataWidth = max(dataMinimum, min(dataWidth, client.right - profileWidth - roleWidth - sessionsMinimum));
    }
    Theme_SetColumnWidth(list, 0, profileWidth);
    Theme_SetColumnWidth(list, 1, roleWidth);
    Theme_SetColumnWidth(list, 2, dataWidth);
    /* The sessions folder: the rest, never narrower than what it says (past it, the list scrolls sideways). */
    Theme_FitLastColumn(list, sessionsMinimum);
}

static void LayoutColumns(void)
{
    Gui_LayoutProfileColumns(g_manager.list);
}

static void UpdateButtons(void);
static void FinishShellWork(void);
static void UpdateNote(void);
static void ReflowMain(void);
static void ShowVersion(void);

/* The profiles selected, by folder, and the one with the focus: what a
 * refill of the list selects again. */
typedef struct ListSelection {
    WCHAR focused[FOLDER_CCH];
    WCHAR folders[MAX_PROFILES][FOLDER_CCH];
    int   count;
} ListSelection;

static void TakeSelection(ListSelection *selection)
{
    const Profile *focused = FocusedProfile();
    DWORD bits = SelectedProfiles();
    int i;
    ZeroMemory(selection, sizeof *selection);
    if (focused) StringCchCopyW(selection->focused, ARRAYSIZE(selection->focused), focused->folder);
    for (i = 0; i < g_manager.profiles.count; i++)
        if (bits & (1u << i))
            StringCchCopyW(selection->folders[selection->count++], ARRAYSIZE(selection->folders[0]), g_manager.profiles.items[i].folder);
}

/* Selects the row of `folder` alone, else the first one: its index, -1 in
 * an empty list. */
static int SetSelectedRow(const WCHAR *folder)
{
    int row = Profiles_Find(&g_manager.profiles, folder);
    if (row < 0 && g_manager.profiles.count > 0) row = 0;
    ListView_SetItemState(g_manager.list, -1, 0, LVIS_SELECTED);
    if (row >= 0) ListView_SetItemState(g_manager.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    return row;
}

/* The program moves the selection: the row comes into sight. */
static void SelectProfileRow(const WCHAR *folder)
{
    int row = SetSelectedRow(folder);
    if (row >= 0) ListView_EnsureVisible(g_manager.list, row, FALSE);
}

/* A refill keeps the view where it was (the viewport follows a row brought
 * into sight even during a refill) and the profiles selected that are still
 * there: only a selection that had to move, its profile gone, comes into
 * sight once drawing is back on. */
static void FillList(const ListSelection *selection)
{
    LVITEMW item;
    const WCHAR *select = selection ? selection->focused : NULL;
    int rowImages[MAX_PROFILES], i, row, other;
    BOOL selectionMoved = select && select[0] && Profiles_Find(&g_manager.profiles, select) < 0;
    SendMessageW(g_manager.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_manager.list);
    RebuildImages(rowImages);
    for (i = 0; i < g_manager.profiles.count; i++) {
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.iImage = rowImages[i];
        item.pszText = g_manager.profiles.items[i].name;
        ListView_InsertItem(g_manager.list, &item);
        UpdateRow(i);
    }
    row = SetSelectedRow(select);
    for (i = 0; selection && i < selection->count; i++)
        if ((other = Profiles_Find(&g_manager.profiles, selection->folders[i])) >= 0 && other != row)
            ListView_SetItemState(g_manager.list, other, LVIS_SELECTED, LVIS_SELECTED);
    SendMessageW(g_manager.list, WM_SETREDRAW, TRUE, 0);
    if (selectionMoved && row >= 0) ListView_EnsureVisible(g_manager.list, row, FALSE);
    LayoutColumns();
    InvalidateRect(g_manager.list, NULL, TRUE);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
    UpdateNote();
}

/* Whether the list shows the same rows: folders, names and icons. */
static BOOL SameProfileRows(const ProfileList *a, const ProfileList *b)
{
    int i;
    if (a->count != b->count) return FALSE;
    for (i = 0; i < a->count; i++) {
        if (!Core_EqualsI(a->items[i].folder, b->items[i].folder) || wcscmp(a->items[i].name, b->items[i].name) != 0 ||
            a->items[i].color != b->items[i].color || wcscmp(a->items[i].badge, b->items[i].badge) != 0 ||
            a->items[i].picture != b->items[i].picture)
            return FALSE;
    }
    return TRUE;
}

/* --------------------------------------------------------------- status */

/* "2.16120.0.0" -> "2.16120.0", as Claude shows it. */
static void ShortVersion(WCHAR *out, size_t cch)
{
    size_t n;
    StringCchCopyW(out, cch, g_manager.pkg.version);
    n = wcslen(out);
    if (n > 2 && out[n - 2] == L'.' && out[n - 1] == L'0') out[n - 2] = 0;
}

static void UpdateStatus(void)
{
    WCHAR text[256], version[32];
    StatusAction action = STATUS_ACTION_NONE;

    ShortVersion(version, ARRAYSIZE(version));

    if (!g_manager.pkg.found) {
        StringCchCopyW(text, ARRAYSIZE(text), TR(Theme_MainStatus(MAIN_STATUS_NO_CLAUDE)));
        action = STATUS_ACTION_GET_CLAUDE;
    } else if (Handler_UserChoice() != USERCHOICE_OURS) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainStatus(MAIN_STATUS_NO_LINKS)), version);
    } else {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainStatus(MAIN_STATUS_ROUTED)), version);
    }
    SetTextIfChanged(IDC_STATUS, text);
    SetTextIfChanged(IDC_STATUS_ACTION, TR(Theme_MainCaption(IDC_STATUS_ACTION, 0)));
    /* Never hide the focused control. */
    if (action == STATUS_ACTION_NONE && GetFocus() == GetDlgItem(g_manager.dlg, IDC_STATUS_ACTION))
        FocusView(g_manager.dlg, SessionsView_Shown());
    g_manager.statusAction = action;
    ShowOptionalButton(IDC_STATUS_ACTION, action != STATUS_ACTION_NONE);
}

/* Text of a static that names a profile: a long name is cut (with an
 * ellipsis) until the whole text fits `box` (client coordinates). `format`
 * has one %s. Returns the height the text takes. */
static int FitNameIn(HWND control, const RECT *box, const WCHAR *format, const WCHAR *name, WCHAR *text, size_t cch)
{
    RECT need;
    HDC dc;
    HFONT old;
    size_t keep;

    dc = GetDC(control);
    old = (HFONT)SelectObject(dc, (HFONT)SendMessageW(control, WM_GETFONT, 0, 0));
    for (keep = wcslen(name); ; keep = Localize_ShorterCut(name, keep)) {
        Localize_FormatCutName(format, name, keep, text, cch);
        need = *box;
        DrawTextW(dc, text, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | Localize_ReadingFlags());
        if (keep <= 1 || (need.bottom - need.top <= box->bottom - box->top && need.right <= box->right)) break;
    }
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    return need.bottom - need.top;
}

static void FitName(HWND control, const WCHAR *format, const WCHAR *name, WCHAR *text, size_t cch)
{
    RECT box;
    GetClientRect(control, &box);
    FitNameIn(control, &box, format, name, text, cch);
}

/* The note under Set as default names the profile the regular Claude icon
 * opens; it depends on that name, the language and the scale only. */
static void UpdateNote(void)
{
    Theme_LayoutSidebarNote(GetDlgItem(g_manager.dlg, IDC_NOTE),
                            TR(Theme_MainNote()),
                            StockProfileName());
}

static void SetKeepText(HWND dialog, const ProfileList *list)
{
    WCHAR text[1024];
    int i = Profiles_Find(list, STOCK_FOLDER);
    FitName(GetDlgItem(dialog, IDC_U_KEEP),
            TR(L"Claude Desktop and its \x201C%s\x201D profile (the one the regular Claude icon opens) are not touched."),
            i >= 0 ? list->items[i].name : STOCK_DEFAULT_NAME, text, ARRAYSIZE(text));
    SetDlgItemTextW(dialog, IDC_U_KEEP, text);
}

static void UpdateButtons(void)
{
    const Profile *p = SelectedProfile();
    DWORD selected = SelectedProfiles();
    BOOL isDefault = p && Core_EqualsI(p->folder, g_manager.profiles.defaultFolder), idle = g_manager.job == NULL;

    /* The shortcuts menu shows the state of a profile selected alone. */
    if (p && !Core_EqualsI(p->folder, g_manager.shortcutStateFolder)) {
        StringCchCopyW(g_manager.shortcutStateFolder, ARRAYSIZE(g_manager.shortcutStateFolder), p->folder);
        g_manager.onDesktop = Shortcut_IsOnDesktop(p);
        g_manager.pinned = TaskbarPin_IsPinned(p);
        g_manager.inStartMenu = Shortcut_IsInStartMenu(p);
    }
    EnableControl(IDC_OPEN, selected && g_manager.pkg.found && idle);
    EnableControl(IDC_STOP, selected && idle);   /* Claude running or not is read when it is pressed: nothing watches it */
    EnableControl(IDC_RESTART, selected && g_manager.pkg.found && idle);
    EnableControl(IDC_NEW, g_manager.profiles.count < MAX_PROFILES);
    EnableControl(IDC_EDIT, selected != 0);
    EnableControl(IDC_DELETE, Deletable(selected) != 0);
    EnableControl(IDC_SYNC, selected && idle);
    EnableControl(IDC_REPAIR, idle);
    EnableControl(IDC_DEFAULT, p && !isDefault);
}

/* A profile's shortcuts, taskbar pins and open windows get its current icon,
 * name and description. */
static void ApplyBadge(const Profile *before, const Profile *after, const WCHAR *icon)
{
    BOOL links;
    FinishShellWork();
    links = Shortcut_Refresh(before, after, icon);
    TaskbarPin_Refresh(before, after, icon);
    Taskbar_Refresh(after, links);   /* its open windows, if it runs */
    Icons_DeleteStale(after, icon);
}

/* Shortcuts, pins and open windows with different artwork or badge drawing
 * parameters get the current icon. */
static void HealIcons(const ProfileList *list)
{
    WCHAR icon[MAX_PATH];
    int i;
    for (i = 0; i < list->count; i++) {
        const Profile *p = &list->items[i];
        if (Icons_IsStale(&g_manager.pkg, p) && Icons_Ensure(&g_manager.pkg, p, icon, ARRAYSIZE(icon))) ApplyBadge(p, p, icon);
    }
}

/* The rows' texts and the buttons again, for the rows the list shows. */
static void UpdateRowsAndButtons(void)
{
    int i;
    for (i = 0; i < g_manager.profiles.count; i++) UpdateRow(i);
    UpdateButtons();
}

static void Refresh(BOOL rescanPackage)
{
    ProfileList fresh;
    ListSelection selected;
    BOOL hadClaude = g_manager.pkg.found, sameRows;

    TakeSelection(&selected);
    if (rescanPackage) Claude_FindPackage(&g_manager.pkg);
    Profiles_Load(&fresh, &g_manager.pkg);
    if (rescanPackage) HealIcons(&fresh);
    sameRows = SameProfileRows(&fresh, &g_manager.profiles) && hadClaude == g_manager.pkg.found;
    g_manager.profiles = fresh;
    if (sameRows) UpdateRowsAndButtons();
    else FillList(&selected);
    SessionsView_SetProfiles(&g_manager.profiles);
    if (!StateChangesBlocked()) SessionsView_Ready(TRUE);
    UpdateStatus();
}

/* What changed shows at once, before work that keeps the window from
 * painting meanwhile: shortcuts and pins take about a second to write. */
static void ShowChanges(void)
{
    RedrawWindow(g_manager.dlg, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
}

/* What Windows shows of the program, in a new language, written on a thread
 * of its own from a copy of the profiles. */
typedef struct LanguageWork {
    ClaudePackage pkg;
    ProfileList   profiles;
} LanguageWork;

static DWORD WINAPI ApplyLanguageWork(void *param)
{
    LanguageWork *work = (LanguageWork *)param;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Install_ApplyLanguage(&work->pkg, &work->profiles);
    if (SUCCEEDED(hr)) CoUninitialize();
    HeapFree(GetProcessHeap(), 0, work);
    return 0;
}

static void StartLanguageWork(void)
{
    LanguageWork *work = (LanguageWork *)HeapAlloc(GetProcessHeap(), 0, sizeof *work);
    if (work) {
        work->pkg = g_manager.pkg;
        work->profiles = g_manager.profiles;
        g_manager.shellWork = CreateThread(NULL, 0, ApplyLanguageWork, work, 0, NULL);
        if (g_manager.shellWork) return;
        HeapFree(GetProcessHeap(), 0, work);
    }
    Install_ApplyLanguage(&g_manager.pkg, &g_manager.profiles);   /* no thread: at once, then */
}

/* Shortcuts and pins have one writer at a time: what writes them waits for
 * the language's work first. */
static void FinishShellWork(void)
{
    if (!g_manager.shellWork) return;
    if (WaitForSingleObject(g_manager.shellWork, SHELL_WORK_WAIT_MS) != WAIT_OBJECT_0)
        Util_Log(L"the shortcuts and pins of the new language were still being written");
    CloseHandle(g_manager.shellWork);
    g_manager.shellWork = NULL;
}

/* --------------------------------------------------------- profile dialog */

typedef struct ProfileDialog {
    const Profile *existing;   /* NULL: a new profile, or several edited */
    DWORD          several;    /* the profiles edited together (two or more): their sessions only */
    const Profile *copyFrom;   /* new profile: whose settings it can start with */
    WCHAR          name[LABEL_CCH];
    int            color;
    WCHAR          badge[BADGE_CCH];   /* once closed: the badge's own text, empty for the initial */
    DWORD         *picture;            /* the picture shown, NULL for the Claude icon; the caller frees it */
    BOOL           pictureChanged;     /* chosen or removed in the dialog */
    BOOL           badgeOwn;           /* the badge shows text the user typed, not the initial */
    BOOL           filling;            /* the dialog sets the badge's text itself */
    int            colorAt[PALETTE_SIZE];   /* each choice's palette index: the colors no other profile uses first */
    BOOL           openNow;
    BOOL           startup;
    BOOL           copy;
    int            sync;       /* once closed, what its sessions do: 0 kept apart, a group joined, or -1 - a profile to keep them the same as */
    DWORD          syncItems;  /* what it syncs with them, chosen in the dialog (SYNC_ITEM_*); 0: as it was */
    int            syncAt[MAX_PROFILES + 1];   /* each choice's value */
    WCHAR          syncFolder[FOLDER_CCH];     /* once closed, the profile chosen to keep the same as, when one is */
    HICON          preview;
    WCHAR          previewBadge[BADGE_CCH];   /* what `preview` shows, so typing redraws it only when this changes */
    const DWORD   *previewPicture;
    int            previewColor;
    int            previewSize;
} ProfileDialog;

/* The color choices with their swatches: the colors no other profile uses,
 * then the others, under a line, each with the profiles that use it. */
static void FillColors(HWND dialog, ProfileDialog *state)
{
    HWND combo = GetDlgItem(dialog, IDC_P_COLOR);
    WCHAR users[PALETTE_SIZE][MAX_PROFILES * 8], text[ARRAYSIZE(users[0]) + 128], usedBy[ARRAYSIZE(users[0]) + 64];
    BOOL used[PALETTE_SIZE] = { 0 }, anyFree = FALSE;
    int i, pass, count = 0, selected = 0;
    for (i = 0; i < PALETTE_SIZE; i++) users[i][0] = 0;
    for (i = 0; i < g_manager.profiles.count; i++) {
        const Profile *other = &g_manager.profiles.items[i];
        if ((state->existing && Core_EqualsI(other->folder, state->existing->folder)) || other->color < 0 || other->color >= PALETTE_SIZE)
            continue;
        if (used[other->color]) StringCchCatW(users[other->color], ARRAYSIZE(users[0]), TR(L", "));
        StringCchCatW(users[other->color], ARRAYSIZE(users[0]), other->name);
        used[other->color] = TRUE;
    }
    for (i = 0; i < PALETTE_SIZE; i++) anyFree = anyFree || !used[i];
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (pass = 0; pass < 2; pass++) {
        BOOL first = TRUE;
        for (i = 0; i < PALETTE_SIZE; i++) {
            LRESULT index;
            LPARAM data = (LPARAM)Icons_PaletteColor(i) | THEME_CHOICE_SWATCH;
            if (used[i] != (pass == 1)) continue;
            if (used[i]) {
                /* After the tab: in the menu only (theme.c). */
                StringCchPrintfW(usedBy, ARRAYSIZE(usedBy), TR(L"used by %s"), users[i]);
                StringCchPrintfW(text, ARRAYSIZE(text), L"%s\t%s", TR(g_ColorNames[i]), usedBy);
                if (first && anyFree) data |= THEME_CHOICE_SEPARATED;
            } else {
                StringCchCopyW(text, ARRAYSIZE(text), TR(g_ColorNames[i]));
            }
            first = FALSE;
            index = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text);
            if (index < 0) continue;
            SendMessageW(combo, CB_SETITEMDATA, (WPARAM)index, data);
            if (i == state->color) selected = count;
            state->colorAt[count++] = i;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)selected, 0);
}

static int ChosenColor(HWND dialog, const ProfileDialog *state)
{
    LRESULT i = SendDlgItemMessageW(dialog, IDC_P_COLOR, CB_GETCURSEL, 0, 0);
    return i >= 0 && i < PALETTE_SIZE ? state->colorAt[i] : 0;
}

/* The badge's own text as typed, cleaned; empty when it is the initial of
 * `name` (the badge then follows the name). */
static void ChosenBadge(HWND dialog, const ProfileDialog *state, const WCHAR *name, WCHAR *badge, size_t cch)
{
    WCHAR raw[32], initial[2] = { 0, 0 };
    GetDlgItemTextW(dialog, IDC_P_BADGE, raw, ARRAYSIZE(raw));
    initial[0] = Icons_ProfileInitial(name);
    if (!state->badgeOwn || !Core_CleanBadge(raw, badge, cch) || wcscmp(badge, initial) == 0) badge[0] = 0;
}

/* The badge box follows the name while the user has not typed in it. */
static void FollowName(HWND dialog, ProfileDialog *state)
{
    WCHAR name[LABEL_CCH], initial[2] = { 0, 0 };
    if (state->badgeOwn) return;
    GetDlgItemTextW(dialog, IDC_P_NAME, name, ARRAYSIZE(name));
    initial[0] = Icons_ProfileInitial(name);
    state->filling = TRUE;
    SetDlgItemTextW(dialog, IDC_P_BADGE, initial);
    state->filling = FALSE;
}

/* With a picture, the icon has no badge to write in: the badge box rests,
 * and the picture can be removed. */
static void ShowPictureState(HWND dialog, const ProfileDialog *state)
{
    EnableWindow(GetDlgItem(dialog, IDC_P_BADGE), state->picture == NULL);
    EnableWindow(GetDlgItem(dialog, IDC_P_NO_PICTURE), state->picture != NULL);
}

/* A picture file the user chose, in `path`; FALSE when none was (an error said). */
static BOOL ChoosePictureFile(HWND owner, WCHAR *path, size_t cch)
{
    COMDLG_FILTERSPEC filters[2];
    IFileDialog *dialog = NULL;
    IShellItem *result = NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    PWSTR chosen = NULL;
    BOOL ok = FALSE;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog, (void **)&dialog);
    if (FAILED(hr)) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The file could not be chosen (error 0x%08lX)."), (unsigned long)hr);
        return FALSE;
    }
    filters[0].pszName = TR(L"Pictures");
    filters[0].pszSpec = L"*.png;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.bmp;*.dib;*.webp;*.ico;*.tif;*.tiff;*.heic;*.heif;*.avif;*.jxr;*.wdp";
    filters[1].pszName = TR(L"All files");
    filters[1].pszSpec = L"*.*";
    IFileDialog_SetTitle(dialog, TR(L"Choose a picture"));
    IFileDialog_SetFileTypes(dialog, ARRAYSIZE(filters), filters);
    if (SUCCEEDED(IFileDialog_GetOptions(dialog, &options)))
        IFileDialog_SetOptions(dialog, options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
    hr = IFileDialog_Show(dialog, owner);
    if (SUCCEEDED(hr) && SUCCEEDED(hr = IFileDialog_GetResult(dialog, &result))) {
        if (SUCCEEDED(hr = IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &chosen))) {
            ok = SUCCEEDED(StringCchCopyW(path, cch, chosen));
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

static void ChoosePicture(HWND dialog, ProfileDialog *state)
{
    WCHAR path[MAX_PATH];
    DWORD *picture;
    HCURSOR old;
    HRESULT hr = S_OK;
    if (!ChoosePictureFile(dialog, path, ARRAYSIZE(path))) return;
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    picture = Icons_ReadPicture(path, &hr);
    SetCursor(old);
    if (!picture) {
        Util_Log(L"could not read the picture %s (0x%08lX)", path, (unsigned long)hr);
        Ui_Message(dialog, MB_ICONWARNING,
                   TR(L"This picture could not be read (error 0x%08lX).\n\nWindows reads PNG, JPEG, GIF, BMP, TIFF and ICO files; "
                      L"WebP and HEIF need their extension from the Microsoft Store."),
                   (unsigned long)hr);
        return;
    }
    if (state->picture) HeapFree(GetProcessHeap(), 0, state->picture);
    state->picture = picture;
    state->pictureChanged = TRUE;
}

/* The "Copy settings from" check box: a long profile name is cut (with an
 * ellipsis) until the caption and the box's glyph fit the control. */
static void SetCopyLabel(HWND checkBox, const WCHAR *name)
{
    WCHAR text[512];
    RECT box;
    SIZE need;
    size_t keep;
    GetClientRect(checkBox, &box);
    for (keep = wcslen(name); ; keep = Localize_ShorterCut(name, keep)) {
        Localize_FormatCutCaption(TR(L"Copy &settings from \x201C%s\x201D"), name, keep, text, ARRAYSIZE(text));
        SetWindowTextW(checkBox, text);
        if (keep <= 1 || !Theme_CheckBoxSize(checkBox, &need) || need.cx <= box.right - box.left) break;
    }
}

/* The profiles the dialog is for, one bit each; none for a new one. */
static DWORD DialogProfiles(const ProfileDialog *state)
{
    int i;
    if (state->several) return state->several;
    i = state->existing ? Profiles_Find(&g_manager.profiles, state->existing->folder) : -1;
    return i >= 0 ? 1u << i : 0;
}

/* What the profile's sessions can do: be kept apart, or the same as those
 * of a group of other profiles, or of another profile alone (a new group
 * then). The group they keep the same now is selected. */
static void FillSessionChoices(HWND dialog, ProfileDialog *state)
{
    HWND combo = GetDlgItem(dialog, IDC_P_SYNC);
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], text[ARRAYSIZE(names) + 64];
    DWORD editing = DialogProfiles(state), members;
    int current = -1, count = 0, selected = 0, group, i;
    for (i = 0; i < g_manager.profiles.count; i++) {
        if (!(editing & (1u << i))) continue;
        if (current < 0) current = g_manager.profiles.items[i].syncGroup;
        else if (current != g_manager.profiles.items[i].syncGroup) current = 0;
    }
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)TR(L"Not synced"));
    state->syncAt[count++] = 0;
    for (group = 1; group <= MAX_PROFILES; group++) {
        if ((members = SessionVault_Group(&g_manager.profiles, group) & ~editing) == 0) continue;
        JoinNames(members, names, ARRAYSIZE(names));
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"With %s"), names);
        if (SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text) < 0) continue;
        if (group == current) selected = count;
        state->syncAt[count++] = group;
    }
    for (i = 0; i < g_manager.profiles.count && count < (int)ARRAYSIZE(state->syncAt); i++) {
        if ((editing & (1u << i)) || g_manager.profiles.items[i].syncGroup) continue;
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"With %s"), g_manager.profiles.items[i].name);
        if (SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)text) < 0) continue;
        state->syncAt[count++] = -1 - i;
    }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)selected, 0);
}

static int ChosenSessions(HWND dialog, const ProfileDialog *state)
{
    LRESULT i = SendDlgItemMessageW(dialog, IDC_P_SYNC, CB_GETCURSEL, 0, 0);
    return i >= 0 && i < (LRESULT)ARRAYSIZE(state->syncAt) ? state->syncAt[i] : 0;
}

/* The other profiles a choice of the dialog syncs with: a group, or a
 * profile and its group. */
static DWORD SyncedWith(int choice)
{
    int other = -1 - choice;
    if (choice > 0) return SessionVault_Group(&g_manager.profiles, choice);
    if (choice < 0 && other < g_manager.profiles.count)
        return (1u << other) | SessionVault_Group(&g_manager.profiles, g_manager.profiles.items[other].syncGroup);
    return 0;
}

/* What the profiles edited sync with those chosen, picked in a dialog of its own. */
static void ChooseDialogItems(HWND dialog, ProfileDialog *state)
{
    DWORD editing = DialogProfiles(state), others = SyncedWith(ChosenSessions(dialog, state)) & ~editing, items;
    if (!others) return;
    items = state->syncItems ? state->syncItems : SessionVault_GroupItems(&g_manager.profiles, others);
    if (SyncUi_ChooseItems(dialog, &g_manager.profiles, editing | others, &items)) state->syncItems = items;
}

static BOOL ValidateProfileDialog(HWND dialog, ProfileDialog *state, BOOL showError)
{
    WCHAR raw[128], clean[LABEL_CCH], folder[FOLDER_CCH], info[MAX_PROFILES * (LABEL_CCH + 8) + MAX_PATH], appData[MAX_PATH], dir[MAX_PATH];
    WCHAR placeholder[FOLDER_CCH];
    const WCHAR *error = NULL;   /* English, translated where it shows */
    BOOL ok, errorShown;

    if (state->several) {
        JoinNames(state->several, info, ARRAYSIZE(info));
        SetDlgItemTextW(dialog, IDC_P_FOLDER, info);
        EnableWindow(GetDlgItem(dialog, IDOK), TRUE);
        return TRUE;
    }
    GetDlgItemTextW(dialog, IDC_P_NAME, raw, ARRAYSIZE(raw));
    if (!state->existing) {
        ok = Core_ValidateNewName(raw, clean, ARRAYSIZE(clean), folder, ARRAYSIZE(folder), &error);
        if (ok && Util_AppData(appData, ARRAYSIZE(appData)) &&
            SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, folder)) &&
            GetFileAttributesW(dir) != INVALID_FILE_ATTRIBUTES) {
            ok = FALSE;
            error = L"A profile folder with this name already exists.";
        }
        StringCchPrintfW(placeholder, ARRAYSIZE(placeholder), PROFILE_PREFIX L"%s", TR(L"<name>"));
        StringCchPrintfW(info, ARRAYSIZE(info), TR(L"Its data will be kept in %%APPDATA%%\\%s."), ok ? folder : placeholder);
    } else {
        ok = Core_ValidateLabel(raw, clean, ARRAYSIZE(clean), &error);
        StringCchPrintfW(info, ARRAYSIZE(info), TR(L"Data folder: %%APPDATA%%\\%s%s"), state->existing->folder,
                         state->existing->isStock ? TR(L"\nThe regular Claude icon opens this profile.") : L"");
    }
    /* The error takes the place of the folder line while there is one. */
    errorShown = !ok && showError && error;
    SetDlgItemTextW(dialog, IDC_P_FOLDER, info);
    SetDlgItemTextW(dialog, IDC_P_ERROR, errorShown ? TR(error) : L"");
    ShowWindow(GetDlgItem(dialog, IDC_P_FOLDER), errorShown ? SW_HIDE : SW_SHOW);
    ShowWindow(GetDlgItem(dialog, IDC_P_ERROR), errorShown ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(dialog, IDOK), ok);
    if (ok) StringCchCopyW(state->name, ARRAYSIZE(state->name), clean);
    return ok;
}

static void UpdatePreview(HWND dialog, ProfileDialog *state)
{
    Profile sample;
    HICON icon;
    WCHAR shown[BADGE_CCH];
    int size;
    ZeroMemory(&sample, sizeof sample);
    GetDlgItemTextW(dialog, IDC_P_NAME, sample.name, ARRAYSIZE(sample.name));
    sample.color = ChosenColor(dialog, state);
    ChosenBadge(dialog, state, sample.name, sample.badge, ARRAYSIZE(sample.badge));
    Icons_BadgeText(&sample, shown, ARRAYSIZE(shown));
    size = MulDiv(PREVIEW_ICON_DIPS, (int)GetDpiForWindow(dialog), 96);
    if (state->preview && wcscmp(state->previewBadge, shown) == 0 && state->previewPicture == state->picture &&
        state->previewColor == sample.color && state->previewSize == size)
        return;
    icon = Icons_CreateWith(&g_manager.pkg, &sample, state->picture, size);
    SendDlgItemMessageW(dialog, IDC_P_PREVIEW, STM_SETICON, (WPARAM)icon, 0);
    if (state->preview) DestroyIcon(state->preview);
    state->preview = icon;
    StringCchCopyW(state->previewBadge, ARRAYSIZE(state->previewBadge), shown);
    state->previewPicture = state->picture;
    state->previewColor = sample.color;
    state->previewSize = size;
}

static INT_PTR CALLBACK ProfileProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ProfileDialog *state = (ProfileDialog *)GetWindowLongPtrW(dialog, DWLP_USER);

    switch (message) {
    case WM_INITDIALOG:
        state = (ProfileDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, state->several ? TR(L"Edit profiles") : state->existing ? TR(L"Edit profile") : TR(L"New profile"));
        FillSessionChoices(dialog, state);
        EnableWindow(GetDlgItem(dialog, IDC_P_SYNC_ITEMS), ChosenSessions(dialog, state) != 0);
        if (state->several) {
            /* Several profiles: what they share, their sessions; the rows of the rest close (Theme_FitDialog). */
            static const int kOwn[] = { IDC_P_NAME_LABEL, IDC_P_NAME, IDC_P_COLOR_LABEL, IDC_P_COLOR, IDC_P_PREVIEW, IDC_P_BADGE_LABEL,
                                        IDC_P_BADGE, IDC_P_PICTURE, IDC_P_NO_PICTURE, IDC_P_STARTUP, IDC_P_COPY, IDC_P_OPEN, IDC_P_ERROR };
            size_t own;
            for (own = 0; own < ARRAYSIZE(kOwn); own++) ShowWindow(GetDlgItem(dialog, kOwn[own]), SW_HIDE);
            ValidateProfileDialog(dialog, state, FALSE);
            SetFocus(GetDlgItem(dialog, IDC_P_SYNC));
            return FALSE;
        }
        SendDlgItemMessageW(dialog, IDC_P_NAME, EM_LIMITTEXT, state->existing ? MAX_LABEL : MAX_NAME, 0);
        SendDlgItemMessageW(dialog, IDC_P_BADGE, EM_LIMITTEXT, 2 * MAX_BADGE, 0);   /* surrogate pairs too */
        FillColors(dialog, state);
        if (state->existing) SetDlgItemTextW(dialog, IDC_P_NAME, state->existing->name);
        state->badgeOwn = state->existing && state->existing->badge[0];
        if (state->badgeOwn) {
            state->filling = TRUE;
            SetDlgItemTextW(dialog, IDC_P_BADGE, state->existing->badge);
            state->filling = FALSE;
        } else {
            FollowName(dialog, state);
        }
        if (state->existing && state->existing->picture) state->picture = Icons_LoadPicture(state->existing);
        ShowPictureState(dialog, state);
        CheckDlgButton(dialog, IDC_P_STARTUP, state->startup ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(dialog, IDC_P_OPEN, state->openNow ? BST_CHECKED : BST_UNCHECKED);
        /* A check box the dialog does not offer leaves no empty row (Theme_FitDialog closes it). */
        ShowWindow(GetDlgItem(dialog, IDC_P_OPEN), !state->existing && g_manager.pkg.found ? SW_SHOW : SW_HIDE);
        if (!state->existing && state->copyFrom) SetCopyLabel(GetDlgItem(dialog, IDC_P_COPY), state->copyFrom->name);
        else ShowWindow(GetDlgItem(dialog, IDC_P_COPY), SW_HIDE);
        ValidateProfileDialog(dialog, state, FALSE);
        UpdatePreview(dialog, state);
        SendDlgItemMessageW(dialog, IDC_P_NAME, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, IDC_P_NAME));
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);   /* Ui_Dialog fits the dialog after this message */
        return FALSE;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_P_NAME:
            if (HIWORD(wp) == EN_CHANGE) {
                ValidateProfileDialog(dialog, state, TRUE);
                FollowName(dialog, state);
                UpdatePreview(dialog, state);
            }
            return TRUE;
        case IDC_P_BADGE:
            if (HIWORD(wp) == EN_CHANGE && !state->filling) {
                WCHAR typed[32];
                GetDlgItemTextW(dialog, IDC_P_BADGE, typed, ARRAYSIZE(typed));
                /* Emptied, the badge goes back to the initial. */
                state->badgeOwn = typed[0] != 0;
                UpdatePreview(dialog, state);
            }
            return TRUE;
        case IDC_P_PICTURE:
            if (HIWORD(wp) == BN_CLICKED) {
                ChoosePicture(dialog, state);
                ShowPictureState(dialog, state);
                UpdatePreview(dialog, state);
            }
            return TRUE;
        case IDC_P_NO_PICTURE:
            if (HIWORD(wp) == BN_CLICKED && state->picture) {
                HeapFree(GetProcessHeap(), 0, state->picture);
                state->picture = NULL;
                state->pictureChanged = TRUE;
                ShowPictureState(dialog, state);
                UpdatePreview(dialog, state);
                SetFocus(GetDlgItem(dialog, IDC_P_PICTURE));
            }
            return TRUE;
        case IDC_P_COLOR:
            if (HIWORD(wp) == CBN_SELCHANGE) UpdatePreview(dialog, state);
            return TRUE;
        case IDC_P_SYNC:
            if (HIWORD(wp) == CBN_SELCHANGE) EnableWindow(GetDlgItem(dialog, IDC_P_SYNC_ITEMS), ChosenSessions(dialog, state) != 0);
            return TRUE;
        case IDC_P_SYNC_ITEMS:
            if (HIWORD(wp) == BN_CLICKED) ChooseDialogItems(dialog, state);
            return TRUE;
        case IDOK:
            if (!ValidateProfileDialog(dialog, state, TRUE)) return TRUE;
            state->sync = ChosenSessions(dialog, state);
            if (state->sync < 0 && -1 - state->sync < g_manager.profiles.count)
                StringCchCopyW(state->syncFolder, ARRAYSIZE(state->syncFolder), g_manager.profiles.items[-1 - state->sync].folder);
            if (state->several) {
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            state->color = ChosenColor(dialog, state);
            ChosenBadge(dialog, state, state->name, state->badge, ARRAYSIZE(state->badge));
            state->openNow = IsDlgButtonChecked(dialog, IDC_P_OPEN) == BST_CHECKED;
            state->startup = IsDlgButtonChecked(dialog, IDC_P_STARTUP) == BST_CHECKED;
            state->copy = !state->existing && state->copyFrom && IsDlgButtonChecked(dialog, IDC_P_COPY) == BST_CHECKED;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_P_FOLDER);

    case WM_DPICHANGED:
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);
        break;

    case WM_APP_FIT_NAMES:
        if (state && !state->several) {
            if (!state->existing && state->copyFrom) SetCopyLabel(GetDlgItem(dialog, IDC_P_COPY), state->copyFrom->name);
            UpdatePreview(dialog, state);
        }
        return TRUE;

    case WM_DESTROY:
        if (state && state->preview) {
            DestroyIcon(state->preview);
            state->preview = NULL;
        }
        break;
    }
    return FALSE;
}

/* ---------------------------------------------------------- uninstall dialog */

typedef struct UninstallDialog {
    const ProfileList *list;
    HWND rows;   /* the profiles to keep, as check boxes (in the view it scrolls in, which has its id) */
    BOOL removeData[MAX_PROFILES];
    int  rowProfile[MAX_PROFILES];   /* each row's index in list */
    int  rowCount;
} UninstallDialog;

static INT_PTR CALLBACK UninstallProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    UninstallDialog *state = (UninstallDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    HWND list;
    WCHAR text[LABEL_CCH + MAX_PATH + 256];
    LVCOLUMNW column;
    LVITEMW item;
    int i;

    switch (message) {
    case WM_INITDIALOG:
        state = (UninstallDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetKeepText(dialog, state->list);
        list = state->rows = GetDlgItem(dialog, IDC_U_LIST);
        ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(list, 0, &column);   /* the view it scrolls in gives it the list's width */
        state->rowCount = 0;
        for (i = 0; i < state->list->count; i++) {
            const Profile *p = &state->list->items[i];
            WCHAR target[MAX_PATH];
            if (p->isStock) continue;
            if (Profiles_LinkTarget(p, target, ARRAYSIZE(target)))
                StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (linked folder, kept: %s)"), p->name, target);
            else
                StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (%%APPDATA%%\\%s)"), p->name, p->folder);
            ZeroMemory(&item, sizeof item);
            item.mask = LVIF_TEXT;
            item.iItem = state->rowCount;
            item.pszText = text;
            ListView_InsertItem(list, &item);
            ListView_SetCheckState(list, state->rowCount, TRUE);
            state->rowProfile[state->rowCount++] = i;
        }
        list = Theme_SmoothView(list);   /* from here, the view that has its place */
        if (state->rowCount == 0) {   /* nothing to keep: their rows close (Theme_FitDialog) */
            ShowWindow(GetDlgItem(dialog, IDC_U_LABEL), SW_HIDE);
            ShowWindow(list, SW_HIDE);
            ShowWindow(GetDlgItem(dialog, IDC_U_HINT), SW_HIDE);
        }
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);   /* Ui_Dialog fits the dialog after this message */
        return TRUE;

    case WM_DPICHANGED:
        PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);
        break;

    case WM_APP_FIT_NAMES:
        if (state) SetKeepText(dialog, state->list);
        return TRUE;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_U_HINT);

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wp) == IDOK) {
            WCHAR ask[2048] = L"";
            int drop = 0, links = 0;
            list = state->rows;
            ZeroMemory(state->removeData, sizeof state->removeData);
            for (i = 0; i < state->rowCount; i++) {
                int index = state->rowProfile[i];
                state->removeData[index] = !ListView_GetCheckState(list, i);
                if (!state->removeData[index]) continue;
                if (Profiles_IsLinked(&state->list->items[index])) links++;
                else drop++;
            }
            if (drop > 0)
                StringCchPrintfW(ask, ARRAYSIZE(ask), drop == 1
                    ? TR(L"The data of %d profile will be moved to the Recycle Bin: its sign-in, local history, Claude Code and Cowork files.\n\n")
                    : TR(L"The data of %d profiles will be moved to the Recycle Bin: their sign-in, local history, Claude Code and Cowork files.\n\n"),
                    drop);
            if (links > 0) {
                WCHAR more[1024];
                StringCchPrintfW(more, ARRAYSIZE(more), links == 1
                    ? TR(L"%d linked profile will be removed from " APP_NAME L"; the folder it links to stays on disk.\n\n")
                    : TR(L"%d linked profiles will be removed from " APP_NAME L"; the folders they link to stay on disk.\n\n"),
                    links);
                StringCchCatW(ask, ARRAYSIZE(ask), more);
            }
            if (drop + links > 0) {
                StringCchCatW(ask, ARRAYSIZE(ask), TR(L"Continue?"));
                if (Ui_Message(dialog, MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2, L"%s", ask) != IDOK) return TRUE;
            }
            EndDialog(dialog, IDOK);
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

/* ---------------------------------------------------------------- actions */

/* What Open, Quit, Restart and Sync do, on a thread of their own: the
 * profiles' Claude quit, their sessions made the same (its progress shown in
 * the header), then opened. A profile of a group gets the others' changes
 * only while it is closed: Restart is the way to give them to one open. */
typedef enum JobKind {
    JOB_OPEN, JOB_QUIT, JOB_RESTART,
    JOB_SYNC,   /* asked for: what it did is said */
    JOB_KEEP    /* the manager opened, or was repaired: only what failed is said */
} JobKind;

typedef struct Job {
    HWND          dialog;
    JobKind       kind;
    ClaudePackage pkg;
    ProfileList   profiles;
    DWORD         chosen;      /* the profiles it is for */
    DWORD         notQuit;     /* ... whose Claude could not be closed */
    DWORD         quitError;
    int           notOpened;
    HRESULT       openError;   /* the first start that failed */
    SyncReport    report;
} Job;

static void JobProgress(void *context, int done, int total)
{
    const Job *job = (const Job *)context;
    PostMessageW(job->dialog, WM_APP_SYNC_PROGRESS, (WPARAM)done, (LPARAM)max(total, 1));
}

static DWORD WINAPI JobThread(void *parameter)
{
    Job *job = (Job *)parameter;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    DWORD synced = job->chosen;
    int i;
    if (job->kind == JOB_QUIT || job->kind == JOB_RESTART)
        for (i = 0; i < job->profiles.count; i++) {
            const Profile *p = &job->profiles.items[i];
            DWORD error = ERROR_SUCCESS;
            if (!(job->chosen & (1u << i)) || !Claude_IsRunning(p)) continue;
            Taskbar_QuitComing(p);
            if (!Claude_Quit(p, &error)) {
                job->notQuit |= 1u << i;
                if (!job->quitError) job->quitError = error;
            }
        }
    /* Opened: one open already only comes forward, its sessions as they are. */
    if (job->kind == JOB_OPEN)
        for (i = 0; i < job->profiles.count; i++)
            if ((synced & (1u << i)) && Claude_IsRunning(&job->profiles.items[i])) synced &= ~(1u << i);
    synced &= ~job->notQuit;
    if (synced) SessionVault_KeepGroups(&job->profiles, synced, JobProgress, job, &job->report);
    if (job->kind == JOB_OPEN || job->kind == JOB_RESTART)
        for (i = 0; i < job->profiles.count; i++) {
            BOOL identity = FALSE;
            HRESULT hr;
            if (!(job->chosen & (1u << i)) || (job->notQuit & (1u << i))) continue;
            hr = Launcher_OpenSynced(&job->pkg, &job->profiles.items[i], NULL, &identity);
            if (FAILED(hr)) {
                if (!job->notOpened++) job->openError = hr;
            } else {
                Util_Log(L"opened %s from the manager%s", job->profiles.items[i].folder, identity ? L"" : L" without package identity");
            }
        }
    if (SUCCEEDED(com)) CoUninitialize();
    if (!PostMessageW(job->dialog, WM_APP_JOB_DONE, 0, (LPARAM)job)) HeapFree(GetProcessHeap(), 0, job);
    return 0;
}

/* The column's foot shows a sync's progress in place of the status, until it ends. */
static void ShowProgress(int done, int total)
{
    HWND bar = GetDlgItem(g_manager.dlg, IDC_PROGRESS);
    g_manager.progressDone = done;
    g_manager.progressTotal = total;
    if (!g_manager.progressShown) {
        g_manager.progressShown = TRUE;
        ShowWindow(GetDlgItem(g_manager.dlg, IDC_STATUS), SW_HIDE);
        ShowWindow(bar, SW_SHOW);
    }
    InvalidateRect(bar, NULL, FALSE);
}

static void HideProgress(void)
{
    if (!g_manager.progressShown) return;
    g_manager.progressShown = FALSE;
    ShowWindow(GetDlgItem(g_manager.dlg, IDC_PROGRESS), SW_HIDE);
    ShowWindow(GetDlgItem(g_manager.dlg, IDC_STATUS), SW_SHOW);
}

static void DrawProgress(const DRAWITEMSTRUCT *item)
{
    WCHAR text[ARRAYSIZE(g_manager.progressText) + 64];
    if (g_manager.progressTotal > 0)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s %d of %d"), g_manager.progressText, min(g_manager.progressDone, g_manager.progressTotal),
                         g_manager.progressTotal);
    else
        StringCchCopyW(text, ARRAYSIZE(text), g_manager.progressText);
    Theme_DrawProgress(item->hwndItem, item->hDC, &item->rcItem, g_manager.progressDone, g_manager.progressTotal, text);
}

/* `kind` for the profiles of `chosen`, on its thread; one at a time. */
static void RunJob(JobKind kind, DWORD chosen)
{
    Job *job;
    HANDLE thread;
    if (g_manager.job || !chosen || StateChangesBlocked()) return;
    if ((kind == JOB_OPEN || kind == JOB_RESTART) && !g_manager.pkg.found) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"Claude Desktop is not installed."));
        return;
    }
    if ((job = (Job *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *job)) == NULL) return;
    job->dialog = g_manager.dlg;
    job->kind = kind;
    job->pkg = g_manager.pkg;
    job->profiles = g_manager.profiles;
    job->chosen = chosen;
    FinishShellWork();
    if ((thread = CreateThread(NULL, 0, JobThread, job, 0, NULL)) == NULL) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Sessions could not be loaded."));
        HeapFree(GetProcessHeap(), 0, job);
        return;
    }
    g_manager.job = thread;
    StringCchCopyW(g_manager.progressText, ARRAYSIZE(g_manager.progressText),
                   kind == JOB_QUIT || kind == JOB_RESTART ? TR(L"Quitting Claude, then keeping sessions the same\x2026")
                                                           : TR(L"Keeping sessions the same\x2026"));
    ShowProgress(0, 0);
    UpdateButtons();
}

/* What a job left undone is said; a sync asked for says what it did. */
static void JobDone(Job *job)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)];
    if (g_manager.job) {
        WaitForSingleObject(g_manager.job, JOB_END_WAIT_MS);   /* it ends right after posting */
        CloseHandle(g_manager.job);
        g_manager.job = NULL;
    }
    HideProgress();
    if (!StateChangesBlocked()) {
        Refresh(FALSE);
        SessionsView_Reload();
        UpdateButtons();
        if (job->notQuit) {
            JoinNames(job->notQuit, names, ARRAYSIZE(names));
            Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), names, job->quitError);
        }
        if (job->notOpened)
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Claude could not be started (error 0x%08lX)."), (unsigned long)job->openError);
        if (job->kind == JOB_SYNC || job->report.failed || job->report.forked)
            SyncUi_ShowReport(g_manager.dlg, &job->profiles, &job->report, NULL);
    }
    HeapFree(GetProcessHeap(), 0, job);
    PostMessageW(g_manager.dlg, WM_APP_SYNC_CONFLICTS, 0, 0);   /* the ones it left, or that waited for it */
}

static DWORD AllProfiles(void)
{
    return g_manager.profiles.count >= 32 ? (DWORD)-1 : (1u << g_manager.profiles.count) - 1;
}

/* The profiles selected whose Claude runs now (read when asked: nothing watches it). */
static DWORD RunningOf(DWORD selected)
{
    DWORD running = 0;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && Claude_IsRunning(&g_manager.profiles.items[i])) running |= 1u << i;
    return running;
}

/* Every profile selected opens, its sessions made the same first. */
static void DoOpen(void)
{
    RunJob(JOB_OPEN, SelectedProfiles());
}

/* Quit, or Restart: the Claude of every profile selected that runs, as its
 * own Quit does, once the user agreed (its windows close, and what runs in
 * it stops); then their sessions are made the same, and with Restart they
 * open again. Restart opens the ones closed too. */
static void DoQuit(BOOL restart)
{
    WCHAR text[512 + MAX_PROFILES * (LABEL_CCH + 8)], names[MAX_PROFILES * (LABEL_CCH + 8)];
    DWORD selected = SelectedProfiles(), running = RunningOf(selected);
    if (!running) {
        const Profile *p = FocusedProfile();
        if (restart) RunJob(JOB_RESTART, selected);
        else if (p) Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(L"Claude is not open for \x201C%s\x201D."), p->name);
        return;
    }
    JoinNames(running, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text),
                     restart ? TR(L"Restart Claude for %s?\n\nIts windows close, and the Claude Code and Cowork sessions running in it stop; "
                                  L"the sessions are made the same, then it opens again.")
                             : TR(L"Quit Claude for %s?\n\nIts windows close, and the Claude Code and Cowork sessions running in it stop."),
                     names);
    if (!Ui_Ask(g_manager.dlg, IDI_QUESTION, text, restart ? TR(L"Restart") : TR(L"Quit"), TR(L"Cancel"), FALSE)) return;
    RunJob(restart ? JOB_RESTART : JOB_QUIT, restart ? selected : running);
}

/* ----------------------------------------------------- sessions kept the same */

/* The profiles of `members` sync `items` (SYNC_ITEM_*) with each other from now on. */
static void SetGroupItems(DWORD members, DWORD items)
{
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((members & (1u << i)) && !Profiles_SetSyncItems(g_manager.profiles.items[i].folder, items))
            Util_Log(L"could not keep what %s syncs (error %lu)", g_manager.profiles.items[i].folder, GetLastError());
    Refresh(FALSE);
}

/* A group left with one profile keeps nothing the same: it is no group. */
static void DropLoneGroups(void)
{
    int group, i;
    for (group = 1; group <= MAX_PROFILES; group++) {
        DWORD members = SessionVault_Group(&g_manager.profiles, group);
        if (!members || (members & (members - 1))) continue;
        for (i = 0; !(members & (1u << i)); i++) {}
        Profiles_SetSyncGroup(g_manager.profiles.items[i].folder, 0);
    }
}

/* The profiles of `bits` keep their sessions as `choice` says (0: apart; a
 * group; -1 - i: the same as profile i, in its group or a new one). Returns
 * the group they joined, 0 for none. */
static int SetSessionsChoice(DWORD bits, int choice)
{
    int group = choice, i;
    DWORD joined, items;
    if (choice < 0) {
        int other = -1 - choice;
        if (other >= g_manager.profiles.count) return 0;
        group = g_manager.profiles.items[other].syncGroup ? g_manager.profiles.items[other].syncGroup : SessionVault_NewGroup(&g_manager.profiles);
        if (!group) return 0;
        bits |= 1u << other;
    }
    /* A profile joining syncs what the others there do. */
    if (group && (joined = SessionVault_Group(&g_manager.profiles, group) & ~bits) != 0) {
        items = SessionVault_GroupItems(&g_manager.profiles, joined);
        for (i = 0; i < g_manager.profiles.count; i++)
            if (bits & (1u << i)) Profiles_SetSyncItems(g_manager.profiles.items[i].folder, items);
    }
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((bits & (1u << i)) && g_manager.profiles.items[i].syncGroup != group &&
            !Profiles_SetSyncGroup(g_manager.profiles.items[i].folder, group))
            Util_Log(L"could not keep the sessions of %s in group %d (error %lu)", g_manager.profiles.items[i].folder, group, GetLastError());
    Refresh(FALSE);
    DropLoneGroups();
    Refresh(FALSE);
    return group;
}

/* ------------------------------------------------------------ the profiles */

static int FreeColor(void)
{
    BOOL used[PALETTE_SIZE] = { 0 };
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if (g_manager.profiles.items[i].color >= 0 && g_manager.profiles.items[i].color < PALETTE_SIZE)
            used[g_manager.profiles.items[i].color] = TRUE;
    for (i = 0; i < PALETTE_SIZE; i++)
        if (!used[i]) return i;
    return g_manager.profiles.count % PALETTE_SIZE;
}

static void SetProfileStartup(const Profile *profile, BOOL enabled)
{
    if (FAILED(Shortcut_SetStartup(&g_manager.pkg, profile, enabled)))
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The Windows sign-in setting of \x201C%s\x201D could not be changed."), profile->name);
}

static void DoNew(void)
{
    ProfileDialog dialog;
    Profile source, created;
    WCHAR folder[FOLDER_CCH], error[256];
    int i;
    if (g_manager.profiles.count >= MAX_PROFILES) {
        Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(APP_NAME L" handles up to %d profiles."), MAX_PROFILES);
        return;
    }
    ZeroMemory(&dialog, sizeof dialog);
    dialog.color = FreeColor();
    dialog.openNow = g_manager.pkg.found;
    /* Settings come from the selected profile, else the default one. */
    ZeroMemory(&source, sizeof source);
    if (SelectedProfile()) source = *SelectedProfile();
    else if ((i = Profiles_DefaultIndex(&g_manager.profiles)) >= 0) source = g_manager.profiles.items[i];
    dialog.copyFrom = source.folder[0] ? &source : NULL;
    if (Ui_Dialog(g_manager.dlg, IDD_PROFILE, ProfileProc, (LPARAM)&dialog) != IDOK || StateChangesBlocked()) {
        if (dialog.picture) HeapFree(GetProcessHeap(), 0, dialog.picture);
        return;
    }
    FinishShellWork();
    if (!Profiles_Create(dialog.name, dialog.color, folder, ARRAYSIZE(folder), error, ARRAYSIZE(error))) {
        if (dialog.picture) HeapFree(GetProcessHeap(), 0, dialog.picture);
        Ui_Message(g_manager.dlg, MB_ICONWARNING, L"%s", error);
        return;
    }
    if (dialog.badge[0] || dialog.picture) {
        DWORD stamp = 0;
        if (dialog.picture && !Icons_SavePicture(folder, dialog.picture, &stamp))
            Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The picture could not be saved: the profile shows the Claude icon."));
        if (!Profiles_Update(folder, dialog.name, dialog.color, dialog.badge, stamp))
            Util_Log(L"could not save the badge and picture of %s (error %lu)", folder, GetLastError());
    }
    if (dialog.picture) HeapFree(GetProcessHeap(), 0, dialog.picture);
    Refresh(FALSE);
    i = Profiles_Find(&g_manager.profiles, folder);
    if (i < 0) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The profile was created but cannot be listed: " APP_NAME L" handles up to %d profiles."), MAX_PROFILES);
        return;
    }
    /* It gets the group's sessions once it is signed in. */
    if (dialog.sync) {
        int other, group;
        /* The list was read again: the profile chosen is found by its folder. */
        if (dialog.sync < 0) dialog.sync = (other = Profiles_Find(&g_manager.profiles, dialog.syncFolder)) >= 0 ? -1 - other : 0;
        if (dialog.sync && (group = SetSessionsChoice(1u << i, dialog.sync)) != 0 && dialog.syncItems)
            SetGroupItems(SessionVault_Group(&g_manager.profiles, group), dialog.syncItems);
        if ((i = Profiles_Find(&g_manager.profiles, folder)) < 0) return;
    }
    SelectProfileRow(folder);
    ShowChanges();
    created = g_manager.profiles.items[i];
    if (dialog.copy) Profiles_CopySettings(&source, &created);
    if (dialog.startup) SetProfileStartup(&created, TRUE);
    if (dialog.openNow && !StateChangesBlocked()) RunJob(JOB_OPEN, 1u << i);
}

/* Profiles that will keep the same sessions (`together`), asked first: what
 * keeping them the same does, and the folders it writes. */
static BOOL ConfirmKeepSame(DWORD together)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 16)], question[1024 + ARRAYSIZE(names)];
    JoinNames(together, names, ARRAYSIZE(names));
    StringCchPrintfW(question, ARRAYSIZE(question),
                     TR(L"Keep the sessions of %s the same?\n\nFrom then on, each time one of them closes or opens, what changed in one goes to "
                        L"the others: new sessions, titles, stars, pins and groups, archived and deleted ones. A session two of them went on "
                        L"with apart is kept as two. One that loses its list, when Claude is reinstalled, gets it back."),
                     names);
    return SyncUi_ConfirmSessions(g_manager.dlg, question, &g_manager.profiles, together, TRUE, TRUE, TR(L"Keep the same"));
}

/* Sync sessions: the sessions that change are those of the groups of the
 * profiles selected; asked first, with their folders. Profiles in no group
 * change nothing (only the vault keeps a version of their lists). */
static BOOL ConfirmSync(DWORD selected)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 16)], question[256 + ARRAYSIZE(names)];
    DWORD changed = 0;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && g_manager.profiles.items[i].syncGroup)
            changed |= SessionVault_Group(&g_manager.profiles, g_manager.profiles.items[i].syncGroup);
    if (!(changed & (changed - 1))) return TRUE;
    JoinNames(changed, names, ARRAYSIZE(names));
    StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Make the sessions of %s the same now?"), names);
    return SyncUi_ConfirmSessions(g_manager.dlg, question, &g_manager.profiles, changed, TRUE, TRUE, TR(L"Sync"));
}

/* The profiles of `folders` keep their sessions as a profile dialog chose
 * (`sync`; a profile chosen by its folder, `syncFolder`), syncing `items`
 * (0: what they did), made the same at once when they joined a group. */
static void ApplySessionsChoice(WCHAR (*folders)[FOLDER_CCH], int count, int sync, const WCHAR *syncFolder, DWORD items)
{
    DWORD bits = 0, changed = 0;
    int i, at, group;
    for (i = 0; i < count; i++)
        if ((at = Profiles_Find(&g_manager.profiles, folders[i])) >= 0) {
            bits |= 1u << at;
            if (sync < 0 || g_manager.profiles.items[at].syncGroup != sync) changed |= 1u << at;
        }
    if (!changed) {
        if (items && sync > 0) SetGroupItems(SessionVault_Group(&g_manager.profiles, sync), items);
        return;
    }
    if (sync < 0 && (sync = (at = Profiles_Find(&g_manager.profiles, syncFolder)) >= 0 ? -1 - at : 0) == 0) return;
    /* Joining a group (of another profile's, or an existing one) changes sessions: asked first. */
    if (sync < 0 && !ConfirmKeepSame(bits | (1u << (-1 - sync)) |
                                     SessionVault_Group(&g_manager.profiles, g_manager.profiles.items[-1 - sync].syncGroup)))
        return;
    if (sync > 0 && !ConfirmKeepSame(bits | SessionVault_Group(&g_manager.profiles, sync))) return;
    if ((group = SetSessionsChoice(bits, sync)) != 0) {
        if (items) SetGroupItems(SessionVault_Group(&g_manager.profiles, group), items);
        RunJob(JOB_SYNC, SessionVault_Group(&g_manager.profiles, group));
    }
}

/* Several profiles edited together: what they share, how their sessions are kept. */
static void DoEditSeveral(DWORD selected)
{
    WCHAR folders[MAX_PROFILES][FOLDER_CCH];
    ProfileDialog dialog;
    int i, count = 0;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.several = selected;
    if (Ui_Dialog(g_manager.dlg, IDD_PROFILE, ProfileProc, (LPARAM)&dialog) != IDOK || StateChangesBlocked()) return;
    for (i = 0; i < g_manager.profiles.count; i++)
        if (selected & (1u << i)) StringCchCopyW(folders[count++], FOLDER_CCH, g_manager.profiles.items[i].folder);
    ApplySessionsChoice(folders, count, dialog.sync, dialog.syncFolder, dialog.syncItems);
}

static void DoEdit(void)
{
    const Profile *selected = SelectedProfile();
    Profile before, after;
    ProfileDialog dialog;
    WCHAR icon[MAX_PATH], folder[1][FOLDER_CCH];
    BOOL atStartup, saved;
    DWORD picture, several = SelectedProfiles();
    if (several & (several - 1)) {
        DoEditSeveral(several);
        return;
    }
    if (!selected) return;
    before = *selected;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.existing = &before;
    dialog.color = before.color;
    dialog.startup = atStartup = Shortcut_IsAtStartup(&before);
    if (Ui_Dialog(g_manager.dlg, IDD_PROFILE, ProfileProc, (LPARAM)&dialog) != IDOK || StateChangesBlocked()) {
        if (dialog.picture) HeapFree(GetProcessHeap(), 0, dialog.picture);
        return;
    }
    FinishShellWork();
    picture = before.picture;
    if (dialog.pictureChanged && !dialog.picture) picture = 0;
    else if (dialog.pictureChanged && !Icons_SavePicture(before.folder, dialog.picture, &picture)) {
        picture = before.picture;
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The picture could not be saved: the profile keeps its icon."));
    }
    if (dialog.picture) HeapFree(GetProcessHeap(), 0, dialog.picture);
    saved = Profiles_Update(before.folder, dialog.name, dialog.color, dialog.badge, picture);
    if (!saved) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The profile could not be saved."));
        return;
    }
    if (!picture && before.picture) Icons_DeletePicture(before.folder);
    after = before;
    StringCchCopyW(after.name, ARRAYSIZE(after.name), dialog.name);
    after.color = dialog.color;
    StringCchCopyW(after.badge, ARRAYSIZE(after.badge), dialog.badge);
    after.picture = picture;
    /* The list shows the new name and icon first; its shortcuts and pins follow. */
    Refresh(FALSE);
    ShowChanges();
    if ((wcscmp(before.name, after.name) != 0 || before.color != after.color || wcscmp(before.badge, after.badge) != 0 ||
         before.picture != after.picture) &&
        Icons_Ensure(&g_manager.pkg, &after, icon, ARRAYSIZE(icon)))
        ApplyBadge(&before, &after, icon);
    if (dialog.startup != atStartup) SetProfileStartup(&after, dialog.startup);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
    StringCchCopyW(folder[0], FOLDER_CCH, before.folder);
    ApplySessionsChoice(folder, 1, dialog.sync, dialog.syncFolder, dialog.syncItems);
}

static BOOL ConfirmDelete(const Profile *p)
{
    WCHAR text[512 + 3 * MAX_PATH], target[MAX_PATH], shownTarget[MAX_PATH], folder[MAX_PATH];
    SyncUi_ShortPath(p->storageDir[0] ? p->storageDir : p->dataDir, folder, ARRAYSIZE(folder));
    if (Profiles_LinkTarget(p, target, ARRAYSIZE(target))) {
        SyncUi_ShortPath(target, shownTarget, ARRAYSIZE(shownTarget));
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Delete the profile \x201C%s\x201D?\n\nIts folder\n    %s\nis a link to\n    %s\nThe data in that folder stays on disk. The link, "
                            L"the profile's shortcuts and Claude's local files for it (logs and cache) are removed."),
                         p->name, folder, shownTarget);
    } else {
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Delete the profile \x201C%s\x201D?\n\nIts data folder goes to the Recycle Bin, with its sign-in, local history, Claude "
                            L"Code and Cowork files:\n    %s\nIts shortcuts are removed."),
                         p->name, folder);
    }
    return Ui_Ask(g_manager.dlg, IDI_WARNING, text, TR(L"Delete profile"), TR(L"Cancel"), TRUE);
}

static BOOL RefuseIfRunning(const Profile *p)
{
    if (!Claude_IsRunning(p)) return FALSE;
    Ui_Message(g_manager.dlg, MB_ICONWARNING,
               TR(L"Quit Claude for the \x201C%s\x201D profile first (right-click its icon in the notification area and choose Quit), then try again."),
               p->name);
    return TRUE;
}

/* Several profiles selected: one question for all of them, then each one
 * deleted as one alone is. One whose Claude runs is not deleted, the user
 * told. */
static void DeleteSeveral(DWORD bits)
{
    ListSelection chosen;
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], text[ARRAYSIZE(names) + 1024 + MAX_PROFILES * (MAX_PATH + 8)];
    int i, p, seen = 0, total = 0;
    ZeroMemory(&chosen, sizeof chosen);
    names[0] = 0;
    for (i = 0; i < g_manager.profiles.count; i++)
        if (bits & (1u << i)) total++;
    for (i = 0; i < g_manager.profiles.count; i++) {
        if (!(bits & (1u << i))) continue;
        if (RefuseIfRunning(&g_manager.profiles.items[i])) return;
        if (seen) StringCchCatW(names, ARRAYSIZE(names), seen == total - 1 ? TR(L" and ") : TR(L", "));
        StringCchCatW(names, ARRAYSIZE(names), g_manager.profiles.items[i].name);
        StringCchCopyW(chosen.folders[chosen.count++], ARRAYSIZE(chosen.folders[0]), g_manager.profiles.items[i].folder);
        seen++;
    }
    StringCchPrintfW(text, ARRAYSIZE(text),
                     TR(L"Delete the profiles %s?\n\nTheir data folders go to the Recycle Bin, with their sign-in, local history, Claude Code "
                        L"and Cowork files (a folder that is a link stays on disk):"),
                     names);
    for (i = 0; i < g_manager.profiles.count; i++)
        if (bits & (1u << i))
            SyncUi_AddPath(text, ARRAYSIZE(text), g_manager.profiles.items[i].storageDir[0] ? g_manager.profiles.items[i].storageDir
                                                                                             : g_manager.profiles.items[i].dataDir);
    SyncUi_AddLine(text, ARRAYSIZE(text), TR(L"Their shortcuts are removed."), FALSE);
    if (!Ui_Ask(g_manager.dlg, IDI_WARNING, text, TR(L"Delete profiles"), TR(L"Cancel"), TRUE) || StateChangesBlocked()) return;
    FinishShellWork();
    SessionsView_PauseWatching();
    for (i = 0; i < chosen.count && !StateChangesBlocked(); i++) {
        Profile profile;
        if ((p = Profiles_Find(&g_manager.profiles, chosen.folders[i])) < 0) continue;
        profile = g_manager.profiles.items[p];
        /* It may have been started while the question was open. */
        if (RefuseIfRunning(&profile)) continue;
        if (Profiles_Delete(g_manager.dlg, &profile) == REMOVE_FAILED)
            Ui_Message(g_manager.dlg, MB_ICONWARNING,
                       TR(L"\x201C%s\x201D could not be deleted completely. Make sure Claude is closed for this profile and try again."),
                       profile.name);
    }
    SessionsView_ResumeWatching();
    Refresh(FALSE);
}

/* The profiles selected, Claude's own apart: one alone is asked about with
 * what its folder holds, several together. */
static void DoDelete(void)
{
    DWORD chosen = Deletable(SelectedProfiles());
    Profile p;
    int i;
    if (!chosen) return;
    if (chosen & (chosen - 1)) {
        DeleteSeveral(chosen);
        return;
    }
    for (i = 0; !(chosen & (1u << i)); i++) {}
    p = g_manager.profiles.items[i];
    if (RefuseIfRunning(&p)) return;
    if (!ConfirmDelete(&p) || StateChangesBlocked()) return;
    /* It may have been started while the question was open. */
    if (RefuseIfRunning(&p)) {
        Refresh(FALSE);
        return;
    }
    FinishShellWork();
    SessionsView_PauseWatching();
    /* REMOVE_CANCELLED (the user answered No to Windows' "delete
     * permanently?") needs no message. */
    if (Profiles_Delete(g_manager.dlg, &p) == REMOVE_FAILED)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"\x201C%s\x201D could not be deleted completely. Make sure Claude is closed for this profile and try again."),
                   p.name);
    SessionsView_ResumeWatching();
    Refresh(FALSE);
}

/* The registry watch reloads the profiles after the write (WatchOutside):
 * the change shows here at once, without a second reload. */
static void DoSetDefault(void)
{
    const Profile *selected = SelectedProfile();
    if (!selected) return;
    if (!Profiles_SetDefault(selected->folder)) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The default profile could not be changed."));
        return;
    }
    StringCchCopyW(g_manager.profiles.defaultFolder, ARRAYSIZE(g_manager.profiles.defaultFolder), selected->folder);
    UpdateRowsAndButtons();
    SessionsView_SetProfiles(&g_manager.profiles);
}

static void CreateShortcutAt(const Profile *p, const WCHAR *path)
{
    HRESULT hr;
    FinishShellWork();
    hr = Shortcut_CreateForProfile(&g_manager.pkg, p, path);
    if (FAILED(hr))
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
}

/* A desktop shortcut for every profile selected that has none. */
static void DoDesktopShortcut(void)
{
    ListSelection selection;
    WCHAR path[MAX_PATH];
    int i, p;
    TakeSelection(&selection);
    for (i = 0; i < selection.count && !StateChangesBlocked(); i++) {
        Profile profile;
        if ((p = Profiles_Find(&g_manager.profiles, selection.folders[i])) < 0) continue;
        profile = g_manager.profiles.items[p];
        if (selection.count > 1 && Shortcut_IsOnDesktop(&profile)) continue;
        if (!Shortcut_DesktopPathFor(&profile, path, ARRAYSIZE(path))) {
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The desktop folder is not available."));
            return;
        }
        CreateShortcutAt(&profile, path);
    }
}

/* Every profile selected pinned to the taskbar. */
static void DoPinTaskbar(void)
{
    ListSelection selection;
    HRESULT hr;
    int i, p;
    TakeSelection(&selection);
    FinishShellWork();
    for (i = 0; i < selection.count && !StateChangesBlocked(); i++) {
        Profile profile;
        if ((p = Profiles_Find(&g_manager.profiles, selection.folders[i])) < 0) continue;
        profile = g_manager.profiles.items[p];
        if (selection.count > 1 && TaskbarPin_IsPinned(&profile)) continue;
        hr = TaskbarPin_Pin(&g_manager.pkg, &profile);
        if (FAILED(hr))
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"\x201C%s\x201D could not be pinned to the taskbar (error 0x%08lX)."),
                       profile.name, (unsigned long)hr);
    }
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
}

/* ------------------------------------------------------------- sessions */

/* The sessions dialogs work on a copy of the profiles: the list can be
 * read again while one is open. */
static ProfileList *CopyProfiles(void)
{
    ProfileList *copy = (ProfileList *)HeapAlloc(GetProcessHeap(), 0, sizeof *copy);
    if (copy) *copy = g_manager.profiles;
    else Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Sessions could not be loaded."));
    return copy;
}

typedef enum SessionsAction {
    SESSIONS_MERGE, SESSIONS_COPY_ALL, SESSIONS_MOVE_ALL, SESSIONS_EXPORT, SESSIONS_IMPORT, SESSIONS_RESTORE, SESSIONS_PURGE
} SessionsAction;

/* What the sessions menu and the list's menu do; what changed shows in the
 * sessions view. */
static void DoSessions(SessionsAction action)
{
    const Profile *focused = FocusedProfile();
    WCHAR folder[FOLDER_CCH] = L"";
    DWORD selected = SelectedProfiles();
    ProfileList *profiles;
    BOOL changed = FALSE;
    if (StateChangesBlocked() || (profiles = CopyProfiles()) == NULL) return;
    if (focused) StringCchCopyW(folder, ARRAYSIZE(folder), focused->folder);
    switch (action) {
    case SESSIONS_MERGE:    changed = SyncUi_Merge(g_manager.dlg, profiles); break;
    case SESSIONS_COPY_ALL: changed = SyncUi_CopyAll(g_manager.dlg, profiles, selected, FALSE); break;
    case SESSIONS_MOVE_ALL: changed = SyncUi_CopyAll(g_manager.dlg, profiles, selected, TRUE); break;
    case SESSIONS_EXPORT:   SyncUi_ExportProfiles(g_manager.dlg, profiles, selected); break;
    case SESSIONS_IMPORT:   changed = SyncUi_Import(g_manager.dlg, profiles, selected); break;
    case SESSIONS_RESTORE:  changed = SyncUi_Restore(g_manager.dlg, profiles, folder[0] ? folder : NULL); break;
    case SESSIONS_PURGE:    changed = SyncUi_Purge(g_manager.dlg, profiles); break;
    }
    HeapFree(GetProcessHeap(), 0, profiles);
    if (changed && !StateChangesBlocked()) SessionsView_Reload();
}

#define IDM_LIST_EXPORT     0x6005
#define IDM_LIST_IMPORT     0x6006
#define IDM_LIST_UNLINK     0x6008
#define IDM_LIST_BACKUP     0x6009
#define IDM_LIST_RESTORE    0x600A
#define IDM_EXIT            0x600B   /* the program menu: closes the window, as Esc does outside a search box */
#define IDM_LANGUAGE        0x6020   /* + 1 + a language; itself: Windows' */
#define IDM_LIST_SAME_FIRST 0x6100   /* + the index of the profile whose sessions the ones selected keep */

/* TRUE (and says so) when the profile, or one sharing its session folder, runs: its sessions are in Claude's memory. */
static BOOL LinkBusy(const Profile *p)
{
    if (!SessionLink_Busy(p)) return FALSE;
    Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
    return TRUE;
}

/* `error` on a line of its own after what `errors` says already. */
static void AddError(WCHAR *errors, size_t cch, const WCHAR *error)
{
    if (errors[0]) StringCchCatW(errors, cch, L"\n\n");
    StringCchCatW(errors, cch, error);
}

/* The profiles of `selected` whose session folder is a link. */
static DWORD LinkedProfiles(DWORD selected)
{
    LinkState state;
    DWORD linked = 0;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++) {
        if (!(selected & (1u << i))) continue;
        SessionLink_Read(&g_manager.profiles, i, &state);
        if (state.kind == LINK_PROFILE || state.kind == LINK_FOLDER || state.kind == LINK_BROKEN) linked |= 1u << i;
    }
    return linked;
}

/* The profiles selected whose session folder is linked get one of their own again. */
static void DoUnlink(void)
{
    WCHAR text[1024], folders[CONFIRM_CCH], error[512 + 2 * LONG_PATH_CCH], errors[4 * (512 + 2 * LONG_PATH_CCH)];
    const Profile *first = NULL;
    DWORD linked = LinkedProfiles(SelectedProfiles());
    int i, count = 0;
    if (!linked || StateChangesBlocked()) return;
    for (i = 0; i < g_manager.profiles.count; i++)
        if (linked & (1u << i)) {
            if (LinkBusy(&g_manager.profiles.items[i])) return;
            if (!count++) first = &g_manager.profiles.items[i];
        }
    if (count == 1)
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Give \x201C%s\x201D a session folder of its own again?\n\nIt keeps the sessions the shared folder lists now; "
                            L"from then on the two lists go apart."),
                         first->name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Give the %d linked profiles selected session folders of their own again?\n\nEach one keeps the sessions its shared folder lists now; "
                            L"from then on the lists go apart."),
                         count);
    folders[0] = 0;
    SyncUi_AddLine(folders, ARRAYSIZE(folders), TR(L"These folders become folders of their own:"), FALSE);
    for (i = 0; i < g_manager.profiles.count; i++) {
        WCHAR folder[LONG_PATH_CCH];
        const Profile *p = &g_manager.profiles.items[i];
        if ((linked & (1u << i)) &&
            SUCCEEDED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\" CLAUDE_ENTRIES_DIR, p->storageDir[0] ? p->storageDir : p->dataDir)))
            SyncUi_AddPath(folders, ARRAYSIZE(folders), folder);
    }
    if (!SyncUi_Confirm(g_manager.dlg, NULL, text, folders, FALSE, TR(L"Unlink"))) return;
    FinishShellWork();
    errors[0] = 0;
    SessionsView_PauseWatching();
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((linked & (1u << i)) && !SessionLink_Remove(&g_manager.profiles, i, error, ARRAYSIZE(error)))
            AddError(errors, ARRAYSIZE(errors), error);
    SessionsView_ResumeWatching();
    if (errors[0]) Ui_Message(g_manager.dlg, MB_ICONWARNING, L"%s", errors);
    Refresh(FALSE);
}

/* The profiles of `bits` by name: "Main and Work", "Main, Work and Test". */
static void JoinNames(DWORD bits, WCHAR *out, size_t cch)
{
    int i, total = 0, seen = 0;
    for (i = 0; i < g_manager.profiles.count; i++)
        if (bits & (1u << i)) total++;
    out[0] = 0;
    for (i = 0; i < g_manager.profiles.count; i++) {
        if (!(bits & (1u << i))) continue;
        if (seen) StringCchCatW(out, cch, seen == total - 1 ? TR(L" and ") : TR(L", "));
        StringCchCatW(out, cch, g_manager.profiles.items[i].name);
        seen++;
    }
}

/* Any profile of `selected` whose sessions are kept the same as others'. */
static BOOL SameSelected(DWORD selected)
{
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && g_manager.profiles.items[i].syncGroup) return TRUE;
    return FALSE;
}

/* The sessions of the profiles selected kept the same as those of profile
 * `other` (an index), and of its group, from now on (sessionvault.c), at
 * once. A session folder of theirs still linked (Claude no longer writes
 * through a link) becomes their own again first, which needs them closed. */
static void DoKeepSame(int other)
{
    WCHAR error[512 + 2 * LONG_PATH_CCH];
    DWORD involved, linked, together;
    int i, group;
    if (StateChangesBlocked() || other < 0 || other >= g_manager.profiles.count) return;
    involved = SelectedProfiles() & ~(1u << other);
    if (!involved) return;
    together = involved | (1u << other) | SessionVault_Group(&g_manager.profiles, g_manager.profiles.items[other].syncGroup);
    if (!ConfirmKeepSame(together)) return;
    linked = LinkedProfiles(together);
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((linked & (1u << i)) && LinkBusy(&g_manager.profiles.items[i])) return;
    FinishShellWork();
    SessionsView_PauseWatching();
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((linked & (1u << i)) && !SessionLink_Remove(&g_manager.profiles, i, error, ARRAYSIZE(error))) {
            SessionsView_ResumeWatching();
            Ui_Message(g_manager.dlg, MB_ICONWARNING, L"%s", error);
            Refresh(FALSE);
            return;
        }
    SessionsView_ResumeWatching();
    if ((group = SetSessionsChoice(involved, -1 - other)) != 0) RunJob(JOB_SYNC, SessionVault_Group(&g_manager.profiles, group));
}

/* The profiles selected keep their sessions as they are, apart from now on. */
static void DoStopSame(void)
{
    if (StateChangesBlocked() || !SameSelected(SelectedProfiles())) return;
    SetSessionsChoice(SelectedProfiles(), 0);
}

/* `target` and the profiles whose session folder is linked to its, one bit each. */
static DWORD SharingProfiles(int target)
{
    LinkState state;
    DWORD sharing = 1u << target;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++) {
        SessionLink_Read(&g_manager.profiles, i, &state);
        if (state.kind == LINK_PROFILE && state.profile == target) sharing |= 1u << i;
    }
    return sharing;
}

/* A linked session folder the user wants made its own, while a profile that
 * shares it runs (its Claude would write the old list back): its Claude quit
 * now, or not; either way the question comes back once they are all closed
 * (AskKeepAgain). */
static void OfferQuitForLink(int target)
{
    WCHAR text[1024];
    DWORD running = RunningOf(SharingProfiles(target));
    int i, first = -1;
    for (i = 0; i < g_manager.profiles.count && first < 0; i++)
        if (running & (1u << i)) first = i;
    g_manager.keepAsked = TRUE;
    if (first < 0) return;   /* closed meanwhile */
    StringCchPrintfW(text, ARRAYSIZE(text),
                     TR(L"\x201C%s\x201D is open. Quit Claude there now? Otherwise this question comes back once it is closed."),
                     g_manager.profiles.items[first].name);
    if (Ui_Ask(g_manager.dlg, IDI_QUESTION, text, TR(L"Quit it"), TR(L"Not now"), FALSE)) RunJob(JOB_QUIT, running);
}

/* The offer KeepSessions could not carry out made again, once no profile
 * sharing a linked session folder runs: after a job, or back from another
 * window. */
static void AskKeepAgain(void)
{
    LinkState state;
    int i;
    if (!g_manager.keepAsked || g_manager.job || StateChangesBlocked()) return;
    for (i = 0; i < g_manager.profiles.count; i++) {
        SessionLink_Read(&g_manager.profiles, i, &state);
        if (state.kind == LINK_PROFILE && (SessionLink_Busy(&g_manager.profiles.items[i]) ||
                                           SessionLink_Busy(&g_manager.profiles.items[state.profile])))
            return;
    }
    g_manager.keepAsked = FALSE;
    PostMessageW(g_manager.dlg, WM_APP_KEEP_SESSIONS, 0, 0);
}

/* What the profiles selected sync with the others they sync with, chosen. */
static void DoSyncItems(void)
{
    DWORD selected = SelectedProfiles(), members = 0, items;
    int i;
    if (StateChangesBlocked()) return;
    for (i = 0; i < g_manager.profiles.count && !members; i++)
        if ((selected & (1u << i)) && g_manager.profiles.items[i].syncGroup)
            members = SessionVault_Group(&g_manager.profiles, g_manager.profiles.items[i].syncGroup);
    if (!members) return;
    items = SessionVault_GroupItems(&g_manager.profiles, members);
    if (SyncUi_ChooseItems(g_manager.dlg, &g_manager.profiles, members, &items)) SetGroupItems(members, items);
}

/* Once the window opened: a session folder linked to another profile's (which
 * Claude no longer writes through) offered a folder of its own, its sessions
 * kept the same as that profile's; then the session lists kept, the ones kept
 * the same made so (sessionvault.c). */
static void KeepSessions(void)
{
    WCHAR text[2048], folder[FOLDER_CCH], otherFolder[FOLDER_CCH], error[512 + 2 * LONG_PATH_CCH];
    LinkState state;
    int i, at;
    for (i = 0; i < g_manager.profiles.count && !StateChangesBlocked(); i++) {
        SessionLink_Read(&g_manager.profiles, i, &state);
        if (state.kind != LINK_PROFILE) continue;
        StringCchCopyW(folder, ARRAYSIZE(folder), g_manager.profiles.items[i].folder);
        StringCchCopyW(otherFolder, ARRAYSIZE(otherFolder), g_manager.profiles.items[state.profile].folder);
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Claude no longer saves the sessions of \x201C%s\x201D: its session folder is linked to the one of \x201C%s\x201D, "
                            L"and Claude refuses to write through a link, so what changes there is lost once it closes.\n\n"
                            L"Give it a folder of its own again, and keep its sessions the same as those of \x201C%s\x201D instead?"),
                         g_manager.profiles.items[i].name, g_manager.profiles.items[state.profile].name, g_manager.profiles.items[state.profile].name);
        if (!Ui_Ask(g_manager.dlg, IDI_WARNING, text, TR(L"Keep the same"), TR(L"Not now"), FALSE)) continue;
        if (SessionLink_Busy(&g_manager.profiles.items[i]) || SessionLink_Busy(&g_manager.profiles.items[state.profile])) {
            OfferQuitForLink(state.profile);
            continue;
        }
        FinishShellWork();
        SessionsView_PauseWatching();
        if (!SessionLink_Remove(&g_manager.profiles, i, error, ARRAYSIZE(error))) {
            SessionsView_ResumeWatching();
            Ui_Message(g_manager.dlg, MB_ICONWARNING, L"%s", error);
            continue;
        }
        SessionsView_ResumeWatching();
        {
            int self = Profiles_Find(&g_manager.profiles, folder), other = Profiles_Find(&g_manager.profiles, otherFolder);
            if (self >= 0 && other >= 0) SetSessionsChoice(1u << self, -1 - other);
        }
        /* The list may have moved: the next profile is the one after this one. */
        if ((at = Profiles_Find(&g_manager.profiles, folder)) >= 0) i = at;
    }
    if (StateChangesBlocked()) return;
    /* Every list kept, the groups made the same, the progress shown. */
    RunJob(JOB_KEEP, AllProfiles());
}

/* A menu item with a profile's name as its words: its ampersands shown, not taken as access keys. */
static void MenuName(const WCHAR *name, WCHAR *label, size_t cch)
{
    size_t at = 0, from;
    for (from = 0; name[from] && at + 2 < cch; from++) {
        if (name[from] == L'&') label[at++] = L'&';
        label[at++] = name[from];
    }
    label[at] = 0;
}

/* -------------------------------------------------------------- the menus */

static void ClearMenu(HMENU menu)
{
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);   /* with its submenus */
}

/* Sync with (the other profiles, checked when the ones selected sync with
 * them already), Stop syncing, and What to sync. */
static void AppendKeepSame(HMENU menu, DWORD selected)
{
    HMENU same = CreatePopupMenu();
    WCHAR label[2 * LABEL_CCH];
    int i, j;
    for (i = 0; same && selected && i < g_manager.profiles.count; i++) {
        BOOL joined = g_manager.profiles.items[i].syncGroup != 0;
        if (selected & (1u << i)) continue;
        for (j = 0; j < g_manager.profiles.count && joined; j++)
            if ((selected & (1u << j)) && g_manager.profiles.items[j].syncGroup != g_manager.profiles.items[i].syncGroup) joined = FALSE;
        MenuName(g_manager.profiles.items[i].name, label, ARRAYSIZE(label));
        AppendMenuW(same, MF_STRING | (joined ? MF_CHECKED : 0), IDM_LIST_SAME_FIRST + (UINT)i, label);
    }
    if (same)
        AppendMenuW(menu, MF_POPUP | (selected && GetMenuItemCount(same) > 0 ? 0 : MF_GRAYED), (UINT_PTR)same, TR(L"Sync &with"));
    AppendMenuW(menu, MF_STRING | (SameSelected(selected) ? 0 : MF_GRAYED), IDC_SYNC_ITEMS, TR(L"Wha&t to sync\x2026"));
    AppendMenuW(menu, MF_STRING | (SameSelected(selected) ? 0 : MF_GRAYED), IDC_SAME_STOP, TR(L"Sto&p syncing"));
}

/* The menu bar's Sessions menu. */
static void FillSessionsMenu(HMENU menu, DWORD selected)
{
    AppendMenuW(menu, MF_STRING | (selected && !g_manager.job ? 0 : MF_GRAYED), IDC_SYNC, TR(Theme_MainCaption(IDC_SYNC, 0)));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_manager.profiles.count >= 2 ? 0 : MF_GRAYED), IDC_MERGE, TR(L"Merge &all sessions\x2026"));
    AppendMenuW(menu, MF_STRING | (selected && g_manager.profiles.count >= 2 ? 0 : MF_GRAYED), IDC_COPY_ALL, TR(L"&Copy all sessions to\x2026"));
    AppendMenuW(menu, MF_STRING | (selected && g_manager.profiles.count >= 2 ? 0 : MF_GRAYED), IDC_MOVE_ALL, TR(L"&Move all sessions to\x2026"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendKeepSame(menu, selected);
    if (LinkedProfiles(selected)) AppendMenuW(menu, MF_STRING, IDM_LIST_UNLINK, TR(L"U&nlink sessions folder\x2026"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDM_LIST_EXPORT, TR(L"E&xport sessions\x2026"));
    AppendMenuW(menu, MF_STRING, IDM_LIST_IMPORT, TR(L"&Import sessions\x2026"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_manager.profiles.count ? 0 : MF_GRAYED), IDC_RESTORE, TR(L"Reco&ver sessions\x2026"));
    AppendMenuW(menu, MF_STRING, IDC_PURGE, TR(L"Recently de&leted\x2026"));
}

/* The menu bar's Shortcuts menu: the state shown is of a profile selected
 * alone; with several, each command does what is left to do for each. */
static void FillShortcutsMenu(HMENU menu, DWORD selected)
{
    const Profile *one = SelectedProfile();
    AppendMenuW(menu, MF_STRING | (selected && !(one && g_manager.onDesktop) ? 0 : MF_GRAYED), IDC_SC_DESKTOP,
                TR(Theme_MainCaption(IDC_SC_DESKTOP, one && g_manager.onDesktop)));
    AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDC_SC_SAVEAS, TR(Theme_MainCaption(IDC_SC_SAVEAS, 0)));
    AppendMenuW(menu, MF_STRING | (selected && !(one && g_manager.pinned) ? 0 : MF_GRAYED), IDC_SC_PIN,
                TR(Theme_MainCaption(IDC_SC_PIN, one && g_manager.pinned)));
    AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDC_SC_START, TR(Theme_MainCaption(IDC_SC_START, one && g_manager.inStartMenu)));
}

/* The menu bar's Program menu: its language, repair, uninstall and exit. */
static void FillProgramMenu(HMENU menu)
{
    HMENU languages = CreatePopupMenu();
    int i;
    if (languages) {
        AppendMenuW(languages, MF_STRING | (Localize_CurrentLanguage() < 0 ? MF_CHECKED : 0), IDM_LANGUAGE, Localize_LanguageName(-1));
        AppendMenuW(languages, MF_SEPARATOR, 0, NULL);
        for (i = 0; i < Localize_LanguageCount(); i++)
            AppendMenuW(languages, MF_STRING | (Localize_CurrentLanguage() == i ? MF_CHECKED : 0), (UINT_PTR)IDM_LANGUAGE + 1 + (UINT_PTR)i,
                        Localize_LanguageName(i));
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)languages, TR(L"&Language"));
    }
    AppendMenuW(menu, MF_STRING | (g_manager.job ? MF_GRAYED : 0), IDC_REPAIR, TR(Theme_MainCaption(IDC_REPAIR, 0)));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDC_UNINSTALL, TR(L"&Uninstall\x2026"));
    AppendMenuW(menu, MF_STRING, IDM_EXIT, TR(L"E&xit"));
}

/* The window's menu bar: Sessions, Program, Shortcuts and Help, named in the
 * current language; what each holds is made as it opens. */
static void MakeMenuBar(HWND dialog)
{
    HMENU bar = CreateMenu();
    g_manager.programMenu = CreatePopupMenu();
    g_manager.sessionsMenu = CreatePopupMenu();
    g_manager.shortcutsMenu = CreatePopupMenu();
    g_manager.helpMenu = CreatePopupMenu();
    if (!bar || !g_manager.programMenu || !g_manager.sessionsMenu || !g_manager.shortcutsMenu || !g_manager.helpMenu) {
        if (bar) DestroyMenu(bar);
        if (g_manager.programMenu) DestroyMenu(g_manager.programMenu);
        if (g_manager.sessionsMenu) DestroyMenu(g_manager.sessionsMenu);
        if (g_manager.shortcutsMenu) DestroyMenu(g_manager.shortcutsMenu);
        if (g_manager.helpMenu) DestroyMenu(g_manager.helpMenu);
        g_manager.programMenu = g_manager.sessionsMenu = g_manager.shortcutsMenu = g_manager.helpMenu = NULL;
        return;
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_manager.sessionsMenu, TR(Theme_MainCaption(IDC_MENU_SESSIONS, 0)));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_manager.programMenu, TR(Theme_MainCaption(IDC_MENU_APP, 0)));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_manager.shortcutsMenu, TR(Theme_MainCaption(IDC_MENU_SHORTCUTS, 0)));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)g_manager.helpMenu, TR(Theme_MainCaption(IDC_MENU_HELP, 0)));
    if (!SetMenu(dialog, bar)) {
        DestroyMenu(bar);   /* with its menus */
        g_manager.programMenu = g_manager.sessionsMenu = g_manager.shortcutsMenu = g_manager.helpMenu = NULL;
    }
}

/* The menu bar's names in a new language. */
static void NameMenuBar(void)
{
    static const int kMenus[] = { IDC_MENU_SESSIONS, IDC_MENU_APP, IDC_MENU_SHORTCUTS, IDC_MENU_HELP };
    HMENU bar = GetMenu(g_manager.dlg);
    UINT i;
    if (!bar) return;
    for (i = 0; i < ARRAYSIZE(kMenus); i++)
        ModifyMenuW(bar, i, MF_BYPOSITION | MF_POPUP | MF_STRING, (UINT_PTR)GetSubMenu(bar, (int)i), TR(Theme_MainCaption(kMenus[i], 0)));
    DrawMenuBar(g_manager.dlg);
}

/* WM_INITMENUPOPUP: one of the menu bar's menus fills as it opens, for the
 * profiles selected now; while the window closes or uninstalls, all of it
 * but Exit is grayed. FALSE for another menu. */
static BOOL FillMenuBarMenu(HMENU menu)
{
    int i, count;
    if (!menu || (menu != g_manager.programMenu && menu != g_manager.sessionsMenu && menu != g_manager.shortcutsMenu &&
                  menu != g_manager.helpMenu)) return FALSE;
    UpdateButtons();   /* the shortcuts' state of the profile selected */
    ClearMenu(menu);
    if (menu == g_manager.helpMenu) Help_FillMenu(menu);
    else if (menu == g_manager.programMenu) FillProgramMenu(menu);
    else if (menu == g_manager.sessionsMenu) FillSessionsMenu(menu, SelectedProfiles());
    else FillShortcutsMenu(menu, SelectedProfiles());
    count = GetMenuItemCount(menu);
    for (i = 0; StateChangesBlocked() && i < count; i++)
        if (GetMenuItemID(menu, i) != IDM_EXIT) EnableMenuItem(menu, (UINT)i, MF_BYPOSITION | MF_GRAYED);
    return TRUE;
}

static void DoLanguage(int choice);
static void Close(HWND dialog);

/* A menu's own commands: TRUE when `cmd` is one; the others are the buttons they stand for. */
static BOOL MenuOwnCommand(UINT cmd)
{
    if (cmd >= IDM_LIST_SAME_FIRST && cmd < IDM_LIST_SAME_FIRST + MAX_PROFILES) {
        DoKeepSame((int)(cmd - IDM_LIST_SAME_FIRST));
        return TRUE;
    }
    if (cmd >= IDM_LANGUAGE && cmd <= IDM_LANGUAGE + (UINT)Localize_LanguageCount()) {
        DoLanguage(cmd == IDM_LANGUAGE ? -1 : (int)(cmd - IDM_LANGUAGE - 1));
        return TRUE;
    }
    switch (cmd) {
    case IDM_EXIT: Close(g_manager.dlg); return TRUE;
    case IDM_LIST_EXPORT: DoSessions(SESSIONS_EXPORT); return TRUE;
    case IDM_LIST_IMPORT: DoSessions(SESSIONS_IMPORT); return TRUE;
    case IDM_LIST_UNLINK: DoUnlink(); return TRUE;
    case IDM_LIST_BACKUP:
        if (SelectedProfiles()) Backup_Create(g_manager.dlg, &g_manager.pkg, &g_manager.profiles, SelectedProfiles());
        return TRUE;
    case IDM_LIST_RESTORE:
        if (SelectedProfile() && !StateChangesBlocked()) {
            FinishShellWork();
            SessionsView_PauseWatching();
            Backup_Restore(g_manager.dlg, &g_manager.pkg, &g_manager.profiles, (int)(SelectedProfile() - g_manager.profiles.items));
            SessionsView_ResumeWatching();
            Refresh(FALSE);
        }
        return TRUE;
    }
    return FALSE;
}

/* What a popup menu chose: its own commands here, the others as the buttons they stand for. */
static void MenuCommand(UINT cmd)
{
    if (cmd && !MenuOwnCommand(cmd)) SendMessageW(g_manager.dlg, WM_COMMAND, MAKEWPARAM(cmd, BN_CLICKED), 0);
}

/* The list's menu, at the mouse or (from the keyboard) under the row with
 * the focus: what the toolbar and the column do to the profiles selected
 * (through their buttons, by their id), keeping their sessions the same,
 * and the profiles backed up; their shortcuts and other session commands
 * are in the menu bar. */
static void ListMenu(LPARAM pos)
{
    HMENU menu;
    const Profile *one;
    DWORD selected = SelectedProfiles();
    BOOL isDefault, idle = g_manager.job == NULL;
    POINT pt;
    if (pos == (LPARAM)-1) {
        RECT rc;
        int focused = ListView_GetNextItem(g_manager.list, -1, LVNI_FOCUSED);
        if (focused < 0 || !ListView_GetItemRect(g_manager.list, focused, &rc, LVIR_LABEL)) GetClientRect(g_manager.list, &rc);
        pt.x = rc.left;
        pt.y = rc.bottom;
        ClientToScreen(g_manager.list, &pt);
    } else {
        pt.x = (short)LOWORD(pos);
        pt.y = (short)HIWORD(pos);
    }
    /* The row just clicked is selected, its WM_APP_SELECTION maybe not handled yet: the buttons follow it now. */
    UpdateButtons();
    one = SelectedProfile();
    isDefault = one && Core_EqualsI(one->folder, g_manager.profiles.defaultFolder);
    if ((menu = CreatePopupMenu()) == NULL) return;
    AppendMenuW(menu, MF_STRING | (selected && g_manager.pkg.found && idle ? 0 : MF_GRAYED), IDC_OPEN, TR(Theme_MainCaption(IDC_OPEN, 0)));
    AppendMenuW(menu, MF_STRING | (RunningOf(selected) && idle ? 0 : MF_GRAYED), IDC_STOP, TR(Theme_MainCaption(IDC_STOP, 0)));
    AppendMenuW(menu, MF_STRING | (selected && g_manager.pkg.found && idle ? 0 : MF_GRAYED), IDC_RESTART, TR(Theme_MainCaption(IDC_RESTART, 0)));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDC_EDIT, TR(Theme_MainCaption(IDC_EDIT, 0)));
    AppendMenuW(menu, MF_STRING | (Deletable(selected) ? 0 : MF_GRAYED), IDC_DELETE, TR(Theme_MainCaption(IDC_DELETE, 0)));
    AppendMenuW(menu, MF_STRING | (one && !isDefault ? 0 : MF_GRAYED), IDC_DEFAULT, TR(Theme_MainCaption(IDC_DEFAULT, 0)));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (selected && idle ? 0 : MF_GRAYED), IDC_SYNC, TR(Theme_MainCaption(IDC_SYNC, 0)));
    AppendKeepSame(menu, selected);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDM_LIST_BACKUP, TR(L"&Back up\x2026"));
    AppendMenuW(menu, MF_STRING | (one ? 0 : MF_GRAYED), IDM_LIST_RESTORE, TR(L"Restore from back&up\x2026"));
    if (selected && g_manager.pkg.found && idle) SetMenuDefaultItem(menu, IDC_OPEN, FALSE);
    MenuCommand((UINT)TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | (Localize_IsRTL() ? TPM_LAYOUTRTL : 0), pt.x, pt.y,
                                       g_manager.dlg, NULL));
    DestroyMenu(menu);   /* with its submenus */
}

/* The profiles and the sessions share the window: Sessions view swaps them,
 * and becomes "< Back" in the same place. The toolbar stays: in the
 * sessions view it acts on the profile shown. */
static const int kProfileControls[] = { IDC_LIST, IDC_SYNC, IDC_REPAIR, IDC_BACKUP_CODE, IDC_NOTE };

static const WCHAR *SessionsButtonCaption(BOOL sessionsShown)
{
    return TR(Theme_MainCaption(IDC_SESSIONS, sessionsShown));
}

/* The view shown gets the focus before the other one hides. */
void Gui_ShowSessions(HWND dialog, const ClaudePackage *package, BOOL showSessions, const WCHAR *folder)
{
    size_t i;
    if (showSessions) {
        SessionsView_Enter(package, folder);
        FocusView(dialog, TRUE);
        for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(dialog, kProfileControls[i]), SW_HIDE);
    } else {
        for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(dialog, kProfileControls[i]), SW_SHOW);
        FocusView(dialog, FALSE);
        SessionsView_Leave();
    }
    SetDlgItemTextW(dialog, IDC_SESSIONS, SessionsButtonCaption(showSessions));
    Theme_SetMainGlyph(dialog, IDC_SESSIONS, showSessions);
    Theme_LayoutMain(dialog);
    SessionsView_Resize();
}

static void ToggleSessions(void)
{
    if (!SessionsView_Shown()) {
        const Profile *p = FocusedProfile();
        Gui_ShowSessions(g_manager.dlg, &g_manager.pkg, TRUE, p ? p->folder : NULL);
        SelectProfileRow(SessionsView_Profile());   /* the toolbar acts on the profile shown */
    } else {
        WCHAR folder[FOLDER_CCH];
        StringCchCopyW(folder, ARRAYSIZE(folder), SessionsView_Profile());
        Gui_ShowSessions(g_manager.dlg, &g_manager.pkg, FALSE, folder);
        SelectProfileRow(folder);   /* the profile the sessions view showed */
    }
}

/* Adds the profile to the Start menu, or takes it out: looked up again
 * first, the entry may have changed since the button was drawn. */
/* Adds every profile selected to the Start menu, or takes it out: one alone
 * goes by what it is now (looked up again: it may have changed since the
 * menu showed); several are all added. */
static void DoStartMenu(void)
{
    ListSelection selection;
    HRESULT hr;
    int i, p;
    TakeSelection(&selection);
    FinishShellWork();
    for (i = 0; i < selection.count && !StateChangesBlocked(); i++) {
        Profile profile;
        BOOL there;
        if ((p = Profiles_Find(&g_manager.profiles, selection.folders[i])) < 0) continue;
        profile = g_manager.profiles.items[p];
        there = Shortcut_IsInStartMenu(&profile);
        if (there && selection.count > 1) continue;
        if (there) {
            if (FAILED(hr = Shortcut_RemoveFromStartMenu(&profile)))
                Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"\x201C%s\x201D could not be removed from the Start menu (error 0x%08lX)."),
                           profile.name, (unsigned long)hr);
        } else if (FAILED(hr = Shortcut_AddToStartMenu(&g_manager.pkg, &profile))) {
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"\x201C%s\x201D could not be added to the Start menu (error 0x%08lX)."),
                       profile.name, (unsigned long)hr);
        }
    }
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
}

/* The folders the shortcuts menu reads: a change there reads its state
 * again, whoever made it (interrupt level: also a file written without the
 * shell). */
static void WatchShortcutFolders(void)
{
    const GUID *known[] = { &FOLDERID_Desktop, &FOLDERID_PublicDesktop, &FOLDERID_Programs };
    SHChangeNotifyEntry entries[ARRAYSIZE(known) + 2];
    WCHAR pins[MAX_PATH], start[MAX_PATH];
    int n = 0;
    size_t i;
    for (i = 0; i < ARRAYSIZE(known); i++) {
        PIDLIST_ABSOLUTE pidl = NULL;
        if (SUCCEEDED(SHGetKnownFolderIDList(known[i], 0, NULL, &pidl)) && pidl) {
            entries[n].pidl = pidl;
            entries[n++].fRecursive = FALSE;
        }
    }
    if (TaskbarPin_Dir(pins, ARRAYSIZE(pins)) && (entries[n].pidl = ILCreateFromPathW(pins)) != NULL) entries[n++].fRecursive = FALSE;
    if (Shortcut_StartMenuDir(start, ARRAYSIZE(start)) && (entries[n].pidl = ILCreateFromPathW(start)) != NULL)
        entries[n++].fRecursive = FALSE;
    if (n > 0)
        g_manager.shortcutsNotify = SHChangeNotifyRegister(g_manager.dlg, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                                           SHCNE_CREATE | SHCNE_DELETE | SHCNE_RENAMEITEM | SHCNE_UPDATEITEM | SHCNE_UPDATEDIR,
                                                           WM_APP_SHORTCUTS, n, entries);
    /* Those buttons are then checked again only when the window is activated. */
    if (!g_manager.shortcutsNotify)
        Util_Log(L"the shortcut folders are not watched (%d of %d found)", n, (int)ARRAYSIZE(entries));
    for (i = 0; i < (size_t)n; i++) CoTaskMemFree((void *)entries[i].pidl);
}

/* What the window shows can change outside it: a profile folder made or
 * removed, a name or color written by another copy, the default profile, the
 * claude:// app picked in Windows, Claude installed or updated, a taskbar pin
 * added or removed. A thread waits for the matching registry and folder
 * notifications and tells the window, which refreshes once for a burst of
 * them. */
typedef struct OutsideKey {
    const WCHAR *path;
    BOOL         subtree;
    DWORD        filter;
    LONG         change;
} OutsideKey;

static const OutsideKey kOutsideKeys[] = {
    /* Its own values and subkeys only: Update and Shortcuts are the manager's own records. */
    { REG_ROOT, FALSE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_PROFILES },
    { REG_PROFILES, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_PROFILES },
    { L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations", TRUE,
      REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, CHANGE_LINKS },
    { REG_PACKAGES, FALSE, REG_NOTIFY_CHANGE_NAME, CHANGE_PACKAGES },
    { REG_TASKBAND, FALSE, REG_NOTIFY_CHANGE_LAST_SET, CHANGE_PINS },
};

/* Opens the key when needed and arms its notification. A key that does not
 * exist or was deleted stays closed and is tried again after the next change
 * the thread sees: for Profiles (missing before the first profile), REG_ROOT's
 * watch reports the change that creates it. */
static void ArmOutsideKey(const OutsideKey *source, HKEY *key, HANDLE event)
{
    if (!*key && RegOpenKeyExW(HKEY_CURRENT_USER, source->path, 0, KEY_NOTIFY, key) != ERROR_SUCCESS) {
        *key = NULL;
        return;
    }
    if (RegNotifyChangeKeyValue(*key, source->subtree, source->filter, event, TRUE) != ERROR_SUCCESS) {
        RegCloseKey(*key);
        *key = NULL;
    }
}

static DWORD WINAPI WatchOutside(void *param)
{
    HWND dialog = (HWND)param;
    HANDLE handles[ARRAYSIZE(kOutsideKeys) + 2], dirs = INVALID_HANDLE_VALUE;
    HKEY keys[ARRAYSIZE(kOutsideKeys) + 2] = { 0 };
    LONG changes[ARRAYSIZE(kOutsideKeys) + 2];
    const OutsideKey *which[ARRAYSIZE(kOutsideKeys) + 2] = { 0 };
    WCHAR appData[MAX_PATH];
    DWORD n = 0, i, k, signaled;

    handles[n] = g_manager.outsideStop;
    changes[n++] = 0;
    for (k = 0; k < ARRAYSIZE(kOutsideKeys); k++) {
        HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!event) continue;
        handles[n] = event;
        which[n] = &kOutsideKeys[k];
        changes[n] = kOutsideKeys[k].change;
        ArmOutsideKey(which[n], &keys[n], event);
        n++;
    }
    /* Profile folders sit directly in %APPDATA%. */
    if (Util_AppData(appData, ARRAYSIZE(appData)) &&
        (dirs = FindFirstChangeNotificationW(appData, FALSE, FILE_NOTIFY_CHANGE_DIR_NAME)) != INVALID_HANDLE_VALUE) {
        handles[n] = dirs;
        changes[n++] = CHANGE_PROFILES;
    }

    for (;;) {
        signaled = WaitForMultipleObjects(n, handles, FALSE, INFINITE);
        if (signaled == WAIT_OBJECT_0 || signaled >= WAIT_OBJECT_0 + n) break;
        i = signaled - WAIT_OBJECT_0;
        /* Armed again first, so a change during the refresh still counts;
         * a key missing until now may exist. A folder watch that cannot be
         * armed again would stay signaled: it is dropped (it is the last). */
        if (handles[i] == dirs && !FindNextChangeNotification(dirs)) {
            Util_Log(L"the profile folders are no longer watched (error %lu)", GetLastError());
            FindCloseChangeNotification(dirs);
            dirs = INVALID_HANDLE_VALUE;
            n--;
        }
        for (k = 1; k < n; k++)
            if (which[k] && (k == i || !keys[k])) ArmOutsideKey(which[k], &keys[k], handles[k]);
        if (InterlockedOr(&g_outside, changes[i]) == 0) PostMessageW(dialog, WM_APP_OUTSIDE, 0, 0);
    }

    for (i = 1; i < n; i++) {
        if (handles[i] == dirs) {
            FindCloseChangeNotification(dirs);
        } else {
            if (keys[i]) RegCloseKey(keys[i]);
            CloseHandle(handles[i]);
        }
    }
    return 0;
}

static void StartWatchingOutside(HWND dialog)
{
    g_manager.outsideStop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (g_manager.outsideStop) g_manager.outsideThread = CreateThread(NULL, 0, WatchOutside, dialog, 0, NULL);
}

static void StopWatchingOutside(void)
{
    if (g_manager.outsideStop) SetEvent(g_manager.outsideStop);
    if (g_manager.outsideThread) {
        /* The worker posts UI notifications and never waits for this thread. */
        WaitForSingleObject(g_manager.outsideThread, INFINITE);
        CloseHandle(g_manager.outsideThread);
    }
    if (g_manager.outsideStop) CloseHandle(g_manager.outsideStop);
    g_manager.outsideThread = g_manager.outsideStop = NULL;
}

/* Several profiles' shortcuts in one folder the user picks (the desktop at
 * first), each named after its profile; a name already there gets a
 * number. */
static void SaveShortcuts(void)
{
    ListSelection selection;
    IFileOpenDialog *folderDialog = NULL;
    IShellItem *desktop = NULL, *result = NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    WCHAR folder[MAX_PATH], name[MAX_PATH], path[MAX_PATH];
    PWSTR chosen = NULL;
    HRESULT hr;
    int i, p, copy;
    TakeSelection(&selection);
    hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&folderDialog);
    if (FAILED(hr)) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
        return;
    }
    IFileOpenDialog_SetTitle(folderDialog, TR(L"Create the shortcuts in"));
    if (SUCCEEDED(IFileOpenDialog_GetOptions(folderDialog, &options)))
        IFileOpenDialog_SetOptions(folderDialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(SHCreateItemInKnownFolder(&FOLDERID_Desktop, 0, NULL, &IID_IShellItem, (void **)&desktop))) {
        IFileOpenDialog_SetDefaultFolder(folderDialog, desktop);
        IShellItem_Release(desktop);
    }
    hr = IFileOpenDialog_Show(folderDialog, g_manager.dlg);
    if (SUCCEEDED(hr) && !StateChangesBlocked() && SUCCEEDED(hr = IFileOpenDialog_GetResult(folderDialog, &result))) {
        if (SUCCEEDED(hr = IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &chosen))) {
            StringCchCopyW(folder, ARRAYSIZE(folder), chosen);
            CoTaskMemFree(chosen);
            for (i = 0; i < selection.count && !StateChangesBlocked(); i++) {
                Profile profile;
                if ((p = Profiles_Find(&g_manager.profiles, selection.folders[i])) < 0) continue;
                profile = g_manager.profiles.items[p];
                for (copy = 1; copy < 100; copy++) {
                    Core_ShortcutFileName(profile.name, copy, name, ARRAYSIZE(name));
                    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", folder, name))) {
                        path[0] = 0;
                        break;
                    }
                    if (Util_QueryPath(path, NULL) == PATH_MISSING) break;
                }
                if (!path[0] || copy == 100) Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The path is too long."));
                else CreateShortcutAt(&profile, path);
            }
        }
        IShellItem_Release(result);
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
    IFileOpenDialog_Release(folderDialog);
}

/* Create shortcut: for one profile, saved where and as the user says; for
 * several, into one folder. */
static void DoSaveShortcut(void)
{
    const Profile *selected = SelectedProfile();
    Profile p;
    IFileSaveDialog *saveDialog = NULL;
    IShellItem *desktop = NULL, *result = NULL;
    COMDLG_FILTERSPEC filter = { TR(L"Shortcut (*.lnk)"), L"*.lnk" };
    WCHAR name[MAX_PATH], path[MAX_PATH];
    PWSTR chosen = NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    HRESULT hr;

    if (!selected) {
        if (SelectedProfiles()) SaveShortcuts();
        return;
    }
    p = *selected;
    hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, (void **)&saveDialog);
    if (FAILED(hr)) {
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
        return;
    }
    Core_ShortcutFileName(p.name, 1, name, ARRAYSIZE(name));
    IFileSaveDialog_SetTitle(saveDialog, TR(L"Create shortcut"));
    IFileSaveDialog_SetFileTypes(saveDialog, 1, &filter);
    IFileSaveDialog_SetDefaultExtension(saveDialog, L"lnk");
    IFileSaveDialog_SetFileName(saveDialog, name);
    if (SUCCEEDED(IFileSaveDialog_GetOptions(saveDialog, &options)))
        IFileSaveDialog_SetOptions(saveDialog, options | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_NODEREFERENCELINKS | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(SHCreateItemInKnownFolder(&FOLDERID_Desktop, 0, NULL, &IID_IShellItem, (void **)&desktop))) {
        IFileSaveDialog_SetDefaultFolder(saveDialog, desktop);
        IShellItem_Release(desktop);
    }
    hr = IFileSaveDialog_Show(saveDialog, g_manager.dlg);
    if (SUCCEEDED(hr) && !StateChangesBlocked() && SUCCEEDED(hr = IFileSaveDialog_GetResult(saveDialog, &result))) {
        if (SUCCEEDED(hr = IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &chosen))) {
            if (FAILED(StringCchCopyW(path, ARRAYSIZE(path), chosen)) ||
                (!Core_EndsWithI(path, L".lnk") && FAILED(StringCchCatW(path, ARRAYSIZE(path), L".lnk"))))
                Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The path is too long."));
            else
                CreateShortcutAt(&p, path);
            CoTaskMemFree(chosen);
        }
        IShellItem_Release(result);
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The shortcut could not be created (error 0x%08lX)."), (unsigned long)hr);
    IFileSaveDialog_Release(saveDialog);
}

/* An uninstall cancelled, or refused by Install_Uninstall (which said why):
 * the window shows the state again. */
static void EndUninstallAttempt(void)
{
    g_manager.uninstallInProgress = FALSE;
    if (StateChangesBlocked()) return;
    g_manager.shortcutStateFolder[0] = 0;
    Refresh(TRUE);
    ShowVersion();
}

/* Works on a private copy of the list: the dialog's choices are indexed on
 * it, and nothing reloads it until the end. Not while the new release
 * downloads or installs: its thread still writes the download. */
static void DoUninstall(void)
{
    UninstallDialog dialog;
    ProfileList snapshot;
    WCHAR stock[LABEL_CCH];
    int stockIndex;

    if (g_manager.uninstallInProgress || g_manager.uninstalled) return;
    if (g_manager.updating) {
        Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(APP_NAME L" is being updated. Try again once the update is done."));
        return;
    }
    FinishShellWork();
    Refresh(TRUE);
    snapshot = g_manager.profiles;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.list = &snapshot;
    g_manager.uninstallInProgress = TRUE;
    if (Ui_Dialog(g_manager.dlg, IDD_UNINSTALL, UninstallProc, (LPARAM)&dialog) != IDOK || g_manager.closing) {
        EndUninstallAttempt();
        return;
    }
    Claude_UpdateRunning(&snapshot);
    stockIndex = Profiles_Find(&snapshot, STOCK_FOLDER);
    StringCchCopyW(stock, ARRAYSIZE(stock), stockIndex >= 0 ? snapshot.items[stockIndex].name : STOCK_DEFAULT_NAME);
    SessionsView_PauseWatching();   /* not resumed once uninstalled: the window closes */
    if (!Install_Uninstall(g_manager.dlg, &snapshot, dialog.removeData)) {
        SessionsView_ResumeWatching();
        EndUninstallAttempt();
        return;
    }
    g_manager.uninstalled = TRUE;
    g_manager.uninstallInProgress = FALSE;
    Ui_Message(g_manager.dlg, MB_ICONINFORMATION,
               TR(APP_NAME L" has been removed.\n\nClaude Desktop and its \x201C%s\x201D profile are untouched."), stock);
    EndDialog(g_manager.dlg, 0);
}

/* Whether `owner` owns `window`, directly or through other windows. */
static BOOL OwnedBy(HWND window, HWND owner)
{
    HWND above;
    for (above = window ? GetWindow(window, GW_OWNER) : NULL; above; above = GetWindow(above, GW_OWNER))
        if (above == owner) return TRUE;
    return FALSE;
}

static BOOL CALLBACK CloseOwnedProc(HWND window, LPARAM owner)
{
    if (OwnedBy(window, (HWND)owner)) PostMessageW(window, WM_CLOSE, 0, 0);
    return TRUE;
}

static void Close(HWND dialog)
{
    g_manager.closing = TRUE;
    EnumThreadWindows(GetCurrentThreadId(), CloseOwnedProc, (LPARAM)dialog);
    EndDialog(dialog, 0);
}

/* Only the user can make Claude Desktop Profiles Manager the app for
 * claude:// links, in Windows' own chooser: say what to pick, then let Windows
 * ask. */
static void SetUpLinks(void)
{
    WCHAR exe[MAX_PATH];
    if (!g_manager.pkg.found || Handler_UserChoice() == USERCHOICE_OURS) return;
    if (!Ui_Ask(g_manager.dlg, IDI_INFORMATION,
                TR(L"Windows now asks which app opens claude:// links.\n\n"
                   L"Choose \x201C" APP_NAME L"\x201D, then Always (or Set default). Sign-ins then always come back to the window that started them."),
                TR(L"Continue"), TR(L"Later"), FALSE))
        return;
    /* Unregistered, the program would not be in the list the user is sent to. */
    if (!Util_InstallExe(exe, ARRAYSIZE(exe)) || !Handler_Register(exe)) {
        Util_Log(L"could not register for claude:// links (error %lu)", GetLastError());
        Ui_Message(g_manager.dlg, MB_ICONERROR, TR(APP_NAME L" could not be registered as an app for claude:// links."));
        return;
    }
    if (!Handler_AskUser())
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"Windows kept the app it uses for claude:// links.\n\n"
                      L"Open Settings > Apps > Default apps > Choose defaults by link type, find CLAUDE and choose \x201C" APP_NAME L"\x201D."));
    UpdateStatus();
}

static void DoStatusAction(void)
{
    if (g_manager.statusAction == STATUS_ACTION_GET_CLAUDE) Util_OpenUrl(APP_DOWNLOAD_URL);
}



/* ------------------------------------------------------------------ repair */

/* The changes our state folder keeps for profiles that are gone: nothing
 * makes them any more. Returns how many went. */
static int DropOrphanedPlans(void)
{
    static const WCHAR *const kPrefixes[] = { L"pending-sync-", L"pending-sessions-" };
    WCHAR state[MAX_PATH], path[MAX_PATH], folder[FOLDER_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    size_t prefix;
    int dropped = 0;
    if (!Util_StateDir(state, ARRAYSIZE(state))) return 0;
    for (prefix = 0; prefix < ARRAYSIZE(kPrefixes); prefix++) {
        WCHAR pattern[64];
        size_t length = wcslen(kPrefixes[prefix]);
        StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s*.txt", kPrefixes[prefix]);
        if ((find = Util_FindFiles(state, pattern, &found, FALSE)) == INVALID_HANDLE_VALUE) continue;
        do {
            size_t nameLength = wcslen(found.cFileName);
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || nameLength <= length + 4 ||
                FAILED(StringCchCopyNW(folder, ARRAYSIZE(folder), found.cFileName + length, nameLength - length - 4)) ||
                Profiles_Find(&g_manager.profiles, folder) >= 0 || FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", state, found.cFileName)))
                continue;
            if (DeleteFileW(path)) {
                dropped++;
                Util_Log(L"repair: the changes waiting for %s, gone, dropped (%s)", folder, found.cFileName);
            }
            /* A plan's entries, in the folder of its name. */
            if (prefix == 0 && SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s%s", state, kPrefixes[prefix], folder))) {
                SHFILEOPSTRUCTW operation;
                WCHAR from[MAX_PATH + 1];
                ZeroMemory(from, sizeof from);
                if (Util_DirExists(path) && SUCCEEDED(StringCchCopyW(from, MAX_PATH, path))) {
                    ZeroMemory(&operation, sizeof operation);
                    operation.wFunc = FO_DELETE;
                    operation.pFrom = from;
                    operation.fFlags = FOF_NO_UI;
                    SHFileOperationW(&operation);
                }
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    return dropped;
}

static void HealIcons(const ProfileList *list);

/* Repair: what an earlier version, or a Windows or Claude change, may have
 * left wrong put right again: the program's registration, the taskbar pins,
 * the profiles' icons, shortcuts and pins, session folders still linked,
 * session groups of one profile, changes waiting for profiles that are gone;
 * then every profile's sessions kept, the groups made the same. */
/* Repair, asked first: what it does, and where: the program's registry
 * keys, the folders of its shortcuts and pins, its own folder, and the
 * sessions' folders of the profiles kept the same or still linked. */
static BOOL ConfirmRepair(void)
{
    static const WCHAR *const kKeys[] = { REG_PROGID, REG_ROOT, REG_REGISTERED, REG_UNINSTALL };
    static const KNOWNFOLDERID *const kFolders[] = { &FOLDERID_Desktop, &FOLDERID_Programs, &FOLDERID_UserPinned };
    WCHAR *folders = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, CONFIRM_CCH * sizeof(WCHAR)), line[LONG_PATH_CCH], state[MAX_PATH];
    DWORD sessions = LinkedProfiles(AllProfiles());
    size_t i;
    int p;
    BOOL ok;
    if (!folders) return FALSE;
    folders[0] = 0;
    SyncUi_AddLine(folders, CONFIRM_CCH, TR(L"The program's registration, shortcuts and taskbar pins:"), FALSE);
    for (i = 0; i < ARRAYSIZE(kKeys); i++)
        if (SUCCEEDED(StringCchPrintfW(line, ARRAYSIZE(line), L"HKCU\\%s", kKeys[i]))) SyncUi_AddLine(folders, CONFIRM_CCH, line, TRUE);
    for (i = 0; i < ARRAYSIZE(kFolders); i++) {
        PWSTR known = NULL;
        if (SUCCEEDED(SHGetKnownFolderPath(kFolders[i], 0, NULL, &known)) &&
            SUCCEEDED(StringCchPrintfW(line, ARRAYSIZE(line), L"%s%s", known, kFolders[i] == &FOLDERID_UserPinned ? L"\\TaskBar" : L"")))
            SyncUi_AddPath(folders, CONFIRM_CCH, line);
        CoTaskMemFree(known);
    }
    if (Util_StateDir(state, ARRAYSIZE(state))) {
        SyncUi_AddLine(folders, CONFIRM_CCH, TR(L"Its own folder (the profiles' icons, the changes waiting for them):"), FALSE);
        SyncUi_AddPath(folders, CONFIRM_CCH, state);
    }
    for (p = 0; p < g_manager.profiles.count; p++)
        if (g_manager.profiles.items[p].syncGroup) sessions |= 1u << p;
    if (sessions) {
        SyncUi_AddSessionFolders(folders, CONFIRM_CCH, &g_manager.profiles, sessions, TRUE);
        SyncUi_AddTranscriptFolder(folders, CONFIRM_CCH, TR(L"Claude Code's conversations, where a session gets a copy of its own:"));
    }
    ok = SyncUi_Confirm(g_manager.dlg, NULL,
                        TR(L"Repair " APP_NAME L"?\n\nIt registers the program again, for claude:// links too (when they do not open here, "
                           L"Windows then asks which app opens them), puts its taskbar pins, shortcuts and icons right, gives the profiles whose "
                           L"session folder is still a link one of their own (their sessions kept the same instead), drops the changes waiting "
                           L"for profiles that are gone, then keeps every profile's sessions."),
                        folders, sessions != 0, TR(L"Repair"));
    HeapFree(GetProcessHeap(), 0, folders);
    return ok;
}

static void DoRepair(void)
{
    WCHAR text[1024], error[512 + 2 * LONG_PATH_CCH], errors[4 * (512 + 2 * LONG_PATH_CCH)];
    LinkState state;
    int i, unlinked = 0, dropped;
    if (StateChangesBlocked() || g_manager.job) return;
    if (!ConfirmRepair()) return;
    FinishShellWork();
    Install_Repair();
    TaskbarPin_RepairOurs();
    Claude_FindPackage(&g_manager.pkg);
    Refresh(TRUE);
    HealIcons(&g_manager.profiles);
    errors[0] = 0;
    SessionsView_PauseWatching();
    for (i = 0; i < g_manager.profiles.count; i++) {
        WCHAR folder[FOLDER_CCH], otherFolder[FOLDER_CCH];
        SessionLink_Read(&g_manager.profiles, i, &state);
        if (state.kind != LINK_PROFILE && state.kind != LINK_FOLDER && state.kind != LINK_BROKEN) continue;
        if (SessionLink_Busy(&g_manager.profiles.items[i]) ||
            (state.kind == LINK_PROFILE && SessionLink_Busy(&g_manager.profiles.items[state.profile]))) {
            StringCchPrintfW(error, ARRAYSIZE(error), TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."),
                             g_manager.profiles.items[i].name);
            AddError(errors, ARRAYSIZE(errors), error);
            continue;
        }
        StringCchCopyW(folder, ARRAYSIZE(folder), g_manager.profiles.items[i].folder);
        otherFolder[0] = 0;
        if (state.kind == LINK_PROFILE) StringCchCopyW(otherFolder, ARRAYSIZE(otherFolder), g_manager.profiles.items[state.profile].folder);
        if (!SessionLink_Remove(&g_manager.profiles, i, error, ARRAYSIZE(error))) {
            AddError(errors, ARRAYSIZE(errors), error);
            continue;
        }
        unlinked++;
        if (otherFolder[0]) {
            int self = Profiles_Find(&g_manager.profiles, folder), other = Profiles_Find(&g_manager.profiles, otherFolder);
            if (self >= 0 && other >= 0) SetSessionsChoice(1u << self, -1 - other);
        }
    }
    SessionsView_ResumeWatching();
    DropLoneGroups();
    dropped = DropOrphanedPlans();
    Refresh(FALSE);
    UpdateStatus();
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Repaired. Session folders given back: %d. Waiting changes of profiles that are gone dropped: %d."),
                     unlinked, dropped);
    if (errors[0]) {
        StringCchCatW(text, ARRAYSIZE(text), L"\n\n");
        StringCchCatW(text, ARRAYSIZE(text), errors);
    }
    Util_Log(L"repair: %d session folder(s) given back, %d plan(s) dropped", unlinked, dropped);
    Ui_Message(g_manager.dlg, errors[0] ? MB_ICONWARNING : MB_ICONINFORMATION, L"%s", text);
    /* Registered again above: Windows asks which app opens claude:// links when it is not this one. */
    if (g_manager.pkg.found && Handler_UserChoice() != USERCHOICE_OURS && !StateChangesBlocked()) SetUpLinks();
    RunJob(JOB_KEEP, AllProfiles());
}

/* ------------------------------------------------------------ new release */

/* The version label while the new release downloads or installs. */
static void ShowUpdateProgress(void)
{
    WCHAR version[32], text[128];
    if (g_manager.installing) {
        SetTextIfChanged(IDC_VERSION, TR(Theme_MainVersion(MAIN_VERSION_INSTALLING)));
    } else if (Update_Available(version, ARRAYSIZE(version))) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_DOWNLOADING)), version);
        SetTextIfChanged(IDC_VERSION, text);
    }
}

/* A newer release shows in the version's place, with its Update button. */
static void ShowUpdate(void)
{
    WCHAR version[32], text[256];
    if (g_manager.updating || !Update_Available(version, ARRAYSIZE(version))) return;
    StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_AVAILABLE)), version);
    SetTextIfChanged(IDC_VERSION, text);
    EnableControl(IDC_UPDATE, TRUE);   /* an update that failed or was dropped turned it off */
    ShowOptionalButton(IDC_UPDATE, TRUE);
}

static void DoUpdate(void)
{
    WCHAR version[32];
    if (g_manager.updating || !Update_Available(version, ARRAYSIZE(version))) return;
    g_manager.updating = TRUE;
    EnableControl(IDC_UPDATE, FALSE);
    ShowUpdateProgress();
    Update_Download(g_manager.dlg, WM_APP_DOWNLOADED);
}

/* This copy's version, the time it was built, in the header. */
static void ShowVersion(void)
{
    WCHAR text[128];
    if (g_manager.updating) {
        ShowUpdateProgress();
        return;
    }
    if (!g_manager.build[0] && !Core_BuildStamp(__DATE__, __TIME__, g_manager.build, ARRAYSIZE(g_manager.build)))
        StringCchCopyW(g_manager.build, ARRAYSIZE(g_manager.build), APP_VERSION_WSTR);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_BUILD)), g_manager.build);
    SetTextIfChanged(IDC_VERSION, text);
    ShowUpdate();
}

static void SetColumnTitles(void)
{
    LVCOLUMNW column;
    const WCHAR *title;
    int i;
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_TEXT;
    for (i = 0; (title = Theme_ProfileColumnTitle(i)) != NULL; i++) {
        column.pszText = (LPWSTR)TR(title);
        ListView_SetColumn(g_manager.list, i, &column);
    }
}

/* The language chosen in the program menu (-1: Windows'). */
static void DoLanguage(int choice)
{
    int languageBefore = Localize_EffectiveLanguage();
    if (StateChangesBlocked()) return;
    FinishShellWork();   /* the previous language's, written in that language */
    if (!Localize_SetLanguage(choice, TRUE)) {
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The language preference could not be saved."));
        return;
    }
    Localize_Window(g_manager.dlg);
    NameMenuBar();
    Theme_Apply(g_manager.dlg);
    SetColumnTitles();
    SetTextIfChanged(IDC_SESSIONS, SessionsButtonCaption(SessionsView_Shown()));
    /* The registry watch reloads the profiles after the write: only the texts change here. */
    UpdateRowsAndButtons();
    UpdateStatus();
    ShowVersion();
    ReflowMain();
    SessionsView_Reload();
    /* What Windows shows of the program speaks the new language too, in
     * the background, once the window does. */
    if (Localize_EffectiveLanguage() != languageBefore) {
        ShowChanges();
        StartLanguageWork();
    }
}

/* The new release installs itself and closes this window. `error`: Windows'
 * answer that goes with `result`. */
static void Downloaded(UpdateResult result, DWORD error)
{
    BOOL openPage = FALSE;
    if (StateChangesBlocked()) {
        g_manager.updating = FALSE;
        g_manager.installing = FALSE;
        Update_RemoveDownload();
        return;
    }
    if (result == UPDATE_READY) {
        FinishShellWork();   /* the new copy replaces this one */
        result = Update_Run(&error);
    }
    if (result == UPDATE_STARTED) {
        g_manager.installing = TRUE;
        ShowUpdateProgress();
        return;
    }
    g_manager.updating = FALSE;
    g_manager.installing = FALSE;
    if (result == UPDATE_NOT_SIGNED)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The new version was not installed: the downloaded file is not signed by the author of " APP_NAME L"."));
    else if (result == UPDATE_NOT_VERIFIED)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The new version was not installed: Windows could not check its signature (error 0x%08lX). Try again later."),
                   (unsigned long)error);
    else if (result == UPDATE_WRONG_VERSION)
        Ui_Message(g_manager.dlg, MB_ICONWARNING,
                   TR(L"The new version was not installed: the downloaded file is not the version that was announced."));
    else if (result == UPDATE_NOT_STARTED)
        openPage = Ui_Message(g_manager.dlg, MB_ICONWARNING | MB_YESNO,
                              TR(L"The new version could not be started (error %lu).\n\nOpen its download page?"), error) == IDYES;
    else if (error != ERROR_SUCCESS)
        openPage = Ui_Message(g_manager.dlg, MB_ICONWARNING | MB_YESNO,
                              TR(L"The new version could not be downloaded (error %lu).\n\nOpen its download page?"), error) == IDYES;
    else
        openPage = Ui_Message(g_manager.dlg, MB_ICONWARNING | MB_YESNO,
                              TR(L"The new version could not be downloaded.\n\nOpen its download page?")) == IDYES;
    if (openPage) Util_OpenUrl(APP_RELEASES_URL);
    ShowVersion();
}

/* ----------------------------------------------------------- main window */

static void SetIcons(HWND dialog)
{
    UINT dpi = GetDpiForWindow(dialog);
    HICON big = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                  GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), 0);
    HICON little = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                     GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), 0);
    SendMessageW(dialog, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, (LPARAM)little);
    if (g_manager.bigIcon) DestroyIcon(g_manager.bigIcon);
    if (g_manager.smallIcon) DestroyIcon(g_manager.smallIcon);
    g_manager.bigIcon = big;
    g_manager.smallIcon = little;
}

static void AddColumns(void)
{
    LVCOLUMNW column;
    int i;
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_WIDTH;
    column.cx = FIRST_COLUMN_WIDTH;
    for (i = 0; Theme_ProfileColumnTitle(i); i++) ListView_InsertColumn(g_manager.list, i, &column);
    SetColumnTitles();
    LayoutColumns();
}

static void LayoutMainControls(void)
{
    if (!g_manager.layoutReady || g_manager.layingOut) return;
    g_manager.layingOut = TRUE;
    Theme_LayoutMain(g_manager.dlg);
    LayoutColumns();
    SessionsView_Resize();
    g_manager.layingOut = FALSE;
}

/* Fits the window to its controls in the current language, fonts and scale. */
static void ReflowMain(void)
{
    Theme_FitDialog(g_manager.dlg);   /* places the controls too */
    g_manager.layoutReady = TRUE;
    LayoutColumns();
    UpdateNote();
    SessionsView_Resize();
    SessionsView_Relayout();
}

static BOOL MainMinimumFrame(HWND dialog, SIZE *size)
{
    SIZE client;
    RECT frame;
    MONITORINFO monitor = { sizeof monitor };
    if (!Theme_MainMinimum(dialog, &client)) return FALSE;
    SetRect(&frame, 0, 0, client.cx, client.cy);
    if (!AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongW(dialog, GWL_STYLE), GetMenu(dialog) != NULL,
                                  (DWORD)GetWindowLongW(dialog, GWL_EXSTYLE), GetDpiForWindow(dialog))) return FALSE;
    size->cx = frame.right - frame.left;
    size->cy = frame.bottom - frame.top;
    if (GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor)) {
        size->cx = min(size->cx, monitor.rcWork.right - monitor.rcWork.left);
        size->cy = min(size->cy, monitor.rcWork.bottom - monitor.rcWork.top);
    }
    return TRUE;
}

/* WM_GETMINMAXINFO and WM_DPICHANGED of a window made from IDD_MAIN: its
 * minimum frame, and its place on a monitor with another scale. Returns
 * whether the message was handled. */
BOOL Gui_MainWindowGeometry(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SIZE minimum = { 0, 0 };
    (void)wp;
    if (message == WM_GETMINMAXINFO) {
        MINMAXINFO *info = (MINMAXINFO *)lp;
        if (!info || !MainMinimumFrame(dialog, &minimum)) return FALSE;
        info->ptMinTrackSize.x = minimum.cx;
        info->ptMinTrackSize.y = minimum.cy;
        return TRUE;
    }
    if (message == WM_DPICHANGED) {
        const RECT *suggested = (const RECT *)lp;
        Theme_Apply(dialog);
        MainMinimumFrame(dialog, &minimum);
        if (suggested) {
            MONITORINFO monitor = { sizeof monitor };
            int width = max(minimum.cx, suggested->right - suggested->left);
            int height = max(minimum.cy, suggested->bottom - suggested->top);
            int x = suggested->left, y = suggested->top;
            if (GetMonitorInfoW(MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST), &monitor)) {
                width = min(width, monitor.rcWork.right - monitor.rcWork.left);
                height = min(height, monitor.rcWork.bottom - monitor.rcWork.top);
                x = min(max(x, monitor.rcWork.left), monitor.rcWork.right - width);
                y = min(max(y, monitor.rcWork.top), monitor.rcWork.bottom - height);
            }
            SetWindowPos(dialog, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK MainProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_INITDIALOG:
        g_manager.dlg = dialog;
        Localize_Window(dialog);
        g_manager.list = GetDlgItem(dialog, IDC_LIST);
        SetIcons(dialog);
        ListView_SetExtendedListViewStyle(g_manager.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        AddColumns();
        Theme_SmoothView(g_manager.list);
        SessionsView_Init(dialog);
        MakeMenuBar(dialog);
        Theme_SetStrong(GetDlgItem(dialog, IDC_SESSIONS));   /* it leads to the other view */
        Theme_Apply(dialog);
        Theme_RememberLayout(dialog);
        ShowVersion();
        Claude_FindPackage(&g_manager.pkg);
        Profiles_Load(&g_manager.profiles, &g_manager.pkg);
        SessionsView_SetProfiles(&g_manager.profiles);
        TaskbarPin_RepairOurs();
        HealIcons(&g_manager.profiles);
        FillList(NULL);
        UpdateStatus();
        ReflowMain();
        WatchShortcutFolders();
        StartWatchingOutside(dialog);
        /* Started to uninstall (Settings > Apps): only the uninstall dialog
         * shows, and the manager never opens (wParam 1). */
        if (lp == GUI_UNINSTALL) {
            g_manager.uninstallOnly = TRUE;
            PostMessageW(dialog, WM_APP_UNINSTALL, 1, 0);
        } else if (lp == GUI_SET_UP_LINKS) {
            PostMessageW(dialog, WM_APP_SETUPLINKS, 0, 0);
        }
        if (lp != GUI_UNINSTALL) {
            Update_Check(dialog, WM_APP_UPDATE);
            PostMessageW(dialog, WM_APP_KEEP_SESSIONS, 0, 0);
        }
        if (g_manager.readyEvent) SetEvent(g_manager.readyEvent);   /* a second launch can now find this window */
        SetFocus(g_manager.list);
        return FALSE;

    case WM_SHOWWINDOW:
        if (wp && !g_manager.uninstallOnly) SessionsView_Warm(&g_manager.profiles);
        if (wp && g_manager.pendingConflicts) PostMessageW(dialog, WM_APP_SYNC_CONFLICTS, 0, 0);
        break;

    case WM_APP_SESSIONS_READY:
        SessionsView_Ready(!StateChangesBlocked());
        return TRUE;

    case WM_APP_OUTSIDE: {
        LONG changes = InterlockedExchange(&g_outside, 0);
        if (!StateChangesBlocked()) {
            if (changes & CHANGE_PINS) g_manager.shortcutStateFolder[0] = 0;
            /* The sessions view reloads by itself when its profiles change. */
            Refresh((changes & CHANGE_PACKAGES) != 0);
        }
        return TRUE;
    }

    case WM_APP_SESSIONS:
        if (!StateChangesBlocked()) SessionsView_Reload();
        return TRUE;

    case WM_APP_KEEP_SESSIONS:
        if (!StateChangesBlocked()) KeepSessions();
        return TRUE;

    case WM_APP_JOB_DONE:
        JobDone((Job *)lp);
        AskKeepAgain();
        return TRUE;

    case WM_APP_SYNC_PROGRESS:
        /* A sync of ours, or of a profile's watcher once it closed; 0 of 0 when that one ended. */
        if (lp > 0) {
            if (!g_manager.job && !g_manager.progressShown)
                StringCchCopyW(g_manager.progressText, ARRAYSIZE(g_manager.progressText), TR(L"Keeping sessions the same\x2026"));
            ShowProgress((int)wp, (int)lp);
        } else if (!g_manager.job) {
            HideProgress();
            if (!StateChangesBlocked()) SessionsView_Reload();
        }
        return TRUE;

    case WM_ACTIVATE:
        /* Back from another window: which profiles run may have changed, and
         * the sessions view shows it; the shortcut, pin and Start menu state
         * is read again (a folder that did not exist at start is not
         * watched). Back from one of its own dialogs, the action that opened
         * it refreshes what it changed. */
        if (LOWORD(wp) != WA_INACTIVE && g_manager.list && !StateChangesBlocked() && !OwnedBy((HWND)lp, dialog)) {
            g_manager.shortcutStateFolder[0] = 0;
            Refresh(FALSE);
            SessionsView_Reload();
            AskKeepAgain();
            /* The dialog manager then gives the focus back to the control
             * that had it, which the refresh may just have hidden. */
            PostMessageW(dialog, WM_APP_RESTORE_FOCUS, 0, 0);
        }
        break;

    case WM_APP_RESTORE_FOCUS: {
        HWND button = GetDlgItem(dialog, IDC_STATUS_ACTION);
        if (GetFocus() == button && !IsWindowVisible(button)) FocusView(dialog, SessionsView_Shown());
        return TRUE;
    }

    case WM_NOTIFY: {
        const NMHDR *header = (const NMHDR *)lp;
        if (header->idFrom != IDC_LIST) {
            LRESULT result;
            if (!SessionsView_Notify(header, &result)) break;
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
            return TRUE;
        }
        if (header->code == NM_SETFOCUS || header->code == NM_KILLFOCUS) InvalidateRect(g_manager.list, NULL, FALSE);
        if (header->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            /* Selecting another row first deselects the old one: the buttons
             * follow once, when the list is done. */
            if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED))
                PostOnce(&g_manager.selectionPending, WM_APP_SELECTION);
        } else if (header->code == NM_DBLCLK) {
            if (((const NMITEMACTIVATE *)lp)->iItem >= 0) DoOpen();
        } else if (header->code == NM_RCLICK || header->code == LVN_BEGINRDRAG) {
            /* The menu at the pointer for a right click, and for a press a touchpad
             * moved past the drag distance: the list starts a right drag then, and
             * would show no menu. Handled here, the list sends no WM_CONTEXTMENU. */
            int item = header->code == LVN_BEGINRDRAG ? ((const NMLISTVIEW *)lp)->iItem : -1;
            if (item >= 0 && !ListView_GetItemState(g_manager.list, item, LVIS_SELECTED)) {
                ListView_SetItemState(g_manager.list, -1, 0, LVIS_SELECTED);
                ListView_SetItemState(g_manager.list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            }
            ListMenu((LPARAM)GetMessagePos());
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
            return TRUE;
        } else if (header->code == LVN_KEYDOWN) {
            WORD key = ((const NMLVKEYDOWN *)lp)->wVKey;
            if (key == VK_DELETE) DoDelete();
            else if (key == 'A' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0)
                ListView_SetItemState(g_manager.list, -1, LVIS_SELECTED, LVIS_SELECTED);   /* Ctrl+A: every profile */
        }
        break;
    }

    case WM_INITMENUPOPUP:
        if (!HIWORD(lp) && FillMenuBarMenu((HMENU)wp)) return TRUE;
        break;

    case WM_COMMAND:
        /* A menu's command (no control sends it): a closing window takes none but Exit and Esc. */
        if (!lp && HIWORD(wp) == 0 && LOWORD(wp) != IDCANCEL) {
            if (StateChangesBlocked() && LOWORD(wp) != IDM_EXIT) return TRUE;
            if (Help_Command(dialog, LOWORD(wp)) || MenuOwnCommand(LOWORD(wp))) return TRUE;
        }
        /* The sessions view shows another profile: the toolbar acts on it. */
        if (LOWORD(wp) == IDC_S_PROFILES && HIWORD(wp) == LBN_SELCHANGE) {
            SessionsView_Command(wp);
            if (SessionsView_Shown()) SelectProfileRow(SessionsView_Profile());
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_OPEN:          DoOpen(); return TRUE;
        case IDC_STOP:          DoQuit(FALSE); return TRUE;
        case IDC_RESTART:       DoQuit(TRUE); return TRUE;
        case IDC_SYNC:          if (ConfirmSync(SelectedProfiles())) RunJob(JOB_SYNC, SelectedProfiles()); return TRUE;
        case IDC_REPAIR:        DoRepair(); return TRUE;
        case IDC_COPY_ALL:      DoSessions(SESSIONS_COPY_ALL); return TRUE;
        case IDC_MOVE_ALL:      DoSessions(SESSIONS_MOVE_ALL); return TRUE;
        case IDC_SAME_STOP:     DoStopSame(); return TRUE;
        case IDC_SYNC_ITEMS:    DoSyncItems(); return TRUE;
        case IDC_NEW:           DoNew(); return TRUE;
        case IDC_EDIT:          DoEdit(); return TRUE;
        case IDC_DELETE:        DoDelete(); return TRUE;
        case IDC_MERGE:         DoSessions(SESSIONS_MERGE); return TRUE;
        case IDC_RESTORE:       DoSessions(SESSIONS_RESTORE); return TRUE;
        case IDC_PURGE:         DoSessions(SESSIONS_PURGE); return TRUE;
        case IDC_BACKUP_CODE:   if (!StateChangesBlocked()) SyncUi_BackUpCode(g_manager.dlg); return TRUE;
        case IDC_DEFAULT:       DoSetDefault(); return TRUE;
        case IDC_SC_DESKTOP:    DoDesktopShortcut(); return TRUE;
        case IDC_SC_SAVEAS:     DoSaveShortcut(); return TRUE;
        case IDC_SC_PIN:        DoPinTaskbar(); return TRUE;
        case IDC_SC_START:      DoStartMenu(); return TRUE;
        case IDC_SESSIONS:      ToggleSessions(); return TRUE;
        case IDC_UNINSTALL:     DoUninstall(); return TRUE;
        case IDC_STATUS_ACTION: DoStatusAction(); return TRUE;
        case IDC_UPDATE:        DoUpdate(); return TRUE;
        case IDCANCEL:
            /* Esc in a search box that holds text clears it. */
            if (!SessionsView_ClearSearch()) Close(dialog);
            return TRUE;
        }
        if (SessionsView_Command(wp)) return TRUE;
        break;

    case WM_CLOSE:
        /* Also sent by an installer updating the program: close any dialog
         * that is open too, so the process really exits. */
        Close(dialog);
        return TRUE;

    case WM_APP_UNINSTALL:
        if (StateChangesBlocked()) return TRUE;
        if (!IsWindowEnabled(dialog)) {
            g_manager.pendingUninstall = TRUE;   /* a dialog is open: after it closes */
            return TRUE;
        }
        DoUninstall();
        if (wp && g_manager.uninstallOnly && !g_manager.uninstalled && !g_manager.closing) EndDialog(dialog, 0);
        return TRUE;

    case WM_APP_SELECTION:
        g_manager.selectionPending = FALSE;
        UpdateButtons();
        return TRUE;

    case WM_APP_SHORTCUTS: {
        PIDLIST_ABSOLUTE *pidls = NULL;
        LONG event = 0;
        HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &pidls, &event);
        if (lock) SHChangeNotification_Unlock(lock);
        PostOnce(&g_manager.shortcutsCheckPending, WM_APP_SHORTCUTS_CHECK);
        return TRUE;
    }

    case WM_APP_SHORTCUTS_CHECK:
        g_manager.shortcutsCheckPending = FALSE;
        if (!StateChangesBlocked()) {
            g_manager.shortcutStateFolder[0] = 0;
            UpdateButtons();
        }
        return TRUE;

    case WM_APP_UPDATE:
        ShowUpdate();
        return TRUE;

    case WM_APP_DOWNLOADED:
        Downloaded((UpdateResult)wp, (DWORD)lp);
        return TRUE;

    case WM_APP_SETUPLINKS:
        if (StateChangesBlocked()) return TRUE;
        if (!IsWindowEnabled(dialog)) {
            g_manager.pendingSetUpLinks = TRUE;   /* a dialog is open: after it closes */
            return TRUE;
        }
        /* The question that follows must be seen, with the manager behind it. */
        if (!IsWindowVisible(dialog)) ShowWindow(dialog, SW_SHOWNORMAL);
        SetForegroundWindow(dialog);
        SetUpLinks();
        return TRUE;

    case WM_APP_SHOW:
        /* Opening the manager while a Settings uninstall is on screen: show
         * the manager once that dialog is cancelled. */
        g_manager.uninstallOnly = FALSE;
        return TRUE;

    case WM_APP_SYNC_CONFLICTS:
        /* From a sync of ours or of a watcher: asked once nothing else is
         * going on, the window shown and no dialog open. */
        g_manager.pendingConflicts = TRUE;
        if (StateChangesBlocked() || g_manager.job || !IsWindowVisible(dialog) || !IsWindowEnabled(dialog)) return TRUE;
        g_manager.pendingConflicts = FALSE;
        {
            DWORD settled = SyncUi_SettleConflicts(dialog, &g_manager.profiles);
            if (settled) RunJob(JOB_KEEP, settled);
        }
        return TRUE;

    case WM_ENABLE:
        if (wp && g_manager.pendingUninstall) {
            g_manager.pendingUninstall = FALSE;
            PostMessageW(dialog, WM_APP_UNINSTALL, 0, 0);
        }
        if (wp && g_manager.pendingSetUpLinks) {
            g_manager.pendingSetUpLinks = FALSE;
            PostMessageW(dialog, WM_APP_SETUPLINKS, 0, 0);
        }
        if (wp && g_manager.pendingConflicts) PostMessageW(dialog, WM_APP_SYNC_CONFLICTS, 0, 0);
        break;

    case WM_DRAWITEM:
        if (wp == IDC_PROGRESS) {
            DrawProgress((const DRAWITEMSTRUCT *)lp);
            return TRUE;
        }
        if (SessionsView_DrawItem((const DRAWITEMSTRUCT *)lp)) return TRUE;
        break;

    case WM_CONTEXTMENU:
        /* The list, from the keyboard, or the view around it below its rows. */
        if (((HWND)wp == g_manager.list || (HWND)wp == GetDlgItem(dialog, IDC_LIST)) && !SessionsView_Shown()) {
            ListMenu(lp);
            return TRUE;
        }
        if (SessionsView_ContextMenu((HWND)wp, lp)) return TRUE;
        break;

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return Theme_CtlColor(message, wp, lp, IDC_VERSION);

    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        Theme_Follow(dialog, message, wp, lp);
        break;

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) LayoutMainControls();
        return TRUE;

    case WM_GETMINMAXINFO:
        if (Gui_MainWindowGeometry(dialog, message, wp, lp)) return TRUE;
        break;

    case WM_DPICHANGED:
        Gui_MainWindowGeometry(dialog, message, wp, lp);
        PostMessageW(dialog, WM_APP_RELAYOUT, 0, 0);
        return TRUE;

    case WM_APP_RELAYOUT: {
        ListSelection selected;
        TakeSelection(&selected);
        SetIcons(dialog);
        ReflowMain();
        FillList(&selected);
        return TRUE;
    }

    case WM_DESTROY:
        g_manager.layoutReady = FALSE;
        FinishShellWork();
        Localize_ForgetWindow(dialog);
        StopWatchingOutside();
        SessionsView_Destroy();
        if (g_manager.shortcutsNotify) SHChangeNotifyDeregister(g_manager.shortcutsNotify);
        g_manager.shortcutsNotify = 0;
        if (g_manager.bigIcon) DestroyIcon(g_manager.bigIcon);
        if (g_manager.smallIcon) DestroyIcon(g_manager.smallIcon);
        g_manager.bigIcon = g_manager.smallIcon = NULL;
        /* A sync under way finishes: its thread posts to no window then. */
        if (g_manager.job) {
            WaitForSingleObject(g_manager.job, JOB_END_WAIT_MS);
            CloseHandle(g_manager.job);
            g_manager.job = NULL;
        }
        break;
    }
    return FALSE;
}

int Gui_Run(GuiStart start)
{
    INITCOMMONCONTROLSEX controls;
    WNDCLASSEXW windowClass;
    HANDLE mutex;
    HWND existing;
    WCHAR exe[MAX_PATH];
    INT_PTR dialogResult;

    if (!Install_IsInstalledCopy()) {
        if (start != GUI_UNINSTALL) return Install_Run(TRUE) ? 0 : 1;
        if (Util_InstallExe(exe, ARRAYSIZE(exe)) && Util_FileExists(exe)) {
            DWORD error;
            if ((INT_PTR)ShellExecuteW(NULL, L"open", exe, L"--uninstall", NULL, SW_SHOWNORMAL) > 32) return 0;
            error = GetLastError();
            Util_Log(L"could not start %s to uninstall (error %lu)", exe, error);
            Ui_Message(NULL, MB_ICONERROR,
                       TR(APP_NAME L" was installed but could not be started (error %lu):\n%s\n\nSecurity software may be blocking it."),
                       error, exe);
            return 1;
        }
        Ui_Message(NULL, MB_ICONINFORMATION, TR(APP_NAME L" is not installed."));
        return 1;
    }

    g_manager.readyEvent = CreateEventW(NULL, TRUE, FALSE, MANAGER_READY_EVENT);
    mutex = CreateMutexW(NULL, FALSE, MANAGER_MUTEX);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (g_manager.readyEvent) WaitForSingleObject(g_manager.readyEvent, MANAGER_READY_TIMEOUT_MS);
        existing = FindWindowW(APP_WINDOW_CLASS, NULL);
        if (existing) {
            /* A dialog open in the manager disables it: bring that forward. */
            HWND top;
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            top = GetLastActivePopup(existing);
            if (!top || !IsWindowVisible(top) || !IsWindowEnabled(top)) top = existing;
            SetForegroundWindow(top);
            PostMessageW(existing, start == GUI_UNINSTALL ? WM_APP_UNINSTALL : WM_APP_SHOW, 0, 0);
            if (start == GUI_SET_UP_LINKS) PostMessageW(existing, WM_APP_SETUPLINKS, 0, 0);
        }
        CloseHandle(mutex);
        if (g_manager.readyEvent) CloseHandle(g_manager.readyEvent);
        g_manager.readyEvent = NULL;
        return 0;
    }

    Install_Repair();
    SetCurrentProcessExplicitAppUserModelID(APP_MANAGER_AUMID);

    controls.dwSize = sizeof controls;
    controls.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS;
    InitCommonControlsEx(&controls);

    ZeroMemory(&windowClass, sizeof windowClass);
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = DefDlgProcW;
    windowClass.cbWndExtra = DLGWINDOWEXTRA;
    windowClass.hInstance = g_hInst;
    windowClass.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP));
    windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    windowClass.hbrBackground = (HBRUSH)(COLOR_3DFACE + 1);
    windowClass.lpszClassName = APP_WINDOW_CLASS;
    if (!RegisterClassExW(&windowClass)) Util_Log(L"could not register the manager's window class (error %lu)", GetLastError());

    dialogResult = DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainProc, (LPARAM)start);
    if (dialogResult == -1) Util_Log(L"could not create the manager's window (error %lu)", GetLastError());
    if (mutex) CloseHandle(mutex);
    if (g_manager.readyEvent) CloseHandle(g_manager.readyEvent);
    g_manager.readyEvent = NULL;
    if (g_manager.uninstalled) Install_FinishUninstall();
    return dialogResult == -1 ? 1 : 0;
}
