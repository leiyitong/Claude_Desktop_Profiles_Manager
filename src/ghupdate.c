/*
 * Releases of main, through what this PC signs in to GitHub with. The
 * repository is private and the people who run this copy are its developers.
 * CI publishes one release a day, tagged and named after it (v2026.10.26),
 * the latest; each push of the day replaces it with its build. When the
 * manager opens, and when its version is clicked, that release is looked up
 * and offered when it was built from another commit than this copy, not on
 * an older day (build.cmd writes this build's number, commit and repository
 * into buildinfo.h; a local build has number 0 and is offered the release
 * only when asked).
 *
 * The sign-in, read on this PC only (no request is made for it):
 *   - GitHub's CLI, gh, signed in: it lists the releases, gives the newest
 *     one's commit and notes and downloads its exe;
 *   - else the GitHub credential git keeps (Git Credential Manager): gh, when
 *     installed, runs with it as GH_TOKEN; without gh, `git ls-remote` finds
 *     the newest release's tag and commit, and Update opens its page;
 *   - else none: this copy was brought here from elsewhere (GhUpdate_Copied).
 *     The version shows so, in red, and nothing is offered.
 * The credential's value stays in memory, is given to gh only, and is wiped
 * once used; nothing of it is logged.
 *
 * Updating downloads the release's exe into the state folder and runs it
 * with --install, as Update_Run does a signed release: it installs itself
 * over this copy. gh and git run hidden, their prompts off, each call given
 * a deadline. Nothing runs on a timer.
 */
#include "app.h"
#include "buildinfo.h"

#ifndef APP_BUILD_NUMBER
#define APP_BUILD_NUMBER 0
#endif
#ifndef APP_BUILD_COMMIT
#define APP_BUILD_COMMIT L""
#endif
#ifndef APP_BUILD_REPO
#define APP_BUILD_REPO L""
#endif

#define TOOL_QUICK_MS     20000          /* a sign-in read from this PC */
#define TOOL_LIST_MS      60000
#define TOOL_DOWNLOAD_MS  (10 * 60 * 1000)
#define TOOL_OUTPUT_MAX   (4u << 20)
#define GH_RELEASES       40             /* the newest releases looked at */
#define GH_UPDATE_DIR     L"gh-update"   /* in the state folder: a release downloaded, in a folder of its tag */
#define TOKEN_CCH         512

static volatile LONG g_copied;           /* no sign-in to GitHub on this PC: a copy brought from elsewhere */

int GhUpdate_OwnBuild(void)
{
    return APP_BUILD_NUMBER;
}

const WCHAR *GhUpdate_OwnCommit(void)
{
    return APP_BUILD_COMMIT;
}

/* The repository this copy was built from; the program's own when the build did not say. */
const WCHAR *GhUpdate_Repo(void)
{
    return APP_BUILD_REPO[0] ? APP_BUILD_REPO : APP_REPO;
}

BOOL GhUpdate_Copied(void)
{
    return InterlockedCompareExchange(&g_copied, 0, 0) != 0;
}

static BOOL ToolIn(const WCHAR *variable, const WCHAR *below, WCHAR *out, size_t cch)
{
    WCHAR base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(variable, base, ARRAYSIZE(base));
    return n && n < ARRAYSIZE(base) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s%s", base, below)) && Util_FileExists(out);
}

static BOOL OnPath(const WCHAR *name, WCHAR *out, size_t cch)
{
    WCHAR *file = NULL;
    DWORD n = SearchPathW(NULL, name, NULL, (DWORD)cch, out, &file);
    return n && n < cch;
}

BOOL GhUpdate_FindGh(WCHAR *out, size_t cch)
{
    /* Where its installer, winget and Scoop put it, should the PATH not name it. */
    return OnPath(L"gh.exe", out, cch) || ToolIn(L"ProgramFiles", L"\\GitHub CLI\\gh.exe", out, cch) ||
           ToolIn(L"LOCALAPPDATA", L"\\Programs\\GitHub CLI\\gh.exe", out, cch) ||
           ToolIn(L"LOCALAPPDATA", L"\\Microsoft\\WinGet\\Links\\gh.exe", out, cch) || ToolIn(L"USERPROFILE", L"\\scoop\\shims\\gh.exe", out, cch);
}

