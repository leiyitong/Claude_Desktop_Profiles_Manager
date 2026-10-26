/*
 * Tests for src/windowsmcp.c: windows-mcp-delta written into a throwaway
 * profile's claude_desktop_config.json, its source extracted from the
 * embedded ZIP, the online setting reaching the env, and removal. Private
 * state only (Util_SetStateDir): the user's profiles and Claude are never
 * touched. Built and run by build.cmd; exits non-zero when a check fails.
 */
#include "../src/app.h"
#include "../src/resource.h"
#include <stdio.h>

static int g_failures, g_checks;

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
}

/* The whole config.json read as UTF-8 bytes (NUL-terminated); "" when it is not there. */
static char *ReadConfig(const Profile *p)
{
    static char buffer[1 << 16];
    WCHAR path[LONG_PATH_CCH];
    DWORD length = 0;
    char *json;
    buffer[0] = 0;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir))) return buffer;
    if ((json = Util_ReadFile(path, 1 << 20, FALSE, &length)) != NULL) {
        if (length >= sizeof buffer) length = sizeof buffer - 1;
        memcpy(buffer, json, length);
        buffer[length] = 0;
        Util_Free(json);
    }
    return buffer;
}

static BOOL Holds(const Profile *p, const char *needle)
{
    return strstr(ReadConfig(p), needle) != NULL;
}

static WCHAR g_uvx[MAX_PATH];   /* a stand-in uvx.exe that exists: an entry whose uvx is gone reads as outdated */

/* `json` written as `p`'s config, the fixtures' "C:\fake\uv\uvx.exe" made the stand-in's path. */
static void WriteConfig(const Profile *p, const char *json)
{
    static const char fake[] = "\"C:\\\\fake\\\\uv\\\\uvx.exe\"";
    WCHAR path[LONG_PATH_CCH];
    char quoted[MAX_PATH * 3 + 2], out[LONG_PATH_CCH * 3 + 1024];
    const char *at, *from = json;
    out[0] = 0;
    Core_JsonQuote(g_uvx, quoted, sizeof quoted);
    while ((at = strstr(from, fake)) != NULL) {
        StringCchCatNA(out, sizeof out, from, (size_t)(at - from));
        StringCchCatA(out, sizeof out, quoted);
        from = at + sizeof fake - 1;
    }
    StringCchCatA(out, sizeof out, from);
    StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir);
    Util_WriteFileAtomic(path, out, strlen(out), UTIL_PUT_REPLACE);
}

