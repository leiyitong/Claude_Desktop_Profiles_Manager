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
 *
 * Claude's IndexedDB is a LevelDB too (core.c has its coding): one record of
 * one object store is read and written there the same way, with what
 * Chromium writes beside a record it puts (its "exists" entry and its
 * store's last version, both the record's new version).
 */
#include "app.h"

#define WEB_ORIGIN      "https://claude.ai"
#define WEB_FILE_MAX    (64u * 1024u * 1024u)
#define WEB_KEY_MAX     1024
#define WEB_VALUE_MAX   (16u * 1024u * 1024u)
#define IDB_NAME_CCH    128
#define IDB_STORE_NAME          0
#define IDB_STORE_LAST_VERSION  4

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
    ULONGLONG tables[512];
    int       tableCount;
    BOOL      tablesLost;             /* more live tables than `tables` holds: its reads would miss some */
    WCHAR     log[LONG_PATH_CCH];     /* the log a batch goes to; empty: a new one */
    ULONGLONG logFile;                /* its number */
    size_t    logLength, logClean;    /* its size; where its last whole record ends */
    ULONGLONG written;                /* the newest file's time, ms since 1970 */
    /* An IndexedDB: the databases' names and stores' metadata are kept, and the entries of the record `target`. */
    BOOL      indexed;
    BYTE      target[WEB_KEY_MAX];    /* its key, encoded */
    size_t    targetLength;
    BYTE      dataKey[WEB_KEY_MAX], existsKey[WEB_KEY_MAX], versionKey[64];   /* found once read; 0 long: not there */
    size_t    dataKeyLength, existsKeyLength, versionKeyLength;
};

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

/* Whether an entry is one the store keeps: claude.ai's in Local Storage; in
 * an IndexedDB, a database's name, a store's metadata, or the target's. */
static BOOL Wanted(const WebStore *store, const BYTE *key, size_t length)
{
    WCHAR name[IDB_NAME_CCH];
    ULONGLONG database, objectStore, ids[3];
    size_t end;
    int kind;
    if (!store->indexed) return length >= store->prefixLength && memcmp(key, store->prefix, store->prefixLength) == 0;
    if (Core_IdbDatabaseName(key, length, name, ARRAYSIZE(name)) || Core_IdbStoreMeta(key, length, &database, &objectStore, &kind)) return TRUE;
    return Core_IdbPrefixRead(key, length, ids, &end) && ids[0] && ids[1] && (ids[2] == 1 || ids[2] == 2) && length - end == store->targetLength &&
           memcmp(key + end, store->target, store->targetLength) == 0;
}

/* An entry of a table or a batch: kept when it is wanted and newer than what was read before. */
static BOOL TakeEntry(void *context, ULONGLONG sequence, const CoreLevelOp *op)
{
    WebStore *store = (WebStore *)context;
    WebEntry *entry;
    int at;
    store->lastSequence = max(store->lastSequence, sequence);
    if (!Wanted(store, op->key, op->keyLength)) return TRUE;
    if ((at = FindEntry(store, op->key, op->keyLength)) >= 0) {
        entry = &store->entries[at];
        if (entry->sequence > sequence) return TRUE;
        Util_Free(entry->value);
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
    if (added && i == store->tableCount) {
        if (store->tableCount < (int)ARRAYSIZE(store->tables)) store->tables[store->tableCount++] = number;
        else store->tablesLost = TRUE;
    }
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
    store->written = max(store->written, Core_FileTimeMs(time));
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
    Util_Free(data);
    if (strncmp(current, "MANIFEST-", 9) != 0 || MultiByteToWideChar(CP_UTF8, 0, current, -1, name, ARRAYSIZE(name)) <= 0) return FALSE;
    if ((data = ReadFileOf(store, name, &length)) == NULL) return FALSE;
    ZeroMemory(&manifest, sizeof manifest);
    manifest.onTable = TakeTable;
    manifest.context = store;
    ok = Core_LevelLogRecords(data, length, TakeEdit, &manifest, &clean) && manifest.hasLog;
    Util_Free(data);
    if (store->tablesLost) {
        Util_Log(L"web storage %s: more tables than this version reads; not read", store->dir);
        ok = FALSE;
    }
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
        Util_Free(data);
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
        Util_Free(data);
        if (!ok) return FALSE;
    }
    return TRUE;
}

/* `store` with the database of `p` in `folder` read; NULL when there is none (Claude makes it at its first start), or
 * it cannot be read. */
