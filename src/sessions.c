/*
 * The sessions view of the manager window: each profile's Claude Code
 * sessions. On the left the profiles, with their badge and how many sessions
 * they list; beside them, as wide as the window lets it, the chosen
 * profile's sessions as a tree (every row drawn here), or why it shows
 * none, as Claude's sidebar shows them: Pinned on top, then the sessions
 * under their headings (by day at first: Today, Yesterday, then the date),
 * and the archived ones below under Archived when every status shows. Above
 * the tree, the search box, its options (sessionsearch.c searches the
 * conversations themselves; the search may be a regular expression) and
 * the filter menu, in the order of Claude's: Group by (date, folder, state,
 * Claude's own groups, none), Sort by, Show (empty groups, pull requests),
 * then Status, the last result and Environment; the choices are kept. A
 * session's tip says where it lives, when it was used and how big its
 * conversation is. The tree's context menu starts as Claude's does (Open
 * in, Pin, Rename, Delete), then shares, copies and shows the folder.
 * Headings start open, as Claude's; those the user folds stay folded, kept
 * for the next time. Each heading shows its first five sessions, as
 * Claude's does: Show more below them shows ten more, then Show all the
 * rest (Expand all shows every heading's, a search every match). Several
 * sessions are chosen together with Ctrl and Shift (click, or Shift and the
 * arrows) or Ctrl+A, and a heading's row or Pinned stands for all its
 * sessions: their menu shares, copies (syncui.c), exports, pins or deletes
 * them all. Ctrl+C takes the sessions chosen,
 * Ctrl+V copies them to the profile shown then.
 * What they do is in sessionedit.c; colors, fonts, rows, buttons and boxes
 * come from theme.c, as everywhere.
 *
 * It follows the disk without polling: a thread waits for folder
 * notifications on each profile's session entries and config.json and on
 * the transcripts folder (while one of them is missing, on its nearest
 * existing parent, for the next folder down) and posts WM_APP_SESSIONS to the
 * window, once for a burst of them. Another thread reads the snapshot and
 * posts WM_APP_SESSIONS_READY; a notification during a read reads again a
 * moment after that read is published. A snapshot that changes nothing the
 * tree shows leaves the tree alone (no flicker, no scroll); one that does
 * keeps the selection, the folded folders and the row on top of the view in
 * its place.
 */
#include "app.h"
#include "resource.h"
#include "sessiontools.h"
#include <commctrl.h>
#include <limits.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdlib.h>
#include <string.h>
#include <windowsx.h>

#define MAX_OPENED     128
#define OPENED_SECTION L"SessionsOpened"   /* the headings the user opened, kept in settings.ini; the others start folded */
#define PLACE_SECTION  L"SessionsPlace"    /* where the tree was: its profile, the row on top (key, kind), how far above it */
#define NOTE_CCH       1024
#define NAMES_CCH      (MAX_PROFILES * (LABEL_CCH + 16))
#define WATCH_MAX      (2 * MAX_PROFILES + 1)
#define NODE_STARRED   (-1)             /* tree lParam: >= 0 a row; else Pinned, or a heading (-2 - bucket) */
#define NODE_ARCHIVED  ((LPARAM)-0x10000000)   /* the Archived part, below the headings; its headings: NODE_ARCHIVED - 1 - bucket */
#define NODE_NONE      ((LPARAM)INT_MIN)   /* no row of the tree */
#define NODE_MORE      ((LPARAM)-0x40000000)   /* a heading's Show more row: NODE_MORE - 2 * bucket, one less under Archived */
#define STARRED_KEY    L"*starred"      /* heading keys that are no path start with '*' (GroupKey, BucketKey) */
#define ARCHIVED_KEY   L"*archived"     /* the Archived part; its headings' keys: "*archived|" and theirs */
#define MORE_KEY       L"*more|"        /* a Show more row's key: this and its heading's */
#define MAX_MORE       64
#define MORE_FIRST     5                /* a heading's sessions shown at first, as Claude's sidebar shows them */
#define MORE_NEXT      10               /* those Show more adds; Show all, then, the rest */
#define IDM_FILTER     0x6000           /* the filter menu's commands (FilterCommand) */
#define IDM_FIND       0x6100           /* the search options' (FindCommand) */
#define MAX_CUSTOM_GROUPS 64            /* of Claude's own groups, the first ones */
#define SEARCH_DELAY_MS   300           /* the conversations are searched once typing pauses this long */
#define SEARCH_REFILL_MS  150           /* sessions found in the conversations show at most this often */
#define TIMER_SEARCH      0x5E01
#define TIMER_SEARCH_REFILL 0x5E02
#define IDM_ACTION     0x5000           /* menu commands: IDM_ACTION + action * MAX_PROFILES + profile */
#define SLOW_LOAD_MS   1000             /* a snapshot read slower than this is logged even when nothing changed */
#define READ_PACE_MS   500              /* the least time from a snapshot to the next one a notification asks for */
#define SETTLE_MS      300              /* notifications closer together than this are one change (Settle) */
#define SETTLE_MAX_MS  2000             /* ... read at the latest this long after the first */
#define PROFILE_ICON_DIPS      32       /* a profile's icon in the side bar */
#define SPACING_DIPS           6        /* the padding of the side bar, the tree and the details (g_view.pad) */
#define TREE_INDENT_DIPS       18       /* a tree level: a folder's arrow */
#define FOLDER_ROW_LINE_TENTHS 19       /* a folder's row: 1.9 lines of text, at least a line and the padding */
#define ARCHIVED_GLYPH         0xE7B8   /* Archive, of Windows' icon font: before an archived session's title */
#define MORE_GLYPH             0xE70D   /* ChevronDown: before Show more */
#define SESSION_GLYPH          0xE943   /* Code: before the title of a session, not archived, as Claude marks it */

/* Before the title of a session that runs elsewhere, in place of the code
 * mark (at the right end of an archived one, whose icon is the archive's):
 * where, by Windows' icon font and a tint of its own. */
#define LOCAL_GLYPH 0xE7F8   /* Laptop: a session on this PC, as Claude's sidebar marks it */
static const struct { DWORD environment; WCHAR glyph; ThemeTint tint; } kEnvironmentMarks[] = {
    { SESSION_ENV_CLOUD, 0xE753, THEME_TINT_TEAL },     /* Cloud */
    { SESSION_ENV_REMOTE, 0xE8EA, THEME_TINT_PURPLE },  /* CellPhone: steered from another device */
    { SESSION_ENV_SSH, 0xE756, THEME_TINT_GREEN },      /* CommandPrompt */
    { SESSION_ENV_SLACK, 0xE8F2, THEME_TINT_GOLD },     /* Chat */
};
#define DAY_MS                 (24ull * 60 * 60 * 1000)
/* What the filter menu chose, kept across starts (settings.ini). */
#define SETTING_SESSIONS_STATUS   L"SessionsStatus"
#define SETTING_SESSIONS_ENV      L"SessionsEnvironments"
#define SETTING_SESSIONS_GROUP    L"SessionsGrouping"
#define SETTING_SESSIONS_GROUP_BY L"SessionsGroupBy"   /* the grouping chosen while folders were the default */
#define SETTING_SESSIONS_SORT     L"SessionsSortBy"
#define SETTING_SESSIONS_EMPTY    L"SessionsEmptyGroups"
#define SETTING_SESSIONS_PRS      L"SessionsPullRequests"
#define SETTING_SESSIONS_SEARCH   L"SessionsSearch"   /* SEARCH_* bits */
#define SETTING_SESSIONS_RESULT   L"SessionsResult"
#define SETTING_SESSIONS_DESC     L"SessionsSortDescending"
#define SEARCH_TITLES   0x1u            /* titles and folders */
#define SEARCH_TEXT     0x2u            /* the conversations' text */
#define SEARCH_REGEX    0x4u
#define SEARCH_CASE     0x8u

/* Tree rows are measured in units (the tree's item height): a session is
 * SESSION_UNITS high, a folder FOLDER_UNITS, plus GAP_UNITS of room above it
 * after the first folder. */
#define SESSION_UNITS  5
#define FOLDER_UNITS   6
#define GAP_UNITS      2

#define PREVIEW_READ_MAX  (8u * 1024u * 1024u)   /* of a longer conversation, the preview shows the end */
#define PREVIEW_LINE_CCH  4096                    /* what one message shows at most */
#define PREVIEW_TEXT_CCH  (1024u * 1024u)         /* what the preview shows at most */

typedef enum Action {
    ACT_OPEN, ACT_RENAME, ACT_STAR, ACT_REMOVE, ACT_KEEP, ACT_SHARE, ACT_SHOW_FOLDER, ACT_PREVIEW, ACT_DELETE_ALL,
    ACT_TO_LOCAL, ACT_BRANCHES, ACT_READ,
    ACT_EXPORT_TEXT, ACT_EXPORT_MARKDOWN, ACT_EXPORT_HTML, ACT_EXPORT_JSON, ACT_EXPORT_ORIGINAL,   /* in READING_* order */
    ACTIONS
} Action;

/* What a menu of several sessions does to them all; and Undo, Redo. */
typedef enum BatchAction {
    BATCH_NONE, BATCH_SHARE, BATCH_EXPORT, BATCH_STAR, BATCH_REMOVE, BATCH_DELETE_ALL, BATCH_IMPORT, BATCH_TAKE, BATCH_CUT,
    BATCH_PASTE, BATCH_UNDO, BATCH_REDO, BATCH_READ
} BatchAction;
#define IDM_BATCH      0x5800           /* a menu of several sessions: IDM_BATCH + BatchAction */

/* The filter menu's choices, as Claude's sidebar offers them, in its order. */
typedef enum ViewStatus { STATUS_ACTIVE, STATUS_ARCHIVED, STATUS_ALL, STATUSES } ViewStatus;
/* Existing persisted values stay in place when a grouping is added. */
typedef enum Grouping { GROUP_DATE, GROUP_FOLDER, GROUP_STATE, GROUP_CUSTOM, GROUP_NONE, GROUP_ENVIRONMENT, GROUPINGS } Grouping;
typedef enum Sorting { SORT_NAME, SORT_CREATED, SORT_ACTIVITY, SORTINGS } Sorting;
typedef enum StateBucket { STATE_OPEN, STATE_WAITING, STATE_IDLE, STATE_BUCKETS } StateBucket;

typedef struct Bucket {                 /* a heading of the tree: a folder, a day, a state or one of Claude's groups */
    WCHAR     key[MAX_PATH];            /* what it is remembered by (folded, on top, selected), whatever the language */
    WCHAR     name[MAX_PATH];
    int       group;                    /* grouped by folder: its SessionGroup, else -1 */
    int       count[2];                 /* the sessions it shows, not archived and archived */
    int       rank;                     /* its place among the headings */
    ULONGLONG newest;                   /* by folder: its sessions' latest time, or its name sorts it */
} Bucket;

typedef struct CustomGroups {           /* Claude's own groups of a profile (its sidebar's), from its web storage */
    WCHAR  folder[FOLDER_CCH];          /* the profile they were read for; "" for none */
    int    count;
    WCHAR  id[MAX_CUSTOM_GROUPS][64];
    WCHAR  name[MAX_CUSTOM_GROUPS][LABEL_CCH];
    int    assignmentCount, assignmentCapacity;
    WCHAR (*assigned)[SESSION_ID_CCH];  /* the sessions placed in a group, by their entry's own id */
    int   *assignedGroup;
} CustomGroups;

typedef struct TreeNodeKey {            /* a row of the tree, found again after a refill */
    WCHAR key[MAX_PATH];                /* a session's key, or a folder's (GroupKey); "" for none */
    BOOL  folder;
    BOOL  inStarred;                    /* a session's row under Pinned */
} TreeNodeKey;

typedef struct WatchedFolder {          /* one folder notification of the watcher */
    WCHAR dir[MAX_PATH];
    BOOL  subtree;
    DWORD filter;
    /* For a folder watched for a deeper one or for one of its files, what a
     * notification must change to ask for a snapshot: the nearest existing
     * folder of `nearest`, and the size and time of `file` ("" for neither:
     * every notification does). */
    WCHAR nearest[MAX_PATH];
    WCHAR file[MAX_PATH];
} WatchedFolder;

typedef struct WatchPlan {
    int           count;
    WatchedFolder folder[WATCH_MAX];
} WatchPlan;

struct ShownRow;

typedef struct SessionLoader {
    SRWLOCK     lock;
    HANDLE      thread, stop, request, cancel;
    HWND        dlg;
    ProfileList profiles;               /* a snapshot from the manager's UI thread */
    ULONG       generation;             /* raised when a read in progress is out of date (RequestLoad) */
    ULONG       readyGeneration;
    SessionSet *ready;                  /* owned here until the UI takes it */
    ULONGLONG   readyTreeHash;          /* TreeContentHash of `ready` */
    ULONGLONG   readySnapshotHash;      /* SnapshotHash of `ready` */
    BOOL        urgent;                 /* a request other than a notification's waits: the next read starts at once */
    WatchPlan   plan;                   /* what the snapshot being read asks to watch (the loader thread's) */
    WatchPlan   readyPlan;              /* what `ready` asks to watch */
    BOOL        failed;
    BOOL        failureLogged;          /* the reads failing since the last one that worked are logged (the loader thread's) */
    int         loggedEntries, loggedRows, loggedGroups;   /* the last snapshot logged (the loader thread's) */
} SessionLoader;

typedef struct SessionsView {
    HWND          dlg, profiles, tree, search, findButton, filterButton;
    HWND          treeArea;             /* the view the tree scrolls in */
    BOOL          shown, loaded, filling;
    BOOL          searchHidden;         /* Ctrl+F hid the search box and its options */
    BOOL          warmed, dirty, rendered, loading, loadFailed, accepting;
    int           actionDepth;
    ProfileList   profilesToRead;       /* the profiles as the manager last gave them, for the next read */
    SessionLoader *loader;
    SessionSet    set;
    ULONGLONG     treeHash;             /* what the tree shows of `set` (TreeContentHash) */
    ULONGLONG     snapshotHash;         /* all `set` holds (SnapshotHash) */
    const ClaudePackage *pkg;           /* the manager's, which it keeps current */
    WCHAR         filter[SESSION_SEARCH_CCH];
    /* The filter menu and the search options (kept: LoadChoices). */
    ViewStatus    status;
    DWORD         environments;         /* SESSION_ENV_* shown; 0: every one */
    Grouping      grouping;
    Sorting       sorting;
    BOOL          descending;
    BOOL          showEmptyGroups, showPullRequests;
    DWORD         searchIn;             /* SEARCH_* */
    WCHAR         modelFilter[128], effortFilter[64];
    int           resultFilter;         /* 0 all; 1..SESSION_RESULTS selects a recorded result, including unknown */
    CoreRegex    *regex;                /* the search as a regular expression, compiled; NULL when not one */
    /* The search in the conversations (sessionsearch.c): what it found, by key. */
    ULONG         textSearch;           /* the search under way's number */
    BOOL          textSearching, textRefillPending, textRefreshPending;
    WCHAR       (*textKeys)[SESSION_ID_CCH];   /* the sessions searched, by the index the search posts */
    int           textKeyCount;
    WCHAR       (*textFound)[SESSION_ID_CCH];
    int           textFoundCount;
    CustomGroups  custom;
    TreeNodeKey   selection;            /* the tree's selected row: its session is the one in the details */
    BOOL          keepPlace;            /* the next refill keeps the view where it is (RememberTopRow) */
    TreeNodeKey   topRow;               /* the row then on top of the view ("" at the very top) */
    int           topRowOffset;         /* how far it was above the view's top, px */
    TreeNodeKey   topRowNext;           /* the row below it: on top instead when it is gone */
    /* The sessions just below and above the selected one when the tree was
     * last filled: the selection moves to one of them when its session goes.
     * Taken before the snapshot the tree was filled from is replaced, the
     * tree's rows then naming that snapshot's rows (RememberNeighbors). */
    TreeNodeKey   neighbor[2];
    BOOL          neighborsTaken;
    BOOL          folderLookedUp;       /* selectedFolder is current for the working folder below */
    WCHAR         folderLookedUpFor[MAX_PATH];   /* that working folder */
    WCHAR         selectedFolder[MAX_PATH];   /* the selected session's folder for Explorer; "" when not on disk */
    int           sessionCount[MAX_PROFILES];   /* the sessions each profile lists, as the side bar counts them */
    Bucket       *buckets;              /* the tree's headings, as last filled */
    int           bucketCount, bucketCapacity, starredCount, archivedCount;
    WCHAR         folder[FOLDER_CCH];   /* the profile shown */
    HIMAGELIST    icons;                /* profile icons for the side bar */
    int           iconSize, pad, indent, unit;
    ThemeFonts    fonts;
    int           fontHeight[THEME_FONTS], fontAscent[THEME_FONTS];
    UINT          measuredDpi;
    HFONT         measuredFont;
    int           measuredLanguage;
    ULONGLONG     imageListHash;        /* what the icons were made for */
    HANDLE        watchThread, watchStop;
    int           watchPaused;          /* SessionsView_PauseWatching calls not resumed yet: no watcher meanwhile */
    WatchPlan     watched;              /* what the watcher thread watches */
    WatchPlan     planned;              /* what the snapshot shown asks to watch */
    WCHAR         opened[MAX_OPENED][MAX_PATH];   /* headings the user opened, by their key, kept; the others start folded */
    int           openedCount;
    WCHAR         renderedFolder[FOLDER_CCH];     /* the profile the tree was filled for */
    BOOL          placeRestored;                  /* the place kept from the last run was put back */
    /* Headings shown further than their first sessions, by their key, while
     * the profile stays shown: once with Show more, twice (all of them). */
    WCHAR         more[MAX_MORE][MAX_PATH];
    BYTE          moreStage[MAX_MORE];
    int           moreCount;
    /* Where each profile's tree was and how much of it was shown, kept while
     * the view is open, so switching profiles and back shows it as it was. */
    struct ProfilePlace {
        WCHAR       folder[FOLDER_CCH];
        BOOL        used;
        TreeNodeKey top, next;
        int         offset;
        WCHAR       more[MAX_MORE][MAX_PATH];
        BYTE        moreStage[MAX_MORE];
        int         moreCount;
    }            *places;
    int           placeCount;
    ULONGLONG     moreClicked;          /* when and where Show more was last clicked (GetTickCount64, the tree's) */
    POINT         moreClickedAt;
    BOOL          onlyShown;            /* the menu's choice was made with Shift, Ctrl or Alt: the profile shown alone, not its sync group */
    BOOL          expandHeld;           /* the tree's drawing held while a folder opens or closes (HoldTreeDrawing) */
    struct ShownRow *rowsShown;         /* the sessions of the last fill, sorted: what headings stand for (FolderRows) */
    int           rowsShownCount, rowsShownTop;   /* rowsShownTop: where the archived part starts */
    /* Sessions chosen together (Ctrl, Shift, Ctrl+A), by key; while `several`
     * the tree shows them as selected instead of its own selection, which
     * keeps the keyboard's place. */
    BOOL          several;
    BOOL          marking;              /* the tree's selection moves with the marks: they stay */
    WCHAR       (*marked)[SESSION_ID_CCH];
    int           markedCount, markedCapacity;
    TreeNodeKey   anchor;               /* where a Shift range starts */
    /* The sessions Ctrl+C (or Ctrl+X: `takenCut`) took, by key, and the profile they were taken in. */
    WCHAR       (*taken)[SESSION_ID_CCH];
    int           takenCount;
    WCHAR         takenFrom[FOLDER_CCH];
    BOOL          takenCut;
    /* Sessions dragged out of the tree (DragSessions), by key, from the profile shown then; the side bar's row they
     * are over (`hot`, files too) and the one they were let go on (`target`), with Shift (`move`). */
    struct {
        BOOL    active;
        WCHAR (*keys)[SESSION_ID_CCH];
        int     count;
        WCHAR   from[FOLDER_CCH];
        int     hot, target;
        BOOL    move;
        BOOL    pressKept;              /* a plain press on a session among those chosen kept them, for a drag */
        BOOL    began;                  /* that press began a drag */
    } drag;
    BOOL          sideTarget;           /* the side bar takes drops (SessionsView_AcceptDrops) */
    ULONGLONG     waitingChangesTried[MAX_PROFILES];   /* each profile's waiting changes as last tried in vain (WaitingChangesState) */
    HANDLE        waitingThread;        /* the thread making waiting changes (StartWaitingChanges), NULL when none runs */
    struct WaitingWork *waitingWork;    /* what it makes */
} SessionsView;

static SessionsView g_view;
static const WCHAR *ResultName(SessionResult result);
static volatile LONG g_reloadMessagePending;   /* a WM_APP_SESSIONS is posted and no snapshot asked for since */

static const int kControls[] = { IDC_S_PROFILES, IDC_S_FOLD, IDC_S_SEARCH, IDC_S_FIND, IDC_S_FILTER, IDC_S_TREE };

static void RequestLoad(BOOL restart);

/* ------------------------------------------------------------------ rows */

static int ShownProfileIndex(void)
{
    int i = Profiles_Find(&g_view.set.profiles, g_view.folder);
    return i >= 0 ? i : (g_view.set.profiles.count > 0 ? 0 : -1);
}

static const Profile *ProfileAt(int p)
{
    return &g_view.set.profiles.items[p];
}

/* The manager's Claude package, which it keeps current; none before the
 * view first shows. */
static const ClaudePackage *Package(void)
{
    static const ClaudePackage none = { 0 };
    return g_view.pkg ? g_view.pkg : &none;
}

static const SessionEntry *EntryOf(const SessionRow *row, int profile)
{
    return profile >= 0 && row->entry[profile] >= 0 ? &g_view.set.entries[row->entry[profile]] : NULL;
}

/* A session's title in a profile, a new one waiting included; "" when it
 * has none anywhere. */
static const WCHAR *TitleIn(const SessionRow *row, int profile)
{
    const SessionEntry *entry = EntryOf(row, profile);
    if (entry && entry->pendingTitle[0]) return entry->pendingTitle;
    return entry && entry->title[0] ? entry->title : SessionStore_RowTitle(&g_view.set, row);
}

/* The same as it is shown: an untitled session says so. */
static const WCHAR *ShownTitle(const SessionRow *row, int profile)
{
    const WCHAR *title = TitleIn(row, profile);
    return title[0] ? title : TR(L"Untitled session");
}

/* Pinned in a profile (Claude's isStarred), a change waiting included. */
static BOOL StarredIn(const SessionEntry *entry)
{
    return entry && (entry->pendingStar >= 0 ? entry->pendingStar == 1 : entry->starred);
}

/* Listed, and not about to be removed: a removal waiting for the profile to
 * close leaves nothing to open, rename or remove there. */
static BOOL ListedAndKept(const SessionEntry *entry)
{
    return entry && !entry->pendingRemove;
}

/* Unread in Claude's sidebar, or so once a change waiting is made. */
static BOOL UnreadIn(const SessionEntry *entry)
{
    return entry && (entry->pendingUnread >= 0 ? entry->pendingUnread != 0 : entry->unread);
}

/* `text` holds the search: as a regular expression, or as typed, its case
 * as the search options say. */
static BOOL TextHasSearch(const WCHAR *text)
{
    if (g_view.searchIn & SEARCH_REGEX) return g_view.regex && Core_RegexSearch(g_view.regex, text, wcslen(text));
    return (g_view.searchIn & SEARCH_CASE) ? wcsstr(text, g_view.filter) != NULL : Core_ContainsI(text, g_view.filter);
}

/* The search in the conversations found session `key`. */
static BOOL FoundInText(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_view.textFoundCount; i++)
        if (Core_EqualsI(g_view.textFound[i], key)) return TRUE;
    return FALSE;
}

/* The search matches what the tree shows (the title, the folder's name or
 * the working folder) or, with the conversations searched, their text. */
static BOOL Matches(const SessionRow *row, int profile)
{
    WCHAR name[MAX_PATH];
    if (!g_view.filter[0]) return TRUE;
    if ((g_view.searchIn & SEARCH_TEXT) && FoundInText(row->key)) return TRUE;
    if (!(g_view.searchIn & SEARCH_TITLES)) return FALSE;
    SessionStore_GroupName(&g_view.set, row->group, name, ARRAYSIZE(name));
    {
        const SessionEntry *entry = EntryOf(row, profile);
        const WCHAR *title;
        for (title = entry ? entry->previousTitles : NULL; title && *title; title += wcslen(title) + 1)
            if (TextHasSearch(title)) return TRUE;
    }
    return TextHasSearch(ShownTitle(row, profile)) || TextHasSearch(name) || TextHasSearch(row->cwd);
}

/* An entry in the status and the environments the filter menu chose. */
static BOOL EntryChosen(const SessionEntry *entry)
{
    if (!entry || (g_view.status == STATUS_ACTIVE && entry->archived) || (g_view.status == STATUS_ARCHIVED && !entry->archived)) return FALSE;
    if (g_view.resultFilter && entry->result != (SessionResult)(g_view.resultFilter - 1)) return FALSE;
    if (g_view.modelFilter[0] && !Core_EqualsI(entry->model, g_view.modelFilter)) return FALSE;
    if (g_view.effortFilter[0] && !Core_EqualsI(entry->effort, g_view.effortFilter)) return FALSE;
    return !g_view.environments || (entry->environment & g_view.environments) != 0;
}

/* Shown in the profile: listed there, chosen by the filter menu, found by the search. */
static BOOL RowVisibleIn(const SessionRow *row, int profile)
{
    return EntryChosen(EntryOf(row, profile)) && Matches(row, profile);
}

static int SelectedRow(void)
{
    return g_view.selection.key[0] && !g_view.selection.folder ? SessionStore_FindRow(&g_view.set, g_view.selection.key) : -1;
}

/* ----------------------------------------------------------------- marks */

static int MarkIndex(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_view.markedCount; i++)
        if (Core_EqualsI(g_view.marked[i], key)) return i;
    return -1;
}

static BOOL IsMarked(const WCHAR *key)
{
    return g_view.several && MarkIndex(key) >= 0;
}

/* Tree node `node` is a session among those chosen together. */
static BOOL NodeChosen(LPARAM node)
{
    return node >= 0 && node < g_view.set.rowCount && IsMarked(g_view.set.rows[node].key);
}

/* Session `key` among the marks (`on`), or out of them. */
static void Mark(const WCHAR *key, BOOL on)
{
    int i = MarkIndex(key);
    if (!on) {
        if (i >= 0 && i != --g_view.markedCount)   /* the last one takes its place: the order does not matter */
            memcpy(g_view.marked[i], g_view.marked[g_view.markedCount], sizeof *g_view.marked);
        return;
    }
    if (i >= 0 || !key[0]) return;
    if (g_view.markedCount == g_view.markedCapacity) {
        int larger = g_view.markedCapacity ? g_view.markedCapacity * 2 : 64;
        WCHAR (*grown)[SESSION_ID_CCH] = (WCHAR (*)[SESSION_ID_CCH])(g_view.marked
            ? HeapReAlloc(GetProcessHeap(), 0, g_view.marked, (size_t)larger * sizeof *g_view.marked)
            : HeapAlloc(GetProcessHeap(), 0, (size_t)larger * sizeof *g_view.marked));
        if (!grown) return;
        g_view.marked = grown;
        g_view.markedCapacity = larger;
    }
    StringCchCopyW(g_view.marked[g_view.markedCount++], SESSION_ID_CCH, key);
}

/* Back to the tree's own selection alone. */
static void ClearMarks(void)
{
    if (!g_view.several && !g_view.markedCount) return;
    g_view.several = FALSE;
    g_view.markedCount = 0;
    if (g_view.tree) InvalidateRect(g_view.tree, NULL, FALSE);
}

/* The sessions marked, as rows of the snapshot shown: a heap array
 * (HeapFree) and its length; NULL for none. */
static int *MarkedRows(int *count)
{
    int *rows, i, r;
    *count = 0;
    if (!g_view.several || !g_view.markedCount) return NULL;
    if ((rows = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)g_view.markedCount * sizeof *rows)) == NULL) return NULL;
    for (i = 0; i < g_view.markedCount; i++)
        if ((r = SessionStore_FindRow(&g_view.set, g_view.marked[i])) >= 0) rows[(*count)++] = r;
    if (!*count) {
        HeapFree(GetProcessHeap(), 0, rows);
        return NULL;
    }
    return rows;
}

/* What a folder of the tree is remembered by (folded, on top, selected),
 * whatever the language and the profiles' names: its path, else its owner's
 * folder for a profile's "no folder". */
static void GroupKey(int group, WCHAR *out, size_t cch)
{
    const SessionGroup *sessionGroup = &g_view.set.groups[group];
    if (sessionGroup->scratchOf >= 0) StringCchPrintfW(out, cch, L"*no folder:%s", ProfileAt(sessionGroup->scratchOf)->folder);
    else if (sessionGroup->path[0]) StringCchCopyW(out, cch, sessionGroup->path);
    else StringCchCopyW(out, cch, L"*unknown");
}

/* The name of the Pinned heading, Claude's, in the current language. */
static const WCHAR *StarredName(void)
{
    return TR(L"Pinned");
}

/* A heading's Show more row, below the last of its sessions shown, at the
 * top or under Archived. */
static LPARAM MoreNode(int bucket, BOOL underArchived)
{
    return NODE_MORE - 2 * (LPARAM)bucket - (underArchived ? 1 : 0);
}

static BOOL IsMoreNode(LPARAM node)
{
    return node <= NODE_MORE && node > NODE_MORE - 0x10000000;
}

/* The heading node of Show more row `node`. */
static LPARAM MoreHeading(LPARAM node)
{
    LPARAM offset = NODE_MORE - node;
    int bucket = (int)(offset / 2);
    return (offset & 1) ? NODE_ARCHIVED - 1 - bucket : -2 - bucket;
}

/* The bucket of a heading's node, at the top or under Archived; -1 for any
 * other node. */
static int NodeBucket(LPARAM node)
{
    if (node >= 0 || node == NODE_STARRED || node == NODE_ARCHIVED || node == NODE_NONE || IsMoreNode(node)) return -1;
    return node < NODE_ARCHIVED ? (int)(NODE_ARCHIVED - 1 - node) : (int)(-2 - node);
}

static BOOL UnderArchived(LPARAM node)
{
    return node < NODE_ARCHIVED && node != NODE_NONE && !IsMoreNode(node);
}

/* Archived in the profile shown: it shows under Archived only. */
static BOOL RowArchived(const SessionRow *row, int profile)
{
    const SessionEntry *entry = EntryOf(row, profile);
    return entry && entry->archived;
}

/* The profiles that list session `row`, one bit each. */
static DWORD ProfilesListing(const SessionRow *row)
{
    DWORD bits = 0;
    int p;
    for (p = 0; p < g_view.set.profiles.count; p++)
        if (row->entry[p] >= 0) bits |= 1u << p;
    return bits;
}

/* The profiles other than `except` that keep session `row`: they list it
 * and no removal of it waits there. */
static DWORD ProfilesKeeping(const SessionRow *row, int except)
{
    DWORD bits = 0;
    int p;
    for (p = 0; p < g_view.set.profiles.count; p++)
        if (p != except && ListedAndKept(EntryOf(row, p))) bits |= 1u << p;
    return bits;
}

/* What the tree shows of a set: its profiles and why one lists nothing, its
 * rows in their order with what titles them, what the search reads, their
 * times, states, stars, archives, changes waiting, environments and pull
 * requests, and their folders. A snapshot
 * that keeps it only redraws the side bar and the details. Pure: the loader
 * thread computes it. */
static ULONGLONG TreeContentHash(const SessionSet *set)
{
    ULONGLONG hash = CORE_HASH_START;
    int i, p;
    hash = Core_HashBytes(hash, &set->profiles.count, sizeof set->profiles.count);
    for (i = 0; i < set->profiles.count; i++) {
        const Profile *profile = &set->profiles.items[i];
        const SessionSource *source = &set->source[i];
        int reasons[3] = { source->signedIn, source->unreadable, source->elsewhere };
        hash = Core_HashText(hash, profile->folder);
        hash = Core_ProfileLookHash(hash, profile);
        hash = Core_HashBytes(hash, reasons, sizeof reasons);
    }
    hash = Core_HashBytes(hash, &set->rowCount, sizeof set->rowCount);
    for (i = 0; i < set->rowCount; i++) {
        const SessionRow *row = &set->rows[i];
        hash = Core_HashText(hash, row->key);
        hash = Core_HashText(hash, row->cwd);
        hash = Core_HashText(hash, SessionStore_RowTitle(set, row));
        hash = Core_HashBytes(hash, &row->group, sizeof row->group);
        hash = Core_HashBytes(hash, &row->transcript, sizeof row->transcript);
        hash = Core_HashBytes(hash, &row->lastActivity, sizeof row->lastActivity);   /* sorts it, and its day */
        hash = Core_HashBytes(hash, &row->live, sizeof row->live);                   /* its state */
        for (p = 0; p < set->profiles.count; p++) {
            const SessionEntry *entry = row->entry[p] >= 0 ? &set->entries[row->entry[p]] : NULL;
            int flags[9] = { entry != NULL, entry && entry->starred, entry && entry->archived, entry ? entry->pendingStar : -1,
                             entry && entry->pendingRemove, entry && entry->pending, entry ? (int)entry->environment : 0,
                             entry ? (int)entry->pullRequest : 0, entry ? entry->pullRequestNumber : 0 };
            hash = Core_HashBytes(hash, flags, sizeof flags);
            if (entry) {
                hash = Core_HashText(hash, entry->title);
                hash = Core_HashText(hash, entry->model);
                hash = Core_HashText(hash, entry->effort);
                {
                    const WCHAR *old;
                    for (old = entry->previousTitles; old && *old; old += wcslen(old) + 1) hash = Core_HashText(hash, old);
                }
                hash = Core_HashText(hash, entry->pendingTitle);
                hash = Core_HashBytes(hash, &entry->result, sizeof entry->result);
                hash = Core_HashText(hash, entry->resultDetail);
                hash = Core_HashBytes(hash, &entry->created, sizeof entry->created);
            }
        }
    }
    hash = Core_HashBytes(hash, &set->groupCount, sizeof set->groupCount);
    for (i = 0; i < set->groupCount; i++) {
        hash = Core_HashText(hash, set->groups[i].name);
        hash = Core_HashText(hash, set->groups[i].path);
        hash = Core_HashBytes(hash, &set->groups[i].scratchOf, sizeof set->groups[i].scratchOf);
    }
    return hash;
}

/* All a snapshot holds, from `treeHash` (its TreeContentHash): a snapshot
 * that keeps it changes nothing shown, so the one shown stays as drawn.
 * Whole structures, their unused bytes included: the store zeroes what it
 * fills, and one that differs only there is shown as a change. Pure: the
 * loader thread computes it. */
static ULONGLONG SnapshotHash(const SessionSet *set, ULONGLONG treeHash)
{
    ULONGLONG hash = treeHash;
    int i;
    hash = Core_HashBytes(hash, set->profiles.items, (size_t)set->profiles.count * sizeof *set->profiles.items);
    hash = Core_HashBytes(hash, set->source, (size_t)set->profiles.count * sizeof *set->source);
    hash = Core_HashBytes(hash, &set->noTranscripts, sizeof set->noTranscripts);
    hash = Core_HashBytes(hash, set->rows, (size_t)set->rowCount * sizeof *set->rows);
    hash = Core_HashBytes(hash, set->groups, (size_t)set->groupCount * sizeof *set->groups);
    hash = Core_HashBytes(hash, set->otherTranscriptIds, (size_t)set->otherTranscriptCount * sizeof *set->otherTranscriptIds);
    for (i = 0; i < set->entryCount; i++) {
        SessionEntry entry = set->entries[i];
        const WCHAR *title;
        entry.previousTitles = NULL;   /* a pointer: what it points to is hashed */
        hash = Core_HashBytes(hash, &entry, sizeof entry);
        for (title = set->entries[i].previousTitles; title && *title; title += wcslen(title) + 1) hash = Core_HashText(hash, title);
    }
    return hash;
}

