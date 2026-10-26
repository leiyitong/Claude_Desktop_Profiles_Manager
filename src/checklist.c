/*
 * A list whose groups fold, with check boxes and values: a property list.
 * What profiles sync (syncui.c), what an export or an import holds and a
 * profile's own (backup.c), a profile's settings (gui.c) and the conflicts
 * settled (syncui.c, backup.c) are all this list. Each group is a semibold
 * heading with its image, its items and values below; a group without rows
 * is a heading alone. A group's box checks all of it, or none once it all is
 * checked, and shows mixed when only some is; so does an item whose values
 * have boxes. The groups start folded, but those the person opened before
 * (settings.ini). A box the list checks itself (a click, Space) is undone,
 * then turned over by the dialog (WM_CHECKLIST_BOX, CheckList_Toggle), from
 * what the list holds.
 *
 * Values (CheckList.values) sit under their group or their item, or on
 * their item's row (`merged`), in the value column: a switch is a check box
 * in its cell (THEME_CELL_BOX); a choice or a text is changed in place, in a
 * drop-down or an edit box over that cell while its row is selected
 * (Theme_PlaceCellEditor), written as it changes. A pick's versions stand
 * side by side, a value column each, the one kept checked: a click on
 * another, or Left and Right, keeps that one. A change tells the dialog
 * (WM_CHECKLIST_VALUE). A value read from JSON shows its parts (an object's
 * members, a list's elements) as values of their own below it, folded at
 * first (CheckList_AddParts), and is written back with what changed in
 * them, those left out removed (CheckList_JsonOf).
 *
 * A dialog with a pane beside the list (IDC_PANE_LIST) shows there what the
 * row selected holds: a heading's items and values, an item's values, a
 * value's parts; a row with nothing below it, itself. The list beside it is
 * an overview: its items show how many values they hold (or their only
 * one), its values how many parts, and the pane shows them, items folding
 * there to their values and values to their parts. A group's own values
 * stay below its heading (the profile's own fields, changed in place). The
 * pane is a list of its own over the same values (`pane`), folded apart
 * from the list, changed there as in the list; each shows what the other
 * changed. Its search (IDC_PANE_SEARCH, Ctrl+F shows or hides it) keeps
 * the rows whose names or values match, and those that hold them.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>

#define CHECKLIST_SUBCLASS 0x434B   /* the list's own messages: its editor's, its size */
#define OWNER_SUBCLASS     0x434C   /* the pane's dialog: its search's messages */
#define EDITOR_ID          1
#define VALUE_SHARE        45       /* one value column changed in place: its share of the width, in percent */
#define VALUE_SHARE_LEAST  25       /* one only shown: as wide as its widest text, at least this share */
#define PICK_ROOM_DIPS     20       /* around a version's text and image */
#define SEARCH_CCH         128

/* The pane's search: what is typed, where it looks, and which values it keeps. */
typedef struct CheckSearch {
    HWND       edit, options;
    WCHAR      text[SEARCH_CCH];
    DWORD      in;                  /* CHECK_SEARCH_* */
    BOOL       shown;
    CoreRegex *regex;               /* the text as a regular expression, compiled; NULL: not one */
    BYTE      *marks;               /* each value: MARK_HERE when it matches, MARK_BELOW when a part of it does */
    int        markCount;
    RECT       listPlace;           /* the pane's view, in the dialog, while the search shows */
} CheckSearch;
#define MARK_HERE  0x1
#define MARK_BELOW 0x2

/* `text` holds the search: as a regular expression, or as typed, its case as the options say. */
static BOOL TextMatches(const CheckSearch *search, const WCHAR *text)
{
    if (!text || !text[0] || !search->text[0]) return FALSE;
    if (search->in & CHECK_SEARCH_REGEX) return search->regex && Core_RegexSearch(search->regex, text, wcslen(text));
    return (search->in & CHECK_SEARCH_CASE) ? wcsstr(text, search->text) != NULL : Core_ContainsI(text, search->text);
}

static BOOL Offered(const CheckList *checks, DWORD bits)
{
    return (checks->offered & bits) != 0;
}

static BOOL IsValueRow(LPARAM param)
{
    return param >= CHECK_VALUE_BASE;
}

/* A value row's value, -1 for another row. */
static int ValueOfRow(const CheckList *checks, LPARAM param)
{
    return IsValueRow(param) && param - CHECK_VALUE_BASE < checks->valueCount ? (int)(param - CHECK_VALUE_BASE) : -1;
}

/* The value `v` is a part of, -1 for none. */
static int ParentOf(const CheckList *checks, int v)
{
    return checks->values[v].parent - 1;
}

/* Value `w` is a part of `v`, or a part of one of its parts. */
static BOOL IsPartOf(const CheckList *checks, int w, int v)
{
    int p;
    for (p = ParentOf(checks, w); p >= 0; p = ParentOf(checks, p))
        if (p == v) return TRUE;
    return FALSE;
}

/* A value's parts come right after it: the first value past them. */
static int PartsEnd(const CheckList *checks, int v)
{
    int end = v + 1;
    while (end < checks->valueCount && IsPartOf(checks, end, v)) end++;
    return end;
}

static BOOL HasParts(const CheckList *checks, int v)
{
    return v + 1 < checks->valueCount && checks->values[v + 1].parent == v + 1;
}

static int Depth(const CheckList *checks, int v)
{
    int depth = 0, p;
    for (p = ParentOf(checks, v); p >= 0; p = ParentOf(checks, p)) depth++;
    return depth;
}

/* How far in value `v`'s row is in the list: below its item, its group, or none, and the values it is a part of. */
static int IndentOf(const CheckList *checks, int v)
{
    const CheckValue *value = &checks->values[v];
    return (value->group < 0 ? 1 : value->item ? 3 : 2) + Depth(checks, v);
}

/* Whether value `v`'s parts show below it, in this list (the pane folds apart). */
static BOOL IsOpen(const CheckList *checks, int v)
{
    return checks->twin ? checks->values[v].paneOpen : checks->values[v].open;
}

static void SetOpen(CheckList *checks, int v, BOOL open)
{
    if (checks->twin) checks->values[v].paneOpen = open;
    else checks->values[v].open = open;
}

/* Value `v`'s box: 0 none, 1 not taken, 2 taken, 3 some of its parts taken. */
static int ValueBox(const CheckList *checks, int v)
{
    int w, end, full = 0, empty = 0;
    if (!checks->values[v].boxed) return 0;
    if (!HasParts(checks, v)) return checks->values[v].taken ? 2 : 1;
    for (w = v + 1, end = PartsEnd(checks, v); w < end; w++)
        if (!HasParts(checks, w) && checks->values[w].boxed) {
            full += checks->values[w].taken != FALSE;
            empty += !checks->values[w].taken;
        }
    if (!full && !empty) return checks->values[v].taken ? 2 : 1;
    return !empty ? 2 : !full ? 1 : 3;
}

/* Value `v` and its parts taken or not (`on`); those it is a part of follow. */
static void TakeValue(CheckList *checks, int v, BOOL on)
{
    int w, end, p;
    for (w = v, end = PartsEnd(checks, v); w < end; w++)
        if (checks->values[w].boxed) checks->values[w].taken = on;
    for (p = ParentOf(checks, v); p >= 0; p = ParentOf(checks, p)) checks->values[p].taken = ValueBox(checks, p) != 1;
}

BOOL CheckList_IsLeftOut(const CheckList *checks, int v)
{
    return v >= 0 && v < checks->valueCount && ValueBox(checks, v) == 1;
}

/* The value item `bit`'s own row shows (`merged`), -1 for none. */
static int MergedValue(const CheckList *checks, DWORD bit)
{
    int i;
    for (i = 0; bit && i < checks->valueCount; i++)
        if (checks->values[i].item == bit && checks->values[i].merged) return i;
    return -1;
}

/* The value group `g`'s heading shows: its own, `merged`; -1 for none. */
static int HeadingValue(const CheckList *checks, int g)
{
    int i;
    for (i = 0; g >= 0 && i < checks->valueCount; i++)
        if (checks->values[i].group == g && !checks->values[i].item && checks->values[i].merged) return i;
    return -1;
}

/* The value a row shows: a value row's, its item's merged one, or its group's on its heading; -1 for none. */
static int RowValue(const CheckList *checks, LPARAM param)
{
    if (IsValueRow(param)) return ValueOfRow(checks, param);
    if (param < 0) return HeadingValue(checks, THEME_HEADING_OF(param));
    return param > 0 ? MergedValue(checks, (DWORD)param) : -1;
}

/* The value columns after the labels': a pick list's own, else one when there are values. */
static int ValueColumns(const CheckList *checks)
{
    if (checks->columns && checks->columnCount > 1) return min(checks->columnCount, CHECKLIST_COLUMNS) - 1;
    return checks->valueCount ? 1 : 0;
}

/* Whether value `value` can change now: not read-only, its box checked, or
 * that of its item, or of its group when it is the group's own. */
static BOOL Enabled(const CheckList *checks, const CheckValue *value)
{
    DWORD mask;
    if (value->readOnly) return FALSE;
    if (value->boxed) return value->taken;
    if (value->item) return (checks->checked & value->item) != 0;
    mask = value->group >= 0 && value->group < checks->groupCount ? checks->groups[value->group].mask & checks->offered : 0;
    return !mask || (checks->checked & mask) != 0;
}

/* A choice or a text, changed in an editor. */
static BOOL Edited(const CheckValue *value)
{
    return !value->readOnly && (value->kind == CHECK_VALUE_CHOICE || value->kind == CHECK_VALUE_TEXT);
}

static const WCHAR *LabelOf(const CheckValue *value)
{
    if (!value->label) return L"";
    return value->literal ? value->label : TR(value->label);
}

static BOOL ItemShown(const CheckList *checks, const CheckItem *item, int g)
{
    return (item->bit & checks->groups[g].mask) && Offered(checks, item->bit);
}

static BOOL GroupHasValues(const CheckList *checks, int g)
{
    int i;
    for (i = 0; i < checks->valueCount; i++)
        if (checks->values[i].group == g) return TRUE;
    return FALSE;
}

/* Group `g` has rows to show below its heading: items, or values not on its heading (an overview: its own only). */
static BOOL HasRows(const CheckList *checks, int g)
{
    int i;
    for (i = 0; i < checks->itemCount; i++)
        if (ItemShown(checks, &checks->items[i], g)) return TRUE;
    for (i = 0; i < checks->valueCount; i++)
        if (checks->values[i].group == g && !(checks->values[i].merged && !checks->values[i].item) &&
            !(checks->overview && (checks->values[i].item || checks->values[i].parent)))
            return TRUE;
    return FALSE;
}

/* The item of bit `bit`, -1 for none. */
static int ItemIndex(const CheckList *checks, DWORD bit)
{
    int i;
    for (i = 0; bit && i < checks->itemCount; i++)
        if (checks->items[i].bit == bit) return i;
    return -1;
}

/* Item `bit`'s values on rows of their own (not on its row), counted. */
static int ItemRows(const CheckList *checks, DWORD bit)
{
    int v, n = 0;
    for (v = 0; bit && v < checks->valueCount; v++)
        n += checks->values[v].item == bit && !checks->values[v].merged && !checks->values[v].parent;
    return n;
}

/* An item folds in the pane, to the values it holds. */
static BOOL ItemFolds(const CheckList *checks, DWORD bit)
{
    return checks->twin && ItemRows(checks, bit) > 0;
}

/* The pane's search keeps some rows only. */
static BOOL Filtering(const CheckList *checks)
{
    return checks->search && checks->search->text[0] && checks->search->marks && checks->search->markCount == checks->valueCount;
}

/* Value `v`'s row shows: always, but while the search keeps those that match and those holding them. */
static BOOL RowKept(const CheckList *checks, int v)
{
    return !Filtering(checks) || checks->search->marks[v] != 0;
}