static WebStore *ReadStore(WebStore *store, const Profile *p, const WCHAR *folder)
{
    const WCHAR *root = p->storageDir[0] ? p->storageDir : p->dataDir;
    if (FAILED(StringCchPrintfW(store->dir, ARRAYSIZE(store->dir), L"%s\\%s", root, folder)) || !Util_DirExists(store->dir)) {
        WebStore_Free(store);
        return NULL;
    }
    if (!ReadManifest(store) || !ReadTables(store) || !ReadLogs(store)) {
        Util_Log(L"web storage %s of %s not read (error %lu)", folder, p->folder, GetLastError());
        WebStore_Free(store);
        return NULL;
    }
    return store;
}

WebStore *WebStore_Open(const Profile *p)
{
    WebStore *store;
    if ((store = (WebStore *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *store)) == NULL) return NULL;
    store->prefix[0] = '_';
    memcpy(store->prefix + 1, WEB_ORIGIN, sizeof WEB_ORIGIN);   /* with its zero */
    store->prefixLength = sizeof WEB_ORIGIN + 1;
    return ReadStore(store, p, CLAUDE_WEB_STORAGE);
}

/* An object store's name (its metadata's value, UTF-16BE) is `name`. */
static BOOL StoreNamed(const WebEntry *entry, const WCHAR *name)
{
    size_t length = wcslen(name), i;
    if (entry->valueLength != 2 * length) return FALSE;
    for (i = 0; i < length; i++)
        if ((WCHAR)((entry->value[2 * i] << 8) | entry->value[2 * i + 1]) != name[i]) return FALSE;
    return TRUE;
}

WebStore *WebStore_OpenIndexed(const Profile *p, const WCHAR *database, const WCHAR *objectStore, const WCHAR *key)
{
    WebStore *store;
    ULONGLONG databaseId = 0, storeId = 0;
    int i;
    if ((store = (WebStore *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *store)) == NULL) return NULL;
    store->indexed = TRUE;
    if ((store->targetLength = Core_IdbStringKey(key, store->target, sizeof store->target)) == 0) {
        WebStore_Free(store);
        return NULL;
    }
    if ((store = ReadStore(store, p, CLAUDE_INDEXED_DB)) == NULL) return NULL;
    /* The database by its name, then its object store by its name: their ids differ from profile to profile. */
    for (i = 0; i < store->count && !databaseId; i++) {
        const WebEntry *entry = &store->entries[i];
        WCHAR name[IDB_NAME_CCH];
        if (entry->put && Core_IdbDatabaseName(entry->key, entry->keyLength, name, ARRAYSIZE(name)) && wcscmp(name, database) == 0)
            databaseId = Core_IdbIntRead(entry->value, entry->valueLength);
    }
    for (i = 0; databaseId && i < store->count && !storeId; i++) {
        const WebEntry *entry = &store->entries[i];
        ULONGLONG db, id;
        int kind;
        if (entry->put && Core_IdbStoreMeta(entry->key, entry->keyLength, &db, &id, &kind) && db == databaseId && kind == IDB_STORE_NAME &&
            StoreNamed(entry, objectStore))
            storeId = id;
    }
    if (storeId && (store->dataKeyLength = Core_IdbPrefix(databaseId, storeId, 1, store->dataKey, sizeof store->dataKey - store->targetLength)) &&
        (store->existsKeyLength = Core_IdbPrefix(databaseId, storeId, 2, store->existsKey, sizeof store->existsKey - store->targetLength)) &&
        (store->versionKeyLength = Core_IdbStoreMetaKey(databaseId, storeId, IDB_STORE_LAST_VERSION, store->versionKey, sizeof store->versionKey))) {
        memcpy(store->dataKey + store->dataKeyLength, store->target, store->targetLength);
        store->dataKeyLength += store->targetLength;
        memcpy(store->existsKey + store->existsKeyLength, store->target, store->targetLength);
        store->existsKeyLength += store->targetLength;
    } else {
        store->dataKeyLength = store->existsKeyLength = store->versionKeyLength = 0;
    }
    return store;
}

void WebStore_Free(WebStore *store)
{
    int i;
    if (!store) return;
    for (i = 0; i < store->count; i++) {
        Util_Free(store->entries[i].key);
        Util_Free(store->entries[i].value);
    }
    Util_Free(store->entries);
    Util_Free(store);
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
    Util_Free(text);
    return utf8;
}

void WebStore_EachName(const WebStore *store, const WCHAR *prefix, void (*each)(void *context, const WCHAR *name), void *context)
{
    size_t prefixLength = wcslen(prefix), skip, i;
    WCHAR name[WEB_KEY_MAX];
    int at;
    if (!store) return;
    skip = store->prefixLength + 1;   /* the origin's part, and the mark of a Latin-1 name */
    for (at = 0; at < store->count; at++) {
        const WebEntry *entry = &store->entries[at];
        size_t length;
        if (!entry->put || entry->keyLength < skip + prefixLength || entry->key[store->prefixLength] != 1) continue;
        length = entry->keyLength - skip;
        if (length >= ARRAYSIZE(name)) continue;
        for (i = 0; i < length; i++) name[i] = (WCHAR)entry->key[skip + i];
        name[length] = 0;
        if (wcsncmp(name, prefix, prefixLength) == 0) each(context, name);
    }
}