static BOOL FindGit(WCHAR *out, size_t cch)
{
    return OnPath(L"git.exe", out, cch) || ToolIn(L"ProgramFiles", L"\\Git\\cmd\\git.exe", out, cch) ||
           ToolIn(L"LOCALAPPDATA", L"\\Programs\\Git\\cmd\\git.exe", out, cch) || ToolIn(L"USERPROFILE", L"\\scoop\\shims\\git.exe", out, cch);
}

/* This process's environment, with gh's and git's prompts, notices and colors off, and `token` as GH_TOKEN when
 * there is one: a block for CreateProcess (Util_Free, wiped first when it holds a token). */
static WCHAR *ToolEnvironment(const WCHAR *token, size_t *bytes)
{
    static const WCHAR kQuiet[] = L"GH_PROMPT_DISABLED=1\0GH_NO_UPDATE_NOTIFIER=1\0GH_SPINNER_DISABLED=1\0NO_COLOR=1\0"
                                  L"GIT_TERMINAL_PROMPT=0\0GCM_INTERACTIVE=never\0GIT_ASKPASS=\0";
    WCHAR *strings = GetEnvironmentStringsW(), *block;
    const WCHAR *at;
    size_t length, tokenLength = token ? wcslen(token) + 10 : 0;   /* "GH_TOKEN=" and its NUL */
    if (!strings) return NULL;
    for (at = strings; *at; at += wcslen(at) + 1) {}
    length = (size_t)(at - strings);
    *bytes = (length + tokenLength + ARRAYSIZE(kQuiet) + 1) * sizeof(WCHAR);
    if ((block = (WCHAR *)Util_Alloc(*bytes)) != NULL) {
        memcpy(block, strings, length * sizeof(WCHAR));
        if (token) {
            StringCchPrintfW(block + length, tokenLength, L"GH_TOKEN=%s", token);
            length += tokenLength;
        }
        memcpy(block + length, kQuiet, sizeof kQuiet);   /* its own last NUL ends the block */
    }
    FreeEnvironmentStringsW(strings);
    return block;
}

/* `exe` run with `args`, hidden, `input` written to it (NULL: none), waited for `ms` at most: its exit code,
 * (DWORD)-1 when it could not start or was stopped at the deadline. What it wrote (its output and its errors)
 * into `*output` (Util_Free), NUL-ended. */
