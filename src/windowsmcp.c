/*
 * Windows-MCP (github.com/CursorTouch/Windows-MCP), an MCP server through
 * which Claude works Windows itself: its apps, windows, files and input.
 * Claude Delta adds windows-mcp-delta, which is Windows-MCP with every one of
 * its tools and more of its own (files, the web, WSL, other accounts'
 * sessions). It is one more member of `mcpServers` in a profile's
 * claude_desktop_config.json, named "windows" (its tools are mcp__windows__Click and the like):
 *
 *     "windows": {
 *       "command": "<uvx.exe>",
 *       "args": ["--from", "<folder>", "windows-mcp-delta", "serve", "--transport", "stdio"],
 *       "env": { "WINDOWS_MCP_DELTA_SESSION": "<profile folder>", "WINDOWS_MCP_DELTA_MANAGER": "<this exe>" }
 *     }
 *
 * uv's uvx runs it from the folder its source is extracted to (the ZIP
 * embedded in this exe, ExtractPackage). Claude from the Store does not see
 * PATH (its package's sandbox), so the command is uvx's full path. uv itself,
 * when it is not there, comes from its own installer (astral.sh), in a
 * console the person sees, once they agreed. The env names the profile (its
 * mailbox id) and this exe (so the server can open a session in a profile),
 * and WINDOWS_MCP_DELTA_ONLINE when the person lets Claude read chats on
 * claude.ai (off by default; Settings). The rest of the file is left as it is.
 *
 * The transport is named: without it Windows-MCP takes the one its own
 * ~/.windows-mcp/config.toml holds (`windows-mcp auth` writes HTTP there),
 * and serves HTTP on 127.0.0.1:8000, which Claude does not speak (it talks
 * to its servers over stdio): Claude waits for it ("Starting"), and once
 * Claude quits, that server stays behind holding the port, so that the next
 * one fails (WSAEADDRINUSE). An entry that is the plain windows-mcp, or one
 * without the transport (version 2026.10.23's first builds), is outdated:
 * Repair rewrites it as windows-mcp-delta. So is ours under its earlier name,
 * "windows-mcp" (until 2026.10.25): Repair moves it to "windows". Servers left
 * behind (a windows-mcp.exe whose Claude is gone) can be ended
 * (WindowsMcp_Leftovers).
 *
 * Claude's own Windows-MCP extension (Settings > Extensions, from its
 * directory) is another server with the same tools: it is kept in the
 * profile's folder as an entry of extensions-installations.json, its unpacked
 * folder under "Claude Extensions" and its settings under "Claude Extensions
 * Settings", which Claude reads as it starts: a folder left there shows as
 * installed again. WindowsMcp_RemoveExtension deletes the three, as Claude's
 * own Uninstall does, with that profile's Claude closed and anything still
 * running from the folder ended; the record goes only once the folder is gone,
 * so that one cut short can be finished.
 *
 * The same file's preferences turn Cowork off (secureVmFeaturesEnabled false:
 * Core_CoworkOff), and its virtual machine, some 9 GB in the profile's
 * vm_bundles, can go with it (Cowork_RemoveVm): Claude fetches it again once
 * Cowork is turned on and used.
 */
#include "app.h"
#include "resource.h"
#include <shellapi.h>
#include <tlhelp32.h>
#include <ctype.h>

#define WINDOWS_MCP_KEY  "windows"       /* the server's name in Claude */
#define OLD_KEY          "windows-mcp"   /* its name until 2026.10.25, and plain Windows-MCP's */
#define EXTENSIONS_FILE  L"extensions-installations.json"
#define EXTENSION_FOLDER L"Claude Extensions"
#define EXTENSION_CONFIG L"Claude Extensions Settings"
#define EXTENSION_ID_CCH 128
#define RETRY_TRIES      10    /* a file Claude's processes still hold, just closed: tried this often... */
#define RETRY_MS         300   /* ...this far apart */
#define PACKAGE_TOKEN    "\"windows-mcp-delta\""   /* the delta entry names it; plain windows-mcp does not */
#define CONFIG_MAX       (4u << 20)
#define UV_INSTALL_MS    (10u * 60 * 1000)   /* the installer's time at most */
#define UV_INSTALLER     L"-NoProfile -ExecutionPolicy ByPass -Command \"irm https://astral.sh/uv/install.ps1 | iex\""

static BOOL ConfigPath(const Profile *p, WCHAR *out, size_t cch)
{
    return p->storageDir[0] && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir));
}

/* Whether the bytes `hay` hold `needle` (a quoted JSON token). */
static BOOL Contains(const char *hay, size_t length, const char *needle)
{
    size_t n = strlen(needle), i;
    for (i = 0; n <= length && i + n <= length; i++)
        if (memcmp(hay + i, needle, n) == 0) return TRUE;
    return FALSE;
}

/* `args` (a JSON array) names the stdio transport: "--transport", then "stdio". */
static BOOL NamesStdio(const char *args, size_t length)
{
    static const char flag[] = "\"--transport\"", value[] = "\"stdio\"";
    size_t i, j;
    for (i = 0; i + sizeof flag - 1 <= length; i++) {
        if (memcmp(args + i, flag, sizeof flag - 1) != 0) continue;
        for (j = i + sizeof flag - 1; j < length && (args[j] == ' ' || args[j] == ',' || args[j] == '\t' || args[j] == '\r' || args[j] == '\n'); j++) {}
        if (j + sizeof value - 1 <= length && memcmp(args + j, value, sizeof value - 1) == 0) return TRUE;
    }
    return FALSE;
}

static BOOL PackageRoot(WCHAR *out, size_t cch);
static BOOL PackageDir(WCHAR *out, size_t cch);
static BOOL WriteRetrying(const WCHAR *path, const char *data, size_t length);

/* mcpServers' member `key` of the config `json`. */
static BOOL ServerAt(const char *json, size_t length, const char *key, const char **server, size_t *serverLength)
{
    const char *servers;
    size_t serversLength;
    return Core_JsonMember(json, length, "mcpServers", &servers, &serversLength) &&
           Core_JsonMember(servers, serversLength, key, server, serverLength);
}