/* ---------------------------------------------------------- measurements */

static HFONT DialogFont(void)
{
    return (HFONT)SendMessageW(g_view.dlg, WM_GETFONT, 0, 0);
}

static HFONT Font(ThemeFont role)
{
    return g_view.fonts.font[role] ? g_view.fonts.font[role] : DialogFont();
}

/* Each font's height and ascent, the details' control height (a button's)
 * measured once for the window's scale and language, read by every paint. */
static void MeasureFonts(void)
{
    TEXTMETRICW metrics;
    HDC dc = GetDC(g_view.dlg);
    HGDIOBJ old;
    int role;
    if (!dc) return;
    old = SelectObject(dc, DialogFont());
    for (role = 0; role < THEME_FONTS; role++) {
        SelectObject(dc, Font((ThemeFont)role));
        GetTextMetricsW(dc, &metrics);
        g_view.fontHeight[role] = metrics.tmHeight;
        g_view.fontAscent[role] = metrics.tmAscent;
    }
    SelectObject(dc, DialogFont());
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old);
    ReleaseDC(g_view.dlg, dc);
}

static int LineHeight(ThemeFont role)
{
    return g_view.fontHeight[role];
}

static int TextWidth(HDC dc, ThemeFont role, const WCHAR *text)
{
    SIZE extent = { 0, 0 };
    HGDIOBJ old = SelectObject(dc, Font(role));
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &extent);
    SelectObject(dc, old);
    return extent.cx;
}

/* An image appended to a list of the profiles' images (image p: profile
 * p): a blank one when the profile's icon could not be made, so that the
 * next profiles' images keep their index. */
static void AddProfileImage(HIMAGELIST list, HICON icon, int size)
{
    BITMAPINFO info;
    void *bits = NULL;
    HBITMAP blank;
    if (!list || (icon && ImageList_AddIcon(list, icon) >= 0)) return;
    ZeroMemory(&info, sizeof info);
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = size;
    info.bmiHeader.biHeight = size;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    if ((blank = CreateDIBSection(NULL, &info, DIB_RGB_COLORS, &bits, NULL, 0)) == NULL) return;
    ZeroMemory(bits, (size_t)size * (size_t)size * 4);   /* transparent */
    ImageList_Add(list, blank, NULL);
    DeleteObject(blank);
}

/* Fonts, icon sizes and row heights for the window's scale. Where the
 * controls go is Theme_LayoutMain's. */
static void Measure(void)
{
    UINT dpi = GetDpiForWindow(g_view.dlg);
    HFONT baseFont = DialogFont();
    int language = Localize_EffectiveLanguage();
    ULONGLONG imageListHash;
    int line, folderRow, i;
    if (!g_view.fonts.font[THEME_FONT_TEXT] || g_view.measuredDpi != dpi || g_view.measuredFont != baseFont || g_view.measuredLanguage != language) {
        Theme_CreateFonts(g_view.dlg, &g_view.fonts);
        g_view.measuredDpi = dpi;
        g_view.measuredFont = baseFont;
        g_view.measuredLanguage = language;
    }
    g_view.iconSize = MulDiv(PROFILE_ICON_DIPS, (int)dpi, 96);
    g_view.pad = MulDiv(SPACING_DIPS, (int)dpi, 96);
    MeasureFonts();
    line = LineHeight(THEME_FONT_TEXT);
    SendMessageW(g_view.search, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search this profile's sessions"));
    SendMessageW(g_view.profiles, LB_SETITEMHEIGHT, 0, MAKELPARAM(max(g_view.iconSize, 2 * line) + 2 * g_view.pad, 0));
    folderRow = max(line * FOLDER_ROW_LINE_TENTHS / 10, LineHeight(THEME_FONT_STRONG) + g_view.pad);
    g_view.unit = max(1, folderRow / FOLDER_UNITS);
    TreeView_SetItemHeight(g_view.tree, g_view.unit);
    TreeView_SetIndent(g_view.tree, MulDiv(TREE_INDENT_DIPS, (int)dpi, 96));
    g_view.indent = (int)TreeView_GetIndent(g_view.tree);
    Theme_SetScrollRow(g_view.tree, SESSION_UNITS * g_view.unit);   /* a wheel line: a session */
    imageListHash = Core_HashBytes(CORE_HASH_START, &dpi, sizeof dpi);
    imageListHash = Core_HashBytes(imageListHash, &g_view.iconSize, sizeof g_view.iconSize);
    imageListHash = Core_HashText(imageListHash, Package()->exe);
    imageListHash = Core_HashBytes(imageListHash, &g_view.set.profiles.count, sizeof g_view.set.profiles.count);
    for (i = 0; i < g_view.set.profiles.count; i++) {
        imageListHash = Core_ProfileLookHash(imageListHash, ProfileAt(i));
    }
    if (g_view.icons && imageListHash == g_view.imageListHash) return;
    g_view.imageListHash = imageListHash;
    if (g_view.icons) ImageList_Destroy(g_view.icons);
    g_view.icons = ImageList_Create(g_view.iconSize, g_view.iconSize, ILC_COLOR32, g_view.set.profiles.count, 1);
    for (i = 0; i < g_view.set.profiles.count; i++) {
        HICON icon = Icons_Create(Package(), ProfileAt(i), g_view.iconSize);
        AddProfileImage(g_view.icons, icon, g_view.iconSize);
        if (icon) DestroyIcon(icon);
    }
}

/* ------------------------------------------------------------------ text */

/* `text` in `rc` (one line, cut with an ellipsis), vertically centered. */
static void TextLine(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, RECT *rc, UINT extra)
{
    HGDIOBJ old = SelectObject(dc, Font(role));
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, rc, DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | DT_END_ELLIPSIS | extra |
              ((extra & DT_PATH_ELLIPSIS) ? 0 : Localize_ReadingFlags()));
    SelectObject(dc, old);
}

/* `text` from x to at most `right`, its baseline on `baseline` (texts of two
 * sizes on one line share it). Returns where it ended. */
static int TextAt(HDC dc, ThemeFont role, COLORREF color, const WCHAR *text, int x, int right, int baseline)
{
    RECT r;
    HGDIOBJ old = SelectObject(dc, Font(role));
    r.left = x;
    r.right = min(right, x + TextWidth(dc, role, text));
    r.top = baseline - g_view.fontAscent[role];
    r.bottom = r.top + g_view.fontHeight[role];
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | Localize_ReadingFlags());
    SelectObject(dc, old);
    return r.right;
}

/* A baseline for text of `role` centered in `rc`. */
static int CenteredBaseline(const RECT *rc, ThemeFont role)
{
    return rc->top + (rc->bottom - rc->top - g_view.fontHeight[role]) / 2 + g_view.fontAscent[role];
}

/* -------------------------------------------------------------- profiles */

/* How many sessions each profile lists, as the side bar shows them: those
 * in the status and the environments the filter menu chose. */
static void CountSessions(void)
{
    int r, p;
    ZeroMemory(g_view.sessionCount, sizeof g_view.sessionCount);
    for (r = 0; r < g_view.set.rowCount; r++)
        for (p = 0; p < g_view.set.profiles.count; p++) {
            const SessionEntry *entry = EntryOf(&g_view.set.rows[r], p);
            if (EntryChosen(entry)) g_view.sessionCount[p]++;
        }
}

/* The side bar filled again: item i is profile i, by its name (screen
 * readers read it, typing selects it), up to one that cannot be added.
 * Selected while drawing is off, the profile shown is not scrolled to: a
 * refill keeps the side bar's place (SessionsView_Enter shows it). */
static void FillProfiles(void)
{
    int i, pick = ShownProfileIndex();
    SendMessageW(g_view.profiles, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_view.profiles, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_view.set.profiles.count; i++)
        if (SendMessageW(g_view.profiles, LB_ADDSTRING, 0, (LPARAM)ProfileAt(i)->name) < 0) break;
    SendMessageW(g_view.profiles, LB_SETCURSEL, (WPARAM)pick, 0);
    SendMessageW(g_view.profiles, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_view.profiles, NULL, TRUE);
}

/* Icon, name, then whether it is open and how many sessions it lists, in
 * gray, on the window's background (a side bar: no frame). */
static void DrawProfile(const DRAWITEMSTRUCT *item)
{
    WCHAR second[64];
    ThemeBuffer buffer;
    RECT rc = item->rcItem, line;
    UINT state = (item->itemState & ODS_SELECTED) ? THEME_ROW_SELECTED : 0;
    COLORREF text;
    HDC dc;
    int p = (int)item->itemID, lineHeight = LineHeight(THEME_FONT_TEXT), count, top;
    if (item->itemID == (UINT)-1 || p < 0 || p >= g_view.set.profiles.count) return;
    if (p == g_view.drag.hot) state |= THEME_ROW_HOT;   /* what is dragged goes here if let go */
    dc = Theme_BufferBegin(&buffer, item->hDC, &rc);
    text = Theme_DrawRow(item->hwndItem, dc, &rc, state, Theme_Color(THEME_FACE));
    if (g_view.icons) ImageList_Draw(g_view.icons, p, dc, rc.left + g_view.pad, rc.top + (rc.bottom - rc.top - g_view.iconSize) / 2, ILD_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    top = rc.top + (rc.bottom - rc.top - 2 * lineHeight) / 2;
    SetRect(&line, rc.left + g_view.pad * 2 + g_view.iconSize, top, rc.right - g_view.pad, top + lineHeight);
    TextLine(dc, THEME_FONT_STRONG, text, ProfileAt(p)->name, &line, 0);
    count = g_view.sessionCount[p];
    if (!g_view.loaded && g_view.loading)
        StringCchCopyW(second, ARRAYSIZE(second), TR(L"Loading sessions\x2026"));
    else if (!g_view.loaded)   /* not read: no count to give */
        StringCchCopyW(second, ARRAYSIZE(second), ProfileAt(p)->running ? TR(L"open") : L"");
    else
        StringCchPrintfW(second, ARRAYSIZE(second), count == 1 ? TR(L"%s%d session") : TR(L"%s%d sessions"),
                         ProfileAt(p)->running ? TR(L"open \x00B7 ") : L"", count);
    OffsetRect(&line, 0, lineHeight);
    TextLine(dc, THEME_FONT_TEXT, Theme_RowMuted(state), second, &line, 0);
    if (item->itemState & ODS_FOCUS) Theme_DrawFocusCue(item->hwndItem, dc, &rc);
    Theme_BufferEnd(&buffer);
}

/* ------------------------------------------------------------------ tree */

/* Headings start folded; one the user opened stays open, the next time too. */
static BOOL WasFolded(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_view.openedCount; i++)
        if (Core_EqualsI(g_view.opened[i], key)) return FALSE;
    return TRUE;
}

/* A heading folded or opened by the user, kept for the next time: the
 * latest opened last, the oldest forgotten first when there are too many. */
static BOOL RecordFolded(const WCHAR *key, BOOL folded)
{
    int i;
    for (i = 0; i < g_view.openedCount && !Core_EqualsI(g_view.opened[i], key); i++) {}
    if (!folded && i < g_view.openedCount) return FALSE;
    if (folded && i == g_view.openedCount) return FALSE;
    if (i < g_view.openedCount || g_view.openedCount == MAX_OPENED) {   /* out of the list: the rest move up */
        if (i == g_view.openedCount) i = 0;
        MoveMemory(g_view.opened[i], g_view.opened[i + 1], (size_t)(g_view.openedCount - i - 1) * sizeof g_view.opened[0]);
        g_view.openedCount--;
    }
    if (!folded) StringCchCopyW(g_view.opened[g_view.openedCount++], ARRAYSIZE(g_view.opened[0]), key);
    return TRUE;
}

static void KeepFolded(void)
{
    if (!Util_SetSettingList(OPENED_SECTION, (const WCHAR (*)[MAX_PATH])g_view.opened, g_view.openedCount))
        Util_Log(L"sessions: the headings opened could not be kept (error %lu)", GetLastError());
}

static void SetFolded(const WCHAR *key, BOOL folded)
{
    if (RecordFolded(key, folded)) KeepFolded();
}

static LPARAM NodeParam(HTREEITEM item)
{
    TVITEMW query;
    ZeroMemory(&query, sizeof query);
    query.mask = TVIF_PARAM;
    query.hItem = item;
    return item && TreeView_GetItem(g_view.tree, &query) ? query.lParam : NODE_NONE;
}

/* The key of tree node `node`; `inStarred`: a session's row under Pinned. */
static void NodeKey(LPARAM node, BOOL inStarred, TreeNodeKey *out)
{
    int bucket = NodeBucket(node);
    out->key[0] = 0;
    out->folder = node < 0;
    out->inStarred = node >= 0 && inStarred;
    if (IsMoreNode(node)) {
        TreeNodeKey heading;
        NodeKey(MoreHeading(node), FALSE, &heading);
        if (heading.key[0]) StringCchPrintfW(out->key, ARRAYSIZE(out->key), MORE_KEY L"%s", heading.key);
    } else if (node == NODE_STARRED) StringCchCopyW(out->key, ARRAYSIZE(out->key), STARRED_KEY);
    else if (node == NODE_ARCHIVED) StringCchCopyW(out->key, ARRAYSIZE(out->key), ARCHIVED_KEY);
    else if (bucket >= 0 && bucket < g_view.bucketCount) {
        if (UnderArchived(node)) StringCchPrintfW(out->key, ARRAYSIZE(out->key), ARCHIVED_KEY L"|%s", g_view.buckets[bucket].key);
        else StringCchCopyW(out->key, ARRAYSIZE(out->key), g_view.buckets[bucket].key);
    }
    else if (node >= 0 && node < g_view.set.rowCount) StringCchCopyW(out->key, ARRAYSIZE(out->key), g_view.set.rows[node].key);
}

static void ItemKey(HTREEITEM item, TreeNodeKey *out)
{
    NodeKey(NodeParam(item), NodeParam(TreeView_GetParent(g_view.tree, item)) == NODE_STARRED, out);
}

/* How far heading `key` shows its sessions: 0 its first ones, 1 ten more,
 * 2 all of them. */
static int MoreStage(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_view.moreCount; i++)
        if (Core_EqualsI(g_view.more[i], key)) return g_view.moreStage[i];
    return 0;
}

/* Heading `key` shown as far as `stage`; the earliest forgotten first when
 * there are too many. */
static void SetMoreStage(const WCHAR *key, int stage)
{
    int i;
    for (i = 0; i < g_view.moreCount && !Core_EqualsI(g_view.more[i], key); i++) {}
    if (i == g_view.moreCount) {
        if (g_view.moreCount == MAX_MORE) {
            MoveMemory(g_view.more[0], g_view.more[1], (size_t)(MAX_MORE - 1) * sizeof g_view.more[0]);
            MoveMemory(g_view.moreStage, g_view.moreStage + 1, (size_t)(MAX_MORE - 1) * sizeof g_view.moreStage[0]);
            i = --g_view.moreCount;
        }
        StringCchCopyW(g_view.more[i], ARRAYSIZE(g_view.more[0]), key);
        g_view.moreCount++;
    }
    g_view.moreStage[i] = (BYTE)min(stage, 2);
}

/* What Show more row `node` says: Show more, and once shown more, Show all. */
static const WCHAR *MoreLabel(LPARAM node)
{
    TreeNodeKey heading;
    NodeKey(MoreHeading(node), FALSE, &heading);
    return MoreStage(heading.key) ? TR(L"Show all") : TR(L"Show more");
}

static BOOL SameNode(const TreeNodeKey *one, const TreeNodeKey *other)
{
    return one->key[0] && one->folder == other->folder && one->inStarred == other->inStarred && Core_EqualsI(one->key, other->key);
}

/* Two rows of one session (in its folder or under Pinned). */
static BOOL SameSession(const TreeNodeKey *one, const TreeNodeKey *other)
{
    return one->key[0] && !one->folder && !other->folder && Core_EqualsI(one->key, other->key);
}

/* The selection, as the tree shows it. Another session (or a folder): the
 * details show its profiles from the top (its folder is looked up again only
 * when it is another one: SelectedFolder). */
static void SetSelection(const TreeNodeKey *key)
{
    g_view.selection = *key;
}

/* The next refill keeps the view where it is (a snapshot read in the
 * background, a new scale): the row on top of the view stays on top, as far
 * above the view's top as it starts now, whatever rows are added or removed
 * above it (gone, the row below it takes its place); at the very top, the
 * view stays there, where new rows show. */
static void RememberTopRow(void)
{
    TVHITTESTINFO hit;
    RECT rc;
    POINT top = { 0, 0 };
    HTREEITEM item;
    g_view.keepPlace = TRUE;
    g_view.topRow.key[0] = g_view.topRowNext.key[0] = 0;
    g_view.topRowOffset = 0;
    MapWindowPoints(g_view.treeArea, g_view.tree, &top, 1);   /* the view's top, in the tree */
    ZeroMemory(&hit, sizeof hit);
    hit.pt = top;
    if ((item = TreeView_HitTest(g_view.tree, &hit)) == NULL || !TreeView_GetItemRect(g_view.tree, item, &rc, FALSE)) return;
    if (item == TreeView_GetRoot(g_view.tree) && top.y <= rc.top) return;
    ItemKey(item, &g_view.topRow);
    g_view.topRowOffset = top.y - rc.top;
    if ((item = TreeView_GetNextVisible(g_view.tree, item)) != NULL) ItemKey(item, &g_view.topRowNext);
}

/* Profile `folder`'s kept place, found or (with `make`) made; NULL for none. */
static struct ProfilePlace *PlaceFor(const WCHAR *folder, BOOL make)
{
    int i;
    if (!folder[0]) return NULL;
    if (!g_view.places) {
        if (!make || (g_view.places = (struct ProfilePlace *)Util_Alloc(MAX_PROFILES * sizeof *g_view.places)) == NULL) return NULL;
    }
    for (i = 0; i < g_view.placeCount; i++)
        if (Core_EqualsI(g_view.places[i].folder, folder)) return &g_view.places[i];
    if (!make || g_view.placeCount >= MAX_PROFILES) return NULL;
    StringCchCopyW(g_view.places[g_view.placeCount].folder, FOLDER_CCH, folder);
    return &g_view.places[g_view.placeCount++];
}

/* The profile shown now: its scroll place and how far each heading is shown kept, so coming back shows it so. */
static void SaveProfilePlace(void)
{
    struct ProfilePlace *slot = PlaceFor(g_view.folder, TRUE);
    if (!slot) return;
    RememberTopRow();            /* fills g_view.topRow/next/offset */
    g_view.keepPlace = FALSE;    /* only captured: no refill of this profile follows */
    slot->used = TRUE;
    slot->top = g_view.topRow;
    slot->next = g_view.topRowNext;
    slot->offset = g_view.topRowOffset;
    memcpy(slot->more, g_view.more, sizeof slot->more);
    memcpy(slot->moreStage, g_view.moreStage, sizeof slot->moreStage);
    slot->moreCount = g_view.moreCount;
}

/* The view set to show profile `folder` where it was left (its scroll and how far shown); FALSE when it was not
 * shown yet, the caller then starting it at the top with its first sessions. */
static BOOL LoadProfilePlace(const WCHAR *folder)
{
    struct ProfilePlace *slot = PlaceFor(folder, FALSE);
    if (!slot || !slot->used) return FALSE;
    g_view.topRow = slot->top;
    g_view.topRowNext = slot->next;
    g_view.topRowOffset = slot->offset;
    memcpy(g_view.more, slot->more, sizeof g_view.more);
    memcpy(g_view.moreStage, slot->moreStage, sizeof g_view.moreStage);
    g_view.moreCount = slot->moreCount;
    g_view.keepPlace = TRUE;
    return TRUE;
}

/* Where the tree is (its profile, the row on top and how far above the
 * view's top), kept for the next run; put back by the first fill of that
 * profile then (RestorePlace). */
static void KeepPlace(void)
{
    WCHAR items[4][MAX_PATH];
    if (!g_view.rendered || !g_view.renderedFolder[0]) return;
    RememberTopRow();
    g_view.keepPlace = FALSE;   /* only kept: no refill follows */
    StringCchCopyW(items[0], MAX_PATH, g_view.renderedFolder);
    StringCchCopyW(items[1], MAX_PATH, g_view.topRow.key[0] ? g_view.topRow.key : L"-");
    StringCchCopyW(items[2], MAX_PATH, g_view.topRow.folder ? L"folder" : g_view.topRow.inStarred ? L"pinned" : L"session");
    StringCchPrintfW(items[3], MAX_PATH, L"%d", g_view.topRowOffset);
    Util_SetSettingList(PLACE_SECTION, (const WCHAR (*)[MAX_PATH])items, 4);
}

static void RestorePlace(void)
{
    WCHAR items[4][MAX_PATH];
    if (g_view.placeRestored) return;
    g_view.placeRestored = TRUE;
    if (Util_GetSettingList(PLACE_SECTION, items, 4) != 4 || !Core_EqualsI(items[0], g_view.folder) || wcscmp(items[1], L"-") == 0) return;
    g_view.keepPlace = TRUE;
    StringCchCopyW(g_view.topRow.key, ARRAYSIZE(g_view.topRow.key), items[1]);
    g_view.topRow.folder = wcscmp(items[2], L"folder") == 0;
    g_view.topRow.inStarred = wcscmp(items[2], L"pinned") == 0;
    g_view.topRowNext.key[0] = 0;
    g_view.topRowOffset = _wtoi(items[3]);
}

/* `item` on top of the view again, as far above its top as it was. */
static void PutBackOnTop(HTREEITEM item)
{
    RECT rc;
    if (GetWindowLongW(g_view.tree, GWL_STYLE) & WS_VSCROLL) {
        TreeView_SelectSetFirstVisible(g_view.tree, item);   /* too tall for its view, the tree scrolls itself by rows */
        return;
    }
    /* The tree is as tall as its rows, and less than 32767 px: its view scrolls it. */
    if (TreeView_GetItemRect(g_view.tree, item, &rc, FALSE))
        SendMessageW(g_view.treeArea, WM_VSCROLL,
                     MAKEWPARAM(SB_THUMBPOSITION, (WORD)min(max(0, rc.top + g_view.topRowOffset), SHRT_MAX)), 0);
}

static HTREEITEM InsertNode(HTREEITEM parent, HTREEITEM after, const WCHAR *text, LPARAM param)
{
    TVINSERTSTRUCTW ins;
    ZeroMemory(&ins, sizeof ins);
    ins.hParent = parent;
    ins.hInsertAfter = after;
    ins.itemex.mask = TVIF_TEXT | TVIF_PARAM | TVIF_INTEGRAL;
    ins.itemex.pszText = (LPWSTR)text;
    ins.itemex.lParam = param;
    ins.itemex.iIntegral = param < 0 && !IsMoreNode(param) ? FOLDER_UNITS + GAP_UNITS : SESSION_UNITS;
    return TreeView_InsertItem(g_view.tree, &ins);
}

/* Prepended: the rows are added from the last. */
static HTREEITEM AddNode(HTREEITEM parent, const WCHAR *text, LPARAM param)
{
    return InsertNode(parent, TVI_FIRST, text, param);
}

/* A heading folded unless the user opened it; while searching, every one
 * open, so that what matches shows (not remembered: the search cleared,
 * the others fold again). */
static void ExpandUnlessFolded(HTREEITEM folder)
{
    TreeNodeKey key;
    if (!folder) return;
    ItemKey(folder, &key);
    if (g_view.filter[0] || !WasFolded(key.key)) TreeView_Expand(g_view.tree, folder, TVE_EXPAND);
}

/* ---------------------------------------------------------------- headings */

static int FindBucket(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_view.bucketCount; i++)
        if (wcscmp(g_view.buckets[i].key, key) == 0) return i;
    return -1;
}

/* The heading of `key`, made when it is new (`rank`: its place, when it
 * has a fixed one). -1 when there is no room for it. */
static int AddBucket(const WCHAR *key, const WCHAR *name, int group, int rank)
{
    Bucket *bucket;
    int i = FindBucket(key);
    if (i >= 0) return i;
    if (g_view.bucketCount == g_view.bucketCapacity) {
        int larger = g_view.bucketCapacity ? g_view.bucketCapacity * 2 : 32;
        Bucket *grown = (Bucket *)(g_view.buckets ? HeapReAlloc(GetProcessHeap(), 0, g_view.buckets, (size_t)larger * sizeof *grown)
                                                  : HeapAlloc(GetProcessHeap(), 0, (size_t)larger * sizeof *grown));
        if (!grown) return -1;
        g_view.buckets = grown;
        g_view.bucketCapacity = larger;
    }
    bucket = &g_view.buckets[g_view.bucketCount];
    ZeroMemory(bucket, sizeof *bucket);
    StringCchCopyW(bucket->key, ARRAYSIZE(bucket->key), key);
    StringCchCopyW(bucket->name, ARRAYSIZE(bucket->name), name);
    bucket->group = group;
    bucket->rank = rank;
    return g_view.bucketCount++;
}

/* What sorts a session by time: when it was made, or used last. */
static ULONGLONG RowTime(const SessionRow *row, int p)
{
    const SessionEntry *entry = EntryOf(row, p);
    if (g_view.sorting == SORT_CREATED && entry && entry->created) return entry->created;
    return row->lastActivity;
}

/* The day of `ms` (since 1970) on this PC's clock, as days since 1601. */
static LONGLONG LocalDay(ULONGLONG ms)
{
    ULARGE_INTEGER time;
    FILETIME utc = Core_MsFileTime(ms), local;
    if (!FileTimeToLocalFileTime(&utc, &local)) local = utc;
    time.LowPart = local.dwLowDateTime;
    time.HighPart = local.dwHighDateTime;
    return (LONGLONG)(time.QuadPart / (10000000ull * 86400));
}

/* The heading of the day of `ms`, as Claude's sidebar has them: Today,
 * Yesterday, then the date, in the interface's language, with its year when
 * that is not this one; Older for a session with no time. They rank by how
 * many days ago; a day is remembered (folded) by its date. */
static int DayBucket(ULONGLONG ms, LONGLONG today)
{
    WCHAR key[48], name[64];
    const WCHAR *picture;
    SYSTEMTIME day, now;
    FILETIME utc, local;
    LONGLONG ago;
    int bucket;
    if (!ms) return AddBucket(L"*day:", TR(L"Older"), -1, INT_MAX);
    ago = max(0, today - LocalDay(ms));   /* a time ahead of this PC's clock: today */
    StringCchPrintfW(key, ARRAYSIZE(key), L"*day:%lld", today - ago);
    if ((bucket = FindBucket(key)) >= 0) return bucket;
    if (ago <= 1) return AddBucket(key, ago ? TR(L"Yesterday") : TR(L"Today"), -1, (int)ago);
    utc = Core_MsFileTime(ms);
    if (!FileTimeToLocalFileTime(&utc, &local)) local = utc;
    GetLocalTime(&now);
    name[0] = 0;
    if (FileTimeToSystemTime(&local, &day)) {
        picture = day.wYear == now.wYear ? TR(L"MMM d") : TR(L"MMM d, yyyy");
        if (!GetDateFormatEx(Localize_LanguageCode(Localize_EffectiveLanguage()), 0, &day, picture, name, ARRAYSIZE(name), NULL))
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &day, NULL, name, ARRAYSIZE(name), NULL);
    }
    return AddBucket(key, name[0] ? name : TR(L"Older"), -1, (int)min(ago, (LONGLONG)INT_MAX - 1));
}

static const WCHAR *StateName(StateBucket state)
{
    switch (state) {
    case STATE_OPEN: return TR(L"Open now");
    case STATE_WAITING: return TR(L"Changes waiting");
    default: return TR(L"Not open");
    }
}

/* ---------------------------------------------------- Claude's own groups */

static BOOL ReadCustomGroup(void *context, const char *value, size_t length)
{
    CustomGroups *custom = (CustomGroups *)context;
    const char *member;
    size_t memberLength;
    if (custom->count >= MAX_CUSTOM_GROUPS) return FALSE;
    if (!Core_JsonMember(value, length, "id", &member, &memberLength) ||
        !Core_JsonString(member, memberLength, custom->id[custom->count], ARRAYSIZE(custom->id[custom->count])))
        return TRUE;
    custom->name[custom->count][0] = 0;
    if (Core_JsonMember(value, length, "name", &member, &memberLength))
        Core_JsonString(member, memberLength, custom->name[custom->count], ARRAYSIZE(custom->name[custom->count]));
    custom->count++;
    return TRUE;
}

/* "code:<session>": "<group id>", the session named as Claude's web page
 * names it: by its entry's own id. */
static BOOL ReadCustomAssignment(void *context, const char *key, size_t keyLength, const char *value, size_t valueLength)
{
    CustomGroups *custom = (CustomGroups *)context;
    WCHAR session[SESSION_ID_CCH], group[64];
    char quoted[SESSION_ID_CCH + 16];
    int g;
    if (keyLength < 6 || keyLength - 5 + 2 > sizeof quoted || memcmp(key, "code:", 5) != 0) return TRUE;
    quoted[0] = '"';
    memcpy(quoted + 1, key + 5, keyLength - 5);
    quoted[keyLength - 5 + 1] = '"';
    if (!Core_JsonString(quoted, keyLength - 5 + 2, session, ARRAYSIZE(session)) || !Core_JsonString(value, valueLength, group, ARRAYSIZE(group)))
        return TRUE;
    for (g = 0; g < custom->count && wcscmp(custom->id[g], group) != 0; g++) {}
    if (g == custom->count) return TRUE;
    if (custom->assignmentCount == custom->assignmentCapacity) {
        int larger = custom->assignmentCapacity ? custom->assignmentCapacity * 2 : 64;
        WCHAR (*keys)[SESSION_ID_CCH] = (WCHAR (*)[SESSION_ID_CCH])(custom->assigned
            ? HeapReAlloc(GetProcessHeap(), 0, custom->assigned, (size_t)larger * sizeof *custom->assigned)
            : HeapAlloc(GetProcessHeap(), 0, (size_t)larger * sizeof *custom->assigned));
        int *groups;
        if (!keys) return FALSE;
        custom->assigned = keys;
        groups = (int *)(custom->assignedGroup ? HeapReAlloc(GetProcessHeap(), 0, custom->assignedGroup, (size_t)larger * sizeof(int))
                                               : HeapAlloc(GetProcessHeap(), 0, (size_t)larger * sizeof(int)));
        if (!groups) return FALSE;
        custom->assignedGroup = groups;
        custom->assignmentCapacity = larger;
    }
    StringCchCopyW(custom->assigned[custom->assignmentCount], SESSION_ID_CCH, session);
    custom->assignedGroup[custom->assignmentCount++] = g;
    return TRUE;
}

/* Claude's groups of profile `p` and the sessions in them, from its web
 * storage (the sidebar's customGroupsByScope, under its account and
 * organization: the last two folders of its entries'): read once for it,
 * or `again`. */
static void LoadCustomGroups(int p, BOOL again)
{
    CustomGroups *custom = &g_view.custom;
    const WCHAR *entries = g_view.set.source[p].entriesDir, *organization, *account;
    WCHAR scope[MAX_PATH];
    char scopeKey[MAX_PATH * 3];
    const char *state, *scopes, *own, *member;
    size_t stateLength, scopesLength, ownLength, memberLength, length;
    WebStore *store;
    char *text;
    if (!again && Core_EqualsI(custom->folder, ProfileAt(p)->folder)) return;
    StringCchCopyW(custom->folder, ARRAYSIZE(custom->folder), ProfileAt(p)->folder);
    custom->count = custom->assignmentCount = 0;
    if (!entries[0] || (organization = wcsrchr(entries, L'\\')) == NULL || organization == entries) return;
    for (account = organization; account > entries && account[-1] != L'\\'; account--) {}
    if (FAILED(StringCchPrintfW(scope, ARRAYSIZE(scope), L"%.*s/%s", (int)(organization - account), account, organization + 1)) ||
        !WideCharToMultiByte(CP_UTF8, 0, scope, -1, scopeKey, (int)sizeof scopeKey, NULL, NULL))
        return;
    if ((store = WebStore_Open(ProfileAt(p))) == NULL) return;
    if ((text = WebStore_Get(store, L"dframe-store", &length)) != NULL) {
        if (Core_JsonMember(text, length, "state", &state, &stateLength) &&
            Core_JsonMember(state, stateLength, "customGroupsByScope", &scopes, &scopesLength) &&
            Core_JsonMember(scopes, scopesLength, scopeKey, &own, &ownLength)) {
            if (Core_JsonMember(own, ownLength, "groups", &member, &memberLength)) Core_JsonEachElement(member, memberLength, ReadCustomGroup, custom);
            if (Core_JsonMember(own, ownLength, "assignments", &member, &memberLength))
                Core_JsonEachMember(member, memberLength, ReadCustomAssignment, custom);
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    WebStore_Free(store);
}

/* The group of Claude's that session entry `entry` is in, -1 for none. */
static int CustomGroupOf(const SessionEntry *entry)
{
    int i;
    for (i = 0; entry && i < g_view.custom.assignmentCount; i++)
        if (Core_EqualsI(g_view.custom.assigned[i], entry->localId)) return g_view.custom.assignedGroup[i];
    return -1;
}

/* The heading session `row` shows under, made when it is new; -1 without
 * headings (grouped by nothing). */
static const WCHAR *EnvironmentName(int i);
static const DWORD kEnvironments[] = { SESSION_ENV_LOCAL, SESSION_ENV_CLOUD, SESSION_ENV_REMOTE, SESSION_ENV_SSH, SESSION_ENV_SLACK };

static int BucketOfRow(const SessionRow *row, const SessionEntry *entry, int p, LONGLONG today)
{
    WCHAR key[MAX_PATH], name[MAX_PATH];
    int g;
    switch (g_view.grouping) {
    case GROUP_FOLDER:
        GroupKey(row->group, key, ARRAYSIZE(key));
        SessionStore_GroupName(&g_view.set, row->group, name, ARRAYSIZE(name));
        return AddBucket(key, name, row->group, 0);
    case GROUP_DATE:
        return DayBucket(RowTime(row, p), today);
    case GROUP_STATE: {
        StateBucket state = row->live ? STATE_OPEN : entry->pending ? STATE_WAITING : STATE_IDLE;
        StringCchPrintfW(key, ARRAYSIZE(key), L"*state:%d", (int)state);
        return AddBucket(key, StateName(state), -1, (int)state);
    }
    case GROUP_ENVIRONMENT:
        for (g = 0; g + 1 < (int)ARRAYSIZE(kEnvironments) && !(entry->environment & kEnvironments[g]); g++) {}
        StringCchPrintfW(key, ARRAYSIZE(key), L"*environment:%lu", kEnvironments[g]);
        return AddBucket(key, EnvironmentName(g), -1, g);
    case GROUP_CUSTOM:
        if ((g = CustomGroupOf(entry)) >= 0) {
            StringCchPrintfW(key, ARRAYSIZE(key), L"*group:%s", g_view.custom.id[g]);
            return AddBucket(key, g_view.custom.name[g], -1, g);
        }
        return AddBucket(L"*group:", TR(L"Ungrouped"), -1, MAX_CUSTOM_GROUPS);
    default:
        return -1;
    }
}

static int g_sortProfile;   /* the profile whose sessions are sorted (qsort has no context) */

/* Sessions in the order and direction the filter menu chose. */
static int CompareRows(int one, int other)
{
    const SessionRow *a = &g_view.set.rows[one], *b = &g_view.set.rows[other];
    int order = Core_CompareSessionOrder(ShownTitle(a, g_sortProfile), RowTime(a, g_sortProfile),
                                         ShownTitle(b, g_sortProfile), RowTime(b, g_sortProfile),
                                         g_view.sorting == SORT_NAME, g_view.descending);
    return order ? order : one - other;
}

static int CompareRowIndexes(const void *one, const void *other)
{
    return CompareRows(*(const int *)one, *(const int *)other);
}

/* Headings in their order: a folder's sessions sort it as they sort them
 * (by its name, or its latest first); days, states and Claude's groups
 * keep theirs. */
static int CompareBuckets(const void *one, const void *other)
{
    const Bucket *a = &g_view.buckets[*(const int *)one], *b = &g_view.buckets[*(const int *)other];
    if (g_view.grouping == GROUP_FOLDER) {
        int order = Core_CompareSessionOrder(a->name, a->newest, b->name, b->newest,
                                             g_view.sorting == SORT_NAME, g_view.descending);
        if (order) return order;
    } else if (a->rank != b->rank) {
        return a->rank < b->rank ? -1 : 1;
    }
    return *(const int *)one - *(const int *)other;
}

/* Each heading's rank, its place; the heading at each place (HeapFree). */
static int *RankBuckets(void)
{
    int *byRank, i;
    if (!g_view.bucketCount || (byRank = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)g_view.bucketCount * sizeof *byRank)) == NULL) return NULL;
    for (i = 0; i < g_view.bucketCount; i++) byRank[i] = i;
    qsort(byRank, (size_t)g_view.bucketCount, sizeof *byRank, CompareBuckets);
    for (i = 0; i < g_view.bucketCount; i++) g_view.buckets[byRank[i]].rank = i;
    return byRank;
}