static BOOL GroupShown(const CheckList *checks, int g)
{
    return Offered(checks, checks->groups[g].mask) || GroupHasValues(checks, g);
}

/* What a row's box stands for: a heading all of its group, an item its own bit, a value none. */
static DWORD RowMask(const CheckList *checks, LPARAM param)
{
    int g = THEME_HEADING_OF(param);
    if (IsValueRow(param)) return 0;
    if (param >= 0) return (DWORD)param & checks->offered;
    return g >= 0 && g < checks->groupCount ? checks->groups[g].mask & checks->offered : 0;
}

/* Item `bit`'s box: 1 none, 2 all, 3 some, of its values' boxes when it has
 * values with boxes, else its bit's. */
static int ItemBox(const CheckList *checks, DWORD bit)
{
    int i, full = 0, empty = 0, some = 0;
    for (i = 0; i < checks->valueCount; i++)
        if (checks->values[i].item == bit && checks->values[i].boxed && !checks->values[i].parent) {
            int box = ValueBox(checks, i);
            full += box == 2;
            empty += box == 1;
            some += box == 3;
        }
    if (!full && !empty && !some) return (checks->checked & bit) ? 2 : 1;
    return !some && !empty ? 2 : !some && !full ? 1 : 3;
}

/* Item `bit` checked when one of its values with a box is taken. */
static void FollowValues(CheckList *checks, DWORD bit)
{
    int box = ItemBox(checks, bit);
    if (!bit) return;
    checks->checked = box == 1 ? checks->checked & ~bit : checks->checked | bit;
}

/* Group `g`'s heading box: 0 none, 1 none checked, 2 all, 3 some. Without
 * values, as its mask says; with, as the boxes of its rows say. */
static int GroupBox(const CheckList *checks, int g)
{
    DWORD mask = checks->groups[g].mask & checks->offered, chosen = checks->checked & mask;
    int i, full = 0, empty = 0, some = 0;
    if (!GroupHasValues(checks, g)) return chosen == mask ? 2 : chosen ? 3 : 1;
    for (i = 0; i < checks->itemCount; i++)
        if (ItemShown(checks, &checks->items[i], g)) {
            int box = ItemBox(checks, checks->items[i].bit);
            full += box == 2;
            empty += box == 1;
            some += box == 3;
        }
    for (i = 0; i < checks->valueCount; i++)
        if (checks->values[i].group == g && !checks->values[i].item && checks->values[i].boxed && !checks->values[i].parent) {
            int box = ValueBox(checks, i);
            full += box == 2;
            empty += box == 1;
            some += box == 3;
        }
    if (!full && !empty && !some) return !mask ? 0 : chosen == mask ? 2 : chosen ? 3 : 1;   /* values without boxes: its own */
    return !some && !empty ? 2 : !some && !full ? 1 : 3;
}

/* The row value `v` is shown on, -1 when it is folded away. One on its item's
 * or its heading's row is a row of its own in a pane showing that row. */
static int RowOf(const CheckList *checks, int v)
{
    LVFINDINFOW find;
    int row;
    ZeroMemory(&find, sizeof find);
    find.flags = LVFI_PARAM;
    find.lParam = !checks->values[v].merged ? CHECK_VALUE_BASE + v
                  : checks->values[v].item ? (LPARAM)checks->values[v].item : THEME_HEADING_ROW(checks->values[v].group);
    row = ListView_FindItem(checks->list, -1, &find);
    if (row < 0 && checks->values[v].merged && checks->twin) {
        find.lParam = CHECK_VALUE_BASE + v;
        row = ListView_FindItem(checks->list, -1, &find);
    }
    return row;
}

/* Cell `column` of `row` set to `text` and `image`, when it shows something else. */
static void SetCell(HWND list, int row, int column, const WCHAR *text, int image)
{
    WCHAR shown[CHECK_VALUE_CCH];
    LVITEMW cell;
    ZeroMemory(&cell, sizeof cell);
    cell.mask = LVIF_TEXT | LVIF_IMAGE;
    cell.iItem = row;
    cell.iSubItem = column;
    cell.pszText = shown;
    cell.cchTextMax = ARRAYSIZE(shown);
    shown[0] = 0;
    cell.iImage = I_IMAGENONE;
    if (ListView_GetItem(list, &cell) && cell.iImage == image && wcscmp(shown, text) == 0) return;
    cell.pszText = (LPWSTR)text;
    cell.iImage = image;
    ListView_SetItem(list, &cell);
}

/* What value `v` shows in the value columns of `row`. */
static void ShowValue(const CheckList *checks, int row, int v)
{
    const CheckValue *value = &checks->values[v];
    WCHAR choice[CHECK_VALUE_CCH], *tab;
    int c, columns = ValueColumns(checks);
    for (c = 0; c < columns; c++) {
        const WCHAR *text = L"";
        int image = I_IMAGENONE;
        switch (value->kind) {
        case CHECK_VALUE_SWITCH:   /* its box, and what it says beside it */
            if (!c) {
                image = THEME_CELL_BOX + (value->on ? THEME_CELL_BOX_CHECKED : 0) + (Enabled(checks, value) ? 0 : THEME_CELL_BOX_DISABLED);
                text = value->text;
            }
            break;
        case CHECK_VALUE_CHOICE:
            /* What follows a tab is for its menu only (theme.c). */
            if (!c && value->choice >= 0 && value->choice < value->choiceCount) {
                StringCchCopyW(choice, ARRAYSIZE(choice), value->choices[value->choice]);
                if ((tab = wcschr(choice, L'\t')) != NULL) *tab = 0;
                text = choice;
            }
            break;
        case CHECK_VALUE_PICK:
            text = value->versions && value->versions[c] ? value->versions[c] : L"\x2014";
            if (value->versions && value->versions[c]) image = value->pick == c ? checks->pickedImage : checks->candidateImage;
            break;
        default:   /* a text, or what a box alone says of itself */
            if (!c) text = value->text;
            break;
        }
        SetCell(checks->list, row, c + 1, text, image);
    }
}

static void FollowSelection(CheckList *checks);
static void FoldGroup(CheckList *checks, int row, BOOL open);
static void ShowPane(CheckList *checks);
static void Mirror(CheckList *checks);
static void FoldValue(CheckList *checks, int row, int v, BOOL open);
static void FreePane(CheckList *pane);
static const WCHAR *ValueText(const CheckValue *value);

/* What every row's box and value come from, worked out at once from the
 * values up: ValueBox, ItemBox and GroupBox one by one go over the values
 * again for each row. */
typedef struct ListFacts {
    void *block;                           /* what holds the rest, one heap block */
    BYTE *valueBox;                        /* each value's box */
    BYTE *itemBox;                         /* each item's */
    int  *itemMerged, *itemRows;           /* each item's value on its row (-1: none), and its values on rows of their own */
    BYTE  groupBox[CHECKLIST_GROUPS_MAX];
    int   headingValue[CHECKLIST_GROUPS_MAX];
} ListFacts;

/* Boxes counted toward the box of what holds them: 2 all taken, 1 none, 3 some. */
static BYTE BoxOfCounts(int full, int empty, int some)
{
    return !some && !empty ? 2 : !some && !full ? 1 : 3;
}

static BOOL ReadFacts(const CheckList *checks, ListFacts *facts)
{
    int n = checks->valueCount, items = checks->itemCount, v, i, g, *full, *empty, *itemCounts, groupCounts[CHECKLIST_GROUPS_MAX][3];
    BOOL groupValues[CHECKLIST_GROUPS_MAX];
    BYTE *block;
    ZeroMemory(facts, sizeof *facts);
    block = (BYTE *)Util_Alloc((size_t)n * (1 + 2 * sizeof(int)) + (size_t)items * (1 + 5 * sizeof(int)) + 16);
    if (!block) return FALSE;
    facts->block = block;
    full = (int *)block;
    empty = full + n;
    facts->itemMerged = empty + n;
    facts->itemRows = facts->itemMerged + items;
    itemCounts = facts->itemRows + items;   /* three per item: full, empty, some */
    facts->valueBox = (BYTE *)(itemCounts + 3 * items);
    facts->itemBox = facts->valueBox + n;
    ZeroMemory(groupCounts, sizeof groupCounts);
    ZeroMemory(groupValues, sizeof groupValues);
    for (i = 0; i < items; i++) facts->itemMerged[i] = -1;
    for (g = 0; g < CHECKLIST_GROUPS_MAX; g++) facts->headingValue[g] = -1;
    /* The values from the last: a value's parts come right after it, so they are counted before it. */
    for (v = n - 1; v >= 0; v--) {
        const CheckValue *value = &checks->values[v];
        int parent = value->parent - 1;
        BOOL leaf = !HasParts(checks, v);
        if (!value->boxed) facts->valueBox[v] = 0;
        else if (leaf || (!full[v] && !empty[v])) facts->valueBox[v] = value->taken ? 2 : 1;
        else facts->valueBox[v] = BoxOfCounts(full[v], empty[v], 0);
        if (value->group >= 0 && value->group < CHECKLIST_GROUPS_MAX) groupValues[value->group] = TRUE;
        if (parent >= 0) {
            if (leaf && value->boxed) {
                full[parent] += value->taken != FALSE;
                empty[parent] += !value->taken;
            } else if (!leaf) {
                full[parent] += full[v];
                empty[parent] += empty[v];
            }
            continue;
        }
        if (value->item && (i = ItemIndex(checks, value->item)) >= 0) {
            if (value->merged) facts->itemMerged[i] = v;
            else facts->itemRows[i]++;
            if (value->boxed) itemCounts[3 * i + (facts->valueBox[v] == 2 ? 0 : facts->valueBox[v] == 1 ? 1 : 2)]++;
        } else if (!value->item && value->group >= 0 && value->group < CHECKLIST_GROUPS_MAX) {
            if (value->merged) facts->headingValue[value->group] = v;
            if (value->boxed) groupCounts[value->group][facts->valueBox[v] == 2 ? 0 : facts->valueBox[v] == 1 ? 1 : 2]++;
        }
    }
    for (i = 0; i < items; i++) {
        int *count = itemCounts + 3 * i;
        facts->itemBox[i] = !count[0] && !count[1] && !count[2] ? ((checks->checked & checks->items[i].bit) ? 2 : 1) : BoxOfCounts(count[0], count[1], count[2]);
    }
    for (g = 0; g < checks->groupCount && g < CHECKLIST_GROUPS_MAX; g++) {
        DWORD mask = checks->groups[g].mask & checks->offered, chosen = checks->checked & mask;
        int *count = groupCounts[g];
        if (!groupValues[g]) {
            facts->groupBox[g] = chosen == mask ? 2 : chosen ? 3 : 1;
            continue;
        }
        for (i = 0; i < items; i++)
            if (ItemShown(checks, &checks->items[i], g)) count[facts->itemBox[i] == 2 ? 0 : facts->itemBox[i] == 1 ? 1 : 2]++;
        if (!count[0] && !count[1] && !count[2]) facts->groupBox[g] = !mask ? 0 : chosen == mask ? 2 : chosen ? 3 : 1;   /* values without boxes: its own */
        else facts->groupBox[g] = BoxOfCounts(count[0], count[1], count[2]);
    }
    return TRUE;
}

/* Each row's box as what is checked says, and its value as it is: a
 * switch's look follows whether it can change; an item of an overview or a
 * pane without a value on its row says how many it holds. */