/* The server runs by uvx (its full path, as this program writes it): ours, or Windows-MCP set up the same way. */
static BOOL RunByUvx(const char *server, size_t serverLength)
{
    WCHAR command[MAX_PATH];
    size_t n;
    return Core_JsonMemberString(server, serverLength, "command", command, ARRAYSIZE(command)) &&
           (n = wcslen(command)) >= 7 && Core_EqualsI(command + n - 7, L"uvx.exe") && (n == 7 || command[n - 8] == L'\\' || command[n - 8] == L'/');
}

/* The entry's uvx was written with its folder and is not there any more (Python reinstalled elsewhere, uv moved):
 * Claude cannot start the server, so the entry is written again with the uvx found now. A bare "uvx.exe" Claude
 * looks up on PATH itself. */
static BOOL UvxGone(const char *server, size_t serverLength)
{
    WCHAR command[MAX_PATH];
    return Core_JsonMemberString(server, serverLength, "command", command, ARRAYSIZE(command)) &&
           (wcschr(command, L'\\') || wcschr(command, L'/')) && !Util_FileExists(command);
}

/* The folder `args` (a JSON array) runs windows-mcp-delta from: the element after "--from". */
static BOOL FromFolder(const char *args, size_t length, WCHAR *out, size_t cch)
{
    WCHAR *list = Core_JsonStringList(args, length), *at;
    BOOL found = FALSE;
    if (cch) out[0] = 0;
    if (!list) return FALSE;
    for (at = list; *at; at += wcslen(at) + 1)
        if (wcscmp(at, L"--from") == 0 && at[wcslen(at) + 1]) {
            found = SUCCEEDED(StringCchCopyW(out, cch, at + wcslen(at) + 1));
            break;
        }
    HeapFree(GetProcessHeap(), 0, list);
    return found;
}

/* The folder profile `p`'s entry runs windows-mcp-delta from, when it is ours (under either name). */
static BOOL EntryFolder(const Profile *p, WCHAR *out, size_t cch)
{
    static const char *const keys[] = { WINDOWS_MCP_KEY, OLD_KEY };
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    const char *server, *args;
    size_t serverLength, argsLength, i;
    char *json;
    BOOL found = FALSE;
    if (cch) out[0] = 0;
    if (!ConfigPath(p, path, ARRAYSIZE(path)) || (json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) == NULL) return FALSE;
    for (i = 0; i < ARRAYSIZE(keys) && !found; i++)
        if (ServerAt(json, length, keys[i], &server, &serverLength) &&
            Core_JsonMember(server, serverLength, "args", &args, &argsLength) && Contains(args, argsLength, PACKAGE_TOKEN))
            found = FromFolder(args, argsLength, out, cch);
    Util_Free(json);
    return found;
}

int WindowsMcp_State(const Profile *p)
{
    WCHAR path[LONG_PATH_CCH], from[LONG_PATH_CCH], root[LONG_PATH_CCH], current[LONG_PATH_CCH];
    DWORD length = 0;
    const char *server, *args;
    size_t serverLength, argsLength;
    char *json;
    int state = WINDOWS_MCP_NONE;
    if (!ConfigPath(p, path, ARRAYSIZE(path)) || (json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) == NULL) return WINDOWS_MCP_NONE;
    if (ServerAt(json, length, WINDOWS_MCP_KEY, &server, &serverLength)) {
        state = WINDOWS_MCP_READY;
        /* Ours, run by uvx: windows-mcp-delta over stdio from this build's folder is current; the plain
         * windows-mcp, one without the transport, one run from an older build's folder (its source is
         * that build's: an update would never reach it), or one whose uvx is gone is outdated, and Repair (or
         * the manager as it opens) rewrites it. Something else at that key is the person's own. */
        if (RunByUvx(server, serverLength) && Core_JsonMember(server, serverLength, "args", &args, &argsLength)) {
            if (!(Contains(args, argsLength, PACKAGE_TOKEN) && NamesStdio(args, argsLength)))
                state = WINDOWS_MCP_OUTDATED;
            else if (FromFolder(args, argsLength, from, ARRAYSIZE(from)) && PackageRoot(root, ARRAYSIZE(root)) &&
                     PackageDir(current, ARRAYSIZE(current)) && Core_PathUnder(from, root) && !Core_PathEquals(from, current))
                state = WINDOWS_MCP_OUTDATED;
            else if (UvxGone(server, serverLength))
                state = WINDOWS_MCP_OUTDATED;
        }
    }
    /* Ours under its earlier name, alone or beside the new one (two servers then): Repair moves it. The
     * person's own server at that name is theirs, and stays. */
    if (ServerAt(json, length, OLD_KEY, &server, &serverLength) && RunByUvx(server, serverLength)) state = WINDOWS_MCP_OUTDATED;
    Util_Free(json);
    return state;
}

/* Whether the person lets Claude read chats on claude.ai (the env variable the server reads): off by default. */
BOOL WindowsMcp_Online(void)
{
    return Util_GetSetting(SETTING_MCP_ONLINE, 0) != 0;
}

/* windows-mcp-delta's own folder: its source's, under the state folder. */
static BOOL PackageRoot(WCHAR *out, size_t cch)
{
    WCHAR state[LONG_PATH_CCH];
    return Util_StateDir(state, ARRAYSIZE(state)) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\windows-mcp-delta", state));
}

/* The source embedded in this exe (a ZIP); FALSE when it is missing. */
static BOOL EmbeddedSource(const void **data, DWORD *size)
{
    HRSRC found = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_WINDOWS_MCP_DELTA), MAKEINTRESOURCEW(10) /* RT_RCDATA */);
    HGLOBAL loaded = found ? LoadResource(NULL, found) : NULL;
    *data = loaded ? LockResource(loaded) : NULL;
    *size = found ? SizeofResource(NULL, found) : 0;
    return *data && *size;
}

/* This build's source folder, named for the version and the source's checksum: several builds of one day each
 * get their own (the first one unpacked would otherwise stay), and uv, which keeps what it built from a folder by
 * its path, builds each one anew. */
static BOOL PackageDir(WCHAR *out, size_t cch)
{
    WCHAR root[LONG_PATH_CCH];
    const void *data;
    DWORD size;
    if (!PackageRoot(root, ARRAYSIZE(root)) || !EmbeddedSource(&data, &size)) return FALSE;
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" APP_VERSION_DATE_W L"-%08lx", root, (unsigned long)Core_Crc32(0, data, size)));
}

