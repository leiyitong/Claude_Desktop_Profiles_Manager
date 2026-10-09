/*
 * Claude's web storage: the Local Storage Chromium keeps for claude.ai in a
 * profile's folder (Local Storage\leveldb, a LevelDB database whose formats
 * core.c reads and writes). What Claude's window keeps there and nowhere
 * else (its sidebar's groups, its editor's settings) is read here and,
 * while that Claude is closed, written.
 *
 * Read: the manifest names the live tables and the log; every entry of
 * theirs is read, and each key's last value (by sequence number) kept.
 * Written: one write batch added to the log, after its last whole record,
 * with the next sequence number: LevelDB takes it in when it next opens, as
 * it takes in its own last writes after a crash. The folder is copied to the
 * backup first. Nothing is written while Claude runs (it holds the database
 * open), and LevelDB's own LOCK file is never opened.
 */
#include "app.h"

#define WEB_ORIGIN      "https://claude.ai"
#define WEB_FILE_MAX    (64u * 1024u * 1024u)
#define WEB_KEY_MAX     1024
#define WEB_VALUE_MAX   (16u * 1024u * 1024u)

typedef struct WebEntry {
    BYTE     *key, *value;
    size_t    keyLength, valueLength;
    ULONGLONG sequence;
    BOOL      put;
    BOOL      changed;    /* set here, waiting for the commit */
} WebEntry;

struct WebStore {
    WCHAR     dir[LONG_PATH_CCH];
    WebEntry *entries;
    int       count, capacity;
    BYTE      prefix[64];             /* "_https://claude.ai" and a zero: claude.ai's keys start so */
    size_t    prefixLength;
    ULONGLONG lastSequence, logNumber, prevLogNumber, nextFile, maxFile;
    ULONGLONG tables[64];
    int       tableCount;
    WCHAR     log[LONG_PATH_CCH];     /* the log a batch goes to; empty: a new one */
    ULONGLONG logFile;                /* its number */
    size_t    logLength, logClean;    /* its size; where its last whole record ends */
    ULONGLONG written;                /* the newest file's time, ms since 1970 */
};

static void Free(void *memory)
{
    if (memory) HeapFree(GetProcessHeap(), 0, memory);
}

static BYTE *Copy(const BYTE *data, size_t length)
{
    BYTE *copy = (BYTE *)HeapAlloc(GetProcessHeap(), 0, max(length, 1));
    if (copy && length) memcpy(copy, data, length);
    return copy;
}

static int FindEntry(const WebStore *store, const BYTE *key, size_t keyLength)
{
    int i;
    for (i = 0; i < store->count; i++)
        if (store->entries[i].keyLength == keyLength && memcmp(store->entries[i].key, key, keyLength) == 0) return i;
    return -1;
}

/* An entry of a table or a batch: kept when it is claude.ai's and newer than what was read before. */
static BOOL TakeEntry(void *context, ULONGLONG sequence, const CoreLevelOp *op)
{
    WebStore *store = (WebStore *)context;
    WebEntry *entry;
    int at;
    store->lastSequence = max(store->lastSequence, sequence);
    if (op->keyLength < store->prefixLength || memcmp(op->key, store->prefix, store->prefixLength) != 0) return TRUE;
    if ((at = FindEntry(store, op->key, op->keyLength)) >= 0) {
        entry = &store->entries[at];
        if (entry->sequence > sequence) return TRUE;
        Free(entry->value);
        entry->value = NULL;
    } else {
        if (store->count == store->capacity) {
            int grown = store->capacity ? store->capacity * 2 : 256;
            WebEntry *bigger = store->entries ? (WebEntry *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, store->entries, grown * sizeof *bigger)
                                              : (WebEntry *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, grown * sizeof *bigger);
            if (!bigger) return FALSE;
            store->entries = bigger;
            store->capacity = grown;
        }
        entry = &store->entries[store->count];
        ZeroMemory(entry, sizeof *entry);
        if ((entry->key = Copy(op->key, op->keyLength)) == NULL) return FALSE;
        entry->keyLength = op->keyLength;
        store->count++;
    }
    entry->sequence = sequence;
    entry->put = op->put;
    entry->valueLength = 0;
    if (op->put) {
        if ((entry->value = Copy(op->value, op->valueLength)) == NULL) return FALSE;
        entry->valueLength = op->valueLength;
    }
    return TRUE;
}

static BOOL TakeBatch(void *context, const BYTE *record, size_t length)
{
    return Core_LevelBatchRead(record, length, TakeEntry, context);
}

static void TakeTable(void *context, ULONGLONG number, BOOL added)
{
    WebStore *store = (WebStore *)context;
    int i;
    for (i = 0; i < store->tableCount && store->tables[i] != number; i++) {}
    if (added && i == store->tableCount && store->tableCount < (int)ARRAYSIZE(store->tables)) store->tables[store->tableCount++] = number;
    if (!added && i < store->tableCount) store->tables[i] = store->tables[--store->tableCount];
}

