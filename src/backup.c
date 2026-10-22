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
 *                  old and new files
 *   its key        that key itself, opened with DPAPI and sealed behind a
 *                  password (Core_SealSecret): the sign-in then restores on
 *                  any PC, its key protected again there for its Windows user
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
#include "operationhistory.h"
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
#define KEY_NAME          "account-key.json"   /* the sign-in's key behind a password, beside the sign-in, never extracted */
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
#define BK_KEY            0x40000000u   /* the sign-in's key, for any PC, behind a password */
#define BK_CONTENT        (BK_CONVERSATIONS | BK_COWORK | BK_APPCONFIG | BK_SIGNIN | BK_KEY)
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
#define BACKUP_ITEMS_MAX   40
#define BACKUP_GLYPHS_MAX  24

typedef struct BackupDialog {
    const WCHAR *title, *text, *action;
    DWORD        offered, chosen;     /* the items offered and checked (BK_* and SYNC_ITEM_*) */
    int          sessions;            /* the sessions the profile lists, -1: not counted */
    CheckList    parts;
    CheckGroup   groups[BACKUP_GROUPS_MAX];   /* what to sync's groups, Sessions and Account its own */
    CheckItem    items[BACKUP_ITEMS_MAX];
    int          groupCount, itemCount;
    WCHAR        glyphs[BACKUP_GLYPHS_MAX];    /* what to sync's, a gold key added for the account */
    ThemeTint    tints[BACKUP_GLYPHS_MAX];
    int          images, keyImage;
    WCHAR        label[256];          /* an item's text, as the list takes it */
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

static void *Alloc(size_t bytes) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes); }
static void Free(void *memory) { if (memory) HeapFree(GetProcessHeap(), 0, memory); }

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
    case BK_KEY:       return TR(L"Sign-in for any PC: its key, behind a password");
    case BK_SIGNIN:    return TR(L"Sign-in on this PC: cookies and the app's storage");
    default:           return NULL;
    }
}

/* The groups and items a backup's list shows: what to sync's, with Sessions
 * and Account the backup's own content, and the Settings group holding the
 * app config too; their glyphs are what to sync's, a gold key added. */
static void BuildTables(BackupDialog *state)
{
    const CheckGroup *sg;
    const CheckItem *si;
    const WCHAR *glyphs;
    const ThemeTint *tints;
    int sgc = 0, sic = 0, images = 0, g, i;
    glyphs = SyncUi_Glyphs(&tints, &images);
    sg = SyncUi_Groups(&sgc);
    si = SyncUi_Items(&sic);
    for (i = 0; i < images && i < BACKUP_GLYPHS_MAX - 1; i++) {
        state->glyphs[i] = glyphs[i];
        state->tints[i] = tints[i];
    }
    state->keyImage = i;
    state->glyphs[i] = 0xE8D7;             /* a gold key for the account, which what to sync has no image of */
    state->tints[i] = THEME_TINT_GOLD;
    state->images = i + 1;
    /* Sessions (the backup's content), then what to sync's layout groups, the Settings one given the app config, then Account. */
    state->groups[0] = sg[0];
    state->groups[0].mask = BK_CONVERSATIONS | BK_COWORK;
    for (g = 1; g < sgc; g++) {
        state->groups[g] = sg[g];
        if (sg[g].mask & SYNC_ITEM_SETTINGS) state->groups[g].mask |= BK_APPCONFIG;
    }
    state->groups[g].mask = BK_SIGNIN | BK_KEY;
    state->groups[g].heading = L"Account";
    state->groups[g].image = state->keyImage;
    state->groups[g].name = L"account";
    state->groupCount = g + 1;
    i = 0;
    state->items[i].bit = BK_CONVERSATIONS; state->items[i].label = NULL; state->items[i++].image = sg[0].image;
    state->items[i].bit = BK_COWORK;        state->items[i].label = NULL; state->items[i++].image = sg[0].image;
    for (g = 0; g < sic && i < BACKUP_ITEMS_MAX - 3; g++)
        if (si[g].bit & BK_LAYOUT) state->items[i++] = si[g];   /* the app config comes after, last in Settings */
    state->items[i].bit = BK_APPCONFIG; state->items[i].label = NULL; state->items[i++].image = state->groups[6].image;
    state->items[i].bit = BK_SIGNIN;    state->items[i].label = NULL; state->items[i++].image = state->keyImage;
    state->items[i].bit = BK_KEY;       state->items[i].label = NULL; state->items[i++].image = state->keyImage;
    state->itemCount = i;
}

/* ------------------------------------------------------------ the dialog */

static const WCHAR *BackupItemLabel(const CheckItem *item, void *context)
{
    BackupDialog *state = (BackupDialog *)context;
    return PartLabel(item->bit, state->sessions, state->label, ARRAYSIZE(state->label));   /* NULL for a layout item: its own label */
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
    CheckList_Setup(&state->parts, list, state->glyphs, state->tints, state->images);
}

void Backup_FillPartsList(HWND list, int sessions)
{
    static BackupDialog shown;   /* a list alone, as the tests show it */
    ZeroMemory(&shown, sizeof shown);
    shown.sessions = sessions;
    shown.offered = BK_ALL;
    shown.chosen = (BK_CONTENT | BK_LAYOUT) & ~(BK_SIGNIN | BK_KEY);   /* sign-in only when asked for, as Backup_Create offers it */
    SetupParts(&shown, list);
}

static void ReadChosen(HWND dialog, BackupDialog *state)
{
    state->chosen = state->parts.checked;
    EnableWindow(GetDlgItem(dialog, IDOK), state->chosen != 0);
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
        if (!(state->offered & BK_SIGNIN)) ShowWindow(GetDlgItem(dialog, IDC_B_NOTE), SW_HIDE);   /* its warning is about sign-in */
        SetupParts(state, list);
        Theme_SmoothView(list);
        ReadChosen(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_B_NOTE);

    case WM_NOTIFY: {
        LRESULT result;
        if (state && CheckList_Notify(&state->parts, dialog, (const NMHDR *)lp, &result)) {
            SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
            return TRUE;
        }
        break;
    }

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
    DWORD size = 0, done = 0;
    char *json, *changed = NULL, quoted[1024];
    BYTE blob[1024];
    DATA_BLOB in, out;
    size_t changedLength = 0;
    HANDLE file;
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
        file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, changed, (DWORD)changedLength, &done, NULL) && done == changedLength;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        HeapFree(GetProcessHeap(), 0, changed);
    }
    HeapFree(GetProcessHeap(), 0, json);
    return ok;
}

typedef enum ArchiveChoice { ARCHIVE_SAVE, ARCHIVE_OPEN, ARCHIVE_FOLDER } ArchiveChoice;