static void ShowBoxes(CheckList *checks)
{
    ListFacts facts;
    BOOL known = ReadFacts(checks, &facts);
    int row, count = ListView_GetItemCount(checks->list);
    checks->filling = TRUE;
    for (row = 0; row < count; row++) {
        LPARAM param = Theme_RowParam(checks->list, row);
        int v = -1, box = 0, g = THEME_HEADING_OF(param), i = -1;
        BOOL heading = param < 0 && g >= 0 && g < checks->groupCount;
        if (IsValueRow(param)) {
            v = ValueOfRow(checks, param);
            if (v >= 0) box = known ? facts.valueBox[v] : ValueBox(checks, v);
        } else if (heading) {
            v = known ? facts.headingValue[g] : HeadingValue(checks, g);
            box = known ? facts.groupBox[g] : GroupBox(checks, g);
        } else if (param > 0) {
            i = ItemIndex(checks, (DWORD)param);
            v = known && i >= 0 ? facts.itemMerged[i] : MergedValue(checks, (DWORD)param);
            box = known && i >= 0 ? facts.itemBox[i] : ItemBox(checks, (DWORD)param);
        }
        if (!checks->plain) ListView_SetItemState(checks->list, row, INDEXTOSTATEIMAGEMASK(box), LVIS_STATEIMAGEMASK);
        if (v >= 0) {
            ShowValue(checks, row, v);
        } else if (param > 0 && !IsValueRow(param) && (checks->overview || checks->twin) && ValueColumns(checks) > 0) {
            WCHAR held[64] = L"";
            int rows = known && i >= 0 ? facts.itemRows[i] : ItemRows(checks, (DWORD)param);
            if (rows) StringCchPrintfW(held, ARRAYSIZE(held), TR(L"%d items"), rows);
            SetCell(checks->list, row, 1, held, I_IMAGENONE);
        }
    }
    checks->filling = FALSE;
    if (checks->editor) EnableWindow(checks->editor, Enabled(checks, &checks->values[checks->editing]));
    if (known) Util_Free(facts.block);
}

static const WCHAR *ItemText(const CheckList *checks, const CheckItem *item)
{
    const WCHAR *text = checks->label ? checks->label(item, checks->context) : NULL;
    return text ? text : item->label ? TR(item->label) : L"";
}

/* Row `row` `indent` steps in, those a pane takes off taken off. */
static void SetIndent(const CheckList *checks, int row, int indent)
{
    LVITEMW item;
    ZeroMemory(&item, sizeof item);
    item.mask = LVIF_INDENT;
    item.iItem = row;
    item.iIndent = max(1, indent - checks->indentLess);
    ListView_SetItem(checks->list, &item);
}

/* Value `v`'s parts show below it: as it was left, or, while the search keeps some rows, when one of them is kept. */
static BOOL PartsShown(const CheckList *checks, int v)
{
    if (checks->overview || !HasParts(checks, v)) return FALSE;
    return Filtering(checks) ? (checks->search->marks[v] & MARK_BELOW) != 0 : IsOpen(checks, v);
}

/* Value `v` inserted at `row`, below its item, its group or the value it is
 * a part of, one step further in, and its parts below it while it is open
 * (an overview's never: the pane shows them): the last row it took. */
static int InsertValueRow(CheckList *checks, int row, int v)
{
    const CheckValue *value = &checks->values[v];
    int w, end, fold = !checks->overview && HasParts(checks, v) ? (PartsShown(checks, v) ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED) : 0;
    Theme_InsertFoldingRow(checks->list, row, LabelOf(value), value->image, CHECK_VALUE_BASE + v, fold);
    SetIndent(checks, row, IndentOf(checks, v));
    if (!PartsShown(checks, v)) return row;
    for (w = v + 1, end = PartsEnd(checks, v); w < end; w++)
        if (ParentOf(checks, w) == v && RowKept(checks, w)) row = InsertValueRow(checks, row + 1, w);
    return row;
}

/* Item `i`'s row shows: always, but while the search keeps some rows, when its name or one of its values is kept. */
static BOOL ItemKept(const CheckList *checks, int i)
{
    int v;
    if (!Filtering(checks)) return TRUE;
    if ((checks->search->in & CHECK_SEARCH_NAMES) && TextMatches(checks->search, ItemText(checks, &checks->items[i]))) return TRUE;
    for (v = 0; v < checks->valueCount; v++)
        if (checks->values[v].item == checks->items[i].bit && !checks->values[v].parent && checks->search->marks[v]) return TRUE;
    return FALSE;
}

/* Item `bit`'s values show below its row: never in an overview; in a pane, while it is open, or holds what the search keeps. */
static BOOL ItemValuesShown(const CheckList *checks, int i)
{
    DWORD bit = checks->items[i].bit;
    int v;
    if (checks->overview) return FALSE;
    if (!ItemFolds(checks, bit)) return TRUE;
    if (!Filtering(checks)) return (checks->openItems & bit) != 0;
    for (v = 0; v < checks->valueCount; v++)
        if (checks->values[v].item == bit && !checks->values[v].merged && !checks->values[v].parent && checks->search->marks[v]) return TRUE;
    return FALSE;
}

/* Item `i`'s values below its row at `row`: the last row they took. */
static int InsertItemValues(CheckList *checks, int row, int i)
{
    DWORD bit = checks->items[i].bit;
    int v;
    for (v = 0; v < checks->valueCount; v++)
        if (checks->values[v].item == bit && !checks->values[v].merged && !checks->values[v].parent && RowKept(checks, v))
            row = InsertValueRow(checks, row + 1, v);
    return row;
}

/* Group `g`'s items with their values, then its own values, below its heading at `row`: the last row they took. */
static int InsertGroupItems(CheckList *checks, int row, int g)
{
    int i, v;
    for (i = 0; i < checks->itemCount; i++) {
        DWORD bit = checks->items[i].bit;
        BOOL shown;
        if (!ItemShown(checks, &checks->items[i], g) || !ItemKept(checks, i)) continue;
        shown = ItemValuesShown(checks, i);
        Theme_InsertFoldingRow(checks->list, ++row, ItemText(checks, &checks->items[i]), checks->items[i].image, (LPARAM)bit,
                               ItemFolds(checks, bit) ? (shown ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED) : 0);
        SetIndent(checks, row, 2);
        if (shown) row = InsertItemValues(checks, row, i);
    }
    for (v = 0; v < checks->valueCount; v++)
        if (checks->values[v].group == g && !checks->values[v].item && !checks->values[v].merged && !checks->values[v].parent && RowKept(checks, v))
            row = InsertValueRow(checks, row + 1, v);
    return row;
}

/* Rows changed in bulk: the list draws, and its view measures it, once after
 * them (Theme_SmoothView keeps its place over a refill). */
static void DrawRowsLater(CheckList *checks, BOOL later)
{
    SendMessageW(checks->list, WM_SETREDRAW, !later, 0);
    if (!later) InvalidateRect(checks->list, NULL, FALSE);
}

/* Every row again: the values without a group, then each group's heading,
 * and its rows when it is open. */
static void InsertRows(CheckList *checks)
{
    int g, row, v;
    DrawRowsLater(checks, TRUE);
    checks->filling = TRUE;
    ListView_DeleteAllItems(checks->list);
    for (v = 0; v < checks->valueCount; v++)
        if (checks->values[v].group < 0 && !checks->values[v].parent) InsertValueRow(checks, ListView_GetItemCount(checks->list), v);
    for (g = 0; g < checks->groupCount; g++) {
        int fold = !HasRows(checks, g) ? THEME_FOLD_NONE : checks->open[g] ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED;
        if (!GroupShown(checks, g)) continue;
        row = ListView_GetItemCount(checks->list);
        Theme_InsertFoldingRow(checks->list, row, TR(checks->groups[g].heading), checks->groups[g].image, THEME_HEADING_ROW(g), fold);
        if (fold == THEME_FOLD_OPEN) InsertGroupItems(checks, row, g);
    }
    checks->filling = FALSE;
    ShowBoxes(checks);
    DrawRowsLater(checks, FALSE);
    FollowSelection(checks);
}

/* ------------------------------------------------------------ values */

/* What a value's cell says, its choice's or its text. */
static const WCHAR *ValueText(const CheckValue *value)
{
    if (value->kind == CHECK_VALUE_CHOICE) return value->choice >= 0 && value->choice < value->choiceCount ? value->choices[value->choice] : L"";
    return value->kind == CHECK_VALUE_PICK ? L"" : value->text;   /* a switch's: what it says beside its box */
}

/* The widest value column value `v` needs, in the list's font: all of
 * VALUE_SHARE when it is changed in place there. */
static int ValueWidth(const CheckList *checks, const CheckValue *value, int room, int image, int whole)
{
    if (Edited(value)) return whole;
    return ListView_GetStringWidth(checks->list, ValueText(value)) + room + (value->kind == CHECK_VALUE_SWITCH ? image : 0);
}

/* The columns across the list. One value column: VALUE_SHARE of the width
 * when a value is changed in place there, else as wide as its widest text
 * (VALUE_SHARE_LEAST at least, VALUE_SHARE at most), the labels the rest:
 * the values this list may show (an overview's: those on its rows, and how
 * many an item holds), or, for a pane, those its rows show now; without
 * any, the labels take it all. A pick list's: each as wide as its title and
 * widest version, the labels the rest, two fifths at least. */
static void SizeColumns(CheckList *checks)
{
    RECT client;
    int columns = ValueColumns(checks), wanted[CHECKLIST_COLUMNS], total = 0, c, v, dpi, room, image;
    if (!columns || !GetClientRect(checks->list, &client) || client.right <= 0) return;
    dpi = (int)GetDpiForWindow(checks->list);
    room = MulDiv(PICK_ROOM_DIPS, dpi, 96);
    image = GetSystemMetricsForDpi(SM_CXSMICON, (UINT)dpi);
    if (!checks->columns || checks->columnCount < 2) {
        int value = 0, row, rows = ListView_GetItemCount(checks->list);
        if (checks->twin) {
            for (row = 0; row < rows && value < client.right; row++) {
                WCHAR text[CHECK_VALUE_CCH] = L"";
                LPARAM param = Theme_RowParam(checks->list, row);
                v = RowValue(checks, param);
                if (v >= 0) {
                    value = max(value, ValueWidth(checks, &checks->values[v], room, image, client.right));
                } else if (param > 0) {
                    ListView_GetItemText(checks->list, row, 1, text, ARRAYSIZE(text));   /* how many an item holds */
                    if (text[0]) value = max(value, ListView_GetStringWidth(checks->list, text) + room);
                }
            }
        } else {
            for (v = 0; v < checks->valueCount && value < client.right; v++) {
                const CheckValue *shown = &checks->values[v];
                if (checks->overview && (shown->parent || (shown->item && !shown->merged))) continue;   /* the pane's */
                value = max(value, ValueWidth(checks, shown, room, image, client.right));
            }
            for (c = 0; checks->overview && c < checks->itemCount; c++) {
                WCHAR held[64];
                int n = ItemRows(checks, checks->items[c].bit);
                if (!n) continue;
                StringCchPrintfW(held, ARRAYSIZE(held), TR(L"%d items"), n);
                value = max(value, ListView_GetStringWidth(checks->list, held) + room);
            }
        }
        if (value) value = max(value, MulDiv(client.right, VALUE_SHARE_LEAST, 100));
        value = min(value, MulDiv(client.right, VALUE_SHARE, 100));
        Theme_SetColumnWidth(checks->list, 0, client.right - value);
        Theme_FitLastColumn(checks->list, 0);
        return;
    }
    for (c = 0; c < columns; c++) {
        wanted[c] = ListView_GetStringWidth(checks->list, checks->columns[c + 1]) + room;
        for (v = 0; v < checks->valueCount; v++)
            if (checks->values[v].kind == CHECK_VALUE_PICK && checks->values[v].versions && checks->values[v].versions[c])
                wanted[c] = max(wanted[c], ListView_GetStringWidth(checks->list, checks->values[v].versions[c]) + image + room);
        total += wanted[c];
    }
    if (total > client.right - client.right * 2 / 5) {
        int shared = 0;
        for (c = 0; c < columns; c++) shared += wanted[c] = MulDiv(wanted[c], client.right - client.right * 2 / 5, total);
        total = shared;
    }
    ListView_SetColumnWidth(checks->list, 0, client.right - total);
    for (c = 0; c < columns; c++) ListView_SetColumnWidth(checks->list, c + 1, wanted[c]);
}

