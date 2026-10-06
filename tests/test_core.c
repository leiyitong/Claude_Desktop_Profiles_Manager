/*
 * Unit tests for src/core.c (the pure helpers). Built and run by build.cmd;
 * exits non-zero when a check fails.
 */
#include "../src/app.h"
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
}

static void TestMisc(void)
{
    WCHAR a[64], b[64];
    Check("path equals ignores case and trailing slash", Core_PathEquals(L"C:\\Users\\X\\Claude\\", L"c:\\users\\x\\claude"));
    Check("path equals is not a prefix test", !Core_PathEquals(L"C:\\Users\\X\\Claude", L"C:\\Users\\X\\Claude-Work"));
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
        added.op = PENDING_STAR;
        Check("pending: a star leaves a title queued", !Core_PendingReplaces(&queued, &added));
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
          back.time == 5 && !back.content[0]);
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

int wmain(void)
{
    TestSessionEntries();
    TestSessionEdits();
    TestSessionSync();
    TestDrawingMath();
    TestDeflate();
    TestProcessTree();
    TestLaunchArgs();
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
    printf("Core tests: %d checks, %d failure(s).\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