/* `key` set to `value`, to be written by the commit. */
static BOOL SetEntry(WebStore *store, const BYTE *key, size_t keyLength, const BYTE *value, size_t valueLength)
{
    CoreLevelOp op;
    int at;
    ZeroMemory(&op, sizeof op);
    op.put = TRUE;
    op.key = key;
    op.keyLength = keyLength;
    op.value = value;
    op.valueLength = valueLength;
    if (!TakeEntry(store, store->lastSequence, &op) || (at = FindEntry(store, key, keyLength)) < 0) return FALSE;
    store->entries[at].changed = TRUE;
    return TRUE;
}

/* `utf8` as UTF-16, a heap block (`*chars` long, a zero after); NULL when it is not UTF-8. */
static WCHAR *Wide(const char *utf8, size_t length, int *chars)
{
    WCHAR *text;
    *chars = length ? MultiByteToWideChar(CP_UTF8, 0, utf8, (int)length, NULL, 0) : 0;
    if (length && *chars <= 0) return NULL;
    if ((text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)*chars + 1) * sizeof(WCHAR))) == NULL) return NULL;
    if (length) MultiByteToWideChar(CP_UTF8, 0, utf8, (int)length, text, *chars);
    text[*chars] = 0;
    return text;
}

BOOL WebStore_Set(WebStore *store, const WCHAR *name, const char *utf8, size_t length)
{
    BYTE key[WEB_KEY_MAX], *value;
    WCHAR *text;
    size_t keyLength, valueLength;
    int chars;
    BOOL ok;
    if (!store || store->indexed || (keyLength = KeyOf(name, key, sizeof key)) == 0 || length > WEB_VALUE_MAX) return FALSE;
    if ((text = Wide(utf8, length, &chars)) == NULL) return FALSE;
    if ((value = (BYTE *)HeapAlloc(GetProcessHeap(), 0, 1 + 2 * (size_t)chars)) == NULL) {
        Util_Free(text);
        return FALSE;
    }
    valueLength = Core_WebStorageValue(text, (size_t)chars, value, 1 + 2 * (size_t)chars);
    Util_Free(text);
    ok = valueLength && SetEntry(store, key, keyLength, value, valueLength);
    Util_Free(value);
    return ok;
}

/* The record's entry, when it holds a string Claude's window put. */
static const WebEntry *IndexedRecord(const WebStore *store, ULONGLONG *version, size_t *header, size_t *text, BYTE *tag, size_t *start,
                                     size_t *bytes)
{
    int at;
    if (!store || !store->indexed || !store->dataKeyLength || (at = FindEntry(store, store->dataKey, store->dataKeyLength)) < 0 ||
        !store->entries[at].put ||
        !Core_IdbValueString(store->entries[at].value, store->entries[at].valueLength, version, header, text, tag, start, bytes))
        return NULL;
    return &store->entries[at];
}

char *WebStore_GetIndexed(const WebStore *store, size_t *length)
{
    ULONGLONG version;
    size_t header, text, start, bytes, i, chars;
    BYTE tag;
    const WebEntry *entry = IndexedRecord(store, &version, &header, &text, &tag, &start, &bytes);
    WCHAR *wide;
    char *utf8 = NULL;
    int size;
    *length = 0;
    if (!entry) return NULL;
    if (tag == 'S') {
        if ((utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, bytes + 1)) == NULL) return NULL;
        memcpy(utf8, entry->value + start, bytes);
        utf8[bytes] = 0;
        *length = bytes;
        return utf8;
    }
    chars = tag == 'c' ? bytes / 2 : bytes;
    if ((wide = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (chars + 1) * sizeof(WCHAR))) == NULL) return NULL;
    for (i = 0; i < chars; i++)
        wide[i] = tag == 'c' ? (WCHAR)(entry->value[start + 2 * i] | (entry->value[start + 2 * i + 1] << 8)) : (WCHAR)entry->value[start + i];
    if ((size = chars ? WideCharToMultiByte(CP_UTF8, 0, wide, (int)chars, NULL, 0, NULL, NULL) : 0) >= 0 &&
        (utf8 = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)size + 1)) != NULL) {
        if (size) WideCharToMultiByte(CP_UTF8, 0, wide, (int)chars, utf8, size, NULL, NULL);
        utf8[size] = 0;
        *length = (size_t)size;
    }
    Util_Free(wide);
    return utf8;
}