typedef struct ShownRow {               /* a session the tree shows, under which heading, in which part */
    int  row, bucket;
    BOOL archived;
} ShownRow;

/* The part of those not archived first, then by heading, then as sessions sort. */
static int CompareShown(const void *one, const void *other)
{
    const ShownRow *a = (const ShownRow *)one, *b = (const ShownRow *)other;
    int rankA = a->bucket >= 0 ? g_view.buckets[a->bucket].rank : -1, rankB = b->bucket >= 0 ? g_view.buckets[b->bucket].rank : -1;
    if (a->archived != b->archived) return a->archived ? 1 : -1;
    if (rankA != rankB) return rankA < rankB ? -1 : 1;
    return CompareRows(a->row, b->row);
}

/* The sessions a heading node shows; NODE_STARRED: the starred ones, not
 * archived; NODE_ARCHIVED: the archived ones. */
static int FolderCount(LPARAM node)
{
    int bucket = NodeBucket(node);
    if (node == NODE_STARRED) return g_view.starredCount;
    if (node == NODE_ARCHIVED) return g_view.archivedCount;
    if (bucket < 0 || bucket >= g_view.bucketCount) return 0;
    return g_view.buckets[bucket].count[UnderArchived(node) || g_view.status == STATUS_ARCHIVED];
}

typedef struct RefillMatch {            /* what a refill finds again of the tree before */
    HTREEITEM   selected;               /* the row selected */
    HTREEITEM   sameSession;            /* its session's other row (in its folder or under Pinned) */
    HTREEITEM   neighbor[2];            /* the sessions below and above it (g_view.neighbor) */
    HTREEITEM   top;                    /* the row on top of the view */
    HTREEITEM   topNext;                /* the row below it */
} RefillMatch;

/* The sessions just below and just above the selected session's row, taken
 * once from the tree as last filled, while the snapshot it was filled from
 * is still the one shown: its rows name that snapshot's rows. */
static void RememberNeighbors(void)
{
    HTREEITEM selected = TreeView_GetSelection(g_view.tree), item;
    int direction;
    if (g_view.neighborsTaken) return;
    g_view.neighborsTaken = TRUE;
    g_view.neighbor[0].key[0] = g_view.neighbor[1].key[0] = 0;
    if (!selected || NodeParam(selected) < 0) return;
    for (direction = 0; direction < 2; direction++)
        for (item = selected; (item = direction == 0 ? TreeView_GetNextVisible(g_view.tree, item)
                                                     : TreeView_GetPrevVisible(g_view.tree, item)) != NULL;)
            if (NodeParam(item) >= 0) {
                ItemKey(item, &g_view.neighbor[direction]);
                break;
            }
}

/* No neighbor to move to: the next fill shows another profile, or another
 * search, whose rows the previous ones do not follow. */
static void ForgetNeighbors(void)
{
    g_view.neighborsTaken = TRUE;
    g_view.neighbor[0].key[0] = g_view.neighbor[1].key[0] = 0;
}

static void MatchNode(RefillMatch *match, HTREEITEM item, LPARAM node, BOOL inStarred)
{
    TreeNodeKey key;
    int direction;
    NodeKey(node, inStarred, &key);
    if (SameNode(&key, &g_view.selection)) match->selected = item;
    else if (SameSession(&key, &g_view.selection)) match->sameSession = item;
    for (direction = 0; direction < 2; direction++)
        if (SameNode(&key, &g_view.neighbor[direction])) match->neighbor[direction] = item;
    if (SameNode(&key, &g_view.topRow)) match->top = item;
    if (SameNode(&key, &g_view.topRowNext)) match->topNext = item;
}

/* The first session of the first folder open: what the details show while
 * nothing was selected (a folder folded meanwhile stays folded). */
static HTREEITEM FirstShownSession(void)
{
    HTREEITEM top;
    for (top = TreeView_GetRoot(g_view.tree); top; top = TreeView_GetNextSibling(g_view.tree, top)) {
        if (NodeParam(top) >= 0) return top;   /* grouped by nothing */
        if (TreeView_GetItemState(g_view.tree, top, TVIS_EXPANDED) & TVIS_EXPANDED) return TreeView_GetChild(g_view.tree, top);
    }
    return NULL;
}

/* How many of a heading's sessions `shown[first..end)` show, as Claude's
 * sidebar shows them: the first five, ten more after Show more, all of them
 * after Show all or while searching; down to the session selected and those
 * chosen, which stay in sight. */
static int HeadingLimit(LPARAM heading, const ShownRow *shown, int first, int end)
{
    TreeNodeKey key;
    int stage, limit, k;
    NodeKey(heading, FALSE, &key);
    stage = MoreStage(key.key);
    limit = g_view.filter[0] || stage >= 2 ? end - first : min(end - first, stage ? MORE_FIRST + MORE_NEXT : MORE_FIRST);
    for (k = end - 1; k >= first + limit; k--) {
        const WCHAR *row = g_view.set.rows[shown[k].row].key;
        if ((!g_view.selection.folder && !g_view.selection.inStarred && Core_EqualsI(g_view.selection.key, row)) || IsMarked(row))
            return k - first + 1;
    }
    return limit;
}

/* Sessions `shown[from..to)`, one part sorted (CompareShown), under
 * `parent` below their headings, the headings in their order (`byRank`),
 * each with its first sessions (HeadingLimit) and Show more below them
 * when it has more; prepended, from the last. Grouped by Claude's groups,
 * with Show empty groups, the top part shows its groups with no session
 * too. */
static void AddPart(HTREEITEM parent, const ShownRow *shown, int from, int to, const int *byRank, BOOL underArchived, RefillMatch *match)
{
    int p = ShownProfileIndex(), i = to - 1, rank;
    BOOL empties = !underArchived && g_view.showEmptyGroups;
    HTREEITEM item;
    if (g_view.grouping == GROUP_NONE || !byRank) {
        for (; i >= from; i--) {
            item = AddNode(parent, ShownTitle(&g_view.set.rows[shown[i].row], p), shown[i].row);
            MatchNode(match, item, shown[i].row, FALSE);
        }
        return;
    }
    for (rank = g_view.bucketCount - 1; rank >= 0; rank--) {
        int bucket = byRank[rank], last = i, limit, k;
        LPARAM node = underArchived ? NODE_ARCHIVED - 1 - bucket : -2 - bucket;
        HTREEITEM heading;
        /* Ungrouped is a fallback, not an empty group the user created. */
        if (!(i >= from && shown[i].bucket == bucket) &&
            !(empties && (g_view.grouping != GROUP_CUSTOM || g_view.buckets[bucket].key[7]))) continue;
        heading = AddNode(parent, g_view.buckets[bucket].name, node);
        MatchNode(match, heading, node, FALSE);
        for (; i >= from && shown[i].bucket == bucket; i--) {}
        limit = HeadingLimit(node, shown, i + 1, last + 1);
        if (limit < last - i) {
            LPARAM more = MoreNode(bucket, underArchived);
            item = AddNode(heading, MoreLabel(more), more);
            MatchNode(match, item, more, FALSE);
        }
        for (k = i + limit; k > i; k--) {
            item = AddNode(heading, ShownTitle(&g_view.set.rows[shown[k].row], p), shown[k].row);
            MatchNode(match, item, shown[k].row, FALSE);
        }
        ExpandUnlessFolded(heading);
    }
}

/* The tree's nodes for profile `p`, as the filter menu chose: Pinned on
 * top, then the sessions under their headings, the archived ones below
 * under Archived when every status shows. `match` notes where the rows of
 * the tree as it was went. */
static void AddTreeNodes(int p, RefillMatch *match)
{
    HTREEITEM item;
    ShownRow *shown;
    int *byRank = NULL, *starred, r, count = 0, top = 0, starredCount = 0, i;
    LONGLONG today = LocalDay(Util_NowMs());
    if (g_view.grouping == GROUP_CUSTOM) {
        LoadCustomGroups(p, FALSE);
        if (g_view.showEmptyGroups)
            for (i = 0; i < g_view.custom.count; i++) {
                WCHAR groupKey[MAX_PATH];
                StringCchPrintfW(groupKey, ARRAYSIZE(groupKey), L"*group:%s", g_view.custom.id[i]);
                AddBucket(groupKey, g_view.custom.name[i], -1, i);
            }
    }
    shown = (ShownRow *)Util_Alloc((size_t)g_view.set.rowCount * sizeof *shown);
    starred = (int *)Util_Alloc((size_t)g_view.set.rowCount * sizeof *starred);
    if (shown && starred) {
        for (r = 0; r < g_view.set.rowCount; r++) {
            const SessionRow *row = &g_view.set.rows[r];
            const SessionEntry *entry = EntryOf(row, p);
            int bucket;
            if (entry && g_view.showEmptyGroups) BucketOfRow(row, entry, p, today);
            if (!RowVisibleIn(row, p)) continue;
            bucket = BucketOfRow(row, entry, p, today);
            shown[count].row = r;
            shown[count].bucket = bucket;
            shown[count].archived = entry->archived;
            count++;
            if (bucket >= 0) {
                Bucket *heading = &g_view.buckets[bucket];
                heading->count[entry->archived ? 1 : 0]++;
                heading->newest = max(heading->newest, RowTime(row, p));
            }
            if (entry->archived) g_view.archivedCount++;
            else if (StarredIn(entry)) starred[starredCount++] = r;
        }
        g_view.starredCount = starredCount;
        byRank = RankBuckets();
        g_sortProfile = p;
        qsort(shown, (size_t)count, sizeof *shown, CompareShown);
        qsort(starred, (size_t)starredCount, sizeof *starred, CompareRowIndexes);
        /* The top part: every session shown when only the archived ones are,
         * else those not archived. */
        for (top = 0; top < count && (!shown[top].archived || g_view.status == STATUS_ARCHIVED); top++) {}
        AddPart(TVI_ROOT, shown, 0, top, byRank, FALSE, match);
        if (starredCount) {
            HTREEITEM folder = AddNode(TVI_ROOT, StarredName(), NODE_STARRED);
            MatchNode(match, folder, NODE_STARRED, FALSE);
            for (i = starredCount - 1; i >= 0; i--) {
                item = AddNode(folder, ShownTitle(&g_view.set.rows[starred[i]], p), starred[i]);
                MatchNode(match, item, starred[i], TRUE);
            }
            ExpandUnlessFolded(folder);
        }
        /* Below, the archived sessions, under their headings too. */
        if (top < count) {
            HTREEITEM archived = InsertNode(TVI_ROOT, TVI_LAST, TR(L"Archived"), NODE_ARCHIVED);
            MatchNode(match, archived, NODE_ARCHIVED, FALSE);
            AddPart(archived, shown, top, count, byRank, TRUE, match);
            ExpandUnlessFolded(archived);
        }
        if (TreeView_GetRoot(g_view.tree) && NodeParam(TreeView_GetRoot(g_view.tree)) < 0) {
            TVITEMEXW first;
            ZeroMemory(&first, sizeof first);
            first.mask = TVIF_INTEGRAL;
            first.hItem = TreeView_GetRoot(g_view.tree);
            first.iIntegral = FOLDER_UNITS;   /* no room above the first heading */
            SendMessageW(g_view.tree, TVM_SETITEMW, 0, (LPARAM)&first);
        }
    }
    g_view.rowsShown = shown;   /* what the headings stand for (FolderRows), till the next fill */
    g_view.rowsShownCount = count;
    g_view.rowsShownTop = top;
    Util_Free(starred);
    Util_Free(byRank);
}

/* After a fill, the row selected: the one `match` found, else the first
 * session shown, else the first heading; NULL in an empty tree. */
static HTREEITEM SelectAfterFill(const RefillMatch *match)
{
    TreeNodeKey key;
    HTREEITEM pick = match->selected ? match->selected : match->sameSession ? match->sameSession :
                     match->neighbor[0] ? match->neighbor[0] : match->neighbor[1] ? match->neighbor[1] : FirstShownSession();
    if (!pick) pick = TreeView_GetRoot(g_view.tree);
    ZeroMemory(&key, sizeof key);
    if (pick) {
        /* Opens the heading it is in, if folded, for now: only what the user
         * folds or opens is kept (TVN_ITEMEXPANDED, never while filling). */
        TreeView_SelectItem(g_view.tree, pick);
        ItemKey(pick, &key);
    }
    SetSelection(&key);
    return pick;
}

/* After a fill, the view keeps its place (RememberTopRow), else shows `pick`. */
static void PlaceAfterFill(const RefillMatch *match, HTREEITEM pick)
{
    /* At the very top, the view stays there: a tree too tall for its view
     * has scrolled itself to the selection. */
    if (match->top) {
        PutBackOnTop(match->top);
    } else if (match->topNext) {
        g_view.topRowOffset = 0;
        PutBackOnTop(match->topNext);
    } else if (g_view.keepPlace && !g_view.topRow.key[0] && TreeView_GetRoot(g_view.tree)) {
        PutBackOnTop(TreeView_GetRoot(g_view.tree));
    } else if (pick && !g_view.keepPlace) {
        TreeView_EnsureVisible(g_view.tree, pick);
    }
    g_view.keepPlace = FALSE;
    g_view.topRow.key[0] = g_view.topRowNext.key[0] = 0;
}

/* The sessions chosen that the tree no longer shows (profile `p`) are no
 * longer chosen. */
static void UnmarkHidden(int p)
{
    int r;
    for (r = g_view.markedCount - 1; r >= 0; r--) {
        int row = SessionStore_FindRow(&g_view.set, g_view.marked[r]);
        if (row < 0 || p < 0 || !RowVisibleIn(&g_view.set.rows[row], p)) Mark(g_view.marked[r], FALSE);
    }
    if (g_view.several && !g_view.markedCount) ClearMarks();
}

/* The tree filled again for the shown profile, as the filter menu chose:
 * Pinned on top, then the sessions under their headings (by day, folder,
 * state or Claude's groups, or none), the archived ones below under
 * Archived when every status shows. The row selected stays selected (its
 * session's other row when it is gone: unpinned, moved; the session below
 * it when its session is gone, else the one above it), else the first
 * session shown, else the first heading; headings stay as the user left
 * them (folded until opened). The view keeps its place (RememberTopRow),
 * else shows the selection: the search, the profile or the filters
 * changed. */
/* The fold button says what it does: fold every heading while one is open, else open them all. */
static void ShowFoldButton(void)
{
    HTREEITEM item;
    int state = 1;
    for (item = TreeView_GetRoot(g_view.tree); item && state; item = TreeView_GetNextSibling(g_view.tree, item))
        if (NodeParam(item) < 0 && (TreeView_GetItemState(g_view.tree, item, TVIS_EXPANDED) & TVIS_EXPANDED)) state = 0;
    Theme_SetMainGlyph(g_view.dlg, IDC_S_FOLD, state);
    SetDlgItemTextW(g_view.dlg, IDC_S_FOLD, TR(Theme_MainCaption(IDC_S_FOLD, state)));
}

static void FillTree(void)
{
    RefillMatch match;
    HTREEITEM pick;
    int p = ShownProfileIndex();
    ZeroMemory(&match, sizeof match);
    RememberNeighbors();   /* a fill from the same snapshot: from the tree as it is */
    g_view.filling = TRUE;
    SendMessageW(g_view.tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_view.tree);
    g_view.bucketCount = g_view.starredCount = g_view.archivedCount = 0;
    Util_Free(g_view.rowsShown);
    g_view.rowsShown = NULL;
    g_view.rowsShownCount = g_view.rowsShownTop = 0;
    if (p >= 0) AddTreeNodes(p, &match);
    pick = SelectAfterFill(&match);
    SendMessageW(g_view.tree, WM_SETREDRAW, TRUE, 0);   /* the view takes the tree's new height */
    PlaceAfterFill(&match, pick);
    g_view.neighborsTaken = FALSE;   /* the tree's rows name the snapshot shown again */
    UnmarkHidden(p);
    InvalidateRect(g_view.tree, NULL, TRUE);
    g_view.filling = FALSE;
    ShowFoldButton();
}

/* --------------------------------------------------------- choosing several */

/* `item` selected in the tree, the sessions chosen staying chosen. */
static void SelectMarking(HTREEITEM item)
{
    g_view.marking = TRUE;
    TreeView_SelectItem(g_view.tree, item);
    g_view.marking = FALSE;
}

/* The tree's row of `key` among the rows shown; NULL when it is not one. */
static HTREEITEM FindShownRow(const TreeNodeKey *key)
{
    HTREEITEM item;
    TreeNodeKey found;
    if (!key->key[0]) return NULL;
    for (item = TreeView_GetRoot(g_view.tree); item; item = TreeView_GetNextVisible(g_view.tree, item)) {
        ItemKey(item, &found);
        if (SameNode(&found, key)) return item;
    }
    return NULL;
}

/* A row shown of session `key`: in its folder, else under Pinned. */
static HTREEITEM FindShownSession(const WCHAR *key)
{
    TreeNodeKey node;
    HTREEITEM item;
    ZeroMemory(&node, sizeof node);
    StringCchCopyW(node.key, ARRAYSIZE(node.key), key);
    if ((item = FindShownRow(&node)) != NULL) return item;
    node.inStarred = TRUE;
    return FindShownRow(&node);
}

static void MarkFolder(HTREEITEM folder, BOOL on);
static int *FolderRows(HTREEITEM folder, int *count);

/* The row after `item` under folder row `folder`, at any depth (Archived
 * holds folders), in the tree's order; NULL past its last. */
static HTREEITEM NextUnder(HTREEITEM folder, HTREEITEM item)
{
    HTREEITEM next = TreeView_GetChild(g_view.tree, item);
    if (next) return next;
    while (item && item != folder) {
        if ((next = TreeView_GetNextSibling(g_view.tree, item)) != NULL) return next;
        item = TreeView_GetParent(g_view.tree, item);
    }
    return NULL;
}

/* Choosing starts from the selection: its session, or a folder's sessions, the first chosen. */
static void StartMarks(void)
{
    HTREEITEM selected;
    if (g_view.several) return;
    g_view.several = TRUE;
    g_view.markedCount = 0;
    if (!g_view.selection.folder) Mark(g_view.selection.key, TRUE);
    else if ((selected = TreeView_GetSelection(g_view.tree)) != NULL && NodeParam(selected) < 0 && NodeParam(selected) != NODE_NONE)
        MarkFolder(selected, TRUE);
}

/* Every session of folder row `folder` (Pinned's too), folded or not, chosen (`on`) or no longer. */
static void MarkFolder(HTREEITEM folder, BOOL on)
{
    int *rows, count, i;
    if ((rows = FolderRows(folder, &count)) == NULL) return;
    for (i = 0; i < count; i++) Mark(g_view.set.rows[rows[i]].key, on);
    HeapFree(GetProcessHeap(), 0, rows);
}

/* Folder row `folder` has sessions, and every one is chosen already. */
static BOOL FolderMarked(HTREEITEM folder)
{
    int *rows, count, i;
    BOOL all = TRUE;
    if ((rows = FolderRows(folder, &count)) == NULL) return FALSE;
    for (i = 0; i < count && all; i++) all = MarkIndex(g_view.set.rows[rows[i]].key) >= 0;
    HeapFree(GetProcessHeap(), 0, rows);
    return all;
}

/* The sessions shown from the anchor's row to `to` chosen, a folder's row
 * standing for all its sessions (the lower edge's too); with `add`, with
 * those chosen already. */
static void MarkRange(HTREEITEM to, BOOL add)
{
    HTREEITEM from = FindShownRow(&g_view.anchor), item;
    int edges = 0;
    if (!from) from = TreeView_GetSelection(g_view.tree);
    if (!from) from = to;
    StartMarks();
    if (!add) g_view.markedCount = 0;
    for (item = TreeView_GetRoot(g_view.tree); item && edges < 2; item = TreeView_GetNextVisible(g_view.tree, item)) {
        LPARAM node = NodeParam(item);
        if (item == from) edges++;
        if (item == to) edges++;   /* both at once when they are the same row */
        if (edges == 0) continue;
        if (node >= 0 && node < g_view.set.rowCount) Mark(g_view.set.rows[node].key, TRUE);
        else if (node != NODE_NONE) MarkFolder(item, TRUE);
    }
}

static void ShowMarks(HTREEITEM selected)
{
    SelectMarking(selected);
    InvalidateRect(g_view.tree, NULL, FALSE);
}

/* A click on session or folder `item` with Ctrl (`toggle`: it, or all of a
 * folder's sessions, chosen or no longer) or Shift (`range`: the sessions
 * from the anchor to it, added to those chosen with Ctrl too). */
static void ClickToChoose(HTREEITEM item, BOOL toggle, BOOL range)
{
    TreeNodeKey key;
    ItemKey(item, &key);
    if (range) {
        MarkRange(item, toggle);
    } else {
        StartMarks();
        if (NodeParam(item) >= 0) Mark(key.key, MarkIndex(key.key) < 0);
        else MarkFolder(item, !FolderMarked(item));
        g_view.anchor = key;
        /* One left, or none: the tree's own selection again, on it. */
        if (g_view.markedCount <= 1) {
            HTREEITEM only = g_view.markedCount ? FindShownSession(g_view.marked[0]) : NULL;
            ClearMarks();
            TreeView_SelectItem(g_view.tree, only ? only : item);
            return;
        }
    }
    ShowMarks(item);
}

/* Shift and an arrow: the range grows or shrinks to the next session up or down. */
static void ChooseWithKey(WPARAM key)
{
    HTREEITEM item = TreeView_GetSelection(g_view.tree);
    while (item && (item = key == VK_UP ? TreeView_GetPrevVisible(g_view.tree, item) : TreeView_GetNextVisible(g_view.tree, item)) != NULL &&
           NodeParam(item) < 0) {}
    if (!item) return;
    MarkRange(item, FALSE);
    ShowMarks(item);
    TreeView_EnsureVisible(g_view.tree, item);
}

/* Ctrl+A: every session the tree shows. */
static void ChooseAllShown(void)
{
    HTREEITEM item;
    StartMarks();
    for (item = TreeView_GetRoot(g_view.tree); item; item = TreeView_GetNextVisible(g_view.tree, item)) {
        LPARAM node = NodeParam(item);
        if (node >= 0 && node < g_view.set.rowCount) Mark(g_view.set.rows[node].key, TRUE);
    }
    InvalidateRect(g_view.tree, NULL, FALSE);
}

static void RunEditKey(WPARAM key);

/* Ctrl+C, Ctrl+X, Ctrl+V, Ctrl+Z and Ctrl+Y (or Ctrl+Shift+Z) run; FALSE for another key. */
static BOOL EditKey(WPARAM key)
{
    if (GetKeyState(VK_CONTROL) >= 0 || GetKeyState(VK_MENU) < 0) return FALSE;
    if (key != 'C' && key != 'X' && key != 'V' && key != 'Z' && key != 'Y') return FALSE;
    RunEditKey(key);
    return TRUE;
}

/* The characters those keys type: Ctrl+C's, Ctrl+V's, Ctrl+X's, Ctrl+Y's and Ctrl+Z's. */
static BOOL EditCharacter(WPARAM character)
{
    return character == 3 || character == 22 || character == 24 || character == 25 || character == 26;
}

/* Show more (Show all) on heading row `item`'s last row: ten more of its
 * sessions shown, then all of them, the rows above it where they are. From
 * the keyboard, or with that row selected, the first of those newly shown
 * is selected. */
static void ShowMore(HTREEITEM item, BOOL select)
{
    LPARAM node = NodeParam(item);
    HTREEITEM heading = TreeView_GetParent(g_view.tree, item), above = TreeView_GetPrevSibling(g_view.tree, item), found;
    TreeNodeKey key, more;
    int *rows, count, shownNow = 0;
    RECT rc;
    if (!IsMoreNode(node) || !heading) return;
    if ((select || TreeView_GetSelection(g_view.tree) == item) && (rows = FolderRows(heading, &count)) != NULL) {
        for (found = above; found; found = TreeView_GetPrevSibling(g_view.tree, found)) shownNow++;
        if (shownNow < count) {
            ZeroMemory(&key, sizeof key);
            StringCchCopyW(key.key, ARRAYSIZE(key.key), g_view.set.rows[rows[shownNow]].key);
            ClearMarks();
            g_view.anchor = key;
            SetSelection(&key);
            select = TRUE;
        }
        HeapFree(GetProcessHeap(), 0, rows);
    }
    NodeKey(MoreHeading(node), FALSE, &key);
    SetMoreStage(key.key, MoreStage(key.key) + 1);
    /* Those newly shown come below the rows above, even with Show more on top of the view. */
    RememberTopRow();
    NodeKey(node, FALSE, &more);
    if (SameNode(&g_view.topRow, &more) && above && TreeView_GetItemRect(g_view.tree, above, &rc, FALSE)) {
        ItemKey(above, &g_view.topRow);
        g_view.topRowOffset += rc.bottom - rc.top;
    }
    FillTree();
    if (select && (found = FindShownRow(&g_view.selection)) != NULL) TreeView_EnsureVisible(g_view.tree, found);
}

/* A folder opens or closes at once: Windows' tree slides the rows below it
 * into place (a smooth scroll) only while it draws, so its drawing is held
 * from the change's start to its end, then the tree is drawn once. A tree
 * already held (refilled, hidden) is left as it is. */
static void HoldTreeDrawing(void)
{
    if (g_view.expandHeld || !(GetWindowLongW(g_view.tree, GWL_STYLE) & WS_VISIBLE)) return;
    g_view.expandHeld = TRUE;
    SendMessageW(g_view.tree, WM_SETREDRAW, FALSE, 0);
}

static void ReleaseTreeDrawing(void)
{
    if (!g_view.expandHeld) return;
    g_view.expandHeld = FALSE;
    SendMessageW(g_view.tree, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_view.tree, NULL, NULL, RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
}

static LRESULT CALLBACK TreeSubclass(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    LRESULT result;
    (void)ref;
    switch (msg) {
    case TVM_EXPAND:   /* the program's: no TVN_ITEMEXPANDING */
        HoldTreeDrawing();
        result = DefSubclassProc(window, msg, wp, lp);
        ReleaseTreeDrawing();
        return result;
    case WM_LBUTTONDOWN: {
        TVHITTESTINFO hit;
        BOOL onRow;
        ZeroMemory(&hit, sizeof hit);
        hit.pt.x = GET_X_LPARAM(lp);
        hit.pt.y = GET_Y_LPARAM(lp);
        onRow = TreeView_HitTest(window, &hit) && (hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT));
        g_view.drag.pressKept = g_view.drag.began = FALSE;
        if (onRow && IsMoreNode(NodeParam(hit.hItem))) {   /* shown further, nothing chosen */
            SetFocus(window);
            ShowMore(hit.hItem, FALSE);
            g_view.moreClicked = GetTickCount64();
            g_view.moreClickedAt = hit.pt;
            return 0;
        }
        if (onRow && (wp & (MK_CONTROL | MK_SHIFT)) && NodeParam(hit.hItem) != NODE_NONE) {
            SetFocus(window);
            ClickToChoose(hit.hItem, (wp & MK_CONTROL) != 0, (wp & MK_SHIFT) != 0);
            return 0;
        }
        /* A plain press on one of the sessions chosen together keeps the
         * marks (so a drag carries them all, DragSessions); the press is
         * what starts a drag. Let go without one (WM_LBUTTONUP), it chooses
         * that session alone, as any plain click does. */
        if (onRow && !(wp & (MK_CONTROL | MK_SHIFT)) && NodeChosen(NodeParam(hit.hItem))) {
            g_view.drag.pressKept = TRUE;
            g_view.drag.began = FALSE;
            break;   /* the tree's own press follows: it selects and may begin a drag */
        }
        /* A plain click on a row chooses it alone, even the one already
         * selected (no change of selection to say so). */
        if (onRow && !(wp & (MK_CONTROL | MK_SHIFT))) {
            ClearMarks();
            ItemKey(hit.hItem, &g_view.anchor);
        }
        break;
    }
    case WM_LBUTTONUP:
        /* A press that kept the marks for a drag that never began: now the
         * click chooses that one session alone. */
        if (g_view.drag.pressKept) {
            TVHITTESTINFO hit;
            g_view.drag.pressKept = FALSE;
            if (!g_view.drag.began) {
                ZeroMemory(&hit, sizeof hit);
                hit.pt.x = GET_X_LPARAM(lp);
                hit.pt.y = GET_Y_LPARAM(lp);
                if (TreeView_HitTest(window, &hit) && (hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT))) {
                    ClearMarks();
                    ItemKey(hit.hItem, &g_view.anchor);
                }
            }
        }
        break;
    case WM_LBUTTONDBLCLK:
        /* A double-click on Show more shows more once: its second click
         * neither shows more nor folds the heading now under it. */
        if (GetTickCount64() - g_view.moreClicked <= GetDoubleClickTime() &&
            abs(GET_X_LPARAM(lp) - g_view.moreClickedAt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 &&
            abs(GET_Y_LPARAM(lp) - g_view.moreClickedAt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2)
            return 0;
        break;
    case WM_KEYDOWN:
        if ((wp == VK_UP || wp == VK_DOWN) && GetKeyState(VK_SHIFT) < 0) {
            ChooseWithKey(wp);
            return 0;
        }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0) {
            ChooseAllShown();
            return 0;
        }
        if (EditKey(wp)) return 0;
        break;
    case WM_CHAR:
        if (wp == 1 || EditCharacter(wp)) return 0;   /* Ctrl+A's and the edit keys' characters: no search of the titles */
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, TreeSubclass, id);
        break;
    }
    result = DefSubclassProc(window, msg, wp, lp);
    /* A folder the input asked to open that had nothing to open sends no
     * TVN_ITEMEXPANDED: the hold ends with the input. */
    ReleaseTreeDrawing();
    return result;
}

/* The archive mark before a heading: Archived and its headings. */
static BOOL ArchivedIn(LPARAM node)
{
    return node == NODE_ARCHIVED || UnderArchived(node);
}

static WCHAR EnvironmentGlyph(LPARAM node, ThemeTint *tint);

/* A session's icon before its title and its tint: the archive mark, amber,
 * when it is archived in the profile shown; else none, as Claude's sidebar
 * shows its titles (where one runs is at the row's end, EndGlyph). */
static WCHAR RowGlyphTinted(LPARAM node, ThemeTint *tint)
{
    *tint = THEME_TINT_AMBER;
    if (node >= 0 && node < g_view.set.rowCount && RowArchived(&g_view.set.rows[node], ShownProfileIndex())) return ARCHIVED_GLYPH;
    return 0;
}

/* Where a session runs and its mark's tint: the laptop on this PC; 0 for no session. */
static WCHAR EnvironmentGlyph(LPARAM node, ThemeTint *tint)
{
    const SessionEntry *entry;
    size_t i;
    if (node < 0 || node >= g_view.set.rowCount || (entry = EntryOf(&g_view.set.rows[node], ShownProfileIndex())) == NULL) return 0;
    for (i = 0; i < ARRAYSIZE(kEnvironmentMarks); i++)
        if (entry->environment & kEnvironmentMarks[i].environment) {
            *tint = kEnvironmentMarks[i].tint;
            return kEnvironmentMarks[i].glyph;
        }
    *tint = THEME_TINT_NONE;
    return LOCAL_GLYPH;
}

static WCHAR RowGlyph(LPARAM node)
{
    ThemeTint tint;
    return RowGlyphTinted(node, &tint);
}

/* A session unread in the profile shown: an orange dot at its row's right end, in place of where it runs, as Claude's sidebar marks it. */
static BOOL RowUnread(LPARAM node)
{
    return node >= 0 && node < g_view.set.rowCount && UnreadIn(EntryOf(&g_view.set.rows[node], ShownProfileIndex()));
}

static int DotSize(void)
{
    return max(4, g_view.pad * 4 / 3);
}

static int DotRoom(void)
{
    return DotSize() + g_view.pad;
}

/* The mark at the right end of a session's row, as Claude's sidebar has
 * it: where it runs (this PC's laptop, the cloud...); 0 for none. */
static WCHAR EndGlyph(LPARAM node, ThemeTint *tint)
{
    return EnvironmentGlyph(node, tint);
}

static int GlyphRoom(HDC dc, WCHAR glyph)
{
    return glyph ? Theme_InlineGlyphWidth(dc, glyph, Font(THEME_FONT_TEXT)) + g_view.pad / 2 : 0;
}

/* The room at a session row's end: its mark's, or the unread dot's in its place (as wide, so the titles end alike). */
static int EndRoom(HDC dc, LPARAM node)
{
    ThemeTint tint;
    WCHAR mark = EndGlyph(node, &tint);
    int room = GlyphRoom(dc, mark);
    return RowUnread(node) ? max(room, DotRoom()) : room;
}

/* A session's pull request, as its row shows it at its right end ("PR #12
 * merged"), in its state's color. FALSE when it shows none. */
static BOOL PullRequestText(LPARAM node, WCHAR *out, size_t cch, COLORREF *color)
{
    const SessionEntry *entry;
    const WCHAR *format;
    ThemeTint tint;
    if (!g_view.showPullRequests || node < 0 || node >= g_view.set.rowCount ||
        (entry = EntryOf(&g_view.set.rows[node], ShownProfileIndex())) == NULL)
        return FALSE;
    switch (entry->pullRequest) {
    case PR_OPEN:   format = TR(L"PR #%d open");   tint = THEME_TINT_GREEN;  break;
    case PR_DRAFT:  format = TR(L"PR #%d draft");  tint = THEME_TINT_NONE;   break;
    case PR_MERGED: format = TR(L"PR #%d merged"); tint = THEME_TINT_PURPLE; break;
    case PR_CLOSED: format = TR(L"PR #%d closed"); tint = THEME_TINT_RED;    break;
    default: return FALSE;
    }
    StringCchPrintfW(out, cch, format, entry->pullRequestNumber);
    *color = Theme_TintColor(tint);
    return TRUE;
}

/* Drawing and tooltip measurement share the title's space: a heading's
 * count after it, a session's icon before it, its waiting suffix after it,
 * its pull request and where it runs at the row's end. */
