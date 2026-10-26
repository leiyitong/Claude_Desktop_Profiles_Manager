/*
 * Profiles dragged out of the list. The drag only finds where they go: it
 * carries a file of our own, empty and hidden (a marker), which the folder
 * it is dropped on copies at once; the shell's notice of that copy
 * (SHChangeNotify) names the folder, and the marker is taken away from it.
 * The notice comes as a message, after the drag is over: the window waits
 * for it as for any other, a few seconds at most. Then the person chooses
 * what the exports hold (Backup_ChooseItems), and the program makes them and
 * writes them there itself (transfer.c), its own window showing how far,
 * nothing of Explorer's copy.
 *
 * While they are dragged, tiles show over the foot of the list (drop
 * zones, a window of their own with its own drop target): Delete, Export,
 * Copy profile and Private sign-in. Let go on one, the drag only says which
 * to the window (DragOut_Profiles' `zoneMessage`), which does it; an export
 * there asks what it holds, then where it goes (DragOut_Export).
 *
 * Files dragged onto the window come through an OLE drop target of ours
 * (DragOut_AcceptFiles), not WM_DROPFILES: a drag of ours brought back over
 * the window is refused there, so that letting it go there cancels it.
 */
#define COBJMACROS
#include "app.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#define MARKER_PREFIX L"~ClaudeProfilesDrop-"
#define MARKERS_DIR   L"drag"            /* in the state folder: the marker a drag carries */
#define DROP_WAIT_MS  5000                /* a drop's notice comes at once; none by then: it was not on a folder */
#define WM_DROP_SEEN  (WM_APP + 1)
#define DROP_TIMER    1
#define ZONES_CLASS   L"ClaudeDesktopProfilesManager.DropZones"
#define ZONE_HEIGHT_DIPS 76               /* a tile: its icon over its words */
#define ZONE_GAP_DIPS    8

/* The tiles, in the order they show: what each says, its icon and tint. */
static const struct { const WCHAR *text; WCHAR glyph; ThemeTint tint; } kZones[DRAG_ZONES] = {
    { L"Export", 0xE898, THEME_TINT_BLUE },
    { L"Copy profile", 0xE8C8, THEME_TINT_TEAL },
    { L"Private sign-in", 0xE727, THEME_TINT_PURPLE },
    { L"Delete", 0xE74D, THEME_TINT_RED },
};

/* The tiles' window while profiles are dragged, and its drop target. */
typedef struct DropZones {
    IDropTarget        iface;
    LONG               refs;
    HWND               window;
    HFONT              font;              /* the owner's, not ours */
    IDropTargetHelper *helper;            /* the drag's image over the tiles too */
    RECT               tiles[DRAG_ZONES]; /* in the window */
    BOOL               ours;              /* a drag of ours is over it */
    int                hot;               /* the tile under the pointer, -1 for none */
    int                chosen;            /* the tile let go on, -1 for none */
} DropZones;

struct DragDrop {
    HWND  window;                         /* receives the shell's notices */
    ULONG notify;
    WCHAR marker[MAX_PATH];               /* the marker's own path */
    WCHAR name[MAX_PATH];                 /* its name, which the copy keeps */
    WCHAR folder[LONG_PATH_CCH];          /* where it was copied, once it was */
    BOOL  seen;
    /* A drag of the list's profiles, once it was let go: who is told where (`message`), and what it carries. */
    HWND         owner;
    UINT         message;
    ProfileList *list;                    /* the profiles as they were, a copy */
    DWORD        profiles;
};

BOOL DragOut_IsMarker(const WCHAR *path)
{
    const WCHAR *name = wcsrchr(path, L'\\');
    return wcsncmp(name ? name + 1 : path, MARKER_PREFIX, ARRAYSIZE(MARKER_PREFIX) - 1) == 0;
}

