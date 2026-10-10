/*
 * Settings, from the gear at the toolbar's right end: how long the session
 * vault keeps the versions of each list (sessionvault.c), and help, the
 * questions someone new asks, each answered in a few lines.
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>

#define HELP_TOPICS   7
#define DAYS_MAX      3650   /* ten years: past that, keep them all (0) */

static const WCHAR *Question(int topic)
{
    switch (topic) {
    case 0: return TR(L"What is this program for?");
    case 1: return TR(L"Where are my conversations saved?");
    case 2: return TR(L"How do profiles keep the same conversations?");
    case 3: return TR(L"Why does a change wait for Claude to close?");
    case 4: return TR(L"I deleted a conversation by mistake");
    case 5: return TR(L"What is the session vault?");
    default: return TR(L"Before reinstalling Claude");
    }
}

static const WCHAR *Answer(int topic)
{
    switch (topic) {
    case 0:
        return TR(L"It runs several Claude accounts side by side, each in a window of its own: a profile, with its own sign-in, settings "
                  L"and conversations. Claude itself is not changed.");
    case 1:
        return TR(L"Their content is in the .claude folder of your user folder, shared by every profile. Each profile keeps its own list of them: "
                  L"titles, stars, archived ones.");
    case 2:
        return TR(L"Sync settings\x2026 links profiles. Each time one of them closes, what changed in its list goes to the others.");
    case 3:
        return TR(L"An open Claude keeps its list in memory and writes it back, so a change to an open profile is made once it closes.");
    case 4:
        return TR(L"Backup & Restore\x2026, then Clean up sessions\x2026: check it and click Restore.");
    case 5:
        return TR(L"A copy of each profile's list of sessions, kept outside Claude for the days set above. Recover sessions\x2026 puts "
                  L"a version back.");
    default:
        return TR(L"Back up Main first (Backup & Restore\x2026). Once Claude is installed again, open Main here and sign in; "
                  L"Recover sessions\x2026 brings its list back if it is gone.");
    }
}

static void ShowAnswer(HWND dialog, int topic)
{
    SetDlgItemTextW(dialog, IDC_G_ANSWER, topic >= 0 && topic < HELP_TOPICS ? Answer(topic) : L"");
}

static INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    static const int kHeadings[] = { IDC_G_VAULT, IDC_G_HELP };
    HWND questions = GetDlgItem(dialog, IDC_G_QUESTIONS);
    (void)lp;
    switch (message) {
    case WM_INITDIALOG: {
        LVCOLUMNW column;
        LVITEMW item;
        int topic;
        size_t i;
        for (i = 0; i < ARRAYSIZE(kHeadings); i++) Theme_SetStrong(GetDlgItem(dialog, kHeadings[i]));
        SendDlgItemMessageW(dialog, IDC_G_DAYS, EM_LIMITTEXT, 4, 0);
        SetDlgItemInt(dialog, IDC_G_DAYS, Util_GetSetting(SETTING_VAULT_DAYS, SETTING_VAULT_DAYS_DEFAULT), FALSE);
        ListView_SetExtendedListViewStyle(questions, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        ZeroMemory(&column, sizeof column);
        ListView_InsertColumn(questions, 0, &column);
        for (topic = 0; topic < HELP_TOPICS; topic++) {
            ZeroMemory(&item, sizeof item);
            item.mask = LVIF_TEXT;
            item.iItem = topic;
            item.pszText = (LPWSTR)Question(topic);
            ListView_InsertItem(questions, &item);
        }
        ListView_SetItemState(questions, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ShowAnswer(dialog, 0);
        Theme_SmoothView(questions);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_G_DAYS_UNIT);

    case WM_NOTIFY:
        if (((const NMHDR *)lp)->hwndFrom == questions && ((const NMHDR *)lp)->code == LVN_ITEMCHANGED) {
            const NMLISTVIEW *change = (const NMLISTVIEW *)lp;
            if ((change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED))
                ShowAnswer(dialog, change->iItem);
            return TRUE;
        }
        break;

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            BOOL read = FALSE;
            UINT days = GetDlgItemInt(dialog, IDC_G_DAYS, &read, FALSE);
            if (!read || days > DAYS_MAX) {
                Ui_Message(dialog, MB_ICONINFORMATION, TR(L"Enter a number of days from 0 to %d."), DAYS_MAX);
                SetFocus(GetDlgItem(dialog, IDC_G_DAYS));
                return TRUE;
            }
            if (!Util_SetSetting(SETTING_VAULT_DAYS, days)) Ui_Message(dialog, MB_ICONWARNING, TR(L"The settings could not be saved."));
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void Settings_Show(HWND owner)
{
    Ui_Dialog(owner, IDD_SETTINGS, SettingsProc, 0);
}
