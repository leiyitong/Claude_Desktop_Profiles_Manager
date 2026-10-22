/* All profile, code, manager state and registry data live in one GUID fixture. */
#include "../src/profiletools.h"
#include "../src/operationhistory.h"
#include <stdio.h>
#include <string.h>
#include <objbase.h>
static int checks, failures;
static void Check(const char *label, BOOL ok) { checks++; if (!ok) { failures++; printf("FAIL %s (error %lu)\n", label, GetLastError()); } }
static BOOL Whole(const WCHAR *path, const char *raw) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); DWORD written; BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return FALSE; ok = WriteFile(f, raw, (DWORD)strlen(raw), &written, NULL) && written == strlen(raw); CloseHandle(f); return ok;
}
static BOOL Has(const WCHAR *path, const char *needle) {
    DWORD length; char *raw = Util_ReadFile(path, 1024 * 1024, FALSE, &length); BOOL has = raw && strstr(raw, needle) != NULL;
    if (raw) HeapFree(GetProcessHeap(), 0, raw); return has;
}
static BOOL BadExtra(const WCHAR *sourcePath, const WCHAR *targetPath) {
    ZipIn *source = Zip_Open(sourcePath); ZipOut *target; DWORD error = 0, i, bytes, written; BOOL ok = source != NULL; char *raw; HANDLE file;
    target = Zip_Create(targetPath, &error); if (!target) { if (source) Zip_Free(source); return FALSE; }
    for (i = 0; ok && i < (DWORD)Zip_Count(source); i++) {
        raw = (char *)Zip_Read(source, (int)i, 4*1024*1024, &bytes); if (!raw) { ok = FALSE; break; }
        ok = Zip_AddData(target, Zip_Name(source, (int)i), raw, bytes); HeapFree(GetProcessHeap(), 0, raw);
    }
    if (source) Zip_Free(source); if (ok) ok = Zip_AddData(target,"ignored.bin","x",1); ok = Zip_Close(target,ok) && ok;
    if (!ok) return FALSE;
    raw = Util_ReadFile(targetPath,8*1024*1024,FALSE,&bytes); if (!raw) return FALSE;
    ok = FALSE;
    for (i=0;i+12<bytes;i++) if (!memcmp(raw+i,"ignored.bin",11)) { raw[i+11]^=0x40;ok=TRUE;break; }
    if (ok) {
        file=CreateFileW(targetPath,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        ok=file!=INVALID_HANDLE_VALUE && WriteFile(file,raw,bytes,&written,NULL) && written==bytes;
        if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    }
    HeapFree(GetProcessHeap(),0,raw);return ok;
}
int wmain(void) {
    WCHAR temporary[MAX_PATH], root[MAX_PATH], state[MAX_PATH], profileDir[MAX_PATH], code[MAX_PATH], key[160], id[40], path[LONG_PATH_CCH];
    WCHAR archive[LONG_PATH_CCH], snapshot[LONG_PATH_CCH], backup[LONG_PATH_CCH], *text;
    ProfileList *list; ProfileSpace space; GUID guid; HKEY fixture = NULL; HANDLE cancel; DWORD error = 0, *pixels, stamp = 0; int mapping[2] = { 1, 0 };
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED); BOOL override = FALSE; ZipIn *zip; char *raw; DWORD length; int i;
    const char usage[] = "{\"version\":2,\"samples\":[{\"t\":1780000000000,\"org\":\"o1\",\"u\":{\"fh\":0,\"sd\":100,\"future\":4,\"\xE6\x96\xB0\xE6\x8C\x87\xE6\xA0\x87\":7}},{\"t\":1780000000001,\"org\":\"o2\",\"u\":{}},{\"t\":1780000000002,\"u\":{\"sd\":3.5}}]}";
    if (!GetTempPathW(ARRAYSIZE(temporary), temporary) || FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(&guid, id, ARRAYSIZE(id)) ||
        FAILED(StringCchPrintfW(root, ARRAYSIZE(root), L"%sCDPM-profile-tools-%s", temporary, id)) || !CreateDirectoryW(root, NULL)) return 2;
    StringCchPrintfW(state, ARRAYSIZE(state), L"%s\\state", root); CreateDirectoryW(state, NULL); Util_SetStateDir(state);
    StringCchPrintfW(code, ARRAYSIZE(code), L"%s\\code", root); CreateDirectoryW(code, NULL); SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR", code);
    StringCchPrintfW(key, ARRAYSIZE(key), L"Software\\CDPM-profile-tools-%s", id);
    Check("private registry root", RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &fixture, NULL) == ERROR_SUCCESS);
    if (!fixture) return 2;
    Check("override HKCU for every profile setter", RegOverridePredefKey(HKEY_CURRENT_USER, fixture) == ERROR_SUCCESS); override = TRUE;
    list = (ProfileList *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *list);
    text = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1024 * 1024 * sizeof(WCHAR));
    pixels = (DWORD *)HeapAlloc(GetProcessHeap(), 0, PICTURE_SIZE * PICTURE_SIZE * sizeof(DWORD));
    if (!list || !text || !pixels) return 2;
    list->count = 2;
    for (i = 0; i < 2; i++) {
        Profile *p = &list->items[i]; StringCchPrintfW(p->folder, FOLDER_CCH, L"Claude-fixture-%d", i); StringCchPrintfW(p->name, LABEL_CCH, L"Fixture %d", i);
        StringCchPrintfW(profileDir, ARRAYSIZE(profileDir), L"%s\\profile%d", root, i); CreateDirectoryW(profileDir, NULL);
        StringCchCopyW(p->storageDir, MAX_PATH, profileDir); StringCchCopyW(p->dataDir, MAX_PATH, profileDir); p->color = i; p->syncGroup = 19;
        p->syncItems = SYNC_ITEM_SESSIONS_NEW | SYNC_ITEM_DETAILS_MODEL; StringCchCopyW(p->note, ARRAYSIZE(p->note), L"fixture note");
    }
    StringCchCopyW(list->defaultFolder, FOLDER_CCH, list->items[0].folder);
    Check("usage unknown metrics and missing values", ProfileTools_UsageText(usage, sizeof usage-1, text, 1024*1024) &&
          wcsstr(text, L"future=4") && wcsstr(text, L"\x65B0\x6307\x6807=7") && wcsstr(text, L"Not recorded") && wcsstr(text, L"3.5"));
    Check("unknown usage version rejected", !ProfileTools_UsageText("{\"version\":3,\"samples\":[]}", 26, text, 1024*1024));
    Check("truncated report rejected", !ProfileTools_UsageText(usage, sizeof usage-1, text, 30));
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\sample.bin", list->items[0].storageDir); Whole(path,"12345");
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\LOCK", list->items[0].storageDir); Whole(path,"not read");
    Check("logical files skip LOCK", ProfileTools_ScanSpace(list->items[0].storageDir, NULL, &space) && space.bytes == 5 && space.files == 1 && space.skipped == 1);
    cancel = CreateEventW(NULL, TRUE, TRUE, NULL);
    Check("space cancellation", !ProfileTools_ScanSpace(root, cancel, &space) && GetLastError() == ERROR_CANCELLED); CloseHandle(cancel);
    for (i = 0; i < PICTURE_SIZE*PICTURE_SIZE; i++) pixels[i] = 0xff336699;
    Check("private real PNG save", Icons_SavePicture(list->items[0].folder, pixels, &stamp)); list->items[0].picture = stamp;
    StringCchPrintfW(archive, ARRAYSIZE(archive), L"%s\\manager.zip", root);
    Check("export manager-only package", ProfileTools_Export(list, archive, &error)); zip = Zip_Open(archive);
    Check("package stores actual picture and excludes data paths", zip && Zip_Find(zip,"pictures/00.png") >= 0 && Zip_Find(zip,"config.json") < 0);
    if (zip) Zip_Free(zip);
    Check("explicit cross-profile mapping imports", ProfileTools_Import(archive, list, mapping, 2, &error));
    StringCchPrintfW(path, ARRAYSIZE(path), REG_PROFILES L"\\%s", list->items[1].folder);
    Check("name follows mapping", Util_RegGetString(HKEY_CURRENT_USER, path, L"Name", text, 1024*1024) && !wcscmp(text,L"Fixture 0"));
    Check("group ID rebuilt by members", Util_RegGetDword(HKEY_CURRENT_USER,path,L"SyncSessions", &error) && error == 1);
    Check("actual mapped PNG stamp saved", Util_RegGetDword(HKEY_CURRENT_USER,path,L"Picture",&error) && error != 0);
    Check("default follows mapping", Util_RegGetString(HKEY_CURRENT_USER,REG_ROOT,L"DefaultProfile",text,1024*1024) && !wcscmp(text,list->items[1].folder));
    { WCHAR complete[LONG_PATH_CCH], bad[LONG_PATH_CCH]; ZipIn *source; ZipOut *broken; int item; BOOL made = TRUE;
      Check("two current PNG fixtures",Icons_SavePicture(list->items[0].folder,pixels,&stamp) && Icons_SavePicture(list->items[1].folder,pixels,&stamp));
      list->items[0].picture = list->items[1].picture = stamp; StringCchPrintfW(complete,ARRAYSIZE(complete),L"%s\\two-pictures.zip",root);
      Check("two-picture source package",ProfileTools_Export(list,complete,&error));source=Zip_Open(complete);
      StringCchPrintfW(bad,ARRAYSIZE(bad),L"%s\\bad-second-picture.zip",root);broken=Zip_Create(bad,&error);
      if(!source||!broken)made=FALSE;
      for(item=0;made&&item<Zip_Count(source);item++) {
          DWORD bytes;void *data=Zip_Read(source,item,4*1024*1024,&bytes);
          if(!data){made=FALSE;break;}
          if(!strcmp(Zip_Name(source,item),"pictures/01.png"))made=Zip_AddData(broken,Zip_Name(source,item),"broken",6);
          else made=Zip_AddData(broken,Zip_Name(source,item),data,bytes);
          HeapFree(GetProcessHeap(),0,data);
      }
      if(source)Zip_Free(source);if(broken)made=Zip_Close(broken,made)&&made;
      Check("malformed later picture rejected before metadata writes",made&&!ProfileTools_Import(bad,list,mapping,2,&error));
      Check("failed preflight preserves mapped metadata",Util_RegGetString(HKEY_CURRENT_USER,path,L"Name",text,1024*1024)&&!wcscmp(text,L"Fixture 0"));
    }
    { Profile *other=&list->items[2]; ZeroMemory(other,sizeof *other);list->count=3;
      StringCchCopyW(other->folder,FOLDER_CCH,L"Claude-unmapped");StringCchCopyW(other->name,LABEL_CCH,L"Unmapped");
      StringCchPrintfW(other->storageDir,MAX_PATH,L"%s\\unmapped",root);StringCchCopyW(other->dataDir,MAX_PATH,other->storageDir);CreateDirectoryW(other->storageDir,NULL);
      other->color=-1;other->syncGroup=1;
      Check("unmapped group fixture",Profiles_SetSyncGroup(other->folder,1));
      Check("existing unmapped group ID never reused",ProfileTools_Import(archive,list,mapping,2,&error));
      StringCchPrintfW(path,ARRAYSIZE(path),REG_PROFILES L"\\%s",list->items[1].folder);
      Check("mapped membership uses separate group",Util_RegGetDword(HKEY_CURRENT_USER,path,L"SyncSessions",&error)&&error==2);
      StringCchPrintfW(path,ARRAYSIZE(path),REG_PROFILES L"\\%s",other->folder);
      Check("unmapped membership remains unchanged",Util_RegGetDword(HKEY_CURRENT_USER,path,L"SyncSessions",&error)&&error==1);
      list->count=2;
    }
    { WCHAR complete[LONG_PATH_CCH], lockedPath[LONG_PATH_CCH], key1[MAX_PATH], bad[LONG_PATH_CCH];HANDLE locked;DWORD originalGroup, restoredGroup;
      for(i=0;i<PICTURE_SIZE*PICTURE_SIZE;i++)pixels[i]=0xffaa6633;
      Check("rollback PNG fixture",Icons_SavePicture(list->items[1].folder,pixels,&stamp));
      list->items[1].picture=stamp;StringCchCopyW(list->items[1].name,LABEL_CCH,L"Keep target");StringCchCopyW(list->items[1].note,ARRAYSIZE(list->items[1].note),L"Keep note");
      list->items[1].color=3;StringCchCopyW(list->items[1].badge,BADGE_CCH,L"K");
      Check("rollback original metadata fixture",Profiles_Update(list->items[1].folder,list->items[1].name,3,L"K",stamp)&&Profiles_SetNote(list->items[1].folder,L"Keep note"));
      StringCchPrintfW(key1,ARRAYSIZE(key1),REG_PROFILES L"\\%s",list->items[1].folder);Util_RegGetDword(HKEY_CURRENT_USER,key1,L"SyncSessions",&originalGroup);
      StringCchPrintfW(lockedPath,ARRAYSIZE(lockedPath),L"%s\\pictures\\%s.png",state,list->items[0].folder);
      Check("second target PNG exists",Icons_SavePicture(list->items[0].folder,pixels,&error));
      locked=CreateFileW(lockedPath,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);Check("hold second target PNG against replacement",locked!=INVALID_HANDLE_VALUE);
      StringCchPrintfW(complete,ARRAYSIZE(complete),L"%s\\two-pictures.zip",root);
      Check("later write failure reports complete rollback",!ProfileTools_Import(complete,list,mapping,2,&error)&&error==ERROR_REQUEST_ABORTED);
      if(locked!=INVALID_HANDLE_VALUE)CloseHandle(locked);
      Check("first target name and note rolled back exactly",Util_RegGetString(HKEY_CURRENT_USER,key1,L"Name",text,1024*1024)&&!wcscmp(text,L"Keep target")&&Util_RegGetString(HKEY_CURRENT_USER,key1,L"Note",text,1024*1024)&&!wcscmp(text,L"Keep note"));
      Check("first target group rolled back exactly",Util_RegGetDword(HKEY_CURRENT_USER,key1,L"SyncSessions",&restoredGroup)&&originalGroup==restoredGroup);
      {DWORD *restored=Icons_LoadPicture(&list->items[1]);Check("first target original PNG restored",restored && restored[0]==0xffaa6633);if(restored)HeapFree(GetProcessHeap(),0,restored);}
      StringCchPrintfW(bad,ARRAYSIZE(bad),L"%s\\manager-bad-extra.zip",root);
      Check("unknown manager item CRC is validated",BadExtra(complete,bad)&&!ProfileTools_Import(bad,list,mapping,2,&error));
      Check("bad unknown item leaves target metadata alone",Util_RegGetString(HKEY_CURRENT_USER,key1,L"Name",text,1024*1024)&&!wcscmp(text,L"Keep target"));
    }
    mapping[0] = mapping[1] = 0;
    Check("duplicate mapping rejected before writes", !ProfileTools_Import(archive,list,mapping,2,&error));
    mapping[0]=0;mapping[1]=1;list->items[0].running=TRUE;
    Check("running target refused", !ProfileTools_Import(archive,list,mapping,2,&error) && error==ERROR_BUSY);list->items[0].running=FALSE;
    StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\bad.zip",root);Whole(path,"broken");
    Check("bad package refused", !ProfileTools_Import(path,list,mapping,2,&error));
    StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\window-state.json",list->items[0].storageDir);
    Whole(path,"{\"x\":1,\"y\":2,\"width\":800,\"height\":600,\"isMaximized\":false,\"displayBounds\":{\"x\":0,\"y\":0,\"width\":1920,\"height\":1080,\"secret\":\"keep\"},\"secret\":\"never snapshot\"}");
    Check("safe snapshot", ProfileTools_SaveSnapshot(&list->items[0],snapshot,ARRAYSIZE(snapshot)));
    zip=Zip_Open(snapshot); raw=zip?(char *)Zip_Read(zip,Zip_Find(zip,"window-state.json"),4096,&length):NULL;
    Check("unknown snapshot fields excluded",raw && strstr(raw,"\"x\":1") && !strstr(raw,"secret")); if(raw)HeapFree(GetProcessHeap(),0,raw);
    raw=zip?(char *)Zip_Read(zip,Zip_Find(zip,"source-presence.json"),4096,&length):NULL;
    Check("missing developer file recorded",raw && strstr(raw,"\"developerSettingsPresent\":false"));if(raw)HeapFree(GetProcessHeap(),0,raw);if(zip)Zip_Free(zip);
    { WCHAR bad[LONG_PATH_CCH];StringCchPrintfW(bad,ARRAYSIZE(bad),L"%s\\snapshot-bad-extra.zip",root);
      Check("unknown snapshot item CRC is validated",BadExtra(snapshot,bad)&&!ProfileTools_RestoreSnapshot(&list->items[0],bad,backup,ARRAYSIZE(backup)));
      Check("bad unknown snapshot item does not alter file",Has(path,"never snapshot"));
    }
    Whole(path,"{\"x\":99,\"width\":900,\"secret\":\"keep current\",\"displayBounds\":{\"x\":30,\"secret\":\"nested current\",\"isMaximized\":\"extension\"}}");
    Check("restore selected supported configuration",ProfileTools_RestoreSnapshot(&list->items[0],snapshot,backup,ARRAYSIZE(backup)));
    Check("restore preserves unknown current members",Has(path,"\"x\":1") && Has(path,"keep current") && Has(path,"nested current") && Has(path,"extension"));
    DeleteFileW(path);
    Check("restore missing current configuration safely",ProfileTools_RestoreSnapshot(&list->items[0],snapshot,backup,ARRAYSIZE(backup)) && Has(path,"\"x\":1"));
    list->items[0].running=TRUE;Check("snapshot restore blocks running profile",!ProfileTools_RestoreSnapshot(&list->items[0],snapshot,backup,ARRAYSIZE(backup)));list->items[0].running=FALSE;
    for(i=0;i<2;i++) {
        WCHAR entries[LONG_PATH_CCH];
        StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\config.json",list->items[i].storageDir);Whole(path,"{\"lastKnownAccountUuid\":\"fixture-account\"}");
        StringCchPrintfW(entries,ARRAYSIZE(entries),L"%s\\claude-code-sessions\\fixture-account\\fixture-org",list->items[i].storageDir);Util_EnsureDir(entries);
        StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\local_11111111-1111-4111-8111-111111111111.json",entries);
        Whole(path,"{\"sessionId\":\"local_11111111-1111-4111-8111-111111111111\",\"cliSessionId\":\"11111111-1111-4111-8111-111111111111\",\"cwd\":\"C:\\\\fixture\",\"title\":\"Fixture\",\"lastActivityAt\":123}");
        StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\archived-sessions.idx",entries);Whole(path,"{\"v\":1,\"archived\":[\"local_11111111-1111-4111-8111-111111111111\"]}");
        StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\claude_desktop_config.json",list->items[i].storageDir);
        Whole(path,i?"{\"preferences\":{\"locale\":\"zh-CN\"}}":"{\"preferences\":{\"locale\":\"en-US\"}}");
    }
    Check("health verifies current scope and index",ProfileTools_HealthText(list,NULL,text,1024*1024) && wcsstr(text,L"fixture-account") && wcsstr(text,L"Archive index v1 structure valid: 1; IDs 1; invalid IDs 0"));
    Check("health compares layout values without displaying them",wcsstr(text,L"Current configuration differences") && !wcsstr(text,L"zh-CN"));
    StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\claude-code-sessions\\fixture-account\\fixture-org\\archived-sessions.idx",list->items[0].storageDir);Whole(path,"{\"v\":1,\"archived\":[9]}");
    Check("invalid index ID type surfaced",ProfileTools_HealthText(list,NULL,text,1024*1024) && wcsstr(text,L"Archive index v1 structure valid: 0; IDs 1; invalid IDs 1"));
    OperationHistory_Record(OPERATION_IMPORT,list->items[0].folder,ERROR_SUCCESS,archive);
    StringCchPrintfW(path,ARRAYSIZE(path),L"%s\\operation-history.dat",state);Check("operation result stored privately",Util_FileExists(path));
    HeapFree(GetProcessHeap(),0,pixels);HeapFree(GetProcessHeap(),0,list);HeapFree(GetProcessHeap(),0,text);
    if(override)RegOverridePredefKey(HKEY_CURRENT_USER,NULL);RegCloseKey(fixture);RegDeleteTreeW(HKEY_CURRENT_USER,key);
    Util_SetStateDir(NULL);SetEnvironmentVariableW(L"CLAUDE_CONFIG_DIR",NULL);
    /* root was constructed directly from GetTempPath plus our GUID above. */
    Check("remove only GUID fixture",Util_DeleteTree(root,&error));if(SUCCEEDED(com))CoUninitialize();
    printf("profile tools: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