static DWORD RunTool(const WCHAR *exe, const WCHAR *args, const char *input, const WCHAR *token, DWORD ms, char **output, size_t *length)
{
    SECURITY_ATTRIBUTES inherit = { sizeof inherit, NULL, TRUE };
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    HANDLE readEnd = NULL, writeEnd = NULL, inRead = NULL, inWrite = NULL;
    WCHAR *command, *environment;
    size_t cch = wcslen(exe) + wcslen(args) + 4, used = 0, cap = 64 * 1024, environmentBytes = 0;
    char *buffer = (char *)Util_Alloc(cap + 1);
    ULONGLONG deadline = GetTickCount64() + ms;
    DWORD code = (DWORD)-1, available, got;
    BOOL running;
    *output = NULL;
    *length = 0;
    if (!buffer || (command = (WCHAR *)Util_Alloc(cch * sizeof(WCHAR))) == NULL) {
        Util_Free(buffer);
        return code;
    }
    StringCchPrintfW(command, cch, L"\"%s\" %s", exe, args);
    if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0) || !SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&inRead, &inWrite, &inherit, 0) || !SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0)) {
        if (readEnd) CloseHandle(readEnd);
        if (writeEnd) CloseHandle(writeEnd);
        if (inRead) CloseHandle(inRead);
        if (inWrite) CloseHandle(inWrite);
        Util_Free(command);
        Util_Free(buffer);
        return code;
    }
    ZeroMemory(&startup, sizeof startup);
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = startup.hStdError = writeEnd;
    environment = ToolEnvironment(token, &environmentBytes);
    running = CreateProcessW(exe, command, NULL, NULL, TRUE, CREATE_NO_WINDOW | (environment ? CREATE_UNICODE_ENVIRONMENT : 0), environment, NULL,
                             &startup, &process);
    if (!running) Util_Log(L"update: %s could not start (error %lu)", exe, GetLastError());
    CloseHandle(writeEnd);   /* the pipes end once the tool's own copies close */
    CloseHandle(inRead);
    if (running && input) WriteFile(inWrite, input, (DWORD)strlen(input), &got, NULL);
    CloseHandle(inWrite);
    if (environment) {
        SecureZeroMemory(environment, environmentBytes);
        Util_Free(environment);
    }
    Util_Free(command);
    if (running) {
        CloseHandle(process.hThread);
        for (;;) {
            BOOL done = WaitForSingleObject(process.hProcess, 50) == WAIT_OBJECT_0;
            while (PeekNamedPipe(readEnd, NULL, 0, NULL, &available, NULL) && available) {
                if (used == cap) {
                    char *bigger;
                    if (cap >= TOOL_OUTPUT_MAX || (bigger = (char *)Util_Alloc(cap * 2 + 1)) == NULL) break;
                    memcpy(bigger, buffer, used);
                    SecureZeroMemory(buffer, used);
                    Util_Free(buffer);
                    buffer = bigger;
                    cap *= 2;
                }
                if (!ReadFile(readEnd, buffer + used, (DWORD)min(available, cap - used), &got, NULL) || !got) break;
                used += got;
            }
            if (done) {
                GetExitCodeProcess(process.hProcess, &code);
                break;
            }
            if (GetTickCount64() >= deadline) {
                Util_Log(L"update: stopped after %lu ms: %s", ms, args);
                TerminateProcess(process.hProcess, 1);
                WaitForSingleObject(process.hProcess, 5000);
                code = (DWORD)-1;
                break;
            }
        }
        CloseHandle(process.hProcess);
    }
    CloseHandle(readEnd);
    buffer[used] = 0;
    *output = buffer;
    *length = used;
    return code;
}

static void FreeSecret(char *text, size_t length)
{
    if (!text) return;
    SecureZeroMemory(text, length);
    Util_Free(text);
}

/* Whether gh is signed in to github.com: the token it keeps, read from this PC only, not kept. */
static BOOL GhSignedIn(const WCHAR *gh)
{
    char *output = NULL;
    size_t length = 0;
    BOOL in = RunTool(gh, L"auth token --hostname github.com", NULL, NULL, TOOL_QUICK_MS, &output, &length) == 0 && length > 8;
    FreeSecret(output, length);
    return in;
}

/* The GitHub credential git keeps (`git credential fill`, its prompts off): its password, a token, into `token`. */
static BOOL GitToken(const WCHAR *git, WCHAR *token, size_t cch)
{
    char *output = NULL, *line, *end;
    size_t length = 0;
    BOOL found = FALSE;
    token[0] = 0;
    if (RunTool(git, L"credential fill", "protocol=https\nhost=github.com\n\n", NULL, TOOL_QUICK_MS, &output, &length) == 0 && output) {
        for (line = output; line && *line; line = end ? end + 1 : NULL) {
            int n;
            if ((end = strchr(line, '\n')) != NULL) *end = 0;
            if (strncmp(line, "password=", 9) != 0) continue;
            n = MultiByteToWideChar(CP_UTF8, 0, line + 9, -1, token, (int)cch);
            if (n > 1) {
                if (token[n - 2] == L'\r') token[n - 2] = 0;
                found = token[0] != 0;
            }
            break;
        }
    }
    FreeSecret(output, length);
    if (!found) SecureZeroMemory(token, cch * sizeof(WCHAR));
    return found;
}

/* How this PC signs in to GitHub, as the check and the download use it. */
typedef struct SignIn {
    WCHAR gh[MAX_PATH], git[MAX_PATH];
    BOOL  hasGh, ghIn;                   /* gh, and it signed in itself */
    WCHAR token[TOKEN_CCH];              /* git's credential, when gh is not signed in; "" none */
} SignIn;