/* Value `v` changed: its row shows it, the pane or its list too, and the dialog is told. */
static void Changed(CheckList *checks, int v)
{
    int row = RowOf(checks, v);
    if (row >= 0) ShowValue(checks, row, v);
    Mirror(checks);
    checks->changedIn = checks->list;
    if (checks->twin) checks->twin->changedIn = checks->list;
    if (checks->owner) SendMessageW(checks->owner, WM_CHECKLIST_VALUE, (WPARAM)v, 0);
}

static void CloseEditor(CheckList *checks)
{
    HWND editor = checks->editor;
    checks->editor = NULL;
    checks->editing = -1;
    if (editor) {
        if (GetFocus() == editor) SetFocus(checks->list);
        DestroyWindow(editor);
    }
}

/* The editor of value `v`, holding its value. */
static HWND OpenEditor(CheckList *checks, int v)
{
    const CheckValue *value = &checks->values[v];
    HWND editor;
    int i;
    if (value->kind == CHECK_VALUE_TEXT) {
        /* Framed as a dialog's edits are (the dialog manager gives them a client edge). */
        editor = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, value->text, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, checks->list,
                                 (HMENU)(INT_PTR)EDITOR_ID, g_hInst, NULL);
        if (editor) SendMessageW(editor, EM_LIMITTEXT, value->limit > 0 && value->limit < CHECK_VALUE_CCH ? value->limit : CHECK_VALUE_CCH - 1, 0);
        if (editor && value->hint) SendMessageW(editor, EM_SETCUEBANNER, TRUE, (LPARAM)value->hint);
    } else {
        editor = CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS, 0, 0, 0,
                                 0, checks->list, (HMENU)(INT_PTR)EDITOR_ID, g_hInst, NULL);
        for (i = 0; editor && i < value->choiceCount; i++) {
            LRESULT at = SendMessageW(editor, CB_ADDSTRING, 0, (LPARAM)value->choices[i]);
            if (at >= 0 && value->swatches) SendMessageW(editor, CB_SETITEMDATA, (WPARAM)at, (LPARAM)value->swatches[i] | THEME_CHOICE_SWATCH);
        }
        if (editor) SendMessageW(editor, CB_SETCURSEL, (WPARAM)value->choice, 0);
    }
    if (!editor) Util_Log(L"a list's value cannot be edited: no editor (error %lu)", GetLastError());
    return editor;
}

/* The selected row's editor over its value, when it is one changed in one; none else. */
static void FollowSelection(CheckList *checks)
{
    int row = ListView_GetNextItem(checks->list, -1, LVNI_SELECTED), v = row >= 0 ? RowValue(checks, Theme_RowParam(checks->list, row)) : -1;
    if (v >= 0 && !Edited(&checks->values[v])) v = -1;
    if (v != checks->editing) {
        CloseEditor(checks);
        if (v >= 0 && (checks->editor = OpenEditor(checks, v)) != NULL) checks->editing = v;
    }
    ShowPane(checks);
    if (!checks->editor) return;
    Theme_PlaceCellEditor(checks->list, row, 1, checks->editor);
    EnableWindow(checks->editor, Enabled(checks, &checks->values[checks->editing]));
}

/* What the editor holds, written to its value as it changes. */
static void TakeEdit(CheckList *checks, UINT notification)
{
    CheckValue *value = &checks->values[checks->editing];
    if (value->kind == CHECK_VALUE_TEXT && notification == EN_CHANGE) {
        GetWindowTextW(checks->editor, value->text, ARRAYSIZE(value->text));
    } else if (value->kind == CHECK_VALUE_CHOICE && notification == CBN_SELCHANGE) {
        LRESULT choice = SendMessageW(checks->editor, CB_GETCURSEL, 0, 0);
        if (choice < 0 || choice >= value->choiceCount || choice == value->choice) return;
        value->choice = (int)choice;
    } else {
        return;
    }
    Changed(checks, checks->editing);
}

/* A switch turned over, or an action asked of the dialog, when it can change. */
static void TurnSwitch(CheckList *checks, int v)
{
    CheckValue *value = &checks->values[v];
    if ((value->kind != CHECK_VALUE_SWITCH && value->kind != CHECK_VALUE_ACTION) || !Enabled(checks, value)) return;
    if (value->kind == CHECK_VALUE_SWITCH) value->on = !value->on;
    Changed(checks, v);
}

/* A pick keeps the version of value column `column`, when it has one. */
static void PickColumn(CheckList *checks, int v, int column)
{
    CheckValue *value = &checks->values[v];
    if (value->kind != CHECK_VALUE_PICK || column < 0 || column >= ValueColumns(checks) || !value->versions || !value->versions[column] ||
        value->pick == column)
        return;
    value->pick = column;
    Changed(checks, v);
}

/* A pick moved to the next version left or right (`step`). */
static void StepPick(CheckList *checks, int v, int step)
{
    const CheckValue *value = &checks->values[v];
    int column;
    for (column = value->pick + step; value->versions && column >= 0 && column < ValueColumns(checks); column += step)
        if (value->versions[column]) {
            PickColumn(checks, v, column);
            return;
        }
}

static LRESULT CALLBACK ListProc(HWND list, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    CheckList *checks = (CheckList *)ref;
    LRESULT result;
    switch (msg) {
    case WM_COMMAND:
        if (checks->editor && (HWND)lp == checks->editor) {
            if (!checks->following) TakeEdit(checks, HIWORD(wp));   /* not what the other list put there */
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORLISTBOX:
        /* The editor's colors are the dialog's, as its other fields' (through the list's view, when it has one). */
        if (checks->editor && (HWND)lp == checks->editor) return SendMessageW(GetParent(list), msg, wp, lp);
        break;
    case WM_SIZE:
        result = DefSubclassProc(list, msg, wp, lp);
        SizeColumns(checks);
        FollowSelection(checks);
        return result;
    case WM_NCDESTROY:
        RemoveWindowSubclass(list, ListProc, id);
        checks->editor = NULL;
        checks->editing = -1;
        if (checks->twin) FreePane(checks);   /* a pane goes with its list */
        break;
    }
    return DefSubclassProc(list, msg, wp, lp);
}

/* F2 on a value changed in an editor, or a click on it: into its editor, a choice's opened by the click. */
static void EnterEditor(CheckList *checks, BOOL open)
{
    if (!checks->editor || !IsWindowEnabled(checks->editor)) return;
    SetFocus(checks->editor);
    if (open && checks->values[checks->editing].kind == CHECK_VALUE_CHOICE) SendMessageW(checks->editor, CB_SHOWDROPDOWN, TRUE, 0);
}

/* ------------------------------------------------------------ JSON values */

static BOOL CountElement(void *context, const char *value, size_t valueLength)
{
    (void)value;
    (void)valueLength;
    (*(int *)context)++;
    return TRUE;
}

static BOOL CountMember(void *context, const char *key, size_t keyLength, const char *value, size_t valueLength)
{
    (void)key;
    (void)keyLength;
    return CountElement(context, value, valueLength);
}

BOOL CheckList_FromJson(CheckValue *value, const char *raw, size_t length)
{
    int count = 0;
    if (!raw || !length) return FALSE;
    value->readOnly = FALSE;
    value->json = raw;
    value->jsonLength = length;
    if ((length == 4 && memcmp(raw, "true", 4) == 0) || (length == 5 && memcmp(raw, "false", 5) == 0)) {
        value->kind = CHECK_VALUE_SWITCH;
        value->on = raw[0] == 't';
        return TRUE;
    }
    value->kind = CHECK_VALUE_TEXT;
    if (raw[0] == '"' && Core_JsonString(raw, length, value->text, ARRAYSIZE(value->text))) return TRUE;
    if ((raw[0] == '-' || (raw[0] >= '0' && raw[0] <= '9')) && length < ARRAYSIZE(value->text) &&
        MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, value->text, ARRAYSIZE(value->text) - 1) == (int)length) {
        value->text[length] = 0;
        return TRUE;
    }
    /* Anything else, or a text too long: what it holds, only shown. */
    value->readOnly = TRUE;
    if (raw[0] == '"') {
        WCHAR *whole = (WCHAR *)Util_Alloc((length + 1) * sizeof(WCHAR));
        BOOL read = whole && Core_JsonString(raw, length, whole, length + 1);
        if (read) {
            StringCchCopyNW(value->text, ARRAYSIZE(value->text), whole, ARRAYSIZE(value->text) - 2);
            StringCchCatW(value->text, ARRAYSIZE(value->text), L"\x2026");
        }
        Util_Free(whole);
        return read;
    }
    if (raw[0] == '[') Core_JsonEachElement(raw, length, CountElement, &count);
    else if (raw[0] == '{') Core_JsonEachMember(raw, length, CountMember, &count);
    if (raw[0] == 'n') StringCchCopyW(value->text, ARRAYSIZE(value->text), L"\x2014");
    else StringCchPrintfW(value->text, ARRAYSIZE(value->text), TR(L"%d items"), count);
    return TRUE;
}

BOOL CheckList_ToJson(const CheckValue *value, const char *raw, size_t length, char *out, size_t cap)
{
    if (value->readOnly || !raw || !length || cap < 8) return FALSE;
    if (value->kind == CHECK_VALUE_SWITCH) {
        if (value->on == (raw[0] == 't')) return FALSE;
        return SUCCEEDED(StringCchCopyA(out, cap, value->on ? "true" : "false"));
    }
    if (value->kind != CHECK_VALUE_TEXT) return FALSE;
    if (raw[0] == '"') {
        if (!Core_JsonQuote(value->text, out, cap)) return FALSE;
    } else {
        /* A number stays one: the text written as it is, when it is one. */
        int bytes = WideCharToMultiByte(CP_UTF8, 0, value->text, -1, out, (int)cap, NULL, NULL);
        if (bytes <= 1 || !(out[0] == '-' || (out[0] >= '0' && out[0] <= '9')) || !Core_JsonIsValue(out, (size_t)bytes - 1)) return FALSE;
    }
    return strlen(out) != length || memcmp(out, raw, length) != 0;
}

/* What building a value's parts goes by. */
typedef struct PartsBuild {
    CheckList        *checks;
    const CheckParts *parts;
    int               parent, depth, index;
} PartsBuild;

static BOOL CountPart(void *context, const char *key, size_t keyLength, const char *raw, size_t rawLength);

static BOOL CountPartElement(void *context, const char *raw, size_t rawLength)
{
    return CountPart(context, NULL, 0, raw, rawLength);
}

/* The values `raw` would take as parts, `depth` levels down: its parts, and theirs. */
static int PartsCount(const char *raw, size_t length, int depth)
{
    int count[2];
    count[0] = 0;
    count[1] = depth;
    if (depth <= 0 || !raw || !length) return 0;
    if (raw[0] == '{') Core_JsonEachMember(raw, length, CountPart, count);
    else if (raw[0] == '[') Core_JsonEachElement(raw, length, CountPartElement, count);
    return count[0];
}

static BOOL CountPart(void *context, const char *key, size_t keyLength, const char *raw, size_t rawLength)
{
    int *count = (int *)context;
    (void)key;
    (void)keyLength;
    count[0] += 1 + PartsCount(raw, rawLength, count[1] - 1);
    return TRUE;
}

/* A string's text, read from its JSON (with its quotes). */
static BOOL StringOf(const char *raw, size_t length, WCHAR *out, size_t cch)
{
    out[0] = 0;
    return length >= 2 && raw[0] == '"' && Core_JsonString(raw, length, out, cch);
}

/* A path of a folder or a file: a drive's, a share's, or Unix's. */
static BOOL LooksLikePath(const WCHAR *text)
{
    return (text[0] && text[1] == L':' && (text[2] == L'\\' || text[2] == L'/')) || wcsncmp(text, L"\\\\", 2) == 0 || text[0] == L'/' ||
           wcsstr(text, L":\\") != NULL;
}

