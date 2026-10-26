/*
 * A clean browser session for manual sign-up or sign-in. Claude itself owns
 * Desktop authorization. In the program's own window (webview.c, when
 * WebView2 is there, the first choice) the claude:// link it ends with goes
 * to this profile at once, and the window's folder goes once it closes; in
 * Edge, Chrome, Brave or Firefox the claude:// router completes it, and
 * their folders stay until the person removes them. The one chosen first is
 * Settings' (SETTING_SIGNIN_BROWSER).
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <wchar.h>

#define PRIVATE_BROWSER_ARGS_CCH (URL_CCH + MAX_PATH + 160)

typedef struct SignInDialog {
    const Profile *profile;
    SignInBrowsers browsers;
    WCHAR root[MAX_PATH];
} SignInDialog;

/* Each browser kind: its name, its file, where it installs itself, and how it opens a private window. */
static const struct { const WCHAR *name, *file, *relative; int family; } kBrowsers[SIGNIN_BROWSERS] = {
    { L"A window of its own (WebView2)", NULL, NULL, 0 },
    { L"Microsoft Edge", L"msedge.exe", L"Microsoft\\Edge\\Application\\msedge.exe", CORE_BROWSER_EDGE },
    { L"Google Chrome", L"chrome.exe", L"Google\\Chrome\\Application\\chrome.exe", CORE_BROWSER_CHROMIUM },
    { L"Brave", L"brave.exe", L"BraveSoftware\\Brave-Browser\\Application\\brave.exe", CORE_BROWSER_CHROMIUM },
    { L"Mozilla Firefox", L"firefox.exe", L"Mozilla Firefox\\firefox.exe", CORE_BROWSER_FIREFOX },
};

#define WEB_FOLDER_PREFIX L"web-"   /* the own window's folders: deleted once it closes, or at the next sign-in */

static BOOL BrowserFile(const WCHAR *path, const WCHAR *file)
{
    const WCHAR *leaf = wcsrchr(path, L'\\');
    size_t n = wcslen(path);
    return n > 3 && path[1] == L':' && path[2] == L'\\' && !wcschr(path, L'"') && leaf &&
           Core_EqualsI(leaf + 1, file) && Util_FileExists(path);
}

/* App Paths is read only; no default-app association is consulted or changed. */
static BOOL RegisteredBrowser(HKEY hive, const WCHAR *file, WCHAR *out, size_t cch)
{
    WCHAR key[MAX_PATH];
    if (FAILED(StringCchPrintfW(key, ARRAYSIZE(key), L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s", file))) return FALSE;
    return Util_RegGetString(hive, key, NULL, out, cch) && BrowserFile(out, file);
}

static BOOL BrowserAt(const WCHAR *environment, const WCHAR *relative, const WCHAR *file, WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(environment, base, ARRAYSIZE(base));
    return n && n < ARRAYSIZE(base) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", base, relative)) && BrowserFile(out, file);
}

static BOOL FindBrowser(int kind, WCHAR *out, size_t cch)
{
    const WCHAR *file = kBrowsers[kind].file, *relative = kBrowsers[kind].relative;
    return RegisteredBrowser(HKEY_CURRENT_USER, file, out, cch) || RegisteredBrowser(HKEY_LOCAL_MACHINE, file, out, cch) ||
           BrowserAt(L"ProgramFiles(x86)", relative, file, out, cch) || BrowserAt(L"ProgramFiles", relative, file, out, cch) ||
           BrowserAt(L"LOCALAPPDATA", relative, file, out, cch);
}

void SignIn_FindBrowsers(SignInBrowsers *found)
{
    int kind, size = GetSystemMetrics(SM_CXICON);
    ZeroMemory(found, sizeof *found);
    for (kind = 0; kind < SIGNIN_BROWSERS; kind++) {
        WCHAR *exe = found->exe[found->count];
        HICON icon = NULL;
        if (kind == SIGNIN_BROWSER_OWN) {
            if (!WebSignIn_Available()) continue;
            icon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, size, size, 0);
        } else {
            if (!FindBrowser(kind, exe, MAX_PATH)) continue;
            ExtractIconExW(exe, 0, &icon, NULL, 1);   /* the browser's own icon, as its shortcuts show it */
        }
        found->kind[found->count] = kind;
        found->icon[found->count++] = icon;
    }
}

void SignIn_FreeBrowsers(SignInBrowsers *found)
{
    int i;
    for (i = 0; i < found->count; i++)
        if (found->icon[i]) DestroyIcon(found->icon[i]);
    ZeroMemory(found, sizeof *found);
}

const WCHAR *SignIn_BrowserName(int kind)
{
    const WCHAR *name = kBrowsers[kind >= 0 && kind < SIGNIN_BROWSERS ? kind : SIGNIN_BROWSER_OWN].name;
    return kind == SIGNIN_BROWSER_OWN || kind < 0 || kind >= SIGNIN_BROWSERS ? TR(name) : name;
}

void SignIn_FillBrowsers(HWND combo, const SignInBrowsers *found, int kind)
{
    int i, chosen = 0;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < found->count; i++) {
        LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)SignIn_BrowserName(found->kind[i]));
        if (index >= 0 && found->icon[i]) SendMessageW(combo, CB_SETITEMDATA, (WPARAM)index, (LPARAM)(THEME_CHOICE_ICON | i));
        if (found->kind[i] == kind) chosen = i;
    }
    Theme_SetChoiceIcons(combo, found->icon, found->count);
    if (found->count) SendMessageW(combo, CB_SETCURSEL, (WPARAM)chosen, 0);
}