/* "<profile> <date>.zip", without the characters a file name cannot hold. */
static void ArchiveName(const WCHAR *profileName, WCHAR *name, size_t cch)
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
        ArchiveName(profileName, name, ARRAYSIZE(name));
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
        Free(manifest);
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
    Free(manifest);
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

/* Every session the profile lists, as a session archive in the backup. */
static BOOL AddSessions(ZipOut *zip, const ProfileList *list, int index, int *count, WCHAR *error, size_t errorCch)
{
    WCHAR temporary[MAX_PATH], dir[LONG_PATH_CCH], tasks[LONG_PATH_CCH];
    SessionSet set;
    int *rows, r, n = 0;
    BOOL ok;
    *count = 0;
    if (!SessionStore_LoadProfiles(&set, list)) {
        StringCchCopyW(error, errorCch, TR(L"Sessions could not be loaded."));
        return FALSE;
    }
    rows = (int *)Alloc((size_t)max(set.rowCount, 1) * sizeof *rows);
    if (!rows) {
        SessionStore_Free(&set);
        return FALSE;
    }
    for (r = 0; r < set.rowCount; r++)
        if (set.rows[r].entry[index] >= 0 && !set.entries[set.rows[r].entry[index]].pendingRemove) rows[n++] = r;
    ok = TRUE;
    if (n) {
        WCHAR folder[MAX_PATH];
        ok = GetTempPathW(ARRAYSIZE(folder), folder) && GetTempFileNameW(folder, L"cdm", 0, temporary) &&
             SessionSync_Export(&set, index, rows, n, temporary, count, error, errorCch) && Zip_AddFile(zip, SESSIONS_ARCHIVE, temporary);
        DeleteFileW(temporary);
    }
    if (ok && SessionStore_EntriesDir(&list->items[index], dir, ARRAYSIZE(dir), NULL) &&
        SUCCEEDED(StringCchPrintfW(tasks, ARRAYSIZE(tasks), L"%s\\" SCHEDULED_TASKS, dir)) && Util_FileExists(tasks))
        ok = Zip_AddFile(zip, "sessions/" "scheduled-tasks.json", tasks) &&
             VaultTasks_AddRegistry(zip, &list->items[index], tasks, "sessions/scheduled-tasks.json", TRUE, error, errorCch);
    Free(rows);
    SessionStore_Free(&set);
    return ok;
}

/* The sessions the profiles of `profiles` list, together; -1 when they could not be read. */
static int CountSessions(const ProfileList *list, DWORD profiles)
{
    SessionSet set;
    int r, i, n = 0;
    if (!SessionStore_LoadProfiles(&set, list)) return -1;
    for (i = 0; i < list->count; i++)
        for (r = 0; (profiles & (1u << i)) && r < set.rowCount; r++)
            if (set.rows[r].entry[i] >= 0 && !set.entries[set.rows[r].entry[i]].pendingRemove) n++;
    SessionStore_Free(&set);
    return n;
}

/* The profile's settings side (sidebar, details, appearance, language, model,
 * settings, permissions), member by member, as the sync reads it, filtered to
 * the items chosen: one JSON in the archive a restore applies part by part. */
static BOOL AddLayout(ZipOut *zip, const ProfileList *list, int index, DWORD items)
{
    WCHAR dir[LONG_PATH_CCH];
    char *layout, *filtered;
    size_t length = 0, filteredLength = 0;
    BOOL ok = TRUE;
    if (!SessionStore_EntriesDir(&list->items[index], dir, ARRAYSIZE(dir), NULL)) return TRUE;   /* not signed in: no layout */
    if ((layout = SessionSync_ReadLayout(&list->items[index], dir, &length, NULL)) == NULL) return TRUE;
    if ((filtered = SessionSync_FilterLayout(layout, length, items, &filteredLength)) != NULL && filteredLength > 2)
        ok = Zip_AddData(zip, LAYOUT_NAME, filtered, filteredLength);
    Free(layout);
    Free(filtered);
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
    if (Core_JsonMember(json, length, "mcpServers", &value, &valueLength) && (out = (char *)Alloc(sizeof head + valueLength + 1)) != NULL) {
        memcpy(out, head, sizeof head - 1);
        memcpy(out + sizeof head - 1, value, valueLength);
        out[sizeof head - 1 + valueLength] = '}';
        ok = Zip_AddData(zip, MCP_NAME, out, sizeof head - 1 + valueLength + 1);
        Free(out);
    }
    Free(json);
    return ok;
}