static LRESULT CALLBACK WatchProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    DragDrop *drop = (DragDrop *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_DROP_SEEN && drop) {
        PIDLIST_ABSOLUTE *items = NULL;
        LONG event = 0;
        HANDLE lock = SHChangeNotification_Lock((HANDLE)wp, (DWORD)lp, &items, &event);
        WCHAR path[LONG_PATH_CCH], *leaf;
        if (!lock) return 0;
        /* A copy of the marker: its folder is where the drop was, and the copy goes. */
        if (!drop->seen && (event & SHCNE_CREATE) && items && items[0] && SHGetPathFromIDListEx(items[0], path, ARRAYSIZE(path), GPFIDL_DEFAULT) &&
            (leaf = wcsrchr(path, L'\\')) != NULL && Core_EqualsI(leaf + 1, drop->name)) {
            DeleteFileW(path);
            *leaf = 0;
            if (leaf == path + 2) StringCchCatW(path, ARRAYSIZE(path), L"\\");   /* a drive's root */
            drop->seen = SUCCEEDED(StringCchCopyW(drop->folder, ARRAYSIZE(drop->folder), path));
            if (drop->seen && drop->owner) {
                KillTimer(window, DROP_TIMER);
                PostMessageW(drop->owner, drop->message, 0, (LPARAM)drop);
            }
        }
        SHChangeNotification_Unlock(lock);
        return 0;
    }
    if (message == WM_TIMER && wp == DROP_TIMER && drop) {
        /* No notice: it was let go elsewhere than in a folder. */
        KillTimer(window, DROP_TIMER);
        if (!drop->seen) DragOut_End(drop);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

DragDrop *DragOut_Begin(WCHAR *marker, size_t cch)
{
    static BOOL registered;
    WCHAR dir[MAX_PATH];
    SHChangeNotifyEntry entry;
    PIDLIST_ABSOLUTE desktop = NULL;
    DragDrop *drop;
    HANDLE file;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = WatchProc;
        wc.hInstance = g_hInst;
        wc.lpszClassName = L"ClaudeDesktopProfilesManager.DropWatch";
        registered = RegisterClassW(&wc) != 0;
    }
    if (!registered || (drop = (DragDrop *)Util_Alloc(sizeof *drop)) == NULL) return NULL;
    if (!Util_StateDir(dir, ARRAYSIZE(dir)) || !Util_JoinPath(dir, MARKERS_DIR, dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir) ||
        FAILED(StringCchPrintfW(drop->name, ARRAYSIZE(drop->name), MARKER_PREFIX L"%08lx-%08lx.tmp", GetCurrentProcessId(), GetTickCount())) ||
        !Util_JoinPath(dir, drop->name, drop->marker, ARRAYSIZE(drop->marker)) ||
        (file = CreateFileW(drop->marker, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL)) == INVALID_HANDLE_VALUE) {
        Util_Free(drop);
        return NULL;
    }
    CloseHandle(file);
    drop->window = CreateWindowExW(0, L"ClaudeDesktopProfilesManager.DropWatch", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, g_hInst, NULL);
    if (drop->window) SetWindowLongPtrW(drop->window, GWLP_USERDATA, (LONG_PTR)drop);
    if (drop->window && SUCCEEDED(SHGetKnownFolderIDList(&FOLDERID_Desktop, 0, NULL, &desktop))) {
        /* What the shell itself makes (a copy into a folder), anywhere below the desktop. */
        entry.pidl = desktop;
        entry.fRecursive = TRUE;
        drop->notify = SHChangeNotifyRegister(drop->window, SHCNRF_ShellLevel | SHCNRF_NewDelivery, SHCNE_CREATE, WM_DROP_SEEN, 1, &entry);
        CoTaskMemFree(desktop);
    }
    if (!drop->notify) {
        Util_Log(L"drag: the shell's notices cannot be had (error %lu)", GetLastError());
        DragOut_End(drop);
        return NULL;
    }
    if (marker) StringCchCopyW(marker, cch, drop->marker);
    return drop;
}

BOOL DragOut_Wait(DragDrop *drop, DWORD ms, WCHAR *folder, size_t cch)
{
    ULONGLONG until = GetTickCount64() + ms;
    MSG message;
    /* The notice comes as a message: the thread's messages handled until it came, or the time is out. */
    while (!drop->seen && GetTickCount64() < until) {
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!drop->seen) MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)max(1, min(until - GetTickCount64(), 100)), QS_ALLINPUT);
    }
    if (drop->seen && folder) StringCchCopyW(folder, cch, drop->folder);
    return drop->seen;
}