BOOL WindowsMcp_PackageDir(WCHAR *out, size_t cch)
{
    return PackageDir(out, cch);
}

/* `name` (a ZIP item, UTF-8, '/') as a safe relative path (no "..", no drive); FALSE when it is not one. */
static BOOL SafePath(const char *name, WCHAR *out, size_t cch)
{
    size_t i;
    if (!Core_ArchiveNameSafe(name, strlen(name)) || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, out, (int)cch) <= 0)
        return FALSE;
    for (i = 0; out[i]; i++)
        if (out[i] == L'/') out[i] = L'\\';
    return out[0] && out[wcslen(out) - 1] != L'\\';   /* a file, not a bare folder entry */
}

/* The older versions' folders removed, so they do not pile up; one a profile's entry still runs from stays
 * (uvx reads it at each start of that profile's Claude) until Repair points that entry at this build's. */
static void RemoveOldPackages(const WCHAR *keep)   /* `keep`: this build's folder */
{
    WCHAR root[LONG_PATH_CCH], pattern[LONG_PATH_CCH], path[LONG_PATH_CCH], used[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE search;
    ProfileList *list;
    DWORD error = 0;
    int i;
    BOOL inUse;
    if (!PackageRoot(root, ARRAYSIZE(root)) || FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*", root))) return;
    if ((list = (ProfileList *)HeapAlloc(GetProcessHeap(), 0, sizeof *list)) == NULL) return;
    Profiles_Load(list, NULL);
    search = FindFirstFileExW(pattern, FindExInfoBasic, &found, FindExSearchLimitToDirectories, NULL, 0);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.' ||
                FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", root, found.cFileName)) || Core_PathEquals(path, keep))
                continue;
            for (i = 0, inUse = FALSE; i < list->count && !inUse; i++)
                inUse = EntryFolder(&list->items[i], used, ARRAYSIZE(used)) && Core_PathEquals(used, path);
            if (!inUse) Util_DeleteTree(path, &error);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    HeapFree(GetProcessHeap(), 0, list);
}

/* windows-mcp-delta's source extracted into its version's folder (once: a `.ready` marker says it is there), the
 * folder uvx runs it from written to `out`. The source is the ZIP embedded in this exe. */
static BOOL ExtractPackage(WCHAR *out, size_t cch)
{
    WCHAR dir[LONG_PATH_CCH], marker[LONG_PATH_CCH], zipPath[LONG_PATH_CCH], path[LONG_PATH_CCH], relative[LONG_PATH_CCH], parent[LONG_PATH_CCH], *slash;
    const void *data;
    DWORD size;
    ZipIn *zip;
    DWORD error = 0;
    int i;
    BOOL ok = TRUE;
    if (!EmbeddedSource(&data, &size)) {
        Util_Log(L"windows-mcp: the embedded windows-mcp-delta source is missing");
        return FALSE;
    }
    if (!PackageDir(dir, ARRAYSIZE(dir)) || FAILED(StringCchPrintfW(marker, ARRAYSIZE(marker), L"%s\\.ready", dir))) return FALSE;
    if (Util_FileExists(marker)) return SUCCEEDED(StringCchCopyW(out, cch, dir));
    Util_DeleteTree(dir, &error);   /* a partial earlier try, if any */
    if (!Util_EnsureDir(dir) || FAILED(StringCchPrintfW(zipPath, ARRAYSIZE(zipPath), L"%s\\.source.zip", dir)) ||
        !Util_WriteFileAtomic(zipPath, (const char *)data, size, UTIL_PUT_REPLACE) || (zip = Zip_Open(zipPath)) == NULL) {
        Util_Log(L"windows-mcp: windows-mcp-delta's source could not be unpacked (error %lu)", GetLastError());
        return FALSE;
    }
    for (i = 0; i < Zip_Count(zip); i++) {
        if (!SafePath(Zip_Name(zip, i), relative, ARRAYSIZE(relative))) continue;   /* a folder entry, or an unsafe name */
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, relative)) ||
            FAILED(StringCchCopyW(parent, ARRAYSIZE(parent), path))) { ok = FALSE; continue; }
        if ((slash = wcsrchr(parent, L'\\')) != NULL) *slash = 0;
        if (!Util_EnsureDir(parent) || !Zip_Extract(zip, i, path)) { ok = FALSE; Util_Log(L"windows-mcp: could not extract %s (error %lu)", relative, GetLastError()); }
    }
    Zip_Free(zip);
    DeleteFileW(zipPath);
    if (ok && Util_WriteFileAtomic(marker, APP_VERSION_DATE, sizeof APP_VERSION_DATE - 1, UTIL_PUT_REPLACE)) {
        RemoveOldPackages(dir);
        return SUCCEEDED(StringCchCopyW(out, cch, dir));
    }
    return FALSE;
}

/* `relative` below the folder in environment variable `base`, when uvx.exe is there. */
static BOOL UvxBelow(const WCHAR *base, const WCHAR *relative, WCHAR *out, size_t cch)
{
    WCHAR root[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(base, root, ARRAYSIZE(root));
    return n && n < ARRAYSIZE(root) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", root, relative)) && Util_FileExists(out);
}

/* uv's installer puts it in ~\.local\bin, an older one in ~\.cargo\bin, winget links it in WinGet\Links; else PATH. */
BOOL WindowsMcp_FindUvx(WCHAR *out, size_t cch)
{
    WCHAR found[MAX_PATH];
    if (UvxBelow(L"USERPROFILE", L".local\\bin\\uvx.exe", out, cch) || UvxBelow(L"USERPROFILE", L".cargo\\bin\\uvx.exe", out, cch) ||
        UvxBelow(L"LOCALAPPDATA", L"Microsoft\\WinGet\\Links\\uvx.exe", out, cch))
        return TRUE;
    return SearchPathW(NULL, L"uvx.exe", NULL, ARRAYSIZE(found), found, NULL) && SUCCEEDED(StringCchCopyW(out, cch, found));
}

BOOL WindowsMcp_InstallUv(HWND owner)
{
    SHELLEXECUTEINFOW run;
    ULONGLONG until;
    DWORD code = 1;
    ZeroMemory(&run, sizeof run);
    run.cbSize = sizeof run;
    run.fMask = SEE_MASK_NOCLOSEPROCESS;
    run.hwnd = owner;
    run.lpFile = L"powershell.exe";
    run.lpParameters = UV_INSTALLER;
    run.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&run) || !run.hProcess) {
        Util_Log(L"windows-mcp: uv's installer could not start (error %lu)", GetLastError());
        return FALSE;
    }
    /* Its console shows how it goes; the window answers meanwhile. */
    until = GetTickCount64() + UV_INSTALL_MS;
    while (GetTickCount64() < until &&
           MsgWaitForMultipleObjects(1, &run.hProcess, FALSE, (DWORD)min(until - GetTickCount64(), 200), QS_ALLINPUT) != WAIT_OBJECT_0) {
        MSG message;
        while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    GetExitCodeProcess(run.hProcess, &code);
    CloseHandle(run.hProcess);
    Util_Log(L"windows-mcp: uv's installer ended with %lu", code);
    return code == 0;
}