BOOL WebStore_SetIndexed(WebStore *store, const char *utf8, size_t length)
{
    ULONGLONG version, next;
    size_t header, text, start, bytes, valueLength;
    BYTE tag, *value, number[8];
    const WebEntry *entry = IndexedRecord(store, &version, &header, &text, &tag, &start, &bytes);
    WCHAR *wide;
    int chars, at;
    BOOL ok;
    if (!entry || length > WEB_VALUE_MAX || (wide = Wide(utf8, length, &chars)) == NULL) return FALSE;
    /* The record's next version, as Chromium gives one: its store's last, plus one. */
    next = version;
    if ((at = FindEntry(store, store->versionKey, store->versionKeyLength)) >= 0 && store->entries[at].put)
        next = max(next, Core_IdbIntRead(store->entries[at].value, store->entries[at].valueLength));
    next++;
    valueLength = 10 + (text - header) + 11 + (size_t)chars;
    if ((value = (BYTE *)HeapAlloc(GetProcessHeap(), 0, valueLength)) == NULL) {
        Util_Free(wide);
        return FALSE;
    }
    valueLength = Core_IdbValueWrite(next, entry->value + header, text - header, wide, (size_t)chars, value, valueLength);
    Util_Free(wide);
    ok = valueLength && SetEntry(store, store->dataKey, store->dataKeyLength, value, valueLength) &&
         SetEntry(store, store->existsKey, store->existsKeyLength, number, Core_IdbInt(next, number)) &&
         SetEntry(store, store->versionKey, store->versionKeyLength, number, Core_IdbInt(next, number));
    Util_Free(value);
    return ok;
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

static size_t PutVarint(BYTE *out, ULONGLONG value)
{
    size_t n = 0;
    while (value >= 0x80) {
        out[n++] = (BYTE)(value | 0x80);
        value >>= 7;
    }
    out[n++] = (BYTE)value;
    return n;
}

/* Chromium's record of claude.ai's storage (META:<origin>, a protobuf): when
 * it changed last (microseconds since 1601) and the size of its keys and
 * values, as they are now. */
static size_t OriginMeta(const WebStore *store, BYTE *out)
{
    FILETIME now;
    ULONGLONG size = 0, modified;
    size_t n = 0;
    int i;
    for (i = 0; i < store->count; i++)
        if (store->entries[i].put) size += (store->entries[i].keyLength - store->prefixLength) + store->entries[i].valueLength;
    GetSystemTimeAsFileTime(&now);
    modified = (((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) / 10;
    out[n++] = 0x08;
    n += PutVarint(out + n, modified);
    out[n++] = 0x10;
    n += PutVarint(out + n, size);
    return n;
}

BOOL WebStore_Commit(WebStore *store, const WCHAR *backup)
{
    static const BYTE kMetaKey[] = "META:" WEB_ORIGIN;
    BYTE meta[32];
    CoreLevelOp *ops = NULL;
    BYTE *batch = NULL, *bytes = NULL;
    size_t batchLength = 0, capacity = 64, appended, start;
    HANDLE file = INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    DWORD written = 0;
    int i, count = 0;
    BOOL ok = FALSE, newLog = FALSE;
    if (!store) return FALSE;
    for (i = 0; i < store->count; i++)
        if (store->entries[i].changed) {
            capacity += 32 + store->entries[i].keyLength + store->entries[i].valueLength;
            count++;
        }
    if (!count) return TRUE;
    capacity += 32 + sizeof kMetaKey + sizeof meta;
    if (!BackUpFolder(store, backup)) {
        Util_Log(L"web storage %s not backed up to %s (error %lu): not written", store->dir, backup, GetLastError());
        return FALSE;
    }
    if ((ops = (CoreLevelOp *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, ((size_t)count + 1) * sizeof *ops)) == NULL ||
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
    if (!store->indexed) {   /* Local Storage's record of the origin; an IndexedDB keeps none */
        ops[count].put = TRUE;
        ops[count].key = kMetaKey;
        ops[count].keyLength = sizeof kMetaKey - 1;
        ops[count].value = meta;
        ops[count++].valueLength = OriginMeta(store, meta);
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
        newLog = TRUE;
        store->logFile = max(store->nextFile, store->maxFile + 1);
        StringCchPrintfW(store->log, ARRAYSIZE(store->log), L"%s\\%06llu.log", store->dir, store->logFile);
    }
    /* The log as it was read, or a new one: a log that changed since, or one
     * of that name made meanwhile, is not LevelDB's as it was read, and the
     * batch's place in its blocks would be wrong. */
    file = CreateFileW(store->log, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ, NULL,
                       newLog ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (newLog) store->log[0] = 0;
        goto done;
    }
    if (!newLog && (!GetFileSizeEx(file, &size) || (ULONGLONG)size.QuadPart != store->logLength)) {
        CloseHandle(file);
        SetLastError(ERROR_INVALID_DATA);
        goto done;
    }
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
    Util_Free(ops);
    Util_Free(batch);
    Util_Free(bytes);
    return ok;
}
