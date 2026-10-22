#include "vaulttools.h"
#include <commctrl.h>
#include <string.h>
#include <wchar.h>

static void Free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
typedef struct ArchiveBuild { const WCHAR *id; char *out; size_t used, cap; BOOL ok; } ArchiveBuild;
static BOOL KeepArchived(void *context, const char *value, size_t length)
{
    ArchiveBuild *b = (ArchiveBuild *)context;
    WCHAR id[SESSION_ID_CCH];
    if (!Core_JsonString(value, length, id, ARRAYSIZE(id))) return b->ok = FALSE;
    if (Core_EqualsI(id, b->id)) return TRUE;
    if (b->used + length + 3 >= b->cap) return b->ok = FALSE;
    if (b->used > 1) b->out[b->used++] = ',';
    memcpy(b->out + b->used, value, length); b->used += length;
    return TRUE;
}
char *VaultTools_ArchiveSet(const char *json, size_t length, const WCHAR *id, BOOL archived, size_t *outLength)
{
    const char *array = "[]", *keys[1] = { "archived" };
    size_t arrayLength = 2;
    char quoted[SESSION_ID_CCH * 6], *out = NULL;
    ArchiveBuild b;
    *outLength = 0;
    if (!Core_JsonIsValue(json, length) || !id[0] || !Core_JsonQuote(id, quoted, sizeof quoted)) return NULL;
    Core_JsonMember(json, length, "archived", &array, &arrayLength);
    b.cap = length + strlen(quoted) + 8; b.used = 1; b.id = id; b.ok = TRUE;
    b.out = (char *)HeapAlloc(GetProcessHeap(), 0, b.cap);
    if (!b.out) return NULL;
    b.out[0] = '[';
    if (Core_JsonEachElement(array, arrayLength, KeepArchived, &b) && b.ok) {
        if (archived) {
            if (b.used > 1) b.out[b.used++] = ',';
            memcpy(b.out + b.used, quoted, strlen(quoted)); b.used += strlen(quoted);
        }
        b.out[b.used++] = ']'; b.out[b.used] = 0;
        out = Core_JsonSetNested(json, length, keys, 1, b.out, outLength);
    }
    Free(b.out); return out;
}

