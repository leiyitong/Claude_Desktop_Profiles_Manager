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
#define WM_APP_GH_UPDATE       (WM_APP + 28)  /* the newest release of main is known: wParam asked, lParam a GhRelease */
#define WM_APP_GH_DOWNLOADED   (WM_APP + 29)  /* a release of main was downloaded, or not: lParam a GhDownload */
#define WM_APP_DOWNLOADED      (WM_APP + 8)   /* wParam: how the download went (UpdateResult), lParam: Windows' error */
#define WM_APP_SELECTION       (WM_APP + 9)   /* the list selection changed */
#define WM_APP_SHORTCUTS       (WM_APP + 10)  /* the desktop, Start menu or taskbar pins folder changed */
#define WM_APP_OUTSIDE         (WM_APP + 11)  /* something the window shows changed outside it (g_outside) */
#define WM_APP_SHORTCUTS_CHECK (WM_APP + 14)  /* once for a burst of WM_APP_SHORTCUTS (12 and 13 are the sessions view's) */
#define WM_APP_PROFILE_ACTION  (WM_APP + 15)  /* the profile dialog: value wParam's action, once the list's click is over */
#define WM_APP_KEEP_SESSIONS   (WM_APP + 16)  /* opened: the session lists kept, a linked session folder offered its own */
#define WM_APP_JOB_DONE        (WM_APP + 17)  /* an Open, Quit, Restart or Sync is done: lParam its Job */
#define WM_APP_FILES_DROPPED   (WM_APP + 18)  /* files dropped on the window: lParam a DroppedFiles (DropFiles frees it) */
#define WM_APP_DRAG_DROPPED    (WM_APP + 19)  /* profiles dragged out were let go in a folder: lParam for DragOut_Dropped */
#define WM_APP_DRAG_ZONE       (WM_APP + 24)
#define WM_APP_SHIELD_TRAY     (WM_APP + 26)  /* the shield's notification icon: lParam its event (NIN_*) */
#define WM_APP_SESSION_DROP    (WM_APP + 27)  /* a session drag was let go on the side bar (SessionsView_HandleDrop) */
#define SHIELD_TRAY_ID         0x5D1E  /* profiles dragged were let go on a tile: wParam its DragZone */

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
enum { CHANGE_PROFILES = 1, CHANGE_LINKS = 2, CHANGE_PACKAGES = 4, CHANGE_PINS = 8, CHANGE_RUNNING = 16 };

/* What the button below the status does (IDC_STATUS_ACTION). */
typedef enum StatusAction { STATUS_ACTION_NONE, STATUS_ACTION_GET_CLAUDE } StatusAction;

typedef struct MainState {
    HWND         dlg;
    HWND         list;
    WCHAR        profileSearch[128];
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
    BOOL         ghFound;          /* a release of main to update to (ghupdate.c), shown in the version's place */
    WCHAR        ghTag[GH_TAG_CCH];
    WCHAR        ghVersion[16];    /* its day */
    BOOL         ghViaGit;         /* found by git alone: Update opens its release page */
    BOOL         ghChecking;       /* a check asked for by a click on the version is under way */
    HANDLE       outsideThread;    /* waits for outside changes (WatchOutside) */
    HANDLE       outsideStop;
    struct RunningWatch *runningWatch;   /* what WatchRunning waits for */
    HANDLE       runningThread;
    HANDLE       readyEvent;       /* MANAGER_READY_EVENT */
    HICON        bigIcon;
    HICON        smallIcon;
    BOOL         uninstalled;      /* nothing may touch the registry any more */
    BOOL         uninstallInProgress; /* its dialog or the uninstall itself: no refresh */
    BOOL         closing;          /* asked to close (installer update) */
    BOOL         openAgain;        /* the windows' size changed: the window opens again at it, in a new process */
    BOOL         pendingUninstall; /* uninstall asked while a dialog was open */
    BOOL         pendingSetUpLinks; /* links setup asked while a dialog was open */
    BOOL         pendingConflicts;  /* a sync left conflicts to the person, not asked yet */
    BOOL         uninstallOnly;    /* started from Settings to uninstall: no manager window */
    BOOL         updating;         /* the newer release is downloading or installing */
    BOOL         installing;
    ShieldState  shieldState;      /* what the shield last reported (WM_APP_SHIELD) */
    BOOL         shieldTray;       /* the shield's notification icon is there (ShieldToast) */
    BOOL         selectionPending; /* WM_APP_SELECTION posted */
    BOOL         shortcutsCheckPending; /* WM_APP_SHORTCUTS_CHECK posted */
    BOOL         layoutReady, layingOut;
    BOOL         profileSearchShown;
    HHOOK        searchHook;
    HANDLE       shellWork;        /* the thread writing shortcuts and pins in a new language */
    BYTE         windowsMcp[MAX_PROFILES];   /* each profile's Windows-MCP (WINDOWS_MCP_*), as its row last read it */
    BOOL         searxng[MAX_PROFILES];      /* each profile has SearXNG search, as its row last read it */
    BOOL         cowork[MAX_PROFILES];       /* Cowork is on in each profile, as its row last read it */
    ULONGLONG    coworkBytes[MAX_PROFILES];  /* and the size of its virtual machine, 0 without one */
} MainState;

static MainState g_manager;
static volatile LONG g_outside;   /* CHANGE_* not handled yet: set by the thread, taken by the window */

/* While uninstalling or closing, nothing may reload state or write the
 * registry. */
static BOOL StateChangesBlocked(void)
{
    return g_manager.uninstalled || g_manager.uninstallInProgress || g_manager.closing;
}

/* Filtering changes row numbers; each row keeps the full list's index plus
 * one, so a missing item's zero cannot stand for the stock profile. */
static int ProfileIndexAtRow(int row)
{
    LPARAM value = row >= 0 ? Theme_RowParam(g_manager.list, row) : 0;
    return value > 0 && value <= g_manager.profiles.count ? (int)value - 1 : -1;
}

static int VisibleProfileRow(int profile)
{
    int row, count = ListView_GetItemCount(g_manager.list);
    if (profile < 0 || profile >= g_manager.profiles.count) return -1;
    for (row = 0; row < count; row++)
        if (ProfileIndexAtRow(row) == profile) return row;
    return -1;
}

static BOOL ProfileMatchesSearch(const Profile *profile)
{
    return !g_manager.profileSearch[0] || Core_ContainsI(profile->name, g_manager.profileSearch) ||
           Core_ContainsI(profile->folder, g_manager.profileSearch) || Core_ContainsI(profile->note, g_manager.profileSearch);
}

/* The profile selected alone: what Edit, Set as default and the shortcuts
 * act on; NULL when none or several are. */
static const Profile *SelectedProfile(void)
{
    int i;
    if (ListView_GetSelectedCount(g_manager.list) != 1) return NULL;
    i = ProfileIndexAtRow(ListView_GetNextItem(g_manager.list, -1, LVNI_SELECTED));
    return (i >= 0 && i < g_manager.profiles.count) ? &g_manager.profiles.items[i] : NULL;
}

/* Of the profiles selected, the one with the keyboard's focus, else the first. */
static const Profile *FocusedProfile(void)
{
    int i = ListView_GetNextItem(g_manager.list, -1, LVNI_FOCUSED | LVNI_SELECTED);
    if (i < 0) i = ListView_GetNextItem(g_manager.list, -1, LVNI_SELECTED);
    i = ProfileIndexAtRow(i);
    return (i >= 0 && i < g_manager.profiles.count) ? &g_manager.profiles.items[i] : NULL;
}

/* The profiles selected, one bit each (MAX_PROFILES <= 32). Each row found
 * must come after the last: a list not made yet answers 0 every time. */
static DWORD SelectedProfiles(void)
{
    DWORD bits = 0;
    int i = -1, next, profile;
    while ((next = ListView_GetNextItem(g_manager.list, i, LVNI_SELECTED)) > i) {
        profile = ProfileIndexAtRow(next);
        if (profile >= 0) bits |= 1u << profile;
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


/* The column's foot, and what of it shows in the profiles view (the
 * sessions view has no column): the status action and Update buttons take
 * room only while they show, the window laid out again when one of them
 * appears or goes. */
static const int kFootControls[] = { IDC_STATUS, IDC_PROGRESS, IDC_STATUS_ACTION, IDC_VERSION, IDC_UPDATE };
static BOOL g_footWanted[ARRAYSIZE(kFootControls)] = { TRUE, FALSE, FALSE, TRUE, FALSE };   /* as the resource has them */

static int FootIndex(int id)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(kFootControls); i++)
        if (kFootControls[i] == id) return i;
    return -1;
}

/* A control of the foot shown or hidden: at once in the profiles view, else
 * once that view shows again. */
static void ShowInFoot(int id, BOOL shown)
{
    int i = FootIndex(id);
    if (i >= 0) g_footWanted[i] = shown;
    ShowWindow(GetDlgItem(g_manager.dlg, id), shown && !SessionsView_Shown() ? SW_SHOW : SW_HIDE);
}

/* The foot of `dialog` as the view shown has it. */
static void ShowFoot(HWND dialog, BOOL profilesView)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(kFootControls); i++)
        ShowWindow(GetDlgItem(dialog, kFootControls[i]), profilesView && g_footWanted[i] ? SW_SHOW : SW_HIDE);
}

static void ShowOptionalButton(int id, BOOL shown)
{
    int i = FootIndex(id);
    if (i >= 0 && g_footWanted[i] == shown) return;
    ShowInFoot(id, shown);
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
            Profiles_JoinNames(&g_manager.profiles, SessionVault_Group(&g_manager.profiles, group) & ~(1u << i), names, ARRAYSIZE(names));
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
    WCHAR text[MAX_PATH], label[LABEL_CCH + 260], address[MAX_PATH];
    int row = VisibleProfileRow(i);
    g_manager.windowsMcp[i] = (BYTE)WindowsMcp_State(p);   /* its row's mark and the column's button */
    g_manager.searxng[i] = SearXng_Get(p, address, ARRAYSIZE(address), NULL);
    g_manager.cowork[i] = Cowork_Get(p, &g_manager.coworkBytes[i]);
    if (row < 0) return;
    if (p->note[0]) StringCchPrintfW(label, ARRAYSIZE(label), L"%s \x2014 %s", p->name, p->note);
    else StringCchCopyW(label, ARRAYSIZE(label), p->name);
    ListView_SetItemText(g_manager.list, row, 0, label);
    ListView_SetItemText(g_manager.list, row, 1,
                         (LPWSTR)TR(Theme_ProfileRole(p->isStock, Core_EqualsI(p->folder, g_manager.profiles.defaultFolder))));
    if (p->formerDir[0] && wcsrchr(p->dataDir, L'\\'))   /* moved out of Claude's package by Claude */
        StringCchPrintfW(text, ARRAYSIZE(text), L"%%LOCALAPPDATA%%\\" CLAUDE_MOVED_DATA L"%s", wcsrchr(p->dataDir, L'\\'));
    else
        StringCchPrintfW(text, ARRAYSIZE(text), L"%%APPDATA%%\\%s", p->folder);
    ListView_SetItemText(g_manager.list, row, 2, text);
    ListView_SetItemText(g_manager.list, row, 3, (LPWSTR)SessionsFolderText(i, text, ARRAYSIZE(text)));
    ListView_SetItemText(g_manager.list, row, PROFILE_COLUMN_STATE, (LPWSTR)TR(Theme_ProfileState(p->running)));
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

/* Status, the last column made, shows right after the profile's name. */
void Gui_OrderProfileColumns(HWND list)
{
    int order[PROFILE_COLUMN_STATE + 1], i, at = 0;
    order[at++] = 0;
    order[at++] = PROFILE_COLUMN_STATE;
    for (i = 1; i < PROFILE_COLUMN_STATE; i++) order[at++] = i;
    ListView_SetColumnOrderArray(list, ARRAYSIZE(order), order);
}

/* Default widths are independent of profile names and the selected language.
 * A user's adjustments persist; the data column follows the available space
 * (the window's minimum size keeps it at least its own minimum). */
void Gui_LayoutProfileColumns(HWND list)
{
    WCHAR text[MAX_PATH];
    int profileWidth, roleWidth, dataMinimum, sessionsMinimum, dataWidth, padding, i, count = ListView_GetItemCount(list), stateWidth;
    int actualProfile = ListView_GetColumnWidth(list, 0), actualRole = ListView_GetColumnWidth(list, 1);
    int actualData = ListView_GetColumnWidth(list, 2), actualState = ListView_GetColumnWidth(list, PROFILE_COLUMN_STATE);
    Theme_ProfileColumnWidths(list, &profileWidth, &roleWidth, &dataMinimum, &sessionsMinimum);
    stateWidth = Theme_ProfileStateWidth(list);
    if (Theme_ColumnResizeIsManual(list, 0)) profileWidth = actualProfile;
    if (Theme_ColumnResizeIsManual(list, 1)) roleWidth = actualRole;
    if (Theme_ColumnResizeIsManual(list, PROFILE_COLUMN_STATE)) stateWidth = actualState;
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
        dataWidth = max(dataMinimum, min(dataWidth, client.right - profileWidth - stateWidth - roleWidth - sessionsMinimum));
    }
    Theme_SetColumnWidth(list, 0, profileWidth);
    Theme_SetColumnWidth(list, PROFILE_COLUMN_STATE, stateWidth);
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
static void WatchRunningProfiles(void);
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

/* A filtered-out focused row must not add an unrelated first row when
 * another member of the old selection remains visible. */
static const WCHAR *VisibleSelectionFolder(const ListSelection *selection)
{
    int i;
    if (!selection) return NULL;
    if (VisibleProfileRow(Profiles_Find(&g_manager.profiles, selection->focused)) >= 0) return selection->focused;
    for (i = 0; i < selection->count; i++)
        if (VisibleProfileRow(Profiles_Find(&g_manager.profiles, selection->folders[i])) >= 0) return selection->folders[i];
    return selection->focused;
}

/* Selects the row of `folder` alone, else the first one: its index, -1 in
 * an empty list. */
static int SetSelectedRow(const WCHAR *folder)
{
    int row = VisibleProfileRow(Profiles_Find(&g_manager.profiles, folder));
    if (row < 0 && ListView_GetItemCount(g_manager.list) > 0) row = 0;
    ListView_SetItemState(g_manager.list, -1, 0, LVIS_SELECTED);
    if (row >= 0) ListView_SetItemState(g_manager.list, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    return row;
}

/* The program moves the selection: the row comes into sight. */
static void SelectProfileRow(const WCHAR *folder)
{
    int row;
    if (g_manager.profileSearch[0] && Profiles_Find(&g_manager.profiles, folder) >= 0 &&
        VisibleProfileRow(Profiles_Find(&g_manager.profiles, folder)) < 0)
        SetDlgItemTextW(g_manager.dlg, IDC_PROFILE_SEARCH, L"");
    row = SetSelectedRow(folder);
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
        if (!ProfileMatchesSearch(&g_manager.profiles.items[i])) continue;
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = ListView_GetItemCount(g_manager.list);
        item.lParam = i + 1;
        item.iImage = rowImages[i];
        item.pszText = g_manager.profiles.items[i].name;
        ListView_InsertItem(g_manager.list, &item);
        UpdateRow(i);
    }
    select = VisibleSelectionFolder(selection);
    row = SetSelectedRow(select);
    for (i = 0; selection && i < selection->count; i++)
        if ((other = VisibleProfileRow(Profiles_Find(&g_manager.profiles, selection->folders[i]))) >= 0 && other != row)
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
            a->items[i].picture != b->items[i].picture || wcscmp(a->items[i].note, b->items[i].note) != 0)
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
    Theme_FitName(GetDlgItem(dialog, IDC_U_KEEP),
            TR(L"Claude Desktop and its \x201C%s\x201D profile (the one the regular Claude icon opens) are not touched."),
            i >= 0 ? list->items[i].name : STOCK_DEFAULT_NAME, text, ARRAYSIZE(text));
    SetDlgItemTextW(dialog, IDC_U_KEEP, text);
}

/* The profiles of `selected` whose Windows-MCP is in `state` (WINDOWS_MCP_*), as their rows read it. */
static DWORD WithWindowsMcp(DWORD selected, int state)
{
    DWORD with = 0;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && g_manager.windowsMcp[i] == state) with |= 1u << i;
    return with;
}

/* A profile's marks at its name's end: with Windows-MCP a computer, amber while it needs Repair Windows MCP; with
 * SearXNG search a globe, left of it. Each as its button's. */
static int ProfileMarks(HWND list, int row, WCHAR *glyphs, ThemeTint *tints, int cap)
{
    int profile = ProfileIndexAtRow(row), count = 0;
    (void)list;
    if (profile < 0) return 0;
    if (count < cap && g_manager.windowsMcp[profile] != WINDOWS_MCP_NONE) {
        tints[count] = g_manager.windowsMcp[profile] == WINDOWS_MCP_OUTDATED ? THEME_TINT_AMBER : THEME_TINT_PURPLE;
        glyphs[count++] = 0xE7F4;   /* TVMonitor */
    }
    if (count < cap && g_manager.searxng[profile]) {
        tints[count] = THEME_TINT_PURPLE;   /* as Windows-MCP's */
        glyphs[count++] = 0xE774;   /* Globe */
    }
    return count;
}

static DWORD WithSearXng(DWORD selected)
{
    DWORD with = 0;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && g_manager.searxng[i]) with |= 1u << i;
    return with;
}

/* SearXNG search sets it up in the profiles selected; Remove SearXNG search once all of them have it. */
static void ShowSearXng(DWORD selected, BOOL idle)
{
    int state = selected && WithSearXng(selected) == selected ? 1 : 0;
    const WCHAR *caption = TR(Theme_MainCaption(IDC_SEARXNG, state));
    WCHAR shown[64];
    EnableControl(IDC_SEARXNG, selected && idle);
    GetDlgItemTextW(g_manager.dlg, IDC_SEARXNG, shown, ARRAYSIZE(shown));
    if (wcscmp(shown, caption) == 0) return;
    SetDlgItemTextW(g_manager.dlg, IDC_SEARXNG, caption);
    Theme_SetMainGlyph(g_manager.dlg, IDC_SEARXNG, state);
}

/* The profiles of `selected` whose Cowork can go: on, or its virtual machine still there. */
static DWORD WithCowork(DWORD selected)
{
    DWORD with = 0;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && (g_manager.cowork[i] || g_manager.coworkBytes[i])) with |= 1u << i;
    return with;
}

/* Remove Cowork from the profiles selected; Restore Cowork once none of them has it. */
static void ShowCowork(DWORD selected, BOOL idle)
{
    int state = selected && !WithCowork(selected) ? 1 : 0;
    const WCHAR *caption = TR(Theme_MainCaption(IDC_COWORK, state));
    WCHAR shown[64];
    EnableControl(IDC_COWORK, selected && idle);
    GetDlgItemTextW(g_manager.dlg, IDC_COWORK, shown, ARRAYSIZE(shown));
    if (wcscmp(shown, caption) == 0) return;
    SetDlgItemTextW(g_manager.dlg, IDC_COWORK, caption);
    Theme_SetMainGlyph(g_manager.dlg, IDC_COWORK, state);
}

/* Windows MCP adds it to the profiles selected, Repair Windows MCP when one
 * has it written without its transport, Remove Windows MCP once all of them
 * have it as it starts. */