static RECT TreeTitleBounds(HDC dc, const RECT *row, int level, LPARAM node, WCHAR *tail, size_t cch)
{
    WCHAR pullRequest[64];
    COLORREF color;
    RECT title = *row;
    title.left += (level + 1) * g_view.indent + g_view.pad / 2;
    title.right -= g_view.pad;
    tail[0] = 0;
    if (IsMoreNode(node)) {
        title.left += GlyphRoom(dc, MORE_GLYPH);
        return title;
    }
    if (node < 0) {
        StringCchPrintfW(tail, cch, L"%d", FolderCount(node));
        title.right -= TextWidth(dc, THEME_FONT_TEXT, tail) + g_view.pad;
        if (ArchivedIn(node)) title.left += GlyphRoom(dc, ARCHIVED_GLYPH);
    } else if (node < g_view.set.rowCount) {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[node], ShownProfileIndex());
        StringCchCopyW(tail, cch, entry && entry->pending ? TR(L"  \x00B7  waiting") : L"");
        title.right -= TextWidth(dc, THEME_FONT_TEXT, tail);
        title.right -= EndRoom(dc, node);
        if (PullRequestText(node, pullRequest, ARRAYSIZE(pullRequest), &color)) title.right -= TextWidth(dc, THEME_FONT_TEXT, pullRequest) + g_view.pad;
        title.left += GlyphRoom(dc, RowGlyph(node));
    }
    return title;
}

/* `glyph` in the room TreeTitleBounds keeps before `title` on `row`. */
static void DrawRowGlyph(HDC dc, const RECT *row, const RECT *title, WCHAR glyph, COLORREF color)
{
    RECT mark = *row;
    if (!glyph) return;
    mark.left = title->left - GlyphRoom(dc, glyph);
    mark.right = title->left;
    Theme_DrawInlineGlyph(dc, &mark, glyph, Font(THEME_FONT_TEXT), color);
}

static int *FolderRows(HTREEITEM folder, int *count);

/* A heading with a session unread under it (in the profile shown). */
static BOOL FolderUnread(HTREEITEM folder)
{
    int count = 0, i, *rows = FolderRows(folder, &count);
    BOOL unread = FALSE;
    for (i = 0; i < count && !unread; i++) unread = RowUnread(rows[i]);
    if (rows) HeapFree(GetProcessHeap(), 0, rows);
    return unread;
}

/* One row of the tree, in the theme's rows: headings in the strong font (as
 * large as the profile list's rows) with their count in gray, Archived and
 * its headings with the archive mark and a line above Archived, an orange
 * dot after the count of one with a session unread; sessions in the text
 * font, an archived one after the amber archive mark with an amber title,
 * and at the row's end where one runs (gray laptop on this PC, the cloud
 * ...) or an orange dot when unread, as Claude's sidebar marks them; in gray a
 * session with no conversation on disk or a removal waiting, and a change
 * waiting after it; Show more in gray after a chevron.
 * A heading whose sessions are all chosen shows as chosen. The keyboard's
 * row has the focus cue. */
static void DrawTreeItem(const NMTVCUSTOMDRAW *customDraw)
{
    WCHAR tail[64], name[MAX_PATH], pullRequest[64];
    HDC dc = customDraw->nmcd.hdc;
    HTREEITEM item = (HTREEITEM)customDraw->nmcd.dwItemSpec;
    LPARAM node = customDraw->nmcd.lItemlParam;
    UINT itemState = TreeView_GetItemState(g_view.tree, item, TVIS_SELECTED | TVIS_EXPANDED), state = 0;
    BOOL hot = (customDraw->nmcd.uItemState & CDIS_HOT) != 0;
    COLORREF text, muted, field = Theme_Color(THEME_FIELD), prColor;
    RECT rc = customDraw->nmcd.rc, cell, title;
    int p = ShownProfileIndex(), left, right, baseline, bucket = NodeBucket(node);

    /* Several sessions chosen: they show as selected, and a heading whose
     * sessions all are; the tree's own selection only by its focus cue. */
    if (g_view.several ? (node >= 0 && node < g_view.set.rowCount && IsMarked(g_view.set.rows[node].key)) || (node < 0 && FolderMarked(item))
                       : (itemState & TVIS_SELECTED) != 0)
        state |= THEME_ROW_SELECTED;
    if (hot) state |= THEME_ROW_HOT;
    /* A heading's room above it stays blank, out of its selection; above
     * Archived a line parts it from what is above. */
    if (node < 0 && !IsMoreNode(node) && rc.bottom - rc.top > FOLDER_UNITS * g_view.unit) {
        RECT room = rc;
        room.bottom = rc.top = rc.bottom - FOLDER_UNITS * g_view.unit;
        Theme_DrawRow(g_view.tree, dc, &room, 0, field);
        if (node == NODE_ARCHIVED) {
            /* Ruled as the theme's menus frame themselves. */
            RECT line = room;
            line.left += g_view.pad;
            line.right -= g_view.pad;
            line.top = (room.top + room.bottom) / 2;
            line.bottom = line.top + max(1, g_view.unit / 6);
            FillRect(dc, &line, Theme_Brush(THEME_EDGE));
        }
    }
    text = Theme_DrawRow(g_view.tree, dc, &rc, state, field);
    muted = Theme_RowMuted(state);
    SetBkMode(dc, TRANSPARENT);
    cell = rc;
    cell.left = rc.left + customDraw->iLevel * g_view.indent;
    cell.right = cell.left + g_view.indent;
    right = rc.right - g_view.pad;
    title = TreeTitleBounds(dc, &rc, customDraw->iLevel, node, tail, ARRAYSIZE(tail));

    if (IsMoreNode(node)) {
        baseline = CenteredBaseline(&rc, THEME_FONT_TEXT);
        DrawRowGlyph(dc, &rc, &title, MORE_GLYPH, muted);
        TextAt(dc, THEME_FONT_TEXT, muted, MoreLabel(node), title.left, title.right, baseline);
    } else if (node < 0) {
        if (node == NODE_STARRED) StringCchCopyW(name, ARRAYSIZE(name), StarredName());
        else if (node == NODE_ARCHIVED) StringCchCopyW(name, ARRAYSIZE(name), TR(L"Archived"));
        else if (bucket >= 0 && bucket < g_view.bucketCount) StringCchCopyW(name, ARRAYSIZE(name), g_view.buckets[bucket].name);
        else name[0] = 0;
        Theme_DrawTreeGlyph(g_view.tree, dc, &cell, state, (itemState & TVIS_EXPANDED) != 0);
        if (ArchivedIn(node))
            DrawRowGlyph(dc, &rc, &title, ARCHIVED_GLYPH, (state & THEME_ROW_SELECTED) ? text : Theme_TintColor(THEME_TINT_AMBER));
        baseline = CenteredBaseline(&rc, THEME_FONT_STRONG);
        left = TextAt(dc, THEME_FONT_STRONG, text, name, title.left, title.right, baseline);
        left = TextAt(dc, THEME_FONT_TEXT, muted, tail, left + g_view.pad, right, baseline);
        if (FolderUnread(item) && left + g_view.pad + DotSize() <= right) {   /* a session unread under it, folded or not */
            RECT dot = rc;
            dot.left = left + g_view.pad;
            dot.right = dot.left + DotSize();
            Theme_DrawDot(dc, &dot, DotSize(), (state & THEME_ROW_SELECTED) ? text : Theme_TintColor(THEME_TINT_AMBER));
        }
    } else if (node < g_view.set.rowCount) {
        const SessionRow *row = &g_view.set.rows[node];
        const SessionEntry *entry = EntryOf(row, p);
        BOOL archived = entry && entry->archived, selected = (state & THEME_ROW_SELECTED) != 0;
        COLORREF titleColor = !row->transcript || (entry && entry->pendingRemove) ? muted
                            : archived && !selected ? Theme_TintColor(THEME_TINT_AMBER) : text;
        ThemeTint tint, markTint;
        WCHAR glyph = RowGlyphTinted(node, &tint), mark = EndGlyph(node, &markTint);
        baseline = CenteredBaseline(&rc, THEME_FONT_TEXT);
        DrawRowGlyph(dc, &rc, &title, glyph, selected ? text : Theme_TintColor(tint));
        left = TextAt(dc, THEME_FONT_TEXT, titleColor, ShownTitle(row, p), title.left, title.right, baseline);
        if (tail[0]) TextAt(dc, THEME_FONT_TEXT, muted, tail, left, right, baseline);
        if (RowUnread(node)) {   /* the dot in its mark's place, centered there */
            int room = EndRoom(dc, node) - g_view.pad / 2;
            RECT dot = rc;
            dot.left = right - (room + DotSize()) / 2;
            dot.right = dot.left + DotSize();
            Theme_DrawDot(dc, &dot, DotSize(), selected ? text : Theme_TintColor(THEME_TINT_AMBER));
            right -= EndRoom(dc, node);
        } else if (mark) {
            RECT where = rc;
            where.left = right - Theme_InlineGlyphWidth(dc, mark, Font(THEME_FONT_TEXT));
            where.right = right;
            Theme_DrawInlineGlyph(dc, &where, mark, Font(THEME_FONT_TEXT),
                                  selected ? text : markTint == THEME_TINT_NONE ? muted : Theme_TintColor(markTint));
            right -= GlyphRoom(dc, mark);
        }
        if (PullRequestText(node, pullRequest, ARRAYSIZE(pullRequest), &prColor)) {
            int width = TextWidth(dc, THEME_FONT_TEXT, pullRequest);
            TextAt(dc, THEME_FONT_TEXT, selected ? text : prColor, pullRequest, right - width, right, baseline);
        }
    }
    if (item == TreeView_GetSelection(g_view.tree) && GetFocus() == g_view.tree) Theme_DrawFocusCue(g_view.tree, dc, &rc);
}

/* Why the tree is empty. */
static void EmptyText(WCHAR *out, size_t cch)
{
    int p = ShownProfileIndex(), r, listed = 0, archived = 0, chosen = 0;
    const SessionSource *source;
    const WCHAR *name;
    if (!g_view.loaded && g_view.loadFailed) {
        StringCchCopyW(out, cch, TR(L"Sessions could not be loaded."));
        return;
    }
    if (!g_view.loaded && g_view.loading) {
        StringCchCopyW(out, cch, TR(L"Loading sessions\x2026"));
        return;
    }
    if (p < 0) {
        StringCchCopyW(out, cch, TR(L"No profile yet."));
        return;
    }
    source = &g_view.set.source[p];
    name = ProfileAt(p)->name;
    for (r = 0; r < g_view.set.rowCount; r++) {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[r], p);
        if (!entry) continue;
        listed++;
        if (entry->archived) archived++;
        if (EntryChosen(entry)) chosen++;
    }
    if (chosen && g_view.filter[0] && (g_view.searchIn & SEARCH_REGEX) && !g_view.regex)
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D is not a regular expression this search takes."), g_view.filter);
    else if (chosen && g_view.filter[0] && g_view.textSearching)
        StringCchPrintfW(out, cch, TR(L"Searching the conversations of \x201C%s\x201D for \x201C%s\x201D\x2026"), name, g_view.filter);
    else if (chosen && g_view.filter[0])
        StringCchPrintfW(out, cch, TR(L"No session of \x201C%s\x201D matches \x201C%s\x201D."), name, g_view.filter);
    else if (listed && !chosen && g_view.status == STATUS_ACTIVE && archived == listed)
        StringCchPrintfW(out, cch, TR(L"All the sessions of \x201C%s\x201D are archived.\nChoose Archived or All under Status in the filter menu to see them."),
                         name);
    else if (listed && !chosen && g_view.status == STATUS_ARCHIVED && !archived)
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D has no archived session."), name);
    else if (listed && !chosen && g_view.resultFilter)
        StringCchCopyW(out, cch, TR(L"No session matches the selected result and other filters."));
    else if (listed && !chosen)
        StringCchPrintfW(out, cch, TR(L"No session of \x201C%s\x201D runs in the environments chosen in the filter menu."), name);
    else if (source->elsewhere)
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D only has sessions run over SSH, in WSL or in the cloud.\n"
                                      L"Their conversation is not on this PC, so they are not listed."), name);
    else if (source->unreadable)
        StringCchPrintfW(out, cch, TR(L"The sessions of \x201C%s\x201D could not be read."), name);
    else if (!source->signedIn)
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D is not signed in to Claude yet.\n"
                                      L"Once it is, the sessions started in its Code tab show here."), name);
    else
        StringCchPrintfW(out, cch, TR(L"\x201C%s\x201D has no Claude Code sessions yet.\n"
                                      L"The sessions started in its Code tab show here."), name);
}

static void DrawEmptyTree(HDC dc)
{
    WCHAR text[512];
    RECT rc;
    HGDIOBJ old = SelectObject(dc, Font(THEME_FONT_TEXT));
    GetClientRect(g_view.tree, &rc);
    InflateRect(&rc, -g_view.pad * 3, -g_view.pad * 3);
    EmptyText(text, ARRAYSIZE(text));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Theme_Color(THEME_MUTED));
    DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | Localize_ReadingFlags());
    SelectObject(dc, old);
}

static LRESULT TreeCustomDraw(NMTVCUSTOMDRAW *customDraw)
{
    switch (customDraw->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPREPAINT:
        /* An empty rectangle requests the row's font and title bounds for
         * label and tooltip measurement, without drawing it. */
        if (IsRectEmpty(&customDraw->nmcd.rc)) {
            RECT row = { 0 };
            WCHAR tail[64];
            HTREEITEM parent;
            int level = 0;
            SelectObject(customDraw->nmcd.hdc, Font(customDraw->nmcd.lItemlParam < 0 && !IsMoreNode(customDraw->nmcd.lItemlParam)
                                                    ? THEME_FONT_STRONG : THEME_FONT_TEXT));
            if (GetClientRect(g_view.tree, &row)) {
                for (parent = TreeView_GetParent(g_view.tree, (HTREEITEM)customDraw->nmcd.dwItemSpec); parent;
                     parent = TreeView_GetParent(g_view.tree, parent)) level++;
                customDraw->nmcd.rc = TreeTitleBounds(customDraw->nmcd.hdc, &row, level, customDraw->nmcd.lItemlParam, tail, ARRAYSIZE(tail));
            }
            return CDRF_NEWFONT;
        }
        DrawTreeItem(customDraw);
        return CDRF_SKIPDEFAULT;
    case CDDS_POSTPAINT:
        if (!TreeView_GetCount(g_view.tree)) DrawEmptyTree(customDraw->nmcd.hdc);
        return CDRF_DODEFAULT;
    }
    return CDRF_DODEFAULT;
}

/* ------------------------------------------------------ names and folders */

/* The selected session's folder as Explorer opens it, "" when it is not on
 * disk: looked up once per working folder selected, not at every paint, for
 * every session of a folder nor with every snapshot (a folder on a share
 * that went away answers slowly). */
static const WCHAR *SelectedFolder(void)
{
    int r = SelectedRow();
    const WCHAR *cwd = r >= 0 ? g_view.set.rows[r].cwd : L"";
    if (g_view.folderLookedUp && wcscmp(cwd, g_view.folderLookedUpFor) == 0) return g_view.selectedFolder;
    g_view.folderLookedUp = TRUE;
    StringCchCopyW(g_view.folderLookedUpFor, ARRAYSIZE(g_view.folderLookedUpFor), cwd);
    g_view.selectedFolder[0] = 0;
    if (cwd[0] && (!SessionStore_WorkingDir(&g_view.set, cwd, g_view.selectedFolder, ARRAYSIZE(g_view.selectedFolder)) ||
                   !Util_DirExists(g_view.selectedFolder)))
        g_view.selectedFolder[0] = 0;
    return g_view.selectedFolder;
}

/* --------------------------------------------------------------- actions */

typedef struct TitleDialog {             /* the Rename session dialog: a session's title in one profile */
    const WCHAR *profile;
    WCHAR        title[SESSION_TITLE_CCH];
} TitleDialog;

/* The title typed in the dialog, without the blanks around it: where it
 * starts in `typed`, and how long it is (0: only blanks). */
static size_t TypedTitle(HWND dialog, WCHAR *typed, size_t cch, const WCHAR **start)
{
    size_t length;
    GetDlgItemTextW(dialog, IDC_T_TITLE, typed, (int)cch);
    for (*start = typed; **start == L' ' || **start == L'\t'; (*start)++) {}
    for (length = wcslen(*start); length > 0 && ((*start)[length - 1] == L' ' || (*start)[length - 1] == L'\t'); length--) {}
    return length;
}

static INT_PTR CALLBACK TitleProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    TitleDialog *request = (TitleDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    WCHAR label[128], typed[SESSION_TITLE_CCH];
    const WCHAR *start;
    size_t length;
    switch (msg) {
    case WM_INITDIALOG:
        request = (TitleDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        StringCchPrintfW(label, ARRAYSIZE(label), TR(L"Title in \x201C%s\x201D:"), request->profile);
        SetDlgItemTextW(dialog, IDC_T_LABEL, label);
        SendDlgItemMessageW(dialog, IDC_T_TITLE, EM_LIMITTEXT, SESSION_TITLE_CCH - 1, 0);
        SetDlgItemTextW(dialog, IDC_T_TITLE, request->title);
        SendDlgItemMessageW(dialog, IDC_T_TITLE, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dialog, IDC_T_TITLE));
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_T_TITLE && HIWORD(wp) == EN_CHANGE) {
            EnableWindow(GetDlgItem(dialog, IDOK), TypedTitle(dialog, typed, ARRAYSIZE(typed), &start) > 0);   /* only blanks: no title */
        } else if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            if (LOWORD(wp) == IDOK) {
                if ((length = TypedTitle(dialog, typed, ARRAYSIZE(typed), &start)) == 0) return TRUE;
                StringCchCopyNW(request->title, ARRAYSIZE(request->title), start, length);
            }
            EndDialog(dialog, LOWORD(wp));
        }
        return TRUE;
    }
    return FALSE;
}

static BOOL CanOpenConversation(const SessionRow *row, int target)
{
    if (!row->transcript || !Core_IsUuid(row->key)) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"This session has no conversation on disk yet: "
                                                      L"it can only be opened from the Code tab of the profile where it was started."));
        return FALSE;
    }
    if (target >= 0 && !g_view.set.source[target].signedIn) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"\x201C%s\x201D is not signed in to Claude yet: sign in there first."),
                   ProfileAt(target)->name);
        return FALSE;
    }
    if (!Package()->found) {
        Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"Claude Desktop is not installed."));
        return FALSE;
    }
    return TRUE;
}

/* Session `key` opened in profile `p` by its Claude (started first when
 * closed); FALSE, the user told, when it could not be. */
static BOOL OpenConversationIn(int p, const WCHAR *key)
{
    HRESULT hr = SessionEdit_Open(Package(), ProfileAt(p), key);
    if (SUCCEEDED(hr)) return TRUE;
    Ui_Message(g_view.dlg, MB_ICONERROR, TR(L"The session could not be opened in \x201C%s\x201D (error 0x%08lX)."), ProfileAt(p)->name,
               (unsigned long)hr);
    return FALSE;
}

static void MakeEdit(PendingEdit *edit, PendingOp op, const WCHAR *key, const WCHAR *value)
{
    ZeroMemory(edit, sizeof *edit);
    edit->op = op;
    StringCchCopyW(edit->key, ARRAYSIZE(edit->key), key);
    StringCchCopyW(edit->value, ARRAYSIZE(edit->value), value);
}

/* `reason`: why, when SessionEdit says ("" when it does not). */
static void TellNotChanged(int p, const WCHAR *reason)
{
    if (reason[0]) Ui_Message(g_view.dlg, MB_ICONERROR, TR(L"The session could not be changed in \x201C%s\x201D. %s"), ProfileAt(p)->name, reason);
    else Ui_Message(g_view.dlg, MB_ICONERROR, TR(L"The session could not be changed in \x201C%s\x201D."), ProfileAt(p)->name);
}

static BOOL OpenSession(int r, int p)
{
    return CanOpenConversation(&g_view.set.rows[r], -1) && OpenConversationIn(p, g_view.set.rows[r].key);
}

/* One session shared with or copied to `target` as several are
 * (sessionsync.c): its entry goes whole, so it keeps its title, its star,
 * whether it is archived and what else Claude keeps in it. */
static BOOL ShareSession(int r, int target)
{
    return SyncUi_ShareOrCopy(g_view.dlg, &g_view.set, ShownProfileIndex(), &r, 1, FALSE, target);
}


/* A change to the entry of profile `p`, made now or when it closes, and
 * kept for Ctrl+Z with what it replaces (the entry itself for a removal);
 * FALSE, the user told, when it could not be. */
static BOOL ChangeEntry(int r, int p, PendingOp op, const WCHAR *value)
{
    const SessionRow *row = &g_view.set.rows[r];
    const SessionEntry *entry = EntryOf(row, p);
    PendingEdit edit;
    WCHAR reason[LONG_PATH_CCH + NOTE_CCH], before[SESSION_TITLE_CCH];
    BOOL waiting, starred = StarredIn(entry), unread = UnreadIn(entry), beforeAuto = FALSE;
    char *content = NULL;
    DWORD length = 0;
    before[0] = 0;
    if (entry && op == PENDING_TITLE) {
        StringCchCopyW(before, ARRAYSIZE(before), entry->pendingTitle[0] ? entry->pendingTitle : entry->title);
        beforeAuto = !entry->pendingTitle[0] && entry->autoTitle;
    }
    if (entry && op == PENDING_REMOVE) content = Util_ReadFile(entry->file, SESSION_ENTRY_MAX_BYTES, FALSE, &length);
    MakeEdit(&edit, op, row->key, value);
    if (!SessionEdit_Change(g_view.dlg, ProfileAt(p), entry, &edit, &waiting, reason, ARRAYSIZE(reason))) {
        if (content) HeapFree(GetProcessHeap(), 0, content);
        TellNotChanged(p, reason);
        return FALSE;
    }
    if (op == PENDING_STAR) SessionUndo_Star(ProfileAt(p), row->key, starred, value[0] == L'1');
    else if (op == PENDING_TITLE) SessionUndo_Title(ProfileAt(p), row->key, before, beforeAuto, value);
    else if (op == PENDING_READ) SessionUndo_Read(ProfileAt(p), row->key, unread, value[0] == L'0');
    else if (op == PENDING_REMOVE) SessionUndo_Listed(ProfileAt(p), row->key, FALSE, content, length);
    return TRUE;
}

/* What a sync group keeps the same that a change of kind `op` changes. */
static DWORD SyncItemOf(PendingOp op)
{
    switch (op) {
    case PENDING_TITLE:  return SYNC_ITEM_SESSIONS_TITLES;
    case PENDING_STAR:   return SYNC_ITEM_SIDEBAR_PINS;
    case PENDING_REMOVE: return SYNC_ITEM_SESSIONS_DELETED;
    case PENDING_READ:   return SYNC_ITEM_DETAILS_UNREAD;
    default:             return 0;
    }
}

/* The other profiles of `p`'s sync group that list session `row` and keep
 * what a change of kind `op` changes the same: the change made in `p` is
 * made in them at once, as their next sync would. One bit each. */
static DWORD SyncedWith(const SessionRow *row, int p, PendingOp op)
{
    const Profile *profile = ProfileAt(p);
    DWORD group, others = 0;
    int q;
    if (g_view.onlyShown || !profile->syncGroup || !SyncItemOf(op)) return 0;
    group = SessionVault_Group(&g_view.set.profiles, profile->syncGroup);
    if (!(SessionVault_GroupItems(&g_view.set.profiles, group) & SyncItemOf(op))) return 0;
    for (q = 0; q < g_view.set.profiles.count; q++)
        if (q != p && (group & (1u << q)) && ListedAndKept(EntryOf(row, q))) others |= 1u << q;
    return others;
}

/* ChangeEntry in profile `p`, then in the profiles of its sync group that
 * keep the same what it changes; one Ctrl+Z undoes them all. */
static BOOL ChangeSynced(int r, int p, PendingOp op, const WCHAR *value)
{
    DWORD others = SyncedWith(&g_view.set.rows[r], p, op);
    int q;
    if (!ChangeEntry(r, p, op, value)) return FALSE;
    for (q = 0; q < g_view.set.profiles.count; q++)
        if ((others & (1u << q)) && !ChangeEntry(r, q, op, value)) break;
    return TRUE;
}

/* A Remote Control session turned into a plain local one in profile `p`: its
 * conversation is already on this PC, so only the entry changes (the fields
 * that make Claude bridge it to claude.ai are taken away). Queued when the
 * profile is open, and then not individually undone. */
static BOOL ConvertEntry(int r, int p)
{
    const SessionRow *row = &g_view.set.rows[r];
    const SessionEntry *entry = EntryOf(row, p);
    PendingEdit edit;
    WCHAR reason[LONG_PATH_CCH + NOTE_CCH];
    BOOL waiting;
    char *before, *after;
    DWORD beforeLength = 0, afterLength = 0;
    if (!entry || !entry->file[0]) return FALSE;
    before = Util_ReadFile(entry->file, SESSION_ENTRY_MAX_BYTES, FALSE, &beforeLength);
    MakeEdit(&edit, PENDING_UNREMOTE, row->key, L"");
    if (!SessionEdit_Change(g_view.dlg, ProfileAt(p), entry, &edit, &waiting, reason, ARRAYSIZE(reason))) {
        if (before) HeapFree(GetProcessHeap(), 0, before);
        TellNotChanged(p, reason);
        return FALSE;
    }
    /* Made at once (closed profile): the before and after entry recorded, so Ctrl+Z puts Remote Control back. */
    if (!waiting && before && (after = Util_ReadFile(entry->file, SESSION_ENTRY_MAX_BYTES, FALSE, &afterLength)) != NULL)
        SessionUndo_Replaced(ProfileAt(p), row->key, before, beforeLength, after, afterLength);
    else if (before)
        HeapFree(GetProcessHeap(), 0, before);
    return TRUE;
}

/* The title of the session in profile `p`, changed when another is typed. */
static BOOL RenameEntry(int r, int p)
{
    TitleDialog request;
    ZeroMemory(&request, sizeof request);
    request.profile = ProfileAt(p)->name;
    StringCchCopyW(request.title, ARRAYSIZE(request.title), TitleIn(&g_view.set.rows[r], p));
    if (Ui_Dialog(g_view.dlg, IDD_TITLE, TitleProc, (LPARAM)&request) != IDOK) return FALSE;
    /* The same title written again would only mark it as chosen: Claude would stop naming the session. */
    return wcscmp(request.title, TitleIn(&g_view.set.rows[r], p)) != 0 && ChangeSynced(r, p, PENDING_TITLE, request.title);
}

static BOOL RemoveEntry(int r, int p)
{
    const SessionRow *row = &g_view.set.rows[r];
    const WCHAR *title = ShownTitle(row, p), *name = ProfileAt(p)->name;
    WCHAR text[1024 + MAX_PROFILES * LABEL_CCH], names[MAX_PROFILES * (LABEL_CCH + 8)];
    DWORD synced = SyncedWith(row, p, PENDING_REMOVE);
    if (synced) {
        /* Its sync group's profiles lose it too. */
        Profiles_JoinNames(&g_view.set.profiles, synced | (1u << p), names, ARRAYSIZE(names));
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Delete \x201C%s\x201D from %s?\n\nThey sync with each other. Its entries go to the Recycle Bin, each once its profile is closed."),
                         title, names);
        return Ui_Ask(g_view.dlg, IDI_QUESTION, text, TR(L"Delete"), TR(L"Cancel"), FALSE) && ChangeSynced(r, p, PENDING_REMOVE, L"");
    }
    if (ProfileAt(p)->running)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Delete \x201C%s\x201D from \x201C%s\x201D?\n\nIts entry goes to the Recycle Bin once %s closes."),
                         title, name, name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Delete \x201C%s\x201D from \x201C%s\x201D?\n\nIts entry goes to the Recycle Bin."), title, name);
    return Ui_Ask(g_view.dlg, IDI_QUESTION, text, TR(L"Delete"), TR(L"Cancel"), FALSE) && ChangeEntry(r, p, PENDING_REMOVE, L"");
}

/* The waiting removal of the entries of profile `p` dropped: queued under
 * the session's id, or under the own id of one of its entries there (made
 * before the session's first message), in one go. */
static BOOL KeepEntry(int r, int p)
{
    const SessionRow *row = &g_view.set.rows[r];
    PendingEdit *edits;
    int entry, count = 1;
    BOOL kept;
    for (entry = row->entry[p]; entry >= 0; entry = g_view.set.entries[entry].duplicate) count++;
    if ((edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *edits)) == NULL) {
        TellNotChanged(p, L"");
        return FALSE;
    }
    count = 0;
    MakeEdit(&edits[count++], PENDING_REMOVE, row->key, L"");
    for (entry = row->entry[p]; entry >= 0; entry = g_view.set.entries[entry].duplicate)
        MakeEdit(&edits[count++], PENDING_REMOVE, g_view.set.entries[entry].localId, L"");
    kept = SessionEdit_Cancel(ProfileAt(p), edits, count);
    HeapFree(GetProcessHeap(), 0, edits);
    if (!kept) {
        TellNotChanged(p, L"");
        return FALSE;
    }
    if (row->entry[p] >= 0) {
        DWORD length = 0;
        char *content = Util_ReadFile(g_view.set.entries[row->entry[p]].file, SESSION_ENTRY_MAX_BYTES, FALSE, &length);
        SessionUndo_Listed(ProfileAt(p), row->key, TRUE, content, length);
    }
    return TRUE;
}

/* Its conversation, its entries and, for a session without a folder, its
 * working folder to the Recycle Bin. Whatever runs it is said before the
 * question; SessionEdit checks again before anything goes. Not undone:
 * the Recycle Bin gives it back. */
static BOOL DeleteSessionEverywhere(int r)
{
    const SessionRow *row = &g_view.set.rows[r];
    const WCHAR *question;
    WCHAR text[4096], names[NAMES_CCH], error[LONG_PATH_CCH + NOTE_CCH], working[MAX_PATH];
    DWORD listing = ProfilesListing(row);
    RemoveResult result;
    BOOL several = Core_BitCount(listing) > 1, folder;
    if (!SessionEdit_CanDelete(&g_view.set, r, error, ARRAYSIZE(error))) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, L"%s", error);
        return FALSE;
    }
    folder = SessionEdit_RemovesWorkingFolder(&g_view.set, r, working, ARRAYSIZE(working));
    Profiles_JoinNames(&g_view.set.profiles, listing, names, ARRAYSIZE(names));
    if (row->transcript && folder)
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation, its working folder and its entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation, its working folder and its entry in %s go to the Recycle Bin.");
    else if (row->transcript)
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation and its entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts conversation and its entry in %s go to the Recycle Bin.");
    else if (folder)
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts working folder and its entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts working folder and its entry in %s go to the Recycle Bin.");
    else
        question = several ? TR(L"Delete \x201C%s\x201D everywhere?\n\nIts entries in %s go to the Recycle Bin.")
                           : TR(L"Delete \x201C%s\x201D everywhere?\n\nIts entry in %s goes to the Recycle Bin.");
    StringCchPrintfW(text, ARRAYSIZE(text), question, ShownTitle(row, ShownProfileIndex()), names);
    if (!Ui_Ask(g_view.dlg, IDI_WARNING, text, TR(L"Delete"), TR(L"Cancel"), TRUE)) return FALSE;
    result = SessionEdit_DeleteEverywhere(g_view.dlg, &g_view.set, r, error, ARRAYSIZE(error));
    if (result == REMOVE_DONE) SessionUndo_Forget(row->key);
    if (result == REMOVE_FAILED) Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"The session could not be deleted. %s"), error);
    return result != REMOVE_CANCELLED;
}

/* The conversation of transcript `path` as the preview shows it: who
 * speaks, then what (Core_TranscriptLine), CRLF between lines. A heap block
 * (HeapFree it); NULL when it cannot be read. */
static WCHAR *PreviewText(const WCHAR *path, ULONGLONG bytes)
{
    DWORD length = 0;
    char *content = Util_ReadFile(path, PREVIEW_READ_MAX, TRUE, &length), *line, *end = NULL;
    WCHAR *text = NULL, *message = NULL;
    TranscriptRole last = TRANSCRIPT_NONE;
    size_t used;
    if (!content) return NULL;
    text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, PREVIEW_TEXT_CCH * sizeof(WCHAR));
    message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, PREVIEW_LINE_CCH * sizeof(WCHAR));
    if (!text || !message) goto done;
    text[0] = 0;
    line = content;
    if (bytes > PREVIEW_READ_MAX) {
        /* Read from its middle: from the first whole line. */
        if ((line = strchr(content, '\n')) == NULL) line = content + length;
        else line++;
        StringCchPrintfW(text, PREVIEW_TEXT_CCH, L"%s\r\n", TR(L"Only the end of this long conversation is shown."));
    }
    for (; *line; line = *end ? end + 1 : end) {
        TranscriptRole role;
        WCHAR head[64];
        if ((end = strchr(line, '\n')) == NULL) end = line + strlen(line);
        if ((role = Core_TranscriptLine(line, (size_t)(end - line), message, PREVIEW_LINE_CCH)) == TRANSCRIPT_NONE) continue;
        if (role != last) {
            StringCchPrintfW(head, ARRAYSIZE(head), L"%s%s  %s\r\n", text[0] ? L"\r\n\r\n" : L"",
                             role == TRANSCRIPT_USER ? L"\x25B6" : L"\x25C6", role == TRANSCRIPT_USER ? TR(L"You") : L"Claude");
            last = role;
        } else {
            StringCchCopyW(head, ARRAYSIZE(head), L"\r\n");
        }
        if (FAILED(StringCchCatW(text, PREVIEW_TEXT_CCH, head)) || FAILED(StringCchCatW(text, PREVIEW_TEXT_CCH, message))) {
            used = wcslen(text);
            if (used > 1) text[used - 1] = 0x2026;   /* full: cut */
            break;
        }
    }
    if (!text[0]) StringCchCopyW(text, PREVIEW_TEXT_CCH, TR(L"This conversation has nothing to show yet."));
done:
    if (message) HeapFree(GetProcessHeap(), 0, message);
    if (content) HeapFree(GetProcessHeap(), 0, content);
    if (text && !text[0]) {
        HeapFree(GetProcessHeap(), 0, text);
        text = NULL;
    }
    return text;
}

typedef struct PreviewDialog {
    const WCHAR *title;
    const WCHAR *text;
} PreviewDialog;

static INT_PTR CALLBACK PreviewProc(HWND dialog, UINT msg, WPARAM wp, LPARAM lp)
{
    const PreviewDialog *preview = (const PreviewDialog *)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dialog, IDC_V_TITLE, preview->title);
        SendDlgItemMessageW(dialog, IDC_V_TEXT, EM_SETLIMITTEXT, 0, 0);
        SetDlgItemTextW(dialog, IDC_V_TEXT, preview->text);
        SetFocus(GetDlgItem(dialog, IDC_V_TEXT));
        SendDlgItemMessageW(dialog, IDC_V_TEXT, EM_SETSEL, 0, 0);
        return FALSE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) EndDialog(dialog, LOWORD(wp));
        return TRUE;
    }
    return FALSE;
}

static void PreviewSession(int r);

/* The conversation of session `r`, read only: as a web page in the
 * program's browser window (WebView2) where there is one, else as text. */
static void ShowSession(int r)
{
    WCHAR page[MAX_PATH], title[SESSION_TITLE_CCH + 32];
    HCURSOR old;
    HRESULT hr = E_FAIL;
    if (g_view.set.rows[r].transcript && WebSignIn_Available()) {
        old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        if (SessionTools_PreviewPage(&g_view.set, r, page, ARRAYSIZE(page))) {
            StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Preview: %s"), ShownTitle(&g_view.set.rows[r], ShownProfileIndex()));
            hr = WebView_ShowPage(g_view.dlg, title, page);
        }
        SetCursor(old);
        if (SUCCEEDED(hr)) return;
        Util_Log(L"session preview: the web page could not be shown (0x%08lX); shown as text", (unsigned long)hr);
    }
    PreviewSession(r);
}