static BOOL ReadSignIn(SignIn *in)
{
    ZeroMemory(in, sizeof *in);
    in->hasGh = GhUpdate_FindGh(in->gh, ARRAYSIZE(in->gh));
    if (in->hasGh && (in->ghIn = GhSignedIn(in->gh)) != FALSE) return TRUE;
    return FindGit(in->git, ARRAYSIZE(in->git)) && GitToken(in->git, in->token, ARRAYSIZE(in->token));
}

static void ForgetSignIn(SignIn *in)
{
    SecureZeroMemory(in->token, sizeof in->token);
}

/* gh's first line of what it said, for a person to read (its error, usually). */
static void FirstLine(const char *text, WCHAR *out, size_t cch)
{
    const char *end = text ? strpbrk(text, "\r\n") : NULL;
    int n;
    out[0] = 0;
    if (!text) return;
    n = MultiByteToWideChar(CP_UTF8, 0, text, end ? (int)(end - text) : -1, out, (int)cch - 1);
    out[n > 0 ? min((size_t)n, cch - 1) : 0] = 0;
}

/* A JSON string member, whole however long, into a new string (Util_Free); NULL when absent. */
static WCHAR *JsonText(const char *json, size_t length, const char *key)
{
    const char *value;
    size_t valueLength;
    WCHAR *text;
    if (!Core_JsonMember(json, length, key, &value, &valueLength) || (text = (WCHAR *)Util_Alloc((valueLength + 1) * sizeof(WCHAR))) == NULL)
        return NULL;
    if (!Core_JsonString(value, valueLength, text, valueLength + 1)) {
        Util_Free(text);
        return NULL;
    }
    return text;
}

/* Whether the release of `version`, built from `commit`, is offered to this copy: one of another commit, not of an
 * older day (a day's release is replaced by each of its builds, its tag kept); to a local build (number 0), only
 * when asked, as it may be the newer. */
static BOOL Offered(const WCHAR *version, const WCHAR *commit, BOOL asked)
{
    if (!commit[0] || Core_SameCommit(commit, APP_BUILD_COMMIT)) return FALSE;
    if (!GhUpdate_OwnBuild()) return asked;
    return wcscmp(version, APP_VERSION_DATE_W) >= 0;
}

static BOOL IsCommit(const WCHAR *text)
{
    size_t i;
    for (i = 0; text[i]; i++)
        if (!iswxdigit(text[i])) return FALSE;
    return i == 40;
}

/* The commit `tag` names, as GitHub's API resolves it. */
static void TagCommit(const SignIn *in, const WCHAR *token, const WCHAR *tag, WCHAR *commit, size_t cch)
{
    WCHAR args[256];
    char *output = NULL;
    size_t length = 0;
    commit[0] = 0;
    StringCchPrintfW(args, ARRAYSIZE(args), L"api repos/%s/commits/%s --jq .sha", GhUpdate_Repo(), tag);
    if (RunTool(in->gh, args, NULL, token, TOOL_LIST_MS, &output, &length) == 0) FirstLine(output, commit, cch);
    if (!IsCommit(commit)) commit[0] = 0;
    Util_Free(output);
}

/* The newest release, its commit, and when it is offered its title and notes, with gh signed in or given git's
 * credential. */
static void CheckWithGh(const SignIn *in, GhRelease *release, BOOL asked)
{
    const WCHAR *token = in->ghIn ? NULL : in->token;
    WCHAR args[512], *text;
    char *output = NULL;
    size_t length = 0;
    StringCchPrintfW(args, ARRAYSIZE(args), L"release list --repo %s --limit %d --json tagName,isDraft,isLatest", GhUpdate_Repo(), GH_RELEASES);
    if (RunTool(in->gh, args, NULL, token, TOOL_LIST_MS, &output, &length) != 0) {
        release->status = GH_FAILED;
        FirstLine(output, release->message, ARRAYSIZE(release->message));
    } else if (!Core_NewestRelease(output, length, release->tag, ARRAYSIZE(release->tag))) {
        release->status = GH_NONE;
    } else {
        Core_ReleaseVersion(release->tag, release->version, ARRAYSIZE(release->version));
        Util_Free(output);
        output = NULL;
        StringCchPrintfW(args, ARRAYSIZE(args), L"release view %s --repo %s --json name,body,targetCommitish", release->tag, GhUpdate_Repo());
        if (RunTool(in->gh, args, NULL, token, TOOL_LIST_MS, &output, &length) == 0) {
            if ((text = JsonText(output, length, "targetCommitish")) != NULL) {
                if (IsCommit(text)) StringCchCopyW(release->commit, ARRAYSIZE(release->commit), text);
                Util_Free(text);
            }
            if ((text = JsonText(output, length, "name")) != NULL) {
                StringCchCopyW(release->title, ARRAYSIZE(release->title), text);
                Util_Free(text);
            }
            release->notes = JsonText(output, length, "body");
        }
        if (!release->commit[0]) TagCommit(in, token, release->tag, release->commit, ARRAYSIZE(release->commit));
        release->status = Offered(release->version, release->commit, asked) ? GH_NEWER : GH_CURRENT;
    }
    Util_Free(output);
}

