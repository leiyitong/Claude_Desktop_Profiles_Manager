/*
 * Files the program writes where the person chose (profiles dragged out of
 * the list), copied by the program itself rather than by Explorer. Each is
 * made first where the program keeps them (the state folder's transfers\,
 * a folder each), then copied a chunk at a time into "<target>.part" beside
 * where it goes, hidden, and given its name once whole (Transfer_Copy). A
 * window shows how far it is, and pauses it, leaves it for later or cancels
 * it. What a close or an error left waits in transfers\ with what it is
 * (transfer.json): the next time the manager opens, it is offered again,
 * and the copy goes on from the bytes the part already holds.
 */
#include "app.h"
#include "resource.h"

#define TRANSFERS_DIR      L"transfers"
#define JOURNAL_NAME       L"transfer.json"
#define PART_SUFFIX        L".part"
#define CHUNK_BYTES        (1u << 20)
#define TAIL_BYTES         (64u * 1024u)   /* the part's last bytes, compared with the file's before the copy goes on */
#define JOURNAL_MAX_BYTES  (64u * 1024u)
#define TRANSFERS_MAX      64

#define WM_TRANSFER_STEP   (WM_APP + 90)   /* to the window: the copy moved on */
#define WM_TRANSFER_ENDED  (WM_APP + 91)   /* to the window: the thread ended, wParam a TRANSFER_* */

/* ------------------------------------------------------------ the copy */

static BOOL Extended(const WCHAR *path, WCHAR *out, size_t cch)
{
    return Util_ExtendedPath(path, out, cch);
}

/* The last `length` bytes before `at` of the two files are the same. */
static BOOL SameTail(HANDLE a, HANDLE b, ULONGLONG at, BYTE *bufferA, BYTE *bufferB)
{
    DWORD length = (DWORD)min((ULONGLONG)TAIL_BYTES, at), readA = 0, readB = 0;
    LARGE_INTEGER from;
    if (!length) return TRUE;
    from.QuadPart = (LONGLONG)(at - length);
    return SetFilePointerEx(a, from, NULL, FILE_BEGIN) && SetFilePointerEx(b, from, NULL, FILE_BEGIN) &&
           ReadFile(a, bufferA, length, &readA, NULL) && ReadFile(b, bufferB, length, &readB, NULL) && readA == length && readB == length &&
           memcmp(bufferA, bufferB, length) == 0;
}

TransferResult Transfer_Copy(const WCHAR *source, const WCHAR *target, TransferStepProc step, void *context, DWORD *code)
{
    WCHAR part[LONG_PATH_CCH], from[LONG_PATH_CCH], to[LONG_PATH_CCH], whole[LONG_PATH_CCH];
    HANDLE in = INVALID_HANDLE_VALUE, out = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size, held, at;
    ULONGLONG done = 0;
    BYTE *buffer = NULL, *check = NULL;
    TransferResult result = TRANSFER_FAILED;
    *code = 0;
    if (FAILED(StringCchPrintfW(part, ARRAYSIZE(part), L"%s" PART_SUFFIX, target)) || !Extended(source, from, ARRAYSIZE(from)) ||
        !Extended(part, to, ARRAYSIZE(to)) || !Extended(target, whole, ARRAYSIZE(whole))) {
        *code = ERROR_FILENAME_EXCED_RANGE;
        return TRANSFER_FAILED;
    }
    if ((buffer = (BYTE *)Util_Alloc(CHUNK_BYTES)) == NULL || (check = (BYTE *)Util_Alloc(TAIL_BYTES)) == NULL) {
        *code = ERROR_NOT_ENOUGH_MEMORY;
        goto done;
    }
    in = CreateFileW(from, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE || !GetFileSizeEx(in, &size)) {
        *code = GetLastError();
        goto done;
    }
    out = CreateFileW(to, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (out == INVALID_HANDLE_VALUE || !GetFileSizeEx(out, &held)) {
        *code = GetLastError();
        goto done;
    }
    /* What the part holds already is kept when it is the file's beginning: its end checked against the file's. */
    done = (ULONGLONG)held.QuadPart;
    if (done > (ULONGLONG)size.QuadPart || !SameTail(in, out, done, buffer, check)) done = 0;
    at.QuadPart = (LONGLONG)done;
    if (!SetFilePointerEx(in, at, NULL, FILE_BEGIN) || !SetFilePointerEx(out, at, NULL, FILE_BEGIN) || !SetEndOfFile(out)) {
        *code = GetLastError();
        goto done;
    }
    for (;;) {
        DWORD want, got = 0;
        if (step && !step(context, done, (ULONGLONG)size.QuadPart)) {
            result = TRANSFER_STOPPED;
            goto done;
        }
        if (done >= (ULONGLONG)size.QuadPart) break;
        want = (DWORD)min((ULONGLONG)CHUNK_BYTES, (ULONGLONG)size.QuadPart - done);
        if (!ReadFile(in, buffer, want, &got, NULL) || got != want) {
            *code = got != want && !GetLastError() ? ERROR_HANDLE_EOF : GetLastError();
            goto done;
        }
        if (!Util_WriteAll(out, buffer, got)) {
            *code = GetLastError();
            goto done;
        }
        done += got;
    }
    if (!FlushFileBuffers(out)) {
        *code = GetLastError();
        goto done;
    }
    CloseHandle(out);
    out = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(to, whole, MOVEFILE_WRITE_THROUGH)) {
        *code = GetLastError();
        goto done;
    }
    SetFileAttributesW(whole, FILE_ATTRIBUTE_NORMAL);
    result = TRANSFER_DONE;
done:
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    Util_Free(buffer);
    Util_Free(check);
    return result;
}

