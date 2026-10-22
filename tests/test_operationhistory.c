#include "../src/operationhistory.h"
#include <stdio.h>
#include <objbase.h>
#define OperationHistory_RecordDetail Fixture_RecordDetail
#define OperationHistory_Record Fixture_Record
#define OperationHistory_RecordReport Fixture_RecordReport
#define OperationHistory_Show Fixture_Show
#include "../src/operationhistory.c"
#undef OperationHistory_RecordDetail
#undef OperationHistory_Record
#undef OperationHistory_RecordReport
#undef OperationHistory_Show
static int checks, failures;
static void Check(const char *label, BOOL ok) { checks++;if(!ok){failures++;printf("FAIL %s\n",label);} }
int wmain(void) {
    WCHAR temp[MAX_PATH],root[MAX_PATH],path[LONG_PATH_CCH],id[40];GUID guid;SyncReport *report;OpHistory *history;DWORD written,error;HANDLE file;int i;
    if(!GetTempPathW(ARRAYSIZE(temp),temp)||FAILED(CoCreateGuid(&guid))||!StringFromGUID2(&guid,id,ARRAYSIZE(id))||
       FAILED(StringCchPrintfW(root,ARRAYSIZE(root),L"%sCDPM-operation-%s",temp,id))||!CreateDirectoryW(root,NULL))return 2;
    Util_SetStateDir(root);report=(SyncReport *)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *report);history=(OpHistory *)HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof *history);
    if(!report||!history)return 2;
    Check("private operation path",OpPath(path,ARRAYSIZE(path))&&wcsstr(path,root)==path);
    Check("missing history empty",OpRead(path,history)&&!history->count);
    Fixture_Record(OPERATION_IMPORT,L"Claude-fixture",0,L"C:\\fixture\\before.zip");
    Check("record round trip",OpRead(path,history)&&history->count==1&&!wcscmp(history->entries[0].folder,L"Claude-fixture"));
    report->added=3;report->updated=2;report->failed=1;report->waiting=1;report->profileCount=1;StringCchCopyW(report->error,ARRAYSIZE(report->error),L"Could not read fixture (Windows error 5)");
    StringCchCopyW(report->profiles[0].folder,FOLDER_CCH,L"Claude-fixture");report->profiles[0].waiting=TRUE;report->profiles[0].added=3;
    Fixture_RecordReport(OPERATION_SYNC,NULL,report);
    Check("per-profile wait and counts persisted",OpRead(path,history)&&history->count==3&&history->entries[0].waiting&&history->entries[0].added==3&&!history->entries[0].error);
    Check("failure reason retained only in summary",history->entries[1].failed==1&&wcsstr(history->entries[1].detail,L"Windows error 5"));
    for(i=0;i<70;i++)Fixture_Record(OPERATION_RESTORE,L"Claude-fixture",ERROR_ACCESS_DENIED,L"");
    Check("result history bounded",OpRead(path,history)&&history->count==64);
    file=CreateFileW(path,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);Check("corrupt fixture",file!=INVALID_HANDLE_VALUE);
    if(file!=INVALID_HANDLE_VALUE){WriteFile(file,"broken",6,&written,NULL);CloseHandle(file);}
    Fixture_Record(OPERATION_RESTORE,L"Claude-fixture",0,NULL);
    {DWORD length;char *raw=Util_ReadFile(path,100,FALSE,&length);Check("malformed history never overwritten",raw&&length==6&&!memcmp(raw,"broken",6));if(raw)HeapFree(GetProcessHeap(),0,raw);}
    HeapFree(GetProcessHeap(),0,report);HeapFree(GetProcessHeap(),0,history);Util_SetStateDir(NULL);Check("private cleanup",Util_DeleteTree(root,&error));
    printf("operation history: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