static BOOL TakeEdit(void *context, const BYTE *record, size_t length)
{
    return Core_LevelManifestEdit(record, length, (CoreLevelManifest *)context);
}

#define WEB_READ_TRIES 3   /* a file a running Claude writes meanwhile is read again */

/* A file of the database, whole (a heap block, empty for an empty file);
 * NULL when it is missing or cannot be read. */
static BYTE *ReadFileOf(const WebStore *store, const WCHAR *name, DWORD *length)
{
    WCHAR path[LONG_PATH_CCH];
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    BYTE *data = NULL;
    int tries;
    *length = 0;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", store->dir, name)) ||
        !GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
        return NULL;
    if (!attributes.nFileSizeHigh && !attributes.nFileSizeLow) return (BYTE *)HeapAlloc(GetProcessHeap(), 0, 1);
    for (tries = 0; !data && tries < WEB_READ_TRIES; tries++) data = (BYTE *)Util_ReadFile(path, WEB_FILE_MAX, FALSE, length);
    return data;
}

static void NoteWritten(WebStore *store, const FILETIME *time)
{
    ULARGE_INTEGER ticks;
    ticks.LowPart = time->dwLowDateTime;
    ticks.HighPart = time->dwHighDateTime;
    if (ticks.QuadPart > 116444736000000000ULL) store->written = max(store->written, (ticks.QuadPart - 116444736000000000ULL) / 10000);
}

/* The manifest: the live tables, the log and the last sequence number. */
static BOOL ReadManifest(WebStore *store)
{
    CoreLevelManifest manifest;
    WCHAR name[64];
    char current[64];
    DWORD length = 0;
    BYTE *data = ReadFileOf(store, L"CURRENT", &length);
    size_t clean, i;
    BOOL ok;
    if (!data) return FALSE;
    for (i = 0; i < length && i < sizeof current - 1 && data[i] != '\n' && data[i] != '\r'; i++) current[i] = (char)data[i];
    current[i] = 0;
    Free(data);
    if (strncmp(current, "MANIFEST-", 9) != 0 || MultiByteToWideChar(CP_UTF8, 0, current, -1, name, ARRAYSIZE(name)) <= 0) return FALSE;
    if ((data = ReadFileOf(store, name, &length)) == NULL) return FALSE;
    ZeroMemory(&manifest, sizeof manifest);
    manifest.onTable = TakeTable;
    manifest.context = store;
    ok = Core_LevelLogRecords(data, length, TakeEdit, &manifest, &clean) && manifest.hasLog;
    Free(data);
    store->logNumber = manifest.logNumber;
    store->prevLogNumber = manifest.prevLogNumber;
    store->nextFile = manifest.nextFile;
    store->lastSequence = manifest.lastSequence;
    return ok;
}

static ULONGLONG FileNumber(const WCHAR *name, const WCHAR *extension)
{
    ULONGLONG number = 0;
    const WCHAR *p = name;
    if (!*p) return 0;
    for (; *p >= L'0' && *p <= L'9'; p++) number = number * 10 + (ULONGLONG)(*p - L'0');
    return p != name && _wcsicmp(p, extension) == 0 ? number : 0;
}