/* `folder`\`name`, or "<name> (2)" and on before its extension, when that or its part is taken. */
static BOOL FreeTarget(const WCHAR *folder, const WCHAR *name, WCHAR *out, size_t cch)
{
    WCHAR stem[MAX_PATH], candidate[MAX_PATH], part[LONG_PATH_CCH];
    const WCHAR *dot = wcsrchr(name, L'.');
    int n;
    if (FAILED(StringCchCopyNW(stem, ARRAYSIZE(stem), name, dot ? (size_t)(dot - name) : wcslen(name)))) return FALSE;
    for (n = 1; n < 1000; n++) {
        if (n == 1) StringCchCopyW(candidate, ARRAYSIZE(candidate), name);
        else StringCchPrintfW(candidate, ARRAYSIZE(candidate), L"%s (%d)%s", stem, n, dot ? dot : L"");
        if (!Util_JoinPath(folder, candidate, out, cch) || FAILED(StringCchPrintfW(part, ARRAYSIZE(part), L"%s" PART_SUFFIX, out))) return FALSE;
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(part) == INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------ the journal */

static BOOL TransfersDir(WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    return Util_StateDir(state, ARRAYSIZE(state)) && Util_JoinPath(state, TRANSFERS_DIR, out, cch);
}

/* Item `item`'s own folder in transfers\, made. */
static BOOL MakeItemDir(TransferItem *item)
{
    WCHAR dir[MAX_PATH], leaf[64];
    static LONG next;
    if (!TransfersDir(dir, ARRAYSIZE(dir)) || !Util_EnsureDir(dir) ||
        FAILED(StringCchPrintfW(leaf, ARRAYSIZE(leaf), L"%08lx-%08lx-%lu", GetCurrentProcessId(), GetTickCount(), (unsigned long)InterlockedIncrement(&next))) ||
        !Util_JoinPath(dir, leaf, item->dir, ARRAYSIZE(item->dir)))
        return FALSE;
    return Util_EnsureDir(item->dir);
}

/* What item `item` is, for the next time: its file's name, where it goes, its name there. */
static BOOL WriteJournal(const TransferItem *item)
{
    char name[MAX_PATH * 6 + 4], folder[LONG_PATH_CCH * 6 + 4], target[LONG_PATH_CCH * 6 + 4], json[sizeof name + sizeof folder + sizeof target + 64];
    WCHAR path[MAX_PATH];
    return Core_JsonQuote(item->name, name, sizeof name) && Core_JsonQuote(item->folder, folder, sizeof folder) &&
           Core_JsonQuote(item->target, target, sizeof target) &&
           SUCCEEDED(StringCchPrintfA(json, sizeof json, "{\"name\":%s,\"folder\":%s,\"target\":%s}", name, folder, target)) &&
           Util_JoinPath(item->dir, JOURNAL_NAME, path, ARRAYSIZE(path)) && Util_WriteFileAtomic(path, json, strlen(json), UTIL_PUT_REPLACE);
}

static BOOL ReadJournal(TransferItem *item)
{
    WCHAR path[MAX_PATH];
    DWORD length = 0;
    char *json;
    BOOL ok;
    if (!Util_JoinPath(item->dir, JOURNAL_NAME, path, ARRAYSIZE(path)) || (json = Util_ReadFile(path, JOURNAL_MAX_BYTES, FALSE, &length)) == NULL) return FALSE;
    ok = Core_JsonMemberString(json, length, "name", item->name, ARRAYSIZE(item->name)) && item->name[0] &&
         Core_JsonMemberString(json, length, "folder", item->folder, ARRAYSIZE(item->folder)) && item->folder[0] &&
         Core_JsonMemberString(json, length, "target", item->target, ARRAYSIZE(item->target)) && item->target[0];
    Util_Free(json);
    return ok;
}

/* Item `item` done with, or dropped (`drop`: its part taken away too). */
static void ForgetItem(const TransferItem *item, BOOL drop)
{
    WCHAR part[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    DWORD code = 0;
    if (drop && item->target[0] && SUCCEEDED(StringCchPrintfW(part, ARRAYSIZE(part), L"%s" PART_SUFFIX, item->target)) &&
        Extended(part, extended, ARRAYSIZE(extended)))
        DeleteFileW(extended);
    if (item->dir[0] && !Util_DeleteTree(item->dir, &code)) Util_Log(L"transfer: %s could not be removed (error %lu)", item->dir, code);
}

static BOOL ItemSource(const TransferItem *item, WCHAR *out, size_t cch)
{
    return Util_JoinPath(item->dir, item->name, out, cch);
}

/* ------------------------------------------------------------ the window */

typedef struct TransferJob {
    const WCHAR      *title;
    TransferItem     *items;
    int               count;
    TransferMakeProc  make;
    void             *context;
    HWND              dialog;
    HANDLE            thread, go, stop;   /* go: set while it runs, reset while paused; stop: set to end it */
    LONG volatile     current;            /* the item made or copied */
    LONG volatile     making;             /* its file being made, not copied yet */
    LONGLONG volatile done, total;        /* its bytes copied, of all */
    BOOL              paused, failed, discard;
    DWORD             code;
    WCHAR             error[512];
} TransferJob;

static BOOL Step(void *context, ULONGLONG done, ULONGLONG total)
{
    TransferJob *job = (TransferJob *)context;
    HANDLE wait[2];
    InterlockedExchange64(&job->done, (LONGLONG)done);
    InterlockedExchange64(&job->total, (LONGLONG)total);
    PostMessageW(job->dialog, WM_TRANSFER_STEP, 0, 0);
    wait[0] = job->stop;
    wait[1] = job->go;
    /* Paused: here until it goes on, or ends. */
    return WaitForMultipleObjects(2, wait, FALSE, INFINITE) != WAIT_OBJECT_0;
}

static DWORD WINAPI TransferThread(void *context)
{
    TransferJob *job = (TransferJob *)context;
    TransferResult result = TRANSFER_DONE;
    for (; job->current < job->count; InterlockedIncrement(&job->current)) {
        TransferItem *item = &job->items[job->current];
        WCHAR source[LONG_PATH_CCH];
        DWORD code = 0;
        if (WaitForSingleObject(job->stop, 0) == WAIT_OBJECT_0) {
            result = TRANSFER_STOPPED;
            break;
        }
        if ((!item->dir[0] && !MakeItemDir(item)) || !ItemSource(item, source, ARRAYSIZE(source))) {
            job->code = GetLastError();
            result = TRANSFER_FAILED;
            break;
        }
        if (GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
            /* Its file made first, what the window cannot pause. */
            InterlockedExchange(&job->making, 1);
            InterlockedExchange64(&job->done, 0);
            InterlockedExchange64(&job->total, 0);
            PostMessageW(job->dialog, WM_TRANSFER_STEP, 0, 0);
            job->error[0] = 0;
            if (!job->make || !job->make(job->context, item, source, job->error, ARRAYSIZE(job->error))) {
                if (!job->error[0]) StringCchCopyW(job->error, ARRAYSIZE(job->error), TR(L"The file to copy is gone."));
                DeleteFileW(source);
                result = TRANSFER_FAILED;
                break;
            }
            InterlockedExchange(&job->making, 0);
        }
        if (!item->target[0] && (!FreeTarget(item->folder, item->name, item->target, ARRAYSIZE(item->target)) || !WriteJournal(item))) {
            item->target[0] = 0;
            job->code = GetLastError() ? GetLastError() : ERROR_FILE_EXISTS;
            result = TRANSFER_FAILED;
            break;
        }
        result = Transfer_Copy(source, item->target, Step, job, &code);
        if (result != TRANSFER_DONE) {
            job->code = code;
            break;
        }
        ForgetItem(item, FALSE);
    }
    InterlockedExchange(&job->making, 0);
    PostMessageW(job->dialog, WM_TRANSFER_ENDED, (WPARAM)result, 0);
    return 0;
}

/* Windows' words for error `code`, without its line break. */
static void ErrorText(DWORD code, WCHAR *out, size_t cch)
{
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code, 0, out, (DWORD)cch, NULL);
    while (n && (out[n - 1] == L'\r' || out[n - 1] == L'\n' || out[n - 1] == L' ')) out[--n] = 0;
    if (!n) StringCchPrintfW(out, cch, TR(L"Error %lu."), (unsigned long)code);
}

/* What the window says: which file goes where, how far, and its buttons as the job is. */
static void ShowJob(HWND dialog, const TransferJob *job)
{
    WCHAR text[LONG_PATH_CCH + MAX_PATH + 64], done[32], total[32];
    int current = min((int)job->current, job->count - 1);
    const TransferItem *item = &job->items[max(current, 0)];
    if (job->count > 1)
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"%d of %d: \x201C%s\x201D to %s"), current + 1, job->count, item->name, item->folder);
    else
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"\x201C%s\x201D to %s"), item->name, item->folder);
    SetDlgItemTextW(dialog, IDC_TR_TEXT, text);
    if (job->failed) {
        WCHAR reason[256];
        if (job->error[0]) StringCchCopyW(reason, ARRAYSIZE(reason), job->error);
        else ErrorText(job->code, reason, ARRAYSIZE(reason));
        StringCchPrintfW(text, ARRAYSIZE(text), TR(L"Stopped: %s What was copied is kept; Retry goes on from there."), reason);
    } else if (job->making) {
        StringCchCopyW(text, ARRAYSIZE(text), TR(L"Making the file\x2026"));
    } else {
        Core_FormatSize((ULONGLONG)job->done, done, ARRAYSIZE(done));
        Core_FormatSize((ULONGLONG)job->total, total, ARRAYSIZE(total));
        StringCchPrintfW(text, ARRAYSIZE(text), job->paused ? TR(L"Paused at %s of %s") : TR(L"%s of %s"), done, total);
    }
    SetDlgItemTextW(dialog, IDC_TR_STATUS, text);
    SetDlgItemTextW(dialog, IDC_TR_PAUSE, job->failed ? TR(L"&Retry") : job->paused ? TR(L"&Continue") : TR(L"&Pause"));
    EnableWindow(GetDlgItem(dialog, IDC_TR_PAUSE), job->failed || !job->making);
    InvalidateRect(GetDlgItem(dialog, IDC_TR_PROGRESS), NULL, FALSE);
}

