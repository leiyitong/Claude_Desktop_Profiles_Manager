/*
 * A profile backed up to one compressed archive (zip.c), and restored from
 * it into a profile, the same or another one, an item at a time. Its list is
 * what to sync's (syncui.c), its own content added:
 *
 *   Code sessions  the sessions it lists, as a session archive of
 *                  sessionsync.c (entries and conversations), and its
 *                  scheduled tasks; restored through that archive's import,
 *                  into the account the profile is signed in to
 *   Cowork         local-agent-mode-sessions (Cowork's sessions; not its VM)
 *   the settings   sidebar, session details, appearance, language, default
 *   side           model, settings and permissions, member by member as the
 *                  sync reads them (SessionSync_ReadLayout), in one layout
 *                  restored part by part (SessionSync_RestoreLayout); the
 *                  theme of config.json beside it
 *   MCP servers    those of claude_desktop_config.json alone, and the window
 *                  and developer settings
 *   sign-in        what keeps it signed in: cookies and the web storage of
 *                  the app, Local State (whose key, DPAPI-protected, opens
 *                  them only on this PC for this Windows user) and
 *                  config.json; whole folders replaced, so no database mixes
 *                  old and new files; and that key itself, opened with
 *                  DPAPI: the sign-in then restores on any PC, its key
 *                  protected again there for its Windows user. Whoever holds
 *                  the archive holds the sign-in. A backup of an older
 *                  version sealed the key behind a password (Core_SealSecret),
 *                  asked for when it is restored
 *
 *   the profile   what the manager keeps of it: its name, note, color,
 *                 badge, picture and windows' title (profile/profile.json,
 *                 profile/picture.png), given to the profile restored into
 *
 * A backup of version 1 (before the items were fine) kept the whole
 * claude_desktop_config.json: it is still restored whole.
 *
 * Caches, logs, the Claude Code and Cowork programs Claude downloads again
 * and Cowork's VM image are never in it. A restore needs the profile closed
 * (and every profile sharing its session folder), and keeps what it replaces
 * in backups\<time>\<folder>-restore of the state folder.
 *
 * One restore can read several backups, of several accounts, into one
 * profile: sessions, scheduled tasks and Cowork from all of them, the
 * newest's copy of what several hold; settings and the sign-in, which only
 * one can give, from the one the person picks where several hold them
 * (IDD_CONFLICTS, the sync conflicts' look).
 */
#include "app.h"
#include "vaulttools.h"
#include "vaulttools_tasks.h"
#include "resource.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wincrypt.h>   /* DPAPI: the sign-in's key */
#include <stdarg.h>
#include <string.h>

#define BACKUP_FORMAT     "claude-desktop-profiles-manager-backup"
#define BACKUP_VERSION    2
#define MANIFEST_NAME     "manifest.json"
#define SESSIONS_ARCHIVE  "sessions/archive.zip"
#define SCHEDULED_TASKS   L"scheduled-tasks.json"
#define COWORK_DIR        L"local-agent-mode-sessions"
#define APPEARANCE_NAME   "settings/appearance.json"   /* config.json's language and theme only */
#define MANIFEST_MAX      (64u * 1024u)
#define JSON_MAX          (16u * 1024u * 1024u)
#define BACKUPS_DIR       L"backups"
#define ARCHIVE_EXTENSION L"zip"
#define LOCAL_STATE       L"Local State"
#define KEY_NAME          "account-key.json"   /* the sign-in's key ({"key": base64}, or sealed by an older version), never extracted */
#define KEY_ITERATIONS    600000               /* PBKDF2-SHA256's, as OWASP advises */
#define PASSWORD_MIN      8

typedef enum BackupPart { PART_SESSIONS, PART_COWORK, PART_SETTINGS, PART_SIGNIN, PART_KEY, PARTS } BackupPart;

/* What a backup holds, its own content a bit each above the what-to-sync bits
 * (SYNC_ITEM_*). The settings side it keeps member by member, through the
 * layout of sessionsync.c under those same SYNC_ITEM_* bits, so a backup's
 * list shows the same groups and items as what to sync (syncui.c), its own
 * content added. The content bits map to the parts the files are written and
 * read by (BitsToParts). */
#define BK_CONVERSATIONS  0x04000000u   /* Code sessions: entries, transcripts, scheduled tasks */
#define BK_COWORK         0x08000000u   /* local-agent-mode-sessions */
#define BK_APPCONFIG      0x10000000u   /* MCP servers, window and developer settings */
#define BK_SIGNIN         0x20000000u   /* this PC: cookies, the app's storage, Local State */
#define BK_KEY            0x40000000u   /* the sign-in's key, for any PC: in the archive with the sign-in, no choice of its own */
#define BK_PROFILE        0x80000000u   /* the profile's own: name, note, color, badge, picture, windows' title */
#define BK_CONTENT        (BK_CONVERSATIONS | BK_COWORK | BK_APPCONFIG | BK_SIGNIN | BK_KEY | BK_PROFILE)
#define PROFILE_NAME      "profile/profile.json"
#define PICTURE_NAME      "profile/picture.png"
#define BK_LAYOUT         (SYNC_ITEM_SIDEBAR | SYNC_ITEM_DETAILS | SYNC_ITEM_APPEARANCE | SYNC_ITEM_LANGUAGE | \
                           SYNC_ITEM_MODEL | SYNC_ITEM_SETTINGS | SYNC_ITEM_PERMISSIONS)
#define BK_ALL            (BK_CONTENT | BK_LAYOUT)
#define LAYOUT_NAME       "layout/layout.json"
#define MCP_NAME          "settings/app.json"   /* just the MCP servers, so a restore leaves the rest */

/* The content bits map to the five parts the file code reads and writes. */
static void BitsToParts(DWORD items, BOOL *chosen)
{
    chosen[PART_SESSIONS] = (items & BK_CONVERSATIONS) != 0;
    chosen[PART_COWORK]   = (items & BK_COWORK) != 0;
    chosen[PART_SETTINGS] = (items & BK_APPCONFIG) != 0;
    chosen[PART_SIGNIN]   = (items & BK_SIGNIN) != 0;
    chosen[PART_KEY]      = (items & BK_KEY) != 0;
}

static const char *const kPartNames[PARTS] = { "sessions", "cowork", "settings", "signin", "key" };

/* The app files saved whole under settings/ (no account state, no layout). */
static const WCHAR *const kSettingsFiles[] = { L"window-state.json", L"developer_settings.json" };
static const WCHAR *const kSigninFiles[] = { L"Local State", L"Preferences", CLAUDE_APP_SETTINGS, L"ant-did", L"ant-device-registry.json",
                                             L"buddy-tokens.json", L"ccd-ids.json" };
static const WCHAR *const kSigninFolders[] = { L"Network", L"Local Storage", L"Session Storage", L"IndexedDB", L"WebStorage" };
/* config.json's theme a restore of the appearance carries over; the language goes through the layout. */
static const char *const kAppearanceKeys[] = { "userThemeMode" };

#define PARTS_OPENED_SECTION L"BackupPartsOpened"

#define BACKUP_GROUPS_MAX  16
#define OWN_VALUES         6    /* the profile's own, a value each: name, note, color, badge, title, picture */
#define LAYOUT_VALUES      4000 /* the settings shown one by one at most: a value per part of a layout, and theirs */
#define SIGNIN_VALUES      (7 + CORE_TOKENS)   /* what the sign-in's item shows: its facts, and its tokens */
#define BACKUP_ITEMS_MAX   40
#define BACKUP_GLYPHS_MAX  48

typedef struct BackupDialog {
    const WCHAR *title, *text, *action;
    DWORD        offered, chosen;     /* the items offered and checked (BK_* and SYNC_ITEM_*) */
    int          sessions;            /* the sessions the profile lists, -1: not counted */
    CheckList    parts;
    CheckGroup   groups[BACKUP_GROUPS_MAX];   /* what to sync's groups, Sessions and Account its own */
    CheckItem    items[BACKUP_ITEMS_MAX];
    int          groupCount, itemCount;
    WCHAR        glyphs[BACKUP_GLYPHS_MAX];    /* what to sync's, a gold key added for the account, then the profile's own */
    ThemeTint    tints[BACKUP_GLYPHS_MAX];
    int          images, keyImage, profileImage, ownImage;   /* ownImage: the first of the profile's own values' (kOwnParts) */
    SessionNames names;               /* the sessions listed, by their ids, for the settings that name them */
    WCHAR        label[256];          /* an item's text, as the list takes it */
    ProfileOwn  *own;                 /* the profile's own, shown and changed as values; NULL or none held: one item */
    CheckValue  *values;              /* the profile's own, then the sessions: a heap block (FreeDialog) */
    DWORD        valueParts[OWN_VALUES];   /* each own value's PROFILE_OWN_* */
    int          ownCount;
    const ArchivedSession *sessionList;   /* the sessions shown one by one under theirs, NULL: none */
    int          sessionCount;
    const char  *layout;              /* the settings shown one by one, as their values (a layout), NULL: none */
    size_t       layoutLength;
    int          layoutFirst;         /* the first of their values */
    const WCHAR *colorNames[PALETTE_SIZE];
    COLORREF     swatches[PALETTE_SIZE];
    /* The sign-in its item shows: an export's profile folder, or an import's backup; NULL: neither. */
    const WCHAR *signinStorage;
    const ZipIn *signinZip;
    BOOL         signinRunning;       /* that profile's Claude runs: it closes for the export */
    /* An export's: the profiles it writes, and those it closed to take their sign-in (to open again after). */
    const ProfileList *exportList;
    DWORD        exportProfiles, reopen;
} BackupDialog;

#define RESTORE_SOURCES 16   /* the backups one restore reads at most */

/* A backup a restore reads: its archive, the parts it holds, when it was written. */
typedef struct RestoreSource {
    WCHAR     path[LONG_PATH_CCH];
    WCHAR     name[MAX_PATH];     /* its file's, without ".zip" */
    WCHAR     written[64];        /* when, as this user writes dates */
    ULONGLONG time;
    ZipIn    *zip;
    DWORD     offered;           /* the items it holds (BK_* and SYNC_ITEM_*) */
    int       version;           /* its backup format; 1: before the items were fine */
} RestoreSource;


/* `format` written at `used` in `buffer` (cut short when it is full): where the text ends. */
static size_t AppendA(char *buffer, size_t cch, size_t used, const char *format, ...)
{
    va_list arguments;
    if (used >= cch) return used;
    va_start(arguments, format);
    StringCchVPrintfA(buffer + used, cch - used, format, arguments);
    va_end(arguments);
    return used + strlen(buffer + used);
}

static const WCHAR *PartLabel(DWORD bit, int sessions, WCHAR *text, size_t cch)
{
    switch (bit) {
    case BK_CONVERSATIONS:
        if (sessions >= 0) StringCchPrintfW(text, cch, TR(L"Code sessions and their conversations (%d)"), sessions);
        else StringCchCopyW(text, cch, TR(L"Code sessions and their conversations"));
        return text;
    case BK_COWORK:    return TR(L"Cowork sessions");
    case BK_APPCONFIG: return TR(L"MCP servers and app settings");
    case BK_SIGNIN:    return TR(L"Sign-in: the account, on this PC or another");
    case BK_PROFILE:   return TR(L"Its name, note, icon and window title");
    default:           return NULL;
    }
}

/* The profile's own values, in their order: its name, note, color, badge, window title, picture. */
static const DWORD kOwnParts[OWN_VALUES] = { PROFILE_OWN_NAME, PROFILE_OWN_NOTE, PROFILE_OWN_COLOR, PROFILE_OWN_BADGE, PROFILE_OWN_TITLE,
                                             PROFILE_OWN_PICTURE };

/* The groups and items a backup's list shows: what to sync's, with Sessions
 * and Account the backup's own content, and the Settings group holding the
 * app config too; their glyphs are what to sync's, a gold key added. */
static void BuildTables(BackupDialog *state)
{
    const CheckGroup *sg;
    const CheckItem *si;
    const WCHAR *glyphs;
    const ThemeTint *tints;
    int sgc = 0, sic = 0, images = 0, g, i, settings = 0;
    glyphs = SyncUi_Glyphs(&tints, &images);
    sg = SyncUi_Groups(&sgc);
    si = SyncUi_Items(&sic);
    for (i = 0; i < images && i < BACKUP_GLYPHS_MAX - 2 - OWN_VALUES; i++) {
        state->glyphs[i] = glyphs[i];
        state->tints[i] = tints[i];
    }
    state->keyImage = i;
    state->glyphs[i] = 0xE8D7;             /* a gold key for the account, which what to sync has no image of */
    state->tints[i] = THEME_TINT_GOLD;
    state->profileImage = ++i;
    state->glyphs[i] = 0xE77B;             /* a person for the profile's own */
    state->tints[i] = THEME_TINT_TEAL;
    state->ownImage = ++i;
    for (g = 0; g < OWN_VALUES && i < BACKUP_GLYPHS_MAX; g++, i++) state->glyphs[i] = Theme_ProfileOwnGlyph(kOwnParts[g], &state->tints[i]);
    state->images = i;
    /* Sessions (the backup's content), then what to sync's layout groups, the Settings one given the app config, then Account. */
    state->groups[0] = sg[0];
    state->groups[0].mask = BK_CONVERSATIONS | BK_COWORK;
    for (g = 1, settings = 0; g < sgc; g++) {
        state->groups[g] = sg[g];
        if (sg[g].mask & SYNC_ITEM_SETTINGS) {
            state->groups[g].mask |= BK_APPCONFIG;
            settings = g;
        }
    }
    state->groups[g].mask = BK_SIGNIN;
    state->groups[g].heading = L"Account";
    state->groups[g].image = state->keyImage;
    state->groups[g].name = L"account";
    g++;
    state->groups[g].mask = BK_PROFILE;
    state->groups[g].heading = L"Profile";
    state->groups[g].image = state->profileImage;
    state->groups[g].name = L"profile";
    state->groupCount = g + 1;
    i = 0;
    state->items[i].bit = BK_CONVERSATIONS; state->items[i].label = NULL; state->items[i++].image = sg[0].image;
    state->items[i].bit = BK_COWORK;        state->items[i].label = NULL; state->items[i++].image = sg[0].image;
    for (g = 0; g < sic && i < BACKUP_ITEMS_MAX - 3; g++)
        if (si[g].bit & BK_LAYOUT) state->items[i++] = si[g];   /* the app config comes after, last in Settings */
    state->items[i].bit = BK_APPCONFIG; state->items[i].label = NULL; state->items[i++].image = state->groups[settings].image;
    state->items[i].bit = BK_SIGNIN;    state->items[i].label = NULL; state->items[i++].image = state->keyImage;
    state->items[i].bit = BK_PROFILE;   state->items[i].label = NULL; state->items[i++].image = state->profileImage;
    state->itemCount = i;
}

/* ------------------------------------------------------------ the dialog */

static const WCHAR *BackupItemLabel(const CheckItem *item, void *context)
{
    BackupDialog *state = (BackupDialog *)context;
    return PartLabel(item->bit, state->sessions, state->label, ARRAYSIZE(state->label));   /* NULL for a layout item: its own label */
}

static void FreeDialog(BackupDialog *state)
{
    Util_Free(state->values);
    state->values = NULL;
}

static CheckValue *AddOwnValue(BackupDialog *state, DWORD part, int kind, const WCHAR *label)
{
    CheckValue *value = &state->values[state->parts.valueCount];
    int i;
    value->group = state->groupCount - 1;   /* Profile, the last group */
    value->kind = kind;
    value->image = state->profileImage;
    for (i = 0; i < OWN_VALUES; i++)
        if (kOwnParts[i] == part) value->image = state->ownImage + i;
    value->label = label;
    value->boxed = TRUE;
    value->taken = (state->own->parts & part) != 0;
    state->valueParts[state->ownCount++] = part;
    state->parts.valueCount++;
    return value;
}

static void AddOwnText(BackupDialog *state, DWORD part, const WCHAR *label, const WCHAR *text, int limit)
{
    CheckValue *value;
    if (!(state->own->held & part)) return;
    value = AddOwnValue(state, part, CHECK_VALUE_TEXT, label);
    StringCchCopyW(value->text, ARRAYSIZE(value->text), text);
    value->limit = limit;
}

/* The profile's own as values of its group, in place of its one item: its
 * texts in edit boxes, its color in a drop-down, its picture a box alone. */
