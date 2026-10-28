/*
 * Claude Delta - run several Claude Desktop accounts side by side on Windows.
 *
 * One executable, several roles (see main.c):
 *   (no arguments)      the profile manager window
 *   --launch <folder>   open a profile (what the shortcuts run)
 *   --url <claude://..> claude:// link handler: delivers the link to the right window
 *   --install [--quiet] copy to %LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager, register,
 *                       then open the installed manager (not with --quiet)
 *   --set-up-links      the manager, offering to make it the app for claude:// links
 *   --watch <folder> [pid]  gives that profile's Claude windows their own taskbar button
 *                       and its notification-area icon the profile's color, makes the
 *                       session changes waiting for the profile each time its Claude
 *                       closes, and opens it again after a Claude update (pid: the
 *                       Claude process just started for it)
 *   --uninstall         the uninstall dialog (Apps & features entry)
 */
#ifndef CLAUDE_DESKTOP_PROFILES_MANAGER_APP_H
#define CLAUDE_DESKTOP_PROFILES_MANAGER_APP_H

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define _WIN32_WINNT  0x0A00
#define NTDDI_VERSION 0x0A000006 /* Windows 10 1809 */

#include <windows.h>
#include <strsafe.h>
#include "version.h"
#include "localize.h"

/* ------------------------------------------------------------------ names */

#define APP_NAME           L"Claude Delta"   /* what the program is called wherever it shows */
#define APP_FOLDER         L"Claude Desktop Profiles Manager"   /* its earlier name, kept where it is stored: its folders, its registry key, its Start menu folder */
#define APP_EARLIER_NAME   L"Claude Desktop Profiles Manager"   /* its name until 2026.10.24: its Start menu folder and shortcut then */
#define APP_EXE            L"ClaudeDesktopProfilesManager.exe"
#define APP_AUMID_PREFIX   L"ClaudeDesktopProfilesManager."
#define APP_DOWNLOAD_URL   L"https://claude.ai/download"
#define APP_REPO           L"leiyitong/Claude_Desktop_Profiles_Manager"   /* this fork's releases: the original's would replace it */
#define APP_RELEASES_URL   L"https://github.com/" APP_REPO L"/releases"
#define APP_AUTHOR_URL     L"https://github.com/Freenitial"
/* Whom the release certificate is issued to (sign.cmd signs with it): an
 * update runs only with this signature. */
#define APP_SIGNER         L"Leo Antonin Gillet"

#define REG_ROOT           L"Software\\Claude Desktop Profiles Manager"
#define REG_PROFILES       REG_ROOT L"\\Profiles"
#define REG_SHORTCUTS      REG_ROOT L"\\Shortcuts"
#define REG_CAPABILITIES   REG_ROOT L"\\Capabilities"
#define REG_UNINSTALL      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ClaudeDesktopProfilesManager"
#define REG_PROGID         L"Software\\Classes\\ClaudeDesktopProfilesManager.Url"
#define PROGID_NAME        L"ClaudeDesktopProfilesManager.Url"
#define REG_REGISTERED     L"Software\\RegisteredApplications"
#define REG_USERCHOICE     L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\claude\\UserChoice"
#define REG_USERCHOICE_LATEST L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\claude\\UserChoiceLatest"
/* The packages installed for the user: one subkey per package full name. */
#define REG_PACKAGES       L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages"
/* The taskbar's pin list (taskbar-pin.c). */
#define REG_TASKBAND       L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband"
#define TASKBAR_WINDOW_CLASS L"Shell_TrayWnd"   /* the primary taskbar */

#define WATCH_MUTEX_PREFIX L"Local\\ClaudeDesktopProfilesManager.Watch."
#define WATCH_QUIT_EVENT   L"Local\\ClaudeDesktopProfilesManager.WatchQuit"
#define WATCH_HANDOVER_EVENT L"Local\\ClaudeDesktopProfilesManager.WatchHandover"
#define MANAGER_MUTEX      L"Local\\ClaudeDesktopProfilesManager.Manager"
#define LOG_MUTEX          L"Local\\ClaudeDesktopProfilesManager.Log"
#define SESSION_MUTEX_PREFIX L"Local\\ClaudeDesktopProfilesManager.Session."
#define SYNC_MUTEX_PREFIX  L"Local\\ClaudeDesktopProfilesManager.Sync."    /* and the state folder's hash */
#define SYNC_LOCK_WAIT_MS  (5u * 60u * 1000u)

#define STOCK_FOLDER       L"Claude"
#define PROFILE_PREFIX     L"Claude-"
#define STOCK_DEFAULT_NAME L"Main"
#define CLAUDE_DESKTOP_SETTINGS L"claude_desktop_config.json"   /* in a profile's data folder */
#define CLAUDE_APP_SETTINGS L"config.json"                      /* ...its app state: the account signed in */
#define CLAUDE_ENTRIES_DIR  L"claude-code-sessions"             /* ...its Claude Code sessions' entries */
#define CLAUDE_SCRATCH_DIR  L"scratch-workspaces"               /* ...the working folders of its sessions without a folder */
#define CLAUDE_WEB_STORAGE  L"Local Storage\\leveldb"           /* ...its web UI's storage (webstore.c) */
#define CLAUDE_INDEXED_DB   L"IndexedDB\\https_claude.ai_0.indexeddb.leveldb"   /* ...its web UI's IndexedDB */
#define CLAUDE_MOVED_DATA   L"Claude-Data"   /* in %LOCALAPPDATA%: where Claude moves the stock folder out of its package */

#define MAX_PROFILES       32
C_ASSERT(MAX_PROFILES <= 32);   /* sets of profiles are DWORD bit masks */
#define MAX_NAME           32   /* characters in a new profile name */
#define MAX_LABEL          48   /* characters in a display name */
#define FOLDER_CCH         64
#define LABEL_CCH          64
#define WINDOW_TITLE_CCH   128   /* a profile's own title for its windows */
#define URL_CCH            4096
/* A path that can pass MAX_PATH, as Claude writes some. A file of a session:
 * Claude Code's or a profile's folder (shorter than MAX_PATH), then at most a
 * project folder and a file (255 characters each), with the \\?\ prefix. */
#define LONG_PATH_CCH      1024
#define SIGNIN_MAX_AGE_MINUTES 15   /* a sign-in started longer ago claims no link */
#define TICKS_PER_SECOND   10000000ULL   /* FILETIME units */
#define PALETTE_SIZE       20   /* the first 8 are the colors of release 1.1, kept at their index */
#define MAX_BADGE          2    /* characters (code points) a badge's own text shows */
#define BADGE_CCH          8
#define PICTURE_SIZE       256  /* a profile's own picture is kept at PICTURE_SIZE x PICTURE_SIZE */
#define SESSION_TITLE_CCH  256
#define SESSION_ID_CCH     64

/* What a profile keeps the same as the others it syncs with (Profile.syncItems), one bit each,
 * under the groups the dialog shows them in (each group's mask, SYNC_ITEM_SESSIONS and so on). */
#define SYNC_ITEM_SESSIONS_NEW        0x00000001u   /* a session one profile has and another lacks */
#define SYNC_ITEM_SESSIONS_TITLES     0x00000002u
#define SYNC_ITEM_SESSIONS_ARCHIVED   0x00000004u   /* archived or not, and Claude's list of the archived */
#define SYNC_ITEM_SESSIONS_DELETED    0x00000008u   /* a deletion */
#define SYNC_ITEM_SIDEBAR_PINS        0x00000010u   /* starred and pinned sessions, starred groups, Edit sidebar */
#define SYNC_ITEM_SIDEBAR_GROUPS      0x00000020u   /* groups, their sections, the groups folded */
#define SYNC_ITEM_SIDEBAR_ORDER       0x00000040u   /* the projects' order */
#define SYNC_ITEM_SIDEBAR_FILTERS     0x00000080u   /* the filter menu, projects shown, grouping and sorting, Recents' filters */
#define SYNC_ITEM_SIDEBAR_LAYOUT      0x00000100u   /* the sidebar's width, Routines' place */
#define SYNC_ITEM_DETAILS_MODEL       0x00000200u   /* each session's model and effort */
#define SYNC_ITEM_DETAILS_PANE        0x00000400u   /* each session's side pane and transcript view */
#define SYNC_ITEM_DETAILS_UNREAD      0x00000800u
#define SYNC_ITEM_DETAILS_COST        0x00001000u
#define SYNC_ITEM_APPEARANCE_FONTS    0x00002000u
#define SYNC_ITEM_APPEARANCE_EDITOR   0x00004000u
#define SYNC_ITEM_APPEARANCE_ZOOM     0x00008000u
#define SYNC_ITEM_APPEARANCE_SPELLING 0x00010000u
#define SYNC_ITEM_LANGUAGE            0x00020000u
#define SYNC_ITEM_MODEL               0x00040000u   /* the default model */
#define SYNC_ITEM_SETTINGS_ARCHIVE    0x00080000u   /* auto-archive when the PR closes */
#define SYNC_ITEM_SETTINGS_COWORK     0x00100000u   /* Cowork's switches and browser */
#define SYNC_ITEM_SETTINGS_REMOTE     0x00200000u   /* Remote Control's folders */
#define SYNC_ITEM_SETTINGS_RECENTS    0x00400000u   /* Cowork's recent folders */
#define SYNC_ITEM_SETTINGS_CODE       0x00800000u   /* Code's scheduled tasks, links in previews, going on after a usage limit */
#define SYNC_ITEM_PERMISSIONS_MODES   0x01000000u   /* folders' permission modes and their warnings confirmed */
#define SYNC_ITEM_PERMISSIONS_TRUSTED 0x02000000u   /* Cowork's trusted folders */
#define SYNC_ITEM_SESSIONS            0x0000000Fu
#define SYNC_ITEM_SIDEBAR             0x000001F0u
#define SYNC_ITEM_DETAILS             0x00001E00u
#define SYNC_ITEM_APPEARANCE          0x0001E000u
#define SYNC_ITEM_SETTINGS            0x00F80000u
#define SYNC_ITEM_PERMISSIONS         0x03000000u
#define SYNC_ITEMS_ALL                0x03FFFFFFu
#define SYNC_ITEMS_DEFAULT            SYNC_ITEMS_ALL   /* what a group syncs until its profiles choose */
#define SYNC_GROUP_COUNT              8   /* the groups, in the order the dialog shows them */

/* ------------------------------------------------------------------ types */

typedef struct Profile {
    WCHAR folder[FOLDER_CCH];  /* "Claude" (stock) or "Claude-<name>": the stable id */
    WCHAR name[LABEL_CCH];     /* display name, editable */
    WCHAR note[256];           /* the person's note, kept by the manager */
    WCHAR dataDir[MAX_PATH];   /* %APPDATA%\<folder>, as Claude sees it */
    WCHAR storageDir[MAX_PATH]; /* file access outside the package; empty when unresolved */
    WCHAR formerDir[MAX_PATH]; /* where the stock folder was before Claude moved it out of its package
                                  (%APPDATA%\Claude), which earlier entries still name; empty otherwise */
    int   color;               /* palette index */
    WCHAR badge[BADGE_CCH];    /* the badge's own text; empty: the name's initial */
    WCHAR windowTitle[WINDOW_TITLE_CCH];   /* its windows' title; empty: Claude's own, then its name ("Claude (<name>)") */
    DWORD picture;             /* the stamp of its own picture, which replaces the Claude icon; 0: none */
    BOOL  isStock;             /* the folder the regular Claude icon opens */
    int   syncGroup;           /* its sessions are kept the same as those of the profiles of this group (sessionvault.c); 0: none */
    DWORD syncItems;           /* what that keeps the same (SYNC_ITEM_*); 0: SYNC_ITEMS_DEFAULT */
    BOOL  running;
    DWORD pid;                 /* main process when running */
} Profile;

typedef struct ProfileList {
    Profile items[MAX_PROFILES];
    int     count;
    WCHAR   defaultFolder[FOLDER_CCH];
} ProfileList;

typedef struct ClaudePackage {
    BOOL  found;
    WCHAR fullName[256];
    WCHAR family[128];
    WCHAR aumid[256];
    WCHAR installDir[MAX_PATH];
    WCHAR exe[MAX_PATH];
    WCHAR version[32];
} ClaudePackage;

typedef enum RouteReason {
    ROUTE_NOTHING_RUNNING,  /* start the default profile */
    ROUTE_ONLY_ONE,
    ROUTE_SIGNIN,           /* the window that opened the browser */
    ROUTE_LAST_USED,        /* topmost Claude window */
    ROUTE_DEFAULT,
    ROUTE_FIRST
} RouteReason;

typedef struct LinkInfo {
    WCHAR target[MAX_PATH];
    WCHAR args[2048];
    WCHAR parsing[512];        /* shell parsing name when the target is not a file */
} LinkInfo;

/* PENDING_TITLE_AUTO: a title Claude gave, put back (undone): titleSource "auto", so that Claude may name the
 * session again; an empty one takes the entry's title away. */
/* PENDING_READ: the session read ("1") or unread ("0") in Claude's sidebar, in the profile's web storage (Core_UnreadSet). */
typedef enum PendingOp { PENDING_TITLE, PENDING_STAR, PENDING_REMOVE, PENDING_TITLE_AUTO, PENDING_UNREMOTE, PENDING_READ } PendingOp;

typedef struct PendingEdit {   /* a change to a session entry, waiting for its profile to close */
    PendingOp op;
    WCHAR     key[SESSION_ID_CCH];         /* the session's id (cliSessionId, else the entry's own) */
    WCHAR     value[SESSION_TITLE_CCH];    /* title; "1"/"0" for a star */
} PendingEdit;

typedef struct CoreSwap { const char *from, *to; } CoreSwap;

/* A change sent to one profile's session entries (sessionsync.c), made at
 * once when it is closed, else kept in a file of ours until it closes. */
typedef enum SyncOpKind {
    SYNC_PUT,      /* its entry for a session written: added, or replacing its own */
    SYNC_REMOVE,   /* its entries for a session taken away */
    SYNC_MARK,     /* Claude's mark that a session was deleted there, written */
    SYNC_UNMARK,   /* ... taken away */
    SYNC_INDEX,    /* its list of archived sessions replaced */
    SYNC_LAYOUT    /* the pins and groups of Claude's sidebar replaced (claude_desktop_config.json) */
} SyncOpKind;

#define SYNC_UNDELETE     0x1   /* a put made even where Claude marked the session deleted (the marks go) */
#define SYNC_REPLACE      0x2   /* a put that replaces the profile's own entry unless it was used since */
#define SYNC_MARKED       0x4   /* a removal that leaves Claude's marks that the session was deleted there */
#define SYNC_CONTENT_CCH  32

typedef struct SyncOp {
    SyncOpKind kind;
    DWORD      flags;                       /* SYNC_UNDELETE, SYNC_REPLACE */
    ULONGLONG  time;                        /* put: the entry's last activity; mark: the time it holds (ms since 1970) */
    ULONGLONG  seen;                        /* put, remove: the profile's own entry's last activity when sent, 0 for none */
    WCHAR      key[SESSION_ID_CCH];         /* the session's id; mark, unmark: the id marked */
    WCHAR      content[SYNC_CONTENT_CCH];   /* put, index, layout: the file of ours holding what is written */
    DWORD      tries;                       /* times making it failed; it waits for the next try until SYNC_TRIES_MAX */
} SyncOp;
#define SYNC_TRIES_MAX 5

typedef struct SyncProfileResult {
    WCHAR folder[FOLDER_CCH];
    int added, updated, removed, skipped, failed;
    BOOL waiting, unavailable;
    WCHAR error[384];
} SyncProfileResult;

typedef struct SyncReport {             /* what sending sessions to profiles did */
    int   added, updated, removed, skipped, failed;
    int   forked;                       /* sessions two profiles went on with apart, kept as two */
    DWORD waiting;                      /* the profiles whose part waits for them to close */
    DWORD unavailable;                  /* the profiles that have no session entries yet: nothing sent there */
    int   conflicts;                    /* parts of a sidebar changed in two profiles each their own way: left to the person */
    WCHAR backup[MAX_PATH];             /* where what was replaced or removed went; "" for nowhere */
    WCHAR error[LONG_PATH_CCH];         /* the first failure, "" for none */
    int profileCount;
    SyncProfileResult profiles[MAX_PROFILES];   /* stable folder keys, including reports from subset syncs */
} SyncReport;

typedef enum RemoveResult { REMOVE_DONE, REMOVE_CANCELLED, REMOVE_FAILED } RemoveResult;

typedef enum SessionEntryKind { ENTRY_NOT_SESSION, ENTRY_LOCAL, ENTRY_ELSEWHERE } SessionEntryKind;

/* How one profile of a group keeping the same sessions has a session (sessionvault.c). */
typedef enum MirrorState {
    MIRROR_LISTED,    /* listed: `hash` its entry (without its own id), `time` when it was last written */
    MIRROR_DELETED,   /* not listed: Claude marked it deleted there at `time` */
    MIRROR_REMOVED,   /* not listed by a profile that had the group's list: Claude took it away */
    MIRROR_MISSING    /* not listed by a profile that never had it, or that lost its whole list */
} MirrorState;

typedef struct MirrorSide {
    MirrorState state;
    ULONGLONG   hash, time;   /* time in ms since 1970 */
} MirrorSide;

#define CORE_MIRROR_DELETED (-1)
#define CORE_MIRROR_NOWHERE (-2)

#define CORE_PROJECT_NAME_MAX 200
#define CORE_SCROLL_EASE_MS   35   /* a wheel notch is 95 % done after three of these */
#define CORE_HASH_START       14695981039346656037ULL

/* ---------------------------------------------------------- core.c (pure) */

BOOL         Core_ValidateNewName(const WCHAR *raw, WCHAR *name, size_t nameCch,
                                  WCHAR *folder, size_t folderCch, const WCHAR **error);
BOOL         Core_ValidateLabel(const WCHAR *raw, WCHAR *label, size_t cch, const WCHAR **error);
/* A badge's own text: `raw` without the spaces around it, cut after MAX_BADGE
 * characters (a surrogate pair is one); FALSE when it holds a control
 * character. An empty result is valid: the badge shows the name's initial. */
BOOL         Core_CleanBadge(const WCHAR *raw, WCHAR *badge, size_t cch);
BOOL         Core_IsProfileFolder(const WCHAR *folder);
BOOL         Core_SanitizeUrl(const WCHAR *in, WCHAR *out, size_t cch);
BOOL         Core_IsSignInUrl(const WCHAR *url);
BOOL         Core_PrivateSignInUrl(const WCHAR *raw, WCHAR *out, size_t cch);
#define CORE_BROWSER_CHROMIUM 0   /* Chrome, Brave: --incognito */
#define CORE_BROWSER_EDGE     1   /* --inprivate */
#define CORE_BROWSER_FIREFOX  2   /* -private-window, a profile of its own */
BOOL         Core_MagicLink(const WCHAR *raw, WCHAR *url, size_t urlCch, WCHAR *email, size_t emailCch);   /* claude.ai/magic-link#<hex>:<address> */
BOOL         Core_PrivateBrowserArgs(int family, const WCHAR *dataDir, const WCHAR *url, WCHAR *out, size_t cch);
BOOL         Core_BuildLaunchArgs(const WCHAR *dataDir, const WCHAR *url, WCHAR *out, size_t cch);
BOOL         Core_LatestSignInStart(const char *text, size_t len, SYSTEMTIME *latest);
BOOL         Core_LastQuit(const char *text, size_t len, SYSTEMTIME *when, BOOL *forUpdate);
/* The running window a claude:// link is suggested for, an index below
 * `count` (-1 when none runs): the one that started the latest sign-in for a
 * sign-in link, else the only one, the one used last (`lastUsed`), the
 * default profile's (`defaultIndex`) or the first. `signInTicks` may be NULL. */
int          Core_SuggestTarget(int count, const ULONGLONG *signInTicks, ULONGLONG nowTicks, ULONGLONG signInMaxAgeTicks,
                                BOOL signInUrl, int lastUsed, int defaultIndex, RouteReason *reason);
/* The default profile's Claude, started by a notification clicked in
 * Windows' notification center (notifyguard.c): the profile the notification
 * belongs to among the `count` of `running`, `stock` the default one. -1
 * when no other runs (it is the default's); the only other one; else, with
 * `*ask`, the one to suggest (`topmost` when it is another that runs). */
int          Core_NotificationTarget(const BOOL *running, int count, int stock, int topmost, BOOL *ask);
BOOL         Core_ArgsSelectProfile(const WCHAR *args, const WCHAR *folder);
BOOL         Core_ArgsReferenceDir(const WCHAR *args, const WCHAR *dir);
BOOL         Core_NamesClaudePackage(const WCHAR *text);
BOOL         Core_LinkOpensProfile(const LinkInfo *link, const WCHAR *folder,
                                   const WCHAR *dataDir, BOOL isStock);
BOOL         Core_IsOurExe(const WCHAR *path);
BOOL         Core_EqualsI(const WCHAR *a, const WCHAR *b);
size_t       Core_TrimmedPathLength(const WCHAR *path);
int          Core_PathCompare(const WCHAR *a, const WCHAR *b);
BOOL         Core_PathEquals(const WCHAR *a, const WCHAR *b);
/* A shortcut showing icon `location` (empty: none; `exists`: its file is there)
 * takes the profile's current icon `current`: when it shows none, one that is
 * gone, or another of ours (in the folder of `current`). An icon the user
 * chose stays while its file does. */
