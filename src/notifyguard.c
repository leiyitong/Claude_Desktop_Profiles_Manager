/*
 * Notifications clicked in Windows' notification center.
 *
 * Every profile's Claude shows its notifications under the package's one
 * app id. A notification clicked while it is on screen goes to the Claude
 * that showed it; one clicked in the notification center, once that Claude
 * has let it go, makes Windows' notification service (WpnUserService) start
 * app\Claude.exe itself, without --user-data-dir: the default profile opens,
 * whichever profile showed the notification (measured on 2.31226: the
 * notification's launch text as the only argument, its parent the service's
 * svchost).
 *
 * Each watcher process runs this guard, one at a time (a named mutex hands
 * it on when that watcher ends). When the default profile's Claude starts
 * that way while it was closed and another profile runs, it is quit and the
 * profile the notification belongs to comes forward: the one other that
 * runs, or the one the user picks among several (IDD_LINK). Started from the
 * notification center while the default profile already runs, the click
 * only brings it forward: nothing tells whose notification it was then.
 */
#include "app.h"
#include <objbase.h>
#include <tlhelp32.h>

#define NOTIFY_GUARD_MUTEX   L"Local\\ClaudeDesktopProfilesManager.NotifyGuard"
#define NOTIFY_SERVICE       L"WpnUserService"   /* each user's instance is WpnUserService_<id> */
#define WM_GUARD_INSTANCE    (WM_APP + 1)        /* to the guard's thread: a Chrome_MessageWindow was made */

static DWORD g_guardThread;

/* A process made its Chromium single-instance window: a Claude starting. */
static void CALLBACK InstanceCreated(HWINEVENTHOOK hook, DWORD event, HWND window, LONG object, LONG child, DWORD thread, DWORD time)
{
    WCHAR windowClass[32];
    (void)hook;
    (void)event;
    (void)thread;
    (void)time;
    if (object != OBJID_WINDOW || child != CHILDID_SELF || !window || !GetClassNameW(window, windowClass, ARRAYSIZE(windowClass)) ||
        CompareStringOrdinal(windowClass, -1, L"Chrome_MessageWindow", -1, FALSE) != CSTR_EQUAL)
        return;
    PostThreadMessageW(g_guardThread, WM_GUARD_INSTANCE, (WPARAM)window, 0);
}

/* The parent of process `pid`, 0 when not known. */
static DWORD ParentOf(DWORD pid)
{
    PROCESSENTRY32W entry;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD parent = 0;
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    ZeroMemory(&entry, sizeof entry);
    entry.dwSize = sizeof entry;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ProcessID == pid) {
                parent = entry.th32ParentProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return parent;
}

/* Process `pid` hosts a user's instance of Windows' notification service. */
static BOOL IsNotificationService(DWORD pid)
{
    SC_HANDLE manager;
    BYTE *buffer = NULL;
    DWORD needed = 0, count = 0, resume = 0, i;
    BOOL found = FALSE;
    if (!pid || (manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE)) == NULL) return FALSE;
    EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_TYPE_ALL, SERVICE_ACTIVE, NULL, 0, &needed, &count, &resume, NULL);
    if (needed && (buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, needed)) != NULL &&
        EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_TYPE_ALL, SERVICE_ACTIVE, buffer, needed, &needed, &count, &resume, NULL)) {
        const ENUM_SERVICE_STATUS_PROCESSW *services = (const ENUM_SERVICE_STATUS_PROCESSW *)buffer;
        for (i = 0; i < count && !found; i++)
            found = services[i].ServiceStatusProcess.dwProcessId == pid &&
                    CompareStringOrdinal(services[i].lpServiceName, (int)wcslen(NOTIFY_SERVICE), NOTIFY_SERVICE, -1, TRUE) == CSTR_EQUAL;
    }
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    CloseServiceHandle(manager);
    return found;
}

/* A Claude made its single-instance window `window`: the default profile's,
 * started by the notification service, goes to the profile the notification
 * belongs to. */
static void InstanceStarted(HWND window)
{
    WCHAR title[MAX_PATH];
    BOOL running[MAX_PROFILES], identity = FALSE, ask = FALSE;
    ClaudePackage pkg;
    ProfileList list;
    DWORD pid = 0, error = 0;
    int stock, target, i;
    HRESULT hr;
    if (!GetWindowTextW(window, title, ARRAYSIZE(title)) || !GetWindowThreadProcessId(window, &pid) || !pid || !Claude_FindPackage(&pkg)) return;
    Profiles_Load(&list, &pkg);
    if ((stock = Profiles_Find(&list, STOCK_FOLDER)) < 0 || !Core_PathEquals(title, list.items[stock].dataDir)) return;
    if (!IsNotificationService(ParentOf(pid))) return;
    for (i = 0; i < list.count; i++) running[i] = list.items[i].running;
    target = Core_NotificationTarget(running, list.count, stock, Claude_TopmostProfile(&list), &ask);
    if (target < 0) {
        Util_Log(L"notification: opened %s, the only Claude open", list.items[stock].folder);
        return;
    }
    if (!Claude_Quit(&list.items[stock], &error))
        Util_Log(L"notification: %s, started by it, could not be quit (error %lu)", list.items[stock].folder, error);
    if (ask && (target = Router_ChooseNotification(&pkg, &list, target)) < 0) {
        Util_Log(L"notification: no profile chosen");
        return;
    }
    hr = Launcher_Open(&pkg, &list.items[target], NULL, NULL, &identity);
    Util_Log(L"notification: %s opened instead of %s (0x%08lX)", list.items[target].folder, list.items[stock].folder, (unsigned long)hr);
}

static DWORD WINAPI GuardThread(void *parameter)
{
    HANDLE mutex = CreateMutexW(NULL, FALSE, NOTIFY_GUARD_MUTEX);
    HWINEVENTHOOK hook;
    MSG message;
    HRESULT com;
    (void)parameter;
    if (!mutex) return 0;
    /* One watcher at a time: this one waits until the guard is its turn. */
    if (WaitForSingleObject(mutex, INFINITE) == WAIT_FAILED) {
        CloseHandle(mutex);
        return 0;
    }
    com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, NULL, InstanceCreated, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hook) Util_Log(L"notification guard: no window hook (error %lu)", GetLastError());
    while (hook && GetMessageW(&message, NULL, 0, 0) > 0) {
        if (message.message == WM_GUARD_INSTANCE) InstanceStarted((HWND)message.wParam);
        else DispatchMessageW(&message);
    }
    if (hook) UnhookWinEvent(hook);
    if (SUCCEEDED(com)) CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

void NotifyGuard_Start(void)
{
    HANDLE thread = CreateThread(NULL, 0, GuardThread, NULL, 0, &g_guardThread);
    if (thread) CloseHandle(thread);
    else Util_Log(L"notification guard: not started (error %lu)", GetLastError());
}
