/*
 * Unit tests for src/core.c (the pure helpers). Built and run by build.cmd;
 * exits non-zero when a check fails.
 */
#include "../src/app.h"
#include <bcrypt.h>   /* sealing as Chromium does, to open it */
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int g_failures, g_checks;

static void Check(const char *name, BOOL ok)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        printf("  FAIL  %s\n", name);
    }
}

static void CheckStr(const char *name, const WCHAR *expected, const WCHAR *actual)
{
    BOOL ok = wcscmp(expected, actual) == 0;
    Check(name, ok);
    if (!ok) wprintf(L"        expected: [%s]\n        actual:   [%s]\n", expected, actual);
}

static ULONGLONG TimeOnTestDay(WORD h, WORD m, WORD s)
{
    SYSTEMTIME st;
    ZeroMemory(&st, sizeof st);
    st.wYear = 2026; st.wMonth = 9; st.wDay = 21;
    st.wHour = h; st.wMinute = m; st.wSecond = s;
    return Core_SystemTimeTicks(&st);
}

static void TestLaunchArgs(void)
{
    WCHAR out[URL_CCH + 2 * MAX_PATH];
    const WCHAR *url = L"claude://login/google-auth?code=abc";
    const WCHAR *spacey = L"C:\\Users\\John Doe\\AppData\\Roaming\\Claude-Client A";

    Check("stock, no link -> no argument", Core_BuildLaunchArgs(NULL, NULL, out, ARRAYSIZE(out)));
    CheckStr("stock, no link", L"", out);
    Check("stock + link built", Core_BuildLaunchArgs(NULL, url, out, ARRAYSIZE(out)));
    CheckStr("stock + link -> quoted link only", L"\"claude://login/google-auth?code=abc\"", out);
    Check("path with spaces built", Core_BuildLaunchArgs(spacey, NULL, out, ARRAYSIZE(out)));
    CheckStr("path with spaces stays one quoted token",
             L"--user-data-dir=\"C:\\Users\\John Doe\\AppData\\Roaming\\Claude-Client A\"", out);
    Check("path + link built", Core_BuildLaunchArgs(spacey, url, out, ARRAYSIZE(out)));
    CheckStr("path + link",
             L"--user-data-dir=\"C:\\Users\\John Doe\\AppData\\Roaming\\Claude-Client A\" \"claude://login/google-auth?code=abc\"", out);
    Check("path with a trailing backslash built",
          Core_BuildLaunchArgs(L"C:\\Users\\me\\AppData\\Roaming\\Claude-Work\\", NULL, out, ARRAYSIZE(out)));
    CheckStr("trailing backslash cannot escape the closing quote",
             L"--user-data-dir=\"C:\\Users\\me\\AppData\\Roaming\\Claude-Work\"", out);
    Check("a quote in the link is refused", !Core_BuildLaunchArgs(NULL, L"claude://x\" --inspect \"", out, ARRAYSIZE(out)));
    Check("a backslash in the link is refused", !Core_BuildLaunchArgs(NULL, L"claude://x\\", out, ARRAYSIZE(out)));
    Check("a link that is not a claude: link (a switch) is refused", !Core_BuildLaunchArgs(NULL, L"--inspect=9229", out, ARRAYSIZE(out)) &&
                                                                      !Core_BuildLaunchArgs(NULL, L"clau", out, ARRAYSIZE(out)));
    Check("a quote in the data folder is refused", !Core_BuildLaunchArgs(L"C:\\x\" --inspect \"", NULL, out, ARRAYSIZE(out)));
    Check("too small a buffer fails", !Core_BuildLaunchArgs(spacey, url, out, 20));
}

static void TestPrivateSignIn(void)
{
    static const WCHAR *const refused[] = {
        L"", L"http://claude.ai/login", L"https://claude.ai.evil.example/login",
        L"https://claude.ai@evil.example/login", L"https://user:password@claude.ai/login",
        L"https://claude.ai:443/login", L"https://www.claude.ai/login", L"claude://login/code",
        L"https://claude.ai/settings", L"https://claude.ai/login-extra", L"https://claude.ai/Login",
        L"https://claude.ai/login\" --load-extension=x", L"https://claude.ai/login\\x",
        L"https://claude.ai/login?x=a b", L"https://claude.ai/login?x=a\nb",
        L"https://claude.ai/login?x=a\x00A0" L"b", L"https://claude.ai/login?x=a\x3000" L"b"
    };
    WCHAR url[URL_CCH], args[URL_CCH + MAX_PATH + 160], longUrl[URL_CCH + 16];
    const WCHAR *folder = L"C:\\Users\\Example User\\Private Sign In\\{test}";
    size_t i;
    Check("private sign-in: official home", Core_PrivateSignInUrl(L"https://claude.ai/", url, ARRAYSIZE(url)));
    Check("private sign-in: bare origin", Core_PrivateSignInUrl(L"https://claude.ai", url, ARRAYSIZE(url)));
    Check("private sign-in: trims outer space and keeps the complete query",
          Core_PrivateSignInUrl(L"  HTTPS://CLAUDE.AI/login/desktop?state=a%2Fb&code=x%22y#finish  ", url, ARRAYSIZE(url)));
    CheckStr("private sign-in: query unchanged", L"HTTPS://CLAUDE.AI/login/desktop?state=a%2Fb&code=x%22y#finish", url);
    for (i = 0; i < ARRAYSIZE(refused); i++) {
        Check("private sign-in: unsafe or unobserved entry refused", !Core_PrivateSignInUrl(refused[i], url, ARRAYSIZE(url)));
        CheckStr("private sign-in: refusal leaves no URL", L"", url);
    }
    Check("private sign-in: no output or no space", !Core_PrivateSignInUrl(L"https://claude.ai/login", NULL, 1) &&
          !Core_PrivateSignInUrl(L"https://claude.ai/login", url, 0));
    Check("private sign-in: short output refused", !Core_PrivateSignInUrl(L"https://claude.ai/login", url, 8));
    StringCchCopyW(longUrl, ARRAYSIZE(longUrl), L"https://claude.ai/login?state=");
    for (i = wcslen(longUrl); i < ARRAYSIZE(longUrl) - 1; i++) longUrl[i] = L'a';
    longUrl[ARRAYSIZE(longUrl) - 1] = 0;
    Check("private sign-in: overlong URL refused", !Core_PrivateSignInUrl(longUrl, url, ARRAYSIZE(url)));
    Check("private browser: Edge arguments", Core_PrivateBrowserArgs(CORE_BROWSER_EDGE, folder, L"https://claude.ai/login?state=x%22y", args, ARRAYSIZE(args)));
    CheckStr("private browser: isolated Edge path and URL each quoted",
        L"--user-data-dir=\"C:\\Users\\Example User\\Private Sign In\\{test}\" --inprivate --new-window --no-first-run --no-default-browser-check \"https://claude.ai/login?state=x%22y\"", args);
    Check("private browser: Chrome arguments", Core_PrivateBrowserArgs(CORE_BROWSER_CHROMIUM, L"C:\\private\\", L"https://claude.ai/login", args, ARRAYSIZE(args)));
    CheckStr("private browser: Chrome uses incognito and strips the path's trailing slash",
        L"--user-data-dir=\"C:\\private\" --incognito --new-window --no-first-run --no-default-browser-check \"https://claude.ai/login\"", args);
    {
        WCHAR link[URL_CCH], email[64];
        /* someone@example.com in base64 */
        Check("magic link: read, with the address it is for",
              Core_MagicLink(L" https://claude.ai/magic-link#0123456789abcdef0123456789ABCDEF:c29tZW9uZUBleGFtcGxlLmNvbQ== ", link, ARRAYSIZE(link), email, ARRAYSIZE(email)) &&
              wcscmp(email, L"someone@example.com") == 0 &&
              wcscmp(link, L"https://claude.ai/magic-link#0123456789abcdef0123456789ABCDEF:c29tZW9uZUBleGFtcGxlLmNvbQ==") == 0);
        Check("magic link: unpadded base64 is read too",
              Core_MagicLink(L"https://claude.ai/magic-link#0123456789abcdef0123456789abcdef:c29tZW9uZUBleGFtcGxlLmNvbQ", NULL, 0, email, ARRAYSIZE(email)) &&
              wcscmp(email, L"someone@example.com") == 0);
        Check("magic link: another host, a short code, no address, or more after it is refused",
              !Core_MagicLink(L"https://claude.ai.example.com/magic-link#0123456789abcdef0123456789abcdef:c29tZW9uZUBleGFtcGxlLmNvbQ==", NULL, 0, NULL, 0) &&
              !Core_MagicLink(L"https://claude.ai/magic-link#0123456789abcdef:c29tZW9uZUBleGFtcGxlLmNvbQ==", NULL, 0, NULL, 0) &&
              !Core_MagicLink(L"https://claude.ai/magic-link#0123456789abcdef0123456789abcdef:aGVsbG8=", NULL, 0, NULL, 0) &&
              !Core_MagicLink(L"https://claude.ai/magic-link#0123456789abcdef0123456789abcdef:c29tZW9uZUBleGFtcGxlLmNvbQ==&x=1", NULL, 0, NULL, 0));
        Check("magic link: a sign-in link private sign-in opens",
              Core_PrivateSignInUrl(L"https://claude.ai/magic-link#0123456789abcdef0123456789abcdef:c29tZW9uZUBleGFtcGxlLmNvbQ==", link, ARRAYSIZE(link)) &&
              !Core_PrivateSignInUrl(L"https://claude.ai/magic-link#zz", link, ARRAYSIZE(link)));
    }
    Check("private browser: Firefox arguments", Core_PrivateBrowserArgs(CORE_BROWSER_FIREFOX, L"C:\\private", L"https://claude.ai/login", args, ARRAYSIZE(args)));
    CheckStr("private browser: Firefox opens a private window in a profile of its own",
        L"-no-remote -profile \"C:\\private\" -private-window \"https://claude.ai/login\"", args);
    Check("private browser: untrusted URL refused", !Core_PrivateBrowserArgs(TRUE, folder, L"https://example.com", args, ARRAYSIZE(args)));
    Check("private browser: quoted directory refused", !Core_PrivateBrowserArgs(TRUE, L"C:\\private\" --foo", L"https://claude.ai/login", args, ARRAYSIZE(args)));
    Check("private browser: control in directory refused", !Core_PrivateBrowserArgs(TRUE, L"C:\\private\n", L"https://claude.ai/login", args, ARRAYSIZE(args)));
    Check("private browser: only a drive-absolute private directory",
        !Core_PrivateBrowserArgs(TRUE, NULL, L"https://claude.ai/login", args, ARRAYSIZE(args)) &&
        !Core_PrivateBrowserArgs(TRUE, L"private", L"https://claude.ai/login", args, ARRAYSIZE(args)) &&
        !Core_PrivateBrowserArgs(TRUE, L"C:\\", L"https://claude.ai/login", args, ARRAYSIZE(args)) &&
        !Core_PrivateBrowserArgs(TRUE, L"1:\\private", L"https://claude.ai/login", args, ARRAYSIZE(args)));
    Check("private browser: small command buffer refused", !Core_PrivateBrowserArgs(TRUE, folder, L"https://claude.ai/login", args, 20));
}

static void TestSanitizeUrl(void)
{
    WCHAR out[URL_CCH], big[URL_CCH + 16];
    int i;

    Check("claude:// accepted", Core_SanitizeUrl(L"claude://login/google-auth?code=4%2F0A&x=1", out, ARRAYSIZE(out)));
    CheckStr("claude:// untouched", L"claude://login/google-auth?code=4%2F0A&x=1", out);
    Check("scheme is case-insensitive", Core_SanitizeUrl(L"CLAUDE://x", out, ARRAYSIZE(out)));
    Check("surrounding spaces trimmed", Core_SanitizeUrl(L"  claude://x  ", out, ARRAYSIZE(out)) && wcscmp(out, L"claude://x") == 0);
    Check("http refused", !Core_SanitizeUrl(L"https://claude.ai", out, ARRAYSIZE(out)));
    Check("look-alike scheme refused", !Core_SanitizeUrl(L"claudex://x", out, ARRAYSIZE(out)));
    Check("bare scheme refused", !Core_SanitizeUrl(L"claude:", out, ARRAYSIZE(out)));
    Check("empty refused", !Core_SanitizeUrl(L"", out, ARRAYSIZE(out)));
    Core_SanitizeUrl(L"claude://a\" --inspect=9229 \"b", out, ARRAYSIZE(out));
    CheckStr("quotes and spaces are percent-encoded", L"claude://a%22%20--inspect=9229%20%22b", out);
    Core_SanitizeUrl(L"claude://a\\b\\", out, ARRAYSIZE(out));
    CheckStr("backslashes are percent-encoded", L"claude://a%5Cb%5C", out);
    Core_SanitizeUrl(L"claude://a\tb", out, ARRAYSIZE(out));
    CheckStr("control characters are percent-encoded", L"claude://a%09b", out);
    Core_SanitizeUrl(L"claude://a\x007F" L"b", out, ARRAYSIZE(out));
    CheckStr("DEL is percent-encoded", L"claude://a%7Fb", out);
    Check("non-breaking space refused", !Core_SanitizeUrl(L"claude://a\x00A0" L"b", out, ARRAYSIZE(out)));
    Check("ideographic space refused", !Core_SanitizeUrl(L"claude://a\x3000" L"b", out, ARRAYSIZE(out)));
    {
        static const WCHAR kOtherSpaces[] = { 0x0085, 0x1680, 0x2000, 0x2003, 0x200A, 0x2028, 0x2029, 0x202F, 0x205F };
        WCHAR spaced[16];
        size_t space;
        BOOL allRefused = TRUE;
        for (space = 0; space < ARRAYSIZE(kOtherSpaces); space++) {
            StringCchPrintfW(spaced, ARRAYSIZE(spaced), L"claude://a%cb", kOtherSpaces[space]);
            if (Core_SanitizeUrl(spaced, out, ARRAYSIZE(out))) {
                printf("        U+%04X accepted\n", (unsigned)kOtherSpaces[space]);
                allRefused = FALSE;
            }
        }
        Check("every other Unicode space refused", allRefused);
    }
    Check("Unicode spaces around the link are trimmed",
          Core_SanitizeUrl(L"\x2003" L"claude://x\x202F", out, ARRAYSIZE(out)) && wcscmp(out, L"claude://x") == 0);
    Core_SanitizeUrl(L"claude://caf\x00E9", out, ARRAYSIZE(out));
    CheckStr("non-ASCII kept", L"claude://caf\x00E9", out);
    for (i = 0; i < URL_CCH + 8; i++) big[i] = L'a';
    memcpy(big, L"claude://", 9 * sizeof(WCHAR));
    big[URL_CCH + 8] = 0;
    Check("overlong link refused", !Core_SanitizeUrl(big, out, ARRAYSIZE(out)));

    Check("google callback is a sign-in link", Core_IsSignInUrl(L"claude://login/google-auth?code=x"));
    Check("magic link is a sign-in link", Core_IsSignInUrl(L"claude://claude.ai/magic-link#abc"));
    Check("SSO callback is a sign-in link", Core_IsSignInUrl(L"claude://sso/callback?x"));
    /* Each marker alone, so none can be dropped unnoticed. */
    Check("\"login\" alone makes a sign-in link", Core_IsSignInUrl(L"claude://login"));
    Check("\"auth\" alone makes a sign-in link", Core_IsSignInUrl(L"claude://oauth2/x"));
    Check("\"magic-link\" alone makes a sign-in link", Core_IsSignInUrl(L"claude://magic-link"));
    Check("\"sso\" alone makes a sign-in link", Core_IsSignInUrl(L"claude://sso"));
    Check("\"callback\" alone makes a sign-in link", Core_IsSignInUrl(L"claude://x/callback"));
    Check("a chat link is not a sign-in link", !Core_IsSignInUrl(L"claude://claude.ai/new?q=hello"));
    Check("sign-in words in the query do not count", !Core_IsSignInUrl(L"claude://claude.ai/new?q=espresso+author+login"));
    Check("sign-in words in the fragment do not count", !Core_IsSignInUrl(L"claude://claude.ai/chat/1#oauth"));
}