int wmain(void)
{
    WCHAR root[MAX_PATH], state[MAX_PATH], profileDir[MAX_PATH], extracted[LONG_PATH_CCH], temp[MAX_PATH];
    Profile p;
    const WCHAR *uvx = g_uvx;

    if (!GetTempPathW(ARRAYSIZE(temp), temp) ||
        FAILED(StringCchPrintfW(root, ARRAYSIZE(root), L"%scdm-mcp-%lu", temp, GetCurrentProcessId()))) {
        printf("setup failed\n");
        return 1;
    }
    StringCchPrintfW(state, ARRAYSIZE(state), L"%s\\state", root);
    StringCchPrintfW(profileDir, ARRAYSIZE(profileDir), L"%s\\Claude-McpFixture", root);
    Util_EnsureDir(state);
    Util_EnsureDir(profileDir);
    {
        WCHAR uvDir[MAX_PATH];
        StringCchPrintfW(uvDir, ARRAYSIZE(uvDir), L"%s\\uv", root);
        StringCchPrintfW(g_uvx, ARRAYSIZE(g_uvx), L"%s\\uvx.exe", uvDir);
        Util_EnsureDir(uvDir);
        Util_WriteFileAtomic(g_uvx, "MZ", 2, UTIL_PUT_REPLACE);
    }
    Util_SetStateDir(state);
    Util_SetSetting(SETTING_MCP_ONLINE, 0);

    ZeroMemory(&p, sizeof p);
    StringCchCopyW(p.folder, ARRAYSIZE(p.folder), L"Claude-McpFixture");
    StringCchCopyW(p.name, ARRAYSIZE(p.name), L"McpFixture");
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), profileDir);

    Check("nothing is there to start", WindowsMcp_State(&p) == WINDOWS_MCP_NONE);
    Check("online is off by default", !WindowsMcp_Online());

    Check("adding windows-mcp-delta succeeds", WindowsMcp_Set(&p, uvx));
    Check("the entry is named windows", Holds(&p, "\"windows\":{"));
    Check("the entry runs it from a folder with uvx", Holds(&p, "\"--from\"") && Holds(&p, "uvx.exe"));
    Check("it names windows-mcp-delta over stdio", Holds(&p, "\"windows-mcp-delta\"") && Holds(&p, "\"--transport\"") && Holds(&p, "\"stdio\""));
    Check("the env names the profile and the manager", Holds(&p, "\"WINDOWS_MCP_DELTA_SESSION\"") &&
          Holds(&p, "Claude-McpFixture") && Holds(&p, "\"WINDOWS_MCP_DELTA_MANAGER\""));
    Check("online is not in the env while it is off", !Holds(&p, "WINDOWS_MCP_DELTA_ONLINE"));
    Check("it reads back as ready", WindowsMcp_State(&p) == WINDOWS_MCP_READY);

    {
        /* This build's folder: the version and the source's checksum, so that every build gets its own (one per day
         * kept the first build's code), with the modules the server needs. */
        WCHAR package[LONG_PATH_CCH];
        const WCHAR *modules[] = { L"pyproject.toml", L"src\\windows_mcp_delta\\online.py", L"src\\windows_mcp_delta\\esc.py",
                                   L"src\\windows_mcp_delta\\tools\\desktop.py", L"src\\windows_mcp_delta\\tools\\idle.py" };
        size_t m;
        Check("this build's folder names the version and a checksum", WindowsMcp_PackageDir(package, ARRAYSIZE(package)) &&
              wcsstr(package, L"\\windows-mcp-delta\\" APP_VERSION_DATE_W L"-") != NULL && wcslen(wcsrchr(package, L'-') + 1) == 8);
        for (m = 0; m < ARRAYSIZE(modules); m++) {
            StringCchPrintfW(extracted, ARRAYSIZE(extracted), L"%s\\%s", package, modules[m]);
            Check("the source was extracted from the embedded ZIP, every module", Util_FileExists(extracted));
        }
        Check("the entry runs it from this build's folder", WindowsMcp_State(&p) == WINDOWS_MCP_READY);
    }

    Check("turning online on saves it", Util_SetSetting(SETTING_MCP_ONLINE, 1) && WindowsMcp_Online());
    Check("writing again now puts online in the env", WindowsMcp_Set(&p, uvx) && Holds(&p, "\"WINDOWS_MCP_DELTA_ONLINE\":\"1\""));
    Check("it is still ready", WindowsMcp_State(&p) == WINDOWS_MCP_READY);

    /* An older plain windows-mcp entry reads as outdated (Repair rewrites it). */
    {
        static const char plain[] =
            "{\"mcpServers\":{\"windows-mcp\":{\"command\":\"C:\\\\fake\\\\uv\\\\uvx.exe\",\"args\":[\"windows-mcp\",\"serve\",\"--transport\",\"stdio\"]}}}";
        WriteConfig(&p, plain);
        Check("a plain windows-mcp entry is outdated", WindowsMcp_State(&p) == WINDOWS_MCP_OUTDATED);
    }

    /* A delta entry run from an older build's folder is outdated too: an update would never reach its source.
     * Set points it at this build's, and the older folder, which no entry runs from any more, goes. */
    {
        WCHAR older[LONG_PATH_CCH], path[LONG_PATH_CCH];
        char quoted[LONG_PATH_CCH * 3 + 2], json[LONG_PATH_CCH * 3 + 512];
        StringCchPrintfW(older, ARRAYSIZE(older), L"%s\\windows-mcp-delta\\2020.01.01", state);
        Util_EnsureDir(older);
        Core_JsonQuote(older, quoted, sizeof quoted);
        StringCchPrintfA(json, sizeof json,
                         "{\"mcpServers\":{\"windows-mcp\":{\"command\":\"C:\\\\fake\\\\uv\\\\uvx.exe\",\"args\":[\"--from\",%s,"
                         "\"windows-mcp-delta\",\"serve\",\"--transport\",\"stdio\"]}}}", quoted);
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p.storageDir);
        WriteConfig(&p, json);
        Check("a delta entry run from an older build's folder is outdated", WindowsMcp_State(&p) == WINDOWS_MCP_OUTDATED);
        /* A folder of the same day without the checksum: what one build per day left. Outdated too. */
        {
            WCHAR sameDay[LONG_PATH_CCH];
            char quotedDay[LONG_PATH_CCH * 3 + 2], dayJson[LONG_PATH_CCH * 3 + 512];
            StringCchPrintfW(sameDay, ARRAYSIZE(sameDay), L"%s\\windows-mcp-delta\\" APP_VERSION_DATE_W, state);
            Core_JsonQuote(sameDay, quotedDay, sizeof quotedDay);
            StringCchPrintfA(dayJson, sizeof dayJson,
                             "{\"mcpServers\":{\"windows\":{\"command\":\"C:\\\\fake\\\\uv\\\\uvx.exe\",\"args\":[\"--from\",%s,"
                             "\"windows-mcp-delta\",\"serve\",\"--transport\",\"stdio\"]}}}", quotedDay);
            WriteConfig(&p, dayJson);
            Check("a folder of the same day without the checksum is outdated", WindowsMcp_State(&p) == WINDOWS_MCP_OUTDATED);
            WriteConfig(&p, json);
        }
        Check("Set points it at this build's folder", WindowsMcp_Set(&p, uvx) && WindowsMcp_State(&p) == WINDOWS_MCP_READY &&
              Holds(&p, "\\\\windows-mcp-delta\\\\" APP_VERSION_DATE));
        Check("the older build's folder goes once no entry runs from it", !Util_DirExists(older));
    }

    /* Ours under its earlier name, "windows-mcp", is outdated; Set moves it to "windows". */
    {
        static const char earlier[] =
            "{\"mcpServers\":{\"windows-mcp\":{\"command\":\"C:\\\\fake\\\\uv\\\\uvx.exe\",\"args\":[\"--from\",\"C:\\\\x\","
            "\"windows-mcp-delta\",\"serve\",\"--transport\",\"stdio\"]},\"other\":{\"command\":\"node\"}}}";
        WriteConfig(&p, earlier);
        Check("ours under its earlier name is outdated", WindowsMcp_State(&p) == WINDOWS_MCP_OUTDATED);
        Check("Set moves it to the new name", WindowsMcp_Set(&p, uvx) && WindowsMcp_State(&p) == WINDOWS_MCP_READY &&
              Holds(&p, "\"windows\":{") && !Holds(&p, "\"windows-mcp\":"));
        Check("the person's other server is kept", Holds(&p, "\"other\":{\"command\":\"node\"}"));
    }

    /* uvx moved (Python reinstalled elsewhere): the entry cannot start, reads as outdated, and Set with the uvx
     * found now makes it ready again. A bare "uvx.exe" (Claude finds it on PATH) is not checked. */
    {
        WCHAR gone[MAX_PATH], kept[MAX_PATH];
        Check("a current entry with its uvx in place is ready", WindowsMcp_Set(&p, uvx) && WindowsMcp_State(&p) == WINDOWS_MCP_READY);
        StringCchCopyW(kept, ARRAYSIZE(kept), g_uvx);
        StringCchPrintfW(gone, ARRAYSIZE(gone), L"%s\\gone\\uvx.exe", root);
        Check("written with a uvx that is not there", WindowsMcp_Set(&p, gone));
        Check("an entry whose uvx is gone is outdated", WindowsMcp_State(&p) == WINDOWS_MCP_OUTDATED);
        Check("Set with the uvx found now makes it ready", WindowsMcp_Set(&p, kept) && WindowsMcp_State(&p) == WINDOWS_MCP_READY);
        Check("a bare uvx.exe is left to PATH", WindowsMcp_Set(&p, L"uvx.exe") && WindowsMcp_State(&p) == WINDOWS_MCP_READY);
        WindowsMcp_Set(&p, uvx);
    }

    /* The person's own server under that earlier name is theirs: not ours, and kept. */
    {
        static const char theirs[] = "{\"mcpServers\":{\"windows-mcp\":{\"command\":\"node\",\"args\":[\"server.js\"]}}}";
        WCHAR path[LONG_PATH_CCH];
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p.storageDir);
        Util_WriteFileAtomic(path, theirs, sizeof theirs - 1, UTIL_PUT_REPLACE);
        Check("the person's own windows-mcp server is not ours", WindowsMcp_State(&p) == WINDOWS_MCP_NONE);
        Check("setting ours up keeps theirs", WindowsMcp_Set(&p, uvx) && Holds(&p, "\"windows-mcp\":{\"command\":\"node\"") &&
              WindowsMcp_State(&p) == WINDOWS_MCP_READY);
    }

    /* Claude's own Windows-MCP extension: found, and uninstalled from its record, folder and settings. */
    {
        static const char installed[] =
            "{\"extensions\":{\"ant.dir.other.tool\":{\"id\":\"ant.dir.other.tool\"},"
            "\"ant.dir.cursortouch.windows-mcp\":{\"id\":\"ant.dir.cursortouch.windows-mcp\",\"version\":\"0.7.2\"}}}";
        WCHAR path[LONG_PATH_CCH], folder[LONG_PATH_CCH], settings[LONG_PATH_CCH], file[LONG_PATH_CCH];
        char *json;
        DWORD length = 0;
        StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\extensions-installations.json", p.storageDir);
        StringCchPrintfW(folder, ARRAYSIZE(folder), L"%s\\Claude Extensions\\ant.dir.cursortouch.windows-mcp", p.storageDir);
        StringCchPrintfW(settings, ARRAYSIZE(settings), L"%s\\Claude Extensions Settings", p.storageDir);
        Check("no extension is there to start", !WindowsMcp_HasExtension(&p) && WindowsMcp_RemoveExtension(NULL, &p));
        Util_EnsureDir(folder);
        Util_EnsureDir(settings);
        StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\manifest.json", folder);
        Util_WriteFileAtomic(file, "{}", 2, UTIL_PUT_REPLACE);
        StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\ant.dir.cursortouch.windows-mcp.json", settings);
        Util_WriteFileAtomic(file, "{\"isEnabled\":true}", 18, UTIL_PUT_REPLACE);
        Util_WriteFileAtomic(path, installed, sizeof installed - 1, UTIL_PUT_REPLACE);
        Check("the Windows-MCP extension is found", WindowsMcp_HasExtension(&p));
        Check("uninstalling it succeeds", WindowsMcp_RemoveExtension(NULL, &p) && !WindowsMcp_HasExtension(&p));
        json = Util_ReadFile(path, 1 << 20, FALSE, &length);
        Check("its record goes, the other extension's stays", json && strstr(json, "ant.dir.other.tool") && !strstr(json, "windows-mcp"));
        if (json) Util_Free(json);
        Check("its folder and settings go", !Util_DirExists(folder) && !Util_FileExists(file));

        /* A folder left by an uninstall cut short shows as installed again in Claude: found, and deleted. */
        StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\.venv\\Lib", folder);
        Util_EnsureDir(file);
        StringCchPrintfW(file, ARRAYSIZE(file), L"%s\\.venv\\Lib\\site.py", folder);
        Util_WriteFileAtomic(file, "x", 1, UTIL_PUT_REPLACE);
        Check("a folder left without its record counts as installed", WindowsMcp_HasExtension(&p));
        Check("uninstalling it again deletes the folder", WindowsMcp_RemoveExtension(NULL, &p) && !Util_DirExists(folder) &&
              !WindowsMcp_HasExtension(&p));
    }

    Check("removing it succeeds", WindowsMcp_Set(&p, NULL));
    Check("it is gone", WindowsMcp_State(&p) == WINDOWS_MCP_NONE);
    Check("only ours went", !Holds(&p, "\"windows\":") && Holds(&p, "\"windows-mcp\":{\"command\":\"node\""));

    /* SearXNG search: set up beside the person's own server, read back, its address changed, taken away; a
     * "searxng" of the person's own (not mcp-searxng) is neither read as ours nor taken away. */
    {
        WCHAR npx[MAX_PATH], url[URL_CCH];
        BOOL priv = FALSE;
        StringCchPrintfW(npx, ARRAYSIZE(npx), L"%s\\nodejs\\npx.cmd", root);   /* none there: the prefetch fails, harmless */
        WriteConfig(&p, "{\"mcpServers\":{\"mine\":{\"command\":\"node\",\"args\":[\"server.js\"]}},\"other\":1}");
        Check("searxng: none to start", !SearXng_Get(&p, url, ARRAYSIZE(url), &priv) && url[0] == 0);
        Check("searxng: set up", SearXng_Set(&p, npx, L"https://searx.example.com", TRUE));
        Check("searxng: read back", SearXng_Get(&p, url, ARRAYSIZE(url), &priv) && wcscmp(url, L"https://searx.example.com") == 0 && priv);
        Check("searxng: the person's server and settings kept", Holds(&p, "\"mine\"") && Holds(&p, "\"other\":1") && Holds(&p, "\"mcp-searxng\""));
        Check("searxng: its address changed", SearXng_Set(&p, npx, L"http://10.0.0.5:8080", FALSE) &&
              SearXng_Get(&p, url, ARRAYSIZE(url), &priv) && wcscmp(url, L"http://10.0.0.5:8080") == 0 && !priv &&
              !Holds(&p, "MCP_HTTP_ALLOW_PRIVATE_URLS"));
        Check("searxng: taken away, the rest kept", SearXng_Set(&p, NULL, NULL, FALSE) && !SearXng_Get(&p, url, ARRAYSIZE(url), NULL) &&
              !Holds(&p, "\"searxng\"") && Holds(&p, "\"mine\"") && Holds(&p, "\"other\":1"));
        WriteConfig(&p, "{\"mcpServers\":{\"searxng\":{\"command\":\"python\",\"args\":[\"my_searx.py\"]}}}");
        Check("searxng: the person's own searxng is not ours", !SearXng_Get(&p, url, ARRAYSIZE(url), NULL));
        Check("searxng: and taking ours away leaves it", SearXng_Set(&p, NULL, NULL, FALSE) && Holds(&p, "my_searx.py"));
    }

    /* Cowork: turned off in the preferences, the rest of the file kept; its virtual machine deleted and measured;
     * turned on again. */
    {
        WCHAR dir[MAX_PATH], disk[MAX_PATH];
        ULONGLONG bytes = 0, freed = 0;
        HANDLE file;
        DWORD written = 0;
        static const char kDisk[] = "a virtual machine's disk, small for the test";
        WriteConfig(&p, "{\"mcpServers\":{\"mine\":{\"command\":\"node\"}},\"preferences\":{\"menuBarEnabled\":true}}");
        StringCchPrintfW(dir, ARRAYSIZE(dir), L"%s\\" COWORK_VM_DIR L"\\claudevm.bundle", p.storageDir);
        StringCchPrintfW(disk, ARRAYSIZE(disk), L"%s\\rootfs.vhdx", dir);
        file = Util_EnsureDir(dir) ? CreateFileW(disk, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL) : INVALID_HANDLE_VALUE;
        if (file != INVALID_HANDLE_VALUE) {
            WriteFile(file, kDisk, sizeof kDisk - 1, &written, NULL);
            CloseHandle(file);
        }
        Check("cowork: on to start, its virtual machine measured", Cowork_Get(&p, &bytes) && bytes == sizeof kDisk - 1);
        Check("cowork: turned off", Cowork_Set(&p, FALSE) && !Cowork_Get(&p, NULL) && Holds(&p, "\"secureVmFeaturesEnabled\":false"));
        Check("cowork: the rest of the file kept", Holds(&p, "\"mine\"") && Holds(&p, "\"menuBarEnabled\":true"));
        Check("cowork: its virtual machine deleted", Cowork_RemoveVm(&p, &freed) && freed == sizeof kDisk - 1 &&
              !Cowork_Get(&p, &bytes) && bytes == 0 && !Util_DirExists(dir));
        Check("cowork: none left to delete is no failure", Cowork_RemoveVm(&p, &freed) && freed == 0);
        Check("cowork: turned on again", Cowork_Set(&p, TRUE) && Cowork_Get(&p, NULL) && Holds(&p, "\"secureVmFeaturesEnabled\":true"));
        {
            char raw[32];
            Check("preferences: one the profile sets is read", Prefs_Get(&p, "menuBarEnabled", raw, sizeof raw) && strcmp(raw, "true") == 0);
            Check("preferences: one it does not set is none", !Prefs_Get(&p, "ccKeepAwakeOnBattery", raw, sizeof raw) && !raw[0]);
            Check("preferences: written beside the rest", Prefs_Set(&p, "ccKeepAwakeOnBattery", "false") &&
                  Prefs_Get(&p, "ccKeepAwakeOnBattery", raw, sizeof raw) && strcmp(raw, "false") == 0 && Holds(&p, "\"mine\"") &&
                  Holds(&p, "\"menuBarEnabled\":true"));
            Check("preferences: taken out, Claude's default again", Prefs_Set(&p, "ccKeepAwakeOnBattery", NULL) &&
                  !Prefs_Get(&p, "ccKeepAwakeOnBattery", raw, sizeof raw) && Holds(&p, "\"menuBarEnabled\":true"));
        }
    }

    Util_DeleteTree(root, NULL);
    printf("Windows MCP tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