BOOL         Core_TakesCurrentIcon(const WCHAR *location, BOOL exists, const WCHAR *current);
BOOL         Core_EndsWithI(const WCHAR *s, const WCHAR *suffix);
BOOL         Core_ContainsI(const WCHAR *s, const WCHAR *needle);
DWORD        Core_HashIgnoringCase(const WCHAR *text);
void         Core_ProfileAumid(const WCHAR *folder, WCHAR *out, size_t cch);
void         Core_ShortcutFileName(const WCHAR *label, int copyNumber, WCHAR *out, size_t cch);
/* The file sessions dragged out of the sessions view are written to: one session's `title`, its characters a name
 * cannot hold made "_", else `several` and the `day` ("Claude sessions 2026-10-05"), then " (n)" for copy
 * `copyNumber` past the first, and ".zip". */
void         Core_SessionsArchiveName(const WCHAR *title, const WCHAR *several, const SYSTEMTIME *day, int copyNumber, WCHAR *out, size_t cch);
ULONGLONG    Core_SystemTimeTicks(const SYSTEMTIME *st);
BOOL         Core_PathUnder(const WCHAR *path, const WCHAR *dir);
BOOL         Core_ProfileFilePath(const Profile *p, const WCHAR *path, WCHAR *out, size_t cch);
BOOL         Core_PackageCachePath(const WCHAR *localAppData, const WCHAR *family, const WCHAR *area, const WCHAR *name,
                                   WCHAR *out, size_t cch);
BOOL         Core_MovedStockDir(const WCHAR *localAppData, const WCHAR *family, WCHAR *out, size_t cch);
BOOL         Core_ProfileHoldsPath(const Profile *p, const WCHAR *path);
/* `hash` taken on through what a profile's icon shows: its name, color, badge and picture. */
ULONGLONG    Core_ProfileLookHash(ULONGLONG hash, const Profile *p);
int          Core_BitCount(DWORD bits);
/* Times as Claude writes them, in ms since 1970: a FILETIME's (0 before 1970), and back. */
ULONGLONG    Core_FileTimeMs(const FILETIME *time);
FILETIME     Core_MsFileTime(ULONGLONG ms);
BOOL         Core_SameFatTime(const FILETIME *a, const FILETIME *b);
BOOL         Core_JsonMember(const char *json, size_t len, const char *key, const char **value, size_t *valueLen);
/* Each member of the JSON object `json`, in order: its key's raw text (between
 * its quotes, escapes as written) and its value's. FALSE when `json` is no
 * object, or `each` returns FALSE. */
typedef BOOL (*CoreJsonMember)(void *context, const char *key, size_t keyLen, const char *value, size_t valueLen);
BOOL         Core_JsonEachMember(const char *json, size_t len, CoreJsonMember each, void *context);
/* Each element of the JSON array `json`, in order, as its raw text. FALSE
 * when `json` is no array, or `each` returns FALSE. */
typedef BOOL (*CoreJsonElement)(void *context, const char *value, size_t valueLen);
BOOL         Core_JsonEachElement(const char *json, size_t len, CoreJsonElement each, void *context);
BOOL         Core_JsonIsValue(const char *text, size_t len);   /* one JSON value, whole (spaces around it aside) */
BOOL         Core_JsonString(const char *raw, size_t len, WCHAR *out, size_t cch);
WCHAR       *Core_JsonStringList(const char *raw, size_t len); /* MULTI_SZ; HeapFree, NULL when absent/invalid */
BOOL         Core_SafeHttpsUrl(const WCHAR *url); /* explicit HTTPS authority, no credentials/control/quotes */
BOOL         Core_JsonNumber(const char *raw, size_t len, ULONGLONG *value);
BOOL         Core_JsonTrue(const char *raw, size_t len);
/* Member `key` of the JSON object `json`: as a string ("" and FALSE when it
 * is none), a number (0 when none) or whether it is true. */
BOOL         Core_JsonMemberString(const char *json, size_t len, const char *key, WCHAR *out, size_t cch);
ULONGLONG    Core_JsonMemberNumber(const char *json, size_t len, const char *key);
BOOL         Core_JsonMemberTrue(const char *json, size_t len, const char *key);
SessionEntryKind Core_SessionEntryKind(const char *json, size_t len);
/* Where a Code session runs, as Claude's sidebar filters sessions: one bit each. */
#define SESSION_ENV_LOCAL  0x01u
#define SESSION_ENV_CLOUD  0x02u
#define SESSION_ENV_REMOTE 0x04u   /* Remote Control */
#define SESSION_ENV_SSH    0x08u
#define SESSION_ENV_SLACK  0x10u
#define SESSION_ENV_ALL    0x1Fu
DWORD        Core_SessionEnvironment(const char *json, size_t len);
/* A session's pull request as Claude shows it: its latest one not dismissed. */
typedef enum PullRequestState { PR_NONE, PR_OPEN, PR_DRAFT, PR_MERGED, PR_CLOSED } PullRequestState;
PullRequestState Core_SessionPullRequest(const char *json, size_t len, int *number);
/* `needle` in UTF-8 `text`; without `matchCase`, ASCII letters in either case. */
BOOL         Core_ContainsUtf8(const char *text, size_t len, const char *needle, size_t needleLen, BOOL matchCase);
/* A regular expression (core.c says what it accepts), compiled once and
 * searched in many texts, from any thread. */
#define CORE_REGEX_CODE   1024
#define CORE_REGEX_RANGES 256
typedef struct CoreRegexInstruction { BYTE op, negate; WCHAR ch; int x, y; } CoreRegexInstruction;
typedef struct CoreRegex {
    CoreRegexInstruction code[CORE_REGEX_CODE];
    WCHAR range[CORE_REGEX_RANGES][2];
    int   codeCount, rangeCount;
    BOOL  ignoreCase, valid;
} CoreRegex;
BOOL         Core_RegexCompile(CoreRegex *rx, const WCHAR *pattern, BOOL ignoreCase);   /* FALSE: not a pattern it takes */
BOOL         Core_RegexSearch(const CoreRegex *rx, const WCHAR *text, size_t length);   /* a match anywhere in the text */
BOOL Core_RegexSearchEnd(const CoreRegex *rx, const WCHAR *text, size_t length, size_t *end);
typedef enum TranscriptRole { TRANSCRIPT_NONE, TRANSCRIPT_USER, TRANSCRIPT_CLAUDE } TranscriptRole;
TranscriptRole Core_TranscriptLine(const char *line, size_t len, WCHAR *out, size_t cch);   /* one line of a .jsonl, for the preview */
#define CHANGELOG_NAME_CCH 24
typedef struct ChangelogVersion {      /* a version of CHANGELOG.md, read by Core_ChangelogRead */
    WCHAR  name[CHANGELOG_NAME_CCH];   /* its heading: "v2026.10.12" */
    size_t from, to;                   /* its lines in the text read: characters */
} ChangelogVersion;
int          Core_ChangelogRead(const char *markdown, size_t length, ChangelogVersion *versions, int max, WCHAR *text, size_t cch);
typedef struct CoreRtfStyle {          /* how Core_MarkdownRtf writes: what the theme gives */
    const WCHAR *face, *codeFace;      /* the text's font, code's */
    int          size;                 /* the text's size, half-points */
    int          indent;               /* a numbered line's text past its number, twips */
    COLORREF     text, muted, link, codeBack;
} CoreRtfStyle;
size_t       Core_MarkdownRtf(const WCHAR *markdown, const CoreRtfStyle *style, char *out, size_t cch);
size_t       Core_Base64Encode(const BYTE *data, size_t length, char *out, size_t cch);   /* characters written, 0: does not fit */
size_t       Core_Base64Decode(const char *text, size_t length, BYTE *out, size_t cap);   /* bytes, (size_t)-1: no base64 */
/* What Chromium's OSCrypt (Electron's safeStorage) sealed on Windows, opened
 * with Local State's key (Backup_ReadAppKey): "v10", a 12-byte nonce, the
 * AES-256-GCM text and its 16-byte tag, checked. The plain bytes in `out`
 * (`cap` at least `length` - 31); FALSE for anything else. */
BOOL         Core_OpenSafeStorage(const BYTE *sealed, size_t length, const BYTE *key, size_t keyLength, BYTE *out, size_t cap, size_t *outLength);
/* `text` sealed as Chromium's App-Bound "v10" cookie value with Local State's `key`: "v10", a random 12-byte
 * nonce, the AES-256-GCM text and its 16-byte tag, into `out` (`cap` at least `length` + 31). FALSE when it
 * cannot. The inverse of Core_OpenSafeStorage; Core_OpenSafeStorage opens what it seals. */
BOOL         Core_SealSafeStorage(const BYTE *text, size_t length, const BYTE *key, size_t keyLength, BYTE *out, size_t cap, size_t *outLength);
BOOL         Core_SealSecret(const BYTE *secret, size_t length, const WCHAR *password, ULONG iterations, char *out, size_t cch);
BOOL         Core_OpenSecret(const char *json, size_t length, const WCHAR *password, BYTE *secret, size_t cap, size_t *secretLength);
BOOL         Core_JsonSetMember(const char *json, size_t len, const char *key, const char *raw, char *out, size_t cap, size_t *outLen);
BOOL         Core_JsonQuote(const WCHAR *text, char *out, size_t cap);
/* SearXNG, as Claude Delta sets it up in a profile: mcp-searxng run by npx (SearXng_Set, windowsmcp.c).
 * Core_SearXngUrl: an instance's address as typed, made the form the server takes (http or https, https for a bare host; a host, no spaces,
 * quotes, backslashes, query or fragment; without the slashes it ends with, or a /search copied from a results page).
 * Core_SearXngEntry: its mcpServers entry. Core_SearXngServer: whether an entry runs mcp-searxng, and its address and
 * whether it may reach private addresses. */
#define SEARXNG_PACKAGE "mcp-searxng"
BOOL         Core_SearXngUrl(const WCHAR *text, WCHAR *out, size_t cch);
BOOL         Core_SearXngEntry(const WCHAR *npx, const WCHAR *url, BOOL allowPrivate, char *out, size_t cap);
BOOL         Core_SearXngServer(const char *server, size_t length, WCHAR *url, size_t cch, BOOL *allowPrivate);
#define SEARXNG_DEFAULT_URL L"https://searxng.cheese.ink"   /* set up when no address is known or set in Settings */
/* Cowork, Claude's agent mode, runs a virtual machine of its own in each profile that used it (vm_bundles, some
 * 9 GB). The preference COWORK_PREFERENCE false turns it off: Claude then offers no Cowork ("Enable the
 * secureVmFeaturesEnabled preference to use this feature") and fetches no virtual machine. Core_CoworkOff: the
 * config (claude_desktop_config.json) turns it off. */
#define COWORK_PREFERENCE "secureVmFeaturesEnabled"
#define COWORK_VM_DIR     L"vm_bundles"
BOOL         Core_CoworkOff(const char *json, size_t len);
/* A release of main, as CI publishes it: one a day, tagged "v<APP_VERSION_DATE>" (v2026.10.26). Core_ReleaseVersion:
 * such a tag's date (FALSE for any other tag). Core_NewestRelease: of the releases `gh release list --json
 * tagName,isDraft,isLatest` lists, the one GitHub marks latest, else the newest day's, drafts left out: its tag.
 * Core_NewestReleaseRef: the same from what `git ls-remote --tags` writes, with the commit its tag names (an
 * annotated tag's "^{}" line). Core_SameCommit: two commit ids, either cut short (7 characters at least), alike. */
BOOL         Core_ReleaseVersion(const WCHAR *tag, WCHAR *version, size_t cch);
BOOL         Core_NewestRelease(const char *json, size_t length, WCHAR *tag, size_t cch);
BOOL         Core_NewestReleaseRef(const char *text, size_t length, WCHAR *tag, size_t tagCch, WCHAR *commit, size_t commitCch);
BOOL         Core_SameCommit(const WCHAR *a, const WCHAR *b);
BOOL         Core_ProjectDirName(const WCHAR *cwd, WCHAR *out, size_t cch);
#define UUID_TEXT_CCH 37   /* 8-4-4-4-12 hex digits and the terminator */
/* What Claude's OAuth token cache (config.json's "oauth:tokenCache*", opened: Core_OpenSafeStorage) says of the
 * sign-in, never a token: whose it is, its plan, and each token's scopes, until when, whether it renews itself. Its
 * members are keyed "acct:<account>|<client>:<organization>:<API>:<scopes>". Core_ReadTokenCache adds to `signIn`
 * what `json` holds (the account and the organization, the plan and its tier, when not known yet). */
#define CORE_TOKENS 8
typedef struct CoreToken {
    WCHAR     scopes[160];
    ULONGLONG expires;      /* ms since 1970, 0: unknown */
    BOOL      renews;       /* it has a refresh token */
} CoreToken;
typedef struct CoreSignIn {
    WCHAR     account[UUID_TEXT_CCH], organization[UUID_TEXT_CCH], plan[64], tier[64];
    CoreToken tokens[CORE_TOKENS];
    int       tokenCount;
} CoreSignIn;
void         Core_ReadTokenCache(const char *json, size_t length, CoreSignIn *signIn);
void         Core_FormatSize(ULONGLONG bytes, WCHAR *out, size_t cch);   /* "3.2 MB", "12 KB" */
/* A release's notes (tools/changelog.py's) in one language: where its English or its Chinese is, without the quotes
 * before it or how to install; FALSE when they are not split so. */
BOOL         Core_ReleaseNotesIn(const WCHAR *notes, BOOL chinese, size_t *start, size_t *length);
BOOL         Core_IsUuid(const WCHAR *id);
BOOL         Core_ResumeLink(const WCHAR *sessionId, WCHAR *out, size_t cch);
void         Core_ScratchName(const SYSTEMTIME *day, DWORD random, WCHAR *out, size_t cch);
BOOL         Core_ScratchDirFor(const WCHAR *dataDir, const WCHAR *entriesDir, WCHAR *out, size_t cch);
size_t       Core_ReplaceChunk(const char *in, size_t len, const CoreSwap *swaps, int count, BOOL last, char *out, size_t *used);
BOOL         Core_PendingFormat(const PendingEdit *edit, WCHAR *out, size_t cch);
BOOL         Core_PendingParse(const WCHAR *line, PendingEdit *edit);
BOOL         Core_PendingReplaces(const PendingEdit *queued, const PendingEdit *added);
BOOL         Core_UnreadHas(const char *json, size_t len, const WCHAR *id);   /* epitaxy-unread-v1 */
char        *Core_UnreadSet(const char *json, size_t len, const WCHAR *id, BOOL unread, size_t *outLen);
BOOL         Core_JsonRemoveMember(const char *json, size_t len, const char *key, char *out, size_t cap, size_t *outLen);
/* A session entry made for a new session (a copy, a branch) without what tied
 * it to the original's other transcripts, in place; FALSE when it is no object. */
BOOL         Core_EntryDropHistory(char *entry, size_t *length);
BOOL         Core_SyncOpFormat(const SyncOp *op, WCHAR *out, size_t cch);
BOOL         Core_SyncOpParse(const WCHAR *line, SyncOp *op);
BOOL         Core_SyncOpReplaces(const SyncOp *queued, const SyncOp *added);
DWORD        Core_SyncGroup(int index);                  /* group `index`'s mask (SYNC_ITEM_*), 0 past the last */
/* Earlier versions kept one bit per group, in the dialog's order: those groups whole. */
DWORD        Core_SyncItemsOfGroups(DWORD groups);
DWORD        Core_SyncGroupsOf(DWORD items);             /* ... and back: a group with any of its items */
int          Core_MirrorResolve(const MirrorSide *sides, int count, const MirrorSide *base);
BOOL         Core_DatedCopyName(const WCHAR *name, const SYSTEMTIME *day, int copy, WCHAR *out, size_t cch);
/* The weekly copy of folder `name` (sessionpurge.c): "<name>_auto_yyyymmdd". */
#define CORE_WEEKLY_COPY_DAYS 7
BOOL         Core_WeeklyCopyName(const WCHAR *name, const SYSTEMTIME *day, WCHAR *out, size_t cch);
/* The day folder `copy` is a copy of folder `name` from: a dated copy
 * (Core_DatedCopyName) or a weekly one (`weekly`). FALSE for any other name. */
BOOL         Core_CopyDay(const WCHAR *name, const WCHAR *copy, SYSTEMTIME *day, BOOL *weekly);
/* A copy is due when the latest one (wYear 0: none) is CORE_WEEKLY_COPY_DAYS days old or more. */
BOOL         Core_WeeklyCopyDue(const SYSTEMTIME *latest, const SYSTEMTIME *today);
BOOL         Core_ScratchFolderName(const WCHAR *cwd, WCHAR *out, size_t cch);
char        *Core_JsonSetNested(const char *json, size_t len, const char *const *keys, int depth, const char *raw, size_t *outLen);
/* As Core_JsonSetNested; `raw` NULL removes the member instead, making nothing on the way (one missing leaves `json`
 * as it is, an object emptied by it stays). */
char        *Core_JsonSetPath(const char *json, size_t len, const char *const *keys, int depth, const char *raw, size_t *outLen);
/* A JSON value read as an environment variable's switch, as Claude Code reads one: 1 for "1", "true", "yes" or "on"
 * (any case; a string or a bare value), 0 for "0", "false", "no" or "off", -1 for anything else ("" too). */
int          Core_EnvSwitch(const char *raw, size_t len);
/* `base` (an object) with its array `key` given the elements of `other`'s
 * that it lacks, after its own: an element whose member `idKey` none of
 * its elements has the same, or one without it that none of them equals.
 * A missing array is an empty one. A heap block with a NUL after it
 * (HeapFree it); NULL when an array is not one, or without memory. */
char        *Core_JsonArrayUnion(const char *base, size_t baseLen, const char *other, size_t otherLen, const char *key, const char *idKey,
                                 size_t *outLen);
typedef size_t (*CoreStringMap)(void *context, const char *text, size_t length, char *out, size_t cap);
char        *Core_JsonMapStrings(const char *json, size_t len, CoreStringMap map, void *context, size_t *outLen);
/* Three-way merge of a part's value in `n` profiles (`values[m]` NULL: it
 * has none) against `base` (NULL: none): what one changed goes to all; what
 * several changed each their own way is merged deeper, `depth` levels at
 * most, objects by member, lists by their items' "id" or first string, lists
 * of plain values as sets; what is left is a conflict at its path (JSON
 * pointer, a list item's step "#" and its key): `decide` gives the member
 * whose version it takes (-1: none yet), else `report` hears of it and the
 * member that changed last (`written`) stands in, counted in `*unresolved`.
 * Only members with `say` count. A heap text (HeapFree it), empty when the
 * value is absent from the result; NULL without memory. */
#define CORE_MERGE_PATH_CCH 512
typedef int  (*CoreMergeDecide)(void *context, const char *path, DWORD members);
typedef void (*CoreMergeReport)(void *context, const char *path, DWORD members, int latest, const char *const *values, const size_t *lengths);
char        *Core_JsonMerge(const char *base, size_t baseLen, const char *const *values, const size_t *lengths, const BOOL *say,
                            const ULONGLONG *written, int n, int depth, CoreMergeDecide decide, CoreMergeReport report, void *context,
                            size_t *outLen, int *unresolved);
BOOL         Core_IsoTime(ULONGLONG ms, char *out, size_t cap);
BOOL         Core_TranscriptBranches(const char *text, size_t len, ULONGLONG keepTime, ULONGLONG dropTime, ULONGLONG since,
                                     BOOL **excluded, int *lineCount);
DWORD        Core_Crc32(DWORD crc, const void *data, size_t size);
/* Deflate (RFC 1951), as ZIP's method 8: the whole input at once. Compressing
 * needs Core_DeflateBound(size) bytes at most; FALSE when `capacity` is short
 * or memory runs out. Inflating reads every kind of block; FALSE for a broken
 * stream or one longer than `capacity`. */
size_t       Core_DeflateBound(size_t size);
BOOL         Core_Deflate(const void *input, size_t size, void *output, size_t capacity, size_t *written);
BOOL         Core_Inflate(const void *input, size_t size, void *output, size_t capacity, size_t *written);

/* LevelDB, as Chromium keeps a window's web storage in it: a log of write
 * batches, tables of sorted entries, a manifest naming the live ones. */
