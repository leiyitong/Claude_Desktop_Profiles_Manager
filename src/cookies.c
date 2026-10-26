/*
 * Claude Desktop keeps a person signed in with the cookies claude.ai sets,
 * in a Chromium cookie store (the profile's data folder, Network\Cookies: a
 * SQLite database whose values are sealed "v10" with the key in Local State,
 * DPAPI-protected to this user). A private sign-in (webview.c) signs in to
 * claude.ai in a window of its own and reads those cookies in plain; this
 * file writes them into a profile's store so that profile is signed in too.
 *
 * SQLite is Windows' own winsqlite3.dll (system, Windows 10+): no third-party
 * code. Each value is sealed with the target profile's own Local State key
 * (Backup_ReadAppKey, then Core_SealSafeStorage), not the sign-in window's,
 * so the store stays consistent with that profile's key. A cookie of the same
 * host, name and path is replaced. The profile's Claude must be closed: it
 * keeps the store open and writes it back as it quits.
 */
#include "app.h"
#include <bcrypt.h>

#define SQLITE_OK    0
#define SQLITE_ROW   100
#define SQLITE_DONE  101
#define SQLITE_TRANSIENT ((void (*)(void *))-1)
#define SQLITE_UTF8  1

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef INT64 sqlite_int64;

typedef int (WINAPI *sqlite3_open_v2_fn)(const char *, sqlite3 **, int, const char *);
typedef int (WINAPI *sqlite3_close_fn)(sqlite3 *);
typedef int (WINAPI *sqlite3_exec_fn)(sqlite3 *, const char *, void *, void *, char **);
typedef int (WINAPI *sqlite3_prepare_v2_fn)(sqlite3 *, const char *, int, sqlite3_stmt **, const char **);
typedef int (WINAPI *sqlite3_step_fn)(sqlite3_stmt *);
typedef int (WINAPI *sqlite3_reset_fn)(sqlite3_stmt *);
typedef int (WINAPI *sqlite3_finalize_fn)(sqlite3_stmt *);
typedef int (WINAPI *sqlite3_bind_text_fn)(sqlite3_stmt *, int, const char *, int, void (*)(void *));
typedef int (WINAPI *sqlite3_bind_blob_fn)(sqlite3_stmt *, int, const void *, int, void (*)(void *));
typedef int (WINAPI *sqlite3_bind_int64_fn)(sqlite3_stmt *, int, sqlite_int64);
typedef int (WINAPI *sqlite3_bind_int_fn)(sqlite3_stmt *, int, int);

#define SQLITE_OPEN_READONLY  0x00000001
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_URI       0x00000040

static struct {
    HMODULE               dll;
    sqlite3_open_v2_fn    open_v2;
    sqlite3_close_fn      close;
    sqlite3_exec_fn       exec;
    sqlite3_prepare_v2_fn prepare_v2;
    sqlite3_step_fn       step;
    sqlite3_reset_fn      reset;
    sqlite3_finalize_fn   finalize;
    sqlite3_bind_text_fn  bind_text;
    sqlite3_bind_blob_fn  bind_blob;
    sqlite3_bind_int64_fn bind_int64;
    sqlite3_bind_int_fn   bind_int;
} sq;

