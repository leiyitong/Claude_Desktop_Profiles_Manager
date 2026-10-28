#ifndef CLAUDE_DESKTOP_PROFILES_MANAGER_RESOURCE_H
#define CLAUDE_DESKTOP_PROFILES_MANAGER_RESOURCE_H

#define IDI_APP           1

/* The manager window's class: IDD_MAIN's in app.rc, the one the program registers and finds. */
#define APP_WINDOW_CLASS_NAME "ClaudeDesktopProfilesManagerMain"
#define APP_WINDOW_CLASS      L"" APP_WINDOW_CLASS_NAME

#define IDD_MAIN          100
#define IDD_PROFILE       101
#define IDD_PROFILE_NEW   121   /* a new profile: a form, not the property list */
#define IDD_SHIELD        122   /* the shield's dialog */
#define IDD_SEARXNG       123   /* SearXNG search set up in the profiles selected */
#define IDD_UPDATE        124   /* a release of main to update to, offered (ghupdate.c) */
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
#define IDD_PREVIEW       114
#define IDD_SETTINGS      115
#define IDD_ABOUT         116
#define IDD_PASSWORD      117
#define IDD_TEXTVIEW      120
#define IDD_TRANSFER      119
#define IDC_TR_TEXT       3000
#define IDC_TR_PROGRESS   3001
#define IDC_TR_STATUS     3002
#define IDC_TR_PAUSE      3003
#define IDC_TR_LATER      3004
/* The pane beside a property list (checklist.c), the same in every dialog that has one. */
#define IDC_PANE_TITLE    3010
#define IDC_PANE_LIST     3011
#define IDC_PANE_LABEL    3012
#define IDC_PANE_SEARCH   3013   /* the pane's search, below its title */
#define IDC_PANE_FIND     3014   /* its options, beside it */
#define IDC_W_LOGS        2900
#define IDC_W_DIAGNOSTICS 2901
#define IDC_T_NOTE        2910
#define IDC_T_TEXT        2911
#define IDC_T_COPY        2912
#define IDD_SIGNIN        118

/* Private sign-in helper (ids are local to this dialog). */
#define IDC_SIGNIN_URL           2800
#define IDC_SIGNIN_BROWSER       2801
#define IDC_SIGNIN_OPEN          2802
#define IDC_SIGNIN_PROFILE       2803
#define IDC_SIGNIN_HELP          2804
#define IDC_SIGNIN_URL_LABEL     2805
#define IDC_SIGNIN_BROWSER_LABEL 2806
#define IDC_SIGNIN_STATUS        2807
#define IDC_SIGNIN_FOLDERS       2808
#define IDC_SIGNIN_CLEANUP       2809
#define IDC_SIGNIN_SKIP          2810

/* the changelogs, embedded (RCDATA): CHANGELOG.md and CHANGELOG.zh-CN.md */
#define IDR_CHANGELOG_EN  200
#define IDR_CHANGELOG_ZH  201
/* windows-mcp-delta's source, a ZIP, embedded (RCDATA): extracted and run by uvx (windowsmcp.c) */
#define IDR_WINDOWS_MCP_DELTA 202

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
#define IDC_BACKUP         1038   /* Back up, in the column */
#define IDC_RESTORE_BACKUP 1048   /* Restore, beside it */
#define IDC_SHORTCUTS      1039   /* its menu, at the toolbar's end */
#define IDC_SYNC_NOW       1041   /* the list's menu */
#define IDC_SETTINGS       1042   /* the gear at the toolbar's end */
#define IDC_ABOUT          1043   /* About, right of it */
#define IDC_PROFILE_SEARCH 1044
#define IDC_PROFILE_SEARCH_LABEL 1045
#define IDC_PROFILE_FIND   1047
#define IDC_PRIVATE_SIGNIN 1046   /* the profile list's menu */
#define IDC_WINDOWS_MCP    1049   /* Windows-MCP added or taken away, in the column */
#define IDC_SHIELD         1050   /* the shield: a floating badge at the window's bottom-right corner */
#define IDC_SEARXNG        1051   /* SearXNG search added or taken away, in the column */
#define IDC_COWORK         1052   /* Cowork removed from Claude or restored, in the column */

