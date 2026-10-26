/*
 * Settings, from the gear at the toolbar's end, laid out as Claude lays out
 * its own: the kinds on the left under a search box, and on the right the
 * page of the kind chosen, a row a setting, its name and what it does on
 * the left and its switch, drop-down, field or button on the right:
 *   - the program's own: its language and the size of its windows
 *     (Theme_Zoom), profiles opened again after a Claude update (taskbar.c),
 *     the browser private sign-in starts with and whether its window serves
 *     DevTools (signin.c, webview.c), how long the session vault keeps the
 *     versions of each list (sessionvault.c);
 *   - what a new profile is made with (gui.c): Windows MCP, SearXNG search
 *     and its address, Cowork off;
 *   - Claude Code's, in its settings.json, which every profile's Code and the
 *     terminal read (and take at once): its environment's switches, how long
 *     it keeps conversations, its permission mode; its sessions cleaned up and
 *     its folder copied (sessionpurge.c), by hand or once a week;
 *   - Claude's preferences in every profile, written while its Claude is
 *     closed, as Claude keeps them in memory (Gui_SetPreferences).
 * Each row also says how far the setting reaches and how much it changes.
 * What changes is kept with OK or Apply. Help and the changelog are in
 * About (about.c).
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <stdlib.h>

#define DAYS_MAX       3650    /* the vault's: ten years; past that, keep them all (0) */
#define CODE_DAYS_MAX  99999   /* Claude Code's days, as many as five digits write */
#define DEVTOOLS_PORT  9222    /* the sign-in window's DevTools port when the switch turns it on */
#define CODE_MAX       (4 * 1024 * 1024)   /* Claude Code's settings.json, read at most */
#define RAW_CCH        64      /* a preference's JSON value, as read */
#define KEYS_MAX       4       /* a member's path in settings.json, its keys at most */
#define KEY_CCH        64
#define LANGUAGES_MAX  8       /* the program's languages, at most */
#define WM_APP_SETTINGS_ACTION  (WM_APP + 0x110)   /* an action's button clicked, done once the click is over */
#define WM_APP_PAGE_EDITED      (WM_APP + 0x111)   /* to the page: its field's edit lost the focus, and goes */

/* The page, and the kinds beside it, in DIPs. */
#define PAGE_CLASS              L"ClaudeDelta.SettingsPage"
#define KINDS_CLASS             L"ClaudeDelta.SettingsKinds"
#define PAGE_MARGIN_DIPS        14    /* around what the page shows */
#define PAGE_ROW_INSET_DIPS     6     /* a row's box past the margin, for its focus cue */
#define PAGE_ROW_PAD_DIPS       7     /* above and below a setting's texts */
#define PAGE_LINE_GAP_DIPS      1     /* between a setting's name and what it does */
#define PAGE_CONTROL_GAP_DIPS   16    /* between a setting's texts and its control */
#define PAGE_CONTROL_PAD_DIPS   10    /* a control's height past its text's */
#define PAGE_TITLE_GAP_DIPS     2     /* below the page's title */
#define PAGE_SECTION_DIPS       14    /* above a section's name */
#define PAGE_SECTION_GAP_DIPS   2     /* below it */
#define PAGE_NOTE_GAP_DIPS      12    /* between a setting's name and its note, on one line */
#define PAGE_ICON_GAP_DIPS      6     /* after an icon */
#define KIND_ROW_DIPS           40    /* a kind's row */
#define KIND_ROW_GAP_DIPS       2     /* between two kinds' rows */
#define KIND_PAD_DIPS           12    /* before a kind's icon */
#define PAGE_CHOICE_MAX_DIPS    220   /* a drop-down, at most */
#define PAGE_FIELD_DIPS         200   /* a text's field */
#define PAGE_DAYS_DIPS          80    /* a number of days' field */
#define PAGE_BUTTON_PAD_DIPS    16    /* beside an action's caption */
#define PAGE_TEXT_MIN_DIPS      120   /* a setting's texts, however wide its control */
#define EDITOR_ID               1

static const int kZooms[] = { 90, 100, 110, 125, 150, 175, 200 };   /* the sizes offered, in percent */
#define ZOOMS ((int)ARRAYSIZE(kZooms))

/* The kinds, on the left, each a page; the items, a section of a page each (a bit each). */
enum { SG_GENERAL, SG_NEW, SG_CODE, SG_DESKTOP, SG_COUNT };
#define SI_UPDATES     0x0001u
#define SI_SIGNIN      0x0002u
#define SI_VAULT       0x0004u
#define SI_MCP         0x0008u
#define SI_SEARXNG     0x0010u
#define SI_COWORK      0x0020u
#define SI_SESSIONS    0x0040u
#define SI_PRIVACY     0x0080u
#define SI_FEATURES    0x0100u
#define SI_GIT         0x0200u
#define SI_PERMISSIONS 0x0400u
#define SI_BROWSER     0x0800u
#define SI_CODE_TAB    0x1000u
#define SI_POWER       0x2000u
#define SI_INTERFACE   0x4000u

typedef struct SettingsKind { DWORD items; const WCHAR *name; WCHAR glyph; ThemeTint tint; } SettingsKind;
static const SettingsKind kKinds[SG_COUNT] = {
    { SI_INTERFACE | SI_UPDATES | SI_SIGNIN | SI_VAULT, L"General", 0xE713, THEME_TINT_BLUE },
    { SI_MCP | SI_SEARXNG | SI_COWORK, L"New profiles", 0xE710, THEME_TINT_GREEN },
    { SI_SESSIONS | SI_PRIVACY | SI_FEATURES | SI_GIT | SI_PERMISSIONS, L"Claude Code", 0xE943, THEME_TINT_PURPLE },
    { SI_BROWSER | SI_CODE_TAB | SI_POWER, L"Claude Desktop", 0xE7F4, THEME_TINT_TEAL },
};
/* The sections, in the order the pages show them, each under its icon and name. */
typedef struct SettingsSection { DWORD item; const WCHAR *name; WCHAR glyph; ThemeTint tint; } SettingsSection;
static const SettingsSection kSections[] = {
    { SI_INTERFACE, L"Interface", 0xE771, THEME_TINT_BLUE },
    { SI_UPDATES, L"Claude updates", 0xE777, THEME_TINT_BLUE },
    { SI_SIGNIN, L"Private sign-in", 0xE72E, THEME_TINT_TEAL },
    { SI_VAULT, L"Session vault", 0xE81C, THEME_TINT_AMBER },
    { SI_MCP, L"Windows MCP", 0xE7F8, THEME_TINT_BLUE },
    { SI_SEARXNG, L"SearXNG search", 0xE721, THEME_TINT_TEAL },
    { SI_COWORK, L"Cowork", 0xE716, THEME_TINT_PURPLE },
    { SI_SESSIONS, L"Conversations", 0xE8BD, THEME_TINT_AMBER },
    { SI_PRIVACY, L"Privacy and network", 0xEA18, THEME_TINT_GREEN },
    { SI_FEATURES, L"Features", 0xE945, THEME_TINT_GOLD },
    { SI_GIT, L"Git and pull requests", 0xE8AB, THEME_TINT_PURPLE },
    { SI_PERMISSIONS, L"Permissions", 0xE8D7, THEME_TINT_RED },
    { SI_BROWSER, L"Cowork and the browser", 0xE774, THEME_TINT_BLUE },
    { SI_CODE_TAB, L"Code tab", 0xE756, THEME_TINT_PURPLE },
    { SI_POWER, L"Power and notifications", 0xE7E8, THEME_TINT_GREEN },
};

/* How far a setting reaches, and how much it changes: each impact's icon before its note. */
enum { SCOPE_PROGRAM, SCOPE_SIGNIN, SCOPE_NEW, SCOPE_MCP, SCOPE_CODE, SCOPE_DESKTOP, SCOPES };
enum { IMPACT_MINIMAL, IMPACT_SMALL, IMPACT_MEDIUM, IMPACT_LARGE, IMPACTS };
static const WCHAR kImpactGlyphs[IMPACTS] = { 0xE946, 0xE946, 0xE7BA, 0xE7BA };
static const ThemeTint kImpactTints[IMPACTS] = { THEME_TINT_GREEN, THEME_TINT_TEAL, THEME_TINT_AMBER, THEME_TINT_RED };
static const WCHAR *const kScopeNames[SCOPES] = { L"This program", L"Sign-in window", L"New profiles", L"Windows MCP", L"Claude Code",
                                                  L"All profiles" };
static const WCHAR *const kImpactNames[IMPACTS] = { L"minimal", L"small", L"medium", L"large" };

/* Where a setting is kept. */
typedef enum SettingStore {
    STORE_LANGUAGE,      /* the program's language (Localize), a choice: Windows' first */
    STORE_ZOOM,          /* the size of its windows (SETTING_ZOOM), a choice */
    STORE_INI,           /* settings.ini's `name`, a switch: on unless 0, `fallback` while unset */
    STORE_BROWSER,       /* private sign-in's browser (settings.ini), a choice */
    STORE_DEVTOOLS,      /* the sign-in window's DevTools port (settings.ini), a switch */
    STORE_VAULT_DAYS,    /* the vault's days (settings.ini), a text */
    STORE_SEARXNG,       /* SearXNG search's address (settings.ini's section), a text */
    STORE_ACTION,        /* `action`, for the dialog's owner to do */
    STORE_ENV,           /* settings.json's environment variable `key` (under "env"): `on` written, read as `match` says */
    STORE_JSON,          /* settings.json's member `key`: on while it is `on` */
    STORE_CODE_DAYS,     /* settings.json's cleanupPeriodDays, a text */
    STORE_CODE_MODE,     /* settings.json's permissions.defaultMode, a choice */
    STORE_ATTRIBUTION,   /* settings.json's attribution of commits and pull requests, both empty */
    STORE_DESKTOP,       /* every profile's preference `key`: `on` or `off`, `fallback` Claude's default */
} SettingStore;
/* How Claude Code reads an environment variable's switch: set at all, true, or false (Core_EnvSwitch). */
enum { ENV_SET, ENV_TRUE, ENV_FALSE };

typedef struct SettingDef {
    DWORD        item;
    SettingStore store;
    const WCHAR *label, *effect;   /* catalog keys: its name, and what it does */
    int          scope, impact;
    const WCHAR *name;             /* STORE_INI's */
    const char  *key;              /* the member, variable or preference, a member's path with dots */
    const char  *on, *off;         /* the JSON written: on (STORE_ENV's: EnvOn), and off (NULL: taken out) */
    int          match;            /* STORE_ENV's: ENV_* */
    int          fallback;         /* on (1) or off (0) while nothing is set */
    int          action;           /* STORE_ACTION's: IDC_G_* */
} SettingDef;

#define INI(item, label, effect, scope, impact, name, fallback) \
    { item, STORE_INI, label, effect, scope, impact, name, NULL, NULL, NULL, 0, fallback, 0 }
#define OWN(item, store, label, effect, scope, impact) { item, store, label, effect, scope, impact, NULL, NULL, NULL, NULL, 0, 0, 0 }
#define ACT(item, label, effect, scope, impact, action) \
    { item, STORE_ACTION, label, effect, scope, impact, NULL, NULL, NULL, NULL, 0, 0, action }
