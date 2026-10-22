#ifndef CDPM_PROFILETOOLS_H
#define CDPM_PROFILETOOLS_H
#ifndef RC_INVOKED
#include "app.h"
#endif
#define IDD_PROFILE_TOOLS 6300
#define IDC_PT_PROFILE 6301
#define IDC_PT_USAGE 6302
#define IDC_PT_HEALTH 6303
#define IDC_PT_SPACE 6304
#define IDC_PT_EXPORT 6305
#define IDC_PT_IMPORT 6306
#define IDC_PT_SNAPSHOTS 6307
#define IDC_PT_HISTORY 6308
#define IDC_PT_NOTE 6309
#define IDD_PT_PROGRESS 6310
#define IDC_PT_PROGRESS_TEXT 6311
#define IDD_PT_MAPPING 6312
#define IDC_PT_MAP_NOTE 6313
#define IDC_PT_MAP_TARGET 6314
#define IDD_PT_SNAPSHOTS 6315
#define IDC_PT_SNAPSHOT_LIST 6316
#define IDC_PT_SNAPSHOT_CREATE 6317
#define IDC_PT_SNAPSHOT_RESTORE 6318
#define IDC_PT_SNAPSHOT_NOTE 6319
#define IDM_PROFILE_TOOLS 6320
#ifndef RC_INVOKED
void Gui_ProfileTools(HWND owner, const ProfileList *profiles, const WCHAR *selectedFolder);
void ProfileTools_Show(HWND owner, const ProfileList *profiles, const WCHAR *selectedFolder);
BOOL ProfileTools_UsageText(const char *json, size_t length, WCHAR *out, size_t cch);
typedef struct ProfileSpace { ULONGLONG bytes, cacheBytes, files, skipped; BOOL complete; } ProfileSpace;
BOOL ProfileTools_ScanSpace(const WCHAR *path, HANDLE cancel, ProfileSpace *space);
BOOL ProfileTools_HealthText(const ProfileList *profiles, HANDLE cancel, WCHAR *out, size_t cch);
BOOL ProfileTools_Export(const ProfileList *profiles, const WCHAR *path, DWORD *error);
BOOL ProfileTools_Import(const WCHAR *path, const ProfileList *profiles, const int *mapping, int mappingCount, DWORD *error);
void ProfileTools_SnapshotEvent(const ProfileList *profiles);
BOOL ProfileTools_SaveSnapshot(const Profile *profile, WCHAR *saved, size_t cch);
BOOL ProfileTools_RestoreSnapshot(const Profile *profile, const WCHAR *archive, WCHAR *backup, size_t cch);
#endif
#endif