static void SetupOwnValues(BackupDialog *state)
{
    const ProfileOwn *own = state->own;
    CheckValue *value;
    int i;
    if (!own || !own->held) return;
    state->itemCount--;   /* BK_PROFILE's item, the last */
    state->parts.itemCount = state->itemCount;
    AddOwnText(state, PROFILE_OWN_NAME, L"Name", own->name, LABEL_CCH - 1);
    AddOwnText(state, PROFILE_OWN_NOTE, L"Notes", own->note, ARRAYSIZE(own->note) - 1);
    if (own->held & PROFILE_OWN_COLOR) {
        for (i = 0; i < PALETTE_SIZE; i++) {
            state->colorNames[i] = TR(g_ColorNames[i]);
            state->swatches[i] = Icons_PaletteColor(i);
        }
        value = AddOwnValue(state, PROFILE_OWN_COLOR, CHECK_VALUE_CHOICE, L"Color");
        value->choices = state->colorNames;
        value->swatches = state->swatches;
        value->choiceCount = PALETTE_SIZE;
        value->choice = own->color >= 0 && own->color < PALETTE_SIZE ? own->color : 0;
    }
    AddOwnText(state, PROFILE_OWN_BADGE, L"Badge", own->badge, BADGE_CCH - 1);
    AddOwnText(state, PROFILE_OWN_TITLE, L"Title", own->title, WINDOW_TITLE_CCH - 1);
    if (own->held & PROFILE_OWN_PICTURE) AddOwnValue(state, PROFILE_OWN_PICTURE, CHECK_VALUE_NONE, L"Picture");
}

/* The values as the person left them, back in the profile's own; its item checked when one is taken. */
static DWORD ReadOwnValues(BackupDialog *state, DWORD chosen)
{
    ProfileOwn *own = state->own;
    int i;
    if (!own || !own->held || !state->ownCount) return chosen;
    own->parts = 0;
    for (i = 0; i < state->ownCount; i++) {
        const CheckValue *value = &state->values[i];
        if (value->taken) own->parts |= state->valueParts[i];
        switch (state->valueParts[i]) {
        case PROFILE_OWN_NAME:  StringCchCopyW(own->name, ARRAYSIZE(own->name), value->text); break;
        case PROFILE_OWN_NOTE:  StringCchCopyW(own->note, ARRAYSIZE(own->note), value->text); break;
        case PROFILE_OWN_BADGE: StringCchCopyW(own->badge, ARRAYSIZE(own->badge), value->text); break;
        case PROFILE_OWN_TITLE: StringCchCopyW(own->title, ARRAYSIZE(own->title), value->text); break;
        case PROFILE_OWN_COLOR: own->color = value->choice; break;
        }
    }
    if (!own->name[0]) own->parts &= ~PROFILE_OWN_NAME;   /* a profile keeps a name */
    return own->parts ? chosen | BK_PROFILE : chosen & ~BK_PROFILE;
}

static int __cdecl LatestFirst(const void *a, const void *b)
{
    ULONGLONG x = ((const ArchivedSession *)a)->lastActivity, y = ((const ArchivedSession *)b)->lastActivity;
    return x > y ? -1 : x < y;
}

/* A time (ms since 1970) as this user writes a date and time. */
static void FormatWhen(ULONGLONG ms, WCHAR *out, size_t cch)
{
    WCHAR date[64], time[64];
    out[0] = 0;
    if (ms && Util_FormatWhen(ms, TIME_NOSECONDS, date, ARRAYSIZE(date), time, ARRAYSIZE(time))) StringCchPrintfW(out, cch, L"%s %s", date, time);
}

/* The sessions one by one under theirs: each one's box, its title, when it was last used. */
static void SetupSessionValues(BackupDialog *state)
{
    int i;
    for (i = 0; i < state->sessionCount; i++) {
        CheckValue *value = &state->values[state->parts.valueCount++];
        value->group = 0;   /* Sessions */
        value->item = BK_CONVERSATIONS;
        value->kind = CHECK_VALUE_NONE;
        value->image = state->groups[0].image;
        value->label = state->sessionList[i].title;
        value->literal = TRUE;
        value->boxed = value->taken = TRUE;
        value->pick = -1;
        value->data = i;
        FormatWhen(state->sessionList[i].lastActivity, value->text, ARRAYSIZE(value->text));
    }
}

/* The sessions left checked, by key into `choice`; none when they all are (every one taken). */
static BOOL TakeSessions(const BackupDialog *state, WCHAR (**keys)[SESSION_ID_CCH], int *count)
{
    int v, all = 0, taken = 0;
    *keys = NULL;
    *count = 0;
    for (v = 0; state->values && v < state->parts.valueCount; v++)
        if (state->values[v].item == BK_CONVERSATIONS && state->values[v].boxed) {
            all++;
            taken += state->values[v].taken != FALSE;
        }
    if (!all || taken == all) return TRUE;
    if ((*keys = (WCHAR (*)[SESSION_ID_CCH])Util_Alloc((size_t)max(taken, 1) * sizeof **keys)) == NULL) return FALSE;
    for (v = 0; v < state->parts.valueCount; v++)
        if (state->values[v].item == BK_CONVERSATIONS && state->values[v].boxed && state->values[v].taken)
            StringCchCopyW((*keys)[(*count)++], SESSION_ID_CCH, state->sessionList[state->values[v].data].key);
    return TRUE;
}

/* The settings one by one (SyncUi_AddLayoutValues), after the other values, each with a box; the sessions they name, by their titles. */
static void SetupLayoutValues(BackupDialog *state)
{
    state->layoutFirst = state->parts.valueCount;
    state->names.sessions = state->sessionList;
    state->names.count = state->sessionList ? state->sessionCount : 0;
    SyncUi_AddLayoutValues(&state->parts, state->layoutFirst + LAYOUT_VALUES, state->layout, state->layoutLength, state->offered & BK_LAYOUT, TRUE,
                           &state->names);
}

/* The settings changed in the list, as a layout of their parts alone: a heap block, NULL for none. */
static char *TakeLayoutChanges(const BackupDialog *state, size_t *length)
{
    return SyncUi_LayoutChanges(&state->parts, state->layoutFirst, state->layout, state->layoutLength, length);
}

/* `layout` (a heap block, freed on failure) with the parts of `changes` its items hold set to theirs; one null
 * there (left out) taken out. */
static char *WithChanges(char *layout, size_t *length, const char *changes, size_t changesLength, DWORD items)
{
    LayoutPart part;
    const char *value;
    size_t valueLength;
    int p;
    for (p = 0; layout && changes && SessionSync_LayoutPart(p, &part); p++) {
        if (!(part.item & items) || !Core_JsonMember(changes, changesLength, part.name, &value, &valueLength)) continue;
        if (valueLength == 4 && memcmp(value, "null", 4) == 0) {
            if (!Core_JsonRemoveMember(layout, *length, part.name, layout, *length + 1, length)) {
                Util_Free(layout);
                return NULL;
            }
            layout[*length] = 0;
        } else {
            layout = SessionSync_SetMember(layout, length, part.name, value, valueLength);
        }
    }
    return layout;
}

/* ------------------------------------------------------------ the sign-in shown */

BOOL Backup_ReadAppKey(const WCHAR *storage, BYTE *key, size_t cap, size_t *length);   /* below, with the key's other code */

/* What config.json (`config`) says of the sign-in: whose it is, and its
 * token caches (Electron's safeStorage) opened with `key` (NULL: they stay
 * closed) and read for what they say of themselves (Core_ReadTokenCache),
 * never a token. */
static void ReadSignInFacts(const char *config, size_t length, const BYTE *key, size_t keyLength, CoreSignIn *facts)
{
    static const char *const kCaches[] = { "oauth:tokenCacheV2", "oauth:tokenCache" };
    size_t i;
    ZeroMemory(facts, sizeof *facts);
    if (!config) return;
    Core_JsonMemberString(config, length, "lastKnownAccountUuid", facts->account, ARRAYSIZE(facts->account));
    for (i = 0; key && i < ARRAYSIZE(kCaches); i++) {
        const char *value;
        size_t valueLength, sealedLength, plainLength = 0;
        BYTE *sealed, *plain;
        if (!Core_JsonMember(config, length, kCaches[i], &value, &valueLength) || valueLength < 2 || value[0] != '"') continue;
        sealed = (BYTE *)Util_Alloc(valueLength);
        plain = (BYTE *)Util_Alloc(valueLength);
        if (sealed && plain && (sealedLength = Core_Base64Decode(value + 1, valueLength - 2, sealed, valueLength)) != (size_t)-1 &&
            Core_OpenSafeStorage(sealed, sealedLength, key, keyLength, plain, valueLength, &plainLength))
            Core_ReadTokenCache((const char *)plain, plainLength, facts);
        if (plain) SecureZeroMemory(plain, valueLength);   /* the tokens themselves */
        Util_Free(plain);
        Util_Free(sealed);
    }
}

/* The bytes of `path`, a file or a folder and all it holds (links not followed). */
static ULONGLONG TreeBytes(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    WIN32_FIND_DATAW found;
    WCHAR inner[LONG_PATH_CCH];
    HANDLE find;
    ULONGLONG bytes = 0;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) return 0;
    if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return ((ULONGLONG)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    if ((find = Util_FindFiles(path, L"*", &found, FALSE)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0 || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (SUCCEEDED(StringCchPrintfW(inner, ARRAYSIZE(inner), L"%s\\%s", path, found.cFileName))) bytes += TreeBytes(inner);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return bytes;
}

/* A read-only value of the sign-in's item: `label` (a catalog key; literal: as it is) and `text`. */
static CheckValue *AddSignInValue(BackupDialog *state, const WCHAR *label, BOOL literal, const WCHAR *text)
{
    CheckValue *value = &state->values[state->parts.valueCount++];
    value->group = state->groupCount - 2;   /* Account, before Profile */
    value->item = BK_SIGNIN;
    value->kind = CHECK_VALUE_TEXT;
    value->readOnly = TRUE;
    value->image = state->keyImage;
    value->label = label;
    value->literal = literal;
    value->pick = -1;
    StringCchCopyW(value->text, ARRAYSIZE(value->text), text);
    return value;
}

/* The sign-in's item holding what it is, a value each: the account and its
 * organization, the plan, the tokens one by one, where it opens, how much it
 * is and, for a profile whose Claude runs, that it closes for the export. */
static void SetupSignInValues(BackupDialog *state)
{
    CoreSignIn facts;
    WCHAR text[CHECK_VALUE_CCH], date[64], hour[64];
    BYTE key[64];
    size_t keyLength = 0, i;
    ULONGLONG bytes = 0;
    DWORD length = 0;
    char *config = NULL;
    BOOL keyed = FALSE;
    int tokens, t;
    if (!(state->offered & BK_SIGNIN) || (!state->signinStorage && !state->signinZip)) return;
    if (state->signinStorage) {
        WCHAR path[LONG_PATH_CCH];
        keyed = Backup_ReadAppKey(state->signinStorage, key, sizeof key, &keyLength);
        if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_APP_SETTINGS, state->signinStorage)))
            config = Util_ReadFile(path, JSON_MAX, FALSE, &length);
        for (i = 0; i < ARRAYSIZE(kSigninFiles) + ARRAYSIZE(kSigninFolders); i++)
            if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", state->signinStorage,
                                           i < ARRAYSIZE(kSigninFiles) ? kSigninFiles[i] : kSigninFolders[i - ARRAYSIZE(kSigninFiles)])))
                bytes += TreeBytes(path);
    } else {
        int item = Zip_Find(state->signinZip, KEY_NAME), n = Zip_Count(state->signinZip);
        char *held = item >= 0 ? (char *)Zip_Read(state->signinZip, item, MANIFEST_MAX, &length) : NULL;
        const char *value;
        size_t valueLength;
        if (held && Core_JsonMember(held, length, "key", &value, &valueLength) && valueLength > 2 && value[0] == '"')
            keyed = (keyLength = Core_Base64Decode(value + 1, valueLength - 2, key, sizeof key)) != (size_t)-1 && keyLength;
        if (held) SecureZeroMemory(held, length);
        Util_Free(held);
        if ((item = Zip_Find(state->signinZip, "signin/" "config.json")) >= 0) config = (char *)Zip_Read(state->signinZip, item, JSON_MAX, &length);
        for (item = 0; item < n; item++)
            if (strncmp(Zip_Name(state->signinZip, item), "signin/", 7) == 0) bytes += Zip_Size(state->signinZip, item);
    }
    ReadSignInFacts(config, length, keyed ? key : NULL, keyLength, &facts);
    SecureZeroMemory(key, sizeof key);
    Util_Free(config);
    AddSignInValue(state, L"Account", FALSE, facts.account[0] ? facts.account : L"\x2014");
    if (facts.organization[0]) AddSignInValue(state, L"Organization", FALSE, facts.organization);
    if (facts.plan[0] || facts.tier[0]) {
        StringCchPrintfW(text, ARRAYSIZE(text), L"%s%s%s", facts.plan, facts.plan[0] && facts.tier[0] ? L" \x00B7 " : L"", facts.tier);
        AddSignInValue(state, L"Plan", FALSE, text);
    }
    if ((tokens = facts.tokenCount) > 0) {
        int parent = state->parts.valueCount;
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%d items"), tokens);
        AddSignInValue(state, L"Access tokens", FALSE, text);
        for (t = 0; t < tokens; t++) {
            CheckValue *value;
            text[0] = 0;
            if (facts.tokens[t].expires && Util_FormatWhen(facts.tokens[t].expires, TIME_NOSECONDS, date, ARRAYSIZE(date), hour, ARRAYSIZE(hour)))
                StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Until %s %s"), date, hour);
            if (facts.tokens[t].renews) StringCchPrintfW(text + wcslen(text), ARRAYSIZE(text) - wcslen(text), L"%s%s", text[0] ? L" \x00B7 " : L"", TR(L"renews itself"));
            value = AddSignInValue(state, NULL, TRUE, text);
            StringCchCopyW(value->name, ARRAYSIZE(value->name), facts.tokens[t].scopes);
            value->label = value->name;
            value->parent = parent + 1;
        }
    }
    AddSignInValue(state, L"Opens on", FALSE, keyed ? TR(L"This PC and any other: its key goes with it") : TR(L"This PC only, for this Windows user"));
    Core_FormatSize(bytes, text, ARRAYSIZE(text));
    AddSignInValue(state, L"Size", FALSE, text);
    if (state->signinRunning) AddSignInValue(state, L"State", FALSE, TR(L"Open: it closes while it is exported, then opens again"));
}

/* The parts offered in the checked list, those chosen checked. */
static void SetupParts(BackupDialog *state, HWND list)
{
    BuildTables(state);
    ZeroMemory(&state->parts, sizeof state->parts);
    state->parts.groups = state->groups;
    state->parts.groupCount = state->groupCount;
    state->parts.items = state->items;
    state->parts.itemCount = state->itemCount;
    state->parts.openedSection = PARTS_OPENED_SECTION;
    state->parts.label = BackupItemLabel;
    state->parts.context = state;
    state->parts.offered = state->offered;
    state->parts.checked = state->chosen & state->offered;
    state->values = (CheckValue *)Util_Alloc((size_t)(OWN_VALUES + max(state->sessionCount, 0) + SIGNIN_VALUES + LAYOUT_VALUES) * sizeof *state->values);
    state->parts.values = state->values;
    if (state->values && (state->offered & BK_PROFILE)) SetupOwnValues(state);
    if (state->values && (state->offered & BK_CONVERSATIONS)) SetupSessionValues(state);
    if (state->values) SetupSignInValues(state);
    if (state->values && state->layout) SetupLayoutValues(state);
    CheckList_Setup(&state->parts, list, state->glyphs, state->tints, state->images);
}

CheckList *Backup_FillLayoutList(HWND list, const char *layout, size_t length)
{
    static BackupDialog shown;   /* an import's list, as the tests show it */
    static ArchivedSession sessions[3];
    static const WCHAR *const titles[3] = { L"Fix the sign-in page", L"Refactor the parser", L"Draft the release notes" };
    ULONGLONG now = Util_NowMs();
    int i;
    for (i = 0; i < 3; i++) {
        StringCchPrintfW(sessions[i].key, ARRAYSIZE(sessions[i].key), L"abababab-0000-4000-8000-00000000c00%d", i + 1);
        StringCchPrintfW(sessions[i].own, ARRAYSIZE(sessions[i].own), L"local_%s", sessions[i].key);
        StringCchCopyW(sessions[i].title, ARRAYSIZE(sessions[i].title), titles[i]);
        sessions[i].lastActivity = now - (ULONGLONG)(i * i + 1) * 3600000ull * 24;
    }
    FreeDialog(&shown);
    ZeroMemory(&shown, sizeof shown);
    shown.sessionList = sessions;
    shown.sessionCount = shown.sessions = 3;
    shown.layout = layout;
    shown.layoutLength = length;
    shown.offered = BK_ALL;
    shown.chosen = (BK_CONTENT | BK_LAYOUT) & ~BK_KEY;
    SetupParts(&shown, list);
    return &shown.parts;
}

