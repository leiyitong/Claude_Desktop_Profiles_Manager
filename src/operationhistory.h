#ifndef CDPM_OPERATIONHISTORY_H
#define CDPM_OPERATIONHISTORY_H
#include "app.h"
typedef enum OperationKind { OPERATION_SYNC, OPERATION_IMPORT, OPERATION_RESTORE, OPERATION_MIGRATION, OPERATION_SNAPSHOT } OperationKind;
void OperationHistory_Record(OperationKind kind, const WCHAR *profileFolder, DWORD error, const WCHAR *backupPath);
void OperationHistory_RecordDetail(OperationKind kind, const WCHAR *profileFolder, DWORD error, const WCHAR *backupPath, const WCHAR *detail);
void OperationHistory_RecordReport(OperationKind kind, const ProfileList *profiles, const SyncReport *report);
void OperationHistory_Show(HWND owner);
#endif