/* sessions view of the main window */
#define IDC_S_SEARCH      1400
#define IDC_S_PROFILES    1402
#define IDC_S_TREE        1403
#define IDC_S_FIND        1406   /* the search options, beside the search box */
#define IDC_S_FILTER      1407   /* the filter menu's button */
#define IDC_S_FOLD        1408   /* every heading folded, or opened when all are: before the search box */

/* rename session dialog */
#define IDC_T_LABEL       1500
#define IDC_T_TITLE       1501

/* profile dialog */
#define IDC_P_LIST        1100
#define IDC_P_PREVIEW     1102
#define IDC_P_FOLDER      1103
#define IDC_P_ERROR       1104
/* the new profile's form (IDD_PROFILE_NEW), around the same preview and folder line */
#define IDC_P_COLOR       1101
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
#define IDC_P_NOTE_LABEL  1119
#define IDC_P_NOTE        1120
#define IDC_P_NAME        1121
#define IDC_P_SIGN_IN     1122

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
#define IDC_B_ALL         1803   /* every box checked */
#define IDC_B_NONE        1804   /* none */
#define IDC_B_INVERT      1805   /* each turned over */

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
#define IDC_Z_PROFILE     2310   /* the profiles to sync with, a check box each (SyncUi_PartnerBoxes), to + MAX_PROFILES */

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
#define IDC_G_PURGE       2612   /* Clean up sessions: an action of the page, no control of its own */
#define IDC_G_CODE_COPY   2613   /* Back up .claude: likewise */
#define IDC_G_LIST        2629   /* the kinds of settings, on the left (settings.c draws them; the template holds their place) */
#define IDC_G_PAGE        2631   /* the page of the kind chosen (settings.c draws it; the template holds its place) */
#define IDC_G_SEARCH      2632   /* above the kinds: the page shows the settings that match instead */
#define IDC_G_ZOOM        2615   /* SETTING_ZOOM changed: what the dialog's owner does then */

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

/* shield dialog */
#define IDC_H_STATE       3100   /* the state line, with its badge */
#define IDC_H_EXIT        3101   /* Claude's exit IP and country */
#define IDC_H_FACTS       3102   /* the IP's properties and the leak checks */
#define IDC_H_ENABLE      3103   /* turn the shield on or off */
#define IDC_H_RECHECK     3104   /* check now */
#define IDC_H_RULES       3105   /* the rules heading */
#define IDC_H_NO_DC       3106   /* end Claude when the exit is a datacenter IP */
#define IDC_H_NO_DNS      3107
#define IDC_H_NO_UDP      3108
#define IDC_H_STOP_TUN    3109   /* end Claude when no tunnel adapter is up */
#define IDC_H_STOP_PROXY  3110   /* end Claude when no proxy app runs */
#define IDC_H_STOP_UNKNOWN 3111  /* end Claude when the exit cannot be read */
#define IDC_H_SCORE_LABEL 3112
#define IDC_H_SCORE       3113   /* the lowest trust score allowed */
#define IDC_H_NO_IPV6     3114   /* end Claude when a public IPv6 could leak */
#define IDC_H_WARN        3115   /* warn only: never end Claude */
#define IDC_H_PROFILE_LABEL 3116
#define IDC_H_PROFILE     3117   /* a profile to set allowed regions for */
#define IDC_H_REGIONS     3118   /* that profile's allowed country codes */
#define IDC_H_LOG         3119   /* the shield's event log */
#define IDC_H_AVAIL       3120   /* Claude availability and service status */
#define IDC_H_REGIONS_LABEL 3121
#define IDC_H_LOG_LABEL   3122
/* IDD_SEARXNG */
#define IDC_SX_TEXT        3200
#define IDC_SX_URL_LABEL   3201
#define IDC_SX_URL         3202
#define IDC_SX_PRIVATE     3203
#define IDC_SX_NOTE        3204
#define IDC_SX_REMOVE      3205
/* IDD_UPDATE */
#define IDC_U_TEXT         3210
#define IDC_U_NOTES        3211

/* message box */
#define IDC_M_ICON        1300
#define IDC_M_TEXT        1301
#define IDC_M_OTHER       1302   /* Ui_Choose's second choice, between OK and Cancel */

#endif