static void ShowWindowsMcp(DWORD selected, BOOL idle)
{
    int state = selected && WithWindowsMcp(selected, WINDOWS_MCP_READY) == selected ? 1 : 0;
    const WCHAR *caption = state == 0 && WithWindowsMcp(selected, WINDOWS_MCP_OUTDATED) ? TR(L"Repair Windows &MCP")
                                                                                       : TR(Theme_MainCaption(IDC_WINDOWS_MCP, state));
    WCHAR shown[64];
    EnableControl(IDC_WINDOWS_MCP, selected && idle);
    GetDlgItemTextW(g_manager.dlg, IDC_WINDOWS_MCP, shown, ARRAYSIZE(shown));
    if (wcscmp(shown, caption) == 0) return;
    SetDlgItemTextW(g_manager.dlg, IDC_WINDOWS_MCP, caption);
    Theme_SetMainGlyph(g_manager.dlg, IDC_WINDOWS_MCP, state);
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
    EnableControl(IDC_OPEN, selected && g_manager.pkg.found && idle && !Shield_Blocking());
    EnableControl(IDC_STOP, selected && idle);   /* Claude running or not is read when it is pressed: nothing watches it */
    EnableControl(IDC_RESTART, selected && g_manager.pkg.found && idle && !Shield_Blocking());
    EnableControl(IDC_NEW, g_manager.profiles.count < MAX_PROFILES);
    EnableControl(IDC_EDIT, selected != 0);
    EnableControl(IDC_DELETE, Deletable(selected) != 0);
    EnableControl(IDC_SYNC, selected != 0);
    EnableControl(IDC_BACKUP, selected != 0);
    EnableControl(IDC_RESTORE_BACKUP, p != NULL);
    EnableControl(IDC_SHORTCUTS, selected != 0);
    EnableControl(IDC_REPAIR, idle);
    ShowWindowsMcp(selected, idle);
    ShowSearXng(selected, idle);
    ShowCowork(selected, idle);
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
    WatchRunningProfiles();
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

/* A new profile and an edit are a form (IDD_PROFILE_NEW): the profile's own
 * fields, how it syncs, and for a new one what it starts with; several
 * profiles edited together show only how they sync. The same values make a
 * property list too (checklist.c, IDD_PROFILE, as an export's: the profile's
 * own, Sync on its heading, then its settings under what to sync's groups),
 * read and shown through the same helpers, which the layout tests fill
 * (Gui_FillProfileList). */
enum { PV_NAME, PV_COLOR, PV_BADGE, PV_PICTURE, PV_TITLE, PV_NOTE, PV_STARTUP, PV_OPEN, PV_COPY, PV_SYNC, PV_SYNC_ITEMS, PV_COUNT };
#define PG_PROFILE             0
#define PG_SYNC                1
#define PG_LAYOUT              2   /* what to sync's groups from here on, Sessions left out */
#define PROFILE_SETTINGS       4000   /* the settings shown one by one at most, and their parts */
#define PROFILE_GLYPHS         48

/* The profile's own values by PV_*, shown as an export shows them
 * (Theme_ProfileOwnGlyph): its name, color, badge, picture, title, notes;
 * then the glyphs of the switches and Sync's, from PV_STARTUP. */
static const DWORD kOwnParts[PV_STARTUP] = { PROFILE_OWN_NAME, PROFILE_OWN_COLOR, PROFILE_OWN_BADGE, PROFILE_OWN_PICTURE, PROFILE_OWN_TITLE,
                                             PROFILE_OWN_NOTE };
static const WCHAR kProfileGlyphs[PV_COUNT - PV_STARTUP] = { 0xE7E8, 0xE8A7, 0xE8C8, 0xE895, 0xE9D5 };
static const ThemeTint kProfileTints[PV_COUNT - PV_STARTUP] = { THEME_TINT_GREEN, THEME_TINT_BLUE, THEME_TINT_TEAL, THEME_TINT_BLUE, THEME_TINT_TEAL };
#define PROFILE_LAYOUT_ITEMS   (SYNC_ITEMS_ALL & ~SYNC_ITEM_SESSIONS)
#define PROFILE_OPENED_SECTION L"ProfileOpened"

typedef struct ProfileDialog {
    HWND           dialog;
    BOOL           form;       /* the new profile's form: its values in its controls, not in a list */
    BOOL           filling;    /* the form sets the badge's text itself */
    const Profile *existing;   /* NULL: a new profile, or several edited */
    DWORD          several;    /* the profiles edited together (two or more): their sessions only */
    const Profile *copyFrom;   /* new profile: whose settings it can start with */
    WCHAR          name[LABEL_CCH];
    int            color;
    WCHAR          badge[BADGE_CCH];   /* once closed: the badge's own text, empty for the initial */
    WCHAR          windowTitle[WINDOW_TITLE_CCH];   /* once closed: its windows' title, empty for the default */
    WCHAR          note[256];
    DWORD         *picture;            /* the picture shown, NULL for the Claude icon; the caller frees it */
    BOOL           pictureChanged;     /* chosen or removed in the dialog */
    BOOL           badgeOwn;           /* the badge shows text the user typed, not the initial */
    BOOL           openNow;
    BOOL           startup;
    BOOL           copy;
    BOOL           signIn;     /* new profile: Save and sign in now, its private sign-in once made */
    int            sync;       /* once closed, what its sessions do: 0 kept apart, a group joined, or -1 - a profile to keep them the same as */
    DWORD          syncItems;  /* what it syncs with them, chosen in the dialog (SYNC_ITEM_*); 0: as it was */
    WCHAR          syncFolder[FOLDER_CCH];     /* once closed, the profile chosen to keep the same as, when one is */
    const char    *layout;     /* its settings as the dialog opens (SessionSync_ReadLayout), the caller's; NULL: none */
    SessionNames   names;      /* its sessions by their ids, for the settings that name them (the caller's) */
    size_t         layoutLength;
    BOOL           running;    /* its Claude runs as the dialog opens: its settings only show */
    char          *settings;   /* once closed: the settings changed, as a layout of their parts alone (a heap block the caller frees) */
    size_t         settingsLength;
    CheckList      list;
    CheckGroup     groups[CHECKLIST_GROUPS_MAX];
    WCHAR          glyphs[PROFILE_GLYPHS];
    ThemeTint      tints[PROFILE_GLYPHS];
    CheckValue    *values;     /* a heap block the caller frees (FreeProfileValues), once the dialog is gone */
    int            at[PV_COUNT];   /* each value of the profile's own and of Sync, -1: not shown */
    int            layoutFirst;    /* the first of its settings */
    int            ownImage;       /* the first of the values' glyphs (PV_*) in the list's images */
    int            colorAt[PALETTE_SIZE];   /* each color choice's palette index: the colors no other profile uses first */
    WCHAR          colorText[PALETTE_SIZE][CHECK_VALUE_CCH];
    const WCHAR   *colorChoices[PALETTE_SIZE];
    COLORREF       swatches[PALETTE_SIZE];  /* a line above the first color another profile uses (THEME_CHOICE_SEPARATED) */
    int            syncAt[MAX_PROFILES + 1];   /* each sync choice's value */
    WCHAR          syncText[MAX_PROFILES + 1][CHECK_VALUE_CCH];
    const WCHAR   *syncChoices[MAX_PROFILES + 1];
    WCHAR          titleHint[LABEL_CCH + 16];   /* the default title, greyed in the empty title's box */
    WCHAR          badgeHint[4];               /* the initial, greyed in the empty badge's box */
    HICON          preview;
    WCHAR          previewBadge[BADGE_CCH];   /* what `preview` shows, so typing redraws it only when this changes */
    const DWORD   *previewPicture;
    int            previewColor;
    int            previewSize;
} ProfileDialog;

static CheckValue *ValueOf(const ProfileDialog *state, int which)
{
    return state->values && state->at[which] >= 0 ? &state->values[state->at[which]] : NULL;
}

static const WCHAR *TextOf(const ProfileDialog *state, int which)
{
    const CheckValue *value = ValueOf(state, which);
    return value ? value->text : L"";
}

static void FreeProfileValues(ProfileDialog *state)
{
    Util_Free(state->values);
    state->values = NULL;
}

/* The name typed, in the form's field or the list's value. */
static void TypedName(const ProfileDialog *state, WCHAR *name, size_t cch)
{
    if (state->form) GetDlgItemTextW(state->dialog, IDC_P_NAME, name, (int)cch);
    else StringCchCopyW(name, cch, TextOf(state, PV_NAME));
}

/* A drop-down of the form showing `value`'s choices as the list would
 * (FillColorChoices, FillSyncChoices), with their swatches. */
static void FillChoiceBox(HWND combo, const CheckValue *value)
{
    int i;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < value->choiceCount; i++) {
        LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)value->choices[i]);
        if (index >= 0 && value->swatches) SendMessageW(combo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)value->swatches[i] | THEME_CHOICE_SWATCH);
    }
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)max(value->choice, 0), 0);
}

/* The choice of a value: the form's drop-down `id`, or the list's value `which`. */
static int ChoiceOf(const ProfileDialog *state, int id, int which)
{
    const CheckValue *value = ValueOf(state, which);
    if (state->form) return (int)SendDlgItemMessageW(state->dialog, id, CB_GETCURSEL, 0, 0);
    return value ? value->choice : -1;
}

/* The color choices with their swatches: the colors no other profile uses,
 * then the others, under a line, each with the profiles that use it. */
static void FillColorChoices(ProfileDialog *state, CheckValue *value)
{
    WCHAR users[PALETTE_SIZE][MAX_PROFILES * 8], usedBy[ARRAYSIZE(users[0]) + 64];
    BOOL used[PALETTE_SIZE] = { 0 }, anyFree = FALSE;
    int i, pass, count = 0;
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
    value->choice = 0;
    for (pass = 0; pass < 2; pass++) {
        BOOL first = TRUE;
        for (i = 0; i < PALETTE_SIZE; i++) {
            if (used[i] != (pass == 1)) continue;
            state->swatches[count] = Icons_PaletteColor(i);
            if (used[i]) {
                /* After the tab: in the menu only (theme.c). */
                StringCchPrintfW(usedBy, ARRAYSIZE(usedBy), TR(L"used by %s"), users[i]);
                StringCchPrintfW(state->colorText[count], ARRAYSIZE(state->colorText[count]), L"%s\t%s", TR(g_ColorNames[i]), usedBy);
                if (first && anyFree) state->swatches[count] |= THEME_CHOICE_SEPARATED;
            } else {
                StringCchCopyW(state->colorText[count], ARRAYSIZE(state->colorText[count]), TR(g_ColorNames[i]));
            }
            first = FALSE;
            if (i == state->color) value->choice = count;
            state->colorChoices[count] = state->colorText[count];
            state->colorAt[count++] = i;
        }
    }
    value->choices = state->colorChoices;
    value->swatches = state->swatches;
    value->choiceCount = count;
}

static int ChosenColor(const ProfileDialog *state)
{
    int choice = ChoiceOf(state, IDC_P_COLOR, PV_COLOR);
    return choice >= 0 && choice < PALETTE_SIZE ? state->colorAt[choice] : state->color;
}

/* The badge's own text as typed, cleaned; empty when it is the initial of
 * `name` (the badge then follows the name). */
static void ChosenBadge(const ProfileDialog *state, const WCHAR *name, WCHAR *badge, size_t cch)
{
    WCHAR typed[32], initial[2] = { 0, 0 };
    if (state->form) GetDlgItemTextW(state->dialog, IDC_P_BADGE, typed, ARRAYSIZE(typed));
    else StringCchCopyW(typed, ARRAYSIZE(typed), TextOf(state, PV_BADGE));
    initial[0] = Icons_ProfileInitial(name);
    if (!state->badgeOwn || !Core_CleanBadge(typed, badge, cch) || wcscmp(badge, initial) == 0) badge[0] = 0;
}

/* The title's box shows, greyed while empty, the default title: Claude's,
 * then the name typed; the badge's, the name's initial. */
static void ShowDefaults(ProfileDialog *state)
{
    WCHAR name[LABEL_CCH];
    TypedName(state, name, ARRAYSIZE(name));
    StringCchPrintfW(state->titleHint, ARRAYSIZE(state->titleHint), L"Claude (%s)", name[0] ? name : L"\x2026");
    state->badgeHint[0] = Icons_ProfileInitial(name);
    state->badgeHint[1] = 0;
    if (state->form) SendDlgItemMessageW(state->dialog, IDC_P_TITLE, EM_SETCUEBANNER, TRUE, (LPARAM)state->titleHint);
}

/* The badge shows the name's initial until the person types their own. */
static void FollowName(ProfileDialog *state)
{
    CheckValue *badge = ValueOf(state, PV_BADGE);
    if (state->form && !state->badgeOwn) {
        state->filling = TRUE;
        SetDlgItemTextW(state->dialog, IDC_P_BADGE, state->badgeHint);
        state->filling = FALSE;
    }
    if (!badge || state->badgeOwn) return;
    StringCchCopyW(badge->text, ARRAYSIZE(badge->text), state->badgeHint);
    CheckList_ShowValue(&state->list, state->at[PV_BADGE]);
}

/* Picture's action says what it does: choose one, or change the one there. */
static void ShowPicture(ProfileDialog *state)
{
    CheckValue *value = ValueOf(state, PV_PICTURE);
    /* The form's picture is two buttons: the badge stays editable for when the picture goes. */
    if (state->form) EnableWindow(GetDlgItem(state->dialog, IDC_P_NO_PICTURE), state->picture != NULL);
    if (!value) return;
    StringCchCopyW(value->text, ARRAYSIZE(value->text), state->picture ? TR(L"Change\x2026") : TR(L"Choose\x2026"));
    CheckList_ShowValue(&state->list, state->at[PV_PICTURE]);
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
 * then). The group they keep the same now is chosen. */
static void FillSyncChoices(ProfileDialog *state, CheckValue *value)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)];
    DWORD editing = DialogProfiles(state), members;
    int current = -1, count = 0, group, i;
    for (i = 0; i < g_manager.profiles.count; i++) {
        if (!(editing & (1u << i))) continue;
        if (current < 0) current = g_manager.profiles.items[i].syncGroup;
        else if (current != g_manager.profiles.items[i].syncGroup) current = 0;
    }
    value->choice = 0;
    StringCchCopyW(state->syncText[count], ARRAYSIZE(state->syncText[count]), TR(L"Not synced"));
    state->syncAt[count++] = 0;
    for (group = 1; group <= MAX_PROFILES && count < (int)ARRAYSIZE(state->syncAt); group++) {
        if ((members = SessionVault_Group(&g_manager.profiles, group) & ~editing) == 0) continue;
        Profiles_JoinNames(&g_manager.profiles, members, names, ARRAYSIZE(names));
        StringCchPrintfW(state->syncText[count], ARRAYSIZE(state->syncText[count]), TR(L"With %s"), names);
        if (group == current) value->choice = count;
        state->syncAt[count++] = group;
    }
    for (i = 0; i < g_manager.profiles.count && count < (int)ARRAYSIZE(state->syncAt); i++) {
        if ((editing & (1u << i)) || g_manager.profiles.items[i].syncGroup) continue;
        StringCchPrintfW(state->syncText[count], ARRAYSIZE(state->syncText[count]), TR(L"With %s"), g_manager.profiles.items[i].name);
        state->syncAt[count++] = -1 - i;
    }
    for (i = 0; i < count; i++) state->syncChoices[i] = state->syncText[i];
    value->choices = state->syncChoices;
    value->choiceCount = count;
}

static int ChosenSessions(const ProfileDialog *state)
{
    const CheckValue *value = ValueOf(state, PV_SYNC);
    int choice = ChoiceOf(state, IDC_P_SYNC, PV_SYNC), count = state->form ? (int)ARRAYSIZE(state->syncAt) : value ? value->choiceCount : 0;
    return choice >= 0 && choice < count ? state->syncAt[choice] : 0;
}

/* What to sync is chosen while the profiles sync with others. */
static void ShowSyncItems(ProfileDialog *state)
{
    CheckValue *value = ValueOf(state, PV_SYNC_ITEMS);
    if (state->form) EnableWindow(GetDlgItem(state->dialog, IDC_P_SYNC_ITEMS), ChosenSessions(state) != 0);
    if (!value) return;
    value->readOnly = ChosenSessions(state) == 0;
    StringCchCopyW(value->text, ARRAYSIZE(value->text), value->readOnly ? L"" : TR(L"Choose\x2026"));
    CheckList_ShowValue(&state->list, state->at[PV_SYNC_ITEMS]);
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
    DWORD editing = DialogProfiles(state), others = SyncedWith(ChosenSessions(state)) & ~editing, items;
    if (!others) return;
    items = state->syncItems ? state->syncItems : SessionVault_GroupItems(&g_manager.profiles, others);
    if (SyncUi_ChooseItems(dialog, &g_manager.profiles, editing | others, &items)) state->syncItems = items;
}

static CheckValue *AddProfileValue(ProfileDialog *state, int which, int group, int kind, const WCHAR *label)
{
    CheckValue *value = &state->values[state->list.valueCount];
    ZeroMemory(value, sizeof *value);
    value->group = group;
    value->kind = kind;
    value->image = state->ownImage + which;
    value->label = label;
    value->pick = -1;
    state->at[which] = state->list.valueCount++;
    return value;
}

static void AddProfileText(ProfileDialog *state, int which, const WCHAR *label, const WCHAR *text, int limit, const WCHAR *hint)
{
    CheckValue *value = AddProfileValue(state, which, PG_PROFILE, CHECK_VALUE_TEXT, label);
    StringCchCopyW(value->text, ARRAYSIZE(value->text), text);
    value->limit = limit;
    value->hint = hint;
}

static void AddProfileSwitch(ProfileDialog *state, int which, const WCHAR *label, BOOL on)
{
    AddProfileValue(state, which, PG_PROFILE, CHECK_VALUE_SWITCH, label)->on = on;
}

/* The list: the groups (Profile, Sync, then what to sync's), their glyphs,
 * the profile's own values, Sync's, then its settings; the item rows those
 * have, shown. */