void DragOut_End(DragDrop *drop)
{
    if (!drop) return;
    if (drop->notify) SHChangeNotifyDeregister(drop->notify);
    if (drop->window) {
        SetWindowLongPtrW(drop->window, GWLP_USERDATA, 0);
        DestroyWindow(drop->window);
    }
    DeleteFileW(drop->marker);
    Util_Free(drop->list);
    Util_Free(drop);
}

/* What making a profile's export takes, on the copy's thread. */
typedef struct ExportMaking {
    const ProfileList  *list;
    const BackupChoice *choice;
} ExportMaking;

static BOOL MakeExport(void *context, const TransferItem *item, const WCHAR *source, WCHAR *error, size_t cch)
{
    const ExportMaking *making = (const ExportMaking *)context;
    return Backup_Write(making->list, item->data, making->choice, source, error, cch);
}

/* ---------------------------------------------------------------- drop zones */

/* `data` is a drag of ours: its file is a marker. */
static BOOL IsOurDrag(IDataObject *data)
{
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    WCHAR path[LONG_PATH_CCH];
    BOOL ours = FALSE;
    if (FAILED(IDataObject_GetData(data, &format, &medium))) return FALSE;
    ours = DragQueryFileW((HDROP)medium.hGlobal, 0, path, ARRAYSIZE(path)) && DragOut_IsMarker(path);
    ReleaseStgMedium(&medium);
    return ours;
}

static int ZoneAt(const DropZones *zones, POINTL pt)
{
    POINT at = { pt.x, pt.y };
    int i;
    ScreenToClient(zones->window, &at);
    for (i = 0; i < DRAG_ZONES; i++)
        if (PtInRect(&zones->tiles[i], at)) return i;
    return -1;
}

static void SetHot(DropZones *zones, int hot)
{
    if (zones->hot == hot) return;
    zones->hot = hot;
    InvalidateRect(zones->window, NULL, FALSE);
    UpdateWindow(zones->window);
}