/* A list's element named by itself: its text, or an object's name, title or id, or a list's first text; else "#n". */
static void ElementName(const char *raw, size_t length, int index, WCHAR *out, size_t cch)
{
    static const char *const kNames[] = { "name", "title", "label", "id" };
    const char *member;
    size_t memberLength, i;
    out[0] = 0;
    if (raw[0] == '"') {
        StringOf(raw, length, out, cch);
    } else if (raw[0] == '{') {
        for (i = 0; i < ARRAYSIZE(kNames) && !out[0]; i++)
            if (Core_JsonMember(raw, length, kNames[i], &member, &memberLength)) StringOf(member, memberLength, out, cch);
    }
    if (!out[0] && raw[0] == '[') {
        /* A list's first element, when it is a text. */
        size_t at = 1, end;
        while (at < length && (raw[at] == ' ' || raw[at] == '\n' || raw[at] == '\r' || raw[at] == '\t')) at++;
        if (at < length && raw[at] == '"') {
            for (end = at + 1; end < length && raw[end] != '"'; end++)
                if (raw[end] == '\\') end++;
            if (end < length) StringOf(raw + at, end + 1 - at, out, cch);
        }
    }
    if (!out[0]) StringCchPrintfW(out, cch, L"#%d", index + 1);
}

/* What a list's element that is itself a short list of texts and numbers
 * (a session's model and effort: its id, then them) says after its first,
 * which names it. */
typedef struct TupleText {
    WCHAR *out;
    size_t cch;
    int    index;
    BOOL   scalar;
} TupleText;

static BOOL TupleElement(void *context, const char *raw, size_t length)
{
    TupleText *tuple = (TupleText *)context;
    WCHAR text[CHECK_VALUE_CCH];
    int n;
    if (raw[0] == '{' || raw[0] == '[') {
        tuple->scalar = FALSE;
        return FALSE;
    }
    if (tuple->index++ == 0) return TRUE;
    if (raw[0] == '"') {
        StringOf(raw, length, text, ARRAYSIZE(text));
    } else {
        n = MultiByteToWideChar(CP_UTF8, 0, raw, (int)min(length, ARRAYSIZE(text) - 1), text, ARRAYSIZE(text) - 1);
        text[max(n, 0)] = 0;
    }
    if (tuple->out[0]) StringCchCatW(tuple->out, tuple->cch, L" \x00B7 ");
    StringCchCatW(tuple->out, tuple->cch, text);
    return TRUE;
}

/* A list's element `raw` that is such a list: what it says, into `out`. */
static BOOL TupleOf(const char *raw, size_t length, WCHAR *out, size_t cch)
{
    TupleText tuple;
    WCHAR said[CHECK_VALUE_CCH];
    if (length < 2 || raw[0] != '[') return FALSE;
    said[0] = 0;
    tuple.out = said;
    tuple.cch = ARRAYSIZE(said);
    tuple.index = 0;
    tuple.scalar = TRUE;
    if (!Core_JsonEachElement(raw, length, TupleElement, &tuple) || !tuple.scalar || tuple.index < 2) return FALSE;
    return SUCCEEDED(StringCchCopyW(out, cch, said));
}

static void AddPartsOf(PartsBuild *build);

static BOOL AddPart(void *context, const char *key, size_t keyLength, const char *raw, size_t rawLength)
{
    PartsBuild *build = (PartsBuild *)context;
    CheckList *checks = build->checks;
    const CheckValue *parent = &checks->values[build->parent];
    CheckValue *value = &checks->values[checks->valueCount];
    WCHAR text[CHECK_NAME_CCH];
    int at = checks->valueCount, index = build->index++;
    BOOL tuple = FALSE;
    ZeroMemory(value, sizeof *value);
    if (!CheckList_FromJson(value, raw, rawLength)) {
        value->kind = CHECK_VALUE_NONE;
        value->json = raw;
        value->jsonLength = rawLength;
    }
    value->parent = build->parent + 1;
    value->group = parent->group;
    value->item = parent->item;
    value->boxed = parent->boxed;
    value->taken = parent->taken;
    value->pick = -1;
    value->key = key;
    value->keyLength = keyLength;
    if (key) {
        /* An object's member: its key (the quotes around it are the JSON's own). */
        if (!Core_JsonString(key - 1, keyLength + 2, text, ARRAYSIZE(text))) StringCchCopyW(text, ARRAYSIZE(text), L"?");
    } else {
        ElementName(raw, rawLength, index, text, ARRAYSIZE(text));
        /* A list's text is its row's name: nothing to change in it; nor in a short list of them, which says the rest. */
        tuple = TupleOf(raw, rawLength, value->text, ARRAYSIZE(value->text));
        if (raw[0] == '"' || tuple) {
            value->kind = CHECK_VALUE_NONE;
            if (!tuple) value->text[0] = 0;
            value->readOnly = TRUE;
        }
    }
    if (build->parts->name && build->parts->name(build->parts->context, text, value->name, ARRAYSIZE(value->name))) {
        value->image = build->parts->named;
    } else {
        StringCchCopyW(value->name, ARRAYSIZE(value->name), text);
        value->image = LooksLikePath(text) ? build->parts->folder : (raw[0] == '{' || raw[0] == '[') && !tuple ? build->parts->list : build->parts->leaf;
    }
    value->label = value->name;
    value->literal = TRUE;
    checks->valueCount++;
    if (build->depth > 1 && !tuple && (raw[0] == '{' || raw[0] == '[')) {
        PartsBuild inner = *build;
        inner.parent = at;
        inner.depth = build->depth - 1;
        inner.index = 0;
        AddPartsOf(&inner);
    }
    return TRUE;
}

static BOOL AddElement(void *context, const char *raw, size_t rawLength)
{
    return AddPart(context, NULL, 0, raw, rawLength);
}

/* The parts of `build->parent`, all of them, when they fit. */
static void AddPartsOf(PartsBuild *build)
{
    const CheckValue *value = &build->checks->values[build->parent];
    if (!value->json || !value->jsonLength || build->checks->valueCount + PartsCount(value->json, value->jsonLength, build->depth) > build->parts->cap)
        return;
    if (value->json[0] == '{') Core_JsonEachMember(value->json, value->jsonLength, AddPart, build);
    else if (value->json[0] == '[') Core_JsonEachElement(value->json, value->jsonLength, AddElement, build);
}

void CheckList_AddParts(CheckList *checks, int v, const CheckParts *parts)
{
    PartsBuild build;
    if (!checks->values || v < 0 || v >= checks->valueCount || v != checks->valueCount - 1) return;
    build.checks = checks;
    build.parts = parts;
    build.parent = v;
    build.depth = parts->depth;
    build.index = 0;
    AddPartsOf(&build);
}

/* JSON written as it grows: a heap block, NULL once memory ran out. */
typedef struct JsonOut {
    char  *data;
    size_t used, cap;
    BOOL   failed;
} JsonOut;

static void Put(JsonOut *out, const char *text, size_t length)
{
    if (out->failed) return;
    if (out->used + length + 1 > out->cap) {
        size_t cap = max(out->cap * 2, out->used + length + 256);
        char *grown = (char *)Util_Alloc(cap);
        if (!grown) {
            out->failed = TRUE;
            return;
        }
        if (out->data) memcpy(grown, out->data, out->used);
        Util_Free(out->data);
        out->data = grown;
        out->cap = cap;
    }
    memcpy(out->data + out->used, text, length);
    out->used += length;
    out->data[out->used] = 0;
}

/* Value `v` or one of its parts changed, or one of its parts was left out. */
static BOOL PartChanged(const CheckList *checks, int v)
{
    const CheckValue *value = &checks->values[v];
    char text[CHECK_VALUE_CCH * 6 + 8];
    int w, end;
    if (!HasParts(checks, v)) return value->json && CheckList_ToJson(value, value->json, value->jsonLength, text, sizeof text);
    for (w = v + 1, end = PartsEnd(checks, v); w < end; w++)
        if (ParentOf(checks, w) == v && ((checks->values[w].boxed && ValueBox(checks, w) == 1) || PartChanged(checks, w))) return TRUE;
    return FALSE;
}

static void PutValue(JsonOut *out, const CheckList *checks, int v)
{
    const CheckValue *value = &checks->values[v];
    char text[CHECK_VALUE_CCH * 6 + 8];
    BOOL object = value->json[0] == '{', first = TRUE;
    int w, end;
    if (!HasParts(checks, v)) {
        if (CheckList_ToJson(value, value->json, value->jsonLength, text, sizeof text)) Put(out, text, strlen(text));
        else Put(out, value->json, value->jsonLength);
        return;
    }
    Put(out, object ? "{" : "[", 1);
    for (w = v + 1, end = PartsEnd(checks, v); w < end; w++) {
        const CheckValue *part = &checks->values[w];
        if (ParentOf(checks, w) != v || (part->boxed && ValueBox(checks, w) == 1)) continue;
        if (!first) Put(out, ",", 1);
        first = FALSE;
        if (object && part->key) {
            Put(out, "\"", 1);
            Put(out, part->key, part->keyLength);
            Put(out, "\":", 2);
        }
        PutValue(out, checks, w);
    }
    Put(out, object ? "}" : "]", 1);
}

char *CheckList_JsonOf(const CheckList *checks, int v, size_t *length)
{
    JsonOut out;
    *length = 0;
    if (v < 0 || v >= checks->valueCount || !checks->values[v].json || !PartChanged(checks, v)) return NULL;
    ZeroMemory(&out, sizeof out);
    PutValue(&out, checks, v);
    if (out.failed) {
        Util_Free(out.data);
        return NULL;
    }
    *length = out.used;
    return out.data;
}

/* ------------------------------------------------------------ the pane */

/* The other list over the same values: the pane's list, or the list's pane; NULL: none. */
static CheckList *Other(const CheckList *checks)
{
    return checks->twin ? checks->twin : checks->pane;
}

/* What changed in this list shown in the other: what is checked, each row's
 * box and value, and its editor's value, but for what is being typed there. */
static void Mirror(CheckList *checks)
{
    CheckList *other = Other(checks);
    const CheckValue *value;
    WCHAR shown[CHECK_VALUE_CCH];
    if (!other || !other->list || other->following || checks->following) return;
    other->checked = checks->checked;
    other->following = TRUE;
    ShowBoxes(other);
    if (other->editor && GetFocus() != other->editor) {
        value = &other->values[other->editing];
        if (value->kind == CHECK_VALUE_TEXT) {
            GetWindowTextW(other->editor, shown, ARRAYSIZE(shown));
            if (wcscmp(shown, value->text) != 0) SetWindowTextW(other->editor, value->text);
        } else if (value->kind == CHECK_VALUE_CHOICE) {
            SendMessageW(other->editor, CB_SETCURSEL, (WPARAM)value->choice, 0);
        }
    }
    other->following = FALSE;
}

/* Which values the search finds (MARK_HERE) and which hold one that it finds (MARK_BELOW), once for the rows to come. */
static void MarkMatches(CheckList *pane)
{
    CheckSearch *search = pane->search;
    int v;
    if (!search) return;
    if (!search->marks || search->markCount != pane->valueCount) {
        Util_Free(search->marks);
        search->marks = pane->valueCount ? (BYTE *)Util_Alloc((size_t)pane->valueCount) : NULL;
        search->markCount = search->marks ? pane->valueCount : 0;
    }
    if (!search->marks) return;
    ZeroMemory(search->marks, (size_t)search->markCount);
    if (!search->text[0]) return;
    for (v = pane->valueCount - 1; v >= 0; v--) {
        const CheckValue *value = &pane->values[v];
        int parent = value->parent - 1;
        if (((search->in & CHECK_SEARCH_NAMES) && TextMatches(search, LabelOf(value))) ||
            ((search->in & CHECK_SEARCH_VALUES) && TextMatches(search, ValueText(value))))
            search->marks[v] |= MARK_HERE;
        if (search->marks[v] && parent >= 0) search->marks[parent] |= MARK_BELOW;
    }
}