static void SetupProfileList(HWND list, ProfileDialog *state)
{
    const CheckGroup *kinds;
    const WCHAR *glyphs;
    const ThemeTint *tints;
    CheckValue *value;
    int kindCount = 0, images = 0, glyphCount, g, i;
    DWORD shown = 0;
    for (i = 0; i < PV_COUNT; i++) state->at[i] = -1;
    glyphs = SyncUi_Glyphs(&tints, &images);
    for (i = 0; i < images && i < PROFILE_GLYPHS - 2 - PV_COUNT; i++) {
        state->glyphs[i] = glyphs[i];
        state->tints[i] = tints[i];
    }
    state->glyphs[i] = 0xE77B;   /* a person: the profile's own */
    state->tints[i] = THEME_TINT_TEAL;
    state->glyphs[i + 1] = 0xE895;   /* Sync */
    state->tints[i + 1] = THEME_TINT_BLUE;
    state->ownImage = i + 2;
    for (g = 0; g < PV_COUNT; g++) {
        ThemeTint *tint = &state->tints[state->ownImage + g];
        if (g < PV_STARTUP) {
            state->glyphs[state->ownImage + g] = Theme_ProfileOwnGlyph(kOwnParts[g], tint);
        } else {
            state->glyphs[state->ownImage + g] = kProfileGlyphs[g - PV_STARTUP];
            *tint = kProfileTints[g - PV_STARTUP];
        }
    }
    glyphCount = state->ownImage + PV_COUNT;
    ZeroMemory(state->groups, sizeof state->groups);
    state->groups[PG_PROFILE].heading = L"Profile";
    state->groups[PG_PROFILE].image = i;
    state->groups[PG_PROFILE].name = L"profile";
    state->groups[PG_SYNC].heading = L"Sync";
    state->groups[PG_SYNC].image = i + 1;
    state->groups[PG_SYNC].name = L"sync";
    kinds = SyncUi_Groups(&kindCount);
    for (g = 1; g < kindCount && PG_LAYOUT + g - 1 < CHECKLIST_GROUPS_MAX; g++) state->groups[PG_LAYOUT + g - 1] = kinds[g];
    ZeroMemory(&state->list, sizeof state->list);
    state->list.groups = state->groups;
    state->list.groupCount = PG_LAYOUT + g - 1;
    state->list.items = SyncUi_Items(&state->list.itemCount);
    state->list.plain = TRUE;
    state->list.openedSection = PROFILE_OPENED_SECTION;
    state->list.openAtFirst = (1u << PG_PROFILE) | (1u << PG_SYNC);
    state->values = (CheckValue *)Util_Alloc((PV_COUNT + PROFILE_SETTINGS) * sizeof *state->values);
    state->list.values = state->values;
    if (state->values && !state->several) {
        AddProfileText(state, PV_NAME, L"Name", state->existing ? state->existing->name : L"", state->existing ? MAX_LABEL : MAX_NAME, NULL);
        ShowDefaults(state);
        FillColorChoices(state, AddProfileValue(state, PV_COLOR, PG_PROFILE, CHECK_VALUE_CHOICE, L"Color"));
        AddProfileText(state, PV_BADGE, L"Badge", state->badgeOwn ? state->existing->badge : state->badgeHint, 2 * MAX_BADGE, state->badgeHint);
        AddProfileValue(state, PV_PICTURE, PG_PROFILE, CHECK_VALUE_ACTION, L"Picture");
        AddProfileText(state, PV_TITLE, L"Title", state->existing ? state->existing->windowTitle : L"", WINDOW_TITLE_CCH - 1, state->titleHint);
        AddProfileText(state, PV_NOTE, L"Notes", state->existing ? state->existing->note : L"", ARRAYSIZE(state->note) - 1, NULL);
        AddProfileSwitch(state, PV_STARTUP, L"Open at Windows sign-in", state->startup);
    }
    if (state->values) {
        value = AddProfileValue(state, PV_SYNC, PG_SYNC, CHECK_VALUE_CHOICE, L"Sync");
        value->merged = TRUE;   /* on Sync's heading */
        FillSyncChoices(state, value);
        AddProfileValue(state, PV_SYNC_ITEMS, PG_SYNC, CHECK_VALUE_ACTION, L"What to sync");
    }
    /* Its settings, under items offered so that their rows show. */
    state->layoutFirst = state->list.valueCount;
    state->list.offered = PROFILE_LAYOUT_ITEMS;
    if (state->values && !state->several) {
        SyncUi_AddLayoutValues(&state->list, PV_COUNT + PROFILE_SETTINGS, state->layout, state->layoutLength, PROFILE_LAYOUT_ITEMS, FALSE,
                               &state->names);
        for (i = state->layoutFirst; i < state->list.valueCount; i++) {
            shown |= state->values[i].item;
            if (state->running) state->values[i].readOnly = TRUE;
        }
    }
    state->list.offered = state->list.checked = shown;
    CheckList_Setup(&state->list, list, state->glyphs, state->tints, glyphCount);
    ShowPicture(state);
    ShowSyncItems(state);
}

static BOOL ValidateProfileDialog(HWND dialog, ProfileDialog *state, BOOL showError)
{
    WCHAR clean[LABEL_CCH], folder[FOLDER_CCH], info[MAX_PROFILES * (LABEL_CCH + 8) + MAX_PATH], appData[MAX_PATH], dir[MAX_PATH];
    WCHAR placeholder[FOLDER_CCH], raw[LABEL_CCH];
    const WCHAR *error = NULL;   /* English, translated where it shows */
    BOOL ok, errorShown;

    if (state->several) {
        Profiles_JoinNames(&g_manager.profiles, state->several, info, ARRAYSIZE(info));
        SetDlgItemTextW(dialog, IDC_P_FOLDER, info);
        EnableWindow(GetDlgItem(dialog, IDOK), TRUE);
        return TRUE;
    }
    TypedName(state, raw, ARRAYSIZE(raw));
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
        if (state->running && state->layoutFirst < state->list.valueCount) {
            StringCchCatW(info, ARRAYSIZE(info), L"\n");
            StringCchCatW(info, ARRAYSIZE(info), TR(L"Its Claude is open: close it to change its settings."));
        }
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
    if (state->several) return;
    ZeroMemory(&sample, sizeof sample);
    TypedName(state, sample.name, ARRAYSIZE(sample.name));
    sample.color = ChosenColor(state);
    ChosenBadge(state, sample.name, sample.badge, ARRAYSIZE(sample.badge));
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

/* Picture's action: a picture chosen, or, when there is one, a menu to
 * choose another or remove it, below the value (in the list or its pane,
 * where it was clicked). */
static void PictureAction(HWND dialog, ProfileDialog *state)
{
    HWND list = state->list.changedIn ? state->list.changedIn : state->list.list;
    HMENU menu;
    RECT box;
    UINT cmd = 1;
    int row = ListView_GetNextItem(list, -1, LVNI_SELECTED);
    if (state->picture) {
        if ((menu = CreatePopupMenu()) == NULL) return;
        AppendMenuW(menu, MF_STRING, 1, TR(L"Choose &picture\x2026"));
        AppendMenuW(menu, MF_STRING, 2, TR(L"Re&move picture"));
        if (row < 0 || !ListView_GetSubItemRect(list, row, 1, LVIR_BOUNDS, &box)) GetClientRect(list, &box);
        MapWindowPoints(list, NULL, (POINT *)&box, 2);
        cmd = Theme_TrackMenu(list, menu, &box);
        DestroyMenu(menu);
    }
    if (cmd == 1) {
        ChoosePicture(dialog, state);
    } else if (cmd == 2) {
        HeapFree(GetProcessHeap(), 0, state->picture);
        state->picture = NULL;
        state->pictureChanged = TRUE;
    }
    ShowPicture(state);
    UpdatePreview(dialog, state);
}

/* Value `v` changed in the list (WM_CHECKLIST_VALUE). An action waits for
 * the list's click to be over (WM_APP_PROFILE_ACTION). */
static void ProfileValueChanged(HWND dialog, ProfileDialog *state, int v)
{
    if (v < 0) return;
    if (v == state->at[PV_NAME]) {
        ValidateProfileDialog(dialog, state, TRUE);
        ShowDefaults(state);
        FollowName(state);
        UpdatePreview(dialog, state);
    } else if (v == state->at[PV_BADGE]) {
        /* Emptied, the badge goes back to the initial. */
        state->badgeOwn = state->values[v].text[0] != 0;
        UpdatePreview(dialog, state);
    } else if (v == state->at[PV_COLOR]) {
        UpdatePreview(dialog, state);
    } else if (v == state->at[PV_SYNC]) {
        ShowSyncItems(state);
    } else if (v == state->at[PV_PICTURE] || v == state->at[PV_SYNC_ITEMS]) {
        PostMessageW(dialog, WM_APP_PROFILE_ACTION, (WPARAM)v, 0);
    }
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

/* The new profile's form: the same choices as the list's, in its fields. */
static void InitProfileForm(HWND dialog, ProfileDialog *state)
{
    CheckValue choices;
    static const int kOwnFields[] = { IDC_P_NAME_LABEL, IDC_P_NAME, IDC_P_COLOR_LABEL, IDC_P_COLOR, IDC_P_PREVIEW, IDC_P_BADGE_LABEL,
                                      IDC_P_BADGE, IDC_P_PICTURE, IDC_P_NO_PICTURE, IDC_P_TITLE_LABEL, IDC_P_TITLE, IDC_P_NOTE_LABEL,
                                      IDC_P_NOTE, IDC_P_STARTUP };
    size_t i;
    SendDlgItemMessageW(dialog, IDC_P_NAME, EM_LIMITTEXT, state->existing ? MAX_LABEL : MAX_NAME, 0);
    SendDlgItemMessageW(dialog, IDC_P_BADGE, EM_LIMITTEXT, 2 * MAX_BADGE, 0);   /* surrogate pairs too */
    SendDlgItemMessageW(dialog, IDC_P_TITLE, EM_LIMITTEXT, WINDOW_TITLE_CCH - 1, 0);
    SendDlgItemMessageW(dialog, IDC_P_NOTE, EM_LIMITTEXT, ARRAYSIZE(state->note) - 1, 0);
    ZeroMemory(&choices, sizeof choices);
    FillColorChoices(state, &choices);
    FillChoiceBox(GetDlgItem(dialog, IDC_P_COLOR), &choices);
    ZeroMemory(&choices, sizeof choices);
    FillSyncChoices(state, &choices);
    FillChoiceBox(GetDlgItem(dialog, IDC_P_SYNC), &choices);
    /* An edit's own values, or those a new profile starts with (a copy's), filled before the badge follows the name. */
    SetDlgItemTextW(dialog, IDC_P_NAME, state->existing ? state->existing->name : state->name);
    SetDlgItemTextW(dialog, IDC_P_TITLE, state->existing ? state->existing->windowTitle : state->windowTitle);
    SetDlgItemTextW(dialog, IDC_P_NOTE, state->existing ? state->existing->note : state->note);
    ShowDefaults(state);
    FollowName(state);
    if (state->badgeOwn) {
        state->filling = TRUE;
        SetDlgItemTextW(dialog, IDC_P_BADGE, state->existing ? state->existing->badge : state->badge);
        state->filling = FALSE;
    }
    CheckDlgButton(dialog, IDC_P_COPY, state->copy ? BST_CHECKED : BST_UNCHECKED);
    ShowPicture(state);
    ShowSyncItems(state);
    CheckDlgButton(dialog, IDC_P_STARTUP, state->startup ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_P_OPEN, state->openNow ? BST_CHECKED : BST_UNCHECKED);
    /* What the form does not offer leaves no empty row (Theme_FitDialog closes it): what a new profile
     * starts with only for a new one, and for several profiles their sync alone. */
    ShowWindow(GetDlgItem(dialog, IDC_P_OPEN), g_manager.pkg.found && !state->existing && !state->several ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dialog, IDC_P_SIGN_IN), !state->existing && !state->several ? SW_SHOW : SW_HIDE);
    if (state->copyFrom) SetCopyLabel(GetDlgItem(dialog, IDC_P_COPY), state->copyFrom->name);
    else ShowWindow(GetDlgItem(dialog, IDC_P_COPY), SW_HIDE);
    if (state->several)
        for (i = 0; i < ARRAYSIZE(kOwnFields); i++) ShowWindow(GetDlgItem(dialog, kOwnFields[i]), SW_HIDE);
    ValidateProfileDialog(dialog, state, FALSE);
    UpdatePreview(dialog, state);
    SetFocus(GetDlgItem(dialog, state->several ? IDC_P_SYNC : IDC_P_NAME));
    if (state->existing) SendDlgItemMessageW(dialog, IDC_P_NAME, EM_SETSEL, 0, -1);
    PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);   /* Ui_Dialog fits the dialog after this message */
}

/* WM_INITDIALOG: the dialog shows `state`; the focus is set. */
static void InitProfileDialog(HWND dialog, ProfileDialog *state)
{
    state->dialog = dialog;
    SetWindowTextW(dialog, state->several ? TR(L"Edit profiles") : state->existing ? TR(L"Edit profile") : TR(L"New profile"));
    if (state->existing) state->badgeOwn = state->existing->badge[0] != 0;   /* a copy's is set by its caller */
    if (state->existing && state->existing->picture) state->picture = Icons_LoadPicture(state->existing);
    if (state->form) {
        InitProfileForm(dialog, state);
        return;
    }
    SetupProfileList(GetDlgItem(dialog, IDC_P_LIST), state);
    Theme_SmoothView(state->list.list);
    if (state->several) ShowWindow(GetDlgItem(dialog, IDC_P_PREVIEW), SW_HIDE);
    ValidateProfileDialog(dialog, state, FALSE);
    UpdatePreview(dialog, state);
    CheckList_SelectValue(&state->list, state->at[state->several ? PV_SYNC : PV_NAME], !state->several);
    if (state->several) SetFocus(state->list.list);
    PostMessageW(dialog, WM_APP_FIT_NAMES, 0, 0);   /* Ui_Dialog fits the dialog after this message */
}

/* What the person chose, into `state`; FALSE when the name is not valid. */
static BOOL TakeProfileChoices(HWND dialog, ProfileDialog *state)
{
    const CheckValue *value;
    if (!ValidateProfileDialog(dialog, state, TRUE)) return FALSE;
    state->sync = ChosenSessions(state);
    if (state->sync < 0 && -1 - state->sync < g_manager.profiles.count)
        StringCchCopyW(state->syncFolder, ARRAYSIZE(state->syncFolder), g_manager.profiles.items[-1 - state->sync].folder);
    if (state->several) return TRUE;
    state->color = ChosenColor(state);
    ChosenBadge(state, state->name, state->badge, ARRAYSIZE(state->badge));
    if (state->form) {
        GetDlgItemTextW(dialog, IDC_P_TITLE, state->windowTitle, ARRAYSIZE(state->windowTitle));
        GetDlgItemTextW(dialog, IDC_P_NOTE, state->note, ARRAYSIZE(state->note));
        state->openNow = IsDlgButtonChecked(dialog, IDC_P_OPEN) == BST_CHECKED;
        state->startup = IsDlgButtonChecked(dialog, IDC_P_STARTUP) == BST_CHECKED;
        state->copy = state->copyFrom && IsDlgButtonChecked(dialog, IDC_P_COPY) == BST_CHECKED;
        return TRUE;
    }
    StringCchCopyW(state->windowTitle, ARRAYSIZE(state->windowTitle), TextOf(state, PV_TITLE));
    StringCchCopyW(state->note, ARRAYSIZE(state->note), TextOf(state, PV_NOTE));
    state->openNow = (value = ValueOf(state, PV_OPEN)) != NULL && value->on;
    state->startup = (value = ValueOf(state, PV_STARTUP)) != NULL && value->on;
    state->copy = (value = ValueOf(state, PV_COPY)) != NULL && value->on;
    state->settings = SyncUi_LayoutChanges(&state->list, state->layoutFirst, state->layout, state->layoutLength, &state->settingsLength);
    return TRUE;
}