static void FillBrowsers(HWND dialog, SignInDialog *state)
{
    SignIn_FindBrowsers(&state->browsers);
    SignIn_FillBrowsers(GetDlgItem(dialog, IDC_SIGNIN_BROWSER), &state->browsers,
                        (int)Util_GetSetting(SETTING_SIGNIN_BROWSER, SIGNIN_BROWSER_OWN));
    if (!state->browsers.count)
        SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS,
                        TR(L"Microsoft Edge or Google Chrome was not found. Install one to use private sign-in."));
    EnableWindow(GetDlgItem(dialog, IDC_SIGNIN_OPEN), state->browsers.count != 0);
}

/* Outside the state tree: uninstall removes that tree while a browser may
 * still be using these directories. Only the user removes them after closing it. */
static BOOL BrowserRoot(WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH];
    if (Util_StateDir(base, ARRAYSIZE(base)) &&
        SUCCEEDED(StringCchPrintfW(out, cch, L"%.*s-private-sign-in", (int)Core_TrimmedPathLength(base), base))) return TRUE;
    if (cch) out[0] = 0;
    SetLastError(ERROR_FILENAME_EXCED_RANGE);
    return FALSE;
}

static BOOL NewBrowserFolder(WCHAR *root, size_t rootCch, BOOL own, WCHAR *folder, size_t cch)
{
    WCHAR id[40];
    GUID guid;
    if (!BrowserRoot(root, rootCch) || !Util_EnsureDir(root)) return FALSE;
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(&guid, id, ARRAYSIZE(id)) ||
        FAILED(StringCchPrintfW(folder, cch, L"%s\\%s%s", root, own ? WEB_FOLDER_PREFIX : L"", id))) {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    /* CREATE_NEW semantics: an existing directory is never reused. */
    return CreateDirectoryW(folder, NULL);
}

/* The own window's folders a closed window left (its browser still held
 * them): no window of ours uses them any more. */