/* The conversation of session `r`, read only, in a window of its own. */
static void PreviewSession(int r)
{
    const SessionRow *row = &g_view.set.rows[r];
    PreviewDialog preview;
    WCHAR *text;
    HCURSOR old;
    if (!row->transcript) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"This session has no conversation on disk yet."));
        return;
    }
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    text = PreviewText(row->transcriptPath, row->transcriptBytes);
    SetCursor(old);
    if (!text) {
        Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"The conversation could not be read: %s"), row->transcriptPath);
        return;
    }
    preview.title = ShownTitle(row, ShownProfileIndex());
    preview.text = text;
    Ui_Dialog(g_view.dlg, IDD_PREVIEW, PreviewProc, (LPARAM)&preview);
    HeapFree(GetProcessHeap(), 0, text);
}

static void ShowMatchingContext(HWND owner, int r)
{
    ULONGLONG offset;
    size_t messageEnd;
    const SessionRow *row = &g_view.set.rows[r];
    WCHAR *context = NULL;
    HCURSOR old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (SessionSearch_Locate(row->transcriptPath, g_view.filter, (g_view.searchIn & SEARCH_REGEX) != 0,
                             (g_view.searchIn & SEARCH_CASE) != 0, &offset, &messageEnd))
        context = SessionTools_Context(row->transcriptPath, offset, g_view.filter, (g_view.searchIn & SEARCH_REGEX) != 0,
                                       (g_view.searchIn & SEARCH_CASE) != 0);
    SetCursor(old);
    if (context) {
        Ui_ReviewText(owner, TR(L"First matching message"), TR(L"Local context of the first match. Long messages are excerpted around the match."), context, NULL);
        HeapFree(GetProcessHeap(), 0, context);
    } else Ui_Message(owner, MB_ICONINFORMATION, TR(L"No matching message could be read. The conversation may have changed."));
}

/* `folder` shown in Explorer; the user told when it cannot be. */
static void ShowFolder(const WCHAR *folder)
{
    INT_PTR result = (INT_PTR)ShellExecuteW(g_view.dlg, L"explore", folder, NULL, NULL, SW_SHOWNORMAL);
    if (result > 32) return;   /* ShellExecute's success */
    Util_Log(L"sessions: %s could not be shown (code %ld)", folder, (long)result);
    Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"The folder %s could not be shown (error %ld)."), folder, (long)result);
}

/* Once an action's dialogs and menus have closed: the snapshot they held
 * back is taken through the manager's message, which checks that the
 * manager may still change what it shows (it may be closing). */
static void TakeHeldSnapshot(void)
{
    if (!g_view.actionDepth) PostMessageW(g_view.dlg, WM_APP_SESSIONS_READY, 0, 0);
}

/* An action on the selected session in profile `p` (-1: the profile shown).
 * What it changed is read again at once: a read begun before is out of date.
 * What Claude does once a session opens, the watcher sees. */
static void RunAction(Action action, int p)
{
    int r = SelectedRow();
    BOOL changed = FALSE;
    if (r < 0) return;
    if (p < 0) p = ShownProfileIndex();
    if (p < 0 || p >= g_view.set.profiles.count) return;
    g_view.actionDepth++;
    SessionUndo_Begin(&g_view.set);
    switch (action) {
    case ACT_OPEN:
        if (ListedAndKept(EntryOf(&g_view.set.rows[r], p))) OpenSession(r, p);
        break;
    case ACT_RENAME:      changed = RenameEntry(r, p); break;
    case ACT_STAR:        changed = ChangeSynced(r, p, PENDING_STAR, StarredIn(EntryOf(&g_view.set.rows[r], p)) ? L"0" : L"1"); break;
    case ACT_READ:        changed = ChangeSynced(r, p, PENDING_READ, UnreadIn(EntryOf(&g_view.set.rows[r], p)) ? L"1" : L"0"); break;
    case ACT_REMOVE:      changed = RemoveEntry(r, p); break;
    case ACT_KEEP:        changed = KeepEntry(r, p); break;
    case ACT_SHARE:       changed = ShareSession(r, p); break;
    case ACT_SHOW_FOLDER:
        if (SelectedFolder()[0]) ShowFolder(SelectedFolder());
        break;
    case ACT_PREVIEW:     ShowSession(r); break;
    case ACT_BRANCHES: {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[r], p);
        if (entry) SessionTools_Details(g_view.dlg, entry->file, 1);
        break;
    }
    case ACT_EXPORT_TEXT:
    case ACT_EXPORT_MARKDOWN:
    case ACT_EXPORT_HTML:
    case ACT_EXPORT_JSON:
    case ACT_EXPORT_ORIGINAL:
        SessionTools_Export(g_view.dlg, &g_view.set, r, (int)(action - ACT_EXPORT_TEXT));
        break;
    case ACT_DELETE_ALL:  changed = DeleteSessionEverywhere(r); break;
    case ACT_TO_LOCAL:    changed = ConvertEntry(r, p); break;
    default: break;
    }
    SessionUndo_End();
    g_view.actionDepth--;
    if (changed) RequestLoad(TRUE);
    TakeHeldSnapshot();
}

/* A profile's name for a menu: an & shown as itself. */
static void EscapeAmpersands(const WCHAR *text, WCHAR *out, size_t cch)
{
    const WCHAR *ampersand;
    out[0] = 0;
    while ((ampersand = wcschr(text, L'&')) != NULL) {
        if (FAILED(StringCchCatNW(out, cch, text, (size_t)(ampersand - text) + 1)) || FAILED(StringCchCatW(out, cch, L"&"))) return;
        text = ampersand + 1;
    }
    StringCchCatW(out, cch, text);
}

static UINT MenuId(Action action, int p)
{
    return IDM_ACTION + (UINT)action * MAX_PROFILES + (UINT)p;
}

/* A menu's choice, run. */
static void RunCommand(UINT cmd)
{
    if (cmd >= IDM_ACTION && cmd < IDM_ACTION + ACTIONS * MAX_PROFILES)
        RunAction((Action)((cmd - IDM_ACTION) / MAX_PROFILES), (int)((cmd - IDM_ACTION) % MAX_PROFILES));
}

/* What can be done to the entry of session `row` in profile `p` (`name` as
 * a menu shows it), in the order of Claude's menu: pin, rename, delete
 * (from that profile when others keep it), or keep it when its removal
 * waits; a remote session turned into a local one. In the tree, with the
 * keys that do it. */
static void AppendEntryActions(HMENU menu, const SessionRow *row, int p, const WCHAR *name)
{
    WCHAR text[LABEL_CCH * 2 + 48];
    const SessionEntry *entry = EntryOf(row, p);
    if (entry->pendingRemove) {
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"&Keep in %s"), name);
        AppendMenuW(menu, MF_STRING, MenuId(ACT_KEEP, p), text);
        return;
    }
    Theme_AppendMenu(menu, MF_STRING, MenuId(ACT_STAR, p), StarredIn(entry) ? TR(L"Un&pin") : TR(L"&Pin"), 0xE718);
    Theme_AppendMenu(menu, MF_STRING, MenuId(ACT_READ, p), UnreadIn(entry) ? TR(L"&Mark as read") : TR(L"&Mark as unread"), 0xE8C3);
    Theme_AppendMenu(menu, MF_STRING, MenuId(ACT_RENAME, p), TR(L"&Rename\x2026\tF2"), 0xE8AC);
    if (entry->environment == SESSION_ENV_REMOTE)
        Theme_AppendMenu(menu, MF_STRING, MenuId(ACT_TO_LOCAL, p), TR(L"Turn into a l&ocal session"), 0xE7F8);
}

/* Delete, last as in Claude's menu, red: out of profile `p` (and those of
 * its sync group), or Keep when its removal waits. */
static void AppendEntryDelete(HMENU menu, const SessionRow *row, int p, const WCHAR *name)
{
    WCHAR text[LABEL_CCH * 2 + 48];
    const SessionEntry *entry = EntryOf(row, p);
    if (!entry || entry->pendingRemove) return;
    if (ProfilesKeeping(row, p) & ~SyncedWith(row, p, PENDING_REMOVE)) StringCchPrintfW(text, ARRAYSIZE(text), TR(L"&Delete from %s\x2026\tDel"), name);
    else StringCchCopyW(text, ARRAYSIZE(text), TR(L"&Delete\x2026\tDel"));
    Theme_AppendMenu(menu, MF_STRING, MenuId(ACT_REMOVE, p), text, 0xE74D);
    Theme_SetMenuDanger(menu, MenuId(ACT_REMOVE, p));
}


static void RunBatch(UINT cmd, const int *rows, int count);
static UINT TrackMenu(HMENU menu, POINT pt);

/* The selected session's actions, at `pt` (screen): Claude's first, in its
 * order (Open in, the profile shown first and the default; Pin, Rename,
 * Delete), then the manager's: the preview, sharing and copying to other
 * profiles, the clipboard, what the session keeps, its folder, and deleting
 * it everywhere. A profile whose entry waits for its removal is not one to
 * open it in, nor to share it with. */
static void ShowMenu(POINT pt)
{
    WCHAR text[LABEL_CCH * 2 + 48], name[LABEL_CCH * 2];
    HMENU menu = CreatePopupMenu(), openIn = CreatePopupMenu(), shareWith = CreatePopupMenu(), reading = CreatePopupMenu();
    int r = SelectedRow(), p = ShownProfileIndex(), otherProfile, openable = 0, unlisted = 0;
    const SessionRow *row;
    const SessionEntry *entry;
    UINT cmd, transcript;
    if (!menu || !openIn || !shareWith || !reading || r < 0 || p < 0) {
        if (menu) DestroyMenu(menu);
        if (openIn) DestroyMenu(openIn);
        if (shareWith) DestroyMenu(shareWith);
        if (reading) DestroyMenu(reading);
        return;
    }
    row = &g_view.set.rows[r];
    entry = EntryOf(row, p);
    transcript = row->transcript ? 0 : MF_GRAYED;
    EscapeAmpersands(ProfileAt(p)->name, name, ARRAYSIZE(name));
    if (ListedAndKept(entry)) {
        AppendMenuW(openIn, MF_STRING, MenuId(ACT_OPEN, p), name);
        SetMenuDefaultItem(openIn, MenuId(ACT_OPEN, p), FALSE);
        openable++;
    }
    for (otherProfile = 0; otherProfile < g_view.set.profiles.count; otherProfile++) {
        const SessionEntry *otherEntry = EntryOf(row, otherProfile);
        if (otherProfile == p) continue;
        EscapeAmpersands(ProfileAt(otherProfile)->name, text, ARRAYSIZE(text));
        if (ListedAndKept(otherEntry)) {
            if (openable == 1 && ListedAndKept(entry)) AppendMenuW(openIn, MF_SEPARATOR, 0, NULL);   /* the other profiles below */
            AppendMenuW(openIn, MF_STRING, MenuId(ACT_OPEN, otherProfile), text);
            openable++;
        } else if (!otherEntry) {
            AppendMenuW(shareWith, MF_STRING, MenuId(ACT_SHARE, otherProfile), text);
            unlisted++;
        }
    }
    Theme_AppendMenu(reading, MF_STRING, MenuId(ACT_EXPORT_ORIGINAL, p), TR(L"&Original files\x2026"), 0xE8B7);
    Theme_AppendMenu(reading, MF_STRING | transcript, MenuId(ACT_EXPORT_MARKDOWN, p), TR(L"&Markdown\x2026"), 0xE8A5);
    Theme_AppendMenu(reading, MF_STRING | transcript, MenuId(ACT_EXPORT_HTML, p), TR(L"&Web page (HTML)\x2026"), 0xE774);
    Theme_AppendMenu(reading, MF_STRING | transcript, MenuId(ACT_EXPORT_JSON, p), TR(L"&JSON\x2026"), 0xE943);
    Theme_AppendMenu(reading, MF_STRING | transcript, MenuId(ACT_EXPORT_TEXT, p), TR(L"Plain &text\x2026"), 0xE8D2);
    Theme_AppendMenu(menu, MF_POPUP | (openable ? 0 : MF_GRAYED), (UINT_PTR)openIn, TR(L"Open i&n"), 0xE8A7);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (entry) AppendEntryActions(menu, row, p, name);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    Theme_AppendMenu(menu, MF_STRING | transcript, MenuId(ACT_PREVIEW, p), TR(L"Pre&view\tSpace"), 0xE7B3);
    Theme_AppendMenu(menu, MF_POPUP | (unlisted ? 0 : MF_GRAYED), (UINT_PTR)shareWith, TR(L"S&hare with"), 0xE72D);
    Theme_AppendMenu(menu, MF_POPUP, (UINT_PTR)reading, TR(L"E&xport"), 0xE898);
    Theme_AppendMenu(menu, MF_STRING | (entry ? 0 : MF_GRAYED), MenuId(ACT_BRANCHES, p), TR(L"Rewind and fork relationships\x2026"), 0xE81C);
    Theme_AppendMenu(menu, MF_STRING | (SelectedFolder()[0] ? 0 : MF_GRAYED), MenuId(ACT_SHOW_FOLDER, p), TR(L"&Show folder"), 0xE838);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendEntryDelete(menu, row, p, name);
    g_view.actionDepth++;
    cmd = TrackMenu(menu, pt);
    DestroyMenu(menu);   /* its submenus with it */
    if (cmd >= IDM_BATCH) RunBatch(cmd, &r, 1);
    else RunCommand(cmd);
    g_view.onlyShown = FALSE;
    g_view.actionDepth--;
    TakeHeldSnapshot();
}

/* ------------------------------------------------------- several sessions */

/* The sessions folder row `folder` stands for, folded or not: a heading's
 * all, in their order, those Show more has not shown yet too (as the tree
 * was last filled); Archived's all; Pinned's rows; none for Show more. A
 * heap array of rows (HeapFree) and its length; NULL for none. */
static int *FolderRows(HTREEITEM folder, int *count)
{
    HTREEITEM child;
    LPARAM node = NodeParam(folder);
    int *rows, capacity = 0, bucket = NodeBucket(node), k;
    *count = 0;
    if (IsMoreNode(node)) return NULL;
    if (bucket >= 0 || node == NODE_ARCHIVED) {
        BOOL archived = node == NODE_ARCHIVED || UnderArchived(node);
        int from = archived ? g_view.rowsShownTop : 0, to = archived ? g_view.rowsShownCount : g_view.rowsShownTop;
        if (to <= from || (rows = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)(to - from) * sizeof *rows)) == NULL) return NULL;
        for (k = from; k < to; k++)
            if (node == NODE_ARCHIVED || g_view.rowsShown[k].bucket == bucket) rows[(*count)++] = g_view.rowsShown[k].row;
    } else {
        for (child = TreeView_GetChild(g_view.tree, folder); child; child = NextUnder(folder, child)) capacity++;
        if (!capacity || (rows = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof *rows)) == NULL) return NULL;
        for (child = TreeView_GetChild(g_view.tree, folder); child; child = NextUnder(folder, child)) {
            LPARAM childNode = NodeParam(child);
            if (childNode >= 0 && childNode < g_view.set.rowCount) rows[(*count)++] = (int)childNode;
        }
    }
    if (*count) return rows;
    HeapFree(GetProcessHeap(), 0, rows);
    return NULL;
}

/* Every session the tree shows for the profile shown, folded or not, as
 * FolderRows gives them. */
static int *ShownRows(int *count)
{
    int *rows, r, p = ShownProfileIndex();
    *count = 0;
    if (p < 0 || !g_view.set.rowCount || (rows = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)g_view.set.rowCount * sizeof *rows)) == NULL)
        return NULL;
    for (r = 0; r < g_view.set.rowCount; r++)
        if (RowVisibleIn(&g_view.set.rows[r], p)) rows[(*count)++] = r;
    if (*count) return rows;
    HeapFree(GetProcessHeap(), 0, rows);
    return NULL;
}

/* How many of sessions `rows` profile `p` lists and keeps, and whether
 * those are all pinned there. */
static int KeptIn(const int *rows, int count, int p, BOOL *allStarred)
{
    int i, kept = 0;
    *allStarred = TRUE;
    for (i = 0; i < count; i++) {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[rows[i]], p);
        if (!ListedAndKept(entry)) continue;
        kept++;
        if (!StarredIn(entry)) *allStarred = FALSE;
    }
    return kept;
}

/* Pinned in the profile shown, or unpinned when they all are. The first
 * change that cannot be made stops the others: the user is told once. */
static BOOL StarSeveral(const int *rows, int count)
{
    int i, p = ShownProfileIndex(), changed = 0;
    BOOL allStarred;
    KeptIn(rows, count, p, &allStarred);
    for (i = 0; i < count; i++) {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[rows[i]], p);
        if (!ListedAndKept(entry) || StarredIn(entry) != allStarred) continue;
        if (!ChangeSynced(rows[i], p, PENDING_STAR, allStarred ? L"0" : L"1")) break;
        changed++;
    }
    return changed > 0;
}

/* Unread among those the profile shown keeps: TRUE when any is. */
static BOOL AnyUnread(const int *rows, int count, int p)
{
    int i;
    for (i = 0; i < count; i++)
        if (ListedAndKept(EntryOf(&g_view.set.rows[rows[i]], p)) && UnreadIn(EntryOf(&g_view.set.rows[rows[i]], p))) return TRUE;
    return FALSE;
}

/* Read in the profile shown when any is unread, else marked unread. */
static BOOL ReadSeveral(const int *rows, int count)
{
    int i, p = ShownProfileIndex(), changed = 0;
    BOOL read = AnyUnread(rows, count, p);
    for (i = 0; i < count; i++) {
        const SessionEntry *entry = EntryOf(&g_view.set.rows[rows[i]], p);
        if (!ListedAndKept(entry) || UnreadIn(entry) != read) continue;
        if (!ChangeSynced(rows[i], p, PENDING_READ, read ? L"1" : L"0")) break;
        changed++;
    }
    return changed > 0;
}

/* Out of the profile shown, after one question: their entries go to the
 * Recycle Bin, once it closes when it runs. One session is asked about as
 * Delete asks. */
static BOOL RemoveSeveral(const int *rows, int count)
{
    WCHAR text[2048 + MAX_PROFILES * LABEL_CCH], names[MAX_PROFILES * (LABEL_CCH + 8)];
    DWORD synced = 0;
    int i, p = ShownProfileIndex(), kept, changed = 0;
    BOOL allStarred;
    if ((kept = KeptIn(rows, count, p, &allStarred)) == 0) return FALSE;
    for (i = 0; kept == 1 && i < count; i++)
        if (ListedAndKept(EntryOf(&g_view.set.rows[rows[i]], p))) return RemoveEntry(rows[i], p);
    for (i = 0; i < count; i++) synced |= SyncedWith(&g_view.set.rows[rows[i]], p, PENDING_REMOVE);
    if (synced) {
        Profiles_JoinNames(&g_view.set.profiles, synced | (1u << p), names, ARRAYSIZE(names));
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"Delete %d sessions from %s?\n\nThey sync with each other. Their entries go to the Recycle Bin, each once its profile is closed."),
                         kept, names);
    } else {
        StringCchPrintfW(text, ARRAYSIZE(text),
                         ProfileAt(p)->running
                         ? TR(L"Delete %d sessions from \x201C%s\x201D?\n\nTheir entries go to the Recycle Bin once it closes.")
                         : TR(L"Delete %d sessions from \x201C%s\x201D?\n\nTheir entries go to the Recycle Bin."),
                         kept, ProfileAt(p)->name);
    }
    if (!Ui_Ask(g_view.dlg, IDI_QUESTION, text, TR(L"Delete"), TR(L"Cancel"), FALSE)) return FALSE;
    for (i = 0; i < count; i++) {
        if (!ListedAndKept(EntryOf(&g_view.set.rows[rows[i]], p))) continue;
        if (!ChangeSynced(rows[i], p, PENDING_REMOVE, L"")) break;
        changed++;
    }
    return changed > 0;
}

/* Delete session everywhere for each, after one question. Those it cannot
 * delete now (SessionEdit_CanDelete: open somewhere) are left, and said
 * once the others are gone. */
static BOOL DeleteSeveralEverywhere(const int *rows, int count)
{
    WCHAR text[1024], error[LONG_PATH_CCH + NOTE_CCH], first[LONG_PATH_CCH + NOTE_CCH];
    int i, deletable = 0, left = 0;
    BOOL changed = FALSE;
    if (count == 1) return DeleteSessionEverywhere(rows[0]);
    first[0] = 0;
    for (i = 0; i < count; i++)
        if (SessionEdit_CanDelete(&g_view.set, rows[i], error, ARRAYSIZE(error))) deletable++;
        else if (!first[0]) StringCchCopyW(first, ARRAYSIZE(first), error);
    if (!deletable) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, L"%s", first);
        return FALSE;
    }
    StringCchPrintfW(text, ARRAYSIZE(text),
                     TR(L"Delete %d sessions everywhere?\n\nTheir conversations, their entries in every profile and the working folders "
                        L"of those without a folder go to the Recycle Bin."), deletable);
    if (!Ui_Ask(g_view.dlg, IDI_WARNING, text, TR(L"Delete"), TR(L"Cancel"), TRUE)) return FALSE;
    first[0] = 0;
    for (i = 0; i < count; i++) {
        RemoveResult result = REMOVE_FAILED;
        if (SessionEdit_CanDelete(&g_view.set, rows[i], error, ARRAYSIZE(error)))
            result = SessionEdit_DeleteEverywhere(g_view.dlg, &g_view.set, rows[i], error, ARRAYSIZE(error));
        if (result == REMOVE_CANCELLED) break;   /* in Windows' question: the rest stays too */
        if (result == REMOVE_DONE) {
            SessionUndo_Forget(g_view.set.rows[rows[i]].key);
            changed = TRUE;
        } else if (!left++) {
            StringCchCopyW(first, ARRAYSIZE(first), error);
        }
    }
    if (left) Ui_Message(g_view.dlg, MB_ICONWARNING, TR(L"Sessions that could not be deleted: %d. %s"), left, first);
    return changed || left;
}

/* Sessions `rows` of the profile shown taken (Ctrl+C, or Ctrl+X: `cut`):
 * Ctrl+V copies them to the profile shown then, or moves them there. */
static void TakeSessions(const int *rows, int count, BOOL cut)
{
    WCHAR (*taken)[SESSION_ID_CCH];
    int i, p = ShownProfileIndex();
    if (p < 0 || count <= 0 || (taken = (WCHAR (*)[SESSION_ID_CCH])HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *taken)) == NULL)
        return;
    for (i = 0; i < count; i++) StringCchCopyW(taken[i], SESSION_ID_CCH, g_view.set.rows[rows[i]].key);
    if (g_view.taken) HeapFree(GetProcessHeap(), 0, g_view.taken);
    g_view.taken = taken;
    g_view.takenCount = count;
    StringCchCopyW(g_view.takenFrom, ARRAYSIZE(g_view.takenFrom), ProfileAt(p)->folder);
    g_view.takenCut = cut;
    Util_Log(L"sessions: %d taken in %s (%s)", count, g_view.takenFrom, cut ? L"Ctrl+X" : L"Ctrl+C");
}

static void ForgetTaken(void)
{
    if (g_view.taken) HeapFree(GetProcessHeap(), 0, g_view.taken);
    g_view.taken = NULL;
    g_view.takenCount = 0;
    g_view.takenCut = FALSE;
}

/* Ctrl+V after Ctrl+X: sessions `rows` moved from profile `from` to `to`,
 * after a question: shared with it (those it lists already are not), then
 * taken out of `from`, which leaves Claude's marks so that it does not take
 * them in again. The conversations stay as they are. Nothing leaves `from`
 * when a share fails. */
static BOOL MoveSessions(const int *rows, int count, int from, int to)
{
    WCHAR question[SESSION_TITLE_CCH + 2 * LABEL_CCH + 160];
    SyncReport report;
    int *adding, i, added = 0;
    if (!(SessionSync_Takers(&g_view.set) & (1u << to))) {
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"\x201C%s\x201D is not signed in to Claude yet: sign in there first."), ProfileAt(to)->name);
        return FALSE;
    }
    if (count == 1)
        StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Move \x201C%s\x201D to \x201C%s\x201D?\n\nIt leaves \x201C%s\x201D; its conversation stays as it is."),
                         ShownTitle(&g_view.set.rows[rows[0]], from), ProfileAt(to)->name, ProfileAt(from)->name);
    else
        StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Move %d sessions to \x201C%s\x201D?\n\nThey leave \x201C%s\x201D; their conversations stay as they are."),
                         count, ProfileAt(to)->name, ProfileAt(from)->name);
    if (!Ui_Ask(g_view.dlg, IDI_QUESTION, question, TR(L"Move"), TR(L"Cancel"), FALSE)) return FALSE;
    if ((adding = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *adding)) == NULL) return FALSE;
    for (i = 0; i < count; i++)
        if (!ListedAndKept(EntryOf(&g_view.set.rows[rows[i]], to))) adding[added++] = rows[i];
    ZeroMemory(&report, sizeof report);
    if (added) SessionSync_Share(&g_view.set, from, adding, added, 1u << to, &report);
    HeapFree(GetProcessHeap(), 0, adding);
    if (!report.failed && !report.skipped && !report.unavailable) SessionSync_Remove(&g_view.set, 1u << from, rows, count, &report);
    else Util_Log(L"sessions: not all could be shared with %s: none taken out of %s", ProfileAt(to)->folder, ProfileAt(from)->folder);
    SyncUi_ShowReport(g_view.dlg, &g_view.set.profiles, &report, NULL);
    return TRUE;
}

/* What the keyboard acts on: the sessions chosen together, the selected
 * folder's, or the selected session, as MarkedRows gives them. */
static int *ChosenRows(int *count)
{
    HTREEITEM selected = TreeView_GetSelection(g_view.tree);
    LPARAM node = NodeParam(selected);
    int *rows;
    *count = 0;
    if (g_view.several && g_view.markedCount >= 2) return MarkedRows(count);
    if (node >= 0 && node < g_view.set.rowCount) {
        if ((rows = (int *)HeapAlloc(GetProcessHeap(), 0, sizeof *rows)) == NULL) return NULL;
        rows[0] = (int)node;
        *count = 1;
        return rows;
    }
    return selected && node != NODE_NONE ? FolderRows(selected, count) : NULL;
}

/* Sessions `keys` from profile `fromFolder` given to profile `p`: those it
 * does not keep yet copied there (syncui.c asks first); with `cut`, every
 * one `fromFolder` keeps, moved. Ctrl+V and a drop on the side bar (`how`,
 * for the log) do this. */
static BOOL GiveSessions(const WCHAR (*keys)[SESSION_ID_CCH], int keyCount, const WCHAR *fromFolder, BOOL cut, int p, const WCHAR *how)
{
    int *rows, i, r, count = 0, from = Profiles_Find(&g_view.set.profiles, fromFolder);
    BOOL changed, move = cut && from >= 0 && from != p;
    if (p < 0 || keyCount <= 0 || (rows = (int *)HeapAlloc(GetProcessHeap(), 0, (size_t)keyCount * sizeof *rows)) == NULL) return FALSE;
    for (i = 0; i < keyCount; i++) {
        if ((r = SessionStore_FindRow(&g_view.set, keys[i])) < 0) continue;
        if (move ? ListedAndKept(EntryOf(&g_view.set.rows[r], from)) : !ListedAndKept(EntryOf(&g_view.set.rows[r], p))) rows[count++] = r;
    }
    Util_Log(L"sessions: %s in %s: %d of the %d from %s %s", how, ProfileAt(p)->folder, count, keyCount, fromFolder,
             move ? L"still there, to move" : L"not there yet");
    if (move && count) {
        changed = MoveSessions(rows, count, from, p);
        HeapFree(GetProcessHeap(), 0, rows);
        return changed;
    }
    if (!count) {
        HeapFree(GetProcessHeap(), 0, rows);
        Ui_Message(g_view.dlg, MB_ICONINFORMATION, TR(L"\x201C%s\x201D already has these sessions."), ProfileAt(p)->name);
        return FALSE;
    }
    changed = SyncUi_ShareOrCopy(g_view.dlg, &g_view.set, from, rows, count, TRUE, p);
    HeapFree(GetProcessHeap(), 0, rows);
    return changed;
}

/* Ctrl+V: the sessions taken given to the profile shown; once moved
 * (Ctrl+X), they are no longer taken. */
static BOOL PasteSessions(void)
{
    int p = ShownProfileIndex(), from = Profiles_Find(&g_view.set.profiles, g_view.takenFrom);
    BOOL changed = GiveSessions((const WCHAR (*)[SESSION_ID_CCH])g_view.taken, g_view.takenCount, g_view.takenFrom, g_view.takenCut, p, L"Ctrl+V");
    if (changed && g_view.takenCut && from >= 0 && from != p) ForgetTaken();
    return changed;
}

/* ------------------------------------------------------- drag and drop
 *
 * Sessions are dragged out of the tree. A drag carries two things in one
 * data object: a private format (DRAG_FORMAT, the source profile's folder
 * and the sessions' keys) the side bar's own drop target reads to give them
 * to another profile, and a marker file (DragOut_Begin, as dragout.c's
 * profile drag) a folder in Explorer copies, telling where to export them.
 * Session archives dragged in from outside are imported into the profile
 * dropped on. */

static UINT DragFormat(void)
{
    static UINT format;
    if (!format) format = RegisterClipboardFormatW(L"ClaudeDeltaSessions");
    return format;
}

/* What a session drag carries in the private format: the profile the sessions are from, then their keys. */
typedef struct DragPayload {
    WCHAR from[FOLDER_CCH];
    int   count;
    WCHAR keys[1][SESSION_ID_CCH];   /* `count` of them */
} DragPayload;

#define DRAG_WAIT_MS 5000   /* a drop in a folder is told at once (DragOut_Wait); none by then: it was not one */

static BOOL g_sideDropHandled;   /* the side bar's target took the drop: no folder export follows */

/* The side bar's row under `pt` (the screen), -1 for none. */
static int SideRowAt(POINTL pt)
{
    POINT at = { pt.x, pt.y };
    RECT client;
    LRESULT hit;
    if (!g_view.shown || !g_view.profiles || !GetClientRect(g_view.profiles, &client)) return -1;
    ScreenToClient(g_view.profiles, &at);
    if (!PtInRect(&client, at)) return -1;
    hit = SendMessageW(g_view.profiles, LB_ITEMFROMPOINT, 0, MAKELPARAM(at.x, at.y));
    return HIWORD(hit) || (int)LOWORD(hit) >= g_view.set.profiles.count ? -1 : (int)LOWORD(hit);
}

static void InvalidateSideRow(int row)
{
    RECT rc;
    if (row >= 0 && SendMessageW(g_view.profiles, LB_GETITEMRECT, (WPARAM)row, (LPARAM)&rc) != LB_ERR) InvalidateRect(g_view.profiles, &rc, FALSE);
}

/* The side bar's row what is dragged would go to, drawn hot; -1 for none. */
static void SetDropRow(int row)
{
    if (g_view.drag.hot == row) return;
    InvalidateSideRow(g_view.drag.hot);
    g_view.drag.hot = row;
    InvalidateSideRow(row);
    if (g_view.profiles) UpdateWindow(g_view.profiles);
}