/* uvx resolves windows-mcp-delta from its folder now, out of sight, so that Claude's first start does not wait. */
static void Prefetch(const WCHAR *uvx, const WCHAR *pkg)
{
    WCHAR command[2 * LONG_PATH_CCH];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (FAILED(StringCchPrintfW(command, ARRAYSIZE(command), L"\"%s\" --from \"%s\" windows-mcp-delta --help", uvx, pkg))) return;
    if (CreateProcessW(uvx, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

/* The server entry's JSON: uvx running windows-mcp-delta from `pkg` over stdio, env naming the profile, this exe,
 * and online when it is on. Built into `raw` (large enough for the quoted paths); FALSE when a part does not fit. */
static BOOL BuildServer(const WCHAR *uvx, const WCHAR *pkg, const WCHAR *folder, const WCHAR *exe, BOOL online,
                        char *raw, size_t cap)
{
    char qUvx[MAX_PATH * 3 + 2], qPkg[LONG_PATH_CCH * 3 + 2], qFolder[FOLDER_CCH * 3 + 2], qExe[LONG_PATH_CCH * 3 + 2];
    if (!Core_JsonQuote(uvx, qUvx, sizeof qUvx) || !Core_JsonQuote(pkg, qPkg, sizeof qPkg) ||
        !Core_JsonQuote(folder, qFolder, sizeof qFolder) || !Core_JsonQuote(exe, qExe, sizeof qExe))
        return FALSE;
    return SUCCEEDED(StringCchPrintfA(raw, cap,
        "{\"command\":%s,\"args\":[\"--from\",%s,\"windows-mcp-delta\",\"serve\",\"--transport\",\"stdio\"],"
        "\"env\":{\"WINDOWS_MCP_DELTA_SESSION\":%s,\"WINDOWS_MCP_DELTA_MANAGER\":%s%s}}",
        qUvx, qPkg, qFolder, qExe, online ? ",\"WINDOWS_MCP_DELTA_ONLINE\":\"1\"" : ""));
}

/* Ours under its earlier name (run by uvx) taken out of the config: the one Set built (`*built`, replaced) or,
 * when it built none, the file as read (`json`), into a new `*out`. Nothing changes when there is none. */
static void DropOldEntry(char **built, size_t *builtLength, const char *json, size_t length, char **out, size_t *outLength)
{
    static const char *const keys[] = { "mcpServers" };
    const char *from = built ? *built : json, *servers, *server;
    size_t fromLength = built ? *builtLength : length, serversLength, serverLength, keptLength = 0;
    char *kept, *result;
    if (!ServerAt(from, fromLength, OLD_KEY, &server, &serverLength) || !RunByUvx(server, serverLength) ||
        !Core_JsonMember(from, fromLength, "mcpServers", &servers, &serversLength) ||
        (kept = (char *)HeapAlloc(GetProcessHeap(), 0, serversLength + 1)) == NULL)
        return;
    if (Core_JsonRemoveMember(servers, serversLength, OLD_KEY, kept, serversLength + 1, &keptLength)) {
        kept[keptLength] = 0;
        if ((result = Core_JsonSetNested(from, fromLength, keys, 1, kept, outLength)) != NULL) {
            if (built && *built) HeapFree(GetProcessHeap(), 0, *built);
            *out = result;
        }
    }
    HeapFree(GetProcessHeap(), 0, kept);
}

BOOL WindowsMcp_Set(const Profile *p, const WCHAR *uvx)
{
    static const char *const keys[] = { "mcpServers", WINDOWS_MCP_KEY };
    WCHAR path[LONG_PATH_CCH], pkg[LONG_PATH_CCH], exe[LONG_PATH_CCH];
    DWORD length = 0;
    char raw[8192], *json, *out = NULL, *servers = NULL;
    const char *value;
    size_t outLength = 0, valueLength;
    BOOL ok, freeJson = TRUE, extracted = FALSE;
    if (!ConfigPath(p, path, ARRAYSIZE(path))) return FALSE;
    if (uvx && !(extracted = ExtractPackage(pkg, ARRAYSIZE(pkg)))) {
        Util_Log(L"windows-mcp: windows-mcp-delta could not be set up for %s", p->folder);
        return FALSE;
    }
    if ((json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) == NULL) {
        if (!uvx) return TRUE;   /* none: nothing to take away */
        json = (char *)"{}";
        length = 2;
        freeJson = FALSE;
    }
    if (uvx) {
        ok = Util_SelfExe(exe, ARRAYSIZE(exe)) &&
             BuildServer(uvx, pkg, p->folder, exe, WindowsMcp_Online(), raw, sizeof raw) &&
             (out = Core_JsonSetNested(json, length, keys, 2, raw, &outLength)) != NULL;
    } else if (Core_JsonMember(json, length, "mcpServers", &value, &valueLength)) {
        /* Taken out of mcpServers, the others kept. */
        size_t serversLength = 0, cap = valueLength + 1;
        servers = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        ok = servers && Core_JsonRemoveMember(value, valueLength, WINDOWS_MCP_KEY, servers, cap, &serversLength);
        if (ok) {
            servers[serversLength] = 0;
            ok = (out = Core_JsonSetNested(json, length, keys, 1, servers, &outLength)) != NULL;
        }
    } else {
        ok = TRUE;   /* no servers: none to take away */
    }
    if (ok) DropOldEntry(out ? &out : NULL, out ? &outLength : NULL, json, length, &out, &outLength);
    ok = ok && (!out || WriteRetrying(path, out, outLength));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    if (servers) HeapFree(GetProcessHeap(), 0, servers);
    if (freeJson) Util_Free(json);
    if (!ok) Util_Log(L"windows-mcp: %s of %s not written (error %lu)", uvx ? L"adding" : L"removal", p->folder, GetLastError());
    else {
        if (uvx && extracted) Prefetch(uvx, pkg);
        if (PackageDir(pkg, ARRAYSIZE(pkg))) RemoveOldPackages(pkg);   /* an older build's folder goes once no entry runs from it */
    }
    return ok;
}

/* Every entry of ours run from another build's folder (or under the earlier name, or by a uvx that is gone) pointed
 * at this build's and the uvx found now, as the manager opens: an update, or uv in a new place, reaches each
 * profile's Windows MCP at its Claude's next start, nothing asked. The person's own servers, and the plain
 * windows-mcp (Repair's), are left alone. */
void WindowsMcp_UpdateEntries(void)
{
    ProfileList *list;
    WCHAR uvx[MAX_PATH], from[LONG_PATH_CCH];
    int i;
    if (!WindowsMcp_FindUvx(uvx, ARRAYSIZE(uvx))) return;   /* no uvx: nothing we wrote runs */
    if ((list = (ProfileList *)HeapAlloc(GetProcessHeap(), 0, sizeof *list)) == NULL) return;
    Profiles_Load(list, NULL);
    for (i = 0; i < list->count; i++)
        if (WindowsMcp_State(&list->items[i]) == WINDOWS_MCP_OUTDATED && EntryFolder(&list->items[i], from, ARRAYSIZE(from)) &&
            WindowsMcp_Set(&list->items[i], uvx))
            Util_Log(L"windows-mcp: %s now runs this build's windows-mcp-delta", list->items[i].folder);
    HeapFree(GetProcessHeap(), 0, list);
}

/* The profiles that have windows-mcp-delta written again, so a change to the online setting reaches their env
 * (Claude reads it at its next start); one run from an older build's folder is pointed at this build's too.
 * Those without it, the plain windows-mcp (Repair's), and the person's own servers are left alone. */
void WindowsMcp_RefreshOnline(void)
{
    ProfileList list;
    ClaudePackage pkg;
    WCHAR uvx[MAX_PATH];
    int i;
    if (!WindowsMcp_FindUvx(uvx, ARRAYSIZE(uvx))) return;   /* no uvx: nothing we wrote runs */
    Claude_FindPackage(&pkg);
    Profiles_Load(&list, &pkg);
    for (i = 0; i < list.count; i++) {
        WCHAR from[LONG_PATH_CCH];
        int state = WindowsMcp_State(&list.items[i]);
        if (state == WINDOWS_MCP_READY || (state == WINDOWS_MCP_OUTDATED && EntryFolder(&list.items[i], from, ARRAYSIZE(from))))
            WindowsMcp_Set(&list.items[i], uvx);
    }
}

/* ------------------------------------------------------------- left behind */

#define LEFTOVER_SCAN_MAX 4096

typedef struct ProcessRow {
    DWORD pid, parent;
    WCHAR exe[MAX_PATH];
} ProcessRow;

/* When process `pid` started; FALSE when it cannot be asked (gone, or not ours to ask). */
static BOOL StartedAt(DWORD pid, ULONGLONG *at)
{
    FILETIME created, exited, kernel, user;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    BOOL ok;
    if (!process) return FALSE;
    ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    *at = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
    return ok;
}

static int RowOf(const ProcessRow *rows, int count, DWORD pid)
{
    int i;
    for (i = 0; i < count; i++)
        if (rows[i].pid == pid) return i;
    return -1;
}

/* A windows-mcp.exe whose Claude is gone: up from it through uvx (or uv, or
 * the cmd.exe a shell launch puts between them), the process that started it
 * no longer runs (its id gone, or taken by a later process). One that a
 * Claude still running started, or a terminal, is not. */
static BOOL LeftBehind(const ProcessRow *rows, int count, int at)
{
    ULONGLONG childStart, parentStart;
    int depth, parent;
    if (!StartedAt(rows[at].pid, &childStart)) return FALSE;
    for (depth = 0; depth < 6; depth++) {
        if ((parent = RowOf(rows, count, rows[at].parent)) < 0) return TRUE;
        if (!StartedAt(rows[parent].pid, &parentStart)) return FALSE;   /* not ours to ask: not ours to end */
        if (parentStart > childStart) return TRUE;                       /* its id taken by a later process */
        if (!Core_EqualsI(rows[parent].exe, L"uvx.exe") && !Core_EqualsI(rows[parent].exe, L"uv.exe") && !Core_EqualsI(rows[parent].exe, L"cmd.exe"))
            return FALSE;
        at = parent;
        childStart = parentStart;
    }
    return FALSE;
}

/* Processes now running, a heap array (Util_Free); NULL when they cannot be listed. */
static ProcessRow *ListProcesses(int *count)
{
    PROCESSENTRY32W entry;
    ProcessRow *rows;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    *count = 0;
    if (snapshot == INVALID_HANDLE_VALUE) return NULL;
    if ((rows = (ProcessRow *)Util_Alloc(LEFTOVER_SCAN_MAX * sizeof *rows)) != NULL) {
        entry.dwSize = sizeof entry;
        if (Process32FirstW(snapshot, &entry))
            do {
                rows[*count].pid = entry.th32ProcessID;
                rows[*count].parent = entry.th32ParentProcessID;
                StringCchCopyW(rows[*count].exe, MAX_PATH, entry.szExeFile);
                (*count)++;
            } while (*count < LEFTOVER_SCAN_MAX && Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return rows;
}

int WindowsMcp_Leftovers(void)
{
    int count = 0, i, found = 0;
    ProcessRow *rows = ListProcesses(&count);
    for (i = 0; rows && i < count; i++)
        if (Core_EqualsI(rows[i].exe, L"windows-mcp.exe") && LeftBehind(rows, count, i)) found++;
    Util_Free(rows);
    return found;
}

static BOOL EndProcess(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    BOOL ended = process && TerminateProcess(process, 1);
    if (process) CloseHandle(process);
    return ended;
}

int WindowsMcp_EndLeftovers(void)
{
    int count = 0, i, j, ended = 0;
    ProcessRow *rows = ListProcesses(&count);
    for (i = 0; rows && i < count; i++) {
        ULONGLONG start, childStart;
        if (!Core_EqualsI(rows[i].exe, L"windows-mcp.exe") || !LeftBehind(rows, count, i) || !StartedAt(rows[i].pid, &start)) continue;
        /* Its Python first (the launcher's child, started after it), then the launcher. */
        for (j = 0; j < count; j++)
            if (rows[j].parent == rows[i].pid && StartedAt(rows[j].pid, &childStart) && childStart >= start) EndProcess(rows[j].pid);
        if (EndProcess(rows[i].pid)) ended++;
        Util_Log(L"windows-mcp: ended a server left behind (process %lu)", rows[i].pid);
    }
    Util_Free(rows);
    return ended;
}

/* ------------------------------------------------- Claude's Windows-MCP extension */

/* `path` written, tried again a few times while another process still holds it (ERROR_SHARING_VIOLATION: Claude's
 * helpers, a moment after Claude closed). */
static BOOL WriteRetrying(const WCHAR *path, const char *data, size_t length)
{
    int tries;
    for (tries = 1; ; tries++) {
        DWORD error;
        if (Util_WriteFileAtomic(path, data, length, UTIL_PUT_REPLACE)) return TRUE;
        error = GetLastError();
        if (tries >= RETRY_TRIES || (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED)) {
            SetLastError(error);
            return FALSE;
        }
        Sleep(RETRY_MS);
    }
}

typedef struct ExtensionSearch { char id[EXTENSION_ID_CCH]; } ExtensionSearch;

/* An id ending "windows-mcp" (Claude's directory names it ant.dir.cursortouch.windows-mcp), made of letters, digits,
 * dots, dashes and underscores only: it names a folder. */
static BOOL WindowsMcpId(const char *id, size_t length)
{
    static const char tail[] = "windows-mcp";
    size_t i;
    if (length < sizeof tail - 1 || length >= EXTENSION_ID_CCH || _strnicmp(id + length - (sizeof tail - 1), tail, sizeof tail - 1) != 0)
        return FALSE;
    for (i = 0; i < length; i++)
        if (!(isalnum((unsigned char)id[i]) || id[i] == '.' || id[i] == '-' || id[i] == '_')) return FALSE;
    return TRUE;
}

static BOOL FindWindowsMcpExtension(void *context, const char *key, size_t keyLen, const char *value, size_t valueLen)
{
    ExtensionSearch *search = (ExtensionSearch *)context;
    (void)value;
    (void)valueLen;
    if (!search->id[0] && WindowsMcpId(key, keyLen)) {
        memcpy(search->id, key, keyLen);
        search->id[keyLen] = 0;
    }
    return TRUE;
}

/* The extension in Claude's record (`json`), its id into `search`. */
static BOOL RecordedId(const char *json, size_t length, ExtensionSearch *search)
{
    const char *extensions;
    size_t extensionsLength;
    search->id[0] = 0;
    return Core_JsonMember(json, length, "extensions", &extensions, &extensionsLength) &&
           Core_JsonEachMember(extensions, extensionsLength, FindWindowsMcpExtension, search) && search->id[0];
}

/* Profile `p`'s Windows-MCP extension: in Claude's record (`*recorded`), else a folder of it left under Claude
 * Extensions (an uninstall cut short), which Claude would show as installed again. */
static BOOL FindExtension(const Profile *p, ExtensionSearch *search, BOOL *recorded)
{
    WCHAR path[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    DWORD length = 0;
    char *json, name[EXTENSION_ID_CCH];
    int n;
    *recorded = FALSE;
    search->id[0] = 0;
    if (!p->storageDir[0]) return FALSE;
    if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" EXTENSIONS_FILE, p->storageDir)) &&
        (json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) != NULL) {
        *recorded = RecordedId(json, length, search);
        Util_Free(json);
        if (*recorded) return TRUE;
    }
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" EXTENSION_FOLDER, p->storageDir)) ||
        (find = Util_FindFiles(path, L"*", &found, FALSE)) == INVALID_HANDLE_VALUE)
        return FALSE;
    do {
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (n = WideCharToMultiByte(CP_UTF8, 0, found.cFileName, -1, name, sizeof name, NULL, NULL)) <= 1 || !WindowsMcpId(name, (size_t)n - 1))
            continue;
        StringCchCopyA(search->id, sizeof search->id, name);
        break;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return search->id[0] != 0;
}

BOOL WindowsMcp_HasExtension(const Profile *p)
{
    ExtensionSearch search;
    BOOL recorded;
    return FindExtension(p, &search, &recorded);
}

/* Every process whose program is in `dir` ended: the extension's server, its Python from its own .venv. */
static void EndProcessesUnder(const WCHAR *dir)
{
    int count = 0, i;
    ProcessRow *rows = ListProcesses(&count);
    for (i = 0; rows && i < count; i++) {
        WCHAR image[LONG_PATH_CCH];
        DWORD size = ARRAYSIZE(image);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, rows[i].pid);
        BOOL under = process && QueryFullProcessImageNameW(process, 0, image, &size) && Core_PathUnder(image, dir);
        if (process) CloseHandle(process);
        if (under && EndProcess(rows[i].pid)) Util_Log(L"windows-mcp: ended %s, run from the extension's folder", rows[i].exe);
    }
    Util_Free(rows);
}

/* `path` deleted, tried again a few times while a file in it is still held. */
static BOOL DeleteRetrying(const WCHAR *path)
{
    DWORD error = 0;
    int tries;
    for (tries = 1; ; tries++) {
        if (Util_DeleteTree(path, &error)) return TRUE;
        if (tries >= RETRY_TRIES) break;
        Sleep(RETRY_MS);
    }
    Util_Log(L"windows-mcp: could not delete %s (error %lu)", path, error);
    return FALSE;
}

BOOL WindowsMcp_RemoveExtension(HWND owner, const Profile *p)
{
    static const char *const keys[] = { "extensions" };
    WCHAR path[LONG_PATH_CCH], id[EXTENSION_ID_CCH], folder[LONG_PATH_CCH], settings[LONG_PATH_CCH];
    DWORD length = 0;
    const char *extensions;
    size_t extensionsLength, keptLength = 0, outLength = 0;
    char *json, *kept = NULL, *out = NULL;
    ExtensionSearch search;
    BOOL recorded, ok;
    (void)owner;
    if (!FindExtension(p, &search, &recorded)) return TRUE;   /* none: nothing to take away */
    if (MultiByteToWideChar(CP_UTF8, 0, search.id, -1, id, ARRAYSIZE(id)) <= 0 ||
        FAILED(StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\" EXTENSION_FOLDER L"\\%s", p->storageDir, id)) ||
        FAILED(StringCchPrintfW(settings, ARRAYSIZE(settings), L"%s\\" EXTENSION_CONFIG L"\\%s.json", p->storageDir, id)) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" EXTENSIONS_FILE, p->storageDir)))
        return FALSE;
    /* Its files first, as Claude's own Uninstall deletes them: the record goes only once they are gone, so that an
     * uninstall cut short leaves it installed, to be finished, never half there. */
    EndProcessesUnder(folder);
    ok = DeleteRetrying(folder);
    if (ok && !DeleteFileW(settings) && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
        Util_Log(L"windows-mcp: could not delete %s (error %lu)", settings, GetLastError());
    if (ok && recorded) {
        ok = FALSE;
        if ((json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) != NULL) {
            if (Core_JsonMember(json, length, "extensions", &extensions, &extensionsLength) &&
                (kept = (char *)HeapAlloc(GetProcessHeap(), 0, extensionsLength + 1)) != NULL &&
                Core_JsonRemoveMember(extensions, extensionsLength, search.id, kept, extensionsLength + 1, &keptLength)) {
                kept[keptLength] = 0;
                ok = (out = Core_JsonSetNested(json, length, keys, 1, kept, &outLength)) != NULL && WriteRetrying(path, out, outLength);
            }
            if (out) HeapFree(GetProcessHeap(), 0, out);
            if (kept) HeapFree(GetProcessHeap(), 0, kept);
            Util_Free(json);
        }
    }
    if (ok) Util_Log(L"windows-mcp: the Windows-MCP extension (%s) uninstalled from %s", id, p->folder);
    else Util_Log(L"windows-mcp: the Windows-MCP extension of %s could not be uninstalled (error %lu)", p->folder, GetLastError());
    return ok;
}


/* ---------------------------------------------------------------- SearXNG */
/* A second server the manager sets up: mcp-searxng, run by Node.js's npx, searching the person's SearXNG instance.
 * The entry names npx.cmd by its full path, as Windows MCP's names uvx: Claude from the Store may not see the PATH
 * a console sees. Claude runs a .cmd through cmd.exe itself. */

static BOOL NpxIn(const WCHAR *variable, const WCHAR *below, WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(variable, base, ARRAYSIZE(base));
    return n && n < ARRAYSIZE(base) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s%s", base, below)) && Util_FileExists(out);
}

BOOL SearXng_FindNpx(WCHAR *out, size_t cch)
{
    WCHAR *file = NULL;
    DWORD n = SearchPathW(NULL, L"npx.cmd", NULL, (DWORD)cch, out, &file);
    if (n && n < cch) return TRUE;
    /* Where Node.js's installer, nvm-windows and Scoop put it, should the PATH not name it. */
    return NpxIn(L"ProgramFiles", L"\\nodejs\\npx.cmd", out, cch) || NpxIn(L"ProgramW6432", L"\\nodejs\\npx.cmd", out, cch) ||
           NpxIn(L"NVM_SYMLINK", L"\\npx.cmd", out, cch) || NpxIn(L"USERPROFILE", L"\\scoop\\apps\\nodejs\\current\\npx.cmd", out, cch) ||
           NpxIn(L"USERPROFILE", L"\\scoop\\apps\\nodejs-lts\\current\\npx.cmd", out, cch);
}

BOOL SearXng_Get(const Profile *p, WCHAR *url, size_t cch, BOOL *allowPrivate)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    const char *server;
    size_t serverLength;
    char *json;
    BOOL has;
    if (url && cch) url[0] = 0;
    if (allowPrivate) *allowPrivate = FALSE;
    if (!ConfigPath(p, path, ARRAYSIZE(path)) || (json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) == NULL) return FALSE;
    has = ServerAt(json, length, SEARXNG_KEY, &server, &serverLength) && Core_SearXngServer(server, serverLength, url, cch, allowPrivate);
    Util_Free(json);
    return has;
}

/* The package fetched into npx's cache, in the background, so that Claude's first start of the server does not wait
 * for the download: npx runs `node -e 0` with mcp-searxng installed, which ends at once. */
static void PrefetchSearXng(const WCHAR *npx)
{
    WCHAR cmd[MAX_PATH], command[MAX_PATH * 2 + 96];
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    UINT n = GetSystemDirectoryW(cmd, ARRAYSIZE(cmd));
    if (!n || n >= ARRAYSIZE(cmd) - 9 || FAILED(StringCchCatW(cmd, ARRAYSIZE(cmd), L"\\cmd.exe")) ||
        FAILED(StringCchPrintfW(command, ARRAYSIZE(command), L"cmd.exe /d /s /c \"\"%s\" --yes --package " L"" SEARXNG_PACKAGE L" node -e 0\"", npx)))
        return;
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    if (CreateProcessW(cmd, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

BOOL SearXng_Set(const Profile *p, const WCHAR *npx, const WCHAR *url, BOOL allowPrivate)
{
    static const char *const keys[] = { "mcpServers", SEARXNG_KEY };
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    char raw[MAX_PATH * 3 + URL_CCH * 3 + 256], *json, *out = NULL, *servers = NULL;
    const char *value, *server;
    size_t outLength = 0, valueLength, serverLength;
    BOOL ok, freeJson = TRUE;
    if (!ConfigPath(p, path, ARRAYSIZE(path))) return FALSE;
    if ((json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) == NULL) {
        if (!npx) return TRUE;   /* none: nothing to take away */
        json = (char *)"{}";
        length = 2;
        freeJson = FALSE;
    }
    if (npx) {
        ok = Core_SearXngEntry(npx, url, allowPrivate, raw, sizeof raw) && (out = Core_JsonSetNested(json, length, keys, 2, raw, &outLength)) != NULL;
    } else if (ServerAt(json, length, SEARXNG_KEY, &server, &serverLength) && Core_SearXngServer(server, serverLength, NULL, 0, NULL) &&
               Core_JsonMember(json, length, "mcpServers", &value, &valueLength)) {
        /* Ours taken out of mcpServers, the others kept; a "searxng" of the person's own (not mcp-searxng) stays. */
        size_t serversLength = 0, cap = valueLength + 1;
        servers = (char *)HeapAlloc(GetProcessHeap(), 0, cap);
        ok = servers && Core_JsonRemoveMember(value, valueLength, SEARXNG_KEY, servers, cap, &serversLength);
        if (ok) {
            servers[serversLength] = 0;
            ok = (out = Core_JsonSetNested(json, length, keys, 1, servers, &outLength)) != NULL;
        }
    } else {
        ok = TRUE;   /* none of ours: nothing to take away */
    }
    ok = ok && (!out || WriteRetrying(path, out, outLength));
    if (out) HeapFree(GetProcessHeap(), 0, out);
    if (servers) HeapFree(GetProcessHeap(), 0, servers);
    if (freeJson) Util_Free(json);
    if (!ok) Util_Log(L"searxng: %s of %s not written (error %lu)", npx ? L"adding" : L"removal", p->folder, GetLastError());
    else if (npx) {
        Util_Log(L"searxng: set up in %s", p->folder);
        PrefetchSearXng(npx);
    } else {
        Util_Log(L"searxng: taken away from %s", p->folder);
    }
    return ok;
}

/* ---------------------------------------------------------------- Cowork */

/* The bytes of the files under `dir`, at any depth (a link's target left out). */
static ULONGLONG TreeBytes(const WCHAR *dir)
{
    WCHAR child[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(dir, L"*", &found, FALSE);
    ULONGLONG bytes = 0;
    if (find == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) bytes += ((ULONGLONG)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        else if (!(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                 SUCCEEDED(StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\%s", dir, found.cFileName)))
            bytes += TreeBytes(child);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return bytes;
}

static BOOL VmDir(const Profile *p, WCHAR *out, size_t cch)
{
    return p->storageDir[0] && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" COWORK_VM_DIR, p->storageDir));
}

BOOL Cowork_Get(const Profile *p, ULONGLONG *vmBytes)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    char *json;
    BOOL on = TRUE;
    if (vmBytes) *vmBytes = VmDir(p, path, ARRAYSIZE(path)) ? TreeBytes(path) : 0;
    if (ConfigPath(p, path, ARRAYSIZE(path)) && (json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) != NULL) {
        on = !Core_CoworkOff(json, length);
        Util_Free(json);
    }
    return on;
}

/* Preference `key` of `p` set to `raw`, or taken out when `raw` is NULL. A profile without settings yet gets them
 * only for a value other than `absent`, which is what Claude takes without them (NULL: any value is written). */
static BOOL SetPreference(const Profile *p, const char *key, const char *raw, const char *absent)
{
    const char *keys[2] = { "preferences", key };
    WCHAR path[LONG_PATH_CCH];
    WIN32_FILE_ATTRIBUTE_DATA file;
    DWORD length = 0;
    char *json, *out = NULL;
    size_t outLength = 0;
    BOOL ok, freeJson = TRUE;
    if (!ConfigPath(p, path, ARRAYSIZE(path))) return FALSE;
    if ((json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) == NULL) {
        /* A config there that cannot be read is never written over. */
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &file) && (file.nFileSizeHigh || file.nFileSizeLow)) {
            Util_Log(L"preferences: the settings of %s cannot be read (error %lu)", p->folder, GetLastError());
            return FALSE;
        }
        if (!raw || (absent && strcmp(raw, absent) == 0)) return TRUE;   /* none: Claude's default already */
        json = (char *)"{}";
        length = 2;
        freeJson = FALSE;
    }
    ok = (out = Core_JsonSetPath(json, length, keys, 2, raw, &outLength)) != NULL && WriteRetrying(path, out, outLength);
    if (out) HeapFree(GetProcessHeap(), 0, out);
    if (freeJson) Util_Free(json);
    if (!ok) Util_Log(L"preferences: %S of %s not written (error %lu)", key, p->folder, GetLastError());
    else Util_Log(L"preferences: %S of %s %S", key, p->folder, raw ? raw : "taken out");
    return ok;
}

BOOL Cowork_Set(const Profile *p, BOOL on)
{
    return SetPreference(p, COWORK_PREFERENCE, on ? "true" : "false", "true");
}

BOOL Prefs_Get(const Profile *p, const char *key, char *raw, size_t cap)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    const char *preferences, *value;
    size_t preferencesLength, valueLength;
    char *json;
    BOOL found = FALSE;
    if (cap) raw[0] = 0;
    if (!ConfigPath(p, path, ARRAYSIZE(path)) || (json = Util_ReadFile(path, CONFIG_MAX, FALSE, &length)) == NULL) return FALSE;
    if (Core_JsonMember(json, length, "preferences", &preferences, &preferencesLength) &&
        Core_JsonMember(preferences, preferencesLength, key, &value, &valueLength) && valueLength < cap) {
        memcpy(raw, value, valueLength);
        raw[valueLength] = 0;
        found = TRUE;
    }
    Util_Free(json);
    return found;
}

BOOL Prefs_Set(const Profile *p, const char *key, const char *raw)
{
    return SetPreference(p, key, raw, NULL);
}

/* The virtual machine lets its disks go a moment after its Claude closed: a file still held is tried again a few times. */
BOOL Cowork_RemoveVm(const Profile *p, ULONGLONG *freed)
{
    WCHAR dir[LONG_PATH_CCH];
    ULONGLONG before;
    DWORD error = 0;
    int tries;
    *freed = 0;
    if (!VmDir(p, dir, ARRAYSIZE(dir))) return FALSE;
    before = TreeBytes(dir);
    for (tries = 1; !Util_DeleteTree(dir, &error); tries++) {
        if (tries >= RETRY_TRIES || (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION && error != ERROR_ACCESS_DENIED)) {
            *freed = before - min(before, TreeBytes(dir));
            Util_Log(L"cowork: the virtual machine of %s not deleted (error %lu)", p->folder, error);
            SetLastError(error);
            return FALSE;
        }
        Sleep(RETRY_MS);
    }
    *freed = before;
    if (before) Util_Log(L"cowork: the virtual machine of %s deleted (%llu bytes)", p->folder, before);
    return TRUE;
}