CheckList *Backup_FillImportList(HWND list)
{
    static const char layout[] = "{\"autoArchive\":true,\"coworkWebSearch\":false,\"coworkBrowser\":\"chrome\",\"coworkSchedules\":true,"
                                 "\"zoom\":1.1,\"locale\":\"zh-CN\",\"defaultModel\":\"claude-opus\","
                                 "\"starred\":[\"local_abababab-0000-4000-8000-00000000c001\",\"local_abababab-0000-4000-8000-00000000c002\"],"
                                 "\"editor\":{\"state\":{\"editorFont\":\"\",\"chatTextSize\":\"m\"},\"version\":0}}";
    return Backup_FillLayoutList(list, layout, sizeof layout - 1);
}

CheckList *Backup_FillPartsList(HWND list, int sessions)
{
    static BackupDialog shown;   /* a list alone, as the tests show it */
    static ProfileOwn own;       /* a profile's own, as an import shows it */
    FreeDialog(&shown);
    ZeroMemory(&shown, sizeof shown);
    ZeroMemory(&own, sizeof own);
    StringCchCopyW(own.name, ARRAYSIZE(own.name), L"Work");
    StringCchCopyW(own.note, ARRAYSIZE(own.note), L"Client projects");
    StringCchCopyW(own.badge, ARRAYSIZE(own.badge), L"W");
    own.color = 2;
    own.held = own.parts = PROFILE_OWN_NAME | PROFILE_OWN_NOTE | PROFILE_OWN_COLOR | PROFILE_OWN_BADGE | PROFILE_OWN_TITLE | PROFILE_OWN_PICTURE;
    shown.own = &own;
    shown.sessions = sessions;
    shown.offered = BK_ALL;
    shown.chosen = (BK_CONTENT | BK_LAYOUT) & ~BK_KEY;   /* everything, as Backup_Create offers it */
    SetupParts(&shown, list);
    return &shown.parts;
}

static void ReadChosen(HWND dialog, BackupDialog *state)
{
    state->chosen = ReadOwnValues(state, state->parts.checked);
    EnableWindow(GetDlgItem(dialog, IDOK), state->chosen != 0);
}

/* An export taking the sign-in of a profile whose Claude runs: its files are
 * in use while it runs (Chromium holds its cookies for itself), so it closes
 * first, asked, to open again once the export is written (`reopen`). FALSE:
 * back to the list, where the sign-in can be left out. */
static BOOL CloseForSignIn(HWND dialog, BackupDialog *state)
{
    const ProfileList *list = state->exportList;
    WCHAR names[MAX_PROFILES * (LABEL_CCH + 8)], text[MAX_PROFILES * (LABEL_CCH + 8) + 512];
    DWORD running = 0, error = 0;
    HCURSOR old;
    int i;
    if (!list || !(state->chosen & BK_SIGNIN)) return TRUE;
    for (i = 0; i < list->count; i++)
        if ((state->exportProfiles & (1u << i)) && Claude_IsRunning(&list->items[i])) running |= 1u << i;
    if (!running) return TRUE;
    Profiles_JoinNames(list, running, names, ARRAYSIZE(names));
    StringCchPrintfW(text, ARRAYSIZE(text),
                     TR(L"%s is open, and its sign-in files are in use while it runs. Close it for the export, then open it again?\n\nTo export without the sign-in, uncheck it in the list."),
                     names);
    if (!Ui_Ask(dialog, IDI_QUESTION, text, TR(L"Close and export"), TR(L"Cancel"), FALSE)) return FALSE;
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    for (i = 0; i < list->count; i++) {
        if (!(running & (1u << i))) continue;
        if (!Claude_Quit(&list->items[i], &error)) {
            SetCursor(old);
            Ui_Message(dialog, MB_ICONERROR, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), list->items[i].name, error);
            return FALSE;
        }
        state->reopen |= 1u << i;
        Util_Log(L"export of %s: closed to take its sign-in", list->items[i].folder);
    }
    SetCursor(old);
    return TRUE;
}

static INT_PTR CALLBACK BackupProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    BackupDialog *state = (BackupDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        HWND list = GetDlgItem(dialog, IDC_B_LIST);
        state = (BackupDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, state->title);
        SetDlgItemTextW(dialog, IDC_B_TEXT, state->text);
        SetDlgItemTextW(dialog, IDOK, state->action);
        SetupParts(state, list);
        Theme_SmoothView(list);
        ReadChosen(dialog, state);
        return TRUE;
    }

    case WM_NOTIFY:
    case WM_CHECKLIST_BOX:
    case WM_CONTEXTMENU: {
        BOOL toggled;
        if (state && CheckList_Message(&state->parts, dialog, message, wp, lp, &toggled)) {
            if (toggled) ReadChosen(dialog, state);
            return TRUE;
        }
        break;
    }

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wp) == IDOK) {
            ReadChosen(dialog, state);
            if (CloseForSignIn(dialog, state)) EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        if (LOWORD(wp) == IDC_B_ALL || LOWORD(wp) == IDC_B_NONE || LOWORD(wp) == IDC_B_INVERT) {
            /* Every box at once, the pane's too: all, none, or each turned over. */
            CheckList_CheckAll(&state->parts, LOWORD(wp) == IDC_B_ALL ? CHECKLIST_ALL : LOWORD(wp) == IDC_B_NONE ? CHECKLIST_NONE : CHECKLIST_INVERT);
            ReadChosen(dialog, state);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------ a password */

typedef struct PasswordDialog {
    const WCHAR *title, *text;
    BOOL         twice;               /* a new password: typed twice, PASSWORD_MIN characters at least */
    WCHAR        password[256];
} PasswordDialog;

static INT_PTR CALLBACK PasswordProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    PasswordDialog *state = (PasswordDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG:
        state = (PasswordDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, state->title);
        SetDlgItemTextW(dialog, IDC_K_TEXT, state->text);
        SendDlgItemMessageW(dialog, IDC_K_PASSWORD, EM_LIMITTEXT, ARRAYSIZE(state->password) - 1, 0);
        SendDlgItemMessageW(dialog, IDC_K_AGAIN, EM_LIMITTEXT, ARRAYSIZE(state->password) - 1, 0);
        if (!state->twice) {
            ShowWindow(GetDlgItem(dialog, IDC_K_AGAIN_LABEL), SW_HIDE);
            ShowWindow(GetDlgItem(dialog, IDC_K_AGAIN), SW_HIDE);
        }
        SetFocus(GetDlgItem(dialog, IDC_K_PASSWORD));
        return FALSE;

    case WM_COMMAND:
        if (!state) break;
        if (LOWORD(wp) == IDOK) {
            WCHAR again[ARRAYSIZE(state->password)];
            const WCHAR *problem = NULL;
            GetDlgItemTextW(dialog, IDC_K_PASSWORD, state->password, ARRAYSIZE(state->password));
            GetDlgItemTextW(dialog, IDC_K_AGAIN, again, ARRAYSIZE(again));
            if (!state->password[0]) problem = TR(L"Enter the password.");
            else if (state->twice && wcslen(state->password) < PASSWORD_MIN) problem = TR(L"Use at least %d characters.");
            else if (state->twice && wcscmp(state->password, again) != 0) problem = TR(L"The two passwords differ.");
            SecureZeroMemory(again, sizeof again);
            if (!problem) {
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            SecureZeroMemory(state->password, sizeof state->password);
            Ui_Message(dialog, MB_ICONINFORMATION, problem, PASSWORD_MIN);
            SendMessageW(dialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dialog, IDC_K_PASSWORD), TRUE);
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

/* `result`, the password forgotten first. */
static BOOL ForgetPassword(PasswordDialog *password, BOOL result)
{
    SecureZeroMemory(password->password, sizeof password->password);
    return result;
}

/* ------------------------------------------------------------ the sign-in's key */

/* Local State's os_crypt key, the AES key of the cookies and the app's
 * storage, opened with DPAPI: FALSE when this Windows user cannot open it
 * (another PC or user) or there is none. */
BOOL Backup_ReadAppKey(const WCHAR *storage, BYTE *key, size_t cap, size_t *length)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD size = 0;
    char *json;
    const char *crypt, *value;
    size_t cryptLength, valueLength, blobLength;
    BYTE blob[1024];
    DATA_BLOB in, out;
    BOOL ok = FALSE;
    *length = 0;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" LOCAL_STATE, storage)) || (json = Util_ReadFile(path, JSON_MAX, FALSE, &size)) == NULL)
        return FALSE;
    if (Core_JsonMember(json, size, "os_crypt", &crypt, &cryptLength) && Core_JsonMember(crypt, cryptLength, "encrypted_key", &value, &valueLength) &&
        valueLength > 2 && value[0] == '"' && (blobLength = Core_Base64Decode(value + 1, valueLength - 2, blob, sizeof blob)) != (size_t)-1 &&
        blobLength > 5 && memcmp(blob, "DPAPI", 5) == 0) {
        in.pbData = blob + 5;
        in.cbData = (DWORD)(blobLength - 5);
        if (CryptUnprotectData(&in, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
            if (out.cbData <= cap) {
                memcpy(key, out.pbData, out.cbData);
                *length = out.cbData;
                ok = TRUE;
            }
            SecureZeroMemory(out.pbData, out.cbData);
            LocalFree(out.pbData);
        }
    }
    HeapFree(GetProcessHeap(), 0, json);
    return ok;
}

/* `key` protected again with DPAPI for this Windows user, as Local State's os_crypt key. */
BOOL Backup_WriteAppKey(const WCHAR *storage, const BYTE *key, size_t length)
{
    static const char *const kKeyPath[] = { "os_crypt", "encrypted_key" };
    WCHAR path[LONG_PATH_CCH];
    DWORD size = 0;
    char *json, *changed = NULL, quoted[1024];
    BYTE blob[1024];
    DATA_BLOB in, out;
    size_t changedLength = 0;
    BOOL ok = FALSE;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" LOCAL_STATE, storage)) || (json = Util_ReadFile(path, JSON_MAX, FALSE, &size)) == NULL)
        return FALSE;
    in.pbData = (BYTE *)key;
    in.cbData = (DWORD)length;
    if (CryptProtectData(&in, L"Claude Desktop Profiles Manager", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        if (out.cbData + 5 <= sizeof blob) {
            memcpy(blob, "DPAPI", 5);
            memcpy(blob + 5, out.pbData, out.cbData);
            quoted[0] = '"';
            if (Core_Base64Encode(blob, out.cbData + 5, quoted + 1, sizeof quoted - 2)) {
                StringCchCatA(quoted, ARRAYSIZE(quoted), "\"");
                changed = Core_JsonSetNested(json, size, kKeyPath, 2, quoted, &changedLength);
            }
        }
        LocalFree(out.pbData);
    }
    if (changed) {
        /* In one step: a Local State cut short would lose the key and with it the sign-in. */
        ok = Util_WriteFileAtomic(path, changed, changedLength, UTIL_PUT_REPLACE);
        HeapFree(GetProcessHeap(), 0, changed);
    }
    HeapFree(GetProcessHeap(), 0, json);
    return ok;
}

typedef enum ArchiveChoice { ARCHIVE_SAVE, ARCHIVE_OPEN, ARCHIVE_FOLDER } ArchiveChoice;

/* "<profile> <date>.zip", without the characters a file name cannot hold. */
void Backup_ArchiveName(const WCHAR *profileName, WCHAR *name, size_t cch)
{
    SYSTEMTIME now;
    size_t i;
    GetLocalTime(&now);
    StringCchPrintfW(name, cch, L"%s %04u-%02u-%02u." ARCHIVE_EXTENSION, profileName, now.wYear, now.wMonth, now.wDay);
    for (i = 0; name[i]; i++)
        if (wcschr(L"\\/:*?\"<>|", name[i])) name[i] = L'_';
}

/* The file dialog that chooses the archive to write (named after the
 * profile), the backups to read, one or several, or the folder the archives
 * of several profiles go to; NULL, said, when it could not be made. */
static IFileDialog *ArchiveDialog(HWND owner, ArchiveChoice choice, const WCHAR *title, const WCHAR *profileName)
{
    IFileDialog *dialog = NULL;
    COMDLG_FILTERSPEC filter;
    FILEOPENDIALOGOPTIONS options = 0, extra;
    HRESULT hr = CoCreateInstance(choice == ARCHIVE_SAVE ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileDialog,
                                  (void **)&dialog);
    if (FAILED(hr)) {
        Ui_Message(owner, MB_ICONERROR, TR(L"The file could not be chosen (error 0x%08lX)."), (unsigned long)hr);
        return NULL;
    }
    IFileDialog_SetTitle(dialog, title);
    if (choice != ARCHIVE_FOLDER) {
        filter.pszName = TR(L"Profile backup (*.zip)");
        filter.pszSpec = L"*." ARCHIVE_EXTENSION;
        IFileDialog_SetFileTypes(dialog, 1, &filter);
        IFileDialog_SetDefaultExtension(dialog, ARCHIVE_EXTENSION);
    }
    if (choice == ARCHIVE_SAVE) {
        WCHAR name[MAX_PATH];
        Backup_ArchiveName(profileName, name, ARRAYSIZE(name));
        IFileDialog_SetFileName(dialog, name);
    }
    extra = choice == ARCHIVE_SAVE ? FOS_OVERWRITEPROMPT : choice == ARCHIVE_OPEN ? FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT : FOS_PICKFOLDERS;
    if (SUCCEEDED(IFileDialog_GetOptions(dialog, &options)))
        IFileDialog_SetOptions(dialog, options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | extra);
    return dialog;
}

/* The dialog's failure said, unless the person cancelled it. */
static void ChoiceFailed(HWND owner, HRESULT hr)
{
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        Ui_Message(owner, MB_ICONERROR, TR(L"The file could not be chosen (error 0x%08lX)."), (unsigned long)hr);
}

/* The archive the user chose to write, or the folder the archives of several profiles go to. */
static BOOL ChooseArchive(HWND owner, ArchiveChoice choice, const WCHAR *title, const WCHAR *profileName, WCHAR *path, size_t cch)
{
    IFileDialog *dialog = ArchiveDialog(owner, choice, title, profileName);
    IShellItem *result = NULL;
    PWSTR chosen = NULL;
    BOOL ok = FALSE, save = choice == ARCHIVE_SAVE;
    HRESULT hr;
    if (!dialog) return FALSE;
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
    ChoiceFailed(owner, hr);
    IFileDialog_Release(dialog);
    return ok;
}

/* The backups the user chose to restore from, RESTORE_SOURCES at most,
 * their paths in `sources`: how many, 0 for none. */
static int ChooseBackups(HWND owner, const WCHAR *title, const WCHAR *profileName, RestoreSource *sources)
{
    IFileDialog *dialog = ArchiveDialog(owner, ARCHIVE_OPEN, title, profileName);
    IFileOpenDialog *open = NULL;
    IShellItemArray *results = NULL;
    DWORD count = 0, i;
    int chosen = 0;
    BOOL tooLong = FALSE;
    HRESULT hr;
    if (!dialog) return 0;
    hr = IFileDialog_Show(dialog, owner);
    if (SUCCEEDED(hr)) hr = IFileDialog_QueryInterface(dialog, &IID_IFileOpenDialog, (void **)&open);
    if (SUCCEEDED(hr)) hr = IFileOpenDialog_GetResults(open, &results);
    if (SUCCEEDED(hr)) hr = IShellItemArray_GetCount(results, &count);
    if (SUCCEEDED(hr) && count > RESTORE_SOURCES) {
        Ui_Message(owner, MB_ICONWARNING, TR(L"Choose %d backups at most."), RESTORE_SOURCES);
        count = 0;
    }
    for (i = 0; SUCCEEDED(hr) && !tooLong && i < count; i++) {
        IShellItem *item = NULL;
        PWSTR path = NULL;
        if (SUCCEEDED(hr = IShellItemArray_GetItemAt(results, i, &item)) && SUCCEEDED(hr = IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
            tooLong = FAILED(StringCchCopyW(sources[chosen].path, ARRAYSIZE(sources[chosen].path), path));
            if (!tooLong) chosen++;
            CoTaskMemFree(path);
        }
        if (item) IShellItem_Release(item);
    }
    if (tooLong) Ui_Message(owner, MB_ICONERROR, TR(L"The path is too long."));
    else ChoiceFailed(owner, hr);
    if (results) IShellItemArray_Release(results);
    if (open) IFileOpenDialog_Release(open);
    IFileDialog_Release(dialog);
    return tooLong || FAILED(hr) ? 0 : chosen;
}

/* Backup `source` opened: the parts it holds, its name, when it was written.
 * FALSE when it is not a backup of ours. */
static BOOL OpenSource(RestoreSource *source)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    SYSTEMTIME utc, local;
    WCHAR date[32], hour[32];
    const WCHAR *leaf = wcsrchr(source->path, L'\\');
    const char *value;
    size_t valueLength, i;
    DWORD length = 0;
    char *manifest;
    int item, part;
    StringCchCopyW(source->name, ARRAYSIZE(source->name), leaf ? leaf + 1 : source->path);
    if (Core_EndsWithI(source->name, L"." ARCHIVE_EXTENSION)) source->name[wcslen(source->name) - ARRAYSIZE(L"." ARCHIVE_EXTENSION) + 1] = 0;
    source->zip = Zip_Open(source->path);
    item = source->zip ? Zip_Find(source->zip, MANIFEST_NAME) : -1;
    manifest = item >= 0 ? (char *)Zip_Read(source->zip, item, MANIFEST_MAX, &length) : NULL;
    if (!manifest || !Core_JsonMember(manifest, length, "format", &value, &valueLength) || valueLength != sizeof(BACKUP_FORMAT) + 1 ||
        memcmp(value + 1, BACKUP_FORMAT, sizeof(BACKUP_FORMAT) - 1) != 0) {
        Util_Free(manifest);
        return FALSE;
    }
    if (Core_JsonMember(manifest, length, "version", &value, &valueLength)) {
        ULONGLONG n = 0;
        Core_JsonNumber(value, valueLength, &n);
        source->version = (int)n;
    }
    if (Core_JsonMember(manifest, length, "items", &value, &valueLength)) {
        ULONGLONG n = 0;
        Core_JsonNumber(value, valueLength, &n);
        source->offered = (DWORD)n;
    } else if (Core_JsonMember(manifest, length, "parts", &value, &valueLength)) {
        /* A backup from before the items were fine: its coarse parts become bits (its settings held the whole config and the layout). */
        static const DWORD kPartBits[PARTS] = { BK_CONVERSATIONS, BK_COWORK, BK_APPCONFIG | BK_LAYOUT, BK_SIGNIN, BK_KEY };
        for (part = 0; part < PARTS; part++) {
            char needle[32];
            StringCchPrintfA(needle, ARRAYSIZE(needle), "\"%s\"", kPartNames[part]);
            for (i = 0; i + strlen(needle) <= valueLength; i++)
                if (memcmp(value + i, needle, strlen(needle)) == 0) source->offered |= kPartBits[part];
        }
    }
    Util_Free(manifest);
    if (GetFileAttributesExW(source->path, GetFileExInfoStandard, &data)) {
        source->time = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
        if (FileTimeToSystemTime(&data.ftLastWriteTime, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, &local) &&
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, NULL, date, ARRAYSIZE(date), NULL) &&
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &local, NULL, hour, ARRAYSIZE(hour)))
            StringCchPrintfW(source->written, ARRAYSIZE(source->written), L"%s %s", date, hour);
    }
    return TRUE;
}

/* ------------------------------------------------------------- writing */

/* `name` (a path inside the archive, "/" between folders) in UTF-8. */
static BOOL Utf8Name(const WCHAR *name, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, out, cap, NULL, NULL);
    int i;
    if (n <= 0) return FALSE;
    for (i = 0; out[i]; i++)
        if (out[i] == '\\') out[i] = '/';
    return TRUE;
}

/* Every file of `dir` (links not followed) under `prefix` in the archive. */
static BOOL AddTreeWithTasks(ZipOut *zip, const WCHAR *dir, const WCHAR *prefix, const Profile *profile)
{
    WCHAR path[LONG_PATH_CCH], name[LONG_PATH_CCH];
    char utf8[3 * LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(dir, L"*", &found, FALSE);
    BOOL ok = TRUE;
    if (find == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0 || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName)) ||
            FAILED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s/%s", prefix, found.cFileName))) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            ok = FALSE;
            break;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = AddTreeWithTasks(zip, path, name, profile) && ok;
        else {
            ok = Utf8Name(name, utf8, sizeof utf8) && Zip_AddFile(zip, utf8, path);
            if (ok && profile && Core_EqualsI(found.cFileName, SCHEDULED_TASKS))
                ok = VaultTasks_AddRegistry(zip, profile, path, utf8, FALSE, NULL, 0);
        }
    } while (ok && FindNextFileW(find, &found));
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) ok = FALSE;
    FindClose(find);
    return ok;
}