/* Profile `index`'s parts `chosen` written to the archive `path`; FALSE with what went wrong in `error`. */
static BOOL WriteBackup(const ProfileList *list, int index, DWORD items, const WCHAR *password, const WCHAR *path, WCHAR *error, size_t errorCch)
{
    const Profile *p = &list->items[index];
    WCHAR cowork[LONG_PATH_CCH];
    char manifest[1024], quoted[3 * LABEL_CCH + 8];
    ZipOut *zip;
    DWORD code = 0;
    BOOL chosen[PARTS];
    int sessions = 0;
    size_t used;
    BOOL ok;
    error[0] = 0;
    BitsToParts(items, chosen);
    if ((zip = Zip_Create(path, &code)) == NULL) {
        StringCchPrintfW(error, errorCch, TR(L"The backup could not be written (error %lu)."), code);
        return FALSE;
    }
    StringCchPrintfW(cowork, ARRAYSIZE(cowork), L"%s\\" COWORK_DIR, p->storageDir);
    if (!Core_JsonQuote(p->name, quoted, sizeof quoted)) StringCchCopyA(quoted, ARRAYSIZE(quoted), "\"\"");
    used = AppendA(manifest, sizeof manifest, 0, "{\"format\":\"" BACKUP_FORMAT "\",\"version\":%d,\"profile\":%s,\"items\":%lu}",
                   BACKUP_VERSION, quoted, (unsigned long)items);
    ok = Zip_AddData(zip, MANIFEST_NAME, manifest, used);
    if (ok && chosen[PART_SESSIONS]) ok = AddSessions(zip, list, index, &sessions, error, errorCch);
    if (ok && chosen[PART_COWORK]) ok = AddTreeWithTasks(zip, cowork, L"cowork", p);
    if (ok && (items & BK_LAYOUT)) ok = AddLayout(zip, list, index, items & BK_LAYOUT);
    if (ok && chosen[PART_SETTINGS]) {
        size_t i;
        ok = AddMcp(zip, p->storageDir);
        for (i = 0; ok && i < ARRAYSIZE(kSettingsFiles); i++) ok = AddIfThere(zip, p->storageDir, kSettingsFiles[i], L"settings");
    }
    if (ok && (items & SYNC_ITEM_APPEARANCE)) ok = AddAppearance(zip, p->storageDir);
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
        BYTE key[64];
        size_t keyLength = 0;
        char sealed[1024];
        if (password && Backup_ReadAppKey(p->storageDir, key, sizeof key, &keyLength) && Core_SealSecret(key, keyLength, password, KEY_ITERATIONS, sealed, sizeof sealed)) {
            ok = Zip_AddData(zip, KEY_NAME, sealed, strlen(sealed));
        } else {
            StringCchPrintfW(error, errorCch, TR(L"The sign-in's key of \x201C%s\x201D could not be read: the backup was not written."), p->name);
            ok = FALSE;
        }
        SecureZeroMemory(key, sizeof key);
        SecureZeroMemory(sealed, sizeof sealed);
    }
    code = Zip_Error(zip);
    ok = Zip_Close(zip, ok) && ok;
    if (!ok && !error[0]) StringCchPrintfW(error, errorCch, TR(L"The backup could not be written (error %lu)."), code ? code : GetLastError());
    if (ok) {
        Util_Log(L"backed up %s to %s", p->folder, path);
        BackupHistory_Record(p, path);
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

BOOL Backup_Create(HWND owner, const ClaudePackage *pkg, const ProfileList *list, DWORD profiles)
{
    WCHAR title[LABEL_CCH + 64], path[LONG_PATH_CCH], folder[LONG_PATH_CCH], name[MAX_PATH], cowork[LONG_PATH_CCH];
    WCHAR error[LONG_PATH_CCH + 256], line[LABEL_CCH + LONG_PATH_CCH + 300], report[8 * (LONG_PATH_CCH + 300)], text[9 * (LONG_PATH_CCH + 300)];
    const Profile *first = NULL;
    BackupDialog dialog;
    PasswordDialog password;
    HCURSOR old;
    int i, count = 0, saved = 0;
    (void)pkg;
    ZeroMemory(&password, sizeof password);
    for (i = 0; i < list->count; i++) {
        if (!(profiles & (1u << i))) continue;
        if (!list->items[i].storageDir[0]) {   /* no folder of its own to back up */
            profiles &= ~(1u << i);
            continue;
        }
        if (!count++) first = &list->items[i];
    }
    if (!count) return FALSE;
    if (count == 1) StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Back up \x201C%s\x201D"), first->name);
    else StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Back up %d profiles"), count);
    ZeroMemory(&dialog, sizeof dialog);
    dialog.title = title;
    dialog.text = TR(L"Choose what the backup holds:");
    dialog.action = TR(L"Back up");
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    dialog.sessions = CountSessions(list, profiles);
    SetCursor(old);
    dialog.offered = dialog.chosen = BK_APPCONFIG | BK_LAYOUT;   /* the settings side, checked by default */
    dialog.offered |= BK_SIGNIN | BK_KEY;                       /* the account, left unchecked: the user asks for it */
    for (i = 0; i < list->count; i++)
        if ((profiles & (1u << i)) && SUCCEEDED(StringCchPrintfW(cowork, ARRAYSIZE(cowork), L"%s\\" COWORK_DIR, list->items[i].storageDir)) &&
            Util_DirExists(cowork))
            dialog.offered |= BK_COWORK, dialog.chosen |= BK_COWORK;
    if (dialog.sessions != 0) dialog.offered |= BK_CONVERSATIONS, dialog.chosen |= BK_CONVERSATIONS;
    if (Ui_Dialog(owner, IDD_BACKUP, BackupProc, (LPARAM)&dialog) != IDOK) return FALSE;
    if (dialog.chosen & BK_KEY) {
        dialog.chosen |= BK_SIGNIN;   /* the key opens the sign-in: it goes with it */
        password.title = title;
        password.text = TR(L"Choose the password that opens the sign-in on another PC. Without it, the backup's sign-in restores on this PC only.");
        password.twice = TRUE;
        if (Ui_Dialog(owner, IDD_PASSWORD, PasswordProc, (LPARAM)&password) != IDOK) return ForgetPassword(&password, FALSE);
    }
    for (i = 0; (dialog.chosen & BK_SIGNIN) && i < list->count; i++)
        if ((profiles & (1u << i)) && Claude_IsRunning(&list->items[i])) {
            Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first: its sign-in files are in use."), list->items[i].name);
            return ForgetPassword(&password, FALSE);
        }
    if (count == 1) {
        if (!ChooseArchive(owner, ARCHIVE_SAVE, title, first->name, path, ARRAYSIZE(path))) return ForgetPassword(&password, FALSE);
        old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
        i = (int)(first - list->items);
        if (!WriteBackup(list, i, dialog.chosen, password.password, path, error, ARRAYSIZE(error))) {
            SetCursor(old);
            Ui_Message(owner, MB_ICONERROR, L"%s", error);
            return ForgetPassword(&password, FALSE);
        }
        SetCursor(old);
        StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Backup saved:\n%s"), path);
        DescribeTaskCoverage(path, report, ARRAYSIZE(report));
        Ui_Message(owner, MB_ICONINFORMATION, L"%s", report);
        return ForgetPassword(&password, TRUE);
    }
    /* Several: one archive each, named after its profile, in the folder chosen. */
    if (!ChooseArchive(owner, ARCHIVE_FOLDER, TR(L"Choose the folder of the backups"), NULL, folder, ARRAYSIZE(folder))) return ForgetPassword(&password, FALSE);
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    report[0] = 0;
    for (i = 0; i < list->count; i++) {
        if (!(profiles & (1u << i))) continue;
        ArchiveName(list->items[i].name, name, ARRAYSIZE(name));
        if (!FreeArchivePath(folder, name, path, ARRAYSIZE(path)))
            StringCchPrintfW(error, ARRAYSIZE(error), TR(L"The backup could not be written (error %lu)."), (unsigned long)ERROR_FILE_EXISTS);
        else if (WriteBackup(list, i, dialog.chosen, password.password, path, error, ARRAYSIZE(error))) {
            saved++;
            DescribeTaskCoverage(path, report, ARRAYSIZE(report));
            continue;
        }
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"\x201C%s\x201D: %s"), list->items[i].name, error);
        if (report[0]) StringCchCatW(report, ARRAYSIZE(report), L"\n");
        StringCchCatW(report, ARRAYSIZE(report), line);
    }
    SetCursor(old);
    StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%d backups saved in:\n%s"), saved, folder);
    if (report[0]) {
        StringCchCatW(text, ARRAYSIZE(text), L"\n\n");
        StringCchCatW(text, ARRAYSIZE(text), report);
    }
    Ui_Message(owner, saved == count ? MB_ICONINFORMATION : MB_ICONWARNING, L"%s", text);
    return ForgetPassword(&password, saved == count);
}

/* ------------------------------------------------------------ restoring */

