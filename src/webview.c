/*
 * Private sign-in in a window of the program's own (signin.c): Microsoft Edge
 * WebView2, which Windows 10 and 11 ship, with a data folder made for this
 * one sign-in and deleted once its window has closed, so the page knows no
 * account and keeps none. The claude:// link a Desktop sign-in ends with is
 * caught before Windows sees it and opened in the profile that asked for it
 * (Launcher_Open): no link to allow, no profile to choose. Pages open in the
 * one window (a provider's own window included); only https pages load.
 * The same window previews a session (WebView_ShowPage): its page alone,
 * from the state folder; a link it holds opens in the default browser.
 *
 * Each page a sign-in goes to is logged by its host and path, never its query
 * or fragment (codes and tokens are there), and so is the claude:// link it
 * ends with, so that a sign-in that never ends can be read afterwards. With
 * SETTING_SIGNIN_DEVTOOLS set (settings.ini, off by default) the window also
 * serves Chromium's DevTools protocol on that port of 127.0.0.1, for as long as
 * it is open: any program of this user can then drive the page and read its
 * cookies, so it is for debugging a sign-in, not for every day.
 *
 * The runtime is found and started as Microsoft's WebView2Loader does: by
 * its folder (its EdgeUpdate entry, else its install folder) and the entry
 * point of its EmbeddedBrowserWebView.dll. No loader DLL is shipped, and the
 * few interfaces used are declared below, their methods in the order of
 * WebView2.idl (SDK 1.0.2903.40); a slot not used is a bare pointer.
 */
#include "app.h"
#include "resource.h"
#include <objbase.h>
#include <shellapi.h>

#define WEB_CLASS            L"ClaudeDesktopProfilesManager.SignIn"
#define WEB_CLIENT_STATE     L"Software\\Microsoft\\EdgeUpdate\\ClientState\\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"   /* the runtime's */
#define WEB_WIDTH_DIPS       960
#define WEB_HEIGHT_DIPS      760
#define SIGNIN_COOKIES_MAX   64   /* claude.ai cookies a sign-in carries, at most */

typedef struct WvToken { INT64 value; } WvToken;   /* EventRegistrationToken */

typedef struct WvEnvironment WvEnvironment;
typedef struct WvController WvController;
typedef struct WvView WvView;
typedef struct WvSettings WvSettings;
typedef struct WvNavigationArgs WvNavigationArgs;
typedef struct WvNewWindowArgs WvNewWindowArgs;

typedef struct WvEnvironmentVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvEnvironment *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvEnvironment *);
    ULONG (STDMETHODCALLTYPE *Release)(WvEnvironment *);
    HRESULT (STDMETHODCALLTYPE *CreateCoreWebView2Controller)(WvEnvironment *, HWND, IUnknown *);
} WvEnvironmentVtbl;
struct WvEnvironment { const WvEnvironmentVtbl *lpVtbl; };

typedef struct WvControllerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvController *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvController *);
    ULONG (STDMETHODCALLTYPE *Release)(WvController *);
    void *get_IsVisible;
    HRESULT (STDMETHODCALLTYPE *put_IsVisible)(WvController *, BOOL);
    void *get_Bounds;
    HRESULT (STDMETHODCALLTYPE *put_Bounds)(WvController *, RECT);
    void *get_ZoomFactor, *put_ZoomFactor, *add_ZoomFactorChanged, *remove_ZoomFactorChanged, *SetBoundsAndZoomFactor;
    HRESULT (STDMETHODCALLTYPE *MoveFocus)(WvController *, int);
    void *add_MoveFocusRequested, *remove_MoveFocusRequested, *add_GotFocus, *remove_GotFocus, *add_LostFocus, *remove_LostFocus;
    void *add_AcceleratorKeyPressed, *remove_AcceleratorKeyPressed, *get_ParentWindow, *put_ParentWindow;
    HRESULT (STDMETHODCALLTYPE *NotifyParentWindowPositionChanged)(WvController *);
    HRESULT (STDMETHODCALLTYPE *Close)(WvController *);
    HRESULT (STDMETHODCALLTYPE *get_CoreWebView2)(WvController *, WvView **);
} WvControllerVtbl;
struct WvController { const WvControllerVtbl *lpVtbl; };