static HRESULT STDMETHODCALLTYPE ZonesQueryInterface(IDropTarget *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = self;
        IDropTarget_AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ZonesAddRef(IDropTarget *self)
{
    return (ULONG)InterlockedIncrement(&((DropZones *)self)->refs);
}

static ULONG STDMETHODCALLTYPE ZonesRelease(IDropTarget *self)
{
    DropZones *zones = (DropZones *)self;
    LONG refs = InterlockedDecrement(&zones->refs);
    if (!refs) {
        if (zones->helper) IDropTargetHelper_Release(zones->helper);
        Util_Free(zones);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE ZonesDragEnter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    DropZones *zones = (DropZones *)self;
    POINT at = { pt.x, pt.y };
    (void)keys;
    zones->ours = IsOurDrag(data);
    SetHot(zones, zones->ours ? ZoneAt(zones, pt) : -1);
    *effect = zones->hot >= 0 && (*effect & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (zones->helper) IDropTargetHelper_DragEnter(zones->helper, zones->window, data, &at, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ZonesDragOver(IDropTarget *self, DWORD keys, POINTL pt, DWORD *effect)
{
    DropZones *zones = (DropZones *)self;
    POINT at = { pt.x, pt.y };
    (void)keys;
    SetHot(zones, zones->ours ? ZoneAt(zones, pt) : -1);
    *effect = zones->hot >= 0 && (*effect & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    if (zones->helper) IDropTargetHelper_DragOver(zones->helper, &at, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ZonesDragLeave(IDropTarget *self)
{
    DropZones *zones = (DropZones *)self;
    if (zones->helper) IDropTargetHelper_DragLeave(zones->helper);
    zones->ours = FALSE;
    SetHot(zones, -1);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ZonesDrop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    DropZones *zones = (DropZones *)self;
    POINT at = { pt.x, pt.y };
    (void)keys;
    zones->chosen = zones->ours ? ZoneAt(zones, pt) : -1;
    /* Nothing is copied: the drag ends as one cancelled, and the tile says what follows. */
    *effect = DROPEFFECT_NONE;
    if (zones->helper) IDropTargetHelper_Drop(zones->helper, data, &at, *effect);
    zones->ours = FALSE;
    SetHot(zones, -1);
    return S_OK;
}

static IDropTargetVtbl kZonesVtbl = {
    ZonesQueryInterface, ZonesAddRef, ZonesRelease, ZonesDragEnter, ZonesDragOver, ZonesDragLeave, ZonesDrop,
};

static LRESULT CALLBACK ZonesProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    DropZones *zones = (DropZones *)GetWindowLongPtrW(window, GWLP_USERDATA);
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        ThemeBuffer buffer;
        RECT client;
        HDC dc = BeginPaint(window, &paint);
        int i;
        GetClientRect(window, &client);
        if (dc && (dc = Theme_BufferBegin(&buffer, dc, &client)) != NULL) {
            FillRect(dc, &client, Theme_Brush(THEME_FACE));
            for (i = 0; zones && i < DRAG_ZONES; i++)
                Theme_DrawDropZone(window, dc, &zones->tiles[i], kZones[i].glyph, kZones[i].tint, TR(kZones[i].text), zones->font,
                                   i == zones->hot);
            Theme_BufferEnd(&buffer);
        }
        EndPaint(window, &paint);
        return 0;
    }
    }
    return DefWindowProcW(window, message, wp, lp);
}

/* The tiles over the foot of `source` (the list), shown without taking the focus; NULL when they cannot be. */
static DropZones *ShowZones(HWND owner, HWND source)
{
    static BOOL registered;
    DropZones *zones;
    RECT area;
    int dpi = (int)GetDpiForWindow(source), gap, width, height, i;
    if (!registered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof wc);
        wc.lpfnWndProc = ZonesProc;
        wc.hInstance = g_hInst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = ZONES_CLASS;
        registered = RegisterClassW(&wc) != 0;
    }
    if (!registered || !GetWindowRect(source, &area) || (zones = (DropZones *)Util_Alloc(sizeof *zones)) == NULL) return NULL;
    zones->iface.lpVtbl = &kZonesVtbl;
    zones->refs = 1;
    zones->hot = zones->chosen = -1;
    zones->font = (HFONT)SendMessageW(owner, WM_GETFONT, 0, 0);
    if (!dpi) dpi = 96;
    gap = MulDiv(ZONE_GAP_DIPS, dpi, 96);
    height = MulDiv(ZONE_HEIGHT_DIPS, dpi, 96) + 2 * gap;
    area.top = max(area.top, area.bottom - height);
    width = area.right - area.left;
    for (i = 0; i < DRAG_ZONES; i++) {
        zones->tiles[i].left = gap + (width - gap) * i / DRAG_ZONES;
        zones->tiles[i].right = (width - gap) * (i + 1) / DRAG_ZONES;
        zones->tiles[i].top = gap;
        zones->tiles[i].bottom = area.bottom - area.top - gap;
    }
    zones->window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, ZONES_CLASS, L"", WS_POPUP, area.left, area.top, width,
                                    area.bottom - area.top, owner, NULL, g_hInst, NULL);
    if (FAILED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDropTargetHelper, (void **)&zones->helper)))
        zones->helper = NULL;
    if (!zones->window || FAILED(RegisterDragDrop(zones->window, &zones->iface))) {
        if (zones->window) DestroyWindow(zones->window);
        IDropTarget_Release(&zones->iface);
        return NULL;
    }
    SetWindowLongPtrW(zones->window, GWLP_USERDATA, (LONG_PTR)zones);
    Theme_TitleBar(zones->window);
    ShowWindow(zones->window, SW_SHOWNOACTIVATE);
    UpdateWindow(zones->window);
    return zones;
}

/* The tiles gone; the one let go on, -1 for none. */
static int HideZones(DropZones *zones)
{
    int chosen;
    if (!zones) return -1;
    chosen = zones->chosen;
    RevokeDragDrop(zones->window);
    SetWindowLongPtrW(zones->window, GWLP_USERDATA, 0);
    DestroyWindow(zones->window);
    IDropTarget_Release(&zones->iface);
    return chosen;
}

/* A folder the person chooses for the exports; FALSE when none was. */
static BOOL ChooseFolder(HWND owner, WCHAR *folder, size_t cch)
{
    IFileOpenDialog *dialog = NULL;
    IShellItem *result = NULL;
    FILEOPENDIALOGOPTIONS options = 0;
    PWSTR chosen = NULL;
    BOOL ok = FALSE;
    HRESULT hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dialog);
    if (FAILED(hr)) return FALSE;
    IFileOpenDialog_SetTitle(dialog, TR(L"Choose where the exports go"));
    if (SUCCEEDED(IFileOpenDialog_GetOptions(dialog, &options)))
        IFileOpenDialog_SetOptions(dialog, options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (SUCCEEDED(hr = IFileOpenDialog_Show(dialog, owner)) && SUCCEEDED(hr = IFileOpenDialog_GetResult(dialog, &result))) {
        if (SUCCEEDED(hr = IShellItem_GetDisplayName(result, SIGDN_FILESYSPATH, &chosen))) {
            ok = SUCCEEDED(StringCchCopyW(folder, cch, chosen));
            CoTaskMemFree(chosen);
        }
        IShellItem_Release(result);
    }
    if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        Ui_Message(owner, MB_ICONERROR, TR(L"The folder could not be chosen (error 0x%08lX)."), (unsigned long)hr);
    IFileOpenDialog_Release(dialog);
    return ok;
}

static void ExportInto(HWND owner, const ProfileList *list, DWORD profiles, const WCHAR *folder);

void DragOut_Export(HWND owner, const ProfileList *list, DWORD profiles)
{
    ProfileList *copy = (ProfileList *)Util_Alloc(sizeof *copy);   /* as they are now: the list may be read again meanwhile */
    if (!copy) return;
    *copy = *list;
    ExportInto(owner, copy, profiles, NULL);
    Util_Free(copy);
}

/* ------------------------------------------------------------- dragging out */

void DragOut_Profiles(HWND owner, UINT message, UINT zoneMessage, HWND source, const ProfileList *list, DWORD profiles, POINT start)
{
    IDragSourceHelper *helper = NULL;
    IShellItem *item = NULL;
    IDataObject *data = NULL;
    DragDrop *drop;
    DropZones *zones;
    WCHAR marker[MAX_PATH];
    DWORD effect = 0;
    HRESULT hr;
    BOOL dropped = FALSE;
    int zone;
    if (!profiles || (drop = DragOut_Begin(marker, ARRAYSIZE(marker))) == NULL) return;
    zones = ShowZones(owner, source);
    if (SUCCEEDED(hr = SHCreateItemFromParsingName(marker, NULL, &IID_IShellItem, (void **)&item)) &&
        SUCCEEDED(hr = IShellItem_BindToHandler(item, NULL, &BHID_DataObject, &IID_IDataObject, (void **)&data))) {
        /* The rows dragged, as the drag's image. */
        if (SUCCEEDED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDragSourceHelper, (void **)&helper))) {
            IDragSourceHelper_InitializeFromWindow(helper, source, &start, data);
            IDragSourceHelper_Release(helper);
        }
        hr = SHDoDragDrop(owner, data, NULL, DROPEFFECT_COPY, &effect);
        dropped = hr == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY);
    }
    if (FAILED(hr)) Util_Log(L"profiles could not be dragged out (0x%08lX)", (unsigned long)hr);
    if (data) IDataObject_Release(data);
    if (item) IShellItem_Release(item);
    if ((zone = HideZones(zones)) >= 0) {   /* let go on a tile: the window does what it says */
        DragOut_End(drop);
        PostMessageW(owner, zoneMessage, (WPARAM)zone, 0);
        return;
    }
    /* Let go where something took it: the profiles as they are now (the list may be read again meanwhile), until the
     * shell's notice says where, or the time is out. */
    if (!dropped || (drop->list = (ProfileList *)Util_Alloc(sizeof *drop->list)) == NULL) {
        DragOut_End(drop);
        return;
    }
    *drop->list = *list;
    drop->profiles = profiles;
    drop->owner = owner;
    drop->message = message;
    if (drop->seen) PostMessageW(owner, message, 0, (LPARAM)drop);
    else if (!SetTimer(drop->window, DROP_TIMER, DROP_WAIT_MS, NULL)) DragOut_End(drop);
}