/* The form's WM_COMMAND, but OK and Cancel: TRUE when it is handled. */
static BOOL ProfileFormCommand(HWND dialog, ProfileDialog *state, WPARAM wp)
{
    switch (LOWORD(wp)) {
    case IDC_P_NAME:
        if (HIWORD(wp) == EN_CHANGE) {
            ValidateProfileDialog(dialog, state, TRUE);
            ShowDefaults(state);
            FollowName(state);
            UpdatePreview(dialog, state);
        }
        return TRUE;
    case IDC_P_BADGE:
        if (HIWORD(wp) == EN_CHANGE && !state->filling) {
            /* Emptied, the badge goes back to the initial. */
            state->badgeOwn = GetWindowTextLengthW(GetDlgItem(dialog, IDC_P_BADGE)) > 0;
            UpdatePreview(dialog, state);
        }
        return TRUE;
    case IDC_P_PICTURE:
        if (HIWORD(wp) == BN_CLICKED) {
            ChoosePicture(dialog, state);
            ShowPicture(state);
            UpdatePreview(dialog, state);
        }
        return TRUE;
    case IDC_P_NO_PICTURE:
        if (HIWORD(wp) == BN_CLICKED && state->picture) {
            HeapFree(GetProcessHeap(), 0, state->picture);
            state->picture = NULL;
            state->pictureChanged = TRUE;
            ShowPicture(state);
            UpdatePreview(dialog, state);
            SetFocus(GetDlgItem(dialog, IDC_P_PICTURE));
        }
        return TRUE;
    case IDC_P_COLOR:
        if (HIWORD(wp) == CBN_SELCHANGE) UpdatePreview(dialog, state);
        return TRUE;
    case IDC_P_SYNC:
        if (HIWORD(wp) == CBN_SELCHANGE) ShowSyncItems(state);
        return TRUE;
    case IDC_P_SYNC_ITEMS:
        if (HIWORD(wp) == BN_CLICKED) ChooseDialogItems(dialog, state);
        return TRUE;
    case IDC_P_SIGN_IN:
        if (HIWORD(wp) == BN_CLICKED && TakeProfileChoices(dialog, state)) {
            state->signIn = TRUE;
            EndDialog(dialog, IDOK);
        }
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK ProfileProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ProfileDialog *state = (ProfileDialog *)GetWindowLongPtrW(dialog, DWLP_USER);

    switch (message) {
    case WM_INITDIALOG:
        state = (ProfileDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        InitProfileDialog(dialog, state);
        return FALSE;

    case WM_NOTIFY:
    case WM_CHECKLIST_BOX:
    case WM_CONTEXTMENU: {
        BOOL toggled;
        if (state && state->values && CheckList_Message(&state->list, dialog, message, wp, lp, &toggled)) return TRUE;
        break;
    }

    case WM_CHECKLIST_VALUE:
        if (state && state->values) ProfileValueChanged(dialog, state, (int)wp);
        return TRUE;

    case WM_APP_PROFILE_ACTION:
        if (state && state->values && (int)wp == state->at[PV_PICTURE]) PictureAction(dialog, state);
        else if (state && state->values && (int)wp == state->at[PV_SYNC_ITEMS]) ChooseDialogItems(dialog, state);
        return TRUE;

    case WM_COMMAND:
        if (!state) break;
        if (state->form && ProfileFormCommand(dialog, state, wp)) return TRUE;
        if (LOWORD(wp) == IDOK) {
            if (TakeProfileChoices(dialog, state)) EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
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
        if (state && state->form && state->copyFrom) SetCopyLabel(GetDlgItem(dialog, IDC_P_COPY), state->copyFrom->name);
        if (state) UpdatePreview(dialog, state);
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

CheckList *Gui_FillProfileList(HWND list)
{
    static ProfileDialog shown;
    static Profile sample;
    static const char layout[] = "{\"autoArchive\":true,\"coworkWebSearch\":false,\"coworkBrowser\":\"chrome\",\"coworkSchedules\":true,"
                                 "\"zoom\":1.1,\"locale\":\"zh-CN\",\"defaultModel\":\"claude-opus\",\"starred\":[\"a\",\"b\"]}";
    FreeProfileValues(&shown);
    ZeroMemory(&shown, sizeof shown);
    ZeroMemory(&sample, sizeof sample);
    StringCchCopyW(sample.name, ARRAYSIZE(sample.name), L"Work");
    StringCchCopyW(sample.folder, ARRAYSIZE(sample.folder), L"Claude-Work");
    StringCchCopyW(sample.note, ARRAYSIZE(sample.note), L"Client projects");
    StringCchCopyW(sample.badge, ARRAYSIZE(sample.badge), L"W");
    sample.color = 2;
    shown.existing = &sample;
    shown.color = sample.color;
    shown.badgeOwn = TRUE;
    shown.startup = TRUE;
    shown.layout = layout;
    shown.layoutLength = sizeof layout - 1;
    SetupProfileList(list, &shown);
    return &shown.list;
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
    LVITEMW item;
    int i;

    switch (message) {
    case WM_INITDIALOG:
        state = (UninstallDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetKeepText(dialog, state->list);
        list = state->rows = GetDlgItem(dialog, IDC_U_LIST);
        Theme_CheckRows(list);
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

typedef struct JobProfileResult {
    BOOL quitTried, openTried, alreadyClosed, alreadyRunning;
    DWORD quitError;
    HRESULT openError;
} JobProfileResult;

typedef struct Job {
    HWND          dialog;
    JobKind       kind;
    ClaudePackage pkg;
    ProfileList   profiles;
    DWORD         chosen;      /* the profiles it is for */
    DWORD         notQuit;     /* ... whose Claude could not be closed */
    JobProfileResult results[MAX_PROFILES];
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
            JobProfileResult *result = &job->results[i];
            DWORD error = ERROR_SUCCESS;
            if (!(job->chosen & (1u << i))) continue;
            if (!Claude_IsRunning(p)) {
                result->alreadyClosed = TRUE;
                if (job->kind == JOB_QUIT) synced &= ~(1u << i);
                continue;
            }
            result->quitTried = TRUE;
            Taskbar_QuitComing(p);
            if (!Claude_Quit(p, &error)) {
                job->notQuit |= 1u << i;
                result->quitError = error ? error : ERROR_GEN_FAILURE;
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
            JobProfileResult *result = &job->results[i];
            HRESULT hr;
            if (!(job->chosen & (1u << i)) || (job->notQuit & (1u << i))) continue;
            result->alreadyRunning = Claude_IsRunning(&job->profiles.items[i]);
            result->openTried = TRUE;
            hr = Launcher_OpenSynced(&job->pkg, &job->profiles.items[i], NULL, &identity);
            result->openError = hr;
            if (SUCCEEDED(hr)) {
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
        ShowInFoot(IDC_STATUS, FALSE);
        ShowInFoot(IDC_PROGRESS, TRUE);
    }
    InvalidateRect(bar, NULL, FALSE);
}

static void HideProgress(void)
{
    if (!g_manager.progressShown) return;
    g_manager.progressShown = FALSE;
    ShowInFoot(IDC_PROGRESS, FALSE);
    ShowInFoot(IDC_STATUS, TRUE);
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

static void FormatJobResult(const Job *job, int index, WCHAR *text, size_t cch)
{
    const JobProfileResult *result = &job->results[index];
    WCHAR line[256];
    text[0] = 0;
    if (job->kind == JOB_QUIT || job->kind == JOB_RESTART) {
        if (result->alreadyClosed) StringCchCopyW(line, ARRAYSIZE(line), TR(L"Quit: already closed"));
        else if (result->quitError == ERROR_CANCELLED) StringCchCopyW(line, ARRAYSIZE(line), TR(L"Quit: cancelled"));
        else if (result->quitError)
            StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Quit: failed (error %lu, 0x%08lX)"), result->quitError,
                             (unsigned long)HRESULT_FROM_WIN32(result->quitError));
        else if (result->quitTried) StringCchCopyW(line, ARRAYSIZE(line), TR(L"Quit: completed"));
        else StringCchCopyW(line, ARRAYSIZE(line), TR(L"Quit: not attempted"));
        StringCchCatW(text, cch, line);
    }
    if (job->kind == JOB_OPEN || job->kind == JOB_RESTART) {
        if (text[0]) StringCchCatW(text, cch, L"\r\n");
        if (!result->openTried) StringCchCopyW(line, ARRAYSIZE(line), TR(L"Open: skipped because quitting failed"));
        else if (result->openError == HRESULT_FROM_WIN32(ERROR_CANCELLED)) StringCchCopyW(line, ARRAYSIZE(line), TR(L"Open: cancelled"));
        else if (FAILED(result->openError))
            StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Open: failed (error 0x%08lX)"), (unsigned long)result->openError);
        else StringCchCopyW(line, ARRAYSIZE(line), result->alreadyRunning ? TR(L"Open: already running, brought forward") : TR(L"Open: completed"));
        StringCchCatW(text, cch, line);
    }
}

static void ShowJobFailure(const Job *job)
{
    WCHAR outcome[512];
    int i;
    for (i = 0; i < job->profiles.count; i++) {
        const JobProfileResult *result = &job->results[i];
        if (!(job->chosen & (1u << i))) continue;
        if ((!result->quitError || result->quitError == ERROR_CANCELLED) &&
            (!result->openTried || SUCCEEDED(result->openError) || result->openError == HRESULT_FROM_WIN32(ERROR_CANCELLED))) continue;
        FormatJobResult(job, i, outcome, ARRAYSIZE(outcome));
        Ui_Message(g_manager.dlg, MB_ICONWARNING, L"%s\n%s", job->profiles.items[i].name, outcome);
        break;
    }
}

/* What a job left undone is said; a sync asked for says what it did. */
static void JobDone(Job *job)
{
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
        if (job->kind == JOB_OPEN || job->kind == JOB_QUIT || job->kind == JOB_RESTART) ShowJobFailure(job);
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

/* The profiles of `bits` the shield lets open: one whose own regions leave out Claude's exit stays closed, said. */
static DWORD ShieldAllowed(DWORD bits)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], exit[8];
    DWORD blocked = Shield_ProfilesBlocked(&g_manager.profiles, bits, exit, ARRAYSIZE(exit));
    if (!blocked) return bits;
    Profiles_JoinNames(&g_manager.profiles, blocked, names, ARRAYSIZE(names));
    Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(L"%s not opened: the shield's regions for it leave out Claude's exit, now in %s."), names,
               exit);
    return bits & ~blocked;
}

/* Every profile selected opens, its sessions made the same first. */
static void DoOpen(void)
{
    DWORD selected = ShieldAllowed(SelectedProfiles());
    if (selected) RunJob(JOB_OPEN, selected);
}

/* Quit, or Restart: the Claude of every profile selected that runs, as its
 * own Quit does, once the user agreed (its windows close, and what runs in
 * it stops); then their sessions are made the same, and with Restart they
 * open again. Restart opens the ones closed too. */
static void DoQuit(BOOL restart)
{
    WCHAR text[512 + MAX_PROFILES * (LABEL_CCH + 8)], names[MAX_PROFILES * (LABEL_CCH + 8)];
    DWORD selected = restart ? ShieldAllowed(SelectedProfiles()) : SelectedProfiles(), running = RunningOf(selected);
    if (!selected) return;
    if (!running) {
        RunJob(restart ? JOB_RESTART : JOB_QUIT, selected);
        return;
    }
    Profiles_JoinNames(&g_manager.profiles, running, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text),
                     restart ? TR(L"Restart Claude for %s?\n\nIts windows and running sessions close, then it opens again.")
                             : TR(L"Quit Claude for %s?\n\nIts windows and running sessions close."),
                     names);
    if (!Ui_Ask(g_manager.dlg, IDI_QUESTION, text, restart ? TR(L"Restart") : TR(L"Quit"), TR(L"Cancel"), FALSE)) return;
    RunJob(restart ? JOB_RESTART : JOB_QUIT, selected);
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

/* The name a new profile starts with: `base` itself when it is free (`asIs`), else it and the first number free
 * ("Work 2"); `base` when none is: the form says why. */
static void FreeName(const WCHAR *base, BOOL asIs, WCHAR *name, size_t cch)
{
    WCHAR clean[LABEL_CCH], folder[FOLDER_CCH], appData[MAX_PATH], dir[MAX_PATH];
    const WCHAR *error;
    int n;
    for (n = asIs ? 1 : 2; n < 100; n++) {
        if (n == 1) StringCchCopyW(name, cch, base);
        else StringCchPrintfW(name, cch, L"%s %d", base, n);
        if (Core_ValidateNewName(name, clean, ARRAYSIZE(clean), folder, ARRAYSIZE(folder), &error) && Util_AppData(appData, ARRAYSIZE(appData)) &&
            SUCCEEDED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", appData, folder)) && GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES)
            return;
    }
    StringCchCopyW(name, cch, base);
}

static void SearXngAddress(WCHAR *url, size_t cch, BOOL *allowPrivate);

/* A new profile made as Settings says, before its Claude first starts: Windows MCP and SearXNG search set up (when
 * uv and Node.js are there; logged when not), Cowork off. */
static void SetUpNewProfile(const Profile *p)
{
    WCHAR tool[MAX_PATH], url[URL_CCH];
    BOOL allowPrivate = FALSE;
    if (Util_GetSetting(SETTING_NEW_WINDOWS_MCP, 1) && WindowsMcp_State(p) == WINDOWS_MCP_NONE) {
        if (WindowsMcp_FindUvx(tool, ARRAYSIZE(tool))) WindowsMcp_Set(p, tool);
        else Util_Log(L"windows-mcp: not set up in the new profile %s: uv is not installed", p->folder);
    }
    if (Util_GetSetting(SETTING_NEW_SEARXNG, 1) && !SearXng_Get(p, url, ARRAYSIZE(url), NULL)) {
        SearXngAddress(url, ARRAYSIZE(url), &allowPrivate);
        if (SearXng_FindNpx(tool, ARRAYSIZE(tool))) SearXng_Set(p, tool, url, allowPrivate);
        else Util_Log(L"searxng: not set up in the new profile %s: Node.js is not installed", p->folder);
    }
    if (Util_GetSetting(SETTING_NEW_COWORK_OFF, 1)) Cowork_Set(p, FALSE);
}

static void ImportInto(int target, const WCHAR *const *files, int count, BOOL made);

/* New…, or Copy profile (`like`, dragged on its tile): a new profile with
 * its picture, badge and notes, its settings copied, a name and color of
 * its own, in the same form. With backups (`files`, dropped where no profile
 * is): the form starts with the name, color, badge, note and windows' title
 * of the first that has them, and the new profile is restored from them
 * before anything else (Backup_RestoreNew). */
static void NewProfile(const Profile *like, const WCHAR *const *files, int count)
{
    ProfileDialog dialog;
    Profile source, created;
    ProfileOwn own;
    WCHAR folder[FOLDER_CCH], error[256];
    INT_PTR result;
    int i;
    if (g_manager.profiles.count >= MAX_PROFILES) {
        Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(APP_NAME L" handles up to %d profiles."), MAX_PROFILES);
        return;
    }
    ZeroMemory(&dialog, sizeof dialog);
    ZeroMemory(&own, sizeof own);
    dialog.form = TRUE;
    dialog.color = FreeColor();
    dialog.openNow = g_manager.pkg.found;
    /* Settings come from the profile copied, else the selected profile, else the default one. */
    ZeroMemory(&source, sizeof source);
    if (like) source = *like;
    else if (SelectedProfile()) source = *SelectedProfile();
    else if ((i = Profiles_DefaultIndex(&g_manager.profiles)) >= 0) source = g_manager.profiles.items[i];
    dialog.copyFrom = source.folder[0] && !count ? &source : NULL;   /* a backup's settings are restored, not copied */
    for (i = 0; i < count && !Backup_ReadOwn(files[i], &own); i++) {}
    if (i < count) {
        if (own.held & PROFILE_OWN_NAME) FreeName(own.name, TRUE, dialog.name, ARRAYSIZE(dialog.name));
        if ((own.held & PROFILE_OWN_COLOR) && own.color >= 0 && own.color < PALETTE_SIZE) dialog.color = own.color;
        if (own.held & PROFILE_OWN_NOTE) StringCchCopyW(dialog.note, ARRAYSIZE(dialog.note), own.note);
        if (own.held & PROFILE_OWN_TITLE) StringCchCopyW(dialog.windowTitle, ARRAYSIZE(dialog.windowTitle), own.title);
        if (own.held & PROFILE_OWN_BADGE) {
            StringCchCopyW(dialog.badge, ARRAYSIZE(dialog.badge), own.badge);
            dialog.badgeOwn = own.badge[0] != 0;
        }
    }
    if (like) {
        FreeName(like->name, FALSE, dialog.name, ARRAYSIZE(dialog.name));
        StringCchCopyW(dialog.note, ARRAYSIZE(dialog.note), like->note);
        StringCchCopyW(dialog.badge, ARRAYSIZE(dialog.badge), like->badge);
        dialog.badgeOwn = like->badge[0] != 0;
        dialog.copy = TRUE;
        if (like->picture && (dialog.picture = Icons_LoadPicture(like)) != NULL) dialog.pictureChanged = TRUE;
    }
    result = Ui_Dialog(g_manager.dlg, IDD_PROFILE_NEW, ProfileProc, (LPARAM)&dialog);
    FreeProfileValues(&dialog);
    Util_Free(dialog.settings);   /* a new profile has none yet */
    if (result != IDOK || StateChangesBlocked()) {
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
    if (dialog.windowTitle[0] && !Profiles_SetWindowTitle(folder, dialog.windowTitle))
        Util_Log(L"could not save the window title of %s (error %lu)", folder, GetLastError());
    if (dialog.note[0] && !Profiles_SetNote(folder, dialog.note))
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The profile notes could not be saved."));
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
    if (count) {
        /* What the backups hold first: Settings' choices for a new profile then fill in what they lack. */
        ImportInto(i, files, count, TRUE);
        if (StateChangesBlocked() || (i = Profiles_Find(&g_manager.profiles, folder)) < 0) return;
        created = g_manager.profiles.items[i];
    }
    SetUpNewProfile(&created);
    UpdateRowsAndButtons();   /* its marks */
    if (dialog.startup) SetProfileStartup(&created, TRUE);
    if (dialog.signIn) SignIn_Open(g_manager.dlg, &created);   /* its private sign-in instead of opening it */
    else if (dialog.openNow && !StateChangesBlocked()) RunJob(JOB_OPEN, 1u << i);
}

static void DoNew(const Profile *like)
{
    NewProfile(like, NULL, 0);
}

/* Profiles that will keep the same sessions (`together`), asked first. */
static BOOL ConfirmKeepSame(DWORD together)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 16)], question[256 + ARRAYSIZE(names)];
    Profiles_JoinNames(&g_manager.profiles, together, names, ARRAYSIZE(names));
    StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Sync the sessions of %s?\n\nFrom now on, what changes in one goes to the others."), names);
    return SyncUi_ConfirmSessions(g_manager.dlg, question, TR(L"Sync"));
}

/* Sync now: the sessions that change are those of the groups of the
 * profiles selected; asked first. Profiles in no group
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
    Profiles_JoinNames(&g_manager.profiles, changed, names, ARRAYSIZE(names));
    StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Make the sessions of %s the same now?"), names);
    return SyncUi_ConfirmSessions(g_manager.dlg, question, TR(L"Sync"));
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
    INT_PTR result;
    int i, count = 0;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.form = TRUE;
    dialog.several = selected;
    result = Ui_Dialog(g_manager.dlg, IDD_PROFILE_NEW, ProfileProc, (LPARAM)&dialog);
    FreeProfileValues(&dialog);
    Util_Free(dialog.settings);
    if (result != IDOK || StateChangesBlocked()) return;
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
    INT_PTR result;
    if (several & (several - 1)) {
        DoEditSeveral(several);
        return;
    }
    if (!selected) return;
    before = *selected;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.form = TRUE;
    dialog.existing = &before;
    dialog.color = before.color;
    dialog.startup = atStartup = Shortcut_IsAtStartup(&before);
    result = Ui_Dialog(g_manager.dlg, IDD_PROFILE_NEW, ProfileProc, (LPARAM)&dialog);
    FreeProfileValues(&dialog);
    if (result != IDOK || StateChangesBlocked()) {
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
    if (wcscmp(before.windowTitle, dialog.windowTitle) != 0 && !Profiles_SetWindowTitle(before.folder, dialog.windowTitle))
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The window title could not be saved."));
    if (wcscmp(before.note, dialog.note) != 0 && !Profiles_SetNote(before.folder, dialog.note))
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The profile notes could not be saved."));
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
    /* The windows of a running profile take a new title at once (its watcher). */
    if (wcscmp(before.windowTitle, dialog.windowTitle) != 0 || wcscmp(before.name, after.name) != 0) Taskbar_Refresh(&after, FALSE);
    if (dialog.startup != atStartup) SetProfileStartup(&after, dialog.startup);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
    StringCchCopyW(folder[0], FOLDER_CCH, before.folder);
    ApplySessionsChoice(folder, 1, dialog.sync, dialog.syncFolder, dialog.syncItems);
}

static BOOL ConfirmDelete(const Profile *p)
{
    WCHAR text[512 + LABEL_CCH], target[MAX_PATH];
    StringCchPrintfW(text, ARRAYSIZE(text),
                     Profiles_LinkTarget(p, target, ARRAYSIZE(target))
                     ? TR(L"Delete the profile \x201C%s\x201D?\n\nIts folder is a link: the data it links to stays on disk.")
                     : TR(L"Delete the profile \x201C%s\x201D?\n\nIts data folder, with its sign-in and history, goes to the Recycle Bin."),
                     p->name);
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
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], text[ARRAYSIZE(names) + 256];
    int i, p;
    ZeroMemory(&chosen, sizeof chosen);
    for (i = 0; i < g_manager.profiles.count; i++) {
        if (!(bits & (1u << i))) continue;
        if (RefuseIfRunning(&g_manager.profiles.items[i])) return;
        StringCchCopyW(chosen.folders[chosen.count++], ARRAYSIZE(chosen.folders[0]), g_manager.profiles.items[i].folder);
    }
    Profiles_JoinNames(&g_manager.profiles, bits, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Delete the profiles %s?\n\nTheir data folders go to the Recycle Bin."), names);
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
    ProfileList *copy = (ProfileList *)Util_Alloc(sizeof *copy);
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
    Util_Free(profiles);
    if (changed && !StateChangesBlocked()) SessionsView_Reload();
}

/* Settings' Claude Code buttons, its dialog `owner`: Claude Code's sessions
 * no list names cleaned up, or its folder copied beside itself. */
static void DoSettingsAction(HWND owner, int action)
{
    ProfileList *profiles;
    if (action == IDC_G_ZOOM) {
        g_manager.openAgain = TRUE;
        return;
    }
    if (StateChangesBlocked()) return;
    if (action == IDC_G_CODE_COPY) {
        SyncUi_BackUpCode(owner);
        return;
    }
    if ((profiles = CopyProfiles()) == NULL) return;
    if (SyncUi_Purge(owner, profiles) && !StateChangesBlocked()) SessionsView_Reload();
    Util_Free(profiles);
}

/* The profiles of `selected` whose Claude has its own Windows-MCP extension installed. */
static DWORD WithExtension(DWORD selected)
{
    DWORD with = 0;
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && WindowsMcp_HasExtension(&g_manager.profiles.items[i])) with |= 1u << i;
    return with;
}

static void DoRemoveExtension(void);
static void DoSearXng(void);
static void DoSearXngButton(void);

#define IDM_LIST_RECOVER    0x6005
#define IDM_LIST_UNLINK     0x6008
#define IDM_LIST_BACKUP     0x6009
#define IDM_LIST_RESTORE    0x600A
#define IDM_LIST_NO_EXTENSION 0x600B   /* Claude's own Windows-MCP extension uninstalled */
#define IDM_LIST_SEARXNG    0x600C   /* SearXNG search set up, changed or taken away */

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
    WCHAR text[1024], error[512 + 2 * LONG_PATH_CCH], errors[4 * (512 + 2 * LONG_PATH_CCH)];
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
                         TR(L"Give \x201C%s\x201D a session folder of its own again?\n\nIt keeps the sessions it lists now."),
                         first->name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Give the %d linked profiles selected session folders of their own again?\n\nEach keeps the sessions it lists now."),
                         count);
    if (!SyncUi_Confirm(g_manager.dlg, NULL, text, FALSE, TR(L"Unlink"))) return;
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

/* The profiles of `involved` sync with profile `other` (an index) and its
 * group from now on (sessionvault.c), asked first. A session folder of
 * theirs still linked (Claude no longer writes through a link) becomes
 * their own again first, which needs them closed. Returns the group, 0 when
 * not done. */
static int JoinGroupOf(DWORD involved, int other)
{
    WCHAR error[512 + 2 * LONG_PATH_CCH];
    DWORD linked, together;
    int i;
    if (StateChangesBlocked() || other < 0 || other >= g_manager.profiles.count) return 0;
    involved &= ~(1u << other);
    if (!involved) return 0;
    together = involved | (1u << other) | SessionVault_Group(&g_manager.profiles, g_manager.profiles.items[other].syncGroup);
    if (!ConfirmKeepSame(together)) return 0;
    linked = LinkedProfiles(together);
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((linked & (1u << i)) && LinkBusy(&g_manager.profiles.items[i])) return 0;
    FinishShellWork();
    SessionsView_PauseWatching();
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((linked & (1u << i)) && !SessionLink_Remove(&g_manager.profiles, i, error, ARRAYSIZE(error))) {
            SessionsView_ResumeWatching();
            Ui_Message(g_manager.dlg, MB_ICONWARNING, L"%s", error);
            Refresh(FALSE);
            return 0;
        }
    SessionsView_ResumeWatching();
    return SetSessionsChoice(involved, -1 - other);
}

/* Sync settings: the profiles the one with the focus syncs with, and what
 * they sync. A profile checked leaves the group it was in for this one, one
 * unchecked leaves this one; a profile joining, or Sync now, syncs them at
 * once. */
static void DoSyncSetup(void)
{
    WCHAR folder[FOLDER_CCH];
    SyncSetup setup;
    const Profile *p = FocusedProfile();
    DWORD before, joining, leaving, members;
    int self, group;
    if (StateChangesBlocked() || !p) return;
    StringCchCopyW(folder, ARRAYSIZE(folder), p->folder);
    ZeroMemory(&setup, sizeof setup);
    setup.profile = (int)(p - g_manager.profiles.items);
    if (!SyncUi_Setup(g_manager.dlg, &g_manager.profiles, &setup) || StateChangesBlocked()) return;
    /* The list read again meanwhile: the choice was made on another one. */
    if ((self = Profiles_Find(&g_manager.profiles, folder)) != setup.profile) return;
    p = &g_manager.profiles.items[self];
    before = p->syncGroup ? SessionVault_Group(&g_manager.profiles, p->syncGroup) & ~(1u << self) : 0;
    leaving = before & ~setup.partners;
    joining = setup.partners & ~before;
    if (joining && !JoinGroupOf(joining, self)) return;
    if (leaving) SetSessionsChoice(leaving, 0);
    if ((self = Profiles_Find(&g_manager.profiles, folder)) < 0 || (group = g_manager.profiles.items[self].syncGroup) == 0) return;
    members = SessionVault_Group(&g_manager.profiles, group);
    if (SessionVault_GroupItems(&g_manager.profiles, members) != setup.items) SetGroupItems(members, setup.items);
    if (joining || (setup.syncNow && ConfirmSync(members))) RunJob(JOB_SYNC, members);
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
                         TR(L"Claude no longer saves the sessions of \x201C%s\x201D: its session folder is a link to that of \x201C%s\x201D.\n\n"
                            L"Give it its own folder, synced with \x201C%s\x201D?"),
                         g_manager.profiles.items[i].name, g_manager.profiles.items[state.profile].name, g_manager.profiles.items[state.profile].name);
        if (!Ui_Ask(g_manager.dlg, IDI_WARNING, text, TR(L"Keep the same"), TR(L"Not now"), FALSE)) continue;
        if (LinkBusy(&g_manager.profiles.items[i]) || LinkBusy(&g_manager.profiles.items[state.profile])) continue;
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

/* -------------------------------------------------------------- the menus */

/* The Shortcuts menu: the state shown is of a profile selected alone; with
 * several, each command does what is left to do for each. */
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

/* An import into profile `target`: the backups or session archives `files`
 * (NULL: chosen), as Import and a drop on the profile do. What it gave the
 * profile of its own (name, icon, windows' title) reaches its shortcuts,
 * pins and windows. */
static void ImportInto(int target, const WCHAR *const *files, int count, BOOL made)
{
    ProfileList *before = CopyProfiles();
    int i;
    if (StateChangesBlocked() || target < 0 || target >= g_manager.profiles.count) {
        Util_Free(before);
        return;
    }
    FinishShellWork();
    SessionsView_PauseWatching();
    if (made) Backup_RestoreNew(g_manager.dlg, &g_manager.profiles, target, files, count);
    else Backup_RestoreFiles(g_manager.dlg, &g_manager.pkg, &g_manager.profiles, target, files, count);
    SessionsView_ResumeWatching();
    Refresh(FALSE);
    for (i = 0; before && i < before->count; i++) {
        const Profile *old = &before->items[i];
        int index = Profiles_Find(&g_manager.profiles, old->folder);
        if (index >= 0) {
            const Profile *after = &g_manager.profiles.items[index];
            WCHAR icon[MAX_PATH];
            BOOL renamed = wcscmp(old->name, after->name) != 0;
            BOOL appearance = renamed || old->color != after->color || wcscmp(old->badge, after->badge) != 0 || old->picture != after->picture;
            if (appearance && Icons_Ensure(&g_manager.pkg, after, icon, ARRAYSIZE(icon))) ApplyBadge(old, after, icon);
            if (renamed || wcscmp(old->windowTitle, after->windowTitle) != 0) Taskbar_Refresh(after, FALSE);
        }
    }
    Util_Free(before);
    g_manager.shortcutStateFolder[0] = 0;
    UpdateButtons();
    if (!StateChangesBlocked()) SessionsView_Reload();
}

/* A menu's own commands: TRUE when `cmd` is one; the others are the buttons they stand for. */
static BOOL MenuOwnCommand(UINT cmd)
{
    switch (cmd) {
    case IDM_LIST_RECOVER: DoSessions(SESSIONS_RESTORE); return TRUE;
    case IDM_LIST_UNLINK: DoUnlink(); return TRUE;
    case IDM_LIST_NO_EXTENSION: DoRemoveExtension(); return TRUE;
    case IDM_LIST_SEARXNG:      DoSearXng(); return TRUE;
    case IDM_LIST_BACKUP:
        if (SelectedProfiles()) Backup_Create(g_manager.dlg, &g_manager.pkg, &g_manager.profiles, SelectedProfiles());
        return TRUE;
    case IDM_LIST_RESTORE:
        if (SelectedProfile()) ImportInto((int)(SelectedProfile() - g_manager.profiles.items), NULL, 0, FALSE);
        return TRUE;
    }
    return FALSE;
}

/* What a menu chose: its own commands here, the others as the buttons they stand for. */
static void MenuCommand(UINT cmd)
{
    if (!cmd || StateChangesBlocked()) return;
    if (!MenuOwnCommand(cmd)) SendMessageW(g_manager.dlg, WM_COMMAND, MAKEWPARAM(cmd, BN_CLICKED), 0);
}

/* The column's button `id` opens `menu` below it, drawn by the program (Theme_TrackMenu). */
static UINT ColumnMenu(int id, HMENU menu)
{
    HWND button = GetDlgItem(g_manager.dlg, id);
    RECT box;
    if (!button || !GetWindowRect(button, &box)) return 0;
    return Theme_TrackMenu(button, menu, &box);
}

static void ShortcutsMenu(void)
{
    HMENU menu = CreatePopupMenu();
    UINT cmd;
    if (!menu) return;
    UpdateButtons();   /* the shortcuts' state of the profile selected */
    FillShortcutsMenu(menu, SelectedProfiles());
    cmd = ColumnMenu(IDC_SHORTCUTS, menu);
    DestroyMenu(menu);
    MenuCommand(cmd);
}

/* The profile whose row is under `at` (the window's client), -1 for none. */
static int ProfileAtPoint(HWND dialog, POINT at)
{
    LVHITTESTINFO hit;
    ZeroMemory(&hit, sizeof hit);
    hit.pt = at;
    MapWindowPoints(dialog, g_manager.list, &hit.pt, 1);
    if (SessionsView_Shown() || ListView_HitTest(g_manager.list, &hit) < 0) return -1;
    return ProfileIndexAtRow(hit.iItem);
}

/* Files dropped on the window (DragOut_AcceptFiles): imported into the
 * profile dropped on, as Import does (Backup_RestoreFiles); dropped where no
 * profile is, into a new one, named in the new profile's form. */
static void DropFiles(HWND dialog, DroppedFiles *dropped)
{
    const WCHAR *files[DROPPED_FILES_MAX];
    POINT at = dropped->at;
    int target, i;
    for (i = 0; i < dropped->count && i < DROPPED_FILES_MAX; i++) files[i] = dropped->paths[i];
    ScreenToClient(dialog, &at);
    target = ProfileAtPoint(dialog, at);
    if (!dropped->count || StateChangesBlocked() || g_manager.job) {
        Util_Free(dropped);
        return;
    }
    SetForegroundWindow(dialog);
    if (target < 0) {
        /* Where no profile is, a session archive goes through the import, into the profiles chosen there: a new
         * profile keeps no sessions before it signs in. Backups make a new profile. */
        const WCHAR *backups[DROPPED_FILES_MAX];
        ProfileList *profiles;
        BOOL changed = FALSE;
        int n = 0, a;
        for (a = 0; a < i; a++) {
            if (!SessionSync_IsArchive(files[a])) backups[n++] = files[a];
            else if ((profiles = CopyProfiles()) != NULL) {
                if (SyncUi_ImportFile(g_manager.dlg, profiles, SelectedProfiles(), files[a])) changed = TRUE;
                Util_Free(profiles);
            }
        }
        if (changed && !StateChangesBlocked()) SessionsView_Reload();
        if (n) NewProfile(NULL, backups, n);
    } else {
        SelectProfileRow(g_manager.profiles.items[target].folder);
        ImportInto(target, files, i, FALSE);
    }
    Util_Free(dropped);
}

/* The list's menu, at the mouse or (from the keyboard) under the row with
 * the focus, drawn by the program: what the toolbar and the column do to
 * the profiles selected (through their buttons, by their id), their
 * sessions moved or merged, and the profiles backed up. */
static void ListMenu(LPARAM pos)
{
    HMENU menu;
    const Profile *one;
    DWORD selected = SelectedProfiles();
    BOOL isDefault, idle = g_manager.job == NULL, several = g_manager.profiles.count >= 2;
    RECT at;
    UINT cmd;
    if (pos == (LPARAM)-1) {
        RECT rc;
        POINT pt;
        int focused = ListView_GetNextItem(g_manager.list, -1, LVNI_FOCUSED);
        if (focused < 0 || !ListView_GetItemRect(g_manager.list, focused, &rc, LVIR_LABEL)) GetClientRect(g_manager.list, &rc);
        pt.x = rc.left;
        pt.y = rc.bottom;
        ClientToScreen(g_manager.list, &pt);
        SetRect(&at, pt.x, pt.y, pt.x, pt.y);
    } else {
        SetRect(&at, (short)LOWORD(pos), (short)HIWORD(pos), (short)LOWORD(pos), (short)HIWORD(pos));
    }
    /* The row just clicked is selected, its WM_APP_SELECTION maybe not handled yet: the buttons follow it now. */
    UpdateButtons();
    one = SelectedProfile();
    isDefault = one && Core_EqualsI(one->folder, g_manager.profiles.defaultFolder);
    if ((menu = CreatePopupMenu()) == NULL) return;
    Theme_AppendMenu(menu, MF_STRING | (selected && g_manager.pkg.found && idle ? 0 : MF_GRAYED), IDC_OPEN, TR(Theme_MainCaption(IDC_OPEN, 0)), 0xE768);
    Theme_AppendMenu(menu, MF_STRING | (RunningOf(selected) && idle ? 0 : MF_GRAYED), IDC_STOP, TR(Theme_MainCaption(IDC_STOP, 0)), 0xE71A);
    Theme_AppendMenu(menu, MF_STRING | (selected && g_manager.pkg.found && idle ? 0 : MF_GRAYED), IDC_RESTART, TR(Theme_MainCaption(IDC_RESTART, 0)), 0xE72C);
    Theme_AppendMenu(menu, MF_STRING | (one && idle ? 0 : MF_GRAYED), IDC_PRIVATE_SIGNIN, TR(L"Private sign-in\x2026"), 0xE727);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (!SessionsView_Shown()) {   /* the profile's sessions (V); the sessions view shows them already */
        Theme_AppendMenu(menu, MF_STRING | (one ? 0 : MF_GRAYED), IDC_SESSIONS, TR(Theme_MainCaption(IDC_SESSIONS, 0)), 0xE8BD);
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    }
    Theme_AppendMenu(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDC_EDIT, TR(Theme_MainCaption(IDC_EDIT, 0)), 0xE70F);
    Theme_AppendMenu(menu, MF_STRING | (Deletable(selected) ? 0 : MF_GRAYED), IDC_DELETE, TR(Theme_MainCaption(IDC_DELETE, 0)), 0xE74D);
    Theme_SetMenuDanger(menu, IDC_DELETE);
    Theme_AppendMenu(menu, MF_STRING | (one && !isDefault ? 0 : MF_GRAYED), IDC_DEFAULT, TR(Theme_MainCaption(IDC_DEFAULT, 0)), 0xE735);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    Theme_AppendMenu(menu, MF_STRING | (selected && idle ? 0 : MF_GRAYED), IDC_SYNC_NOW, TR(L"&Sync now"), 0xE895);
    Theme_AppendMenu(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDC_SYNC, TR(Theme_MainCaption(IDC_SYNC, 0)), 0xE713);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    Theme_AppendMenu(menu, MF_STRING | (several ? 0 : MF_GRAYED), IDC_MERGE, TR(L"Merge &all sessions\x2026"), 0xE8AB);
    Theme_AppendMenu(menu, MF_STRING | (selected && several ? 0 : MF_GRAYED), IDC_COPY_ALL, TR(L"&Copy all sessions to\x2026"), 0xE8C8);
    Theme_AppendMenu(menu, MF_STRING | (selected && several ? 0 : MF_GRAYED), IDC_MOVE_ALL, TR(L"&Move all sessions to\x2026"), 0xE8DE);
    if (LinkedProfiles(selected)) Theme_AppendMenu(menu, MF_STRING, IDM_LIST_UNLINK, TR(L"Un&link sessions folder\x2026"), 0xE71B);
    if (WithExtension(selected))   /* shown only where it is installed, as the unlink is */
        Theme_AppendMenu(menu, MF_STRING | (idle ? 0 : MF_GRAYED), IDM_LIST_NO_EXTENSION, TR(L"Uninstall the Windows-MCP e&xtension\x2026"), 0xE74D);
    Theme_AppendMenu(menu, MF_STRING | (selected && idle ? 0 : MF_GRAYED), IDM_LIST_SEARXNG, TR(L"SearXN&G search\x2026"), 0xE721);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    Theme_AppendMenu(menu, MF_STRING | (selected ? 0 : MF_GRAYED), IDM_LIST_BACKUP, TR(L"E&xport\x2026"), 0xE898);
    Theme_AppendMenu(menu, MF_STRING | (one ? 0 : MF_GRAYED), IDM_LIST_RESTORE, TR(L"&Import\x2026"), 0xE896);
    Theme_AppendMenu(menu, MF_STRING | (g_manager.profiles.count ? 0 : MF_GRAYED), IDM_LIST_RECOVER, TR(L"Recover sessio&ns\x2026"), 0xE81C);
    cmd = Theme_TrackMenu(g_manager.list, menu, &at);
    DestroyMenu(menu);
    MenuCommand(cmd);
}

/* The profiles and the sessions share the window: Sessions view swaps them,
 * and becomes "< Back" in the same place. The toolbar stays: in the
 * sessions view it acts on the profile shown. */
static const int kProfileControls[] = { IDC_PROFILE_FIND, IDC_LIST, IDC_SYNC, IDC_BACKUP, IDC_RESTORE_BACKUP, IDC_REPAIR, IDC_WINDOWS_MCP, IDC_SEARXNG, IDC_COWORK,
                                        IDC_NOTE };

static void ShowProfileSearch(HWND dialog, BOOL show)
{
    g_manager.profileSearchShown = show;
    if (!show) {
        FocusView(dialog, FALSE);
        SetDlgItemTextW(dialog, IDC_PROFILE_SEARCH, L"");
    }
    ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH_LABEL), show ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH), show ? SW_SHOW : SW_HIDE);
    Theme_LayoutMain(dialog);
    if (show) {
        SetFocus(GetDlgItem(dialog, IDC_PROFILE_SEARCH));
        SendDlgItemMessageW(dialog, IDC_PROFILE_SEARCH, EM_SETSEL, 0, -1);
    }
}