/* The logs LevelDB replays when it opens (from the manifest's on), in order: each batch read. */
static BOOL ReadLogs(WebStore *store)
{
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(store->dir, L"*", &found, FALSE);
    ULONGLONG logs[32];
    int count = 0, i, j;
    BOOL ok = TRUE;
    if (find == INVALID_HANDLE_VALUE) return FALSE;
    do {
        ULONGLONG number = FileNumber(found.cFileName, L".log");
        ULONGLONG any = number ? number : max(FileNumber(found.cFileName, L".ldb"), FileNumber(found.cFileName, L".sst"));
        store->maxFile = max(store->maxFile, any);
        NoteWritten(store, &found.ftLastWriteTime);
        if (number && (number >= store->logNumber || number == store->prevLogNumber) && count < (int)ARRAYSIZE(logs)) logs[count++] = number;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    for (i = 1; i < count; i++)
        for (j = i; j > 0 && logs[j - 1] > logs[j]; j--) {
            ULONGLONG swap = logs[j];
            logs[j] = logs[j - 1];
            logs[j - 1] = swap;
        }
    for (i = 0; i < count && ok; i++) {
        WCHAR name[32];
        DWORD length = 0;
        BYTE *data;
        size_t clean = 0;
        StringCchPrintfW(name, ARRAYSIZE(name), L"%06llu.log", logs[i]);
        if ((data = ReadFileOf(store, name, &length)) == NULL) {
            ok = FALSE;
            break;
        }
        ok = Core_LevelLogRecords(data, length, TakeBatch, store, &clean);
        if (i == count - 1) {
            StringCchPrintfW(store->log, ARRAYSIZE(store->log), L"%s\\%s", store->dir, name);
            store->logFile = logs[i];
            store->logLength = length;
            store->logClean = clean;
        }
        Free(data);
    }
    return ok;
}

static BOOL ReadTables(WebStore *store)
{
    int i;
    for (i = 0; i < store->tableCount; i++) {
        WCHAR name[32];
        DWORD length = 0;
        BYTE *data;
        BOOL ok;
        StringCchPrintfW(name, ARRAYSIZE(name), L"%06llu.ldb", store->tables[i]);
        if ((data = ReadFileOf(store, name, &length)) == NULL) {
            StringCchPrintfW(name, ARRAYSIZE(name), L"%06llu.sst", store->tables[i]);
            if ((data = ReadFileOf(store, name, &length)) == NULL) return FALSE;
        }
        ok = Core_LevelTableRead(data, length, TakeEntry, store);
        Free(data);
        if (!ok) return FALSE;
    }
    return TRUE;
}

WebStore *WebStore_Open(const Profile *p)
{
    WebStore *store;
    const WCHAR *root = p->storageDir[0] ? p->storageDir : p->dataDir;
    if ((store = (WebStore *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *store)) == NULL) return NULL;
    store->prefix[0] = '_';
    memcpy(store->prefix + 1, WEB_ORIGIN, sizeof WEB_ORIGIN);   /* with its zero */
    store->prefixLength = sizeof WEB_ORIGIN + 1;
    if (FAILED(StringCchPrintfW(store->dir, ARRAYSIZE(store->dir), L"%s\\" CLAUDE_WEB_STORAGE, root)) || !Util_DirExists(store->dir)) {
        WebStore_Free(store);   /* none yet: Claude makes it at its first start */
        return NULL;
    }
    if (!ReadManifest(store) || !ReadTables(store) || !ReadLogs(store)) {
        Util_Log(L"web storage of %s not read (error %lu)", p->folder, GetLastError());
        WebStore_Free(store);
        return NULL;
    }
    return store;
}

void WebStore_Free(WebStore *store)
{
    int i;
    if (!store) return;
    for (i = 0; i < store->count; i++) {
        Free(store->entries[i].key);
        Free(store->entries[i].value);
    }
    Free(store->entries);
    Free(store);
}

ULONGLONG WebStore_Written(const WebStore *store)
{
    return store ? store->written : 0;
}

static size_t KeyOf(const WCHAR *name, BYTE *key, size_t capacity)
{
    return Core_WebStorageKey(WEB_ORIGIN, name, key, capacity);
}

char *WebStore_Get(const WebStore *store, const WCHAR *name, size_t *length)
{
    BYTE key[WEB_KEY_MAX];
    size_t keyLength = KeyOf(name, key, sizeof key), textLength = 0;
    WCHAR *text;
    char *utf8 = NULL;
    int at, bytes;
    *length = 0;
    if (!store || !keyLength || (at = FindEntry(store, key, keyLength)) < 0 || !store->entries[at].put) return NULL;
    if ((text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (store->entries[at].valueLength + 1) * sizeof(WCHAR))) == NULL) return NULL;
    if (Core_WebStorageText(store->entries[at].value, store->entries[at].valueLength, text, store->entries[at].valueLength + 1, &textLength) &&
        (bytes = WideCharToMultiByte(CP_UTF8, 0, text, (int)textLength, NULL, 0, NULL, NULL)) >= 0 &&
        (utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)bytes + 1)) != NULL) {
        WideCharToMultiByte(CP_UTF8, 0, text, (int)textLength, utf8, bytes, NULL, NULL);
        utf8[bytes] = 0;
        *length = (size_t)bytes;
    }
    Free(text);
    return utf8;
}

BOOL WebStore_Set(WebStore *store, const WCHAR *name, const char *utf8, size_t length)
{
    BYTE key[WEB_KEY_MAX], *value;
    WCHAR *text;
    size_t keyLength = KeyOf(name, key, sizeof key), valueLength;
    CoreLevelOp op;
    int chars, at;
    if (!store || !keyLength || length > WEB_VALUE_MAX) return FALSE;
    chars = length ? MultiByteToWideChar(CP_UTF8, 0, utf8, (int)length, NULL, 0) : 0;
    if (length && chars <= 0) return FALSE;
    if ((text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)chars + 1) * sizeof(WCHAR))) == NULL) return FALSE;
    if (length) MultiByteToWideChar(CP_UTF8, 0, utf8, (int)length, text, chars);
    if ((value = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 1 + 2 * (size_t)chars)) == NULL) {
        Free(text);
        return FALSE;
    }
    valueLength = Core_WebStorageValue(text, (size_t)chars, value, 1 + 2 * (size_t)chars);
    Free(text);
    ZeroMemory(&op, sizeof op);
    op.put = TRUE;
    op.key = key;
    op.keyLength = keyLength;
    op.value = value;
    op.valueLength = valueLength;
    if (!valueLength || !TakeEntry(store, store->lastSequence, &op) || (at = FindEntry(store, key, keyLength)) < 0) {
        Free(value);
        return FALSE;
    }
    Free(value);
    store->entries[at].changed = TRUE;
    return TRUE;
}

