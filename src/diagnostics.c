/* Local, deliberately limited diagnostics. Credentials, profile labels,
 * paths, URLs and log bodies never enter the text copied for sharing. */
#include "app.h"
#include "resource.h"
#include "profiletools.h"
#include <shellapi.h>

typedef struct TextReview {
    const WCHAR *title, *note, *text, *action;
} TextReview;

static BOOL CopyText(HWND owner, const WCHAR *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    void *target = memory ? GlobalLock(memory) : NULL;
    if (!target) {
        if (memory) GlobalFree(memory);
        return FALSE;
    }
    CopyMemory(target, text, bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) {
        GlobalFree(memory);
        return FALSE;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        CloseClipboard();
        GlobalFree(memory);
        return FALSE;
    }
    CloseClipboard();
    return TRUE;
}

static INT_PTR CALLBACK TextProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    TextReview *review = (TextReview *)GetWindowLongPtrW(dialog, DWLP_USER);
    if (message == WM_INITDIALOG) {
        review = (TextReview *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetWindowTextW(dialog, review->title);
        SetDlgItemTextW(dialog, IDC_T_NOTE, review->note);
        SendDlgItemMessageW(dialog, IDC_T_TEXT, EM_SETLIMITTEXT, 1024 * 1024, 0);
        SetDlgItemTextW(dialog, IDC_T_TEXT, review->text);
        SetDlgItemTextW(dialog, IDOK, review->action ? review->action : TR(L"Close"));
        if (!review->action) ShowWindow(GetDlgItem(dialog, IDCANCEL), SW_HIDE);
        return TRUE;
    }
    if (message == WM_COMMAND && review) {
        if (LOWORD(wp) == IDC_T_COPY) {
            if (!CopyText(dialog, review->text)) Ui_Message(dialog, MB_ICONERROR, TR(L"The text could not be copied."));
            return TRUE;
        }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, LOWORD(wp));
            return TRUE;
        }
    }
    return FALSE;
}

BOOL Ui_ReviewText(HWND owner, const WCHAR *title, const WCHAR *note, const WCHAR *text, const WCHAR *action)
{
    TextReview review = { title, note, text, action };
    return Ui_Dialog(owner, IDD_TEXTVIEW, TextProc, (LPARAM)&review) == IDOK;
}

BOOL Diagnostics_Text(const ClaudePackage *pkg, const ProfileList *profiles, WCHAR *out, size_t cch)
{
    WCHAR line[256];
    int i;
    if (FAILED(StringCchPrintfW(out, cch, TR(L"Claude Desktop Profiles Manager %s\r\nClaude Desktop: %s\r\nProfiles: %d\r\n"),
                                L"" APP_VERSION_DATE, pkg->found ? pkg->version : TR(L"Not found"), profiles->count)))
        return FALSE;
    for (i = 0; i < profiles->count; i++) {
        StringCchPrintfW(line, ARRAYSIZE(line), TR(L"Profile %d: running=%d, storage_resolved=%d, sync_group=%d\r\n"), i + 1,
                         profiles->items[i].running, profiles->items[i].storageDir[0] != 0, profiles->items[i].syncGroup);
        if (FAILED(StringCchCatW(out, cch, line))) return FALSE;
    }
    return TRUE;
}

void Diagnostics_Show(HWND owner)
{
    ClaudePackage pkg;
    ProfileList profiles;
    WCHAR text[8192];
    Claude_FindPackage(&pkg);
    Profiles_Load(&profiles, &pkg);
    Claude_UpdateRunning(&profiles);
    if (!Diagnostics_Text(&pkg, &profiles, text, ARRAYSIZE(text))) return;
    if (Ui_ReviewText(owner, TR(L"Diagnostics"),
                  TR(L"Only versions and profile status are included. Account names, paths, links and log contents are omitted."), text, TR(L"Local profile tools\x2026")))
        Gui_ProfileTools(owner, &profiles, NULL);
}

void Diagnostics_OpenLogs(HWND owner)
{
    WCHAR folder[MAX_PATH];
    if (!Util_LogDirectory(folder, ARRAYSIZE(folder)) ||
        (INT_PTR)ShellExecuteW(owner, L"open", folder, NULL, NULL, SW_SHOWNORMAL) <= 32)
        Ui_Message(owner, MB_ICONERROR, TR(L"The log folder could not be opened."));
}
