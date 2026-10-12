/*
 * The sessions view's search of the conversations themselves (sessions.c
 * searches the titles and folders): on a thread of its own, each transcript
 * of the sessions shown is read and its messages, as the preview shows them
 * (Core_TranscriptLine), are searched for the text, as typed or as a
 * regular expression (Core_RegexSearch). A plain text is first looked for in
 * the file's bytes: most transcripts do not hold it and are not decoded at
 * all. Each session found is posted to the window as it is found
 * (WM_APP_SESSION_SEARCH, wParam the search's number, lParam its index),
 * then the end (lParam -1). A new search stops the one under way.
 */
#include "app.h"

#define SEARCH_FILE_MAX   (256u * 1024u * 1024u)   /* a transcript larger than this is not read */
#define SEARCH_LINE_CCH   (64u * 1024u)            /* what one message is searched in at most */

typedef struct SessionSearch {
    HWND           window;
    ULONG          generation;
    WCHAR          text[SESSION_SEARCH_CCH];
    char           bytes[SESSION_SEARCH_CCH * 3];  /* the text in UTF-8, for the files' bytes */
    size_t         byteLength;
    BOOL           prefilter;                      /* the bytes can rule a file out */
    BOOL           regex, matchCase;
    CoreRegex      compiled;
    WCHAR        (*paths)[LONG_PATH_CCH];
    int            count;
    volatile LONG *stop;
} SessionSearch;

static HANDLE g_thread;
static volatile LONG g_stop;

/* A message's text holds the search. */
static BOOL TextMatches(const SessionSearch *search, const WCHAR *text)
{
    if (search->regex) return Core_RegexSearch(&search->compiled, text, wcslen(text));
    return search->matchCase ? wcsstr(text, search->text) != NULL : Core_ContainsI(text, search->text);
}

/* One transcript holds the search in one of its messages. */
static BOOL TranscriptMatches(const SessionSearch *search, const WCHAR *path, WCHAR *message)
{
    DWORD length = 0;
    char *content = Util_ReadFile(path, SEARCH_FILE_MAX, FALSE, &length), *line, *end;
    BOOL found = FALSE;
    if (!content) return FALSE;
    if (!search->prefilter || Core_ContainsUtf8(content, length, search->bytes, search->byteLength, search->matchCase)) {
        for (line = content; *line && !found && !InterlockedCompareExchange(search->stop, 0, 0); line = *end ? end + 1 : end) {
            if ((end = strchr(line, '\n')) == NULL) end = line + strlen(line);
            if (Core_TranscriptLine(line, (size_t)(end - line), message, SEARCH_LINE_CCH) != TRANSCRIPT_NONE) found = TextMatches(search, message);
        }
    }
    HeapFree(GetProcessHeap(), 0, content);
    return found;
}

static DWORD WINAPI SearchProc(void *param)
{
    SessionSearch *search = (SessionSearch *)param;
    WCHAR *message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, SEARCH_LINE_CCH * sizeof(WCHAR));
    int i;
    for (i = 0; message && i < search->count && !InterlockedCompareExchange(search->stop, 0, 0); i++)
        if (search->paths[i][0] && TranscriptMatches(search, search->paths[i], message))
            PostMessageW(search->window, WM_APP_SESSION_SEARCH, (WPARAM)search->generation, (LPARAM)i);
    if (!InterlockedCompareExchange(search->stop, 0, 0)) PostMessageW(search->window, WM_APP_SESSION_SEARCH, (WPARAM)search->generation, (LPARAM)-1);
    if (message) HeapFree(GetProcessHeap(), 0, message);
    HeapFree(GetProcessHeap(), 0, search->paths);
    HeapFree(GetProcessHeap(), 0, search);
    return 0;
}

void SessionSearch_Stop(void)
{
    if (!g_thread) return;
    InterlockedExchange(&g_stop, 1);
    /* It stops between two messages or two files: soon. */
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
}

BOOL SessionSearch_Start(HWND window, ULONG generation, const WCHAR *text, BOOL regex, BOOL matchCase,
                         WCHAR (*paths)[LONG_PATH_CCH], int count)
{
    SessionSearch *search;
    size_t i;
    SessionSearch_Stop();
    search = (SessionSearch *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *search);
    if (!search || FAILED(StringCchCopyW(search->text, ARRAYSIZE(search->text), text)) ||
        (regex && !Core_RegexCompile(&search->compiled, text, !matchCase))) {
        if (search) HeapFree(GetProcessHeap(), 0, search);
        HeapFree(GetProcessHeap(), 0, paths);
        return FALSE;
    }
    search->window = window;
    search->generation = generation;
    search->regex = regex;
    search->matchCase = matchCase;
    search->paths = paths;
    search->count = count;
    search->stop = &g_stop;
    /* A quote, a backslash or a control character is escaped in the file,
     * and the bytes ignore the case of ASCII letters only: with one of
     * those, or another letter that has a case, every file is decoded. */
    search->prefilter = !regex;
    for (i = 0; search->prefilter && text[i]; i++)
        if (text[i] == L'"' || text[i] == L'\\' || text[i] < 0x20 || (!matchCase && text[i] > 0x7F && towlower(text[i]) != towupper(text[i])))
            search->prefilter = FALSE;
    if (search->prefilter) {
        int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, search->bytes, (int)sizeof search->bytes, NULL, NULL);
        if (bytes <= 1) search->prefilter = FALSE;
        else search->byteLength = (size_t)bytes - 1;
    }
    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(NULL, 0, SearchProc, search, 0, NULL);
    if (!g_thread) {
        Util_Log(L"sessions: the conversations cannot be searched (error %lu)", GetLastError());
        HeapFree(GetProcessHeap(), 0, search->paths);
        HeapFree(GetProcessHeap(), 0, search);
        return FALSE;
    }
    return TRUE;
}