#define ENV(item, label, effect, impact, key, match) \
    { item, STORE_ENV, label, effect, SCOPE_CODE, impact, NULL, "env." key, NULL, NULL, match, 0, 0 }
#define CODE(item, store, label, effect, impact, key, on) { item, store, label, effect, SCOPE_CODE, impact, NULL, key, on, NULL, 0, 0, 0 }
#define PREF(item, label, effect, impact, key, on, off, fallback) \
    { item, STORE_DESKTOP, label, effect, SCOPE_DESKTOP, impact, NULL, key, on, off, 0, fallback, 0 }

static const SettingDef kSettings[] = {
    OWN(SI_INTERFACE, STORE_LANGUAGE, L"Language", L"The language of this program's windows.", SCOPE_PROGRAM, IMPACT_MINIMAL),
    OWN(SI_INTERFACE, STORE_ZOOM, L"Zoom", L"How large this program's windows are; the main window takes it once Settings closes.",
        SCOPE_PROGRAM, IMPACT_SMALL),
    INI(SI_UPDATES, L"Open profiles again after a Claude update", L"A profile that a Claude update closed opens again once the update is in.",
        SCOPE_PROGRAM, IMPACT_SMALL, SETTING_REOPEN_AFTER_UPDATE, 1),
    OWN(SI_SIGNIN, STORE_BROWSER, L"Browser", L"Where private sign-in opens claude.ai: the program's own window, or a browser in private mode.",
        SCOPE_SIGNIN, IMPACT_SMALL),
    OWN(SI_SIGNIN, STORE_DEVTOOLS, L"DevTools port (CDP)", L"The sign-in window serves DevTools on port 9222 of this PC only, so that tools can drive it.",
        SCOPE_SIGNIN, IMPACT_MINIMAL),
    OWN(SI_VAULT, STORE_VAULT_DAYS, L"Days to keep versions", L"How long the vault keeps the earlier versions of each list of sessions. 0 keeps them all.",
        SCOPE_PROGRAM, IMPACT_SMALL),
    INI(SI_MCP, L"Set up in new profiles", L"New profiles get Windows MCP, which lets Claude see and use this PC's desktop. It needs uv.",
        SCOPE_NEW, IMPACT_MEDIUM, SETTING_NEW_WINDOWS_MCP, 1),
    INI(SI_MCP, L"Read and search claude.ai chats", L"Windows MCP may read and search this account's chats on claude.ai.",
        SCOPE_MCP, IMPACT_MEDIUM, SETTING_MCP_ONLINE, 0),
    INI(SI_SEARXNG, L"Set up in new profiles", L"New profiles search the web through SearXNG. It needs Node.js.",
        SCOPE_NEW, IMPACT_SMALL, SETTING_NEW_SEARXNG, 1),
    OWN(SI_SEARXNG, STORE_SEARXNG, L"Address", L"The SearXNG instance new profiles search with. Empty: searxng.cheese.ink.", SCOPE_NEW, IMPACT_SMALL),
    INI(SI_COWORK, L"Turn off in new profiles", L"New profiles start without Cowork, and its virtual machine (several GB) is never downloaded.",
        SCOPE_NEW, IMPACT_MEDIUM, SETTING_NEW_COWORK_OFF, 1),
    CODE(SI_SESSIONS, STORE_CODE_DAYS, L"Days to keep conversations",
         L"Claude Code deletes conversations older than this when it starts, without asking. Empty: its default, 30.",
         IMPACT_LARGE, "cleanupPeriodDays", NULL),
    ACT(SI_SESSIONS, L"Clean up sessions", L"Pick sessions of Claude Code to delete, or bring back those deleted.", SCOPE_CODE, IMPACT_MEDIUM,
        IDC_G_PURGE),
    ACT(SI_SESSIONS, L"Back up .claude", L"Copies Claude Code's .claude folder beside it now.", SCOPE_CODE, IMPACT_MINIMAL, IDC_G_CODE_COPY),
    INI(SI_SESSIONS, L"Back up once a week", L"Once a week, the program copies the .claude folder beside it.",
        SCOPE_PROGRAM, IMPACT_MINIMAL, SETTING_WEEKLY_BACKUP, 0),
    ENV(SI_PRIVACY, L"Turn off telemetry", L"Claude Code stops sending usage data. Features it turns on step by step may stay off.",
        IMPACT_LARGE, "DISABLE_TELEMETRY", ENV_SET),
    ENV(SI_PRIVACY, L"Turn off error reporting", L"Claude Code stops sending crash and error reports.", IMPACT_SMALL, "DISABLE_ERROR_REPORTING",
        ENV_SET),
    ENV(SI_PRIVACY, L"Turn off nonessential traffic",
        L"Telemetry, error reporting, automatic updates and feedback surveys off together; what needs them stops working.",
        IMPACT_LARGE, "CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC", ENV_SET),
    ENV(SI_PRIVACY, L"Turn off feedback surveys", L"Claude Code stops asking how a session went.", IMPACT_MINIMAL,
        "CLAUDE_CODE_DISABLE_FEEDBACK_SURVEY", ENV_TRUE),
    ENV(SI_PRIVACY, L"Turn off automatic updates", L"Claude Code in the terminal stops updating itself: update it by hand.", IMPACT_SMALL,
        "DISABLE_AUTOUPDATER", ENV_TRUE),
    ENV(SI_FEATURES, L"Turn off auto memory", L"Claude Code stops keeping notes of its own on your projects between sessions.", IMPACT_MEDIUM,
        "CLAUDE_CODE_DISABLE_AUTO_MEMORY", ENV_TRUE),
    ENV(SI_FEATURES, L"Turn off the 1M-token context", L"Models keep their standard context: long sessions are compacted sooner, and cost less.",
        IMPACT_MEDIUM, "CLAUDE_CODE_DISABLE_1M_CONTEXT", ENV_TRUE),
    ENV(SI_FEATURES, L"Turn off claude.ai connectors",
        L"Connectors added on claude.ai (Gmail, Google Drive and others) are not offered to Claude Code.", IMPACT_MEDIUM,
        "ENABLE_CLAUDEAI_MCP_SERVERS", ENV_FALSE),
    ENV(SI_FEATURES, L"Agent teams (experimental)", L"A session can start a team of agents that work together. It uses more of your usage.",
        IMPACT_SMALL, "CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS", ENV_TRUE),
    ENV(SI_FEATURES, L"Stay in the project folder", L"Each shell command starts in the project's folder again, wherever the last one went.",
        IMPACT_SMALL, "CLAUDE_BASH_MAINTAIN_PROJECT_WORKING_DIR", ENV_TRUE),
    ENV(SI_FEATURES, L"Hide cost warnings", L"Claude Code no longer warns about what a session costs.", IMPACT_MINIMAL, "DISABLE_COST_WARNINGS",
        ENV_TRUE),
    CODE(SI_FEATURES, STORE_JSON, L"Hide tips while Claude works", L"No tips beside the spinner while Claude works.", IMPACT_MINIMAL,
         "spinnerTipsEnabled", "false"),
    CODE(SI_GIT, STORE_ATTRIBUTION, L"Leave Claude out of commits and pull requests",
         L"Commits and pull requests no longer say they were made with Claude Code: no Co-Authored-By line, no link.", IMPACT_SMALL,
         "attribution", NULL),
    ENV(SI_GIT, L"Turn off built-in git instructions",
        L"Claude Code's own instructions for commits and pull requests are left out: it follows yours alone.", IMPACT_MEDIUM,
        "CLAUDE_CODE_DISABLE_GIT_INSTRUCTIONS", ENV_TRUE),
    CODE(SI_PERMISSIONS, STORE_CODE_MODE, L"Permission mode at start",
         L"The mode new sessions start in. Auto and Bypass permissions let Claude act without asking.", IMPACT_LARGE,
         "permissions.defaultMode", NULL),
    CODE(SI_PERMISSIONS, STORE_JSON, L"Approve every project MCP server",
         L"Servers that a project's .mcp.json lists start without asking. Only for projects you trust.", IMPACT_LARGE,
         "enableAllProjectMcpServers", "true"),
    PREF(SI_BROWSER, L"Cowork web search", L"Cowork can search the web.", IMPACT_MEDIUM, "coworkWebSearchEnabled", "true", "false", 1),
    PREF(SI_BROWSER, L"Cowork browser tools", L"Cowork can open and use a browser.", IMPACT_MEDIUM, "coworkBrowserToolsEnabled", "true", "false", 1),
    PREF(SI_BROWSER, L"Claude in Chrome", L"Claude can work in Chrome through its extension.", IMPACT_MEDIUM, "chromeExtensionEnabled", "true",
         "false", 1),
    PREF(SI_BROWSER, L"Allow all browser actions", L"Claude acts in the browser on every site without asking first.", IMPACT_LARGE,
         "allowAllBrowserActions", "true", "false", 0),
    PREF(SI_BROWSER, L"Cowork scheduled tasks", L"Cowork can run tasks on a schedule while Claude is open.", IMPACT_SMALL,
         "coworkScheduledTasksEnabled", "true", "false", 0),
    PREF(SI_CODE_TAB, L"Code scheduled tasks", L"Code sessions can run on a schedule while Claude is open.", IMPACT_SMALL,
         "ccdScheduledTasksEnabled", "true", "false", 0),
    PREF(SI_CODE_TAB, L"Allow bypass permissions mode", L"Bypass permissions can be picked in Code: Claude then runs every tool without asking.",
         IMPACT_LARGE, "bypassPermissionsModeEnabled", "true", "false", 0),
    PREF(SI_CODE_TAB, L"Prompt suggestions", L"Code suggests what to type next.", IMPACT_MINIMAL, "ccPromptSuggestions", "true", "false", 1),
    PREF(SI_CODE_TAB, L"Archive when the PR closes", L"A Code session is archived once its pull request is merged or closed.", IMPACT_SMALL,
         "ccAutoArchiveOnPrClose", "true", "false", 0),
    PREF(SI_POWER, L"Keep the computer awake while Claude works", L"Windows does not sleep while a Code session works.", IMPACT_SMALL,
         "ccKeepAwakeWhileWorking", "true", "false", 1),
    PREF(SI_POWER, L"Keep it awake on battery too", L"The same on battery power, which runs it down sooner.", IMPACT_SMALL,
         "ccKeepAwakeOnBattery", "true", "false", 1),
    PREF(SI_POWER, L"Notification sound", L"Claude's notifications play a sound.", IMPACT_MINIMAL, "notificationSound", "\"system\"", "\"none\"",
         1),
};
#define SETTINGS_COUNT ((int)ARRAYSIZE(kSettings))
#define PAGE_ROWS_MAX  (SETTINGS_COUNT + (int)ARRAYSIZE(kSections) + 2)

/* Claude Code's permission modes, as settings.json writes them; the first: none set. */
static const char *const kModes[] = { NULL, "default", "acceptEdits", "plan", "auto", "dontAsk", "bypassPermissions" };
static const WCHAR *const kModeNames[ARRAYSIZE(kModes)] = { L"Not set", L"Ask permissions", L"Accept edits", L"Plan mode", L"Auto",
                                                           L"Don't ask", L"Bypass permissions" };
#define MODES ((int)ARRAYSIZE(kModes))