static void TestNames(void)
{
    WCHAR name[LABEL_CCH], folder[FOLDER_CCH], label[LABEL_CCH];
    const WCHAR *err = NULL;

    Check("simple name", Core_ValidateNewName(L"Work", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    CheckStr("simple name -> folder", L"Claude-Work", folder);
    Check("trimmed", Core_ValidateNewName(L"  Client A  ", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err) &&
                         wcscmp(name, L"Client A") == 0 && wcscmp(folder, L"Claude-Client A") == 0);
    Check("accents allowed", Core_ValidateNewName(L"Zo\x00EB perso", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("digits, dash, dot, underscore allowed", Core_ValidateNewName(L"team_2.0-b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("empty refused", !Core_ValidateNewName(L"   ", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err) && err);
    Check("33 characters refused", !Core_ValidateNewName(L"abcdefghijabcdefghijabcdefghijabc", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("32 characters accepted", Core_ValidateNewName(L"abcdefghijabcdefghijabcdefghijab", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("slash refused", !Core_ValidateNewName(L"a/b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("backslash refused", !Core_ValidateNewName(L"a\\b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("colon refused", !Core_ValidateNewName(L"a:b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("quote refused", !Core_ValidateNewName(L"a\"b", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("leading dot refused", !Core_ValidateNewName(L".work", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("trailing dot refused", !Core_ValidateNewName(L"work.", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("3p reserved", !Core_ValidateNewName(L"3P", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("Data reserved", !Core_ValidateNewName(L"data", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("<x>-Data reserved", !Core_ValidateNewName(L"Work-Data", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("<x>-3p reserved", !Core_ValidateNewName(L"Work-3p", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("Database allowed", Core_ValidateNewName(L"Database", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err));
    Check("a character outside the BMP refused", !Core_ValidateNewName(L"a\xD83D\xDE00", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err) && err);
    Check("Unicode spaces around a name trimmed", Core_ValidateNewName(L"\x2003Work\x00A0", name, ARRAYSIZE(name), folder, ARRAYSIZE(folder), &err) &&
                                                  wcscmp(name, L"Work") == 0);

    Check("label: anything printable", Core_ValidateLabel(L"  Perso (Zo\x00EB) \x2022 2026 ", label, ARRAYSIZE(label), &err) &&
                                           wcscmp(label, L"Perso (Zo\x00EB) \x2022 2026") == 0);
    Check("label: a no-break space inside kept", Core_ValidateLabel(L"a\x00A0" L"b", label, ARRAYSIZE(label), &err) &&
                                                 wcscmp(label, L"a\x00A0" L"b") == 0);
    Check("label: empty refused", !Core_ValidateLabel(L"", label, ARRAYSIZE(label), &err));
    Check("label: control char refused", !Core_ValidateLabel(L"a\nb", label, ARRAYSIZE(label), &err));
    Check("label: DEL refused", !Core_ValidateLabel(L"a\x007F" L"b", label, ARRAYSIZE(label), &err) && err);
    Check("label: C1 controls refused", !Core_ValidateLabel(L"a\x0080" L"b", label, ARRAYSIZE(label), &err) &&
                                        !Core_ValidateLabel(L"a\x009F" L"b", label, ARRAYSIZE(label), &err));
    Check("label: 48 characters accepted", Core_ValidateLabel(L"abcdefghijabcdefghijabcdefghijabcdefghijabcdefgh", label, ARRAYSIZE(label), &err));
    Check("label: 49 characters refused", !Core_ValidateLabel(L"abcdefghijabcdefghijabcdefghijabcdefghijabcdefghi", label, ARRAYSIZE(label), &err));

    {
        WCHAR badge[BADGE_CCH];
        Check("badge: trimmed, two characters kept", Core_CleanBadge(L"  AB ", badge, ARRAYSIZE(badge)) && wcscmp(badge, L"AB") == 0);
        Check("badge: cut after two characters", Core_CleanBadge(L"\x5DE5\x4F5C\x5BA4", badge, ARRAYSIZE(badge)) &&
                                                   wcscmp(badge, L"\x5DE5\x4F5C") == 0);
        Check("badge: a surrogate pair is one character", Core_CleanBadge(L"\xD83D\xDE80" L"XY", badge, ARRAYSIZE(badge)) &&
                                                            wcscmp(badge, L"\xD83D\xDE80" L"X") == 0);
        Check("badge: a space left at the cut goes", Core_CleanBadge(L"A B", badge, ARRAYSIZE(badge)) && wcscmp(badge, L"A") == 0);
        Check("badge: empty is valid (the initial)", Core_CleanBadge(L"   ", badge, ARRAYSIZE(badge)) && badge[0] == 0 &&
                                                       Core_CleanBadge(NULL, badge, ARRAYSIZE(badge)) && badge[0] == 0);
        Check("badge: a control character refused", !Core_CleanBadge(L"A\tB", badge, ARRAYSIZE(badge)) && badge[0] == 0);
        Check("badge: a buffer too short keeps whole characters", Core_CleanBadge(L"\xD83D\xDE80\xD83D\xDE80", badge, 4) &&
                                                                    wcscmp(badge, L"\xD83D\xDE80") == 0);
    }

    Check("folder Claude-Work", Core_IsProfileFolder(L"Claude-Work"));
    Check("folder case-insensitive prefix", Core_IsProfileFolder(L"claude-work"));
    Check("folder with spaces", Core_IsProfileFolder(L"Claude-Client A"));
    Check("folder Claude-3p is Claude's own", !Core_IsProfileFolder(L"Claude-3p"));
    Check("folder Claude-Work-Data is Claude's own", !Core_IsProfileFolder(L"Claude-Work-Data"));
    Check("folder Claude is the stock one", !Core_IsProfileFolder(L"Claude"));
    Check("folder Claude- is empty", !Core_IsProfileFolder(L"Claude-"));
    Check("folder Codex is unrelated", !Core_IsProfileFolder(L"Codex"));
    Check("folder with a control character is not a profile", !Core_IsProfileFolder(L"Claude-a\tb"));
    Check("folder with DEL or a C1 control is not a profile", !Core_IsProfileFolder(L"Claude-a\x007F" L"b") &&
                                                              !Core_IsProfileFolder(L"Claude-a\x0090" L"b"));
    {
        WCHAR longFolder[FOLDER_CCH + 1];
        wmemset(longFolder, L'a', FOLDER_CCH);
        longFolder[FOLDER_CCH] = 0;
        memcpy(longFolder, PROFILE_PREFIX, wcslen(PROFILE_PREFIX) * sizeof(WCHAR));
        Check("folder too long to keep is not a profile", !Core_IsProfileFolder(longFolder));
    }
}

/* `*st` starts as no log line could set it, so a value left over cannot pass. */
static void ResetTime(SYSTEMTIME *st)
{
    FillMemory(st, sizeof *st, 0xFF);
}

static BOOL LatestSignIn(const char *log, SYSTEMTIME *st)
{
    ResetTime(st);
    return Core_LatestSignInStart(log, strlen(log), st);
}

static BOOL IsTime(const SYSTEMTIME *st, WORD day, WORD hour, WORD minute, WORD second)
{
    return st->wYear == 2026 && st->wMonth == 9 && st->wDay == day && st->wHour == hour && st->wMinute == minute &&
           st->wSecond == second;
}

static void TestLogParsing(void)
{
    static const char log[] =
        "ogin/app-google-auth\n"
        "2026-09-21 07:01:06 [info] Starting app {\n"
        "2026-09-21 07:01:24 [info] [Auth] Using system browser for: /login/app-google-auth\r\n"
        "2026-09-21 07:02:54 [info] [Auth] Using system browser for: /login/app-google-auth\r\n"
        "2026-09-21 07:02:31 [info] [account] User is logged out\n"
        "  [Auth] Using system browser for: continuation line without a timestamp\n"
        "2026-09-21 07:03:08 [info] second-instance: suppressing duplicate argv";
    static const char started[] = "2026-09-21 07:01:06 [info] Starting app {\n";
    static const char invalid[] = "2026-13-21 07:01:06 [info] [Auth] Using system browser for: /x\n";
    static const char validThenInvalid[] =
        "2026-09-21 07:02:54 [info] [Auth] Using system browser for: /login/app-google-auth\n"
        "2026-09-31 99:99:99 [info] [Auth] Using system browser for: /login/app-google-auth\n";
    static const char validThenMissingDay[] =
        "2026-09-21 07:02:54 [info] [Auth] Using system browser for: /login/app-google-auth\n"
        "2026-09-31 07:05:00 [info] [Auth] Using system browser for: /login/app-google-auth\n";
    SYSTEMTIME st;

    Check("latest sign-in start found", LatestSignIn(log, &st));
    Check("latest sign-in start is 07:02:54", IsTime(&st, 21, 7, 2, 54));
    Check("no sign-in start", !LatestSignIn(started, &st));
    Check("empty log", !LatestSignIn("", &st));
    Check("missing log", !Core_LatestSignInStart(NULL, 0, &st));
    Check("invalid date refused", !LatestSignIn(invalid, &st));
    Check("a later invalid time does not hide a valid sign-in", LatestSignIn(validThenInvalid, &st) && IsTime(&st, 21, 7, 2, 54));
    Check("a day that does not exist (September 31) is not a sign-in",
          LatestSignIn(validThenMissingDay, &st) && IsTime(&st, 21, 7, 2, 54));
}

/* `*forUpdate` starts the other way round, and `*st` as no line could set
 * it, so a value left over cannot pass. */
static BOOL LastQuit(const char *log, BOOL *forUpdate, SYSTEMTIME *st, BOOL expectedForUpdate)
{
    *forUpdate = !expectedForUpdate;
    ResetTime(st);
    return Core_LastQuit(log, strlen(log), st, forUpdate);
}

static void TestQuitParsing(void)
{
    /* The window whose updater installs the update. */
    static const char updater[] =
        "2026-09-28 20:48:10 [info] [stealth-relaunch] Saved navigation history (14 entries, active=12)\n"
        "2026-09-28 20:48:10 [info] [CCD] Stopping 4 active session(s) on quit\n"
        "2026-09-28 20:48:12 [info] Session stop before update took 1460ms\n"
        "2026-09-28 20:48:12 [info] beforeQuitForUpdate handler fired, going down for update\n"
        "2026-09-28 20:48:13 [info] Windows session ending (close-app) - quitting the app\n";
    /* Another window, closed by Windows for the same update; the new version
     * already logs its start. */
    static const char closed[] =
        "2026-09-22 20:36:32 [info] willQuit: handler is ready for quit, so quitting\n"
        "2026-09-28 20:48:01 [info] [process-memory] trigger=interval\n"
        "2026-09-28 20:48:13 [info] Windows session ending (close-app) - quitting the app\r\n"
        "2026-09-28 20:48:14 [info] Starting app {\n"
        "  appVersion: '2.16120.0',\n"
        "2026-09-28 20:48:14 [info] [quit-cleanup] previous quit: {\n"
        "  for_update: false,\n";
    static const char userQuit[] =
        "2026-09-23 01:35:53 [info] Windows session ending (close-app) - quitting the app\n"
        "2026-09-24 12:38:15 [info] Quitting app on main window close since tray is disabled\n"
        "2026-09-24 12:38:36 [info] beforeQuit: handler fired, going down\n"
        "2026-09-24 12:38:36 [info] beforeQuit: handler is ready for quit, so quitting\n"
        "2026-09-24 12:38:36 [info] willQuit: handler is ready for quit, so quitting\n";
    static const char shutdown[] =
        "2026-09-28 20:48:13 [info] Windows session ending (close-app) - quitting the app\n"
        "2026-09-29 09:40:15 [info] Windows session ending (shutdown) - quitting the app\n";
    static const char noQuit[] =
        "2026-09-28 20:48:14 [info] Starting app {\n"
        "2026-09-28 20:48:14 [info] [quit-cleanup] previous quit: {\n"
        "  Windows session ending (close-app) in a continuation line\n"
        "2026-09-28 20:48:21 [info] [CCD] Removing old version: 2.1.280\n";
    /* Each marker alone as the last quit line, after one saying the opposite,
     * so none can be dropped unnoticed. */
    static const struct { const char *name, *log; BOOL forUpdate; } kMarkers[] = {
        { "beforeQuitForUpdate",
          "2026-09-27 08:00:00 [info] willQuit: handler is ready for quit, so quitting\n"
          "2026-09-28 20:48:12 [info] beforeQuitForUpdate handler fired, going down for update\n", TRUE },
        { "Quitting app",
          "2026-09-27 08:00:00 [info] Windows session ending (close-app) - quitting the app\n"
          "2026-09-28 20:48:12 [info] Quitting app on main window close since tray is disabled\n", FALSE },
        { "beforeQuit:",
          "2026-09-27 08:00:00 [info] Windows session ending (close-app) - quitting the app\n"
          "2026-09-28 20:48:12 [info] beforeQuit: handler fired, going down\n", FALSE },
        { "willQuit:",
          "2026-09-27 08:00:00 [info] Windows session ending (close-app) - quitting the app\n"
          "2026-09-28 20:48:12 [info] willQuit: handler is ready for quit, so quitting\n", FALSE },
    };
    BOOL forUpdate = FALSE;
    SYSTEMTIME st;
    size_t marker;

    Check("updater: quit found", LastQuit(updater, &forUpdate, &st, TRUE));
    Check("updater: for an update", forUpdate);
    Check("updater: time of the last quit line", IsTime(&st, 28, 20, 48, 13));
    Check("closed by Windows: for an update", LastQuit(closed, &forUpdate, &st, TRUE) && forUpdate && IsTime(&st, 28, 20, 48, 13));
    Check("quit from the window: not an update", LastQuit(userQuit, &forUpdate, &st, FALSE) && !forUpdate && IsTime(&st, 24, 12, 38, 36));
    Check("Windows shutdown: not an update", LastQuit(shutdown, &forUpdate, &st, FALSE) && !forUpdate && IsTime(&st, 29, 9, 40, 15));
    for (marker = 0; marker < ARRAYSIZE(kMarkers); marker++) {
        BOOL ok = LastQuit(kMarkers[marker].log, &forUpdate, &st, kMarkers[marker].forUpdate) &&
                  forUpdate == kMarkers[marker].forUpdate && IsTime(&st, 28, 20, 48, 12);
        Check("a quit marker alone decides", ok);
        if (!ok) printf("        marker %s\n", kMarkers[marker].name);
    }
    Check("no quit line", !LastQuit(noQuit, &forUpdate, &st, FALSE));
    Check("empty log", !LastQuit("", &forUpdate, &st, FALSE));
    Check("missing quit log", !Core_LastQuit(NULL, 0, &st, &forUpdate));
    Check("a quit line dated a day that does not exist is not used",
          !LastQuit("2026-02-30 10:00:00 [info] Quitting app\n", &forUpdate, &st, FALSE));
}

static void TestRouting(void)
{
    const ULONGLONG window = SIGNIN_MAX_AGE_MINUTES * 60 * TICKS_PER_SECOND;
    ULONGLONG ticks[3];
    int n;
    RouteReason why;

    {
        BOOL running[4] = { TRUE, FALSE, FALSE, FALSE }, ask = TRUE;
        Check("notification: nothing but the default profile runs -> it stays",
              Core_NotificationTarget(running, 4, 0, -1, &ask) == -1 && !ask);
        running[2] = TRUE;
        Check("notification: one other runs -> that one, no question", Core_NotificationTarget(running, 4, 0, 0, &ask) == 2 && !ask);
        running[3] = TRUE;
        Check("notification: several others run -> asked, the one used last suggested",
              Core_NotificationTarget(running, 4, 0, 3, &ask) == 3 && ask);
        Check("notification: the default profile used last is not suggested -> the first other",
              Core_NotificationTarget(running, 4, 0, 0, &ask) == 2 && ask);
        running[0] = FALSE;
        Check("notification: the default profile itself counts for nothing",
              Core_NotificationTarget(running, 4, 0, 1, &ask) == 2 && ask);
    }
    n = Core_SuggestTarget(0, NULL, TimeOnTestDay(7, 3, 0), window, TRUE, -1, -1, &why);
    Check("nothing running -> the default profile, no window", n == -1 && why == ROUTE_NOTHING_RUNNING);

    n = Core_SuggestTarget(1, NULL, TimeOnTestDay(7, 3, 0), window, TRUE, -1, -1, &why);
    Check("one window -> it", n == 0 && why == ROUTE_ONLY_ONE);

    ticks[0] = TimeOnTestDay(7, 2, 54);
    n = Core_SuggestTarget(1, ticks, TimeOnTestDay(7, 2, 59), window, TRUE, -1, -1, &why);
    Check("one window that started the sign-in -> it, named as the sign-in's", n == 0 && why == ROUTE_SIGNIN);

    ticks[0] = TimeOnTestDay(7, 2, 54); ticks[1] = 0;
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 2, 59), window, TRUE, 1, 1, &why);
    Check("sign-in -> the window that opened the browser, not the last used", n == 0 && why == ROUTE_SIGNIN);

    ticks[0] = TimeOnTestDay(7, 2, 54); ticks[1] = TimeOnTestDay(7, 3, 2);
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 3, 7), window, TRUE, 0, 0, &why);
    Check("sign-in -> the most recent browser opening", n == 1 && why == ROUTE_SIGNIN);

    ticks[0] = TimeOnTestDay(6, 30, 0); ticks[1] = TimeOnTestDay(6, 40, 0);
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 3, 0), window, TRUE, 1, 0, &why);
    Check("stale sign-ins -> the window used last", n == 1 && why == ROUTE_LAST_USED);

    ticks[0] = TimeOnTestDay(7, 5, 0); ticks[1] = 0;
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 3, 0), window, TRUE, 1, 1, &why);
    Check("a sign-in stamped two minutes ahead is ignored", n == 1 && why == ROUTE_LAST_USED);

    ticks[0] = TimeOnTestDay(7, 3, 30); ticks[1] = 0;
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 3, 0), window, TRUE, 1, 1, &why);
    Check("a sign-in stamped seconds ahead (clock skew) still counts", n == 0 && why == ROUTE_SIGNIN);

    n = Core_SuggestTarget(3, NULL, TimeOnTestDay(7, 3, 0), window, TRUE, 2, 0, &why);
    Check("sign-in link, no log -> the window used last", n == 2 && why == ROUTE_LAST_USED);

    ticks[0] = TimeOnTestDay(7, 2, 54); ticks[1] = 0;
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 3, 0), window, FALSE, 1, 0, &why);
    Check("other link -> last used window, whatever the sign-ins", n == 1 && why == ROUTE_LAST_USED);

    n = Core_SuggestTarget(2, NULL, TimeOnTestDay(7, 3, 0), window, FALSE, -1, 1, &why);
    Check("other link, no window in front -> default profile", n == 1 && why == ROUTE_DEFAULT);

    n = Core_SuggestTarget(2, NULL, TimeOnTestDay(7, 3, 0), window, FALSE, -1, -1, &why);
    Check("other link, default not running -> first window", n == 0 && why == ROUTE_FIRST);

    n = Core_SuggestTarget(2, NULL, TimeOnTestDay(7, 3, 0), window, FALSE, 2, 5, &why);
    Check("out-of-range last used and default -> first window", n == 0 && why == ROUTE_FIRST);

    ticks[0] = TimeOnTestDay(6, 48, 0); ticks[1] = 0;
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 3, 0), window, TRUE, 1, 1, &why);
    Check("a sign-in exactly at the age limit still counts", n == 0 && why == ROUTE_SIGNIN);
    ticks[0] = TimeOnTestDay(6, 47, 59);
    n = Core_SuggestTarget(2, ticks, TimeOnTestDay(7, 3, 0), window, TRUE, 1, 1, &why);
    Check("a sign-in a second past the age limit does not", n == 1 && why == ROUTE_LAST_USED);
}

static void TestShortcuts(void)
{
    const WCHAR *work = L"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work";
    const WCHAR *stock = L"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude";
    LinkInfo li;
    WCHAR out[MAX_PATH];

    Check("--launch quoted", Core_ArgsSelectProfile(L"--launch \"Claude-Work\"", L"Claude-Work"));
    Check("--launch unquoted", Core_ArgsSelectProfile(L"--launch Claude-Work", L"claude-work"));
    Check("--launch with spaces", Core_ArgsSelectProfile(L"--launch \"Claude-Client A\"", L"Claude-Client A"));
    Check("--launch another profile", !Core_ArgsSelectProfile(L"--launch \"Claude-Work\"", L"Claude"));
    Check("no --launch", !Core_ArgsSelectProfile(L"\"Claude-Work\"", L"Claude-Work"));
    Check("--launch without value", !Core_ArgsSelectProfile(L"--launch", L"Claude-Work"));

    Check("quoted dir argument", Core_ArgsReferenceDir(L"\"C:\\Tools\\start.cmd\" \"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work\"", work));
    Check("--user-data-dir=\"dir\"", Core_ArgsReferenceDir(L"--user-data-dir=\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude\"", stock));
    Check("dir with trailing backslash", Core_ArgsReferenceDir(L"--user-data-dir=\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude\\\"", stock));
    Check("stock dir is not a prefix match of Claude-Work", !Core_ArgsReferenceDir(L"\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work\"", stock));
    Check("dir inside a longer path is not a match", !Core_ArgsReferenceDir(L"\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude\\sub\"", stock));
    Check("--user-data-dir=dir unquoted", Core_ArgsReferenceDir(L"--user-data-dir=C:\\Data\\Claude-Work --x", L"C:\\Data\\Claude-Work"));
    Check("a folder of three characters or fewer (a root) never matches", !Core_ArgsReferenceDir(L"\"C:\\\"", L"C:\\"));

    ZeroMemory(&li, sizeof li);
    StringCchCopyW(li.target, ARRAYSIZE(li.target), L"C:\\Users\\x\\AppData\\Local\\Programs\\Claude Desktop Profiles Manager\\ClaudeDesktopProfilesManager.exe");
    StringCchCopyW(li.args, ARRAYSIZE(li.args), L"--launch \"Claude-Work\"");
    Check("our shortcut opens Work", Core_LinkOpensProfile(&li, L"Claude-Work", work, FALSE));
    Check("our Work shortcut does not open stock", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    li.args[0] = 0;
    Check("our manager shortcut opens no profile", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));

    ZeroMemory(&li, sizeof li);
    StringCchCopyW(li.target, ARRAYSIZE(li.target), L"C:\\Program Files\\WindowsApps\\Claude_2.16120.0.0_x64__pzs8sxrjxfjjc\\app\\Claude.exe");
    Check("plain Claude.exe shortcut opens stock", Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    Check("plain Claude.exe shortcut does not open Work", !Core_LinkOpensProfile(&li, L"Claude-Work", work, FALSE));
    StringCchCopyW(li.args, ARRAYSIZE(li.args), L"--user-data-dir=\"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work\"");
    Check("Claude.exe --user-data-dir=Work opens Work", Core_LinkOpensProfile(&li, L"Claude-Work", work, FALSE));
    Check("Claude.exe --user-data-dir=Work does not open stock", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));

    ZeroMemory(&li, sizeof li);
    StringCchCopyW(li.parsing, ARRAYSIZE(li.parsing), L"::{4234D49B-0245-4DF3-B780-3893943456E1}\\Claude_pzs8sxrjxfjjc!Claude");
    Check("Start-menu style Claude shortcut opens stock", Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    StringCchCopyW(li.parsing, ARRAYSIZE(li.parsing), L"::{4234D49B-0245-4DF3-B780-3893943456E1}\\Claude_pzs8sxrjxfjjc!SshAskpass");
    Check("another app of the package is not Claude", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    StringCchCopyW(li.parsing, ARRAYSIZE(li.parsing), L"shell:AppsFolder\\AnthropicPBC.Claude_fnn82j28hfe8t!Claude");
    Check("the Microsoft Store package's Claude opens stock", Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));
    Check("the Store package's Claude does not open Work", !Core_LinkOpensProfile(&li, L"Claude-Work", work, FALSE));
    StringCchCopyW(li.parsing, ARRAYSIZE(li.parsing), L"shell:AppsFolder\\Contoso.NotClaude_abc!Claude");
    Check("a package whose name only ends in Claude is not Claude's", !Core_LinkOpensProfile(&li, L"Claude", stock, TRUE));

    Check("package name: sideloaded Claude", Core_NamesClaudePackage(L"Claude_1.2.3.0_x64__pzs8sxrjxfjjc"));
    Check("package name: Claude of a publisher", Core_NamesClaudePackage(L"AnthropicPBC.Claude_2.0.0.0_x64__fnn82j28hfe8t"));
    Check("package name: case does not count", Core_NamesClaudePackage(L"claude_1_x64__a"));
    Check("package name: in a parsing name", Core_NamesClaudePackage(L"::{4234D49B-0245-4DF3-B780-3893943456E1}\\Claude_x!Claude"));
    Check("package name: other packages", !Core_NamesClaudePackage(L"ClaudeHelper_1_x64__a") && !Core_NamesClaudePackage(L"MyClaude_1_x64__a") &&
                                          !Core_NamesClaudePackage(L"Claude") && !Core_NamesClaudePackage(NULL));

    Core_ShortcutFileName(L"Work", 1, out, ARRAYSIZE(out));
    CheckStr("shortcut file name", L"Claude (Work).lnk", out);
    Core_ShortcutFileName(L"A/B:C*", 2, out, ARRAYSIZE(out));
    CheckStr("shortcut file name, sanitized, second copy", L"Claude (A_B_C_) (2).lnk", out);
    Core_ShortcutFileName(L"a\tb", 1, out, ARRAYSIZE(out));
    CheckStr("shortcut file name, control character replaced", L"Claude (a_b).lnk", out);
    Core_ShortcutFileName(L"a\x007F" L"b\x0085" L"c\x009F", 1, out, ARRAYSIZE(out));
    CheckStr("shortcut file name, DEL and C1 controls replaced", L"Claude (a_b_c_).lnk", out);
    Core_ShortcutFileName(NULL, 1, out, ARRAYSIZE(out));
    CheckStr("shortcut file name without a label", L"Claude ().lnk", out);

    {
        SYSTEMTIME day;
        WCHAR longTitle[200];
        size_t i;
        ZeroMemory(&day, sizeof day);
        day.wYear = 2026;
        day.wMonth = 10;
        day.wDay = 5;
        Core_SessionsArchiveName(L"Fix the build", L"Claude sessions", &day, 1, out, ARRAYSIZE(out));
        CheckStr("sessions archive: one session, its title", L"Fix the build.zip", out);
        Core_SessionsArchiveName(L"a/b:c?", L"Claude sessions", &day, 2, out, ARRAYSIZE(out));
        CheckStr("sessions archive: characters a name cannot hold, second copy", L"a_b_c_ (2).zip", out);
        Core_SessionsArchiveName(L"  Notes. . ", L"Claude sessions", &day, 1, out, ARRAYSIZE(out));
        CheckStr("sessions archive: spaces around and dots after taken off", L"Notes.zip", out);
        Core_SessionsArchiveName(NULL, L"Claude sessions", &day, 1, out, ARRAYSIZE(out));
        CheckStr("sessions archive: several, named after the day", L"Claude sessions 2026-10-05.zip", out);
        Core_SessionsArchiveName(L" . ", L"Claude \x4F1A\x8BDD", &day, 3, out, ARRAYSIZE(out));
        CheckStr("sessions archive: a title of nothing, the name given, third copy", L"Claude \x4F1A\x8BDD 2026-10-05 (3).zip", out);
        Core_SessionsArchiveName(L"con", L"Claude sessions", &day, 1, out, ARRAYSIZE(out));
        CheckStr("sessions archive: a device name kept apart", L"con_.zip", out);
        Core_SessionsArchiveName(L"LPT1", L"Claude sessions", &day, 1, out, ARRAYSIZE(out));
        CheckStr("sessions archive: LPT1 too", L"LPT1_.zip", out);
        Core_SessionsArchiveName(L"COM10", L"Claude sessions", &day, 1, out, ARRAYSIZE(out));
        CheckStr("sessions archive: COM10 is a name", L"COM10.zip", out);
        for (i = 0; i < ARRAYSIZE(longTitle) - 1; i++) longTitle[i] = L'x';
        longTitle[ARRAYSIZE(longTitle) - 1] = 0;
        Core_SessionsArchiveName(longTitle, L"Claude sessions", &day, 1, out, ARRAYSIZE(out));
        Check("sessions archive: a long title cut to 80", wcslen(out) == 80 + 4 && Core_EndsWithI(out, L".zip"));
    }
}

static void TestMisc(void)
{
    WCHAR a[64], b[64];
    Check("path equals ignores case and trailing slash", Core_PathEquals(L"C:\\Users\\X\\Claude\\", L"c:\\users\\x\\claude"));
    Check("path equals is not a prefix test", !Core_PathEquals(L"C:\\Users\\X\\Claude", L"C:\\Users\\X\\Claude-Work"));
    {
        const WCHAR *current = L"C:\\Users\\X\\AppData\\Local\\M\\icons\\04CA42D6-3F258D42.ico";
        Check("a shortcut without an icon takes the profile's", Core_TakesCurrentIcon(L"", FALSE, current));
        Check("a shortcut showing the current icon keeps it",
              !Core_TakesCurrentIcon(L"c:\\users\\x\\appdata\\local\\m\\ICONS\\04ca42d6-3f258d42.ico", TRUE, current));
        Check("a shortcut whose icon file is gone takes the profile's", Core_TakesCurrentIcon(L"C:\\Users\\X\\Desktop\\69A9.ico", FALSE, current));
        Check("a shortcut showing another icon of ours takes the current one",
              Core_TakesCurrentIcon(L"C:\\Users\\X\\AppData\\Local\\M\\icons\\04CA42D6-11111111.ico", TRUE, current));
        Check("an icon the user chose stays while its file is there", !Core_TakesCurrentIcon(L"C:\\Users\\X\\Desktop\\69A9.ico", TRUE, current));
        Check("an icon in a folder below ours is not ours", !Core_TakesCurrentIcon(L"C:\\Users\\X\\AppData\\Local\\M\\icons\\old\\a.ico", TRUE, current));
    }
    {
        FILETIME time = Core_MsFileTime(1760000000123ULL);
        FILETIME before = { 0, 0 };
        Check("ms since 1970 to a FILETIME and back", Core_FileTimeMs(&time) == 1760000000123ULL && Core_FileTimeMs(&before) == 0);
    }
    {
        static const char kEntry[] = "{\"title\":\"Plan\",\"lastActivityAt\":1760000000123,\"isArchived\":true,\"n\":\"x\"}";
        WCHAR title[16];
        Check("member text, number and truth", Core_JsonMemberString(kEntry, sizeof kEntry - 1, "title", title, ARRAYSIZE(title)) &&
              wcscmp(title, L"Plan") == 0 && Core_JsonMemberNumber(kEntry, sizeof kEntry - 1, "lastActivityAt") == 1760000000123ULL &&
              Core_JsonMemberTrue(kEntry, sizeof kEntry - 1, "isArchived"));
        Check("member absent or of another kind", !Core_JsonMemberString(kEntry, sizeof kEntry - 1, "lastActivityAt", title, ARRAYSIZE(title)) &&
              title[0] == 0 && Core_JsonMemberNumber(kEntry, sizeof kEntry - 1, "n") == 0 && !Core_JsonMemberTrue(kEntry, sizeof kEntry - 1, "x"));
    }
    {
        char entry[] = "{\"sessionId\":\"local_1\",\"priorCliSessionIds\":[\"a\"],\"title\":\"T\",\"stagedTranscriptPath\":\"x\"}";
        size_t length = sizeof entry - 1;
        Check("a new session's entry leaves the original's other transcripts out",
              Core_EntryDropHistory(entry, &length) && length == strlen("{\"sessionId\":\"local_1\",\"title\":\"T\"}") &&
              memcmp(entry, "{\"sessionId\":\"local_1\",\"title\":\"T\"}", length) == 0);
    }
    Check("bit count", Core_BitCount(0) == 0 && Core_BitCount(0x80000001u) == 2 && Core_BitCount(~0u) == 32);
    Check("ends with", Core_EndsWithI(L"x\\APP\\claude.EXE", L"\\app\\Claude.exe"));
    Check("contains", Core_ContainsI(L"abc --User-Data-Dir=x", L"--user-data-dir"));
    Check("our exe by name", Core_IsOurExe(L"D:\\portable\\claudedesktopprofilesmanager.exe"));
    Check("not our exe", !Core_IsOurExe(L"C:\\x\\Claude.exe"));
    Check("hash is case-insensitive", Core_HashIgnoringCase(L"Claude-Work") == Core_HashIgnoringCase(L"CLAUDE-WORK"));
    Check("hash differs", Core_HashIgnoringCase(L"Claude-Work") != Core_HashIgnoringCase(L"Claude-Perso"));
    Check("hash never changes (it names stored files and IDs)", Core_HashIgnoringCase(L"Claude-Work") == 0xC8C85B73u &&
                                                                 Core_HashIgnoringCase(L"") == 2166136261u);
    Core_ProfileAumid(L"Claude-Work", a, ARRAYSIZE(a));
    Core_ProfileAumid(L"claude-work", b, ARRAYSIZE(b));
    Check("AUMID stable and without spaces", wcscmp(a, b) == 0 && !wcschr(a, L' ') &&
          wcsncmp(a, APP_AUMID_PREFIX L"Profile.", wcslen(APP_AUMID_PREFIX L"Profile.")) == 0);
    Check("AUMID text never changes (pins and shortcuts carry it)", wcscmp(a, L"ClaudeDesktopProfilesManager.Profile.C8C85B73") == 0);
    Check("hash ignores the case of accented letters too", Core_HashIgnoringCase(L"Claude-\x00E9t\x00E9") ==
                                                            Core_HashIgnoringCase(L"CLAUDE-\x00C9T\x00C9"));
    {
        WCHAR longText[513];
        wmemset(longText, L'a', 511);
        longText[511] = 0;
        Check("hash: 511 characters are hashed", Core_HashIgnoringCase(longText) != 2166136261u);
        longText[511] = L'a';
        longText[512] = 0;
        Check("hash: 512 characters or more hash as an empty text", Core_HashIgnoringCase(longText) == 2166136261u);
    }
    Check("text hash: the bytes and the terminator", Core_HashText(CORE_HASH_START, L"ab") ==
                                                     Core_HashBytes(CORE_HASH_START, L"ab", 3 * sizeof(WCHAR)));
    Check("text hash: texts one after another never run together",
          Core_HashText(Core_HashText(CORE_HASH_START, L"ab"), L"c") != Core_HashText(Core_HashText(CORE_HASH_START, L"a"), L"bc"));
    Check("text hash: case counts", Core_HashText(CORE_HASH_START, L"a") != Core_HashText(CORE_HASH_START, L"A"));
    Check("path length without trailing separators", Core_TrimmedPathLength(L"C:\\a\\/") == 4 && Core_TrimmedPathLength(L"C:\\") == 3);
    Check("equals ignoring case", Core_EqualsI(L"ProgId", L"PROGID") && !Core_EqualsI(L"a", L"ab") && !Core_EqualsI(NULL, L"a"));
    Check("path order: equal paths", Core_PathCompare(L"C:\\Build\\", L"c:\\build") == 0);
    Check("path order: an empty path first", Core_PathCompare(L"", L"C:\\a") < 0 && Core_PathCompare(L"C:\\a", L"") > 0 &&
                                             Core_PathCompare(L"", L"") == 0);
    Check("path order: by name", Core_PathCompare(L"C:\\a", L"C:\\b") < 0 && Core_PathCompare(L"C:\\b", L"C:\\a") > 0);
    Check("package cache path", Core_PackageCachePath(L"C:\\L", L"Claude_x", L"Roaming", L"Claude", a, ARRAYSIZE(a)) &&
                                wcscmp(a, L"C:\\L\\Packages\\Claude_x\\LocalCache\\Roaming\\Claude") == 0);
    Check("package cache path needs a package", !Core_PackageCachePath(L"C:\\L", L"", L"Local", L"Claude", a, ARRAYSIZE(a)));
}

static void TestPathsAndTimes(void)
{
    SYSTEMTIME st;
    FILETIME t0, t1, t2, t3;
    Check("file inside the folder", Core_PathUnder(L"C:\\Users\\X\\Desktop\\a.lnk", L"C:\\Users\\X\\Desktop"));
    Check("folder with trailing slash", Core_PathUnder(L"c:\\users\\x\\desktop\\sub\\a.lnk", L"C:\\Users\\X\\Desktop\\"));
    Check("the folder itself", Core_PathUnder(L"C:\\Users\\X\\Desktop", L"C:\\Users\\X\\Desktop"));
    Check("a sibling with the same prefix is outside", !Core_PathUnder(L"C:\\Users\\X\\Desktop2\\a.lnk", L"C:\\Users\\X\\Desktop"));
    Check("another folder", !Core_PathUnder(L"D:\\a.lnk", L"C:\\Users\\X\\Desktop"));
    Check("a file under a drive root", Core_PathUnder(L"C:\\a.lnk", L"C:\\"));
    Check("a drive root is inside itself", Core_PathUnder(L"C:\\", L"C:\\"));
    Check("a forward slash separates folders too", Core_PathUnder(L"C:\\a/b", L"C:\\a") && !Core_PathUnder(L"C:\\ab/c", L"C:\\a"));
    Check("a different drive is outside a root", !Core_PathUnder(L"D:\\a.lnk", L"C:\\"));

    ZeroMemory(&st, sizeof st);
    st.wYear = 2026; st.wMonth = 9; st.wDay = 27; st.wHour = 8; st.wMinute = 15; st.wSecond = 40; st.wMilliseconds = 500;
    SystemTimeToFileTime(&st, &t0);
    st.wSecond = 41;
    SystemTimeToFileTime(&st, &t1);
    st.wSecond = 42;
    SystemTimeToFileTime(&st, &t2);
    st.wMinute = 16; st.wSecond = 40;
    SystemTimeToFileTime(&st, &t3);
    Check("same FAT time within a second", Core_SameFatTime(&t0, &t1));
    Check("2 seconds later is another FAT time", !Core_SameFatTime(&t0, &t2));
    Check("another minute", !Core_SameFatTime(&t0, &t3));
}

static void TestProfileFilePaths(void)
{
    Profile p;
    WCHAR out[MAX_PATH], physical[MAX_PATH], large[MAX_PATH];
    const WCHAR *logical = L"C:\\Users\\X\\AppData\\Roaming\\Claude";
    const WCHAR *storage = L"C:\\Users\\X\\AppData\\Local\\Packages\\Claude_test\\LocalCache\\Roaming\\Claude";
    ZeroMemory(&p, sizeof p);
    StringCchCopyW(p.dataDir, ARRAYSIZE(p.dataDir), logical);
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), storage);
    StringCchPrintfW(physical, ARRAYSIZE(physical), L"%s\\scratch-workspaces\\a\\b\\work", storage);
    Check("profile path: logical root resolves", Core_ProfileFilePath(&p, logical, out, ARRAYSIZE(out)) &&
          wcscmp(out, storage) == 0);
    Check("profile path: logical descendant resolves", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: case does not affect ownership", Core_ProfileFilePath(&p,
          L"c:\\users\\x\\appdata\\roaming\\claude\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: physical descendant stays physical", Core_ProfileFilePath(&p, physical, out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: an external project stays external", Core_ProfileFilePath(&p, L"D:\\Project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"D:\\Project") == 0);
    Check("profile path: similarly named sibling stays separate", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude-Work\\project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\Users\\X\\AppData\\Roaming\\Claude-Work\\project") == 0);
    StringCchCatW(p.dataDir, ARRAYSIZE(p.dataDir), L"\\");
    StringCchCatW(p.storageDir, ARRAYSIZE(p.storageDir), L"\\");
    Check("profile path: trailing root separators are bounded", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    StringCchCopyW(out, ARRAYSIZE(out), logical);
    Check("profile path: in-place logical root resolves", Core_ProfileFilePath(&p, out, out, ARRAYSIZE(out)) &&
          wcscmp(out, storage) == 0);
    StringCchCopyW(out, ARRAYSIZE(out), physical);
    Check("profile path: in-place physical path is preserved", Core_ProfileFilePath(&p, out, out, ARRAYSIZE(out)) &&
          wcscmp(out, physical) == 0);
    Check("profile path: a small output fails empty", !Core_ProfileFilePath(&p, logical, out, 8) && !out[0]);
    Check("profile path: a small external output fails empty", !Core_ProfileFilePath(&p, L"D:\\Project", out, 8) && !out[0]);
    wmemset(large, L'x', ARRAYSIZE(large) - 1);
    large[ARRAYSIZE(large) - 1] = 0;
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), large);
    Check("profile path: a long mapped path fails empty", !Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\project", out, ARRAYSIZE(out)) && !out[0]);
    p.storageDir[0] = 0;
    Check("profile path: unresolved storage refuses logical I/O", !Core_ProfileFilePath(&p, logical, out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: unresolved storage permits external I/O", Core_ProfileFilePath(&p, L"D:\\Project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"D:\\Project") == 0);
    Check("profile path: a missing input fails empty", !Core_ProfileFilePath(&p, NULL, out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: an empty input fails empty", !Core_ProfileFilePath(&p, L"", out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: a missing profile fails empty", !Core_ProfileFilePath(NULL, logical, out, ARRAYSIZE(out)) && !out[0]);
    Check("profile path: a missing output is refused", !Core_ProfileFilePath(&p, logical, NULL, ARRAYSIZE(out)));

    /* The stock folder Claude moved out of its package: it names it there now, and earlier entries name it as it was. */
    ZeroMemory(&p, sizeof p);
    StringCchCopyW(p.formerDir, ARRAYSIZE(p.formerDir), logical);
    StringCchCopyW(p.dataDir, ARRAYSIZE(p.dataDir), L"C:\\Users\\X\\AppData\\Local\\Claude-Data\\claude_test");
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), p.dataDir);
    Check("moved profile: holds its folder", Core_ProfileHoldsPath(&p, L"C:\\Users\\X\\AppData\\Local\\Claude-Data\\claude_test\\a"));
    Check("moved profile: holds its former folder", Core_ProfileHoldsPath(&p, L"C:\\Users\\X\\AppData\\Roaming\\Claude\\a"));
    Check("moved profile: not a sibling of its former folder", !Core_ProfileHoldsPath(&p, L"C:\\Users\\X\\AppData\\Roaming\\Claude-Work"));
    Check("moved profile: its own path stays", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Local\\Claude-Data\\claude_test\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\Users\\X\\AppData\\Local\\Claude-Data\\claude_test\\scratch-workspaces\\a\\b\\work") == 0);
    Check("moved profile: a former path leads into its folder now", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\scratch-workspaces\\a\\b\\work", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\Users\\X\\AppData\\Local\\Claude-Data\\claude_test\\scratch-workspaces\\a\\b\\work") == 0);
    p.formerDir[0] = 0;
    Check("unmoved profile: no former folder", !Core_ProfileHoldsPath(&p, L"C:\\Users\\X\\AppData\\Roaming\\Claude\\a"));

    Check("moved stock folder: Claude-Data, the family in lower case",
          Core_MovedStockDir(L"C:\\Users\\X\\AppData\\Local", L"Claude_pzs8sxrjxfjjc", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\Users\\X\\AppData\\Local\\Claude-Data\\claude_pzs8sxrjxfjjc") == 0);
    Check("moved stock folder: a trailing separator is not doubled",
          Core_MovedStockDir(L"C:\\L\\", L"AnthropicPBC.Claude_fnn82j28hfe8t", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\L\\Claude-Data\\anthropicpbc.claude_fnn82j28hfe8t") == 0);
    Check("moved stock folder: none for a network %LOCALAPPDATA%", !Core_MovedStockDir(L"\\\\server\\share", L"Claude_x", out, ARRAYSIZE(out)) && !out[0]);
    Check("moved stock folder: none without a package", !Core_MovedStockDir(L"C:\\L", L"", out, ARRAYSIZE(out)) &&
          !Core_MovedStockDir(L"C:\\L", NULL, out, ARRAYSIZE(out)) && !Core_MovedStockDir(NULL, L"Claude_x", out, ARRAYSIZE(out)));
    Check("moved stock folder: none for a family not of the form name_publisher",
          !Core_MovedStockDir(L"C:\\L", L"Claude", out, ARRAYSIZE(out)) && !Core_MovedStockDir(L"C:\\L", L"_x", out, ARRAYSIZE(out)) &&
          !Core_MovedStockDir(L"C:\\L", L"Claude_", out, ARRAYSIZE(out)) && !Core_MovedStockDir(L"C:\\L", L"a_b_c", out, ARRAYSIZE(out)) &&
          !Core_MovedStockDir(L"C:\\L", L"a_b.c", out, ARRAYSIZE(out)) && !Core_MovedStockDir(L"C:\\L", L"a\\b_c", out, ARRAYSIZE(out)));
    Check("moved stock folder: a small output fails", !Core_MovedStockDir(L"C:\\L", L"Claude_x", out, 8));
    Check("profile path: an empty output buffer is refused", !Core_ProfileFilePath(&p, logical, out, 0));
    StringCchCopyW(p.storageDir, ARRAYSIZE(p.storageDir), p.dataDir);
    Check("profile path: a physical profile preserves its path", Core_ProfileFilePath(&p,
          L"C:\\Users\\X\\AppData\\Roaming\\Claude\\project", out, ARRAYSIZE(out)) &&
          wcscmp(out, L"C:\\Users\\X\\AppData\\Roaming\\Claude\\project") == 0);
}

static BOOL MemberIs(const char *json, const char *key, const char *expect)
{
    const char *v = NULL;
    size_t n = 0;
    if (!Core_JsonMember(json, strlen(json), key, &v, &n)) return expect == NULL;
    return expect && n == strlen(expect) && memcmp(v, expect, n) == 0;
}

typedef struct Members {
    char key[4][32], value[4][64];
    int  count, stopAt;
} Members;

static BOOL SeeMember(void *context, const char *key, size_t keyLen, const char *value, size_t valueLen)
{
    Members *seen = (Members *)context;
    if (seen->stopAt && seen->count == seen->stopAt) return FALSE;
    if (seen->count < 4) {
        StringCchCopyNA(seen->key[seen->count], sizeof seen->key[0], key, keyLen);
        StringCchCopyNA(seen->value[seen->count], sizeof seen->value[0], value, valueLen);
    }
    seen->count++;
    return TRUE;
}

static void TestJsonAndVersions(void)
{
    const char *cfg = "\xEF\xBB\xBF{ \"preferences\": {\"menuBarEnabled\": true, \"x\": [1, \"]\"]},\n"
                      "  \"mcpServers\": {\"a\": {\"command\": \"c:\\\\t\\\"x.exe\", \"args\": []}}, \"n\": 12 }";
    DWORD a[4], b[4];

    Check("json: object member", MemberIs(cfg, "mcpServers", "{\"a\": {\"command\": \"c:\\\\t\\\"x.exe\", \"args\": []}}"));
    Check("json: nested object kept whole", MemberIs(cfg, "preferences", "{\"menuBarEnabled\": true, \"x\": [1, \"]\"]}"));
    Check("json: number", MemberIs(cfg, "n", "12"));
    Check("json: nested key is not a member", MemberIs(cfg, "menuBarEnabled", NULL));
    Check("json: missing key", MemberIs(cfg, "locale", NULL));
    Check("json: string with its quotes", MemberIs("{\"tag_name\":\"v1.2.3\",\"x\":1}", "tag_name", "\"v1.2.3\""));
    Check("json: not an object", MemberIs("[1]", "a", NULL));
    Check("json: truncated", MemberIs("{\"a\": {\"b\": 1", "a", NULL));

    {
        /* A session record as Claude Desktop writes it. */
        static const char rec[] = "{\"sessionId\":\"local_1\",\"title\":\"Revue du d\xC3\xA9ploiement\","
                                  "\"cwd\":\"C:\\\\Users\\\\Zo\\u00eb\\\\a \\\"b\\\"\",\"isStarred\":true,\"isArchived\":false,"
                                  "\"lastActivityAt\":1790639580461,\"emoji\":\"\\ud83d\\ude00\",\"bad\":\"\\x\"}";
        const char *v;
        size_t n;
        WCHAR s[64];
        ULONGLONG t;
        Check("json string: UTF-8 kept", Core_JsonMember(rec, strlen(rec), "title", &v, &n) &&
                                          Core_JsonString(v, n, s, ARRAYSIZE(s)) && wcscmp(s, L"Revue du d\x00E9ploiement") == 0);
        Check("json string: escapes", Core_JsonMember(rec, strlen(rec), "cwd", &v, &n) &&
                                      Core_JsonString(v, n, s, ARRAYSIZE(s)) && wcscmp(s, L"C:\\Users\\Zo\x00EB\\a \"b\"") == 0);
        Check("json string: surrogate pair", Core_JsonMember(rec, strlen(rec), "emoji", &v, &n) &&
                                             Core_JsonString(v, n, s, ARRAYSIZE(s)) && s[0] == 0xD83D && s[1] == 0xDE00 && s[2] == 0);
        Check("json string: bad escape refused", Core_JsonMember(rec, strlen(rec), "bad", &v, &n) && !Core_JsonString(v, n, s, ARRAYSIZE(s)));
        Check("json string: a lone surrogate escape becomes U+FFFD", Core_JsonString("\"a\\ud83db\"", 10, s, ARRAYSIZE(s)) &&
              s[0] == L'a' && s[1] == 0xFFFD && s[2] == L'b' && s[3] == 0);
        Check("json string: too small a buffer fails", Core_JsonMember(rec, strlen(rec), "title", &v, &n) && !Core_JsonString(v, n, s, 8));
        Check("json string: not a string", Core_JsonMember(rec, strlen(rec), "isStarred", &v, &n) && !Core_JsonString(v, n, s, ARRAYSIZE(s)));
        Check("json string: empty", Core_JsonString("\"\"", 2, s, ARRAYSIZE(s)) && s[0] == 0);
        Check("json string: incomplete escape refused", !Core_JsonString("\"\\\"", 3, s, ARRAYSIZE(s)));
        Check("json string: unescaped quote refused", !Core_JsonString("\"a\"b\"", 5, s, ARRAYSIZE(s)));
        Check("json string: unescaped control refused", !Core_JsonString("\"a\nb\"", 5, s, ARRAYSIZE(s)));
        Check("json string: malformed UTF-8 refused", !Core_JsonString("\"\xC3\"", 3, s, ARRAYSIZE(s)));
        Check("json string: direct UTF-8 exact buffer", Core_JsonString("\"\xC3\xA9\"", 4, s, 2) && s[0] == 0xE9 && s[1] == 0);
        Check("json string: direct surrogate pair", Core_JsonString("\"\xF0\x9F\x98\x80\"", 6, s, 3) &&
              s[0] == 0xD83D && s[1] == 0xDE00 && s[2] == 0);
        Check("json string: escaped and direct text agree", Core_JsonString("\"\xC3\xA9\\u00e9\"", 10, s, 3) &&
              wcscmp(s, L"\xE9\xE9") == 0);
        Check("json string: empty fits terminator only", Core_JsonString("\"\"", 2, s, 1) && s[0] == 0);
        Check("json string: nonempty needs space beyond terminator", !Core_JsonString("\"x\"", 3, s, 1) && s[0] == 0);
        {
            WCHAR tiny[2] = { 0x1234, 0x5678 };
            Check("json string: direct conversion preserves buffer boundary", !Core_JsonString("\"x\"", 3, tiny, 1) &&
                  tiny[0] == 0 && tiny[1] == 0x5678);
            tiny[0] = 0x1234;
            Check("json string: escaped conversion preserves buffer boundary", !Core_JsonString("\"\\u0078\"", 8, tiny, 1) &&
                  tiny[0] == 0 && tiny[1] == 0x5678);
        }
        Check("json number", Core_JsonMember(rec, strlen(rec), "lastActivityAt", &v, &n) && Core_JsonNumber(v, n, &t) && t == 1790639580461ULL);
        Check("json number: not a number", !Core_JsonNumber("12a", 3, &t) && !Core_JsonNumber("", 0, &t));
        Check("json number: no sign or fraction", !Core_JsonNumber("-1", 2, &t) && !Core_JsonNumber("1.5", 3, &t));
        Check("json number: largest unsigned value", Core_JsonNumber("18446744073709551615", 20, &t) && t == ~0ULL);
        Check("json number: unsigned overflow refused", !Core_JsonNumber("18446744073709551616", 20, &t));
        Check("json true", Core_JsonMember(rec, strlen(rec), "isStarred", &v, &n) && Core_JsonTrue(v, n));
        Check("json false", Core_JsonMember(rec, strlen(rec), "isArchived", &v, &n) && !Core_JsonTrue(v, n));
    }

    {
        static const char obj[] = " { \"a\\\"b\" : [1, {\"x\":\"}\"}] , \"local_1\":{\"k\":2},\"n\":null } ";
        Members seen;
        ZeroMemory(&seen, sizeof seen);
        Check("json members: each, in order, keys and values as written",
              Core_JsonEachMember(obj, strlen(obj), SeeMember, &seen) && seen.count == 3 &&
              strcmp(seen.key[0], "a\\\"b") == 0 && strcmp(seen.value[0], "[1, {\"x\":\"}\"}]") == 0 &&
              strcmp(seen.key[1], "local_1") == 0 && strcmp(seen.value[1], "{\"k\":2}") == 0 &&
              strcmp(seen.key[2], "n") == 0 && strcmp(seen.value[2], "null") == 0);
        ZeroMemory(&seen, sizeof seen);
        Check("json members: an empty object has none", Core_JsonEachMember("{ }", 3, SeeMember, &seen) && seen.count == 0);
        Check("json members: not an object, or cut short, refused",
              !Core_JsonEachMember("[1]", 3, SeeMember, &seen) && !Core_JsonEachMember("{\"a\":1", 6, SeeMember, &seen) &&
              !Core_JsonEachMember("{\"a\":1 \"b\":2}", 13, SeeMember, &seen));
        seen.stopAt = 1;
        seen.count = 0;
        Check("json members: stopped when told", !Core_JsonEachMember(obj, strlen(obj), SeeMember, &seen) && seen.count == 1);
        Check("json value: one whole value", Core_JsonIsValue(" [1,{\"a\":\"]\"}] ", 15) && Core_JsonIsValue("1.0954", 6) &&
              Core_JsonIsValue("\"en-US\"", 7) && Core_JsonIsValue("null", 4));
        Check("json value: plain text, two values or a cut one refused",
              !Core_JsonIsValue("en-US x", 7) && !Core_JsonIsValue("{\"a\":1} 2", 9) && !Core_JsonIsValue("{\"a\":1", 6) && !Core_JsonIsValue("  ", 2));
        {
            static const char *const kValid[] = { "{\"a\":[1,2.5e-3,-0,true,false,null,\"x\\u00e9\\n\\\"\"]}", "[]", "{}", " \"s\" ", "-0.5E+2",
                                                  "[ [ ] , { } ]", "{\"\":0}" };
            static const char *const kInvalid[] = { "{]", "[1,,2]", "{\"a\":}", "{\"a\" 1}", "[1,]", "{\"a\":1,}", "en-US", "\"\\x\"",
                                                    "\"a\tb\"", "01", "1.", ".5", "tru", "[truex]", "{1:2}", "\"\\u12g4\"", "[1 2]", "-" };
            char deep[2 * 300 + 1];
            size_t k;
            BOOL all = TRUE;
            for (k = 0; k < ARRAYSIZE(kValid); k++) all = all && Core_JsonIsValue(kValid[k], strlen(kValid[k]));
            Check("json value: every form JSON has", all);
            for (k = 0, all = TRUE; k < ARRAYSIZE(kInvalid); k++) all = all && !Core_JsonIsValue(kInvalid[k], strlen(kInvalid[k]));
            Check("json value: misplaced or missing parts, bare words, bad escapes and numbers are none", all);
            memset(deep, '[', 300);
            memset(deep + 300, ']', 300);
            deep[600] = 0;
            Check("json value: nesting past the limit is refused", !Core_JsonIsValue(deep, 600) && Core_JsonIsValue(deep + 100, 400));
        }
    }

    Check("version: v1.2.3", Core_ParseVersion(L"v1.2.3", a) && a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 0);
    Check("version: 2.1 and 2.1.0 are equal", Core_ParseVersion(L"2.1", a) && Core_ParseVersion(L"2.1.0", b) && Core_CompareVersions(a, b) == 0);
    Check("version: 1.10 is newer than 1.9", Core_ParseVersion(L"1.10", a) && Core_ParseVersion(L"1.9", b) && Core_CompareVersions(a, b) > 0);
    Check("version: 1.0.0 is older than 1.0.1", Core_ParseVersion(L"v1.0.0", a) && Core_ParseVersion(L"v1.0.1", b) && Core_CompareVersions(a, b) < 0);
    Check("version: suffix ignored", Core_ParseVersion(L"v3.4-beta", a) && a[0] == 3 && a[1] == 4);
    Check("version: not a version", !Core_ParseVersion(L"latest", a) && !Core_ParseVersion(NULL, a));
    Check("version: a number above 65535 refused", !Core_ParseVersion(L"1.65536", a));
    Check("version: four numbers at most", Core_ParseVersion(L"1.2.3.4.5", a) && a[0] == 1 && a[3] == 4);
    Check("version: this program's numbers and text agree",
          Core_ParseVersion(APP_VERSION_WSTR, a) && a[0] == APP_VERSION_MAJOR && a[1] == APP_VERSION_MINOR &&
          a[2] == APP_VERSION_PATCH && a[3] == 0);
}

static BOOL EntryKindIs(const char *json, SessionEntryKind expected)
{
    return Core_SessionEntryKind(json, strlen(json)) == expected;
}

static void TestSessionEntries(void)
{
    Check("entry: a session of this PC", EntryKindIs("{\"sessionId\":\"local_1\",\"cwd\":\"C:\\\\x\",\"sshConfig\":null}", ENTRY_LOCAL));
    Check("entry: over SSH", EntryKindIs("{\"sessionId\":\"local_2\",\"sshConfig\":{\"host\":\"box\"}}", ENTRY_ELSEWHERE));
    Check("entry: in WSL", EntryKindIs("{\"sessionId\":\"local_3\",\"wslConfig\":{\"distro\":\"Ubuntu\"}}", ENTRY_ELSEWHERE));
    Check("entry: in the cloud", EntryKindIs("{\"sessionId\":\"local_4\",\"cloudSessionId\":\"session_01\"}", ENTRY_ELSEWHERE));
    Check("entry: moved to the cloud", EntryKindIs("{\"sessionId\":\"local_5\",\"movedToCloud\":true}", ENTRY_ELSEWHERE));
    Check("entry: not moved", EntryKindIs("{\"sessionId\":\"local_6\",\"movedToCloud\":false,\"cloudSessionId\":null}", ENTRY_LOCAL));
    Check("entry: moved to the cloud, as an object", EntryKindIs("{\"sessionId\":\"local_8\",\"movedToCloud\":{\"cloudSessionId\":\"session_02\"}}",
                                                                 ENTRY_ELSEWHERE));
    Check("entry: moved to nowhere", EntryKindIs("{\"sessionId\":\"local_9\",\"movedToCloud\":null}", ENTRY_LOCAL));
    Check("entry: a nested sshConfig is not the entry's",
          EntryKindIs("{\"sessionId\":\"local_7\",\"meta\":{\"sshConfig\":{\"host\":\"box\"}}}", ENTRY_LOCAL));
    Check("entry: no sessionId", EntryKindIs("{\"title\":\"x\"}", ENTRY_NOT_SESSION));
    Check("entry: sessionId not a string", EntryKindIs("{\"sessionId\":12}", ENTRY_NOT_SESSION));
    Check("entry: empty sessionId", EntryKindIs("{\"sessionId\":\"\"}", ENTRY_NOT_SESSION));
    Check("entry: an interrupted write is not a session",
          EntryKindIs("{\"sessionId\":\"local_1\",\"cliSessionId\":\"unfinished", ENTRY_NOT_SESSION) &&
          EntryKindIs("{\"sessionId\":\"local_1\"", ENTRY_NOT_SESSION));
    Check("entry: a byte order mark is accepted", EntryKindIs("\xef\xbb\xbf{\"sessionId\":\"local_1\"}", ENTRY_LOCAL));
    Check("entry: not JSON", EntryKindIs("local_1", ENTRY_NOT_SESSION) && Core_SessionEntryKind(NULL, 0) == ENTRY_NOT_SESSION);
}

static BOOL SetMemberGives(const char *json, const char *key, const char *raw, const char *expected)
{
    char out[256];
    size_t n = 0;
    BOOL ok = Core_JsonSetMember(json, strlen(json), key, raw, out, sizeof out, &n);
    if (!expected) return !ok;
    return ok && n == strlen(expected) && memcmp(out, expected, n) == 0;
}

static BOOL QuotesAs(const WCHAR *s, const char *expected)
{
    char out[64];
    return Core_JsonQuote(s, out, sizeof out) && strcmp(out, expected) == 0;
}

/* Runs `in` through Core_ReplaceChunk cut in two at `cut`, as a file read in
 * two chunks, and compares with `expected`. */
static BOOL ReplaceCut(const char *in, size_t cut, const CoreSwap *swaps, int count, const char *expected)
{
    char buf[256], out[512];
    size_t len = strlen(in), held, used = 0, n = 0;
    memcpy(buf, in, cut);
    n += Core_ReplaceChunk(buf, cut, swaps, count, FALSE, out + n, &used);
    held = cut - used;
    memmove(buf, buf + used, held);
    memcpy(buf + held, in + cut, len - cut);
    n += Core_ReplaceChunk(buf, held + len - cut, swaps, count, TRUE, out + n, &used);
    return n == strlen(expected) && memcmp(out, expected, n) == 0;
}

/* Transcript lines as the preview shows them. */
/* CHANGELOG.md's form (tools/changelog.py) as the program shows it. */
static void TestChangelogRead(void)
{
    static const char kMarkdown[] =
        "\xEF\xBB\xBF# Changelog\r\n\r\n> The program in a line.\r\n\r\nWhat changed.\r\n\r\n"
        "## v2026.10.12\r\n\r\nA **summary** of the day.\r\n\r\n### New features\r\n\r\n"
        "1. **Bold**: an `item` with [a link](https://example.com/x) and **Sync settings\xE2\x80\xA6**\r\n"
        "2. Escaped \\_name\\_, \\*star\\* and \\[brackets\\]\r\n\r\n"
        "### Bug fixes\r\n\r\n1. Fixed one\r\n\r\n"
        "## v2026.10.11\r\n\r\nAnother day.\r\n\r\n### Improvements\r\n\r\n7. One\r\n";
    static const WCHAR kFirst[] =
        L"A **summary** of the day.\n\n### New features\n"
        L"1. **Bold**: an `item` with [a link](https://example.com/x) and **Sync settings\x2026**\n2. Escaped \\_name\\_, \\*star\\* and \\[brackets\\]\n\n"
        L"### Bug fixes\n1. Fixed one";
    static const char kChinese[] = "## v2026.09.30\n\n\xE4\xB8\x80\xE5\xA4\xA9\n\n### \xE6\x96\xB0\xE5\xA2\x9E\n\n\xE4\xB8\x80\n\xE4\xBA\x8C\n";
    ChangelogVersion versions[4];
    WCHAR text[1024], shown[512];
    int count = Core_ChangelogRead(kMarkdown, sizeof kMarkdown - 1, versions, ARRAYSIZE(versions), text, ARRAYSIZE(text));
    Check("changelog: its versions, newest first", count == 2 && wcscmp(versions[0].name, L"v2026.10.12") == 0 && wcscmp(versions[1].name, L"v2026.10.11") == 0);
    if (count == 2) {
        StringCchCopyNW(shown, ARRAYSIZE(shown), text + versions[0].from, versions[0].to - versions[0].from);
        Check("changelog: a version's summary, then its sections, their lines numbered in each, in Markdown", wcscmp(shown, kFirst) == 0);
        if (wcscmp(shown, kFirst) != 0) wprintf(L"        got: %ls\n", shown);
        StringCchCopyNW(shown, ARRAYSIZE(shown), text + versions[1].from, versions[1].to - versions[1].from);
        Check("changelog: each section numbered from 1, whatever the file says", wcscmp(shown, L"Another day.\n\n### Improvements\n1. One") == 0);
    }
    count = Core_ChangelogRead(kChinese, sizeof kChinese - 1, versions, ARRAYSIZE(versions), text, ARRAYSIZE(text));
    Check("changelog: Chinese, LF line ends, lines numbered that the file did not", count == 1 &&
          versions[0].to - versions[0].from == 20 && wcsncmp(text + versions[0].from, L"\x4E00\x5929\n\n### \x65B0\x589E\n1. \x4E00\n2. \x4E8C", 20) == 0);
    Check("changelog: at most the versions asked for", Core_ChangelogRead(kMarkdown, sizeof kMarkdown - 1, versions, 1, text, ARRAYSIZE(text)) == 1);
    Check("changelog: what does not fit is nothing", Core_ChangelogRead(kMarkdown, sizeof kMarkdown - 1, versions, ARRAYSIZE(versions), text, 20) == 0 && text[0] == 0);
    Check("changelog: no version is nothing", Core_ChangelogRead("# Changelog\n\nText\n", 19, versions, ARRAYSIZE(versions), text, ARRAYSIZE(text)) == 0);
    Check("changelog: no text is nothing", Core_ChangelogRead(NULL, 0, versions, ARRAYSIZE(versions), text, ARRAYSIZE(text)) == 0 &&
          Core_ChangelogRead("", 0, versions, ARRAYSIZE(versions), text, ARRAYSIZE(text)) == 0);
}

/* Markdown as the theme shows it: the fonts and colors given, headings bold
 * and larger, numbered lines indented past their number, a quote muted, and
 * within them bold, code, links and escapes; RTF's own characters escaped,
 * the rest of Unicode as \uN. */
static void TestMarkdownRtf(void)
{
    static const WCHAR kMarkdown[] =
        L"## v2026.10.12\n\nA \\*plain\\* **bold** `c{o}de` [link](https://example.com) \x4E2D\\\\\n\n"
        L"### New features\n1. One\n10. Ten\n\n> A quote\n";
    CoreRtfStyle style;
    char rtf[2048];
    size_t written;
    ZeroMemory(&style, sizeof style);
    style.face = L"Segoe UI";
    style.codeFace = L"Consolas";
    style.size = 18;
    style.indent = 400;
    style.text = RGB(1, 2, 3);
    style.muted = RGB(4, 5, 6);
    style.link = RGB(7, 8, 9);
    style.codeBack = RGB(10, 11, 12);
    written = Core_MarkdownRtf(kMarkdown, &style, rtf, ARRAYSIZE(rtf));
    Check("markdown: RTF in the fonts and colors given",
          written == strlen(rtf) && strncmp(rtf, "{\\rtf1\\ansi\\deff0\\uc1{\\fonttbl{\\f0\\fnil Segoe UI;}{\\f1\\fmodern Consolas;}}", 70) == 0 &&
          strstr(rtf, "{\\colortbl ;\\red1\\green2\\blue3;\\red4\\green5\\blue6;\\red7\\green8\\blue9;\\red10\\green11\\blue12;}\\f0\\fs18\\cf1") != NULL &&
          rtf[written - 1] == '}');
    Check("markdown: the first heading bold and larger, with no space above", strstr(rtf, "\\pard\\sb0\\sa120\\b\\fs23 v2026.10.12\\b0\\fs18\\par") != NULL);
    Check("markdown: a later heading with space above", strstr(rtf, "\\pard\\sb220\\sa60\\b\\fs20 New features\\b0\\fs18\\par") != NULL);
    Check("markdown: a paragraph's bold, code and link; escapes gone, RTF's own characters escaped, the rest as \\uN",
          strstr(rtf, "\\pard\\sa120 A *plain* {\\b bold} {\\f1\\highlight4 c\\{o\\}de} {\\cf3\\ul link} \\u20013?\\\\\\par") != NULL);
    Check("markdown: numbered lines indented past their number",
          strstr(rtf, "\\pard\\fi-400\\li400\\tx400\\sa40 1.\\tab One\\par") != NULL && strstr(rtf, "10.\\tab Ten\\par") != NULL);
    Check("markdown: a quote in the muted color", strstr(rtf, "\\pard\\li400\\sa120\\cf2 A quote\\cf1\\par") != NULL);
    if (!strstr(rtf, "\\pard\\sa120 A *plain*")) printf("        got: %s\n", rtf);
    Check("markdown: what does not fit is nothing", Core_MarkdownRtf(kMarkdown, &style, rtf, 64) == 0 && rtf[0] == 0);
    Check("markdown: no text, no style, nothing", Core_MarkdownRtf(NULL, &style, rtf, ARRAYSIZE(rtf)) == 0 && Core_MarkdownRtf(kMarkdown, NULL, rtf, ARRAYSIZE(rtf)) == 0);
}

/* Base64, and a secret sealed behind a password: opened by it alone, its
 * text changed fails, the salt and nonce new each time. */
static void TestSealedSecret(void)
{
    static const BYTE kSecret[32] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32 };
    char text[64], sealed[1024], other[1024];
    BYTE bytes[64], opened[64];
    size_t length = 0;
    Check("base64: encoded", Core_Base64Encode((const BYTE *)"foobar", 6, text, sizeof text) == 8 && strcmp(text, "Zm9vYmFy") == 0);
    Check("base64: padded", Core_Base64Encode((const BYTE *)"fo", 2, text, sizeof text) == 4 && strcmp(text, "Zm8=") == 0 &&
          Core_Base64Encode((const BYTE *)"f", 1, text, sizeof text) == 4 && strcmp(text, "Zg==") == 0);
    Check("base64: decoded, padding or not", Core_Base64Decode("Zm9vYmFy", 8, bytes, sizeof bytes) == 6 && memcmp(bytes, "foobar", 6) == 0 &&
          Core_Base64Decode("Zm8=", 4, bytes, sizeof bytes) == 2 && memcmp(bytes, "fo", 2) == 0 && Core_Base64Decode("Zg", 2, bytes, sizeof bytes) == 1);
    Check("base64: anything else refused", Core_Base64Decode("Zm9v!mFy", 8, bytes, sizeof bytes) == (size_t)-1 &&
          Core_Base64Decode("Zm8=Zm8=", 8, bytes, sizeof bytes) == (size_t)-1 && Core_Base64Decode("Zm9vYmFy", 8, bytes, 3) == (size_t)-1);
    Check("base64: what does not fit is nothing", Core_Base64Encode((const BYTE *)"foobar", 6, text, 8) == 0 && text[0] == 0);
    Check("sealed: written", Core_SealSecret(kSecret, sizeof kSecret, L"correct horse \x4E2D", 1000, sealed, sizeof sealed) &&
          strstr(sealed, "\"kdf\":\"pbkdf2-sha256\"") && strstr(sealed, "\"iterations\":1000") && strstr(sealed, "\"cipher\":\"aes-256-gcm\""));
    Check("sealed: opened by its password", Core_OpenSecret(sealed, strlen(sealed), L"correct horse \x4E2D", opened, sizeof opened, &length) &&
          length == sizeof kSecret && memcmp(opened, kSecret, sizeof kSecret) == 0);
    Check("sealed: not by another", !Core_OpenSecret(sealed, strlen(sealed), L"correct horse", opened, sizeof opened, &length) && length == 0);
    {
        char *at = strstr(sealed, "\"sealed\":\"") + 10;
        *at = *at == 'A' ? 'B' : 'A';   /* one character of what is sealed, changed */
        Check("sealed: its text changed, it does not open", !Core_OpenSecret(sealed, strlen(sealed), L"correct horse \x4E2D", opened, sizeof opened, &length));
    }
    Check("sealed: a new salt and nonce each time",
          Core_SealSecret(kSecret, sizeof kSecret, L"pw", 1000, sealed, sizeof sealed) && Core_SealSecret(kSecret, sizeof kSecret, L"pw", 1000, other, sizeof other) &&
          strcmp(sealed, other) != 0);
    Check("sealed: no password, nothing", !Core_SealSecret(kSecret, sizeof kSecret, L"", 1000, sealed, sizeof sealed) && sealed[0] == 0);
    {
        /* A changed file asking for endless work is refused before any of it is done. */
        ULONGLONG started = GetTickCount64();
        char *at;
        Core_SealSecret(kSecret, sizeof kSecret, L"pw", 1000, sealed, sizeof sealed);
        at = strstr(sealed, "\"iterations\":1000");
        StringCchPrintfA(other, sizeof other, "%.*s\"iterations\":4000000000%s", (int)(at - sealed), sealed, at + strlen("\"iterations\":1000"));
        Check("sealed: too many iterations asked, it does not open, at once",
              !Core_OpenSecret(other, strlen(other), L"pw", opened, sizeof opened, &length) && GetTickCount64() - started < 2000);
    }
}

static void TestTranscriptLines(void)
{
    WCHAR out[256], small[12];
    static const char user[] = "{\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":\"  Fix the build\\nplease \"}}";
    static const char claude[] = "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"thinking\",\"thinking\":\"hm\"},"
                                 "{\"type\":\"text\",\"text\":\"On it.\"},{\"type\":\"tool_use\",\"name\":\"Bash\","
                                 "\"input\":{\"command\":\"build.cmd\",\"description\":\"Build it\"}},"
                                 "{\"type\":\"tool_use\",\"name\":\"TodoWrite\",\"input\":{\"todos\":[]}}]}}";
    static const char result[] = "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"ok\"}]}}";
    static const char command[] = "{\"type\":\"user\",\"message\":{\"content\":\"<command-name>/clear</command-name>\"}}";
    static const char meta[] = "{\"type\":\"user\",\"isMeta\":true,\"message\":{\"content\":\"Caveat\"}}";
    static const char side[] = "{\"type\":\"assistant\",\"isSidechain\":true,\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"x\"}]}}";
    static const char summary[] = "{\"type\":\"summary\",\"summary\":\"A title\"}";
    Check("transcript: a user's text, trimmed, its line break as CRLF",
          Core_TranscriptLine(user, strlen(user), out, ARRAYSIZE(out)) == TRANSCRIPT_USER && wcscmp(out, L"Fix the build\r\nplease") == 0);
    Check("transcript: Claude's text and tool calls, thinking left out",
          Core_TranscriptLine(claude, strlen(claude), out, ARRAYSIZE(out)) == TRANSCRIPT_CLAUDE &&
          wcscmp(out, L"On it.\r\n[Bash] Build it\r\n[TodoWrite]") == 0);
    Check("transcript: a tool result shows nothing", Core_TranscriptLine(result, strlen(result), out, ARRAYSIZE(out)) == TRANSCRIPT_NONE);
    Check("transcript: a command's plumbing shows nothing", Core_TranscriptLine(command, strlen(command), out, ARRAYSIZE(out)) == TRANSCRIPT_NONE);
    Check("transcript: a meta line shows nothing", Core_TranscriptLine(meta, strlen(meta), out, ARRAYSIZE(out)) == TRANSCRIPT_NONE);
    Check("transcript: a side chain shows nothing", Core_TranscriptLine(side, strlen(side), out, ARRAYSIZE(out)) == TRANSCRIPT_NONE);
    Check("transcript: other lines show nothing", Core_TranscriptLine(summary, strlen(summary), out, ARRAYSIZE(out)) == TRANSCRIPT_NONE &&
          Core_TranscriptLine("not json", 8, out, ARRAYSIZE(out)) == TRANSCRIPT_NONE);
    Check("transcript: a long text is cut with an ellipsis",
          Core_TranscriptLine(user, strlen(user), small, ARRAYSIZE(small)) == TRANSCRIPT_USER && wcslen(small) < ARRAYSIZE(small) &&
          small[wcslen(small) - 1] == 0x2026 && wcsncmp(small, L"Fix the", 7) == 0);
}

static void TestSessionEdits(void)
{
    static const CoreSwap swaps[] = {
        { "\"sessionId\":\"a1\"", "\"sessionId\":\"b2\"" },
        { "\"cwd\":\"C:\\\\old\"", "\"cwd\":\"D:\\\\newer\"" },
    };
    const char *line = "{\"sessionId\":\"a1\",\"cwd\":\"C:\\\\old\",\"x\":\"\\\"sessionId\\\":\\\"a1\\\"\"}\n{\"sessionId\":\"a1\"}";
    const char *swapped = "{\"sessionId\":\"b2\",\"cwd\":\"D:\\\\newer\",\"x\":\"\\\"sessionId\\\":\\\"a1\\\"\"}\n{\"sessionId\":\"b2\"}";
    PendingEdit e, back;
    WCHAR text[400], name[64];
    SYSTEMTIME day;
    size_t cut;
    BOOL allCuts = TRUE;

    Check("json set: a member is replaced in place", SetMemberGives("{\"title\":\"a\",\"n\":1}", "title", "\"b\"", "{\"title\":\"b\",\"n\":1}"));
    Check("json set: a missing member is added last", SetMemberGives("{\"n\":1}\n", "isStarred", "true", "{\"n\":1,\"isStarred\":true}\n"));
    Check("json set: into an empty object", SetMemberGives("{ }", "k", "1", "{\"k\":1 }"));
    Check("json set: a nested member of that name is not the one",
          SetMemberGives("{\"x\":{\"title\":\"n\"}}", "title", "\"t\"", "{\"x\":{\"title\":\"n\"},\"title\":\"t\"}"));
    Check("json set: the BOM stays", SetMemberGives("\xEF\xBB\xBF{\"a\":0}", "a", "2", "\xEF\xBB\xBF{\"a\":2}"));
    Check("json set: not an object", SetMemberGives("[1]", "a", "1", NULL) && SetMemberGives("", "a", "1", NULL));
    {
        char small[8];
        size_t n;
        Check("json set: too small a buffer fails", !Core_JsonSetMember("{\"a\":1}", 7, "title", "\"long\"", small, sizeof small, &n));
    }

    Check("json quote: plain", QuotesAs(L"Trading", "\"Trading\""));
    Check("json quote: quotes and backslashes", QuotesAs(L"C:\\a \"b\"", "\"C:\\\\a \\\"b\\\"\""));
    Check("json quote: a control character", QuotesAs(L"a\nb", "\"a\\u000ab\""));
    Check("json quote: UTF-8", QuotesAs(L"red\x00E9marrage", "\"red\xC3\xA9marrage\""));
    Check("json quote: a surrogate pair", QuotesAs(L"\xD83D\xDE00", "\"\xF0\x9F\x98\x80\""));
    Check("json quote: a lone surrogate becomes U+FFFD", QuotesAs(L"a\xD800", "\"a\xEF\xBF\xBD\""));
    {
        char small[7];
        Check("json quote: too small a buffer fails", !Core_JsonQuote(L"abcdef", small, 4));
        Check("json quote: a character of four bytes and the terminator fit exactly", Core_JsonQuote(L"\xD83D\xDE00", small, 7));
        Check("json quote: a character of four bytes needs room for all of them", !Core_JsonQuote(L"\xD83D\xDE00", small, 6));
    }

    Check("project folder: as Claude Code names it", Core_ProjectDirName(L"C:\\Users\\Zo\x00EB Martin\\Desktop\\my-project", name, ARRAYSIZE(name)) &&
                                                     wcscmp(name, L"C--Users-Zo--Martin-Desktop-my-project") == 0);
    {
        WCHAR longPath[CORE_PROJECT_NAME_MAX + 2], big[CORE_PROJECT_NAME_MAX + 8];
        wmemset(longPath, L'a', CORE_PROJECT_NAME_MAX);
        longPath[CORE_PROJECT_NAME_MAX] = 0;
        Check("project folder: 200 characters are kept", Core_ProjectDirName(longPath, big, ARRAYSIZE(big)) && wcslen(big) == CORE_PROJECT_NAME_MAX);
        longPath[CORE_PROJECT_NAME_MAX] = L'a';
        longPath[CORE_PROJECT_NAME_MAX + 1] = 0;
        Check("project folder: longer ones get a hash we do not make", !Core_ProjectDirName(longPath, big, ARRAYSIZE(big)));
        Check("project folder: none for an empty folder", !Core_ProjectDirName(L"", big, ARRAYSIZE(big)));
        Check("project folder: a long working folder fits its own buffer",
              Core_ProjectDirName(L"C:\\Users\\Zo\x00EB Martin\\AppData\\Roaming\\Claude-Work\\scratch-workspaces\\"
                                  L"0f0f0f0f-0f0f-4f0f-8f0f-0f0f0f0f0f0f\\1e1e1e1e-1e1e-4e1e-8e1e-1e1e1e1e1e1e\\scratch-2026-10-01-abcdef",
                                  big, ARRAYSIZE(big)));
    }
    {
        WCHAR scratchDir[MAX_PATH];
        Check("scratch area of an entries folder", Core_ScratchDirFor(L"C:\\R\\Claude-Work", L"C:\\S\\claude-code-sessions\\acc\\org",
                                                                      scratchDir, ARRAYSIZE(scratchDir)) &&
              wcscmp(scratchDir, L"C:\\R\\Claude-Work\\scratch-workspaces\\acc\\org") == 0);
        Check("scratch area needs an account and an organization", !Core_ScratchDirFor(L"C:\\R", L"org", scratchDir, ARRAYSIZE(scratchDir)) &&
              !Core_ScratchDirFor(L"C:\\R", L"C:\\S\\\\org", scratchDir, ARRAYSIZE(scratchDir)));
    }

    Check("session id: valid", Core_IsUuid(L"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d"));
    Check("session id: wrong length or character", !Core_IsUuid(L"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4") &&
                                                  !Core_IsUuid(L"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3g4d") &&
                                                  !Core_IsUuid(L"0a1b2c3d04e5f-4a6b-8c7d-9e0f1a2b3c4d") && !Core_IsUuid(NULL));
    Check("resume link", Core_ResumeLink(L"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d", text, ARRAYSIZE(text)) &&
                         wcscmp(text, L"claude://resume?session=0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d") == 0);
    Check("resume link: never with something else", !Core_ResumeLink(L"x&evil=1", text, ARRAYSIZE(text)));

    ZeroMemory(&day, sizeof day);
    day.wYear = 2026;
    day.wMonth = 9;
    day.wDay = 15;
    Core_ScratchName(&day, 0x1242EDE0, name, ARRAYSIZE(name));
    CheckStr("scratch folder: as Claude Desktop names them", L"scratch-2026-09-15-42ede0", name);

    for (cut = 0; cut <= strlen(line); cut++)
        if (!ReplaceCut(line, cut, swaps, 2, swapped)) {
            printf("        cut at %u\n", (unsigned)cut);
            allCuts = FALSE;
        }
    Check("replace: the same wherever the file is cut into chunks", allCuts);
    {
        /* Swaps whose `from` starts another one's: the first that matches wins, wherever the cut. */
        static const CoreSwap longerFirst[] = { { "abcdef", "X" }, { "abc", "Y" } };
        static const CoreSwap shorterFirst[] = { { "abc", "Y" }, { "abcdef", "X" } };
        static const struct { const CoreSwap *swaps; const char *in, *expected; } kPrefixCases[] = {
            { longerFirst, "abcdef", "X" },
            { longerFirst, "abcdeZ", "YdeZ" },
            { longerFirst, "abcabcdef", "YX" },
            { shorterFirst, "abcdef", "Ydef" },
        };
        size_t prefixCase;
        allCuts = TRUE;
        for (prefixCase = 0; prefixCase < ARRAYSIZE(kPrefixCases); prefixCase++)
            for (cut = 0; cut <= strlen(kPrefixCases[prefixCase].in); cut++)
                if (!ReplaceCut(kPrefixCases[prefixCase].in, cut, kPrefixCases[prefixCase].swaps, 2, kPrefixCases[prefixCase].expected)) {
                    printf("        %s cut at %u\n", kPrefixCases[prefixCase].in, (unsigned)cut);
                    allCuts = FALSE;
                }
        Check("replace: swaps sharing a start give the same result wherever the cut", allCuts);
    }
    Check("replace: nothing to swap", ReplaceCut("{\"x\":1}", 3, swaps, 2, "{\"x\":1}"));
    Check("replace: the start of a swap at the very end stays", ReplaceCut("ab\"sessionId\":\"a", 5, swaps, 1, "ab\"sessionId\":\"a"));

    ZeroMemory(&e, sizeof e);
    e.op = PENDING_TITLE;
    StringCchCopyW(e.key, ARRAYSIZE(e.key), L"0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d");
    StringCchCopyW(e.value, ARRAYSIZE(e.value), L"Weekly\treview\n");
    Check("pending: written", Core_PendingFormat(&e, text, ARRAYSIZE(text)) &&
                             wcscmp(text, L"title\t0a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d\tWeekly review ") == 0);
    Check("pending: read back", Core_PendingParse(text, &back) && back.op == PENDING_TITLE &&
                               wcscmp(back.key, e.key) == 0 && wcscmp(back.value, L"Weekly review ") == 0);
    Check("pending: a star", Core_PendingParse(L"star\tk\t1", &back) && back.op == PENDING_STAR && back.value[0] == L'1');
    Check("pending: a removal", Core_PendingParse(L"remove\tk\t", &back) && back.op == PENDING_REMOVE && back.value[0] == 0);
    Check("pending: a session turned local", Core_PendingParse(L"unremote\tk\t", &back) && back.op == PENDING_UNREMOTE && back.value[0] == 0);
    Check("pending: Claude's own title put back", Core_PendingParse(L"auto-title\tk\tPlan", &back) && back.op == PENDING_TITLE_AUTO &&
                                                  wcscmp(back.value, L"Plan") == 0);
    Check("pending: ... or none", Core_PendingParse(L"auto-title\tk\t", &back) && back.op == PENDING_TITLE_AUTO && back.value[0] == 0);
    Check("pending: an unknown change or a missing field is skipped",
          !Core_PendingParse(L"rename\tk\tx", &back) && !Core_PendingParse(L"title\tk", &back) && !Core_PendingParse(L"title\t\tx", &back));
    StringCchCopyW(e.key, ARRAYSIZE(e.key), L"a\tb");
    Check("pending: a key with a tab is refused", !Core_PendingFormat(&e, text, ARRAYSIZE(text)));
    StringCchCopyW(e.key, ARRAYSIZE(e.key), L"k");
    e.op = PENDING_STAR;
    StringCchCopyW(e.value, ARRAYSIZE(e.value), L"1");
    Check("pending: a star written", Core_PendingFormat(&e, text, ARRAYSIZE(text)) && wcscmp(text, L"star\tk\t1") == 0);
    e.op = PENDING_REMOVE;
    e.value[0] = 0;
    Check("pending: a removal written", Core_PendingFormat(&e, text, ARRAYSIZE(text)) && wcscmp(text, L"remove\tk\t") == 0);
    e.op = PENDING_TITLE_AUTO;
    Check("pending: Claude's own title written", Core_PendingFormat(&e, text, ARRAYSIZE(text)) && wcscmp(text, L"auto-title\tk\t") == 0);
    e.op = (PendingOp)7;
    Check("pending: an unknown change is not written", !Core_PendingFormat(&e, text, ARRAYSIZE(text)));
    {
        PendingEdit queued, added;
        ZeroMemory(&queued, sizeof queued);
        ZeroMemory(&added, sizeof added);
        StringCchCopyW(queued.key, ARRAYSIZE(queued.key), L"Session");
        StringCchCopyW(added.key, ARRAYSIZE(added.key), L"SESSION");
        queued.op = PENDING_TITLE;
        added.op = PENDING_TITLE;
        Check("pending: a newer title replaces the older one", Core_PendingReplaces(&queued, &added));
        added.op = PENDING_TITLE_AUTO;
        Check("pending: Claude's own title put back replaces a title", Core_PendingReplaces(&queued, &added) &&
                                                                       Core_PendingReplaces(&added, &queued));
        added.op = PENDING_STAR;
        Check("pending: a star leaves a title queued", !Core_PendingReplaces(&queued, &added));
        queued.op = PENDING_TITLE_AUTO;
        Check("pending: ... and Claude's own", !Core_PendingReplaces(&queued, &added));
        queued.op = PENDING_TITLE;
        added.op = PENDING_REMOVE;
        Check("pending: a removal replaces every change", Core_PendingReplaces(&queued, &added));
        queued.op = PENDING_REMOVE;
        added.op = PENDING_STAR;
        Check("pending: any change cancels a queued removal", Core_PendingReplaces(&queued, &added));
        StringCchCopyW(added.key, ARRAYSIZE(added.key), L"Other");
        Check("pending: another session's change replaces nothing", !Core_PendingReplaces(&queued, &added));
    }
}

/* `json` without member `key`, as Core_JsonRemoveMember gives it ("" when it fails). */
static BOOL RemovedIs(const char *json, const char *key, const char *expected)
{
    char out[256];
    size_t length;
    if (!Core_JsonRemoveMember(json, strlen(json), key, out, sizeof out, &length)) return expected == NULL;
    return expected && length == strlen(expected) && memcmp(out, expected, length) == 0;
}

static void TestSessionSync(void)
{
    static const WCHAR kId[] = L"0b6a3a3e-1111-4222-8333-944445555666";
    SyncOp op, back, queued, added;
    WCHAR text[256], path[MAX_PATH];
    char inPlace[64];
    size_t length;

    ZeroMemory(&op, sizeof op);
    op.kind = SYNC_PUT;
    op.flags = SYNC_UNDELETE | SYNC_REPLACE;
    op.time = 1790639580461ULL;
    op.seen = 1790639000000ULL;
    StringCchCopyW(op.key, ARRAYSIZE(op.key), kId);
    StringCchCopyW(op.content, ARRAYSIZE(op.content), L"3.json");
    Check("sync plan: a put written", Core_SyncOpFormat(&op, text, ARRAYSIZE(text)) &&
          wcscmp(text, L"put\t3\t1790639580461\t1790639000000\t0b6a3a3e-1111-4222-8333-944445555666\t3.json") == 0);
    Check("sync plan: read back", Core_SyncOpParse(text, &back) && back.kind == SYNC_PUT && back.flags == op.flags &&
          back.time == op.time && back.seen == op.seen && wcscmp(back.key, kId) == 0 && wcscmp(back.content, L"3.json") == 0);
    Check("sync plan: a mark, with nothing to write", Core_SyncOpParse(L"mark\t0\t5\t0\tk\t", &back) && back.kind == SYNC_MARK &&
          back.time == 5 && !back.content[0] && !back.tries);
    op.tries = 2;
    Check("sync plan: the tries of one that failed come after", Core_SyncOpFormat(&op, text, ARRAYSIZE(text)) &&
          wcscmp(text, L"put\t3\t1790639580461\t1790639000000\t0b6a3a3e-1111-4222-8333-944445555666\t3.json\t2") == 0 &&
          Core_SyncOpParse(text, &back) && back.tries == 2 && wcscmp(back.content, L"3.json") == 0);
    Check("sync plan: tries are a count past zero, the last field",
          !Core_SyncOpParse(L"put\t0\t1\t0\tk\t3.json\t0", &back) && !Core_SyncOpParse(L"put\t0\t1\t0\tk\t3.json\tx", &back) &&
          !Core_SyncOpParse(L"put\t0\t1\t0\tk\t3.json\t2\t", &back) && !Core_SyncOpParse(L"put\t0\t1\t0\tk\t3.json\t", &back));
    op.tries = 0;
    Check("sync plan: the largest numbers", Core_SyncOpParse(L"put\t4294967295\t18446744073709551615\t0\tk\t", &back) &&
          back.flags == MAXDWORD && back.time == _UI64_MAX);
    Check("sync plan: broken lines are refused",
          !Core_SyncOpParse(L"put\t0\t1\t0\tk", &back) && !Core_SyncOpParse(L"move\t0\t1\t0\tk\t", &back) &&
          !Core_SyncOpParse(L"put\t-1\t1\t0\tk\t", &back) && !Core_SyncOpParse(L"put\t0\t1\t0\t\t", &back) &&
          !Core_SyncOpParse(L"put\t\t1\t0\tk\t", &back) && !Core_SyncOpParse(L"put\t4294967296\t1\t0\tk\t", &back) &&
          !Core_SyncOpParse(L"put\t0\t18446744073709551616\t0\tk\t", &back));
    Check("sync plan: what is written is a file of ours, never a path",
          !Core_SyncOpParse(L"put\t0\t1\t0\tk\t..\\config.json", &back) && !Core_SyncOpParse(L"put\t0\t1\t0\tk\tC:\\x", &back) &&
          !Core_SyncOpParse(L"put\t0\t1\t0\tk\t.json", &back) && !Core_SyncOpParse(L"put\t0\t1\t0\tk\ta/b", &back));
    StringCchCopyW(op.content, ARRAYSIZE(op.content), L"a\\b");
    Check("sync plan: a path is not written", !Core_SyncOpFormat(&op, text, ARRAYSIZE(text)));
    StringCchCopyW(op.content, ARRAYSIZE(op.content), L"3.json");
    StringCchCopyW(op.key, ARRAYSIZE(op.key), L"a\tb");
    Check("sync plan: a key with a tab is not written", !Core_SyncOpFormat(&op, text, ARRAYSIZE(text)));
    op.kind = (SyncOpKind)9;
    StringCchCopyW(op.key, ARRAYSIZE(op.key), kId);
    Check("sync plan: an unknown change is not written", !Core_SyncOpFormat(&op, text, ARRAYSIZE(text)));

    ZeroMemory(&queued, sizeof queued);
    ZeroMemory(&added, sizeof added);
    StringCchCopyW(queued.key, ARRAYSIZE(queued.key), L"Session");
    StringCchCopyW(added.key, ARRAYSIZE(added.key), L"SESSION");
    queued.kind = SYNC_PUT;
    added.kind = SYNC_REMOVE;
    Check("sync plan: a removal replaces a put of the same session", Core_SyncOpReplaces(&queued, &added));
    queued.kind = SYNC_REMOVE;
    added.kind = SYNC_PUT;
    Check("sync plan: a put replaces a removal", Core_SyncOpReplaces(&queued, &added));
    added.kind = SYNC_MARK;
    Check("sync plan: a mark leaves the session's entry alone", !Core_SyncOpReplaces(&queued, &added));
    queued.kind = SYNC_UNMARK;
    Check("sync plan: a mark replaces its removal", Core_SyncOpReplaces(&queued, &added));
    queued.kind = added.kind = SYNC_INDEX;
    Check("sync plan: a list of archived sessions replaces the one before", Core_SyncOpReplaces(&queued, &added));
    queued.kind = SYNC_INDEX;
    added.kind = SYNC_LAYOUT;
    Check("sync plan: the sidebar's layout and the archived list are apart", !Core_SyncOpReplaces(&queued, &added));
    queued.kind = SYNC_LAYOUT;
    Check("sync plan: a layout replaces the one before", Core_SyncOpReplaces(&queued, &added));
    {
        DWORD all = 0, overlap = 0;
        int g;
        for (g = 0; g < SYNC_GROUP_COUNT; g++) {
            overlap |= all & Core_SyncGroup(g);
            all |= Core_SyncGroup(g);
        }
        Check("sync items: the groups part every item, none twice", all == SYNC_ITEMS_ALL && !overlap && !Core_SyncGroup(SYNC_GROUP_COUNT) &&
                                                                    !Core_SyncGroup(-1));
        Check("sync items: an earlier version's groups are their items whole",
              Core_SyncItemsOfGroups(0x01) == SYNC_ITEM_SESSIONS && Core_SyncItemsOfGroups(0x80) == SYNC_ITEM_PERMISSIONS &&
              Core_SyncItemsOfGroups(0x7F) == (SYNC_ITEMS_ALL & ~SYNC_ITEM_PERMISSIONS) && Core_SyncItemsOfGroups(0xFF) == SYNC_ITEMS_ALL &&
              !Core_SyncItemsOfGroups(0));
        Check("sync items: ... and back, a group with any of its items",
              Core_SyncGroupsOf(SYNC_ITEM_SESSIONS_TITLES | SYNC_ITEM_APPEARANCE_ZOOM) == 0x09 && Core_SyncGroupsOf(SYNC_ITEMS_ALL) == 0xFF &&
              Core_SyncGroupsOf(SYNC_ITEM_MODEL) == 0x20 && !Core_SyncGroupsOf(0));
    }
    added.flags = SYNC_MARKED;
    added.kind = SYNC_REMOVE;
    added.time = added.seen = 0;
    StringCchCopyW(added.key, ARRAYSIZE(added.key), kId);
    added.content[0] = 0;
    Check("sync plan: a layout and a marked removal written and read back",
          Core_SyncOpFormat(&added, text, ARRAYSIZE(text)) && Core_SyncOpParse(text, &back) && back.kind == SYNC_REMOVE &&
          back.flags == SYNC_MARKED && Core_SyncOpParse(L"layout\t0\t0\t0\tlayout\t2.json", &back) && back.kind == SYNC_LAYOUT &&
          wcscmp(back.content, L"2.json") == 0);
    StringCchCopyW(added.key, ARRAYSIZE(added.key), L"SESSION");
    queued.kind = added.kind = SYNC_PUT;
    StringCchCopyW(added.key, ARRAYSIZE(added.key), L"Other");
    Check("sync plan: another session's change replaces nothing", !Core_SyncOpReplaces(&queued, &added));

    Check("crc32: the check value", Core_Crc32(0, "123456789", 9) == 0xCBF43926u);
    Check("crc32: goes on piece by piece", Core_Crc32(Core_Crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    Check("crc32: nothing", Core_Crc32(0, "", 0) == 0);

    Check("archive name: files inside the folder",
          Core_ArchiveNameSafe("manifest.json", 13) && Core_ArchiveNameSafe("entries/1.json", 14) &&
          Core_ArchiveNameSafe("claude/projects/C--work/a b.jsonl", 33));
    Check("archive name: nothing outside it",
          !Core_ArchiveNameSafe("", 0) && !Core_ArchiveNameSafe("/x", 2) && !Core_ArchiveNameSafe("a/../../x", 9) &&
          !Core_ArchiveNameSafe("..", 2) && !Core_ArchiveNameSafe("./x", 3) && !Core_ArchiveNameSafe("a//b", 4) &&
          !Core_ArchiveNameSafe("a/", 2) && !Core_ArchiveNameSafe("a\\..\\x", 6) && !Core_ArchiveNameSafe("C:/x", 4));
    Check("archive name: nothing Windows would read otherwise",
          !Core_ArchiveNameSafe("a:stream", 8) && !Core_ArchiveNameSafe("a/b.", 4) && !Core_ArchiveNameSafe("a/b ", 4) &&
          !Core_ArchiveNameSafe("a\x01" "b", 3) && !Core_ArchiveNameSafe("a*b", 3) && !Core_ArchiveNameSafe("a?b", 3));
    Check("archive name: its length, not its terminator", !Core_ArchiveNameSafe("ab\0/../x", 8) && Core_ArchiveNameSafe("ab/../x", 2));
    Check("archive name: dots inside a name are a name", Core_ArchiveNameSafe("cowork/draft..v2.md", 19) &&
          Core_ArchiveNameSafe("a/..b/c", 7) && !Core_ArchiveNameSafe("a/b../c", 7));
    Check("archive name: no device, whatever follows its name",
          !Core_ArchiveNameSafe("CON", 3) && !Core_ArchiveNameSafe("a/nul.txt", 9) && !Core_ArchiveNameSafe("a/Com1.json", 11) &&
          !Core_ArchiveNameSafe("lpt9", 4) && !Core_ArchiveNameSafe("aux .x", 6) && !Core_ArchiveNameSafe("COM\xC2\xB9", 5) &&
          Core_ArchiveNameSafe("COM0", 4) && Core_ArchiveNameSafe("console.log", 11) && Core_ArchiveNameSafe("a/nulls", 7));

    StringCchPrintfW(path, ARRAYSIZE(path), L"projects\\C--work\\%s.jsonl", kId);
    Check("conversation file: a transcript", Core_ConversationFileName(path));
    StringCchPrintfW(path, ARRAYSIZE(path), L"projects\\C--work\\%s\\subagents\\agent-1.jsonl", kId);
    Check("conversation file: in its folder", Core_ConversationFileName(path));
    StringCchPrintfW(path, ARRAYSIZE(path), L"file-history\\%s\\abc@v2", kId);
    Check("conversation file: its file history", Core_ConversationFileName(path));
    StringCchPrintfW(path, ARRAYSIZE(path), L"TASKS\\%s\\1.json", kId);
    Check("conversation file: its tasks", Core_ConversationFileName(path));
    Check("conversation file: Claude Code's own files are not one",
          !Core_ConversationFileName(L"settings.json") && !Core_ConversationFileName(L"projects\\C--work\\memory\\MEMORY.md") &&
          !Core_ConversationFileName(L"projects\\C--work\\not-a-session-id-at-all-here-0000000.jsonl"));
    StringCchPrintfW(path, ARRAYSIZE(path), L"projects\\%s.jsonl", kId);
    Check("conversation file: a transcript outside a project's folder is not one", !Core_ConversationFileName(path));
    StringCchPrintfW(path, ARRAYSIZE(path), L"plugins\\%s\\hook.ps1", kId);
    Check("conversation file: another folder of Claude Code's is not one", !Core_ConversationFileName(path));
    StringCchPrintfW(path, ARRAYSIZE(path), L"projects\\C--work\\%s.json", kId);
    Check("conversation file: another extension is not one", !Core_ConversationFileName(path));
    StringCchPrintfW(path, ARRAYSIZE(path), L"file-history\\%s", kId);
    Check("conversation file: a store's folder alone is not one", !Core_ConversationFileName(path));

    Check("json remove: a member in the middle", RemovedIs("{\"a\":1,\"b\":2,\"c\":3}", "b", "{\"a\":1,\"c\":3}"));
    Check("json remove: the first member", RemovedIs("{ \"a\": 1, \"b\": [1,2] }", "a", "{ \"b\": [1,2] }"));
    Check("json remove: the last member", RemovedIs("{ \"a\": 1, \"b\": [1,2] }", "b", "{ \"a\": 1 }"));
    Check("json remove: the only member", RemovedIs("{\"a\":{\"b\":1}}", "a", "{}"));
    Check("json remove: a nested key is no member", RemovedIs("{\"a\":{\"b\":1}}", "b", "{\"a\":{\"b\":1}}"));
    Check("json remove: not an object", RemovedIs("[1]", "a", NULL));
    StringCchCopyA(inPlace, sizeof inPlace, "{\"x\":\"long value\",\"y\":true}");
    Check("json remove: in place", Core_JsonRemoveMember(inPlace, strlen(inPlace), "x", inPlace, sizeof inPlace, &length) &&
          length == 10 && memcmp(inPlace, "{\"y\":true}", 10) == 0);
    Check("json remove: too small", !Core_JsonRemoveMember("{\"a\":1}", 7, "b", inPlace, 3, &length));
}

static MirrorSide Side(MirrorState state, ULONGLONG hash, ULONGLONG time)
{
    MirrorSide side;
    side.state = state;
    side.hash = hash;
    side.time = time;
    return side;
}

static void TestMirror(void)
{
    MirrorSide sides[3], base = Side(MIRROR_LISTED, 7, 0), gone = Side(MIRROR_DELETED, 0, 0);
    WCHAR name[64];
    SYSTEMTIME day;

    sides[0] = Side(MIRROR_LISTED, 7, 10);
    sides[1] = Side(MIRROR_LISTED, 7, 20);
    Check("mirror: nothing changed, the profiles keep theirs", Core_MirrorResolve(sides, 2, &base) == 0);
    sides[1] = Side(MIRROR_LISTED, 9, 20);
    Check("mirror: the side changed since the last sync wins", Core_MirrorResolve(sides, 2, &base) == 1);
    sides[0] = Side(MIRROR_LISTED, 8, 30);
    Check("mirror: of two changed sides, the latest written wins", Core_MirrorResolve(sides, 2, &base) == 0);
    sides[0] = Side(MIRROR_MISSING, 0, 0);
    sides[1] = Side(MIRROR_MISSING, 0, 0);
    Check("mirror: profiles that lost it get the base's back", Core_MirrorResolve(sides, 2, &base) == 2);
    sides[1] = Side(MIRROR_LISTED, 7, 5);
    Check("mirror: ... or a profile's copy of it, the same", Core_MirrorResolve(sides, 2, &base) == 1);
    sides[0] = Side(MIRROR_DELETED, 0, 40);
    Check("mirror: deleted in one profile, it goes everywhere", Core_MirrorResolve(sides, 2, &base) == CORE_MIRROR_DELETED);
    sides[1] = Side(MIRROR_LISTED, 9, 50);
    Check("mirror: a change made after the deletion keeps it", Core_MirrorResolve(sides, 2, &base) == 1);
    sides[1] = Side(MIRROR_LISTED, 9, 30);
    Check("mirror: a deletion after the change takes it away", Core_MirrorResolve(sides, 2, &base) == CORE_MIRROR_DELETED);
    sides[0] = Side(MIRROR_REMOVED, 0, 0);
    sides[1] = Side(MIRROR_LISTED, 7, 5);
    Check("mirror: taken away by Claude in one profile, it goes everywhere", Core_MirrorResolve(sides, 2, &base) == CORE_MIRROR_DELETED);
    sides[1] = Side(MIRROR_LISTED, 9, 1);
    Check("mirror: ... unless another profile changed it", Core_MirrorResolve(sides, 2, &base) == 1);
    sides[0] = Side(MIRROR_LISTED, 3, 10);
    sides[1] = Side(MIRROR_MISSING, 0, 0);
    Check("mirror: a new session goes to the others", Core_MirrorResolve(sides, 2, NULL) == 0);
    sides[1] = Side(MIRROR_DELETED, 0, 20);
    Check("mirror: a new session deleted later elsewhere goes", Core_MirrorResolve(sides, 2, NULL) == CORE_MIRROR_DELETED);
    sides[0] = Side(MIRROR_MISSING, 0, 0);
    sides[1] = Side(MIRROR_MISSING, 0, 0);
    Check("mirror: nowhere at all", Core_MirrorResolve(sides, 2, NULL) == CORE_MIRROR_NOWHERE);
    Check("mirror: deleted before, it stays deleted", Core_MirrorResolve(sides, 2, &gone) == CORE_MIRROR_DELETED);
    sides[2] = Side(MIRROR_LISTED, 4, 1);
    Check("mirror: deleted before, listed again since, it comes back", Core_MirrorResolve(sides, 3, &gone) == 2);

    day.wYear = 2026;
    day.wMonth = 10;
    day.wDay = 7;
    Check("dated copy: the first of the day", Core_DatedCopyName(L".claude", &day, 1, name, ARRAYSIZE(name)) &&
          wcscmp(name, L".claude_20261007") == 0);
    Check("dated copy: the next ones", Core_DatedCopyName(L".claude", &day, 3, name, ARRAYSIZE(name)) &&
          wcscmp(name, L".claude_20261007_3") == 0);
    Check("dated copy: no name, no copy", !Core_DatedCopyName(L"", &day, 1, name, ARRAYSIZE(name)) &&
          !Core_DatedCopyName(L".claude", &day, 0, name, ARRAYSIZE(name)));

    {
        SYSTEMTIME found, none, today;
        BOOL weekly = TRUE;
        Check("weekly copy: its name", Core_WeeklyCopyName(L".claude", &day, name, ARRAYSIZE(name)) &&
              wcscmp(name, L".claude_auto_20261007") == 0);
        Check("copy day: a weekly copy", Core_CopyDay(L".claude", L".claude_auto_20261007", &found, &weekly) && weekly &&
              found.wYear == 2026 && found.wMonth == 10 && found.wDay == 7);
        Check("copy day: a dated copy", Core_CopyDay(L".claude", L".claude_20261007", &found, &weekly) && !weekly && found.wDay == 7);
        Check("copy day: the third dated copy of a day", Core_CopyDay(L".claude", L".claude_20261007_3", &found, &weekly) && !weekly);
        Check("copy day: not a copy", !Core_CopyDay(L".claude", L".claude", &found, &weekly) &&
              !Core_CopyDay(L".claude", L".claude_bak_20261006", &found, &weekly) &&
              !Core_CopyDay(L".claude", L".claude_20261307", &found, &weekly) &&
              !Core_CopyDay(L".claude", L".claude_auto_20261007_2", &found, &weekly) &&
              !Core_CopyDay(L".claude", L".claude_2026100", &found, &weekly) &&
              !Core_CopyDay(L".claude", L".claude_20261007_", &found, &weekly) &&
              !Core_CopyDay(L".claude", L".clauded_20261007", &found, &weekly));
        ZeroMemory(&none, sizeof none);
        today = day;
        today.wDay = 13;
        today.wHour = 23;
        Check("weekly copy: due with none", Core_WeeklyCopyDue(&none, &today));
        Check("weekly copy: not due after six days", !Core_WeeklyCopyDue(&day, &today));
        today.wDay = 14;
        today.wHour = 0;
        Check("weekly copy: due on the seventh day", Core_WeeklyCopyDue(&day, &today));
        today.wDay = 6;
        Check("weekly copy: a copy dated later than today is no reason", !Core_WeeklyCopyDue(&day, &today));
    }
}

/* A transcript line: `uuid` after `parent` ("" for none), written at second `second` of a day. */
static void TranscriptLine(char *text, size_t cap, const char *type, const char *uuid, const char *parent, int second, BOOL sidechain)
{
    char line[512];
    if (parent[0])
        StringCchPrintfA(line, sizeof line, "{\"type\":\"%s\",\"uuid\":\"%s\",\"parentUuid\":\"%s\",\"isSidechain\":%s,"
                         "\"timestamp\":\"2026-10-08T10:00:%02d.000Z\",\"sessionId\":\"s\"}\n", type, uuid, parent, sidechain ? "true" : "false", second);
    else
        StringCchPrintfA(line, sizeof line, "{\"type\":\"%s\",\"uuid\":\"%s\",\"parentUuid\":null,\"isSidechain\":false,"
                         "\"timestamp\":\"2026-10-08T10:00:%02d.000Z\",\"sessionId\":\"s\"}\n", type, uuid, second);
    StringCchCatA(text, cap, line);
}

/* 2026-10-08T10:00:<second>Z in ms since 1970. */
static ULONGLONG AtSecond(int second)
{
    SYSTEMTIME at;
    FILETIME time;
    ZeroMemory(&at, sizeof at);
    at.wYear = 2026;
    at.wMonth = 10;
    at.wDay = 8;
    at.wHour = 10;
    at.wSecond = (WORD)second;
    SystemTimeToFileTime(&at, &time);
    return ((((ULONGLONG)time.dwHighDateTime << 32) | time.dwLowDateTime) - 116444736000000000ULL) / 10000;
}

static size_t MapCodeIds(void *context, const char *text, size_t length, char *out, size_t cap)
{
    (void)context;
    if (length < 6 || memcmp(text, "local_", 6) != 0) return (size_t)-1;
    if (FAILED(StringCchPrintfA(out, cap, "cli:%.*s", (int)(length - 6), text + 6))) return (size_t)-1;
    return strlen(out);
}

static void TestSessionsKeptTheSame(void)
{
    char text[4096], iso[32];
    const char *keys[3] = { "preferences", "epitaxyPrefs", "starred-local-code-sessions" };
    char *set, *mapped;
    BOOL *excluded = NULL;
    WCHAR name[MAX_PATH];
    size_t length = 0;
    int lines = 0;

    Check("scratch: a folder in a profile's no-folder area",
          Core_ScratchFolderName(L"C:\\Users\\a\\AppData\\Roaming\\Claude\\scratch-workspaces\\acc\\org\\scratch-2026-10-07-8e3fb5", name,
                                 ARRAYSIZE(name)) && wcscmp(name, L"scratch-2026-10-07-8e3fb5") == 0);
    Check("scratch: a subfolder of it names the same folder",
          Core_ScratchFolderName(L"D:\\p\\Claude-Work\\SCRATCH-WORKSPACES\\a\\o\\scratch-x\\src", name, ARRAYSIZE(name)) &&
          wcscmp(name, L"scratch-x") == 0);
    Check("scratch: a project folder is none", !Core_ScratchFolderName(L"C:\\Users\\a\\Desktop\\work", name, ARRAYSIZE(name)) && !name[0]);
    Check("scratch: the area itself, or its account, is none",
          !Core_ScratchFolderName(L"C:\\d\\scratch-workspaces\\acc\\org", name, ARRAYSIZE(name)) &&
          !Core_ScratchFolderName(L"C:\\d\\scratch-workspaces\\acc", name, ARRAYSIZE(name)) &&
          !Core_ScratchFolderName(L"C:\\d\\scratch-workspaces\\acc\\org\\..", name, ARRAYSIZE(name)));

    set = Core_JsonSetNested("{\"a\":1}", 7, keys, 3, "[\"local_1\"]", &length);
    Check("json nested: the objects on the way are made",
          set && strcmp(set, "{\"a\":1,\"preferences\":{\"epitaxyPrefs\":{\"starred-local-code-sessions\":[\"local_1\"]}}}") == 0);
    if (set) HeapFree(GetProcessHeap(), 0, set);
    StringCchCopyA(text, sizeof text, "{\"preferences\":{\"x\":true,\"epitaxyPrefs\":{\"starred-local-code-sessions\":[],\"y\":2}},\"z\":3}");
    set = Core_JsonSetNested(text, strlen(text), keys, 3, "[\"local_2\"]", &length);
    Check("json nested: what is there is replaced, the rest kept",
          set && strcmp(set, "{\"preferences\":{\"x\":true,\"epitaxyPrefs\":{\"starred-local-code-sessions\":[\"local_2\"],\"y\":2}},\"z\":3}") == 0 &&
          length == strlen(set));
    if (set) HeapFree(GetProcessHeap(), 0, set);
    Check("json nested: no object on the way, nothing", Core_JsonSetNested("{\"preferences\":[1]}", 19, keys, 3, "1", &length) == NULL);
    {
        static const char *const env[] = { "env", "DISABLE_TELEMETRY" }, *const top[] = { "spinnerTipsEnabled" };
        StringCchCopyA(text, sizeof text, "{\"env\":{\"A\":\"1\",\"DISABLE_TELEMETRY\":\"1\"},\"model\":\"opus\"}");
        set = Core_JsonSetPath(text, strlen(text), env, 2, NULL, &length);
        Check("json path: a nested member removed, the rest kept",
              set && strcmp(set, "{\"env\":{\"A\":\"1\"},\"model\":\"opus\"}") == 0 && length == strlen(set));
        if (set) HeapFree(GetProcessHeap(), 0, set);
        set = Core_JsonSetPath("{\"model\":\"opus\"}", 16, env, 2, NULL, &length);
        Check("json path: a member already missing leaves the text as it is, nothing made",
              set && strcmp(set, "{\"model\":\"opus\"}") == 0);
        if (set) HeapFree(GetProcessHeap(), 0, set);
        set = Core_JsonSetPath("{}", 2, env, 2, "\"1\"", &length);
        Check("json path: set as Core_JsonSetNested sets", set && strcmp(set, "{\"env\":{\"DISABLE_TELEMETRY\":\"1\"}}") == 0);
        if (set) HeapFree(GetProcessHeap(), 0, set);
        StringCchCopyA(text, sizeof text, "{\"spinnerTipsEnabled\":false,\"a\":1}");
        set = Core_JsonSetPath(text, strlen(text), top, 1, NULL, &length);
        Check("json path: a top-level member removed", set && strcmp(set, "{\"a\":1}") == 0);
        if (set) HeapFree(GetProcessHeap(), 0, set);
        Check("json path: removing from no object fails", Core_JsonSetPath("[1]", 3, top, 1, NULL, &length) == NULL &&
              Core_JsonSetPath("{\"env\":3}", 9, env, 2, NULL, &length) == NULL);
        Check("env switch: on for 1, true, yes and on, in any case and as a bare value",
              Core_EnvSwitch("\"1\"", 3) == 1 && Core_EnvSwitch("\"TRUE\"", 6) == 1 && Core_EnvSwitch("\"yes\"", 5) == 1 &&
              Core_EnvSwitch("\"On\"", 4) == 1 && Core_EnvSwitch("1", 1) == 1 && Core_EnvSwitch("true", 4) == 1);
        Check("env switch: off for 0, false, no and off",
              Core_EnvSwitch("\"0\"", 3) == 0 && Core_EnvSwitch("\"false\"", 7) == 0 && Core_EnvSwitch("\"NO\"", 4) == 0 &&
              Core_EnvSwitch("\"off\"", 5) == 0 && Core_EnvSwitch("false", 5) == 0);
        Check("env switch: anything else is neither", Core_EnvSwitch("\"\"", 2) == -1 && Core_EnvSwitch("\"2\"", 3) == -1 &&
              Core_EnvSwitch("\"\\u00e9\"", 8) == -1 && Core_EnvSwitch(NULL, 0) == -1 && Core_EnvSwitch("{}", 2) == -1);
    }

    StringCchCopyA(text, sizeof text, "{\"local_9\":[\"local_1\",\"code:local_2\",\"x\\\"local_3\"],\"n\":1}");
    mapped = Core_JsonMapStrings(text, strlen(text), MapCodeIds, NULL, &length);
    Check("json strings: keys and values mapped, the rest kept",
          mapped && strcmp(mapped, "{\"cli:9\":[\"cli:1\",\"code:local_2\",\"x\\\"local_3\"],\"n\":1}") == 0 && length == strlen(mapped));
    if (mapped) HeapFree(GetProcessHeap(), 0, mapped);
    Check("json strings: a string left open is refused", Core_JsonMapStrings("[\"abc", 5, MapCodeIds, NULL, &length) == NULL);

    Check("iso time: as transcripts write it", Core_IsoTime(AtSecond(7) + 45, iso, sizeof iso) && strcmp(iso, "2026-10-08T10:00:07.045Z") == 0);

    /* A conversation both profiles went on with from line b, apart: a -> b -> c1 -> d1 (keep) and b -> c2 (drop). */
    text[0] = 0;
    TranscriptLine(text, sizeof text, "user", "a", "", 1, FALSE);
    TranscriptLine(text, sizeof text, "assistant", "b", "a", 2, FALSE);
    TranscriptLine(text, sizeof text, "user", "c1", "b", 10, FALSE);
    TranscriptLine(text, sizeof text, "user", "c2", "b", 11, FALSE);
    TranscriptLine(text, sizeof text, "progress", "p1", "c1", 12, FALSE);
    TranscriptLine(text, sizeof text, "assistant", "d1", "c1", 13, FALSE);
    TranscriptLine(text, sizeof text, "assistant", "x", "c2", 14, TRUE);
    Check("branches: two lines of conversation since the last sync",
          Core_TranscriptBranches(text, strlen(text), AtSecond(13), AtSecond(11), AtSecond(5), &excluded, &lines) && lines == 7);
    if (excluded)
        Check("branches: the copy leaves out the other branch alone and what follows it",
              !excluded[0] && !excluded[1] && excluded[2] && !excluded[3] && excluded[4] && excluded[5] && !excluded[6]);
    if (excluded) HeapFree(GetProcessHeap(), 0, excluded);
    excluded = NULL;
    /* Its last use a second before c1 (a use takes in what was written a second after it). */
    Check("branches: one went on from the other, one conversation",
          !Core_TranscriptBranches(text, strlen(text), AtSecond(13), AtSecond(9), AtSecond(5), &excluded, &lines) && !excluded);
    Check("branches: nothing new since the last sync, nothing to keep apart",
          !Core_TranscriptBranches(text, strlen(text), AtSecond(13), AtSecond(11), AtSecond(20), &excluded, &lines));
    Check("branches: the same line, nothing to keep apart",
          !Core_TranscriptBranches(text, strlen(text), AtSecond(13), AtSecond(13), AtSecond(5), &excluded, &lines));
}

static void TestDeflate(void)
{
    /* zlib's own output for 1129 bytes of words: a block of dynamic codes, which Core_Deflate never writes. */
    static const BYTE kDynamic[] = {
        0x75, 0x53, 0xD1, 0x6E, 0x03, 0x21, 0x0C, 0xFB, 0x15, 0x7E, 0x8D, 0x02, 0xBD, 0xA1, 0xA3, 0x80, 0x80, 0xDB,
        0xA9, 0x7F, 0x5F, 0x2D, 0x71, 0x18, 0x64, 0xDA, 0x4B, 0xA3, 0x4B, 0x1C, 0xC7, 0x71, 0xCA, 0x1D, 0xB3, 0x2F,
        0xB7, 0x79, 0x58, 0x77, 0x5E, 0xD5, 0xF8, 0xD0, 0xCF, 0x51, 0xAA, 0xE9, 0xA1, 0xF7, 0x58, 0xB2, 0x79, 0x96,
        0xE4, 0x43, 0x33, 0x29, 0xE6, 0xD3, 0xB8, 0x64, 0x2F, 0x1F, 0x66, 0x69, 0x34, 0x9B, 0xBB, 0x6B, 0xB1, 0x0E,
        0x53, 0xA3, 0x1B, 0x57, 0x0B, 0xBA, 0xEB, 0x65, 0xB3, 0x3D, 0x96, 0x88, 0xF4, 0xC3, 0xFA, 0x23, 0x68, 0x8C,
        0xB4, 0xD2, 0x24, 0x46, 0x48, 0x4A, 0x6B, 0xDA, 0xAB, 0x10, 0xDE, 0xE3, 0x91, 0x27, 0x19, 0x72, 0xC4, 0x45,
        0x85, 0xF8, 0xAA, 0xA5, 0x0D, 0x4E, 0x88, 0x58, 0xAC, 0x43, 0x39, 0x88, 0x11, 0x4E, 0x81, 0x84, 0x3C, 0xDA,
        0x7B, 0xB2, 0xDE, 0xEC, 0x94, 0x6D, 0xEE, 0x2B, 0x7E, 0x87, 0x19, 0xC5, 0x96, 0x9F, 0x39, 0x2C, 0x0D, 0xD3,
        0xB6, 0x3D, 0xA9, 0xBC, 0x58, 0xC6, 0xD4, 0x8A, 0x92, 0x40, 0xAB, 0xE5, 0x4B, 0x83, 0xA8, 0x00, 0xF9, 0xBD,
        0x9D, 0x6D, 0x17, 0xAA, 0xCE, 0x00, 0x28, 0x02, 0xE4, 0x72, 0x87, 0x0C, 0x06, 0x12, 0xC1, 0x95, 0x54, 0x1A,
        0x10, 0xCA, 0x19, 0x52, 0xB8, 0xC9, 0x95, 0xEB, 0x80, 0xB7, 0xB6, 0xF2, 0x8C, 0xE9, 0x8F, 0x3B, 0xEB, 0x01,
        0x98, 0x58, 0x7B, 0x0D, 0xFF, 0x68, 0x1D, 0x76, 0x4E, 0x98, 0x25, 0x6E, 0x82, 0x40, 0x28, 0x63, 0xA6, 0x0A,
        0x92, 0x0E, 0x1A, 0xF1, 0x83, 0x93, 0xF3, 0x8B, 0x15, 0x49, 0x07, 0xCF, 0x42, 0xC7, 0x4E, 0xBE, 0xA9, 0xE1,
        0x5F, 0x59, 0x8F, 0xB5, 0x00, 0xC7, 0xFC, 0xB4, 0x80, 0xD4, 0xB5, 0x02, 0x9E, 0xB9, 0x1F, 0x6D, 0x39, 0xAE,
        0x98, 0xA1, 0xD7, 0xF9, 0x2F, 0x2E, 0x3E, 0xEA, 0xD7, 0x21, 0x7E, 0x62, 0x15, 0xF9, 0x14, 0xE6, 0xF5, 0x10,
        0x90, 0xB3, 0x3D, 0x3D, 0xB5, 0xC0, 0xFA, 0xF0, 0xA5, 0xA4, 0x98, 0x7F, 0x8D, 0xA4, 0x0E, 0x36, 0x03, 0x2D,
        0xEB, 0x9F, 0xED, 0x03
    };
    static const char kText[] = "Claude Desktop Profiles Manager keeps each profile apart. ";
    BYTE *plain = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 200000), *packed = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 250000);
    BYTE *back = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 200000);
    size_t packedSize = 0, backSize = 0, i;
    DWORD seed = 12345;
    if (!plain || !packed || !back) {
        Check("deflate: buffers allocated", FALSE);
        return;
    }
    for (i = 0; i < 100000; i++) plain[i] = (BYTE)kText[i % (sizeof kText - 1)];
    Check("deflate: repeated text compresses", Core_Deflate(plain, 100000, packed, Core_DeflateBound(100000), &packedSize) &&
                                               packedSize > 0 && packedSize < 100000 / 20);
    Check("deflate: repeated text comes back whole", Core_Inflate(packed, packedSize, back, 200000, &backSize) && backSize == 100000 &&
                                                     memcmp(back, plain, 100000) == 0);
    for (i = 0; i < 150000; i++) {
        seed = seed * 1103515245u + 12345u;
        plain[i] = (BYTE)(seed >> 16);
    }
    Check("deflate: random bytes stay within the bound", Core_Deflate(plain, 150000, packed, Core_DeflateBound(150000), &packedSize) &&
                                                         packedSize <= Core_DeflateBound(150000));
    Check("deflate: random bytes come back whole", Core_Inflate(packed, packedSize, back, 200000, &backSize) && backSize == 150000 &&
                                                   memcmp(back, plain, 150000) == 0);
    Check("deflate: nothing in, an empty stream out", Core_Deflate(plain, 0, packed, Core_DeflateBound(0), &packedSize) && packedSize > 0 &&
                                                      Core_Inflate(packed, packedSize, back, 16, &backSize) && backSize == 0);
    Check("deflate: an output too small is refused", !Core_Deflate(plain, 150000, packed, 1000, &packedSize));
    Check("inflate: zlib's dynamic codes", Core_Inflate(kDynamic, sizeof kDynamic, back, 200000, &backSize) && backSize == 1129 &&
                                           Core_Crc32(0, back, backSize) == 0xA71EBEB7u);
    Check("inflate: an output too small is refused", !Core_Inflate(kDynamic, sizeof kDynamic, back, 1000, &backSize));
    Check("inflate: a cut stream is refused", !Core_Inflate(kDynamic, sizeof kDynamic / 2, back, 200000, &backSize));
    packed[0] = 0x07;   /* the last block, of the reserved type 3 */
    Check("inflate: a reserved block type is refused", !Core_Inflate(packed, 4, back, 200000, &backSize));
    HeapFree(GetProcessHeap(), 0, plain);
    HeapFree(GetProcessHeap(), 0, packed);
    HeapFree(GetProcessHeap(), 0, back);
}

static void TestDrawingMath(void)
{
    int pending, frames, units, moved, movedAfterSixFrames = 0;

    Check("scroll: nothing left, nothing to do", Core_ScrollStep(0, 16) == 0);
    Check("scroll: the first frame covers a good part at once", Core_ScrollStep(90, 16) >= 30 && Core_ScrollStep(-90, 16) <= -30);
    Check("scroll: a late frame covers more, so the pace holds", Core_ScrollStep(90, 48) > Core_ScrollStep(90, 16));
    Check("scroll: never past the end", Core_ScrollStep(90, 1000) == 90 && Core_ScrollStep(-90, 1000) == -90);
    Check("scroll: at least a pixel, so it ends", Core_ScrollStep(1, 1) == 1 && Core_ScrollStep(-1, 1) == -1);
    /* A notch of three 30 px lines at 16 ms a frame: most of it within 100
     * ms, all of it within 250, the whole way exactly. */
    for (pending = 90, moved = 0, frames = 0; (units = Core_ScrollStep(pending, 16)) != 0 && frames < 100; frames++) {
        pending -= units;
        moved += units;
        if (frames == 5) movedAfterSixFrames = moved;
    }
    Check("scroll: most of a notch within 100 ms", movedAfterSixFrames >= 80 || (frames <= 5 && moved == 90));
    Check("scroll: all of it within 250 ms", frames * 16 <= 250);
    Check("scroll: the whole way exactly", moved == 90);
    Check("scroll: more notches start faster", Core_ScrollStep(270, 16) > Core_ScrollStep(90, 16));
    {
        int rest = 0, i, px = 0;
        Check("wheel: a notch moves a notch", Core_WheelPixels(-120, 72, &rest) == -72 && rest == 0);
        Check("wheel: a mouse's notch up", Core_WheelPixels(120, 72, &rest) == 72 && rest == 0);
        /* A precision touchpad's notch in 120 messages of 1, for 48 px lines
         * and 24 px ones: each alone is less than a px. */
        for (i = 0; i < 120; i++) px += Core_WheelPixels(-1, 48, &rest);
        Check("wheel: a touchpad's smallest steps add up to the notch", px == -48 && rest == 0);
        for (px = 0, i = 0; i < 120; i++) px += Core_WheelPixels(1, 24, &rest);
        Check("wheel: up as well", px == 24 && rest == 0);
        for (px = 0, i = 0; i < 8; i++) px += Core_WheelPixels(-15, 72, &rest);
        Check("wheel: steps of 15 add up to the notch", px == -72 && rest == 0);
        Check("wheel: short of a px, it waits", Core_WheelPixels(-1, 72, &rest) == 0 && rest == -72);
        Check("wheel: the other way drops what waited", Core_WheelPixels(1, 72, &rest) == 0 && rest == 72);
        Check("wheel: a page at once", Core_WheelPixels(-120, 600, &rest) == -600);
    }

    Check("hash: FNV-1a start", CORE_HASH_START == 0xCBF29CE484222325ULL);
    Check("hash: FNV-1a of \"a\"", Core_HashBytes(CORE_HASH_START, "a", 1) == 0xAF63DC4C8601EC8CULL);
    Check("hash: FNV-1a of \"foobar\"", Core_HashBytes(CORE_HASH_START, "foobar", 6) == 0x85944171F73967E8ULL);
    Check("hash: goes on piece by piece", Core_HashBytes(Core_HashBytes(CORE_HASH_START, "foo", 3), "bar", 3) ==
                                          Core_HashBytes(CORE_HASH_START, "foobar", 6));
    {
        RECT work = { 0, 0, 1920, 1040 }, owner = { 100, 100, 900, 700 }, edge = { 1500, 800, 1900, 1000 }, got;
        Core_CenterRect(&owner, 400, 200, &work, &got);
        Check("center: on its owner", got.left == 300 && got.top == 300 && got.right == 700 && got.bottom == 500);
        Core_CenterRect(&edge, 600, 400, &work, &got);
        Check("center: kept inside the work area", got.right == 1920 && got.bottom == 1040 && got.left == 1320 && got.top == 640);
        Core_CenterRect(&owner, 2000, 1200, &work, &got);
        Check("center: bigger than the work area, its top left corner inside", got.left == 0 && got.top == 0);
        Core_CenterRect(&work, 400, 200, &work, &got);
        Check("center: on the work area itself (owner hidden)", got.left == 760 && got.top == 420);
    }
}

static void TestProcessTree(void)
{
    /* Claude (10) started 11 and 12; 12 started 13; 20 is another program; 30 has
     * the id of Claude's gone parent; 14 claims 10 as parent but started before it
     * (an older process whose parent's id Claude got). */
    static const CoreProcess kProcesses[] = {
        { 10, 1, 1000 }, { 11, 10, 1100 }, { 12, 10, 1200 }, { 13, 12, 1300 }, { 20, 1, 900 }, { 14, 10, 500 },
        { 15, 14, 600 }, { 16, 13, 0 }, { 17, 17, 1400 },
    };
    BOOL chosen[ARRAYSIZE(kProcesses)];
    int n = Core_ProcessDescendants(kProcesses, ARRAYSIZE(kProcesses), 0, chosen);
    Check("process tree: children and grandchildren", chosen[1] && chosen[2] && chosen[3]);
    Check("process tree: the root and other programs left", !chosen[0] && !chosen[4]);
    Check("process tree: an older process with a reused parent id left, and its children",
          !chosen[5] && !chosen[6]);
    Check("process tree: a process whose start is unknown left", !chosen[7]);
    Check("process tree: a process its own parent left", !chosen[8]);
    Check("process tree: the count", n == 3);
    Check("process tree: a root out of range marks none", Core_ProcessDescendants(kProcesses, ARRAYSIZE(kProcesses), 9, chosen) == 0 && !chosen[1]);
}

/* ---------------------------------------------------------------- LevelDB */

typedef struct LevelSeen {
    int       count;
    ULONGLONG sequence[8];
    BOOL      put[8];
    char      key[8][16];
    char      value[8][16];
    size_t    recordLengths[8];
    int       records;
    BYTE     *last;     /* the last record, copied */
    size_t    lastLength;
} LevelSeen;

static BOOL SeeEntry(void *context, ULONGLONG sequence, const CoreLevelOp *op)
{
    LevelSeen *seen = (LevelSeen *)context;
    if (seen->count >= 8) return FALSE;
    seen->sequence[seen->count] = sequence;
    seen->put[seen->count] = op->put;
    memcpy(seen->key[seen->count], op->key, min(op->keyLength, 15));
    seen->key[seen->count][min(op->keyLength, 15)] = 0;
    if (op->put) memcpy(seen->value[seen->count], op->value, min(op->valueLength, 15));
    seen->value[seen->count][op->put ? min(op->valueLength, 15) : 0] = 0;
    seen->count++;
    return TRUE;
}

static BOOL SeeRecord(void *context, const BYTE *record, size_t length)
{
    LevelSeen *seen = (LevelSeen *)context;
    if (seen->records < 8) seen->recordLengths[seen->records] = length;
    seen->records++;
    if (seen->last) HeapFree(GetProcessHeap(), 0, seen->last);
    seen->last = (BYTE *)HeapAlloc(GetProcessHeap(), 0, max(length, 1));
    if (seen->last) memcpy(seen->last, record, length);
    seen->lastLength = length;
    return TRUE;
}

static void PutFixed(BYTE *out, ULONGLONG value, int bytes)
{
    int i;
    for (i = 0; i < bytes; i++) out[i] = (BYTE)(value >> (8 * i));
}

/* A block of a table: entries of whole keys (none shared), one restart. */
static size_t TableBlock(BYTE *out, const BYTE *const *keys, const size_t *keyLengths, const char *const *values, int count)
{
    size_t at = 0;
    int i;
    for (i = 0; i < count; i++) {
        size_t valueLength = strlen(values[i]);
        out[at++] = 0;
        out[at++] = (BYTE)keyLengths[i];
        out[at++] = (BYTE)valueLength;
        memcpy(out + at, keys[i], keyLengths[i]);
        at += keyLengths[i];
        memcpy(out + at, values[i], valueLength);
        at += valueLength;
    }
    PutFixed(out + at, 0, 4);
    PutFixed(out + at + 4, 1, 4);
    return at + 8;
}

/* A table: one data block (Snappy-compressed when `compressed`, as one literal), its index block pointing to it, the footer. */
static size_t MakeTable(BYTE *out, BOOL compressed)
{
    BYTE keyA[9], keyB[9], data[128];
    const BYTE *keys[2] = { keyA, keyB };
    const char *values[2] = { "x", "" };
    size_t keyLengths[2] = { 9, 9 }, dataLength, at = 0, dataSize, indexOffset, indexStart, footer;
    keyA[0] = 'a';
    PutFixed(keyA + 1, (5ULL << 8) | 1, 8);   /* sequence 5, a value */
    keyB[0] = 'b';
    PutFixed(keyB + 1, (6ULL << 8) | 0, 8);   /* sequence 6, deleted */
    dataLength = TableBlock(data, keys, keyLengths, values, 2);
    if (compressed) {
        out[at++] = (BYTE)dataLength;          /* the inflated length, under 128 */
        out[at++] = (BYTE)(60 << 2);           /* a literal, its length - 1 in the next byte */
        out[at++] = (BYTE)(dataLength - 1);
    }
    memcpy(out + at, data, dataLength);
    at += dataLength;
    dataSize = at;
    out[at++] = (BYTE)(compressed ? 1 : 0);
    PutFixed(out + at, 0, 4);                  /* its checksum: not read */
    at += 4;
    /* The index: one entry, the last key of the block and where the block is. */
    indexOffset = indexStart = at;
    out[at++] = 0;
    out[at++] = 9;
    out[at++] = 2;
    memcpy(out + at, keyB, 9);
    at += 9;
    out[at++] = 0;                             /* offset 0 */
    out[at++] = (BYTE)dataSize;
    PutFixed(out + at, 0, 4);
    PutFixed(out + at + 4, 1, 4);
    at += 8;
    footer = at + 5;
    out[at] = 0;
    PutFixed(out + at + 1, 0, 4);
    memset(out + footer, 0, 48);
    out[footer + 2] = (BYTE)indexOffset;       /* the metaindex first: offset 0, size 0 */
    out[footer + 3] = (BYTE)(at - indexStart);
    PutFixed(out + footer + 40, 0xdb4775248b80fb57ULL, 8);
    return footer + 48;
}

typedef struct TableSeen {
    ULONGLONG added[4], gone[4];
    int addedCount, goneCount;
} TableSeen;

static void SeeTable(void *context, ULONGLONG number, BOOL added)
{
    TableSeen *seen = (TableSeen *)context;
    if (added && seen->addedCount < 4) seen->added[seen->addedCount++] = number;
    if (!added && seen->goneCount < 4) seen->gone[seen->goneCount++] = number;
}

/* ------------------------------------------------------ three-way merge */

typedef struct MergeProbe {
    char  paths[8][CORE_MERGE_PATH_CCH];
    DWORD members[8];
    int   latest[8], count;
    const char *decidePath;   /* the one path the person decided, for member decideFor */
    int   decideFor;
} MergeProbe;

static int ProbeDecide(void *context, const char *path, DWORD members)
{
    MergeProbe *probe = (MergeProbe *)context;
    (void)members;
    return probe->decidePath && strcmp(path, probe->decidePath) == 0 ? probe->decideFor : -1;
}

static void ProbeReport(void *context, const char *path, DWORD members, int latest, const char *const *values, const size_t *lengths)
{
    MergeProbe *probe = (MergeProbe *)context;
    (void)values;
    (void)lengths;
    if (probe->count >= 8) return;
    StringCchCopyA(probe->paths[probe->count], CORE_MERGE_PATH_CCH, path);
    probe->members[probe->count] = members;
    probe->latest[probe->count++] = latest;
}

/* `base` and two members' values merged (NULL: absent), the result compared with `expected` (NULL: absent). */
static BOOL MergeTwo(const char *base, const char *a, const char *b, ULONGLONG writtenA, ULONGLONG writtenB, int depth, MergeProbe *probe,
                     const char *expected, int unresolvedExpected)
{
    const char *values[2] = { a, b };
    size_t lengths[2] = { a ? strlen(a) : 0, b ? strlen(b) : 0 }, outLen = 0;
    BOOL say[2] = { TRUE, TRUE };
    ULONGLONG written[2] = { writtenA, writtenB };
    int unresolved = -1;
    char *merged = Core_JsonMerge(base, base ? strlen(base) : 0, values, lengths, say, written, 2, depth, ProbeDecide, ProbeReport, probe,
                                  &outLen, &unresolved);
    BOOL ok = merged && unresolved == unresolvedExpected &&
              (expected ? outLen == strlen(expected) && memcmp(merged, expected, outLen) == 0 : outLen == 0);
    if (!ok) printf("        merged: %s (unresolved %d)\n", merged ? merged : "(null)", unresolved);
    if (merged) HeapFree(GetProcessHeap(), 0, merged);
    return ok;
}

/* `json` (NULL: none) with `id` read or marked unread, as Core_UnreadSet makes it. */
static void CheckUnread(const char *name, const char *json, const WCHAR *id, BOOL unread, const char *expected)
{
    size_t length = 0;
    char *set = Core_UnreadSet(json, json ? strlen(json) : 0, id, unread, &length);
    BOOL ok = set && length == strlen(expected) && strcmp(set, expected) == 0;
    if (!ok) printf("    got %s\n", set ? set : "(null)");
    Check(name, ok);
    if (set) HeapFree(GetProcessHeap(), 0, set);
}

/* Claude's sessions unread (epitaxy-unread-v1): found in either list, read
 * out of both, marked unread as its Mark as unread does. */
static void TestUnreadMarks(void)
{
    static const char kClaude[] = "{\"state\":{\"unreadIds\":[\"local_509c1a37-c07d-463f-a86a-d56c9e3953cb\"],\"explicitUnreadIds\":[]},\"version\":0}";
    static const char kBoth[] = "{\"state\":{\"unreadIds\":[\"local_a\",\"local_b\"],\"explicitUnreadIds\":[\"local_a\",\"local_c\"]},\"version\":0}";
    PendingEdit back;
    Check("unread: a session Claude marked", Core_UnreadHas(kClaude, strlen(kClaude), L"local_509c1a37-c07d-463f-a86a-d56c9e3953cb"));
    Check("unread: one marked by hand", Core_UnreadHas(kBoth, strlen(kBoth), L"local_c"));
    Check("unread: one in neither list, or no value", !Core_UnreadHas(kBoth, strlen(kBoth), L"local_d") && !Core_UnreadHas(NULL, 0, L"local_a") &&
                                                     !Core_UnreadHas(kBoth, strlen(kBoth), L""));
    CheckUnread("unread: read takes it out of both lists, the others kept in their order", kBoth, L"local_a", FALSE,
                "{\"state\":{\"unreadIds\":[\"local_b\"],\"explicitUnreadIds\":[\"local_c\"]},\"version\":0}");
    CheckUnread("unread: Claude's own mark read", kClaude, L"local_509c1a37-c07d-463f-a86a-d56c9e3953cb", FALSE,
                "{\"state\":{\"unreadIds\":[],\"explicitUnreadIds\":[]},\"version\":0}");
    CheckUnread("unread: marked unread by hand, once", kBoth, L"local_c", TRUE,
                "{\"state\":{\"unreadIds\":[\"local_a\",\"local_b\"],\"explicitUnreadIds\":[\"local_a\",\"local_c\"]},\"version\":0}");
    CheckUnread("unread: marked unread, Claude's own mark kept", kClaude, L"local_x", TRUE,
                "{\"state\":{\"unreadIds\":[\"local_509c1a37-c07d-463f-a86a-d56c9e3953cb\"],\"explicitUnreadIds\":[\"local_x\"]},\"version\":0}");
    CheckUnread("unread: no value yet: one made", NULL, L"local_x", TRUE,
                "{\"state\":{\"unreadIds\":[],\"explicitUnreadIds\":[\"local_x\"]},\"version\":0}");
    CheckUnread("unread: a list missing is made, the store's other members kept", "{\"state\":{\"unreadIds\":[\"local_a\"],\"other\":1},\"version\":2}",
                L"local_a", FALSE, "{\"state\":{\"unreadIds\":[],\"other\":1,\"explicitUnreadIds\":[]},\"version\":2}");
    Check("pending: a session read", Core_PendingParse(L"read\tk\t1", &back) && back.op == PENDING_READ && back.value[0] == L'1');
}

/* `base` joined with `other`'s scheduledTasks, as Core_JsonArrayUnion does; `expected` NULL: no result. */
static void CheckUnion(const char *name, const char *base, const char *other, const char *expected)
{
    size_t length = 0;
    char *joined = Core_JsonArrayUnion(base, strlen(base), other, strlen(other), "scheduledTasks", "id", &length);
    BOOL ok = expected ? joined && length == strlen(expected) && strcmp(joined, expected) == 0 : joined == NULL;
    if (!ok && joined) printf("    got %s\n", joined);
    Check(name, ok);
    if (joined) HeapFree(GetProcessHeap(), 0, joined);
}

/* The scheduled tasks of several backups restored into one profile: each
 * backup's tasks join the profile's, by id, the backup's own kept. */
static void TestJsonArrayUnion(void)
{
    CheckUnion("union: the other's new task added, the same id's kept as it is",
               "{\"scheduledTasks\":[{\"id\":\"a\",\"n\":1}],\"recordedSkips\":{}}",
               "{\"scheduledTasks\":[{\"id\":\"a\",\"n\":2},{\"id\":\"b\"}]}",
               "{\"scheduledTasks\":[{\"id\":\"a\",\"n\":1},{\"id\":\"b\"}],\"recordedSkips\":{}}");
    CheckUnion("union: an empty array given the other's",
               "{\"scheduledTasks\":[ ],\"x\":true}", "{\"scheduledTasks\":[{\"id\":\"b\"}]}",
               "{\"scheduledTasks\":[ {\"id\":\"b\"}],\"x\":true}");
    CheckUnion("union: written as Claude writes it, spaces kept",
               "{\n  \"scheduledTasks\": [\n    {\"id\": \"a\"}\n  ]\n}", "{\"scheduledTasks\":[{\"id\":\"b\"}]}",
               "{\n  \"scheduledTasks\": [\n    {\"id\": \"a\"}\n  ,{\"id\":\"b\"}]\n}");
    CheckUnion("union: no array yet, the other's made",
               "{\"x\":1}", "{\"scheduledTasks\":[{\"id\":\"b\"}]}", "{\"x\":1,\"scheduledTasks\":[{\"id\":\"b\"}]}");
    CheckUnion("union: the other without the array, unchanged",
               "{\"scheduledTasks\":[{\"id\":\"a\"}]}", "{\"recordedSkips\":{}}", "{\"scheduledTasks\":[{\"id\":\"a\"}]}");
    CheckUnion("union: elements without an id, added unless equal",
               "{\"scheduledTasks\":[1,\"s\"]}", "{\"scheduledTasks\":[\"s\",2]}", "{\"scheduledTasks\":[1,\"s\",2]}");
    CheckUnion("union: not an object, none", "[1]", "{\"scheduledTasks\":[]}", NULL);
    CheckUnion("union: an array that is not one, none", "{\"scheduledTasks\":{}}", "{\"scheduledTasks\":[]}", NULL);
    CheckUnion("union: the other damaged, the first as it is", "{\"scheduledTasks\":[]}", "{\"scheduledTasks\":[{\"id\":\"b\"}",
               "{\"scheduledTasks\":[]}");
}

/* `plain` sealed as OSCrypt does on Windows ("v10", nonce, AES-256-GCM, tag), with bcrypt itself. */
static size_t SealLikeChromium(const BYTE *key, const BYTE *nonce, const char *plain, BYTE *out)
{
    BCRYPT_ALG_HANDLE aes = NULL;
    BCRYPT_KEY_HANDLE handle = NULL;
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO mode;
    ULONG done = 0, length = (ULONG)strlen(plain);
    BOOL ok;
    BCRYPT_INIT_AUTH_MODE_INFO(mode);
    memcpy(out, "v10", 3);
    memcpy(out + 3, nonce, 12);
    mode.pbNonce = (PUCHAR)nonce;
    mode.cbNonce = 12;
    mode.pbTag = out + 15 + length;
    mode.cbTag = 16;
    ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&aes, BCRYPT_AES_ALGORITHM, NULL, 0)) &&
         BCRYPT_SUCCESS(BCryptSetProperty(aes, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_GCM, sizeof BCRYPT_CHAIN_MODE_GCM, 0)) &&
         BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(aes, &handle, NULL, 0, (PUCHAR)key, 32, 0)) &&
         BCRYPT_SUCCESS(BCryptEncrypt(handle, (PUCHAR)plain, length, &mode, NULL, 0, out + 15, length, &done, 0));
    if (handle) BCryptDestroyKey(handle);
    if (aes) BCryptCloseAlgorithmProvider(aes, 0);
    return ok && done == length ? 15 + length + 16 : 0;
}

/* Electron's safeStorage, which keeps Claude's token cache in config.json:
 * opened with Local State's key, checked by its tag. */
static void TestSafeStorage(void)
{
    BYTE key[32], other[32], nonce[12], sealed[256], plain[256];
    size_t length, opened = 0, i;
    const char *text = "{\"org:scopes\":{\"expiresAt\":1}}";
    for (i = 0; i < sizeof key; i++) {
        key[i] = (BYTE)(i * 7 + 1);
        other[i] = (BYTE)(i * 7 + 2);
    }
    for (i = 0; i < sizeof nonce; i++) nonce[i] = (BYTE)(0xA0 + i);
    length = SealLikeChromium(key, nonce, text, sealed);
    Check("safe storage: sealed by bcrypt for the test", length == strlen(text) + 31);
    Check("safe storage: opened with its key", Core_OpenSafeStorage(sealed, length, key, sizeof key, plain, sizeof plain, &opened) &&
          opened == strlen(text) && memcmp(plain, text, opened) == 0);
    Check("safe storage: another key opens nothing", !Core_OpenSafeStorage(sealed, length, other, sizeof other, plain, sizeof plain, &opened) && opened == 0);
    sealed[20] ^= 1;
    Check("safe storage: a changed byte opens nothing", !Core_OpenSafeStorage(sealed, length, key, sizeof key, plain, sizeof plain, &opened));
    sealed[20] ^= 1;
    sealed[1] = '2';
    Check("safe storage: only v10", !Core_OpenSafeStorage(sealed, length, key, sizeof key, plain, sizeof plain, &opened));
    sealed[1] = '1';
    Check("safe storage: too short, nothing", !Core_OpenSafeStorage(sealed, 30, key, sizeof key, plain, sizeof plain, &opened));
    Check("safe storage: no room, nothing", !Core_OpenSafeStorage(sealed, length, key, sizeof key, plain, 4, &opened));

    /* Sealing is the inverse: what Core_SealSafeStorage writes, Core_OpenSafeStorage opens, with the same key. */
    {
        BYTE resealed[256];
        size_t resealedLength = 0, reopened = 0;
        Check("safe storage: sealed again by Core_SealSafeStorage",
              Core_SealSafeStorage((const BYTE *)text, strlen(text), key, sizeof key, resealed, sizeof resealed, &resealedLength) &&
              resealedLength == strlen(text) + 31 && memcmp(resealed, "v10", 3) == 0);
        Check("safe storage: and opened back to the same text",
              Core_OpenSafeStorage(resealed, resealedLength, key, sizeof key, plain, sizeof plain, &reopened) &&
              reopened == strlen(text) && memcmp(plain, text, reopened) == 0);
        Check("safe storage: two seals differ (a random nonce)",
              Core_SealSafeStorage((const BYTE *)text, strlen(text), key, sizeof key, sealed, sizeof sealed, &opened) &&
              memcmp(resealed + 3, sealed + 3, 12) != 0);
        Check("safe storage: no room to seal, nothing",
              !Core_SealSafeStorage((const BYTE *)text, strlen(text), key, sizeof key, resealed, 10, &resealedLength) && resealedLength == 0);
        Check("safe storage: a wrong key length seals nothing",
              !Core_SealSafeStorage((const BYTE *)text, strlen(text), key, 16, resealed, sizeof resealed, &resealedLength));
    }
}

/* Claude's token cache, opened: what it says of the sign-in, never a token;
 * its members keyed by account, client, organization, API and scopes. */
static void TestTokenCache(void)
{
    const char *cache = "{\"acct:a-1|c-1:o-1:https://api.anthropic.com:user:inference user:profile\":"
                        "{\"token\":\"sk-1\",\"refreshToken\":\"rt-1\",\"expiresAt\":1794041189656,\"subscriptionType\":\"max\",\"rateLimitTier\":\"tier_20x\"},"
                        "\"acct:a-1|c-2:o-1:https://api.anthropic.com:user:office\":{\"token\":\"sk-2\",\"expiresAt\":1822886130157,\"subscriptionType\":null},"
                        "\"not a token\":{\"expiresAt\":1}}";
    CoreSignIn signIn;
    WCHAR size[32];
    ZeroMemory(&signIn, sizeof signIn);
    Core_ReadTokenCache(cache, strlen(cache), &signIn);
    Check("token cache: its account, organization, plan and tier",
          wcscmp(signIn.account, L"a-1") == 0 && wcscmp(signIn.organization, L"o-1") == 0 && wcscmp(signIn.plan, L"max") == 0 &&
          wcscmp(signIn.tier, L"tier_20x") == 0);
    Check("token cache: each token's scopes, until when, whether it renews; another member none",
          signIn.tokenCount == 2 && wcscmp(signIn.tokens[0].scopes, L"user:inference user:profile") == 0 && signIn.tokens[0].expires == 1794041189656ull &&
          signIn.tokens[0].renews && wcscmp(signIn.tokens[1].scopes, L"user:office") == 0 && !signIn.tokens[1].renews);
    Core_FormatSize(40960, size, ARRAYSIZE(size));
    Check("size: kilobytes", wcscmp(size, L"40 KB") == 0);
    Core_FormatSize(3435973ull, size, ARRAYSIZE(size));
    Check("size: megabytes, a tenth", wcscmp(size, L"3.2 MB") == 0);
    Core_FormatSize(20010000000ull, size, ARRAYSIZE(size));
    Check("size: gigabytes from one, a tenth", wcscmp(size, L"18.6 GB") == 0);
    {
        static const WCHAR kNotes[] = L"> Unsigned build. \x672A\x7B7E\x540D\r\n\r\n> Run several accounts.\r\n\r\nWhat the day brings.\r\n\r\n"
                                      L"## Improvements\r\n\r\n1. Faster\r\n\r\n---\r\n\r\n> \x591A\x8D26\x53F7\r\n\r\n\x6458\x8981\r\n\r\n"
                                      L"## \x4F18\x5316\x6539\x8FDB\r\n\r\n1. \x66F4\x5FEB\r\n\r\n\r\n---\r\n\r\n## \xD83D\xDCE5 Installation / \x5B89\x88C5\r\n";
        size_t start = 0, length = 0;
        Check("release notes: the English, from its summary to the rule, quotes and footer left out",
              Core_ReleaseNotesIn(kNotes, FALSE, &start, &length) &&
              wcsncmp(kNotes + start, L"What the day brings.\r\n\r\n## Improvements\r\n\r\n1. Faster", length) == 0 &&
              length == wcslen(L"What the day brings.\r\n\r\n## Improvements\r\n\r\n1. Faster"));
        Check("release notes: the Chinese, between the rules",
              Core_ReleaseNotesIn(kNotes, TRUE, &start, &length) &&
              wcsncmp(kNotes + start, L"\x6458\x8981\r\n\r\n## \x4F18\x5316\x6539\x8FDB\r\n\r\n1. \x66F4\x5FEB", length) == 0 &&
              length == wcslen(L"\x6458\x8981\r\n\r\n## \x4F18\x5316\x6539\x8FDB\r\n\r\n1. \x66F4\x5FEB"));
        Check("release notes: without the rules, not split", !Core_ReleaseNotesIn(L"Just notes\n", TRUE, &start, &length) &&
              !Core_ReleaseNotesIn(NULL, FALSE, &start, &length));
    }
}

static void TestJsonMerge(void)
{
    MergeProbe probe;
    const char *groups = "{\"groups\":[{\"id\":\"g1\",\"name\":\"Work\"}],\"assignments\":{\"s1\":\"g1\"}}";

    ZeroMemory(&probe, sizeof probe);
    Check("merge: one profile changed it -> its value, no conflict",
          MergeTwo(groups, "{\"groups\":[{\"id\":\"g1\",\"name\":\"Job\"}],\"assignments\":{\"s1\":\"g1\"}}", groups, 2, 1, 6, &probe,
                   "{\"groups\":[{\"id\":\"g1\",\"name\":\"Job\"}],\"assignments\":{\"s1\":\"g1\"}}", 0) && probe.count == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: each added a group of its own -> both kept, no conflict",
          MergeTwo(groups, "{\"groups\":[{\"id\":\"g1\",\"name\":\"Work\"},{\"id\":\"g2\",\"name\":\"A\"}],\"assignments\":{\"s1\":\"g1\"}}",
                   "{\"groups\":[{\"id\":\"g1\",\"name\":\"Work\"},{\"id\":\"g3\",\"name\":\"B\"}],\"assignments\":{\"s1\":\"g1\",\"s2\":\"g3\"}}",
                   1, 2, 6, &probe,
                   "{\"groups\":[{\"id\":\"g1\",\"name\":\"Work\"},{\"id\":\"g3\",\"name\":\"B\"},{\"id\":\"g2\",\"name\":\"A\"}],"
                   "\"assignments\":{\"s1\":\"g1\",\"s2\":\"g3\"}}", 0) && probe.count == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: the same group renamed two ways -> a conflict at its name, the later one standing in",
          MergeTwo(groups, "{\"groups\":[{\"id\":\"g1\",\"name\":\"Job\"}],\"assignments\":{\"s1\":\"g1\"}}",
                   "{\"groups\":[{\"id\":\"g1\",\"name\":\"Tasks\"}],\"assignments\":{\"s1\":\"g1\"}}", 1, 2, 6, &probe,
                   "{\"groups\":[{\"id\":\"g1\",\"name\":\"Tasks\"}],\"assignments\":{\"s1\":\"g1\"}}", 1) &&
          probe.count == 1 && strcmp(probe.paths[0], "/groups/#g1/name") == 0 && probe.members[0] == 0x3 && probe.latest[0] == 1);

    ZeroMemory(&probe, sizeof probe);
    probe.decidePath = "/groups/#g1/name";
    probe.decideFor = 0;
    Check("merge: the person's choice settles it",
          MergeTwo(groups, "{\"groups\":[{\"id\":\"g1\",\"name\":\"Job\"}],\"assignments\":{\"s1\":\"g1\"}}",
                   "{\"groups\":[{\"id\":\"g1\",\"name\":\"Tasks\"}],\"assignments\":{\"s1\":\"g1\"}}", 1, 2, 6, &probe,
                   "{\"groups\":[{\"id\":\"g1\",\"name\":\"Job\"}],\"assignments\":{\"s1\":\"g1\"}}", 0) && probe.count == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: a list of plain values merges as a set (each one's additions, either one's removals)",
          MergeTwo("[\"a\",\"b\"]", "[\"b\",\"c\"]", "[\"a\",\"b\",\"d\"]", 2, 1, 6, &probe, "[\"b\",\"c\",\"d\"]", 0) && probe.count == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: taken out by one, changed by the other -> a conflict at it",
          MergeTwo("{\"x\":1,\"y\":1}", "{\"y\":1}", "{\"x\":2,\"y\":1}", 1, 2, 6, &probe, "{\"x\":2,\"y\":1}", 1) &&
          probe.count == 1 && strcmp(probe.paths[0], "/x") == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: first sync, no base -> both groups kept, the session placed two ways a conflict",
          MergeTwo(NULL, "{\"groups\":[{\"id\":\"a\",\"name\":\"A\"}],\"assignments\":{\"s1\":\"a\"}}",
                   "{\"groups\":[{\"id\":\"b\",\"name\":\"B\"}],\"assignments\":{\"s1\":\"b\",\"s2\":\"b\"}}", 1, 2, 6, &probe,
                   "{\"groups\":[{\"id\":\"b\",\"name\":\"B\"},{\"id\":\"a\",\"name\":\"A\"}],\"assignments\":{\"s1\":\"b\",\"s2\":\"b\"}}", 1) &&
          probe.count == 1 && strcmp(probe.paths[0], "/assignments/s1") == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: with no depth, two changes are one conflict at the value itself",
          MergeTwo("{\"root\":1}", "{\"root\":2}", "{\"root\":3}", 1, 2, 0, &probe, "{\"root\":3}", 1) &&
          probe.count == 1 && strcmp(probe.paths[0], "") == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: a tuple told apart by its first string stays whole",
          MergeTwo("[[\"s1\",\"opus\",\"high\"]]", "[[\"s1\",\"sonnet\",\"low\"]]", "[[\"s1\",\"opus\",\"max\"]]", 1, 2, 6, &probe,
                   "[[\"s1\",\"opus\",\"max\"]]", 1) &&
          probe.count == 1 && strcmp(probe.paths[0], "/#s1") == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: a key with / and ~ is escaped in the path",
          MergeTwo("{\"a/b~c\":1}", "{\"a/b~c\":2}", "{\"a/b~c\":3}", 2, 1, 6, &probe, "{\"a/b~c\":2}", 1) &&
          probe.count == 1 && strcmp(probe.paths[0], "/a~1b~0c") == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: the same value written with other spaces is no change",
          MergeTwo("{\"a\": [1, 2]}", "{\"a\":[1,2]}", "{ \"a\" : [1,2] }", 1, 2, 6, &probe, "{\"a\": [1, 2]}", 0) && probe.count == 0);

    ZeroMemory(&probe, sizeof probe);
    Check("merge: taken out by both -> gone", MergeTwo("{\"x\":1}", NULL, NULL, 1, 2, 6, &probe, NULL, 0));
    {
        const char *values[2] = { "{\"x\":5}", "{\"x\":9}" };
        size_t lengths[2] = { 7, 7 }, outLen = 0;
        BOOL say[2] = { TRUE, FALSE };
        ULONGLONG written[2] = { 1, 2 };
        int unresolved = -1;
        char *merged = Core_JsonMerge("{\"x\":1}", 7, values, lengths, say, written, 2, 6, NULL, NULL, NULL, &outLen, &unresolved);
        Check("merge: a profile whose view does not count changes nothing",
              merged && unresolved == 0 && outLen == 7 && memcmp(merged, "{\"x\":5}", 7) == 0);
        if (merged) HeapFree(GetProcessHeap(), 0, merged);
    }
}

static void TestLevelDb(void)
{
    static const BYTE kSnappy[] = { 0x0C, 0x08, 'a', 'b', 'c', 0x15, 0x03 };
    static const BYTE kBadSnappy[] = { 0x0C, 0x08, 'a', 'b', 'c', 0x15, 0x09 };
    BYTE zeros[32], ones[32], decoded[16], batch[256], *log, *big, table[512], key[64], value[64];
    CoreLevelOp ops[2];
    LevelSeen seen;
    CoreLevelManifest manifest;
    TableSeen tables;
    WCHAR text[16];
    size_t length, batchLength, logLength, appended, cleanEnd, i, n;

    Check("CRC32C of \"123456789\"", Core_Crc32c(0, "123456789", 9) == 0xE3069283u);
    memset(zeros, 0, sizeof zeros);
    memset(ones, 0xFF, sizeof ones);
    Check("CRC32C of 32 zeros and of 32 0xFF bytes, as LevelDB's tests say",
          Core_Crc32c(0, zeros, sizeof zeros) == 0x8a9136aau && Core_Crc32c(0, ones, sizeof ones) == 0x62a8ab43u);
    Check("CRC32C goes on from a part", Core_Crc32c(Core_Crc32c(0, "1234", 4), "56789", 5) == 0xE3069283u);
    Check("a masked CRC differs from the CRC", Core_LevelMask(0xE3069283u) != 0xE3069283u);

    Check("Snappy: the length comes first", Core_SnappyLength(kSnappy, sizeof kSnappy, &length) && length == 12);
    Check("Snappy: a literal and a copy of it", Core_SnappyDecode(kSnappy, sizeof kSnappy, decoded, 12) && memcmp(decoded, "abcabcabcabc", 12) == 0);
    Check("Snappy: a copy from before the start is refused", !Core_SnappyDecode(kBadSnappy, sizeof kBadSnappy, decoded, 12));
    Check("Snappy: a length that does not match is refused", !Core_SnappyDecode(kSnappy, sizeof kSnappy, decoded, 11));

    /* A write batch: a value put, a key deleted, from sequence 100. */
    ZeroMemory(ops, sizeof ops);
    ops[0].put = TRUE;
    ops[0].key = (const BYTE *)"k1";
    ops[0].keyLength = 2;
    ops[0].value = (const BYTE *)"v1";
    ops[0].valueLength = 2;
    ops[1].key = (const BYTE *)"k2";
    ops[1].keyLength = 2;
    batchLength = Core_LevelBatchWrite(100, ops, 2, batch, sizeof batch);
    ZeroMemory(&seen, sizeof seen);
    Check("a batch is written", batchLength == 12 + 1 + 1 + 2 + 1 + 2 + 1 + 1 + 2);
    Check("a batch reads back: the put, then the deletion, each with its sequence",
          Core_LevelBatchRead(batch, batchLength, SeeEntry, &seen) && seen.count == 2 && seen.sequence[0] == 100 && seen.put[0] &&
          strcmp(seen.key[0], "k1") == 0 && strcmp(seen.value[0], "v1") == 0 && seen.sequence[1] == 101 && !seen.put[1] &&
          strcmp(seen.key[1], "k2") == 0);
    Check("a batch cut short is refused", !Core_LevelBatchRead(batch, batchLength - 1, SeeEntry, &seen));

    /* The log: the batch added to an empty log, then a record spanning three blocks, then one where a block has no room left. */
    log = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 4 * 32768);
    big = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 70000);
    if (!log || !big) {
        Check("log buffers", FALSE);
        return;
    }
    for (i = 0; i < 70000; i++) big[i] = (BYTE)(i * 7);
    logLength = Core_LevelLogAppend(0, batch, batchLength, log, 4 * 32768);
    Check("one record in an empty log: a header and the batch", logLength == 7 + batchLength);
    appended = Core_LevelLogAppend(logLength, big, 70000, log + logLength, 4 * 32768 - logLength);
    Check("a record longer than a block takes three", appended == 70000 + 3 * 7);
    logLength += appended;
    ZeroMemory(&seen, sizeof seen);
    Check("the log reads back: both records whole, the long one put together",
          Core_LevelLogRecords(log, logLength, SeeRecord, &seen, &cleanEnd) && seen.records == 2 && seen.recordLengths[0] == batchLength &&
          seen.lastLength == 70000 && seen.last && memcmp(seen.last, big, 70000) == 0 && cleanEnd == logLength);
    if (seen.last) HeapFree(GetProcessHeap(), 0, seen.last);

    /* A record that ends three bytes before a block's end: the next one starts in the next block, the gap zeros. */
    memset(log, 0, 4 * 32768);
    n = Core_LevelLogAppend(0, big, 32768 - 3 - 7, log, 4 * 32768);
    appended = Core_LevelLogAppend(n, batch, batchLength, log + n, 4 * 32768 - n);
    Check("a block's last bytes too few for a header stay zeros", n == 32765 && appended == 3 + 7 + batchLength &&
          log[n] == 0 && log[n + 1] == 0 && log[n + 2] == 0);
    ZeroMemory(&seen, sizeof seen);
    Check("both read back across the gap", Core_LevelLogRecords(log, n + appended, SeeRecord, &seen, &cleanEnd) && seen.records == 2 &&
          seen.lastLength == batchLength && memcmp(seen.last, batch, batchLength) == 0 && cleanEnd == n + appended);
    if (seen.last) HeapFree(GetProcessHeap(), 0, seen.last);
    /* Garbage after the records: reading stops before it, at the clean end. */
    log[n + appended] = 0x55;
    log[n + appended + 4] = 9;
    log[n + appended + 6] = 1;
    ZeroMemory(&seen, sizeof seen);
    Check("garbage after the last record is not read", Core_LevelLogRecords(log, n + appended + 20, SeeRecord, &seen, &cleanEnd) &&
          seen.records == 2 && cleanEnd == n + appended);
    if (seen.last) HeapFree(GetProcessHeap(), 0, seen.last);
    /* A record whose checksum is wrong is not read. */
    log[7 + 3] ^= 1;
    ZeroMemory(&seen, sizeof seen);
    Core_LevelLogRecords(log, n + appended, SeeRecord, &seen, &cleanEnd);
    Check("a bad checksum skips its block, a later block still reads", seen.records == 1 && cleanEnd == n + appended &&
          seen.lastLength == batchLength && memcmp(seen.last, batch, batchLength) == 0);
    if (seen.last) HeapFree(GetProcessHeap(), 0, seen.last);
    /* A writer can pad an interrupted tail and append in the next block. */
    memset(log + n + appended + 20, 0, 65536 - (n + appended + 20));
    appended = Core_LevelLogAppend(65536, batch, batchLength, log + 65536, 2 * 32768);
    ZeroMemory(&seen, sizeof seen);
    Check("a good record appended after a corrupt tail survives reopening",
          Core_LevelLogRecords(log, 65536 + appended, SeeRecord, &seen, &cleanEnd) && seen.records == 2 && cleanEnd == 65536 + appended);
    if (seen.last) HeapFree(GetProcessHeap(), 0, seen.last);
    HeapFree(GetProcessHeap(), 0, log);
    HeapFree(GetProcessHeap(), 0, big);

    /* Tables: their entries with sequence and type, stored or Snappy-compressed. */
    for (i = 0; i < 2; i++) {
        size_t tableLength = MakeTable(table, i == 1);
        ZeroMemory(&seen, sizeof seen);
        Check(i ? "a table with a Snappy block reads its entries" : "a table reads its entries",
              Core_LevelTableRead(table, tableLength, SeeEntry, &seen) && seen.count == 2 && seen.sequence[0] == 5 && seen.put[0] &&
              strcmp(seen.key[0], "a") == 0 && strcmp(seen.value[0], "x") == 0 && seen.sequence[1] == 6 && !seen.put[1] &&
              strcmp(seen.key[1], "b") == 0);
        table[tableLength - 1] ^= 1;
        Check("a table without LevelDB's magic is refused", !Core_LevelTableRead(table, tableLength, SeeEntry, &seen));
    }

    /* A manifest's edit: the log, the next file, the last sequence, a table added and one gone. */
    {
        static const BYTE kEdit[] = { 1, 3, 'c', 'm', 'p', 2, 7, 3, 9, 4, 0xD2, 0x09, 7, 0, 5, 100, 1, 'a', 1, 'b', 6, 0, 3, 9, 0 };
        ZeroMemory(&manifest, sizeof manifest);
        ZeroMemory(&tables, sizeof tables);
        manifest.onTable = SeeTable;
        manifest.context = &tables;
        Check("a manifest's edit is read whole", Core_LevelManifestEdit(kEdit, sizeof kEdit, &manifest));
        Check("it names the log, the next file and the last sequence",
              manifest.hasLog && manifest.logNumber == 7 && manifest.nextFile == 9 && manifest.lastSequence == 1234);
        Check("it tells the table made live and the one gone",
              tables.addedCount == 1 && tables.added[0] == 5 && tables.goneCount == 1 && tables.gone[0] == 3);
        Check("an edit with a tag LevelDB does not write is refused", !Core_LevelManifestEdit((const BYTE *)"\x08\x01", 2, &manifest));
    }

    /* Chromium's Local Storage: keys and values. */
    length = Core_WebStorageKey("https://claude.ai", L"dframe-store", key, sizeof key);
    Check("a Local Storage key: the origin, a zero, a Latin-1 mark, the name",
          length == 1 + 17 + 1 + 1 + 12 && memcmp(key, "_https://claude.ai\0\1dframe-store", length) == 0);
    length = Core_WebStorageValue(L"caf\x00E9", 4, value, sizeof value);
    Check("a value every character of which fits Latin-1 is stored so", length == 5 && value[0] == 1 && value[4] == 0xE9);
    Check("and reads back", Core_WebStorageText(value, length, text, ARRAYSIZE(text), &n) && n == 4 && wcscmp(text, L"caf\x00E9") == 0);
    length = Core_WebStorageValue(L"\x672A\x5206\x7C7B", 3, value, sizeof value);
    Check("a value with other characters is UTF-16", length == 7 && value[0] == 0 && value[1] == 0x2A && value[2] == 0x67);
    Check("and reads back too", Core_WebStorageText(value, length, text, ARRAYSIZE(text), &n) && n == 3 && text[2] == 0x7C7B);
    Check("a value without its mark is refused", !Core_WebStorageText((const BYTE *)"\x02x", 2, text, ARRAYSIZE(text), &n));
}

#define JSON_ARG(text) text, sizeof text - 1   /* a literal and its length */

static BOOL CountElement(void *context, const char *value, size_t valueLen)
{
    (void)value;
    (void)valueLen;
    (*(int *)context)++;
    return TRUE;
}

static BOOL RegexFinds(const WCHAR *pattern, const WCHAR *text, BOOL ignoreCase)
{
    static CoreRegex rx;
    return Core_RegexCompile(&rx, pattern, ignoreCase) && Core_RegexSearch(&rx, text, wcslen(text));
}

static BOOL RegexTaken(const WCHAR *pattern)
{
    static CoreRegex rx;
    return Core_RegexCompile(&rx, pattern, FALSE);
}

/* The sessions view's search and filters: the regular expressions it
 * takes, a text in UTF-8 transcripts, a session's environment and pull
 * request as Claude's sidebar shows them. */
static void TestSessionSearch(void)
{
    static const char kPrs[] =
        "{\"sessionId\":\"local_1\",\"prs\":[{\"prNumber\":2,\"state\":\"MERGED\",\"dismissed\":true},"
        "{\"prNumber\":3,\"repo\":\"a/b\",\"state\":\"OPEN\",\"branch\":\"x]\"}]}";
    static const char kDraft[] = "{\"prs\":[{\"prNumber\":7,\"state\":\"OPEN\",\"isDraft\":true}]}";
    static const char kDismissed[] = "{\"prs\":[{\"prNumber\":2,\"state\":\"MERGED\",\"dismissed\":true}]}";
    static const char kTranscript[] = "{\"type\":\"user\",\"message\":{\"content\":\"Build the \xE4\xB8\xAD\xE6\x96\x87 Setup\"}}";
    int number = -1, elements = 0;
    Check("json: each element of an array, nested ones whole", Core_JsonEachElement(JSON_ARG(" [{\"id\":\"a\",\"x\":[1,2]}, \"s,]\", 3 ]"),
                                                                                   CountElement, &elements) && elements == 3);
    elements = 0;
    Check("json: an empty array has no element, an object is no array",
          Core_JsonEachElement(JSON_ARG("[]"), CountElement, &elements) && elements == 0 && !Core_JsonEachElement(JSON_ARG("{}"), CountElement, &elements));
    Check("regex: a literal is found anywhere", RegexFinds(L"setup", L"run the setup now", FALSE));
    Check("regex: case matters unless ignored", !RegexFinds(L"Setup", L"run the setup", FALSE) && RegexFinds(L"Setup", L"run the setup", TRUE));
    Check("regex: dot, star and plus", RegexFinds(L"a.c", L"xabcx", FALSE) && RegexFinds(L"ab*c", L"ac", FALSE) && !RegexFinds(L"ab+c", L"ac", FALSE));
    Check("regex: alternation and groups", RegexFinds(L"(foo|bar)baz", L"xbarbazx", FALSE) && !RegexFinds(L"(?:foo|bar)baz", L"bazbar", FALSE));
    Check("regex: classes, ranges and negation", RegexFinds(L"[0-9]+-[a-f]", L"id 42-b", FALSE) && !RegexFinds(L"[^a-z]", L"abc", FALSE));
    Check("regex: \\d \\w \\s and their capitals", RegexFinds(L"\\d\\d\\s\\w", L"x 12 a", FALSE) && !RegexFinds(L"\\D", L"123", FALSE) &&
                                                      RegexFinds(L"[\\s,]x", L"a,x", FALSE));
    Check("regex: anchors at the text's and a line's ends", RegexFinds(L"^bc", L"a\nbc", FALSE) && !RegexFinds(L"^bc", L"abc", FALSE) &&
                                                             RegexFinds(L"ab$", L"ab\ncd", FALSE) && !RegexFinds(L"a$", L"ab", FALSE));
    Check("regex: counted repeats", RegexFinds(L"^a{2,3}$", L"aaa", FALSE) && !RegexFinds(L"^a{2,3}$", L"aaaa", FALSE) &&
                                     RegexFinds(L"x{2}", L"axxb", FALSE) && RegexFinds(L"a{1,}b", L"aaab", FALSE));
    Check("regex: a brace that bounds nothing is a character", RegexFinds(L"a{b", L"xa{bx", FALSE));
    Check("regex: escaped specials are characters", RegexFinds(L"1\\.5\\(x\\)", L"v1.5(x)", FALSE) && !RegexFinds(L"1\\.5", L"105", FALSE));
    Check("regex: Chinese text and case-insensitive classes", RegexFinds(L"\x4E2D\x6587", L"\x5B66\x4E2D\x6587", FALSE) &&
                                                                RegexFinds(L"[A-C]x", L"bx", TRUE));
    Check("regex: an empty pattern matches", RegexFinds(L"", L"anything", FALSE));
    Check("regex: a pattern that would backtrack forever still answers", !RegexFinds(L"(a*)*b", L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", FALSE));
    Check("regex: malformed patterns are refused", !RegexTaken(L"(ab") && !RegexTaken(L"ab)") && !RegexTaken(L"[ab") &&
                                                    !RegexTaken(L"*a") && !RegexTaken(L"a{5,2}") && !RegexTaken(L"[z-a]"));
    Check("utf-8: found in either case", Core_ContainsUtf8(kTranscript, sizeof kTranscript - 1, "setup", 5, FALSE) &&
                                         !Core_ContainsUtf8(kTranscript, sizeof kTranscript - 1, "setup", 5, TRUE));
    Check("utf-8: Chinese bytes are found as they are", Core_ContainsUtf8(kTranscript, sizeof kTranscript - 1, "\xE4\xB8\xAD\xE6\x96\x87", 6, FALSE));
    Check("utf-8: what is not there is not found", !Core_ContainsUtf8(kTranscript, sizeof kTranscript - 1, "missing", 7, FALSE));
    Check("pull request: the latest one not dismissed", Core_SessionPullRequest(kPrs, sizeof kPrs - 1, &number) == PR_OPEN && number == 3);
    Check("pull request: a draft", Core_SessionPullRequest(kDraft, sizeof kDraft - 1, &number) == PR_DRAFT && number == 7);
    Check("pull request: none when all were dismissed", Core_SessionPullRequest(kDismissed, sizeof kDismissed - 1, &number) == PR_NONE && number == 0);
    Check("pull request: none without prs", Core_SessionPullRequest(JSON_ARG("{\"sessionId\":\"x\"}"), &number) == PR_NONE);
    Check("environment: this PC's", Core_SessionEnvironment(JSON_ARG("{\"sessionId\":\"x\"}")) == SESSION_ENV_LOCAL);
    Check("environment: Remote Control", Core_SessionEnvironment(JSON_ARG("{\"remoteControlUserEnabled\":true}")) == SESSION_ENV_REMOTE);
    Check("environment: SSH and the cloud", Core_SessionEnvironment(JSON_ARG("{\"sshConfig\":{\"host\":\"h\"}}")) == SESSION_ENV_SSH &&
                                            Core_SessionEnvironment(JSON_ARG("{\"movedToCloud\":true}")) == SESSION_ENV_CLOUD);
}

static void TestSessionMetadataHelpers(void)
{
    static const char titles[] = "[\"Old title\",\"\",\"\\u4e2d\\u6587\"]";
    WCHAR *list = Core_JsonStringList(titles, sizeof titles - 1);
    CoreRegex regex;
    size_t end = 0;
    Check("old titles decode as separate complete strings", list && wcscmp(list, L"Old title") == 0 &&
          wcscmp(list + wcslen(list) + 1, L"\x4e2d\x6587") == 0);
    if (list) HeapFree(GetProcessHeap(), 0, list);
    list = Core_JsonStringList("[\"a\",3]", 7);
    Check("non-string old titles rejected", list == NULL);
    if (list) HeapFree(GetProcessHeap(), 0, list);
    Check("https artifact accepted", Core_SafeHttpsUrl(L"https://claude.ai/public/artifacts/abc?view=1#part"));
    Check("http artifact rejected", !Core_SafeHttpsUrl(L"http://example.invalid/a"));
    Check("file artifact rejected", !Core_SafeHttpsUrl(L"file:///C:/sample.txt"));
    Check("javascript artifact rejected", !Core_SafeHttpsUrl(L"javascript:alert(1)"));
    Check("artifact credentials rejected", !Core_SafeHttpsUrl(L"https://user@example.invalid/a"));
    Check("artifact command switches rejected", !Core_SafeHttpsUrl(L"https://example.invalid/\" --argument"));
    Check("artifact controls rejected", !Core_SafeHttpsUrl(L"https://example.invalid/a\nnext"));
    Check("artifact backslash rejected", !Core_SafeHttpsUrl(L"https://example.invalid\\a"));
    Check("artifact empty authority rejected", !Core_SafeHttpsUrl(L"https:///a"));
    Check("regex first match end preserved", Core_RegexCompile(&regex, L"Beta[0-9]+", FALSE) &&
          Core_RegexSearchEnd(&regex, L"Alpha Beta12 later", 18, &end) && end >= 11 && end <= 12);
}

/* The shield's facts: the regions Claude serves, Cloudflare's trace, each
 * IP service's answer as it gives it (8.8.8.8's, read on 2026-10-24), a
 * STUN server's, and the rules on them. */
static void TestReleases(void)
{
    static const char list[] =
        "[{\"isDraft\":false,\"isLatest\":false,\"tagName\":\"build-143\"},{\"isDraft\":false,\"isLatest\":false,\"tagName\":\"v2026.10.25\"},"
        "{\"isDraft\":true,\"isLatest\":false,\"tagName\":\"v2026.12.01\"},{\"isDraft\":false,\"isLatest\":true,\"tagName\":\"v2026.10.26\"},"
        "{\"isDraft\":false,\"isLatest\":false,\"tagName\":\"v1.1.0\"}]";
    static const char noLatest[] = "[{\"tagName\":\"v2026.10.25\"},{\"tagName\":\"v2026.10.27\"},{\"tagName\":\"v2026.10.26\"}]";
    static const char refs[] =
        "1111111111111111111111111111111111111111\trefs/tags/build-143\n"
        "2222222222222222222222222222222222222222\trefs/tags/v2026.10.25\r\n"
        "3333333333333333333333333333333333333333\trefs/tags/v2026.10.26\n"
        "4444444444444444444444444444444444444444\trefs/tags/v2026.10.26^{}\n"
        "5555555555555555555555555555555555555555\trefs/heads/v2026.12.31\n"
        "6666666666666666666666666666666666666666\trefs/tags/v1.1.0";
    WCHAR tag[64], version[16], commit[48];
    Check("releases: a day's tag", Core_ReleaseVersion(L"v2026.10.26", version, ARRAYSIZE(version)) && wcscmp(version, L"2026.10.26") == 0);
    Check("releases: other tags are none", !Core_ReleaseVersion(L"build-143", version, ARRAYSIZE(version)) && version[0] == 0 &&
          !Core_ReleaseVersion(L"v1.1.0", NULL, 0) && !Core_ReleaseVersion(L"v2026.10.2", NULL, 0) && !Core_ReleaseVersion(L"2026.10.26", NULL, 0) &&
          !Core_ReleaseVersion(L"v2026.1a.26", NULL, 0) && !Core_ReleaseVersion(NULL, NULL, 0));
    Check("releases: the one marked latest, drafts and other tags left out", Core_NewestRelease(list, sizeof list - 1, tag, ARRAYSIZE(tag)) &&
          wcscmp(tag, L"v2026.10.26") == 0);
    Check("releases: none marked latest, the newest day's", Core_NewestRelease(noLatest, sizeof noLatest - 1, tag, ARRAYSIZE(tag)) &&
          wcscmp(tag, L"v2026.10.27") == 0);
    Check("releases: none", !Core_NewestRelease("[{\"tagName\":\"build-1\"}]", 23, tag, ARRAYSIZE(tag)) && tag[0] == 0 &&
          !Core_NewestRelease(NULL, 0, tag, ARRAYSIZE(tag)));
    Check("releases: the newest tag git lists, with the commit an annotated tag names",
          Core_NewestReleaseRef(refs, sizeof refs - 1, tag, ARRAYSIZE(tag), commit, ARRAYSIZE(commit)) && wcscmp(tag, L"v2026.10.26") == 0 &&
          wcscmp(commit, L"4444444444444444444444444444444444444444") == 0);
    {
        static const char light[] = "1111111111111111111111111111111111111111\trefs/tags/build-143\n"
                                    "2222222222222222222222222222222222222222\trefs/tags/v2026.10.25\r\n";
        Check("releases: a light tag's own commit", Core_NewestReleaseRef(light, sizeof light - 1, tag, ARRAYSIZE(tag), commit, ARRAYSIZE(commit)) &&
              wcscmp(tag, L"v2026.10.25") == 0 && wcscmp(commit, L"2222222222222222222222222222222222222222") == 0);
    }
    Check("releases: git lists none", !Core_NewestReleaseRef("abc\trefs/tags/v1\n", 17, tag, ARRAYSIZE(tag), commit, ARRAYSIZE(commit)) &&
          tag[0] == 0 && commit[0] == 0);
    Check("releases: commits alike cut short", Core_SameCommit(L"c9814e9", L"c9814E9a0b1c2d3e4f5a6b7c8d9e0f1a2b3c4d5e") &&
          !Core_SameCommit(L"c9814e9", L"ebdbf7e0000") && !Core_SameCommit(L"", L"c9814e9") && !Core_SameCommit(L"c98", L"c98"));
}

static void TestSearXng(void)
{
    WCHAR url[URL_CCH];
    char entry[2048];
    BOOL priv = TRUE;
    Check("searxng: an address as typed", Core_SearXngUrl(L"  https://searx.example.com/  ", url, ARRAYSIZE(url)) &&
          wcscmp(url, L"https://searx.example.com") == 0);
    Check("searxng: a path kept, a results page's /search dropped", Core_SearXngUrl(L"http://10.0.0.5:8080/sx/search", url, ARRAYSIZE(url)) &&
          wcscmp(url, L"http://10.0.0.5:8080/sx") == 0);
    Check("searxng: a bare host is https's", Core_SearXngUrl(L"searx.example.com/", url, ARRAYSIZE(url)) && wcscmp(url, L"https://searx.example.com") == 0);
    Check("searxng: another scheme, refused", !Core_SearXngUrl(L"ftp://searx.example.com", url, ARRAYSIZE(url)) && url[0] == 0);
    Check("searxng: no host, refused", !Core_SearXngUrl(L"https://", url, ARRAYSIZE(url)) && !Core_SearXngUrl(L"https:///x", url, ARRAYSIZE(url)));
    Check("searxng: a query, a quote or a space, refused", !Core_SearXngUrl(L"https://s.example.com/?q=1", url, ARRAYSIZE(url)) &&
          !Core_SearXngUrl(L"https://s.example.com/\"x", url, ARRAYSIZE(url)) && !Core_SearXngUrl(L"https://s.ex ample.com", url, ARRAYSIZE(url)));
    Check("searxng: the entry", Core_SearXngEntry(L"C:\\Program Files\\nodejs\\npx.cmd", L"https://searx.example.com", FALSE, entry, sizeof entry) &&
          strcmp(entry, "{\"command\":\"C:\\\\Program Files\\\\nodejs\\\\npx.cmd\",\"args\":[\"-y\",\"mcp-searxng\"],"
                        "\"env\":{\"SEARXNG_URL\":\"https://searx.example.com\"}}") == 0);
    Check("searxng: read back, private off", Core_SearXngServer(entry, strlen(entry), url, ARRAYSIZE(url), &priv) &&
          wcscmp(url, L"https://searx.example.com") == 0 && !priv);
    Check("searxng: the entry with private addresses", Core_SearXngEntry(L"npx.cmd", L"https://s", TRUE, entry, sizeof entry) &&
          strstr(entry, "\"MCP_HTTP_ALLOW_PRIVATE_URLS\":\"true\"") != NULL &&
          Core_SearXngServer(entry, strlen(entry), url, ARRAYSIZE(url), &priv) && priv);
    Check("searxng: a pinned version is ours too",
          Core_SearXngServer("{\"command\":\"npx\",\"args\":[\"-y\",\"mcp-searxng@0.9.1\"]}", sizeof "{\"command\":\"npx\",\"args\":[\"-y\",\"mcp-searxng@0.9.1\"]}" - 1, NULL, 0, NULL));
    Check("searxng: another server is not",
          !Core_SearXngServer("{\"command\":\"npx\",\"args\":[\"-y\",\"mcp-searxng-fork\"]}", sizeof "{\"command\":\"npx\",\"args\":[\"-y\",\"mcp-searxng-fork\"]}" - 1, url, ARRAYSIZE(url), &priv) &&
          url[0] == 0 && !priv);
    Check("searxng: no room, nothing", !Core_SearXngEntry(L"npx.cmd", L"https://s", FALSE, entry, 20) && entry[0] == 0);
}

/* Cowork is off only where Claude's preferences say false, as Claude reads them. */
static void TestCowork(void)
{
    static const char off[] = "{\"mcpServers\":{},\"preferences\":{\"menuBarEnabled\":true,\"secureVmFeaturesEnabled\":false}}";
    static const char on[] = "{\"preferences\":{\"secureVmFeaturesEnabled\":true}}";
    static const char elsewhere[] = "{\"secureVmFeaturesEnabled\":false,\"preferences\":{\"coworkWebSearchEnabled\":false}}";
    static const char quoted[] = "{\"preferences\":{\"secureVmFeaturesEnabled\":\"false\"}}";
    Check("cowork: off where the preference is false", Core_CoworkOff(off, sizeof off - 1));
    Check("cowork: on where it is true", !Core_CoworkOff(on, sizeof on - 1));
    Check("cowork: on without it, or with it outside the preferences", !Core_CoworkOff("{}", 2) && !Core_CoworkOff(elsewhere, sizeof elsewhere - 1));
    Check("cowork: a string \"false\" is not false", !Core_CoworkOff(quoted, sizeof quoted - 1));
}

static void TestShieldFacts(void)
{
    static const char kTrace[] = "fl=123f45\r\nh=claude.ai\r\nip=203.0.113.7\r\nts=1761300000.1\r\nvisit_scheme=https\r\nloc=jp\r\ntls=TLSv1.3\r\n";
    static const char kNetCoffee[] = "{\"ip\": \"8.8.8.8\", \"cidr\": \"8.8.8.0/24\", \"is_datacenter\": true, \"isResidential\": false, \"isBroadcast\": null, "
                                     "\"is_vpn\": true, \"is_proxy\": false, \"is_tor\": false, \"is_crawler\": false, \"is_abuser\": true, \"is_mobile\": false, "
                                     "\"company_type\": \"hosting\", \"company_name\": \"Google LLC\", \"asn\": 15169, \"asOrganization\": \"Google LLC\", "
                                     "\"country\": \"United States\", \"countryCode\": \"us\", \"region\": \"\", \"city\": \"\", \"timezone\": \"America/Los_Angeles\", "
                                     "\"trust_score\": 63}";
    static const char kIpApiIs[] = "{\"ip\":\"8.8.8.8\",\"is_datacenter\":true,\"is_tor\":false,\"is_proxy\":false,\"is_vpn\":\"NordVPN\",\"is_abuser\":false,"
                                   "\"is_mobile\":false,\"company\":{\"name\":\"Google LLC\",\"type\":\"hosting\"},\"asn\":{\"asn\":15169,\"org\":\"Google LLC\"},"
                                   "\"location\":{\"country\":\"United States\",\"country_code\":\"US\",\"state\":\"California\",\"city\":\"Mountain View\","
                                   "\"timezone\":\"America/Los_Angeles\"}}";
    static const char kIpApiCom[] = "{\"status\":\"success\",\"country\":\"United States\",\"countryCode\":\"US\",\"regionName\":\"Virginia\",\"city\":\"Ashburn\","
                                    "\"timezone\":\"America/New_York\",\"isp\":\"Google LLC\",\"org\":\"Google Public DNS\",\"as\":\"AS15169 Google LLC\","
                                    "\"mobile\":false,\"proxy\":true,\"hosting\":true}";
    static const char kProxyCheck[] = "{\"status\": \"ok\", \"8.8.8.8\": {\"asn\": \"AS15169\", \"provider\": \"Google LLC\", \"country\": \"United States\", "
                                      "\"isocode\": \"US\", \"region\": \"California\", \"timezone\": \"America/Los_Angeles\", \"city\": \"Mountain View\", "
                                      "\"proxy\": \"no\", \"type\": \"Residential\"}}";
    static const BYTE kTransaction[12] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    WCHAR ip[64], loc[8];
    CoreIpRisk risk;
    BYTE request[32], response[64];
    CoreShieldFacts facts;
    CoreShieldRules rules;
    size_t n;

    Check("shield: Claude works in the US, Japan, Taiwan, Ukraine", Core_ClaudeRegionSupported(L"US", NULL) && Core_ClaudeRegionSupported(L"jp", L"") &&
          Core_ClaudeRegionSupported(L"TW", NULL) && Core_ClaudeRegionSupported(L"UA", L"Kyiv City"));
    Check("shield: and in a listed country's territory", Core_ClaudeRegionSupported(L"PR", NULL) && Core_ClaudeRegionSupported(L"GU", NULL));
    Check("shield: not in China, Hong Kong, Macao, Russia", !Core_ClaudeRegionSupported(L"CN", NULL) && !Core_ClaudeRegionSupported(L"HK", NULL) &&
          !Core_ClaudeRegionSupported(L"MO", NULL) && !Core_ClaudeRegionSupported(L"ru", NULL));
    Check("shield: nor in Ukraine's occupied regions", !Core_ClaudeRegionSupported(L"UA", L"Autonomous Republic of Crimea") &&
          !Core_ClaudeRegionSupported(L"UA", L"Donetsk Oblast") && !Core_ClaudeRegionSupported(L"UA", L"Zaporizhzhia"));
    Check("shield: Cloudflare's own codes and odd ones are no region", !Core_ClaudeRegionSupported(L"T1", NULL) &&
          !Core_ClaudeRegionSupported(L"XX", NULL) && !Core_ClaudeRegionSupported(L"", NULL) && !Core_ClaudeRegionSupported(L"USA", NULL) &&
          !Core_ClaudeRegionSupported(L"S ", NULL) && !Core_ClaudeRegionSupported(NULL, NULL));

    Check("shield: the trace gives the IP and the country", Core_ParseTrace(kTrace, sizeof kTrace - 1, ip, ARRAYSIZE(ip), loc, ARRAYSIZE(loc)) &&
          wcscmp(ip, L"203.0.113.7") == 0 && wcscmp(loc, L"JP") == 0);
    Check("shield: an IPv6 trace", Core_ParseTrace("ip=2001:db8::1\nloc=US", 21, ip, ARRAYSIZE(ip), loc, ARRAYSIZE(loc)) &&
          wcscmp(ip, L"2001:db8::1") == 0 && wcscmp(loc, L"US") == 0);
    Check("shield: no IP, or one with other characters, is none", !Core_ParseTrace("h=claude.ai\nloc=US\n", 19, ip, ARRAYSIZE(ip), loc, ARRAYSIZE(loc)) &&
          !Core_ParseTrace("ip=1.2.3.4<x\n", 13, ip, ARRAYSIZE(ip), loc, ARRAYSIZE(loc)));

    Check("shield: ip.net.coffee's answer", Core_ParseIpRisk(SHIELD_SOURCE_NETCOFFEE, kNetCoffee, sizeof kNetCoffee - 1, L"8.8.8.8", &risk) &&
          wcscmp(risk.countryCode, L"US") == 0 && risk.trustScore == 63 && risk.datacenter == 1 && risk.residential == 0 && risk.vpn == 1 &&
          risk.proxy == 0 && risk.tor == 0 && risk.abuser == 1 && risk.asn == 15169 && wcscmp(risk.isp, L"Google LLC") == 0 &&
          wcscmp(risk.timezone, L"America/Los_Angeles") == 0);
    Check("shield: ipapi.is's answer, a flag given as a name", Core_ParseIpRisk(SHIELD_SOURCE_IPAPIIS, kIpApiIs, sizeof kIpApiIs - 1, L"8.8.8.8", &risk) &&
          wcscmp(risk.countryCode, L"US") == 0 && wcscmp(risk.region, L"California") == 0 && risk.trustScore == -1 && risk.datacenter == 1 &&
          risk.residential == 0 && risk.vpn == 1 && risk.tor == 0 && risk.asn == 15169);
    Check("shield: ip-api.com's answer", Core_ParseIpRisk(SHIELD_SOURCE_IPAPICOM, kIpApiCom, sizeof kIpApiCom - 1, L"8.8.8.8", &risk) &&
          wcscmp(risk.city, L"Ashburn") == 0 && risk.datacenter == 1 && risk.proxy == 1 && risk.vpn == -1 && risk.tor == -1 && risk.asn == 15169);
    Check("shield: proxycheck.io's answer, keyed by the IP", Core_ParseIpRisk(SHIELD_SOURCE_PROXYCHECK, kProxyCheck, sizeof kProxyCheck - 1, L"8.8.8.8", &risk) &&
          wcscmp(risk.countryCode, L"US") == 0 && risk.residential == 1 && risk.datacenter == 0 && risk.proxy == 0 && risk.asn == 15169);
    Check("shield: an answer for another IP, a failure or no JSON is none",
          !Core_ParseIpRisk(SHIELD_SOURCE_PROXYCHECK, kProxyCheck, sizeof kProxyCheck - 1, L"1.1.1.1", &risk) &&
          !Core_ParseIpRisk(SHIELD_SOURCE_IPAPICOM, "{\"status\":\"fail\",\"message\":\"private range\"}", 43, L"10.0.0.1", &risk) &&
          !Core_ParseIpRisk(SHIELD_SOURCE_NETCOFFEE, "<html>", 6, L"8.8.8.8", &risk) && !Core_ParseIpRisk(SHIELD_SOURCES, "{}", 2, L"8.8.8.8", &risk));

    n = Core_StunRequest(request, sizeof request, kTransaction);
    Check("shield: a STUN Binding request", n == 20 && request[0] == 0 && request[1] == 1 && request[2] == 0 && request[3] == 0 &&
          request[4] == 0x21 && request[7] == 0x42 && memcmp(request + 8, kTransaction, 12) == 0);
    /* 203.0.113.5:54321 as XOR-MAPPED-ADDRESS, after an attribute of another kind. */
    memset(response, 0, sizeof response);
    response[0] = 0x01; response[1] = 0x01; response[3] = 20;
    memcpy(response + 4, request + 4, 16);
    response[20] = 0x80; response[21] = 0x22; response[23] = 4; memcpy(response + 24, "test", 4);   /* SOFTWARE */
    response[28] = 0x00; response[29] = 0x20; response[31] = 8;
    response[33] = 0x01; response[34] = (BYTE)(0xD4 ^ 0x21); response[35] = (BYTE)(0x31 ^ 0x12);
    response[36] = (BYTE)(203 ^ 0x21); response[37] = (BYTE)(0 ^ 0x12); response[38] = (BYTE)(113 ^ 0xA4); response[39] = (BYTE)(5 ^ 0x42);
    Check("shield: a STUN response gives the address the server saw", Core_StunMappedAddress(response, 40, kTransaction, ip, ARRAYSIZE(ip)) &&
          wcscmp(ip, L"203.0.113.5") == 0);
    response[19] ^= 0xFF;
    Check("shield: another request's response is none", !Core_StunMappedAddress(response, 40, kTransaction, ip, ARRAYSIZE(ip)));
    response[19] ^= 0xFF;
    Check("shield: a response cut short is none", !Core_StunMappedAddress(response, 36, kTransaction, ip, ARRAYSIZE(ip)));

    ZeroMemory(&facts, sizeof facts);
    ZeroMemory(&rules, sizeof rules);
    facts.exitKnown = facts.supported = facts.riskKnown = facts.profileAllowed = TRUE;
    facts.trustScore = 90;
    facts.datacenter = facts.dnsLeak = facts.udpLeak = facts.ipv6Leak = 0;
    facts.tunUp = facts.proxyApp = 1;
    rules.allowIpv6Leak = TRUE;
    Check("shield: a clean exit stops nothing and concerns nothing", Core_ShieldVerdict(&facts, &rules) == 0 && Core_ShieldConcerns(&facts) == 0);
    facts.supported = FALSE;
    Check("shield: an exit where Claude does not work stops it", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_REGION);
    facts.supported = TRUE;
    facts.datacenter = 1;
    Check("shield: a datacenter IP stops Claude unless allowed", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_DATACENTER);
    rules.allowDatacenter = TRUE;
    Check("shield: allowed, it is only a concern", Core_ShieldVerdict(&facts, &rules) == 0 && Core_ShieldConcerns(&facts) == SHIELD_STOP_DATACENTER);
    facts.datacenter = 0;
    facts.dnsLeak = facts.udpLeak = 1;
    rules.allowUdpLeak = TRUE;
    Check("shield: a DNS leak stops Claude, an allowed UDP one does not", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_DNS);
    facts.dnsLeak = facts.udpLeak = 0;
    rules.minScore = 95;
    Check("shield: under the lowest score allowed", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_SCORE);
    facts.trustScore = -1;
    Check("shield: a score no one gave stops nothing without the unknown rule", Core_ShieldVerdict(&facts, &rules) == 0);
    rules.stopWhenUnknown = TRUE;
    Check("shield: with it, it does", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_UNKNOWN);
    rules.minScore = 0;
    facts.exitKnown = FALSE;
    facts.tunUp = 0;
    rules.stopWhenTunDown = TRUE;
    Check("shield: no exit read and the TUN adapter down", Core_ShieldVerdict(&facts, &rules) == (SHIELD_STOP_UNKNOWN | SHIELD_STOP_TUN) &&
          Core_ShieldConcerns(&facts) == SHIELD_STOP_UNKNOWN);
    rules.stopWhenUnknown = FALSE;
    Check("shield: no exit read, without the unknown rule, stops only for the TUN adapter", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_TUN);

    ZeroMemory(&facts, sizeof facts);
    ZeroMemory(&rules, sizeof rules);
    facts.exitKnown = facts.supported = facts.riskKnown = facts.profileAllowed = TRUE;
    facts.trustScore = 90;
    facts.datacenter = facts.dnsLeak = facts.udpLeak = facts.ipv6Leak = 0;
    facts.tunUp = facts.proxyApp = 1;
    rules.allowIpv6Leak = FALSE;
    facts.ipv6Leak = 1;
    Check("shield: a public IPv6 that could leak stops Claude unless allowed", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_IPV6);
    rules.warnOnly = TRUE;
    Check("shield: warn-only turns every stop into none, red but not ending Claude",
          Core_ShieldVerdict(&facts, &rules) == 0 && (Core_ShieldConcerns(&facts) & SHIELD_STOP_IPV6));
    rules.warnOnly = FALSE;
    rules.allowIpv6Leak = TRUE;
    facts.ipv6Leak = 0;
    facts.profileAllowed = FALSE;
    Check("shield: an exit outside the profile's allowed regions stops it", Core_ShieldVerdict(&facts, &rules) == SHIELD_STOP_PROFILE);
    facts.profileAllowed = TRUE;
    facts.tzMismatch = facts.langMismatch = facts.cfSplit = 1;
    Check("shield: a time-zone, language or split-exit mismatch is a concern, never a stop",
          Core_ShieldVerdict(&facts, &rules) == 0 &&
          (Core_ShieldConcerns(&facts) & (SHIELD_CONCERN_TZ | SHIELD_CONCERN_LANG | SHIELD_CONCERN_SPLIT)) ==
          (SHIELD_CONCERN_TZ | SHIELD_CONCERN_LANG | SHIELD_CONCERN_SPLIT));

    Check("shield: a time zone's offset, by IANA name or country", Core_TimezoneOffsetMinutes(L"America/Los_Angeles") == -480 &&
          Core_TimezoneOffsetMinutes(L"US") == -300 && Core_TimezoneOffsetMinutes(L"Asia/Shanghai") == 480 &&
          Core_TimezoneOffsetMinutes(L"JP") == 540 && Core_TimezoneOffsetMinutes(L"Asia/Kolkata") == 330);
    Check("shield: an unknown zone has no offset", Core_TimezoneOffsetMinutes(L"Mars/Olympus") == INT_MIN && Core_TimezoneOffsetMinutes(L"") == INT_MIN);
    Check("shield: a region's languages", Core_RegionLanguageMatches(L"JP", L"ja") && Core_RegionLanguageMatches(L"CA", L"fr") &&
          Core_RegionLanguageMatches(L"US", L"en-US") && !Core_RegionLanguageMatches(L"JP", L"en") && !Core_RegionLanguageMatches(L"CN", L"en"));
    Check("shield: a region not in the table claims no language mismatch", Core_RegionLanguageMatches(L"ZZ", L"en") && Core_RegionLanguageMatches(L"US", NULL));
    Check("shield: a profile's allowed regions", Core_RegionAllowed(L"US", L"US, JP") && Core_RegionAllowed(L"jp", L"US,JP") &&
          !Core_RegionAllowed(L"CN", L"US JP") && Core_RegionAllowed(L"CN", L"") && Core_RegionAllowed(L"CN", NULL));
    Check("shield: a list being typed, or holding no whole code, allows any region",
          Core_RegionAllowed(L"US", L"U") && Core_RegionAllowed(L"CN", L"USA") && Core_RegionAllowed(L"CN", L" , ") &&
          !Core_RegionAllowed(L"CN", L"U, JP"));
    Check("shield: a list of regions is two-letter codes only, or nothing",
          Core_RegionCodes(L"US, jp;TW") && Core_RegionCodes(L"US\x3001JP\xFF0CTW") && Core_RegionAllowed(L"JP", L"US\x3001JP") &&
          Core_RegionCodes(L"") && Core_RegionCodes(L" , ") && Core_RegionCodes(NULL) &&
          !Core_RegionCodes(L"USA") && !Core_RegionCodes(L"US, J") && !Core_RegionCodes(L"\x7F8E\x56FD") && !Core_RegionCodes(L"U1"));
}

/* Electron's name for a profile's zoom levels: libc++'s CityHash64 of its folder, each length's branch. */
static BOOL ZoomKeyIs(const WCHAR *folder, const char *expected)
{
    char key[24];
    return Core_ZoomPartitionKey(folder, key, sizeof key) && strcmp(key, expected) == 0;
}

/* IndexedDB's coding, as Claude's Chromium writes it: a record of idb-keyval's store (measured on 3.6556). */
static void TestIndexedDbCoding(void)
{
    static const BYTE kRecord[] = { 0xCB, 0x09, 0xFF, 0x15, 0xFE, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0x10, '"', 0x0B,
                                    '{', '"', 's', 't', 'a', 't', 'e', '"', ':', '1', '}' };
    static const BYTE kKey[] = { 0x01, 0x02, 0x00, 'a', 0x00, 'b' };
    static const BYTE kPrefix[] = { 0x00, 0x01, 0x01, 0x01 }, kWide[] = { 0x20, 0x34, 0x12, 0x05, 0x02 };
    BYTE out[128], header[32], string[64];
    WCHAR name[64], text[64];
    ULONGLONG ids[3], version, database, store;
    size_t at, headerAt, textAt, start, bytes, n;
    int kind;
    BYTE tag;
    Check("indexeddb: a prefix, each id in as few bytes as it takes",
          Core_IdbPrefix(1, 1, 1, out, sizeof out) == sizeof kPrefix && memcmp(out, kPrefix, sizeof kPrefix) == 0 &&
          Core_IdbPrefix(0x1234, 5, 2, out, sizeof out) == sizeof kWide && memcmp(out, kWide, sizeof kWide) == 0 &&
          Core_IdbPrefixRead(out, sizeof kWide, ids, &at) && ids[0] == 0x1234 && ids[1] == 5 && ids[2] == 2 && at == sizeof kWide &&
          !Core_IdbPrefix(1, 1, 0x100000000ULL, out, sizeof out));
    Check("indexeddb: a string key, and versions little-endian", Core_IdbStringKey(L"ab", out, sizeof out) == sizeof kKey &&
          memcmp(out, kKey, sizeof kKey) == 0 && Core_IdbInt(1227, out) == 2 && out[0] == 0xCB && out[1] == 0x04 &&
          Core_IdbInt(0, out) == 1 && out[0] == 0 && Core_IdbIntRead((const BYTE *)"\xCB\x04", 2) == 1227);
    /* A database's name: 0, 0, 0, 201, the origin, the name (each a string key without its type). */
    n = Core_IdbPrefix(0, 0, 0, out, sizeof out);
    out[n++] = 201;
    at = Core_IdbStringKey(L"https_claude.ai_0@1", string, sizeof string);
    memcpy(out + n, string + 1, at - 1);
    n += at - 1;
    at = Core_IdbStringKey(L"keyval-store", string, sizeof string);
    memcpy(out + n, string + 1, at - 1);
    n += at - 1;
    Check("indexeddb: a database's name", Core_IdbDatabaseName(out, n, name, ARRAYSIZE(name)) && wcscmp(name, L"keyval-store") == 0 &&
          !Core_IdbDatabaseName(out, n - 1, name, ARRAYSIZE(name)));
    n = Core_IdbStoreMetaKey(5, 1, 4, out, sizeof out);
    Check("indexeddb: an object store's metadata", n == 7 && Core_IdbStoreMeta(out, n, &database, &store, &kind) && database == 5 && store == 1 &&
          kind == 4 && !Core_IdbStoreMeta(kPrefix, sizeof kPrefix, &database, &store, &kind));
    Check("indexeddb: a record's string read, past Blink's and V8's headers",
          Core_IdbValueString(kRecord, sizeof kRecord, &version, &headerAt, &textAt, &tag, &start, &bytes) && version == 1227 && headerAt == 2 &&
          textAt == 19 && tag == '"' && bytes == 11 && memcmp(kRecord + start, "{\"state\":1}", 11) == 0 &&
          !Core_IdbValueString(kRecord, sizeof kRecord - 1, &version, &headerAt, &textAt, &tag, &start, &bytes));
    memcpy(header, kRecord + headerAt, textAt - headerAt);
    header[textAt - headerAt] = 0;   /* a padding byte, left out */
    StringCchCopyW(text, ARRAYSIZE(text), L"{\"state\":1}");
    n = Core_IdbValueWrite(1227, header, textAt - headerAt + 1, text, wcslen(text), out, sizeof out);
    Check("indexeddb: a record written as Chromium writes it, Latin-1 only", n == sizeof kRecord && memcmp(out, kRecord, n) == 0 &&
          !Core_IdbValueWrite(1, header, textAt - headerAt, L"\x5DE5", 1, out, sizeof out));
}

/* The manager's shortcut under its earlier name, which the manager deletes, is never its own. */
static void TestEarlierName(void)
{
    Check("the manager's earlier name is not its name (the shortcut deleted would be its own)", _wcsicmp(APP_EARLIER_NAME, APP_NAME) != 0);
}

static void TestZoomPartitionKey(void)
{
    char key[24];
    Check("zoom key: up to 16 bytes", ZoomKeyIs(L"a", "9382670497875102604") && ZoomKeyIs(L"ab", "3838761676342601430") &&
          ZoomKeyIs(L"abcdef", "9245708179745019710"));
    Check("zoom key: 17 to 64 bytes", ZoomKeyIs(L"Claude-Work1", "16790056513377664451") && ZoomKeyIs(L"Profiles\\Claude-Work", "14762905009297811195"));
    Check("zoom key: a profile's folder", ZoomKeyIs(L"C:\\Users\\User\\AppData\\Roaming\\Claude-Work", "472582469390467225") &&
          ZoomKeyIs(L"D:\\Data\\Claude\\\x5DE5\x4F5C\\Claude-\x5DE5\x4F5C-Profile-With-A-Long-Name-0123456789", "341076873408563783"));
    Check("zoom key: no folder, none", !Core_ZoomPartitionKey(L"", key, sizeof key) && !Core_ZoomPartitionKey(NULL, key, sizeof key));
}

int wmain(void)
{
    Check("session order: natural names ascend", Core_CompareSessionOrder(L"Chat 2", 100, L"Chat 10", 200, TRUE, FALSE) < 0);
    Check("session order: natural names descend", Core_CompareSessionOrder(L"Chat 2", 100, L"Chat 10", 200, TRUE, TRUE) > 0);
    Check("session order: times ascend", Core_CompareSessionOrder(L"Z", 100, L"A", 200, FALSE, FALSE) < 0);
    Check("session order: times descend", Core_CompareSessionOrder(L"Z", 100, L"A", 200, FALSE, TRUE) > 0);
    Check("session order: equal names follow the chosen time direction", Core_CompareSessionOrder(L"Chat", 100, L"chat", 200, TRUE, FALSE) < 0);
    Check("session order: identical keys are equal", Core_CompareSessionOrder(L"Chat", 100, L"chat", 100, TRUE, TRUE) == 0);
    {
        WCHAR detail[32];
        Check("last result is tied to the latest assistant", Core_SessionResult(JSON_ARG("{\"lastAssistantUuid\":\"a\",\"postTurnSummaryFor\":\"a\",\"postTurnSummary\":{\"summarizes_uuid\":\"a\",\"status_category\":\"completed\",\"status_detail\":\"Done\"}}"), detail, ARRAYSIZE(detail)) == RESULT_COMPLETED && wcscmp(detail, L"Done") == 0);
        Check("stale summaries are unknown", Core_SessionResult(JSON_ARG("{\"lastAssistantUuid\":\"b\",\"postTurnSummary\":{\"summarizes_uuid\":\"a\",\"status_category\":\"completed\"}}"), detail, ARRAYSIZE(detail)) == RESULT_UNKNOWN && !detail[0]);
        Check("conflicting summary association is unknown", Core_SessionResult(JSON_ARG("{\"lastAssistantUuid\":\"a\",\"postTurnSummaryFor\":\"b\",\"postTurnSummary\":{\"summarizes_uuid\":\"a\",\"status_category\":\"completed\"}}"), detail, ARRAYSIZE(detail)) == RESULT_UNKNOWN);
        Check("a blocked result is recognized", Core_SessionResult(JSON_ARG("{\"lastAssistantUuid\":\"a\",\"postTurnSummary\":{\"summarizes_uuid\":\"a\",\"status_category\":\"blocked\"}}"), detail, ARRAYSIZE(detail)) == RESULT_BLOCKED);
        Check("unknown result categories stay unknown", Core_SessionResult(JSON_ARG("{\"lastAssistantUuid\":\"a\",\"postTurnSummary\":{\"summarizes_uuid\":\"a\",\"status_category\":\"future\"}}"), detail, ARRAYSIZE(detail)) == RESULT_UNKNOWN);
        Check("missing summaries stay unknown", Core_SessionResult(JSON_ARG("{}"), detail, ARRAYSIZE(detail)) == RESULT_UNKNOWN);
        {
            static const char summary[] = "{\"lastAssistantUuid\":\"a\",\"postTurnSummary\":{\"summarizes_uuid\":\"a\",\"status_category\":\"completed\",\"status_detail\":\"A long summary that exceeds the destination buffer without disappearing\"}}";
            Check("long summaries retain a bounded prefix", Core_SessionResult(summary, sizeof summary - 1, detail, ARRAYSIZE(detail)) == RESULT_COMPLETED &&
                  wcsncmp(detail, L"A long summary", 14) == 0 && wcslen(detail) == ARRAYSIZE(detail) - 1);
        }
    }
    TestSessionSearch();
    TestJsonMerge();
    TestUnreadMarks();
    TestJsonArrayUnion();
    TestSafeStorage();
    TestTokenCache();
    TestLevelDb();
    TestSessionEntries();
    TestSessionMetadataHelpers();
    TestTranscriptLines();
    TestChangelogRead();
    TestMarkdownRtf();
    TestSealedSecret();
    TestSessionEdits();
    TestSessionSync();
    TestMirror();
    TestSessionsKeptTheSame();
    TestDrawingMath();
    TestDeflate();
    TestProcessTree();
    TestLaunchArgs();
    TestPrivateSignIn();
    TestSanitizeUrl();
    TestNames();
    TestLogParsing();
    TestQuitParsing();
    TestRouting();
    TestShortcuts();
    TestMisc();
    TestPathsAndTimes();
    TestProfileFilePaths();
    TestJsonAndVersions();
    TestShieldFacts();
    TestZoomPartitionKey();
    TestIndexedDbCoding();
    TestEarlierName();
    TestSearXng();
    TestCowork();
    TestReleases();
    printf("Core tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