/* Ctrl+F: the search box of the view shown, shown and gone into; gone into
 * when it shows and the focus is elsewhere; hidden, and emptied, when it
 * shows and holds the focus. */
static void ToggleSearch(HWND dialog)
{
    HWND search = GetDlgItem(dialog, IDC_PROFILE_SEARCH);
    if (SessionsView_Shown()) {
        SessionsView_ToggleSearch();
    } else if (g_manager.profileSearchShown && GetFocus() != search) {
        SetFocus(search);
        SendMessageW(search, EM_SETSEL, 0, -1);
    } else {
        ShowProfileSearch(dialog, !g_manager.profileSearchShown);
    }
}

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
        ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH_LABEL), SW_HIDE);
        ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH), SW_HIDE);
    } else {
        for (i = 0; i < ARRAYSIZE(kProfileControls); i++) ShowWindow(GetDlgItem(dialog, kProfileControls[i]), SW_SHOW);
        ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH_LABEL), g_manager.profileSearchShown ? SW_SHOW : SW_HIDE);
        ShowWindow(GetDlgItem(dialog, IDC_PROFILE_SEARCH), g_manager.profileSearchShown ? SW_SHOW : SW_HIDE);
        FocusView(dialog, FALSE);
        SessionsView_Leave();
    }
    ShowFoot(dialog, !showSessions);
    SetDlgItemTextW(dialog, IDC_SESSIONS, SessionsButtonCaption(showSessions));
    Theme_SetMainGlyph(dialog, IDC_SESSIONS, showSessions);
    Theme_LayoutMain(dialog);
    /* The note was hidden meanwhile: a rename or a new language there is fitted now that it shows. */
    if (!showSessions) UpdateNote();
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

/* The Status column follows each profile's Claude without polling: a thread
 * waits for the main process of each one running to end, and for a file
 * made or removed right in the data folder of each one closed (a starting
 * Claude makes its lock file there first; the file itself is never opened).
 * Each brings a look at the instance windows (Claude_UpdateRunning), and one
 * more a moment later: the window comes right after the lock file. A change
 * ends the thread and refreshes the window, which watches what then runs. */
#define RUNNING_LOOK_AGAIN_MS 1500

typedef struct RunningWatch {
    HWND        dialog;
    HANDLE      stop;
    ProfileList profiles;   /* as the window shows them */
    ProfileList now;        /* the thread's look */
} RunningWatch;

/* What the watch waits for changed: a profile came or went, or one started or closed. */
static BOOL RunningChanged(const ProfileList *now, const ProfileList *shown)
{
    int i;
    if (now->count != shown->count) return TRUE;
    for (i = 0; i < shown->count; i++)
        if (now->items[i].running != shown->items[i].running || now->items[i].pid != shown->items[i].pid ||
            !Core_EqualsI(now->items[i].folder, shown->items[i].folder) || wcscmp(now->items[i].storageDir, shown->items[i].storageDir) != 0)
            return TRUE;
    return FALSE;
}

static DWORD WINAPI WatchRunning(void *param)
{
    RunningWatch *watch = (RunningWatch *)param;
    HANDLE handles[MAX_PROFILES + 1];
    BOOL folder[MAX_PROFILES + 1];
    DWORD n = 0, wait, timeout = 0, i;   /* a first look at once: a change before the handles were made */
    int p;
    handles[n] = watch->stop;
    folder[n++] = FALSE;
    for (p = 0; p < watch->profiles.count; p++) {
        const Profile *profile = &watch->profiles.items[p];
        HANDLE handle = NULL;
        if (profile->running && profile->pid) {
            handle = OpenProcess(SYNCHRONIZE, FALSE, profile->pid);
            folder[n] = FALSE;
        } else if (!profile->running && profile->storageDir[0]) {
            handle = FindFirstChangeNotificationW(profile->storageDir, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME);
            if (handle == INVALID_HANDLE_VALUE) handle = NULL;
            folder[n] = TRUE;
        }
        if (handle) handles[n++] = handle;
    }
    for (;;) {
        wait = WaitForMultipleObjects(n, handles, FALSE, timeout);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_FAILED) break;
        if (wait > WAIT_OBJECT_0 && wait < WAIT_OBJECT_0 + n) {
            i = wait - WAIT_OBJECT_0;
            /* A process that ended, or a folder no longer watched, stays signaled: it goes. */
            if (!folder[i] || !FindNextChangeNotification(handles[i])) {
                if (folder[i]) FindCloseChangeNotification(handles[i]);
                else CloseHandle(handles[i]);
                handles[i] = handles[--n];
                folder[i] = folder[n];
            }
        }
        watch->now = watch->profiles;
        Claude_UpdateRunning(&watch->now);
        if (RunningChanged(&watch->now, &watch->profiles)) {
            if (InterlockedOr(&g_outside, CHANGE_RUNNING) == 0) PostMessageW(watch->dialog, WM_APP_OUTSIDE, 0, 0);
            break;
        }
        timeout = wait == WAIT_TIMEOUT ? INFINITE : RUNNING_LOOK_AGAIN_MS;
    }
    for (i = 1; i < n; i++) {
        if (folder[i]) FindCloseChangeNotification(handles[i]);
        else CloseHandle(handles[i]);
    }
    return 0;
}