/* A row of the page: its title, a section's name, a setting, or what a search found none of. */
enum { ROW_TITLE, ROW_SECTION, ROW_SETTING, ROW_EMPTY };
typedef struct PageRow {
    int          kind;
    int          v;                             /* ROW_SETTING: its value; ROW_SECTION: its section; else -1 */
    const WCHAR *text;                          /* the title's, the section's or the empty page's words, translated */
    RECT         box;                           /* in the page */
    RECT         name, effect, note, control;   /* ROW_SETTING's texts (its note after its icon) and its control */
} PageRow;

/* The dialog's state, in DWLP_USER; the page's too. */
typedef struct SettingsDialog {
    SettingsLanguageProc applyLanguage;   /* the program takes a new language; the dialog follows it itself */
    SettingsActionProc   act;             /* an action, or the windows' size changed */
    CheckValue           values[SETTINGS_COUNT];   /* one a setting, as kSettings */
    BOOL                 savedOn[SETTINGS_COUNT];  /* a switch, as saved */
    int                  savedChoice[SETTINGS_COUNT];
    WCHAR                savedText[SETTINGS_COUNT][CHECK_VALUE_CCH];
    int                  onIn[SETTINGS_COUNT];     /* a preference: the profiles it is on in */
    BOOL                 touched[SETTINGS_COUNT];  /* a preference on in some profiles only, turned over since */
    int                  profiles;                 /* the profiles read */
    char                *code;                     /* Claude Code's settings.json as read; NULL: none */
    DWORD                codeLength;
    BOOL                 codeLocked;               /* there, but not a JSON object read whole: left as it is */
    int                  zoom;                     /* the windows' size, as saved */
    SignInBrowsers       browsers;                 /* those offered */
    const WCHAR         *browserNames[SIGNIN_BROWSERS];
    const WCHAR         *modeNames[MODES];
    const WCHAR         *languageNames[LANGUAGES_MAX + 1];
    WCHAR                zoomTexts[ZOOMS][8];
    const WCHAR         *zoomNames[ZOOMS];
    /* The window: its kinds on the left, the page on the right (in its view), both drawn here. */
    HWND                 dialog, kinds, page, editor;
    int                  kind;                     /* the page's kind; its rows are a search's while `search` holds text */
    int                  hotKind;                  /* the kind under the mouse; -1 none */
    WCHAR                search[64];
    PageRow              rows[PAGE_ROWS_MAX];
    int                  rowCount;
    int                  focus;                    /* the keyboard's row, a setting's; -1 none */
    int                  hot;                      /* the row whose control is under the mouse; -1 none */
    int                  editing;                  /* the row whose field the editor is over; -1 none */
    WCHAR                editStart[CHECK_VALUE_CCH];   /* its text before, which Escape gives back */
    int                  width, height;            /* the page's width the rows are laid out for, and their height */
    int                  controlHeight;
    ThemeFonts           fonts;
    UINT                 fontDpi;                  /* the DPI and the language the fonts were made for */
    int                  fontLanguage;
} SettingsDialog;

/* A saved DevTools port that serves (one that does not is off). */
static BOOL DevToolsOn(DWORD port)
{
    return port > 0 && port < 65536;
}

/* The size offered nearest to `zoom`'s place: its own, else 100%. */
static int ZoomIndex(int zoom)
{
    int i;
    for (i = 0; i < ZOOMS; i++)
        if (kZooms[i] == zoom) return i;
    for (i = 0; i < ZOOMS; i++)
        if (kZooms[i] == 100) return i;
    return 0;
}

/* ------------------------------------------------------------ settings.json */

/* `path`'s keys, split at its dots: how many. */
static int SplitPath(const char *path, char (*keys)[KEY_CCH], const char **names)
{
    int depth = 0;
    size_t length;
    while (*path && depth < KEYS_MAX) {
        length = strcspn(path, ".");
        if (length >= KEY_CCH) return 0;
        memcpy(keys[depth], path, length);
        keys[depth][length] = 0;
        names[depth] = keys[depth];
        depth++;
        path += length;
        if (*path == '.') path++;
    }
    return depth;
}

/* Member `path` of `json`: its raw value. */
static BOOL MemberAt(const char *json, size_t length, const char *path, const char **raw, size_t *rawLength)
{
    char keys[KEYS_MAX][KEY_CCH];
    const char *names[KEYS_MAX];
    int depth = SplitPath(path, keys, names), i;
    if (!json || !depth) return FALSE;
    for (i = 0; i < depth; i++)
        if (!Core_JsonMember(json, length, names[i], &json, &length)) return FALSE;
    *raw = json;
    *rawLength = length;
    return TRUE;
}

static BOOL RawIs(const char *raw, size_t length, const char *text)
{
    return text && length == strlen(text) && memcmp(raw, text, length) == 0;
}

/* `json` (a heap block, or NULL for "{}") with member `path` set to `raw`, or taken out (NULL); FALSE, and `json` as
 * it was, when it cannot be. */
static BOOL SetMember(char **json, size_t *length, const char *path, const char *raw)
{
    char keys[KEYS_MAX][KEY_CCH];
    const char *names[KEYS_MAX];
    int depth = SplitPath(path, keys, names);
    size_t outLength = 0;
    char *out;
    if (!depth || (out = Core_JsonSetPath(*json ? *json : "{}", *json ? *length : 2, names, depth, raw, &outLength)) == NULL) return FALSE;
    if (*json) HeapFree(GetProcessHeap(), 0, *json);
    *json = out;
    *length = outLength;
    return TRUE;
}

/* Claude Code's settings.json, read: none there is no settings; one that is not a JSON object read whole is not
 * changed. */
static char *ReadCode(DWORD *length, BOOL *locked)
{
    WCHAR path[LONG_PATH_CCH];
    WIN32_FILE_ATTRIBUTE_DATA file;
    char *json;
    size_t at = 0;
    *length = 0;
    *locked = FALSE;
    if (!SessionStore_ClaudeCodePath(L"settings.json", path, ARRAYSIZE(path))) {
        *locked = TRUE;
        return NULL;
    }
    if ((json = Util_ReadFile(path, CODE_MAX, FALSE, length)) == NULL) {
        *locked = GetFileAttributesExW(path, GetFileExInfoStandard, &file) && (file.nFileSizeHigh || file.nFileSizeLow);
        return NULL;
    }
    while (at < *length && (json[at] == ' ' || json[at] == '\t' || json[at] == '\r' || json[at] == '\n')) at++;
    if (at == *length) {   /* empty: as none */
        Util_Free(json);
        *length = 0;
        return NULL;
    }
    if (json[at] != '{' || !Core_JsonIsValue(json, *length)) {
        Util_Free(json);
        *locked = TRUE;
        return NULL;
    }
    return json;
}

/* --------------------------------------------------------------- the values */

/* Setting `def` is kept in Claude Code's settings.json. */
static BOOL InCode(const SettingDef *def)
{
    return def->store == STORE_ENV || def->store == STORE_JSON || def->store == STORE_CODE_DAYS || def->store == STORE_CODE_MODE ||
           def->store == STORE_ATTRIBUTION;
}

/* What turns environment switch `def` on: "false" for one Claude Code turns off with it, else "1". */
static const char *EnvOn(const SettingDef *def)
{
    return def->match == ENV_FALSE ? "\"false\"" : "\"1\"";
}

/* What switch `def` is in Claude Code's settings now. */
static BOOL CodeSwitchOn(const SettingsDialog *state, const SettingDef *def)
{
    const char *raw;
    size_t length;
    if (def->store == STORE_ATTRIBUTION) {
        const char *pr;
        size_t prLength;
        if (MemberAt(state->code, state->codeLength, "attribution", &raw, &length) && RawIs(raw, length, "false")) return TRUE;
        if (MemberAt(state->code, state->codeLength, "includeCoAuthoredBy", &raw, &length) && RawIs(raw, length, "false")) return TRUE;
        return MemberAt(state->code, state->codeLength, "attribution.commit", &raw, &length) && RawIs(raw, length, "\"\"") &&
               MemberAt(state->code, state->codeLength, "attribution.pr", &pr, &prLength) && RawIs(pr, prLength, "\"\"");
    }
    if (!MemberAt(state->code, state->codeLength, def->key, &raw, &length)) return FALSE;
    if (def->store == STORE_JSON) return RawIs(raw, length, def->on);
    return def->match == ENV_SET ? TRUE : Core_EnvSwitch(raw, length) == (def->match == ENV_TRUE ? 1 : 0);
}

/* The profiles that set preference `def` on, read from each: how many, and of them all (`state->profiles`). */
static int PreferenceOnIn(const SettingsDialog *state, const SettingDef *def)
{
    const ProfileList *profiles = Gui_Profiles();
    char raw[RAW_CCH];
    int i, on = 0;
    for (i = 0; i < state->profiles; i++) {
        if (!Prefs_Get(&profiles->items[i], def->key, raw, ARRAYSIZE(raw))) on += def->fallback ? 1 : 0;
        else on += strcmp(raw, def->off) != 0 ? 1 : 0;
    }
    return on;
}

/* Value `v` as setting `def` stands now, read: its saved state the same. */
static void ReadValue(SettingsDialog *state, int v)
{
    const SettingDef *def = &kSettings[v];
    CheckValue *value = &state->values[v];
    const char *raw;
    size_t length;
    int i;
    value->readOnly = FALSE;
    switch (def->store) {
    case STORE_LANGUAGE:
        value->choice = Localize_CurrentLanguage() + 1;   /* Windows' language, the first */
        break;
    case STORE_ZOOM:
        value->choice = ZoomIndex(state->zoom);
        break;
    case STORE_INI:
        value->on = Util_GetSetting(def->name, (DWORD)def->fallback) != 0;
        break;
    case STORE_DEVTOOLS:
        value->on = DevToolsOn(Util_GetSetting(SETTING_SIGNIN_DEVTOOLS, 0));
        break;
    case STORE_BROWSER: {
        int kind = (int)Util_GetSetting(SETTING_SIGNIN_BROWSER, SIGNIN_BROWSER_OWN);
        value->choice = 0;   /* the one saved is gone: the first */
        for (i = 0; i < state->browsers.count; i++)
            if (state->browsers.kind[i] == kind) value->choice = i;
        break;
    }
    case STORE_VAULT_DAYS:
        StringCchPrintfW(value->text, ARRAYSIZE(value->text), L"%lu", Util_GetSetting(SETTING_VAULT_DAYS, SETTING_VAULT_DAYS_DEFAULT));
        break;
    case STORE_SEARXNG: {
        WCHAR saved[1][MAX_PATH];
        value->text[0] = 0;
        if (Util_GetSettingList(SETTING_SEARXNG_SECTION, saved, 1) == 1) StringCchCopyW(value->text, ARRAYSIZE(value->text), saved[0]);
        break;
    }
    case STORE_ACTION:
        break;
    case STORE_ENV:
    case STORE_JSON:
    case STORE_ATTRIBUTION:
        value->on = CodeSwitchOn(state, def);
        value->readOnly = state->codeLocked;
        break;
    case STORE_CODE_DAYS: {
        ULONGLONG days;
        value->text[0] = 0;
        if (MemberAt(state->code, state->codeLength, def->key, &raw, &length) && Core_JsonNumber(raw, length, &days))
            StringCchPrintfW(value->text, ARRAYSIZE(value->text), L"%llu", days);
        value->readOnly = state->codeLocked;
        break;
    }
    case STORE_CODE_MODE:
        value->choice = 0;
        if (MemberAt(state->code, state->codeLength, def->key, &raw, &length)) {
            for (i = 1; i < MODES; i++)
                if (length == strlen(kModes[i]) + 2 && raw[0] == '"' && memcmp(raw + 1, kModes[i], length - 2) == 0) value->choice = i;
            if (RawIs(raw, length, "\"manual\"")) value->choice = 1;   /* an older name of the default */
        }
        value->readOnly = state->codeLocked;
        break;
    case STORE_DESKTOP:
        state->onIn[v] = PreferenceOnIn(state, def);
        value->on = state->profiles > 0 && state->onIn[v] == state->profiles;
        value->readOnly = state->profiles == 0;
        state->touched[v] = FALSE;
        break;
    }
    state->savedOn[v] = value->on;
    state->savedChoice[v] = value->choice;
    StringCchCopyW(state->savedText[v], ARRAYSIZE(state->savedText[v]), value->text);
}

