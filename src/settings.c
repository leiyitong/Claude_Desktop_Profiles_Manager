/*
 * Settings, from the gear at the toolbar's end: the interface language,
 * how long the session vault keeps the versions of each list
 * (sessionvault.c), and the weekly copy of .claude (sessionpurge.c), as
 * Backup & Restore has it. Help and the changelog are in About (about.c).
 */
#include "app.h"
#include "resource.h"
#include <commctrl.h>

#define DAYS_MAX      3650   /* ten years: past that, keep them all (0) */

/* The dialog's state, in DWLP_USER. */
typedef struct SettingsDialog {
    SettingsLanguageProc applyLanguage;   /* the program takes a new language; the dialog follows it itself */
    UINT days;                            /* the vault's days as saved */
    BOOL weekly;                          /* the weekly copy of .claude, as saved */
} SettingsDialog;

/* Windows' language first, then each of the program's, the one in use
 * chosen: a choice's index is its language + 1. */
static void FillLanguages(HWND combo)
{
    int language;
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (language = -1; language < Localize_LanguageCount(); language++)
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)Localize_LanguageName(language));
    SendMessageW(combo, CB_SETCURSEL, (WPARAM)(Localize_CurrentLanguage() + 1), 0);
}

/* What the dialog shows differs from what is saved and in use. */
static BOOL Changed(HWND dialog, const SettingsDialog *state)
{
    LRESULT choice = SendDlgItemMessageW(dialog, IDC_G_LANGUAGE, CB_GETCURSEL, 0, 0);
    BOOL read = FALSE;
    UINT days = GetDlgItemInt(dialog, IDC_G_DAYS, &read, FALSE);
    BOOL weekly = IsDlgButtonChecked(dialog, IDC_G_WEEKLY) == BST_CHECKED;
    return (choice != CB_ERR && (int)choice - 1 != Localize_CurrentLanguage()) || !read || days != state->days || weekly != state->weekly;
}

/* Apply only while there is something to apply; the focus it had goes to OK. */
static void UpdateApply(HWND dialog, const SettingsDialog *state)
{
    HWND apply = GetDlgItem(dialog, IDC_G_APPLY);
    BOOL changed = Changed(dialog, state);
    if (!changed && GetFocus() == apply) SendMessageW(dialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dialog, IDOK), TRUE);
    EnableWindow(apply, changed);
}

/* The dialog in the language now in use: its captions, the texts it fills,
 * then its layout and theme, as a new language gives them. */
static void FollowLanguage(HWND dialog)
{
    Localize_Window(dialog);
    FillLanguages(GetDlgItem(dialog, IDC_G_LANGUAGE));
    Theme_FitDialog(dialog);
    Theme_Apply(dialog);
}

/* What the dialog shows, saved (the days and the weekly copy only when they
 * changed) and applied; FALSE, and says why, when the days are not a number
 * it takes. */
static BOOL ApplySettings(HWND dialog, SettingsDialog *state)
{
    LRESULT choice = SendDlgItemMessageW(dialog, IDC_G_LANGUAGE, CB_GETCURSEL, 0, 0);
    int languageBefore = Localize_EffectiveLanguage();
    BOOL read = FALSE, weekly = IsDlgButtonChecked(dialog, IDC_G_WEEKLY) == BST_CHECKED;
    UINT days = GetDlgItemInt(dialog, IDC_G_DAYS, &read, FALSE);
    if (!read || days > DAYS_MAX) {
        Ui_Message(dialog, MB_ICONINFORMATION, TR(L"Enter a number of days from 0 to %d."), DAYS_MAX);
        SendMessageW(dialog, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dialog, IDC_G_DAYS), TRUE);
        return FALSE;
    }
    if (days != state->days) {
        if (Util_SetSetting(SETTING_VAULT_DAYS, days)) state->days = days;
        else Ui_Message(dialog, MB_ICONWARNING, TR(L"The settings could not be saved."));
    }
    if (weekly != state->weekly) {
        if (Util_SetSetting(SETTING_WEEKLY_BACKUP, weekly)) state->weekly = weekly;
        else Ui_Message(dialog, MB_ICONWARNING, TR(L"The settings could not be saved."));
    }
    if (choice != CB_ERR && (int)choice - 1 != Localize_CurrentLanguage() && state->applyLanguage) state->applyLanguage((int)choice - 1);
    if (Localize_EffectiveLanguage() != languageBefore) FollowLanguage(dialog);
    else SendDlgItemMessageW(dialog, IDC_G_LANGUAGE, CB_SETCURSEL, (WPARAM)(Localize_CurrentLanguage() + 1), 0);   /* one not taken goes back */
    UpdateApply(dialog, state);
    return TRUE;
}

static INT_PTR CALLBACK SettingsProc(HWND dialog, UINT message, WPARAM wp, LPARAM lp)
{
    static const int kHeadings[] = { IDC_G_LANGUAGE_LABEL, IDC_G_VAULT, IDC_G_CODE };
    SettingsDialog *state = (SettingsDialog *)GetWindowLongPtrW(dialog, DWLP_USER);
    switch (message) {
    case WM_INITDIALOG: {
        size_t i;
        state = (SettingsDialog *)lp;
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        state->days = Util_GetSetting(SETTING_VAULT_DAYS, SETTING_VAULT_DAYS_DEFAULT);
        state->weekly = Util_GetSetting(SETTING_WEEKLY_BACKUP, 0) != 0;
        CheckDlgButton(dialog, IDC_G_WEEKLY, state->weekly ? BST_CHECKED : BST_UNCHECKED);
        for (i = 0; i < ARRAYSIZE(kHeadings); i++) Theme_SetStrong(GetDlgItem(dialog, kHeadings[i]));
        FillLanguages(GetDlgItem(dialog, IDC_G_LANGUAGE));
        SendDlgItemMessageW(dialog, IDC_G_DAYS, EM_LIMITTEXT, 4, 0);
        SetDlgItemInt(dialog, IDC_G_DAYS, state->days, FALSE);
        UpdateApply(dialog, state);
        return TRUE;
    }

    case WM_CTLCOLORSTATIC:
        return Theme_CtlColor(message, wp, lp, IDC_G_DAYS_UNIT);

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wp)) {
        case IDC_G_LANGUAGE:
            if (HIWORD(wp) == CBN_SELCHANGE) UpdateApply(dialog, state);
            return TRUE;
        case IDC_G_DAYS:
            if (HIWORD(wp) == EN_CHANGE) UpdateApply(dialog, state);
            return TRUE;
        case IDC_G_WEEKLY:
            if (HIWORD(wp) == BN_CLICKED) UpdateApply(dialog, state);
            return TRUE;
        case IDC_G_APPLY:
            if (HIWORD(wp) == BN_CLICKED) ApplySettings(dialog, state);
            return TRUE;
        case IDOK:
            if (!Changed(dialog, state) || ApplySettings(dialog, state)) EndDialog(dialog, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void Settings_Show(HWND owner, SettingsLanguageProc applyLanguage)
{
    SettingsDialog state;
    ZeroMemory(&state, sizeof state);
    state.applyLanguage = applyLanguage;
    Ui_Dialog(owner, IDD_SETTINGS, SettingsProc, (LPARAM)&state);
}