/* `data` carries sessions of ours (the private format): the payload, read in place (same process); NULL for none. */
static DragPayload *SessionsOf(IDataObject *data, STGMEDIUM *medium)
{
    FORMATETC format = { 0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    format.cfFormat = (CLIPFORMAT)DragFormat();
    if (!data || FAILED(IDataObject_GetData(data, &format, medium))) return NULL;
    return (DragPayload *)GlobalLock(medium->hGlobal);
}

/* `data` carries session archives dragged in from outside (files, none of them a drag's marker). */
static BOOL ArchivesIn(IDataObject *data)
{
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    WCHAR path[LONG_PATH_CCH];
    BOOL files = FALSE;
    if (!data || FAILED(IDataObject_GetData(data, &format, &medium))) return FALSE;
    files = DragQueryFileW((HDROP)medium.hGlobal, 0, path, ARRAYSIZE(path)) && !DragOut_IsMarker(path);
    ReleaseStgMedium(&medium);
    return files;
}

/* The side bar's drop target: sessions of ours given to the profile under the pointer, or archives from outside
 * imported into it. A drag over its own source profile does nothing (it already has them). */
typedef struct SideTarget {
    IDropTarget        iface;
    LONG               refs;
    IDropTargetHelper *helper;
    int                kind;   /* 0 none, 1 our sessions, 2 archives from outside */
} SideTarget;

static UINT WM_SESSION_FILES_DROPPED;   /* set by SessionsView_AcceptDrops: a DroppedFiles for the window */

/* What the drop would do, as its row and effect: sessions to another profile (copy, Shift moves), or an import. */
static DWORD SideEffect(SideTarget *target, DWORD keys, POINTL pt, IDataObject *data)
{
    int row = SideRowAt(pt);
    DWORD effect = DROPEFFECT_NONE;
    if (target->kind == 1 && data) {
        STGMEDIUM medium;
        DragPayload *payload = SessionsOf(data, &medium);
        if (payload) {
            int from = Profiles_Find(&g_view.set.profiles, payload->from);
            BOOL move = (keys & MK_SHIFT) != 0;
            if (row >= 0 && row != from) effect = move ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
            GlobalUnlock(medium.hGlobal);
            ReleaseStgMedium(&medium);
        }
    } else if (target->kind == 2 && row >= 0) {
        effect = DROPEFFECT_COPY;   /* a session archive imported into this profile */
    }
    SetDropRow(effect != DROPEFFECT_NONE ? row : -1);
    return effect;
}

static HRESULT STDMETHODCALLTYPE SideQuery(IDropTarget *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = self;
        IDropTarget_AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE SideAddRef(IDropTarget *self)
{
    return (ULONG)InterlockedIncrement(&((SideTarget *)self)->refs);
}

static ULONG STDMETHODCALLTYPE SideReleaseRef(IDropTarget *self)
{
    SideTarget *target = (SideTarget *)self;
    LONG refs = InterlockedDecrement(&target->refs);
    if (!refs) {
        if (target->helper) IDropTargetHelper_Release(target->helper);
        HeapFree(GetProcessHeap(), 0, target);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE SideDragEnter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    SideTarget *target = (SideTarget *)self;
    POINT at = { pt.x, pt.y };
    STGMEDIUM medium;
    DragPayload *payload = SessionsOf(data, &medium);
    target->kind = payload ? 1 : (ArchivesIn(data) ? 2 : 0);
    if (payload) { GlobalUnlock(medium.hGlobal); ReleaseStgMedium(&medium); }
    *effect = SideEffect(target, keys, pt, data);
    if (target->helper) IDropTargetHelper_DragEnter(target->helper, g_view.profiles, data, &at, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SideDragOver(IDropTarget *self, DWORD keys, POINTL pt, DWORD *effect)
{
    SideTarget *target = (SideTarget *)self;
    POINT at = { pt.x, pt.y };
    *effect = SideEffect(target, keys, pt, NULL);
    if (target->helper) IDropTargetHelper_DragOver(target->helper, &at, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE SideDragLeave(IDropTarget *self)
{
    SideTarget *target = (SideTarget *)self;
    if (target->helper) IDropTargetHelper_DragLeave(target->helper);
    target->kind = 0;
    SetDropRow(-1);
    return S_OK;
}

static void ImportArchivesInto(int profile, IDataObject *data);

static HRESULT STDMETHODCALLTYPE SideDrop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    SideTarget *target = (SideTarget *)self;
    POINT at = { pt.x, pt.y };
    int row = SideRowAt(pt);
    *effect = SideEffect(target, keys, pt, data);
    if (target->helper) IDropTargetHelper_Drop(target->helper, data, &at, *effect);
    SetDropRow(-1);
    if (*effect != DROPEFFECT_NONE && row >= 0) {
        g_sideDropHandled = TRUE;   /* the drag ends here, not in a folder */
        if (target->kind == 1) {
            STGMEDIUM medium;
            DragPayload *payload = SessionsOf(data, &medium);
            if (payload) {
                /* The drop is over before the give, which shows dialogs: the keys are kept and a message does it. */
                g_view.drag.target = row;
                g_view.drag.move = (keys & MK_SHIFT) != 0;
                StringCchCopyW(g_view.drag.from, ARRAYSIZE(g_view.drag.from), payload->from);
                if (g_view.drag.keys) HeapFree(GetProcessHeap(), 0, g_view.drag.keys);
                g_view.drag.count = payload->count;
                g_view.drag.keys = (WCHAR (*)[SESSION_ID_CCH])HeapAlloc(GetProcessHeap(), 0, (size_t)max(payload->count, 1) * SESSION_ID_CCH * sizeof(WCHAR));
                if (g_view.drag.keys) memcpy(g_view.drag.keys, payload->keys, (size_t)payload->count * SESSION_ID_CCH * sizeof(WCHAR));
                else g_view.drag.count = 0;
                GlobalUnlock(medium.hGlobal);
                ReleaseStgMedium(&medium);
                if (g_view.drag.keys) PostMessageW(g_view.dlg, WM_SESSION_FILES_DROPPED, 1, 0);   /* wParam 1: give sessions */
            }
        } else if (target->kind == 2) {
            ImportArchivesInto(row, data);
        }
    }
    target->kind = 0;
    return S_OK;
}

static IDropTargetVtbl kSideVtbl = { SideQuery, SideAddRef, SideReleaseRef, SideDragEnter, SideDragOver, SideDragLeave, SideDrop };

/* Session archives in `data` imported into profile `row`: the drop is over first, the work (it shows a dialog)
 * done once a message reaches the window, as dragout.c's file drop is. */
static void ImportArchivesInto(int row, IDataObject *data)
{
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    DroppedFiles *files;
    UINT count, i;
    int n = 0;
    if (FAILED(IDataObject_GetData(data, &format, &medium))) return;
    if ((files = (DroppedFiles *)Util_Alloc(sizeof *files)) != NULL) {
        count = DragQueryFileW((HDROP)medium.hGlobal, 0xFFFFFFFF, NULL, 0);
        for (i = 0; i < count && n < DROPPED_FILES_MAX; i++)
            if (DragQueryFileW((HDROP)medium.hGlobal, i, files->paths[n], ARRAYSIZE(files->paths[n])) && !DragOut_IsMarker(files->paths[n])) n++;
        files->count = n;
        g_view.drag.target = row;
        if (n <= 0 || !PostMessageW(g_view.dlg, WM_SESSION_FILES_DROPPED, 2, (LPARAM)files)) Util_Free(files);   /* wParam 2: import files */
    }
    ReleaseStgMedium(&medium);
}

/* Sessions `rows` of profile `profile` exported to one archive in `folder` (Transfer_Run shows how far). */
typedef struct SessionExport {
    const SessionSet *set;
    int               profile;
    const int        *rows;
    int               rowCount;
} SessionExport;

static BOOL MakeSessionArchive(void *context, const TransferItem *item, const WCHAR *source, WCHAR *error, size_t cch)
{
    const SessionExport *ex = (const SessionExport *)context;
    int exported = 0;
    (void)item;
    return SessionSync_Export(ex->set, ex->profile, ex->rows, ex->rowCount, source, &exported, error, cch);
}

static void ExportSessionsTo(const WCHAR *folder, int profile, const int *rows, int rowCount)
{
    TransferItem item;
    SessionExport ex;
    SYSTEMTIME now;
    const WCHAR *title = rowCount == 1 && rows[0] >= 0 && rows[0] < g_view.set.rowCount ? ShownTitle(&g_view.set.rows[rows[0]], profile) : NULL;
    if (rowCount <= 0) return;
    ZeroMemory(&item, sizeof item);
    GetLocalTime(&now);
    Core_SessionsArchiveName(rowCount == 1 ? title : NULL, TR(L"Claude sessions"), &now, 1, item.name, ARRAYSIZE(item.name));
    StringCchCopyW(item.folder, ARRAYSIZE(item.folder), folder);
    ex.set = &g_view.set;
    ex.profile = profile;
    ex.rows = rows;
    ex.rowCount = rowCount;
    Transfer_Run(g_view.dlg, TR(L"Export"), &item, 1, MakeSessionArchive, &ex);
}

/* The sessions dragged out of the tree: those chosen together when the one
 * dragged is among them, else that session alone, or a heading's. */
static int *DraggedRows(HTREEITEM item, int *count)
{
    LPARAM node = NodeParam(item);
    int *rows;
    *count = 0;
    if (node >= 0 && node < g_view.set.rowCount && IsMarked(g_view.set.rows[node].key)) return MarkedRows(count);
    if (node >= 0 && node < g_view.set.rowCount) {
        if ((rows = (int *)HeapAlloc(GetProcessHeap(), 0, sizeof *rows)) == NULL) return NULL;
        rows[0] = (int)node;
        *count = 1;
        return rows;
    }
    return item && node != NODE_NONE ? FolderRows(item, count) : NULL;
}

/* A drag of the sessions `item` stands for (TVN_BEGINDRAG, from `pt` in the
 * tree): dropped on a profile they are given to it, in a folder they are
 * exported there. */
/* A CF_HDROP in an HGLOBAL naming the one file `path` (DROPFILES, the path, two NULs), for the marker a folder
 * in Explorer copies; NULL when it cannot be made. */
static HGLOBAL HdropOf(const WCHAR *path)
{
    size_t chars = wcslen(path) + 2;   /* the path, its NUL, and the list's closing NUL */
    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + chars * sizeof(WCHAR));
    DROPFILES *drop;
    if (!global || (drop = (DROPFILES *)GlobalLock(global)) == NULL) {
        if (global) GlobalFree(global);
        return NULL;
    }
    ZeroMemory(drop, sizeof *drop);
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    ZeroMemory((BYTE *)drop + sizeof(DROPFILES), chars * sizeof(WCHAR));
    StringCchCopyW((WCHAR *)((BYTE *)drop + sizeof(DROPFILES)), chars, path);
    GlobalUnlock(global);
    return global;
}

/* One format's data set on `data` (its HGLOBAL kept by the object); the global is freed on failure. */
static BOOL SetGlobal(IDataObject *data, UINT cf, HGLOBAL global)
{
    FORMATETC format = { 0, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    format.cfFormat = (CLIPFORMAT)cf;
    ZeroMemory(&medium, sizeof medium);
    medium.tymed = TYMED_HGLOBAL;
    medium.hGlobal = global;
    if (SUCCEEDED(IDataObject_SetData(data, &format, &medium, TRUE))) return TRUE;
    GlobalFree(global);
    return FALSE;
}

static void DragSessions(HTREEITEM item, POINT pt)
{
    DragPayload *payload;
    HGLOBAL global, hdrop;
    IDragSourceHelper *helper = NULL;
    IDataObject *data = NULL;
    DragDrop *drop;
    WCHAR marker[MAX_PATH], folder[LONG_PATH_CCH];
    DWORD effect = 0;
    HRESULT hr = E_FAIL;
    int *rows = NULL, count = 0, profile = ShownProfileIndex(), i;
    size_t bytes;
    if (profile < 0 || (rows = DraggedRows(item, &count)) == NULL || count <= 0) { if (rows) HeapFree(GetProcessHeap(), 0, rows); return; }
    g_view.drag.began = TRUE;   /* the press that started this kept the chosen ones (TreeSubclass) */

    bytes = sizeof(DragPayload) + (size_t)(count - 1) * SESSION_ID_CCH * sizeof(WCHAR);
    if ((global = GlobalAlloc(GMEM_MOVEABLE, bytes)) == NULL || (payload = (DragPayload *)GlobalLock(global)) == NULL) {
        if (global) GlobalFree(global);
        HeapFree(GetProcessHeap(), 0, rows);
        return;
    }
    ZeroMemory(payload, bytes);
    StringCchCopyW(payload->from, ARRAYSIZE(payload->from), ProfileAt(profile)->folder);
    payload->count = count;
    for (i = 0; i < count; i++) StringCchCopyW(payload->keys[i], SESSION_ID_CCH, g_view.set.rows[rows[i]].key);
    GlobalUnlock(global);

    if ((drop = DragOut_Begin(marker, ARRAYSIZE(marker))) == NULL) {
        GlobalFree(global);
        HeapFree(GetProcessHeap(), 0, rows);
        return;
    }
    g_sideDropHandled = FALSE;
    Util_Log(L"sessions: drag of %d from %s begins", count, ProfileAt(profile)->folder);
    /* One data object of our own carries both: the private format the side bar reads, and the marker (CF_HDROP)
     * a folder copies. A shell data object does not always take our own format, so we make our own. */
    if (SUCCEEDED(hr = SHCreateDataObject(NULL, 0, NULL, NULL, &IID_IDataObject, (void **)&data))) {
        BOOL ours = SetGlobal(data, DragFormat(), global);
        if (!ours) GlobalFree(global), global = NULL;
        if ((hdrop = HdropOf(marker)) != NULL) SetGlobal(data, CF_HDROP, hdrop);
        if (SUCCEEDED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDragSourceHelper, (void **)&helper))) {
            IDragSourceHelper_InitializeFromWindow(helper, g_view.tree, &pt, data);
            IDragSourceHelper_Release(helper);
        }
        hr = SHDoDragDrop(g_view.dlg, data, NULL, DROPEFFECT_COPY | DROPEFFECT_MOVE, &effect);
    } else {
        GlobalFree(global);
    }
    SetDropRow(-1);
    if (data) IDataObject_Release(data);
    Util_Log(L"sessions: drag ended (0x%08lX, effect %lu, %s)", (unsigned long)hr, effect, g_sideDropHandled ? L"on a profile" : L"not on a profile");

    /* Let go on a profile: the side bar's target gave or imported them. Let go in a folder: where it is, once the
     * shell's notice says, then the sessions are exported there. */
    if (!g_sideDropHandled && hr == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY) && DragOut_Wait(drop, DRAG_WAIT_MS, folder, ARRAYSIZE(folder))) {
        SetForegroundWindow(g_view.dlg);
        ExportSessionsTo(folder, profile, rows, count);
    }
    DragOut_End(drop);
    HeapFree(GetProcessHeap(), 0, rows);
}

static SideTarget *g_sideTarget;

void SessionsView_AcceptDrops(UINT filesMessage)
{
    SideTarget *target;
    WM_SESSION_FILES_DROPPED = filesMessage;
    if (g_sideTarget || !g_view.profiles) return;
    if ((target = (SideTarget *)Util_Alloc(sizeof *target)) == NULL) return;
    target->iface.lpVtbl = &kSideVtbl;
    target->refs = 1;
    if (FAILED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDropTargetHelper, (void **)&target->helper)))
        target->helper = NULL;
    if (SUCCEEDED(RegisterDragDrop(g_view.profiles, &target->iface))) g_sideTarget = target;
    else IDropTarget_Release(&target->iface);
}

static void StopAcceptingDrops(void)
{
    if (!g_sideTarget) return;
    RevokeDragDrop(g_view.profiles);
    IDropTarget_Release(&g_sideTarget->iface);
    g_sideTarget = NULL;
}

/* The drop posted from the side bar's target, once the drag is over: sessions
 * given to the profile dropped on (wParam 1), or archives imported into it
 * (wParam 2, lParam a DroppedFiles this frees). */
void SessionsView_HandleDrop(WPARAM wp, LPARAM lp)
{
    int to = g_view.drag.target;
    g_view.drag.target = -1;
    if (!g_view.shown || to < 0 || to >= g_view.set.profiles.count) {
        if (wp == 2 && lp) Util_Free((void *)lp);
        return;
    }
    SetForegroundWindow(g_view.dlg);
    if (wp == 1 && g_view.drag.keys && g_view.drag.count > 0) {
        g_view.actionDepth++;
        SessionUndo_Begin(&g_view.set);
        if (GiveSessions((const WCHAR (*)[SESSION_ID_CCH])g_view.drag.keys, g_view.drag.count, g_view.drag.from, g_view.drag.move, to, L"drag"))
            RequestLoad(TRUE);
        SessionUndo_End();
        g_view.actionDepth--;
        TakeHeldSnapshot();
    } else if (wp == 2 && lp) {
        DroppedFiles *files = (DroppedFiles *)lp;
        int i;
        BOOL changed = FALSE;
        SendMessageW(g_view.profiles, LB_SETCURSEL, (WPARAM)to, 0);
        SessionsView_Command(MAKEWPARAM(IDC_S_PROFILES, LBN_SELCHANGE));
        for (i = 0; i < files->count; i++)
            if (SyncUi_ImportFile(g_view.dlg, &g_view.set.profiles, 1u << to, files->paths[i])) changed = TRUE;
        if (changed) RequestLoad(TRUE);
        Util_Free(files);
    }
    if (g_view.drag.keys) { HeapFree(GetProcessHeap(), 0, g_view.drag.keys); g_view.drag.keys = NULL; }
    g_view.drag.count = 0;
}

/* Ctrl+Z, Ctrl+Y: the latest action undone, or made again; what it
 * changed is read again at once. */
static void UndoOrRedo(BOOL redo)
{
    WCHAR error[LONG_PATH_CCH];
    BOOL ok;
    if (!g_view.shown || !(redo ? SessionUndo_CanRedo() : SessionUndo_CanUndo())) return;
    g_view.actionDepth++;
    ok = redo ? SessionUndo_Redo(g_view.dlg, &g_view.set, error, ARRAYSIZE(error))
              : SessionUndo_Undo(g_view.dlg, &g_view.set, error, ARRAYSIZE(error));
    if (!ok) Ui_Message(g_view.dlg, MB_ICONWARNING, redo ? TR(L"Not all of it could be done again. %s") : TR(L"Not all of it could be undone. %s"), error);
    g_view.actionDepth--;
    RequestLoad(TRUE);
    TakeHeldSnapshot();
}

/* Heading `item` opened or folded, and kept so, as a click would. */
static void FoldHeading(HTREEITEM item, BOOL open, BOOL *changed)
{
    TreeNodeKey key;
    if (!item || NodeParam(item) >= 0 || NodeParam(item) == NODE_NONE || IsMoreNode(NodeParam(item))) return;
    TreeView_Expand(g_view.tree, item, open ? TVE_EXPAND : TVE_COLLAPSE);   /* no TVN_ITEMEXPANDED: kept here */
    NodeKey(NodeParam(item), FALSE, &key);
    if (key.key[0] && RecordFolded(key.key, !open)) *changed = TRUE;
}

/* Every heading with sessions left to show shown whole, as Show all does;
 * FALSE when none had any. */
static BOOL ShowAllHeadings(void)
{
    HTREEITEM item;
    TreeNodeKey key;
    BOOL any = FALSE;
    for (item = TreeView_GetRoot(g_view.tree); item; item = NextUnder(NULL, item)) {
        LPARAM node = NodeParam(item);
        if (!IsMoreNode(node)) continue;
        NodeKey(MoreHeading(node), FALSE, &key);
        SetMoreStage(key.key, 2);
        any = TRUE;
    }
    return any;
}

/* Every heading opened, Archived's own too, and its sessions all shown, or
 * every one folded and back to its first sessions, the inner ones first;
 * kept once for the next time. */
static void FoldAll(BOOL open)
{
    HTREEITEM top, inner;
    BOOL changed = FALSE;
    if (!g_view.shown) return;
    for (top = TreeView_GetRoot(g_view.tree); top; top = TreeView_GetNextSibling(g_view.tree, top)) {
        if (open) FoldHeading(top, TRUE, &changed);
        for (inner = TreeView_GetChild(g_view.tree, top); inner; inner = TreeView_GetNextSibling(g_view.tree, inner))
            FoldHeading(inner, open, &changed);
        if (!open) FoldHeading(top, FALSE, &changed);
    }
    if (changed) KeepFolded();
    if (open ? ShowAllHeadings() : g_view.moreCount > 0) {
        if (!open) g_view.moreCount = 0;
        RememberTopRow();
        FillTree();
    }
}


/* A menu's choice for sessions `rows` of the profile shown, run, and kept
 * for Ctrl+Z; what it changed is read again at once, as RunAction does. */
static void RunBatch(UINT cmd, const int *rows, int count)
{
    int p = ShownProfileIndex();
    BOOL changed = FALSE;
    if (p < 0) return;
    if (cmd == IDM_BATCH + BATCH_UNDO || cmd == IDM_BATCH + BATCH_REDO) {
        UndoOrRedo(cmd == IDM_BATCH + BATCH_REDO);
        return;
    }
    g_view.actionDepth++;
    SessionUndo_Begin(&g_view.set);
    switch (cmd) {
    case IDM_BATCH + BATCH_SHARE:      changed = SyncUi_ShareOrCopy(g_view.dlg, &g_view.set, p, rows, count, FALSE, -1); break;
    case IDM_BATCH + BATCH_EXPORT:     SyncUi_Export(g_view.dlg, &g_view.set, p, rows, count); break;
    case IDM_BATCH + BATCH_STAR:       changed = StarSeveral(rows, count); break;
    case IDM_BATCH + BATCH_READ:       changed = ReadSeveral(rows, count); break;
    case IDM_BATCH + BATCH_REMOVE:     changed = RemoveSeveral(rows, count); break;
    case IDM_BATCH + BATCH_DELETE_ALL: changed = DeleteSeveralEverywhere(rows, count); break;
    case IDM_BATCH + BATCH_IMPORT:     changed = SyncUi_Import(g_view.dlg, &g_view.set.profiles, 1u << p); break;
    case IDM_BATCH + BATCH_TAKE:       TakeSessions(rows, count, FALSE); break;
    case IDM_BATCH + BATCH_CUT:        TakeSessions(rows, count, TRUE); break;
    case IDM_BATCH + BATCH_PASTE:      changed = PasteSessions(); break;
    default: break;
    }
    SessionUndo_End();
    g_view.actionDepth--;
    if (changed) RequestLoad(TRUE);
    TakeHeldSnapshot();
}

/* The sessions chosen, acted on together (Del, Shift+Del). */
static void RunOnMarks(UINT cmd)
{
    int *rows, count;
    if ((rows = MarkedRows(&count)) == NULL) return;
    RunBatch(cmd, rows, count);
    HeapFree(GetProcessHeap(), 0, rows);
}

/* The view's menus are the program's own (theme.c), as Claude's: an item
 * that deletes red, the keys that do one at its right end. */
static UINT TrackMenu(HMENU menu, POINT pt)
{
    RECT at;
    UINT cmd;
    SetRect(&at, pt.x, pt.y, pt.x, pt.y);
    cmd = Theme_TrackMenu(g_view.dlg, menu, &at);
    /* Chosen with Shift, Ctrl or Alt held: in the profile shown alone, not across its sync group. */
    g_view.onlyShown = cmd && (GetKeyState(VK_SHIFT) < 0 || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0);
    return cmd;
}

/* What can be done to sessions `rows` together, at `pt` (screen): pin or
 * delete them in the profile shown, first as in Claude's menu; share them
 * with other profiles or copy them there (a dialog chooses which), export
 * them, or delete them everywhere. `keys`: the sessions chosen, which Del
 * deletes in the profile shown and Shift+Del everywhere. */
static void BatchMenu(POINT pt, const int *rows, int count, BOOL keys)
{
    HMENU menu;
    int p = ShownProfileIndex(), kept;
    UINT others = g_view.set.profiles.count > 1 ? 0 : MF_GRAYED, cmd;
    BOOL allStarred;
    if (p < 0 || (menu = CreatePopupMenu()) == NULL) return;
    kept = KeptIn(rows, count, p, &allStarred);
    Theme_AppendMenu(menu, MF_STRING | (kept ? 0 : MF_GRAYED), IDM_BATCH + BATCH_STAR, kept && allStarred ? TR(L"Un&pin") : TR(L"&Pin"), 0xE718);
    Theme_AppendMenu(menu, MF_STRING | (kept ? 0 : MF_GRAYED), IDM_BATCH + BATCH_READ,
                AnyUnread(rows, count, p) ? TR(L"&Mark as read") : TR(L"&Mark as unread"), 0xE8C3);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    Theme_AppendMenu(menu, MF_STRING | others, IDM_BATCH + BATCH_SHARE, TR(L"&Share with\x2026"), 0xE72D);
    Theme_AppendMenu(menu, MF_STRING, IDM_BATCH + BATCH_EXPORT, TR(L"E&xport sessions\x2026"), 0xE898);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    Theme_AppendMenu(menu, MF_STRING | (kept ? 0 : MF_GRAYED), IDM_BATCH + BATCH_REMOVE, keys ? TR(L"&Delete\x2026\tDel") : TR(L"&Delete\x2026"), 0xE74D);
    Theme_SetMenuDanger(menu, IDM_BATCH + BATCH_REMOVE);
    g_view.actionDepth++;   /* `rows` name the snapshot shown: it stays while the menu is open */
    cmd = TrackMenu(menu, pt);
    DestroyMenu(menu);
    RunBatch(cmd, rows, count);
    g_view.onlyShown = FALSE;
    g_view.actionDepth--;
    TakeHeldSnapshot();
}

/* Below the tree's rows: every session it shows exported, an archive
 * imported into the profile shown, the sessions taken pasted there, and the
 * latest action undone or done again. */
static void AreaMenu(POINT pt)
{
    HMENU menu;
    int *rows, count;
    UINT cmd;
    if (ShownProfileIndex() < 0 || (menu = CreatePopupMenu()) == NULL) return;
    g_view.actionDepth++;
    rows = ShownRows(&count);
    Theme_AppendMenu(menu, MF_STRING | (count ? 0 : MF_GRAYED), IDM_BATCH + BATCH_EXPORT, TR(L"E&xport sessions\x2026"), 0xE898);
    Theme_AppendMenu(menu, MF_STRING, IDM_BATCH + BATCH_IMPORT, TR(L"&Import sessions\x2026"), 0xE896);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    Theme_AppendMenu(menu, MF_STRING | (SessionUndo_CanUndo() ? 0 : MF_GRAYED), IDM_BATCH + BATCH_UNDO, TR(L"&Undo\tCtrl+Z"), 0xE7A7);
    Theme_AppendMenu(menu, MF_STRING | (SessionUndo_CanRedo() ? 0 : MF_GRAYED), IDM_BATCH + BATCH_REDO, TR(L"&Redo\tCtrl+Y"), 0xE7A6);
    cmd = TrackMenu(menu, pt);
    DestroyMenu(menu);
    RunBatch(cmd, rows, count);
    g_view.onlyShown = FALSE;
    if (rows) HeapFree(GetProcessHeap(), 0, rows);
    g_view.actionDepth--;
    TakeHeldSnapshot();
}

/* Ctrl+C, Ctrl+X: the sessions the keyboard acts on taken; Ctrl+V: those
 * taken copied (or moved) to the profile shown; Ctrl+Z, Ctrl+Y (or
 * Ctrl+Shift+Z): the latest action undone, or done again. */
static void RunEditKey(WPARAM key)
{
    int *rows = NULL, count = 0;
    if (!g_view.shown) return;
    if (key == 'Z' || key == 'Y') {
        UndoOrRedo(key == 'Y' || GetKeyState(VK_SHIFT) < 0);
        return;
    }
    g_view.actionDepth++;   /* the rows name the snapshot shown */
    if (key != 'V') rows = ChosenRows(&count);
    if (key == 'V' || rows) RunBatch(IDM_BATCH + (key == 'C' ? BATCH_TAKE : key == 'X' ? BATCH_CUT : BATCH_PASTE), rows, count);
    if (rows) HeapFree(GetProcessHeap(), 0, rows);
    g_view.actionDepth--;
    TakeHeldSnapshot();
}

/* The side bar's list: Ctrl+V there pastes into the profile it shows, the
 * one just picked after Ctrl+C or Ctrl+X in another; Ctrl+Z and Ctrl+Y undo
 * and do again, as in the tree. */
static LRESULT CALLBACK ProfilesSubclass(HWND window, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)ref;
    switch (msg) {
    case WM_KEYDOWN:
        if ((wp == 'V' || wp == 'Z' || wp == 'Y') && EditKey(wp)) return 0;
        break;
    case WM_CHAR:
        if (wp == 22 || wp == 25 || wp == 26) return 0;   /* Ctrl+V's, Ctrl+Y's and Ctrl+Z's characters: no search of the names */
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(window, ProfilesSubclass, id);
        break;
    }
    return DefSubclassProc(window, msg, wp, lp);
}

/* --------------------------------------------------------------- watcher */

#define WATCH_EVERYTHING (FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE)

typedef struct WatcherParameters {      /* what the watcher thread owns */
    HWND      dlg;
    HANDLE    stop;
    WatchPlan plan;
} WatcherParameters;

typedef struct WatchHandle {
    HWND                 dlg;
    HANDLE               change, wait, stop;
    const WatchedFolder *folder;
    ULONGLONG            state;         /* WatchedState as last told */
} WatchHandle;

static BOOL Probed(const WatchedFolder *folder)
{
    return folder->nearest[0] || folder->file[0];
}

/* What a notification on a probed folder must change to ask for a snapshot:
 * the nearest existing folder of `nearest`, the size and time of `file`. */
static ULONGLONG WatchedState(const WatchedFolder *folder)
{
    WCHAR nearest[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA file;
    ULONGLONG state = CORE_HASH_START;
    if (folder->nearest[0]) {
        if (!Util_ExistingDir(folder->nearest, nearest, ARRAYSIZE(nearest))) nearest[0] = 0;
        state = Core_HashText(state, nearest);
    }
    if (folder->file[0]) {
        if (!GetFileAttributesExW(folder->file, GetFileExInfoStandard, &file)) ZeroMemory(&file, sizeof file);
        state = Core_HashBytes(state, &file.ftLastWriteTime, sizeof file.ftLastWriteTime);
        state = Core_HashBytes(state, &file.nFileSizeHigh, sizeof file.nFileSizeHigh);
        state = Core_HashBytes(state, &file.nFileSizeLow, sizeof file.nFileSizeLow);
    }
    return state;
}

/* One WM_APP_SESSIONS for a burst of notifications: RequestLoad clears the flag. */
static void PostReload(HWND dlg)
{
    SessionSearch_Invalidate();
    if (InterlockedExchange(&g_reloadMessagePending, 1) == 0) PostMessageW(dlg, WM_APP_SESSIONS, 0, 0);
}

/* Re-armed first, so a change while it runs notifies again (and a stopping
 * watcher is not called back at once for the same change). A probed folder
 * asks for a snapshot only when what it watches changed: Chromium writes its
 * own files next to config.json all the time. A notification lost stops the
 * watcher; the snapshot it asks for watches again. */
static void CALLBACK WatchChanged(void *param, BOOLEAN timedOut)
{
    WatchHandle *watch = (WatchHandle *)param;
    BOOL rearmed = FindNextChangeNotification(watch->change);
    DWORD error = GetLastError();
    (void)timedOut;
    if (WaitForSingleObject(watch->stop, 0) == WAIT_OBJECT_0) return;
    if (!rearmed) {
        Util_Log(L"sessions: %s is no longer watched (error %lu)", watch->folder->dir, error);
        SetEvent(watch->stop);
    } else if (Probed(watch->folder)) {
        ULONGLONG state = WatchedState(watch->folder);
        if (state == watch->state) return;
        watch->state = state;
    }
    PostReload(watch->dlg);
}

static DWORD WINAPI WatchProc(void *param)
{
    WatcherParameters *parameters = (WatcherParameters *)param;
    WatchHandle handles[WATCH_MAX];
    int i, count = 0;
    ZeroMemory(handles, sizeof handles);
    for (i = 0; i < parameters->plan.count; i++) {
        const WatchedFolder *folder = &parameters->plan.folder[i];
        WatchHandle *watch = &handles[count];
        watch->change = FindFirstChangeNotificationW(folder->dir, folder->subtree, folder->filter);
        if (watch->change == INVALID_HANDLE_VALUE) {
            Util_Log(L"sessions: %s cannot be watched (error %lu)", folder->dir, GetLastError());
            continue;
        }
        watch->dlg = parameters->dlg;
        watch->stop = parameters->stop;
        watch->folder = folder;
        if (Probed(folder)) watch->state = WatchedState(folder);   /* once notified: nothing between is missed */
        if (!RegisterWaitForSingleObject(&watch->wait, watch->change, WatchChanged, watch, INFINITE, WT_EXECUTEINWAITTHREAD)) {
            Util_Log(L"sessions: %s cannot be watched (error %lu)", folder->dir, GetLastError());
            FindCloseChangeNotification(watch->change);
            continue;
        }
        count++;
    }
    /* The first snapshot follows registration too, so a write between the
     * directory lookup and this thread starting cannot leave an old cache. */
    PostReload(parameters->dlg);
    WaitForSingleObject(parameters->stop, INFINITE);
    for (i = 0; i < count; i++) {
        UnregisterWaitEx(handles[i].wait, INVALID_HANDLE_VALUE);
        FindCloseChangeNotification(handles[i].change);
    }
    HeapFree(GetProcessHeap(), 0, parameters);
    return 0;
}

static void StopWatching(void)
{
    if (g_view.watchStop) SetEvent(g_view.watchStop);
    if (g_view.watchThread) {
        WaitForSingleObject(g_view.watchThread, INFINITE);
        CloseHandle(g_view.watchThread);
    }
    if (g_view.watchStop) CloseHandle(g_view.watchStop);
    g_view.watchThread = g_view.watchStop = NULL;
    g_view.watched.count = 0;
}

static BOOL SamePath(const WCHAR *one, const WCHAR *other)
{
    return (!one[0] && !other[0]) || Core_PathEquals(one, other);
}

/* The watcher runs, and watches `plan` already: not one that lost a
 * notification and is stopping. */
static BOOL WatchingThese(const WatchPlan *plan)
{
    int i;
    if (!g_view.watchThread || WaitForSingleObject(g_view.watchThread, 0) != WAIT_TIMEOUT ||
        WaitForSingleObject(g_view.watchStop, 0) != WAIT_TIMEOUT ||
        g_view.watched.count != plan->count)
        return FALSE;
    for (i = 0; i < plan->count; i++) {
        const WatchedFolder *watching = &g_view.watched.folder[i], *planned = &plan->folder[i];
        if (!SamePath(watching->dir, planned->dir) || watching->subtree != planned->subtree || watching->filter != planned->filter ||
            !SamePath(watching->nearest, planned->nearest) || !SamePath(watching->file, planned->file))
            return FALSE;
    }
    return TRUE;
}

static void AddWatch(WatchPlan *plan, const WCHAR *dir, BOOL subtree, DWORD filter, const WCHAR *nearest, const WCHAR *file)
{
    WatchedFolder *folder;
    if (plan->count >= WATCH_MAX) return;
    folder = &plan->folder[plan->count];
    if (FAILED(StringCchCopyW(folder->dir, ARRAYSIZE(folder->dir), dir)) ||
        FAILED(StringCchCopyW(folder->nearest, ARRAYSIZE(folder->nearest), nearest)) ||
        FAILED(StringCchCopyW(folder->file, ARRAYSIZE(folder->file), file)))
        return;
    folder->subtree = subtree;
    folder->filter = filter;
    plan->count++;
}

/* `target` watched at any depth once it exists; until then its nearest
 * existing folder, alone, for the next folder down: a folder watched with
 * all it holds wakes on every write there (a profile's folder holds
 * Chromium's cache). */
static void WatchFolderOrParent(WatchPlan *plan, const WCHAR *target, const WCHAR *nearest)
{
    if (Core_PathEquals(nearest, target)) AddWatch(plan, target, TRUE, WATCH_EVERYTHING, L"", L"");
    else AddWatch(plan, nearest, FALSE, FILE_NOTIFY_CHANGE_DIR_NAME, target, L"");
}

/* What to watch for the profiles of a snapshot: each one's session entries
 * and its own folder (its config.json names the account shown; its entries
 * folder appears there), and Claude Code's transcripts. Planned with every
 * snapshot, on the loader thread (the folders may be on a share): the watch
 * follows folders as they are made. */
static void PlanWatch(const ProfileList *profiles, WatchPlan *plan)
{
    WCHAR target[MAX_PATH], nearest[MAX_PATH], config[MAX_PATH];
    int i;
    plan->count = 0;
    for (i = 0; i < profiles->count; i++) {
        const Profile *profile = &profiles->items[i];
        if (!SessionStore_SessionsDir(profile, target, ARRAYSIZE(target))) continue;
        if (SessionStore_WatchDir(profile, nearest, ARRAYSIZE(nearest)) && !Core_PathEquals(nearest, profile->storageDir))
            WatchFolderOrParent(plan, target, nearest);
        if (Util_DirExists(profile->storageDir) &&
            SUCCEEDED(StringCchPrintfW(config, ARRAYSIZE(config), L"%s\\" CLAUDE_APP_SETTINGS, profile->storageDir)))
            AddWatch(plan, profile->storageDir, FALSE, WATCH_EVERYTHING, target, config);
    }
    if (SessionStore_ProjectsDir(target, ARRAYSIZE(target)) && Util_ExistingDir(target, nearest, ARRAYSIZE(nearest)))
        WatchFolderOrParent(plan, target, nearest);
}

/* The watcher watching `plan` (the snapshot's): a new watcher asks for a
 * snapshot once it watches. */
static void Watch(const WatchPlan *plan)
{
    WatcherParameters *parameters;
    DWORD error;
    if (g_view.watchPaused || WatchingThese(plan)) return;
    StopWatching();
    parameters = (WatcherParameters *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *parameters);
    if (parameters && (g_view.watchStop = CreateEventW(NULL, TRUE, FALSE, NULL)) != NULL) {
        parameters->dlg = g_view.dlg;
        parameters->stop = g_view.watchStop;
        parameters->plan = *plan;
        g_view.watched = *plan;
        g_view.watchThread = CreateThread(NULL, 0, WatchProc, parameters, 0, NULL);
        if (g_view.watchThread) return;
    }
    error = parameters ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;   /* HeapAlloc sets none */
    if (parameters) HeapFree(GetProcessHeap(), 0, parameters);
    if (g_view.watchStop) CloseHandle(g_view.watchStop);
    g_view.watchStop = NULL;
    g_view.watched.count = 0;
    Util_Log(L"sessions: the folder watcher cannot start (error %lu)", error);
}

/* ---------------------------------------------------------------- loader */

static void FreeSnapshot(SessionSet *set)
{
    if (!set) return;
    SessionStore_Free(set);
    HeapFree(GetProcessHeap(), 0, set);
}

/* `fresh` (a snapshot, or NULL for a read that failed) handed to the UI,
 * unless it went out of date while it was read (RequestLoad's restart) or
 * the loader stops: then it is freed here. */
static void Publish(SessionLoader *loader, ULONG generation, SessionSet *fresh)
{
    SessionSet *old = NULL;
    ULONGLONG treeHash = fresh ? TreeContentHash(fresh) : 0;
    ULONGLONG snapshotHash = fresh ? SnapshotHash(fresh, treeHash) : 0;
    BOOL publish = FALSE;
    AcquireSRWLockExclusive(&loader->lock);
    if (loader->generation == generation && WaitForSingleObject(loader->stop, 0) != WAIT_OBJECT_0) {
        old = loader->ready;
        loader->ready = fresh;
        loader->readyTreeHash = treeHash;
        loader->readySnapshotHash = snapshotHash;
        if (fresh) loader->readyPlan = loader->plan;
        loader->readyGeneration = generation;
        loader->failed = fresh == NULL;
        publish = TRUE;
    }
    ReleaseSRWLockExclusive(&loader->lock);
    FreeSnapshot(old);
    if (publish) PostMessageW(loader->dlg, WM_APP_SESSIONS_READY, 0, 0);
    else FreeSnapshot(fresh);
}

/* A snapshot is logged when what it holds changed or it was slow, a failed
 * read once until one works: reads follow every write to a transcript, and
 * the log is kept short. */
static void LogSnapshot(SessionLoader *loader, const SessionSet *set, DWORD elapsedMs)
{
    if (!set) {
        if (!loader->failureLogged) Util_Log(L"sessions could not be read (%lu ms)", elapsedMs);
        loader->failureLogged = TRUE;
        return;
    }
    loader->failureLogged = FALSE;
    if (elapsedMs < SLOW_LOAD_MS && set->entryCount == loader->loggedEntries && set->rowCount == loader->loggedRows &&
        set->groupCount == loader->loggedGroups)
        return;
    Util_Log(L"sessions loaded in %lu ms: %d entries, %d conversations, %d folders", elapsedMs, set->entryCount, set->rowCount,
             set->groupCount);
    loader->loggedEntries = set->entryCount;
    loader->loggedRows = set->rowCount;
    loader->loggedGroups = set->groupCount;
}

/* Notifications come in bursts: Claude writes an entry and its neighbours,
 * Claude Code its transcript a line at a time. A notification's request is
 * read once none has followed for SETTLE_MS, or SETTLE_MAX_MS after it; any
 * other request, a change of ours above all, at once. FALSE when the loader
 * stops. */
static BOOL Settle(SessionLoader *loader)
{
    HANDLE events[3] = { loader->stop, loader->cancel, loader->request };
    ULONGLONG first = GetTickCount64();
    for (;;) {
        ULONGLONG waited = GetTickCount64() - first;
        DWORD woke;
        BOOL urgent;
        AcquireSRWLockShared(&loader->lock);
        urgent = loader->urgent;
        ReleaseSRWLockShared(&loader->lock);
        if (urgent || waited >= SETTLE_MAX_MS) return TRUE;
        woke = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, (DWORD)min(SETTLE_MS, SETTLE_MAX_MS - waited));
        if (woke == WAIT_OBJECT_0) return FALSE;
        if (woke != WAIT_OBJECT_0 + 2) return TRUE;   /* quiet, or a change of ours */
    }
}