typedef struct CoreLevelOp {
    BOOL        put;          /* a value; FALSE: the key deleted */
    const BYTE *key;
    size_t      keyLength;
    const BYTE *value;
    size_t      valueLength;
} CoreLevelOp;
typedef BOOL (*CoreLevelEntry)(void *context, ULONGLONG sequence, const CoreLevelOp *op);
typedef BOOL (*CoreLevelRecord)(void *context, const BYTE *record, size_t length);
typedef struct CoreLevelManifest {
    ULONGLONG logNumber, prevLogNumber, nextFile, lastSequence;
    BOOL      hasLog;
    void    (*onTable)(void *context, ULONGLONG number, BOOL added);   /* a table made live, or gone */
    void     *context;
} CoreLevelManifest;
DWORD        Core_Crc32c(DWORD crc, const void *data, size_t size);
DWORD        Core_LevelMask(DWORD crc);
BOOL         Core_SnappyLength(const BYTE *in, size_t length, size_t *outLength);
BOOL         Core_SnappyDecode(const BYTE *in, size_t length, BYTE *out, size_t outLength);
/* Each whole record of a log, in order; `cleanEnd`: where the last whole one ends. */
BOOL         Core_LevelLogRecords(const BYTE *log, size_t length, CoreLevelRecord each, void *context, size_t *cleanEnd);
/* What to add to a log `fileLength` bytes long for one record: 0 when `capacity` is short. */
size_t       Core_LevelLogAppend(size_t fileLength, const BYTE *record, size_t length, BYTE *out, size_t capacity);
BOOL         Core_LevelBatchRead(const BYTE *batch, size_t length, CoreLevelEntry each, void *context);
size_t       Core_LevelBatchWrite(ULONGLONG sequence, const CoreLevelOp *ops, int count, BYTE *out, size_t capacity);
BOOL         Core_LevelTableRead(const BYTE *table, size_t length, CoreLevelEntry each, void *context);
BOOL         Core_LevelManifestEdit(const BYTE *edit, size_t length, CoreLevelManifest *state);
size_t       Core_WebStorageKey(const char *origin, const WCHAR *name, BYTE *out, size_t capacity);   /* Local Storage's key */
size_t       Core_WebStorageValue(const WCHAR *text, size_t length, BYTE *out, size_t capacity);
BOOL         Core_WebStorageText(const BYTE *value, size_t length, WCHAR *out, size_t cch, size_t *outLength);
/* IndexedDB's keys and values, as Chromium codes them in its LevelDB (core.c). */
size_t       Core_IdbPrefix(ULONGLONG database, ULONGLONG store, ULONGLONG index, BYTE *out, size_t capacity);
BOOL         Core_IdbPrefixRead(const BYTE *key, size_t length, ULONGLONG ids[3], size_t *end);
size_t       Core_IdbInt(ULONGLONG value, BYTE *out);   /* at most 8 bytes */
ULONGLONG    Core_IdbIntRead(const BYTE *data, size_t length);
size_t       Core_IdbStringKey(const WCHAR *text, BYTE *out, size_t capacity);
BOOL         Core_IdbDatabaseName(const BYTE *key, size_t length, WCHAR *name, size_t cch);   /* a database's name entry */
size_t       Core_IdbStoreMetaKey(ULONGLONG database, ULONGLONG store, int kind, BYTE *out, size_t capacity);
BOOL         Core_IdbStoreMeta(const BYTE *key, size_t length, ULONGLONG *database, ULONGLONG *store, int *kind);
BOOL         Core_IdbValueString(const BYTE *value, size_t length, ULONGLONG *version, size_t *header, size_t *text, BYTE *tag,
                                 size_t *start, size_t *bytes);
size_t       Core_IdbValueWrite(ULONGLONG version, const BYTE *header, size_t headerLength, const WCHAR *text, size_t length, BYTE *out,
                                size_t capacity);
/* A process of a snapshot: its id, its parent's, and when it started (0: unknown). */
typedef struct CoreProcess {
    DWORD     pid, parent;
    ULONGLONG started;
} CoreProcess;
/* Marks in `chosen` every process `processes[root]` started, at any depth: a
 * process is a child only when it started after its parent, since Windows
 * gives a gone process's id to later ones. The root is not marked. Returns
 * how many are. */
int          Core_ProcessDescendants(const CoreProcess *processes, int count, int root, BOOL *chosen);
BOOL         Core_ArchiveNameSafe(const char *name, size_t length);
BOOL         Core_ConversationFileName(const WCHAR *relative);
int          Core_ScrollStep(int pending, int elapsedMs);
int          Core_WheelPixels(int delta, int notchPx, int *rest);
ULONGLONG    Core_HashBytes(ULONGLONG hash, const void *data, size_t size);
ULONGLONG    Core_HashText(ULONGLONG hash, const WCHAR *text);
BOOL         Core_ZoomPartitionKey(const WCHAR *folder, char *out, size_t cch);   /* Electron's name for a profile's zoom levels */
void         Core_CenterRect(const RECT *on, int width, int height, const RECT *work, RECT *out);
BOOL         Core_ParseVersion(const WCHAR *text, DWORD parts[4]);
int          Core_CompareVersions(const DWORD a[4], const DWORD b[4]);

/* The shield's facts (shield.c): Claude's exit IP as Cloudflare's trace gives
 * it, whether Claude works there (Anthropic's supported regions), what an IP
 * service says of that IP, the UDP exit a STUN server sees, and what the
 * rules make of them all. A three-way fact is 1 (yes), 0 (no) or -1 (not known). */
typedef enum ShieldSource { SHIELD_SOURCE_NETCOFFEE, SHIELD_SOURCE_IPAPIIS, SHIELD_SOURCE_IPAPICOM, SHIELD_SOURCE_PROXYCHECK,
                            SHIELD_SOURCES } ShieldSource;
typedef struct CoreIpRisk {
    int       source;                  /* the ShieldSource that answered */
    WCHAR     country[64], countryCode[4], region[64], city[64], timezone[64], isp[128];
    ULONGLONG asn;
    int       trustScore;              /* 0 to 100, -1: not known (ip.net.coffee alone scores) */
    int       datacenter, residential, mobile, vpn, proxy, tor, abuser;
} CoreIpRisk;
BOOL         Core_ParseTrace(const char *text, size_t len, WCHAR *ip, size_t ipCch, WCHAR *loc, size_t locCch);
BOOL         Core_ClaudeRegionSupported(const WCHAR *countryCode, const WCHAR *region);
/* `json` as source `source` answers for `ip` (proxycheck.io keys its answer by it). */
BOOL         Core_ParseIpRisk(int source, const char *json, size_t len, const WCHAR *ip, CoreIpRisk *out);
size_t       Core_StunRequest(BYTE *out, size_t cap, const BYTE *transaction);   /* a Binding request, 20 bytes; `transaction` 12 */
BOOL         Core_StunMappedAddress(const BYTE *message, size_t len, const BYTE *transaction, WCHAR *ip, size_t cch);
#define SHIELD_STOP_REGION     0x001u   /* Claude's exit is where Claude does not work */
#define SHIELD_STOP_DATACENTER 0x002u
#define SHIELD_STOP_SCORE      0x004u   /* under the lowest trust score allowed */
#define SHIELD_STOP_DNS        0x008u   /* a DNS server in a region Claude does not serve */
#define SHIELD_STOP_UDP        0x010u   /* UDP leaves by another IP than Claude's exit */
#define SHIELD_STOP_UNKNOWN    0x020u   /* Claude's exit, or a fact a rule needs, could not be read */
#define SHIELD_STOP_TUN        0x040u   /* no TUN adapter is up */
#define SHIELD_STOP_PROXY_APP  0x080u   /* no proxy app runs */
#define SHIELD_STOP_IPV6       0x100u   /* a public IPv6 that could leave Claude's proxy */
#define SHIELD_STOP_PROFILE    0x200u   /* the exit is not a region this profile allows */
#define SHIELD_CONCERN_TZ      0x400u   /* this PC's time zone does not match the exit's region */
#define SHIELD_CONCERN_LANG    0x800u   /* this PC's language does not match the exit's region */
#define SHIELD_CONCERN_SPLIT   0x1000u  /* the default (Cloudflare) exit is in another country than Claude's */
#define SHIELD_LOW_SCORE       50       /* a trust score under this one is a concern, as ip.net.coffee rates it */
typedef struct CoreShieldFacts {
    BOOL exitKnown, supported, riskKnown;
    int  trustScore;                   /* -1: not known */
    int  datacenter, dnsLeak, udpLeak, tunUp, proxyApp, ipv6Leak;
    int  tzMismatch, langMismatch, cfSplit;   /* concerns only (shown, never a stop by default) */
    BOOL profileAllowed;               /* the exit is in a region this profile allows (TRUE when it has no list) */
} CoreShieldFacts;
typedef struct CoreShieldRules {
    BOOL allowDatacenter, allowDnsLeak, allowUdpLeak, allowIpv6Leak;
    BOOL stopWhenUnknown, stopWhenTunDown, stopWhenProxyAppStops;
    BOOL warnOnly;                     /* never end Claude: a broken rule only shows red */
    int  minScore;                     /* 0: none */
} CoreShieldRules;
DWORD        Core_ShieldVerdict(const CoreShieldFacts *facts, const CoreShieldRules *rules);   /* what makes the shield stop Claude */
DWORD        Core_ShieldConcerns(const CoreShieldFacts *facts);   /* what is not as it should be, rules aside */
/* The exit's time zone offset from UTC in minutes, DST aside, for an IANA
 * name ("America/Los_Angeles") or a 2-letter country code; INT_MIN when not
 * known. Multi-zone countries give their most populous zone. */
int          Core_TimezoneOffsetMinutes(const WCHAR *ianaOrCode);
/* `primary` (a language's first subtag, "en", "zh") is one the region of
 * `countryCode` commonly uses; TRUE when the region is not known (no
 * mismatch to claim). */
BOOL         Core_RegionLanguageMatches(const WCHAR *countryCode, const WCHAR *primary);
/* The exit is in one of `allowed` (comma- or space-separated 2-letter
 * codes); TRUE when the list is empty (the profile allows any region). */
BOOL         Core_RegionAllowed(const WCHAR *countryCode, const WCHAR *allowed);
BOOL         Core_RegionCodes(const WCHAR *text);   /* two-letter codes only, between spaces, commas or semicolons; or none */

/* --------------------------------------------------------------- util.c */

extern HINSTANCE g_hInst;

BOOL      Util_KnownFolder(const GUID *id, WCHAR *out, size_t cch);
BOOL      Util_AppData(WCHAR *out, size_t cch);        /* %APPDATA% */
BOOL      Util_LocalAppData(WCHAR *out, size_t cch);   /* %LOCALAPPDATA% */
BOOL      Util_SelfExe(WCHAR *out, size_t cch);
BOOL      Util_InstallDir(WCHAR *out, size_t cch);     /* %LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager */
BOOL      Util_InstallExe(WCHAR *out, size_t cch);
BOOL      Util_StateDir(WCHAR *out, size_t cch);       /* %LOCALAPPDATA%\Claude Desktop Profiles Manager */
#define SETTING_VAULT_DAYS     L"VaultKeepDays"        /* days the vault keeps a list's versions; 0: all */
#define SETTING_VAULT_DAYS_DEFAULT 30
#define SETTING_WEEKLY_BACKUP  L"WeeklyClaudeBackup"   /* 1: .claude copied beside itself once a week */
#define SETTING_REOPEN_AFTER_UPDATE L"ReopenAfterClaudeUpdate"   /* 0: a profile closed by a Claude update stays closed */
#define SETTING_SIGNIN_BROWSER L"SignInBrowser"   /* the browser private sign-in starts with: a SIGNIN_BROWSER_* */
#define SETTING_SIGNIN_DEVTOOLS L"SignInDevToolsPort"   /* a port: the sign-in window serves DevTools (CDP) on 127.0.0.1 there; 0, none */
#define SETTING_SIGNIN_SKIP_ONBOARDING L"SignInSkipOnboarding"   /* 1: once signed in, tell claude.ai the account's onboarding is finished, so it does not ask; 0, off */
#define SETTING_ZOOM           L"Zoom"   /* the size of every window of the program, in percent (Theme_Zoom) */
#define SETTING_MCP_ONLINE     L"WindowsMcpOnline"   /* 1: windows-mcp-delta may read and search chats on claude.ai */
#define SETTING_SEARXNG_SECTION L"SearXNG"   /* a section whose one item is the SearXNG address set up last, or set in Settings; none: SEARXNG_DEFAULT_URL */
#define SETTING_SEARXNG_PRIVATE L"SearXngPrivateUrls"   /* 1: SearXNG search set up last could reach private addresses */
#define SETTING_NEW_WINDOWS_MCP L"NewProfileWindowsMcp"   /* 0: a new profile is made without Windows MCP; else with it, uv being there */
#define SETTING_NEW_SEARXNG    L"NewProfileSearXng"   /* 0: a new profile is made without SearXNG search; else with it, Node.js being there */
#define SETTING_NEW_COWORK_OFF L"NewProfileCoworkOff"   /* 0: a new profile keeps Cowork; else Cowork is off in it, and no virtual machine comes */
#define SETTING_GH_LATER       L"GhBuildLater"   /* the release put off with Later (its commit's checksum): offered again only once another is out */
#define SETTING_PANE_SEARCH    L"PaneSearch"   /* where a property list's pane searches, and how (CHECK_SEARCH_*) */
DWORD     Util_GetSetting(const WCHAR *name, DWORD fallback);
BOOL      Util_SetSetting(const WCHAR *name, DWORD value);
/* A list kept in a section of its own of settings.ini: its items in order, at most `capacity`. */
int       Util_GetSettingList(const WCHAR *section, WCHAR (*items)[MAX_PATH], int capacity);
BOOL      Util_SetSettingList(const WCHAR *section, const WCHAR (*items)[MAX_PATH], int count);   /* none: the section goes */
void      Util_SetStateDir(const WCHAR *dir);          /* tests: a private state folder */
/* One sync at a time, across processes (the state folder's lock: a test has
 * its own). The thread that holds it takes it again; NULL after
 * SYNC_LOCK_WAIT_MS, the sync then not made. */
HANDLE    Util_SyncLock(void);
void      Util_SyncUnlock(HANDLE lock);
BOOL      Util_FileExists(const WCHAR *path);
BOOL      Util_DirExists(const WCHAR *path);
typedef enum PathState { PATH_MISSING, PATH_PRESENT, PATH_UNREACHABLE } PathState;
PathState Util_QueryPath(const WCHAR *path, DWORD *attributes);
BOOL      Util_ExistingDir(const WCHAR *path, WCHAR *out, size_t cch);
BOOL      Util_EnsureDir(const WCHAR *path);
BOOL      Util_RegGetString(HKEY root, const WCHAR *key, const WCHAR *value, WCHAR *out, size_t cch);
BOOL      Util_RegSetString(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data);
BOOL      Util_RegSetStringIfDifferent(HKEY root, const WCHAR *key, const WCHAR *value, const WCHAR *data, BOOL *changed);
BOOL      Util_RegGetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out);
BOOL      Util_RegSetDword(HKEY root, const WCHAR *key, const WCHAR *value, DWORD data);
BOOL      Util_RegKeyExists(HKEY root, const WCHAR *key);
BOOL      Util_RegValueExists(HKEY root, const WCHAR *key, const WCHAR *value);
LSTATUS   Util_RegDeleteValue(HKEY root, const WCHAR *key, const WCHAR *value);
LSTATUS   Util_RegDeleteTree(HKEY root, const WCHAR *key);
ULONGLONG Util_LocalNowTicks(void);
void      Util_Log(const WCHAR *fmt, ...);
BOOL      Util_LogDirectory(WCHAR *out, size_t cch);
BOOL      Util_OpenUrl(const WCHAR *url);
BOOL      Util_Spawn(const WCHAR *exe, const WCHAR *args, DWORD *pid);
BOOL      Util_ClipboardText(WCHAR *out, size_t cch);   /* the clipboard's text, whole; FALSE when it holds none or a longer one */
BOOL      Util_IsDirectoryLink(const WCHAR *path);     /* a junction or directory symlink */
BOOL      Util_FitsRecycleBin(const WCHAR *path);
RemoveResult Util_Recycle(HWND owner, const WCHAR *const *paths, int count);
char     *Util_ReadFile(const WCHAR *path, DWORD maxBytes, BOOL tail, DWORD *len);
BOOL      Util_ExtendedPath(const WCHAR *path, WCHAR *out, size_t cch);   /* the \\?\ form, for paths past MAX_PATH */
ULONGLONG Util_NowMs(void);   /* the system clock, in ms since 1970 */
/* A time in ms since 1970 as the user's short date and time (TIME_NOSECONDS in
 * `timeFlags` leaves the seconds out); FALSE for 0 or one that cannot be shown. */
BOOL      Util_FormatWhen(ULONGLONG ms, DWORD timeFlags, WCHAR *date, size_t dateCch, WCHAR *time, size_t timeCch);
/* `items` with room for `need` of `size` bytes (the new ones zero), its
 * `capacity` doubled as needed; NULL, `items` left as it was, without memory. */
void     *Util_Grow(void *items, int *capacity, int need, size_t size);
/* A new id as Claude names its sessions: a GUID in lower case without braces;
 * `random` (optional) gets 32 of its random bits. */
BOOL      Util_NewId(WCHAR *out, size_t cch, DWORD *random);
void     *Util_Alloc(size_t bytes);                    /* zeroed, from the process heap; Util_Free */
void      Util_Free(void *block);                      /* NULL does nothing */
char     *Util_CopyBytes(const char *data, size_t length);   /* zero-terminated; Util_Free */
BOOL      Util_JoinPath(const WCHAR *dir, const WCHAR *name, WCHAR *out, size_t cch);   /* dir\name */
BOOL      Util_WriteAll(HANDLE file, const void *data, size_t length);   /* every byte, in chunks WriteFile takes */
/* A file replaced in one step: written beside it, flushed, then put in place. */
#define UTIL_TEMPORARY_SUFFIX L".cdm-new"
typedef enum UtilPut { UTIL_PUT_NEW, UTIL_PUT_REPLACE, UTIL_PUT_OVER } UtilPut;
BOOL      Util_WriteTemporary(const WCHAR *path, const char *data, size_t length, WCHAR *temporary, size_t cch);
BOOL      Util_PutInPlace(const WCHAR *temporary, const WCHAR *path, UtilPut how);
BOOL      Util_WriteFileAtomic(const WCHAR *path, const char *data, size_t length, UtilPut how);
HANDLE    Util_FindFiles(const WCHAR *dir, const WCHAR *pattern, WIN32_FIND_DATAW *found, BOOL foldersOnly);
BOOL      Util_CopyTree(const WCHAR *from, const WCHAR *to, BOOL replace, DWORD *error);
BOOL      Util_DeleteTree(const WCHAR *path, DWORD *error);
BOOL      Ui_ReviewText(HWND owner, const WCHAR *title, const WCHAR *note, const WCHAR *text, const WCHAR *action);
void      Diagnostics_Show(HWND owner);
void      Diagnostics_OpenLogs(HWND owner);
BOOL      Diagnostics_Text(const ClaudePackage *pkg, const ProfileList *profiles, WCHAR *out, size_t cch);

/* -------------------------------------------------------------- claude.c */

BOOL    Claude_FindPackage(ClaudePackage *pkg);
HRESULT Claude_Launch(const ClaudePackage *pkg, const Profile *profile, const WCHAR *url,
                      DWORD *pid, BOOL *withIdentity);
void    Claude_UpdateRunning(ProfileList *list);
/* The next window after `after` (NULL: the first) of a running Claude's
 * single-instance lock, titled with its data folder (`title`); NULL after the last. */
HWND    Claude_NextInstanceWindow(HWND after, WCHAR *title, int cch);
BOOL    Claude_IsInstanceWindow(HWND window);   /* one of those, as made (a window event) */
void    Claude_RefreshRunning(Profile *profile);
BOOL    Claude_IsRunning(const Profile *profile);
BOOL    Claude_LastSignInStart(const ClaudePackage *pkg, const Profile *profile, ULONGLONG *ticks);
BOOL    Claude_ClosedForUpdate(const ClaudePackage *pkg, const Profile *profile);
int     Claude_TopmostProfile(const ProfileList *list);
/* Quits the profile's Claude: asked to close as Windows does before an
 * update (Restart Manager), ended when it does not within Windows' time, and
 * then the programs it started that still run. TRUE when none runs any more;
 * blocks for seconds, so it runs off the window's thread. */
BOOL    Claude_Quit(const Profile *profile, DWORD *error);
/* Every process of what `what` names ended at once, nothing asked: Claude's
 * app in any profile (its package's) with the programs it started, and
 * Claude Code on its own (its claude.exe).
 * `within`: only processes whose program is in that folder (tests), NULL: all.
 * Returns how many ended; `*failed`: how many would not. */
#define CLAUDE_END_DESKTOP 0x1u
#define CLAUDE_END_CODE    0x2u
int     Claude_EndAll(const ClaudePackage *pkg, DWORD what, const WCHAR *within, int *failed);

/* ------------------------------------------------------------ profiles.c */

void         Profiles_Load(ProfileList *list, const ClaudePackage *pkg);
BOOL         Profiles_ResolveStorage(Profile *p, const WCHAR *localAppData, const WCHAR *family);
int          Profiles_Find(const ProfileList *list, const WCHAR *folder);
/* The profiles of `bits` by name: "Personal", "Personal and Work", "Personal, Work and Test". */
void         Profiles_JoinNames(const ProfileList *list, DWORD bits, WCHAR *out, size_t cch);
int          Profiles_DefaultIndex(const ProfileList *list);
BOOL         Profiles_Create(const WCHAR *name, int color, WCHAR *folder, size_t folderCch, WCHAR *error, size_t errorCch);
/* The name, color, badge text (empty: the initial) and picture stamp (0: none). */
BOOL         Profiles_Update(const WCHAR *folder, const WCHAR *label, int color, const WCHAR *badge, DWORD picture);
BOOL         Profiles_SetDefault(const WCHAR *folder);
BOOL         Profiles_SetSyncGroup(const WCHAR *folder, int group);   /* 0: its sessions kept apart */
BOOL         Profiles_SetSyncItems(const WCHAR *folder, DWORD items);  /* SYNC_ITEM_*; 0: the default */
BOOL         Profiles_SetWindowTitle(const WCHAR *folder, const WCHAR *title);   /* empty: the default title */
BOOL         Profiles_SetNote(const WCHAR *folder, const WCHAR *note);
void         SignIn_Open(HWND owner, const Profile *profile);
/* The browsers a private sign-in can use: the program's own window (WebView2) when it can show, then those installed,
 * each with its icon (SignIn_FreeBrowsers). */