void DragOut_Dropped(HWND owner, LPARAM lp)
{
    DragDrop *drop = (DragDrop *)lp;
    ProfileList *list;
    DWORD profiles;
    WCHAR folder[LONG_PATH_CCH];
    if (!drop || !drop->list) return;
    /* Where they go is known, and the drag is over: what they hold is chosen, then they are made and written there. */
    list = drop->list;
    profiles = drop->profiles;
    drop->list = NULL;
    StringCchCopyW(folder, ARRAYSIZE(folder), drop->folder);
    DragOut_End(drop);
    SetForegroundWindow(owner);
    ExportInto(owner, list, profiles, folder);
    Util_Free(list);
}

/* The exports of `profiles` made and written into `folder` (NULL: chosen
 * once what they hold is), what they hold chosen first. */
static void ExportInto(HWND owner, const ProfileList *list, DWORD profiles, const WCHAR *folder)
{
    WCHAR chosen[LONG_PATH_CCH];
    TransferItem *items;
    BackupChoice choice;
    ExportMaking making;
    int i, count = 0;
    if (!Backup_ChooseItems(owner, list, profiles, &choice)) return;
    if (!folder) {
        if (!ChooseFolder(owner, chosen, ARRAYSIZE(chosen))) {
            Backup_Reopen(list, choice.reopen);
            Backup_FreeChoice(&choice);
            return;
        }
        folder = chosen;
    }
    if ((items = (TransferItem *)Util_Alloc(MAX_PROFILES * sizeof *items)) != NULL) {
        for (i = 0; i < list->count && count < MAX_PROFILES; i++) {
            if (!(profiles & (1u << i)) || !list->items[i].storageDir[0]) continue;
            Backup_ArchiveName(list->items[i].name, items[count].name, ARRAYSIZE(items[count].name));
            StringCchCopyW(items[count].folder, ARRAYSIZE(items[count].folder), folder);
            items[count++].data = i;
        }
        making.list = list;
        making.choice = &choice;
        Transfer_Run(owner, TR(L"Export"), items, count, MakeExport, &making);
        Util_Free(items);
    }
    Backup_Reopen(list, choice.reopen);   /* closed for their sign-in: open again */
    Backup_FreeChoice(&choice);
}