static BOOL AddTree(ZipOut *zip, const WCHAR *dir, const WCHAR *prefix)
{
    return AddTreeWithTasks(zip, dir, prefix, NULL);
}

/* `storage`\`file` under `prefix`/`file`, when it is there. */
static BOOL AddIfThere(ZipOut *zip, const WCHAR *storage, const WCHAR *file, const WCHAR *prefix)
{
    WCHAR path[LONG_PATH_CCH], name[MAX_PATH];
    char utf8[3 * MAX_PATH];
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", storage, file)) || !Util_FileExists(path)) return TRUE;
    return SUCCEEDED(StringCchPrintfW(name, ARRAYSIZE(name), L"%s/%s", prefix, file)) && Utf8Name(name, utf8, sizeof utf8) &&
           Zip_AddFile(zip, utf8, path);
}

/* The language and theme of config.json, as a JSON object of their own. */
static BOOL AddAppearance(ZipOut *zip, const WCHAR *storage)
{
    WCHAR path[LONG_PATH_CCH];
    char out[1024];
    size_t used = 0, i;
    DWORD length = 0;
    char *json;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_APP_SETTINGS, storage)) ||
        (json = Util_ReadFile(path, JSON_MAX, FALSE, &length)) == NULL)
        return TRUE;
    out[used++] = '{';
    for (i = 0; i < ARRAYSIZE(kAppearanceKeys); i++) {
        const char *value;
        size_t valueLength;
        if (!Core_JsonMember(json, length, kAppearanceKeys[i], &value, &valueLength) || used + strlen(kAppearanceKeys[i]) + valueLength + 8 > sizeof out)
            continue;
        if (used > 1) out[used++] = ',';
        used = AppendA(out, sizeof out, used, "\"%s\":", kAppearanceKeys[i]);
        memcpy(out + used, value, valueLength);
        used += valueLength;
    }
    out[used++] = '}';
    HeapFree(GetProcessHeap(), 0, json);
    return Zip_AddData(zip, APPEARANCE_NAME, out, used);
}

/* Whether the export of `choice` keeps the session `key`: every one without keys. */
static BOOL KeepsSession(const BackupChoice *choice, const WCHAR *key)
{
    int i;
    if (!choice || !choice->sessions) return TRUE;
    for (i = 0; i < choice->sessionCount; i++)
        if (_wcsicmp(choice->sessions[i], key) == 0) return TRUE;
    return FALSE;
}

/* The sessions the profile lists that `choice` keeps, as a session archive in the backup. */
static BOOL AddSessions(ZipOut *zip, const ProfileList *list, int index, const BackupChoice *choice, int *count, WCHAR *error, size_t errorCch)
{
    WCHAR temporary[MAX_PATH] = L"", dir[LONG_PATH_CCH], tasks[LONG_PATH_CCH];
    SessionSet set;
    int *rows, r, n = 0;
    BOOL ok;
    *count = 0;
    if (!SessionStore_LoadProfiles(&set, list)) {
        StringCchCopyW(error, errorCch, TR(L"Sessions could not be loaded."));
        return FALSE;
    }
    rows = (int *)Util_Alloc((size_t)max(set.rowCount, 1) * sizeof *rows);
    if (!rows) {
        SessionStore_Free(&set);
        return FALSE;
    }
    for (r = 0; r < set.rowCount; r++)
        if (set.rows[r].entry[index] >= 0 && !set.entries[set.rows[r].entry[index]].pendingRemove && KeepsSession(choice, set.rows[r].key)) rows[n++] = r;
    ok = TRUE;
    if (n) {
        WCHAR folder[MAX_PATH];
        ok = GetTempPathW(ARRAYSIZE(folder), folder) && GetTempFileNameW(folder, L"cdm", 0, temporary) &&
             SessionSync_Export(&set, index, rows, n, temporary, count, error, errorCch) && Zip_AddFile(zip, SESSIONS_ARCHIVE, temporary);
        if (temporary[0]) DeleteFileW(temporary);
    }
    if (ok && SessionStore_EntriesDir(&list->items[index], dir, ARRAYSIZE(dir), NULL) &&
        SUCCEEDED(StringCchPrintfW(tasks, ARRAYSIZE(tasks), L"%s\\" SCHEDULED_TASKS, dir)) && Util_FileExists(tasks))
        ok = Zip_AddFile(zip, "sessions/" "scheduled-tasks.json", tasks) &&
             VaultTasks_AddRegistry(zip, &list->items[index], tasks, "sessions/scheduled-tasks.json", TRUE, error, errorCch);
    Util_Free(rows);
    SessionStore_Free(&set);
    return ok;
}

/* The sessions the profiles of `profiles` list, together; -1 when they could not be read. For one, `*out` gets
 * each one's key, title and last activity, the latest first (a heap block, NULL for none). */
static int ListSessions(const ProfileList *list, DWORD profiles, ArchivedSession **out)
{
    SessionSet set;
    int r, i, n = 0;
    if (out) *out = NULL;
    if (!SessionStore_LoadProfiles(&set, list)) return -1;
    if (out && Core_BitCount(profiles) == 1 && (*out = (ArchivedSession *)Util_Alloc((size_t)max(set.rowCount, 1) * sizeof **out)) == NULL) {
        SessionStore_Free(&set);
        return -1;
    }
    for (i = 0; i < list->count; i++)
        for (r = 0; (profiles & (1u << i)) && r < set.rowCount; r++) {
            const SessionEntry *entry;
            if (set.rows[r].entry[i] < 0 || (entry = &set.entries[set.rows[r].entry[i]])->pendingRemove) continue;
            if (out && *out) {
                ArchivedSession *session = &(*out)[n];
                StringCchCopyW(session->key, ARRAYSIZE(session->key), set.rows[r].key);
                StringCchCopyW(session->own, ARRAYSIZE(session->own), entry->localId);
                StringCchCopyW(session->title, ARRAYSIZE(session->title), entry->title[0] ? entry->title : set.rows[r].key);
                session->lastActivity = entry->lastActivity;
            }
            n++;
        }
    SessionStore_Free(&set);
    if (out && *out) qsort(*out, (size_t)n, sizeof **out, LatestFirst);
    return n;
}

/* The profile's settings side (sidebar, details, appearance, language, model,
 * settings, permissions), member by member, as the sync reads it, filtered to
 * the items chosen: one JSON in the archive a restore applies part by part. */
static BOOL AddLayout(ZipOut *zip, const ProfileList *list, int index, DWORD items, const BackupChoice *choice)
{
    WCHAR dir[LONG_PATH_CCH];
    char *layout, *filtered;
    size_t length = 0, filteredLength = 0;
    BOOL ok = TRUE;
    if (!SessionStore_EntriesDir(&list->items[index], dir, ARRAYSIZE(dir), NULL)) return TRUE;   /* not signed in: no layout */
    if ((layout = SessionSync_ReadLayout(&list->items[index], dir, &length, NULL)) == NULL) return TRUE;
    /* The settings the person changed, theirs. */
    if ((filtered = SessionSync_FilterLayout(layout, length, items, &filteredLength)) != NULL)
        filtered = WithChanges(filtered, &filteredLength, choice->settings, choice->settingsLength, items);
    if (filtered && filteredLength > 2)
        ok = Zip_AddData(zip, LAYOUT_NAME, filtered, filteredLength);
    Util_Free(layout);
    Util_Free(filtered);
    return ok;
}

/* Just the MCP servers of config.json, so a restore of the app config leaves
 * the rest of config.json (which the layout keeps) alone. */
static BOOL AddMcp(ZipOut *zip, const WCHAR *storage)
{
    WCHAR path[LONG_PATH_CCH];
    char *json, *out;
    const char *value;
    size_t valueLength;
    DWORD length = 0;
    BOOL ok = TRUE;
    static const char head[] = "{\"mcpServers\":";
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, storage)) ||
        (json = Util_ReadFile(path, JSON_MAX, FALSE, &length)) == NULL)
        return TRUE;
    if (Core_JsonMember(json, length, "mcpServers", &value, &valueLength) && (out = (char *)Util_Alloc(sizeof head + valueLength + 1)) != NULL) {
        memcpy(out, head, sizeof head - 1);
        memcpy(out + sizeof head - 1, value, valueLength);
        out[sizeof head - 1 + valueLength] = '}';
        ok = Zip_AddData(zip, MCP_NAME, out, sizeof head - 1 + valueLength + 1);
        Util_Free(out);
    }
    Util_Free(json);
    return ok;
}

/* Profile `p`'s own as it is: every part, its picture when it has one. */
static void OwnOfProfile(const Profile *p, ProfileOwn *own)
{
    WCHAR picture[MAX_PATH];
    ZeroMemory(own, sizeof *own);
    StringCchCopyW(own->name, ARRAYSIZE(own->name), p->name);
    StringCchCopyW(own->note, ARRAYSIZE(own->note), p->note);
    StringCchCopyW(own->badge, ARRAYSIZE(own->badge), p->badge);
    StringCchCopyW(own->title, ARRAYSIZE(own->title), p->windowTitle);
    own->color = p->color;
    own->held = PROFILE_OWN_NAME | PROFILE_OWN_NOTE | PROFILE_OWN_COLOR | PROFILE_OWN_BADGE | PROFILE_OWN_TITLE;
    if (p->picture && Icons_PicturePath(p->folder, picture, ARRAYSIZE(picture)) && Util_FileExists(picture)) own->held |= PROFILE_OWN_PICTURE;
    own->parts = own->held;
}

/* Member `key` of the profile's own, `text` quoted, after those written up to `used`. */
static size_t AddOwnMember(char *json, size_t cch, size_t used, const char *key, const WCHAR *text, BOOL *ok)
{
    char quoted[256 * 6 + 4];
    if (!*ok || !(*ok = Core_JsonQuote(text, quoted, sizeof quoted))) return used;
    return AppendA(json, cch, used, "%s\"%s\":%s", used > 1 ? "," : "", key, quoted);
}

/* The parts of `own` taken, as profile/profile.json, and the profile's picture when it is one of them. */
static BOOL AddProfileOwn(ZipOut *zip, const Profile *p, const ProfileOwn *own)
{
    char json[4096];
    WCHAR picture[MAX_PATH];
    DWORD parts = own->parts & own->held;
    size_t used = AppendA(json, sizeof json, 0, "{");
    BOOL ok = TRUE;
    if (parts & PROFILE_OWN_NAME) used = AddOwnMember(json, sizeof json, used, "name", own->name, &ok);
    if (parts & PROFILE_OWN_NOTE) used = AddOwnMember(json, sizeof json, used, "note", own->note, &ok);
    if (parts & PROFILE_OWN_COLOR) used = AppendA(json, sizeof json, used, "%s\"color\":%d", used > 1 ? "," : "", own->color);
    if (parts & PROFILE_OWN_BADGE) used = AddOwnMember(json, sizeof json, used, "badge", own->badge, &ok);
    if (parts & PROFILE_OWN_TITLE) used = AddOwnMember(json, sizeof json, used, "windowTitle", own->title, &ok);
    used = AppendA(json, sizeof json, used, "}");
    ok = ok && used < sizeof json - 1 && Zip_AddData(zip, PROFILE_NAME, json, used);
    if (ok && (parts & PROFILE_OWN_PICTURE) && p->picture && Icons_PicturePath(p->folder, picture, ARRAYSIZE(picture)) && Util_FileExists(picture))
        ok = Zip_AddFile(zip, PICTURE_NAME, picture);
    return ok;
}

/* The profile's own that backup `zip` holds, every part of it taken. */
static BOOL ReadProfileOwn(const ZipIn *zip, ProfileOwn *own)
{
    DWORD length = 0;
    ULONGLONG color = 0;
    const char *value;
    size_t valueLength;
    int found = Zip_Find(zip, PROFILE_NAME);
    char *json = found >= 0 ? (char *)Zip_Read(zip, found, MANIFEST_MAX, &length) : NULL;
    ZeroMemory(own, sizeof *own);
    own->color = -1;
    if (!json) return FALSE;
    if (Core_JsonMemberString(json, length, "name", own->name, ARRAYSIZE(own->name)) && own->name[0]) own->held |= PROFILE_OWN_NAME;
    if (Core_JsonMemberString(json, length, "note", own->note, ARRAYSIZE(own->note))) own->held |= PROFILE_OWN_NOTE;
    if (Core_JsonMemberString(json, length, "badge", own->badge, ARRAYSIZE(own->badge))) own->held |= PROFILE_OWN_BADGE;
    if (Core_JsonMemberString(json, length, "windowTitle", own->title, ARRAYSIZE(own->title))) own->held |= PROFILE_OWN_TITLE;
    if (Core_JsonMember(json, length, "color", &value, &valueLength) && Core_JsonNumber(value, valueLength, &color) && color < PALETTE_SIZE) {
        own->color = (int)color;
        own->held |= PROFILE_OWN_COLOR;
    }
    Util_Free(json);
    if (Zip_Find(zip, PICTURE_NAME) >= 0) own->held |= PROFILE_OWN_PICTURE;
    own->parts = own->held;
    return TRUE;
}

