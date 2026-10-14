/*
 * A list whose groups fold, with check boxes: what profiles sync (syncui.c)
 * and what a backup keeps (backup.c), alike. Each group is a semibold
 * heading with its image and its items below; a group without item rows is
 * a heading alone. A group's box checks all of it, or none once it all is
 * checked, and shows mixed when only some is. The groups start folded, but
 * those the person opened before (settings.ini). A box the list checks
 * itself (a click, Space) is undone, then turned over by the dialog
 * (WM_CHECKLIST_BOX, CheckList_Toggle), from what the list holds.
 */
#include "app.h"
#include <commctrl.h>

static BOOL Offered(const CheckList *checks, DWORD bits)
{
    return (checks->offered & bits) != 0;
}

/* Group `g` has item rows to show. */
static BOOL HasItems(const CheckList *checks, int g)
{
    int i;
    for (i = 0; i < checks->itemCount; i++)
        if ((checks->items[i].bit & checks->groups[g].mask) && Offered(checks, checks->items[i].bit)) return TRUE;
    return FALSE;
}

/* What a row's box stands for: a heading all of its group, an item its own bit. */
static DWORD RowMask(const CheckList *checks, LPARAM param)
{
    int g = THEME_HEADING_OF(param);
    if (param >= 0) return (DWORD)param & checks->offered;
    return g >= 0 && g < checks->groupCount ? checks->groups[g].mask & checks->offered : 0;
}

/* Each row's box as what is checked says: a group's checked when all of it is, mixed when some. */
static void ShowBoxes(CheckList *checks)
{
    int row, count = ListView_GetItemCount(checks->list);
    checks->filling = TRUE;
    for (row = 0; row < count; row++) {
        DWORD mask = RowMask(checks, Theme_RowParam(checks->list, row)), chosen = checks->checked & mask;
        ListView_SetItemState(checks->list, row, INDEXTOSTATEIMAGEMASK(chosen == mask ? 2 : chosen ? 3 : 1), LVIS_STATEIMAGEMASK);
    }
    checks->filling = FALSE;
}

static const WCHAR *ItemText(const CheckList *checks, const CheckItem *item)
{
    const WCHAR *text = checks->label ? checks->label(item, checks->context) : NULL;
    return text ? text : item->label ? TR(item->label) : L"";
}

/* Group `g`'s items, below its heading at `row`. */
static void InsertGroupItems(CheckList *checks, int row, int g)
{
    int i;
    for (i = 0; i < checks->itemCount; i++)
        if ((checks->items[i].bit & checks->groups[g].mask) && Offered(checks, checks->items[i].bit))
            Theme_InsertFoldingRow(checks->list, ++row, ItemText(checks, &checks->items[i]), checks->items[i].image, (LPARAM)checks->items[i].bit, 0);
}

/* Every row again: each group's heading, and its items when it is open. */
static void InsertRows(CheckList *checks)
{
    int g, row;
    checks->filling = TRUE;
    ListView_DeleteAllItems(checks->list);
    for (g = 0; g < checks->groupCount; g++) {
        int fold = !HasItems(checks, g) ? THEME_FOLD_NONE : checks->open[g] ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED;
        if (!Offered(checks, checks->groups[g].mask)) continue;
        row = ListView_GetItemCount(checks->list);
        Theme_InsertFoldingRow(checks->list, row, TR(checks->groups[g].heading), checks->groups[g].image, THEME_HEADING_ROW(g), fold);
        if (fold == THEME_FOLD_OPEN) InsertGroupItems(checks, row, g);
    }
    checks->filling = FALSE;
    ShowBoxes(checks);
}