typedef struct WvViewVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvView *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvView *);
    ULONG (STDMETHODCALLTYPE *Release)(WvView *);
    HRESULT (STDMETHODCALLTYPE *get_Settings)(WvView *, WvSettings **);
    void *get_Source;
    HRESULT (STDMETHODCALLTYPE *Navigate)(WvView *, LPCWSTR);
    void *NavigateToString;
    HRESULT (STDMETHODCALLTYPE *add_NavigationStarting)(WvView *, IUnknown *, WvToken *);
    HRESULT (STDMETHODCALLTYPE *remove_NavigationStarting)(WvView *, WvToken);
    void *add_ContentLoading, *remove_ContentLoading, *add_SourceChanged, *remove_SourceChanged, *add_HistoryChanged, *remove_HistoryChanged;
    void *add_NavigationCompleted, *remove_NavigationCompleted, *add_FrameNavigationStarting, *remove_FrameNavigationStarting;
    void *add_FrameNavigationCompleted, *remove_FrameNavigationCompleted, *add_ScriptDialogOpening, *remove_ScriptDialogOpening;
    void *add_PermissionRequested, *remove_PermissionRequested, *add_ProcessFailed, *remove_ProcessFailed;
    void *AddScriptToExecuteOnDocumentCreated, *RemoveScriptToExecuteOnDocumentCreated, *ExecuteScript, *CapturePreview, *Reload;
    void *PostWebMessageAsJson, *PostWebMessageAsString, *add_WebMessageReceived, *remove_WebMessageReceived, *CallDevToolsProtocolMethod;
    void *get_BrowserProcessId, *get_CanGoBack, *get_CanGoForward, *GoBack, *GoForward, *GetDevToolsProtocolEventReceiver, *Stop;
    HRESULT (STDMETHODCALLTYPE *add_NewWindowRequested)(WvView *, IUnknown *, WvToken *);
    HRESULT (STDMETHODCALLTYPE *remove_NewWindowRequested)(WvView *, WvToken);
} WvViewVtbl;
struct WvView { const WvViewVtbl *lpVtbl; };

typedef struct WvSettingsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvSettings *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvSettings *);
    ULONG (STDMETHODCALLTYPE *Release)(WvSettings *);
    void *get_IsScriptEnabled, *put_IsScriptEnabled, *get_IsWebMessageEnabled, *put_IsWebMessageEnabled;
    void *get_AreDefaultScriptDialogsEnabled, *put_AreDefaultScriptDialogsEnabled, *get_IsStatusBarEnabled, *put_IsStatusBarEnabled;
    void *get_AreDevToolsEnabled;
    HRESULT (STDMETHODCALLTYPE *put_AreDevToolsEnabled)(WvSettings *, BOOL);
    void *get_AreDefaultContextMenusEnabled, *put_AreDefaultContextMenusEnabled, *get_AreHostObjectsAllowed;
    HRESULT (STDMETHODCALLTYPE *put_AreHostObjectsAllowed)(WvSettings *, BOOL);
} WvSettingsVtbl;
struct WvSettings { const WvSettingsVtbl *lpVtbl; };

typedef struct WvNavigationArgsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvNavigationArgs *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvNavigationArgs *);
    ULONG (STDMETHODCALLTYPE *Release)(WvNavigationArgs *);
    HRESULT (STDMETHODCALLTYPE *get_Uri)(WvNavigationArgs *, LPWSTR *);
    void *get_IsUserInitiated, *get_IsRedirected, *get_RequestHeaders, *get_Cancel;
    HRESULT (STDMETHODCALLTYPE *put_Cancel)(WvNavigationArgs *, BOOL);
} WvNavigationArgsVtbl;
struct WvNavigationArgs { const WvNavigationArgsVtbl *lpVtbl; };

typedef struct WvNewWindowArgsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvNewWindowArgs *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvNewWindowArgs *);
    ULONG (STDMETHODCALLTYPE *Release)(WvNewWindowArgs *);
    HRESULT (STDMETHODCALLTYPE *get_Uri)(WvNewWindowArgs *, LPWSTR *);
    void *put_NewWindow, *get_NewWindow;
    HRESULT (STDMETHODCALLTYPE *put_Handled)(WvNewWindowArgs *, BOOL);
} WvNewWindowArgsVtbl;
struct WvNewWindowArgs { const WvNewWindowArgsVtbl *lpVtbl; };

/* EmbeddedBrowserWebView.dll's entry point, as WebView2Loader calls it. */
typedef HRESULT (STDMETHODCALLTYPE *CreateEnvironmentFn)(BOOL checkRunningInstance, int runtimeType, LPCWSTR userDataFolder,
                                                        IUnknown *options, IUnknown *completed);

/* ------------------------------------------------------------- the window */

typedef struct WebSignIn {
    LONG          refs;          /* the window's and each handler's */
    HWND          window, owner;
    Profile       profile;
    WCHAR         url[URL_CCH];
    WCHAR         folder[MAX_PATH];   /* its data folder, deleted once it is unused */
    WvController *controller;
    WvView       *view;
    WvToken       navigation, newWindow;
    BOOL          told;          /* the dialog was told how it ended */
    BOOL          preview;       /* a page of ours: `url` alone loads, its links open in the default browser; `folder` is kept */
    ULONGLONG     offered;       /* the Magic link last offered from the clipboard (its hash): not asked again */
    ULONGLONG     settleAt;      /* a session cookie was seen: when its fellows have had time to be written (0: not yet) */
} WebSignIn;