/* The database's files copied to `backup` (made), LOCK aside. */
static BOOL BackUpFolder(const WebStore *store, const WCHAR *backup)
{
    WIN32_FIND_DATAW found;
    HANDLE find;
    BOOL ok = TRUE;
    if (!Util_EnsureDir(backup) || (find = Util_FindFiles(store->dir, L"*", &found, FALSE)) == INVALID_HANDLE_VALUE) return FALSE;
    do {
        WCHAR from[LONG_PATH_CCH], to[LONG_PATH_CCH];
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || _wcsicmp(found.cFileName, L"LOCK") == 0) continue;
        if (FAILED(StringCchPrintfW(from, ARRAYSIZE(from), L"%s\\%s", store->dir, found.cFileName)) ||
            FAILED(StringCchPrintfW(to, ARRAYSIZE(to), L"%s\\%s", backup, found.cFileName)) || !CopyFileW(from, to, FALSE))
            ok = FALSE;
    } while (ok && FindNextFileW(find, &found));
    FindClose(find);
    return ok;
}

BOOL WebStore_Commit(WebStore *store, const WCHAR *backup)
{
    CoreLevelOp *ops = NULL;
    BYTE *batch = NULL, *bytes = NULL;
    size_t batchLength = 0, capacity = 64, appended, start;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD written = 0;
    int i, count = 0;
    BOOL ok = FALSE;
    if (!store) return FALSE;
    for (i = 0; i < store->count; i++)
        if (store->entries[i].changed) {
            capacity += 32 + store->entries[i].keyLength + store->entries[i].valueLength;
            count++;
        }
    if (!count) return TRUE;
    if (!BackUpFolder(store, backup)) {
        Util_Log(L"web storage %s not backed up to %s (error %lu): not written", store->dir, backup, GetLastError());
        return FALSE;
    }
    if ((ops = (CoreLevelOp *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)count * sizeof *ops)) == NULL ||
        (batch = (BYTE *)HeapAlloc(GetProcessHeap(), 0, capacity)) == NULL)
        goto done;
    for (i = 0, count = 0; i < store->count; i++)
        if (store->entries[i].changed) {
            ops[count].put = TRUE;
            ops[count].key = store->entries[i].key;
            ops[count].keyLength = store->entries[i].keyLength;
            ops[count].value = store->entries[i].value;
            ops[count].valueLength = store->entries[i].valueLength;
            count++;
        }
    if ((batchLength = Core_LevelBatchWrite(store->lastSequence + 1, ops, count, batch, capacity)) == 0) goto done;
    /* Right after the last whole record when the log ends there; else in the
     * next block: LevelDB reads nothing more of a block after zeros (padding)
     * or garbage (a write cut short), which stay. */
    start = store->logClean == store->logLength ? store->logLength : (store->logLength + 32767) / 32768 * 32768;
    capacity = (start - store->logLength) + batchLength + (batchLength / 32768 + 2) * 7 + 32768;
    if ((bytes = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, capacity)) == NULL) goto done;
    if ((appended = Core_LevelLogAppend(start, batch, batchLength, bytes + (start - store->logLength), capacity - (start - store->logLength))) == 0)
        goto done;
    appended += start - store->logLength;
    if (!store->log[0]) {   /* no log LevelDB would replay: a new one, numbered after every file */
        store->logFile = max(store->nextFile, store->maxFile + 1);
        StringCchPrintfW(store->log, ARRAYSIZE(store->log), L"%s\\%06llu.log", store->dir, store->logFile);
    }
    file = CreateFileW(store->log, FILE_APPEND_DATA | SYNCHRONIZE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    ok = WriteFile(file, bytes, (DWORD)appended, &written, NULL) && written == appended && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) {
        store->lastSequence += (ULONGLONG)count;
        store->logLength += appended;
        store->logClean = store->logLength;
        for (i = 0; i < store->count; i++) store->entries[i].changed = FALSE;
        Util_Log(L"web storage %s: %d value(s) written", store->dir, count);
    }
done:
    if (!ok) Util_Log(L"web storage %s not written (error %lu)", store->dir, GetLastError());
    Free(ops);
    Free(batch);
    Free(bytes);
    return ok;
}