static void RemoveOldWebFolders(const WCHAR *root)
{
    WCHAR pattern[MAX_PATH], folder[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    DWORD error;
    if (!root[0] || FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\" WEB_FOLDER_PREFIX L"*", root)) ||
        (find = FindFirstFileW(pattern, &found)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            SUCCEEDED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\%s", root, found.cFileName)))
            Util_DeleteTree(folder, &error);
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

/* `url` opened for `profile` in browser `chosen` of `browsers`, in a folder
 * of its own: the program's window, or a private window of a browser
 * installed. FALSE once `owner` was told why not. */
static BOOL Launch(HWND owner, const Profile *profile, const SignInBrowsers *browsers, int chosen, const WCHAR *url)
{
    WCHAR root[MAX_PATH], folder[MAX_PATH], args[PRIVATE_BROWSER_ARGS_CCH];
    BOOL own = browsers->kind[chosen] == SIGNIN_BROWSER_OWN, built;
    DWORD error;
    HRESULT hr;
    if (!NewBrowserFolder(root, ARRAYSIZE(root), own, folder, ARRAYSIZE(folder))) {
        error = GetLastError();
        Ui_Message(owner, MB_ICONERROR, TR(L"The private browser folder could not be created (error %lu)."), error);
        return FALSE;
    }
    if (own) {
        if (FAILED(hr = WebSignIn_Open(owner, profile, url, folder))) {
            Util_DeleteTree(folder, &error);
            Ui_Message(owner, MB_ICONERROR, TR(L"The sign-in window could not be opened (error 0x%08lX). Choose Microsoft Edge or Google Chrome."),
                       (unsigned long)hr);
            return FALSE;
        }
        return TRUE;
    }
    built = Core_PrivateBrowserArgs(kBrowsers[browsers->kind[chosen]].family, folder, url, args, ARRAYSIZE(args));
    if (!built || !Util_Spawn(browsers->exe[chosen], args, NULL)) {
        error = built ? GetLastError() : ERROR_INVALID_PARAMETER;
        SecureZeroMemory(args, sizeof args);
        Ui_Message(owner, MB_ICONERROR, TR(L"The private browser could not be started (error %lu)."), error);
        return FALSE;
    }
    /* Chromium may hand off to child processes, so its launcher's exit is not
     * proof that this folder is unused. Never delete it or stop its processes. */
    SecureZeroMemory(args, sizeof args);
    return TRUE;
}

BOOL SignIn_OfferMagicLink(HWND owner, WCHAR *url, size_t cch)
{
    WCHAR text[URL_CCH], email[256], question[512];
    BOOL open;
    if (!Util_ClipboardText(text, ARRAYSIZE(text))) return FALSE;
    open = Core_MagicLink(text, url, cch, email, ARRAYSIZE(email));
    SecureZeroMemory(text, sizeof text);
    if (!open) return FALSE;
    StringCchPrintfW(question, ARRAYSIZE(question), TR(L"The clipboard holds a Magic link, Claude's sign-in link for %s. Open it?"), email);
    if (!Ui_Ask(owner, IDI_QUESTION, question, TR(L"Open Magic link"), TR(L"Not now"), FALSE)) {
        SecureZeroMemory(url, cch * sizeof *url);
        return FALSE;
    }
    return TRUE;
}

/* The link the clipboard holds that a sign-in starts with: a Magic link, asked; else Claude Desktop's own sign-in link. */
static BOOL ClipboardSignIn(HWND owner, WCHAR *url, size_t cch)
{
    WCHAR text[URL_CCH];
    BOOL taken = FALSE;
    if (SignIn_OfferMagicLink(owner, url, cch)) return TRUE;
    if (Util_ClipboardText(text, ARRAYSIZE(text)) && wcschr(text, L'?') && !Core_MagicLink(text, NULL, 0, NULL, 0))
        taken = Core_PrivateSignInUrl(text, url, cch);
    SecureZeroMemory(text, sizeof text);
    return taken;
}

static void OpenBrowser(HWND dialog, SignInDialog *state)
{
    WCHAR raw[URL_CCH], url[URL_CCH];
    LRESULT chosen = SendDlgItemMessageW(dialog, IDC_SIGNIN_BROWSER, CB_GETCURSEL, 0, 0);
    BOOL own;
    if (chosen < 0 || chosen >= state->browsers.count) return;
    own = state->browsers.kind[chosen] == SIGNIN_BROWSER_OWN;
    ZeroMemory(raw, sizeof raw);
    if (GetWindowTextLengthW(GetDlgItem(dialog, IDC_SIGNIN_URL)) >= (int)ARRAYSIZE(raw) ||
        !GetDlgItemTextW(dialog, IDC_SIGNIN_URL, raw, ARRAYSIZE(raw)) ||
        !Core_PrivateSignInUrl(raw, url, ARRAYSIZE(url))) {
        SecureZeroMemory(raw, sizeof raw);
        Ui_Message(dialog, MB_ICONWARNING,
            TR(L"Use an HTTPS link to claude.ai, at its home page or /login path. Paste the complete link from Claude Desktop without changing it."));
        return;
    }
    SecureZeroMemory(raw, sizeof raw);
    if (!Launch(dialog, state->profile, &state->browsers, (int)chosen, url)) {
        SecureZeroMemory(url, sizeof url);
        return;
    }
    SecureZeroMemory(url, sizeof url);
    EnableWindow(GetDlgItem(dialog, IDC_SIGNIN_FOLDERS), TRUE);
    SetDlgItemTextW(dialog, IDC_SIGNIN_URL, L"https://claude.ai/login");
    SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS, own
        ? TR(L"Sign in in the window that opened. Once Claude Desktop is authorized, the window gives this profile its link and closes.")
        : TR(L"A separate private browser was started. Complete registration or sign-in yourself. If this is a Desktop sign-in, allow the claude:// link and choose this profile."));
}

static INT_PTR CALLBACK SignInProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    SignInDialog *state = (SignInDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        WCHAR profile[LABEL_CCH + 40];
        state = (SignInDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        StringCchPrintfW(profile, ARRAYSIZE(profile), TR(L"Profile: %s"), state->profile->name);
        SetDlgItemTextW(dialog, IDC_SIGNIN_PROFILE, profile);
        Theme_SetStrong(GetDlgItem(dialog, IDC_SIGNIN_PROFILE));
        /* A too-long paste stays invalid rather than becoming a truncated URL. */
        SendDlgItemMessageW(dialog, IDC_SIGNIN_URL, EM_LIMITTEXT, URL_CCH, 0);
        SetDlgItemTextW(dialog, IDC_SIGNIN_URL, L"https://claude.ai/login");
        BrowserRoot(state->root, ARRAYSIZE(state->root));
        RemoveOldWebFolders(state->root);
        EnableWindow(GetDlgItem(dialog, IDC_SIGNIN_FOLDERS), state->root[0] && Util_DirExists(state->root));
        FillBrowsers(dialog, state);
        return TRUE;
    }
    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_SIGNIN_OPEN:
            OpenBrowser(dialog, state);
            return TRUE;
        case IDC_SIGNIN_FOLDERS:
            if (state->root[0] && (INT_PTR)ShellExecuteW(dialog, L"open", state->root, NULL, NULL, SW_SHOWNORMAL) <= 32)
                Ui_Message(dialog, MB_ICONWARNING, TR(L"The private browser folders could not be opened."));
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_APP_SIGNIN_DONE:
        if (state && wp) SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS, TR(L"Claude Desktop got the sign-in link: this profile signs in."));
        else if (state && lp) SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS, TR(L"The sign-in window could not show the page. Choose Microsoft Edge or Google Chrome."));
        else if (state) SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS, TR(L"The sign-in window was closed before Claude Desktop was authorized."));
        return TRUE;
    case WM_DESTROY:
        SetDlgItemTextW(dialog, IDC_SIGNIN_URL, L"");
        if (state) SignIn_FreeBrowsers(&state->browsers);   /* the drop-down's icons, with it */
        break;
    }
    return FALSE;
}

