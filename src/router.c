/*
 * --launch and --url.
 *
 * With several profiles, the user picks the one that opens a claude:// link
 * (IDD_LINK); the one the link most likely belongs to is selected. A sign-in
 * started in one Claude window finishes in the browser, which hands a
 * claude:// link back to Windows. Claude only accepts that link in the window
 * that opened the browser; any other window logs "Google sign-in code does not
 * answer a sign-in this app started; ignoring". So for a sign-in link the
 * window whose main.log most recently says "[Auth] Using system browser for:"
 * is selected; for any other link, the Claude window used last.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <wchar.h>

#define REDACTED_URL_CCH 160   /* the start of a link, enough to tell it in the log */
#define LINK_ICON_DIPS   24    /* a profile's icon in the list, as in the manager's */
#define WM_APP_KEEP_CHOICE (WM_APP + 1)   /* the list's selection changed: one row stays chosen */

/* The link without its query or fragment, which can hold a sign-in code. */
static void Redact(const WCHAR *url, WCHAR *out, size_t cch)
{
    const WCHAR *cut = wcspbrk(url, L"?#");
    if (cut)
        StringCchPrintfW(out, cch, L"%.*s%c...", (int)(cut - url), url, *cut);
    else
        StringCchCopyW(out, cch, url);
}

static const WCHAR *RouteReasonText(RouteReason reason)
{
    switch (reason) {
    case ROUTE_NOTHING_RUNNING: return L"no Claude window open: default profile";
    case ROUTE_ONLY_ONE:        return L"only open window";
    case ROUTE_SIGNIN:          return L"window that started the sign-in";
    case ROUTE_LAST_USED:       return L"last used window";
    case ROUTE_DEFAULT:         return L"default profile";
    default:                    return L"first open window";
    }
}

/* ------------------------------------------------------- the link dialog */

typedef struct LinkChoice {
    const ClaudePackage *pkg;
    const ProfileList   *list;
    const WCHAR         *shown;     /* the link without its query or fragment */
    BOOL                 signIn;
    int                  starter;   /* the profile whose window started the sign-in; -1: not known */
    int                  chosen;    /* selected at first, then the last row selected; once closed with OK, the profile chosen */
    HWND                 rows;      /* one per profile, in the list's order (in the view it scrolls in, which has its id) */
} LinkChoice;