/* The parts of `own` taken given to `p`, its picture from backup `zip`; the
 * others stay the profile's. */
static BOOL RestoreProfileOwn(const ZipIn *zip, const Profile *p, const ProfileOwn *own)
{
    WCHAR temporary[MAX_PATH], dir[MAX_PATH];
    DWORD parts = own->parts & own->held, stamp = p->picture;
    int found, color = (parts & PROFILE_OWN_COLOR) && own->color >= 0 && own->color < PALETTE_SIZE ? own->color : p->color;
    const WCHAR *name = (parts & PROFILE_OWN_NAME) && own->name[0] ? own->name : p->name, *badge = (parts & PROFILE_OWN_BADGE) ? own->badge : p->badge;
    BOOL ok;
    /* Its picture through a file of the state folder: pictures are read from files (Icons_ReadPicture). */
    if ((parts & PROFILE_OWN_PICTURE) && (found = Zip_Find(zip, PICTURE_NAME)) >= 0 && Util_StateDir(dir, ARRAYSIZE(dir)) &&
        SUCCEEDED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s\\restored-picture-%lu.png", dir, GetCurrentProcessId()))) {
        DWORD *pixels = NULL, saved = 0;
        HRESULT read = S_OK;
        if (Zip_ExtractTo(zip, found, temporary, UTIL_PUT_OVER) && (pixels = Icons_ReadPicture(temporary, &read)) != NULL &&
            Icons_SavePicture(p->folder, pixels, &saved))
            stamp = saved;
        Util_Free(pixels);
        DeleteFileW(temporary);
    }
    ok = Profiles_Update(p->folder, name, color, badge, stamp) && (!(parts & PROFILE_OWN_NOTE) || Profiles_SetNote(p->folder, own->note)) &&
         (!(parts & PROFILE_OWN_TITLE) || Profiles_SetWindowTitle(p->folder, own->title));
    Util_Log(L"profile %s takes its own of a backup (parts 0x%02lX)%s", p->folder, parts, ok ? L"" : L": FAILED");
    return ok;
}

/* Profile `index`'s parts `chosen` written to the archive `path`; FALSE with what went wrong in `error`. */

static BOOL WriteBackup(const ProfileList *list, int index, const BackupChoice *choice, const WCHAR *path, WCHAR *error, size_t errorCch)
{
    DWORD items = choice->items;
    const ProfileOwn *own = &choice->own;
    const Profile *p = &list->items[index];
    WCHAR cowork[LONG_PATH_CCH];
    char manifest[1024], quoted[3 * LABEL_CCH + 8];
    ZipOut *zip;
    DWORD code = 0;
    BOOL chosen[PARTS];
    BYTE key[64];
    size_t keyLength = 0;
    int sessions = 0;
    size_t used;
    BOOL ok;
    error[0] = 0;
    items &= ~BK_KEY;
    /* The sign-in's key with it, when this Windows user opens it: then it restores on any PC. */
    if ((items & BK_SIGNIN) && Backup_ReadAppKey(p->storageDir, key, sizeof key, &keyLength)) items |= BK_KEY;
    else if (items & BK_SIGNIN) Util_Log(L"backup of %s: the sign-in's key could not be read, the sign-in restores on this PC only", p->folder);
    BitsToParts(items, chosen);
    if ((zip = Zip_Create(path, &code)) == NULL) {
        StringCchPrintfW(error, errorCch, TR(L"The export could not be written (error %lu)."), code);
        return FALSE;
    }
    StringCchPrintfW(cowork, ARRAYSIZE(cowork), L"%s\\" COWORK_DIR, p->storageDir);
    if (!Core_JsonQuote(p->name, quoted, sizeof quoted)) StringCchCopyA(quoted, ARRAYSIZE(quoted), "\"\"");
    used = AppendA(manifest, sizeof manifest, 0, "{\"format\":\"" BACKUP_FORMAT "\",\"version\":%d,\"profile\":%s,\"items\":%lu}",
                   BACKUP_VERSION, quoted, (unsigned long)items);
    ok = Zip_AddData(zip, MANIFEST_NAME, manifest, used);
    if (ok && chosen[PART_SESSIONS]) ok = AddSessions(zip, list, index, choice, &sessions, error, errorCch);
    if (ok && chosen[PART_COWORK]) ok = AddTreeWithTasks(zip, cowork, L"cowork", p);
    if (ok && (items & BK_LAYOUT)) ok = AddLayout(zip, list, index, items & BK_LAYOUT, choice);
    if (ok && chosen[PART_SETTINGS]) {
        size_t i;
        ok = AddMcp(zip, p->storageDir);
        for (i = 0; ok && i < ARRAYSIZE(kSettingsFiles); i++) ok = AddIfThere(zip, p->storageDir, kSettingsFiles[i], L"settings");
    }
    if (ok && (items & SYNC_ITEM_APPEARANCE)) ok = AddAppearance(zip, p->storageDir);
    if (ok && (items & BK_PROFILE)) {
        ProfileOwn mine;
        if (!own || !own->held) OwnOfProfile(p, &mine);
        ok = AddProfileOwn(zip, p, own && own->held ? own : &mine);
    }
    if (ok && chosen[PART_SIGNIN]) {
        size_t i;
        WCHAR dir[LONG_PATH_CCH], prefix[MAX_PATH];
        for (i = 0; ok && i < ARRAYSIZE(kSigninFiles); i++) ok = AddIfThere(zip, p->storageDir, kSigninFiles[i], L"signin");
        for (i = 0; ok && i < ARRAYSIZE(kSigninFolders); i++) {
            if (FAILED(StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\%s", p->storageDir, kSigninFolders[i])) || !Util_DirExists(dir)) continue;
            StringCchPrintfW(prefix, ARRAYSIZE(prefix), L"signin/%s", kSigninFolders[i]);
            ok = AddTree(zip, dir, prefix);
        }
    }
    if (ok && chosen[PART_KEY]) {
        char text[256];
        size_t length;
        StringCchCopyA(text, sizeof text, "{\"key\":\"");
        length = strlen(text);
        if (Core_Base64Encode(key, keyLength, text + length, sizeof text - length - 3)) {
            StringCchCatA(text, sizeof text, "\"}");
            ok = Zip_AddData(zip, KEY_NAME, text, strlen(text));
        }
        SecureZeroMemory(text, sizeof text);
    }
    SecureZeroMemory(key, sizeof key);
    code = Zip_Error(zip);
    ok = Zip_Close(zip, ok) && ok;
    if (!ok && !error[0]) StringCchPrintfW(error, errorCch, TR(L"The export could not be written (error %lu)."), code ? code : GetLastError());
    if (ok) {
        Util_Log(L"backed up %s to %s", p->folder, path);
    }
    return ok;
}

/* `folder`\`name`, or "<name> (2).zip" and on while that is taken. */
static BOOL FreeArchivePath(const WCHAR *folder, const WCHAR *name, WCHAR *path, size_t cch)
{
    WCHAR stem[MAX_PATH];
    int n;
    if (FAILED(StringCchPrintfW(path, cch, L"%s\\%s", folder, name))) return FALSE;
    if (!Util_FileExists(path)) return TRUE;
    StringCchCopyW(stem, ARRAYSIZE(stem), name);
    if (Core_EndsWithI(stem, L"." ARCHIVE_EXTENSION)) stem[wcslen(stem) - ARRAYSIZE(ARCHIVE_EXTENSION)] = 0;
    for (n = 2; n < 100; n++)
        if (SUCCEEDED(StringCchPrintfW(path, cch, L"%s\\%s (%d)." ARCHIVE_EXTENSION, folder, stem, n)) && !Util_FileExists(path)) return TRUE;
    return FALSE;
}

static void DescribeTaskCoverage(const WCHAR *archive, WCHAR *text, size_t cch)
{
    ZipIn *zip = Zip_Open(archive); int i;
    if (!zip) return;
    for (i = 0; i < Zip_Count(zip); i++) if (VaultTasks_IsCoverage(Zip_Name(zip, i))) {
        WCHAR line[2048];
        if (VaultTasks_Describe(zip, Zip_Name(zip, i), line, ARRAYSIZE(line))) { StringCchCatW(text, cch, L"\n"); StringCchCatW(text, cch, line); }
    }
    Zip_Free(zip);
}

void Backup_Reopen(const ProfileList *list, DWORD profiles)
{
    ClaudePackage pkg;
    DWORD pid;
    BOOL identity;
    HRESULT hr;
    int i;
    if (!profiles || !Claude_FindPackage(&pkg)) return;
    for (i = 0; i < list->count; i++) {
        if (!(profiles & (1u << i))) continue;
        pid = 0;
        identity = FALSE;
        hr = Claude_Launch(&pkg, &list->items[i], NULL, &pid, &identity);
        if (FAILED(hr)) Util_Log(L"export of %s: could not open it again (0x%08lX)", list->items[i].folder, (unsigned long)hr);
        else Util_Log(L"export of %s: opened again (pid %lu)", list->items[i].folder, pid);
    }
}

BOOL Backup_Write(const ProfileList *list, int index, const BackupChoice *choice, const WCHAR *path, WCHAR *error, size_t errorCch)
{
    return WriteBackup(list, index, choice, path, error, errorCch);
}

void Backup_FreeChoice(BackupChoice *choice)
{
    Util_Free(choice->sessions);
    Util_Free(choice->settings);
    choice->sessions = NULL;
    choice->sessionCount = 0;
    choice->settings = NULL;
    choice->settingsLength = 0;
}

/* Profile `p`'s settings as a layout, to show them one by one: a heap block, NULL for none. */
static char *ProfileLayout(const Profile *p, size_t *length)
{
    WCHAR dir[LONG_PATH_CCH];
    *length = 0;
    return SessionStore_EntriesDir(p, dir, ARRAYSIZE(dir), NULL) ? SessionSync_ReadLayout(p, dir, length, NULL) : NULL;
}

/* The profiles of `profiles` that have a folder to back up; `*first` the first of them. */
static DWORD BackedUpProfiles(const ProfileList *list, DWORD profiles, const Profile **first, int *count)
{
    int i;
    *first = NULL;
    *count = 0;
    for (i = 0; i < list->count; i++) {
        if (!(profiles & (1u << i))) continue;
        if (!list->items[i].storageDir[0]) {   /* no folder of its own to back up */
            profiles &= ~(1u << i);
            continue;
        }
        if (!(*count)++) *first = &list->items[i];
    }
    return profiles;
}

static void BackupTitle(const Profile *first, int count, WCHAR *title, size_t cch)
{
    if (count == 1) StringCchPrintfW(title, cch, TR(L"Export \x201C%s\x201D"), first->name);
    else StringCchPrintfW(title, cch, TR(L"Export %d profiles"), count);
}

BOOL Backup_ChooseItems(HWND owner, const ProfileList *list, DWORD profiles, BackupChoice *choice)
{
    WCHAR title[LABEL_CCH + 64], cowork[LONG_PATH_CCH];
    const Profile *first;
    ArchivedSession *sessions = NULL;
    BackupDialog dialog;
    HCURSOR old;
    char *layout = NULL;
    int i, count;
    BOOL ok;
    ZeroMemory(choice, sizeof *choice);
    profiles = BackedUpProfiles(list, profiles, &first, &count);
    if (!count) return FALSE;
    BackupTitle(first, count, title, ARRAYSIZE(title));
    ZeroMemory(&dialog, sizeof dialog);
    dialog.title = title;
    dialog.text = TR(L"Choose what the export holds:");
    dialog.action = TR(L"Export");
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    dialog.sessions = ListSessions(list, profiles, count == 1 ? &sessions : NULL);
    if (count == 1) dialog.layout = layout = ProfileLayout(first, &dialog.layoutLength);
    SetCursor(old);
    dialog.sessionList = sessions;
    dialog.sessionCount = sessions ? max(dialog.sessions, 0) : 0;
    dialog.offered = BK_APPCONFIG | BK_LAYOUT | BK_SIGNIN | BK_PROFILE;
    dialog.chosen = BK_SIGNIN | BK_PROFILE;   /* checked at first: the account and the profile's own */
    for (i = 0; i < list->count; i++)
        if ((profiles & (1u << i)) && SUCCEEDED(StringCchPrintfW(cowork, ARRAYSIZE(cowork), L"%s\\" COWORK_DIR, list->items[i].storageDir)) &&
            Util_DirExists(cowork))
            dialog.offered |= BK_COWORK;
    if (dialog.sessions != 0) dialog.offered |= BK_CONVERSATIONS;
    if (count == 1) {
        OwnOfProfile(first, &choice->own);
        dialog.own = &choice->own;
        dialog.signinStorage = first->storageDir;   /* what its sign-in holds, in the list */
        dialog.signinRunning = Claude_IsRunning(first);
    }
    dialog.exportList = list;
    dialog.exportProfiles = profiles;
    ok = Ui_Dialog(owner, IDD_BACKUP, BackupProc, (LPARAM)&dialog) == IDOK && TakeSessions(&dialog, &choice->sessions, &choice->sessionCount);
    choice->reopen = dialog.reopen;
    if (!ok && dialog.reopen) Backup_Reopen(list, dialog.reopen);   /* closed for it, then not exported after all */
    choice->items = dialog.chosen;
    if (ok) choice->settings = TakeLayoutChanges(&dialog, &choice->settingsLength);
    FreeDialog(&dialog);
    Util_Free(sessions);
    Util_Free(layout);
    if (!ok) Backup_FreeChoice(choice);
    return ok;
}

BOOL Backup_Create(HWND owner, const ClaudePackage *pkg, const ProfileList *list, DWORD profiles)
{
    WCHAR title[LABEL_CCH + 64], path[LONG_PATH_CCH], folder[LONG_PATH_CCH], name[MAX_PATH];
    WCHAR error[LONG_PATH_CCH + 256], line[LABEL_CCH + LONG_PATH_CCH + 300], report[8 * (LONG_PATH_CCH + 300)], text[9 * (LONG_PATH_CCH + 300)];
    const Profile *first;
    BackupChoice choice;
    HCURSOR old;
    int i, count, saved = 0;
    BOOL ok = FALSE;
    (void)pkg;
    profiles = BackedUpProfiles(list, profiles, &first, &count);
    if (!count || !Backup_ChooseItems(owner, list, profiles, &choice)) return FALSE;
    BackupTitle(first, count, title, ARRAYSIZE(title));
    if (count == 1) {
        if (!ChooseArchive(owner, ARCHIVE_SAVE, title, first->name, path, ARRAYSIZE(path))) goto done;
        old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        i = (int)(first - list->items);
        ok = WriteBackup(list, i, &choice, path, error, ARRAYSIZE(error));
        SetCursor(old);
        Backup_Reopen(list, choice.reopen);   /* written: what closed for it opens again */
        choice.reopen = 0;
        if (!ok) {
            Ui_Message(owner, MB_ICONERROR, L"%s", error);
            goto done;
        }
        StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Exported:\n%s"), path);
        DescribeTaskCoverage(path, report, ARRAYSIZE(report));
        Ui_Message(owner, MB_ICONINFORMATION, L"%s", report);
        goto done;
    }
    /* Several: one archive each, named after its profile, in the folder chosen. */
    if (!ChooseArchive(owner, ARCHIVE_FOLDER, TR(L"Choose the folder to export to"), NULL, folder, ARRAYSIZE(folder))) goto done;
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    report[0] = 0;
    for (i = 0; i < list->count; i++) {
        if (!(profiles & (1u << i))) continue;
        Backup_ArchiveName(list->items[i].name, name, ARRAYSIZE(name));
        if (!FreeArchivePath(folder, name, path, ARRAYSIZE(path)))
            StringCchPrintfW(error, ARRAYSIZE(error), TR(L"The export could not be written (error %lu)."), (unsigned long)ERROR_FILE_EXISTS);
        else if (WriteBackup(list, i, &choice, path, error, ARRAYSIZE(error))) {
            saved++;
            DescribeTaskCoverage(path, report, ARRAYSIZE(report));
            continue;
        }
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"\x201C%s\x201D: %s"), list->items[i].name, error);
        if (report[0]) StringCchCatW(report, ARRAYSIZE(report), L"\n");
        StringCchCatW(report, ARRAYSIZE(report), line);
    }
    SetCursor(old);
    Backup_Reopen(list, choice.reopen);   /* written: what closed for them opens again */
    choice.reopen = 0;
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%d profiles exported to:\n%s"), saved, folder);
    if (report[0]) {
        StringCchCatW(text, ARRAYSIZE(text), L"\n\n");
        StringCchCatW(text, ARRAYSIZE(text), report);
    }
    Ui_Message(owner, saved == count ? MB_ICONINFORMATION : MB_ICONWARNING, L"%s", text);
    ok = saved == count;
done:
    Backup_Reopen(list, choice.reopen);   /* closed for their sign-in: open again */
    Backup_FreeChoice(&choice);
    return ok;
}