/* A path inside the archive that stays inside the folder it is restored to. */
static BOOL SafeRelative(const char *name, const char *prefix, WCHAR *out, size_t cch)
{
    size_t length = strlen(prefix);
    const char *rest;
    int n;
    size_t i;
    if (strncmp(name, prefix, length) != 0 || name[length] != '/') return FALSE;
    rest = name + length + 1;
    if (!*rest || strstr(rest, "..") || strchr(rest, ':') || rest[0] == '/' || rest[0] == '\\') return FALSE;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, rest, -1, out, (int)cch);
    if (n <= 0) return FALSE;
    for (i = 0; out[i]; i++) {
        if (out[i] == L'/') out[i] = L'\\';
        if (out[i] < 0x20 || wcschr(L"*?\"<>|", out[i])) return FALSE;
    }
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

/* A folder of the profile gone (its copy is in the backup), so that a
 * restored one holds only the restored files. */
static BOOL RemoveFolder(const WCHAR *storage, const WCHAR *item, DWORD *code)
{
    WCHAR folder[LONG_PATH_CCH];
    if (FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", storage, item))) {
        *code = ERROR_FILENAME_EXCED_RANGE;
        return FALSE;
    }
    return Util_DeleteTree(folder, code);
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
        Free(appearance);
        return FALSE;
    }
    json = Util_ReadFile(path, JSON_MAX, FALSE, &length);
    if (!json) {
        /* No config.json yet: the appearance becomes it. */
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD done = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, appearance, appearanceLength, &done, NULL) && done == appearanceLength;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        Free(appearance);
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
        if ((next = (char *)Alloc(cap)) == NULL) {
            ok = FALSE;
            break;
        }
        ok = Core_JsonSetMember(out ? out : json, out ? outLength : length, kAppearanceKeys[i], raw, next, cap, &outLength);
        Free(out);
        out = next;
    }
    if (ok && out) {
        HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD done = 0;
        ok = file != INVALID_HANDLE_VALUE && WriteFile(file, out, (DWORD)outLength, &done, NULL) && done == outLength;
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    Free(out);
    Free(json);
    Free(appearance);
    return ok;
}

/* The backup's scheduled tasks joined to the profile's (`path`): the
 * backup's own as it has them, the profile's others kept, the profile's
 * file kept in `backup` before the restore's first change to it. */
static BOOL RestoreTasks(const ZipIn *zip, int item, const WCHAR *path, const WCHAR *backup, const Profile *profile,
                         const char *registryMember, BOOL code)
{
    WCHAR kept[LONG_PATH_CCH];
    DWORD length = 0, currentLength = 0, done = 0, taskError = 0;
    char *restored = (char *)Zip_Read(zip, item, JSON_MAX, &length), *current, *joined = NULL, *mapped;
    size_t joinedLength = 0, mappedLength = 0;
    const char *out;
    HANDLE file;
    BOOL ok, aware = FALSE;
    if (!restored) return FALSE;
    current = Util_ReadFile(path, JSON_MAX, FALSE, &currentLength);
    if (current) {
        /* A source registry has its own safe backup name, so several Cowork
         * accounts and Code cannot silently replace each other's safety copy. */
        ULONGLONG hash = Core_HashBytes(CORE_HASH_START, registryMember, strlen(registryMember));
        if (FAILED(StringCchPrintfW(kept, ARRAYSIZE(kept), L"%s\\tasks-%016I64x.json", backup, hash)) ||
            (!Util_FileExists(kept) && !CopyFileW(path, kept, TRUE))) {
            Free(current); Free(restored); return FALSE;
        }
    }
    mapped = VaultTasks_RestoreRegistry(zip, profile, registryMember, restored, length, code, &aware, &mappedLength, &taskError);
    if (aware && !mapped) { Free(current); Free(restored); SetLastError(taskError); return FALSE; }
    if (mapped) { Free(restored); restored = mapped; length = (DWORD)mappedLength; }
    out = restored; joinedLength = length;
    if (current) {
        joined = Core_JsonArrayUnion(restored, length, current, currentLength, "scheduledTasks", "id", &joinedLength);
        if (joined) out = joined; else joinedLength = length;
    }
    if (aware && SessionLink_Busy(profile)) { Free(joined); Free(restored); Free(current); SetLastError(ERROR_BUSY); return FALSE; }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = file != INVALID_HANDLE_VALUE && WriteFile(file, out, (DWORD)joinedLength, &done, NULL) && done == joinedLength;
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    Free(joined); Free(restored); Free(current);
    return ok;
}

/* The backup's Code sessions sent to the profile through their archive's
 * import, its scheduled tasks joined to the profile's. */
static BOOL RestoreSessions(const ZipIn *zip, const ProfileList *list, int index, const WCHAR *backup, int *sessions, BOOL *signedOut, WCHAR *error,
                            size_t errorCch)
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
            ok = SessionSync_Import(&set, temporary, 1u << index, sessions, &report) && report.failed == 0;
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
    DWORD mcpLength = 0, length = 0, done = 0;
    BOOL ok = TRUE, freeJson = FALSE;
    if (item < 0 || (mcp = (char *)Zip_Read(zip, item, JSON_MAX, &mcpLength)) == NULL) return TRUE;
    if (!Core_JsonMember(mcp, mcpLength, "mcpServers", &value, &valueLength) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, storage))) {
        Free(mcp);
        return item < 0 ? TRUE : FALSE;
    }
    if ((json = Util_ReadFile(path, JSON_MAX, FALSE, &length)) != NULL) freeJson = TRUE;
    else { json = (char *)"{}"; length = 2; }
    cap = (size_t)length + valueLength + 32;
    raw = (char *)Alloc(valueLength + 1);
    out = (char *)Alloc(cap);
    if (raw && out) {
        HANDLE file;
        memcpy(raw, value, valueLength);
        raw[valueLength] = 0;
        if (Core_JsonSetMember(json, length, "mcpServers", raw, out, cap, &outLength)) {
            file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            ok = file != INVALID_HANDLE_VALUE && WriteFile(file, out, (DWORD)outLength, &done, NULL) && done == outLength;
            if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        } else {
            ok = FALSE;
        }
    } else {
        ok = FALSE;
    }
    Free(raw);
    Free(out);
    if (freeJson) Free(json);
    Free(mcp);
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
    Free(sealed);
    return note;
}

/* A restore, asked first: the profile and the backups. */
static BOOL ConfirmRestore(HWND owner, const Profile *p, const RestoreSource *sources, int count)
{
    WCHAR question[LABEL_CCH + LONG_PATH_CCH + 128];
    const WCHAR *name = wcsrchr(sources[0].path, L'\\');
    if (count == 1)
        StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Restore \x201C%s\x201D from %s?"), p->name, name ? name + 1 : sources[0].path);
    else
        StringCchPrintfW(question, ARRAYSIZE(question), TR(L"Restore \x201C%s\x201D from %d backups?"), p->name, count);
    return SyncUi_Confirm(owner, IDI_WARNING, question, TRUE, TR(L"Restore"));
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

/* The restore conflicts' list, the sync conflicts' look: a row per unit that
 * several of the backups hold and one only can give (the settings side, or
 * the account), a column per backup, the one it comes from checked. */
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
    HWND                 list;
    HICON                icon;
} PickDialog;