static void StopWatchingRunning(void)
{
    if (g_manager.runningWatch) SetEvent(g_manager.runningWatch->stop);
    if (g_manager.runningThread) {
        WaitForSingleObject(g_manager.runningThread, INFINITE);
        CloseHandle(g_manager.runningThread);
    }
    if (g_manager.runningWatch) {
        if (g_manager.runningWatch->stop) CloseHandle(g_manager.runningWatch->stop);
        HeapFree(GetProcessHeap(), 0, g_manager.runningWatch);
    }
    g_manager.runningThread = NULL;
    g_manager.runningWatch = NULL;
}

/* The profiles the window shows, watched: started again only when what it
 * shows of them changed, or the last watch ended. */
static void WatchRunningProfiles(void)
{
    RunningWatch *watch;
    if (g_manager.runningThread && WaitForSingleObject(g_manager.runningThread, 0) == WAIT_TIMEOUT && g_manager.runningWatch &&
        !RunningChanged(&g_manager.profiles, &g_manager.runningWatch->profiles))
        return;
    StopWatchingRunning();
    if (StateChangesBlocked() || (watch = (RunningWatch *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *watch)) == NULL) return;
    watch->dialog = g_manager.dlg;
    watch->profiles = g_manager.profiles;
    if ((watch->stop = CreateEventW(NULL, TRUE, FALSE, NULL)) == NULL ||
        (g_manager.runningThread = CreateThread(NULL, 0, WatchRunning, watch, 0, NULL)) == NULL) {
        Util_Log(L"profiles starting and closing are not watched (error %lu)", GetLastError());
        if (watch->stop) CloseHandle(watch->stop);
        HeapFree(GetProcessHeap(), 0, watch);
        return;
    }
    g_manager.runningWatch = watch;
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
    WCHAR folder[MAX_PATH], path[MAX_PATH];
    PWSTR chosen = NULL;
    HRESULT hr;
    int i, p;
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
                if (!Shortcut_FreePath(folder, profile.name, path, ARRAYSIZE(path))) Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"The path is too long."));
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

/* The manager started again, its window at the windows' new size; this one
 * has let go of the window's mutex. */
static void OpenAgain(void)
{
    WCHAR self[MAX_PATH], command[MAX_PATH + 2];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD n = GetModuleFileNameW(NULL, self, ARRAYSIZE(self));
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (!n || n >= ARRAYSIZE(self) || FAILED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\"", self)) ||
        !CreateProcessW(self, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process)) {
        Util_Log(L"could not open the manager again at its new size (error %lu)", GetLastError());
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

static void RemoveShieldTray(HWND dialog);

static void Close(HWND dialog)
{
    g_manager.closing = TRUE;
    Shield_Stop();
    RemoveShieldTray(dialog);
    EnumThreadWindows(GetCurrentThreadId(), CloseOwnedProc, (LPARAM)dialog);
    EndDialog(dialog, 0);
}

/* Only the user can make Claude Delta the app for
 * claude:// links, in Windows' own chooser: say what to pick, then let Windows
 * ask. */
static void SetUpLinks(void)
{
    WCHAR exe[MAX_PATH];
    if (!g_manager.pkg.found || Handler_UserChoice() == USERCHOICE_OURS) return;
    if (!Ui_Ask(g_manager.dlg, IDI_INFORMATION,
                TR(L"Windows now asks which app opens claude:// links.\n\nChoose \x201C" APP_NAME L"\x201D, then Always."),
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
/* Repair, asked first, in a sentence. */
static BOOL ConfirmRepair(void)
{
    DWORD sessions = LinkedProfiles(AllProfiles());
    int p;
    for (p = 0; p < g_manager.profiles.count; p++)
        if (g_manager.profiles.items[p].syncGroup) sessions |= 1u << p;
    return SyncUi_Confirm(g_manager.dlg, NULL,
                          TR(L"Repair " APP_NAME L"?\n\nIt registers the program and claude:// links again, fixes shortcuts, pins and icons, "
                             L"then syncs the profiles' sessions."),
                          sessions != 0, TR(L"Repair"));
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
    } else if (g_manager.ghFound) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_DOWNLOADING)), g_manager.ghVersion);
        SetTextIfChanged(IDC_VERSION, text);
    } else if (Update_Available(version, ARRAYSIZE(version))) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_DOWNLOADING)), version);
        SetTextIfChanged(IDC_VERSION, text);
    }
}

/* A newer release, or release of main, shows in the version's place, with its Update button. */
static void ShowUpdate(void)
{
    WCHAR version[32], text[256];
    if (g_manager.updating) return;
    if (g_manager.ghFound) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_AVAILABLE)), g_manager.ghVersion);
        SetTextIfChanged(IDC_VERSION, text);
        EnableControl(IDC_UPDATE, TRUE);
        ShowOptionalButton(IDC_UPDATE, TRUE);
        return;
    }
    if (!Update_Available(version, ARRAYSIZE(version))) return;
    StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_AVAILABLE)), version);
    SetTextIfChanged(IDC_VERSION, text);
    EnableControl(IDC_UPDATE, TRUE);   /* an update that failed or was dropped turned it off */
    ShowOptionalButton(IDC_UPDATE, TRUE);
}

static void DoGhUpdate(void);

static void DoUpdate(void)
{
    WCHAR version[32];
    if (g_manager.ghFound) {
        DoGhUpdate();
        return;
    }
    if (g_manager.updating || !Update_Available(version, ARRAYSIZE(version))) return;
    g_manager.updating = TRUE;
    EnableControl(IDC_UPDATE, FALSE);
    ShowUpdateProgress();
    Update_Download(g_manager.dlg, WM_APP_DOWNLOADED);
}

/* This copy's version, the day of its changes (the changelogs' heading of it), in the header. */
static void ShowVersion(void)
{
    WCHAR text[128];
    if (g_manager.updating) {
        ShowUpdateProgress();
        return;
    }
    if (g_manager.ghChecking) {
        SetTextIfChanged(IDC_VERSION, TR(Theme_MainVersion(MAIN_VERSION_CHECKING)));
        return;
    }
    if (!g_manager.build[0]) StringCchCopyW(g_manager.build, ARRAYSIZE(g_manager.build), APP_VERSION_DATE_W);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(Theme_MainVersion(MAIN_VERSION_BUILD)), g_manager.build);
    SetTextIfChanged(IDC_VERSION, text);
    ShowUpdate();
}

/* ------------------------------------------------------------ releases of main (ghupdate.c) */

static INT_PTR CALLBACK GhUpdateProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    const GhRelease *release;
    WCHAR text[512], *notes = NULL;
    size_t start, length;
    switch (msg) {
    case WM_INITDIALOG:
        release = (const GhRelease *)lp;
        /* Its notes in the interface's language: the release has them in English, then in Chinese, then how to install. */
        if (release->notes && Core_ReleaseNotesIn(release->notes, Core_EqualsI(Localize_LanguageCode(Localize_EffectiveLanguage()), L"zh-CN"),
                                                  &start, &length) &&
            (notes = (WCHAR *)Util_Alloc((length + 1) * sizeof(WCHAR))) != NULL) {
            memcpy(notes, release->notes + start, length * sizeof(WCHAR));
            notes[length] = 0;
        }
        if (!release->local)
            StringCchPrintfW(text, ARRAYSIZE(text), TR(L"A new version is available: %s.\nThis copy is version %s (%s)."), release->title,
                             APP_VERSION_DATE_W, GhUpdate_OwnCommit());
        else
            StringCchPrintfW(text, ARRAYSIZE(text), TR(L"The newest version is %s.\nThis copy was built on this PC (%s), and may be newer."),
                             release->title, GhUpdate_OwnCommit());
        SetDlgItemTextW(dialog, IDC_U_TEXT, text);
        Theme_SetRichText(GetDlgItem(dialog, IDC_U_NOTES), notes ? notes : release->notes ? release->notes
                          : release->viaGit ? TR(L"Its notes are on its release page, which Update now opens.") : L"");
        Util_Free(notes);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, LOWORD(wp));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* A release, as Later keeps it: its commit's checksum. */
static DWORD ReleaseMark(const GhRelease *release)
{
    return Core_Crc32(0, release->commit, (DWORD)(wcslen(release->commit) * sizeof(WCHAR)));
}

/* The release found offered: Update now updates, Later leaves it beside the version and offers it again only once
 * another is out. */
static void OfferGhRelease(const GhRelease *release)
{
    Theme_RichTextReady();
    if (Ui_Dialog(g_manager.dlg, IDD_UPDATE, GhUpdateProc, (LPARAM)release) == IDOK) DoGhUpdate();
    else Util_SetSetting(SETTING_GH_LATER, ReleaseMark(release));
}

/* What the check found (GhUpdate_Check): a release to update to shows beside the version and is offered, once when
 * found as the manager opens, each time when asked; asked, the other answers are said too. A copy brought from
 * elsewhere (no sign-in to GitHub on this PC) shows its version in red, and is told nothing. */
static void GhChecked(GhRelease *release, BOOL asked)
{
    BOOL offer;
    g_manager.ghChecking = FALSE;
    InvalidateRect(GetDlgItem(g_manager.dlg, IDC_VERSION), NULL, TRUE);   /* red or not, as GhUpdate_Copied says now */
    if (release->status == GH_NEWER) {
        g_manager.ghFound = TRUE;
        g_manager.ghViaGit = release->viaGit;
        StringCchCopyW(g_manager.ghTag, ARRAYSIZE(g_manager.ghTag), release->tag);
        StringCchCopyW(g_manager.ghVersion, ARRAYSIZE(g_manager.ghVersion), release->version);
    }
    ShowVersion();
    offer = release->status == GH_NEWER && !StateChangesBlocked() && !g_manager.updating && IsWindowEnabled(g_manager.dlg) &&
            IsWindowVisible(g_manager.dlg) && (asked || Util_GetSetting(SETTING_GH_LATER, 0) != ReleaseMark(release));
    if (offer) OfferGhRelease(release);
    else if (asked && release->status == GH_CURRENT)
        Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(L"This is the newest version (%s, %s)."), APP_VERSION_DATE_W, GhUpdate_OwnCommit());
    else if (asked && release->status == GH_NONE)
        Ui_Message(g_manager.dlg, MB_ICONINFORMATION, TR(L"%s has no releases yet."), GhUpdate_Repo());
    else if (asked && release->status == GH_FAILED)
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The newest version could not be looked up: %s"), release->message);
    GhUpdate_Free(release);
}

/* A click on the version: the newest release looked up now. */
static void CheckGhNow(void)
{
    if (g_manager.updating || g_manager.ghChecking || StateChangesBlocked()) return;
    g_manager.ghChecking = TRUE;
    ShowVersion();
    GhUpdate_Check(g_manager.dlg, WM_APP_GH_UPDATE, TRUE);
}

static void DoGhUpdate(void)
{
    WCHAR page[512];
    if (g_manager.updating || !g_manager.ghFound) return;
    if (g_manager.ghViaGit) {   /* no gh to download it with: its page, where the browser is signed in */
        GhUpdate_ReleasePage(g_manager.ghTag, page, ARRAYSIZE(page));
        Util_OpenUrl(page);
        return;
    }
    g_manager.updating = TRUE;
    EnableControl(IDC_UPDATE, FALSE);
    ShowUpdateProgress();
    GhUpdate_Download(g_manager.dlg, WM_APP_GH_DOWNLOADED, g_manager.ghTag);
}

/* The release downloaded installs itself over this copy and closes this window. */
static void GhDownloaded(GhDownload *download)
{
    DWORD error = 0;
    if (!StateChangesBlocked()) {
        if (download->ok) {
            FinishShellWork();   /* the new copy replaces this one */
            if (GhUpdate_Run(download, &error)) {
                g_manager.installing = TRUE;
                ShowUpdateProgress();
                Util_Free(download);
                return;
            }
            Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The new version could not be started (error %lu)."), error);
        } else {
            Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The new version could not be downloaded: %s"), download->message);
        }
    }
    g_manager.updating = FALSE;
    g_manager.installing = FALSE;
    EnableControl(IDC_UPDATE, TRUE);
    Util_Free(download);
    ShowVersion();
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

/* The language chosen in Settings (-1: Windows'). */
static void DoLanguage(int choice)
{
    int languageBefore = Localize_EffectiveLanguage();
    if (StateChangesBlocked()) return;
    FinishShellWork();   /* the previous language's, written in that language */
    if (!Localize_SetLanguage(choice, TRUE)) {
        /* Over Settings, which is open: a box owned by the window under it would enable that window on closing. */
        Ui_Message(GetLastActivePopup(g_manager.dlg), MB_ICONWARNING, TR(L"The language preference could not be saved."));
        return;
    }
    Localize_Window(g_manager.dlg);
    Theme_Apply(g_manager.dlg);
    SetColumnTitles();
    SendDlgItemMessageW(g_manager.dlg, IDC_PROFILE_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Name, folder or notes"));
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
    if (result == UPDATE_NOT_SIGNED)   /* this fork's releases are not signed: the user installs them from the page */
        openPage = Ui_Message(g_manager.dlg, MB_ICONWARNING | MB_YESNO,
                              TR(L"The new version is not signed by the author of " APP_NAME L", so it was not installed.\n\n"
                                 L"Open its download page to install it yourself?")) == IDYES;
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

static void AddColumns(void)
{
    LVCOLUMNW column;
    int i;
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_WIDTH;
    column.cx = FIRST_COLUMN_WIDTH;
    for (i = 0; Theme_ProfileColumnTitle(i); i++) ListView_InsertColumn(g_manager.list, i, &column);
    Gui_OrderProfileColumns(g_manager.list);
    SetColumnTitles();
    LayoutColumns();
}

static void LayoutMainControls(void)
{
    if (!g_manager.layoutReady || g_manager.layingOut) return;
    g_manager.layingOut = TRUE;
    Theme_LayoutMain(g_manager.dlg);
    LayoutColumns();
    g_manager.layingOut = FALSE;
}

/* Fits the window to its controls in the current language, fonts and scale. */
static void ReflowMain(void)
{
    Theme_FitDialog(g_manager.dlg);   /* places the controls too */
    g_manager.layoutReady = TRUE;
    LayoutColumns();
    UpdateNote();
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

/* DialogBox owns the message loop; its filter sees Ctrl+F from every main-window control. */
static LRESULT CALLBACK SearchShortcut(int code, WPARAM wp, LPARAM lp)
{
    const MSG *message = (const MSG *)lp;
    if (code == MSGF_DIALOGBOX && message->message == WM_KEYDOWN && message->wParam == 'F' &&
        GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0 && g_manager.dlg &&
        GetAncestor(message->hwnd, GA_ROOT) == g_manager.dlg && IsWindowEnabled(g_manager.dlg)) {
        ToggleSearch(g_manager.dlg);
        return 1;
    }
    return CallNextHookEx(g_manager.searchHook, code, wp, lp);
}

/* WM_INITDIALOG: the controls, the profiles and what watches them; `start`
 * (GuiStart) may ask for the uninstall dialog alone, or to set up links. */
static void InitMain(HWND dialog, LPARAM start)
{
    g_manager.dlg = dialog;
    g_manager.profileSearchShown = FALSE;
    g_manager.searchHook = SetWindowsHookExW(WH_MSGFILTER, SearchShortcut, NULL, GetCurrentThreadId());
    Localize_Window(dialog);
    g_manager.list = GetDlgItem(dialog, IDC_LIST);
    SendDlgItemMessageW(dialog, IDC_PROFILE_SEARCH, EM_LIMITTEXT, ARRAYSIZE(g_manager.profileSearch) - 1, 0);
    SendDlgItemMessageW(dialog, IDC_PROFILE_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Name, folder or notes"));
    Theme_SetWindowIcons(dialog, &g_manager.bigIcon, &g_manager.smallIcon);
    ListView_SetExtendedListViewStyle(g_manager.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    AddColumns();
    Theme_SetRowMarks(g_manager.list, ProfileMarks);
    Theme_SmoothView(g_manager.list);
    SessionsView_Init(dialog);
    Theme_SetStrong(GetDlgItem(dialog, IDC_SESSIONS));   /* it leads to the other view */
    Shield_SetupButton(dialog, IDC_SHIELD);
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
    DragOut_AcceptFiles(dialog, WM_APP_FILES_DROPPED);   /* backups dropped on a profile (DropFiles); a drag of ours refused */
    SessionsView_AcceptDrops(WM_APP_SESSION_DROP);       /* sessions dragged onto a profile in the view, or archives dragged in */
    WatchRunningProfiles();
    /* Started to uninstall (Settings > Apps): only the uninstall dialog
     * shows, and the manager never opens (wParam 1). */
    if (start == GUI_UNINSTALL) {
        g_manager.uninstallOnly = TRUE;
        PostMessageW(dialog, WM_APP_UNINSTALL, 1, 0);
    } else if (start == GUI_SET_UP_LINKS) {
        PostMessageW(dialog, WM_APP_SETUPLINKS, 0, 0);
    }
    if (start != GUI_UNINSTALL) {
        Update_Check(dialog, WM_APP_UPDATE);
        GhUpdate_Check(dialog, WM_APP_GH_UPDATE, FALSE);
        PostMessageW(dialog, WM_APP_KEEP_SESSIONS, 0, 0);
        Shield_Start(dialog, &g_manager.pkg);
    }
    if (g_manager.readyEvent) SetEvent(g_manager.readyEvent);   /* a second launch can now find this window */
    SetFocus(g_manager.list);
}

/* The profile list's notifications: TRUE when one is answered, with
 * DWLP_MSGRESULT set. */
static BOOL ListNotify(HWND dialog, const NMHDR *header)
{
    switch (header->code) {
    case NM_SETFOCUS:
    case NM_KILLFOCUS:
        InvalidateRect(g_manager.list, NULL, FALSE);
        break;
    case LVN_ITEMCHANGED: {
        const NMLISTVIEW *change = (const NMLISTVIEW *)header;
        /* Selecting another row first deselects the old one: the buttons
         * follow once, when the list is done. */
        if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED))
            PostOnce(&g_manager.selectionPending, WM_APP_SELECTION);
        break;
    }
    case NM_DBLCLK:
        if (((const NMITEMACTIVATE *)header)->iItem >= 0) DoOpen();
        break;
    case LVN_BEGINDRAG:
        /* Out of the window, a profile is a file: its backup (dragout.c). */
        if (!StateChangesBlocked() && !g_manager.job && SelectedProfiles())
            DragOut_Profiles(dialog, WM_APP_DRAG_DROPPED, WM_APP_DRAG_ZONE, g_manager.list, &g_manager.profiles, SelectedProfiles(),
                             ((const NMLISTVIEW *)header)->ptAction);
        break;
    case NM_RCLICK:
    case LVN_BEGINRDRAG: {
        /* The menu at the pointer for a right click, and for a press a touchpad
         * moved past the drag distance: the list starts a right drag then, and
         * would show no menu. Handled here, the list sends no WM_CONTEXTMENU. */
        LVHITTESTINFO hit;
        DWORD position = GetMessagePos();
        int item;
        /* The row under the pointer, as Explorer: one not selected is chosen alone first,
         * so that the menu is its own (the list itself left the selection as it was). */
        ZeroMemory(&hit, sizeof hit);
        hit.pt.x = (short)LOWORD(position);
        hit.pt.y = (short)HIWORD(position);
        ScreenToClient(g_manager.list, &hit.pt);
        item = header->code == LVN_BEGINRDRAG ? ((const NMLISTVIEW *)header)->iItem : ListView_HitTest(g_manager.list, &hit);
        if (item >= 0 && !ListView_GetItemState(g_manager.list, item, LVIS_SELECTED)) {
            ListView_SetItemState(g_manager.list, -1, 0, LVIS_SELECTED);
            ListView_SetItemState(g_manager.list, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        ListMenu((LPARAM)GetMessagePos());
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
        return TRUE;
    }
    case LVN_KEYDOWN: {
        WORD key = ((const NMLVKEYDOWN *)header)->wVKey;
        if (key == VK_DELETE) DoDelete();
        else if (key == 'A' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0)
            ListView_SetItemState(g_manager.list, -1, LVIS_SELECTED, LVIS_SELECTED);   /* Ctrl+A: every profile */
        break;
    }
    }
    return FALSE;
}

/* WM_COMMAND: TRUE when it is handled. */

/* Windows MCP: Windows-MCP added to the profiles selected (uv installed
 * first when it is not, asked), or taken away when they all have it. Claude
 * reads its servers when it starts: a profile open is closed for it and
 * opened again, or, when some are closed, left out, as the person chooses. */
static void DoWindowsMcp(void)
{
    WCHAR uvx[MAX_PATH], names[MAX_PROFILES * (LABEL_CCH + 8)], text[MAX_PROFILES * (LABEL_CCH + 8) + 512];
    DWORD selected = SelectedProfiles(), running = 0, reopen = 0, failed = 0, error = 0, extension;
    BOOL remove;
    int i;
    if (!selected || StateChangesBlocked() || g_manager.job) return;
    remove = WithWindowsMcp(selected, WINDOWS_MCP_READY) == selected;
    /* Claude's own Windows-MCP extension beside it is a second server with the same tools: set up with
     * ours, it can go, in the same close (asked). */
    if ((extension = remove ? 0 : WithExtension(selected)) != 0) {
        Profiles_JoinNames(&g_manager.profiles, extension, names, ARRAYSIZE(names));
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"%s also has Claude's Windows-MCP extension: two servers would offer the same tools. Uninstall the extension?"), names);
        if (!Ui_Ask(g_manager.dlg, IDI_QUESTION, text, TR(L"Uninstall it"), TR(L"Keep it"), FALSE)) extension = 0;
    }
    if (!remove && !WindowsMcp_FindUvx(uvx, ARRAYSIZE(uvx))) {
        if (!Ui_Ask(g_manager.dlg, IDI_QUESTION,
                    TR(L"Windows-MCP runs through uv, which is not installed. Install uv now with its own installer (from astral.sh, in a PowerShell window)?"),
                    TR(L"Install uv"), TR(L"Cancel"), FALSE))
            return;
        if (!WindowsMcp_InstallUv(g_manager.dlg) || !WindowsMcp_FindUvx(uvx, ARRAYSIZE(uvx))) {
            Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"uv could not be installed. Install it from https://docs.astral.sh/uv/ and try again."));
            return;
        }
    }
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && Claude_IsRunning(&g_manager.profiles.items[i])) running |= 1u << i;
    if (running) {
        BOOL several = (running & (running - 1)) != 0;
        int choice;
        Profiles_JoinNames(&g_manager.profiles, running, names, ARRAYSIZE(names));
        if (remove)
            StringCchPrintfW(text, ARRAYSIZE(text), several
                ? TR(L"%s are open: Claude reads its MCP servers when it starts. Close them, take Windows-MCP away, and open them again?")
                : TR(L"%s is open: Claude reads its MCP servers when it starts. Close it, take Windows-MCP away, and open it again?"), names);
        else
            StringCchPrintfW(text, ARRAYSIZE(text), several
                ? TR(L"%s are open: Claude reads its MCP servers when it starts. Close them, set up Windows-MCP, and open them again?")
                : TR(L"%s is open: Claude reads its MCP servers when it starts. Close it, set up Windows-MCP, and open it again?"), names);
        /* Some selected are closed: those alone, the open ones left as they are, is a choice too. */
        if (running != selected)
            choice = Ui_Choose(g_manager.dlg, IDI_QUESTION, text, remove ? TR(L"Close and remove") : TR(L"Close and set up"),
                               remove ? TR(L"Remove only from those not open") : TR(L"Set up only those not open"), TR(L"Cancel"));
        else
            choice = Ui_Ask(g_manager.dlg, IDI_QUESTION, text, remove ? TR(L"Close and remove") : TR(L"Close and set up"), TR(L"Cancel"), FALSE);
        if (!choice) return;
        if (choice == 2) {
            selected &= ~running;
            running = 0;
        }
    }
    FinishShellWork();
    for (i = 0; i < g_manager.profiles.count; i++) {
        const Profile *p = &g_manager.profiles.items[i];
        if (!(selected & (1u << i))) continue;
        if ((running & (1u << i)) && !Claude_Quit(p, &error)) {
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), p->name, error);
            continue;
        }
        if (running & (1u << i)) reopen |= 1u << i;
        if (!WindowsMcp_Set(p, remove ? NULL : uvx)) failed |= 1u << i;
        if ((extension & (1u << i)) && !WindowsMcp_RemoveExtension(g_manager.dlg, p)) failed |= 1u << i;
    }
    /* Servers an earlier entry left running once their Claude closed (an HTTP one keeps its port, and the desktop's control, open). */
    if ((i = WindowsMcp_Leftovers()) > 0) {
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"%d Windows-MCP server(s) started earlier are still running in the background, though the Claude that started them has closed. End them?"),
                         i);
        if (Ui_Ask(g_manager.dlg, IDI_QUESTION, text, TR(L"End them"), TR(L"Leave them"), FALSE)) WindowsMcp_EndLeftovers();
    }
    if (failed) {
        Profiles_JoinNames(&g_manager.profiles, failed, names, ARRAYSIZE(names));
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The MCP servers of %s could not be changed."), names);
    }
    UpdateRowsAndButtons();   /* their computer marks, and the button */
    if (reopen) RunJob(JOB_OPEN, reopen);
}

