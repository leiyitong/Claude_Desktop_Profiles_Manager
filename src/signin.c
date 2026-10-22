/*
 * A clean system-browser session for manual sign-up or sign-in. Claude itself
 * owns Desktop authorization and the existing claude:// router completes it.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <wchar.h>

#define PRIVATE_BROWSER_ARGS_CCH (URL_CCH + MAX_PATH + 160)

typedef struct PrivateBrowser {
    WCHAR exe[MAX_PATH];
    BOOL edge;
} PrivateBrowser;

typedef struct SignInDialog {
    const Profile *profile;
    PrivateBrowser browsers[2];
    int count;
    WCHAR root[MAX_PATH];
} SignInDialog;

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

static BOOL FindBrowser(BOOL edge, WCHAR *out, size_t cch)
{
    const WCHAR *file = edge ? L"msedge.exe" : L"chrome.exe";
    const WCHAR *relative = edge ? L"Microsoft\\Edge\\Application\\msedge.exe" : L"Google\\Chrome\\Application\\chrome.exe";
    return RegisteredBrowser(HKEY_CURRENT_USER, file, out, cch) || RegisteredBrowser(HKEY_LOCAL_MACHINE, file, out, cch) ||
           BrowserAt(L"ProgramFiles(x86)", relative, file, out, cch) || BrowserAt(L"ProgramFiles", relative, file, out, cch) ||
           BrowserAt(L"LOCALAPPDATA", relative, file, out, cch);
}

static void FillBrowsers(HWND dialog, SignInDialog *state)
{
    int i;
    for (i = 0; i < 2; i++) {
        PrivateBrowser browser;
        browser.edge = i == 0;
        if (FindBrowser(browser.edge, browser.exe, ARRAYSIZE(browser.exe))) {
            state->browsers[state->count++] = browser;
            SendDlgItemMessageW(dialog, IDC_SIGNIN_BROWSER, CB_ADDSTRING, 0,
                                (LPARAM)(browser.edge ? L"Microsoft Edge" : L"Google Chrome"));
        }
    }
    if (state->count) SendDlgItemMessageW(dialog, IDC_SIGNIN_BROWSER, CB_SETCURSEL, 0, 0);
    else SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS,
        TR(L"Microsoft Edge or Google Chrome was not found. Install one to use private sign-in."));
    EnableWindow(GetDlgItem(dialog, IDC_SIGNIN_OPEN), state->count != 0);
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

static BOOL NewBrowserFolder(SignInDialog *state, WCHAR *folder, size_t cch)
{
    WCHAR id[40];
    GUID guid;
    if (!BrowserRoot(state->root, ARRAYSIZE(state->root)) || !Util_EnsureDir(state->root)) return FALSE;
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(&guid, id, ARRAYSIZE(id)) ||
        FAILED(StringCchPrintfW(folder, cch, L"%s\\%s", state->root, id))) {
        SetLastError(ERROR_INVALID_NAME);
        return FALSE;
    }
    /* CREATE_NEW semantics: an existing directory is never reused. */
    return CreateDirectoryW(folder, NULL);
}

static void OpenBrowser(HWND dialog, SignInDialog *state)
{
    WCHAR raw[URL_CCH], url[URL_CCH], folder[MAX_PATH], args[PRIVATE_BROWSER_ARGS_CCH];
    LRESULT chosen = SendDlgItemMessageW(dialog, IDC_SIGNIN_BROWSER, CB_GETCURSEL, 0, 0);
    DWORD error;
    BOOL built;
    if (chosen < 0 || chosen >= state->count) return;
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
    if (!NewBrowserFolder(state, folder, ARRAYSIZE(folder))) {
        error = GetLastError();
        SecureZeroMemory(url, sizeof url);
        Ui_Message(dialog, MB_ICONERROR, TR(L"The private browser folder could not be created (error %lu)."), error);
        return;
    }
    EnableWindow(GetDlgItem(dialog, IDC_SIGNIN_FOLDERS), TRUE);
    built = Core_PrivateBrowserArgs(state->browsers[chosen].edge, folder, url, args, ARRAYSIZE(args));
    SecureZeroMemory(url, sizeof url);
    if (!built || !Util_Spawn(state->browsers[chosen].exe, args, NULL)) {
        error = built ? GetLastError() : ERROR_INVALID_PARAMETER;
        SecureZeroMemory(args, sizeof args);
        Ui_Message(dialog, MB_ICONERROR, TR(L"The private browser could not be started (error %lu)."), error);
        return;
    }
    SecureZeroMemory(args, sizeof args);
    /* Chromium may hand off to child processes, so its launcher's exit is not
     * proof that this folder is unused. Never delete it or stop its processes. */
    SetDlgItemTextW(dialog, IDC_SIGNIN_URL, L"https://claude.ai/login");
    SetDlgItemTextW(dialog, IDC_SIGNIN_STATUS,
        TR(L"A separate private browser was started. Complete registration or sign-in yourself. If this is a Desktop sign-in, allow the claude:// link and choose this profile."));
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
    case WM_DESTROY:
        SetDlgItemTextW(dialog, IDC_SIGNIN_URL, L"");
        break;
    }
    return FALSE;
}

void SignIn_Open(HWND owner, const Profile *profile)
{
    SignInDialog state;
    if (!profile) return;
    ZeroMemory(&state, sizeof state);
    state.profile = profile;
    Ui_Dialog(owner, IDD_SIGNIN, SignInProc, (LPARAM)&state);
}