enum { SIGNIN_BROWSER_OWN, SIGNIN_BROWSER_EDGE, SIGNIN_BROWSER_CHROME, SIGNIN_BROWSER_BRAVE, SIGNIN_BROWSER_FIREFOX, SIGNIN_BROWSERS };
typedef struct SignInBrowsers {
    int   count;
    int   kind[SIGNIN_BROWSERS];
    WCHAR exe[SIGNIN_BROWSERS][MAX_PATH];   /* empty for the program's own */
    HICON icon[SIGNIN_BROWSERS];
} SignInBrowsers;
void         SignIn_FindBrowsers(SignInBrowsers *found);
const WCHAR *SignIn_BrowserName(int kind);   /* a SIGNIN_BROWSER_*'s name, in the interface's language */
/* A Magic link on the clipboard (Core_MagicLink), offered to the person: TRUE, the link in `url`, once they chose to open it. */
BOOL         SignIn_OfferMagicLink(HWND owner, WCHAR *url, size_t cch);
/* windowsmcp.c: Windows-MCP in a profile's mcpServers (claude_desktop_config.json), run by uv's uvx. */
/* A profile's Windows-MCP: none; one written without its transport (it can start as an HTTP server Claude cannot
 * talk to: rewritten when added again); or one that starts (also the person's own, whatever it is). */
enum { WINDOWS_MCP_NONE, WINDOWS_MCP_OUTDATED, WINDOWS_MCP_READY };
int          WindowsMcp_State(const Profile *p);
int          WindowsMcp_Leftovers(void);      /* servers whose Claude is gone, still running (an HTTP one holds its port) */
int          WindowsMcp_EndLeftovers(void);   /* those ended: how many */
BOOL         WindowsMcp_FindUvx(WCHAR *out, size_t cch);   /* its full path: Claude from the Store does not see PATH */
BOOL         WindowsMcp_InstallUv(HWND owner);              /* uv's own installer, in a console, waited for */
BOOL         WindowsMcp_Set(const Profile *p, const WCHAR *uvx);   /* windows-mcp-delta added with `uvx`; NULL takes it away */
BOOL         WindowsMcp_Online(void);   /* whether the person lets Claude read claude.ai chats (SETTING_MCP_ONLINE) */
void         WindowsMcp_RefreshOnline(void);   /* rewrite the env of profiles that have it, so the setting takes effect */
void         WindowsMcp_UpdateEntries(void);   /* entries of ours from an older build pointed at this build's source */
void         WindowsMcp_AfterClose(const WCHAR *folder);   /* the watcher's: what those two leave while a profile runs */
BOOL         WindowsMcp_PackageDir(WCHAR *out, size_t cch);   /* this build's source folder (version and checksum) */
BOOL         WindowsMcp_HasExtension(const Profile *p);   /* Claude's own Windows-MCP extension is installed in it, or left there */
BOOL         WindowsMcp_RemoveExtension(HWND owner, const Profile *p);   /* that extension deleted (its Claude closed); TRUE: all gone */
/* SearXNG search set up in a profile: the entry "searxng", mcp-searxng run by npx (windowsmcp.c). */
#define SEARXNG_KEY "searxng"
BOOL         SearXng_FindNpx(WCHAR *out, size_t cch);   /* npx.cmd's full path: Node.js is installed */
BOOL         SearXng_Get(const Profile *p, WCHAR *url, size_t cch, BOOL *allowPrivate);   /* it has ours: its address */
BOOL         SearXng_Set(const Profile *p, const WCHAR *npx, const WCHAR *url, BOOL allowPrivate);   /* npx NULL: ours taken away */
/* Cowork in a profile (windowsmcp.c): on unless its preferences turn it off (Core_CoworkOff); `vmBytes` (when not
 * NULL) gets its virtual machine's size, 0 without one. Cowork_Set writes the preference (its Claude closed);
 * Cowork_RemoveVm deletes the virtual machine, `freed` getting what went. */
BOOL         Cowork_Get(const Profile *p, ULONGLONG *vmBytes);
BOOL         Cowork_Set(const Profile *p, BOOL on);
BOOL         Cowork_RemoveVm(const Profile *p, ULONGLONG *freed);
/* Claude's preference `key` in a profile (claude_desktop_config.json, under "preferences"; windowsmcp.c): Prefs_Get
 * copies its JSON value to `raw`, FALSE when the profile sets none (Claude's default) or it does not fit; Prefs_Set
 * writes `raw`, or takes it out when NULL, its Claude closed (Claude keeps them in memory and writes them back). */
BOOL         Prefs_Get(const Profile *p, const char *key, char *raw, size_t cap);
BOOL         Prefs_Set(const Profile *p, const char *key, const char *raw);
void         SignIn_FreeBrowsers(SignInBrowsers *found);
/* `combo` lists them with their icons, `kind` chosen when it is there, else the first. */
void         SignIn_FillBrowsers(HWND combo, const SignInBrowsers *found, int kind);
/* webview.c: private sign-in in a WebView2 window of its own, its data folder `folder` (deleted after it). */
BOOL         WebSignIn_Available(void);
HRESULT      WebSignIn_Open(HWND owner, const Profile *profile, const WCHAR *url, const WCHAR *folder);
HRESULT      WebView_ShowPage(HWND owner, const WCHAR *title, const WCHAR *path);   /* a page of ours, its links in the default browser */

/* --------------------------------------------------------------- cookies.c */
/* A cookie read from the sign-in window, in plain, to write into a profile's store. */
#define COOKIE_NAME_CCH   256
#define COOKIE_VALUE_CCH  4096
#define COOKIE_HOST_CCH   256
#define COOKIE_PATH_CCH   256
typedef struct SignInCookie {
    WCHAR     name[COOKIE_NAME_CCH];
    WCHAR     value[COOKIE_VALUE_CCH];
    WCHAR     host[COOKIE_HOST_CCH];     /* the cookie's domain, as Chromium keys it (".claude.ai") */
    WCHAR     path[COOKIE_PATH_CCH];
    ULONGLONG expires;                   /* Chromium's microseconds since 1601; 0: a session cookie */
    int       secure, httpOnly, sameSite;   /* sameSite: 0 none, 1 lax, 2 strict (ICoreWebView2 order) */
} SignInCookie;
/* `cookies` written into profile `target`'s Claude cookie store (Network\Cookies), each value sealed with that
 * profile's own Local State key (Core_SealSafeStorage), replacing any of the same name, host and path. The
 * profile's Claude must be closed. TRUE when the store was written; `*wrote` the number put in. */
BOOL         Cookies_Write(const Profile *target, const SignInCookie *cookies, int count, int *wrote);
BOOL         Cookies_SignInHost(const WCHAR *host);   /* a .claude.ai host whose cookies a sign-in carries */
/* The sign-in cookies in a WebView2 data folder (webview.c's, <folder>\EBWebView): read from its own cookie
 * store, decrypted with its own Local State key, into `out` (`cap`), `*count` of them. TRUE when it could be read. */
BOOL         Cookies_ReadWebView(const WCHAR *webViewFolder, SignInCookie *out, int cap, int *count);
/* Whether that folder's store already holds a non-empty claude.ai session cookie: the sign-in is done. */
BOOL         Cookies_WebViewSignedIn(const WCHAR *webViewFolder);
DWORD        Profiles_SyncItems(const Profile *p);                    /* what it keeps the same, the default for none chosen */
void         Profiles_CopySettings(const Profile *from, const Profile *to);
RemoveResult Profiles_Delete(HWND owner, const Profile *profile);
RemoveResult Profiles_RecycleData(HWND owner, const Profile *profile);
BOOL         Profiles_IsLinked(const Profile *profile);
BOOL         Profiles_LinkTarget(const Profile *profile, WCHAR *out, size_t cch);

/* --------------------------------------------------------- sessionlink.c */
/* A profile's entries folder shared with another profile or kept in a
 * folder of the user's, through a directory junction earlier versions made:
 * read, and taken away. */
typedef enum LinkKind { LINK_NOT_SIGNED_IN, LINK_NO_SESSIONS, LINK_OWN, LINK_PROFILE, LINK_FOLDER, LINK_BROKEN } LinkKind;
typedef struct LinkState {
    LinkKind kind;
    int      profile;                 /* LINK_PROFILE: the profile whose folder it shares */
    WCHAR    dir[LONG_PATH_CCH];      /* its entries folder, as Claude finds it */
    WCHAR    target[LONG_PATH_CCH];   /* LINK_PROFILE and LINK_FOLDER: where it leads */
} LinkState;
void SessionLink_Read(const ProfileList *list, int index, LinkState *state);
/* Its Claude runs, or the Claude of a profile sharing its entries folder:
 * the entries are not written then (Claude writes its own back). */
BOOL SessionLink_Busy(const Profile *p);
/* A batch of writes on this thread: SessionLink_Busy resolves each entries folder once until it ends (nested ones count). */
void SessionLink_BeginBatch(void);
void SessionLink_EndBatch(void);
BOOL SessionLink_Remove(const ProfileList *list, int index, WCHAR *error, size_t errorCch);

/* ----------------------------------------------------------------- zip.c */
/* ZIP archives, files stored or deflated, names in UTF-8 with "/" between
 * folders; no ZIP64 (files and archive under 4 GB, 65535 files at most). A
 * large file is copied in stored, a chunk at a time. Zip_Close(zip, TRUE) puts
 * the archive in place (it was written next to it); FALSE, or a failure,
 * removes it. A file taken out is written next to its place first too. */
typedef struct ZipOut ZipOut;
typedef struct ZipIn ZipIn;
ZipOut     *Zip_Create(const WCHAR *path, DWORD *error);
BOOL        Zip_AddData(ZipOut *zip, const char *name, const void *data, size_t size);
BOOL        Zip_AddFile(ZipOut *zip, const char *name, const WCHAR *path);
BOOL        Zip_Has(const ZipOut *zip, const char *name);   /* a file of that name went in already (case ignored) */
DWORD       Zip_Error(const ZipOut *zip);   /* the first failure's Windows error */
BOOL        Zip_Close(ZipOut *zip, BOOL keep);
ZipIn      *Zip_Open(const WCHAR *path);   /* NULL: not a ZIP archive this module reads */
ZipIn      *Zip_OpenItem(const ZipIn *zip, int index);   /* the archive item `index` holds, read where it lies when stored */
void        Zip_Free(ZipIn *zip);
int         Zip_Count(const ZipIn *zip);
const char *Zip_Name(const ZipIn *zip, int index);
DWORD       Zip_Size(const ZipIn *zip, int index);
int         Zip_Find(const ZipIn *zip, const char *name);
void       *Zip_Read(const ZipIn *zip, int index, DWORD maxBytes, DWORD *size);   /* checked by its CRC; HeapFree it, NUL after it */
BOOL        Zip_Extract(const ZipIn *zip, int index, const WCHAR *path);   /* over one there */
BOOL        Zip_ExtractTo(const ZipIn *zip, int index, const WCHAR *path, UtilPut how);
BOOL        Zip_Validate(const WCHAR *path, HANDLE cancel, int *files, WCHAR *error, size_t cch);
BOOL        Zip_ValidateAll(const ZipIn *zip, HANDLE cancel, WCHAR *error, size_t cch);

/* -------------------------------------------------------------- backup.c */
/* A profile backed up to one archive, parts chosen (Code sessions, Cowork
 * sessions, settings, sign-in), and restored from one into a closed profile. */
BOOL Backup_Create(HWND owner, const ClaudePackage *pkg, const ProfileList *list, DWORD profiles);   /* one bit per profile: an archive each */
BOOL Backup_Restore(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index);
/* A restore from the backups `paths` (dropped on the profile), as Backup_Restore. */
BOOL Backup_RestoreFiles(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index, const WCHAR *const *paths, int pathCount);
/* What the manager keeps of a profile, as a backup carries it (its name, note, color, badge, windows' title and
 * picture): the parts it holds, and those taken, which an export writes and an import gives the profile. */
#define PROFILE_OWN_NAME    0x01u
#define PROFILE_OWN_NOTE    0x02u
#define PROFILE_OWN_COLOR   0x04u
#define PROFILE_OWN_BADGE   0x08u
#define PROFILE_OWN_TITLE   0x10u
#define PROFILE_OWN_PICTURE 0x20u
typedef struct ProfileOwn {
    DWORD held, parts;   /* PROFILE_OWN_*: those there are, those taken */
    WCHAR name[LABEL_CCH], note[256], badge[BADGE_CCH], title[WINDOW_TITLE_CCH];
    int   color;
} ProfileOwn;
/* The profile's own that backup `path` holds, every part taken; FALSE when it holds none (or is no backup). */
BOOL Backup_ReadOwn(const WCHAR *path, ProfileOwn *own);
/* A restore from `paths` into profile `index`, made just now for them with their own (Backup_ReadOwn): of
 * that only the picture is offered again, and the restore is not asked twice. */
BOOL Backup_RestoreNew(HWND owner, const ProfileList *list, int index, const WCHAR *const *paths, int pathCount);
/* What an export takes, as the person chose it: its items; for one profile, its own as left (nothing held: each
 * one's as it is) and the sessions kept, by key (NULL: every one; a heap block, which Backup_FreeChoice frees). */
typedef struct BackupChoice {
    DWORD        items;   /* BK_* and SYNC_ITEM_* */
    ProfileOwn   own;
    WCHAR      (*sessions)[SESSION_ID_CCH];
    int          sessionCount;
    char        *settings;   /* the settings the person changed, as a layout of those parts alone; NULL: none (a heap block) */
    size_t       settingsLength;
    DWORD        reopen;     /* the profiles closed for the export (their sign-in is in use while they run), to open again */
} BackupChoice;
void Backup_FreeChoice(BackupChoice *choice);
void Backup_Reopen(const ProfileList *list, DWORD profiles);   /* those closed for an export, opened again */
/* What a backup of `profiles` holds, as the person chooses it (everything at first); FALSE when cancelled. */
BOOL Backup_ChooseItems(HWND owner, const ProfileList *list, DWORD profiles, BackupChoice *choice);
BOOL Backup_Write(const ProfileList *list, int index, const BackupChoice *choice, const WCHAR *path, WCHAR *error, size_t errorCch);
void Backup_ArchiveName(const WCHAR *profileName, WCHAR *name, size_t cch);   /* "<name> <date>.zip" */
struct CheckList;
struct CheckList *Backup_FillPartsList(HWND list, int sessions);   /* the parts a backup keeps as its dialog shows them, a profile's own as values (tests) */
struct CheckList *Backup_FillImportList(HWND list);   /* an import's list: three sessions and some settings one by one (tests) */
struct CheckList *Backup_FillLayoutList(HWND list, const char *layout, size_t length);   /* the same, with the settings of `layout` (tests) */
/* Local State's os_crypt key in `storage`, opened with DPAPI for this Windows user; written protected again for it. */
BOOL Backup_ReadAppKey(const WCHAR *storage, BYTE *key, size_t cap, size_t *length);
BOOL Backup_WriteAppKey(const WCHAR *storage, const BYTE *key, size_t length);

/* --------------------------------------------------------------- icons.c */

extern const WCHAR *const g_ColorNames[PALETTE_SIZE];
COLORREF Icons_PaletteColor(int index);
BOOL  Icons_Ensure(const ClaudePackage *pkg, const Profile *profile, WCHAR *out, size_t cch);
HICON Icons_Create(const ClaudePackage *pkg, const Profile *profile, int size);
/* As Icons_Create, with `picture` (PICTURE_SIZE squared pixels, 0xAARRGGBB;
 * NULL: the Claude icon and the badge) in place of the profile's own. */
HICON Icons_CreateWith(const ClaudePackage *pkg, const Profile *profile, const DWORD *picture, int size);
WCHAR Icons_ProfileInitial(const WCHAR *name);
void  Icons_BadgeText(const Profile *profile, WCHAR *out, size_t cch);   /* its own text, else the initial */
/* Pictures: any image Windows reads (WIC: PNG, JPEG, GIF's first frame, BMP,
 * TIFF, ICO, and WebP or HEIF with their extensions), cut to its middle square
 * and scaled to PICTURE_SIZE. A heap block the caller frees with HeapFree. */
DWORD *Icons_ReadPicture(const WCHAR *path, HRESULT *hr);
DWORD *Icons_LoadPicture(const Profile *profile);   /* the profile's own, NULL when it has none or it cannot be read */
BOOL   Icons_SavePicture(const WCHAR *folder, const DWORD *pixels, DWORD *stamp);
void   Icons_DeletePicture(const WCHAR *folder);
BOOL   Icons_PicturePath(const WCHAR *folder, WCHAR *out, size_t cch);   /* where a profile's own picture is kept (a PNG) */
HICON Icons_CreateTray(const ClaudePackage *pkg, const Profile *profile, int size, BOOL darkTaskbar);
BOOL  Icons_IsStale(const ClaudePackage *pkg, const Profile *profile);
void  Icons_DeleteStale(const Profile *profile, const WCHAR *keep);

/* ----------------------------------------------------------- shortcuts.c */

#define MAX_LINK_COPIES    50   /* "Claude (<name>) (n).lnk": n up to this */
#define AUMID_CCH          64   /* a profile's or the manager's AppUserModelID */
#define APP_MANAGER_AUMID  APP_AUMID_PREFIX L"Manager"   /* the manager's windows and Start menu shortcut */