/* Value `v` differs from what is saved: a preference on in some profiles only, once turned over. */
static BOOL ValueChanged(const SettingsDialog *state, int v)
{
    const CheckValue *value = &state->values[v];
    switch (value->kind) {
    case CHECK_VALUE_SWITCH: return value->on != state->savedOn[v] || state->touched[v];
    case CHECK_VALUE_CHOICE: return value->choice != state->savedChoice[v];
    case CHECK_VALUE_TEXT:   return wcscmp(value->text, state->savedText[v]) != 0;
    default:                 return FALSE;
    }
}

/* The value of setting `store`, the first that is kept there. */
static int ValueOfStore(SettingStore store)
{
    int v;
    for (v = 0; v < SETTINGS_COUNT; v++)
        if (kSettings[v].store == store) return v;
    return -1;
}

/* The texts the values show in the interface's language: the choices, the actions' captions and the texts' hints. */
static void ShowNotes(SettingsDialog *state)
{
    int v, i;
    for (i = 0; i < state->browsers.count; i++) state->browserNames[i] = SignIn_BrowserName(state->browsers.kind[i]);
    for (i = 0; i < MODES; i++) state->modeNames[i] = TR(kModeNames[i]);
    for (i = -1; i < Localize_LanguageCount() && i < LANGUAGES_MAX; i++) state->languageNames[i + 1] = Localize_LanguageName(i);
    for (i = 0; i < ZOOMS; i++) {
        StringCchPrintfW(state->zoomTexts[i], ARRAYSIZE(state->zoomTexts[i]), L"%d%%", kZooms[i]);
        state->zoomNames[i] = state->zoomTexts[i];
    }
    for (v = 0; v < SETTINGS_COUNT; v++) {
        const SettingDef *def = &kSettings[v];
        CheckValue *value = &state->values[v];
        if (value->kind == CHECK_VALUE_ACTION)
            StringCchCopyW(value->text, ARRAYSIZE(value->text), def->action == IDC_G_PURGE ? TR(L"Choose\x2026") : TR(L"Back up now\x2026"));
        else if (value->kind == CHECK_VALUE_TEXT)
            value->hint = def->store == STORE_SEARXNG ? SEARXNG_DEFAULT_URL + 8 : def->store == STORE_CODE_DAYS ? L"30" : NULL;   /* without https:// */
    }
}

/* The values, one a setting, read as it stands. */
static void FillValues(SettingsDialog *state)
{
    int v;
    state->profiles = Gui_Profiles()->count;
    state->code = ReadCode(&state->codeLength, &state->codeLocked);
    ShowNotes(state);
    for (v = 0; v < SETTINGS_COUNT; v++) {
        const SettingDef *def = &kSettings[v];
        CheckValue *value = &state->values[v];
        ZeroMemory(value, sizeof *value);
        value->item = def->item;
        value->label = def->label;
        value->pick = -1;
        value->data = v;
        switch (def->store) {
        case STORE_LANGUAGE:
            value->kind = CHECK_VALUE_CHOICE;
            value->choices = state->languageNames;
            value->choiceCount = min(Localize_LanguageCount(), LANGUAGES_MAX) + 1;
            break;
        case STORE_ZOOM:
            value->kind = CHECK_VALUE_CHOICE;
            value->choices = state->zoomNames;
            value->choiceCount = ZOOMS;
            break;
        case STORE_BROWSER:
            value->kind = CHECK_VALUE_CHOICE;
            value->choices = state->browserNames;
            value->choiceCount = state->browsers.count;
            break;
        case STORE_CODE_MODE:
            value->kind = CHECK_VALUE_CHOICE;
            value->choices = state->modeNames;
            value->choiceCount = MODES;
            break;
        case STORE_VAULT_DAYS:
        case STORE_CODE_DAYS:
            value->kind = CHECK_VALUE_TEXT;
            value->limit = def->store == STORE_VAULT_DAYS ? 4 : 5;
            break;
        case STORE_SEARXNG:
            value->kind = CHECK_VALUE_TEXT;
            value->limit = MAX_PATH - 1;
            break;
        case STORE_ACTION:
            value->kind = CHECK_VALUE_ACTION;
            break;
        default:
            value->kind = CHECK_VALUE_SWITCH;
            break;
        }
        ReadValue(state, v);
    }
    ShowNotes(state);
}

/* What setting `v` shows: a choice's name, a text (`*hint`: its hint, none typed), an action's caption. */
static void ValueText(const SettingsDialog *state, int v, WCHAR *out, size_t cch, BOOL *hint)
{
    const CheckValue *value = &state->values[v];
    WCHAR *tab;
    *hint = FALSE;
    out[0] = 0;
    switch (value->kind) {
    case CHECK_VALUE_CHOICE:
        /* What follows a tab is for a menu only (theme.c). */
        if (value->choice >= 0 && value->choice < value->choiceCount && value->choices[value->choice])
            StringCchCopyW(out, cch, value->choices[value->choice]);
        if ((tab = wcschr(out, L'\t')) != NULL) *tab = 0;
        break;
    case CHECK_VALUE_TEXT:
        if (value->text[0]) StringCchCopyW(out, cch, value->text);
        else if (value->hint) {
            StringCchCopyW(out, cch, value->hint);
            *hint = TRUE;
        }
        break;
    case CHECK_VALUE_ACTION:
        StringCchCopyW(out, cch, value->text);
        break;
    default:
        break;
    }
}

/* Below what setting `v` does: how far it reaches and how much it changes, and how it stands where that is not plain. */
static void NoteOf(const SettingsDialog *state, int v, WCHAR *out, size_t cch)
{
    const SettingDef *def = &kSettings[v];
    WCHAR more[256] = L"";
    StringCchPrintfW(out, cch, TR(L"%s \x00B7 %s impact"), TR(kScopeNames[def->scope]), TR(kImpactNames[def->impact]));
    if (def->store == STORE_DESKTOP && state->profiles == 0)
        StringCchCopyW(more, ARRAYSIZE(more), TR(L"There is no profile yet."));
    else if (def->store == STORE_DESKTOP && state->onIn[v] > 0 && state->onIn[v] < state->profiles && !state->touched[v])
        StringCchPrintfW(more, ARRAYSIZE(more), TR(L"On now in %d of %d profiles."), state->onIn[v], state->profiles);
    else if (InCode(def) && state->codeLocked)
        StringCchCopyW(more, ARRAYSIZE(more), TR(L"Claude Code's settings.json cannot be read: it is left as it is."));
    if (more[0]) {
        StringCchCatW(out, cch, L" \x00B7 ");
        StringCchCatW(out, cch, more);
    }
}

/* ---------------------------------------------------------------- the page */

static int Px(HWND window, int dips)
{
    UINT dpi = window ? GetDpiForWindow(window) : 0;
    return MulDiv(dips, dpi ? (int)dpi : 96, 96);
}

static HFONT Font(const SettingsDialog *state, int role)
{
    return state->fonts.font[role] ? state->fonts.font[role] : (HFONT)SendMessageW(state->dialog, WM_GETFONT, 0, 0);
}

/* The fonts at the page's scale, in the language's face: made again when either changed. */
static void EnsureFonts(SettingsDialog *state, BOOL again)
{
    UINT dpi = GetDpiForWindow(state->dialog);
    if (!again && state->fonts.font[THEME_FONT_TEXT] && state->fontDpi == dpi && state->fontLanguage == Localize_EffectiveLanguage()) return;
    Theme_CreateFonts(state->dialog, &state->fonts);
    state->fontDpi = dpi;
    state->fontLanguage = Localize_EffectiveLanguage();
    state->width = -1;   /* laid out again */
}

static int TextHeight(HDC dc, HFONT font, const WCHAR *text, int width)
{
    RECT rc = { 0, 0, 0, 0 };
    HGDIOBJ old = SelectObject(dc, font);
    rc.right = max(1, width);
    DrawTextW(dc, text, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | Localize_ReadingFlags());
    SelectObject(dc, old);
    return rc.bottom;
}

static int TextWidth(HDC dc, HFONT font, const WCHAR *text)
{
    RECT rc = { 0, 0, 0, 0 };
    HGDIOBJ old = SelectObject(dc, font);
    DrawTextW(dc, text, -1, &rc, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX | Localize_ReadingFlags());
    SelectObject(dc, old);
    return rc.right;
}

/* How wide setting `v`'s control is. */
static int ControlWidth(const SettingsDialog *state, HDC dc, int v)
{
    const CheckValue *value = &state->values[v];
    HWND page = state->page;
    HFONT font = Font(state, THEME_FONT_TEXT);
    int widest = 0, i;
    switch (value->kind) {
    case CHECK_VALUE_SWITCH:
        return 2 * Px(page, THEME_SWITCH_HEIGHT_DIPS);
    case CHECK_VALUE_CHOICE:
        for (i = 0; i < value->choiceCount; i++) {
            WCHAR choice[CHECK_VALUE_CCH], *tab;
            if (!value->choices[i]) continue;
            StringCchCopyW(choice, ARRAYSIZE(choice), value->choices[i]);
            if ((tab = wcschr(choice, L'\t')) != NULL) *tab = 0;
            widest = max(widest, Theme_DropDownWidth(page, font, choice));
        }
        return min(widest, Px(page, PAGE_CHOICE_MAX_DIPS));
    case CHECK_VALUE_TEXT:
        return Px(page, kSettings[v].store == STORE_SEARXNG ? PAGE_FIELD_DIPS : PAGE_DAYS_DIPS);
    case CHECK_VALUE_ACTION:
        return TextWidth(dc, font, value->text) + 2 * Px(page, PAGE_BUTTON_PAD_DIPS);
    default:
        return 0;
    }
}