/* The newest release by its tag, git signing in with its own credential: no notes, and Update opens its page. */
static void CheckWithGit(const SignIn *in, GhRelease *release, BOOL asked)
{
    WCHAR args[512];
    char *output = NULL;
    size_t length = 0;
    release->viaGit = TRUE;
    StringCchPrintfW(args, ARRAYSIZE(args), L"ls-remote --tags https://github.com/%s.git", GhUpdate_Repo());
    if (RunTool(in->git, args, NULL, NULL, TOOL_LIST_MS, &output, &length) != 0) {
        release->status = GH_FAILED;
        FirstLine(output, release->message, ARRAYSIZE(release->message));
    } else if (!Core_NewestReleaseRef(output, length, release->tag, ARRAYSIZE(release->tag), release->commit, ARRAYSIZE(release->commit))) {
        release->status = GH_NONE;
    } else {
        Core_ReleaseVersion(release->tag, release->version, ARRAYSIZE(release->version));
        release->status = Offered(release->version, release->commit, asked) ? GH_NEWER : GH_CURRENT;
    }
    Util_Free(output);
}

typedef struct CheckJob { HWND notify; UINT message; BOOL asked; } CheckJob;

static DWORD WINAPI CheckThread(void *param)
{
    CheckJob *job = (CheckJob *)param;
    GhRelease *release = (GhRelease *)Util_Alloc(sizeof *release);
    SignIn in;
    BOOL signedIn;
    if (!release) {
        Util_Free(job);
        return 0;
    }
    release->local = !GhUpdate_OwnBuild();
    signedIn = ReadSignIn(&in);
    InterlockedExchange(&g_copied, signedIn ? 0 : 1);
    if (!signedIn) release->status = GH_COPIED;
    else if (in.hasGh) CheckWithGh(&in, release, job->asked);
    else CheckWithGit(&in, release, job->asked);
    if (!release->title[0] && release->version[0]) StringCchPrintfW(release->title, ARRAYSIZE(release->title), L"Claude Delta v%s", release->version);
    Util_Log(L"update: %s, newest %s (%.7s), this one %s (%s), signed in %s (%s)", GhUpdate_Repo(), release->version[0] ? release->version : L"none",
             release->commit, APP_VERSION_DATE_W, APP_BUILD_COMMIT[0] ? APP_BUILD_COMMIT : L"no commit",
             !signedIn ? L"nowhere" : in.ghIn ? L"with gh" : in.hasGh ? L"with git, through gh" : L"with git",
             release->status == GH_NEWER ? L"offered" : release->status == GH_CURRENT ? L"not newer" : release->status == GH_NONE ? L"none"
             : release->status == GH_COPIED ? L"a copy from elsewhere" : L"failed");
    ForgetSignIn(&in);
    if (!PostMessageW(job->notify, job->message, (WPARAM)job->asked, (LPARAM)release)) GhUpdate_Free(release);
    Util_Free(job);
    return 0;
}

void GhUpdate_Check(HWND notify, UINT message, BOOL asked)
{
    CheckJob *job = (CheckJob *)Util_Alloc(sizeof *job);
    HANDLE thread;
    if (!job) return;
    job->notify = notify;
    job->message = message;
    job->asked = asked;
    if ((thread = CreateThread(NULL, 0, CheckThread, job, 0, NULL)) != NULL) CloseHandle(thread);
    else Util_Free(job);
}

