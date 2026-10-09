/*
 * Claude Desktop Profiles Manager - run several Claude Desktop accounts side by side on Windows.
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

#define APP_NAME           L"Claude Desktop Profiles Manager"
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

#define MAX_PROFILES       32
C_ASSERT(MAX_PROFILES <= 32);   /* sets of profiles are DWORD bit masks */
#define MAX_NAME           32   /* characters in a new profile name */
#define MAX_LABEL          48   /* characters in a display name */
#define FOLDER_CCH         64
#define LABEL_CCH          64
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

/* What a profile keeps the same as the others it syncs with (Profile.syncItems). */
#define SYNC_ITEM_SESSIONS     0x01u   /* its sessions: new ones, titles, archived and deleted ones */
#define SYNC_ITEM_SIDEBAR      0x02u   /* the sidebar: pins, groups, project order, filters, folded groups */
#define SYNC_ITEM_DETAILS      0x04u   /* each session's model and effort, side pane, unread mark and cost */
#define SYNC_ITEM_APPEARANCE   0x08u   /* fonts, the editor's settings, zoom, spelling */
#define SYNC_ITEM_LANGUAGE     0x10u
#define SYNC_ITEM_MODEL        0x20u   /* the default model */
#define SYNC_ITEM_SETTINGS     0x40u   /* Claude's switches (auto-archive, Cowork, Remote Control) and Cowork's recent folders */
#define SYNC_ITEM_PERMISSIONS  0x80u   /* folders' permission modes and their confirmations, Cowork's trusted folders */
#define SYNC_ITEMS_ALL         0xFFu
#define SYNC_ITEMS_DEFAULT     (SYNC_ITEMS_ALL & ~SYNC_ITEM_PERMISSIONS)
#define SYNC_ITEM_COUNT        8

/* ------------------------------------------------------------------ types */

typedef struct Profile {
    WCHAR folder[FOLDER_CCH];  /* "Claude" (stock) or "Claude-<name>": the stable id */
    WCHAR name[LABEL_CCH];     /* display name, editable */
    WCHAR dataDir[MAX_PATH];   /* %APPDATA%\<folder>, as Claude sees it */
    WCHAR storageDir[MAX_PATH]; /* file access outside the package; empty when unresolved */
    int   color;               /* palette index */
    WCHAR badge[BADGE_CCH];    /* the badge's own text; empty: the name's initial */
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

typedef enum PendingOp { PENDING_TITLE, PENDING_STAR, PENDING_REMOVE } PendingOp;

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
} SyncOp;