/* Each row's place at the page's width, and the page's height. */
static void PageLayout(SettingsDialog *state)
{
    HWND page = state->page;
    RECT client;
    HDC dc;
    int margin, inset, pad, gap, line, right, y, i;
    if (!page || !GetClientRect(page, &client)) return;
    EnsureFonts(state, FALSE);
    if ((dc = GetDC(page)) == NULL) return;
    margin = Px(page, PAGE_MARGIN_DIPS);
    inset = Px(page, PAGE_ROW_INSET_DIPS);
    pad = Px(page, PAGE_ROW_PAD_DIPS);
    gap = Px(page, PAGE_CONTROL_GAP_DIPS);
    line = Px(page, PAGE_LINE_GAP_DIPS);
    right = max(margin + Px(page, PAGE_TEXT_MIN_DIPS), client.right - margin);
    state->width = client.right;
    state->controlHeight = TextHeight(dc, Font(state, THEME_FONT_TEXT), L"Ag", 1000) + Px(page, PAGE_CONTROL_PAD_DIPS);
    y = margin;
    for (i = 0; i < state->rowCount; i++) {
        PageRow *row = &state->rows[i];
        int height;
        switch (row->kind) {
        case ROW_TITLE:
            height = TextHeight(dc, Font(state, THEME_FONT_HEADING), row->text, right - margin);
            SetRect(&row->box, margin, y, right, y + height);
            y += height + Px(page, PAGE_TITLE_GAP_DIPS);
            break;
        case ROW_SECTION: {
            HFONT strong = Font(state, THEME_FONT_STRONG);
            int icon = Theme_InlineGlyphWidth(dc, kSections[row->v].glyph, strong) + Px(page, PAGE_ICON_GAP_DIPS);
            if (i > 0 && state->rows[i - 1].kind != ROW_TITLE) y += Px(page, PAGE_SECTION_DIPS);
            height = TextHeight(dc, strong, row->text, right - margin - icon);
            SetRect(&row->box, margin, y, right, y + height);
            y += height + Px(page, PAGE_SECTION_GAP_DIPS);
            break;
        }
        case ROW_EMPTY:
            height = TextHeight(dc, Font(state, THEME_FONT_TEXT), row->text, right - margin);
            SetRect(&row->box, margin, y, right, y + height);
            y += height;
            break;
        default: {
            const SettingDef *def = &kSettings[row->v];
            WCHAR note[512];
            int width = ControlWidth(state, dc, row->v), textRight = max(margin + Px(page, PAGE_TEXT_MIN_DIPS), right - width - gap);
            int nameWidth, noteWidth, nameHeight, effectHeight, noteHeight, inner, top, room = textRight - margin;
            HFONT text = Font(state, THEME_FONT_TEXT);
            BOOL beside;
            NoteOf(state, row->v, note, ARRAYSIZE(note));
            /* The note (its impact's icon first) on the name's line when both fit there; else below what it does. */
            nameWidth = TextWidth(dc, text, TR(def->label));
            noteWidth = Theme_InlineGlyphWidth(dc, kImpactGlyphs[def->impact], text) + Px(page, PAGE_ICON_GAP_DIPS) + TextWidth(dc, text, note);
            beside = nameWidth + Px(page, PAGE_NOTE_GAP_DIPS) + noteWidth <= room;
            nameHeight = TextHeight(dc, text, TR(def->label), room);
            effectHeight = TextHeight(dc, text, TR(def->effect), room);
            noteHeight = beside ? 0 : TextHeight(dc, text, note, room - (noteWidth - TextWidth(dc, text, note)));
            inner = nameHeight + line + effectHeight + (beside ? 0 : line + noteHeight);
            height = max(inner, state->controlHeight) + 2 * pad;
            SetRect(&row->box, margin - inset, y, right + inset, y + height);
            top = y + (height - inner) / 2;
            SetRect(&row->name, margin, top, textRight, top + nameHeight);
            SetRect(&row->effect, margin, row->name.bottom + line, textRight, row->name.bottom + line + effectHeight);
            if (beside) SetRect(&row->note, margin + nameWidth + Px(page, PAGE_NOTE_GAP_DIPS), top, textRight, top + nameHeight);
            else SetRect(&row->note, margin, row->effect.bottom + line, textRight, row->effect.bottom + line + noteHeight);
            SetRect(&row->control, right - width, y + (height - state->controlHeight) / 2, right, y + (height + state->controlHeight) / 2);
            y += height;
            break;
        }
        }
    }
    state->height = y + margin;
    ReleaseDC(page, dc);
}

static void PageText(HDC dc, HFONT font, const WCHAR *text, const RECT *rc, COLORREF color)
{
    RECT at = *rc;
    HGDIOBJ old = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &at, DT_WORDBREAK | DT_NOPREFIX | Localize_ReadingFlags());
    SelectObject(dc, old);
}

static void PagePaint(SettingsDialog *state, HDC target, const RECT *update)
{
    ThemeBuffer buffer;
    HWND page = state->page;
    HDC dc = Theme_BufferBegin(&buffer, target, update);
    COLORREF text = Theme_Color(THEME_TEXT), muted = Theme_Color(THEME_MUTED);
    int i, rule = max(1, Px(page, 1));
    FillRect(dc, update, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < state->rowCount; i++) {
        const PageRow *row = &state->rows[i];
        RECT seen;
        if (!IntersectRect(&seen, &row->box, update)) continue;
        if (row->kind == ROW_TITLE) {
            PageText(dc, Font(state, THEME_FONT_HEADING), row->text, &row->box, text);
        } else if (row->kind == ROW_SECTION) {
            /* Its icon, in its tint, then its name. */
            HFONT strong = Font(state, THEME_FONT_STRONG);
            RECT icon = row->box, name = row->box;
            icon.right = icon.left + Theme_InlineGlyphWidth(dc, kSections[row->v].glyph, strong);
            icon.bottom = icon.top + TextHeight(dc, strong, L"Ag", 1000);
            Theme_DrawInlineGlyph(dc, &icon, kSections[row->v].glyph, strong, Theme_TintColor(kSections[row->v].tint));
            name.left = icon.right + Px(page, PAGE_ICON_GAP_DIPS);
            PageText(dc, strong, row->text, &name, text);
        } else if (row->kind == ROW_EMPTY) {
            PageText(dc, Font(state, THEME_FONT_TEXT), row->text, &row->box, muted);
        } else {
            const SettingDef *def = &kSettings[row->v];
            const CheckValue *value = &state->values[row->v];
            HFONT font = Font(state, THEME_FONT_TEXT);
            WCHAR note[512], shown[CHECK_VALUE_CCH];
            UINT look = (value->readOnly ? THEME_BUTTON_DISABLED : 0) | (state->hot == i ? THEME_BUTTON_HOT : 0);
            BOOL hint;
            /* A rule between two settings of a section. */
            if (i > 0 && state->rows[i - 1].kind == ROW_SETTING) {
                RECT between;
                SetRect(&between, row->name.left, row->box.top, row->control.right, row->box.top + rule);
                FillRect(dc, &between, Theme_Brush(THEME_SEPARATOR));
            }
            NoteOf(state, row->v, note, ARRAYSIZE(note));
            PageText(dc, font, TR(def->label), &row->name, value->readOnly ? muted : text);
            PageText(dc, font, TR(def->effect), &row->effect, muted);
            {
                /* The note after its impact's icon. */
                RECT icon = row->note, words = row->note;
                icon.right = icon.left + Theme_InlineGlyphWidth(dc, kImpactGlyphs[def->impact], font);
                icon.bottom = icon.top + TextHeight(dc, font, L"Ag", 1000);
                Theme_DrawInlineGlyph(dc, &icon, kImpactGlyphs[def->impact], font, Theme_TintColor(kImpactTints[def->impact]));
                words.left = icon.right + Px(page, PAGE_ICON_GAP_DIPS);
                PageText(dc, font, note, &words, muted);
            }
            ValueText(state, row->v, shown, ARRAYSIZE(shown), &hint);
            switch (value->kind) {
            case CHECK_VALUE_SWITCH:
                Theme_DrawSwitch(page, dc, &row->control, value->on, look);
                break;
            case CHECK_VALUE_CHOICE:
                Theme_DrawDropDown(page, dc, &row->control, shown, font, look);
                break;
            case CHECK_VALUE_TEXT:
                Theme_DrawField(page, dc, &row->control, state->editing == i ? L"" : shown, hint, font, look);
                break;
            case CHECK_VALUE_ACTION:
                Theme_DrawButton(page, dc, &row->control, shown, font, look, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
                break;
            default:
                break;
            }
            if (state->focus == i && GetFocus() == page) Theme_DrawFocusCue(page, dc, &row->box);
        }
    }
    Theme_BufferEnd(&buffer);
}

static void PageRedrawRow(const SettingsDialog *state, int i)
{
    if (state->page && i >= 0 && i < state->rowCount) InvalidateRect(state->page, &state->rows[i].box, FALSE);
}

/* The rows the page shows, the kind chosen's, or a search's, laid out and drawn. */
static void PageBuild(SettingsDialog *state)
{
    int s, v, height = state->height;
    BOOL any = FALSE;
    if (state->editor) DestroyWindow(state->editor);   /* its row goes */
    state->editor = NULL;
    state->editing = -1;
    state->rowCount = 0;
    state->focus = state->hot = -1;
    state->rows[state->rowCount].kind = ROW_TITLE;
    state->rows[state->rowCount].v = -1;
    state->rows[state->rowCount++].text = state->search[0] ? TR(L"Search results") : TR(kKinds[state->kind].name);
    for (s = 0; s < (int)ARRAYSIZE(kSections); s++) {
        BOOL named = FALSE;
        if (!state->search[0] && !(kKinds[state->kind].items & kSections[s].item)) continue;
        for (v = 0; v < SETTINGS_COUNT && state->rowCount < PAGE_ROWS_MAX - 1; v++) {
            const SettingDef *def = &kSettings[v];
            if (def->item != kSections[s].item) continue;
            if (state->search[0] && !Core_ContainsI(TR(def->label), state->search) && !Core_ContainsI(TR(def->effect), state->search) &&
                !Core_ContainsI(TR(kSections[s].name), state->search))
                continue;
            if (!named) {
                state->rows[state->rowCount].kind = ROW_SECTION;
                state->rows[state->rowCount].v = s;
                state->rows[state->rowCount++].text = TR(kSections[s].name);
                named = TRUE;
            }
            state->rows[state->rowCount].kind = ROW_SETTING;
            state->rows[state->rowCount].v = v;
            state->rows[state->rowCount++].text = NULL;
            any = TRUE;
        }
    }
    if (!any) {
        state->rows[state->rowCount].kind = ROW_EMPTY;
        state->rows[state->rowCount].v = -1;
        state->rows[state->rowCount++].text = TR(L"No setting matches.");
    }
    if (!state->page) return;
    PageLayout(state);
    if (state->height != height) Theme_ViewChanged(state->page);
    Theme_ViewReveal(state->page, 0, 0);   /* from its top */
    InvalidateRect(state->page, NULL, FALSE);
}

/* The setting row at `pt` (the page's client), -1 for none. */
static int PageRowAt(const SettingsDialog *state, POINT pt)
{
    int i;
    for (i = 0; i < state->rowCount; i++)
        if (state->rows[i].kind == ROW_SETTING && PtInRect(&state->rows[i].box, pt)) return i;
    return -1;
}

/* Row `i` the keyboard's, in sight. */
static void PageFocus(SettingsDialog *state, int i)
{
    int margin = Px(state->page, PAGE_MARGIN_DIPS);
    if (i < 0 || i >= state->rowCount || state->rows[i].kind != ROW_SETTING) return;
    PageRedrawRow(state, state->focus);
    state->focus = i;
    PageRedrawRow(state, i);
    Theme_ViewReveal(state->page, state->rows[i].box.top - margin, state->rows[i].box.bottom + margin);
}

/* The setting row `step` settings on from the keyboard's (the first or the last at an end). */
static int PageStep(const SettingsDialog *state, int step)
{
    int direction = step > 0 ? 1 : -1, left = step > 0 ? step : -step, found = state->focus, i = state->focus;
    if (i < 0) i = direction > 0 ? -1 : state->rowCount;   /* none yet: from the first (or the last) */
    for (i += direction; i >= 0 && i < state->rowCount && left > 0; i += direction)
        if (state->rows[i].kind == ROW_SETTING) {
            found = i;
            left--;
        }
    return found;
}

static void UpdateApply(HWND dialog, const SettingsDialog *state);

/* Value `v` changed in the page: its row drawn again, Apply as it now stands. */
static void PageChanged(SettingsDialog *state, int i)
{
    int v = state->rows[i].v;
    if (kSettings[v].store == STORE_DESKTOP && state->onIn[v] > 0 && state->onIn[v] < state->profiles) state->touched[v] = TRUE;
    PageRedrawRow(state, i);
    UpdateApply(state->dialog, state);
}

static void PageCloseEditor(SettingsDialog *state)
{
    HWND editor = state->editor;
    int row = state->editing;
    state->editor = NULL;
    state->editing = -1;
    if (!editor) return;
    if (GetFocus() == editor) SetFocus(state->page);
    DestroyWindow(editor);
    PageRedrawRow(state, row);
}

/* Enter keeps what was typed, Escape gives the text before back; either leaves the field. */
static LRESULT CALLBACK EditorProc(HWND edit, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    SettingsDialog *state = (SettingsDialog *)ref;
    if (msg == WM_GETDLGCODE && lp && (((const MSG *)lp)->wParam == VK_RETURN || ((const MSG *)lp)->wParam == VK_ESCAPE))
        return DLGC_WANTALLKEYS;
    if (msg == WM_KEYDOWN && (wp == VK_RETURN || wp == VK_ESCAPE)) {
        if (wp == VK_ESCAPE) SetWindowTextW(edit, state->editStart);   /* its EN_CHANGE takes it back */
        SetFocus(state->page);
        return 0;
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 0x1B)) return 0;   /* no beep */
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(edit, EditorProc, id);
    return DefSubclassProc(edit, msg, wp, lp);
}