/* Row `row`'s cells: the backup it comes from checked, the others that hold it ringed. */
static void ShowPicks(const PickDialog *state, int row)
{
    int unit = state->rows[row], column;
    for (column = 0; column < state->columnCount; column++) {
        int s = state->columns[column];
        LVITEMW item;
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_IMAGE;
        item.iItem = row;
        item.iSubItem = column + 1;
        item.iImage = !(state->sources[s].offered & kUnitMask[unit]) ? I_IMAGENONE : state->from[unit] == s ? PICK_CHOSEN : PICK_CANDIDATE;
        ListView_SetItem(state->list, &item);
    }
}

/* The columns across `width`: each backup's as wide as its name and date, the parts' the rest, two fifths at least. */
static void SizePickColumns(const PickDialog *state, int width)
{
    int wanted[RESTORE_SOURCES], total = 0, i, dpi = GetDpiForWindow(state->list), least = width * 2 / 5;
    int image = GetSystemMetricsForDpi(SM_CXSMICON, dpi), room = MulDiv(20, dpi, 96);
    for (i = 0; i < state->columnCount; i++) {
        const RestoreSource *source = &state->sources[state->columns[i]];
        wanted[i] = max(ListView_GetStringWidth(state->list, source->name) + room, ListView_GetStringWidth(state->list, source->written) + image + room);
        total += wanted[i];
    }
    if (total > width - least) {
        int shared = 0;
        for (i = 0; i < state->columnCount; i++) shared += wanted[i] = MulDiv(wanted[i], width - least, total);
        total = shared;
    }
    ListView_SetColumnWidth(state->list, 0, width - total);
    for (i = 0; i < state->columnCount; i++) ListView_SetColumnWidth(state->list, i + 1, wanted[i]);
}

static void FillPicks(HWND dialog, PickDialog *state)
{
    LVCOLUMNW column;
    LVITEMW item;
    RECT client;
    int i, row, width;
    state->list = GetDlgItem(dialog, IDC_X_LIST);
    ListView_SetExtendedListViewStyle(state->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_SUBITEMIMAGES);
    ListView_SetImageList(state->list, Theme_GlyphImages(state->list, kPickGlyphs, kPickTints, PICK_IMAGES), LVSIL_SMALL);
    GetClientRect(state->list, &client);
    width = client.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(state->list));
    ZeroMemory(&column, sizeof column);
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = (LPWSTR)TR(L"Restore");
    column.cx = width * 2 / 5;
    ListView_InsertColumn(state->list, 0, &column);
    for (i = 0; i < state->columnCount; i++) {
        column.pszText = (LPWSTR)state->sources[state->columns[i]].name;
        column.cx = (width - width * 2 / 5) / state->columnCount;
        ListView_InsertColumn(state->list, i + 1, &column);
    }
    for (row = 0; row < state->rowCount; row++) {
        int unit = state->rows[row];
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        item.iItem = row;
        item.pszText = (LPWSTR)(unit == PICK_UNIT_SETTINGS ? TR(L"Settings") : TR(L"Account"));
        item.iImage = unit == PICK_UNIT_SETTINGS ? PICK_SETTINGS : PICK_ACCOUNT;
        item.lParam = row;
        ListView_InsertItem(state->list, &item);
        for (i = 0; i < state->columnCount; i++) {
            const RestoreSource *source = &state->sources[state->columns[i]];
            ListView_SetItemText(state->list, row, i + 1, (source->offered & kUnitMask[unit]) ? (LPWSTR)source->written : (LPWSTR)L"\x2014");
        }
        ShowPicks(state, row);
    }
    SizePickColumns(state, width);
    Theme_SmoothView(state->list);
    ListView_SetItemState(state->list, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);   /* the arrows choose from the start */
}

/* Row `row` given the next backup left or right (`step`) that holds its part. */
static void StepPick(PickDialog *state, int row, int step)
{
    int unit, column, at = -1;
    if (row < 0 || row >= state->rowCount) return;
    unit = state->rows[row];
    for (column = 0; column < state->columnCount; column++)
        if (state->columns[column] == state->from[unit]) at = column;
    for (column = at + step; column >= 0 && column < state->columnCount; column += step)
        if (state->sources[state->columns[column]].offered & kUnitMask[unit]) {
            state->from[unit] = state->columns[column];
            ShowPicks(state, row);
            return;
        }
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
        if (SUCCEEDED(LoadIconWithScaleDown(NULL, IDI_INFORMATION, GetSystemMetricsForDpi(SM_CXICON, GetDpiForWindow(dialog)),
                                            GetSystemMetricsForDpi(SM_CYICON, GetDpiForWindow(dialog)), &state->icon)))
            SendDlgItemMessageW(dialog, IDC_X_ICON, STM_SETICON, (WPARAM)state->icon, 0);
        Theme_SetGlyph(GetDlgItem(dialog, IDOK), 0xE73E, THEME_TINT_GREEN);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)TR(L"The newest of each"));
        for (i = 0; i < state->columnCount; i++)
            SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_ADDSTRING, 0, (LPARAM)state->sources[state->columns[i]].name);
        SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_SETCURSEL, 0, 0);
        FillPicks(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_X_NOTE);

    case WM_NOTIFY:
        if (state && state->list && ((const NMHDR *)lp)->hwndFrom == state->list) {
            const NMHDR *header = (const NMHDR *)lp;
            if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
                /* A click on a backup's cell restores the part from it. */
                LVHITTESTINFO hit;
                ZeroMemory(&hit, sizeof hit);
                hit.pt = ((const NMITEMACTIVATE *)lp)->ptAction;
                if (ListView_SubItemHitTest(state->list, &hit) >= 0 && hit.iItem >= 0 && hit.iItem < state->rowCount && hit.iSubItem > 0 &&
                    hit.iSubItem <= state->columnCount &&
                    (state->sources[state->columns[hit.iSubItem - 1]].offered & kUnitMask[state->rows[hit.iItem]])) {
                    state->from[state->rows[hit.iItem]] = state->columns[hit.iSubItem - 1];
                    ShowPicks(state, hit.iItem);
                }
                return TRUE;
            }
            if (header->code == LVN_KEYDOWN) {
                WORD key = ((const NMLVKEYDOWN *)lp)->wVKey;
                if (key == VK_LEFT || key == VK_RIGHT) {
                    StepPick(state, ListView_GetNextItem(state->list, -1, LVNI_FOCUSED), key == VK_LEFT ? -1 : 1);
                    SetWindowLongPtrW(dialog, DWLP_MSGRESULT, TRUE);
                }
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_X_PROFILE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                /* Every part from the newest backup holding it, or from the one chosen where it holds it. */
                LRESULT choice = SendDlgItemMessageW(dialog, IDC_X_PROFILE, CB_GETCURSEL, 0, 0);
                int row;
                for (row = 0; row < state->rowCount; row++) {
                    int unit = state->rows[row], s, newest = -1;
                    for (s = 0; s < state->columnCount; s++)
                        if (state->sources[state->columns[s]].offered & kUnitMask[unit]) newest = state->columns[s];
                    if (choice <= 0) state->from[unit] = newest;
                    else if (choice - 1 < state->columnCount && (state->sources[state->columns[choice - 1]].offered & kUnitMask[unit]))
                        state->from[unit] = state->columns[choice - 1];
                    ShowPicks(state, row);
                }
            }
            return TRUE;
        case IDOK:
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