/* One of the handlers WebView2 calls back: its Invoke takes two pointers.
 * The interface is the handler's start, its table its own (each Invoke differs). */
typedef struct WvHandler WvHandler;
typedef HRESULT (STDMETHODCALLTYPE *InvokeFn)(WvHandler *, void *, void *);
typedef struct WvHandlerVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvHandler *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvHandler *);
    ULONG (STDMETHODCALLTYPE *Release)(WvHandler *);
    InvokeFn Invoke;
} WvHandlerVtbl;
struct WvHandler {
    const WvHandlerVtbl *lpVtbl;
    WvHandlerVtbl        vtbl;
    LONG                 refs;
    WebSignIn           *state;
};

static void ReleaseState(WebSignIn *state)
{
    if (InterlockedDecrement(&state->refs) > 0) return;
    SecureZeroMemory(state->url, sizeof state->url);
    if (state->folder[0] && !state->preview) {
        DWORD error = 0;
        /* Its browser processes end once the controller is closed: what they still hold stays for the next sign-in. */
        if (!Util_DeleteTree(state->folder, &error)) Util_Log(L"sign-in window: its data folder is kept for now (error %lu)", error);
    }
    HeapFree(GetProcessHeap(), 0, state);
}

/* Any interface asked for is this handler's own: WebView2 asks only for the one it calls. */
static HRESULT STDMETHODCALLTYPE HandlerQuery(WvHandler *self, REFIID iid, void **out)
{
    (void)iid;
    if (!out) return E_POINTER;
    *out = self;
    self->lpVtbl->AddRef(self);
    return S_OK;
}

static ULONG STDMETHODCALLTYPE HandlerAddRef(WvHandler *self)
{
    return (ULONG)InterlockedIncrement(&self->refs);
}

static ULONG STDMETHODCALLTYPE HandlerRelease(WvHandler *self)
{
    LONG left = InterlockedDecrement(&self->refs);
    if (left == 0) {
        ReleaseState(self->state);
        HeapFree(GetProcessHeap(), 0, self);
    }
    return (ULONG)left;
}

/* A handler whose Invoke is `invoke`, holding `state`: the interface pointer to give WebView2. */
static IUnknown *NewHandler(WebSignIn *state, InvokeFn invoke)
{
    WvHandler *handler = (WvHandler *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *handler);
    if (!handler) return NULL;
    handler->vtbl.QueryInterface = HandlerQuery;
    handler->vtbl.AddRef = HandlerAddRef;
    handler->vtbl.Release = HandlerRelease;
    handler->vtbl.Invoke = invoke;
    handler->lpVtbl = &handler->vtbl;
    handler->refs = 1;
    handler->state = state;
    InterlockedIncrement(&state->refs);
    return (IUnknown *)handler;
}

static WebSignIn *StateOf(WvHandler *self)
{
    return self->state;
}

static BOOL HasScheme(const WCHAR *uri, const WCHAR *scheme)
{
    size_t n = wcslen(scheme);
    return uri && _wcsnicmp(uri, scheme, n) == 0;
}

/* `uri` up to its query or fragment: what a log may keep of a sign-in's page. */
static void PageOf(const WCHAR *uri, WCHAR *out, size_t cch)
{
    size_t n = wcscspn(uri, L"?#");
    StringCchCopyNW(out, cch, uri, n);
}

#define SIGNIN_POLL_TIMER 0x5C01
#define SIGNIN_SETTLE_MS  800   /* once a session cookie appears, give the rest a moment to be written */

/* The sign-in is done (claude.ai set a session cookie): read the cookies from this window's store, write them
 * into the profile that asked, open it, and close the window. The profile's Claude is closed first if it runs. */
static void SaveCookiesAndOpen(WebSignIn *state)
{
    SignInCookie *cookies;
    ClaudePackage pkg;
    DWORD error = 0;
    int count = 0;
    BOOL ran;
    if (state->told) return;
    state->told = TRUE;
    KillTimer(state->window, SIGNIN_POLL_TIMER);
    cookies = (SignInCookie *)HeapAlloc(GetProcessHeap(), 0, (size_t)SIGNIN_COOKIES_MAX * sizeof *cookies);
    if (!cookies || !Cookies_ReadWebView(state->folder, cookies, SIGNIN_COOKIES_MAX, &count) || count <= 0) {
        if (cookies) HeapFree(GetProcessHeap(), 0, cookies);
        Util_Log(L"sign-in window: signed in, but its cookies could not be read for %s", state->profile.folder);
        if (state->owner) PostMessageW(state->owner, WM_APP_SIGNIN_DONE, FALSE, (LPARAM)E_FAIL);
        if (state->window) PostMessageW(state->window, WM_CLOSE, 0, 0);
        return;
    }
    ran = Claude_IsRunning(&state->profile);
    if (ran) Claude_Quit(&state->profile, &error);   /* it holds its cookie store open and writes it back on exit */
    if (Cookies_Write(&state->profile, cookies, count, &count) && Claude_FindPackage(&pkg)) {
        Launcher_Open(&pkg, &state->profile, NULL, NULL, NULL);
        Util_Log(L"sign-in window: %d cookie(s) -> %s, opened", count, state->profile.folder);
        if (state->owner) PostMessageW(state->owner, WM_APP_SIGNIN_DONE, TRUE, (LPARAM)S_OK);
    } else {
        Util_Log(L"sign-in window: the cookies could not be written to %s", state->profile.folder);
        if (state->owner) PostMessageW(state->owner, WM_APP_SIGNIN_DONE, FALSE, (LPARAM)E_FAIL);
    }
    SecureZeroMemory(cookies, (size_t)SIGNIN_COOKIES_MAX * sizeof *cookies);
    HeapFree(GetProcessHeap(), 0, cookies);
    if (state->window) PostMessageW(state->window, WM_CLOSE, 0, 0);
}