/* Every profile with its icon, the open ones said so; the one suggested selected. */
static void FillLinkRows(HWND dialog, LinkChoice *choice)
{
    int pixels = MulDiv(LINK_ICON_DIPS, (int)GetDpiForWindow(dialog), 96), i;
    HIMAGELIST images = ImageList_Create(pixels, pixels, ILC_COLOR32, choice->list->count, 1);
    WCHAR text[LABEL_CCH + 64];
    LVCOLUMNW column;
    LVITEMW item;
    ListView_SetExtendedListViewStyle(choice->rows, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(choice->rows, 0, &column);   /* the view it scrolls in gives it the list's width */
    if (images) ListView_SetImageList(choice->rows, images, LVSIL_SMALL);   /* the list destroys it with itself */
    for (i = 0; i < choice->list->count; i++) {
        const Profile *profile = &choice->list->items[i];
        HICON icon = images ? Icons_Create(choice->pkg, profile, pixels) : NULL;
        if (images && !icon) icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, pixels, pixels, 0);
        ZeroMemory(&item, sizeof item);
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.iImage = icon ? ImageList_AddIcon(images, icon) : I_IMAGENONE;
        if (item.iImage < 0) item.iImage = I_IMAGENONE;
        if (icon) DestroyIcon(icon);
        if (profile->running) StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%s   (open now)"), profile->name);
        else StringCchCopyW(text, ARRAYSIZE(text), profile->name);
        item.pszText = text;
        ListView_InsertItem(choice->rows, &item);
    }
    ListView_SetItemState(choice->rows, choice->chosen, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Theme_SmoothView(choice->rows);
    ListView_EnsureVisible(choice->rows, choice->chosen, FALSE);
}

static INT_PTR CALLBACK LinkProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    LinkChoice *choice = (LinkChoice *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        WCHAR text[256 + LABEL_CCH];
        choice = (LinkChoice *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        if (!choice->signIn)
            StringCchCopyW(text, ARRAYSIZE(text), TR(L"Which profile opens this link?"));
        else if (choice->starter >= 0)
            StringCchPrintfW(text, ARRAYSIZE(text), TR(L"This sign-in was started in \x201C%s\x201D: it finishes only there."),
                             choice->list->items[choice->starter].name);
        else
            StringCchCopyW(text, ARRAYSIZE(text), TR(L"A sign-in finishes only in the window that started it: choose that profile."));
        SetDlgItemTextW(dialog, IDC_L_TEXT, text);
        SetDlgItemTextW(dialog, IDC_L_LINK, choice->shown);
        choice->rows = GetDlgItem(dialog, IDC_L_LIST);
        FillLinkRows(dialog, choice);
        /* Started by the browser, which hands the foreground on with the link. */
        SetForegroundWindow(dialog);
        SetFocus(choice->rows);
        return FALSE;   /* the focus is set */
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_L_LINK);

    case WM_NOTIFY:
        if (choice && ((const NMHDR *)lp)->hwndFrom == choice->rows) {
            const NMHDR *header = (const NMHDR *)lp;
            if (header->code == NM_DBLCLK) {
                if (((const NMITEMACTIVATE *)lp)->iItem >= 0) PostMessageW(dialog, WM_COMMAND, IDOK, 0);
                return TRUE;
            }
            if (header->code == LVN_ITEMCHANGED) {
                /* Checked once the list is done: a click on another row
                 * clears the selection before it sets the new one. */
                PostMessageW(dialog, WM_APP_KEEP_CHOICE, 0, 0);
                return TRUE;
            }
        }
        break;

    /* A profile is always chosen: a click beside the rows or Ctrl+Space (the
     * input language switch of some layouts, typed while the dialog took the
     * focus) would clear the selection and leave Open with nothing to open. */
    case WM_APP_KEEP_CHOICE:
        if (choice) {
            int row = ListView_GetNextItem(choice->rows, -1, LVNI_SELECTED);
            if (row >= 0 && row < choice->list->count) choice->chosen = row;
            else ListView_SetItemState(choice->rows, choice->chosen, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        return TRUE;

    case WM_COMMAND:
        if (!choice) break;
        switch (LOWORD(wp)) {
        case IDOK: {
            int row = ListView_GetNextItem(choice->rows, -1, LVNI_SELECTED);
            if (row >= 0 && row < choice->list->count) choice->chosen = row;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* The profile the user chose for the link, `suggested` selected at first; -1
 * when none was. A dialog that cannot be shown leaves the suggestion. */
static int ChooseProfile(const ClaudePackage *pkg, const ProfileList *list, int suggested, BOOL signIn, int starter,
                         const WCHAR *shown)
{
    LinkChoice choice;
    INT_PTR result;
    ZeroMemory(&choice, sizeof choice);
    choice.pkg = pkg;
    choice.list = list;
    choice.shown = shown;
    choice.signIn = signIn;
    choice.starter = starter;
    choice.chosen = suggested;
    result = Ui_Dialog(NULL, IDD_LINK, LinkProc, (LPARAM)&choice);
    if (result == -1) return suggested;
    return result == IDOK ? choice.chosen : -1;
}

/* The manager, from this same program. */
static void OpenManager(void)
{
    WCHAR exe[MAX_PATH];
    if (!Util_SelfExe(exe, ARRAYSIZE(exe)) || !Util_Spawn(exe, L"", NULL))
        Util_Log(L"could not open the manager (error %lu)", GetLastError());
}

static void ReportMissingClaude(void)
{
    Util_Log(L"Claude Desktop is not installed");
    if (Ui_Message(NULL, MB_ICONWARNING | MB_YESNO, TR(L"Claude Desktop is not installed.\n\nOpen the download page?")) == IDYES)
        Util_OpenUrl(APP_DOWNLOAD_URL);
}

/* A profile about to open whose sessions are kept the same as others': one of
 * those still open gets the latest of them only once it closes, so opening
 * it alone (no link) offers to quit it first. Then the lists are made the
 * same (sessionvault.c), or the profile's own list kept. */
static void KeepBeforeOpen(const ClaudePackage *pkg, const Profile *p, const WCHAR *url)
{
    ProfileList list;
    WCHAR text[1024];
    DWORD error = 0;
    int index, i;
    Profiles_Load(&list, pkg);
    if ((index = Profiles_Find(&list, p->folder)) < 0) return;
    for (i = 0; !url && list.items[index].syncSessions && i < list.count; i++) {
        if (i == index || !list.items[i].syncSessions || !Claude_IsRunning(&list.items[i])) continue;
        StringCchPrintfW(text, ARRAYSIZE(text),
                         TR(L"\x201C%s\x201D is open: the sessions of \x201C%s\x201D are made the same as its own only once it closes.\n\nQuit it first?"),
                         list.items[i].name, list.items[index].name);
        if (!Ui_Ask(NULL, IDI_QUESTION, text, TR(L"Quit it"), TR(L"Open anyway"), FALSE)) break;
        if (!Claude_Quit(&list.items[i], &error))
            Ui_Message(NULL, MB_ICONWARNING, TR(L"Claude for \x201C%s\x201D could not be closed (error %lu)."), list.items[i].name, error);
    }
    SessionVault_BeforeOpen(&list, index);
}

/* Opens a profile, with a claude:// link or without, and watches it. Whether
 * its Claude runs is looked at again: the caller's profile can be older, and
 * one quit a moment ago must start with a watcher of its own. A profile about
 * to start first gets the session changes waiting for it (sessionedit.c) and
 * its list of sessions (KeepBeforeOpen): its Claude reads its sessions as it
 * starts. */
HRESULT Launcher_Open(const ClaudePackage *pkg, const Profile *p, const WCHAR *url, DWORD *pid, BOOL *identity)
{
    Profile watched = *p;
    DWORD launched = 0;
    HRESULT hr;
    Claude_RefreshRunning(&watched);
    if (!watched.running) {
        SessionEdit_ApplyPending(NULL, p);
        KeepBeforeOpen(pkg, p, url);
    }
    hr = Claude_Launch(pkg, p, url, &launched, identity);
    if (pid) *pid = launched;
    if (SUCCEEDED(hr)) {
        if (!watched.running) watched.pid = launched;   /* the watcher waits for this process only */
        if (!watched.running || !Taskbar_IsWatched(&watched)) Taskbar_Watch(&watched);
    }
    return hr;
}

int Launcher_Run(const WCHAR *folder)
{
    ProfileList list;
    ClaudePackage pkg;
    BOOL identity = FALSE, havePackage = Claude_FindPackage(&pkg);
    DWORD pid = 0;
    HRESULT hr;
    int i;

    Profiles_Load(&list, &pkg);
    i = Profiles_Find(&list, folder);
    if (i < 0) {
        /* Its name went with it: the folder, without the prefix, is the name it had by default. */
        const WCHAR *name = Core_IsProfileFolder(folder) ? folder + wcslen(PROFILE_PREFIX) : folder;
        Util_Log(L"shortcut for %s, which no longer exists", folder);
        if (Ui_Message(NULL, MB_ICONWARNING | MB_YESNO,
                       TR(L"This shortcut opens the Claude profile \x201C%s\x201D, which no longer exists.\n\nOpen " APP_NAME L"?"),
                       name) == IDYES)
            OpenManager();
        return 1;
    }
    if (!havePackage) {
        ReportMissingClaude();
        return 1;
    }
    hr = Launcher_Open(&pkg, &list.items[i], NULL, &pid, &identity);
    if (FAILED(hr)) {
        Util_Log(L"could not open %s (0x%08lX)", list.items[i].folder, (unsigned long)hr);
        Ui_Message(NULL, MB_ICONERROR, TR(L"Claude could not be started (error 0x%08lX)."), (unsigned long)hr);
        return 1;
    }
    Util_Log(L"opened %s (pid %lu)%s", list.items[i].folder, pid, identity ? L"" : L" without package identity");
    return 0;
}

int Router_Run(const WCHAR *rawUrl)
{
    WCHAR url[URL_CCH], redactedUrl[REDACTED_URL_CCH];
    ProfileList list;
    ClaudePackage pkg;
    ULONGLONG signIn[MAX_PROFILES];
    int running[MAX_PROFILES];
    int runningCount = 0, i, topmostIndex, defaultIndex, topmostPosition = -1, defaultPosition = -1, suggested, target;
    WCHAR how[FOLDER_CCH + 96];
    BOOL signInUrl, identity = FALSE;
    HRESULT hr;
    RouteReason reason;

    if (!Core_SanitizeUrl(rawUrl, url, ARRAYSIZE(url))) {
        Util_Log(L"ignored a link that is not a claude:// link");
        return 1;
    }
    Redact(url, redactedUrl, ARRAYSIZE(redactedUrl));
    if (!Claude_FindPackage(&pkg)) {
        ReportMissingClaude();
        return 1;
    }

    Profiles_Load(&list, &pkg);
    signInUrl = Core_IsSignInUrl(url);
    topmostIndex = Claude_TopmostProfile(&list);
    defaultIndex = Profiles_DefaultIndex(&list);
    if (defaultIndex < 0) {
        Util_Log(L"link %s: no profile found", redactedUrl);
        return 1;
    }
    for (i = 0; i < list.count; i++) {
        if (!list.items[i].running) continue;
        if (i == topmostIndex) topmostPosition = runningCount;
        if (i == defaultIndex) defaultPosition = runningCount;
        running[runningCount++] = i;
    }
    /* Reading the logs matters only when the dialog shows (it names the window
     * that started the sign-in) or when several windows could claim the link. */
    for (i = 0; i < runningCount; i++) {
        signIn[i] = 0;
        if (signInUrl && (list.count > 1 || runningCount > 1)) Claude_LastSignInStart(&pkg, &list.items[running[i]], &signIn[i]);
    }

    suggested = Core_SuggestTarget(runningCount, signIn, Util_LocalNowTicks(), SIGNIN_MAX_AGE_MINUTES * 60 * TICKS_PER_SECOND,
                                   signInUrl, topmostPosition, defaultPosition, &reason);
    target = suggested >= 0 ? running[suggested] : defaultIndex;
    StringCchCopyW(how, ARRAYSIZE(how), RouteReasonText(reason));
    if (list.count > 1) {
        int chosen = ChooseProfile(&pkg, &list, target, signInUrl, reason == ROUTE_SIGNIN ? target : -1, redactedUrl);
        if (chosen < 0) {
            Util_Log(L"link %s: no profile chosen, not opened (suggested %s: %s)", redactedUrl, list.items[target].folder,
                     RouteReasonText(reason));
            return 1;
        }
        if (chosen == target) StringCchPrintfW(how, ARRAYSIZE(how), L"chosen as suggested: %s", RouteReasonText(reason));
        else StringCchPrintfW(how, ARRAYSIZE(how), L"chosen; suggested %s: %s", list.items[target].folder, RouteReasonText(reason));
        target = chosen;
    }
    hr = Launcher_Open(&pkg, &list.items[target], url, NULL, &identity);
    if (SUCCEEDED(hr)) {
        Util_Log(L"link %s -> %s (%s)%s", redactedUrl, list.items[target].folder, how, identity ? L"" : L" without package identity");
        return 0;
    }
    Util_Log(L"link %s -> %s (%s) FAILED (0x%08lX)", redactedUrl, list.items[target].folder, how, (unsigned long)hr);
    Ui_Message(NULL, MB_ICONERROR, TR(L"Claude could not be started (error 0x%08lX)."), (unsigned long)hr);
    return 1;
}
