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
#define IDC_UNINSTALL      1013   /* the program menu */
#define IDC_UPDATE         1015
#define IDC_SC_START       1016
#define IDC_SESSIONS       1017
#define IDC_MERGE          1019   /* the sessions menu */
#define IDC_STOP           1021
#define IDC_RESTORE        1022
#define IDC_PURGE          1023
#define IDC_BACKUP_CODE    1024
#define IDC_MENU_APP       1025
#define IDC_MENU_SESSIONS  1026
#define IDC_MENU_SHORTCUTS 1027
#define IDC_RESTART        1028
#define IDC_SYNC           1029
#define IDC_REPAIR         1030
#define IDC_VERSION        1031
#define IDC_PROGRESS       1032
#define IDC_COPY_ALL       1033   /* the sessions menu */
#define IDC_MOVE_ALL       1034
#define IDC_SET_UP_LINKS   1035   /* the program menu */
#define IDC_SAME_STOP      1036   /* the sessions menu */
#define IDC_MENU_HELP      1037   /* Help: its questions (IDC_HELP is a cursor's id) */

/* sessions view of the main window */
#define IDC_S_SEARCH      1400
#define IDC_S_ARCHIVED    1401
#define IDC_S_PROFILES    1402
#define IDC_S_TREE        1403
#define IDC_S_DETAILS     1404
#define IDC_S_PARTS       1405

/* session title dialog */
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

/* message box */
#define IDC_M_ICON        1300
#define IDC_M_TEXT        1301

#endif