/* ------------------------------------------------------------ files dragged in */

typedef struct FileTarget {
    IDropTarget        iface;
    LONG               refs;
    HWND               window;
    UINT               message;
    IDropTargetHelper *helper;            /* what is dragged, its image over the window too */
    BOOL               taken;             /* files, none of them a drag's marker */
} FileTarget;

/* The files `data` holds (CF_HDROP), into `out` when there is one: none when one of them is a drag's marker. */
static int FilesOf(IDataObject *data, DroppedFiles *out)
{
    FORMATETC format = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medium;
    WCHAR path[LONG_PATH_CCH];
    UINT count, i;
    int n = 0;
    if (FAILED(IDataObject_GetData(data, &format, &medium))) return 0;
    count = DragQueryFileW((HDROP)medium.hGlobal, 0xFFFFFFFF, NULL, 0);
    for (i = 0; i < count; i++) {
        if (!DragQueryFileW((HDROP)medium.hGlobal, i, path, ARRAYSIZE(path))) continue;
        if (DragOut_IsMarker(path)) {
            n = 0;   /* a drag of ours */
            break;
        }
        if (out && n < DROPPED_FILES_MAX) StringCchCopyW(out->paths[n], ARRAYSIZE(out->paths[n]), path);
        n++;
    }
    ReleaseStgMedium(&medium);
    if (out) out->count = min(n, DROPPED_FILES_MAX);
    return n;
}