static BOOL StartThread(HWND dialog, TransferJob *job)
{
    job->failed = FALSE;
    job->paused = FALSE;
    job->code = 0;
    job->error[0] = 0;
    ResetEvent(job->stop);
    SetEvent(job->go);
    if ((job->thread = CreateThread(NULL, 0, TransferThread, job, 0, NULL)) == NULL) {
        job->failed = TRUE;
        job->code = GetLastError();
    }
    ShowJob(dialog, job);
    return job->thread != NULL;
}

/* The thread asked to end (Later, Cancel); the window ends once it has. */
static void StopThread(HWND dialog, TransferJob *job, BOOL discard)
{
    job->discard = discard;
    if (!job->thread) {
        EndDialog(dialog, discard ? IDCANCEL : IDC_TR_LATER);
        return;
    }
    SetEvent(job->stop);
    EnableWindow(GetDlgItem(dialog, IDC_TR_PAUSE), FALSE);
    EnableWindow(GetDlgItem(dialog, IDC_TR_LATER), FALSE);
    EnableWindow(GetDlgItem(dialog, IDCANCEL), FALSE);
}

static INT_PTR CALLBACK TransferProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    TransferJob *job = (TransferJob *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG:
        job = (TransferJob *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        job->dialog = dialog;
        SetWindowTextW(dialog, job->title);
        StartThread(dialog, job);
        return TRUE;

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_TR_STATUS);

    case WM_DRAWITEM: {
        const DRAWITEMSTRUCT *draw = (const DRAWITEMSTRUCT *)lp;
        if (!job || draw->CtlID != IDC_TR_PROGRESS) break;
        Theme_DrawProgress(dialog, draw->hDC, &draw->rcItem,
                           job->total > 0 ? (int)(job->done * 1000 / job->total) : 0, job->making || job->total <= 0 ? 0 : 1000, NULL);
        return TRUE;
    }

    case WM_TRANSFER_STEP:
        if (job) ShowJob(dialog, job);
        return TRUE;

    case WM_TRANSFER_ENDED:
        if (!job) return TRUE;
        if (job->thread) {
            WaitForSingleObject(job->thread, INFINITE);
            CloseHandle(job->thread);
            job->thread = NULL;
        }
        if (wp == TRANSFER_DONE) EndDialog(dialog, IDOK);
        else if (wp == TRANSFER_STOPPED) EndDialog(dialog, job->discard ? IDCANCEL : IDC_TR_LATER);
        else {
            job->failed = TRUE;
            ShowJob(dialog, job);
        }
        return TRUE;

    case WM_COMMAND:
        if (!job) break;
        switch (LOWORD(wp)) {
        case IDC_TR_PAUSE:
            if (job->failed) {
                StartThread(dialog, job);   /* Retry: from the item that stopped, its part kept */
            } else {
                job->paused = !job->paused;
                if (job->paused) ResetEvent(job->go);
                else SetEvent(job->go);
                ShowJob(dialog, job);
            }
            return TRUE;
        case IDC_TR_LATER:
            StopThread(dialog, job, FALSE);
            return TRUE;
        case IDCANCEL:
            if (HIWORD(wp) == BN_CLICKED && !Ui_Ask(dialog, IDI_QUESTION, TR(L"Cancel the export? What was copied is deleted."), TR(L"Cancel export"),
                                                    TR(L"Go on"), TRUE))
                return TRUE;
            StopThread(dialog, job, TRUE);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        if (job) StopThread(dialog, job, FALSE);   /* closed: left for later, as Later does */
        return TRUE;
    }
    return FALSE;
}