/* ------------------------------------------------------------ restoring */

/* A path inside the archive that stays inside the folder it is restored to
 * (Core_ArchiveNameSafe: a ".." part never, "draft..v2.md" as any name). */
static BOOL SafeRelative(const char *name, const char *prefix, WCHAR *out, size_t cch)
{
    size_t length = strlen(prefix), i;
    const char *rest;
    if (strncmp(name, prefix, length) != 0 || name[length] != '/') return FALSE;
    rest = name + length + 1;
    if (!Core_ArchiveNameSafe(rest, strlen(rest)) || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rest, -1, out, (int)cch) <= 0)
        return FALSE;
    for (i = 0; out[i]; i++)
        if (out[i] == L'/') out[i] = L'\\';
    return TRUE;
}

static BOOL RestoreTasks(const ZipIn *zip, int item, const WCHAR *path, const WCHAR *backup, const Profile *profile,
                         const char *registryMember, BOOL code);

/* Every item under `prefix` extracted into `dir`, folders made. */
static BOOL ExtractTreeWithTasks(const ZipIn *zip, const char *prefix, const WCHAR *dir, DWORD *code, const Profile *profile, const WCHAR *backup)
{
    WCHAR relative[LONG_PATH_CCH], path[LONG_PATH_CCH], parent[LONG_PATH_CCH];
    int i;
    BOOL ok = TRUE;
    for (i = 0; i < Zip_Count(zip); i++) {
        WCHAR *slash;
        if (!SafeRelative(Zip_Name(zip, i), prefix, relative, ARRAYSIZE(relative))) continue;
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, relative)) || FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), path))) {
            ok = FALSE;
            continue;
        }
        if ((slash = wcsrchr(parent, L'\\')) != NULL) *slash = 0;
        if (!Util_EnsureDir(parent) ||
            !(profile && Core_EndsWithI(relative, L"\\" SCHEDULED_TASKS)
                ? RestoreTasks(zip, i, path, backup, profile, Zip_Name(zip, i), FALSE) : Zip_Extract(zip, i, path))) {
            *code = GetLastError();
            ok = FALSE;
        }
    }
    return ok;
}

static BOOL ExtractTree(const ZipIn *zip, const char *prefix, const WCHAR *dir, DWORD *code)
{
    return ExtractTreeWithTasks(zip, prefix, dir, code, NULL, NULL);
}

/* A file or folder of the profile, copied into the backup of a restore. */
static BOOL KeepCopy(const WCHAR *storage, const WCHAR *item, const WCHAR *backup, BOOL folder, DWORD *code)
{
    WCHAR from[LONG_PATH_CCH], to[LONG_PATH_CCH], extendedFrom[LONG_PATH_CCH], extendedTo[LONG_PATH_CCH];
    if (FAILED(StringCchPrintfW(from, ARRAYSIZE(from), L"%s\\%s", storage, item)) || FAILED(StringCchPrintfW(to, ARRAYSIZE(to), L"%s\\%s", backup, item)) ||
        !Util_ExtendedPath(from, extendedFrom, ARRAYSIZE(extendedFrom)) || !Util_ExtendedPath(to, extendedTo, ARRAYSIZE(extendedTo))) {
        *code = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    if (folder ? !Util_DirExists(from) : !Util_FileExists(from)) return TRUE;
    if (folder) return Util_CopyTree(from, to, TRUE, code);
    if (Util_EnsureDir(backup) && CopyFileW(extendedFrom, extendedTo, FALSE)) return TRUE;
    *code = GetLastError();
    return FALSE;
}

#define SET_ASIDE_SUFFIX L".cdm-old"

/* The sign-in folder `item` of the profile and its name while set aside, in their \\?\ form. */
static BOOL SignInPaths(const WCHAR *storage, const WCHAR *item, WCHAR *folder, WCHAR *aside, size_t cch)
{
    WCHAR plain[LONG_PATH_CCH];
    return SUCCEEDED(StringCchPrintfW(plain, ARRAYSIZE(plain), L"%s\\%s", storage, item)) && Util_ExtendedPath(plain, folder, cch) &&
           SUCCEEDED(StringCchPrintfW(aside, cch, L"%s" SET_ASIDE_SUFFIX, folder));
}

/* The sign-in folders put aside (renamed) before the backup's take their
 * place: a restore cut short puts them back as they were (PutBackSignIn)
 * rather than leaving the profile half signed in. Their copy is in the
 * restore's own backup too. */
static BOOL SetAsideSignIn(const WCHAR *storage, DWORD *code)
{
    WCHAR folder[LONG_PATH_CCH], aside[LONG_PATH_CCH];
    DWORD ignored;
    size_t i;
    for (i = 0; i < ARRAYSIZE(kSigninFolders); i++) {
        if (!SignInPaths(storage, kSigninFolders[i], folder, aside, ARRAYSIZE(folder))) {
            *code = ERROR_FILENAME_EXCED_RANGE;
            break;
        }
        if (Util_DirExists(aside)) Util_DeleteTree(aside, &ignored);   /* left by a restore cut short before */
        if (Util_DirExists(folder) && !MoveFileExW(folder, aside, 0)) {
            *code = GetLastError();
            break;
        }
    }
    if (i == ARRAYSIZE(kSigninFolders)) return TRUE;
    while (i-- > 0)
        if (SignInPaths(storage, kSigninFolders[i], folder, aside, ARRAYSIZE(folder)) && Util_DirExists(aside)) MoveFileExW(aside, folder, 0);
    return FALSE;
}

/* After the restore: what was set aside goes, or, when it failed, what the
 * restore wrote goes and what was set aside comes back. */
static void PutBackSignIn(const WCHAR *storage, BOOL restored)
{
    WCHAR folder[LONG_PATH_CCH], aside[LONG_PATH_CCH];
    DWORD code;
    size_t i;
    for (i = 0; i < ARRAYSIZE(kSigninFolders); i++) {
        if (!SignInPaths(storage, kSigninFolders[i], folder, aside, ARRAYSIZE(folder)) || !Util_DirExists(aside)) continue;
        if (restored) {
            if (!Util_DeleteTree(aside, &code)) Util_Log(L"restore: %s could not be removed (error %lu)", aside, code);
            continue;
        }
        if ((Util_DirExists(folder) && !Util_DeleteTree(folder, &code)) || !MoveFileExW(aside, folder, 0))
            Util_Log(L"restore: %s could not be put back (error %lu); its copy is in the restore's backup", folder, GetLastError());
    }
}

static BOOL RestoreBackupDir(const Profile *p, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    SYSTEMTIME now;
    GetLocalTime(&now);
    return Util_StateDir(state, ARRAYSIZE(state)) &&
           SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" BACKUPS_DIR L"\\%04u%02u%02u-%02u%02u%02u\\%s-restore", state, now.wYear, now.wMonth,
                                      now.wDay, now.wHour, now.wMinute, now.wSecond, p->folder)) &&
           Util_EnsureDir(out);
}

/* config.json given the language and theme of the backup's appearance. */
static BOOL RestoreAppearance(const ZipIn *zip, const WCHAR *storage)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0, appearanceLength = 0;
    int item = Zip_Find(zip, APPEARANCE_NAME);
    char *json, *appearance, *out = NULL;
    size_t i, outLength = 0;
    BOOL ok = TRUE;
    if (item < 0 || (appearance = (char *)Zip_Read(zip, item, MANIFEST_MAX, &appearanceLength)) == NULL) return TRUE;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_APP_SETTINGS, storage))) {
        Util_Free(appearance);
        return FALSE;
    }
    json = Util_ReadFile(path, JSON_MAX, FALSE, &length);
    if (!json) {
        /* No config.json yet: the appearance becomes it. */
        ok = Util_WriteFileAtomic(path, appearance, appearanceLength, UTIL_PUT_NEW);
        Util_Free(appearance);
        return ok;
    }
    for (i = 0; ok && i < ARRAYSIZE(kAppearanceKeys); i++) {
        const char *value;
        size_t valueLength, cap;
        char raw[512], *next;
        if (!Core_JsonMember(appearance, appearanceLength, kAppearanceKeys[i], &value, &valueLength) || valueLength >= sizeof raw) continue;
        memcpy(raw, value, valueLength);
        raw[valueLength] = 0;
        cap = (out ? outLength : length) + valueLength + 64;
        if ((next = (char *)Util_Alloc(cap)) == NULL) {
            ok = FALSE;
            break;
        }
        ok = Core_JsonSetMember(out ? out : json, out ? outLength : length, kAppearanceKeys[i], raw, next, cap, &outLength);
        Util_Free(out);
        out = next;
    }
    if (ok && out) ok = Util_WriteFileAtomic(path, out, outLength, UTIL_PUT_REPLACE);
    Util_Free(out);
    Util_Free(json);
    Util_Free(appearance);
    return ok;
}

/* The backup's scheduled tasks joined to the profile's (`path`): the
 * backup's own as it has them, the profile's others kept, the profile's
 * file kept in `backup` before the restore's first change to it. */
static BOOL RestoreTasks(const ZipIn *zip, int item, const WCHAR *path, const WCHAR *backup, const Profile *profile,
                         const char *registryMember, BOOL code)
{
    WCHAR kept[LONG_PATH_CCH];
    DWORD length = 0, currentLength = 0, taskError = 0;
    char *restored = (char *)Zip_Read(zip, item, JSON_MAX, &length), *current, *joined = NULL, *mapped;
    size_t joinedLength = 0, mappedLength = 0;
    const char *out;
    BOOL ok, aware = FALSE;
    if (!restored) return FALSE;
    current = Util_ReadFile(path, JSON_MAX, FALSE, &currentLength);
    if (current) {
        /* A source registry has its own safe backup name, so several Cowork
         * accounts and Code cannot silently replace each other's safety copy. */
        ULONGLONG hash = Core_HashBytes(CORE_HASH_START, registryMember, strlen(registryMember));
        if (FAILED(StringCchPrintfW(kept, ARRAYSIZE(kept), L"%s\\tasks-%016I64x.json", backup, hash)) ||
            (!Util_FileExists(kept) && !CopyFileW(path, kept, TRUE))) {
            Util_Free(current); Util_Free(restored); return FALSE;
        }
    }
    mapped = VaultTasks_RestoreRegistry(zip, profile, registryMember, restored, length, code, &aware, &mappedLength, &taskError);
    if (aware && !mapped) { Util_Free(current); Util_Free(restored); SetLastError(taskError); return FALSE; }
    if (mapped) { Util_Free(restored); restored = mapped; length = (DWORD)mappedLength; }
    out = restored; joinedLength = length;
    if (current) {
        joined = Core_JsonArrayUnion(restored, length, current, currentLength, "scheduledTasks", "id", &joinedLength);
        if (joined) out = joined; else joinedLength = length;
    }
    if (aware && SessionLink_Busy(profile)) { Util_Free(joined); Util_Free(restored); Util_Free(current); SetLastError(ERROR_BUSY); return FALSE; }
    ok = Util_WriteFileAtomic(path, out, joinedLength, UTIL_PUT_REPLACE);
    Util_Free(joined); Util_Free(restored); Util_Free(current);
    return ok;
}

/* The backup's Code sessions `pick` takes (NULL: all) sent to the profile
 * through their archive's import, its scheduled tasks joined to the
 * profile's. */
static BOOL RestoreSessions(const ZipIn *zip, const ProfileList *list, int index, const SessionPick *pick, const WCHAR *backup, int *sessions,
                            BOOL *signedOut, WCHAR *error, size_t errorCch)
{
    WCHAR folder[MAX_PATH], temporary[MAX_PATH] = L"", dir[LONG_PATH_CCH], tasks[LONG_PATH_CCH];
    SessionSet set;
    SyncReport report;
    int item = Zip_Find(zip, SESSIONS_ARCHIVE), taskItem = Zip_Find(zip, "sessions/" "scheduled-tasks.json");
    BOOL ok = TRUE, signedIn = FALSE, located;
    *sessions = 0;
    located = SessionStore_EntriesDir(&list->items[index], dir, ARRAYSIZE(dir), &signedIn);
    *signedOut = !located && !signedIn;
    if (!located) {
        /* An account restored in this operation may not have initialized
         * its entries folder yet. Never report its sessions as restored. */
        StringCchCopyW(error, errorCch, TR(L"Open this profile in Claude once, then close it and restore its sessions and settings again."));
        return FALSE;
    }
    if (item >= 0) {
        ok = GetTempPathW(ARRAYSIZE(folder), folder) && GetTempFileNameW(folder, L"cdm", 0, temporary) && Zip_Extract(zip, item, temporary);
        if (ok && SessionStore_LoadProfiles(&set, list)) {
            ZeroMemory(&report, sizeof report);
            ok = SessionSync_ImportPicked(&set, temporary, 1u << index, pick, sessions, &report) && report.failed == 0;
            if (!ok) StringCchCopyW(error, errorCch, report.error);
            SessionStore_Free(&set);
        } else if (ok) {
            StringCchCopyW(error, errorCch, TR(L"Sessions could not be loaded."));
            ok = FALSE;
        }
        DeleteFileW(temporary);
    }
    if (ok && taskItem >= 0 && SUCCEEDED(StringCchPrintfW(tasks, ARRAYSIZE(tasks), L"%s\\" SCHEDULED_TASKS, dir)))
        ok = RestoreTasks(zip, taskItem, tasks, backup, &list->items[index], "sessions/scheduled-tasks.json", TRUE);
    return ok;
}

/* The backup's MCP servers set into config.json, the rest of it left as it
 * is (the layout keeps those members). */
static BOOL RestoreMcp(const ZipIn *zip, const WCHAR *storage)
{
    WCHAR path[LONG_PATH_CCH];
    int item = Zip_Find(zip, MCP_NAME);
    char *mcp, *json, *raw, *out;
    const char *value;
    size_t valueLength, outLength = 0, cap;
    DWORD mcpLength = 0, length = 0;
    BOOL ok = TRUE, freeJson = FALSE;
    if (item < 0 || (mcp = (char *)Zip_Read(zip, item, JSON_MAX, &mcpLength)) == NULL) return TRUE;
    if (!Core_JsonMember(mcp, mcpLength, "mcpServers", &value, &valueLength) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, storage))) {
        Util_Free(mcp);
        return FALSE;
    }
    if ((json = Util_ReadFile(path, JSON_MAX, FALSE, &length)) != NULL) freeJson = TRUE;
    else { json = (char *)"{}"; length = 2; }
    cap = (size_t)length + valueLength + 32;
    raw = (char *)Util_Alloc(valueLength + 1);
    out = (char *)Util_Alloc(cap);
    if (raw && out) {
        memcpy(raw, value, valueLength);
        raw[valueLength] = 0;
        ok = Core_JsonSetMember(json, length, "mcpServers", raw, out, cap, &outLength) &&
             Util_WriteFileAtomic(path, out, outLength, UTIL_PUT_REPLACE);
    } else {
        ok = FALSE;
    }
    Util_Free(raw);
    Util_Free(out);
    if (freeJson) Util_Free(json);
    Util_Free(mcp);
    return ok;
}

/* The restored sign-in's key, opened on this PC: as it is when DPAPI opens it
 * (the PC and Windows user it was backed up on), else from the backup's key
 * behind a password, protected again for this Windows user. A line for the
 * report, NULL when there is nothing to say. */
