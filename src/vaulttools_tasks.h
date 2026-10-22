#ifndef CDM_VAULTTOOLS_TASKS_H
#define CDM_VAULTTOOLS_TASKS_H
#include "app.h"
BOOL Zip_ValidateAll(const ZipIn *zip, HANDLE cancel, WCHAR *error, size_t cch);
BOOL VaultTasks_AddRegistry(ZipOut *zip, const Profile *profile, const WCHAR *registryPath, const char *registryMember, BOOL code,
                           WCHAR *error, size_t cch);
/* NULL without a definitions manifest (legacy archive), or on error; *aware distinguishes those cases. */
char *VaultTasks_RestoreRegistry(const ZipIn *zip, const Profile *profile, const char *registryMember, const char *json, size_t length,
                                BOOL code, BOOL *aware, size_t *outLength, DWORD *error);
BOOL VaultTasks_ValidateCoverage(const ZipIn *zip, const char *name);
BOOL VaultTasks_IsCoverage(const char *name);
BOOL VaultTasks_Describe(const ZipIn *zip, const char *name, WCHAR *out, size_t cch);
#endif