BOOL Transfer_Run(HWND owner, const WCHAR *title, TransferItem *items, int count, TransferMakeProc make, void *context)
{
    TransferJob job;
    INT_PTR result;
    int i;
    if (count <= 0) return TRUE;
    ZeroMemory(&job, sizeof job);
    job.title = title;
    job.items = items;
    job.count = count;
    job.make = make;
    job.context = context;
    job.go = CreateEventW(NULL, TRUE, TRUE, NULL);
    job.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!job.go || !job.stop) {
        if (job.go) CloseHandle(job.go);
        if (job.stop) CloseHandle(job.stop);
        return FALSE;
    }
    result = Ui_Dialog(owner, IDD_TRANSFER, TransferProc, (LPARAM)&job);
    if (job.thread) {
        SetEvent(job.stop);
        WaitForSingleObject(job.thread, INFINITE);
        CloseHandle(job.thread);
    }
    CloseHandle(job.go);
    CloseHandle(job.stop);
    /* Cancelled: what is not done goes, its part with it. Left for later: kept, made or not. */
    if (result == IDCANCEL)
        for (i = job.current; i < count; i++) ForgetItem(&items[i], TRUE);
    for (i = job.current; result != IDCANCEL && i < count; i++)
        if (!items[i].target[0] && items[i].dir[0]) ForgetItem(&items[i], FALSE);   /* not made whole yet: nothing to go on from */
    if (result != IDOK && result != IDCANCEL && result != IDC_TR_LATER) Util_Log(L"transfer window failed (error %lu)", GetLastError());
    return result == IDOK;
}