typedef struct PickState { const VaultRestoreItem *items; int count; BOOL *chosen, filling, syncGroup; HWND rows; } PickState;
static const WCHAR *Status(int status)
{
    switch (status) {
    case 0: return TR(L"New session"); case 1: return TR(L"Replace session entry"); case 2: return TR(L"Unchanged");
    case 3: return TR(L"Skip; session was deleted"); default: return TR(L"Content unavailable");
    }
}
static void ReadChecks(HWND dialog, PickState *s)
{
    int i, total = 0, visible = ListView_GetItemCount(s->rows), checked = 0, eligible = 0;
    WCHAR countText[128];
    for (i = 0; i < visible; i++) {
        LVITEMW item; ZeroMemory(&item, sizeof item); item.mask = LVIF_PARAM; item.iItem = i;
        if (ListView_GetItem(s->rows, &item) && item.lParam >= 0 && item.lParam < s->count) {
            if (s->items[item.lParam].status < 3) eligible++;
            s->chosen[item.lParam] = s->items[item.lParam].status < 3 && ListView_GetCheckState(s->rows, i);
            if (s->chosen[item.lParam]) checked++;
        }
    }
    for (i = 0; i < s->count; i++) if (s->chosen[i]) total++;
    EnableWindow(GetDlgItem(dialog, IDOK), total > 0);
    EnableWindow(GetDlgItem(dialog, IDC_VAULT_ALL), eligible > 0);
    CheckDlgButton(dialog, IDC_VAULT_ALL, eligible > 0 && checked == eligible ? BST_CHECKED : BST_UNCHECKED);
    StringCchPrintfW(countText, ARRAYSIZE(countText), TR(L"Selected: %d; shown: %d."), total, visible);
    SetDlgItemTextW(dialog, IDC_VAULT_COUNT, countText);
}
static void Fill(HWND dialog, PickState *s)
{
    WCHAR query[512]; int i, row = 0;
    GetDlgItemTextW(dialog, IDC_VAULT_QUERY, query, ARRAYSIZE(query));
    s->filling = TRUE; ListView_DeleteAllItems(s->rows);
    for (i = 0; i < s->count; i++) {
        LVITEMW item;
        if (query[0] && !Core_ContainsI(s->items[i].title, query) && !Core_ContainsI(s->items[i].key, query)) continue;
        ZeroMemory(&item, sizeof item); item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = row;
        item.pszText = (LPWSTR)s->items[i].title; item.lParam = i;
        ListView_InsertItem(s->rows, &item);
        ListView_SetItemText(s->rows, row, 1, (LPWSTR)Status(s->items[i].status));
        ListView_SetCheckState(s->rows, row, s->chosen[i]); row++;
    }
    s->filling = FALSE; ReadChecks(dialog, s);
}
static INT_PTR CALLBACK PickProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    PickState *s = (PickState *)GetWindowLongPtrW(dialog, DWLP_USER);
    int i;
    if (message == WM_INITDIALOG) {
        LVCOLUMNW column; RECT rect;
        s = (PickState *)lp; SetWindowLongPtrW(dialog, DWLP_USER, lp);
        SetDlgItemTextW(dialog, IDC_VAULT_NOTE, s->syncGroup
            ? TR(L"Only checked entries and their archived marks are recovered. Other sidebar settings stay as they are. Sync group members may follow. Deleted sessions stay deleted; transcript history is not restored.")
            : TR(L"Only checked entries and their archived marks are recovered. Other sidebar settings stay as they are. Deleted sessions stay deleted; transcript history is not restored."));
        s->rows = GetDlgItem(dialog, IDC_VAULT_ROWS);
        ListView_SetExtendedListViewStyle(s->rows, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        GetClientRect(s->rows, &rect); ZeroMemory(&column, sizeof column); column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = (LPWSTR)TR(L"Title"); column.cx = (rect.right - rect.left) * 2 / 3; ListView_InsertColumn(s->rows, 0, &column);
        column.pszText = (LPWSTR)TR(L"Status"); column.cx = (rect.right - rect.left) / 3; ListView_InsertColumn(s->rows, 1, &column);
        Fill(dialog, s); Theme_SmoothView(s->rows); return TRUE;
    }
    if (!s) return FALSE;
    if (message == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == s->rows && ((NMHDR *)lp)->code == LVN_ITEMCHANGED && !s->filling) {
        const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
        if ((change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) {
            s->filling = TRUE;
            if (change->iItem >= 0) {
                LVITEMW item; ZeroMemory(&item, sizeof item); item.mask = LVIF_PARAM; item.iItem = change->iItem;
                if (ListView_GetItem(s->rows, &item) && item.lParam >= 0 && item.lParam < s->count && s->items[item.lParam].status >= 3)
                    ListView_SetCheckState(s->rows, change->iItem, FALSE);
            }
            s->filling = FALSE; ReadChecks(dialog, s);
        }
        return TRUE;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wp)) {
        case IDC_VAULT_QUERY: if (HIWORD(wp) == EN_CHANGE) { ReadChecks(dialog, s); Fill(dialog, s); } return TRUE;
        case IDC_VAULT_ALL:
            s->filling = TRUE;
            for (i = 0; i < ListView_GetItemCount(s->rows); i++) ListView_SetCheckState(s->rows, i, IsDlgButtonChecked(dialog, IDC_VAULT_ALL) == BST_CHECKED);
            s->filling = FALSE; ReadChecks(dialog, s); Fill(dialog, s); return TRUE;
        case IDOK: ReadChecks(dialog, s); EndDialog(dialog, IDOK); return TRUE;
        case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
        }
    }
    return FALSE;
}
BOOL VaultTools_Choose(HWND owner, const VaultRestoreItem *items, int count, BOOL syncGroup, BOOL *chosen)
{
    PickState s; ZeroMemory(&s, sizeof s); s.items = items; s.count = count; s.chosen = chosen; s.syncGroup = syncGroup;
    return Ui_Dialog(owner, IDD_VAULT_PICK, PickProc, (LPARAM)&s) == IDOK;
}