BOOL    Shortcut_Read(const WCHAR *lnk, LinkInfo *info);
BOOL    Shortcut_HasAppId(const WCHAR *lnk, const WCHAR *aumid);
HRESULT Shortcut_CreateForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
BOOL    Shortcut_IsAtStartup(const Profile *profile);
BOOL    Shortcut_StartMenuDir(WCHAR *out, size_t cch);
BOOL    Shortcut_IsInStartMenu(const Profile *profile);
HRESULT Shortcut_AddToStartMenu(const ClaudePackage *pkg, const Profile *profile);
HRESULT Shortcut_RemoveFromStartMenu(const Profile *profile);
HRESULT Shortcut_SetStartup(const ClaudePackage *pkg, const Profile *profile, BOOL on);
HRESULT Shortcut_WriteForProfile(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
BOOL    Shortcut_DesktopPathFor(const Profile *profile, WCHAR *out, size_t cch);
/* A free "Claude (<label>).lnk", else "Claude (<label>) (n).lnk", in `dir`. */
BOOL    Shortcut_FreePath(const WCHAR *dir, const WCHAR *label, WCHAR *out, size_t cch);
BOOL    Shortcut_IsOnDesktop(const Profile *profile);
void    Shortcut_RemoveOurs(const Profile *profile);
HRESULT Shortcut_Update(const WCHAR *lnk, const Profile *after, const WCHAR *icon);
BOOL    Shortcut_RenamedPath(const WCHAR *path, const Profile *before, const Profile *after, WCHAR *out, size_t cch);
BOOL    Shortcut_Refresh(const Profile *before, const Profile *after, const WCHAR *icon);
void    Shortcut_RefreshProfiles(const ProfileList *list, const WCHAR *const *icons);
DWORD   Shortcut_ListOurs(const WCHAR *folder, WCHAR (*paths)[MAX_PATH], DWORD capacity);
HRESULT Shortcut_CreateManagerLink(const WCHAR *exe);
HRESULT Shortcut_RestoreManagerLink(const WCHAR *exe);   /* S_FALSE: it is as it should be */
void    Shortcut_RemoveManagerLink(void);

/* -------------------------------------------------------------- update.c */

typedef enum UpdateResult {
    UPDATE_READY,            /* downloaded and verified, held unchanged: Update_Run starts it */
    UPDATE_STARTED,          /* it runs, and installs itself over this copy */
    UPDATE_NOT_DOWNLOADED,
    UPDATE_NOT_SIGNED,       /* not signed by APP_SIGNER: deleted */
    UPDATE_NOT_VERIFIED,     /* Windows could not check the signature (the error is its answer): deleted */
    UPDATE_WRONG_VERSION,    /* signed, but not the release's version, newer than this one: deleted */
    UPDATE_NOT_STARTED       /* verified, but Windows did not start it (the error says why): deleted */
} UpdateResult;

void         Update_Check(HWND notify, UINT message);
BOOL         Update_Available(WCHAR *version, size_t cch);
void         Update_Download(HWND notify, UINT message);   /* message: wParam an UpdateResult, lParam an error */
UpdateResult Update_Run(DWORD *error);
void         Update_RemoveDownload(void);
BOOL         Update_DownloadPath(WCHAR *out, size_t cch);

/* ------------------------------------------------------------- ghupdate.c */
/* Releases of main (CI's one a day, "v<date>", in the private repository) offered with what this PC signs in to
 * GitHub with, gh or git's credential: found when the manager opens or when asked, downloaded with gh, run with
 * --install. No sign-in at all: a copy brought from elsewhere (GH_COPIED, GhUpdate_Copied), offered nothing. */
#define GH_TAG_CCH 32
typedef enum GhStatus { GH_NEWER, GH_CURRENT, GH_NONE, GH_COPIED, GH_FAILED } GhStatus;
typedef struct GhRelease {
    GhStatus status;
    BOOL     viaGit;                   /* found by git, no gh: Update opens its release page */
    BOOL     local;                    /* this copy was built on this PC: offered a release only when asked */
    WCHAR    tag[GH_TAG_CCH];          /* the newest release's, v2026.10.26 */
    WCHAR    version[16];              /* its day, 2026.10.26 */
    WCHAR    commit[48];               /* what it was built from */
    WCHAR    title[256];               /* its name, "Claude Delta v2026.10.26" */
    WCHAR   *notes;                    /* its Markdown; NULL: none */
    WCHAR    message[256];             /* gh's first line, when it failed */
} GhRelease;
typedef struct GhDownload {
    BOOL  ok;
    WCHAR path[LONG_PATH_CCH];         /* the exe gh wrote */
    WCHAR message[256];                /* why not, when it did not */
} GhDownload;
BOOL         GhUpdate_FindGh(WCHAR *out, size_t cch);
int          GhUpdate_OwnBuild(void);         /* this copy's build number, 0 when built locally */
const WCHAR *GhUpdate_OwnCommit(void);
const WCHAR *GhUpdate_Repo(void);             /* "owner/repository" its builds come from */
BOOL         GhUpdate_Copied(void);           /* the last check found no sign-in to GitHub on this PC */
void         GhUpdate_ReleasePage(const WCHAR *tag, WCHAR *out, size_t cch);
void         GhUpdate_Check(HWND notify, UINT message, BOOL asked);   /* message: wParam `asked`, lParam a GhRelease (GhUpdate_Free) */
void         GhUpdate_Free(GhRelease *release);
void         GhUpdate_Download(HWND notify, UINT message, const WCHAR *tag);   /* message: lParam a GhDownload (Util_Free) */
BOOL         GhUpdate_Run(const GhDownload *download, DWORD *error);   /* it runs, and installs itself over this copy */

/* --------------------------------------------------------- taskbar-pin.c */

BOOL    TaskbarPin_Dir(WCHAR *out, size_t cch);
BOOL    TaskbarPin_IsPinned(const Profile *profile);
BOOL    TaskbarPin_HasAppId(const WCHAR *aumid);
BOOL    TaskbarPin_UsesIcon(const WCHAR *icon);
BOOL    TaskbarPin_ValidateList(const BYTE *list, size_t len);
HRESULT TaskbarPin_ReadList(HKEY key, BYTE **list, DWORD *len);
typedef struct TaskbarPin_ValueIO {   /* the pin list's values: the user's, or a test's */
    void *context;
    LSTATUS (*read)(void *context, const WCHAR *name, DWORD *type, BYTE *data, DWORD *bytes);
    LSTATUS (*write)(void *context, const WCHAR *name, DWORD type, const BYTE *data, DWORD bytes);
    LSTATUS (*erase)(void *context, const WCHAR *name);
} TaskbarPin_ValueIO;
typedef struct TaskbarPin_CommitResult {
    BOOL listCommitted;
    BOOL uncertain; /* a new shortcut must be kept while the list and its records are uncertain */
    LSTATUS rollbackError;
} TaskbarPin_CommitResult;
typedef struct TaskbarPin_Value {
    BOOL  exists;
    DWORD type, bytes;
    BYTE *data;
} TaskbarPin_Value;
#define TASKBAR_PIN_NONE ((size_t)-1)
typedef struct TaskbarPin_Merge {
    size_t rank;        /* the entry's index in the new list */
    BOOL   replacedPin; /* it took the place of an active entry */
} TaskbarPin_Merge;
#define TASKBAR_PIN_MAX_OBSOLETE 16   /* shortcuts of dropped entries deleted at once; more only linger */
typedef struct TaskbarPin_Addition {
    const BYTE  *entry;
    DWORD        entrySize;
    const WCHAR *shortcut;      /* the .lnk the entry pins */
    const WCHAR *aumid;
    const WCHAR *pinsDir;       /* the only folder a dropped entry's shortcut is deleted from */
    BOOL         replacedPin;   /* out: the entry took the place of an active one */
    WCHAR        obsoleteShortcuts[TASKBAR_PIN_MAX_OBSOLETE][MAX_PATH];   /* out: the shortcuts to delete */
    DWORD        obsoleteShortcutCount;
} TaskbarPin_Addition;
BOOL    TaskbarPin_RegistryValues(HKEY key, TaskbarPin_ValueIO *io);
HRESULT TaskbarPin_CommitPrepared(const TaskbarPin_ValueIO *io, const TaskbarPin_Value *base,
                                  const BYTE *list, size_t listLen,
                                  const BYTE *records, size_t recordsLen, BOOL writeRecords, TaskbarPin_CommitResult *result);
HRESULT TaskbarPin_AddToList(const TaskbarPin_ValueIO *io, TaskbarPin_Addition *addition, TaskbarPin_CommitResult *result);
HRESULT TaskbarPin_RepairList(const TaskbarPin_ValueIO *io);
HRESULT TaskbarPin_RemoveFromList(const TaskbarPin_ValueIO *io, WCHAR (*shortcuts)[MAX_PATH], DWORD shortcutCount);
BOOL    TaskbarPin_ListHasAppId(const BYTE *list, size_t len, const WCHAR *appId, BOOL live);
BOOL    TaskbarPin_ListHasProfile(const BYTE *list, size_t len, const Profile *profile);
HRESULT TaskbarPin_MergeEntry(const BYTE *list, size_t len, const BYTE *entry, size_t entrySize,
                              const WCHAR *lnk, const WCHAR *aumid, BOOL *dropped,
                              BYTE *out, size_t cap, size_t *outLen, TaskbarPin_Merge *merge);
HRESULT TaskbarPin_BuildRecord(const BYTE *entry, size_t size, BYTE **data, size_t *len);
HRESULT TaskbarPin_PrepareRecords(const BYTE *oldList, size_t oldLen, const BYTE *newList, size_t newLen,
                                  const BOOL *dropped, size_t fresh, BOOL edited,
                                  const BYTE *records, size_t recordsLen, BYTE **out, size_t *outLen);
HRESULT TaskbarPin_UpdateLeftover(const ClaudePackage *pkg, const Profile *profile, const WCHAR *lnk);
HRESULT TaskbarPin_Pin(const ClaudePackage *pkg, const Profile *profile);
void    TaskbarPin_Refresh(const Profile *before, const Profile *after, const WCHAR *icon);
/* The shortcuts Windows made for the profile's taskbar button, whose icon
 * that button shows, get `icon` (Core_TakesCurrentIcon) and the profile's
 * name. TRUE when one changed. */
BOOL    TaskbarPin_RefreshImplicit(const Profile *profile, const WCHAR *icon);
void    TaskbarPin_RefreshProfiles(const ProfileList *list, const WCHAR *const *icons);
void    TaskbarPin_TellShortcutChanged(const WCHAR *lnk);
void    TaskbarPin_RepairOurs(void);
void    TaskbarPin_RemoveOurs(void);
size_t  TaskbarPin_InjectAppId(const BYTE *item, size_t cb, const WCHAR *appId, BYTE *out, size_t cap);
size_t  TaskbarPin_RepairItem(const BYTE *item, size_t cb, BYTE *out, size_t cap);

/* ------------------------------------------------------------- handler.c */

typedef enum UserChoiceState { USERCHOICE_NONE, USERCHOICE_OURS, USERCHOICE_OTHER } UserChoiceState;

BOOL            Handler_Register(const WCHAR *exe);
UserChoiceState Handler_UserChoice(void);
BOOL            Handler_AskUser(void);
void            Handler_Unregister(void);

/* -------------------------------------------------------------- taskbar.c */

void Taskbar_Watch(const Profile *profile);
void Taskbar_Refresh(const Profile *profile, BOOL linksChanged);
void Taskbar_QuitComing(const Profile *profile);   /* the manager is about to quit its Claude: not for an update */
BOOL Taskbar_StopWatchers(BOOL giveBack);
BOOL Taskbar_IsWatched(const Profile *profile);
int  Taskbar_WatchRun(const WCHAR *folder, DWORD pid, void (*claudeClosed)(const WCHAR *folder));

/* --------------------------------------------------------- sessionstore.c */

#define SESSION_ENTRY_MAX_BYTES (4u * 1024u * 1024u)   /* larger, a file is not one of Claude's small JSON files */

typedef enum SessionResult { RESULT_UNKNOWN, RESULT_COMPLETED, RESULT_BLOCKED, RESULT_REVIEW, RESULT_ACTION, SESSION_RESULTS } SessionResult;
SessionResult Core_SessionResult(const char *json, size_t length, WCHAR *detail, size_t cch);
int Core_CompareSessionOrder(const WCHAR *nameA, ULONGLONG timeA, const WCHAR *nameB, ULONGLONG timeB, BOOL byName, BOOL descending);

typedef struct SessionEntry {          /* one profile's entry for a session */
    WCHAR     localId[SESSION_ID_CCH]; /* the entry's own id, its session's key until the first message */
    WCHAR     file[LONG_PATH_CCH];     /* its local_*.json */
    WCHAR     title[SESSION_TITLE_CCH];
    WCHAR    *previousTitles;         /* decoded MULTI_SZ, owned by SessionSet */
    WCHAR     model[128], effort[64], permissionMode[64];
    BOOL      userTitle;               /* named by someone, not by Claude */
    BOOL      autoTitle;               /* named by Claude itself (titleSource "auto"): it may name it again */
    BOOL      starred;
    BOOL      archived;
    BOOL      damaged;                 /* Claude's startup took it for a session whose transcript is gone (transcriptUnavailable) */
    BOOL      unread;                  /* Claude's sidebar shows it unread (its web storage, Core_UnreadHas) */
    ULONGLONG lastActivity;            /* ms since 1970 */
    ULONGLONG created;                 /* ms since 1970; 0 when not known */
    DWORD     environment;             /* SESSION_ENV_*: where it runs */
    PullRequestState pullRequest;      /* its pull request as Claude shows it */
    int       pullRequestNumber;
    SessionResult result;
    WCHAR     resultDetail[512];       /* a summary tied to the last assistant message, not live task state */
    /* Changes made here while the profile runs, waiting for it to close. */
    BOOL      pending;
    WCHAR     pendingTitle[SESSION_TITLE_CCH];   /* "" when unchanged */
    int       pendingStar;             /* -1 unchanged, 0 or 1 */
    int       pendingUnread;           /* -1 unchanged, 0 (read) or 1 */
    BOOL      pendingRemove;
    int       duplicate;               /* another entry of the session in the same profile, -1 when none */
    int       firstOtherTranscript, otherTranscriptCount;   /* the session's other transcripts, in SessionSet.otherTranscriptIds */
} SessionEntry;

typedef struct SessionRow {            /* one transcript */
    WCHAR     key[SESSION_ID_CCH];     /* its id, or the entry's own id before its first message */
    WCHAR     cwd[MAX_PATH];
    WCHAR     transcriptPath[LONG_PATH_CCH];
    int       group;
    BOOL      transcript;              /* on disk */
    ULONGLONG transcriptBytes;         /* with its other transcripts that no other session goes on with */
    ULONGLONG lastActivity;
    DWORD     live;                    /* the profiles running it now, one bit each (MAX_PROFILES <= 32) */
    int       entry[MAX_PROFILES];     /* into SessionSet.entries, the one Claude goes on with; -1: that profile does not list it */
} SessionRow;

typedef struct SessionGroup {          /* a project folder, or one profile's "no folder" sessions */
    WCHAR     name[MAX_PATH];          /* the folder's own name; empty for "no folder" and an unknown folder */
    WCHAR     path[MAX_PATH];          /* empty for "no folder" and an unknown folder */
    int       scratchOf;               /* that profile for "no folder", else -1 */
    ULONGLONG lastActivity;
} SessionGroup;

typedef struct SessionSource {         /* what reading one profile's entries found */
    BOOL      signedIn;                /* its config.json names an account */
    BOOL      complete;                /* every entry was read, or the folder is known absent */
    int       unreadable;              /* entries that are not a session entry */
    int       elsewhere;               /* SSH, WSL or cloud sessions: their conversation is not on this PC */
    int       pending;                 /* waiting changes its entries can take once it is closed */
    WCHAR     entriesDir[MAX_PATH];    /* claude-code-sessions\<account>\<organization>; empty when it has none */
    WCHAR     scratchDir[MAX_PATH];    /* logical scratch-workspaces\<account>\<organization>: its "no folder" */
    WCHAR     formerScratchDir[MAX_PATH]; /* the same below its formerDir, named by earlier entries; empty without one */
} SessionSource;

typedef struct SessionRowIndex SessionRowIndex;

typedef struct SessionSet {
    ProfileList      profiles;
    SessionSource    source[MAX_PROFILES];
    BOOL             noTranscripts;    /* no Claude Code transcripts folder */
    SessionEntry    *entries;
    SessionRow      *rows;             /* by group, then last activity */
    SessionGroup    *groups;
    SessionRowIndex *index;            /* rows by key (SessionStore_FindRow) */
    WCHAR          (*otherTranscriptIds)[SESSION_ID_CCH];
    int              entryCount, entryCap, rowCount, rowCap, groupCount, groupCap, otherTranscriptCount, otherTranscriptCapacity;
} SessionSet;

#define SESSION_PENDING_MAX 256

BOOL         SessionStore_LoadProfiles(SessionSet *set, const ProfileList *profiles);
BOOL         SessionStore_LoadProfilesCancel(SessionSet *set, const ProfileList *profiles, HANDLE cancel);
BOOL         SessionStore_LoadEntries(SessionSet *set, const Profile *profile);   /* FALSE: not every entry could be read */
void         SessionStore_Free(SessionSet *set);
int          SessionStore_FindRow(const SessionSet *set, const WCHAR *key);
int          SessionStore_FindEntry(const SessionSet *set, int profile, const WCHAR *key, int *row);
const WCHAR **SessionStore_TranscriptIds(const SessionSet *set, int row, int *count);   /* HeapFree the array */
BOOL         SessionStore_ClaimedByOther(const SessionSet *set, int row, int profile, const WCHAR *id);
const WCHAR *SessionStore_OwnId(const SessionEntry *entry);                                 /* without local_ */
DWORD        SessionStore_RunningNow(const ProfileList *profiles, const WCHAR *sessionId, BOOL *outside);
BOOL         SessionStore_ClaudeCodePath(const WCHAR *sub, WCHAR *out, size_t cch);
BOOL         SessionStore_ProjectsDir(WCHAR *out, size_t cch);
BOOL         SessionStore_SessionsDir(const Profile *p, WCHAR *out, size_t cch);
/* The folder of the entries Claude shows for `p`: claude-code-sessions\<account>\<organization>.
 * FALSE when there is none; `signedIn` then says whether an account is known. */
BOOL         SessionStore_EntriesDir(const Profile *p, WCHAR *out, size_t cch, BOOL *signedIn);
/* The entries' cache on disk (entry-cache.bin, which a new process reads first): written now (`now`) or once its
 * last write is old enough; for tests, the cache dropped and read again from it (how many it holds), and how many
 * entries were read from their files since the process started. */
void         SessionStore_SaveEntryCache(BOOL now);
int          SessionStore_ReadEntryCacheAgain(void);
LONG         SessionStore_FreshReads(void);
BOOL         SessionStore_WorkingDir(const SessionSet *set, const WCHAR *cwd, WCHAR *out, size_t cch);
BOOL         SessionStore_WatchDir(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionStore_PendingPath(const Profile *p, WCHAR *out, size_t cch);
int          SessionStore_LoadPending(const Profile *p, PendingEdit *edits, int capacity);   /* -1: unreadable */
const WCHAR *SessionStore_RowTitle(const SessionSet *set, const SessionRow *row);           /* "": untitled */
void         SessionStore_GroupName(const SessionSet *set, int group, WCHAR *out, size_t cch);

/* ---------------------------------------------------------- sessiontools.c */
BOOL SessionTools_OpenArtifact(HWND owner, const WCHAR *url, const WCHAR *title, const WCHAR *source);
void SessionTools_Details(HWND owner, const WCHAR *file, int page);
void SessionTools_Catalog(HWND owner, const Profile *profile, BOOL historical);
/* A reading copy's format; READING_ORIGINAL: the session's own files (one copied, several in a ZIP archive). */
enum { READING_TEXT, READING_MARKDOWN, READING_HTML, READING_JSON, READING_ORIGINAL };
void SessionTools_Export(HWND owner, const SessionSet *set, int row, int format);
BOOL SessionTools_PreviewPage(const SessionSet *set, int row, WCHAR *out, size_t cch);   /* the session as a web page, for the preview */
WCHAR        *SessionTools_Context(const WCHAR *path, ULONGLONG offset, const WCHAR *query, BOOL regex, BOOL matchCase);
BOOL SessionTools_ExportFile(const WCHAR *source, const WCHAR *target, int format, DWORD *messages, DWORD *omitted);   /* READING_* but ORIGINAL */

/* ---------------------------------------------------------- sessionedit.c */

typedef enum CopyResult { COPY_MADE, COPY_CANCELLED, COPY_FAILED } CopyResult;

/* `owner`: the window Windows' questions about the Recycle Bin belong to; NULL away from the manager. */
HRESULT      SessionEdit_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *sessionId);
BOOL         SessionEdit_Change(HWND owner, const Profile *p, const SessionEntry *entry, const PendingEdit *edit, BOOL *waiting,
                                WCHAR *error, size_t errorCch);
BOOL         SessionEdit_Cancel(const Profile *p, const PendingEdit *edits, int count);
void         SessionEdit_Forget(const Profile *p, const WCHAR *const *keys, int count);
int          SessionEdit_ApplyPending(HWND owner, const Profile *p);
int          SessionEdit_ApplyPendingAfterRun(const Profile *p);   /* its Claude has just closed */
void         SessionEdit_ApplyPendingFor(const WCHAR *folder);
CopyResult   SessionEdit_CopyConversation(HWND owner, const SessionSet *set, int row, int target, WCHAR *newId, size_t idCch,
                                          WCHAR *error, size_t errorCch);
BOOL         SessionEdit_RemoveCopy(const SessionSet *set, int row, int target, const WCHAR *copyId, WCHAR *left, size_t leftCch);
BOOL         SessionEdit_CanDelete(const SessionSet *set, int row, WCHAR *error, size_t errorCch);
BOOL         SessionEdit_RemovesWorkingFolder(const SessionSet *set, int row, WCHAR *physical, size_t cch);
RemoveResult SessionEdit_DeleteEverywhere(HWND owner, const SessionSet *set, int row, WCHAR *error, size_t errorCch);
BOOL         SessionEdit_ListFiles(const SessionSet *set, int row, WCHAR (**paths)[LONG_PATH_CCH], int *count,
                                   WCHAR *error, size_t errorCch);
BOOL         SessionEdit_ListConversation(const SessionSet *set, int row, WCHAR (**paths)[LONG_PATH_CCH], int *count,
                                          WCHAR *error, size_t errorCch);
BOOL         SessionEdit_ListTranscriptFiles(const WCHAR *const *ids, int idCount, WCHAR (**paths)[LONG_PATH_CCH], int *count,
                                             WCHAR *error, size_t errorCch);
BOOL         SessionEdit_TemporaryDir(WCHAR *out, size_t cch);
BOOL         SessionEdit_CopiedCwd(const WCHAR *copyId, WCHAR *cwd, size_t cch);
HANDLE       SessionEdit_Lock(const Profile *p);   /* the lock of a profile's entry changes; NULL when not taken */
void         SessionEdit_Unlock(HANDLE lock);

/* ------------------------------------------------------------ webstore.c */
/* Claude's web storage: the Local Storage its window keeps for claude.ai in
 * the profile's folder. Read at any time; written, as one batch, only while
 * that Claude is closed, its folder copied to `backup` first. Values are
 * UTF-8 text, keys the names the window gives them. */
typedef struct WebStore WebStore;
WebStore *WebStore_Open(const Profile *p);   /* NULL when it has none, or it cannot be read */
char     *WebStore_Get(const WebStore *store, const WCHAR *name, size_t *length);   /* a heap block; NULL when absent */
BOOL      WebStore_Set(WebStore *store, const WCHAR *name, const char *utf8, size_t length);   /* waits for the commit */
BOOL      WebStore_Commit(WebStore *store, const WCHAR *backup);
/* One record of claude.ai's IndexedDB in the profile's folder, a string under string key `key` of object store
   `objectStore` in database `database`: read, and set where Claude put one (WebStore_Commit writes it). */
WebStore *WebStore_OpenIndexed(const Profile *p, const WCHAR *database, const WCHAR *objectStore, const WCHAR *key);
char     *WebStore_GetIndexed(const WebStore *store, size_t *length);   /* UTF-8, a heap block; NULL when there is none */
BOOL      WebStore_SetIndexed(WebStore *store, const char *utf8, size_t length);   /* Latin-1 text only */
ULONGLONG WebStore_Written(const WebStore *store);   /* when its newest file was written, ms since 1970 */
void      WebStore_Free(WebStore *store);
/* Each value whose name starts with `prefix` (Latin-1 names only), in no order. */
void      WebStore_EachName(const WebStore *store, const WCHAR *prefix, void (*each)(void *context, const WCHAR *name), void *context);

/* ---------------------------------------------------------- sessionsync.c */

/* A change chosen elsewhere (sessionvault.c), for SessionSync_Send. */
typedef struct SyncSend {
    int         profile;   /* in the set */
    SyncOp      op;        /* its kind, flags, key, time and seen; content is filled in */
    const char *content;   /* put, index: what is written (the caller keeps it until the send returns) */
    size_t      length;
} SyncSend;

DWORD        SessionSync_Takers(const SessionSet *set);   /* the profiles whose entries can take sessions */
/* The pins, groups and settings of the sidebar of `p` (its
 * claude_desktop_config.json and its web storage), for the account and
 * organization of `entriesDir`, as a layout of ours: a heap block (HeapFree
 * it), NULL when it has none. `written`: when they were last written, in ms
 * since 1970. Its member LAYOUT_CANNOT_TAKE (an object, each member a part's
 * name) names the parts the profile could not take as sent: one of a value
 * Claude has not made there, or of a file it has not. */
#define LAYOUT_CANNOT_TAKE "cannotTake"
char        *SessionSync_ReadLayout(const Profile *p, const WCHAR *entriesDir, size_t *length, ULONGLONG *written);
/* A layout (SessionSync_ReadLayout) with only the parts whose item is in
 * `items` (SYNC_ITEM_*): the others removed. A heap block (HeapFree it),
 * NULL without memory. */
char        *SessionSync_FilterLayout(const char *layout, size_t length, DWORD items, size_t *outLength);
/* A layout applied to a closed profile `p` (whose entries are in
 * `entriesDir`), member by member as a sync would: TRUE when it was made or
 * was already so. For a backup's restore, so the profile stays closed. */
BOOL         SessionSync_RestoreLayout(const Profile *p, const WCHAR *entriesDir, const char *layout, size_t length, SyncReport *report);
/* Part `index` of a layout: its name, what it belongs to (SYNC_ITEM_*), and
 * whether it is made the same member by member (each session's own); FALSE
 * past the last. */
typedef struct LayoutPart {
    const char  *name;
    const WCHAR *label;   /* as the person sees it (a catalog key), NULL for none */
    DWORD        item;
    BOOL         byKey;
    BOOL         ownOnly;   /* written only over a value of the profile's own: one that shows none cannot take it */
    BOOL         mergeOnly; /* merged into what each profile has, never taken away nor in conflict (SessionSync_MergePart) */
} LayoutPart;
/* A merge-only part's `other` merged into `base`: a heap block; NULL for another part or no memory. */
char        *SessionSync_MergePart(const LayoutPart *part, const char *base, size_t baseLength, const char *other, size_t otherLength,
                                   size_t *outLength);
/* `json` (a heap block, freed) with its member `key` set to `raw` (`length`
 * bytes); NULL, freed, when it cannot be. */
char        *SessionSync_SetMember(char *json, size_t *used, const char *key, const char *raw, size_t length);
BOOL         SessionSync_LayoutPart(int index, LayoutPart *part);
/* A member showing `shown` of a part (NULL: none) in its `layout` already
 * holds what `value`, sent to it, would leave there. */
BOOL         SessionSync_PartHolds(const LayoutPart *part, const char *layout, size_t layoutLength, const char *shown, size_t shownLength,
                                   const char *value, size_t valueLength);
/* From now on, each change this thread makes in a profile calls `step` (a
 * progress bar's); NULL for none. */
void         SessionSync_OnEachChange(void (*step)(void *context), void *context);
/* From now on, each session entry this thread sends to a profile (put, or
 * taken away) is told to `sent` once it is in that profile's plan, before it
 * is made: `content` is the entry put, NULL for a removal. NULL for none. */
typedef void (*SyncSentProc)(void *context, const Profile *p, const SyncOp *op, const char *content, size_t length);
void         SessionSync_OnEachSent(SyncSentProc sent, void *context);
/* `changes` made as the other sends are: at once in a closed profile, else
 * waiting for it to close; backed up first, each checked again then. */
BOOL         SessionSync_Send(const SessionSet *set, const SyncSend *changes, int count, SyncReport *report);
BOOL         SessionSync_Merge(const SessionSet *set, DWORD profiles, SyncReport *report);
/* The sessions of `rows` taken out of `profiles`, Claude's marks left so
 * that it does not take them in again: their conversations stay. */
BOOL         SessionSync_Remove(const SessionSet *set, DWORD profiles, const int *rows, int rowCount, SyncReport *report);
BOOL         SessionSync_Share(const SessionSet *set, int from, const int *rows, int rowCount, DWORD targets, SyncReport *report);
CopyResult   SessionSync_Copy(HWND owner, const SessionSet *set, int from, const int *rows, int rowCount, DWORD targets,
                              SyncReport *report);
int          SessionSync_ApplyPending(const Profile *p, SyncReport *report);   /* report may be NULL */
int          SessionSync_PendingCount(const Profile *p);
BOOL         SessionSync_SetUnread(const Profile *p, const WCHAR *const *ids, int count, BOOL unread);   /* closed profile's web storage */
BOOL         SessionSync_Forget(const Profile *p, const WCHAR *key);   /* the changes waiting for `p` on that session dropped */
int          SessionSync_Queued(const Profile *p, SyncOp **ops);   /* its changes waiting, a heap block in *ops; -1 unreadable */
WCHAR       *SessionSync_PendingText(const ProfileList *profiles); /* read-only, HeapFree */
void         SyncUi_Pending(HWND owner, const ProfileList *profiles);
BOOL         SessionSync_PlanPath(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionSync_Export(const SessionSet *set, int profile, const int *rows, int rowCount, const WCHAR *archive,
                                int *exported, WCHAR *error, size_t errorCch);
BOOL         SessionSync_IsArchive(const WCHAR *archive);   /* a session archive this version reads */
BOOL         SessionSync_Import(const SessionSet *set, const WCHAR *archive, DWORD targets, int *sessions, SyncReport *report);
/* An archive's sessions, each its key (its transcript's id, or its entry's own), title and last activity: a heap
 * block (Util_Free), their count; -1 when it is no archive of ours or cannot be read. */
typedef struct ArchivedSession {
    WCHAR     key[SESSION_ID_CCH];
    WCHAR     own[SESSION_ID_CCH];   /* its entry's own id (local_<id>), as Claude's settings name it; "" unknown */
    WCHAR     title[SESSION_TITLE_CCH];
    ULONGLONG lastActivity;
} ArchivedSession;
int          SessionSync_ArchiveSessions(const ZipIn *zip, ArchivedSession **out);
/* Some of an archive's sessions, by key: those imported, and only their conversations' files (an archive of an
 * earlier version, which does not say which are whose, gives them all). NULL `keys`: every one. */
typedef struct SessionPick {
    const WCHAR (*keys)[SESSION_ID_CCH];
    int          count;
} SessionPick;
BOOL         SessionSync_ImportPicked(const SessionSet *set, const WCHAR *archive, DWORD targets, const SessionPick *pick, int *sessions,
                                      SyncReport *report);

/* --------------------------------------------------------- sessionvault.c */
/* Every list of sessions kept out of Claude's reach, and the profiles whose
 * sessions are kept the same: those of one group (Profile.syncGroup) share a
 * list, VAULT_GROUP_LIST for group 1 and VAULT_GROUP_LIST-<n> for the others;
 * a profile alone keeps one named after its folder. */

#define VAULT_GROUP_LIST L"group"
#define VAULT_VERSIONS_SHOWN 200

typedef struct VaultVersion {
    WCHAR     name[40];          /* its file's name */
    ULONGLONG time;              /* when it was kept, ms since 1970 */
    int       sessions;          /* how many it lists */
} VaultVersion;

typedef struct VaultIds {        /* session ids, each once */
    WCHAR (*ids)[SESSION_ID_CCH];
    int    count, capacity;
    BOOL   failed;              /* an incomplete collection must not authorize deletion */
} VaultIds;

/* How far a sync is: `done` of `total` steps; the total grows as the work is known. */
typedef void (*SyncProgress)(void *context, int done, int total);

DWORD SessionVault_Group(const ProfileList *list, int group);   /* its profiles, one bit each; none for group 0 */
int   SessionVault_NewGroup(const ProfileList *list);           /* the lowest group no profile is in; 0 when there is none */
DWORD SessionVault_GroupItems(const ProfileList *list, DWORD members);   /* what all of `members` keep the same (SYNC_ITEM_*) */
/* A part of the sidebar two profiles of a group changed each their own way
 * since the last sync: it stays as each has it until the person chooses. */
#define VAULT_SHOWN_CCH 160
typedef struct VaultConflict {
    char      part[64];      /* the layout's part (SessionSync_LayoutPart) */
    char      path[CORE_MERGE_PATH_CCH];   /* the element in it (Core_JsonMerge) */
    DWORD     item;          /* what it belongs to (SYNC_ITEM_*) */
    DWORD     members;       /* the profiles of the list that changed it each their own way, one bit each */
    int       latest;        /* the one of them that changed it last, -1 not known */
    WCHAR     label[VAULT_SHOWN_CCH];                      /* the element as shown */
    WCHAR     shown[MAX_PROFILES][VAULT_SHOWN_CCH];        /* each member's version as shown, by profile */
} VaultConflict;
int   SessionVault_Conflicts(const ProfileList *list, int group, VaultConflict *out, int capacity);
/* The person's choices: element `chosen[i]` takes the version of profile
 * `profiles[i]` (an index of `list`); the next sync makes them. */
BOOL  SessionVault_Decide(const ProfileList *list, int group, const VaultConflict *chosen, const int *profiles, int count);
BOOL  SessionVault_GroupListName(int group, WCHAR *out, size_t cch);
BOOL  SessionVault_ListName(const ProfileList *list, int index, WCHAR *out, size_t cch);
/* `profiles`' list kept as `listName`; with `same`, each of them made to list
 * the same sessions (changes made since the last version win, the latest
 * first; a deletion goes everywhere; a profile that lost the list gets it; a
 * session two of them went on with apart is kept as two), and the same pins
 * and groups in Claude's sidebar. */
BOOL  SessionVault_Keep(const ProfileList *list, DWORD profiles, const WCHAR *listName, BOOL same, SyncReport *report);
/* The groups of `profiles` made the same, and each of them in no group kept
 * when it is closed; `progress` (may be NULL) follows it. */
BOOL  SessionVault_KeepGroups(const ProfileList *list, DWORD profiles, SyncProgress progress, void *context, SyncReport *report);
void  SessionVault_KeepAll(const ProfileList *list);   /* every group's, and every closed profile's alone */
void  SessionVault_BeforeOpen(const ProfileList *list, int index);
void  SessionVault_AfterClose(const WCHAR *folder);   /* the watcher's call each time its profile's Claude closed */
/* An entry as profile `target` of `set` gets it: one working in another
 * profile's "no folder" area works in the target's own, under the same
 * folder name (made, with the files it lacks). A heap block; NULL when the
 * entry goes as it is. */
char *SessionVault_ForProfile(const SessionSet *set, int target, const char *content, size_t length, size_t *outLength);
int   SessionVault_Versions(const WCHAR *listName, VaultVersion *out, int capacity);   /* newest first */
BOOL  SessionVault_Restore(const ProfileList *list, int index, const WCHAR *listName, const WCHAR *version, SyncReport *report);
/* The ids of the sessions some kept list names (their transcripts too), and
 * of those deleted, which only undeleting them brings back. */
BOOL  SessionVault_Ids(VaultIds *listed, VaultIds *deleted);
BOOL  SessionVault_HasId(const VaultIds *ids, const WCHAR *id);
void  SessionVault_FreeIds(VaultIds *ids);
BOOL  SessionVault_AddDeleted(const WCHAR *const *ids, int count);
/* The keys of the sessions any version of any kept list names. */
BOOL  SessionVault_ListedIn(const ProfileList *list, int index, VaultIds *keys);   /* the keys profile `index`'s kept lists had */
/* Deleted sessions (by key) put back in each profile whose kept list had them,
 * as that list last had them, at once where it is closed, else once it closes;
 * they leave the deleted ones. Returns how many came back, -1 on a failure. */
int   SessionVault_Undelete(const ProfileList *list, const WCHAR *const *keys, int count, SyncReport *report);

/* --------------------------------------------------------- sessionpurge.c */
/* The conversations of sessions no profile lists any more, deleted for good
 * from Claude Code's folder or put back in the lists; and that folder copied whole. */

typedef enum PurgeKind { PURGE_DELETED, PURGE_UNKNOWN } PurgeKind;

typedef struct PurgeItem {
    WCHAR     id[SESSION_ID_CCH];
    WCHAR     path[LONG_PATH_CCH];     /* its transcript */
    WCHAR     title[SESSION_TITLE_CCH];
    WCHAR     project[MAX_PATH];       /* its working folder, as the transcript names it */
    PurgeKind kind;                    /* deleted in Claude, or no list ever named it */
    BOOL      restorable;              /* a kept list had it (SessionVault_Undelete) */
    DWORD     profiles;                /* the profiles whose kept lists had it, one bit each */
    ULONGLONG bytes, written;          /* written: ms since 1970 */
} PurgeItem;

#define PURGE_RECENT_HOURS 24   /* an unknown conversation written since is left alone: it may be in use */

int          SessionPurge_List(const ProfileList *profiles, PurgeItem **items, WCHAR *error, size_t errorCch);   /* HeapFree the array */
RemoveResult SessionPurge_Delete(HWND owner, const ProfileList *profiles, const PurgeItem *items, const int *chosen, int count,
                                 int *deleted, WCHAR *error, size_t errorCch);
BOOL         SessionPurge_CodeFolder(WCHAR *out, size_t cch);
BOOL         SessionPurge_BackupName(WCHAR *out, size_t cch);   /* the folder a copy of it made today goes to */
CopyResult   SessionPurge_BackUp(HWND owner, const WCHAR *to, DWORD *error);   /* Windows' progress shown only with an owner */
/* With no Claude of `profiles` running and no copy of Claude Code's folder made
 * in the last CORE_WEEKLY_COPY_DAYS days, one made beside it, silently; the
 * weekly copies beyond the two latest removed. */
void         SessionPurge_WeeklyBackUp(const ProfileList *profiles);

/* ---------------------------------------------------------------- syncui.c */

/* What sending sessions did, said; `first` (may be NULL) opens it. */
void SyncUi_ShowReport(HWND owner, const ProfileList *profiles, const SyncReport *report, const WCHAR *first);
BOOL SyncUi_Merge(HWND owner, const ProfileList *profiles);
/* Every session of `sources` listed by the profiles chosen too; with `move`,
 * then taken out of `sources` (none of them keeping its sessions the same). */
BOOL SyncUi_CopyAll(HWND owner, const ProfileList *profiles, DWORD sources, BOOL move);
BOOL SyncUi_ShareOrCopy(HWND owner, const SessionSet *set, int from, const int *rows, int rowCount, BOOL copy, int to);   /* to: -1 to choose */
BOOL SyncUi_Export(HWND owner, const SessionSet *set, int profile, const int *rows, int rowCount);
BOOL SyncUi_ExportProfiles(HWND owner, const ProfileList *profiles, DWORD chosen);
BOOL SyncUi_Import(HWND owner, const ProfileList *profiles, DWORD chosen);
BOOL SyncUi_ImportFile(HWND owner, const ProfileList *profiles, DWORD chosen, const WCHAR *file);   /* the session archive `file` (NULL: chosen) */
/* ------------------------------------------------------------ transfer.c */

typedef enum TransferResult { TRANSFER_DONE, TRANSFER_STOPPED, TRANSFER_FAILED } TransferResult;
/* A copy's progress, `done` bytes of `total`: FALSE stops it (left for later, or cancelled). */
typedef BOOL (*TransferStepProc)(void *context, ULONGLONG done, ULONGLONG total);
/* `source` copied to `target` by way of "<target>.part", hidden, from what that part holds already when it is the
 * file's beginning; named `target` once whole. */
TransferResult Transfer_Copy(const WCHAR *source, const WCHAR *target, TransferStepProc step, void *context, DWORD *code);
typedef struct TransferItem {
    WCHAR name[MAX_PATH];          /* its file's name */
    WCHAR folder[LONG_PATH_CCH];   /* where it goes */
    WCHAR target[LONG_PATH_CCH];   /* its path there, a free name, once its copy begins */
    WCHAR dir[MAX_PATH];           /* its own folder in the state folder's transfers\, its file and what it is there */
    int   data;                    /* the caller's: what `make` makes */
} TransferItem;
/* Item `item`'s file made at `source`; FALSE, `error` saying why, when it cannot be. Called on the copy's thread. */
typedef BOOL (*TransferMakeProc)(void *context, const TransferItem *item, const WCHAR *source, WCHAR *error, size_t cch);
/* `items` made (`make`, NULL: made already) and copied where they go, in a window that shows how far, pauses, leaves
 * them for later or cancels them: TRUE once all are there. */
BOOL Transfer_Run(HWND owner, const WCHAR *title, TransferItem *items, int count, TransferMakeProc make, void *context);
int  Transfer_Left(TransferItem *items, int cap);   /* what a close or an error left, made, to go on with */
void Transfer_ResumeLeft(HWND owner);               /* those offered again: gone on with from where they stopped, or dropped */

/* dragout.c: the profiles of `profiles` dragged out of the list `source` (from `start`, its client), as their backups.
 * Once the shell tells where they were let go, `owner` gets `message`, which it answers with DragOut_Dropped: what they
 * hold chosen, then they are written there. */
typedef enum DragZone { DRAG_ZONE_EXPORT, DRAG_ZONE_COPY, DRAG_ZONE_SIGNIN, DRAG_ZONE_DELETE, DRAG_ZONES } DragZone;
/* While they are dragged, tiles over the list's foot take them too: let go on one, `owner` gets `zoneMessage`,
 * wParam the DragZone, and does what it says (an export: DragOut_Export, what they hold chosen, then where). */
void DragOut_Profiles(HWND owner, UINT message, UINT zoneMessage, HWND source, const ProfileList *list, DWORD profiles, POINT start);
void DragOut_Export(HWND owner, const ProfileList *list, DWORD profiles);
void DragOut_Dropped(HWND owner, LPARAM lp);
BOOL DragOut_IsMarker(const WCHAR *path);   /* the file a drag carries to find where it goes: no backup */
/* Files dragged onto `window`, taken by an OLE drop target: one of them a drag's marker, it is refused, so that a drag
 * of ours let go there is cancelled. Those dropped come to `window` as `message`, lParam a DroppedFiles (`at` on the
 * screen), which it frees (Util_Free). DragOut_RefuseFiles revokes it before the window goes. */
#define DROPPED_FILES_MAX 16
typedef struct DroppedFiles {
    POINT at;
    int   count;
    WCHAR paths[DROPPED_FILES_MAX][LONG_PATH_CCH];
} DroppedFiles;
struct IDropTarget;
struct IDropTarget *DragOut_FileTarget(HWND window, UINT message);   /* the target itself, unregistered */
BOOL DragOut_AcceptFiles(HWND window, UINT message);
void DragOut_RefuseFiles(HWND window);
/* Where a drag goes: its marker made (its path in `marker`) and the shell's notices watched for a copy of it; the
 * folder that copy is in once it came (`ms` at most, the thread's messages handled), the copy taken away. */
typedef struct DragDrop DragDrop;
DragDrop *DragOut_Begin(WCHAR *marker, size_t cch);
BOOL      DragOut_Wait(DragDrop *drop, DWORD ms, WCHAR *folder, size_t cch);
void      DragOut_End(DragDrop *drop);
BOOL SyncUi_Restore(HWND owner, const ProfileList *profiles, const WCHAR *selected);
/* What `members` keep the same, chosen in a dialog (IDD_SYNC_ITEMS) from `*items`: FALSE when cancelled. */
BOOL SyncUi_ChooseItems(HWND owner, const ProfileList *profiles, DWORD members, DWORD *items);
/* What to sync, as a list whose groups fold (Theme_SetFoldingRows), `items` checked: each group, its items under it. */
void SyncUi_FillItemsList(HWND list, DWORD items);

/* What a profile syncs, and with which, chosen in a dialog (IDD_SYNC_SETUP). */
typedef struct SyncSetup {
    int   profile;    /* the profile set up */
    DWORD partners;   /* the profiles it syncs with */
    DWORD items;      /* what they sync (SYNC_ITEM_*) */
    BOOL  syncNow;    /* closed with Sync now */
} SyncSetup;
/* `setup->profile` given, the rest filled from its group and the choice made:
 * FALSE when cancelled. */
BOOL SyncUi_Setup(HWND owner, const ProfileList *profiles, SyncSetup *setup);
void SyncUi_PartnerBoxes(HWND dialog, const WCHAR *const *names, const BOOL *checked, int count);   /* Sync settings' row of profiles */
/* The conflicts syncs left to the person (SessionVault_Conflicts), one
 * dialog per group (IDD_CONFLICTS): the profiles of the groups settled. */
DWORD SyncUi_SettleConflicts(HWND owner, const ProfileList *profiles);
BOOL SyncUi_Purge(HWND owner, const ProfileList *profiles);
BOOL SyncUi_BackUpCode(HWND owner);
/* A change to data, asked first in a sentence or two: `question`, and
 * that what it replaces is backed up first when `backedUp`. TRUE to go on. */
BOOL SyncUi_Confirm(HWND owner, LPCWSTR icon, const WCHAR *question, BOOL backedUp, const WCHAR *button);
BOOL SyncUi_ConfirmSessions(HWND owner, const WCHAR *question, const WCHAR *button);   /* sessions written: SyncUi_Confirm, backed up first */

/* ------------------------------------------------------- sessionsearch.c */

#define SESSION_SEARCH_CCH 128   /* what the sessions view's search box takes */
/* Looks for `text` (a regular expression with `regex`) in the conversations
 * of `paths` ("" for none), on a thread of its own, posting each one found
 * to `window` (WM_APP_SESSION_SEARCH); it frees `paths`. A search under way
 * stops first. FALSE when it cannot start (a pattern it does not take). */
BOOL SessionSearch_Start(HWND window, ULONG generation, const WCHAR *text, BOOL regex, BOOL matchCase,
                         WCHAR (*paths)[LONG_PATH_CCH], int count);
BOOL SessionSearch_Locate(const WCHAR *path, const WCHAR *text, BOOL regex, BOOL matchCase, ULONGLONG *offset, size_t *messageEnd);
void SessionSearch_Stop(void);   /* the search under way, stopped and gone */
void SessionSearch_Invalidate(void);   /* a watched file changed; cached matches are no longer current */

/* -------------------------------------------------------- sessionundo.c */
/* What the manager changes in sessions, undone and redone (Ctrl+Z, Ctrl+Y in
 * the sessions view) while it runs. Between SessionUndo_Begin and
 * SessionUndo_End the changes make one action: stars, titles and entries
 * removed or kept, told by the caller, and every entry sessionsync.c sends
 * meanwhile, which it sees itself. `set`, the sessions as the caller sees
 * them, names the entries those replace or take away. */
void SessionUndo_Begin(const SessionSet *set);
void SessionUndo_End(void);
void SessionUndo_Star(const Profile *p, const WCHAR *key, BOOL before, BOOL after);
void SessionUndo_Read(const Profile *p, const WCHAR *key, BOOL unreadBefore, BOOL unreadAfter);
void SessionUndo_Title(const Profile *p, const WCHAR *key, const WCHAR *before, BOOL beforeAuto, const WCHAR *after);
/* Session `key` listed by `p` from now on (`listed`), or taken away;
 * `content`: its entry, as it is or as it was (a heap block the undo keeps,
 * NULL when it could not be read). */
void SessionUndo_Listed(const Profile *p, const WCHAR *key, BOOL listed, char *content, size_t length);
/* An entry's content changed in place, both sides listed (a conversion to a
 * local session): undo writes `before`, redo `after`; both heap blocks are
 * taken over. */
void SessionUndo_Replaced(const Profile *p, const WCHAR *key, char *before, size_t beforeLength, char *after, size_t afterLength);
BOOL SessionUndo_CanUndo(void);
BOOL SessionUndo_CanRedo(void);
/* The latest action undone, or the latest one undone made again, through the
 * profiles of `set`: each change at once where the profile is closed, else
 * once it closes. FALSE, `error` saying why when it can, when not all of it
 * could be; it moves to the other list either way. */
BOOL SessionUndo_Undo(HWND owner, const SessionSet *set, WCHAR *error, size_t errorCch);
BOOL SessionUndo_Redo(HWND owner, const SessionSet *set, WCHAR *error, size_t errorCch);
void SessionUndo_Forget(const WCHAR *key);   /* a session deleted everywhere: what names it is undone no more */
void SessionUndo_Clear(void);

/* ------------------------------------------------------------ settings.c */

/* The language and the windows' size above, every other setting by its kind
 * in a list beside: the program's own, what new profiles are made with,
 * Claude Code's (its settings file, every profile's) and Claude's (each
 * profile's preferences), each with what it reaches and how much it changes.
 * Apply and OK save what changed and give a new language (-1: Windows') to
 * `applyLanguage`, which puts it in use; the dialog then follows it. */
typedef void (*SettingsLanguageProc)(int language);
/* One of Settings' Claude Code actions (IDC_G_PURGE, IDC_G_CODE_COPY), its dialog `owner`; IDC_G_ZOOM: the
 * windows' size changed (saved), which the manager's window takes once it opens again. */
typedef void (*SettingsActionProc)(HWND owner, int action);
void Settings_Show(HWND owner, SettingsLanguageProc applyLanguage, SettingsActionProc act);
/* The page's `index`-th setting, as it shows it: its name, and its value (a switch's "1" or "0"); FALSE past the last. */
BOOL Settings_PageRow(HWND page, int index, WCHAR *title, size_t titleCch, WCHAR *shown, size_t shownCch);
/* A change of Claude's preferences that Settings makes in every profile (Gui_SetPreferences): `raw` the JSON value,
 * NULL for Claude's default. */
typedef struct PreferenceChange { const char *key; const char *raw; } PreferenceChange;
/* The profiles the manager shows (gui.c), and `changes` made in each of them: an open one is closed for it and
 * opened again, asked first. FALSE when nothing was changed (cancelled, or the program busy); one that could not be
 * made is said. */
const ProfileList *Gui_Profiles(void);
BOOL Gui_SetPreferences(HWND owner, const PreferenceChange *changes, int count);
void About_Show(HWND owner);
void About_Fill(HWND dialog);   /* IDD_ABOUT: the version, help's questions and the changelog's versions by month, the newest shown */

/* --------------------------------------------------------------- shield.c */

/* The shield watches Claude's exit IP and the network it goes through, and
 * ends Claude when a rule says the exit is not safe (shield.c). The manager
 * keeps it running while its window is open. */
#define WM_APP_SHIELD (WM_APP + 25)   /* to the manager: the shield checked again, wParam its ShieldState */
typedef enum ShieldState {
    SHIELD_OFF,        /* the user turned it off */
    SHIELD_CHECKING,   /* a check is under way, none finished yet */
    SHIELD_SAFE,       /* nothing a rule or the defaults object to */
    SHIELD_CONCERN,    /* something is not ideal, but no rule stops Claude */
    SHIELD_STOPPED,    /* a rule's condition is met: Claude is kept ended */
    SHIELD_STATES
} ShieldState;
typedef struct ShieldSnapshot {
    ShieldState state;
    ULONGLONG   checkedMs;             /* when the last check finished (Util_NowMs), 0: none */
    BOOL        exitKnown;
    WCHAR       exitIp[64], exitLoc[8];   /* Claude's exit IP and the country Cloudflare gives it */
    CoreIpRisk  risk;                  /* what an IP service said of the exit, risk.source the one that answered */
    BOOL        riskKnown;
    int         tunUp, proxyApp, udpLeak, dnsLeak, ipv6Leak;   /* three-way facts */
    int         tzMismatch, langMismatch, cfSplit;
    WCHAR       udpIp[64], ipv6[64];   /* the UDP exit a STUN server saw, and the public IPv6 that could leak; "" when none */
    WCHAR       cfIp[64], cfLoc[8];    /* the default (Cloudflare) exit, for the split check */
    int         latencyClaude, latencyAnthropic;   /* ms to each, -1 when it did not answer */
    WCHAR       statusText[64];        /* Anthropic's service status, "" when not read */
    DWORD       stop, concerns;        /* SHIELD_STOP_* from the verdict and from the defaults */
    int         endedDesktop, endedCode;   /* processes the last stop ended */
} ShieldSnapshot;
/* One line of the shield's log: when, what state it reached, and why. */
typedef struct ShieldEvent { ULONGLONG whenMs; ShieldState state; DWORD reasons; int ended; } ShieldEvent;
#define SHIELD_EVENTS_MAX 32
void  Shield_Start(HWND manager, const ClaudePackage *pkg);   /* the window the check posts WM_APP_SHIELD to */
void  Shield_Stop(void);
void  Shield_Recheck(void);                 /* check again now when enabled (an event, a rule change) */
void  Shield_CheckNow(void);                /* the "check now" button: read the exit and leaks once even while off */
void  Shield_SetEnabled(BOOL on);           /* the shield on or off, kept */
BOOL  Shield_IsEnabled(void);
void  Shield_Snapshot(ShieldSnapshot *out); /* the latest, for the dialog */
void  Shield_GetRules(CoreShieldRules *rules);
void  Shield_SetRules(const CoreShieldRules *rules);   /* kept, and a check run with them */
BOOL  Shield_Blocking(void);                /* SHIELD_STOPPED now: the manager opens no profile, routes no link */
/* Of the profiles `bits` of `list`, those whose own regions leave out Claude's exit (its code in `exit`), while the
 * shield is on and does more than warn. */
DWORD Shield_ProfilesBlocked(const ProfileList *list, DWORD bits, WCHAR *exit, size_t cch);
int   Shield_Events(ShieldEvent *out, int max);   /* the log, newest first; returns how many */
void  Shield_GetProfileRegions(const WCHAR *folder, WCHAR *out, size_t cch);   /* the codes a profile's exit must be in, "" for any */
void  Shield_SetProfileRegions(const WCHAR *folder, const WCHAR *codes);       /* kept, and a check run */
BOOL  Shield_QuickBlock(const ClaudePackage *pkg, const Profile *profile);   /* a one-shot check for the router: TRUE when the exit is unsafe for `profile` */
void  Shield_SetupButton(HWND dialog, int id);   /* the badge made an icon button, raised over the corner, its state shown */
void  Shield_ShowState(HWND dialog, int id, ShieldState state);   /* the badge's glyph, color and tip for `state` */
void  Shield_ShowDialog(HWND owner);        /* the shield's dialog */
const WCHAR *Shield_ReasonText(DWORD reason);   /* a short line for one SHIELD_STOP_ or SHIELD_CONCERN_ bit */

/* ------------------------------------------------------------- sessions.c */

#define WM_APP_SESSIONS (WM_APP + 12)   /* to the manager: session entries or transcripts changed on disk */
#define WM_APP_SESSIONS_READY (WM_APP + 13) /* background session snapshot ready for the window */
#define WM_APP_SYNC_PROGRESS (WM_APP + 20)  /* to the manager, from any process: wParam steps done of lParam; 0 of 0 once done */
#define WM_APP_SYNC_CONFLICTS (WM_APP + 21) /* to the manager, from any process: a sync left conflicts to the person */
#define WM_APP_SIGNIN_DONE (WM_APP + 23)   /* to the private sign-in dialog: wParam TRUE once its window handed Claude the link; lParam an HRESULT */
#define WM_APP_SESSION_SEARCH (WM_APP + 22) /* to the manager: wParam search found lParam's session (-1: done; sessionsearch.c) */

void         SessionsView_Init(HWND dlg);
void         SessionsView_Warm(const ProfileList *profiles);
void         SessionsView_SetProfiles(const ProfileList *profiles);
void         SessionsView_Ready(BOOL allowChanges);
BOOL         SessionsView_SearchFound(WPARAM search, LPARAM index);   /* WM_APP_SESSION_SEARCH; FALSE for stale messages */
void         SessionsView_SearchAll(void);
void         SessionsView_Enter(const ClaudePackage *pkg, const WCHAR *folder);   /* pkg: the manager's, kept current, read at each use */
void         SessionsView_Leave(void);
BOOL         SessionsView_Shown(void);
const WCHAR *SessionsView_Profile(void);
void         SessionsView_Reload(void);
void         SessionsView_Notified(void);   /* WM_APP_SESSIONS */
/* The folder watch stopped while profile folders are moved or removed, and
 * started again after (calls pair up). */
void         SessionsView_PauseWatching(void);
void         SessionsView_ResumeWatching(void);
BOOL         SessionsView_Command(WPARAM wp);
BOOL         SessionsView_ClearSearch(void);
void         SessionsView_ToggleSearch(void);   /* Ctrl+F: its search box shown and gone into, or hidden and emptied */
BOOL         SessionsView_Notify(const NMHDR *header, LRESULT *result);
BOOL         SessionsView_ContextMenu(HWND from, LPARAM pos);
/* WM_CONTEXTMENU from the profiles' list: the row under `pos` (from the keyboard, the one selected) shown, as a
 * click shows it. 1: a profile's row, whose menu the manager shows; 0: the list, below its rows; -1: not the list. */
int          SessionsView_ChooseProfileAt(HWND from, LPARAM pos);
/* Sessions in the view can be dragged: within it onto another profile (copied, or moved with Shift), out to a
 * folder (exported), and session archives dragged in onto a profile (imported). The side bar's own drop target
 * takes them; `dropMessage` reaches the window once a drop is over (SessionsView_HandleDrop does the work). */
void         SessionsView_AcceptDrops(UINT dropMessage);
void         SessionsView_HandleDrop(WPARAM wp, LPARAM lp);
BOOL         SessionsView_DrawItem(const DRAWITEMSTRUCT *item);
void         SessionsView_Relayout(void);
void         SessionsView_Destroy(void);

/* ----------------------------------------------------------------- tray.c */

/* Where Windows keeps the light or dark choice of apps and of the taskbar. */
#define REG_PERSONALIZE L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"

BOOL Tray_ClaudeImagePath(const ClaudePackage *pkg, BOOL lightApps, WCHAR *out, size_t cch);
BOOL Tray_IsHost(HWND window);
void Tray_Apply(const ClaudePackage *pkg, const Profile *profile);
void Tray_GiveBack(const ClaudePackage *pkg, DWORD pid);

/* -------------------------------------------------------------- install.c */

BOOL Install_IsInstalledCopy(void);
BOOL Install_IsRegistered(void);
BOOL Install_Run(BOOL openManager);
void Install_Repair(void);
void Install_ApplyLanguage(const ClaudePackage *pkg, const ProfileList *list);
BOOL Install_Uninstall(HWND owner, const ProfileList *list, const BOOL *removeData);
void Install_FinishUninstall(void);

/* --------------------------------------------------------------- theme.c */
/* The look of every window, in one place: Theme_Apply on a dialog themes its
 * controls (buttons, check boxes, lists and their headers, edits, combo boxes,
 * frames, focus rectangles, smooth wheel scrolling); what a window draws
 * itself takes its colors, fonts and rows from here. Also: fitting a dialog
 * to its captions in every language (Theme_FitDialog), the manager window's
 * layout (Theme_MainMinimum, Theme_LayoutMain), every dialog's shell
 * (Ui_Dialog) and the themed message box (Ui_Ask, Ui_Message). */

typedef enum ThemeColor {
    THEME_TEXT,
    THEME_MUTED,        /* secondary text */
    THEME_FACE,         /* window background */
    THEME_FIELD,        /* lists, trees, edits */
    THEME_MAIN_BLUE,    /* a selected row, selected text, a button's frame */
    THEME_BRIGHT_BLUE,  /* a selected row's frame; a button under the mouse or pressed */
    THEME_PALE_BLUE,    /* a row or a button under the mouse */
    THEME_SEPARATOR,    /* a line between two parts of a window */
    THEME_EDGE,         /* a menu's frame, a rule across a list: a quarter of the way from the field to the text */
    THEME_COLORS
} ThemeColor;

#define THEME_ROW_SELECTED 0x1
#define THEME_ROW_HOT      0x2
#define THEME_ROW_PLAIN    0x4   /* a side list's row: a gray, not the blues, selected or under the mouse */

/* Geometry the theme draws and places with, which the tests check too. */
#define THEME_CORNER_RADIUS_DIPS      4       /* buttons, selected rows, views and edits */
#define THEME_VIEW_MAX_PX             30000   /* a control taller than this scrolls itself in its view */
#define THEME_MAIN_MARGIN_DIPS        15      /* around the manager window's content */
#define THEME_MAIN_GAP_DIPS           9       /* between its neighboring buttons */
#define THEME_MAIN_READING_WIDTH_DIPS 1200    /* a wider manager window centers its content */
#define THEME_PROFILE_COLUMN_DIPS     180     /* the profile list's first column */
#define THEME_SWITCH_HEIGHT_DIPS      20      /* an on-off switch (Theme_DrawSwitch), nine fifths as wide */
#define THEME_FIELD_PADDING_DIPS      8       /* inside a field's frame, before its text (Theme_DrawField) */

/* A drop-down list item's data (CB_SETITEMDATA) can show a color swatch
 * beside its label, in the closed box and in its menu: the color in the low
 * 24 bits, with THEME_CHOICE_SWATCH; THEME_CHOICE_SEPARATED draws a line
 * above the item in the menu. */
#define THEME_CHOICE_SWATCH    0x01000000
#define THEME_CHOICE_SEPARATED 0x02000000
/* ...or THEME_CHOICE_ICON with an index in the low 16 bits: the icon at that index of the array Theme_SetChoiceIcons
 * gave the drop-down (the caller's, kept while the drop-down lives), before its label. */
#define THEME_CHOICE_ICON      0x04000000
void     Theme_SetChoiceIcons(HWND combo, const HICON *icons, int count);

#define THEME_BUTTON_HOT      0x1
#define THEME_BUTTON_PRESSED  0x2
#define THEME_BUTTON_DISABLED 0x4
#define THEME_BUTTON_DEFAULT  0x8

typedef enum ThemeFont {
    THEME_FONT_TEXT,
    THEME_FONT_STRONG,   /* semibold */
    THEME_FONT_HEADING,  /* semibold, larger */
    THEME_FONT_CURRENT,  /* semibold, underlined */
    THEME_FONT_ABSENT,   /* struck out */
    THEME_FONT_ITALIC,
    THEME_FONTS
} ThemeFont;

typedef struct ThemeFonts { HFONT font[THEME_FONTS]; } ThemeFonts;

typedef struct ThemeBuffer {   /* drawing off screen, shown at once */
    HDC     target, dc;
    HBITMAP bitmap;
    HGDIOBJ old;
    RECT    rc;
} ThemeBuffer;

void     Theme_Init(void);
BOOL     Theme_IsDark(void);
COLORREF Theme_Color(ThemeColor color);
HBRUSH   Theme_Brush(ThemeColor color);
COLORREF Theme_DrawRow(HWND owner, HDC dc, const RECT *rc, UINT state, COLORREF around);   /* returns the text color */
COLORREF Theme_RowMuted(UINT state);
void     Theme_DrawTreeGlyph(HWND tree, HDC dc, const RECT *cell, UINT state, BOOL open);   /* a folder's arrow; state: THEME_ROW_* */
void     Theme_DrawFocusCue(HWND owner, HDC dc, const RECT *row);   /* the keyboard's row, after keyboard use only */
void     Theme_DrawButton(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state, UINT format);
void     Theme_DrawSwitch(HWND owner, HDC dc, const RECT *rc, BOOL on, UINT state);   /* THEME_BUTTON_HOT, _DISABLED */
void     Theme_DrawField(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, BOOL hint, HFONT font, UINT state);
void     Theme_FieldText(HWND owner, const RECT *field, RECT *text);   /* where Theme_DrawField puts its text: an edit's place */
void     Theme_DrawDropDown(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state);   /* THEME_BUTTON_* */
int      Theme_DropDownWidth(HWND owner, HFONT font, const WCHAR *text);   /* fits `text` whole, in the current language */
void     Theme_DropDownLabel(HWND owner, const RECT *box, RECT *label);   /* where Theme_DrawDropDown draws the label of `box` */
BOOL     Theme_TableCellText(HWND list, int row, int column, RECT *text);   /* where a table cell's text is drawn; a wider text shows a tip */
UINT     Theme_TrackDropDown(HWND owner, HMENU menu, const RECT *screenBox);
/* `menu` drawn by the program itself below `screenBox` (a button, or a point
 * for a context menu): its strings, separators, checks and grayed items, no
 * submenus. The command chosen, 0 when dismissed. */
UINT     Theme_TrackMenu(HWND owner, HMENU menu, const RECT *screenBox);
/* An item of a menu Theme_TrackMenu shows: drawn red, it deletes; appended with `glyph` (Windows' icon
 * font), its icon before its words. Both keep the item's data. */
void     Theme_SetMenuDanger(HMENU menu, UINT id);
void     Theme_TitleBar(HWND window);   /* a window of the program's own: its title bar in the system's mode */
BOOL     Theme_AppendMenu(HMENU menu, UINT flags, UINT_PTR id, const WCHAR *text, WCHAR glyph);
void     Theme_SetStrong(HWND control);   /* its text semibold, at the dialog's scale */
/* The color of a button's icon, by what the button does, in a shade for each mode. */
typedef enum ThemeTint {
    THEME_TINT_NONE,     /* the caption's color */
    THEME_TINT_GREEN, THEME_TINT_RED, THEME_TINT_BLUE, THEME_TINT_TEAL, THEME_TINT_PURPLE, THEME_TINT_AMBER, THEME_TINT_GOLD,
    THEME_TINTS
} ThemeTint;
void     Theme_SetGlyph(HWND button, WCHAR glyph, ThemeTint tint);   /* an icon before a push button's caption: a character of Windows' icon font, 0 for none */
void     Theme_SetIconButton(HWND button, WCHAR glyph, ThemeTint tint);   /* that icon alone, the caption in a tip under the mouse */
BOOL     Theme_IsIconButton(HWND button);   /* it shows its icon alone, its caption in a tip */
/* A character of Windows' icon font beside text in `textFont` (a row's mark):
 * its width on `dc`, and drawn at the start of `rc`, vertically centered. */
int      Theme_InlineGlyphWidth(HDC dc, WCHAR glyph, HFONT textFont);
void     Theme_DrawInlineGlyph(HDC dc, const RECT *rc, WCHAR glyph, HFONT textFont, COLORREF color);
void     Theme_DrawDot(HDC dc, const RECT *rc, int size, COLORREF color);   /* an unread mark */
void     Theme_DrawDropZone(HWND owner, HDC dc, const RECT *rc, WCHAR glyph, ThemeTint tint, const WCHAR *text, HFONT font, BOOL hot);
/* An image list of `count` characters of Windows' icon font, each in its tint
 * (THEME_TINT_NONE: muted), at the small icon size of `owner`'s monitor. */
struct _IMAGELIST *Theme_GlyphImages(HWND owner, const WCHAR *glyphs, const ThemeTint *tints, int count);   /* an HIMAGELIST */
COLORREF Theme_TintColor(ThemeTint tint);   /* in the current mode; the caption's color for none, and in a contrast theme */
/* A report list's rows can end their first cell with marks, drawn by the theme's tables: `proc` gives a row's
 * characters of Windows' icon font and their tints, the first rightmost, at most `cap` (THEME_ROW_MARKS), and
 * returns how many (NULL: no marks). */
#define THEME_ROW_MARKS 4
typedef int (*ThemeRowMarkProc)(HWND list, int row, WCHAR *glyphs, ThemeTint *tints, int cap);
void     Theme_SetRowMarks(HWND list, ThemeRowMarkProc proc);
void     Theme_SetMainGlyph(HWND dialog, int id, int state);   /* the manager window's button `id` gets the icon of its caption in `state` */
BOOL     Theme_CheckBoxSize(HWND control, SIZE *size);
void     Theme_CreateFonts(HWND dialog, ThemeFonts *fonts);
void     Theme_FreeFonts(ThemeFonts *fonts);
HDC      Theme_BufferBegin(ThemeBuffer *buffer, HDC target, const RECT *rc);
void     Theme_BufferEnd(ThemeBuffer *buffer);
void     Theme_SetScrollRow(HWND control, int rowPx);   /* how far a wheel line scrolls, 0: one row of the control */
int      Theme_ScrollTarget(HWND window, WPARAM request, int linePx);   /* the position a WM_VSCROLL asks of a window scrolled by the pixel */
void     Theme_SetHeadingRows(HWND list);   /* a report list's rows with nothing after their first column are headings: semibold */
/* A report list whose heading rows fold (each one's overlay index, INDEXTOOVERLAYMASK, a THEME_FOLD_*):
 * a heading shows an arrow in its indent's cell, before its check box or icon (headings at indent 1, their
 * rows below); its owner folds them, hitting the arrow at Theme_FoldArrowRect. */
#define THEME_FOLD_CLOSED 1
#define THEME_FOLD_OPEN   2
#define THEME_FOLD_NONE   3   /* a heading with nothing to fold: no arrow */
/* A read-only rich edit (MSFTEDIT_CLASS, which Theme_RichTextReady loads) showing Markdown, in the theme's
 * fonts and colors (Core_MarkdownRtf), again in a new theme. */
BOOL     Theme_RichTextReady(void);
void     Theme_SetRichText(HWND rich, const WCHAR *markdown);
void     Theme_SetFoldingRows(HWND list);
BOOL     Theme_FoldArrowRect(HWND list, int row, RECT *cell);
/* Its rows: a heading's lParam names its group, every other row's is its own, never negative. */
#define THEME_HEADING_ROW(k) ((LPARAM)(-1 - (k)))
#define THEME_HEADING_OF(lp) ((int)(-1 - (lp)))
LPARAM   Theme_RowParam(HWND list, int row);
void     Theme_InsertFoldingRow(HWND list, int row, const WCHAR *text, int image, LPARAM param, int fold);
void     Theme_SetFold(HWND list, int row, int fold);
BOOL     Theme_OnFoldArrow(HWND list, int row, POINT pt);   /* heading `row`'s arrow under `pt` (the list's client) */
void     Theme_DeleteFoldedRows(HWND list, int row);        /* heading `row` folds: its rows go */
void     Theme_FocusRow(HWND list, int row);                /* the keyboard's row, alone selected, in sight */
void     Theme_CheckRows(HWND list);   /* rows with check boxes, in one column as wide as the list */
BOOL     Theme_CheckRowChanged(const NMHDR *header, HWND list);   /* `list`'s LVN_ITEMCHANGED that turned a row's box */
WCHAR    Theme_ProfileOwnGlyph(DWORD part, ThemeTint *tint);   /* a profile's own field (PROFILE_OWN_*) as its row shows it */
/* A table cell's image (LVS_EX_SUBITEMIMAGES) that is the theme's check box, drawn as a row's: a switch's value,
 * THEME_CELL_BOX plus its flags, checked or not, enabled or not (a list keeps a cell's image in 16 bits). */
#define THEME_CELL_BOX           (-100)
#define THEME_CELL_BOX_CHECKED   0x1
#define THEME_CELL_BOX_DISABLED  0x2
#define THEME_IS_CELL_BOX(image) ((image) >= THEME_CELL_BOX && (image) <= THEME_CELL_BOX + 3)
/* An editor of a list's cell, a child of the list (an edit, or a drop-down list): themed as a dialog's controls
 * are, in the list's font, over cell (`row`, `column`) and as tall as its row, shown. */
void     Theme_PlaceCellEditor(HWND list, int row, int column, HWND editor);
/* A click (NM_CLICK, NM_DBLCLK) on a folding list: the heading row it folds or
 * opens (its arrow, or a double-click off a check box), else -1; `hit` gets
 * where it was. */
struct tagLVHITTESTINFO;
int      Theme_FoldClickRow(HWND list, const NMHDR *header, struct tagLVHITTESTINFO *hit);
/* Left or Right (`key`) on a folding list: the focused heading's row, to fold
 * (Left) or open (Right), else -1; Left on a row below a heading goes up to it. */
int      Theme_FoldKeyRow(HWND list, WORD key);
#define THEME_FOLD_MENU_EXPAND   1
#define THEME_FOLD_MENU_COLLAPSE 2
UINT     Theme_FoldMenu(HWND owner, HWND list, HWND from, LPARAM pos);   /* WM_CONTEXTMENU: Expand all, Collapse all, or 0 */

/* ------------------------------------------------------------ checklist.c */

/* A list whose groups fold, with check boxes and values: a property list (what to sync, what an export or an import
 * holds, a profile's own and its settings, conflicts settled). Each group a heading with its image and its items
 * and values below, a group without rows a heading alone; a group's box checks all of it, mixed when only some is.
 * The groups opened are kept in settings.ini (`openedSection`, NULL: not kept). */
#define CHECKLIST_GROUPS_MAX 16
#define CHECKLIST_COLUMNS    (1 + MAX_PROFILES)   /* the labels' column and a pick list's versions, at most */
typedef struct CheckGroup { DWORD mask; const WCHAR *heading; int image; const WCHAR *name; } CheckGroup;   /* name: as kept opened */
typedef struct CheckItem { DWORD bit; const WCHAR *label; int image; } CheckItem;
typedef const WCHAR *(*CheckLabelProc)(const CheckItem *item, void *context);   /* an item's text; NULL: TR(label) */
/* A value of a group, or of one of its items (`item`: shown below it, or on its row when `merged`; a group's own
 * `merged` on its heading): a box alone (its
 * text beside it); a switch, a check box in the value's cell (its `text` beside the box); a choice or a text, changed in place in a drop-down or
 * an edit box while its row is selected; an action, its text, which the dialog acts on when it is clicked (F2,
 * Space); or a pick, versions side by side, one per value column, the one kept checked (clicked, or Left and
 * Right). With a box of its own (`boxed`) it is taken or not, and an item's box
 * follows its values'; without, it follows its item's. Read-only, it only shows. A change of its value tells the
 * list's dialog (WM_CHECKLIST_VALUE); a value without a group (`group` -1) is a row of its own, above the groups. A
 * value can be a part of another (`parent`: a list's element, an object's member), right after it in `values`, its
 * row one step further in below the other's, shown while that one is open; a box of a value with parts takes them
 * all, and shows mixed when only some are taken. */
#define CHECK_VALUE_NONE   0
#define CHECK_VALUE_SWITCH 1
#define CHECK_VALUE_CHOICE 2
#define CHECK_VALUE_TEXT   3
#define CHECK_VALUE_PICK   4
#define CHECK_VALUE_ACTION 5
#define CHECK_VALUE_CCH    256
#define CHECK_NAME_CCH     160
typedef struct CheckValue {
    int                 group;                /* the group it is under, -1: none */
    DWORD               item;                 /* the item it belongs to, 0: its group's own */
    int                 kind, image;          /* CHECK_VALUE_*, its image */
    const WCHAR        *label;                /* a catalog key, or as it is shown when `literal` */
    BOOL                literal, readOnly, merged;
    BOOL                boxed, taken;         /* a box of its own, and whether it is checked */
    BOOL                on;                   /* a switch's */
    int                 choice;               /* a choice's: an index of `choices`, in the interface's language */
    const WCHAR *const *choices;
    const COLORREF     *swatches;             /* a choice's colors, shown beside them; NULL: none */
    int                 choiceCount;
    WCHAR               text[CHECK_VALUE_CCH];   /* a text's, an action's, or what a box alone says beside it */
    int                 limit;                /* a text's length at most, 0: CHECK_VALUE_CCH - 1 */
    const WCHAR        *hint;                 /* a text's, its edit box shows it while empty; NULL: none */
    const WCHAR *const *versions;             /* a pick's text in each value column, NULL there: it has none */
    int                 pick;                 /* the value column kept, -1: none */
    LPARAM              data;                 /* the caller's */
    int                 parent;               /* 1 + the value it is a part of; 0: none */
    BOOL                open, paneOpen;       /* one with parts: they show below it, in the list, in its pane */
    const char         *json;                 /* the JSON it was read from (CheckList_FromJson), the caller's; NULL: none */
    size_t              jsonLength;
    const char         *key;                  /* an object's member: its key as the JSON writes it (no quotes); NULL: none */
    size_t              keyLength;
    WCHAR               name[CHECK_NAME_CCH]; /* the label CheckList_AddParts gave it */
} CheckValue;
#define CHECK_VALUE_BASE ((LPARAM)0x100000000LL)   /* a value row's lParam, plus its index: past every item's bit */
typedef struct CheckList {
    const CheckGroup *groups;
    int               groupCount;
    const CheckItem  *items;
    int               itemCount;
    CheckValue       *values;             /* NULL: none */
    int               valueCount;
    const WCHAR *const *columns;          /* a pick list's column titles, the labels' first; NULL: one value column, untitled */
    int               columnCount;
    BOOL              plain;              /* no boxes: versions and values only */
    int               pickedImage, candidateImage;   /* a pick's version kept, and the others it has */
    const WCHAR      *openedSection;
    DWORD             openAtFirst;        /* the groups open (a bit each) while none were kept opened */
    DWORD             offered, checked;   /* the bits shown, those checked */
    CheckLabelProc    label;
    void             *context;
    HWND              list, owner;        /* CheckList_Setup's: the list, and its dialog */
    HWND              editor;             /* the selected value's, NULL: none */
    int               editing;            /* its value, -1: none */
    BOOL              open[CHECKLIST_GROUPS_MAX];
    BOOL              filling, following;
    /* The pane beside the list, when its dialog has one (IDC_PANE_*): what the row selected holds, a list of its own
     * over the same values (`pane`, made and freed with it), whose `twin` is this one; each follows the other. The
     * list beside a pane is an overview (`overview`): its items without their values, its values without their
     * parts, which the pane shows; a group's own values stay below its heading. */
    struct CheckList *pane, *twin;
    BOOL              overview;
    HWND              paneTitle;          /* the row's name */
    LPARAM            scope;              /* the pane's: the row of the list it shows, 0: none */
    int               indentLess;         /* the pane's: its rows that much further out than in the list */
    DWORD             openItems;          /* the pane's: the items whose values show below them */
    struct CheckSearch *search;           /* the pane's search (IDC_PANE_SEARCH), NULL: none */
    HWND              changedIn;          /* the list a value was last changed in, the pane's or this one */
} CheckList;
/* The pane's search options (CheckSearch.in): what it looks in, and how. */
#define CHECK_SEARCH_NAMES  0x1u
#define CHECK_SEARCH_VALUES 0x2u
#define CHECK_SEARCH_REGEX  0x4u
#define CHECK_SEARCH_CASE   0x8u
#define WM_CHECKLIST_BOX   (WM_APP + 0x100)   /* a box clicked: wParam its row, lParam its list; the dialog calls CheckList_Message */
#define WM_CHECKLIST_VALUE (WM_APP + 0x101)   /* sent to the list's dialog: value wParam changed */
void CheckList_Setup(CheckList *checks, HWND list, const WCHAR *glyphs, const ThemeTint *tints, int images);
void CheckList_Toggle(CheckList *checks, int row);
#define CHECKLIST_ALL    0
#define CHECKLIST_NONE   1
#define CHECKLIST_INVERT 2
void CheckList_CheckAll(CheckList *checks, int how);   /* every box checked (CHECKLIST_ALL), none, or each turned over */
void CheckList_FoldAll(CheckList *checks, BOOL open);
void CheckList_Relabel(CheckList *checks);   /* every row again in the interface's language, the list's and its pane's */
void CheckList_Refresh(CheckList *checks);   /* the boxes and values shown again, as the caller changed them */
void CheckList_ShowValue(CheckList *checks, int v);   /* value `v` shown again, as the caller changed it */
void CheckList_SelectValue(CheckList *checks, int v, BOOL edit);   /* value `v`'s row selected, its group opened; `edit`: into its editor */
/* A value as JSON `raw` shows it: true or false a switch, a number or a string a text (as long as it fits), anything
 * else its count of items, read-only; FALSE for no value. Back: the JSON it now stands for, as the kind of value
 * `raw` was (a number only while it is one), FALSE when it did not change or cannot be one. */
BOOL CheckList_FromJson(CheckValue *value, const char *raw, size_t length);
BOOL CheckList_ToJson(const CheckValue *value, const char *raw, size_t length, char *out, size_t cap);
/* What value `v` (read by CheckList_FromJson, and its parts) stands for now: a heap block, NULL when nothing of it
 * changed (or without memory). A part left out (its box) is not in it. */
char *CheckList_JsonOf(const CheckList *checks, int v, size_t *length);
/* A name for `text` (a session's title for its id): TRUE when it gives one. */
typedef BOOL (*CheckNameProc)(void *context, const WCHAR *text, WCHAR *name, size_t cch);
typedef struct CheckParts {
    int           cap, depth;        /* values in the list at most; levels below the value at most */
    CheckNameProc name;
    void         *context;
    int           named, folder, list, leaf;   /* the images: of a part `name` names, a folder's path, a list or an object, the rest */
} CheckParts;
/* The parts of value `v` (an object's members, a list's elements; its `json`) added right after it, a value each,
 * each named by its key, or a list's element by its own text or name (`name` first); boxed as `v` is. None when they
 * would not all fit in `cap`; past `depth`, a list or an object shows what it holds, read-only. */
void CheckList_AddParts(CheckList *checks, int v, const CheckParts *parts);
BOOL CheckList_IsLeftOut(const CheckList *checks, int v);   /* a value with a box, none of it taken */
BOOL CheckList_Notify(CheckList *checks, HWND dialog, const NMHDR *header, LRESULT *result);   /* TRUE: the dialog answers `result` */
/* WM_NOTIFY (the list's and its pane's: CheckList_Notify, its answer set as
 * the dialog's), WM_CHECKLIST_BOX (a box turned over) and WM_CONTEXTMENU
 * (Expand/Collapse all on the list) handled alike for every checked-list
 * dialog, the sync's, the export's and the import's, the profile's: TRUE
 * when it was this list's, `*toggled` then TRUE for a box, so the dialog
 * refreshes what the choice drives. */
BOOL CheckList_Message(CheckList *checks, HWND dialog, UINT message, WPARAM wp, LPARAM lp, BOOL *toggled);
/* The what-to-sync groups ([0] Sessions, then the layout groups), their
 * items, and the glyph set they draw with: a backup's list builds on them. */
const CheckGroup *SyncUi_Groups(int *count);
const CheckItem  *SyncUi_Items(int *count);
const WCHAR      *SyncUi_Glyphs(const ThemeTint **tints, int *images);
/* Sessions by their ids, for naming them in a list (a CheckNameProc's context). */
typedef struct SessionNames {
    const ArchivedSession *sessions;
    int                    count;
} SessionNames;
BOOL              SyncUi_SessionName(void *names, const WCHAR *text, WCHAR *name, size_t cch);
/* The settings of `layout` (SessionSync_ReadLayout) added one by one to the values of `checks` (room for `cap` in
 * all), each part with a label whose item is in `items`: under the group of `checks` holding its item, below its
 * item's row, or on it when it is the item's only value, else its group's own (on its heading when it is the only
 * one); a switch, a text, or what it holds, a list's elements and an object's members one by one below it (sessions
 * named by `names`, NULL: none), each with a box when `boxed`. Each one's `data` is its part (SessionSync_LayoutPart). */
void              SyncUi_AddLayoutValues(CheckList *checks, int cap, const char *layout, size_t length, DWORD items, BOOL boxed,
                                         const SessionNames *names);
/* The conflicts dialog's list as a sync's conflicts fill it: two elements of a sidebar group that `name` and
 * "Personal" each changed their own way (tests). */
CheckList        *SyncUi_FillConflictsList(HWND list, const WCHAR *name);
/* Those values from `first` on that were changed, as a layout of their parts alone, a part left out (its box) null:
 * a heap block, NULL for none. */
char             *SyncUi_LayoutChanges(const CheckList *checks, int first, const char *layout, size_t length, size_t *outLength);
HWND     Theme_SmoothView(HWND control);   /* a list or tree scrolled by the pixel; returns the view, which has its place */
/* A window of ours in a view that is no list or tree answers how tall what it shows is; it says when that changed, and
 * asks for a band of itself in sight. */
#define WM_THEME_CONTENT_HEIGHT (WM_APP + 0x3E1)
void     Theme_ViewChanged(HWND control);
void     Theme_ViewReveal(HWND control, int top, int bottom);
void     Theme_Follow(HWND dialog, UINT msg, WPARAM wp, LPARAM lp);
void     Theme_Apply(HWND dialog);   /* what it keeps on the dialog goes with the dialog */
void     Theme_RememberLayout(HWND dialog);
void     Theme_FitDialog(HWND dialog);
BOOL     Theme_MainMinimum(HWND dialog, SIZE *client);
void     Theme_LayoutMain(HWND dialog);
void     Theme_ProfileColumnWidths(HWND list, int *profile, int *role, int *dataMinimum, int *sessionsMinimum);   /* the minimums may be NULL */
const WCHAR *Theme_SessionsFolderState(int state);   /* the sessions folder column's words: own, not signed in, no sessions, broken */
void     Theme_FitLastColumn(HWND list, int minimum);   /* the width the other columns leave, at least `minimum`, unless the user sized it or drags it */
/* The catalog keys of what the manager window shows, the ones its layout is
 * measured with: a button's caption in `state` (0: the resource's, 1: its
 * other one), a profile column's title (NULL past the last), a row's role. */
const WCHAR *Theme_MainCaption(int id, int state);
const WCHAR *Theme_ProfileColumnTitle(int column);
#define PROFILE_COLUMN_STATE 4   /* the profile list's Status column: its index, shown second */
const WCHAR *Theme_ProfileState(BOOL running);   /* what the Status column says */
int      Theme_ProfileStateWidth(HWND list);     /* the Status column's width: its title or its widest state, in any language */
const WCHAR *Theme_ProfileRole(BOOL stock, BOOL isDefault);
/* The version label's text in each state (its one %s: this build, the
 * release available, the one downloading), the status in each state (its
 * %s: Claude Desktop's version), the note under the column's actions (its
 * %s: the profile the regular Claude icon opens) and the sessions details'
 * captions, also measured with the window: catalog keys. */
typedef enum MainVersion { MAIN_VERSION_BUILD, MAIN_VERSION_AVAILABLE, MAIN_VERSION_DOWNLOADING, MAIN_VERSION_INSTALLING,
                           MAIN_VERSION_CHECKING, MAIN_VERSIONS } MainVersion;
typedef enum MainStatus { MAIN_STATUS_NO_CLAUDE, MAIN_STATUS_NO_LINKS, MAIN_STATUS_ROUTED, MAIN_STATUSES } MainStatus;
const WCHAR *Theme_MainVersion(MainVersion state);
const WCHAR *Theme_MainStatus(MainStatus state);
void         Theme_DrawProgress(HWND owner, HDC dc, const RECT *rc, int done, int total, const WCHAR *text);   /* a sync's, in the column's foot */
const WCHAR *Theme_MainNote(void);
BOOL     Theme_ColumnResizeIsManual(HWND list, int column);   /* the user sized it (a header divider dragged or double-clicked) */
void     Theme_SetColumnWidth(HWND list, int column, int width);
void     Theme_LayoutSidebarNote(HWND note, const WCHAR *format, const WCHAR *name);
/* A static's text, `format` naming a profile in its one %s: a long name is
 * cut (with an ellipsis) until the whole text fits the control. */
void     Theme_FitName(HWND control, const WCHAR *format, const WCHAR *name, WCHAR *text, size_t cch);
/* The program's icon at the window's scale, for its title bar and taskbar
 * button; the icons it replaces in `*large` and `*little` are destroyed. */
void     Theme_SetWindowIcons(HWND window, HICON *large, HICON *little);
/* An icon of `instance` (NULL: one of Windows', IDI_*) at `width` x `height`, the caller's to destroy. */
HICON    Theme_LoadIcon(HINSTANCE instance, LPCWSTR name, int width, int height);
INT_PTR  Theme_CtlColor(UINT msg, WPARAM wp, LPARAM lp, int mutedId);
BOOL     Ui_Ask(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel);
/* The themed message box with two choices and Cancel: 1 for the first, 2 for the second, 0 for Cancel (or Esc). */
int      Ui_Choose(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *first, const WCHAR *second, const WCHAR *cancel);
int      Ui_Message(HWND owner, UINT flags, const WCHAR *format, ...);   /* MessageBox look-alike, `format` translated */
INT_PTR  Ui_Dialog(HWND owner, int id, DLGPROC proc, LPARAM param);   /* every dialog opens here */
/* The size of the program's windows, in percent of their resources' (SETTING_ZOOM): their font, and so all of them. */
#define THEME_ZOOM_MIN 80
#define THEME_ZOOM_MAX 200
int      Theme_Zoom(void);
INT_PTR  Theme_DialogBox(int id, HWND owner, DLGPROC proc, LPARAM param);   /* dialog `id` at Theme_Zoom's size */

/* ------------------------------------------------ router.c / main.c / gui.c */

int     Router_Run(const WCHAR *rawUrl);
int     Router_ChooseNotification(const ClaudePackage *pkg, const ProfileList *list, int suggested);   /* IDD_LINK for a notification; -1: none */
void    NotifyGuard_Start(void);   /* in a watcher: notifications clicked in the notification center go to their profile */
int     Launcher_Run(const WCHAR *folder, const WCHAR *url);   /* url: NULL, or a claude:// link opened there */
HRESULT Launcher_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *url, DWORD *pid, BOOL *identity);
HRESULT Launcher_OpenSynced(const ClaudePackage *pkg, const Profile *p, DWORD *pid, BOOL *identity);   /* sessions already made the same */
typedef enum GuiStart { GUI_MANAGER, GUI_SET_UP_LINKS, GUI_UNINSTALL } GuiStart;
int     Gui_Run(GuiStart start);
BOOL    Gui_MainWindowGeometry(HWND dialog, UINT message, WPARAM wp, LPARAM lp);
void    Gui_OrderProfileColumns(HWND list);   /* Status shown second */
void    Gui_LayoutProfileColumns(HWND list);
struct CheckList *Gui_FillProfileList(HWND list);   /* the edit dialog's list of a profile, its settings one by one (tests) */
void    Gui_ShowSessions(HWND dialog, const ClaudePackage *package, BOOL showSessions, const WCHAR *folder);

#endif