static const WCHAR *OpenSignInKey(HWND owner, const ZipIn *zip, const Profile *p, const WCHAR *title, BOOL hasKey)
{
    PasswordDialog password;
    BYTE key[64];
    size_t keyLength = 0;
    DWORD length = 0;
    char *sealed;
    int item;
    const WCHAR *note = NULL;
    if (Backup_ReadAppKey(p->storageDir, key, sizeof key, &keyLength)) {
        SecureZeroMemory(key, sizeof key);
        return NULL;   /* this PC and Windows user open it */
    }
    if (!hasKey || (item = Zip_Find(zip, KEY_NAME)) < 0 || (sealed = (char *)Zip_Read(zip, item, MANIFEST_MAX, &length)) == NULL)
        return TR(L"The sign-in was backed up on another PC or for another Windows user: sign in again in this profile.");
    {
        /* The key in the clear (this version's backups): kept for this Windows user at once. */
        const char *value;
        size_t valueLength;
        if (Core_JsonMember(sealed, length, "key", &value, &valueLength) && valueLength > 2 && value[0] == '"' &&
            (keyLength = Core_Base64Decode(value + 1, valueLength - 2, key, sizeof key)) != (size_t)-1 && keyLength) {
            if (!Backup_WriteAppKey(p->storageDir, key, keyLength))
                note = TR(L"The sign-in's key could not be kept for this Windows user: sign in again in this profile.");
            SecureZeroMemory(key, sizeof key);
            SecureZeroMemory(sealed, length);
            Util_Free(sealed);
            return note;
        }
    }
    ZeroMemory(&password, sizeof password);
    password.title = title;
    password.text = TR(L"The sign-in was backed up on another PC. Enter the password chosen for it then:");
    for (;;) {
        HCURSOR old;
        BOOL opened;
        if (Ui_Dialog(owner, IDD_PASSWORD, PasswordProc, (LPARAM)&password) != IDOK) {
            note = TR(L"The sign-in's key stays closed: sign in again in this profile.");
            break;
        }
        old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        opened = Core_OpenSecret(sealed, length, password.password, key, sizeof key, &keyLength);
        SetCursor(old);
        ForgetPassword(&password, TRUE);
        if (opened) {
            if (!Backup_WriteAppKey(p->storageDir, key, keyLength)) note = TR(L"The sign-in's key could not be kept for this Windows user: sign in again in this profile.");
            break;
        }
        Ui_Message(owner, MB_ICONWARNING, TR(L"This password does not open the sign-in's key."));
    }
    SecureZeroMemory(key, sizeof key);
    SecureZeroMemory(sealed, length);
    Util_Free(sealed);
    return note;
}

/* -------------------------------------------- several backups, one profile */

/* The oldest first: restored in that order, the newest's copy of what several hold is the one kept. */
static void SortSources(RestoreSource *sources, int count)
{
    int s, t;
    for (s = 1; s < count; s++)
        for (t = s; t > 0 && sources[t - 1].time > sources[t].time; t--) {
            RestoreSource swap = sources[t];
            sources[t] = sources[t - 1];
            sources[t - 1] = swap;
        }
}

/* The newest backup holding any of `mask`, -1 for none. */
static int NewestWith(const RestoreSource *sources, int count, DWORD mask)
{
    int s;
    for (s = count - 1; s >= 0; s--)
        if (sources[s].offered & mask) return s;
    return -1;
}

/* The restore conflicts are the sync conflicts' list (checklist.c, read-only):
 * a row per unit that several of the backups hold and one only can give (the
 * settings side, or the account), a column per backup, the one it comes from
 * checked. */
enum { PICK_UNIT_SETTINGS, PICK_UNIT_ACCOUNT, PICK_UNITS };
static const DWORD kUnitMask[PICK_UNITS] = { BK_APPCONFIG | BK_LAYOUT, BK_SIGNIN | BK_KEY };
enum { PICK_CHOSEN, PICK_CANDIDATE, PICK_SETTINGS, PICK_ACCOUNT, PICK_IMAGES };
static const WCHAR kPickGlyphs[PICK_IMAGES] = { 0xEC61, 0xEA3A, 0xE713, 0xE8D7 };
static const ThemeTint kPickTints[PICK_IMAGES] = { THEME_TINT_GREEN, THEME_TINT_NONE, THEME_TINT_BLUE, THEME_TINT_GOLD };

typedef struct PickDialog {
    const RestoreSource *sources;
    int                  columns[RESTORE_SOURCES];   /* the backup of each column after the first */
    int                  columnCount;
    int                  rows[PICK_UNITS];   /* the units in conflict */
    int                  rowCount;
    int                 *from;                       /* by part: the backup it comes from */
    CheckList            list;
    CheckValue           values[PICK_UNITS];         /* a unit each, as `rows` */
    const WCHAR         *versions[PICK_UNITS][RESTORE_SOURCES];   /* each unit's backup's date, NULL where it holds none */
    const WCHAR         *titles[1 + RESTORE_SOURCES];
    HICON                icon;
} PickDialog;

/* The list in `list`: each unit a pick of the backups holding it, the one it comes from kept; the first selected, so
 * that the arrows choose from the start. */
static void FillPicks(HWND list, PickDialog *state)
{
    int row, i;
    ZeroMemory(&state->list, sizeof state->list);
    state->titles[0] = TR(L"Restore");
    for (i = 0; i < state->columnCount; i++) state->titles[i + 1] = state->sources[state->columns[i]].name;
    state->list.values = state->values;
    state->list.columns = state->titles;
    state->list.columnCount = 1 + state->columnCount;
    state->list.plain = TRUE;
    state->list.pickedImage = PICK_CHOSEN;
    state->list.candidateImage = PICK_CANDIDATE;
    for (row = 0; row < state->rowCount; row++) {
        int unit = state->rows[row];
        CheckValue *value = &state->values[row];
        ZeroMemory(value, sizeof *value);
        value->group = -1;
        value->kind = CHECK_VALUE_PICK;
        value->image = unit == PICK_UNIT_SETTINGS ? PICK_SETTINGS : PICK_ACCOUNT;
        value->label = unit == PICK_UNIT_SETTINGS ? L"Settings" : L"Account";
        value->pick = -1;
        for (i = 0; i < state->columnCount; i++) {
            const RestoreSource *source = &state->sources[state->columns[i]];
            state->versions[row][i] = (source->offered & kUnitMask[unit]) ? source->written : NULL;
            if (state->versions[row][i] && state->from[unit] == state->columns[i]) value->pick = i;
        }
        value->versions = state->versions[row];
        state->list.valueCount++;
    }
    CheckList_Setup(&state->list, list, kPickGlyphs, kPickTints, PICK_IMAGES);
    CheckList_SelectValue(&state->list, 0, FALSE);
}

/* Every part from the newest backup holding it (`choice` 0), or from the one of column `choice` - 1 where it holds it. */
static void PickAll(PickDialog *state, LRESULT choice)
{
    int row, i;
    for (row = 0; row < state->list.valueCount; row++) {
        CheckValue *value = &state->values[row];
        for (i = 0; i < state->columnCount; i++)
            if (value->versions[i] && (choice <= 0 || i == choice - 1)) value->pick = i;
    }
    CheckList_Refresh(&state->list);
}

static INT_PTR CALLBACK PickProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    PickDialog *state = (PickDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        int i;
        state = (PickDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, TR(L"Restore conflicts"));
        SetDlgItemTextW(dialog, IDC_X_TEXT, TR(L"Several of these backups hold these, and only one can be restored. Choose the backup of each:"));
        SetDlgItemTextW(dialog, IDC_X_NOTE, TR(L"Click the backup to restore from, or press Left and Right. Sessions come from every backup."));
        SetDlgItemTextW(dialog, IDOK, TR(L"Restore"));
        SetDlgItemTextW(dialog, IDCANCEL, TR(L"Cancel"));
        if ((state->icon = Theme_LoadIcon(NULL, IDI_INFORMATION, GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(dialog)),
                                          GetSystemMetricsForDpi(SM_CYICON, GetDpiForWindow(dialog)))) != NULL)
            SendDlgItemMessageW(dialog, IDC_X_ICON, STM_SETICON, (WPARAM)state->icon, 0);
        Theme_SetGlyph(GetDlgItem(dialog, IDOK), 0xE73E, THEME_TINT_GREEN);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)TR(L"The newest of each"));
        for (i = 0; i < state->columnCount; i++)
            SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)state->sources[state->columns[i]].name);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_SETCURSEL, 0, 0);
        FillPicks(GetDlgItem(dialog, IDC_X_LIST), state);
        Theme_SmoothView(state->list.list);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_X_NOTE);

    case WM_NOTIFY: {
        BOOL toggled;
        if (state && CheckList_Message(&state->list, dialog, message, wp, lp, &toggled)) return TRUE;
        break;
    }

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_X_PROFILE:
            if (HIWORD(wp) == CBN_SELCHANGE) PickAll(state, SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_GETCURSEL, 0, 0));
            return TRUE;
        case IDOK: {
            int row;
            for (row = 0; row < state->list.valueCount; row++)
                if (state->values[row].pick >= 0) state->from[state->rows[row]] = state->columns[state->values[row].pick];
            EndDialog(dialog, IDOK);
            return TRUE;
        }
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

/* The backup each part only one can give comes from (`from`, by part): the
 * newest holding it, or the one the person chose where several of the
 * chosen ones hold it. FALSE when the person cancelled. */
static BOOL ChooseSources(HWND owner, const RestoreSource *sources, int count, DWORD chosen, int *from)
{
    PickDialog dialog;
    int unit, s, row;
    ZeroMemory(&dialog, sizeof dialog);
    dialog.sources = sources;
    dialog.from = from;
    for (unit = 0; unit < PICK_UNITS; unit++) {
        int holders = 0;
        from[unit] = NewestWith(sources, count, kUnitMask[unit]);
        if (!(chosen & kUnitMask[unit])) continue;
        for (s = 0; s < count; s++)
            if (sources[s].offered & kUnitMask[unit]) holders++;
        if (holders > 1) dialog.rows[dialog.rowCount++] = unit;
    }
    if (!dialog.rowCount) return TRUE;
    for (s = 0; s < count; s++)
        for (row = 0; row < dialog.rowCount; row++)
            if (sources[s].offered & kUnitMask[dialog.rows[row]]) {
                dialog.columns[dialog.columnCount++] = s;
                break;
            }
    return Ui_Dialog(owner, IDD_CONFLICTS, PickProc, (LPARAM)&dialog) == IDOK;
}

typedef struct RestoreResult {
    DWORD code;
    int sessions;
    BOOL signedOut;
    const WCHAR *keyNote;
    WCHAR error[LONG_PATH_CCH + 256];
} RestoreResult;

/* The chosen content, in the order the target account must receive it.
 * Kept separate from the chooser and protective backup for fixture tests. */
/* What an import takes, as the person chose it: its items, the profile's
 * own as left (NULL: as the backup has it), the sessions checked (NULL:
 * all), and the settings changed (a layout of those parts alone, NULL: none). */
typedef struct RestoreChoice {
    DWORD              items;
    const ProfileOwn  *own;
    const SessionPick *sessions;
    const char        *settings;
    size_t             settingsLength;
} RestoreChoice;

static BOOL RestoreChosen(HWND owner, const ProfileList *list, int index, const RestoreSource *sources, int count, const RestoreChoice *choice,
                          const int *from, const WCHAR *backup, const WCHAR *title, RestoreResult *result)
{
    const Profile *p = &list->items[index];
    DWORD chosen = choice->items;
    const ProfileOwn *own = choice->own;
    const SessionPick *pick = choice->sessions;
    BOOL ok = TRUE;
    HCURSOR old;
    int s;
    size_t i;
    ZeroMemory(result, sizeof *result);
    if ((chosen & BK_SIGNIN) && from[PICK_UNIT_ACCOUNT] >= 0) {
        const RestoreSource *source = &sources[from[PICK_UNIT_ACCOUNT]];
        /* Establish the target account before resolving its session folder
         * or applying settings from the separately chosen backup. */
        if ((ok = SetAsideSignIn(p->storageDir, &result->code)) != FALSE) {
            ok = ExtractTree(source->zip, "signin", p->storageDir, &result->code);
            PutBackSignIn(p->storageDir, ok);
        }
        old = SetCursor(LoadCursorW(NULL, IDC_ARROW));
        if (ok) result->keyNote = OpenSignInKey(owner, source->zip, p, title, (source->offered & BK_KEY) != 0);
        SetCursor(old);
        if (!ok) return FALSE;
    }
    /* Sessions and Cowork from every backup, the oldest first: what several hold, as the newest has it. */
    for (s = 0; s < count; s++) {
        int found = 0;
        if ((chosen & BK_CONVERSATIONS) && (sources[s].offered & BK_CONVERSATIONS)) {
            ok = RestoreSessions(sources[s].zip, list, index, pick, backup, &found, &result->signedOut, result->error, ARRAYSIZE(result->error)) && ok;
            result->sessions += found;
        }
        if ((chosen & BK_COWORK) && (sources[s].offered & BK_COWORK)) {
            WCHAR cowork[LONG_PATH_CCH];
            ok = SUCCEEDED(StringCchPrintfW(cowork, ARRAYSIZE(cowork), L"%s\\" COWORK_DIR, p->storageDir)) &&
                 ExtractTreeWithTasks(sources[s].zip, "cowork", cowork, &result->code, p, backup) && ok;
        }
    }
    /* The settings side (app config, theme and the layout) from one backup. */
    if ((chosen & (BK_APPCONFIG | BK_LAYOUT)) && from[PICK_UNIT_SETTINGS] >= 0) {
        const RestoreSource *source = &sources[from[PICK_UNIT_SETTINGS]];
        const ZipIn *zip = source->zip;
        if ((chosen & BK_APPCONFIG) && (source->offered & BK_APPCONFIG)) {
            if (source->version >= 2) {
                ok = RestoreMcp(zip, p->storageDir) && ok;
            } else {
                /* A backup from before: its whole config.json. */
                int found = Zip_Find(zip, "settings/" "claude_desktop_config.json");
                WCHAR target[LONG_PATH_CCH];
                if (found >= 0 && SUCCEEDED(StringCchPrintfW(target, ARRAYSIZE(target), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir)) &&
                    !Zip_Extract(zip, found, target)) {
                    result->code = GetLastError();
                    ok = FALSE;
                }
            }
            for (i = 0; i < ARRAYSIZE(kSettingsFiles); i++) {
                char name[MAX_PATH];
                WCHAR target[LONG_PATH_CCH];
                int found;
                WideCharToMultiByte(CP_UTF8, 0, kSettingsFiles[i], -1, name + 9, sizeof name - 9, NULL, NULL);
                memcpy(name, "settings/", 9);
                if ((found = Zip_Find(zip, name)) < 0 || FAILED(StringCchPrintfW(target, ARRAYSIZE(target), L"%s\\%s", p->storageDir, kSettingsFiles[i])))
                    continue;
                if (!Zip_Extract(zip, found, target)) {
                    result->code = GetLastError();
                    ok = FALSE;
                }
            }
        }
        if (chosen & SYNC_ITEM_APPEARANCE) ok = RestoreAppearance(zip, p->storageDir) && ok;
        if ((chosen & BK_LAYOUT) && (source->offered & BK_LAYOUT)) {
            int found = Zip_Find(zip, LAYOUT_NAME);
            WCHAR dir[LONG_PATH_CCH];
            BOOL signedIn = FALSE;
            char *layout, *filtered;
            size_t filteredLength = 0;
            DWORD rawLength = 0;
            if (found >= 0 && !SessionStore_EntriesDir(p, dir, ARRAYSIZE(dir), &signedIn)) {
                StringCchCopyW(result->error, ARRAYSIZE(result->error), TR(L"Open this profile in Claude once, then close it and restore its sessions and settings again."));
                ok = FALSE;
            } else if (found >= 0 && (layout = (char *)Zip_Read(zip, found, JSON_MAX, &rawLength)) != NULL) {
                /* The settings the person changed, theirs. */
                if ((filtered = SessionSync_FilterLayout(layout, rawLength, chosen & BK_LAYOUT, &filteredLength)) != NULL)
                    filtered = WithChanges(filtered, &filteredLength, choice->settings, choice->settingsLength, chosen & BK_LAYOUT);
                if (filtered && filteredLength > 2 && !SessionSync_RestoreLayout(p, dir, filtered, filteredLength, NULL)) ok = FALSE;
                Util_Free(filtered);
                Util_Free(layout);
            }
        }
    }
    /* The profile's own, from the newest backup that holds it: as the person left it, or all of it. */
    for (s = count - 1; (chosen & BK_PROFILE) && s >= 0; s--)
        if (sources[s].offered & BK_PROFILE) {
            ProfileOwn held;
            if (!own || !own->held) own = ReadProfileOwn(sources[s].zip, &held) ? &held : NULL;
            ok = own && RestoreProfileOwn(sources[s].zip, p, own) && ok;
            break;
        }
    return ok;
}

/* Whether profile `p`, or one that shares its session folder, is open: then said. */
static BOOL TellIfBusy(HWND owner, const Profile *p)
{
    if (!SessionLink_Busy(p)) return FALSE;
    Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
    return TRUE;
}

/* What restoring the parts `chosen` replaces in profile `p`, kept first in
 * a new folder, `backup`. FALSE with the error in `code`. */
