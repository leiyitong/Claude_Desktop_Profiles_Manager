/*
 * cookies.c end to end: a sign-in window's WebView2 store read, its cookies
 * written into a profile's store, read back with that profile's own key.
 * Real winsqlite3, real DPAPI, real v10 sealing — no GUI, private temp dirs.
 */
#include "../src/app.h"
#include <stdio.h>
#include <bcrypt.h>

static int g_checks, g_failures;
static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) { g_failures++; printf("  FAIL  %s\n", name); }
}

/* winsqlite3, as cookies.c loads it, to build and read the test stores. */
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef int (WINAPI *sq_open_t)(const char *, sqlite3 **);
typedef int (WINAPI *sq_exec_t)(sqlite3 *, const char *, void *, void *, char **);
typedef int (WINAPI *sq_close_t)(sqlite3 *);
typedef int (WINAPI *sq_prepare_t)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (WINAPI *sq_step_t)(sqlite3_stmt *);
typedef int (WINAPI *sq_finalize_t)(sqlite3_stmt *);
typedef const unsigned char *(WINAPI *sq_text_t)(sqlite3_stmt *, int);
typedef const void *(WINAPI *sq_blob_t)(sqlite3_stmt *, int);
typedef int (WINAPI *sq_bytes_t)(sqlite3_stmt *, int);
static HMODULE g_sq;
static sq_open_t sq_open; static sq_exec_t sq_exec; static sq_close_t sq_close; static sq_prepare_t sq_prepare;
static sq_step_t sq_step; static sq_finalize_t sq_finalize; static sq_text_t sq_text; static sq_blob_t sq_blob; static sq_bytes_t sq_bytes;

#define SQ(name) (void (*)(void))GetProcAddress(g_sq, name)
static BOOL LoadSq(void)
{
    if (!(g_sq = LoadLibraryW(L"winsqlite3.dll"))) return FALSE;
    sq_open = (sq_open_t)SQ("sqlite3_open");
    sq_exec = (sq_exec_t)SQ("sqlite3_exec");
    sq_close = (sq_close_t)SQ("sqlite3_close");
    sq_prepare = (sq_prepare_t)SQ("sqlite3_prepare_v2");
    sq_step = (sq_step_t)SQ("sqlite3_step");
    sq_finalize = (sq_finalize_t)SQ("sqlite3_finalize");
    sq_text = (sq_text_t)SQ("sqlite3_column_text");
    sq_blob = (sq_blob_t)SQ("sqlite3_column_blob");
    sq_bytes = (sq_bytes_t)SQ("sqlite3_column_bytes");
    return sq_open && sq_exec && sq_close && sq_prepare && sq_step && sq_finalize && sq_text && sq_blob && sq_bytes;
}

static const char kSchema[] =
    "CREATE TABLE cookies(creation_utc INTEGER NOT NULL,host_key TEXT NOT NULL,top_frame_site_key TEXT NOT NULL,"
    "name TEXT NOT NULL,value TEXT NOT NULL,encrypted_value BLOB NOT NULL,path TEXT NOT NULL,expires_utc INTEGER NOT NULL,"
    "is_secure INTEGER NOT NULL,is_httponly INTEGER NOT NULL,last_access_utc INTEGER NOT NULL,has_expires INTEGER NOT NULL,"
    "is_persistent INTEGER NOT NULL,priority INTEGER NOT NULL,samesite INTEGER NOT NULL,source_scheme INTEGER NOT NULL,"
    "source_port INTEGER NOT NULL,last_update_utc INTEGER NOT NULL,source_type INTEGER NOT NULL,has_cross_site_ancestor INTEGER NOT NULL);"
    "CREATE UNIQUE INDEX cookies_unique_index ON cookies(host_key,top_frame_site_key,has_cross_site_ancestor,name,path,source_scheme,source_port);";

static BOOL Utf8(const WCHAR *w, char *out, int cap) { return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL) > 0; }

/* An empty Chromium cookie store at `path`. */
static BOOL MakeStore(const WCHAR *path)
{
    char utf8[LONG_PATH_CCH * 3];
    sqlite3 *db = NULL;
    BOOL ok = FALSE;
    if (!Utf8(path, utf8, sizeof utf8) || sq_open(utf8, &db) != 0) { if (db) sq_close(db); return FALSE; }
    ok = sq_exec(db, kSchema, NULL, NULL, NULL) == 0;
    sq_close(db);
    return ok;
}

