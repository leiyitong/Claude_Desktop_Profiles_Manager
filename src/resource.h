#ifndef CLAUDE_DESKTOP_PROFILES_MANAGER_RESOURCE_H
#define CLAUDE_DESKTOP_PROFILES_MANAGER_RESOURCE_H

#define IDI_APP           1

/* The manager window's class: IDD_MAIN's in app.rc, the one the program registers and finds. */
#define APP_WINDOW_CLASS_NAME "ClaudeDesktopProfilesManagerMain"
#define APP_WINDOW_CLASS      L"" APP_WINDOW_CLASS_NAME

#define IDD_MAIN          100
#define IDD_PROFILE       101
#define IDD_UNINSTALL     102
#define IDD_MESSAGE       103
#define IDD_TITLE         104
#define IDD_SYNC          105
#define IDD_LINK          106
#define IDD_BACKUP        107
#define IDD_RESTORE       108
#define IDD_PURGE         109
#define IDD_SYNC_ITEMS    110
#define IDD_CONFLICTS     111
#define IDD_SYNC_SETUP    112
#define IDD_BACKUP_HUB    113
#define IDD_PREVIEW       114
#define IDD_SETTINGS      115
#define IDD_ABOUT         116
#define IDD_PASSWORD      117

/* the changelogs, embedded (RCDATA): CHANGELOG.md and CHANGELOG.zh-CN.md */
#define IDR_CHANGELOG_EN  200
#define IDR_CHANGELOG_ZH  201

/* main window; the ids of the commands its menus give are the buttons' they stand for */
#define IDC_STATUS         1000
#define IDC_STATUS_ACTION  1001
#define IDC_LIST           1002
#define IDC_OPEN           1003
#define IDC_NEW            1004
#define IDC_EDIT           1005
#define IDC_DELETE         1006
#define IDC_DEFAULT        1007
#define IDC_NOTE           1008
#define IDC_SC_DESKTOP     1010   /* the shortcuts menu */
#define IDC_SC_SAVEAS      1011
#define IDC_SC_PIN         1012
#define IDC_UPDATE         1015
#define IDC_SC_START       1016
#define IDC_SESSIONS       1017
#define IDC_MERGE          1019   /* the list's menu */
#define IDC_STOP           1021
#define IDC_RESTART        1028
#define IDC_SYNC           1029
#define IDC_REPAIR         1030
#define IDC_VERSION        1031
#define IDC_PROGRESS       1032
#define IDC_COPY_ALL       1033   /* the list's menu */
#define IDC_MOVE_ALL       1034
#define IDC_BACKUP         1038   /* Backup & Restore, in the column */
#define IDC_SHORTCUTS      1039   /* its menu, at the toolbar's end */
#define IDC_SYNC_NOW       1041   /* the list's menu */
#define IDC_SETTINGS       1042   /* the gear at the toolbar's end */
#define IDC_ABOUT          1043   /* About, right of it */

/* sessions view of the main window */
#define IDC_S_SEARCH      1400
#define IDC_S_PROFILES    1402
#define IDC_S_TREE        1403
#define IDC_S_FIND        1406   /* the search options, beside the search box */
#define IDC_S_FILTER      1407   /* the filter menu's button */

/* rename session dialog */
#define IDC_T_LABEL       1500
#define IDC_T_TITLE       1501

/* profile dialog */
#define IDC_P_NAME        1100
#define IDC_P_COLOR       1101
#define IDC_P_PREVIEW     1102
#define IDC_P_FOLDER      1103
#define IDC_P_ERROR       1104
#define IDC_P_OPEN        1105
#define IDC_P_STARTUP     1106
#define IDC_P_COPY        1107
#define IDC_P_BADGE       1108
#define IDC_P_PICTURE     1109
#define IDC_P_NO_PICTURE  1110
#define IDC_P_SYNC        1111
#define IDC_P_SYNC_LABEL  1112
#define IDC_P_NAME_LABEL  1113
#define IDC_P_COLOR_LABEL 1114
#define IDC_P_BADGE_LABEL 1115
#define IDC_P_SYNC_ITEMS  1116
#define IDC_P_TITLE_LABEL 1117
#define IDC_P_TITLE       1118