/* The claude:// link the sign-in ended with, opened in the profile that asked; the window closes. */
static void HandOver(WebSignIn *state, const WCHAR *uri)
{
    WCHAR link[URL_CCH];
    ClaudePackage pkg;
    HRESULT hr = E_FAIL;
    if (state->told) return;
    state->told = TRUE;
    WCHAR page[URL_CCH];
    PageOf(uri, page, ARRAYSIZE(page));
    if (Core_SanitizeUrl(uri, link, ARRAYSIZE(link)) && Claude_FindPackage(&pkg)) hr = Launcher_Open(&pkg, &state->profile, link, NULL, NULL);
    SecureZeroMemory(link, sizeof link);
    Util_Log(L"sign-in window: %s -> %s%s (0x%08lX)", page, state->profile.folder, SUCCEEDED(hr) ? L"" : L" FAILED", (unsigned long)hr);
    if (state->owner) PostMessageW(state->owner, WM_APP_SIGNIN_DONE, SUCCEEDED(hr), (LPARAM)hr);
    if (state->window) PostMessageW(state->window, WM_CLOSE, 0, 0);
}

/* Each page: a claude:// link is the sign-in's end; an https page or a blank one loads; nothing else does. */
static HRESULT STDMETHODCALLTYPE OnNavigationStarting(WvHandler *self, void *sender, void *argsPointer)
{
    WebSignIn *state = StateOf(self);
    WvNavigationArgs *args = (WvNavigationArgs *)argsPointer;
    LPWSTR uri = NULL;
    (void)sender;
    if (FAILED(args->lpVtbl->get_Uri(args, &uri)) || !uri) return S_OK;
    if (state->preview) {
        if (_wcsicmp(uri, state->url) != 0) {
            args->lpVtbl->put_Cancel(args, TRUE);
            if (HasScheme(uri, L"https://")) ShellExecuteW(state->window, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
        }
    } else if (HasScheme(uri, L"claude:")) {
        args->lpVtbl->put_Cancel(args, TRUE);
        HandOver(state, uri);
    } else if (!HasScheme(uri, L"https://") && !HasScheme(uri, L"about:blank")) {
        WCHAR page[URL_CCH];
        PageOf(uri, page, ARRAYSIZE(page));
        Util_Log(L"sign-in window: %s not loaded (only https pages are)", page);
        args->lpVtbl->put_Cancel(args, TRUE);
    } else if (HasScheme(uri, L"https://")) {
        WCHAR page[URL_CCH];
        PageOf(uri, page, ARRAYSIZE(page));
        Util_Log(L"sign-in window: %s", page);
    }
    CoTaskMemFree(uri);
    return S_OK;
}

/* A page that opens a window of its own (a provider's sign-in) goes on in this one. */
static HRESULT STDMETHODCALLTYPE OnNewWindow(WvHandler *self, void *sender, void *argsPointer)
{
    WebSignIn *state = StateOf(self);
    WvNewWindowArgs *args = (WvNewWindowArgs *)argsPointer;
    LPWSTR uri = NULL;
    (void)sender;
    args->lpVtbl->put_Handled(args, TRUE);
    if (SUCCEEDED(args->lpVtbl->get_Uri(args, &uri)) && uri) {
        if (state->preview) {
            if (HasScheme(uri, L"https://")) ShellExecuteW(state->window, L"open", uri, NULL, NULL, SW_SHOWNORMAL);
        } else if (HasScheme(uri, L"claude:")) HandOver(state, uri);
        else if (HasScheme(uri, L"https://") && state->view) state->view->lpVtbl->Navigate(state->view, uri);
        CoTaskMemFree(uri);
    }
    return S_OK;
}

static void FitView(WebSignIn *state)
{
    RECT client;
    if (state->controller && GetClientRect(state->window, &client)) state->controller->lpVtbl->put_Bounds(state->controller, client);
}

static void Failed(WebSignIn *state, HRESULT hr)
{
    Util_Log(L"sign-in window: WebView2 could not start (0x%08lX)", (unsigned long)hr);
    if (state->owner && !state->told) PostMessageW(state->owner, WM_APP_SIGNIN_DONE, FALSE, (LPARAM)hr);
    state->told = TRUE;
    if (state->window) PostMessageW(state->window, WM_CLOSE, 0, 0);
}

static HRESULT STDMETHODCALLTYPE OnController(WvHandler *self, void *result, void *controllerPointer)
{
    WebSignIn *state = StateOf(self);
    WvController *controller = (WvController *)controllerPointer;
    WvSettings *settings = NULL;
    IUnknown *handler;
    HRESULT hr = (HRESULT)(INT_PTR)result;
    if (!state->window) return S_OK;   /* closed meanwhile */
    if (FAILED(hr) || !controller) {
        Failed(state, FAILED(hr) ? hr : E_FAIL);
        return S_OK;
    }
    controller->lpVtbl->AddRef(controller);
    state->controller = controller;
    if (FAILED(hr = controller->lpVtbl->get_CoreWebView2(controller, &state->view)) || !state->view) {
        Failed(state, FAILED(hr) ? hr : E_FAIL);
        return S_OK;
    }
    if (SUCCEEDED(state->view->lpVtbl->get_Settings(state->view, &settings)) && settings) {
        /* F12 and the context menu are off, except when the DevTools port is set (debugging a sign-in). */
        settings->lpVtbl->put_AreDevToolsEnabled(settings, !state->preview && Util_GetSetting(SETTING_SIGNIN_DEVTOOLS, 0) ? TRUE : FALSE);
        settings->lpVtbl->put_AreHostObjectsAllowed(settings, FALSE);
        settings->lpVtbl->Release(settings);
    }
    if ((handler = NewHandler(state, OnNavigationStarting)) != NULL) {
        state->view->lpVtbl->add_NavigationStarting(state->view, handler, &state->navigation);
        handler->lpVtbl->Release(handler);
    }
    if ((handler = NewHandler(state, OnNewWindow)) != NULL) {
        state->view->lpVtbl->add_NewWindowRequested(state->view, handler, &state->newWindow);
        handler->lpVtbl->Release(handler);
    }
    FitView(state);
    controller->lpVtbl->put_IsVisible(controller, TRUE);
    controller->lpVtbl->MoveFocus(controller, 0 /* COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC */);
    state->view->lpVtbl->Navigate(state->view, state->url);
    if (!state->preview) SecureZeroMemory(state->url, sizeof state->url);   /* a sign-in link: kept no longer than needed */
    /* A sign-in watches its own cookie store: once claude.ai sets a session cookie, the cookies are read and
     * written into the profile (SaveCookiesAndOpen), so a web sign-in signs the profile in, not only a Desktop one. */
    if (!state->preview) SetTimer(state->window, SIGNIN_POLL_TIMER, 1000, NULL);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE OnEnvironment(WvHandler *self, void *result, void *environmentPointer)
{
    WebSignIn *state = StateOf(self);
    WvEnvironment *environment = (WvEnvironment *)environmentPointer;
    IUnknown *handler;
    HRESULT hr = (HRESULT)(INT_PTR)result;
    if (!state->window) return S_OK;
    if (FAILED(hr) || !environment) {
        Failed(state, FAILED(hr) ? hr : E_FAIL);
        return S_OK;
    }
    if ((handler = NewHandler(state, OnController)) == NULL) {
        Failed(state, E_OUTOFMEMORY);
        return S_OK;
    }
    if (FAILED(hr = environment->lpVtbl->CreateCoreWebView2Controller(environment, state->window, handler))) Failed(state, hr);
    handler->lpVtbl->Release(handler);
    return S_OK;
}

static LRESULT CALLBACK WebProc(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    WebSignIn *state = (WebSignIn *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)((const CREATESTRUCTW *)lp)->lpCreateParams);
        return DefWindowProcW(window, msg, wp, lp);
    }
    if (!state) return DefWindowProcW(window, msg, wp, lp);
    switch (msg) {
    case WM_SIZE:
        FitView(state);
        return 0;
    case WM_MOVE:
        if (state->controller) state->controller->lpVtbl->NotifyParentWindowPositionChanged(state->controller);
        return 0;
    case WM_SETFOCUS:
        if (state->controller) state->controller->lpVtbl->MoveFocus(state->controller, 0);
        return 0;
    case WM_TIMER:
        if (wp == SIGNIN_POLL_TIMER && !state->told) {
            if (!state->settleAt) {
                if (Cookies_WebViewSignedIn(state->folder)) state->settleAt = GetTickCount64() + SIGNIN_SETTLE_MS;
            } else if (GetTickCount64() >= state->settleAt) {
                SaveCookiesAndOpen(state);
            }
        }
        return 0;
    case WM_DPICHANGED: {
        const RECT *suggested = (const RECT *)lp;
        SetWindowPos(window, NULL, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLIPBOARDUPDATE: {
        /* A Magic link copied (from the mail Claude sent) while signing in: opened here, asked once. */
        WCHAR text[URL_CCH], url[URL_CCH];
        ULONGLONG hash;
        if (!state->preview && state->view && Util_ClipboardText(text, ARRAYSIZE(text)) && Core_MagicLink(text, NULL, 0, NULL, 0) &&
            (hash = Core_HashText(CORE_HASH_START, text)) != state->offered) {
            state->offered = hash;
            if (SignIn_OfferMagicLink(window, url, ARRAYSIZE(url))) state->view->lpVtbl->Navigate(state->view, url);
            SecureZeroMemory(url, sizeof url);
        }
        SecureZeroMemory(text, sizeof text);
        return 0;
    }
    case WM_DESTROY:
        if (!state->preview) { RemoveClipboardFormatListener(window); KillTimer(window, SIGNIN_POLL_TIMER); }
        if (state->view) {
            state->view->lpVtbl->remove_NavigationStarting(state->view, state->navigation);
            state->view->lpVtbl->remove_NewWindowRequested(state->view, state->newWindow);
            state->view->lpVtbl->Release(state->view);
            state->view = NULL;
        }
        if (state->controller) {
            state->controller->lpVtbl->Close(state->controller);
            state->controller->lpVtbl->Release(state->controller);
            state->controller = NULL;
        }
        if (state->owner && !state->told) PostMessageW(state->owner, WM_APP_SIGNIN_DONE, FALSE, 0);   /* closed by the person */
        state->window = NULL;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        ReleaseState(state);   /* the window's */
        return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}

/* ------------------------------------------------------------- the runtime */

/* The runtime's EmbeddedBrowserWebView.dll for this process: in the folder
 * its EdgeUpdate entry names (per user, then per machine), else under the
 * newest version of its install folder. */
static BOOL RuntimeDll(WCHAR *out, size_t cch)
{
#if defined(_M_ARM64) || defined(__aarch64__)
    static const WCHAR kArch[] = L"arm64";
#else
    static const WCHAR kArch[] = L"x64";
#endif
    static const WCHAR *const kBases[] = { L"ProgramFiles(x86)", L"ProgramFiles", L"LOCALAPPDATA" };
    WCHAR folder[MAX_PATH], base[MAX_PATH], pattern[MAX_PATH], best[64] = L"";
    DWORD bestParts[4], parts[4];
    WIN32_FIND_DATAW found;
    HANDLE find;
    size_t i;
    if ((Util_RegGetString(HKEY_CURRENT_USER, WEB_CLIENT_STATE, L"EBWebView", folder, ARRAYSIZE(folder)) ||
         Util_RegGetString(HKEY_LOCAL_MACHINE, WEB_CLIENT_STATE, L"EBWebView", folder, ARRAYSIZE(folder))) &&
        SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s\\EmbeddedBrowserWebView.dll", folder, kArch)) && Util_FileExists(out))
        return TRUE;
    for (i = 0; i < ARRAYSIZE(kBases); i++) {
        DWORD n = GetEnvironmentVariableW(kBases[i], base, ARRAYSIZE(base));
        if (!n || n >= ARRAYSIZE(base) || FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\Microsoft\\EdgeWebView\\Application\\*", base)))
            continue;
        best[0] = 0;
        if ((find = FindFirstFileW(pattern, &found)) == INVALID_HANDLE_VALUE) continue;
        do {
            /* A version folder: digits and dots; the newest by its numbers. */
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Core_ParseVersion(found.cFileName, parts) &&
                (!best[0] || Core_CompareVersions(parts, bestParts) > 0)) {
                StringCchCopyW(best, ARRAYSIZE(best), found.cFileName);
                CopyMemory(bestParts, parts, sizeof parts);
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
        if (best[0] && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\Microsoft\\EdgeWebView\\Application\\%s\\EBWebView\\%s\\EmbeddedBrowserWebView.dll",
                                                  base, best, kArch)) && Util_FileExists(out))
            return TRUE;
    }
    if (cch) out[0] = 0;
    return FALSE;
}

BOOL WebSignIn_Available(void)
{
    WCHAR dll[MAX_PATH];
    return RuntimeDll(dll, ARRAYSIZE(dll));
}

/* ICoreWebView2EnvironmentOptions, its methods in WebView2.idl's order: the browser's arguments alone, the rest
 * left to the runtime. Made only when the DevTools port is set. */
typedef struct WvOptions WvOptions;
typedef struct WvOptionsVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(WvOptions *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(WvOptions *);
    ULONG (STDMETHODCALLTYPE *Release)(WvOptions *);
    HRESULT (STDMETHODCALLTYPE *get_AdditionalBrowserArguments)(WvOptions *, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *put_AdditionalBrowserArguments)(WvOptions *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *get_Language)(WvOptions *, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *put_Language)(WvOptions *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *get_TargetCompatibleBrowserVersion)(WvOptions *, LPWSTR *);
    HRESULT (STDMETHODCALLTYPE *put_TargetCompatibleBrowserVersion)(WvOptions *, LPCWSTR);
    HRESULT (STDMETHODCALLTYPE *get_AllowSingleSignOnUsingOSPrimaryAccount)(WvOptions *, BOOL *);
    HRESULT (STDMETHODCALLTYPE *put_AllowSingleSignOnUsingOSPrimaryAccount)(WvOptions *, BOOL);
} WvOptionsVtbl;
struct WvOptions {
    const WvOptionsVtbl *lpVtbl;
    LONG                 refs;
    WCHAR                arguments[64];
};
static const IID IID_WvOptions = { 0x2fde08a8, 0x1e9a, 0x4766, { 0x8c, 0x05, 0x95, 0xa9, 0xce, 0xb9, 0xd1, 0xc5 } };

static HRESULT OptionsText(const WCHAR *text, LPWSTR *out)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    if (!out) return E_POINTER;
    if ((*out = (LPWSTR)CoTaskMemAlloc(bytes)) == NULL) return E_OUTOFMEMORY;
    memcpy(*out, text, bytes);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE OptionsQuery(WvOptions *self, REFIID iid, void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_WvOptions)) {
        *out = self;
        InterlockedIncrement(&self->refs);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE OptionsAddRef(WvOptions *self) { return (ULONG)InterlockedIncrement(&self->refs); }
static ULONG STDMETHODCALLTYPE OptionsRelease(WvOptions *self)
{
    LONG left = InterlockedDecrement(&self->refs);
    if (!left) HeapFree(GetProcessHeap(), 0, self);
    return (ULONG)left;
}
static HRESULT STDMETHODCALLTYPE OptionsArguments(WvOptions *self, LPWSTR *out) { return OptionsText(self->arguments, out); }
static HRESULT STDMETHODCALLTYPE OptionsEmpty(WvOptions *self, LPWSTR *out) { (void)self; return OptionsText(L"", out); }
/* The oldest runtime that has what is used here: any runtime installed is as new. */
static HRESULT STDMETHODCALLTYPE OptionsVersion(WvOptions *self, LPWSTR *out) { (void)self; return OptionsText(L"86.0.616.0", out); }
static HRESULT STDMETHODCALLTYPE OptionsNoSso(WvOptions *self, BOOL *out) { (void)self; if (!out) return E_POINTER; *out = FALSE; return S_OK; }
static HRESULT STDMETHODCALLTYPE OptionsPutText(WvOptions *self, LPCWSTR value) { (void)self; (void)value; return S_OK; }
static HRESULT STDMETHODCALLTYPE OptionsPutBool(WvOptions *self, BOOL value) { (void)self; (void)value; return S_OK; }
static const WvOptionsVtbl kOptionsVtbl = { OptionsQuery, OptionsAddRef, OptionsRelease, OptionsArguments, OptionsPutText,
                                            OptionsEmpty, OptionsPutText, OptionsVersion, OptionsPutText, OptionsNoSso, OptionsPutBool };

/* The options serving DevTools on `port` of 127.0.0.1; NULL without memory. */
static IUnknown *DevToolsOptions(DWORD port)
{
    WvOptions *options = (WvOptions *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *options);
    if (!options) return NULL;
    options->lpVtbl = &kOptionsVtbl;
    options->refs = 1;
    StringCchPrintfW(options->arguments, ARRAYSIZE(options->arguments), L"--remote-debugging-port=%lu", port);
    return (IUnknown *)options;
}

/* The environment made in `folder`, its controller then made for `state`'s window (OnEnvironment). */
static HRESULT StartRuntime(WebSignIn *state)
{
    static CreateEnvironmentFn create;
    WCHAR dll[MAX_PATH];
    IUnknown *handler, *options = NULL;
    DWORD port = state->preview ? 0 : Util_GetSetting(SETTING_SIGNIN_DEVTOOLS, 0);
    HRESULT hr;
    if (!create) {
        HMODULE module;
        if (!RuntimeDll(dll, ARRAYSIZE(dll))) return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        if ((module = LoadLibraryExW(dll, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)) == NULL) return HRESULT_FROM_WIN32(GetLastError());
        create = (CreateEnvironmentFn)(void *)GetProcAddress(module, "CreateWebViewEnvironmentWithOptionsInternal");
        if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    if ((handler = NewHandler(state, OnEnvironment)) == NULL) return E_OUTOFMEMORY;
    if (port > 0 && port < 65536 && (options = DevToolsOptions(port)) != NULL)
        Util_Log(L"sign-in window: DevTools on http://127.0.0.1:%lu (SignInDevToolsPort in settings.ini)", port);
    /* The installed runtime (0); a runtime of another kind (1) as WebView2Loader tries it next. */
    hr = create(TRUE, 0, state->folder, options, handler);
    if (FAILED(hr)) hr = create(TRUE, 1, state->folder, options, handler);
    if (options) options->lpVtbl->Release(options);
    handler->lpVtbl->Release(handler);
    return hr;
}

/* The window for `state` (its url, folder and title set), shown, its runtime started. */
static HRESULT OpenWindow(WebSignIn *state, const WCHAR *title)
{
    static BOOL registered;
    UINT dpi = GetDpiForWindow(state->owner);
    HRESULT hr;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = WebProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APP));
        wc.hbrBackground = Theme_Brush(THEME_FIELD);
        wc.lpszClassName = WEB_CLASS;
        if (!RegisterClassW(&wc)) {
            hr = HRESULT_FROM_WIN32(GetLastError());
            state->folder[0] = 0;   /* the caller's still */
            ReleaseState(state);
            return hr;
        }
        registered = TRUE;
    }
    if (!dpi) dpi = 96;
    /* Owned by the manager, it would have no button of its own nor a place in Alt+Tab: it has both. */
    state->window = CreateWindowExW(WS_EX_APPWINDOW, WEB_CLASS, title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                                    MulDiv(WEB_WIDTH_DIPS, (int)dpi, 96), MulDiv(WEB_HEIGHT_DIPS, (int)dpi, 96), state->owner, NULL, g_hInst, state);
    if (!state->window) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        state->folder[0] = 0;
        ReleaseState(state);
        return hr;
    }
    Theme_TitleBar(state->window);
    if (!state->preview) AddClipboardFormatListener(state->window);
    ShowWindow(state->window, SW_SHOWNORMAL);
    if (FAILED(hr = StartRuntime(state))) {
        state->owner = NULL;   /* the caller says it */
        state->folder[0] = 0;
        DestroyWindow(state->window);
        return hr;
    }
    return S_OK;
}

static WebSignIn *NewState(HWND owner, const WCHAR *url, const WCHAR *folder)
{
    WebSignIn *state = (WebSignIn *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *state);
    if (!state) return NULL;
    state->refs = 1;   /* the window's */
    state->owner = owner;
    if (FAILED(StringCchCopyW(state->url, ARRAYSIZE(state->url), url)) || FAILED(StringCchCopyW(state->folder, ARRAYSIZE(state->folder), folder))) {
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }
    return state;
}

HRESULT WebSignIn_Open(HWND owner, const Profile *profile, const WCHAR *url, const WCHAR *folder)
{
    WebSignIn *state = NewState(owner, url, folder);
    WCHAR title[LABEL_CCH + 64];
    if (!state) return E_INVALIDARG;
    state->profile = *profile;
    StringCchPrintfW(title, ARRAYSIZE(title), TR(L"Sign in to Claude: %s"), profile->name);
    return OpenWindow(state, title);
}

/* `path` as a file: URL, as WebView2 reports it back: its separators slashes,
 * its bytes outside letters, digits and -._~:/ escaped as %XX of UTF-8. */
static BOOL FileUrl(const WCHAR *path, WCHAR *out, size_t cch)
{
    static const char kHex[] = "0123456789ABCDEF";
    char utf8[MAX_PATH * 3];
    size_t used;
    const char *c;
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, (int)sizeof utf8, NULL, NULL) || FAILED(StringCchCopyW(out, cch, L"file:///"))) return FALSE;
    used = wcslen(out);
    for (c = utf8; *c; c++) {
        BYTE b = (BYTE)*c;
        if (used + 4 > cch) return FALSE;
        if (b == '\\') out[used++] = L'/';
        else if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || strchr("-._~:/", b)) out[used++] = (WCHAR)b;
        else {
            out[used++] = L'%';
            out[used++] = (WCHAR)kHex[b >> 4];
            out[used++] = (WCHAR)kHex[b & 15];
        }
    }
    out[used] = 0;
    return TRUE;
}

HRESULT WebView_ShowPage(HWND owner, const WCHAR *title, const WCHAR *path)
{
    WCHAR url[URL_CCH], folder[MAX_PATH], state[MAX_PATH];
    WebSignIn *view;
    /* The previews share one folder of their own: it is no account's. */
    if (!FileUrl(path, url, ARRAYSIZE(url)) || !Util_StateDir(state, ARRAYSIZE(state)) ||
        FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\preview-webview", state)) || !Util_EnsureDir(folder))
        return E_INVALIDARG;
    if ((view = NewState(NULL, url, folder)) == NULL) return E_OUTOFMEMORY;
    view->preview = TRUE;
    view->owner = owner;   /* only its window's owner: told nothing */
    view->told = TRUE;
    return OpenWindow(view, title);
}