/* The pane's rows, what the list's row `scope` holds, as the list shows it
 * below that row, further out: a value's parts; a heading's value, then its
 * items (folding to their values) and its own values; an item's value, then
 * its values; a value without parts, itself. A heading's or an item's value
 * with nothing else below it is that value. The search keeps some only. */
static void InsertPaneRows(CheckList *pane)
{
    LPARAM scope = pane->scope;
    int row = -1, v = RowValue(pane, scope), w, end, g = THEME_HEADING_OF(scope), i;
    BOOL alone;
    if (v >= 0 && !IsValueRow(scope) && (scope < 0 ? g < 0 || g >= pane->groupCount || HasRows(pane, g) : ItemRows(pane, (DWORD)scope) > 0)) v = -1;
    MarkMatches(pane);
    DrawRowsLater(pane, TRUE);
    pane->filling = TRUE;
    ListView_DeleteAllItems(pane->list);
    pane->indentLess = 0;
    alone = v >= 0 && !HasParts(pane, v);
    if (v >= 0 && !alone) {
        pane->indentLess = IndentOf(pane, v);
        for (w = v + 1, end = PartsEnd(pane, v); w < end; w++)
            if (ParentOf(pane, w) == v && RowKept(pane, w)) row = InsertValueRow(pane, row + 1, w);
    } else if (alone) {
        pane->indentLess = IndentOf(pane, v) - 1;
        if (RowKept(pane, v)) InsertValueRow(pane, 0, v);
    } else if (scope < 0 && g >= 0 && g < pane->groupCount) {
        pane->indentLess = 1;
        if ((w = HeadingValue(pane, g)) >= 0 && RowKept(pane, w)) row = InsertValueRow(pane, 0, w);
        InsertGroupItems(pane, row, g);
    } else if (scope > 0) {
        pane->indentLess = 2;
        if ((w = MergedValue(pane, (DWORD)scope)) >= 0 && RowKept(pane, w)) row = InsertValueRow(pane, 0, w);
        if ((i = ItemIndex(pane, (DWORD)scope)) >= 0) InsertItemValues(pane, row, i);
    }
    pane->filling = FALSE;
    ShowBoxes(pane);
    DrawRowsLater(pane, FALSE);
    if (alone && ListView_GetItemCount(pane->list) > 0) Theme_FocusRow(pane->list, 0);   /* a value alone: its editor at once */
    FollowSelection(pane);
}

/* The pane, as the list's row selected is: its name above it, what it holds in it. */
static void ShowPane(CheckList *checks)
{
    CheckList *pane = checks->pane;
    WCHAR title[CHECK_NAME_CCH], shown[CHECK_NAME_CCH];
    int row;
    LPARAM scope;
    if (!pane || !pane->list) return;
    row = ListView_GetNextItem(checks->list, -1, LVNI_SELECTED);
    scope = row >= 0 ? Theme_RowParam(checks->list, row) : 0;
    title[0] = shown[0] = 0;
    if (row >= 0) ListView_GetItemText(checks->list, row, 0, title, ARRAYSIZE(title));
    if (checks->paneTitle && (!GetWindowTextW(checks->paneTitle, shown, ARRAYSIZE(shown)) || wcscmp(shown, title) != 0))
        SetWindowTextW(checks->paneTitle, title);
    if (scope == pane->scope) return;
    pane->scope = scope;
    CloseEditor(pane);
    InsertPaneRows(pane);
    SizeColumns(pane);
}

/* The pane's rows again, as the search now keeps them. */
static void ApplySearch(CheckList *pane)
{
    CheckSearch *search = pane->search;
    if (!search) return;
    GetWindowTextW(search->edit, search->text, ARRAYSIZE(search->text));
    if ((search->in & CHECK_SEARCH_REGEX) && search->text[0]) {
        if (!search->regex) search->regex = (CoreRegex *)Util_Alloc(sizeof *search->regex);
        if (search->regex && !Core_RegexCompile(search->regex, search->text, !(search->in & CHECK_SEARCH_CASE))) {
            Util_Free(search->regex);   /* not a pattern it takes: nothing matches */
            search->regex = NULL;
        }
    } else {
        Util_Free(search->regex);
        search->regex = NULL;
    }
    CloseEditor(pane);
    InsertPaneRows(pane);
    SizeColumns(pane);
}

/* The search's options, in a menu below their button: where it looks, and how. */
static void SearchMenu(CheckList *pane)
{
    CheckSearch *search = pane->search;
    HMENU menu = CreatePopupMenu();
    RECT below;
    UINT cmd;
    if (!menu) return;
    AppendMenuW(menu, MF_STRING | ((search->in & CHECK_SEARCH_NAMES) ? MF_CHECKED : 0), CHECK_SEARCH_NAMES, TR(L"Search &names"));
    AppendMenuW(menu, MF_STRING | ((search->in & CHECK_SEARCH_VALUES) ? MF_CHECKED : 0), CHECK_SEARCH_VALUES, TR(L"Search &values"));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | ((search->in & CHECK_SEARCH_REGEX) ? MF_CHECKED : 0), CHECK_SEARCH_REGEX, TR(L"&Regular expression"));
    AppendMenuW(menu, MF_STRING | ((search->in & CHECK_SEARCH_CASE) ? MF_CHECKED : 0), CHECK_SEARCH_CASE, TR(L"Match &case"));
    GetWindowRect(search->options, &below);
    cmd = Theme_TrackMenu(search->options, menu, &below);
    DestroyMenu(menu);
    if (!cmd) return;
    search->in ^= cmd;
    if (!(search->in & (CHECK_SEARCH_NAMES | CHECK_SEARCH_VALUES))) search->in |= cmd == CHECK_SEARCH_NAMES ? CHECK_SEARCH_VALUES : CHECK_SEARCH_NAMES;
    if (!Util_SetSetting(SETTING_PANE_SEARCH, search->in)) Util_Log(L"the pane's search options could not be kept (error %lu)", GetLastError());
    ApplySearch(pane);
}

/* The search shown above the pane, or hidden, the pane taking its room; hidden, it keeps nothing out. */
static void ShowSearch(CheckList *pane, BOOL show)
{
    CheckSearch *search = pane->search;
    HWND view = GetParent(pane->list) != pane->owner ? GetParent(pane->list) : pane->list;
    RECT place, edit, gap = { 0, 0, 0, 4 };
    if (!search || !search->edit || search->shown == show) return;
    GetWindowRect(view, &place);
    GetWindowRect(search->edit, &edit);
    MapWindowPoints(NULL, pane->owner, (POINT *)&place, 2);
    MapWindowPoints(NULL, pane->owner, (POINT *)&edit, 2);
    MapDialogRect(pane->owner, &gap);
    place.top = show ? edit.bottom + gap.bottom : edit.top;
    search->shown = show;
    ShowWindow(search->edit, show ? SW_SHOW : SW_HIDE);
    ShowWindow(search->options, show ? SW_SHOW : SW_HIDE);
    SetWindowPos(view, NULL, place.left, place.top, place.right - place.left, place.bottom - place.top, SWP_NOZORDER | SWP_NOACTIVATE);
    if (show) {
        SetFocus(search->edit);
        SendMessageW(search->edit, EM_SETSEL, 0, -1);
    } else {
        if (GetFocus() == search->edit) SetFocus(pane->list);
        if (search->text[0]) SetWindowTextW(search->edit, L"");   /* EN_CHANGE: every row again */
    }
}

/* Ctrl+F in the dialog of the pane shown last: its search shown, or hidden. */
static CheckList *g_searchPane;
static HHOOK g_searchHook;

static LRESULT CALLBACK SearchShortcut(int code, WPARAM wp, LPARAM lp)
{
    const MSG *message = (const MSG *)lp;
    if (code == MSGF_DIALOGBOX && g_searchPane && message->message == WM_KEYDOWN && message->wParam == 'F' && GetKeyState(VK_CONTROL) < 0 &&
        GetKeyState(VK_MENU) >= 0 && GetAncestor(message->hwnd, GA_ROOT) == g_searchPane->owner) {
        CheckSearch *search = g_searchPane->search;
        if (search->shown && GetFocus() != search->edit) {
            SetFocus(search->edit);   /* shown, elsewhere: into it first */
            SendMessageW(search->edit, EM_SETSEL, 0, -1);
        } else {
            ShowSearch(g_searchPane, !search->shown);
        }
        return 1;
    }
    return CallNextHookEx(g_searchHook, code, wp, lp);
}