static BOOL KeepReplaced(const Profile *p, DWORD chosen, WCHAR *backup, size_t cch, DWORD *code)
{
    size_t i;
    BOOL ok = RestoreBackupDir(p, backup, cch);
    if (!ok) *code = GetLastError();
    if (ok && (chosen & BK_COWORK)) ok = KeepCopy(p->storageDir, COWORK_DIR, backup, TRUE, code);
    if (ok && (chosen & (BK_APPCONFIG | BK_LAYOUT))) {
        /* The settings side touches config.json, Preferences and the app files. */
        for (i = 0; ok && i < ARRAYSIZE(kSettingsFiles); i++) ok = KeepCopy(p->storageDir, kSettingsFiles[i], backup, FALSE, code);
        ok = ok && KeepCopy(p->storageDir, CLAUDE_APP_SETTINGS, backup, FALSE, code);
        ok = ok && KeepCopy(p->storageDir, CLAUDE_DESKTOP_SETTINGS, backup, FALSE, code);
        ok = ok && KeepCopy(p->storageDir, L"Preferences", backup, FALSE, code);
    }
    if (ok && (chosen & BK_SIGNIN)) {
        for (i = 0; ok && i < ARRAYSIZE(kSigninFiles); i++) ok = KeepCopy(p->storageDir, kSigninFiles[i], backup, FALSE, code);
        for (i = 0; ok && i < ARRAYSIZE(kSigninFolders); i++) ok = KeepCopy(p->storageDir, kSigninFolders[i], backup, TRUE, code);
    }
    return ok;
}

static void AddReportLine(WCHAR *report, size_t cch, const WCHAR *line)
{
    StringCchCatW(report, cch, L"\n");
    StringCchCatW(report, cch, line);
}

/* What a restore of `count` backups into `p` did, said, and where what it replaced is kept (`backup`; NULL for a
 * profile made for the restore, which had nothing to replace). */
static void TellRestored(HWND owner, const Profile *p, int count, DWORD chosen, BOOL ok, const RestoreResult *result, const WCHAR *backup)
{
    WCHAR report[4 * LONG_PATH_CCH], line[LONG_PATH_CCH + 256];
    report[0] = 0;
    if (ok) StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Restored into \x201C%s\x201D."), p->name);
    else if (result->error[0]) StringCchCopyW(report, ARRAYSIZE(report), result->error);
    else StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Some files could not be restored (error %lu)."), result->code);
    if ((chosen & BK_CONVERSATIONS) && !result->signedOut) {
        if (count == 1) StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Sessions in the archive: %d"), result->sessions);
        else StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Sessions in the backups: %d"), result->sessions);
        AddReportLine(report, ARRAYSIZE(report), line);
    }
    if (result->signedOut) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"The Code sessions were left out: \x201C%s\x201D is not signed in to Claude yet."), p->name);
        AddReportLine(report, ARRAYSIZE(report), line);
    }
    if (result->keyNote) AddReportLine(report, ARRAYSIZE(report), result->keyNote);
    if (backup) {
        AddReportLine(report, ARRAYSIZE(report), TR(L"What was replaced or removed is kept in:"));
        AddReportLine(report, ARRAYSIZE(report), backup);
    }
    Ui_Message(owner, ok ? MB_ICONINFORMATION : MB_ICONWARNING, L"%s", report);
}

/* The sessions the backups `sources` hold, each once (the newest's), the
 * latest first: a heap block, their count; 0 for none, or when one cannot
 * be read (its sessions then come whole). */
static int BackupSessions(const RestoreSource *sources, int count, ArchivedSession **out)
{
    ArchivedSession *all = NULL, *some = NULL, *grown;
    int s, i, j, n = 0, found;
    *out = NULL;
    for (s = 0; s < count; s++) {
        ZipIn *inner;
        int at;
        if (!(sources[s].offered & BK_CONVERSATIONS) || (at = Zip_Find(sources[s].zip, SESSIONS_ARCHIVE)) < 0) continue;
        inner = Zip_OpenItem(sources[s].zip, at);
        found = inner ? SessionSync_ArchiveSessions(inner, &some) : -1;
        Zip_Free(inner);
        grown = found < 0 ? NULL : all ? (ArchivedSession *)HeapReAlloc(GetProcessHeap(), 0, all, (size_t)max(n + found, 1) * sizeof *all)
                                       : (ArchivedSession *)Util_Alloc((size_t)max(n + found, 1) * sizeof *all);
        if (!grown) {
            Util_Free(some);
            Util_Free(all);
            return 0;
        }
        all = grown;
        for (i = 0; i < found; i++) {
            for (j = 0; j < n && _wcsicmp(all[j].key, some[i].key) != 0; j++)
                ;
            if (j == n) all[n++] = some[i];
            else if (some[i].lastActivity >= all[j].lastActivity) all[j] = some[i];
        }
        Util_Free(some);
        some = NULL;
    }
    if (all) qsort(all, (size_t)n, sizeof *all, LatestFirst);
    *out = all;
    return n;
}

BOOL Backup_ReadOwn(const WCHAR *path, ProfileOwn *own)
{
    RestoreSource *source = (RestoreSource *)Util_Alloc(sizeof *source);
    BOOL ok = FALSE;
    ZeroMemory(own, sizeof *own);
    own->color = -1;
    if (!source) return FALSE;
    if (SUCCEEDED(StringCchCopyW(source->path, ARRAYSIZE(source->path), path)) && OpenSource(source) && (source->offered & BK_PROFILE))
        ok = ReadProfileOwn(source->zip, own);
    Zip_Free(source->zip);
    Util_Free(source);
    return ok;
}

/* A restore from `paths` (NULL: chosen first) into profile `index`, made once its window's choices are
 * confirmed. `made`: the profile was just made for it, its own given there (Backup_ReadOwn): of the backup's own
 * only the picture is offered, and nothing in the profile is said to be replaced. */
static BOOL RestoreFiles(HWND owner, const ProfileList *list, int index, const WCHAR *const *paths, int pathCount, BOOL made)
{
    const Profile *p = &list->items[index];
    WCHAR title[LABEL_CCH + 64], text[LABEL_CCH + 256], backup[LONG_PATH_CCH];
    WCHAR (*keys)[SESSION_ID_CCH] = NULL;
    ArchivedSession *sessions = NULL;
    SessionPick pick;
    RestoreChoice choice;
    char *layout = NULL, *changes = NULL;
    RestoreSource *sources;
    BackupDialog dialog;
    ProfileOwn own;
    DWORD code = 0;
    HCURSOR old;
    int s, count, from[PICK_UNITS];
    BOOL ok = FALSE;
    RestoreResult result;
    ZeroMemory(&dialog, sizeof dialog);
    if (!p->storageDir[0]) return FALSE;
    if (TellIfBusy(owner, p)) return FALSE;   /* said before the choices, checked again after them */
    StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Restore \x201C%s\x201D from a backup"), p->name);
    if ((sources = (RestoreSource *)Util_Alloc(RESTORE_SOURCES * sizeof *sources)) == NULL) return FALSE;
    if (paths) {
        for (count = 0; count < pathCount && count < RESTORE_SOURCES; count++)
            if (FAILED(StringCchCopyW(sources[count].path, ARRAYSIZE(sources[count].path), paths[count]))) break;
        if (count < pathCount) {
            Ui_Message(owner, MB_ICONWARNING, TR(L"Choose %d backups at most."), RESTORE_SOURCES);
            goto done;
        }
    } else if ((count = ChooseBackups(owner, title, p->name, sources)) == 0) {
        goto done;
    }
    {
        /* An archive of sessions alone (exported from the sessions view) goes through their import, into this profile. */
        int kept = 0, imported = 0;
        for (s = 0; s < count; s++) {
            if (SessionSync_IsArchive(sources[s].path)) {
                if (SyncUi_ImportFile(owner, list, 1u << index, sources[s].path)) imported++;
                continue;
            }
            if (kept != s) sources[kept] = sources[s];
            kept++;
        }
        count = kept;
        if (!count) {
            ok = imported > 0;
            goto done;
        }
    }
    for (s = 0; s < count; s++)
        if (!OpenSource(&sources[s])) {
            if (count == 1) Ui_Message(owner, MB_ICONWARNING, TR(L"This file is not a backup of " APP_NAME L"."));
            else Ui_Message(owner, MB_ICONWARNING, TR(L"\x201C%s\x201D is not a backup of " APP_NAME L"."), sources[s].name);
            goto done;
        }
    SortSources(sources, count);
    ZeroMemory(&dialog, sizeof dialog);
    if (made && count == 1)   /* a new profile: nothing in it is replaced */
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D."), p->name);
    else if (made)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D from these %d backups."), p->name, count);
    else if (count == 1)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D. What it replaces is kept in a backup first."), p->name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D from these %d backups. What it replaces is kept in a backup first."),
                         p->name, count);
    dialog.title = title;
    dialog.text = text;
    dialog.action = TR(L"Restore");
    /* The sessions they hold, one by one, to leave some out. */
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    dialog.sessionCount = BackupSessions(sources, count, &sessions);
    SetCursor(old);
    dialog.sessionList = sessions;
    dialog.sessions = sessions ? dialog.sessionCount : -1;
    /* The settings of the newest backup that holds them, one by one, to change some first. */
    for (s = count - 1; s >= 0; s--)
        if ((sources[s].offered & BK_LAYOUT) && Zip_Find(sources[s].zip, LAYOUT_NAME) >= 0) {
            DWORD layoutLength = 0;
            if ((layout = (char *)Zip_Read(sources[s].zip, Zip_Find(sources[s].zip, LAYOUT_NAME), JSON_MAX, &layoutLength)) != NULL) {
                dialog.layout = layout;
                dialog.layoutLength = layoutLength;
            }
            break;
        }
    for (s = 0; s < count; s++) dialog.offered |= sources[s].offered;
    dialog.offered &= ~BK_KEY;   /* not a choice: it opens the sign-in when this PC cannot */
    dialog.chosen = dialog.offered & (BK_SIGNIN | BK_PROFILE);   /* checked at first: the account and the profile's own */
    for (s = count - 1; s >= 0 && !dialog.signinZip; s--)
        if (sources[s].offered & BK_SIGNIN) dialog.signinZip = sources[s].zip;   /* the newest sign-in, as its item shows it */
    /* The profile's own of the newest backup that holds it, to change before it is given. */
    ZeroMemory(&own, sizeof own);
    for (s = count - 1; s >= 0; s--)
        if (sources[s].offered & BK_PROFILE) {
            if (ReadProfileOwn(sources[s].zip, &own)) dialog.own = &own;
            break;
        }
    if (made) {
        own.held &= PROFILE_OWN_PICTURE;
        own.parts = own.held;
        if (!own.held) {
            dialog.own = NULL;
            dialog.offered &= ~BK_PROFILE;
            dialog.chosen &= ~BK_PROFILE;
        }
    }
    ZeroMemory(&pick, sizeof pick);
    ZeroMemory(&choice, sizeof choice);
    if (Ui_Dialog(owner, IDD_BACKUP, BackupProc, (LPARAM)&dialog) != IDOK || !TakeSessions(&dialog, &keys, &pick.count)) goto done;
    pick.keys = (const WCHAR (*)[SESSION_ID_CCH])keys;
    changes = TakeLayoutChanges(&dialog, &choice.settingsLength);
    choice.items = dialog.chosen;
    choice.own = &own;
    choice.sessions = keys ? &pick : NULL;
    choice.settings = changes;
    if (!ChooseSources(owner, sources, count, dialog.chosen, from)) goto done;   /* chosen in the window: not asked again */
    if (TellIfBusy(owner, p)) goto done;
    /* A complete outer/inner archive pass finishes before even creating the
     * target's safety-backup folder. All sources stay held open by ZipIn. */
    for (s = 0; s < count; s++)
        if (!Backup_CheckArchive(owner, sources[s].path)) goto done;
    if (TellIfBusy(owner, p)) goto done;
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    /* What the restore replaces, kept first. */
    if (!(ok = KeepReplaced(p, dialog.chosen, backup, ARRAYSIZE(backup), &code))) {
        SetCursor(old);
        Ui_Message(owner, MB_ICONERROR, TR(L"What the restore would replace could not be backed up (error %lu): nothing was changed."), code);
        goto done;
    }
    ok = RestoreChosen(owner, list, index, sources, count, &choice, from, backup, title, &result);
    SetCursor(old);
    for (s = 0; s < count; s++) Util_Log(L"restored %s from %s%s", p->folder, sources[s].path, ok ? L"" : L" (partly)");
    TellRestored(owner, p, count, dialog.chosen, ok, &result, made ? NULL : backup);
done:
    for (s = 0; s < RESTORE_SOURCES; s++) Zip_Free(sources[s].zip);
    Util_Free(sources);
    FreeDialog(&dialog);
    Util_Free(sessions);
    Util_Free(keys);
    Util_Free(layout);
    Util_Free(changes);
    return ok;
}

BOOL Backup_RestoreFiles(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index, const WCHAR *const *paths, int pathCount)
{
    (void)pkg;
    return RestoreFiles(owner, list, index, paths, pathCount, FALSE);
}

BOOL Backup_RestoreNew(HWND owner, const ProfileList *list, int index, const WCHAR *const *paths, int pathCount)
{
    return RestoreFiles(owner, list, index, paths, pathCount, TRUE);
}

BOOL Backup_Restore(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index)
{
    return Backup_RestoreFiles(owner, pkg, list, index, NULL, 0);
}

typedef struct ArchiveCheck {
    HWND           dialog;   /* the window showing the check, once it shows */
    const WCHAR   *path;
    HANDLE         cancel, thread;
    LONG volatile  done;
    BOOL           ok;
    int            files;
    WCHAR          error[1024];
} ArchiveCheck;

#define WM_ARCHIVE_CHECKED     (WM_APP + 81)
#define ARCHIVE_CHECK_QUIET_MS 500   /* a check done sooner shows no window */

static DWORD WINAPI CheckArchiveThread(void *context)
{
    ArchiveCheck *state = (ArchiveCheck *)context;
    HWND dialog;
    state->ok = Zip_Validate(state->path, state->cancel, &state->files, state->error, ARRAYSIZE(state->error));
    InterlockedExchange(&state->done, 1);
    dialog = (HWND)InterlockedCompareExchangePointer((PVOID volatile *)&state->dialog, NULL, NULL);
    if (dialog) PostMessageW(dialog, WM_ARCHIVE_CHECKED, 0, 0);
    return 0;
}

static INT_PTR CALLBACK CheckArchiveProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ArchiveCheck *state = (ArchiveCheck *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        state = (ArchiveCheck *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, TR(L"Checking the archive"));
        SetDlgItemTextW(dialog, IDC_T_NOTE, TR(L"Every file of the archive is read before anything changes."));
        SetDlgItemTextW(dialog, IDC_T_TEXT, state->path);
        ShowWindow(GetDlgItem(dialog, IDC_T_COPY), SW_HIDE);
        ShowWindow(GetDlgItem(dialog, IDOK), SW_HIDE);
        /* The check may have ended while the window was made: then its message is posted here. */
        InterlockedExchangePointer((PVOID volatile *)&state->dialog, dialog);
        if (InterlockedCompareExchange(&state->done, 0, 0)) PostMessageW(dialog, WM_ARCHIVE_CHECKED, 0, 0);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_ARCHIVE_CHECKED) {
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) {
        SetEvent(state->cancel);
        SetDlgItemTextW(dialog, IDC_T_TEXT, TR(L"Cancelling after the current archive item"));
        EnableWindow(GetDlgItem(dialog, IDCANCEL), FALSE);
        return TRUE;
    }
    return FALSE;
}

/* Archive `path` read whole before an import changes anything: a window
 * shows only when that takes a while, and says why when it fails. */
BOOL Backup_CheckArchive(HWND owner, const WCHAR *path)
{
    ArchiveCheck state;
    BOOL ok, cancelled;
    ZeroMemory(&state, sizeof state);
    state.path = path;
    if ((state.cancel = CreateEventW(NULL, TRUE, FALSE, NULL)) == NULL) return FALSE;
    if ((state.thread = CreateThread(NULL, 0, CheckArchiveThread, &state, 0, NULL)) == NULL) {
        Util_Log(L"archive %s not checked: no thread (error %lu)", path, GetLastError());
        CloseHandle(state.cancel);
        return FALSE;
    }
    if (WaitForSingleObject(state.thread, ARCHIVE_CHECK_QUIET_MS) == WAIT_TIMEOUT) Ui_Dialog(owner, IDD_TEXTVIEW, CheckArchiveProc, (LPARAM)&state);
    WaitForSingleObject(state.thread, INFINITE);
    CloseHandle(state.thread);
    cancelled = WaitForSingleObject(state.cancel, 0) == WAIT_OBJECT_0;
    ok = state.ok && !cancelled;
    if (!ok && !cancelled) Ui_Message(owner, MB_ICONERROR, TR(L"The archive cannot be imported. %s"), state.error);
    CloseHandle(state.cancel);
    return ok;
}