static DWORD TakenEffect(const FileTarget *target, DWORD allowed)
{
    return target->taken && (allowed & DROPEFFECT_COPY) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

static HRESULT STDMETHODCALLTYPE TargetQueryInterface(IDropTarget *self, REFIID riid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDropTarget)) {
        *out = self;
        IDropTarget_AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE TargetAddRef(IDropTarget *self)
{
    return (ULONG)InterlockedIncrement(&((FileTarget *)self)->refs);
}

static ULONG STDMETHODCALLTYPE TargetRelease(IDropTarget *self)
{
    FileTarget *target = (FileTarget *)self;
    LONG refs = InterlockedDecrement(&target->refs);
    if (!refs) {
        if (target->helper) IDropTargetHelper_Release(target->helper);
        Util_Free(target);
    }
    return (ULONG)refs;
}

static HRESULT STDMETHODCALLTYPE TargetDragEnter(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    FileTarget *target = (FileTarget *)self;
    POINT at = { pt.x, pt.y };
    (void)keys;
    target->taken = FilesOf(data, NULL) > 0;
    *effect = TakenEffect(target, *effect);
    if (target->helper) IDropTargetHelper_DragEnter(target->helper, target->window, data, &at, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE TargetDragOver(IDropTarget *self, DWORD keys, POINTL pt, DWORD *effect)
{
    FileTarget *target = (FileTarget *)self;
    POINT at = { pt.x, pt.y };
    (void)keys;
    *effect = TakenEffect(target, *effect);
    if (target->helper) IDropTargetHelper_DragOver(target->helper, &at, *effect);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE TargetDragLeave(IDropTarget *self)
{
    FileTarget *target = (FileTarget *)self;
    if (target->helper) IDropTargetHelper_DragLeave(target->helper);
    target->taken = FALSE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE TargetDrop(IDropTarget *self, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect)
{
    FileTarget *target = (FileTarget *)self;
    POINT at = { pt.x, pt.y };
    DroppedFiles *files;
    (void)keys;
    *effect = TakenEffect(target, *effect);
    if (target->helper) IDropTargetHelper_Drop(target->helper, data, &at, *effect);
    /* The window acts on them once the drag is over, the program they came from no longer waiting. */
    if (*effect != DROPEFFECT_NONE && (files = (DroppedFiles *)Util_Alloc(sizeof *files)) != NULL) {
        files->at = at;
        if (FilesOf(data, files) <= 0 || !PostMessageW(target->window, target->message, 0, (LPARAM)files)) {
            Util_Free(files);
            *effect = DROPEFFECT_NONE;
        }
    }
    target->taken = FALSE;
    return S_OK;
}

static IDropTargetVtbl kFileTargetVtbl = {
    TargetQueryInterface, TargetAddRef, TargetRelease, TargetDragEnter, TargetDragOver, TargetDragLeave, TargetDrop,
};

IDropTarget *DragOut_FileTarget(HWND window, UINT message)
{
    FileTarget *target = (FileTarget *)Util_Alloc(sizeof *target);
    if (!target) return NULL;
    target->iface.lpVtbl = &kFileTargetVtbl;
    target->refs = 1;
    target->window = window;
    target->message = message;
    if (FAILED(CoCreateInstance(&CLSID_DragDropHelper, NULL, CLSCTX_INPROC_SERVER, &IID_IDropTargetHelper, (void **)&target->helper)))
        target->helper = NULL;
    return &target->iface;
}

BOOL DragOut_AcceptFiles(HWND window, UINT message)
{
    IDropTarget *target = DragOut_FileTarget(window, message);
    HRESULT hr = target ? RegisterDragDrop(window, target) : E_OUTOFMEMORY;
    if (target) IDropTarget_Release(target);   /* the registration keeps it */
    if (FAILED(hr)) Util_Log(L"files dropped on the window cannot be taken (0x%08lX)", (unsigned long)hr);
    return SUCCEEDED(hr);
}

void DragOut_RefuseFiles(HWND window)
{
    RevokeDragDrop(window);
}
