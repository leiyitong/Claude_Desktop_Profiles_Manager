/*
 * The sessions view's search of the conversations themselves (sessions.c
 * searches the titles and folders): on a thread of its own, each transcript
 * of the sessions shown is read and its messages, as the preview shows them
 * (Core_TranscriptLine), are searched for the text, as typed or as a
 * regular expression (Core_RegexSearch). A plain text is first looked for in
 * each line's bytes: most lines do not hold it and are not decoded at all.
 * A transcript is read a chunk at a time, never whole (one can pass 100 MB).
 * Each session found is posted to the window as it is found
 * (WM_APP_SESSION_SEARCH, wParam the search's number, lParam its index),
 * then the end (lParam -1). A new search stops the one under way.
 */
#include "app.h"

#define SEARCH_LINE_MAX      (256u * 1024u * 1024u)   /* a transcript line longer than this is not searched, nor the rest of its file */
#define SEARCH_LINE_CCH      (64u * 1024u)            /* what one message is searched in at most */
#define SEARCH_CHUNK         (4u * 1024u * 1024u)     /* read at a time; a longer line makes it grow */
#define SEARCH_CACHE_BUCKETS 1024
#define SEARCH_CACHE_MAX     8192                     /* results kept; one more empties the cache */
#define SEARCH_SETTLED_MS    2000
#define SEARCH_TAIL_BYTES    4096                     /* before where a read ended, compared before reading on from there */

/* What a search found in one transcript, kept by the file's path and the
 * search while the file is as it was then (its size and times). A result
 * holds only in the epoch it was read in (a file notification begins
 * another, SessionSearch_Invalidate), unless the file had been written
 * well before the read (settled): a write after it gives it another time
 * then. A transcript that only grew since (Claude Code adds lines to its
 * end) keeps a match found, and is searched on from where the read ended,
 * once the bytes before that are still the ones read. */
typedef struct SearchCacheEntry {
    struct SearchCacheEntry *next;
    WCHAR    *path;
    DWORD     hash;                    /* of the path, its case ignored */
    WCHAR     query[SESSION_SEARCH_CCH];
    BOOL      regex, matchCase, found, settled;
    LONG      epoch;
    WIN32_FILE_ATTRIBUTE_DATA file;
    ULONGLONG scanned;                 /* where the read ended: after the line found, else after the last whole line */
    ULONGLONG tail;                    /* TailHash at `scanned` */
    ULONGLONG offset;                  /* the line found's */
    size_t    messageEnd;
} SearchCacheEntry;

/* The search worker and SessionSearch_Locate (the window's thread) share the
 * cache: the lock covers looking in it and adding to it, never a file read,
 * so the window never waits for a long transcript. */
static SRWLOCK g_cacheLock = SRWLOCK_INIT;
static SearchCacheEntry *g_cache[SEARCH_CACHE_BUCKETS];
static int g_cacheCount;
static volatile LONG g_cacheEpoch;
static volatile LONG g_transcriptReads;   /* transcripts opened to be searched: what the tests count */

typedef struct SessionSearch {
    HWND           window;
    ULONG          generation;
    WCHAR          text[SESSION_SEARCH_CCH];
    char           bytes[SESSION_SEARCH_CCH * 3];  /* the text in UTF-8, for the lines' bytes */
    size_t         byteLength;
    BOOL           prefilter;                      /* the bytes can rule a line out */
    BOOL           regex, matchCase;
    CoreRegex      compiled;
    WCHAR        (*paths)[LONG_PATH_CCH];
    int            count;
    volatile LONG *stop;
} SessionSearch;

static HANDLE g_thread;
static volatile LONG g_stop;

static BOOL Stopped(const SessionSearch *search)
{
    return InterlockedCompareExchange(search->stop, 0, 0) != 0;
}

/* A message's text holds the search. */
static BOOL TextMatches(const SessionSearch *search, const WCHAR *text, size_t *messageEnd)
{
    size_t at, length = wcslen(text), n = wcslen(search->text);
    if (search->regex) return Core_RegexSearchEnd(&search->compiled, text, length, messageEnd);
    for (at = 0; at + n <= length; at++)
        if (search->matchCase ? wcsncmp(text + at, search->text, n) == 0 :
            CompareStringOrdinal(text + at, (int)n, search->text, (int)n, TRUE) == CSTR_EQUAL) {
            *messageEnd = at + n;
            return TRUE;
        }
    return FALSE;
}