/* A Local State with a random 32-byte os_crypt key, DPAPI-wrapped (Backup_WriteAppKey). */
static BOOL MakeLocalState(const WCHAR *dir, BYTE key[32])
{
    WCHAR path[LONG_PATH_CCH];
    static const char empty[] = "{\"os_crypt\":{}}";
    return BCRYPT_SUCCESS(BCryptGenRandom(NULL, key, 32, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) &&
           SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\Local State", dir)) &&
           Util_WriteFileAtomic(path, empty, sizeof empty - 1, UTIL_PUT_REPLACE) &&
           Backup_WriteAppKey(dir, key, 32);
}

/* One claude.ai cookie sealed with `key`, inserted into the store at `path`. */
static BOOL SeedCookie(const WCHAR *path, const BYTE key[32], const char *host, const char *name, const WCHAR *value)
{
    char utf8[LONG_PATH_CCH * 3], valueUtf8[512], sql[2048], hex[2048];
    BYTE sealed[1024];
    size_t sealedLength = 0, i;
    sqlite3 *db = NULL;
    BOOL ok = FALSE;
    if (!Utf8(value, valueUtf8, sizeof valueUtf8) ||
        !Core_SealSafeStorage((const BYTE *)valueUtf8, strlen(valueUtf8), key, 32, sealed, sizeof sealed, &sealedLength))
        return FALSE;
    for (i = 0; i < sealedLength; i++) StringCchPrintfA(hex + i * 2, sizeof hex - i * 2, "%02x", sealed[i]);
    StringCchPrintfA(sql, sizeof sql,
        "INSERT INTO cookies VALUES (1,'%s','','%s','',x'%s','/',0,1,1,1,0,0,1,1,2,443,1,0,0)", host, name, hex);
    if (Utf8(path, utf8, sizeof utf8) && sq_open(utf8, &db) == 0) ok = sq_exec(db, sql, NULL, NULL, NULL) == 0;
    if (db) sq_close(db);
    return ok;
}

/* The decrypted value of cookie `name` in the store at `path`, with `key`; "" when absent. */
static void ReadCookie(const WCHAR *path, const BYTE key[32], const char *name, WCHAR *out, int cch)
{
    char utf8[LONG_PATH_CCH * 3], sql[256];
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    out[0] = 0;
    StringCchPrintfA(sql, sizeof sql, "SELECT encrypted_value FROM cookies WHERE name='%s'", name);
    if (!Utf8(path, utf8, sizeof utf8) || sq_open(utf8, &db) != 0) { if (db) sq_close(db); return; }
    if (sq_prepare(db, sql, -1, &st, NULL) == 0 && sq_step(st) == 100) {
        const BYTE *blob = sq_blob(st, 0);
        int len = sq_bytes(st, 0);
        BYTE plain[512];
        size_t plainLength = 0;
        if (blob && len > 0 && Core_OpenSafeStorage(blob, (size_t)len, key, 32, plain, sizeof plain, &plainLength)) {
            int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)plain, (int)plainLength, out, cch - 1);
            out[n > 0 ? n : 0] = 0;
        }
    }
    if (st) sq_finalize(st);
    sq_close(db);
}