int Transfer_Left(TransferItem *items, int cap)
{
    WCHAR dir[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    int count = 0;
    if (!TransfersDir(dir, ARRAYSIZE(dir)) || (find = Util_FindFiles(dir, L"*", &found, TRUE)) == INVALID_HANDLE_VALUE) return 0;
    do {
        TransferItem item;
        WCHAR source[LONG_PATH_CCH];
        if (found.cFileName[0] == L'.') continue;
        ZeroMemory(&item, sizeof item);
        if (!Util_JoinPath(dir, found.cFileName, item.dir, ARRAYSIZE(item.dir))) continue;
        /* One without its journal, or without its file, was cut short before the copy began: nothing to go on with. */
        if (!ReadJournal(&item) || !ItemSource(&item, source, ARRAYSIZE(source)) || GetFileAttributesW(source) == INVALID_FILE_ATTRIBUTES) {
            ForgetItem(&item, item.target[0] != 0);
            continue;
        }
        if (count < cap) items[count++] = item;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return count;
}

void Transfer_ResumeLeft(HWND owner)
{
    TransferItem *items = (TransferItem *)Util_Alloc(TRANSFERS_MAX * sizeof *items);
    WCHAR question[256];
    int count, i;
    if (!items) return;
    if ((count = Transfer_Left(items, TRANSFERS_MAX)) > 0) {
        StringCchPrintfW(question, ARRAYSIZE(question), TR(L"%d exports were not finished. Go on with them now, from where they stopped?"), count);
        if (Ui_Ask(owner, IDI_QUESTION, question, TR(L"Continue"), TR(L"Discard"), FALSE)) {
            Transfer_Run(owner, TR(L"Export"), items, count, NULL, NULL);
        } else {
            for (i = 0; i < count; i++) ForgetItem(&items[i], TRUE);
        }
    }
    Util_Free(items);
}