/* The message of one transcript line (without its newline) holds the search. */
static BOOL LineMatches(const SessionSearch *search, const char *line, size_t length, WCHAR *message, size_t *messageEnd)
{
    size_t capacity = length + 8;
    WCHAR *decoded;
    BOOL found = FALSE;
    if (search->prefilter && !Core_ContainsUtf8(line, length, search->bytes, search->byteLength, search->matchCase)) return FALSE;
    decoded = capacity > SEARCH_LINE_CCH ? (WCHAR *)HeapAlloc(GetProcessHeap(), 0, capacity * sizeof(WCHAR)) : message;
    if (decoded && Core_TranscriptLine(line, length, decoded, capacity > SEARCH_LINE_CCH ? capacity : SEARCH_LINE_CCH) != TRANSCRIPT_NONE)
        found = TextMatches(search, decoded, messageEnd);
    if (decoded && decoded != message) HeapFree(GetProcessHeap(), 0, decoded);
    return found;
}

/* One of the lines of `file` from byte `from` (a line's start) to its end
 * holds the search: `offset` gets the line's, `scanned` where the read
 * ended (after that line, else after the last whole line: a last line
 * without its newline, which Claude Code may be writing, is searched and
 * read again the next time). `complete`: read to its end or to the line
 * found, not stopped. */
static BOOL ReadMatches(const SessionSearch *search, HANDLE file, ULONGLONG from, WCHAR *message, BOOL *complete,
                        ULONGLONG *offset, size_t *messageEnd, ULONGLONG *scanned)
{
    size_t capacity = SEARCH_CHUNK, held = 0;
    ULONGLONG at = from;                     /* the file's byte at buffer[0] */
    char *buffer = (char *)HeapAlloc(GetProcessHeap(), 0, capacity), *grown;
    LARGE_INTEGER position;
    BOOL found = FALSE, ended = FALSE;
    *complete = FALSE;
    *scanned = from;
    position.QuadPart = (LONGLONG)from;
    if (!buffer || !SetFilePointerEx(file, position, NULL, FILE_BEGIN)) goto done;
    while (!found && !ended && !Stopped(search)) {
        DWORD got = 0;
        size_t start = 0;
        const char *newline;
        if (held == capacity) {   /* a line longer than the buffer: it grows */
            if (capacity >= SEARCH_LINE_MAX || (grown = (char *)HeapReAlloc(GetProcessHeap(), 0, buffer, capacity * 2)) == NULL) goto done;
            buffer = grown;
            capacity *= 2;
        }
        if (!ReadFile(file, buffer + held, (DWORD)(capacity - held), &got, NULL)) goto done;
        held += got;
        while (!found && !Stopped(search) && (newline = (const char *)memchr(buffer + start, '\n', held - start)) != NULL) {
            size_t length = (size_t)(newline - (buffer + start));
            if (LineMatches(search, buffer + start, length, message, messageEnd)) {
                found = TRUE;
                *offset = at + start;
            }
            start += length + 1;
        }
        *scanned = at + start;
        if (!found && got == 0) {
            ended = TRUE;
            if (start < held && LineMatches(search, buffer + start, held - start, message, messageEnd)) {
                found = TRUE;
                *offset = at + start;
                *scanned = at + held;
            }
        }
        MoveMemory(buffer, buffer + start, held - start);
        held -= start;
        at += start;
    }
    *complete = (found || ended) && !Stopped(search);
done:
    if (buffer) HeapFree(GetProcessHeap(), 0, buffer);
    return found;
}

/* The SEARCH_TAIL_BYTES of `file` before byte `end` (fewer near its start), hashed. */
static BOOL TailHash(HANDLE file, ULONGLONG end, ULONGLONG *hash)
{
    char bytes[SEARCH_TAIL_BYTES];
    DWORD want = (DWORD)min(end, (ULONGLONG)sizeof bytes), got = 0;
    LARGE_INTEGER position;
    position.QuadPart = (LONGLONG)(end - want);
    if (!SetFilePointerEx(file, position, NULL, FILE_BEGIN) || !ReadFile(file, bytes, want, &got, NULL) || got != want) return FALSE;
    *hash = Core_HashBytes(Core_HashBytes(CORE_HASH_START, &end, sizeof end), bytes, got);
    return TRUE;
}