void CheckList_Setup(CheckList *checks, HWND list, const WCHAR *glyphs, const ThemeTint *tints, int images)
{
    WCHAR opened[CHECKLIST_GROUPS_MAX][MAX_PATH];
    LVCOLUMNW column;
    HIMAGELIST states;
    int g, o, openedCount = checks->openedSection ? Util_GetSettingList(checks->openedSection, opened, CHECKLIST_GROUPS_MAX) : 0;
    checks->list = list;
    checks->checked &= checks->offered;
    if (checks->groupCount > CHECKLIST_GROUPS_MAX) checks->groupCount = CHECKLIST_GROUPS_MAX;
    ZeroMemory(checks->open, sizeof checks->open);
    for (g = 0; g < checks->groupCount; g++)
        for (o = 0; o < openedCount; o++)
            if (Core_EqualsI(opened[o], checks->groups[g].name)) checks->open[g] = TRUE;
    checks->filling = TRUE;
    ListView_SetExtendedListViewStyle(list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    if ((states = ListView_GetImageList(list, LVSIL_STATE)) != NULL) ImageList_SetImageCount(states, 3);   /* the third: some of a group */
    ListView_SetImageList(list, Theme_GlyphImages(list, glyphs, tints, images), LVSIL_SMALL);
    ZeroMemory(&column, sizeof column);
    ListView_InsertColumn(list, 0, &column);   /* the view it scrolls in gives it the list's width */
    Theme_SetFoldingRows(list);
    checks->filling = FALSE;
    InsertRows(checks);
}

/* The groups open, kept for the next time. */
static void KeepOpenedGroups(const CheckList *checks)
{
    WCHAR opened[CHECKLIST_GROUPS_MAX][MAX_PATH];
    int g, count = 0;
    if (!checks->openedSection) return;
    for (g = 0; g < checks->groupCount; g++)
        if (checks->open[g] && SUCCEEDED(StringCchCopyW(opened[count], ARRAYSIZE(opened[count]), checks->groups[g].name))) count++;
    if (!Util_SetSettingList(checks->openedSection, (const WCHAR (*)[MAX_PATH])opened, count))
        Util_Log(L"%s: the groups opened could not be kept (error %lu)", checks->openedSection, GetLastError());
}

/* Heading `row` opened or folded by the person. */
static void FoldGroup(CheckList *checks, int row, BOOL open)
{
    int g = THEME_HEADING_OF(Theme_RowParam(checks->list, row));
    if (g < 0 || g >= checks->groupCount || !HasItems(checks, g) || !checks->open[g] == !open) return;
    checks->open[g] = open;
    checks->filling = TRUE;
    Theme_SetFold(checks->list, row, open ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED);
    if (open) InsertGroupItems(checks, row, g);
    else Theme_DeleteFoldedRows(checks->list, row);
    checks->filling = FALSE;
    ShowBoxes(checks);
    KeepOpenedGroups(checks);
}

void CheckList_FoldAll(CheckList *checks, BOOL open)
{
    int g;
    for (g = 0; g < checks->groupCount; g++) checks->open[g] = open && HasItems(checks, g);
    InsertRows(checks);
    if (ListView_GetItemCount(checks->list)) Theme_FocusRow(checks->list, 0);
    KeepOpenedGroups(checks);
}

void CheckList_Toggle(CheckList *checks, int row)
{
    DWORD mask;
    if (row < 0 || row >= ListView_GetItemCount(checks->list)) return;
    mask = RowMask(checks, Theme_RowParam(checks->list, row));
    checks->checked = (checks->checked & mask) == mask ? checks->checked & ~mask : checks->checked | mask;
    ShowBoxes(checks);
}

BOOL CheckList_Message(CheckList *checks, HWND dialog, UINT message, WPARAM wp, LPARAM lp, BOOL *toggled)
{
    *toggled = FALSE;
    if (message == WM_CHECKLIST_BOX) {
        CheckList_Toggle(checks, (int)wp);
        *toggled = TRUE;
        return TRUE;
    }
    if (message == WM_CONTEXTMENU && ((HWND)wp == checks->list || (HWND)wp == GetParent(checks->list))) {
        UINT cmd = Theme_FoldMenu(dialog, checks->list, (HWND)wp, lp);
        if (cmd) CheckList_FoldAll(checks, cmd == THEME_FOLD_MENU_EXPAND);
        return TRUE;
    }
    return FALSE;
}

BOOL CheckList_Notify(CheckList *checks, HWND dialog, const NMHDR *header, LRESULT *result)
{
    *result = 0;
    if (header->hwndFrom != checks->list) return FALSE;
    if (header->code == LVN_ITEMCHANGING) {
        const NMLISTVIEW *change = (const NMLISTVIEW *)header;
        if (checks->filling || !(change->uChanged & LVIF_STATE) || !((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) return FALSE;
        PostMessageW(dialog, WM_CHECKLIST_BOX, (WPARAM)change->iItem, 0);
        *result = TRUE;
        return TRUE;
    }
    if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
        /* The arrow folds or opens a group; so does a double-click on its heading. */
        LVHITTESTINFO hit;
        ZeroMemory(&hit, sizeof hit);
        hit.pt = ((const NMITEMACTIVATE *)header)->ptAction;
        if (ListView_SubItemHitTest(checks->list, &hit) >= 0 && Theme_RowParam(checks->list, hit.iItem) < 0 &&
            (Theme_OnFoldArrow(checks->list, hit.iItem, hit.pt) || (header->code == NM_DBLCLK && !(hit.flags & LVHT_ONITEMSTATEICON)))) {
            int g = THEME_HEADING_OF(Theme_RowParam(checks->list, hit.iItem));
            if (g >= 0 && g < checks->groupCount) FoldGroup(checks, hit.iItem, !checks->open[g]);
        }
        return FALSE;
    }
    if (header->code == LVN_KEYDOWN) {
        /* Right opens a group, Left folds it, or goes from an item up to its group. */
        WORD key = ((const NMLVKEYDOWN *)header)->wVKey;
        int row = ListView_GetNextItem(checks->list, -1, LVNI_FOCUSED);
        if (row < 0 || (key != VK_LEFT && key != VK_RIGHT)) return FALSE;
        if (Theme_RowParam(checks->list, row) < 0) {
            FoldGroup(checks, row, key == VK_RIGHT);
        } else if (key == VK_LEFT) {
            while (row > 0 && Theme_RowParam(checks->list, row) >= 0) row--;
            Theme_FocusRow(checks->list, row);
        }
    }
    return FALSE;
}