/* Claude's own Windows-MCP extension uninstalled from the profiles selected that have it. Claude reads its
 * extensions when it starts and keeps the list: those open are closed for it and opened again (asked). */
static void DoRemoveExtension(void)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], text[MAX_PROFILES * (LABEL_CCH + 8) + 512];
    DWORD with, running = 0, reopen = 0, failed = 0, error = 0;
    int i;
    if (StateChangesBlocked() || g_manager.job || (with = WithExtension(SelectedProfiles())) == 0) return;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((with & (1u << i)) && Claude_IsRunning(&g_manager.profiles.items[i])) running |= 1u << i;
    Profiles_JoinNames(&g_manager.profiles, running ? running : with, names, ARRAYSIZE(names));
    if (running)
        StringCchPrintfW(text, ARRAYSIZE(text), (running & (running - 1))
            ? TR(L"%s are open: Claude reads its extensions when it starts. Close them, uninstall the Windows-MCP extension, and open them again?")
            : TR(L"%s is open: Claude reads its extensions when it starts. Close it, uninstall the Windows-MCP extension, and open it again?"), names);
    else
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Uninstall Claude's Windows-MCP extension from %s?"), names);
    if (!Ui_Ask(g_manager.dlg, IDI_QUESTION, text, TR(L"Uninstall"), TR(L"Cancel"), FALSE)) return;
    FinishShellWork();
    for (i = 0; i < g_manager.profiles.count; i++) {
        const Profile *p = &g_manager.profiles.items[i];
        if (!(with & (1u << i))) continue;
        if ((running & (1u << i)) && !Claude_Quit(p, &error)) {
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), p->name, error);
            continue;
        }
        if (running & (1u << i)) reopen |= 1u << i;
        if (!WindowsMcp_RemoveExtension(g_manager.dlg, p)) failed |= 1u << i;
    }
    if (failed) {
        Profiles_JoinNames(&g_manager.profiles, failed, names, ARRAYSIZE(names));
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The Windows-MCP extension of %s could not be uninstalled."), names);
    }
    if (reopen) RunJob(JOB_OPEN, reopen);
}

/* What the SearXNG dialog shows, and what was chosen there. */
typedef struct SearXngChoice {
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)];   /* the profiles it is for */
    WCHAR url[URL_CCH];
    BOOL  allowPrivate;
    BOOL  canRemove;                               /* some of them have it */
    BOOL  remove;                                  /* Remove was pressed */
} SearXngChoice;

static INT_PTR CALLBACK SearXngProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    SearXngChoice *choice = (SearXngChoice *)GetWindowLongPtrW(dialog, DWLP_USER);
    WCHAR text[MAX_PROFILES * (LABEL_CCH + 8) + 256], typed[URL_CCH];
    switch (msg) {
    case WM_INITDIALOG:
        choice = (SearXngChoice *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Claude in %s searches the web through your SearXNG instance, with the mcp-searxng server (Node.js runs it)."),
                         choice->names);
        SetDlgItemTextW(dialog, IDC_SX_TEXT, text);
        SendDlgItemMessageW(dialog, IDC_SX_URL, EM_LIMITTEXT, URL_CCH - 1, 0);
        SendDlgItemMessageW(dialog, IDC_SX_URL, EM_SETCUEBANNER, TRUE, (LPARAM)L"https://searx.example.com");
        SetDlgItemTextW(dialog, IDC_SX_URL, choice->url);
        CheckDlgButton(dialog, IDC_SX_PRIVATE, choice->allowPrivate ? BST_CHECKED : BST_UNCHECKED);
        ShowWindow(GetDlgItem(dialog, IDC_SX_REMOVE), choice->canRemove ? SW_SHOW : SW_HIDE);
        EnableWindow(GetDlgItem(dialog, IDC_SX_REMOVE), choice->canRemove);
        SetFocus(GetDlgItem(dialog, IDC_SX_URL));
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK:
            GetDlgItemTextW(dialog, IDC_SX_URL, typed, ARRAYSIZE(typed));
            if (!Core_SearXngUrl(typed, choice->url, ARRAYSIZE(choice->url))) {
                Ui_Message(dialog, MB_ICONINFORMATION, TR(L"Type your SearXNG instance's address, such as https://searx.example.com."));
                SetFocus(GetDlgItem(dialog, IDC_SX_URL));
                return TRUE;
            }
            choice->allowPrivate = IsDlgButtonChecked(dialog, IDC_SX_PRIVATE) == BST_CHECKED;
            choice->remove = FALSE;
            EndDialog(dialog, IDOK);
            return TRUE;
        case IDC_SX_REMOVE:
            if (!choice->canRemove) return TRUE;
            choice->remove = TRUE;
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

/* SearXNG search set up in profiles `change` (mcp-searxng, run by Node.js's npx: windowsmcp.c) with `url`, or ours
 * taken away from them (`remove`). Claude reads its MCP servers when it starts: those open are closed for it and
 * opened again (asked), as for Windows MCP. */
static void ApplySearXng(DWORD change, BOOL remove, const WCHAR *url, BOOL allowPrivate)
{
    WCHAR npx[MAX_PATH], last[1][MAX_PATH], names[MAX_PROFILES * (LABEL_CCH + 8)], text[MAX_PROFILES * (LABEL_CCH + 8) + 512];
    DWORD running = 0, reopen = 0, failed = 0, error = 0;
    int i, answer;
    if (!change) return;
    if (!remove) {
        if (!SearXng_FindNpx(npx, ARRAYSIZE(npx))) {
            Ui_Message(g_manager.dlg, MB_ICONWARNING,
                       TR(L"SearXNG search runs on Node.js, which is not installed. Install Node.js from https://nodejs.org and try again."));
            return;
        }
        if (wcslen(url) < MAX_PATH) {
            StringCchCopyW(last[0], ARRAYSIZE(last[0]), url);
            Util_SetSettingList(SETTING_SEARXNG_SECTION, (const WCHAR (*)[MAX_PATH])last, 1);
        }
        Util_SetSetting(SETTING_SEARXNG_PRIVATE, allowPrivate ? 1 : 0);
    }
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((change & (1u << i)) && Claude_IsRunning(&g_manager.profiles.items[i])) running |= 1u << i;
    if (running) {
        BOOL several = (running & (running - 1)) != 0;
        Profiles_JoinNames(&g_manager.profiles, running, names, ARRAYSIZE(names));
        if (remove)
            StringCchPrintfW(text, ARRAYSIZE(text), several
                ? TR(L"%s are open: Claude reads its MCP servers when it starts. Close them, take SearXNG search away, and open them again?")
                : TR(L"%s is open: Claude reads its MCP servers when it starts. Close it, take SearXNG search away, and open it again?"), names);
        else
            StringCchPrintfW(text, ARRAYSIZE(text), several
                ? TR(L"%s are open: Claude reads its MCP servers when it starts. Close them, set up SearXNG search, and open them again?")
                : TR(L"%s is open: Claude reads its MCP servers when it starts. Close it, set up SearXNG search, and open it again?"), names);
        if (running != change)
            answer = Ui_Choose(g_manager.dlg, IDI_QUESTION, text, remove ? TR(L"Close and remove") : TR(L"Close and set up"),
                               remove ? TR(L"Remove only from those not open") : TR(L"Set up only those not open"), TR(L"Cancel"));
        else
            answer = Ui_Ask(g_manager.dlg, IDI_QUESTION, text, remove ? TR(L"Close and remove") : TR(L"Close and set up"), TR(L"Cancel"), FALSE);
        if (!answer) return;
        if (answer == 2) {
            change &= ~running;
            running = 0;
        }
    }
    FinishShellWork();
    for (i = 0; i < g_manager.profiles.count; i++) {
        const Profile *p = &g_manager.profiles.items[i];
        if (!(change & (1u << i))) continue;
        if ((running & (1u << i)) && !Claude_Quit(p, &error)) {
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), p->name, error);
            continue;
        }
        if (running & (1u << i)) reopen |= 1u << i;
        if (!SearXng_Set(p, remove ? NULL : npx, url, allowPrivate)) failed |= 1u << i;
    }
    if (failed) {
        Profiles_JoinNames(&g_manager.profiles, failed, names, ARRAYSIZE(names));
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The MCP servers of %s could not be changed."), names);
    }
    UpdateRowsAndButtons();   /* their globe marks, and the button */
    if (reopen) RunJob(JOB_OPEN, reopen);
}

/* The address SearXNG search is set up with: the one set in Settings or set up last, else SEARXNG_DEFAULT_URL. */
static void SearXngAddress(WCHAR *url, size_t cch, BOOL *allowPrivate)
{
    WCHAR saved[1][MAX_PATH];
    *allowPrivate = Util_GetSetting(SETTING_SEARXNG_PRIVATE, 0) != 0;
    if (Util_GetSettingList(SETTING_SEARXNG_SECTION, saved, 1) == 1 && saved[0][0] && SUCCEEDED(StringCchCopyW(url, cch, saved[0]))) return;
    StringCchCopyW(url, cch, SEARXNG_DEFAULT_URL);
    *allowPrivate = FALSE;
}

/* The address SearXNG search is set up with when none is typed: the first profile selected that has it, else
 * SearXngAddress. */
static void KnownSearXng(DWORD selected, WCHAR *url, size_t cch, BOOL *allowPrivate)
{
    int i;
    for (i = 0; i < g_manager.profiles.count; i++)
        if ((selected & (1u << i)) && SearXng_Get(&g_manager.profiles.items[i], url, cch, allowPrivate) && url[0]) return;
    SearXngAddress(url, cch, allowPrivate);
}

/* The list's menu: the dialog, to set SearXNG search up, change its address, or take it away. */
static void DoSearXng(void)
{
    SearXngChoice choice;
    DWORD selected = SelectedProfiles(), with;
    if (!selected || StateChangesBlocked() || g_manager.job) return;
    ZeroMemory(&choice, sizeof choice);
    with = WithSearXng(selected);
    KnownSearXng(selected, choice.url, ARRAYSIZE(choice.url), &choice.allowPrivate);
    choice.canRemove = with != 0;
    Profiles_JoinNames(&g_manager.profiles, selected, choice.names, ARRAYSIZE(choice.names));
    if (Ui_Dialog(g_manager.dlg, IDD_SEARXNG, SearXngProc, (LPARAM)&choice) != IDOK) return;
    ApplySearXng(choice.remove ? with : selected, choice.remove, choice.url, choice.allowPrivate);
}

/* The column's button, as Windows MCP's: once every profile selected has it, ours taken away; else set up at once in
 * those without it, with the address known (KnownSearXng). */
static void DoSearXngButton(void)
{
    WCHAR url[URL_CCH];
    BOOL allowPrivate = FALSE;
    DWORD selected = SelectedProfiles(), with;
    if (!selected || StateChangesBlocked() || g_manager.job) return;
    with = WithSearXng(selected);
    if (with == selected) {
        ApplySearXng(with, TRUE, NULL, FALSE);
        return;
    }
    KnownSearXng(selected, url, ARRAYSIZE(url), &allowPrivate);
    ApplySearXng(selected & ~with, FALSE, url, allowPrivate);
}

/* Cowork removed from the profiles selected that have it: turned off in Claude's preferences and its virtual machine
 * deleted, asked first; or restored, once none of them has it (Claude fetches the virtual machine again when Cowork
 * is used). Claude reads its preferences when it starts: those open are closed for it and opened again. */