/* Straight into the browser Settings chose (the program's window by
 * default), at Claude's sign-in page, or at the link the clipboard holds (a
 * Magic link, asked; Claude Desktop's sign-in link); the dialog only when
 * that browser is not there any more. */
void SignIn_Open(HWND owner, const Profile *profile)
{
    SignInDialog state;
    WCHAR url[URL_CCH], root[MAX_PATH];
    int i, chosen = -1, kind = (int)Util_GetSetting(SETTING_SIGNIN_BROWSER, SIGNIN_BROWSER_OWN);
    if (!profile) return;
    ZeroMemory(&state, sizeof state);
    state.profile = profile;
    SignIn_FindBrowsers(&state.browsers);
    for (i = 0; i < state.browsers.count; i++)
        if (state.browsers.kind[i] == kind) chosen = i;
    if (chosen >= 0) {
        if (BrowserRoot(root, ARRAYSIZE(root))) RemoveOldWebFolders(root);
        if (!ClipboardSignIn(owner, url, ARRAYSIZE(url))) StringCchCopyW(url, ARRAYSIZE(url), L"https://claude.ai/login");
        Launch(owner, profile, &state.browsers, chosen, url);
        SecureZeroMemory(url, sizeof url);
        SignIn_FreeBrowsers(&state.browsers);
        return;
    }
    SignIn_FreeBrowsers(&state.browsers);
    Ui_Dialog(owner, IDD_SIGNIN, SignInProc, (LPARAM)&state);
}