static BOOL PreviewRestore(HWND owner, const ProfileList *list, int index, const RestoreSource *sources, int count, DWORD chosen, const int *from)
{
    enum { PREVIEW_CCH = 65536 };
    WCHAR *text = (WCHAR *)Alloc(PREVIEW_CCH * sizeof(WCHAR)), line[LONG_PATH_CCH + 512];
    WCHAR temporary[MAX_PATH] = L"", folder[MAX_PATH], error[512];
    SessionSet set;
    BOOL accepted = FALSE, loaded = FALSE, truncated = FALSE, ok = TRUE;
    int s, i;
    if (!text) return FALSE;
    StringCchCopyW(text, PREVIEW_CCH, TR(L"Backups are processed oldest first. Each source is compared with the current target; later sources may supersede earlier ones.\r\n"));
    if (chosen & BK_SIGNIN)
        StringCchCatW(text, PREVIEW_CCH, TR(L"Sign-in data will be replaced. Session comparisons below use the current account; the restored account may require initialization before sessions can be imported.\r\n"));
    if (list->items[index].syncGroup)
        StringCchCatW(text, PREVIEW_CCH, TR(L"This profile belongs to a sync group; later synchronization may propagate restored items.\r\n"));
    if (chosen & BK_CONVERSATIONS) {
        StringCchCatW(text, PREVIEW_CCH, TR(L"Session import can clear Claude deletion marks; entries already pending removal are kept out.\r\n"));
        loaded = SessionStore_LoadProfiles(&set, list);
        if (!loaded) ok = FALSE;
    }
    for (s = 0; s < count && ok; s++) {
        StringCchPrintfW(line, ARRAYSIZE(line), L"\r\n%s\r\n", sources[s].name);
        if (FAILED(StringCchCatW(text, PREVIEW_CCH, line))) truncated = TRUE;
        for (i = 0; i < Zip_Count(sources[s].zip) && ok; i++) {
            const char *name = Zip_Name(sources[s].zip, i);
            const WCHAR *action = NULL;
            WCHAR relative[LONG_PATH_CCH], target[LONG_PATH_CCH];
            if ((chosen & (BK_CONVERSATIONS | BK_COWORK)) && VaultTasks_IsCoverage(name)) {
                if (VaultTasks_Describe(sources[s].zip, name, line, ARRAYSIZE(line)) && wcslen(text) + wcslen(line) + 4096 < PREVIEW_CCH)
                    StringCchCatW(text, PREVIEW_CCH, line);
                else truncated = TRUE;
                continue;
            }
            if ((chosen & BK_CONVERSATIONS) && strcmp(name, SESSIONS_ARCHIVE) == 0) {
                WCHAR *sessions = NULL;
                ok = GetTempPathW(ARRAYSIZE(folder), folder) && GetTempFileNameW(folder, L"cdp", 0, temporary) &&
                     Zip_Extract(sources[s].zip, i, temporary);
                if (ok) sessions = SessionSync_ImportPreview(&set, temporary, 1u << index, error, ARRAYSIZE(error));
                if (!sessions) ok = FALSE;
                if (sessions && !truncated && wcslen(text) + wcslen(sessions) + 4096 < PREVIEW_CCH)
                    StringCchCatW(text, PREVIEW_CCH, sessions);
                else if (sessions) truncated = TRUE;
                Free(sessions);
                if (temporary[0]) DeleteFileW(temporary);
                temporary[0] = 0;
                continue;
            }
            if ((chosen & BK_COWORK) && SafeRelative(name, "cowork", relative, ARRAYSIZE(relative))) {
                if (FAILED(StringCchPrintfW(target, ARRAYSIZE(target), L"%s\\" COWORK_DIR L"\\%s", list->items[index].storageDir, relative))) { ok = FALSE; break; }
                action = Util_FileExists(target) ? TR(L"Replace existing file") : TR(L"Add file");
            } else if ((chosen & BK_CONVERSATIONS) && strcmp(name, "sessions/scheduled-tasks.json") == 0)
                action = TR(L"Merge scheduled tasks");
            else if (s == from[PICK_UNIT_SETTINGS] && (chosen & BK_APPCONFIG) && strncmp(name, "settings/", 9) == 0)
                action = TR(L"Apply selected app settings");
            else if (s == from[PICK_UNIT_SETTINGS] && (chosen & BK_LAYOUT) && strcmp(name, LAYOUT_NAME) == 0)
                action = TR(L"Merge selected sidebar and settings items");
            else if (s == from[PICK_UNIT_SETTINGS] && (chosen & SYNC_ITEM_APPEARANCE) && strcmp(name, APPEARANCE_NAME) == 0)
                action = TR(L"Apply appearance settings");
            else if (s == from[PICK_UNIT_ACCOUNT] && (chosen & BK_SIGNIN) && strncmp(name, "signin/", 7) == 0)
                action = TR(L"Replace sign-in state");
            if (action) {
                WCHAR wide[512];
                if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, ARRAYSIZE(wide))) StringCchCopyW(wide, ARRAYSIZE(wide), L"?");
                StringCchPrintfW(line, ARRAYSIZE(line), L"%s | %s\r\n", wide, action);
                if (!truncated && wcslen(text) + wcslen(line) + 1024 < PREVIEW_CCH) StringCchCatW(text, PREVIEW_CCH, line);
                else truncated = TRUE;
            }
        }
    }
    if (truncated) StringCchCatW(text, PREVIEW_CCH, TR(L"\r\nThe display limit was reached; more archive items will be processed."));
    if (ok) accepted = Ui_ReviewText(owner, TR(L"Restore preview"),
                                    TR(L"Review selected changes before restoring. Existing data is backed up first. This preview does not apply changes or validate all archive contents."),
                                    text, TR(L"Continue"));
    else Ui_Message(owner, MB_ICONERROR, TR(L"The archive or a target entry could not be read. Nothing was imported."));
    if (loaded) SessionStore_Free(&set);
    Free(text);
    return accepted;
}