/* Reads a snapshot, and what it asks to watch, for each request: a request
 * during a read is read once that read is published, a read that failed
 * included; a cancelled read is dropped. */
static DWORD WINAPI LoadProc(void *param)
{
    SessionLoader *loader = (SessionLoader *)param;
    HANDLE events[2] = { loader->stop, loader->request }, paced[2] = { loader->stop, loader->cancel };
    for (;;) {
        ProfileList profiles;
        SessionSet *fresh;
        ULONG generation;
        ULONGLONG started;
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE) != WAIT_OBJECT_0 + 1 || !Settle(loader)) break;
        AcquireSRWLockExclusive(&loader->lock);
        profiles = loader->profiles;
        generation = loader->generation;
        loader->urgent = FALSE;
        ResetEvent(loader->cancel);
        ReleaseSRWLockExclusive(&loader->lock);
        if (WaitForSingleObject(loader->stop, 0) == WAIT_OBJECT_0) break;
        started = GetTickCount64();
        fresh = (SessionSet *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *fresh);
        if (fresh) {
            Claude_UpdateRunning(&profiles);
            if (SessionStore_LoadProfilesCancel(fresh, &profiles, loader->cancel)) {
                PlanWatch(&fresh->profiles, &loader->plan);
            } else {
                HeapFree(GetProcessHeap(), 0, fresh);
                fresh = NULL;
                if (WaitForSingleObject(loader->cancel, 0) == WAIT_OBJECT_0) continue;   /* out of date: read again at once */
            }
        }
        LogSnapshot(loader, fresh, (DWORD)(GetTickCount64() - started));
        Publish(loader, generation, fresh);
        /* While Claude Code runs, its transcript is written all the time: the
         * next read waits a little, unless a change of ours needs it now. */
        if (WaitForMultipleObjects(ARRAYSIZE(paced), paced, FALSE, READ_PACE_MS) == WAIT_OBJECT_0) break;
    }
    return 0;
}

static void FreeLoader(SessionLoader *loader)
{
    if (loader->thread) CloseHandle(loader->thread);
    if (loader->stop) CloseHandle(loader->stop);
    if (loader->request) CloseHandle(loader->request);
    if (loader->cancel) CloseHandle(loader->cancel);
    FreeSnapshot(loader->ready);
    HeapFree(GetProcessHeap(), 0, loader);
}

static BOOL StartLoader(void)
{
    SessionLoader *loader;
    if (g_view.loader) return TRUE;
    loader = (SessionLoader *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *loader);
    if (!loader) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);   /* HeapAlloc sets none */
        return FALSE;
    }
    InitializeSRWLock(&loader->lock);
    loader->dlg = g_view.dlg;
    loader->loggedEntries = loader->loggedRows = loader->loggedGroups = -1;   /* the first snapshot is logged */
    loader->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    loader->request = CreateEventW(NULL, FALSE, FALSE, NULL);
    loader->cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (loader->stop && loader->request && loader->cancel)
        loader->thread = CreateThread(NULL, 0, LoadProc, loader, 0, NULL);
    if (!loader->thread) {
        DWORD error = GetLastError();
        FreeLoader(loader);
        SetLastError(error);
        return FALSE;
    }
    g_view.loader = loader;
    return TRUE;
}

static void StopLoader(void)
{
    SessionLoader *loader = g_view.loader;
    if (!loader) return;
    SetEvent(loader->stop);
    SetEvent(loader->cancel);
    WaitForSingleObject(loader->thread, INFINITE);
    FreeLoader(loader);
    g_view.loader = NULL;
}

/* No snapshot could be read: the tree and the side bar say so. */
static void ShowLoadFailed(void)
{
    g_view.loadFailed = TRUE;
    g_view.loading = FALSE;
    InvalidateRect(g_view.tree, NULL, FALSE);
    InvalidateRect(g_view.profiles, NULL, FALSE);
}

/* A snapshot asked for. `restart`: what is being read is out of date (the
 * profiles changed, or an action of ours changed the entries), so that read
 * stops and no snapshot from before is shown. Otherwise (a notification, the
 * window activated) the read in progress is published and another follows:
 * notifications coming faster than a read cannot keep every read from
 * finishing. Not `urgent`: a notification's, read once its burst is over
 * (Settle). */
static void AskForSnapshot(BOOL restart, BOOL urgent)
{
    SessionLoader *loader;
    if (!g_view.warmed) return;
    if (!StartLoader()) {
        if (!g_view.loadFailed) Util_Log(L"sessions: the reader cannot start (error %lu)", GetLastError());
        ShowLoadFailed();
        return;
    }
    InterlockedExchange(&g_reloadMessagePending, 0);
    loader = g_view.loader;
    AcquireSRWLockExclusive(&loader->lock);
    loader->profiles = g_view.profilesToRead;
    if (restart || urgent) loader->urgent = TRUE;
    if (restart) {
        loader->generation++;
        SetEvent(loader->cancel);
    }
    SetEvent(loader->request);
    ReleaseSRWLockExclusive(&loader->lock);
    if (g_view.loadFailed) {   /* loading again, no longer failed */
        InvalidateRect(g_view.tree, NULL, FALSE);
        InvalidateRect(g_view.profiles, NULL, FALSE);
    }
    g_view.loading = TRUE;
    g_view.loadFailed = FALSE;
}

static void RequestLoad(BOOL restart)
{
    AskForSnapshot(restart, TRUE);
}

/* ------------------------------------------------------------------- API */

static BOOL SameProfiles(const ProfileList *one, const ProfileList *other)
{
    int i;
    if (one->count != other->count) return FALSE;
    for (i = 0; i < one->count; i++) {
        const Profile *oneProfile = &one->items[i], *otherProfile = &other->items[i];
        if (wcscmp(oneProfile->folder, otherProfile->folder) != 0 || wcscmp(oneProfile->name, otherProfile->name) != 0 ||
            wcscmp(oneProfile->dataDir, otherProfile->dataDir) != 0 || wcscmp(oneProfile->storageDir, otherProfile->storageDir) != 0 ||
            Core_ProfileLookHash(CORE_HASH_START, oneProfile) != Core_ProfileLookHash(CORE_HASH_START, otherProfile) ||
            oneProfile->isStock != otherProfile->isStock ||
            oneProfile->running != otherProfile->running || oneProfile->pid != otherProfile->pid) return FALSE;
    }
    return TRUE;
}

void SessionsView_SetProfiles(const ProfileList *profiles)
{
    if (!profiles || SameProfiles(&g_view.profilesToRead, profiles)) return;
    g_view.profilesToRead = *profiles;
    if (!g_view.loaded) g_view.set.profiles = *profiles;
    RequestLoad(TRUE);
}

void SessionsView_Warm(const ProfileList *profiles)
{
    SessionsView_SetProfiles(profiles);
    if (g_view.warmed) return;
    g_view.warmed = TRUE;
    RequestLoad(FALSE);                /* the first snapshot starts the watcher, which asks for another once it watches */
}

/* ------------------------------------------------- filter menu and search */

/* The filter menu's commands: IDM_FILTER + one of these (+ the choice). */
enum { FILTER_STATUS = 0, FILTER_ENV_ALL = 8, FILTER_ENV = 9, FILTER_GROUP = 16, FILTER_SORT = 24, FILTER_EMPTY = 30, FILTER_PRS,
       FILTER_RESULT = 32, FILTER_CLEAR = FILTER_RESULT + SESSION_RESULTS + 1, FILTER_RESET,
       FILTER_DESCENDING = FILTER_SORT + SORTINGS, FILTER_ASCENDING };

static const WCHAR *StatusName(ViewStatus status)
{
    switch (status) {
    case STATUS_ARCHIVED: return TR(L"Archived");
    case STATUS_ALL: return TR(L"All");
    default: return TR(L"Active");
    }
}

static const WCHAR *EnvironmentName(int i)
{
    switch (i) {
    case 0: return TR(L"Local");
    case 1: return TR(L"Cloud");
    case 2: return TR(L"Remote Control");
    case 3: return L"SSH";
    default: return L"Slack";
    }
}

static const WCHAR *GroupingName(Grouping grouping)
{
    switch (grouping) {
    case GROUP_DATE: return TR(L"Date");
    case GROUP_FOLDER: return TR(L"Folder");
    case GROUP_STATE: return TR(L"State");
    case GROUP_CUSTOM: return TR(L"Custom groups");
    case GROUP_ENVIRONMENT: return TR(L"Environment");
    default: return TR(L"None");
    }
}

static const WCHAR *SortingName(Sorting sorting)
{
    switch (sorting) {
    case SORT_NAME: return TR(L"Name");
    case SORT_CREATED: return TR(L"Date created");
    default: return TR(L"Last updated");
    }
}

/* The filter menu's and the search options' choices, and the headings left open, as last kept. */
static void LoadChoices(void)
{
    DWORD grouping;
    g_view.status = (ViewStatus)min(Util_GetSetting(SETTING_SESSIONS_STATUS, STATUS_ACTIVE), (DWORD)STATUSES - 1);
    g_view.environments = Util_GetSetting(SETTING_SESSIONS_ENV, 0) & SESSION_ENV_ALL;
    if (g_view.environments == SESSION_ENV_ALL) g_view.environments = 0;
    /* By day, as Claude's sidebar, unless another grouping was chosen: one
     * kept with folders the default counts but for that default. */
    grouping = Util_GetSetting(SETTING_SESSIONS_GROUP, GROUPINGS);
    if (grouping >= GROUPINGS) {
        grouping = Util_GetSetting(SETTING_SESSIONS_GROUP_BY, GROUP_FOLDER);
        if (grouping == GROUP_FOLDER || grouping >= GROUPINGS) grouping = GROUP_DATE;
    }
    g_view.grouping = (Grouping)grouping;
    g_view.sorting = (Sorting)min(Util_GetSetting(SETTING_SESSIONS_SORT, SORT_ACTIVITY), (DWORD)SORTINGS - 1);
    /* Old installations sorted names upwards and times downwards. */
    g_view.descending = Util_GetSetting(SETTING_SESSIONS_DESC, g_view.sorting != SORT_NAME) != 0;
    g_view.showEmptyGroups = Util_GetSetting(SETTING_SESSIONS_EMPTY, 0) != 0;
    g_view.showPullRequests = Util_GetSetting(SETTING_SESSIONS_PRS, 1) != 0;
    g_view.resultFilter = (int)min(Util_GetSetting(SETTING_SESSIONS_RESULT, 0), (DWORD)SESSION_RESULTS);
    g_view.searchIn = Util_GetSetting(SETTING_SESSIONS_SEARCH, SEARCH_TITLES) & (SEARCH_TITLES | SEARCH_TEXT | SEARCH_REGEX | SEARCH_CASE);
    if (!(g_view.searchIn & (SEARCH_TITLES | SEARCH_TEXT))) g_view.searchIn |= SEARCH_TITLES;
    g_view.openedCount = Util_GetSettingList(OPENED_SECTION, g_view.opened, MAX_OPENED);
}

static void SaveChoices(void)
{
    if (!Util_SetSetting(SETTING_SESSIONS_STATUS, (DWORD)g_view.status) || !Util_SetSetting(SETTING_SESSIONS_ENV, g_view.environments) ||
        !Util_SetSetting(SETTING_SESSIONS_GROUP, (DWORD)g_view.grouping) || !Util_SetSetting(SETTING_SESSIONS_SORT, (DWORD)g_view.sorting) ||
        !Util_SetSetting(SETTING_SESSIONS_EMPTY, (DWORD)g_view.showEmptyGroups) ||
        !Util_SetSetting(SETTING_SESSIONS_PRS, (DWORD)g_view.showPullRequests) || !Util_SetSetting(SETTING_SESSIONS_SEARCH, g_view.searchIn) ||
        !Util_SetSetting(SETTING_SESSIONS_DESC, (DWORD)g_view.descending) ||
        !Util_SetSetting(SETTING_SESSIONS_RESULT, (DWORD)g_view.resultFilter))
        Util_Log(L"sessions: the filter menu's choices could not be kept (error %lu)", GetLastError());
}

/* The search as a regular expression, compiled once; NULL when the search
 * is no regular expression, or not one it takes. */
static void CompileSearch(void)
{
    if ((g_view.searchIn & SEARCH_REGEX) && g_view.filter[0]) {
        if (!g_view.regex) g_view.regex = (CoreRegex *)HeapAlloc(GetProcessHeap(), 0, sizeof *g_view.regex);
        if (g_view.regex && Core_RegexCompile(g_view.regex, g_view.filter, !(g_view.searchIn & SEARCH_CASE))) return;
    }
    if (g_view.regex) HeapFree(GetProcessHeap(), 0, g_view.regex);
    g_view.regex = NULL;
}

/* The search in the conversations, stopped; what it found, forgotten. */
static void StopTextSearch(void)
{
    if (g_view.dlg) {
        KillTimer(g_view.dlg, TIMER_SEARCH);
        KillTimer(g_view.dlg, TIMER_SEARCH_REFILL);
    }
    SessionSearch_Stop();
    g_view.textSearch++;   /* what an older search posted is ignored */
    g_view.textSearching = g_view.textRefillPending = g_view.textRefreshPending = FALSE;
    g_view.textFoundCount = g_view.textKeyCount = 0;
}

/* The conversations of the sessions the filter menu chose in the profile
 * shown, searched for the text typed, on a thread of their own. */
static void StartTextSearch(void)
{
    WCHAR (*paths)[LONG_PATH_CCH];
    int p = ShownProfileIndex(), r, count = 0;
    if (p < 0 || !g_view.set.rowCount || !g_view.filter[0] || ((g_view.searchIn & SEARCH_REGEX) && !g_view.regex)) return;
    paths = (WCHAR (*)[LONG_PATH_CCH])HeapAlloc(GetProcessHeap(), 0, (size_t)g_view.set.rowCount * sizeof *paths);
    if (!paths) return;
    if (g_view.textKeys) HeapFree(GetProcessHeap(), 0, g_view.textKeys);
    g_view.textKeys = (WCHAR (*)[SESSION_ID_CCH])HeapAlloc(GetProcessHeap(), 0, (size_t)g_view.set.rowCount * sizeof *g_view.textKeys);
    if (g_view.textFound) HeapFree(GetProcessHeap(), 0, g_view.textFound);
    g_view.textFound = (WCHAR (*)[SESSION_ID_CCH])HeapAlloc(GetProcessHeap(), 0, (size_t)g_view.set.rowCount * sizeof *g_view.textFound);
    if (!g_view.textKeys || !g_view.textFound) {
        HeapFree(GetProcessHeap(), 0, paths);
        return;
    }
    for (r = 0; r < g_view.set.rowCount; r++) {
        const SessionRow *row = &g_view.set.rows[r];
        if (!row->transcript || !EntryChosen(EntryOf(row, p))) continue;
        StringCchCopyW(paths[count], LONG_PATH_CCH, row->transcriptPath);
        StringCchCopyW(g_view.textKeys[count], SESSION_ID_CCH, row->key);
        count++;
    }
    g_view.textKeyCount = count;
    g_view.textSearching = SessionSearch_Start(g_view.dlg, ++g_view.textSearch, g_view.filter, (g_view.searchIn & SEARCH_REGEX) != 0,
                                               (g_view.searchIn & SEARCH_CASE) != 0, paths, count);
    InvalidateRect(g_view.tree, NULL, FALSE);   /* the empty tree says it searches */
}

/* A snapshot can change message text without changing any tree row. Let an
 * in-flight search finish before refreshing, so a stream cannot starve it. */
static BOOL RefreshTextSearch(void)
{
    if (!g_view.shown || !(g_view.searchIn & SEARCH_TEXT) || !g_view.filter[0]) return FALSE;
    if (g_view.textSearching) {
        g_view.textRefreshPending = TRUE;
        return FALSE;
    }
    StopTextSearch();
    StartTextSearch();
    return TRUE;
}

static void CALLBACK SearchTimer(HWND window, UINT message, UINT_PTR id, DWORD time)
{
    (void)message;
    (void)time;
    KillTimer(window, id);
    if (id == TIMER_SEARCH) {
        StartTextSearch();
    } else if (g_view.shown) {   /* TIMER_SEARCH_REFILL: what was found meanwhile shows, the view where it is */
        g_view.textRefillPending = FALSE;
        RememberTopRow();
        FillTree();
    }
}

/* The search typed or its options changed: the tree shows what matches the
 * titles and folders at once, the conversations once typing pauses. */
static void ApplySearch(void)
{
    GetWindowTextW(g_view.search, g_view.filter, ARRAYSIZE(g_view.filter));
    CompileSearch();
    StopTextSearch();
    if ((g_view.searchIn & SEARCH_TEXT) && g_view.filter[0] && g_view.dlg) SetTimer(g_view.dlg, TIMER_SEARCH, SEARCH_DELAY_MS, SearchTimer);
    ForgetNeighbors();
    FillTree();
}

BOOL SessionsView_SearchFound(WPARAM search, LPARAM index)
{
    if (!g_view.shown || (ULONG)search != g_view.textSearch) return FALSE;   /* an older search's */
    if (index >= 0 && index < g_view.textKeyCount && g_view.textFound && g_view.textFoundCount < g_view.textKeyCount) {
        StringCchCopyW(g_view.textFound[g_view.textFoundCount++], SESSION_ID_CCH, g_view.textKeys[index]);
        if (!g_view.textRefillPending) {   /* those found together show together */
            g_view.textRefillPending = TRUE;
            SetTimer(g_view.dlg, TIMER_SEARCH_REFILL, SEARCH_REFILL_MS, SearchTimer);
        }
    } else if (index < 0) {
        g_view.textSearching = FALSE;
        KillTimer(g_view.dlg, TIMER_SEARCH_REFILL);
        g_view.textRefillPending = FALSE;
        RememberTopRow();
        FillTree();   /* publish this complete pass before its pending refresh clears the keys */
        if (g_view.textRefreshPending) RefreshTextSearch();
        InvalidateRect(g_view.tree, NULL, FALSE);   /* the empty tree no longer says it searches */
    } else return FALSE;
    return TRUE;
}

/* A menu below `button`, its own window's commands; 0 for none chosen. A
 * click on the button that closes it does not open it again. */
static UINT MenuBelow(HWND button, HMENU menu)
{
    RECT box;
    GetWindowRect(button, &box);
    return Theme_TrackDropDown(g_view.dlg, menu, &box);
}

/* The status and the environments as they were first: everything shown but the archived. */
static BOOL FiltersCleared(void)
{
    return g_view.status == STATUS_ACTIVE && !g_view.environments && !g_view.resultFilter && !g_view.modelFilter[0] && !g_view.effortFilter[0];
}

static BOOL DisplayDefaults(void)
{
    return FiltersCleared() && g_view.grouping == GROUP_DATE && g_view.sorting == SORT_ACTIVITY &&
           g_view.descending && !g_view.showEmptyGroups && g_view.showPullRequests;
}

/* A choice of the filter menu, kept and shown. */
static void FilterCommand(UINT cmd)
{
    UINT choice;
    int p = ShownProfileIndex();
    if (cmd < IDM_FILTER || cmd > IDM_FILTER + FILTER_RESET) return;
    choice = cmd - IDM_FILTER;
    if (choice < STATUSES) {
        g_view.status = (ViewStatus)choice;
    } else if (choice == FILTER_ENV_ALL) {
        g_view.environments = 0;
    } else if (choice >= FILTER_ENV && choice < FILTER_ENV + ARRAYSIZE(kEnvironments)) {
        DWORD bit = kEnvironments[choice - FILTER_ENV];
        g_view.environments = g_view.environments ? g_view.environments ^ bit : bit;
        if (g_view.environments == SESSION_ENV_ALL) g_view.environments = 0;
    } else if (choice >= FILTER_GROUP && choice < FILTER_GROUP + GROUPINGS) {
        g_view.grouping = (Grouping)(choice - FILTER_GROUP);
        if (g_view.grouping == GROUP_CUSTOM && p >= 0) LoadCustomGroups(p, TRUE);   /* as Claude has them now */
    } else if (choice >= FILTER_SORT && choice < FILTER_SORT + SORTINGS) {
        g_view.sorting = (Sorting)(choice - FILTER_SORT);
    } else if (choice == FILTER_DESCENDING || choice == FILTER_ASCENDING) {
        g_view.descending = choice == FILTER_DESCENDING;
    } else if (choice == FILTER_EMPTY) {
        g_view.showEmptyGroups = !g_view.showEmptyGroups;
    } else if (choice == FILTER_PRS) {
        g_view.showPullRequests = !g_view.showPullRequests;
    } else if (choice >= FILTER_RESULT && choice < FILTER_CLEAR) {
        g_view.resultFilter = (int)(choice - FILTER_RESULT);
    } else if (choice == FILTER_CLEAR || choice == FILTER_RESET) {
        g_view.status = STATUS_ACTIVE;
        g_view.environments = 0;
        g_view.resultFilter = 0;
        g_view.modelFilter[0] = g_view.effortFilter[0] = 0;
        if (choice == FILTER_RESET) {
            g_view.grouping = GROUP_DATE;
            g_view.sorting = SORT_ACTIVITY;
            g_view.descending = g_view.showPullRequests = TRUE;
            g_view.showEmptyGroups = FALSE;
        }
    } else return;
    SaveChoices();
    CountSessions();
    InvalidateRect(g_view.profiles, NULL, FALSE);
    if ((g_view.searchIn & SEARCH_TEXT) && g_view.filter[0]) ApplySearch();   /* a previous zero-match search covered the old filters only */
    else {
        ForgetNeighbors();
        FillTree();
    }
}

/* The filter menu, below its button, in the order of Claude's sidebar:
 * Group by, Sort by and Show (pull requests, empty groups), each with its
 * choice on the right; then the filters, Status, the last result and
 * Environment; clearing them, and the defaults. The sessions not on this PC
 * (SSH, WSL, the cloud) are counted under Environment: they are not listed. */
static void FilterMenu(void)
{
    static const Grouping kGroupOrder[] = { GROUP_DATE, GROUP_STATE, GROUP_ENVIRONMENT, GROUP_FOLDER, GROUP_CUSTOM, GROUP_NONE };
    static const Sorting kSortOrder[] = { SORT_ACTIVITY, SORT_CREATED, SORT_NAME };
    WCHAR text[160];
    HMENU menu = CreatePopupMenu(), status = CreatePopupMenu(), environment = CreatePopupMenu(), grouping = CreatePopupMenu(),
          sorting = CreatePopupMenu(), result = CreatePopupMenu(), show = CreatePopupMenu();
    int p = ShownProfileIndex(), i, chosen = 0, last = 0;
    UINT cmd;
    if (!menu || !status || !environment || !grouping || !sorting || !result || !show) {
        if (menu) DestroyMenu(menu);
        if (status) DestroyMenu(status);
        if (environment) DestroyMenu(environment);
        if (grouping) DestroyMenu(grouping);
        if (sorting) DestroyMenu(sorting);
        if (result) DestroyMenu(result);
        if (show) DestroyMenu(show);
        return;
    }
    for (i = 0; i < STATUSES; i++) AppendMenuW(status, MF_STRING, IDM_FILTER + FILTER_STATUS + (UINT)i, StatusName((ViewStatus)i));
    CheckMenuRadioItem(status, IDM_FILTER + FILTER_STATUS, IDM_FILTER + FILTER_STATUS + STATUSES - 1, IDM_FILTER + FILTER_STATUS + (UINT)g_view.status,
                       MF_BYCOMMAND);
    AppendMenuW(environment, MF_STRING | (g_view.environments ? 0 : MF_CHECKED), IDM_FILTER + FILTER_ENV_ALL, TR(L"All environments"));
    AppendMenuW(environment, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < (int)ARRAYSIZE(kEnvironments); i++) {
        BOOL on = (g_view.environments & kEnvironments[i]) != 0;
        AppendMenuW(environment, MF_STRING | (on ? MF_CHECKED : 0), IDM_FILTER + FILTER_ENV + (UINT)i, EnvironmentName(i));
        if (on) chosen++, last = i;
    }
    if (p >= 0 && g_view.set.source[p].elsewhere) {
        int elsewhere = g_view.set.source[p].elsewhere;
        StringCchPrintfW(text, ARRAYSIZE(text), elsewhere == 1 ? TR(L"%d session not on this PC is not listed") : TR(L"%d sessions not on this PC are not listed"),
                         elsewhere);
        AppendMenuW(environment, MF_SEPARATOR, 0, NULL);
        AppendMenuW(environment, MF_STRING | MF_GRAYED, 0, text);
    }
    for (i = 0; i < (int)ARRAYSIZE(kGroupOrder); i++) {
        if (kGroupOrder[i] == GROUP_NONE) AppendMenuW(grouping, MF_SEPARATOR, 0, NULL);
        AppendMenuW(grouping, MF_STRING, IDM_FILTER + FILTER_GROUP + (UINT)kGroupOrder[i], GroupingName(kGroupOrder[i]));
    }
    CheckMenuRadioItem(grouping, IDM_FILTER + FILTER_GROUP, IDM_FILTER + FILTER_GROUP + GROUPINGS - 1, IDM_FILTER + FILTER_GROUP + (UINT)g_view.grouping,
                       MF_BYCOMMAND);
    for (i = 0; i < (int)ARRAYSIZE(kSortOrder); i++)
        AppendMenuW(sorting, MF_STRING, IDM_FILTER + FILTER_SORT + (UINT)kSortOrder[i], SortingName(kSortOrder[i]));
    CheckMenuRadioItem(sorting, IDM_FILTER + FILTER_SORT, IDM_FILTER + FILTER_SORT + SORTINGS - 1, IDM_FILTER + FILTER_SORT + (UINT)g_view.sorting,
                       MF_BYCOMMAND);
    AppendMenuW(sorting, MF_SEPARATOR, 0, NULL);
    AppendMenuW(sorting, MF_STRING, IDM_FILTER + FILTER_DESCENDING, TR(L"Descending"));
    AppendMenuW(sorting, MF_STRING, IDM_FILTER + FILTER_ASCENDING, TR(L"Ascending"));
    CheckMenuRadioItem(sorting, IDM_FILTER + FILTER_DESCENDING, IDM_FILTER + FILTER_ASCENDING,
                       IDM_FILTER + (g_view.descending ? FILTER_DESCENDING : FILTER_ASCENDING), MF_BYCOMMAND);
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s\t%s", TR(L"&Group by"), GroupingName(g_view.grouping));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)grouping, text);
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s\t%s %s", TR(L"S&ort by"), SortingName(g_view.sorting),
                     g_view.descending ? L"\x2193" : L"\x2191");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sorting, text);
    AppendMenuW(show, MF_STRING | (g_view.showPullRequests ? MF_CHECKED : 0), IDM_FILTER + FILTER_PRS, TR(L"Meta&data (PR status)"));
    AppendMenuW(show, MF_SEPARATOR, 0, NULL);
    AppendMenuW(show, MF_STRING | (g_view.showEmptyGroups ? MF_CHECKED : 0) | (g_view.grouping == GROUP_NONE ? MF_GRAYED : 0),
                IDM_FILTER + FILTER_EMPTY, TR(L"Show e&mpty groups"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)show, TR(L"Sho&w"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s\t%s", TR(L"&Status"), StatusName(g_view.status));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)status, text);
    for (i = 0; i <= SESSION_RESULTS; i++)
        AppendMenuW(result, MF_STRING | (g_view.resultFilter == i ? MF_CHECKED : 0), IDM_FILTER + FILTER_RESULT + (UINT)i,
                    i ? ResultName((SessionResult)(i - 1)) : TR(L"All"));
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s\t%s", TR(L"Last recorded result"),
                     g_view.resultFilter ? ResultName((SessionResult)(g_view.resultFilter - 1)) : TR(L"All"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)result, text);
    StringCchPrintfW(text, ARRAYSIZE(text), L"%s\t%s", TR(L"&Environment"),
                     !chosen ? TR(L"All") : chosen == 1 ? EnvironmentName(last) : TR(L"Several"));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)environment, text);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (FiltersCleared() ? MF_GRAYED : 0), IDM_FILTER + FILTER_CLEAR, TR(L"&Clear filters"));
    AppendMenuW(menu, MF_STRING | (DisplayDefaults() ? MF_GRAYED : 0), IDM_FILTER + FILTER_RESET, TR(L"&Reset to defaults"));
    cmd = MenuBelow(g_view.filterButton, menu);
    DestroyMenu(menu);   /* its submenus with it */
    FilterCommand(cmd);
}

/* The search options, below their button: where the search looks (the
 * titles and folders, the conversations, or both), and how it reads what
 * is typed (a regular expression; the case). */
typedef struct AllSearch {
    HWND list, profiles;
    BOOL *included;
    int row, profile;
    ULONG generation;
} AllSearch;

static const WCHAR *ResultName(SessionResult result)
{
    switch (result) {
    case RESULT_COMPLETED: return TR(L"Completed");
    case RESULT_BLOCKED: return TR(L"Blocked");
    case RESULT_REVIEW: return TR(L"Ready for review");
    case RESULT_ACTION: return TR(L"Needs action");
    default: return TR(L"Unknown or outdated");
    }
}

static void AllSearchAdd(AllSearch *state, int r, int matchedProfile)
{
    const SessionRow *row = &g_view.set.rows[r];
    WCHAR names[NAMES_CCH] = L"";
    LVITEMW item;
    int p, first = -1, at;
    if (state->included[r]) return;
    for (p = 0; p < g_view.set.profiles.count; p++)
        if (EntryChosen(EntryOf(row, p))) {
            if (first < 0) first = p;
            else StringCchCatW(names, ARRAYSIZE(names), L", ");
            StringCchCatW(names, ARRAYSIZE(names), ProfileAt(p)->name);
        }
    if (first < 0) return;
    if (matchedProfile >= 0) first = matchedProfile;
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = ListView_GetItemCount(state->list);
    item.pszText = (LPWSTR)ShownTitle(row, first);
    item.lParam = r;
    at = ListView_InsertItem(state->list, &item);
    if (at < 0) return;
    ListView_SetItemText(state->list, at, 1, names);
    ListView_SetItemText(state->list, at, 2, (LPWSTR)row->cwd);
    ListView_SetItemText(state->list, at, 3, (LPWSTR)ResultName(EntryOf(row, first)->result));
    state->included[r] = TRUE;
}

static void AllSearchSelection(HWND dialog, AllSearch *state)
{
    LVITEMW item;
    int selected = ListView_GetNextItem(state->list, -1, LVNI_SELECTED), p;
    SendMessageW(state->profiles, CB_RESETCONTENT, 0, 0);
    state->row = -1;
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_PARAM;
    item.iItem = selected;
    if (selected >= 0 && ListView_GetItem(state->list, &item)) {
        state->row = (int)item.lParam;
        for (p = 0; p < g_view.set.profiles.count; p++) {
            const SessionEntry *entry = EntryOf(&g_view.set.rows[state->row], p);
            if (EntryChosen(entry) && ListedAndKept(entry)) {
                LRESULT at = SendMessageW(state->profiles, CB_ADDSTRING, 0, (LPARAM)ProfileAt(p)->name);
                if (at >= 0) SendMessageW(state->profiles, CB_SETITEMDATA, (WPARAM)at, p);
            }
        }
    }
    SendMessageW(state->profiles, CB_SETCURSEL, 0, 0);
    EnableWindow(GetDlgItem(dialog, IDOK), state->row >= 0 && SendMessageW(state->profiles, CB_GETCOUNT, 0, 0) > 0);
    EnableWindow(GetDlgItem(dialog, IDC_ST_HIT), state->row >= 0 && g_view.set.rows[state->row].transcript);
}

static INT_PTR CALLBACK AllSearchProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    AllSearch *state = (AllSearch *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        const WCHAR *titles[] = { TR(L"Session title"), TR(L"Profiles"), TR(L"Folder"), TR(L"Last recorded result") };
        static const int widths[] = { 240, 170, 240, 150 };
        WCHAR (*paths)[LONG_PATH_CCH];
        int r, p;
        size_t i;
        state = (AllSearch *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        state->list = GetDlgItem(dialog, IDC_X_LIST);
        state->profiles = GetDlgItem(dialog, IDC_X_PROFILE);
        SetWindowTextW(dialog, TR(L"Search all profiles"));
        SetDlgItemTextW(dialog, IDC_X_TEXT, g_view.filter);
        SetDlgItemTextW(dialog, IDC_X_LABEL, TR(L"Open in:"));
        SetDlgItemTextW(dialog, IDC_X_NOTE, TR(L"Current filters apply. Each local session is listed once. Choose a profile to open it."));
        SetDlgItemTextW(dialog, IDOK, TR(L"Open"));
        SetDlgItemTextW(dialog, IDC_ST_HIT, TR(L"First matching message\x2026"));
        SetDlgItemTextW(dialog, IDCANCEL, TR(L"Close"));
        ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        for (i = 0; i < ARRAYSIZE(titles); i++) {
            LVCOLUMNW column;
            ZeroMemory(&column, sizeof column);
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = (LPWSTR)titles[i];
            column.cx = MulDiv(widths[i], (int)GetDpiForWindow(dialog), 96);
            ListView_InsertColumn(state->list, (int)i, &column);
        }
        Theme_SmoothView(state->list);
        for (r = 0; r < g_view.set.rowCount; r++)
            for (p = 0; p < g_view.set.profiles.count; p++)
                if (EntryChosen(EntryOf(&g_view.set.rows[r], p)) && (g_view.searchIn & SEARCH_TITLES) && Matches(&g_view.set.rows[r], p)) {
                    AllSearchAdd(state, r, p);
                    break;
                }
        AllSearchSelection(dialog, state);
        if (!(g_view.searchIn & SEARCH_TEXT)) return TRUE;
        paths = (WCHAR (*)[LONG_PATH_CCH])HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                  (size_t)max(g_view.set.rowCount, 1) * sizeof *paths);
        if (!paths) return TRUE;
        for (r = 0; r < g_view.set.rowCount; r++)
            for (p = 0; p < g_view.set.profiles.count; p++)
                if (g_view.set.rows[r].transcript && EntryChosen(EntryOf(&g_view.set.rows[r], p))) {
                    StringCchCopyW(paths[r], LONG_PATH_CCH, g_view.set.rows[r].transcriptPath);
                    break;
                }
        if (!SessionSearch_Start(dialog, state->generation, g_view.filter, (g_view.searchIn & SEARCH_REGEX) != 0,
                                 (g_view.searchIn & SEARCH_CASE) != 0, paths, g_view.set.rowCount))
            SetDlgItemTextW(dialog, IDC_X_NOTE, TR(L"The conversations could not be searched. Only title and folder matches are shown."));
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_APP_SESSION_SEARCH && (ULONG)wp == state->generation) {
        if (lp >= 0 && lp < g_view.set.rowCount) AllSearchAdd(state, (int)lp, -1);
        else if (lp == -1) SetDlgItemTextW(dialog, IDC_X_TEXT, TR(L"Search complete"));
        return TRUE;
    }
    if (message == WM_NOTIFY && ((const NMHDR *)lp)->hwndFrom == state->list && ((const NMHDR *)lp)->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
        if ((change->uChanged & LVIF_STATE) && ((change->uOldState ^ change->uNewState) & LVIS_SELECTED)) AllSearchSelection(dialog, state);
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDC_ST_HIT) {
        if (state->row >= 0) ShowMatchingContext(dialog, state->row);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDOK) {
        LRESULT at = SendMessageW(state->profiles, CB_GETCURSEL, 0, 0);
        if (state->row >= 0 && at >= 0) {
            state->profile = (int)SendMessageW(state->profiles, CB_GETITEMDATA, (WPARAM)at, 0);
            EndDialog(dialog, IDOK);
        }
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}