static BOOL FileStamp(const WCHAR *path, WIN32_FILE_ATTRIBUTE_DATA *data)
{
    WCHAR extended[LONG_PATH_CCH];
    return Util_ExtendedPath(path, extended, ARRAYSIZE(extended)) && GetFileAttributesExW(extended, GetFileExInfoStandard, data) &&
           !(data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL SameStamp(const WIN32_FILE_ATTRIBUTE_DATA *a, const WIN32_FILE_ATTRIBUTE_DATA *b)
{
    return a->nFileSizeHigh == b->nFileSizeHigh && a->nFileSizeLow == b->nFileSizeLow &&
           CompareFileTime(&a->ftLastWriteTime, &b->ftLastWriteTime) == 0 && CompareFileTime(&a->ftCreationTime, &b->ftCreationTime) == 0;
}

static ULONGLONG StampSize(const WIN32_FILE_ATTRIBUTE_DATA *data)
{
    return ((ULONGLONG)data->nFileSizeHigh << 32) | data->nFileSizeLow;
}

/* The file is the one `was` stamped, longer only (lines added to its end, as far as its times tell). */
static BOOL OnlyGrew(const WIN32_FILE_ATTRIBUTE_DATA *was, const WIN32_FILE_ATTRIBUTE_DATA *now)
{
    return CompareFileTime(&was->ftCreationTime, &now->ftCreationTime) == 0 && StampSize(now) > StampSize(was);
}

static BOOL SameSearch(const SearchCacheEntry *entry, const SessionSearch *search, const WCHAR *path, DWORD hash)
{
    return entry->hash == hash && entry->regex == search->regex && entry->matchCase == search->matchCase &&
           wcscmp(entry->query, search->text) == 0 && Core_PathEquals(entry->path, path);
}

/* A copy of the result kept for `path` and `search` (its path left out); FALSE when none is. */
static BOOL KeptResult(const SessionSearch *search, const WCHAR *path, DWORD hash, SearchCacheEntry *kept)
{
    const SearchCacheEntry *entry;
    AcquireSRWLockShared(&g_cacheLock);
    for (entry = g_cache[hash % SEARCH_CACHE_BUCKETS]; entry && !SameSearch(entry, search, path, hash); entry = entry->next) {}
    if (entry) {
        *kept = *entry;
        kept->path = NULL;
        kept->next = NULL;
    }
    ReleaseSRWLockShared(&g_cacheLock);
    return entry != NULL;
}

static void FreeEntry(SearchCacheEntry *entry)
{
    if (entry->path) HeapFree(GetProcessHeap(), 0, entry->path);
    HeapFree(GetProcessHeap(), 0, entry);
}

/* Keeps `result` (its path, hash and search filled here) in place of what was kept for the same file and search;
 * SEARCH_CACHE_MAX results kept already empty the cache first. */
static void KeepResult(const SessionSearch *search, const WCHAR *path, DWORD hash, const SearchCacheEntry *result)
{
    size_t bytes = (wcslen(path) + 1) * sizeof(WCHAR);
    SearchCacheEntry *entry = (SearchCacheEntry *)HeapAlloc(GetProcessHeap(), 0, sizeof *entry), **at, *gone = NULL;
    WCHAR *copy = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, bytes);
    int b;
    if (!entry || !copy) {
        if (entry) HeapFree(GetProcessHeap(), 0, entry);
        if (copy) HeapFree(GetProcessHeap(), 0, copy);
        return;
    }
    *entry = *result;
    CopyMemory(copy, path, bytes);
    entry->path = copy;
    entry->hash = hash;
    StringCchCopyW(entry->query, ARRAYSIZE(entry->query), search->text);
    entry->regex = search->regex;
    entry->matchCase = search->matchCase;
    AcquireSRWLockExclusive(&g_cacheLock);
    for (at = &g_cache[hash % SEARCH_CACHE_BUCKETS]; *at && !SameSearch(*at, search, path, hash); at = &(*at)->next) {}
    if (*at) {
        gone = *at;
        entry->next = gone->next;
        gone->next = NULL;
        *at = entry;
    } else {
        if (g_cacheCount >= SEARCH_CACHE_MAX) {
            for (b = 0; b < SEARCH_CACHE_BUCKETS; b++) {
                SearchCacheEntry *item = g_cache[b], *next;
                for (; item; item = next) {
                    next = item->next;
                    item->next = gone;
                    gone = item;
                }
                g_cache[b] = NULL;
            }
            g_cacheCount = 0;
        }
        entry->next = g_cache[hash % SEARCH_CACHE_BUCKETS];
        g_cache[hash % SEARCH_CACHE_BUCKETS] = entry;
        g_cacheCount++;
    }
    ReleaseSRWLockExclusive(&g_cacheLock);
    while (gone) {
        SearchCacheEntry *next = gone->next;
        FreeEntry(gone);
        gone = next;
    }
}

static HANDLE OpenTranscript(const WCHAR *path)
{
    WCHAR extended[LONG_PATH_CCH];
    if (!Util_ExtendedPath(path, extended, ARRAYSIZE(extended))) return INVALID_HANDLE_VALUE;
    InterlockedIncrement(&g_transcriptReads);
    return CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
}

static BOOL TranscriptMatches(const SessionSearch *search, const WCHAR *path, WCHAR *message, ULONGLONG *offset, size_t *messageEnd)
{
    WIN32_FILE_ATTRIBUTE_DATA before, after;
    SearchCacheEntry kept, result;
    LONG epoch = InterlockedCompareExchange(&g_cacheEpoch, 0, 0);
    ULONGLONG started = Util_NowMs(), from = 0, tail = 0;
    DWORD hash = Core_HashIgnoringCase(path);
    BOOL stamped, have, complete = FALSE, found = FALSE;
    HANDLE file;
    ZeroMemory(&kept, sizeof kept);
    stamped = FileStamp(path, &before);
    have = stamped && KeptResult(search, path, hash, &kept);
    if (have && SameStamp(&kept.file, &before) && (kept.settled || kept.epoch == epoch)) {
        *offset = kept.offset;
        *messageEnd = kept.messageEnd;
        return kept.found;
    }
    if ((file = OpenTranscript(path)) == INVALID_HANDLE_VALUE) return FALSE;
    ZeroMemory(&result, sizeof result);
    /* Lines added only: what was read before is still what it found. */
    if (have && OnlyGrew(&kept.file, &before) && TailHash(file, kept.scanned, &tail) && tail == kept.tail) {
        if (kept.found) {
            found = complete = TRUE;
            *offset = kept.offset;
            *messageEnd = kept.messageEnd;
            result.scanned = kept.scanned;
        } else {
            from = kept.scanned;
        }
    }
    if (!complete) found = ReadMatches(search, file, from, message, &complete, offset, messageEnd, &result.scanned);
    complete = complete && TailHash(file, result.scanned, &result.tail);
    CloseHandle(file);
    if (stamped && complete && FileStamp(path, &after) && SameStamp(&before, &after) &&
        epoch == InterlockedCompareExchange(&g_cacheEpoch, 0, 0)) {
        result.file = after;
        result.epoch = epoch;
        result.settled = Core_FileTimeMs(&after.ftLastWriteTime) + SEARCH_SETTLED_MS <= started;
        result.found = found;
        result.offset = found ? *offset : 0;
        result.messageEnd = found ? *messageEnd : 0;
        KeepResult(search, path, hash, &result);
    }
    return found;
}

void SessionSearch_Invalidate(void)
{
    InterlockedIncrement(&g_cacheEpoch);
}

static DWORD WINAPI SearchProc(void *param)
{
    SessionSearch *search = (SessionSearch *)param;
    WCHAR *message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, SEARCH_LINE_CCH * sizeof(WCHAR));
    int i;
    for (i = 0; message && i < search->count && !InterlockedCompareExchange(search->stop, 0, 0); i++) {
        ULONGLONG offset = 0;
        size_t messageEnd = 0;
        BOOL found;
        found = search->paths[i][0] && TranscriptMatches(search, search->paths[i], message, &offset, &messageEnd);
        if (found) PostMessageW(search->window, WM_APP_SESSION_SEARCH, (WPARAM)search->generation, (LPARAM)i);
    }
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

/* A cached hit includes its line offset and its decoded match end. Re-read
 * stale/evicted files using exactly the worker's message semantics. */
BOOL SessionSearch_Locate(const WCHAR *path, const WCHAR *text, BOOL regex, BOOL matchCase, ULONGLONG *offset, size_t *messageEnd)
{
    SessionSearch *search = (SessionSearch *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *search);
    WCHAR *message = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, SEARCH_LINE_CCH * sizeof(WCHAR));
    volatile LONG stop = 0;
    BOOL found = FALSE;
    *offset = 0;
    *messageEnd = 0;
    if (search && message && SUCCEEDED(StringCchCopyW(search->text, ARRAYSIZE(search->text), text)) &&
        (!regex || Core_RegexCompile(&search->compiled, text, !matchCase))) {
        search->regex = regex;
        search->matchCase = matchCase;
        search->stop = &stop;
        found = TranscriptMatches(search, path, message, offset, messageEnd);
    }
    if (message) HeapFree(GetProcessHeap(), 0, message);
    if (search) HeapFree(GetProcessHeap(), 0, search);
    return found;
}