void GhUpdate_Free(GhRelease *release)
{
    if (!release) return;
    Util_Free(release->notes);
    Util_Free(release);
}

void GhUpdate_ReleasePage(const WCHAR *tag, WCHAR *out, size_t cch)
{
    StringCchPrintfW(out, cch, L"https://github.com/%s/releases/tag/%s", GhUpdate_Repo(), tag);
}

typedef struct DownloadJob { HWND notify; UINT message; WCHAR tag[GH_TAG_CCH]; } DownloadJob;

/* `tag`'s folder for its download, in the state folder: made empty. */
static BOOL DownloadDir(const WCHAR *tag, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    if (!Util_StateDir(state, ARRAYSIZE(state)) || FAILED(StringCchPrintfW(out, cch, L"%s\\" GH_UPDATE_DIR L"\\%s", state, tag))) return FALSE;
    Util_DeleteTree(out, NULL);
    return Util_EnsureDir(out);
}

static DWORD WINAPI DownloadThread(void *param)
{
    DownloadJob *job = (DownloadJob *)param;
    GhDownload *result = (GhDownload *)Util_Alloc(sizeof *result);
    WCHAR dir[LONG_PATH_CCH], args[LONG_PATH_CCH + 256];
    char *output = NULL;
    size_t length = 0;
    SignIn in;
    if (!result) {
        Util_Free(job);
        return 0;
    }
    if (!ReadSignIn(&in) || !in.hasGh) {
        StringCchCopyW(result->message, ARRAYSIZE(result->message), L"gh");
    } else if (!DownloadDir(job->tag, dir, ARRAYSIZE(dir))) {
        StringCchPrintfW(result->message, ARRAYSIZE(result->message), L"error %lu", GetLastError());
    } else {
        StringCchPrintfW(args, ARRAYSIZE(args), L"release download %s --repo %s --pattern " APP_EXE L" --dir \"%s\" --clobber", job->tag,
                         GhUpdate_Repo(), dir);
        if (RunTool(in.gh, args, NULL, in.ghIn ? NULL : in.token, TOOL_DOWNLOAD_MS, &output, &length) != 0) {
            FirstLine(output, result->message, ARRAYSIZE(result->message));
        } else if (SUCCEEDED(StringCchPrintfW(result->path, ARRAYSIZE(result->path), L"%s\\" APP_EXE, dir)) && Util_FileExists(result->path)) {
            /* An exe, whole: what gh wrote, checked before it runs. */
            DWORD size = 0;
            char *head = Util_ReadFile(result->path, 64u << 20, FALSE, &size);
            result->ok = head && size >= 64 * 1024 && head[0] == 'M' && head[1] == 'Z';
            Util_Free(head);
            if (!result->ok) StringCchCopyW(result->message, ARRAYSIZE(result->message), L"not an exe");
        }
        Util_Free(output);
    }
    ForgetSignIn(&in);
    Util_Log(L"update: %s downloaded: %s", job->tag, result->ok ? L"yes" : result->message);
    if (!PostMessageW(job->notify, job->message, 0, (LPARAM)result)) Util_Free(result);
    Util_Free(job);
    return 0;
}

void GhUpdate_Download(HWND notify, UINT message, const WCHAR *tag)
{
    DownloadJob *job = (DownloadJob *)Util_Alloc(sizeof *job);
    HANDLE thread;
    if (!job) return;
    job->notify = notify;
    job->message = message;
    StringCchCopyW(job->tag, ARRAYSIZE(job->tag), tag);
    if ((thread = CreateThread(NULL, 0, DownloadThread, job, 0, NULL)) != NULL) CloseHandle(thread);
    else Util_Free(job);
}

BOOL GhUpdate_Run(const GhDownload *download, DWORD *error)
{
    BOOL started;
    *error = ERROR_SUCCESS;
    AllowSetForegroundWindow(ASFW_ANY);
    started = Util_Spawn(download->path, L"--install", NULL);
    if (!started) {
        *error = GetLastError();
        Util_Log(L"update: could not start %s (error %lu)", download->path, *error);
    }
    return started;
}