typedef struct SyncReport {             /* what sending sessions to profiles did */
    int   added, updated, removed, skipped, failed;
    int   forked;                       /* sessions two profiles went on with apart, kept as two */
    DWORD waiting;                      /* the profiles whose part waits for them to close */
    DWORD unavailable;                  /* the profiles that have no session entries yet: nothing sent there */
    int   conflicts;                    /* parts of a sidebar changed in two profiles each their own way: left to the person */
    WCHAR backup[MAX_PATH];             /* where what was replaced or removed went; "" for nowhere */
    WCHAR error[LONG_PATH_CCH];         /* the first failure, "" for none */
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
BOOL         Core_BuildLaunchArgs(const WCHAR *dataDir, const WCHAR *url, WCHAR *out, size_t cch);
BOOL         Core_LatestSignInStart(const char *text, size_t len, SYSTEMTIME *latest);
BOOL         Core_LastQuit(const char *text, size_t len, SYSTEMTIME *when, BOOL *forUpdate);
/* The running window a claude:// link is suggested for, an index below
 * `count` (-1 when none runs): the one that started the latest sign-in for a
 * sign-in link, else the only one, the one used last (`lastUsed`), the
 * default profile's (`defaultIndex`) or the first. `signInTicks` may be NULL. */
int          Core_SuggestTarget(int count, const ULONGLONG *signInTicks, ULONGLONG nowTicks, ULONGLONG signInMaxAgeTicks,
                                BOOL signInUrl, int lastUsed, int defaultIndex, RouteReason *reason);
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
BOOL         Core_EndsWithI(const WCHAR *s, const WCHAR *suffix);
BOOL         Core_ContainsI(const WCHAR *s, const WCHAR *needle);
DWORD        Core_HashIgnoringCase(const WCHAR *text);
void         Core_ProfileAumid(const WCHAR *folder, WCHAR *out, size_t cch);
void         Core_ShortcutFileName(const WCHAR *label, int copyNumber, WCHAR *out, size_t cch);
ULONGLONG    Core_SystemTimeTicks(const SYSTEMTIME *st);
BOOL         Core_PathUnder(const WCHAR *path, const WCHAR *dir);
BOOL         Core_PathWithVariable(const WCHAR *path, const WCHAR *variable, const WCHAR *value, WCHAR *out, size_t cch);   /* "%APPDATA%\..." */
BOOL         Core_ProfileFilePath(const Profile *p, const WCHAR *path, WCHAR *out, size_t cch);
BOOL         Core_PackageCachePath(const WCHAR *localAppData, const WCHAR *family, const WCHAR *area, const WCHAR *name,
                                   WCHAR *out, size_t cch);
BOOL         Core_SameFatTime(const FILETIME *a, const FILETIME *b);
BOOL         Core_JsonMember(const char *json, size_t len, const char *key, const char **value, size_t *valueLen);
/* Each member of the JSON object `json`, in order: its key's raw text (between
 * its quotes, escapes as written) and its value's. FALSE when `json` is no
 * object, or `each` returns FALSE. */
typedef BOOL (*CoreJsonMember)(void *context, const char *key, size_t keyLen, const char *value, size_t valueLen);
BOOL         Core_JsonEachMember(const char *json, size_t len, CoreJsonMember each, void *context);
BOOL         Core_JsonIsValue(const char *text, size_t len);   /* one JSON value, whole (spaces around it aside) */
BOOL         Core_JsonString(const char *raw, size_t len, WCHAR *out, size_t cch);
BOOL         Core_JsonNumber(const char *raw, size_t len, ULONGLONG *value);
BOOL         Core_JsonTrue(const char *raw, size_t len);
SessionEntryKind Core_SessionEntryKind(const char *json, size_t len);
BOOL         Core_JsonSetMember(const char *json, size_t len, const char *key, const char *raw, char *out, size_t cap, size_t *outLen);
BOOL         Core_JsonQuote(const WCHAR *text, char *out, size_t cap);
BOOL         Core_ProjectDirName(const WCHAR *cwd, WCHAR *out, size_t cch);
#define UUID_TEXT_CCH 37   /* 8-4-4-4-12 hex digits and the terminator */
BOOL         Core_IsUuid(const WCHAR *id);
BOOL         Core_ResumeLink(const WCHAR *sessionId, WCHAR *out, size_t cch);
void         Core_ScratchName(const SYSTEMTIME *day, DWORD random, WCHAR *out, size_t cch);
BOOL         Core_ScratchDirFor(const WCHAR *dataDir, const WCHAR *entriesDir, WCHAR *out, size_t cch);
size_t       Core_ReplaceChunk(const char *in, size_t len, const CoreSwap *swaps, int count, BOOL last, char *out, size_t *used);
BOOL         Core_PendingFormat(const PendingEdit *edit, WCHAR *out, size_t cch);
BOOL         Core_PendingParse(const WCHAR *line, PendingEdit *edit);
BOOL         Core_PendingReplaces(const PendingEdit *queued, const PendingEdit *added);
BOOL         Core_JsonRemoveMember(const char *json, size_t len, const char *key, char *out, size_t cap, size_t *outLen);
BOOL         Core_SyncOpFormat(const SyncOp *op, WCHAR *out, size_t cch);
BOOL         Core_SyncOpParse(const WCHAR *line, SyncOp *op);
BOOL         Core_SyncOpReplaces(const SyncOp *queued, const SyncOp *added);
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
BOOL         Core_BuildStamp(const char *date, const char *time, WCHAR *out, size_t cch);   /* __DATE__, __TIME__ as "2026.10.08 17:20" */
BOOL         Core_ScratchFolderName(const WCHAR *cwd, WCHAR *out, size_t cch);
char        *Core_JsonSetNested(const char *json, size_t len, const char *const *keys, int depth, const char *raw, size_t *outLen);
typedef size_t (*CoreStringMap)(void *context, const char *text, size_t length, char *out, size_t cap);
char        *Core_JsonMapStrings(const char *json, size_t len, CoreStringMap map, void *context, size_t *outLen);
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
ULONGLONG    Core_HashBytes(ULONGLONG hash, const void *data, size_t size);
ULONGLONG    Core_HashText(ULONGLONG hash, const WCHAR *text);
void         Core_CenterRect(const RECT *on, int width, int height, const RECT *work, RECT *out);
BOOL         Core_ParseVersion(const WCHAR *text, DWORD parts[4]);
int          Core_CompareVersions(const DWORD a[4], const DWORD b[4]);

/* --------------------------------------------------------------- util.c */

extern HINSTANCE g_hInst;

BOOL      Util_KnownFolder(const GUID *id, WCHAR *out, size_t cch);
BOOL      Util_AppData(WCHAR *out, size_t cch);        /* %APPDATA% */
BOOL      Util_LocalAppData(WCHAR *out, size_t cch);   /* %LOCALAPPDATA% */
BOOL      Util_SelfExe(WCHAR *out, size_t cch);
BOOL      Util_InstallDir(WCHAR *out, size_t cch);     /* %LOCALAPPDATA%\Programs\Claude Desktop Profiles Manager */
BOOL      Util_InstallExe(WCHAR *out, size_t cch);
BOOL      Util_StateDir(WCHAR *out, size_t cch);       /* %LOCALAPPDATA%\Claude Desktop Profiles Manager */
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
BOOL      Util_OpenUrl(const WCHAR *url);
BOOL      Util_Spawn(const WCHAR *exe, const WCHAR *args, DWORD *pid);
BOOL      Util_IsDirectoryLink(const WCHAR *path);     /* a junction or directory symlink */
BOOL      Util_FitsRecycleBin(const WCHAR *path);
RemoveResult Util_Recycle(HWND owner, const WCHAR *const *paths, int count);
char     *Util_ReadFile(const WCHAR *path, DWORD maxBytes, BOOL tail, DWORD *len);
BOOL      Util_ExtendedPath(const WCHAR *path, WCHAR *out, size_t cch);   /* the \\?\ form, for paths past MAX_PATH */
HANDLE    Util_FindFiles(const WCHAR *dir, const WCHAR *pattern, WIN32_FIND_DATAW *found, BOOL foldersOnly);
BOOL      Util_CopyTree(const WCHAR *from, const WCHAR *to, BOOL replace, DWORD *error);
BOOL      Util_DeleteTree(const WCHAR *path, DWORD *error);

/* -------------------------------------------------------------- claude.c */

BOOL    Claude_FindPackage(ClaudePackage *pkg);
HRESULT Claude_Launch(const ClaudePackage *pkg, const Profile *profile, const WCHAR *url,
                      DWORD *pid, BOOL *withIdentity);
void    Claude_UpdateRunning(ProfileList *list);
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

/* ------------------------------------------------------------ profiles.c */

void         Profiles_Load(ProfileList *list, const ClaudePackage *pkg);
BOOL         Profiles_ResolveStorage(Profile *p, const WCHAR *localAppData, const WCHAR *family);
int          Profiles_Find(const ProfileList *list, const WCHAR *folder);
int          Profiles_DefaultIndex(const ProfileList *list);
BOOL         Profiles_Create(const WCHAR *name, int color, WCHAR *folder, size_t folderCch, WCHAR *error, size_t errorCch);
/* The name, color, badge text (empty: the initial) and picture stamp (0: none). */
BOOL         Profiles_Update(const WCHAR *folder, const WCHAR *label, int color, const WCHAR *badge, DWORD picture);
BOOL         Profiles_SetDefault(const WCHAR *folder);
BOOL         Profiles_SetSyncGroup(const WCHAR *folder, int group);   /* 0: its sessions kept apart */
BOOL         Profiles_SetSyncItems(const WCHAR *folder, DWORD items);  /* SYNC_ITEM_*; 0: the default */
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
BOOL SessionLink_Remove(const ProfileList *list, int index, WCHAR *error, size_t errorCch);

/* ----------------------------------------------------------------- zip.c */
/* ZIP archives, files stored or deflated, names in UTF-8 with "/" between
 * folders; no ZIP64 (files and archive under 4 GB, 65535 files at most). A
 * file goes in whole: 512 MB at most. Zip_Close(zip, TRUE) puts the archive
 * in place (it was written next to it); FALSE, or a failure, removes it. */
typedef struct ZipOut ZipOut;
typedef struct ZipIn ZipIn;
ZipOut     *Zip_Create(const WCHAR *path, DWORD *error);
BOOL        Zip_AddData(ZipOut *zip, const char *name, const void *data, size_t size);
BOOL        Zip_AddFile(ZipOut *zip, const char *name, const WCHAR *path);
DWORD       Zip_Error(const ZipOut *zip);   /* the first failure's Windows error */
BOOL        Zip_Close(ZipOut *zip, BOOL keep);
ZipIn      *Zip_Open(const WCHAR *path);   /* NULL: not a ZIP archive this module reads */
void        Zip_Free(ZipIn *zip);
int         Zip_Count(const ZipIn *zip);
const char *Zip_Name(const ZipIn *zip, int index);
DWORD       Zip_Size(const ZipIn *zip, int index);
int         Zip_Find(const ZipIn *zip, const char *name);
void       *Zip_Read(const ZipIn *zip, int index, DWORD maxBytes, DWORD *size);   /* checked by its CRC; HeapFree it, NUL after it */
BOOL        Zip_Extract(const ZipIn *zip, int index, const WCHAR *path);

/* -------------------------------------------------------------- backup.c */
/* A profile backed up to one archive, parts chosen (Code sessions, Cowork
 * sessions, settings, sign-in), and restored from one into a closed profile. */
BOOL Backup_Create(HWND owner, const ClaudePackage *pkg, const ProfileList *list, DWORD profiles);   /* one bit per profile: an archive each */
BOOL Backup_Restore(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index);

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
HICON Icons_CreateBadge(int color, int size);
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

typedef struct SessionEntry {          /* one profile's entry for a session */
    WCHAR     localId[SESSION_ID_CCH]; /* the entry's own id, its session's key until the first message */
    WCHAR     file[LONG_PATH_CCH];     /* its local_*.json */
    WCHAR     title[SESSION_TITLE_CCH];
    BOOL      userTitle;               /* named by someone, not by Claude */
    BOOL      starred;
    BOOL      archived;
    BOOL      damaged;                 /* Claude's startup took it for a session whose transcript is gone (transcriptUnavailable) */
    ULONGLONG lastActivity;            /* ms since 1970 */
    /* Changes made here while the profile runs, waiting for it to close. */
    BOOL      pending;
    WCHAR     pendingTitle[SESSION_TITLE_CCH];   /* "" when unchanged */
    int       pendingStar;             /* -1 unchanged, 0 or 1 */
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
    int       unreadable;              /* entries that are not a session entry */
    int       elsewhere;               /* SSH, WSL or cloud sessions: their conversation is not on this PC */
    int       pending;                 /* waiting changes its entries can take once it is closed */
    WCHAR     entriesDir[MAX_PATH];    /* claude-code-sessions\<account>\<organization>; empty when it has none */
    WCHAR     scratchDir[MAX_PATH];    /* logical scratch-workspaces\<account>\<organization>: its "no folder" */
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
BOOL         SessionStore_WorkingDir(const SessionSet *set, const WCHAR *cwd, WCHAR *out, size_t cch);
BOOL         SessionStore_WatchDir(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionStore_PendingPath(const Profile *p, WCHAR *out, size_t cch);
int          SessionStore_LoadPending(const Profile *p, PendingEdit *edits, int capacity);   /* -1: unreadable */
const WCHAR *SessionStore_RowTitle(const SessionSet *set, const SessionRow *row);           /* "": untitled */
void         SessionStore_GroupName(const SessionSet *set, int group, WCHAR *out, size_t cch);

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
 * since 1970. */
char        *SessionSync_ReadLayout(const Profile *p, const WCHAR *entriesDir, size_t *length, ULONGLONG *written);
/* Part `index` of a layout: its name, what it belongs to (SYNC_ITEM_*), and
 * whether it is made the same member by member (each session's own); FALSE
 * past the last. */
typedef struct LayoutPart {
    const char *name;
    DWORD       item;
    BOOL        byKey;
} LayoutPart;
BOOL         SessionSync_LayoutPart(int index, LayoutPart *part);
/* From now on, each change this thread makes in a profile calls `step` (a
 * progress bar's); NULL for none. */
void         SessionSync_OnEachChange(void (*step)(void *context), void *context);
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
int          SessionSync_Queued(const Profile *p, SyncOp **ops);   /* its changes waiting, a heap block in *ops; -1 unreadable */
BOOL         SessionSync_PlanPath(const Profile *p, WCHAR *out, size_t cch);
BOOL         SessionSync_Export(const SessionSet *set, int profile, const int *rows, int rowCount, const WCHAR *archive,
                                int *exported, WCHAR *error, size_t errorCch);
BOOL         SessionSync_IsArchive(const WCHAR *archive);   /* a session archive this version reads */
BOOL         SessionSync_Import(const SessionSet *set, const WCHAR *archive, DWORD targets, int *sessions, SyncReport *report);

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
} VaultIds;

/* How far a sync is: `done` of `total` steps; the total grows as the work is known. */
typedef void (*SyncProgress)(void *context, int done, int total);

DWORD SessionVault_Group(const ProfileList *list, int group);   /* its profiles, one bit each; none for group 0 */
int   SessionVault_NewGroup(const ProfileList *list);           /* the lowest group no profile is in; 0 when there is none */
DWORD SessionVault_GroupItems(const ProfileList *list, DWORD members);   /* what all of `members` keep the same (SYNC_ITEM_*) */
/* A part of the sidebar two profiles of a group changed each their own way
 * since the last sync: it stays as each has it until the person chooses. */
typedef struct VaultConflict {
    char  part[64];      /* the layout's part (SessionSync_LayoutPart) */
    DWORD item;          /* what it belongs to (SYNC_ITEM_*) */
    DWORD members;       /* the profiles of the list that changed it, one bit each */
} VaultConflict;
int   SessionVault_Conflicts(const ProfileList *list, int group, VaultConflict *out, int capacity);
/* Every conflict of `item` in `group` settled for the version of profile
 * `profile` (an index of `list`); the next sync makes it. */
BOOL  SessionVault_Decide(const ProfileList *list, int group, DWORD item, int profile);
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
BOOL  SessionVault_EverListed(VaultIds *keys);
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
BOOL SyncUi_ShareOrCopy(HWND owner, const SessionSet *set, int from, const int *rows, int rowCount, BOOL copy);
BOOL SyncUi_Export(HWND owner, const SessionSet *set, int profile, const int *rows, int rowCount);
BOOL SyncUi_ExportProfiles(HWND owner, const ProfileList *profiles, DWORD chosen);
BOOL SyncUi_Import(HWND owner, const ProfileList *profiles, DWORD chosen);
BOOL SyncUi_Restore(HWND owner, const ProfileList *profiles, const WCHAR *selected);
BOOL SyncUi_KeepSame(HWND owner, const ProfileList *profiles, int group);
/* What `members` keep the same, chosen in a dialog (IDD_SYNC_ITEMS) from `*items`: FALSE when cancelled. */
BOOL SyncUi_ChooseItems(HWND owner, const ProfileList *profiles, DWORD members, DWORD *items);
/* The conflicts syncs left to the person (SessionVault_Conflicts), one
 * dialog per group (IDD_CONFLICTS): the profiles of the groups settled. */
DWORD SyncUi_SettleConflicts(HWND owner, const ProfileList *profiles);
BOOL SyncUi_Purge(HWND owner, const ProfileList *profiles);
BOOL SyncUi_BackUpCode(HWND owner);
/* A change to data, said before it is made and asked: `question`, the
 * folders it changes (`folders`: lines made with SyncUi_AddLine and
 * SyncUi_AddSessionFolders), and, when `backedUp`, where what it replaces
 * goes first. TRUE to go on. */
#define CONFIRM_CCH 8192
BOOL SyncUi_Confirm(HWND owner, LPCWSTR icon, const WCHAR *question, const WCHAR *folders, BOOL backedUp, const WCHAR *button);
void SyncUi_AddLine(WCHAR *text, size_t cch, const WCHAR *line, BOOL indent);
void SyncUi_ShortPath(const WCHAR *path, WCHAR *out, size_t cch);   /* with %LOCALAPPDATA%, %APPDATA% or %USERPROFILE% for its start */
void SyncUi_AddPath(WCHAR *text, size_t cch, const WCHAR *path);   /* a path, short, on an indented line */
/* The folders a change to the sessions of the profiles of `bits` writes in
 * each: the entries, the working folders of sessions without a folder, and
 * (`layout`) the sidebar's settings; with a heading. */
void SyncUi_AddSessionFolders(WCHAR *text, size_t cch, const ProfileList *profiles, DWORD bits, BOOL layout);
void SyncUi_AddTranscriptFolder(WCHAR *text, size_t cch, const WCHAR *heading);   /* Claude Code's conversations, under `heading` */
/* Sessions of the profiles of `bits` written, asked first (SyncUi_Confirm):
 * their folders, the sidebar's settings when `layout`, Claude Code's
 * conversations when `transcripts`. */
BOOL SyncUi_ConfirmSessions(HWND owner, const WCHAR *question, const ProfileList *profiles, DWORD bits, BOOL layout, BOOL transcripts,
                            const WCHAR *button);

/* ---------------------------------------------------------------- help.c */

/* The questions someone new asks, under `button`; the answer picked shown. */
void Help_FillMenu(HMENU menu);
BOOL Help_Command(HWND owner, UINT command);

/* ------------------------------------------------------------- sessions.c */

#define WM_APP_SESSIONS (WM_APP + 12)   /* to the manager: session entries or transcripts changed on disk */
#define WM_APP_SESSIONS_READY (WM_APP + 13) /* background session snapshot ready for the window */
#define WM_APP_SYNC_PROGRESS (WM_APP + 20)  /* to the manager, from any process: wParam steps done of lParam; 0 of 0 once done */
#define WM_APP_SYNC_CONFLICTS (WM_APP + 21) /* to the manager, from any process: a sync left conflicts to the person */

void         SessionsView_Init(HWND dlg);
void         SessionsView_Warm(const ProfileList *profiles);
void         SessionsView_SetProfiles(const ProfileList *profiles);
void         SessionsView_Ready(BOOL allowChanges);
void         SessionsView_Enter(const ClaudePackage *pkg, const WCHAR *folder);   /* pkg: the manager's, kept current, read at each use */
void         SessionsView_Leave(void);
BOOL         SessionsView_Shown(void);
const WCHAR *SessionsView_Profile(void);
void         SessionsView_Reload(void);
/* The folder watch stopped while profile folders are moved or removed, and
 * started again after (calls pair up). */
void         SessionsView_PauseWatching(void);
void         SessionsView_ResumeWatching(void);
BOOL         SessionsView_Command(WPARAM wp);
BOOL         SessionsView_ClearSearch(void);
BOOL         SessionsView_Notify(const NMHDR *header, LRESULT *result);
BOOL         SessionsView_ContextMenu(HWND from, LPARAM pos);
BOOL         SessionsView_DrawItem(const DRAWITEMSTRUCT *item);
void         SessionsView_Relayout(void);
void         SessionsView_Resize(void);
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
    THEME_COLORS
} ThemeColor;

#define THEME_ROW_SELECTED 0x1
#define THEME_ROW_HOT      0x2

/* Geometry the theme draws and places with, which the tests check too. */
#define THEME_CORNER_RADIUS_DIPS      4       /* buttons, selected rows, views and edits */
#define THEME_VIEW_MAX_PX             30000   /* a control taller than this scrolls itself in its view */
#define THEME_MAIN_MARGIN_DIPS        15      /* around the manager window's content */
#define THEME_MAIN_GAP_DIPS           9       /* between its neighboring buttons */
#define THEME_MAIN_READING_WIDTH_DIPS 1200    /* a wider manager window centers its content */
#define THEME_PROFILE_COLUMN_DIPS     180     /* the profile list's first column */

/* A drop-down list item's data (CB_SETITEMDATA) can show a color swatch
 * beside its label, in the closed box and in its menu: the color in the low
 * 24 bits, with THEME_CHOICE_SWATCH; THEME_CHOICE_SEPARATED draws a line
 * above the item in the menu. */
#define THEME_CHOICE_SWATCH    0x01000000
#define THEME_CHOICE_SEPARATED 0x02000000

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
void     Theme_DrawDropDown(HWND owner, HDC dc, const RECT *rc, const WCHAR *text, HFONT font, UINT state);   /* THEME_BUTTON_* */
int      Theme_DropDownWidth(HWND owner, HFONT font, const WCHAR *text);   /* fits `text` whole, in the current language */
void     Theme_DropDownLabel(HWND owner, const RECT *box, RECT *label);   /* where Theme_DrawDropDown draws the label of `box` */
BOOL     Theme_TableCellText(HWND list, int row, int column, RECT *text);   /* where a table cell's text is drawn; a wider text shows a tip */
UINT     Theme_TrackDropDown(HWND owner, HMENU menu, const RECT *screenBox);
void     Theme_SetStrong(HWND control);   /* its text semibold, at the dialog's scale */
/* The color of a button's icon, by what the button does, in a shade for each mode. */
typedef enum ThemeTint {
    THEME_TINT_NONE,     /* the caption's color */
    THEME_TINT_GREEN, THEME_TINT_RED, THEME_TINT_BLUE, THEME_TINT_TEAL, THEME_TINT_PURPLE, THEME_TINT_AMBER, THEME_TINT_GOLD,
    THEME_TINTS
} ThemeTint;
void     Theme_SetGlyph(HWND button, WCHAR glyph, ThemeTint tint);   /* an icon before a push button's caption: a character of Windows' icon font, 0 for none */
COLORREF Theme_TintColor(ThemeTint tint);   /* in the current mode; the caption's color for none, and in a contrast theme */
void     Theme_SetMainGlyph(HWND dialog, int id, int state);   /* the manager window's button `id` gets the icon of its caption in `state` */
BOOL     Theme_CheckBoxSize(HWND control, SIZE *size);
void     Theme_CreateFonts(HWND dialog, ThemeFonts *fonts);
void     Theme_FreeFonts(ThemeFonts *fonts);
HDC      Theme_BufferBegin(ThemeBuffer *buffer, HDC target, const RECT *rc);
void     Theme_BufferEnd(ThemeBuffer *buffer);
void     Theme_SetScrollRow(HWND control, int rowPx);   /* how far a wheel line scrolls, 0: one row of the control */
int      Theme_ScrollTarget(HWND window, WPARAM request, int linePx);   /* the position a WM_VSCROLL asks of a window scrolled by the pixel */
HWND     Theme_SmoothView(HWND control);   /* a list or tree scrolled by the pixel; returns the view, which has its place */
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
const WCHAR *Theme_ProfileRole(BOOL stock, BOOL isDefault);
/* The version label's text in each state (its one %s: this build, the
 * release available, the one downloading), the status in each state (its
 * %s: Claude Desktop's version), the note under the column's actions (its
 * %s: the profile the regular Claude icon opens) and the sessions details'
 * captions, also measured with the window: catalog keys. */
typedef enum MainVersion { MAIN_VERSION_BUILD, MAIN_VERSION_AVAILABLE, MAIN_VERSION_DOWNLOADING, MAIN_VERSION_INSTALLING, MAIN_VERSIONS } MainVersion;
typedef enum MainStatus { MAIN_STATUS_NO_CLAUDE, MAIN_STATUS_NO_LINKS, MAIN_STATUS_ROUTED, MAIN_STATUSES } MainStatus;
typedef enum SessionsCaption { SESSIONS_ACTIONS, SESSIONS_DELETE_EVERYWHERE, SESSIONS_CAPTIONS } SessionsCaption;
const WCHAR *Theme_MainVersion(MainVersion state);
const WCHAR *Theme_MainStatus(MainStatus state);
void         Theme_DrawProgress(HWND owner, HDC dc, const RECT *rc, int done, int total, const WCHAR *text);   /* a sync's, in the column's foot */
const WCHAR *Theme_MainNote(void);
const WCHAR *Theme_SessionsCaption(SessionsCaption caption);
BOOL     Theme_ColumnResizeIsManual(HWND list, int column);   /* the user sized it (a header divider dragged or double-clicked) */
void     Theme_SetColumnWidth(HWND list, int column, int width);
void     Theme_LayoutSidebarNote(HWND note, const WCHAR *format, const WCHAR *name);
INT_PTR  Theme_CtlColor(UINT msg, WPARAM wp, LPARAM lp, int mutedId);
BOOL     Ui_Ask(HWND owner, LPCWSTR icon, const WCHAR *text, const WCHAR *ok, const WCHAR *cancel, BOOL defaultCancel);
int      Ui_Message(HWND owner, UINT flags, const WCHAR *format, ...);   /* MessageBox look-alike, `format` translated */
INT_PTR  Ui_Dialog(HWND owner, int id, DLGPROC proc, LPARAM param);   /* every dialog opens here */

/* ------------------------------------------------ router.c / main.c / gui.c */

int     Router_Run(const WCHAR *rawUrl);
int     Launcher_Run(const WCHAR *folder);
HRESULT Launcher_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *url, DWORD *pid, BOOL *identity);
HRESULT Launcher_OpenSynced(const ClaudePackage *pkg, const Profile *p, DWORD *pid, BOOL *identity);   /* sessions already made the same */
typedef enum GuiStart { GUI_MANAGER, GUI_SET_UP_LINKS, GUI_UNINSTALL } GuiStart;
int     Gui_Run(GuiStart start);
BOOL    Gui_MainWindowGeometry(HWND dialog, UINT message, WPARAM wp, LPARAM lp);
void    Gui_LayoutProfileColumns(HWND list);
void    Gui_ShowSessions(HWND dialog, const ClaudePackage *package, BOOL showSessions, const WCHAR *folder);

#endif