/* The pane's dialog: what its search's box and options say. */
static LRESULT CALLBACK PaneOwnerProc(HWND owner, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    CheckList *pane = (CheckList *)ref;
    if (message == WM_COMMAND && pane->search && lp) {
        if ((HWND)lp == pane->search->edit && HIWORD(wp) == EN_CHANGE) {
            ApplySearch(pane);
            return 0;
        }
        if ((HWND)lp == pane->search->options && HIWORD(wp) == BN_CLICKED) {
            SearchMenu(pane);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(owner, PaneOwnerProc, id);
    return DefSubclassProc(owner, message, wp, lp);
}

/* The pane's search, when its dialog has one (IDC_PANE_SEARCH, IDC_PANE_FIND): shown, looking where it last did. */
static void SetupSearch(CheckList *pane)
{
    HWND edit = GetDlgItem(pane->owner, IDC_PANE_SEARCH);
    CheckSearch *search;
    if (!edit || (search = (CheckSearch *)Util_Alloc(sizeof *search)) == NULL) return;
    search->edit = edit;
    search->options = GetDlgItem(pane->owner, IDC_PANE_FIND);
    search->in = Util_GetSetting(SETTING_PANE_SEARCH, CHECK_SEARCH_NAMES | CHECK_SEARCH_VALUES) &
                 (CHECK_SEARCH_NAMES | CHECK_SEARCH_VALUES | CHECK_SEARCH_REGEX | CHECK_SEARCH_CASE);
    if (!(search->in & (CHECK_SEARCH_NAMES | CHECK_SEARCH_VALUES))) search->in |= CHECK_SEARCH_NAMES | CHECK_SEARCH_VALUES;
    search->shown = TRUE;
    pane->search = search;
    SendMessageW(edit, EM_LIMITTEXT, SEARCH_CCH - 1, 0);
    SendMessageW(edit, EM_SETCUEBANNER, TRUE, (LPARAM)TR(L"Search names and values"));
    SetWindowSubclass(pane->owner, PaneOwnerProc, OWNER_SUBCLASS, (DWORD_PTR)pane);
    if (!g_searchHook) g_searchHook = SetWindowsHookExW(WH_MSGFILTER, SearchShortcut, NULL, GetCurrentThreadId());
    g_searchPane = pane;
}

/* The pane gone with its window: its search, and what it kept. */
static void FreePane(CheckList *pane)
{
    if (pane->twin && pane->twin->pane == pane) pane->twin->pane = NULL;
    if (pane->owner) RemoveWindowSubclass(pane->owner, PaneOwnerProc, OWNER_SUBCLASS);
    if (g_searchPane == pane) {
        g_searchPane = NULL;
        if (g_searchHook) UnhookWindowsHookEx(g_searchHook);
        g_searchHook = NULL;
    }
    if (pane->search) {
        Util_Free(pane->search->regex);
        Util_Free(pane->search->marks);
        Util_Free(pane->search);
    }
    Util_Free(pane);
}

/* The pane beside the list, when its dialog has one (IDC_PANE_LIST): a list
 * of its own over the same values, freed with its window, and its search.
 * The list's first row is selected when none is, so that the pane shows
 * something. */
static void SetupPane(CheckList *checks, const WCHAR *glyphs, const ThemeTint *tints, int images)
{
    HWND list = GetDlgItem(checks->owner, IDC_PANE_LIST), label = GetDlgItem(checks->owner, IDC_PANE_LABEL);
    CheckList *pane;
    checks->paneTitle = GetDlgItem(checks->owner, IDC_PANE_TITLE);
    if (!list || GetWindowSubclass(list, ListProc, CHECKLIST_SUBCLASS, NULL) || (pane = (CheckList *)Util_Alloc(sizeof *pane)) == NULL) return;
    if (label) Theme_SetStrong(label);
    pane->groups = checks->groups;
    pane->groupCount = checks->groupCount;
    pane->items = checks->items;
    pane->itemCount = checks->itemCount;
    pane->values = checks->values;
    pane->valueCount = checks->valueCount;
    pane->columns = checks->columns;
    pane->columnCount = checks->columnCount;
    pane->plain = checks->plain;
    pane->pickedImage = checks->pickedImage;
    pane->candidateImage = checks->candidateImage;
    pane->offered = checks->offered;
    pane->checked = checks->checked;
    pane->label = checks->label;
    pane->context = checks->context;
    pane->twin = checks;
    checks->pane = pane;
    CheckList_Setup(pane, list, glyphs, tints, images);
    SetupSearch(pane);
    Theme_SmoothView(list);
    if (ListView_GetNextItem(checks->list, -1, LVNI_SELECTED) < 0 && ListView_GetItemCount(checks->list) > 0) Theme_FocusRow(checks->list, 0);
    FollowSelection(checks);
}

/* ------------------------------------------------------------ the list */

void CheckList_Setup(CheckList *checks, HWND list, const WCHAR *glyphs, const ThemeTint *tints, int images)
{
    WCHAR opened[CHECKLIST_GROUPS_MAX][MAX_PATH];
    HIMAGELIST states;
    int g, o, c, columns, openedCount = checks->openedSection ? Util_GetSettingList(checks->openedSection, opened, CHECKLIST_GROUPS_MAX) : 0;
    checks->list = list;
    checks->owner = GetAncestor(list, GA_ROOT);
    checks->overview = !checks->twin && GetDlgItem(checks->owner, IDC_PANE_LIST) != NULL;   /* its pane shows the rest */
    checks->checked &= checks->offered;
    checks->editor = NULL;
    checks->editing = -1;
    if (checks->groupCount > CHECKLIST_GROUPS_MAX) checks->groupCount = CHECKLIST_GROUPS_MAX;
    ZeroMemory(checks->open, sizeof checks->open);
    for (g = 0; g < checks->groupCount; g++) {
        for (o = 0; o < openedCount; o++)
            if (Core_EqualsI(opened[o], checks->groups[g].name)) checks->open[g] = TRUE;
        if (!openedCount && g < 32 && (checks->openAtFirst & (1u << g))) checks->open[g] = TRUE;
    }
    for (g = 0; g < checks->itemCount; g++) FollowValues(checks, checks->items[g].bit);
    checks->filling = TRUE;
    if (!checks->plain) {
        Theme_CheckRows(list);
        if ((states = ListView_GetImageList(list, LVSIL_STATE)) != NULL) ImageList_SetImageCount(states, 3);   /* the third: some of a group */
    } else {
        LVCOLUMNW column;
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(list, 0, &column);
    }
    if ((columns = ValueColumns(checks)) > 0) {
        LVCOLUMNW column;
        ListView_SetExtendedListViewStyle(list, ListView_GetExtendedListViewStyle(list) | LVS_EX_SUBITEMIMAGES);
        ZeroMemory(&column, sizeof column);
        for (c = 1; c <= columns; c++) {
            if (checks->columns) {
                column.mask = LVCF_TEXT;
                column.pszText = (LPWSTR)checks->columns[c];
            }
            ListView_InsertColumn(list, c, &column);
        }
        if (checks->columns && checks->columnCount > 0) {
            column.mask = LVCF_TEXT;
            column.pszText = (LPWSTR)checks->columns[0];
            ListView_SetColumn(list, 0, &column);
        }
    }
    if (columns > 0 || checks->twin) SetWindowSubclass(list, ListProc, CHECKLIST_SUBCLASS, (DWORD_PTR)checks);   /* a pane: freed with it */
    ListView_SetImageList(list, Theme_GlyphImages(list, glyphs, tints, images), LVSIL_SMALL);
    Theme_SetFoldingRows(list);
    checks->filling = FALSE;
    if (checks->twin) InsertPaneRows(checks);
    else InsertRows(checks);
    SizeColumns(checks);
    if (!checks->twin) SetupPane(checks, glyphs, tints, images);
}

/* Row `param`, the group `g` it is under opened first when it is folded away: -1 for none. */
static int OpenedRowOf(CheckList *checks, LPARAM param, int g)
{
    LVFINDINFOW find;
    int row, heading;
    ZeroMemory(&find, sizeof find);
    find.flags = LVFI_PARAM;
    find.lParam = param;
    if ((row = ListView_FindItem(checks->list, -1, &find)) >= 0 || g < 0 || g >= checks->groupCount) return row;
    find.lParam = THEME_HEADING_ROW(g);
    if ((heading = ListView_FindItem(checks->list, -1, &find)) >= 0) FoldGroup(checks, heading, TRUE);
    find.lParam = param;
    return ListView_FindItem(checks->list, -1, &find);
}

static void FoldItem(CheckList *checks, int row, DWORD bit, BOOL open);

void CheckList_SelectValue(CheckList *checks, int v, BOOL edit)
{
    int row, g, top;
    const CheckValue *value;
    if (v < 0 || v >= checks->valueCount) return;
    for (top = v; ParentOf(checks, top) >= 0; top = ParentOf(checks, top))
        ;
    value = &checks->values[top];
    g = value->group;
    if (checks->overview && checks->pane && (value->item || top != v)) {
        /* An overview: the row that holds it here, then the value itself in the pane. */
        LPARAM holder = value->item ? (LPARAM)value->item : value->merged ? THEME_HEADING_ROW(g) : CHECK_VALUE_BASE + top;
        if ((row = OpenedRowOf(checks, holder, g)) < 0) return;
        Theme_FocusRow(checks->list, row);
        FollowSelection(checks);
        CheckList_SelectValue(checks->pane, v, edit);
        return;
    }
    if ((row = RowOf(checks, v)) < 0 && g >= 0 && g < checks->groupCount) {
        LVFINDINFOW find;
        int heading;
        ZeroMemory(&find, sizeof find);
        find.flags = LVFI_PARAM;
        find.lParam = THEME_HEADING_ROW(g);
        if ((heading = ListView_FindItem(checks->list, -1, &find)) >= 0) FoldGroup(checks, heading, TRUE);
        row = RowOf(checks, v);
    }
    if (row < 0 && value->item) {
        /* A pane: the item holding it, opened. */
        int itemRow = OpenedRowOf(checks, (LPARAM)value->item, -1);
        if (itemRow >= 0) FoldItem(checks, itemRow, value->item, TRUE);
        row = RowOf(checks, v);
    }
    if (row < 0) {
        /* The values it is a part of, opened from the outermost. */
        int chain[16], n = 0, p, at;
        for (p = ParentOf(checks, v); p >= 0 && n < (int)ARRAYSIZE(chain); p = ParentOf(checks, p)) chain[n++] = p;
        while (n-- > 0)
            if ((at = RowOf(checks, chain[n])) >= 0) FoldValue(checks, at, chain[n], TRUE);
        row = RowOf(checks, v);
    }
    if (row < 0) return;
    Theme_FocusRow(checks->list, row);
    FollowSelection(checks);
    if (!edit) return;
    EnterEditor(checks, FALSE);
    if (checks->editor && GetFocus() == checks->editor && checks->values[v].kind == CHECK_VALUE_TEXT) SendMessageW(checks->editor, EM_SETSEL, 0, -1);
}

void CheckList_ShowValue(CheckList *checks, int v)
{
    int row = v >= 0 && v < checks->valueCount ? RowOf(checks, v) : -1;
    if (row >= 0) ShowValue(checks, row, v);
    if (checks->editor && checks->editing == v) EnableWindow(checks->editor, Enabled(checks, &checks->values[v]));
    Mirror(checks);
}

void CheckList_Refresh(CheckList *checks)
{
    CloseEditor(checks);
    ShowBoxes(checks);
    SizeColumns(checks);
    FollowSelection(checks);
    Mirror(checks);
}

/* Whether row `row` shows what it holds below it (its arrow open). */
static BOOL RowOpen(HWND list, int row)
{
    return (int)((ListView_GetItemState(list, row, LVIS_OVERLAYMASK) & LVIS_OVERLAYMASK) >> 8) == THEME_FOLD_OPEN;
}

/* Value `v`'s parts shown below its row (`open`), or taken away. While the
 * search keeps some rows, those it keeps, or all of them when it keeps none. */
static void FoldValue(CheckList *checks, int row, int v, BOOL open)
{
    int w, end = PartsEnd(checks, v), at = row, kept = 0;
    if (checks->overview || !HasParts(checks, v) || RowOpen(checks->list, row) == !!open) return;
    SetOpen(checks, v, open);
    DrawRowsLater(checks, TRUE);
    checks->filling = TRUE;
    Theme_SetFold(checks->list, row, open ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED);
    if (open) {
        for (w = v + 1; w < end; w++)
            if (ParentOf(checks, w) == v && RowKept(checks, w)) {
                at = InsertValueRow(checks, at + 1, w);
                kept++;
            }
        for (w = v + 1; !kept && w < end; w++)
            if (ParentOf(checks, w) == v) at = InsertValueRow(checks, at + 1, w);
    } else {
        while (row + 1 < ListView_GetItemCount(checks->list) && (w = ValueOfRow(checks, Theme_RowParam(checks->list, row + 1))) > v && w < end)
            ListView_DeleteItem(checks->list, row + 1);
    }
    checks->filling = FALSE;
    ShowBoxes(checks);
    DrawRowsLater(checks, FALSE);
    FollowSelection(checks);
    SizeColumns(checks);
}

/* Item `bit`'s values shown below its row in the pane (`open`), or taken away. */
static void FoldItem(CheckList *checks, int row, DWORD bit, BOOL open)
{
    int i = ItemIndex(checks, bit), w, at;
    if (i < 0 || !ItemFolds(checks, bit) || RowOpen(checks->list, row) == !!open) return;
    checks->openItems = open ? checks->openItems | bit : checks->openItems & ~bit;
    DrawRowsLater(checks, TRUE);
    checks->filling = TRUE;
    Theme_SetFold(checks->list, row, open ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED);
    if (open) {
        if ((at = InsertItemValues(checks, row, i)) == row)
            for (w = 0; w < checks->valueCount; w++)   /* the search keeps none of them: all */
                if (checks->values[w].item == bit && !checks->values[w].merged && !checks->values[w].parent) at = InsertValueRow(checks, at + 1, w);
    } else {
        while (row + 1 < ListView_GetItemCount(checks->list) && (w = ValueOfRow(checks, Theme_RowParam(checks->list, row + 1))) >= 0 &&
               checks->values[w].item == bit)
            ListView_DeleteItem(checks->list, row + 1);
    }
    checks->filling = FALSE;
    ShowBoxes(checks);
    DrawRowsLater(checks, FALSE);
    FollowSelection(checks);
    SizeColumns(checks);
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
    if (g < 0 || g >= checks->groupCount || !HasRows(checks, g) || !checks->open[g] == !open) return;
    checks->open[g] = open;
    DrawRowsLater(checks, TRUE);
    checks->filling = TRUE;
    Theme_SetFold(checks->list, row, open ? THEME_FOLD_OPEN : THEME_FOLD_CLOSED);
    if (open) InsertGroupItems(checks, row, g);
    else Theme_DeleteFoldedRows(checks->list, row);
    checks->filling = FALSE;
    ShowBoxes(checks);
    DrawRowsLater(checks, FALSE);
    FollowSelection(checks);
    SizeColumns(checks);
    KeepOpenedGroups(checks);
}

void CheckList_Relabel(CheckList *checks)
{
    LVFINDINFOW find;
    int row = ListView_GetNextItem(checks->list, -1, LVNI_SELECTED);
    ZeroMemory(&find, sizeof find);
    find.flags = LVFI_PARAM;
    find.lParam = row >= 0 ? Theme_RowParam(checks->list, row) : 0;
    CloseEditor(checks);
    InsertRows(checks);
    if (row >= 0 && (row = ListView_FindItem(checks->list, -1, &find)) >= 0) Theme_FocusRow(checks->list, row);
    FollowSelection(checks);
    SizeColumns(checks);
    if (checks->pane) {
        CloseEditor(checks->pane);
        InsertPaneRows(checks->pane);
        SizeColumns(checks->pane);
    }
}

void CheckList_FoldAll(CheckList *checks, BOOL open)
{
    int g, v;
    if (checks->twin) {
        /* A pane: every item's values and every value's parts in it. */
        for (v = 0; v < checks->valueCount; v++) checks->values[v].paneOpen = open && HasParts(checks, v);
        checks->openItems = 0;
        for (g = 0; open && g < checks->itemCount; g++) checks->openItems |= checks->items[g].bit;
        CloseEditor(checks);
        InsertPaneRows(checks);
        SizeColumns(checks);
        return;
    }
    for (g = 0; g < checks->groupCount; g++) checks->open[g] = open && HasRows(checks, g);
    InsertRows(checks);
    if (ListView_GetItemCount(checks->list)) Theme_FocusRow(checks->list, 0);
    KeepOpenedGroups(checks);
}

/* All of item `bit`, its values with boxes too, checked or none (`on`). */
static void SetItem(CheckList *checks, DWORD bit, BOOL on)
{
    int v;
    checks->checked = on ? checks->checked | bit : checks->checked & ~bit;
    for (v = 0; v < checks->valueCount; v++)
        if (checks->values[v].item == bit && checks->values[v].boxed) checks->values[v].taken = on;
}

void CheckList_Toggle(CheckList *checks, int row)
{
    LPARAM param;
    int v, g, i;
    BOOL on;
    if (checks->plain || row < 0 || row >= ListView_GetItemCount(checks->list)) return;
    param = Theme_RowParam(checks->list, row);
    if ((v = ValueOfRow(checks, param)) >= 0) {
        CheckValue *value = &checks->values[v];
        if (!value->boxed) {
            TurnSwitch(checks, v);   /* a row without a box: Space turns its switch over */
        } else {
            TakeValue(checks, v, ValueBox(checks, v) != 2);   /* all of it, or none once all of it is taken */
            FollowValues(checks, value->item);
        }
    } else if (param < 0 && (g = THEME_HEADING_OF(param)) >= 0 && g < checks->groupCount && GroupHasValues(checks, g)) {
        /* All of the group, items and values, or none once it all is. */
        on = GroupBox(checks, g) != 2;
        checks->checked = on ? checks->checked | RowMask(checks, param) : checks->checked & ~RowMask(checks, param);
        for (i = 0; i < checks->itemCount; i++)
            if (ItemShown(checks, &checks->items[i], g)) SetItem(checks, checks->items[i].bit, on);
        for (i = 0; i < checks->valueCount; i++)
            if (checks->values[i].group == g && !checks->values[i].item && checks->values[i].boxed) checks->values[i].taken = on;
    } else if (param > 0) {
        SetItem(checks, (DWORD)param, ItemBox(checks, (DWORD)param) != 2);   /* an item, its values with boxes: all, or none once all are */
    } else {
        DWORD mask = RowMask(checks, param);
        checks->checked = (checks->checked & mask) == mask ? checks->checked & ~mask : checks->checked | mask;
    }
    ShowBoxes(checks);
    Mirror(checks);
}

/* `taken` as `how` makes it: CHECKLIST_ALL, CHECKLIST_NONE, or turned over. */
static BOOL TakenAs(BOOL taken, int how)
{
    return how == CHECKLIST_ALL ? TRUE : how == CHECKLIST_NONE ? FALSE : !taken;
}

void CheckList_CheckAll(CheckList *checks, int how)
{
    DWORD items = 0, groups = 0, others;
    int v, w, end, i, g, leaves;
    if (checks->plain) return;
    /* The values at their leaves (no boxed part below them); from the end, so that the parts right after a value
     * are settled before it: one with parts then follows them, as its box shows. */
    for (v = checks->valueCount - 1; v >= 0; v--) {
        CheckValue *value = &checks->values[v];
        if (!value->boxed) continue;
        for (leaves = 0, w = v + 1, end = PartsEnd(checks, v); w < end; w++) leaves += checks->values[w].boxed && !HasParts(checks, w);
        value->taken = leaves ? ValueBox(checks, v) != 1 : TakenAs(value->taken, how);
    }
    /* Items: one with boxed values follows them; one without, its own bit. */
    for (i = 0; i < checks->itemCount; i++) {
        DWORD bit = checks->items[i].bit;
        BOOL valued = FALSE;
        if (!Offered(checks, bit)) continue;
        items |= bit;
        for (v = 0; v < checks->valueCount && !valued; v++) valued = checks->values[v].item == bit && checks->values[v].boxed;
        if (valued) FollowValues(checks, bit);
        else checks->checked = TakenAs((checks->checked & bit) != 0, how) ? checks->checked | bit : checks->checked & ~bit;
    }
    /* A group's bits that no item shows: its heading's box alone stands for them. */
    for (g = 0; g < checks->groupCount; g++) groups |= checks->groups[g].mask;
    others = groups & checks->offered & ~items;
    checks->checked = how == CHECKLIST_ALL ? checks->checked | others
                    : how == CHECKLIST_NONE ? checks->checked & ~others : checks->checked ^ others;
    ShowBoxes(checks);
    Mirror(checks);
}

BOOL CheckList_Message(CheckList *checks, HWND dialog, UINT message, WPARAM wp, LPARAM lp, BOOL *toggled)
{
    *toggled = FALSE;
    if (message == WM_NOTIFY) {
        LRESULT result;
        if (!CheckList_Notify(checks, dialog, (const NMHDR *)lp, &result)) return FALSE;
        SetWindowLongPtrW(dialog, DWLP_MSGRESULT, result);
        return TRUE;
    }
    if (message == WM_CHECKLIST_BOX) {
        /* In the list, or in its pane. */
        CheckList_Toggle(checks->pane && (HWND)lp == checks->pane->list ? checks->pane : checks, (int)wp);
        *toggled = TRUE;
        return TRUE;
    }
    if (message == WM_CONTEXTMENU && checks->pane && ((HWND)wp == checks->pane->list || (HWND)wp == GetParent(checks->pane->list)))
        return CheckList_Message(checks->pane, dialog, message, wp, lp, toggled);
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
    if (checks->pane && header->hwndFrom == checks->pane->list) return CheckList_Notify(checks->pane, dialog, header, result);
    if (header->hwndFrom != checks->list) return FALSE;
    if (header->code == LVN_ITEMCHANGING) {
        const NMLISTVIEW *change = (const NMLISTVIEW *)header;
        if (checks->filling || !(change->uChanged & LVIF_STATE) || !((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) return FALSE;
        PostMessageW(dialog, WM_CHECKLIST_BOX, (WPARAM)change->iItem, (LPARAM)checks->list);
        *result = TRUE;
        return TRUE;
    }
    if (header->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *change = (const NMLISTVIEW *)header;
        if (checks->valueCount && !checks->filling && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED))
            FollowSelection(checks);
        return FALSE;
    }
    if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
        /* The arrow folds or opens a group; so does a double-click on its heading. A click on a value turns a switch
         * over, keeps a pick's version, or goes into its editor. */
        LVHITTESTINFO hit;
        int row = Theme_FoldClickRow(checks->list, header, &hit), g = row >= 0 ? THEME_HEADING_OF(Theme_RowParam(checks->list, row)) : -1, v;
        LPARAM param = row >= 0 ? Theme_RowParam(checks->list, row) : 0;
        if (g >= 0 && g < checks->groupCount) {
            FoldGroup(checks, row, !checks->open[g]);
        } else if (row >= 0 && (v = ValueOfRow(checks, param)) >= 0 && HasParts(checks, v)) {
            FoldValue(checks, row, v, !RowOpen(checks->list, row));
        } else if (row >= 0 && param > 0 && !IsValueRow(param) && ItemFolds(checks, (DWORD)param)) {
            FoldItem(checks, row, (DWORD)param, !RowOpen(checks->list, row));   /* a pane's item, to its values */
        } else if (hit.iItem >= 0 && hit.iSubItem >= 1 && (v = RowValue(checks, Theme_RowParam(checks->list, hit.iItem))) >= 0) {
            if (checks->values[v].kind == CHECK_VALUE_PICK) PickColumn(checks, v, hit.iSubItem - 1);
            else if (hit.iSubItem == 1 && (checks->values[v].kind == CHECK_VALUE_SWITCH || checks->values[v].kind == CHECK_VALUE_ACTION) &&
                     (header->code == NM_CLICK || checks->values[v].kind == CHECK_VALUE_SWITCH))
                TurnSwitch(checks, v);
            else if (hit.iSubItem == 1 && checks->editing == v) EnterEditor(checks, TRUE);
        }
        return FALSE;
    }
    if (header->code == LVN_KEYDOWN) {
        /* Right opens a group, Left folds it, or goes from a row up to its group; on a pick, the version left or right
         * is kept. F2 goes into a value's editor. */
        WORD key = ((const NMLVKEYDOWN *)header)->wVKey;
        int focused = ListView_GetNextItem(checks->list, -1, LVNI_FOCUSED), v, row;
        LPARAM param = focused >= 0 ? Theme_RowParam(checks->list, focused) : 0;
        v = focused >= 0 ? RowValue(checks, param) : -1;
        if ((key == VK_LEFT || key == VK_RIGHT) && v >= 0 && checks->values[v].kind == CHECK_VALUE_PICK) {
            StepPick(checks, v, key == VK_LEFT ? -1 : 1);
        } else if ((key == VK_LEFT || key == VK_RIGHT) && v >= 0 && IsValueRow(param) && HasParts(checks, v) && !checks->overview &&
                   (key == VK_RIGHT) != RowOpen(checks->list, focused)) {
            FoldValue(checks, focused, v, key == VK_RIGHT);   /* Right opens a value's parts, Left folds them */
        } else if ((key == VK_LEFT || key == VK_RIGHT) && param > 0 && !IsValueRow(param) && ItemFolds(checks, (DWORD)param) &&
                   (key == VK_RIGHT) != RowOpen(checks->list, focused)) {
            FoldItem(checks, focused, (DWORD)param, key == VK_RIGHT);   /* a pane's item: its values */
        } else if (key == VK_LEFT && v >= 0 && IsValueRow(param) && ParentOf(checks, v) >= 0 && (row = RowOf(checks, ParentOf(checks, v))) >= 0) {
            Theme_FocusRow(checks->list, row);   /* from a part up to the value it is a part of */
        } else if (key == VK_LEFT && v >= 0 && IsValueRow(param) && ParentOf(checks, v) < 0 && checks->values[v].item && checks->twin &&
                   (row = OpenedRowOf(checks, (LPARAM)checks->values[v].item, -1)) >= 0) {
            Theme_FocusRow(checks->list, row);   /* from a value up to the item it is under */
        } else if ((row = Theme_FoldKeyRow(checks->list, key)) >= 0) {
            FoldGroup(checks, row, key == VK_RIGHT);
        } else if (key == VK_F2) {
            if (v >= 0 && checks->values[v].kind == CHECK_VALUE_ACTION) TurnSwitch(checks, v);
            else EnterEditor(checks, FALSE);
        } else if (key == VK_SPACE && checks->plain && v >= 0) {
            TurnSwitch(checks, v);   /* without boxes, the list itself turns none over */
        }
    }
    return FALSE;
}