/* A real edit over row `i`'s field, holding its text, to type in. */
static void PageOpenEditor(SettingsDialog *state, int i)
{
    const PageRow *row = &state->rows[i];
    CheckValue *value = &state->values[row->v];
    HFONT font = Font(state, THEME_FONT_TEXT);
    RECT text;
    HDC dc;
    int height = 0;
    if (value->kind != CHECK_VALUE_TEXT || value->readOnly) return;
    PageCloseEditor(state);
    if ((dc = GetDC(state->page)) != NULL) {
        height = TextHeight(dc, font, L"Ag", 1000);
        ReleaseDC(state->page, dc);
    }
    Theme_FieldText(state->page, &row->control, &text);
    height = min(max(height, 1), text.bottom - text.top);
    state->editor = CreateWindowExW(0, WC_EDITW, value->text, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, text.left,
                                    text.top + (text.bottom - text.top - height) / 2, text.right - text.left, height, state->page,
                                    (HMENU)(INT_PTR)EDITOR_ID, g_hInst, NULL);
    if (!state->editor) {
        Util_Log(L"settings: a field cannot be edited: no editor (error %lu)", GetLastError());
        return;
    }
    state->editing = i;
    StringCchCopyW(state->editStart, ARRAYSIZE(state->editStart), value->text);
    SendMessageW(state->editor, WM_SETFONT, (WPARAM)font, FALSE);
    SendMessageW(state->editor, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    SendMessageW(state->editor, EM_LIMITTEXT, value->limit > 0 && value->limit < CHECK_VALUE_CCH ? value->limit : CHECK_VALUE_CCH - 1, 0);
    if (value->hint) SendMessageW(state->editor, EM_SETCUEBANNER, TRUE, (LPARAM)value->hint);
    SetWindowSubclass(state->editor, EditorProc, 1, (DWORD_PTR)state);
    SendMessageW(state->editor, EM_SETSEL, 0, -1);
    SetFocus(state->editor);
    PageRedrawRow(state, i);
}

/* Row `i`'s control used: a switch turned over, a choice picked from its menu, a field typed in, an action done. */
static void PageActivate(SettingsDialog *state, int i)
{
    CheckValue *value;
    if (i < 0 || i >= state->rowCount || state->rows[i].kind != ROW_SETTING) return;
    value = &state->values[state->rows[i].v];
    if (value->readOnly) return;
    switch (value->kind) {
    case CHECK_VALUE_SWITCH:
        value->on = !value->on;
        PageChanged(state, i);
        break;
    case CHECK_VALUE_CHOICE: {
        HMENU menu = CreatePopupMenu();
        RECT box = state->rows[i].control;
        UINT chosen;
        int c;
        if (!menu) return;
        for (c = 0; c < value->choiceCount; c++)
            if (value->choices[c]) AppendMenuW(menu, MF_STRING | (c == value->choice ? MF_CHECKED : 0), (UINT_PTR)c + 1, value->choices[c]);
        MapWindowPoints(state->page, NULL, (POINT *)&box, 2);
        chosen = Theme_TrackDropDown(state->page, menu, &box);
        DestroyMenu(menu);
        if (chosen >= 1 && (int)chosen <= value->choiceCount && (int)chosen - 1 != value->choice) {
            value->choice = (int)chosen - 1;
            PageChanged(state, i);
        }
        break;
    }
    case CHECK_VALUE_TEXT:
        PageOpenEditor(state, i);
        break;
    case CHECK_VALUE_ACTION:
        PostMessageW(state->dialog, WM_APP_SETTINGS_ACTION, (WPARAM)state->rows[i].v, 0);
        break;
    default:
        break;
    }
}

/* Left and Right on a row: a switch off or on, the choice before or after. */
static void PageNudge(SettingsDialog *state, int i, int step)
{
    CheckValue *value;
    if (i < 0 || i >= state->rowCount || state->rows[i].kind != ROW_SETTING) return;
    value = &state->values[state->rows[i].v];
    if (value->readOnly) return;
    if (value->kind == CHECK_VALUE_SWITCH && value->on != (step > 0)) {
        value->on = step > 0;
        PageChanged(state, i);
    } else if (value->kind == CHECK_VALUE_CHOICE && value->choice + step >= 0 && value->choice + step < value->choiceCount) {
        value->choice += step;
        PageChanged(state, i);
    }
}

static LRESULT CALLBACK PageProc(HWND page, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsDialog *state = (SettingsDialog *)GetWindowLongPtrW(page, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        state = (SettingsDialog *)((const CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(page, GWLP_USERDATA, (LONG_PTR)state);
        state->page = page;
    }
    if (!state) return DefWindowProcW(page, msg, wp, lp);
    switch (msg) {
    case WM_THEME_CONTENT_HEIGHT: {
        RECT client;
        if (state->width < 0 || (GetClientRect(page, &client) && client.right != state->width)) PageLayout(state);
        return state->height;
    }
    case WM_SIZE:
        if ((int)LOWORD(lp) != state->width) {
            int height = state->height;
            PageCloseEditor(state);
            PageLayout(state);
            if (state->height != height) Theme_ViewChanged(page);
            InvalidateRect(page, NULL, FALSE);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(page, &paint);
        if (state->width < 0 || state->fontDpi != GetDpiForWindow(page)) PageLayout(state);
        if (dc && !IsRectEmpty(&paint.rcPaint)) PagePaint(state, dc, &paint.rcPaint);
        EndPaint(page, &paint);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        if (state->focus < 0) state->focus = PageStep(state, 1);
        PageRedrawRow(state, state->focus);
        return 0;
    case WM_KILLFOCUS:
        PageRedrawRow(state, state->focus);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt;
        int i;
        RECT control;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        SetFocus(page);
        if ((i = PageRowAt(state, pt)) < 0) return 0;
        PageFocus(state, i);
        control = state->rows[i].control;
        InflateRect(&control, Px(page, 4), Px(page, 4));
        if (PtInRect(&control, pt)) PageActivate(state, i);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt;
        int i, hot = -1;
        TRACKMOUSEEVENT track;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if ((i = PageRowAt(state, pt)) >= 0 && PtInRect(&state->rows[i].control, pt)) hot = i;
        if (hot != state->hot) {
            PageRedrawRow(state, state->hot);
            state->hot = hot;
            PageRedrawRow(state, hot);
        }
        ZeroMemory(&track, sizeof track);
        track.cbSize = sizeof track;
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = page;
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        PageRedrawRow(state, state->hot);
        state->hot = -1;
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_UP:    PageFocus(state, PageStep(state, -1)); return 0;
        case VK_DOWN:  PageFocus(state, PageStep(state, 1)); return 0;
        case VK_PRIOR: PageFocus(state, PageStep(state, -5)); return 0;
        case VK_NEXT:  PageFocus(state, PageStep(state, 5)); return 0;
        case VK_HOME:  PageFocus(state, PageStep(state, -PAGE_ROWS_MAX)); return 0;
        case VK_END:   PageFocus(state, PageStep(state, PAGE_ROWS_MAX)); return 0;
        case VK_LEFT:  PageNudge(state, state->focus, -1); return 0;
        case VK_RIGHT: PageNudge(state, state->focus, 1); return 0;
        case VK_SPACE:
        case VK_F2:    PageActivate(state, state->focus); return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L' ') return 0;   /* Space, done on its key */
        break;
    case WM_COMMAND:
        if (LOWORD(wp) == EDITOR_ID && state->editor && (HWND)lp == state->editor) {
            if (HIWORD(wp) == EN_CHANGE && state->editing >= 0) {
                CheckValue *value = &state->values[state->rows[state->editing].v];
                GetWindowTextW(state->editor, value->text, ARRAYSIZE(value->text));
                PageChanged(state, state->editing);
            } else if (HIWORD(wp) == EN_KILLFOCUS) {
                PostMessageW(page, WM_APP_PAGE_EDITED, 0, 0);   /* not while the focus moves */
            }
            return 0;
        }
        break;
    case WM_APP_PAGE_EDITED:
        if (state->editor && GetFocus() != state->editor) PageCloseEditor(state);
        return 0;
    case WM_CTLCOLOREDIT:
        return (LRESULT)Theme_CtlColor(msg, wp, lp, 0);
    case WM_NCDESTROY:
        state->page = NULL;
        state->editor = NULL;
        break;
    }
    return DefWindowProcW(page, msg, wp, lp);
}

/* ------------------------------------------------------------ the kinds */

/* Kind `g`'s row in the kinds' window. */
static void KindRect(HWND kinds, int g, RECT *rc)
{
    RECT client;
    int height = Px(kinds, KIND_ROW_DIPS), step = height + Px(kinds, KIND_ROW_GAP_DIPS);
    GetClientRect(kinds, &client);
    SetRect(rc, 0, g * step, client.right, g * step + height);
}

static int KindAt(HWND kinds, POINT pt)
{
    RECT row;
    int g;
    for (g = 0; g < SG_COUNT; g++) {
        KindRect(kinds, g, &row);
        if (PtInRect(&row, pt)) return g;
    }
    return -1;
}

/* Each kind a row as tall as Claude's: its icon in its tint, its name; the page's selected while no search shows. */
static void KindsPaint(SettingsDialog *state, HDC target, const RECT *update)
{
    ThemeBuffer buffer;
    HWND kinds = state->kinds;
    HDC dc = Theme_BufferBegin(&buffer, target, update);
    HFONT font = Font(state, THEME_FONT_HEADING);
    int g;
    FillRect(dc, update, Theme_Brush(THEME_FACE));
    SetBkMode(dc, TRANSPARENT);
    for (g = 0; g < SG_COUNT; g++) {
        RECT row, icon, label;
        UINT look = (g == state->kind && !state->search[0] ? THEME_ROW_SELECTED : 0) | (g == state->hotKind ? THEME_ROW_HOT : 0);
        COLORREF color;
        HGDIOBJ old;
        KindRect(kinds, g, &row);
        color = Theme_DrawRow(kinds, dc, &row, look, Theme_Color(THEME_FACE));
        icon = row;
        icon.left += Px(kinds, KIND_PAD_DIPS);
        icon.right = icon.left + Theme_InlineGlyphWidth(dc, kKinds[g].glyph, font);
        Theme_DrawInlineGlyph(dc, &icon, kKinds[g].glyph, font, Theme_TintColor(kKinds[g].tint));
        label = row;
        label.left = icon.right + Px(kinds, PAGE_ICON_GAP_DIPS);
        label.right -= Px(kinds, KIND_PAD_DIPS);
        old = SelectObject(dc, font);
        SetTextColor(dc, color);
        DrawTextW(dc, TR(kKinds[g].name), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | Localize_ReadingFlags());
        SelectObject(dc, old);
        if (g == state->kind && GetFocus() == kinds) Theme_DrawFocusCue(kinds, dc, &row);
    }
    Theme_BufferEnd(&buffer);
}

/* Kind `g`'s page, the search left. */
static void ChooseKind(SettingsDialog *state, int g)
{
    if (g < 0 || g >= SG_COUNT || (g == state->kind && !state->search[0])) return;
    state->kind = g;
    if (state->search[0]) {
        state->search[0] = 0;
        SetDlgItemTextW(state->dialog, IDC_G_SEARCH, L"");   /* its EN_CHANGE finds nothing new */
    }
    if (state->kinds) InvalidateRect(state->kinds, NULL, FALSE);
    PageBuild(state);
}

static LRESULT CALLBACK KindsProc(HWND kinds, UINT msg, WPARAM wp, LPARAM lp)
{
    SettingsDialog *state = (SettingsDialog *)GetWindowLongPtrW(kinds, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        state = (SettingsDialog *)((const CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(kinds, GWLP_USERDATA, (LONG_PTR)state);
        state->kinds = kinds;
    }
    if (!state) return DefWindowProcW(kinds, msg, wp, lp);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        HDC dc = BeginPaint(kinds, &paint);
        EnsureFonts(state, FALSE);
        if (dc && !IsRectEmpty(&paint.rcPaint)) KindsPaint(state, dc, &paint.rcPaint);
        EndPaint(kinds, &paint);
        return 0;
    }
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(kinds, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        SetFocus(kinds);
        ChooseKind(state, KindAt(kinds, pt));
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt;
        TRACKMOUSEEVENT track;
        int hot;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        if ((hot = KindAt(kinds, pt)) != state->hotKind) {
            state->hotKind = hot;
            InvalidateRect(kinds, NULL, FALSE);
        }
        ZeroMemory(&track, sizeof track);
        track.cbSize = sizeof track;
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = kinds;
        TrackMouseEvent(&track);
        return 0;
    }
    case WM_MOUSELEAVE:
        state->hotKind = -1;
        InvalidateRect(kinds, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_UP:   ChooseKind(state, state->kind - 1); return 0;
        case VK_DOWN: ChooseKind(state, state->kind + 1); return 0;
        case VK_HOME: ChooseKind(state, 0); return 0;
        case VK_END:  ChooseKind(state, SG_COUNT - 1); return 0;
        }
        break;
    case WM_NCDESTROY:
        state->kinds = NULL;
        break;
    }
    return DefWindowProcW(kinds, msg, wp, lp);
}

/* The page's class and the kinds'. */
static BOOL RegisterClasses(void)
{
    static BOOL registered;
    static const struct { const WCHAR *name; WNDPROC proc; } kClasses[] = { { PAGE_CLASS, PageProc }, { KINDS_CLASS, KindsProc } };
    WNDCLASSW wc;
    size_t i;
    if (registered) return TRUE;
    for (i = 0; i < ARRAYSIZE(kClasses); i++) {
        ZeroMemory(&wc, sizeof wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = kClasses[i].proc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = kClasses[i].name;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Util_Log(L"settings: the class %s cannot be registered (error %lu)", kClasses[i].name, GetLastError());
            return FALSE;
        }
    }
    registered = TRUE;
    return TRUE;
}

BOOL Settings_PageRow(HWND page, int index, WCHAR *title, size_t titleCch, WCHAR *shown, size_t shownCch)
{
    const SettingsDialog *state = page ? (const SettingsDialog *)GetWindowLongPtrW(page, GWLP_USERDATA) : NULL;
    int i, seen = 0;
    BOOL hint;
    if (title && titleCch) title[0] = 0;
    if (shown && shownCch) shown[0] = 0;
    for (i = 0; state && i < state->rowCount; i++) {
        if (state->rows[i].kind != ROW_SETTING || seen++ != index) continue;
        if (title) StringCchCopyW(title, titleCch, TR(kSettings[state->rows[i].v].label));
        if (shown) {
            const CheckValue *value = &state->values[state->rows[i].v];
            if (value->kind == CHECK_VALUE_SWITCH) StringCchCopyW(shown, shownCch, value->on ? L"1" : L"0");
            else ValueText(state, state->rows[i].v, shown, shownCch, &hint);
        }
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------- the dialog */

/* What the dialog shows differs from what is saved and in use. */
static BOOL Changed(const SettingsDialog *state)
{
    int v;
    for (v = 0; v < SETTINGS_COUNT; v++)
        if (ValueChanged(state, v)) return TRUE;
    return FALSE;
}

/* Apply only while there is something to apply; the focus it had goes to OK. */
static void UpdateApply(HWND dialog, const SettingsDialog *state)
{
    HWND apply = GetDlgItem(dialog, IDC_G_APPLY);
    BOOL changed = Changed(state);
    if (!changed && GetFocus() == apply) SendMessageW(dialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dialog, IDOK), TRUE);
    EnableWindow(apply, changed);
}

/* Setting `v` shown, in sight and the keyboard's: its kind's page, the search left; a text's field typed in. */
static void ShowSetting(SettingsDialog *state, int v)
{
    int g, i;
    for (g = 0; g < SG_COUNT; g++)
        if (kKinds[g].items & kSettings[v].item) break;
    if (g < SG_COUNT) ChooseKind(state, g);
    for (i = 0; i < state->rowCount; i++)
        if (state->rows[i].kind == ROW_SETTING && state->rows[i].v == v) {
            SetFocus(state->page);
            PageFocus(state, i);
            if (state->values[v].kind == CHECK_VALUE_TEXT) PageOpenEditor(state, i);
            return;
        }
}

/* The dialog in the language now in use: its captions, the texts it fills,
 * then its layout and theme, as a new language gives them. */
static void FollowLanguage(HWND dialog, SettingsDialog *state)
{
    Localize_Window(dialog);
    ShowNotes(state);
    SendDlgItemMessageW(dialog, IDC_G_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search settings"));
    EnsureFonts(state, TRUE);
    if (state->kinds) InvalidateRect(state->kinds, NULL, FALSE);
    Theme_FitDialog(dialog);
    Theme_Apply(dialog);
    PageBuild(state);
}

/* A text that is no number it takes: said, and shown. */
static BOOL NotDays(HWND dialog, SettingsDialog *state, int v, UINT least, UINT most, BOOL emptyIsNone)
{
    const WCHAR *text = state->values[v].text, *end = NULL;
    unsigned long days;
    if (!ValueChanged(state, v) || (emptyIsNone && !text[0])) return FALSE;
    days = text[0] ? wcstoul(text, (WCHAR **)&end, 10) : 0;
    if (text[0] && end && !*end && text[0] >= L'0' && text[0] <= L'9' && days >= least && days <= most) return FALSE;
    Ui_Message(dialog, MB_ICONINFORMATION, TR(L"Enter a number of days from %d to %d."), (int)least, (int)most);
    ShowSetting(state, v);
    return TRUE;
}

/* What changed in Claude Code's settings.json, written there in one step, its contents read again first. */
static BOOL WriteCode(HWND dialog, SettingsDialog *state)
{
    WCHAR path[LONG_PATH_CCH], dir[LONG_PATH_CCH], *slash;
    char *json, number[24];
    DWORD read = 0;
    size_t length;
    BOOL locked, ok = TRUE, any = FALSE;
    int v;
    for (v = 0; v < SETTINGS_COUNT; v++)
        if (InCode(&kSettings[v]) && ValueChanged(state, v)) any = TRUE;
    if (!any) return TRUE;
    json = ReadCode(&read, &locked);
    length = read;
    if (locked || !SessionStore_ClaudeCodePath(L"settings.json", path, ARRAYSIZE(path))) {
        Util_Free(json);
        Ui_Message(dialog, MB_ICONWARNING, TR(L"Claude Code's settings.json cannot be read: it is left as it is."));
        return FALSE;
    }
    if (json) {   /* the heap block SetMember replaces */
        char *copy = (char *)HeapAlloc(GetProcessHeap(), 0, length + 1);
        if (copy) {
            memcpy(copy, json, length);
            copy[length] = 0;
        }
        Util_Free(json);
        if ((json = copy) == NULL) return FALSE;
    }
    for (v = 0; ok && v < SETTINGS_COUNT; v++) {
        const SettingDef *def = &kSettings[v];
        const CheckValue *value = &state->values[v];
        const char *raw;
        size_t rawLength;
        if (!InCode(def) || !ValueChanged(state, v)) continue;
        switch (def->store) {
        case STORE_ENV:
            ok = SetMember(&json, &length, def->key, value->on ? EnvOn(def) : NULL);
            break;
        case STORE_JSON:
            ok = SetMember(&json, &length, def->key, value->on ? def->on : NULL);
            break;
        case STORE_ATTRIBUTION:
            if (value->on) {
                ok = SetMember(&json, &length, "attribution.commit", "\"\"") && SetMember(&json, &length, "attribution.pr", "\"\"");
            } else {
                if (MemberAt(json, length, "attribution", &raw, &rawLength) && RawIs(raw, rawLength, "false"))
                    ok = SetMember(&json, &length, "attribution", NULL);
                else
                    ok = SetMember(&json, &length, "attribution.commit", NULL) && SetMember(&json, &length, "attribution.pr", NULL);
                if (ok && MemberAt(json, length, "includeCoAuthoredBy", &raw, &rawLength) && RawIs(raw, rawLength, "false"))
                    ok = SetMember(&json, &length, "includeCoAuthoredBy", NULL);
            }
            break;
        case STORE_CODE_DAYS:
            StringCchPrintfA(number, ARRAYSIZE(number), "%lu", wcstoul(value->text, NULL, 10));
            ok = SetMember(&json, &length, def->key, value->text[0] ? number : NULL);
            break;
        case STORE_CODE_MODE:
            if (value->choice > 0 && value->choice < MODES) {
                char quoted[32];
                StringCchPrintfA(quoted, ARRAYSIZE(quoted), "\"%s\"", kModes[value->choice]);
                ok = SetMember(&json, &length, def->key, quoted);
            } else {
                ok = SetMember(&json, &length, def->key, NULL);
            }
            break;
        default:
            break;
        }
    }
    if (ok && json) {
        StringCchCopyW(dir, ARRAYSIZE(dir), path);
        if ((slash = wcsrchr(dir, L'\\')) != NULL) {
            *slash = 0;
            CreateDirectoryW(dir, NULL);   /* Claude Code not run yet: its folder made */
        }
        ok = Util_WriteFileAtomic(path, json, length, UTIL_PUT_REPLACE);
        Util_Log(ok ? L"settings: Claude Code's settings.json written" : L"settings: Claude Code's settings.json not written (error %lu)",
                 GetLastError());
    }
    if (json) HeapFree(GetProcessHeap(), 0, json);
    if (!ok) {
        Ui_Message(dialog, MB_ICONWARNING, TR(L"The settings could not be saved."));
        return FALSE;
    }
    Util_Free(state->code);
    state->code = ReadCode(&state->codeLength, &state->codeLocked);
    for (v = 0; v < SETTINGS_COUNT; v++)
        if (InCode(&kSettings[v])) ReadValue(state, v);
    return TRUE;
}

/* What changed in Claude's preferences, made in every profile (Gui_SetPreferences); FALSE when it was not. */
static BOOL WritePreferences(HWND dialog, SettingsDialog *state)
{
    PreferenceChange changes[SETTINGS_COUNT];
    int v, count = 0;
    for (v = 0; v < SETTINGS_COUNT; v++)
        if (kSettings[v].store == STORE_DESKTOP && ValueChanged(state, v)) {
            changes[count].key = kSettings[v].key;
            changes[count].raw = state->values[v].on ? kSettings[v].on : kSettings[v].off;
            count++;
        }
    if (!count) return TRUE;
    if (!Gui_SetPreferences(dialog, changes, count)) return FALSE;
    state->profiles = Gui_Profiles()->count;
    for (v = 0; v < SETTINGS_COUNT; v++)
        if (kSettings[v].store == STORE_DESKTOP) ReadValue(state, v);   /* those still open keep theirs */
    return TRUE;
}

/* What the dialog shows, saved where it changed and applied; FALSE, and says why, when a text is not one it takes, or
 * something could not be saved. */
static BOOL ApplySettings(HWND dialog, SettingsDialog *state)
{
    int languageBefore = Localize_EffectiveLanguage(), language = ValueOfStore(STORE_LANGUAGE), zoom = ValueOfStore(STORE_ZOOM), v;
    BOOL ok = TRUE;
    if (state->editor) PageCloseEditor(state);
    for (v = 0; v < SETTINGS_COUNT; v++) {
        const SettingDef *def = &kSettings[v];
        CheckValue *value = &state->values[v];
        if (def->store == STORE_VAULT_DAYS && NotDays(dialog, state, v, 0, DAYS_MAX, FALSE)) return FALSE;
        if (def->store == STORE_CODE_DAYS && NotDays(dialog, state, v, 1, CODE_DAYS_MAX, TRUE)) return FALSE;
        if (def->store == STORE_SEARXNG && ValueChanged(state, v) && value->text[0]) {
            /* One typed made the form the server takes; none, the default's. */
            WCHAR url[MAX_PATH];
            if (!Core_SearXngUrl(value->text, url, ARRAYSIZE(url)) || !url[0]) {
                Ui_Message(dialog, MB_ICONINFORMATION, TR(L"Type your SearXNG instance's address, such as https://searx.example.com."));
                ShowSetting(state, v);
                return FALSE;
            }
            StringCchCopyW(value->text, ARRAYSIZE(value->text), url);
        }
    }
    for (v = 0; v < SETTINGS_COUNT; v++) {
        const SettingDef *def = &kSettings[v];
        CheckValue *value = &state->values[v];
        BOOL saved = TRUE;
        if (!ValueChanged(state, v) || InCode(def) || def->store == STORE_DESKTOP || def->store == STORE_LANGUAGE || def->store == STORE_ZOOM)
            continue;
        switch (def->store) {
        case STORE_INI:
            saved = Util_SetSetting(def->name, value->on ? 1 : 0);
            if (saved && wcscmp(def->name, SETTING_MCP_ONLINE) == 0) WindowsMcp_RefreshOnline();   /* taken at Claude's next start */
            break;
        case STORE_DEVTOOLS:
            /* On: 9222 (settings.ini can still name another port); off: none. The next sign-in window takes it. */
            saved = Util_SetSetting(SETTING_SIGNIN_DEVTOOLS, value->on ? DEVTOOLS_PORT : 0);
            break;
        case STORE_BROWSER:
            saved = value->choice >= 0 && value->choice < state->browsers.count &&
                    Util_SetSetting(SETTING_SIGNIN_BROWSER, (DWORD)state->browsers.kind[value->choice]);
            break;
        case STORE_VAULT_DAYS:
            saved = Util_SetSetting(SETTING_VAULT_DAYS, (DWORD)wcstoul(value->text, NULL, 10));
            break;
        case STORE_SEARXNG: {
            WCHAR url[1][MAX_PATH];
            StringCchCopyW(url[0], ARRAYSIZE(url[0]), value->text);
            saved = Util_SetSettingList(SETTING_SEARXNG_SECTION, (const WCHAR (*)[MAX_PATH])url, url[0][0] ? 1 : 0);
            break;
        }
        default:
            break;
        }
        if (saved) {
            ReadValue(state, v);
        } else if (ok) {
            Ui_Message(dialog, MB_ICONWARNING, TR(L"The settings could not be saved."));
            ok = FALSE;
        }
    }
    if (!WriteCode(dialog, state)) ok = FALSE;
    if (!WritePreferences(dialog, state)) ok = FALSE;
    if (zoom >= 0 && ValueChanged(state, zoom)) {
        /* The manager's window opens again at that size once Settings closes. */
        if (!Util_SetSetting(SETTING_ZOOM, (DWORD)kZooms[state->values[zoom].choice])) {
            Ui_Message(dialog, MB_ICONWARNING, TR(L"The settings could not be saved."));
            ok = FALSE;
        } else {
            state->zoom = kZooms[state->values[zoom].choice];
            ReadValue(state, zoom);
            if (state->act) state->act(dialog, IDC_G_ZOOM);
        }
    }
    if (language >= 0 && ValueChanged(state, language) && state->applyLanguage) state->applyLanguage(state->values[language].choice - 1);
    if (language >= 0) ReadValue(state, language);   /* one not taken goes back */
    if (Localize_EffectiveLanguage() != languageBefore) {
        FollowLanguage(dialog, state);
    } else {
        ShowNotes(state);
        state->width = -1;   /* the notes may differ: laid out again */
        PageLayout(state);
        Theme_ViewChanged(state->page);
        InvalidateRect(state->page, NULL, FALSE);
    }
    UpdateApply(dialog, state);
    return ok;
}

/* A window of `className` in place of the template's placeholder `id`, in its place in the tab order too. */
static HWND ReplacePlaceholder(HWND dialog, int id, const WCHAR *className, DWORD style, SettingsDialog *state)
{
    HWND placeholder = GetDlgItem(dialog, id), made = NULL;
    RECT rc;
    if (!placeholder || !GetWindowRect(placeholder, &rc)) return NULL;
    MapWindowPoints(NULL, dialog, (POINT *)&rc, 2);
    made = CreateWindowExW(0, className, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, rc.left, rc.top, rc.right - rc.left,
                           rc.bottom - rc.top, dialog, (HMENU)(INT_PTR)id, g_hInst, state);
    if (!made) {
        Util_Log(L"settings: %s cannot be made (error %lu)", className, GetLastError());
        return NULL;
    }
    SetWindowPos(made, placeholder, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DestroyWindow(placeholder);
    return made;
}

/* The kinds and the page, drawn here, in place of the template's placeholders; the page in its view. */
static void InitSettings(HWND dialog, SettingsDialog *state)
{
    state->dialog = dialog;
    state->focus = state->hot = state->hotKind = state->editing = -1;
    state->width = -1;
    state->zoom = Theme_Zoom();
    SignIn_FindBrowsers(&state->browsers);
    FillValues(state);
    SendDlgItemMessageW(dialog, IDC_G_SEARCH, EM_LIMITTEXT, ARRAYSIZE(state->search) - 1, 0);
    SendDlgItemMessageW(dialog, IDC_G_SEARCH, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search settings"));
    if (RegisterClasses()) {
        ReplacePlaceholder(dialog, IDC_G_LIST, KINDS_CLASS, 0, state);
        if (ReplacePlaceholder(dialog, IDC_G_PAGE, PAGE_CLASS, WS_CLIPCHILDREN, state)) {
            PageBuild(state);
            Theme_SmoothView(state->page);
            Theme_SetScrollRow(state->page, Px(state->page, 24));   /* a wheel's line */
        }
    }
    UpdateApply(dialog, state);
}

static INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SettingsDialog *state = (SettingsDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG:
        state = (SettingsDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        InitSettings(dialog, state);
        return TRUE;

    case WM_APP_SETTINGS_ACTION:
        if (state && state->act && (int)wp >= 0 && (int)wp < SETTINGS_COUNT) state->act(dialog, kSettings[wp].action);
        return TRUE;

    case WM_DPICHANGED:
        if (state) state->width = -1;   /* the page's fonts and rows at the new scale */
        break;

    case WM_DESTROY:
        if (state) {
            SignIn_FreeBrowsers(&state->browsers);
            Util_Free(state->code);
            state->code = NULL;
            Theme_FreeFonts(&state->fonts);
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_G_SEARCH:
            if (HIWORD(wp) == EN_CHANGE) {
                WCHAR search[ARRAYSIZE(state->search)];
                GetDlgItemTextW(dialog, IDC_G_SEARCH, search, ARRAYSIZE(search));
                if (wcscmp(search, state->search) != 0) {
                    StringCchCopyW(state->search, ARRAYSIZE(state->search), search);
                    if (state->kinds) InvalidateRect(state->kinds, NULL, FALSE);   /* none chosen while it searches */
                    PageBuild(state);
                }
            }
            return TRUE;
        case IDC_G_APPLY:
            if (HIWORD(wp) == BN_CLICKED) ApplySettings(dialog, state);
            return TRUE;
        case IDOK:
            if (state->editor) PageCloseEditor(state);
            if (!Changed(state) || ApplySettings(dialog, state)) EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            if (state->editor && GetFocus() == state->editor) return TRUE;   /* Escape in a field leaves the field */
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void Settings_Show(HWND owner, SettingsLanguageProc applyLanguage, SettingsActionProc act)
{
    SettingsDialog *state = (SettingsDialog *)Util_Alloc(sizeof *state);
    if (!state) return;
    state->applyLanguage = applyLanguage;
    state->act = act;
    Ui_Dialog(owner, IDD_SETTINGS, SettingsProc, (LPARAM)state);
    Util_Free(state);
}
