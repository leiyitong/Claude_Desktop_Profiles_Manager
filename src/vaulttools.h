#ifndef CDM_VAULTTOOLS_H
#define CDM_VAULTTOOLS_H
#define IDD_VAULT_PICK 6500
#define IDC_VAULT_QUERY 6501
#define IDC_VAULT_ROWS 6502
#define IDC_VAULT_ALL 6503
#define IDC_VAULT_NOTE 6504
#define IDC_PURGE_OPEN 6505
#define IDD_PURGE_OPEN 6506
#define IDC_PURGE_TARGET 6507
#define IDC_PURGE_EXPLAIN 6508
#define IDC_VAULT_COUNT 6509


#ifndef RC_INVOKED
#include "app.h"
typedef struct VaultRestoreItem {
    WCHAR key[SESSION_ID_CCH], title[SESSION_TITLE_CCH];
    int status; /* 0 new, 1 changed, 2 same, 3 deleted, 4 unavailable */
} VaultRestoreItem;
int SessionVault_RestoreItems(const ProfileList *list, int index, const WCHAR *listName, const WCHAR *version, VaultRestoreItem **items);
BOOL SessionVault_RestoreSelected(const ProfileList *list, int index, const WCHAR *listName, const WCHAR *version,
                                  const WCHAR *const *keys, int count, SyncReport *report);
BOOL VaultTools_Choose(HWND owner, const VaultRestoreItem *items, int count, BOOL syncGroup, BOOL *chosen);
/* One archived bit changed, preserving every other entry and unknown member. */
char *VaultTools_ArchiveSet(const char *json, size_t length, const WCHAR *id, BOOL archived, size_t *outLength);
BOOL SessionPurge_CanOpen(const PurgeItem *item);
BOOL Backup_CheckArchive(HWND owner, const WCHAR *path);
#endif /* RC_INVOKED */
#endif