/* The chosen content, in the order the target account must receive it.
 * Kept separate from the chooser and protective backup for fixture tests. */
static BOOL RestoreChosen(HWND owner, const ProfileList *list, int index, const RestoreSource *sources, int count, DWORD chosen,
                          const int *from, const WCHAR *backup, const WCHAR *title, RestoreResult *result)
{
    const Profile *p = &list->items[index];
    BOOL ok = TRUE;
    HCURSOR old;
    int s;
    size_t i;
    ZeroMemory(result, sizeof *result);
    if ((chosen & BK_SIGNIN) && from[PICK_UNIT_ACCOUNT] >= 0) {
        const RestoreSource *source = &sources[from[PICK_UNIT_ACCOUNT]];
        /* Establish the target account before resolving its session folder
         * or applying settings from the separately chosen backup. */
        for (i = 0; ok && i < ARRAYSIZE(kSigninFolders); i++) ok = RemoveFolder(p->storageDir, kSigninFolders[i], &result->code);
        if (ok) ok = ExtractTree(source->zip, "signin", p->storageDir, &result->code);
        old = SetCursor(LoadCursorW(NULL, IDC_ARROW));
        if (ok) result->keyNote = OpenSignInKey(owner, source->zip, p, title, (source->offered & BK_KEY) != 0);
        SetCursor(old);
        if (!ok) return FALSE;
    }
    /* Sessions and Cowork from every backup, the oldest first: what several hold, as the newest has it. */
    for (s = 0; s < count; s++) {
        int found = 0;
        if ((chosen & BK_CONVERSATIONS) && (sources[s].offered & BK_CONVERSATIONS)) {
            ok = RestoreSessions(sources[s].zip, list, index, backup, &found, &result->signedOut, result->error, ARRAYSIZE(result->error)) && ok;
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
                if ((filtered = SessionSync_FilterLayout(layout, rawLength, chosen & BK_LAYOUT, &filteredLength)) != NULL && filteredLength > 2 &&
                    !SessionSync_RestoreLayout(p, dir, filtered, filteredLength, NULL))
                    ok = FALSE;
                Free(filtered);
                Free(layout);
            }
        }
    }
    return ok;
}

BOOL Backup_Restore(HWND owner, const ClaudePackage *pkg, const ProfileList *list, int index)
{
    const Profile *p = &list->items[index];
    WCHAR title[LABEL_CCH + 64], text[LABEL_CCH + 256], backup[LONG_PATH_CCH];
    WCHAR report[4 * LONG_PATH_CCH], line[LONG_PATH_CCH + 256];
    RestoreSource *sources;
    BackupDialog dialog;
    DWORD code = 0;
    HCURSOR old;
    int s, count, from[PICK_UNITS];
    size_t i;
    BOOL ok = FALSE;
    RestoreResult result;
    (void)pkg;
    if (!p->storageDir[0]) return FALSE;
    if (SessionLink_Busy(p)) {   /* said before the choices, checked again after them */
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
        return FALSE;
    }
    StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Restore \x201C%s\x201D from a backup"), p->name);
    if ((sources = (RestoreSource *)Alloc(RESTORE_SOURCES * sizeof *sources)) == NULL) return FALSE;
    if ((count = ChooseBackups(owner, title, p->name, sources)) == 0) goto done;
    for (s = 0; s < count; s++)
        if (!OpenSource(&sources[s])) {
            if (count == 1) Ui_Message(owner, MB_ICONWARNING, TR(L"This file is not a backup of " APP_NAME L"."));
            else Ui_Message(owner, MB_ICONWARNING, TR(L"\x201C%s\x201D is not a backup of " APP_NAME L"."), sources[s].name);
            goto done;
        }
    SortSources(sources, count);
    ZeroMemory(&dialog, sizeof dialog);
    if (count == 1)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D. What it replaces is kept in a backup first."), p->name);
    else
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Choose what to restore into \x201C%s\x201D from these %d backups. What it replaces is kept in a backup first."),
                         p->name, count);
    dialog.title = title;
    dialog.text = text;
    dialog.action = TR(L"Restore");
    dialog.sessions = -1;
    for (s = 0; s < count; s++) dialog.offered |= sources[s].offered;
    dialog.offered &= ~BK_KEY;   /* not a choice: it opens the sign-in when this PC cannot */
    dialog.chosen = dialog.offered & ~(BK_SIGNIN | BK_KEY);
    if (Ui_Dialog(owner, IDD_BACKUP, BackupProc, (LPARAM)&dialog) != IDOK || !ChooseSources(owner, sources, count, dialog.chosen, from) ||
        !PreviewRestore(owner, list, index, sources, count, dialog.chosen, from) || !ConfirmRestore(owner, p, sources, count))
        goto done;
    if (SessionLink_Busy(p)) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
        goto done;
    }
    /* A complete outer/inner archive pass finishes before even creating the
     * target's safety-backup folder. All sources stay held open by ZipIn. */
    for (s = 0; s < count; s++) if (!Backup_CheckArchive(owner, sources[s].path)) goto done;
    if (SessionLink_Busy(p)) {
        Ui_Message(owner, MB_ICONINFORMATION, TR(L"Close \x201C%s\x201D first, and every profile that shares its session folder."), p->name);
        goto done;
    }
    old = SetCursor(LoadCursorW(NULL, IDC_WAIT));
    ok = RestoreBackupDir(p, backup, ARRAYSIZE(backup));
    /* What the restore replaces, kept first. */
    if (!ok) code = GetLastError();
    if (ok && (dialog.chosen & BK_COWORK)) ok = KeepCopy(p->storageDir, COWORK_DIR, backup, TRUE, &code);
    if (ok && (dialog.chosen & (BK_APPCONFIG | BK_LAYOUT))) {
        /* The settings side touches config.json, Preferences and the app files. */
        for (i = 0; ok && i < ARRAYSIZE(kSettingsFiles); i++) ok = KeepCopy(p->storageDir, kSettingsFiles[i], backup, FALSE, &code);
        ok = ok && KeepCopy(p->storageDir, CLAUDE_APP_SETTINGS, backup, FALSE, &code);
        ok = ok && KeepCopy(p->storageDir, CLAUDE_DESKTOP_SETTINGS, backup, FALSE, &code);
        ok = ok && KeepCopy(p->storageDir, L"Preferences", backup, FALSE, &code);
    }
    if (ok && (dialog.chosen & BK_SIGNIN)) {
        for (i = 0; ok && i < ARRAYSIZE(kSigninFiles); i++) ok = KeepCopy(p->storageDir, kSigninFiles[i], backup, FALSE, &code);
        for (i = 0; ok && i < ARRAYSIZE(kSigninFolders); i++) ok = KeepCopy(p->storageDir, kSigninFolders[i], backup, TRUE, &code);
    }
    if (!ok) {
        SetCursor(old);
        Ui_Message(owner, MB_ICONERROR, TR(L"What the restore would replace could not be backed up (error %lu): nothing was changed."), code);
        goto done;
    }
    ok = RestoreChosen(owner, list, index, sources, count, dialog.chosen, from, backup, title, &result);
    code = result.code;
    OperationHistory_Record(OPERATION_RESTORE, p->folder, ok ? ERROR_SUCCESS : code ? code : ERROR_GEN_FAILURE, backup);
    SetCursor(old);
    for (s = 0; s < count; s++) Util_Log(L"restored %s from %s%s", p->folder, sources[s].path, ok ? L"" : L" (partly)");
    report[0] = 0;
    if (ok) StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Restored into \x201C%s\x201D."), p->name);
    else if (result.error[0]) StringCchCopyW(report, ARRAYSIZE(report), result.error);
    else StringCchPrintfW(report, ARRAYSIZE(report), TR(L"Some files could not be restored (error %lu)."), code);
    if ((dialog.chosen & BK_CONVERSATIONS) && !result.signedOut) {
        if (count == 1) StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Sessions in the archive: %d"), result.sessions);
        else StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Sessions in the backups: %d"), result.sessions);
        StringCchCatW(report, ARRAYSIZE(report), L"\n");
        StringCchCatW(report, ARRAYSIZE(report), line);
    }
    if (result.signedOut) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"The Code sessions were left out: \x201C%s\x201D is not signed in to Claude yet."), p->name);
        StringCchCatW(report, ARRAYSIZE(report), L"\n");
        StringCchCatW(report, ARRAYSIZE(report), line);
    }
    if (result.keyNote) {
        StringCchCatW(report, ARRAYSIZE(report), L"\n");
        StringCchCatW(report, ARRAYSIZE(report), result.keyNote);
    }
    StringCchCatW(report, ARRAYSIZE(report), L"\n");
    StringCchCatW(report, ARRAYSIZE(report), TR(L"What was replaced or removed is kept in:"));
    StringCchCatW(report, ARRAYSIZE(report), L"\n");
    StringCchCatW(report, ARRAYSIZE(report), backup);
    Ui_Message(owner, ok ? MB_ICONINFORMATION : MB_ICONWARNING, L"%s", report);