static void DoCowork(void)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], openNames[MAX_PROFILES * (LABEL_CCH + 8)], size[32];
    WCHAR text[2 * MAX_PROFILES * (LABEL_CCH + 8) + 1024], open[MAX_PROFILES * (LABEL_CCH + 8) + 256];
    DWORD selected = SelectedProfiles(), change, running = 0, reopen = 0, failed = 0, busy = 0, error = 0;
    ULONGLONG bytes = 0, freed;
    BOOL restore;
    int i, choice;
    if (!selected || StateChangesBlocked() || g_manager.job) return;
    for (i = 0; i < g_manager.profiles.count; i++)   /* as they are now: a virtual machine may have come since */
        if (selected & (1u << i)) g_manager.cowork[i] = Cowork_Get(&g_manager.profiles.items[i], &g_manager.coworkBytes[i]);
    change = WithCowork(selected);
    restore = change == 0;
    if (restore) change = selected;
    for (i = 0; i < g_manager.profiles.count; i++) {
        if (!(change & (1u << i))) continue;
        bytes += g_manager.coworkBytes[i];
        if (Claude_IsRunning(&g_manager.profiles.items[i])) running |= 1u << i;
    }
    Profiles_JoinNames(&g_manager.profiles, change, names, ARRAYSIZE(names));
    Profiles_JoinNames(&g_manager.profiles, running, openNames, ARRAYSIZE(openNames));
    /* Said briefly: one profile by its name, several by their count. */
    if (restore) {
        if (running) {
            if (running & (running - 1))
                StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%d profiles are open. Close them, restore Cowork, and open them again?"), Core_BitCount(running));
            else
                StringCchPrintfW(text, ARRAYSIZE(text), TR(L"\x201C%s\x201D is open. Close it, restore Cowork, and open it again?"), openNames);
            choice = running != change
                ? Ui_Choose(g_manager.dlg, IDI_QUESTION, text, TR(L"Close and restore"), TR(L"Restore only those not open"), TR(L"Cancel"))
                : Ui_Ask(g_manager.dlg, IDI_QUESTION, text, TR(L"Close and restore"), TR(L"Cancel"), FALSE);
            if (!choice) return;
            if (choice == 2) {
                change &= ~running;
                running = 0;
            }
        }
    } else {
        BOOL one = !(change & (change - 1));
        Core_FormatSize(bytes, size, ARRAYSIZE(size));
        if (one && bytes)
            StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Remove Cowork from \x201C%s\x201D? Its virtual machine (%s) is deleted."), names, size);
        else if (one)
            StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Remove Cowork from \x201C%s\x201D?"), names);
        else if (bytes)
            StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Remove Cowork from %d profiles? Their virtual machines (%s) are deleted."), Core_BitCount(change), size);
        else
            StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Remove Cowork from %d profiles?"), Core_BitCount(change));
        if (running) {
            if (running & (running - 1))
                StringCchPrintfW(open, ARRAYSIZE(open), TR(L"%d of them are open: they are closed and opened again."), Core_BitCount(running));
            else
                StringCchPrintfW(open, ARRAYSIZE(open), TR(L"\x201C%s\x201D is open: it is closed and opened again."), openNames);
            StringCchCatW(text, ARRAYSIZE(text), L"\n\n");
            StringCchCatW(text, ARRAYSIZE(text), open);
        }
        choice = running && running != change
            ? Ui_Choose(g_manager.dlg, IDI_WARNING, text, TR(L"Close and remove"), TR(L"Remove only from those not open"), TR(L"Cancel"))
            : Ui_Ask(g_manager.dlg, IDI_WARNING, text, running ? TR(L"Close and remove") : TR(L"Remove Cowork"), TR(L"Cancel"), FALSE);
        if (!choice) return;
        if (choice == 2) {
            change &= ~running;
            running = 0;
        }
    }
    FinishShellWork();
    for (i = 0; i < g_manager.profiles.count; i++) {
        const Profile *p = &g_manager.profiles.items[i];
        if (!(change & (1u << i))) continue;
        if ((running & (1u << i)) && !Claude_Quit(p, &error)) {
            Ui_Message(g_manager.dlg, MB_ICONERROR, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), p->name, error);
            continue;
        }
        if (running & (1u << i)) reopen |= 1u << i;
        /* Off first: a Claude that starts meanwhile fetches no virtual machine again. */
        if (!Cowork_Set(p, restore)) failed |= 1u << i;
        else if (!restore && !Cowork_RemoveVm(p, &freed)) busy |= 1u << i;
    }
    if (failed) {
        Profiles_JoinNames(&g_manager.profiles, failed, names, ARRAYSIZE(names));
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"Cowork could not be changed in %s."), names);
    }
    if (busy) {
        Profiles_JoinNames(&g_manager.profiles, busy, names, ARRAYSIZE(names));
        Ui_Message(g_manager.dlg, MB_ICONWARNING, TR(L"The virtual machine of %s could not be deleted, as something still uses it. Try again in a moment."), names);
    }
    UpdateRowsAndButtons();   /* the button, and the rows' tips */
    if (reopen) RunJob(JOB_OPEN, reopen);
}

const ProfileList *Gui_Profiles(void)
{
    return &g_manager.profiles;
}

BOOL Gui_SetPreferences(HWND owner, const PreferenceChange *changes, int count)
{
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], text[MAX_PROFILES * (LABEL_CCH + 8) + 512];
    DWORD change = 0, running = 0, reopen = 0, failed = 0, error = 0;
    int i, c, choice;
    if (!count || StateChangesBlocked()) return FALSE;
    for (i = 0; i < g_manager.profiles.count; i++) {
        change |= 1u << i;
        if (Claude_IsRunning(&g_manager.profiles.items[i])) running |= 1u << i;
    }
    if (running) {
        Profiles_JoinNames(&g_manager.profiles, running, names, ARRAYSIZE(names));
        StringCchPrintfW(text, ARRAYSIZE(text), (running & (running - 1))
            ? TR(L"%s are open: Claude reads these settings when it starts. Close them, change the settings, and open them again?")
            : TR(L"%s is open: Claude reads these settings when it starts. Close it, change the settings, and open it again?"), names);
        /* A sync or an open under way: those open keep theirs until they close and the settings are applied again. */
        choice = g_manager.job ? 2
               : running != change ? Ui_Choose(owner, IDI_QUESTION, text, TR(L"Close and change"), TR(L"Change only those not open"), TR(L"Cancel"))
                                   : Ui_Ask(owner, IDI_QUESTION, text, TR(L"Close and change"), TR(L"Cancel"), FALSE);
        if (!choice) return FALSE;
        if (choice == 2) {
            change &= ~running;
            running = 0;
        }
    }
    FinishShellWork();
    for (i = 0; i < g_manager.profiles.count; i++) {
        const Profile *p = &g_manager.profiles.items[i];
        if (!(change & (1u << i))) continue;
        if ((running & (1u << i)) && !Claude_Quit(p, &error)) {
            Ui_Message(owner, MB_ICONERROR, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), p->name, error);
            continue;
        }
        if (running & (1u << i)) reopen |= 1u << i;
        for (c = 0; c < count; c++)
            if (!Prefs_Set(p, changes[c].key, changes[c].raw)) failed |= 1u << i;
    }
    if (failed) {
        Profiles_JoinNames(&g_manager.profiles, failed, names, ARRAYSIZE(names));
        Ui_Message(owner, MB_ICONWARNING, TR(L"The settings of %s could not all be changed."), names);
    }
    if (reopen) RunJob(JOB_OPEN, reopen);
    return TRUE;
}

/* Profiles dragged were let go on a tile (dragout.c): what it says, for the
 * profiles selected; one alone for a copy or a sign-in, the one with the
 * focus when several are. */
static void DragZoneChosen(DragZone zone)
{
    const Profile *one = SelectedProfile() ? SelectedProfile() : FocusedProfile();
    if (StateChangesBlocked() || g_manager.job) return;
    switch (zone) {
    case DRAG_ZONE_EXPORT: DragOut_Export(g_manager.dlg, &g_manager.profiles, SelectedProfiles()); break;
    case DRAG_ZONE_COPY:   if (one) DoNew(one); break;
    case DRAG_ZONE_SIGNIN: if (one) SignIn_Open(g_manager.dlg, one); break;
    case DRAG_ZONE_DELETE: DoDelete(); break;
    default: break;
    }
}
static BOOL MainCommand(HWND dialog, WPARAM wp)
{
    if (LOWORD(wp) == IDC_VERSION && HIWORD(wp) == STN_CLICKED) {   /* the version: the newest release looked up */
        CheckGhNow();
        return TRUE;
    }
    /* The sessions view shows another profile: the toolbar acts on it. */
    if (LOWORD(wp) == IDC_S_PROFILES && HIWORD(wp) == LBN_SELCHANGE) {
        SessionsView_Command(wp);
        if (SessionsView_Shown()) SelectProfileRow(SessionsView_Profile());
        return TRUE;
    }
    switch (LOWORD(wp)) {
    case IDC_PROFILE_FIND:
        ShowProfileSearch(dialog, !g_manager.profileSearchShown);
        return TRUE;
    case IDC_PROFILE_SEARCH:
        if (HIWORD(wp) == EN_CHANGE && g_manager.list) {
            ListSelection selection;
            TakeSelection(&selection);
            GetDlgItemTextW(dialog, IDC_PROFILE_SEARCH, g_manager.profileSearch, ARRAYSIZE(g_manager.profileSearch));
            FillList(&selection);
        }
        return TRUE;
    case IDC_OPEN:          DoOpen(); return TRUE;
    case IDC_STOP:          DoQuit(FALSE); return TRUE;
    case IDC_RESTART:       DoQuit(TRUE); return TRUE;
    case IDC_PRIVATE_SIGNIN:
        if (!StateChangesBlocked() && !g_manager.job && SelectedProfile()) SignIn_Open(dialog, SelectedProfile());
        return TRUE;
    case IDC_SYNC_NOW:      if (ConfirmSync(SelectedProfiles())) RunJob(JOB_SYNC, SelectedProfiles()); return TRUE;
    case IDC_SYNC:          DoSyncSetup(); return TRUE;
    case IDC_BACKUP:        MenuCommand(IDM_LIST_BACKUP); return TRUE;
    case IDC_RESTORE_BACKUP: MenuCommand(IDM_LIST_RESTORE); return TRUE;
    case IDC_REPAIR:        DoRepair(); return TRUE;
    case IDC_WINDOWS_MCP:   DoWindowsMcp(); return TRUE;
    case IDC_SEARXNG:       DoSearXngButton(); return TRUE;
    case IDC_COWORK:        DoCowork(); return TRUE;
    case IDC_SHORTCUTS:     ShortcutsMenu(); return TRUE;
    case IDC_SETTINGS:
        Settings_Show(g_manager.dlg, DoLanguage, DoSettingsAction);
        if (g_manager.openAgain && !StateChangesBlocked() && !g_manager.job) Close(g_manager.dlg);
        return TRUE;
    case IDC_ABOUT:         About_Show(g_manager.dlg); return TRUE;
    case IDC_SHIELD:        Shield_ShowDialog(g_manager.dlg); return TRUE;
    case IDC_COPY_ALL:      DoSessions(SESSIONS_COPY_ALL); return TRUE;
    case IDC_MOVE_ALL:      DoSessions(SESSIONS_MOVE_ALL); return TRUE;
    case IDC_NEW:           DoNew(NULL); return TRUE;
    case IDC_EDIT:          DoEdit(); return TRUE;
    case IDC_DELETE:        DoDelete(); return TRUE;
    case IDC_MERGE:         DoSessions(SESSIONS_MERGE); return TRUE;
    case IDC_DEFAULT:       DoSetDefault(); return TRUE;
    case IDC_SC_DESKTOP:    DoDesktopShortcut(); return TRUE;
    case IDC_SC_SAVEAS:     DoSaveShortcut(); return TRUE;
    case IDC_SC_PIN:        DoPinTaskbar(); return TRUE;
    case IDC_SC_START:      DoStartMenu(); return TRUE;
    case IDC_SESSIONS:      ToggleSessions(); return TRUE;
    case IDC_STATUS_ACTION: DoStatusAction(); return TRUE;
    case IDC_UPDATE:        DoUpdate(); return TRUE;
    case IDCANCEL:
        /* Esc clears a search box that holds text, else closes the sessions
         * view (back to the profiles), and never closes the window: its
         * close box, Alt+F4 and the tray do (WM_CLOSE), so a key pressed by
         * mistake cannot quit the manager. */
        if (SessionsView_Shown()) {
            if (!SessionsView_ClearSearch()) ToggleSessions();
        } else if (g_manager.profileSearchShown) ShowProfileSearch(dialog, FALSE);
        return TRUE;
    }
    return SessionsView_Command(wp);
}

/* WM_APP_SYNC_CONFLICTS, from a sync of ours or of a watcher: asked once
 * nothing else is going on, the window shown and no dialog open. */
static void SettleConflicts(HWND dialog)
{
    ProfileList *copy;
    DWORD settled, now = 0;
    int i, at;
    g_manager.pendingConflicts = TRUE;
    if (StateChangesBlocked() || g_manager.job || !IsWindowVisible(dialog) || !IsWindowEnabled(dialog)) return;
    g_manager.pendingConflicts = FALSE;
    /* The list may change while the person chooses: the dialog has its own copy, its choices go by folder. */
    if ((copy = CopyProfiles()) == NULL) return;
    settled = SyncUi_SettleConflicts(dialog, copy);
    for (i = 0; i < copy->count; i++)
        if ((settled & (1u << i)) && (at = Profiles_Find(&g_manager.profiles, copy->items[i].folder)) >= 0) now |= 1u << at;
    Util_Free(copy);
    if (now) RunJob(JOB_KEEP, now);
}

/* WM_DESTROY: what InitMain started stops. */
static void DestroyMain(HWND dialog)
{
    g_manager.layoutReady = FALSE;
    DragOut_RefuseFiles(dialog);
    if (g_manager.searchHook) UnhookWindowsHookEx(g_manager.searchHook);
    g_manager.searchHook = NULL;
    FinishShellWork();
    Localize_ForgetWindow(dialog);
    StopWatchingOutside();
    StopWatchingRunning();
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
}

/* The shield's notification icon, there only while its notification shows:
 * Windows drops a notification whose icon goes before it is shown. */
static void RemoveShieldTray(HWND dialog)
{
    NOTIFYICONDATAW icon;
    if (!g_manager.shieldTray) return;
    ZeroMemory(&icon, sizeof icon);
    icon.cbSize = sizeof icon;
    icon.hWnd = dialog;
    icon.uID = SHIELD_TRAY_ID;
    Shell_NotifyIconW(NIM_DELETE, &icon);
    g_manager.shieldTray = FALSE;
}

/* A system notification that the shield just ended Claude, and why; its
 * icon goes once the notification times out, is closed or clicked (a click
 * opens the shield's dialog). */
static void ShieldToast(HWND dialog)
{
    NOTIFYICONDATAW icon;
    ShieldSnapshot s;
    WCHAR body[256];
    const WCHAR *reason = L"";
    int b;
    Shield_Snapshot(&s);
    for (b = 0; b < 16; b++)
        if (s.stop & (1u << b)) { reason = Shield_ReasonText(1u << b); break; }
    StringCchPrintfW(body, ARRAYSIZE(body), TR(L"Claude was stopped: %s. It stays off until the exit is safe again."), reason);
    RemoveShieldTray(dialog);
    ZeroMemory(&icon, sizeof icon);
    icon.cbSize = sizeof icon;
    icon.hWnd = dialog;
    icon.uID = SHIELD_TRAY_ID;
    icon.uFlags = NIF_INFO | NIF_ICON | NIF_MESSAGE | NIF_TIP;
    icon.uCallbackMessage = WM_APP_SHIELD_TRAY;
    icon.hIcon = g_manager.smallIcon ? g_manager.smallIcon : LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP));
    icon.dwInfoFlags = NIIF_WARNING;
    StringCchCopyW(icon.szTip, ARRAYSIZE(icon.szTip), APP_NAME);
    StringCchCopyW(icon.szInfoTitle, ARRAYSIZE(icon.szInfoTitle), APP_NAME);
    StringCchCopyW(icon.szInfo, ARRAYSIZE(icon.szInfo), body);
    g_manager.shieldTray = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
}

static INT_PTR CALLBACK MainProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    switch (message) {
    case WM_INITDIALOG:
        InitMain(dialog, lp);
        return FALSE;   /* InitMain set the focus */

    case WM_SHOWWINDOW:
        if (wp && !g_manager.uninstallOnly) SessionsView_Warm(&g_manager.profiles);
        if (wp && g_manager.pendingConflicts) PostMessageW(dialog, WM_APP_SYNC_CONFLICTS, 0, 0);
        break;

    case WM_APP_SESSIONS_READY:
        SessionsView_Ready(!StateChangesBlocked());
        return TRUE;

    case WM_APP_SHIELD_TRAY:
        switch (LOWORD(lp)) {
        case NIN_BALLOONUSERCLICK:
            RemoveShieldTray(dialog);
            if (IsWindowEnabled(dialog)) Shield_ShowDialog(dialog);
            break;
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE:
        case WM_LBUTTONUP:
            RemoveShieldTray(dialog);
            break;
        }
        return TRUE;

    case WM_APP_SHIELD:
        Shield_ShowState(dialog, IDC_SHIELD, (ShieldState)wp);
        if ((ShieldState)wp == SHIELD_STOPPED && g_manager.shieldState != SHIELD_STOPPED) {
            ShieldToast(dialog);   /* just stopped: tell the user why Claude closed */
            UpdateButtons();       /* Open and Restart turn off while it blocks */
        } else if ((ShieldState)wp != SHIELD_STOPPED && g_manager.shieldState == SHIELD_STOPPED) {
            UpdateButtons();       /* safe again: Open and Restart come back */
        }
        g_manager.shieldState = (ShieldState)wp;
        return TRUE;

    case WM_APP_SESSION_SEARCH:
        SessionsView_SearchFound(wp, lp);
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
        if (!StateChangesBlocked()) SessionsView_Notified();
        return TRUE;

    case WM_APP_KEEP_SESSIONS:
        if (!StateChangesBlocked()) KeepSessions();
        if (!StateChangesBlocked()) Transfer_ResumeLeft(dialog);   /* exports a close or an error cut short */
        return TRUE;

    case WM_APP_JOB_DONE:
        JobDone((Job *)lp);
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
        LRESULT result;
        if (header->idFrom == IDC_LIST) return ListNotify(dialog, header);
        if (!SessionsView_Notify(header, &result)) break;
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
        return TRUE;
    }

    case WM_COMMAND:
        return MainCommand(dialog, wp);

    case WM_APP_FILES_DROPPED:
        if (lp) DropFiles(dialog, (DroppedFiles *)lp);
        return TRUE;

    case WM_APP_DRAG_DROPPED:
        DragOut_Dropped(dialog, lp);
        return TRUE;

    case WM_APP_SESSION_DROP:
        SessionsView_HandleDrop(wp, lp);
        return TRUE;

    case WM_APP_DRAG_ZONE:
        DragZoneChosen((DragZone)wp);
        return TRUE;

    case WM_APP_SIGNIN_DONE:
        /* A private sign-in opened straight from here (signin.c): only a window that could not show the page is said. */
        if (!wp && lp) Ui_Message(dialog, MB_ICONWARNING, TR(L"The sign-in window could not show the page. Choose Microsoft Edge or Google Chrome."));
        return TRUE;

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

    case WM_APP_GH_UPDATE:
        if (lp) GhChecked((GhRelease *)lp, wp != 0);
        return TRUE;

    case WM_APP_GH_DOWNLOADED:
        if (lp) GhDownloaded((GhDownload *)lp);
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
        SettleConflicts(dialog);
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
        /* The sessions view's profiles: the profile's menu, as the list's, once it shows. */
        switch (SessionsView_ChooseProfileAt((HWND)wp, lp)) {
        case 1:
            SelectProfileRow(SessionsView_Profile());
            ListMenu(lp);
            return TRUE;
        case 0:
            return TRUE;
        }
        if (SessionsView_ContextMenu((HWND)wp, lp)) return TRUE;
        break;

    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
        INT_PTR brush = Theme_CtlColor(message, wp, lp, IDC_VERSION);
        /* A copy brought from elsewhere, no sign-in to GitHub on this PC (ghupdate.c): its version in red. */
        if (message == WM_CTLCOLORSTATIC && (HWND)lp == GetDlgItem(dialog, IDC_VERSION) && GhUpdate_Copied())
            SetTextColor((HDC)wp, Theme_TintColor(THEME_TINT_RED));
        return brush;
    }

    case WM_SETCURSOR:
        /* The version can be clicked: it looks for a newer release. */
        if ((HWND)wp == GetDlgItem(dialog, IDC_VERSION) && !g_manager.updating) {
            SetCursor(LoadCursorW(NULL, IDC_HAND));
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
            return TRUE;
        }
        break;

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
        Theme_SetWindowIcons(dialog, &g_manager.bigIcon, &g_manager.smallIcon);
        ReflowMain();
        FillList(&selected);
        return TRUE;
    }

    case WM_DESTROY:
        DestroyMain(dialog);
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
    WindowsMcp_UpdateEntries();   /* an update reaches each profile's Windows MCP at its next start */
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

    dialogResult = Theme_DialogBox(IDD_MAIN, NULL, MainProc, (LPARAM)start);
    if (dialogResult == -1) Util_Log(L"could not create the manager's window (error %lu)", GetLastError());
    if (mutex) CloseHandle(mutex);
    if (g_manager.readyEvent) CloseHandle(g_manager.readyEvent);
    g_manager.readyEvent = NULL;
    if (g_manager.uninstalled) Install_FinishUninstall();
    else if (g_manager.openAgain) OpenAgain();
    return dialogResult == -1 ? 1 : 0;
}