static BOOL LoadSqlite(void)
{
    if (sq.dll) return TRUE;
    if ((sq.dll = LoadLibraryW(L"winsqlite3.dll")) == NULL) {
        Util_Log(L"cookies: winsqlite3.dll is not available (error %lu)", GetLastError());
        return FALSE;
    }
    sq.open_v2    = (sqlite3_open_v2_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_open_v2");
    sq.close      = (sqlite3_close_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_close");
    sq.exec       = (sqlite3_exec_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_exec");
    sq.prepare_v2 = (sqlite3_prepare_v2_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_prepare_v2");
    sq.step       = (sqlite3_step_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_step");
    sq.reset      = (sqlite3_reset_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_reset");
    sq.finalize   = (sqlite3_finalize_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_finalize");
    sq.bind_text  = (sqlite3_bind_text_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_bind_text");
    sq.bind_blob  = (sqlite3_bind_blob_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_bind_blob");
    sq.bind_int64 = (sqlite3_bind_int64_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_bind_int64");
    sq.bind_int   = (sqlite3_bind_int_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_bind_int");
    if (!sq.open_v2 || !sq.close || !sq.exec || !sq.prepare_v2 || !sq.step || !sq.reset || !sq.finalize ||
        !sq.bind_text || !sq.bind_blob || !sq.bind_int64 || !sq.bind_int) {
        Util_Log(L"cookies: winsqlite3.dll is missing an entry point");
        return FALSE;
    }
    return TRUE;
}

BOOL Cookies_SignInHost(const WCHAR *host)
{
    /* The cookies that keep claude.ai signed in are on .claude.ai; its
     * subdomains too (the login flow uses a few). Not another site's. */
    return host && (Core_EqualsI(host, L"claude.ai") || Core_EqualsI(host, L".claude.ai") ||
                    (wcslen(host) > 10 && Core_EqualsI(host + wcslen(host) - 10, L".claude.ai")));
}

/* A WCHAR string as UTF-8 into `out`; its byte length (not the NUL). */
static int Utf8Of(const WCHAR *text, char *out, int cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, out, cap, NULL, NULL);
    return n > 0 ? n - 1 : 0;
}

/* Chromium stores times as microseconds since 1601-01-01 (the Windows epoch /
 * 10). A FILETIME is 100-ns since 1601, so the value is FILETIME / 10. */
#define COOKIE_INSERT \
    "INSERT OR REPLACE INTO cookies (creation_utc,host_key,top_frame_site_key,name,value,encrypted_value,path," \
    "expires_utc,is_secure,is_httponly,last_access_utc,has_expires,is_persistent,priority,samesite,source_scheme," \
    "source_port,last_update_utc,source_type,has_cross_site_ancestor) VALUES (?,?,'',?,'',?,?,?,?,?,?,?,?,1,?,2,?,?,0,0)"

BOOL Cookies_Write(const Profile *target, const SignInCookie *cookies, int count, int *wrote)
{
    WCHAR path[LONG_PATH_CCH];
    char utf8Path[LONG_PATH_CCH * 3];
    BYTE key[64];
    size_t keyLength = 0;
    sqlite3 *db = NULL;
    sqlite3_stmt *insert = NULL;
    ULONGLONG nowUtc;
    FILETIME ft;
    int i, put = 0;
    BOOL ok = FALSE;
    if (wrote) *wrote = 0;
    if (!target || !cookies || count <= 0) return FALSE;
    if (!LoadSqlite()) return FALSE;
    if (!Backup_ReadAppKey(target->storageDir, key, sizeof key, &keyLength) || keyLength != 32) {
        Util_Log(L"cookies: %s has no Local State key: its Claude has not started yet", target->folder);
        return FALSE;
    }
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\Network\\Cookies", target->storageDir)) ||
        !Util_FileExists(path) || Utf8Of(path, utf8Path, sizeof utf8Path) == 0) {
        SecureZeroMemory(key, sizeof key);
        Util_Log(L"cookies: %s has no cookie store yet (Network\\Cookies): open its Claude once first", target->folder);
        return FALSE;
    }
    GetSystemTimeAsFileTime(&ft);
    nowUtc = (((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10;
    if (sq.open_v2(utf8Path, &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
        Util_Log(L"cookies: could not open %s", path);
        SecureZeroMemory(key, sizeof key);
        return FALSE;
    }
    sq.exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; BEGIN IMMEDIATE", NULL, NULL, NULL);
    if (sq.prepare_v2(db, COOKIE_INSERT, -1, &insert, NULL) != SQLITE_OK) {
        Util_Log(L"cookies: the store's cookies table is not as expected");
        sq.exec(db, "ROLLBACK", NULL, NULL, NULL);
        sq.close(db);
        SecureZeroMemory(key, sizeof key);
        return FALSE;
    }
    ok = TRUE;
    for (i = 0; i < count && ok; i++) {
        char name[COOKIE_NAME_CCH * 3], host[COOKIE_HOST_CCH * 3], cookiePath[COOKIE_PATH_CCH * 3], value[COOKIE_VALUE_CCH * 3];
        BYTE sealed[COOKIE_VALUE_CCH * 3 + 64];
        size_t sealedLength = 0;
        int valueBytes, bind = 1;
        ULONGLONG expires = cookies[i].expires;
        if (!Cookies_SignInHost(cookies[i].host)) continue;
        valueBytes = Utf8Of(cookies[i].value, value, sizeof value);
        if (!Core_SealSafeStorage((const BYTE *)value, (size_t)valueBytes, key, 32, sealed, sizeof sealed, &sealedLength)) {
            ok = FALSE;
            break;
        }
        Utf8Of(cookies[i].name, name, sizeof name);
        Utf8Of(cookies[i].host, host, sizeof host);
        Utf8Of(cookies[i].path[0] ? cookies[i].path : L"/", cookiePath, sizeof cookiePath);
        sq.reset(insert);
        sq.bind_int64(insert, bind++, (sqlite_int64)nowUtc);            /* creation_utc */
        sq.bind_text(insert, bind++, host, -1, SQLITE_TRANSIENT);       /* host_key */
        sq.bind_text(insert, bind++, name, -1, SQLITE_TRANSIENT);       /* name */
        sq.bind_blob(insert, bind++, sealed, (int)sealedLength, SQLITE_TRANSIENT);   /* encrypted_value */
        sq.bind_text(insert, bind++, cookiePath, -1, SQLITE_TRANSIENT); /* path */
        sq.bind_int64(insert, bind++, (sqlite_int64)expires);          /* expires_utc */
        sq.bind_int(insert, bind++, cookies[i].secure ? 1 : 0);        /* is_secure */
        sq.bind_int(insert, bind++, cookies[i].httpOnly ? 1 : 0);      /* is_httponly */
        sq.bind_int64(insert, bind++, (sqlite_int64)nowUtc);           /* last_access_utc */
        sq.bind_int(insert, bind++, expires ? 1 : 0);                  /* has_expires */
        sq.bind_int(insert, bind++, expires ? 1 : 0);                  /* is_persistent */
        sq.bind_int(insert, bind++, cookies[i].sameSite);              /* samesite */
        sq.bind_int(insert, bind++, 443);                             /* source_port */
        sq.bind_int64(insert, bind++, (sqlite_int64)nowUtc);          /* last_update_utc */
        if (sq.step(insert) != SQLITE_DONE) {
            Util_Log(L"cookies: writing %s failed", cookies[i].name);
            ok = FALSE;
        } else {
            put++;
        }
        SecureZeroMemory(sealed, sizeof sealed);
        SecureZeroMemory(value, sizeof value);
    }
    sq.finalize(insert);
    sq.exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    sq.close(db);
    SecureZeroMemory(key, sizeof key);
    if (ok) {
        Util_Log(L"cookies: %d sign-in cookie(s) written to %s", put, target->folder);
        if (wrote) *wrote = put;
    }
    return ok && put > 0;
}

/* ------------------------------------------------ reading the sign-in window's store */

typedef int (WINAPI *sqlite3_column_count_fn)(sqlite3_stmt *);
typedef const unsigned char *(WINAPI *sqlite3_column_text_fn)(sqlite3_stmt *, int);
typedef const void *(WINAPI *sqlite3_column_blob_fn)(sqlite3_stmt *, int);
typedef int (WINAPI *sqlite3_column_bytes_fn)(sqlite3_stmt *, int);
typedef sqlite_int64 (WINAPI *sqlite3_column_int64_fn)(sqlite3_stmt *, int);
typedef int (WINAPI *sqlite3_column_int_fn)(sqlite3_stmt *, int);
static sqlite3_column_text_fn  sq_column_text;
static sqlite3_column_blob_fn  sq_column_blob;
static sqlite3_column_bytes_fn sq_column_bytes;
static sqlite3_column_int64_fn sq_column_int64;
static sqlite3_column_int_fn   sq_column_int;

static BOOL LoadSqliteReaders(void)
{
    if (!LoadSqlite()) return FALSE;
    if (!sq_column_text) {
        sq_column_text  = (sqlite3_column_text_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_column_text");
        sq_column_blob  = (sqlite3_column_blob_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_column_blob");
        sq_column_bytes = (sqlite3_column_bytes_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_column_bytes");
        sq_column_int64 = (sqlite3_column_int64_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_column_int64");
        sq_column_int   = (sqlite3_column_int_fn)(void (*)(void))GetProcAddress(sq.dll, "sqlite3_column_int");
    }
    return sq_column_text && sq_column_blob && sq_column_bytes && sq_column_int64 && sq_column_int;
}

static void WideOf(const unsigned char *utf8, int bytes, WCHAR *out, int cch)
{
    int n = utf8 && bytes > 0 && cch > 1 ? MultiByteToWideChar(CP_UTF8, 0, (const char *)utf8, bytes, out, cch - 1) : 0;
    out[max(0, min(n, cch - 1))] = 0;
}

/* `webViewFolder`\EBWebView is where Edge WebView2 keeps the session: its cookie store, and the folder whose
 * Local State holds its key (Backup_ReadAppKey appends "Local State"). */
static BOOL WebViewPaths(const WCHAR *webViewFolder, WCHAR *cookies, size_t cookiesCch, WCHAR *stateDir, size_t stateDirCch)
{
    return SUCCEEDED(StringCchPrintfW(cookies, cookiesCch, L"%s\\EBWebView\\Default\\Network\\Cookies", webViewFolder)) &&
           SUCCEEDED(StringCchPrintfW(stateDir, stateDirCch, L"%s\\EBWebView", webViewFolder));
}

/* Rows of `query` in the cookie store at `path` (read-only, so the open window's writes are seen), each handed to
 * `each`; FALSE when it could not be read. */
typedef void (*CookieRow)(void *context, sqlite3_stmt *row);
static BOOL EachCookie(const WCHAR *path, const char *query, CookieRow each, void *context)
{
    WCHAR uri[LONG_PATH_CCH + 24];
    char utf8Uri[(LONG_PATH_CCH + 24) * 3];
    size_t i;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    BOOL ok = FALSE;
    if (!LoadSqliteReaders() || !Util_FileExists(path)) return FALSE;
    /* file:///C:/... URI with immutable=1: SQLite opens it read-only and takes no lock the live window would
     * block on. A Windows path's backslashes become slashes, as a file URI needs. */
    if (FAILED(StringCchPrintfW(uri, ARRAYSIZE(uri), L"file:///%s?immutable=1", path))) return FALSE;
    for (i = 0; uri[i]; i++) if (uri[i] == L'\\') uri[i] = L'/';
    if (Utf8Of(uri, utf8Uri, sizeof utf8Uri) == 0) return FALSE;
    if (sq.open_v2(utf8Uri, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
        if (db) sq.close(db);
        return FALSE;
    }
    if (sq.prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
        ok = TRUE;
        while (sq.step(stmt) == SQLITE_ROW) each(context, stmt);
        sq.finalize(stmt);
    }
    sq.close(db);
    return ok;
}

typedef struct SignInScan { BOOL signedIn; } SignInScan;
static void ScanSignIn(void *context, sqlite3_stmt *row)
{
    SignInScan *scan = (SignInScan *)context;
    if (sq_column_bytes(row, 0) > 3) scan->signedIn = TRUE;   /* a sealed sessionKey value ("v10" + more) */
}

BOOL Cookies_WebViewSignedIn(const WCHAR *webViewFolder)
{
    WCHAR cookies[LONG_PATH_CCH], stateDir[LONG_PATH_CCH];
    SignInScan scan = { FALSE };
    if (!webViewFolder || !WebViewPaths(webViewFolder, cookies, ARRAYSIZE(cookies), stateDir, ARRAYSIZE(stateDir))) return FALSE;
    EachCookie(cookies, "SELECT encrypted_value FROM cookies WHERE host_key LIKE '%claude.ai' AND name IN ('sessionKey','sessionKeyV3')",
               ScanSignIn, &scan);
    return scan.signedIn;
}

typedef struct CookieRead { SignInCookie *out; int cap, count; const BYTE *key; } CookieRead;
static void CollectCookie(void *context, sqlite3_stmt *row)
{
    CookieRead *read = (CookieRead *)context;
    SignInCookie *c;
    const BYTE *blob;
    int blobLength;
    BYTE plain[COOKIE_VALUE_CCH * 3];
    size_t plainLength = 0;
    if (read->count >= read->cap) return;
    c = &read->out[read->count];
    ZeroMemory(c, sizeof *c);
    WideOf(sq_column_text(row, 0), sq_column_bytes(row, 0), c->host, COOKIE_HOST_CCH);
    if (!Cookies_SignInHost(c->host)) return;
    WideOf(sq_column_text(row, 1), sq_column_bytes(row, 1), c->name, COOKIE_NAME_CCH);
    WideOf(sq_column_text(row, 3), sq_column_bytes(row, 3), c->path, COOKIE_PATH_CCH);
    c->expires  = (ULONGLONG)sq_column_int64(row, 4);
    c->secure   = sq_column_int(row, 5);
    c->httpOnly = sq_column_int(row, 6);
    c->sameSite = sq_column_int(row, 7);
    blob = (const BYTE *)sq_column_blob(row, 2);
    blobLength = sq_column_bytes(row, 2);
    if (!blob || blobLength <= 0 || (size_t)blobLength > sizeof plain + 64 ||
        !Core_OpenSafeStorage(blob, (size_t)blobLength, read->key, 32, plain, sizeof plain, &plainLength)) {
        SecureZeroMemory(plain, sizeof plain);
        return;   /* a value this key cannot open (another profile's cookie that slipped in): left out */
    }
    WideOf(plain, (int)plainLength, c->value, COOKIE_VALUE_CCH);
    SecureZeroMemory(plain, sizeof plain);
    if (c->value[0] || !c->httpOnly) read->count++;
}

BOOL Cookies_ReadWebView(const WCHAR *webViewFolder, SignInCookie *out, int cap, int *count)
{
    WCHAR cookies[LONG_PATH_CCH], stateDir[LONG_PATH_CCH];
    BYTE key[64];
    size_t keyLength = 0;
    CookieRead read;
    BOOL ok;
    if (count) *count = 0;
    if (!webViewFolder || !out || cap <= 0 || !WebViewPaths(webViewFolder, cookies, ARRAYSIZE(cookies), stateDir, ARRAYSIZE(stateDir)))
        return FALSE;
    if (!Backup_ReadAppKey(stateDir, key, sizeof key, &keyLength) || keyLength != 32) {
        Util_Log(L"cookies: the sign-in window has no Local State key yet");
        return FALSE;
    }
    read.out = out;
    read.cap = cap;
    read.count = 0;
    read.key = key;
    ok = EachCookie(cookies, "SELECT host_key,name,encrypted_value,path,expires_utc,is_secure,is_httponly,samesite "
                             "FROM cookies WHERE host_key LIKE '%claude.ai'", CollectCookie, &read);
    SecureZeroMemory(key, sizeof key);
    if (count) *count = read.count;
    return ok && read.count > 0;
}