int wmain(void)
{
    WCHAR root[MAX_PATH], temp[MAX_PATH], webView[MAX_PATH], ebDefault[MAX_PATH], target[MAX_PATH], targetNet[MAX_PATH];
    WCHAR webCookies[MAX_PATH], targetCookies[MAX_PATH], read[256];
    BYTE webKey[32], targetKey[32];
    Profile p;
    SignInCookie cookies[16];
    int count = 0, wrote = 0;

    if (!LoadSq() || !GetTempPathW(ARRAYSIZE(temp), temp)) { printf("Cookie tests: setup failed.\n"); return 1; }
    StringCchPrintfW(root, ARRAYSIZE(root), L"%scdm-cookies-%lu", temp, GetCurrentProcessId());
    Util_EnsureDir(root);
    Util_SetStateDir(root);   /* the log stays in the fixture */
    StringCchPrintfW(webView, ARRAYSIZE(webView), L"%s\\web", root);
    StringCchPrintfW(ebDefault, ARRAYSIZE(ebDefault), L"%s\\EBWebView\\Default\\Network", webView);
    StringCchPrintfW(target, ARRAYSIZE(target), L"%s\\Claude-CookieFixture", root);
    StringCchPrintfW(targetNet, ARRAYSIZE(targetNet), L"%s\\Network", target);
    StringCchPrintfW(webCookies, ARRAYSIZE(webCookies), L"%s\\Cookies", ebDefault);
    StringCchPrintfW(targetCookies, ARRAYSIZE(targetCookies), L"%s\\Cookies", targetNet);

    Check("make the web view's folders", Util_EnsureDir(ebDefault) && Util_EnsureDir(targetNet));
    Check("the web view has a Local State key", MakeLocalState(webView, webKey));   /* <web>\EBWebView\Local State */
    {
        WCHAR eb[MAX_PATH];
        StringCchPrintfW(eb, ARRAYSIZE(eb), L"%s\\EBWebView", webView);
        Check("the web view's EBWebView Local State", MakeLocalState(eb, webKey));
    }
    Check("the target profile has its own Local State key", MakeLocalState(target, targetKey));
    Check("the keys differ", memcmp(webKey, targetKey, 32) != 0);
    Check("make the cookie stores", MakeStore(webCookies) && MakeStore(targetCookies));

    Check("the web view store seeded with a sealed sessionKey",
          SeedCookie(webCookies, webKey, ".claude.ai", "sessionKey", L"sk-ant-sid01-REAL-SESSION-VALUE") &&
          SeedCookie(webCookies, webKey, ".claude.ai", "lastActiveOrg", L"org-abc-123") &&
          SeedCookie(webCookies, webKey, ".other.com", "unrelated", L"leave-me"));

    ZeroMemory(&p, sizeof p);
    StringCchCopyW(p.folder, ARRAYSIZE(p.folder), L"Claude-CookieFixture");
    StringCchCopyW(p.name, ARRAYSIZE(p.name), L"CookieFixture");
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), target);

    Check("the sign-in is seen as done", Cookies_WebViewSignedIn(webView));
    Check("reading the web view's cookies", Cookies_ReadWebView(webView, cookies, ARRAYSIZE(cookies), &count) && count == 2);
    {
        BOOL hasSession = FALSE;
        int i;
        for (i = 0; i < count; i++)
            if (wcscmp(cookies[i].name, L"sessionKey") == 0) {
                hasSession = TRUE;
                Check("the session cookie's value decrypted to the original", wcscmp(cookies[i].value, L"sk-ant-sid01-REAL-SESSION-VALUE") == 0);
                Check("its host and flags came through", wcscmp(cookies[i].host, L".claude.ai") == 0 && cookies[i].secure && cookies[i].httpOnly);
            }
        Check("the session cookie was among them; the other site's was not", hasSession);
    }

    Check("writing them into the target profile", Cookies_Write(&p, cookies, count, &wrote) && wrote == 2);
    ReadCookie(targetCookies, targetKey, "sessionKey", read, ARRAYSIZE(read));
    Check("the target store holds the session, sealed with the target's key", wcscmp(read, L"sk-ant-sid01-REAL-SESSION-VALUE") == 0);
    ReadCookie(targetCookies, targetKey, "lastActiveOrg", read, ARRAYSIZE(read));
    Check("and the org cookie", wcscmp(read, L"org-abc-123") == 0);
    ReadCookie(targetCookies, webKey, "sessionKey", read, ARRAYSIZE(read));
    Check("the web view's key does not open the target's cookie", read[0] == 0);

    /* Writing again replaces, not duplicates: the value updates in place. */
    StringCchCopyW(cookies[0].value, ARRAYSIZE(cookies[0].value), L"sk-ant-sid01-REFRESHED");
    Check("a second write succeeds", Cookies_Write(&p, cookies, 1, &wrote));
    if (wcscmp(cookies[0].name, L"sessionKey") == 0) {
        ReadCookie(targetCookies, targetKey, "sessionKey", read, ARRAYSIZE(read));
        Check("the value was replaced in place", wcscmp(read, L"sk-ant-sid01-REFRESHED") == 0);
    }

    Util_DeleteTree(root, NULL);
    printf("Cookie tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