done:
    for (s = 0; s < RESTORE_SOURCES; s++) Zip_Free(sources[s].zip);
    Free(sources);
    return ok;
}

typedef struct ArchiveCheck {
    HWND dialog;
    const WCHAR *path;
    HANDLE cancel, thread;
    BOOL finished, ok, automatic;
    int files;
    WCHAR error[1024];
} ArchiveCheck;

#define WM_ARCHIVE_CHECKED (WM_APP + 81)

static DWORD WINAPI CheckArchiveThread(void *context)
{
    ArchiveCheck *state = (ArchiveCheck *)context;
    state->ok = Zip_Validate(state->path, state->cancel, &state->files, state->error, ARRAYSIZE(state->error));
    PostMessageW(state->dialog, WM_ARCHIVE_CHECKED, 0, 0);
    return 0;
}

static INT_PTR CALLBACK CheckArchiveProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    ArchiveCheck *state = (ArchiveCheck *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        state = (ArchiveCheck *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        state->dialog = dialog;
        SetWindowTextW(dialog, TR(L"Validate archive"));
        SetDlgItemTextW(dialog, IDC_T_NOTE, TR(L"Offline format, item CRC and nested session archive checks. This does not verify authenticity or account validity."));
        SetDlgItemTextW(dialog, IDC_T_TEXT, state->path);
        SetDlgItemTextW(dialog, IDOK, TR(L"Close"));
        ShowWindow(GetDlgItem(dialog, IDC_T_COPY), SW_HIDE);
        EnableWindow(GetDlgItem(dialog, IDOK), FALSE);
        state->thread = CreateThread(NULL, 0, CheckArchiveThread, state, 0, NULL);
        if (!state->thread) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_ARCHIVE_CHECKED) {
        WCHAR text[2048];
        state->finished = TRUE;
        if (state->automatic && WaitForSingleObject(state->cancel, 0) == WAIT_OBJECT_0) { state->ok = FALSE; EndDialog(dialog, IDCANCEL); return TRUE; }
        if (state->automatic && state->ok) { EndDialog(dialog, IDOK); return TRUE; }
        if (state->ok) StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Validation passed: %d archive items checked."), state->files);
        else StringCchCopyW(text, ARRAYSIZE(text), state->error);
        SetDlgItemTextW(dialog, IDC_T_TEXT, text);
        EnableWindow(GetDlgItem(dialog, IDOK), TRUE);
        ShowWindow(GetDlgItem(dialog, IDCANCEL), SW_HIDE);
        return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK))) {
        if (state->finished) EndDialog(dialog, IDOK);
        else {
            SetEvent(state->cancel);
            SetDlgItemTextW(dialog, IDC_T_TEXT, TR(L"Cancelling after the current archive item"));
        }
        return TRUE;
    }
    return FALSE;
}

BOOL Backup_CheckArchive(HWND owner, const WCHAR *path)
{
    ArchiveCheck state;
    BOOL ok;
    ZeroMemory(&state, sizeof state); state.path = path; state.automatic = TRUE;
    state.cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!state.cancel) return FALSE;
    Ui_Dialog(owner, IDD_TEXTVIEW, CheckArchiveProc, (LPARAM)&state);
    if (state.thread) { WaitForSingleObject(state.thread, INFINITE); CloseHandle(state.thread); }
    ok = state.finished && state.ok && WaitForSingleObject(state.cancel, 0) != WAIT_OBJECT_0;
    CloseHandle(state.cancel);
    return ok;
}

void Backup_ValidateArchive(HWND owner, const WCHAR *path)
{
    RestoreSource *sources = NULL;
    int i, count = 1;
    if (!path) {
        sources = (RestoreSource *)Alloc(RESTORE_SOURCES * sizeof *sources);
        if (!sources) return;
        count = ChooseBackups(owner, TR(L"Validate archive"), L"", sources);
    }
    for (i = 0; i < count; i++) {
        ArchiveCheck state;
        ZeroMemory(&state, sizeof state);
        state.path = path ? path : sources[i].path;
        state.cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!state.cancel) break;
        Ui_Dialog(owner, IDD_TEXTVIEW, CheckArchiveProc, (LPARAM)&state);
        if (state.thread) { WaitForSingleObject(state.thread, INFINITE); CloseHandle(state.thread); }
        CloseHandle(state.cancel);
    }
    Free(sources);
}