/* uninstall dialog */
#define IDC_U_KEEP        1200
#define IDC_U_LABEL       1201
#define IDC_U_LIST        1202
#define IDC_U_HINT        1203

/* sessions sent between profiles */
#define IDC_Y_TEXT        1600
#define IDC_Y_TO_LABEL    1603
#define IDC_Y_LIST        1604
#define IDC_Y_NOTE        1606

/* the profile that opens a claude:// link */
#define IDC_L_TEXT        1700
#define IDC_L_LINK        1701
#define IDC_L_LIST        1702

/* a profile's backup and restore */
#define IDC_B_TEXT        1800
#define IDC_B_LIST        1801
#define IDC_B_NOTE        1802

/* sessions recovered from the vault */
#define IDC_R_TEXT          1900
#define IDC_R_PROFILE_LABEL 1901
#define IDC_R_PROFILE       1902
#define IDC_R_VERSION_LABEL 1903
#define IDC_R_LIST          1904
#define IDC_R_NOTE          1905

/* conversations no profile lists, cleaned up */
#define IDC_C_TEXT        2000
#define IDC_C_LIST        2001
#define IDC_C_BACKUP      2002
#define IDC_C_NOTE        2003
#define IDC_C_RESTORE     2004
#define IDC_C_SHOW_LABEL  2005
#define IDC_C_PROFILE     2006
#define IDC_C_ALL         2007

/* what profiles keep the same */
#define IDC_I_TEXT        2100
#define IDC_I_LIST        2101
#define IDC_I_NOTE        2102

/* what a profile syncs, and with which */
#define IDC_Z_TEXT        2300
#define IDC_Z_LIST        2301
#define IDC_Z_ITEMS_LABEL 2302
#define IDC_Z_ITEMS       2303
#define IDC_Z_NOTE        2304
#define IDC_Z_SYNC_NOW    2305

/* Backup & Restore: each button the command it stands for */
#define IDC_H_PROFILE     2400
#define IDC_H_SESSIONS    2401
#define IDC_H_CODE_LABEL  2402
#define IDC_H_BACKUP      2403
#define IDC_H_RESTORE     2404
#define IDC_H_EXPORT      2405
#define IDC_H_IMPORT      2406
#define IDC_H_RECOVER     2407
#define IDC_H_PURGE       2408
#define IDC_H_CODE        2409
#define IDC_H_NOTE        2410
#define IDC_H_WEEKLY      2411   /* not a command: it sets SETTING_WEEKLY_BACKUP */

/* conflicts of a sync, settled */
#define IDC_X_TEXT        2200
#define IDC_X_LIST        2201
#define IDC_X_LABEL       2202
#define IDC_X_PROFILE     2203
#define IDC_X_NOTE        2204
#define IDC_X_ICON        2205

/* a session's conversation, previewed */
#define IDC_V_TITLE       2500
#define IDC_V_TEXT        2501

/* settings: the language and the vault's days */
#define IDC_G_CODE        2604
#define IDC_G_WEEKLY      2605   /* SETTING_WEEKLY_BACKUP, as Backup & Restore's IDC_H_WEEKLY */
#define IDC_G_VAULT       2600
#define IDC_G_DAYS_LABEL  2601
#define IDC_G_DAYS        2602
#define IDC_G_DAYS_UNIT   2603
#define IDC_G_LANGUAGE_LABEL 2607
#define IDC_G_LANGUAGE    2608
#define IDC_G_APPLY       2609

/* a password: a backup's sign-in for any PC */
#define IDC_K_TEXT        2800
#define IDC_K_LABEL       2801
#define IDC_K_PASSWORD    2802
#define IDC_K_AGAIN_LABEL 2803
#define IDC_K_AGAIN       2804

/* About: the version, help and the changelog */
#define IDC_W_TEXT        2700
#define IDC_W_TOPICS      2701
#define IDC_W_NAME        2702
#define IDC_W_VERSION     2703

/* message box */
#define IDC_M_ICON        1300
#define IDC_M_TEXT        1301

#endif