void SessionsView_SearchAll(void)
{
    AllSearch state;
    INT_PTR result;
    if (!g_view.shown || !g_view.filter[0] || ((g_view.searchIn & SEARCH_REGEX) && !g_view.regex)) return;
    ZeroMemory(&state, sizeof state);
    state.included = (BOOL *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)max(g_view.set.rowCount, 1) * sizeof(BOOL));
    if (!state.included) return;
    StopTextSearch();
    state.generation = ++g_view.textSearch;
    g_view.actionDepth++;
    result = Ui_Dialog(g_view.dlg, IDD_SESSIONSEARCH, AllSearchProc, (LPARAM)&state);
    SessionSearch_Stop();
    if (result == IDOK && state.profile >= 0 && state.profile < g_view.set.profiles.count) OpenSession(state.row, state.profile);
    g_view.actionDepth--;
    TakeHeldSnapshot();
    HeapFree(GetProcessHeap(), 0, state.included);
    ApplySearch();
}

static INT_PTR CALLBACK ExecutionFilterProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    int i;
    (void)lp;
    if (message == WM_INITDIALOG) {
        SetWindowTextW(dialog, TR(L"Execution filters"));
        SetDlgItemTextW(dialog, IDC_ST_MODEL_LABEL, TR(L"Model:"));
        SetDlgItemTextW(dialog, IDC_ST_EFFORT_LABEL, TR(L"Effort:"));
        SetDlgItemTextW(dialog, IDOK, TR(L"Apply"));
        SetDlgItemTextW(dialog, IDCANCEL, TR(L"Cancel"));
        for (i = 0; i < g_view.set.entryCount; i++) {
            const SessionEntry *entry = &g_view.set.entries[i];
            if (entry->model[0] && SendDlgItemMessageW(dialog, IDC_ST_MODEL, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)entry->model) == CB_ERR)
                SendDlgItemMessageW(dialog, IDC_ST_MODEL, CB_ADDSTRING, 0, (LPARAM)entry->model);
            if (entry->effort[0] && SendDlgItemMessageW(dialog, IDC_ST_EFFORT, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)entry->effort) == CB_ERR)
                SendDlgItemMessageW(dialog, IDC_ST_EFFORT, CB_ADDSTRING, 0, (LPARAM)entry->effort);
        }
        SetDlgItemTextW(dialog, IDC_ST_MODEL, g_view.modelFilter);
        SetDlgItemTextW(dialog, IDC_ST_EFFORT, g_view.effortFilter);
        SendDlgItemMessageW(dialog, IDC_ST_MODEL, CB_LIMITTEXT, ARRAYSIZE(g_view.modelFilter) - 1, 0);
        SendDlgItemMessageW(dialog, IDC_ST_EFFORT, CB_LIMITTEXT, ARRAYSIZE(g_view.effortFilter) - 1, 0);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDOK) {
        GetDlgItemTextW(dialog, IDC_ST_MODEL, g_view.modelFilter, ARRAYSIZE(g_view.modelFilter));
        GetDlgItemTextW(dialog, IDC_ST_EFFORT, g_view.effortFilter, ARRAYSIZE(g_view.effortFilter));
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}

static void FindMenu(void)
{
    HMENU menu = CreatePopupMenu();
    UINT cmd;
    DWORD bit;
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | ((g_view.searchIn & SEARCH_TITLES) ? MF_CHECKED : 0), IDM_FIND + 0, TR(L"Search &titles and folders"));
    AppendMenuW(menu, MF_STRING | ((g_view.searchIn & SEARCH_TEXT) ? MF_CHECKED : 0), IDM_FIND + 1, TR(L"Search the con&versations"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | ((g_view.searchIn & SEARCH_REGEX) ? MF_CHECKED : 0), IDM_FIND + 2, TR(L"&Regular expression"));
    AppendMenuW(menu, MF_STRING | ((g_view.searchIn & SEARCH_CASE) ? MF_CHECKED : 0), IDM_FIND + 3, TR(L"Match &case"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (g_view.filter[0] ? 0 : MF_GRAYED), IDM_FIND + 4, TR(L"Search all profiles"));
    AppendMenuW(menu, MF_STRING, IDM_FIND + 5, TR(L"Pending changes"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_FIND + 6, TR(L"Execution filters\x2026"));
    AppendMenuW(menu, MF_STRING, IDM_FIND + 7, TR(L"Historical accounts and organizations\x2026"));
    AppendMenuW(menu, MF_STRING, IDM_FIND + 8, TR(L"SSH, WSL and cloud metadata\x2026"));
    cmd = MenuBelow(g_view.findButton, menu);
    DestroyMenu(menu);
    if (cmd == IDM_FIND + 4) { SessionsView_SearchAll(); return; }
    if (cmd == IDM_FIND + 6) {
        if (Ui_Dialog(g_view.dlg, IDD_SESSIONFILTER, ExecutionFilterProc, 0) == IDOK) ApplySearch();
        return;
    }
    if (cmd == IDM_FIND + 7 || cmd == IDM_FIND + 8) {
        int p = ShownProfileIndex();
        if (p >= 0) {
            g_view.actionDepth++;
            SessionTools_Catalog(g_view.dlg, ProfileAt(p), cmd == IDM_FIND + 7);
            g_view.actionDepth--;
            TakeHeldSnapshot();
        }
        return;
    }
    if (cmd == IDM_FIND + 5) {
        g_view.actionDepth++;
        SyncUi_Pending(g_view.dlg, &g_view.set.profiles);
        g_view.actionDepth--;
        TakeHeldSnapshot();
        return;
    }
    if (cmd < IDM_FIND || cmd > IDM_FIND + 3) return;
    bit = 1u << (cmd - IDM_FIND);
    g_view.searchIn ^= bit;
    if (!(g_view.searchIn & (SEARCH_TITLES | SEARCH_TEXT))) g_view.searchIn |= bit == SEARCH_TITLES ? SEARCH_TEXT : SEARCH_TITLES;   /* it looks somewhere */
    SaveChoices();
    ApplySearch();
}

void SessionsView_Init(HWND dlg)
{
    g_view.dlg = dlg;
    g_view.profiles = GetDlgItem(dlg, IDC_S_PROFILES);
    g_view.tree = GetDlgItem(dlg, IDC_S_TREE);
    g_view.search = GetDlgItem(dlg, IDC_S_SEARCH);
    g_view.findButton = GetDlgItem(dlg, IDC_S_FIND);
    g_view.filterButton = GetDlgItem(dlg, IDC_S_FILTER);
    g_view.treeArea = Theme_SmoothView(g_view.tree);
    Theme_SmoothView(g_view.profiles);
    SetWindowSubclass(g_view.tree, TreeSubclass, 1, 0);
    SetWindowSubclass(g_view.profiles, ProfilesSubclass, 1, 0);
    g_view.drag.hot = g_view.drag.target = -1;
    SendMessageW(g_view.search, EM_LIMITTEXT, ARRAYSIZE(g_view.filter) - 1, 0);
    LoadChoices();
}

BOOL SessionsView_Shown(void)
{
    return g_view.shown;
}

const WCHAR *SessionsView_Profile(void)
{
    int p = ShownProfileIndex();
    return p >= 0 ? ProfileAt(p)->folder : g_view.folder;
}

/* Controls are filled only while visible. A warm snapshot does no hidden
 * tree insertion or icon rendering on the manager's startup path. */
static void Render(BOOL force)
{
    if (!g_view.shown) return;
    if (g_view.rendered && !force && !g_view.dirty) {
        InvalidateRect(g_view.profiles, NULL, FALSE);
        return;
    }
    Measure();
    FillProfiles();
    if (g_view.loaded) RestorePlace();   /* the first fill with sessions: where the last run left the tree */
    FillTree();
    StringCchCopyW(g_view.renderedFolder, ARRAYSIZE(g_view.renderedFolder), g_view.folder);
    g_view.rendered = TRUE;
    g_view.dirty = FALSE;
}

/* What the waiting changes of profile `p` of `set` are tried with: its
 * queue file as it is now and how many changes its entries can take. */
static ULONGLONG WaitingChangesState(const SessionSet *set, int p)
{
    WCHAR path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA file;
    ULONGLONG state = Core_HashText(CORE_HASH_START, set->profiles.items[p].folder);
    int queue;
    for (queue = 0; queue < 2; queue++) {   /* its changes, and the sessions sent to it */
        ZeroMemory(&file, sizeof file);
        if (!(queue ? SessionSync_PlanPath(&set->profiles.items[p], path, ARRAYSIZE(path))
                    : SessionStore_PendingPath(&set->profiles.items[p], path, ARRAYSIZE(path))) ||
            !GetFileAttributesExW(path, GetFileExInfoStandard, &file))
            ZeroMemory(&file, sizeof file);
        state = Core_HashBytes(state, &file.ftLastWriteTime, sizeof file.ftLastWriteTime);
        state = Core_HashBytes(state, &file.nFileSizeHigh, sizeof file.nFileSizeHigh);
        state = Core_HashBytes(state, &file.nFileSizeLow, sizeof file.nFileSizeLow);
    }
    return Core_HashBytes(state, &set->source[p].pending, sizeof set->source[p].pending);
}

/* The changes waiting for profiles that have closed, made on a thread of
 * their own: making them takes the lock every sync takes (Util_SyncLock),
 * which a sync under way holds as long as it runs, minutes at most (the
 * session vault's, or a watcher's once its Claude closed, which is when a
 * snapshot finds a profile closed with changes waiting). The reader thread
 * never writes entries; SessionEdit checks each profile is still closed. */
typedef struct WaitingWork {
    HWND          dlg;
    ProfileList   profiles;             /* the snapshot's */
    DWORD         tried;                /* the profiles whose changes are made, one bit each */
    ULONGLONG     state[MAX_PROFILES];  /* WaitingChangesState of each as they were tried */
    DWORD         made;                 /* those that made any (the thread's until it ends) */
    volatile LONG ended;
} WaitingWork;

#define WAITING_STOP_MS 10000           /* the view's end waits this long at most for changes being made */

static DWORD WINAPI MakeWaitingChanges(void *param)
{
    WaitingWork *work = (WaitingWork *)param;
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);   /* removals go to the Recycle Bin */
    int p;
    for (p = 0; p < work->profiles.count; p++)
        if ((work->tried & (1u << p)) && SessionEdit_ApplyPending(work->dlg, &work->profiles.items[p]) > 0) work->made |= 1u << p;
    if (SUCCEEDED(com)) CoUninitialize();
    InterlockedExchange(&work->ended, 1);
    PostMessageW(work->dlg, WM_APP_SESSIONS_READY, 0, 0);   /* SessionsView_Ready takes the result */
    return 0;
}

/* The changes waiting for the profiles of `set` that are closed, made unless
 * a thread makes some already (the snapshot after it tries again). A
 * profile's count holds only the changes its entries can take; when none of
 * them can be made (their queue cannot be written, for one), they are tried
 * again only once the queue or that count changed. */
static void StartWaitingChanges(const SessionSet *set)
{
    WaitingWork *work;
    int p;
    if (g_view.waitingThread || (work = (WaitingWork *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *work)) == NULL) return;
    for (p = 0; p < set->profiles.count; p++) {
        if (!set->source[p].pending || set->profiles.items[p].running) continue;
        work->state[p] = WaitingChangesState(set, p);
        if (work->state[p] != g_view.waitingChangesTried[p]) work->tried |= 1u << p;
    }
    if (!work->tried) {
        HeapFree(GetProcessHeap(), 0, work);
        return;
    }
    work->dlg = g_view.dlg;
    work->profiles = set->profiles;
    g_view.waitingWork = work;
    if ((g_view.waitingThread = CreateThread(NULL, 0, MakeWaitingChanges, work, 0, NULL)) == NULL) {
        Util_Log(L"sessions: the changes waiting for closed profiles are made here (no thread, error %lu)", GetLastError());
        MakeWaitingChanges(work);
    }
}

/* What the thread made: a profile whose changes were all tried in vain is
 * tried again once its queue or its entries change; one that made any is
 * read again, the snapshot shown being out of date. */
static void TakeWaitingChanges(void)
{
    WaitingWork *work = g_view.waitingWork;
    BOOL made;
    int p;
    if (!work || !InterlockedCompareExchange(&work->ended, 0, 0)) return;
    if (g_view.waitingThread) {
        WaitForSingleObject(g_view.waitingThread, INFINITE);   /* it ends right after posting */
        CloseHandle(g_view.waitingThread);
    }
    g_view.waitingThread = NULL;
    g_view.waitingWork = NULL;
    for (p = 0; p < work->profiles.count; p++)
        if ((work->tried & (1u << p)) && !(work->made & (1u << p))) g_view.waitingChangesTried[p] = work->state[p];
    made = work->made != 0;
    HeapFree(GetProcessHeap(), 0, work);
    if (made) RequestLoad(TRUE);
}

/* The view ends: changes being made finish, for WAITING_STOP_MS at most (a
 * sync under way may hold their lock longer); the thread then ends unwaited,
 * its work kept, posting to no window. */
static void StopWaitingChanges(void)
{
    if (!g_view.waitingThread) return;
    if (WaitForSingleObject(g_view.waitingThread, WAITING_STOP_MS) == WAIT_OBJECT_0) {
        HeapFree(GetProcessHeap(), 0, g_view.waitingWork);
    } else {
        Util_Log(L"sessions: the changes waiting for closed profiles were still being made");
    }
    CloseHandle(g_view.waitingThread);
    g_view.waitingThread = NULL;
    g_view.waitingWork = NULL;
}

/* The snapshot the loader published, shown: unless it went out of date
 * since it was read (RequestLoad's restart). */
static void AcceptSnapshot(void)
{
    SessionLoader *loader = g_view.loader;
    SessionSet *fresh;
    ULONGLONG treeHash, snapshotHash;
    BOOL failed;
    AcquireSRWLockExclusive(&loader->lock);
    fresh = loader->ready;
    treeHash = loader->readyTreeHash;
    snapshotHash = loader->readySnapshotHash;
    failed = loader->failed && loader->readyGeneration == loader->generation;
    loader->ready = NULL;
    loader->failed = FALSE;
    if (loader->readyGeneration != loader->generation) {
        FreeSnapshot(fresh);
        fresh = NULL;
    }
    if (fresh) g_view.planned = loader->readyPlan;
    ReleaseSRWLockExclusive(&loader->lock);
    if (failed) ShowLoadFailed();
    if (!fresh) return;
    StartWaitingChanges(fresh);
    /* The same as the one shown (a notification of a write that changed
     * nothing listed, or one of ours read already): nothing is drawn again. */
    if (g_view.loaded && snapshotHash == g_view.snapshotHash) {
        FreeSnapshot(fresh);
        g_view.loading = FALSE;
        Watch(&g_view.planned);
        RefreshTextSearch();   /* a transcript rewritten to the same size */
        return;
    }
    /* While the tree's rows still name the rows of the snapshot shown. */
    if (g_view.shown && g_view.loaded && treeHash != g_view.treeHash) RememberTopRow();
    if (g_view.rendered) RememberNeighbors();
    g_view.dirty = g_view.dirty || !g_view.loaded || treeHash != g_view.treeHash;
    SessionStore_Free(&g_view.set);
    g_view.set = *fresh;
    HeapFree(GetProcessHeap(), 0, fresh);
    g_view.loaded = TRUE;
    g_view.loading = FALSE;
    g_view.treeHash = treeHash;
    g_view.snapshotHash = snapshotHash;
    CountSessions();
    Watch(&g_view.planned);
    RefreshTextSearch();   /* the previous pass stays drawn until this one publishes results */
    Render(FALSE);
}

void SessionsView_Ready(BOOL allowChanges)
{
    if (!allowChanges || g_view.actionDepth || g_view.accepting) return;
    g_view.accepting = TRUE;
    TakeWaitingChanges();
    if (g_view.loader) AcceptSnapshot();
    /* A notification handled while the manager was busy asked for no
     * snapshot: ask now (it also lets the watcher notify again). */
    if (InterlockedCompareExchange(&g_reloadMessagePending, 0, 0)) AskForSnapshot(FALSE, FALSE);
    g_view.accepting = FALSE;
}

void SessionsView_Enter(const ClaudePackage *pkg, const WCHAR *folder)
{
    size_t i;
    int shownProfile;
    g_view.pkg = pkg;
    if (folder && !Core_EqualsI(folder, g_view.folder)) ForgetNeighbors();
    if (folder) StringCchCopyW(g_view.folder, ARRAYSIZE(g_view.folder), folder);
    SessionsView_Warm(&g_view.profilesToRead);
    SessionsView_Ready(TRUE);          /* before the view shows: it renders once, below */
    g_view.shown = TRUE;
    if ((g_view.searchIn & SEARCH_TEXT) && g_view.filter[0]) {
        StopTextSearch();
        StartTextSearch();
    }
    /* Shown again as it was left, its place too, unless another profile is shown or the snapshot changed. */
    Render(!g_view.rendered || !Core_EqualsI(g_view.renderedFolder, g_view.folder));
    for (i = 0; i < ARRAYSIZE(kControls); i++)
        ShowWindow(GetDlgItem(g_view.dlg, kControls[i]),
                   g_view.searchHidden && (kControls[i] == IDC_S_SEARCH || kControls[i] == IDC_S_FIND) ? SW_HIDE : SW_SHOW);
    /* The profile shown, in sight in the side bar (the tree's selection is: FillTree). */
    if ((shownProfile = ShownProfileIndex()) >= 0) SendMessageW(g_view.profiles, LB_SETCARETINDEX, (WPARAM)shownProfile, FALSE);
}

void SessionsView_Leave(void)
{
    size_t i;
    KeepPlace();
    for (i = 0; i < ARRAYSIZE(kControls); i++) ShowWindow(GetDlgItem(g_view.dlg, kControls[i]), SW_HIDE);
    g_view.shown = FALSE;
}

void SessionsView_Reload(void)
{
    SessionSearch_Invalidate();
    RequestLoad(FALSE);
}

/* A notification's WM_APP_SESSIONS. None is read when a snapshot was asked
 * for since it was posted: that read starts after the change, which is most
 * often one of ours (an action writes, then asks for the snapshot itself). */
void SessionsView_Notified(void)
{
    if (InterlockedCompareExchange(&g_reloadMessagePending, 0, 0)) AskForSnapshot(FALSE, FALSE);
}

/* A folder a handle is open in cannot be moved or renamed (Windows answers
 * "access denied"), and the watcher holds one in every profile's folder:
 * it stops while profile folders go to the Recycle Bin or are linked,
 * and no snapshot that arrives meanwhile starts it again. */
void SessionsView_PauseWatching(void)
{
    g_view.watchPaused++;
    StopWatching();
}

/* The next snapshot watches the folders as they are then. */
void SessionsView_ResumeWatching(void)
{
    if (g_view.watchPaused > 0 && --g_view.watchPaused == 0 && g_view.warmed) RequestLoad(FALSE);
}

/* Fonts or scale changed: everything measured and filled again, the row on
 * top of the view kept there. */
void SessionsView_Relayout(void)
{
    if (!g_view.shown) return;
    RememberTopRow();
    Render(TRUE);
}

BOOL SessionsView_ClearSearch(void)
{
    if (!g_view.shown || GetFocus() != g_view.search || GetWindowTextLengthW(g_view.search) == 0) return FALSE;
    SetWindowTextW(g_view.search, L"");
    return TRUE;
}

void SessionsView_ToggleSearch(void)
{
    if (!g_view.shown) return;
    if (!g_view.searchHidden && GetFocus() != g_view.search) {
        SetFocus(g_view.search);   /* shown, the focus elsewhere: into it first */
        SendMessageW(g_view.search, EM_SETSEL, 0, -1);
        return;
    }
    g_view.searchHidden = !g_view.searchHidden;
    ShowWindow(g_view.search, g_view.searchHidden ? SW_HIDE : SW_SHOW);
    ShowWindow(g_view.findButton, g_view.searchHidden ? SW_HIDE : SW_SHOW);
    if (!g_view.searchHidden) {
        SetFocus(g_view.search);
        SendMessageW(g_view.search, EM_SETSEL, 0, -1);
        return;
    }
    SetFocus(g_view.tree);
    if (GetWindowTextLengthW(g_view.search)) SetWindowTextW(g_view.search, L"");   /* hidden, it keeps nothing out (EN_CHANGE) */
}

BOOL SessionsView_Command(WPARAM wp)
{
    switch (LOWORD(wp)) {
    case IDOK:   /* Enter: the session selected opened, or Show more */
        if (g_view.shown && GetFocus() == g_view.tree) {
            HTREEITEM selected = TreeView_GetSelection(g_view.tree);
            if (SelectedRow() >= 0) RunAction(ACT_OPEN, -1);
            else if (selected && IsMoreNode(NodeParam(selected))) ShowMore(selected, TRUE);
        }
        return g_view.shown;
    case IDC_S_SEARCH:
        if (HIWORD(wp) == EN_CHANGE && g_view.shown) ApplySearch();
        return TRUE;
    case IDC_S_FIND:
        if (HIWORD(wp) == BN_CLICKED && g_view.shown) FindMenu();
        return TRUE;
    case IDC_S_FILTER:
        if (HIWORD(wp) == BN_CLICKED && g_view.shown) FilterMenu();
        return TRUE;
    case IDC_S_FOLD:
        if (HIWORD(wp) == BN_CLICKED && g_view.shown) {
            HTREEITEM item;
            BOOL open = TRUE;
            for (item = TreeView_GetRoot(g_view.tree); item && open; item = TreeView_GetNextSibling(g_view.tree, item))
                if (NodeParam(item) < 0 && (TreeView_GetItemState(g_view.tree, item, TVIS_EXPANDED) & TVIS_EXPANDED)) open = FALSE;
            FoldAll(open);
            ShowFoldButton();
        }
        return TRUE;
    case IDC_S_PROFILES:
        if (HIWORD(wp) == LBN_SELCHANGE && g_view.shown) {
            LRESULT i = SendMessageW(g_view.profiles, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < g_view.set.profiles.count && !Core_EqualsI(ProfileAt((int)i)->folder, g_view.folder)) {
                SaveProfilePlace();   /* the profile being left: its scroll and how far shown, to come back to */
                StringCchCopyW(g_view.folder, ARRAYSIZE(g_view.folder), ProfileAt((int)i)->folder);
                if (!LoadProfilePlace(g_view.folder)) {
                    g_view.moreCount = 0;   /* not shown yet: its headings start at their first sessions, at the top */
                    g_view.topRow.key[0] = g_view.topRowNext.key[0] = 0;
                    g_view.keepPlace = FALSE;
                }
                ClearMarks();
                ApplySearch();   /* its conversations searched, if they are */
            }
        }
        return TRUE;
    }
    return FALSE;
}

int SessionsView_ChooseProfileAt(HWND from, LPARAM pos)
{
    LRESULT hit;
    POINT pt;
    if (!g_view.shown || !g_view.profiles || (from != g_view.profiles && from != GetParent(g_view.profiles))) return -1;
    if (pos == (LPARAM)-1) return SendMessageW(g_view.profiles, LB_GETCURSEL, 0, 0) >= 0;
    pt.x = GET_X_LPARAM(pos);
    pt.y = GET_Y_LPARAM(pos);
    ScreenToClient(g_view.profiles, &pt);
    hit = SendMessageW(g_view.profiles, LB_ITEMFROMPOINT, 0, MAKELPARAM(pt.x, pt.y));
    if (HIWORD(hit) || (int)LOWORD(hit) >= g_view.set.profiles.count) return 0;   /* below the rows */
    if ((LRESULT)LOWORD(hit) != SendMessageW(g_view.profiles, LB_GETCURSEL, 0, 0)) {
        SendMessageW(g_view.profiles, LB_SETCURSEL, LOWORD(hit), 0);
        SessionsView_Command(MAKEWPARAM(IDC_S_PROFILES, LBN_SELCHANGE));
    }
    return 1;
}

/* WM_CONTEXTMENU: the tree's menu, at the mouse or (from the keyboard) at
 * the selected row, kept on the view when that row is scrolled out of it.
 * A session among several chosen: the menu of them all, the choice kept; a
 * folder's row or Pinned: the menu of its sessions; below the rows: export
 * and import. */
BOOL SessionsView_ContextMenu(HWND from, LPARAM pos)
{
    POINT pt;
    HTREEITEM item;
    LPARAM node;
    int *rows, count;
    BOOL several;
    if (!g_view.shown || from != g_view.tree) return FALSE;
    if (pos == (LPARAM)-1) {
        RECT rc, view;
        item = TreeView_GetSelection(g_view.tree);
        if (!item || !TreeView_GetItemRect(g_view.tree, item, &rc, TRUE)) return TRUE;
        pt.x = rc.left;
        pt.y = rc.bottom;
        ClientToScreen(g_view.tree, &pt);
        GetWindowRect(g_view.treeArea, &view);
        pt.x = max(view.left, min(pt.x, view.right));
        pt.y = max(view.top, min(pt.y, view.bottom));
        several = g_view.several && g_view.markedCount >= 2;
    } else {
        TVHITTESTINFO hit;
        pt.x = GET_X_LPARAM(pos);
        pt.y = GET_Y_LPARAM(pos);
        ZeroMemory(&hit, sizeof hit);
        hit.pt = pt;
        ScreenToClient(g_view.tree, &hit.pt);
        if (!TreeView_HitTest(g_view.tree, &hit) || !(hit.flags & (TVHT_ONITEM | TVHT_ONITEMRIGHT | TVHT_ONITEMINDENT | TVHT_ONITEMBUTTON))) {
            AreaMenu(pt);
            return TRUE;
        }
        item = hit.hItem;
        node = NodeParam(item);
        several = g_view.several && g_view.markedCount >= 2 && node >= 0 && node < g_view.set.rowCount && IsMarked(g_view.set.rows[node].key);
        if (!several) {
            ClearMarks();
            TreeView_SelectItem(g_view.tree, item);
        }
    }
    node = NodeParam(item);
    if (several) {
        if ((rows = MarkedRows(&count)) != NULL) {
            BatchMenu(pt, rows, count, TRUE);
            HeapFree(GetProcessHeap(), 0, rows);
        }
    } else if (node >= 0) {
        ShowMenu(pt);
    } else if (node != NODE_NONE && (rows = FolderRows(item, &count)) != NULL) {
        BatchMenu(pt, rows, count, FALSE);
        HeapFree(GetProcessHeap(), 0, rows);
    }
    return TRUE;
}

/* The tip of a session's row: its title, its last result, its folder, when
 * it was last used and how large it is, and where it is open at once. */
static void RowTip(const SessionRow *row, WCHAR *text, size_t cch)
{
    WCHAR line[NAMES_CCH + 256], date[64], time[32];
    const SessionEntry *entry = EntryOf(row, ShownProfileIndex());
    StringCchCopyW(text, cch, ShownTitle(row, ShownProfileIndex()));
    if (entry && entry->result != RESULT_UNKNOWN) {
        StringCchCatW(text, cch, L"\n");
        StringCchCatW(text, cch, TR(L"Last recorded result"));
        StringCchCatW(text, cch, L": ");
        StringCchCatW(text, cch, ResultName(entry->result));
        StringCchCatW(text, cch, L"\n");
        StringCchCatW(text, cch, entry->resultDetail);
    }
    if (row->cwd[0]) {
        StringCchCatW(text, cch, L"\n");
        StringCchCatW(text, cch, row->cwd);
    }
    if (Util_FormatWhen(row->lastActivity, TIME_NOSECONDS, date, ARRAYSIZE(date), time, ARRAYSIZE(time))) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"\nLast used %s %s"), date, time);
        StringCchCatW(text, cch, line);
    }
    if (row->transcript) {
        Core_FormatSize(row->transcriptBytes, date, ARRAYSIZE(date));
        StringCchPrintfW(line, ARRAYSIZE(line), L"\n%s", date);
    } else {
        StringCchCopyW(line, ARRAYSIZE(line), TR(L"\nNo conversation on disk"));
    }
    StringCchCatW(text, cch, line);
    if (Core_BitCount(row->live) >= 2) {
        WCHAR names[NAMES_CCH];
        Profiles_JoinNames(&g_view.set.profiles, row->live, names, ARRAYSIZE(names));
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"\nOpen in %s at once: go on in one of them only."), names);
        StringCchCatW(text, cch, line);
    }
}

/* A key pressed in the tree; 1 keeps it from the tree's incremental search. */
static LRESULT TreeKey(WORD vk)
{
    int r = SelectedRow();
    const SessionEntry *entry = r >= 0 ? EntryOf(&g_view.set.rows[r], ShownProfileIndex()) : NULL;
    BOOL shift = GetKeyState(VK_SHIFT) < 0;
    HTREEITEM selected = TreeView_GetSelection(g_view.tree);
    if (g_view.several && g_view.markedCount >= 2) {
        if (vk == VK_DELETE) RunOnMarks(IDM_BATCH + (shift ? BATCH_DELETE_ALL : BATCH_REMOVE));
        return 0;
    }
    if (vk == VK_SPACE && selected && IsMoreNode(NodeParam(selected))) {
        ShowMore(selected, TRUE);
        return 1;
    }
    if (r >= 0 && vk == VK_SPACE) {
        RunAction(ACT_PREVIEW, -1);
        return 1;   /* not a letter of the tree's incremental search */
    }
    if (r >= 0 && vk == VK_DELETE && shift) RunAction(ACT_DELETE_ALL, -1);
    else if (ListedAndKept(entry) && vk == VK_F2) RunAction(ACT_RENAME, -1);
    else if (ListedAndKept(entry) && vk == VK_DELETE) RunAction(ACT_REMOVE, -1);
    return 0;
}

/* The tree's notifications. Nothing once the view is destroyed: the tree
 * can still notify while the dialog's children go. */
BOOL SessionsView_Notify(const NMHDR *header, LRESULT *result)
{
    *result = 0;
    if (!g_view.dlg || header->idFrom != IDC_S_TREE) return FALSE;
    switch (header->code) {
    case NM_CUSTOMDRAW:
        *result = TreeCustomDraw((NMTVCUSTOMDRAW *)header);
        return TRUE;
    case TVN_KEYDOWN:
        *result = TreeKey(((const NMTVKEYDOWN *)header)->wVKey);
        return TRUE;
    case TVN_SELCHANGEDW: {
        const NMTREEVIEWW *change = (const NMTREEVIEWW *)header;
        TreeNodeKey key;
        if (!g_view.filling) {
            NodeKey(change->itemNew.lParam, NodeParam(TreeView_GetParent(g_view.tree, change->itemNew.hItem)) == NODE_STARRED, &key);
            if (!g_view.marking && !g_view.drag.pressKept) {   /* a press kept the chosen ones: TreeSubclass decides */
                ClearMarks();
                g_view.anchor = key;
            }
            SetSelection(&key);
        }
        return TRUE;
    }
    case TVN_BEGINDRAGW: {
        const NMTREEVIEWW *drag = (const NMTREEVIEWW *)header;
        DragSessions(drag->itemNew.hItem, drag->ptDrag);
        return TRUE;
    }
    case TVN_ITEMEXPANDINGW:
        HoldTreeDrawing();
        return TRUE;
    case TVN_ITEMEXPANDEDW: {
        const NMTREEVIEWW *change = (const NMTREEVIEWW *)header;
        TreeNodeKey key;
        ReleaseTreeDrawing();
        if (!g_view.filling) ShowFoldButton();
        if (!g_view.filling && change->itemNew.lParam < 0 && (change->action == TVE_COLLAPSE || change->action == TVE_EXPAND)) {
            NodeKey(change->itemNew.lParam, FALSE, &key);
            SetFolded(key.key, change->action == TVE_COLLAPSE);
        }
        return TRUE;
    }
    case TVN_GETINFOTIPW: {
        NMTVGETINFOTIPW *tip = (NMTVGETINFOTIPW *)header;
        int bucket = NodeBucket(tip->lParam);
        if (tip->cchTextMax <= 0) return TRUE;
        if (tip->lParam >= 0 && tip->lParam < g_view.set.rowCount) {
            RowTip(&g_view.set.rows[tip->lParam], tip->pszText, (size_t)tip->cchTextMax);
        } else if (bucket >= 0 && bucket < g_view.bucketCount) {
            const Bucket *heading = &g_view.buckets[bucket];
            if (heading->group >= 0 && heading->group < g_view.set.groupCount && g_view.set.groups[heading->group].path[0])
                StringCchCopyW(tip->pszText, (size_t)tip->cchTextMax, g_view.set.groups[heading->group].path);
            else
                StringCchCopyW(tip->pszText, (size_t)tip->cchTextMax, heading->name);
        }
        return TRUE;
    }
    }
    return FALSE;
}

BOOL SessionsView_DrawItem(const DRAWITEMSTRUCT *item)
{
    if (!g_view.dlg) return FALSE;
    if (item->CtlID == IDC_S_PROFILES) {
        DrawProfile(item);
        return TRUE;
    }
    return FALSE;
}

void SessionsView_Destroy(void)
{
    StopAcceptingDrops();
    if (g_view.drag.keys) HeapFree(GetProcessHeap(), 0, g_view.drag.keys);
    if (g_view.places) HeapFree(GetProcessHeap(), 0, g_view.places);
    if (g_view.shown) KeepPlace();
    StopWatching();
    StopLoader();
    StopWaitingChanges();
    SessionStore_Free(&g_view.set);
    Theme_FreeFonts(&g_view.fonts);
    if (g_view.icons) ImageList_Destroy(g_view.icons);
    StopTextSearch();
    if (g_view.buckets) HeapFree(GetProcessHeap(), 0, g_view.buckets);
    if (g_view.regex) HeapFree(GetProcessHeap(), 0, g_view.regex);
    if (g_view.textKeys) HeapFree(GetProcessHeap(), 0, g_view.textKeys);
    if (g_view.textFound) HeapFree(GetProcessHeap(), 0, g_view.textFound);
    if (g_view.custom.assigned) HeapFree(GetProcessHeap(), 0, g_view.custom.assigned);
    if (g_view.custom.assignedGroup) HeapFree(GetProcessHeap(), 0, g_view.custom.assignedGroup);
    if (g_view.marked) HeapFree(GetProcessHeap(), 0, g_view.marked);
    Util_Free(g_view.rowsShown);
    ForgetTaken();
    SessionUndo_Clear();
    ZeroMemory(&g_view, sizeof g_view);
    InterlockedExchange(&g_reloadMessagePending, 0);
}
