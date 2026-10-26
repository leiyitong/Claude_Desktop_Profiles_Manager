/*
 * ZIP archives, written and read: files stored or deflated (methods 0 and
 * 8, Core_Deflate and Core_Inflate), names in UTF-8, without ZIP64: every
 * file and the archive under 4 GB, at most 65535 files. A file up to
 * ZIP_INLINE_MAX goes in read whole, deflated when that makes it smaller; a
 * larger one is copied in stored, a chunk at a time, so that its size is
 * not bound by memory. An archive is written through a temporary file that
 * takes its name only once complete, so a failure leaves no half archive;
 * a file taken out of one likewise. An archive inside another opens where it
 * lies when it is stored (Zip_OpenItem).
 */
#include "app.h"
#include "vaulttools_tasks.h"
#include <string.h>
#include <stdlib.h>

#define ZIP_LOCAL_SIGNATURE   0x04034B50u
#define ZIP_CENTRAL_SIGNATURE 0x02014B50u
#define ZIP_END_SIGNATURE     0x06054B50u
#define ZIP_LOCAL_BYTES       30
#define ZIP_CENTRAL_BYTES     46
#define ZIP_END_BYTES         22
#define ZIP_COMMENT_MAX       0xFFFFu
#define ZIP_VERSION           20        /* 2.0: deflate and folders */
#define ZIP_UTF8_NAMES        0x0800u
#define ZIP_ENCRYPTED         0x0001u
#define ZIP_STORED            0
#define ZIP_DEFLATED          8
#define ZIP_MAX_ITEMS         0xFFFE
#define ZIP_MAX_OFFSET        0xFFFFFFFEull
#define ZIP_MAX_FILE          (512u * 1024u * 1024u)   /* a file read whole into memory (a deflated one) */
#define ZIP_INLINE_MAX        (16u * 1024u * 1024u)    /* larger files are copied in stored, a chunk at a time */
#define ZIP_CHUNK             (1u << 20)
#define ZIP_NAME_MAX          1024
#define ZIP_DIRECTORY_MAX     (64u * 1024u * 1024u)

typedef struct ZipItem {
    char  *name;
    DWORD  crc, packed, size, offset;
    WORD   method, time, date;
} ZipItem;

struct ZipOut {
    HANDLE   file;
    WCHAR    path[LONG_PATH_CCH], temporary[LONG_PATH_CCH];
    ZipItem *items;
    int      count, capacity;
    ULONGLONG written;
    DWORD    error;
};

struct ZipIn {
    HANDLE    file;
    ULONGLONG base, length;   /* where the archive starts in the file (an archive inside another), and its size */
    ZipItem  *items;
    int       count;
    WCHAR    *temporary;      /* a file of its own, taken away with it; NULL for none */
};


static void Put16(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void Put32(BYTE *p, DWORD v) { Put16(p, v); Put16(p + 2, v >> 16); }
static DWORD Get16(const BYTE *p) { return (DWORD)p[0] | (DWORD)p[1] << 8; }
static DWORD Get32(const BYTE *p) { return Get16(p) | Get16(p + 2) << 16; }

/* A FILETIME as MS-DOS's local date and time (2-second steps, 1980 on). */
static void DosTime(const FILETIME *when, WORD *time, WORD *date)
{
    FILETIME local;
    SYSTEMTIME st;
    *time = 0;
    *date = (1 << 5) | 1;   /* 1980-01-01 */
    if (!FileTimeToLocalFileTime(when, &local) || !FileTimeToSystemTime(&local, &st) || st.wYear < 1980) return;
    *time = (WORD)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    *date = (WORD)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
}

/* ---------------------------------------------------------------- writing */

ZipOut *Zip_Create(const WCHAR *path, DWORD *error)
{
    ZipOut *zip = (ZipOut *)Util_Alloc(sizeof *zip);
    WCHAR target[LONG_PATH_CCH];
    *error = ERROR_SUCCESS;
    if (!zip) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (FAILED(StringCchCopyW(zip->path, ARRAYSIZE(zip->path), path)) || !Util_ExtendedPath(path, target, ARRAYSIZE(target)) ||
        FAILED(StringCchPrintfW(zip->temporary, ARRAYSIZE(zip->temporary), L"%s" UTIL_TEMPORARY_SUFFIX, target))) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        Util_Free(zip);
        return NULL;
    }
    zip->file = CreateFileW(zip->temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (zip->file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        Util_Free(zip);
        return NULL;
    }
    return zip;
}

DWORD Zip_Error(const ZipOut *zip) { return zip ? zip->error : ERROR_INVALID_PARAMETER; }

static BOOL Emit(ZipOut *zip, const void *data, size_t length)
{
    if (zip->error) return FALSE;
    if (zip->written + length > ZIP_MAX_OFFSET) {
        zip->error = ERROR_FILE_TOO_LARGE;
        return FALSE;
    }
    if (length && !Util_WriteAll(zip->file, data, length)) {
        zip->error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        return FALSE;
    }
    zip->written += length;
    return TRUE;
}

BOOL Zip_Has(const ZipOut *zip, const char *name)
{
    int i;
    for (i = 0; zip && i < zip->count; i++)
        if (_stricmp(zip->items[i].name, name) == 0) return TRUE;
    return FALSE;
}

/* A new item's record and local header, written; its CRC and sizes are those
 * of `item` as given (a stream's are written into its header once known). */
static ZipItem *BeginItem(ZipOut *zip, const char *name, const FILETIME *when, WORD method, DWORD crc, DWORD packed, DWORD size)
{
    BYTE header[ZIP_LOCAL_BYTES];
    size_t nameLength = strlen(name);
    ZipItem *item;
    if (zip->error) return NULL;
    if (!nameLength || nameLength > ZIP_NAME_MAX || zip->count >= ZIP_MAX_ITEMS) {
        zip->error = ERROR_INVALID_PARAMETER;
        return NULL;
    }
    if (zip->count == zip->capacity) {
        int capacity = zip->capacity ? zip->capacity * 2 : 64;
        ZipItem *grown = zip->items ? (ZipItem *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, zip->items, (size_t)capacity * sizeof *grown)
                                    : (ZipItem *)Util_Alloc((size_t)capacity * sizeof *grown);
        if (!grown) {
            zip->error = ERROR_NOT_ENOUGH_MEMORY;
            return NULL;
        }
        zip->items = grown;
        zip->capacity = capacity;
    }
    item = &zip->items[zip->count];
    ZeroMemory(item, sizeof *item);
    if ((item->name = Util_CopyBytes(name, nameLength)) == NULL) {
        zip->error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    item->crc = crc;
    item->packed = packed;
    item->size = size;
    item->method = method;
    item->offset = (DWORD)zip->written;
    DosTime(when, &item->time, &item->date);
    Put32(header, ZIP_LOCAL_SIGNATURE);
    Put16(header + 4, ZIP_VERSION);
    Put16(header + 6, ZIP_UTF8_NAMES);
    Put16(header + 8, item->method);
    Put16(header + 10, item->time);
    Put16(header + 12, item->date);
    Put32(header + 14, item->crc);
    Put32(header + 18, item->packed);
    Put32(header + 22, item->size);
    Put16(header + 26, (DWORD)nameLength);
    Put16(header + 28, 0);
    zip->count++;
    if (!Emit(zip, header, sizeof header) || !Emit(zip, name, nameLength)) return NULL;
    return item;
}

static BOOL AddItem(ZipOut *zip, const char *name, const void *data, size_t size, const FILETIME *when)
{
    size_t packedSize = 0;
    BYTE *packed = NULL;
    const void *body = data;
    WORD method = ZIP_STORED;
    BOOL ok;
    if (zip->error) return FALSE;
    if (size > ZIP_MAX_FILE) {
        zip->error = ERROR_FILE_TOO_LARGE;
        return FALSE;
    }
    /* Deflated when that makes it smaller. */
    if (size >= 64 && (packed = (BYTE *)HeapAlloc(GetProcessHeap(), 0, Core_DeflateBound(size))) != NULL &&
        Core_Deflate(data, size, packed, Core_DeflateBound(size), &packedSize) && packedSize < size) {
        body = packed;
        method = ZIP_DEFLATED;
    } else {
        packedSize = size;
    }
    ok = BeginItem(zip, name, when, method, Core_Crc32(0, data, size), (DWORD)packedSize, (DWORD)size) != NULL && Emit(zip, body, packedSize);
    Util_Free(packed);
    return ok;
}

/* The open `file` copied in stored from where it is to its end, as it is
 * now: one still being written is taken up to there. */
static BOOL AddStream(ZipOut *zip, const char *name, HANDLE file, const FILETIME *when)
{
    BYTE *buffer, sizes[12];
    ZipItem *item;
    ULONGLONG size = 0;
    LARGE_INTEGER at, end;
    DWORD got = 0, crc = 0;
    BOOL ok;
    if ((buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, ZIP_CHUNK)) == NULL) {
        zip->error = ERROR_NOT_ENOUGH_MEMORY;
        return FALSE;
    }
    ok = (item = BeginItem(zip, name, when, ZIP_STORED, 0, 0, 0)) != NULL;
    while (ok && ReadFile(file, buffer, ZIP_CHUNK, &got, NULL) && got > 0) {
        crc = Core_Crc32(crc, buffer, got);
        size += got;
        ok = Emit(zip, buffer, got);
    }
    Util_Free(buffer);
    if (!ok) return FALSE;
    /* Emit kept the whole archive under 4 GB, so the file is too. */
    item = &zip->items[zip->count - 1];
    item->crc = crc;
    item->packed = item->size = (DWORD)size;
    Put32(sizes, crc);
    Put32(sizes + 4, (DWORD)size);
    Put32(sizes + 8, (DWORD)size);
    at.QuadPart = (LONGLONG)item->offset + 14;
    end.QuadPart = (LONGLONG)zip->written;
    if (!SetFilePointerEx(zip->file, at, NULL, FILE_BEGIN) || !Util_WriteAll(zip->file, sizes, sizeof sizes) ||
        !SetFilePointerEx(zip->file, end, NULL, FILE_BEGIN)) {
        zip->error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        return FALSE;
    }
    return TRUE;
}

BOOL Zip_AddData(ZipOut *zip, const char *name, const void *data, size_t size)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return AddItem(zip, name, data ? data : "", data ? size : 0, &now);
}

BOOL Zip_AddFile(ZipOut *zip, const char *name, const WCHAR *path)
{
    WCHAR longPath[LONG_PATH_CCH];
    HANDLE file;
    LARGE_INTEGER size;
    FILETIME written;
    BYTE *data = NULL;
    DWORD got = 0;
    BOOL ok;
    if (zip->error) return FALSE;
    if (!Util_ExtendedPath(path, longPath, ARRAYSIZE(longPath))) StringCchCopyW(longPath, ARRAYSIZE(longPath), path);
    /* Shared for writing too: a file a running program keeps open still copies. */
    file = CreateFileW(longPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        zip->error = GetLastError();
        return FALSE;
    }
    if (!GetFileSizeEx(file, &size) || !GetFileTime(file, NULL, NULL, &written)) {
        zip->error = GetLastError();
        CloseHandle(file);
        return FALSE;
    }
    if ((ULONGLONG)size.QuadPart > ZIP_INLINE_MAX) {
        ok = AddStream(zip, name, file, &written);
        CloseHandle(file);
        return ok;
    }
    if (size.QuadPart && (data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)size.QuadPart)) == NULL) {
        zip->error = ERROR_NOT_ENOUGH_MEMORY;
        CloseHandle(file);
        return FALSE;
    }
    ok = !size.QuadPart || (ReadFile(file, data, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart);
    if (!ok) zip->error = GetLastError() ? GetLastError() : ERROR_READ_FAULT;
    CloseHandle(file);
    ok = ok && AddItem(zip, name, data ? data : (BYTE *)"", (size_t)size.QuadPart, &written);
    Util_Free(data);
    return ok;
}

BOOL Zip_Close(ZipOut *zip, BOOL keep)
{
    BYTE end[ZIP_END_BYTES];
    ULONGLONG directory;
    int i;
    BOOL ok;
    if (!zip) return FALSE;
    directory = zip->written;
    for (i = 0; keep && !zip->error && i < zip->count; i++) {
        const ZipItem *item = &zip->items[i];
        BYTE header[ZIP_CENTRAL_BYTES];
        size_t nameLength = strlen(item->name);
        ZeroMemory(header, sizeof header);
        Put32(header, ZIP_CENTRAL_SIGNATURE);
        Put16(header + 4, ZIP_VERSION);
        Put16(header + 6, ZIP_VERSION);
        Put16(header + 8, ZIP_UTF8_NAMES);
        Put16(header + 10, item->method);
        Put16(header + 12, item->time);
        Put16(header + 14, item->date);
        Put32(header + 16, item->crc);
        Put32(header + 20, item->packed);
        Put32(header + 24, item->size);
        Put16(header + 28, (DWORD)nameLength);
        Put32(header + 42, item->offset);
        Emit(zip, header, sizeof header);
        Emit(zip, item->name, nameLength);
    }
    if (keep && !zip->error) {
        ZeroMemory(end, sizeof end);
        Put32(end, ZIP_END_SIGNATURE);
        Put16(end + 8, (DWORD)zip->count);
        Put16(end + 10, (DWORD)zip->count);
        Put32(end + 12, (DWORD)(zip->written - directory));
        Put32(end + 16, (DWORD)directory);
        Emit(zip, end, sizeof end);
        if (!zip->error && !FlushFileBuffers(zip->file)) zip->error = GetLastError();
    }
    CloseHandle(zip->file);
    ok = keep && !zip->error;
    if (ok && !Util_PutInPlace(zip->temporary, zip->path, UTIL_PUT_REPLACE)) {
        zip->error = GetLastError();
        ok = FALSE;
    }
    if (!ok) DeleteFileW(zip->temporary);
    for (i = 0; i < zip->count; i++) Util_Free(zip->items[i].name);
    Util_Free(zip->items);
    if (!ok) SetLastError(zip->error ? zip->error : ERROR_CANCELLED);
    Util_Free(zip);
    return ok;
}

/* ---------------------------------------------------------------- reading */

static BOOL ReadAt(HANDLE file, ULONGLONG offset, void *buffer, DWORD length)
{
    OVERLAPPED at;
    DWORD got = 0;
    ZeroMemory(&at, sizeof at);
    at.Offset = (DWORD)offset;
    at.OffsetHigh = (DWORD)(offset >> 32);
    return ReadFile(file, buffer, length, &got, &at) && got == length;
}

/* The archive in `length` bytes of `file` from `base`, which it owns. */
static ZipIn *OpenRange(HANDLE file, ULONGLONG base, ULONGLONG length)
{
    ZipIn *zip = (ZipIn *)Util_Alloc(sizeof *zip);
    BYTE *tail = NULL, *directory = NULL;
    DWORD tailLength, at, directorySize, directoryOffset, count, i;
    LONG found = -1;
    if (!zip) {
        CloseHandle(file);
        return NULL;
    }
    zip->file = file;
    zip->base = base;
    zip->length = length;
    if (length < ZIP_END_BYTES || length > 0xFFFFFFFFull) goto failed;
    tailLength = (DWORD)min(zip->length, (ULONGLONG)ZIP_END_BYTES + ZIP_COMMENT_MAX);
    if ((tail = (BYTE *)Util_Alloc(tailLength)) == NULL || !ReadAt(zip->file, zip->base + zip->length - tailLength, tail, tailLength)) goto failed;
    for (at = tailLength - ZIP_END_BYTES + 1; at-- > 0;)
        if (Get32(tail + at) == ZIP_END_SIGNATURE) {
            found = (LONG)at;
            break;
        }
    if (found < 0) goto failed;
    count = Get16(tail + found + 10);
    directorySize = Get32(tail + found + 12);
    directoryOffset = Get32(tail + found + 16);
    if (directorySize > ZIP_DIRECTORY_MAX || (ULONGLONG)directoryOffset + directorySize > zip->length) goto failed;
    if ((directory = (BYTE *)Util_Alloc(directorySize + 1)) == NULL || !ReadAt(zip->file, zip->base + directoryOffset, directory, directorySize)) goto failed;
    if ((zip->items = (ZipItem *)Util_Alloc((size_t)max(count, 1) * sizeof *zip->items)) == NULL) goto failed;
    for (i = 0, at = 0; i < count; i++) {
        ZipItem *item = &zip->items[i];
        DWORD nameLength, extra, comment;
        if (at + ZIP_CENTRAL_BYTES > directorySize || Get32(directory + at) != ZIP_CENTRAL_SIGNATURE) goto failed;
        nameLength = Get16(directory + at + 28);
        extra = Get16(directory + at + 30);
        comment = Get16(directory + at + 32);
        if (!nameLength || at + ZIP_CENTRAL_BYTES + nameLength + extra + comment > directorySize) goto failed;
        if (Get16(directory + at + 8) & ZIP_ENCRYPTED) goto failed;
        item->method = (WORD)Get16(directory + at + 10);
        item->crc = Get32(directory + at + 16);
        item->packed = Get32(directory + at + 20);
        item->size = Get32(directory + at + 24);
        item->offset = Get32(directory + at + 42);
        /* Its data lies before the directory: a size beyond it is never read. */
        if ((ULONGLONG)item->offset + ZIP_LOCAL_BYTES + item->packed > directoryOffset) goto failed;
        if ((item->name = Util_CopyBytes((const char *)directory + at + ZIP_CENTRAL_BYTES, nameLength)) == NULL) goto failed;
        zip->count++;
        at += ZIP_CENTRAL_BYTES + nameLength + extra + comment;
    }
    Util_Free(tail);
    Util_Free(directory);
    return zip;
failed:
    Util_Free(tail);
    Util_Free(directory);
    Zip_Free(zip);
    return NULL;
}

ZipIn *Zip_Open(const WCHAR *path)
{
    WCHAR longPath[LONG_PATH_CCH];
    LARGE_INTEGER size;
    HANDLE file;
    if (!Util_ExtendedPath(path, longPath, ARRAYSIZE(longPath))) StringCchCopyW(longPath, ARRAYSIZE(longPath), path);
    file = CreateFileW(longPath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (file == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(file, &size)) {
        CloseHandle(file);
        return NULL;
    }
    return OpenRange(file, 0, (ULONGLONG)size.QuadPart);
}

void Zip_Free(ZipIn *zip)
{
    int i;
    if (!zip) return;
    if (zip->file && zip->file != INVALID_HANDLE_VALUE) CloseHandle(zip->file);
    if (zip->temporary) {
        DeleteFileW(zip->temporary);
        Util_Free(zip->temporary);
    }
    for (i = 0; i < zip->count; i++) Util_Free(zip->items[i].name);
    Util_Free(zip->items);
    Util_Free(zip);
}

int Zip_Count(const ZipIn *zip) { return zip ? zip->count : 0; }
const char *Zip_Name(const ZipIn *zip, int index) { return zip && index >= 0 && index < zip->count ? zip->items[index].name : ""; }
DWORD Zip_Size(const ZipIn *zip, int index) { return zip && index >= 0 && index < zip->count ? zip->items[index].size : 0; }

int Zip_Find(const ZipIn *zip, const char *name)
{
    int i;
    for (i = 0; zip && i < zip->count; i++)
        if (strcmp(zip->items[i].name, name) == 0) return i;
    return -1;
}

/* Where an item's data starts, its local header checked; 0 when it cannot be read. */
static ULONGLONG DataStart(const ZipIn *zip, const ZipItem *item)
{
    BYTE local[ZIP_LOCAL_BYTES];
    ULONGLONG start;
    if (!ReadAt(zip->file, zip->base + item->offset, local, sizeof local) || Get32(local) != ZIP_LOCAL_SIGNATURE) return 0;
    start = (ULONGLONG)item->offset + ZIP_LOCAL_BYTES + Get16(local + 26) + Get16(local + 28);
    return start + item->packed <= zip->length ? start : 0;
}

void *Zip_Read(const ZipIn *zip, int index, DWORD maxBytes, DWORD *size)
{
    const ZipItem *item;
    BYTE *packed = NULL, *data = NULL;
    ULONGLONG start;
    size_t done = 0;
    BOOL ok = FALSE;
    if (size) *size = 0;
    if (!zip || index < 0 || index >= zip->count) return NULL;
    item = &zip->items[index];
    if (item->size > maxBytes || item->size > ZIP_MAX_FILE || item->packed > ZIP_MAX_FILE ||
        (item->method != ZIP_STORED && item->method != ZIP_DEFLATED))
        return NULL;
    if ((start = DataStart(zip, item)) == 0) return NULL;
    if ((data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)item->size + 1)) == NULL) return NULL;
    if (item->method == ZIP_STORED) {
        ok = item->packed == item->size && (!item->size || ReadAt(zip->file, zip->base + start, data, item->size));
        done = item->size;
    } else if ((packed = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)item->packed + 1)) != NULL) {
        ok = (!item->packed || ReadAt(zip->file, zip->base + start, packed, item->packed)) &&
             Core_Inflate(packed, item->packed, data, item->size, &done) && done == item->size;
    }
    Util_Free(packed);
    if (!ok || Core_Crc32(0, data, done) != item->crc) {
        Util_Free(data);
        return NULL;
    }
    data[done] = 0;
    if (size) *size = (DWORD)done;
    return data;
}

/* A stored item's data passed to `out` (NULL: only read) a chunk at a time
 * and checked by its CRC: any size, never whole in memory. */
static BOOL CopyStored(const ZipIn *zip, const ZipItem *item, HANDLE out)
{
    ULONGLONG at = DataStart(zip, item);
    DWORD left = item->size, crc = 0;
    BYTE *buffer;
    BOOL ok = TRUE;
    if (item->method != ZIP_STORED || item->packed != item->size || (!at && item->size)) return FALSE;
    if ((buffer = (BYTE *)HeapAlloc(GetProcessHeap(), 0, ZIP_CHUNK)) == NULL) return FALSE;
    while (ok && left > 0) {
        DWORD chunk = min(left, ZIP_CHUNK);
        ok = ReadAt(zip->file, zip->base + at, buffer, chunk) && (!out || Util_WriteAll(out, buffer, chunk));
        crc = Core_Crc32(crc, buffer, chunk);
        at += chunk;
        left -= chunk;
    }
    Util_Free(buffer);
    return ok && crc == item->crc;
}

/* Whether item `index` reads back whole and matches its CRC. */
static BOOL ItemIntact(const ZipIn *zip, int index)
{
    const ZipItem *item = &zip->items[index];
    void *data;
    if (item->method == ZIP_STORED) return CopyStored(zip, item, NULL);
    if ((data = Zip_Read(zip, index, ZIP_MAX_FILE, NULL)) == NULL) return FALSE;
    Util_Free(data);
    return TRUE;
}

BOOL Zip_ExtractTo(const ZipIn *zip, int index, const WCHAR *path, UtilPut how)
{
    WCHAR temporary[LONG_PATH_CCH];
    const ZipItem *item;
    void *data = NULL;
    DWORD size = 0;
    BOOL ok;
    if (!zip || index < 0 || index >= zip->count) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    item = &zip->items[index];
    if (item->method != ZIP_STORED && (data = Zip_Read(zip, index, ZIP_MAX_FILE, &size)) == NULL) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if (data) {
        ok = Util_WriteTemporary(path, (const char *)data, size, temporary, ARRAYSIZE(temporary));
        Util_Free(data);
    } else {
        WCHAR target[LONG_PATH_CCH];
        HANDLE out;
        if (!Util_ExtendedPath(path, target, ARRAYSIZE(target)) ||
            FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s" UTIL_TEMPORARY_SUFFIX, target))) {
            SetLastError(ERROR_FILENAME_EXCED_RANGE);
            return FALSE;
        }
        if ((out = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)) == INVALID_HANDLE_VALUE)
            return FALSE;
        ok = CopyStored(zip, item, out) && FlushFileBuffers(out);
        CloseHandle(out);
        if (!ok) {
            DeleteFileW(temporary);
            SetLastError(ERROR_INVALID_DATA);
        }
    }
    return ok && Util_PutInPlace(temporary, path, how);
}

BOOL Zip_Extract(const ZipIn *zip, int index, const WCHAR *path)
{
    return Zip_ExtractTo(zip, index, path, UTIL_PUT_REPLACE);
}

ZipIn *Zip_OpenItem(const ZipIn *zip, int index)
{
    WCHAR folder[MAX_PATH], temporary[MAX_PATH];
    const ZipItem *item;
    ULONGLONG start;
    HANDLE file;
    ZipIn *inner = NULL;
    if (!zip || index < 0 || index >= zip->count) return NULL;
    item = &zip->items[index];
    /* Stored: read where it lies, through a handle of its own. */
    if (item->method == ZIP_STORED && item->packed == item->size && (start = DataStart(zip, item)) != 0) {
        if (!DuplicateHandle(GetCurrentProcess(), zip->file, GetCurrentProcess(), &file, 0, FALSE, DUPLICATE_SAME_ACCESS)) return NULL;
        return OpenRange(file, zip->base + start, item->size);
    }
    /* Deflated (ZIP_INLINE_MAX at most): through a file of its own, taken away with it. */
    if (!GetTempPathW(ARRAYSIZE(folder), folder) || !GetTempFileNameW(folder, L"cdz", 0, temporary)) return NULL;
    if (!Zip_Extract(zip, index, temporary) || (inner = Zip_Open(temporary)) == NULL ||
        (inner->temporary = (WCHAR *)Util_CopyBytes((const char *)temporary, (wcslen(temporary) + 1) * sizeof(WCHAR))) == NULL) {
        if (inner) Zip_Free(inner);
        DeleteFileW(temporary);
        return NULL;
    }
    return inner;
}

static BOOL ArchiveWideName(const char *name, WCHAR *out, size_t cch)
{
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, out, (int)cch);
    if (n <= 1) return FALSE;
    if (out[n - 2] == L'/') out[n - 2] = 0;
    return TRUE;
}

static int CompareArchiveNames(const void *a, const void *b)
{
    const char *left = *(const char *const *)a, *right = *(const char *const *)b;
    WCHAR x[ZIP_NAME_MAX], y[ZIP_NAME_MAX];
    if (!ArchiveWideName(left, x, ARRAYSIZE(x)) || !ArchiveWideName(right, y, ARRAYSIZE(y))) return strcmp(left, right);
    return CompareStringOrdinal(x, -1, y, -1, TRUE) - CSTR_EQUAL;
}

/* General integrity check on the caller's stable read handle. No manifest
 * interpretation and no extraction: migration/snapshot readers validate the
 * same ZipIn they later read. A stored item is read a chunk at a time;
 * cancellation is checked between items. */
BOOL Zip_ValidateAll(const ZipIn *zip, HANDLE cancel, WCHAR *error, size_t cch)
{
    const char **names = NULL;
    BOOL ok = FALSE;
    int i;
    error[0] = 0;
    if (!zip || (names = (const char **)Util_Alloc((size_t)max(Zip_Count(zip), 1) * sizeof *names)) == NULL) goto done;
    for (i = 0; i < Zip_Count(zip); i++) {
        const char *name = Zip_Name(zip, i);
        size_t length = strlen(name);
        WCHAR wide[ZIP_NAME_MAX];
        if (length >= ZIP_NAME_MAX || !Core_ArchiveNameSafe(name, length && name[length - 1] == '/' ? length - 1 : length) ||
            !ArchiveWideName(name, wide, ARRAYSIZE(wide)))
            goto done;
        names[i] = name;
    }
    qsort(names, (size_t)Zip_Count(zip), sizeof *names, CompareArchiveNames);
    for (i = 1; i < Zip_Count(zip); i++) {
        WCHAR before[ZIP_NAME_MAX], after[ZIP_NAME_MAX];
        size_t n;
        if (CompareArchiveNames(&names[i - 1], &names[i]) == 0 || !ArchiveWideName(names[i - 1], before, ARRAYSIZE(before)) ||
            !ArchiveWideName(names[i], after, ARRAYSIZE(after)))
            goto done;
        n = wcslen(before);
        if (names[i - 1][strlen(names[i - 1]) - 1] != '/' && wcslen(after) > n && after[n] == L'/' &&
            CompareStringOrdinal(before, (int)n, after, (int)n, TRUE) == CSTR_EQUAL)
            goto done;
    }
    /* An unrelated sibling may sort between a file and its descendant, so
     * check every ancestor by binary search rather than adjacent names only. */
    for (i = 0; i < Zip_Count(zip); i++) {
        char prefix[ZIP_NAME_MAX];
        size_t at, length = strlen(Zip_Name(zip, i));
        StringCchCopyA(prefix, ARRAYSIZE(prefix), Zip_Name(zip, i));
        for (at = 0; at < length; at++)
            if (prefix[at] == '/') {
                const char *key = prefix;
                const char *const *parent;
                prefix[at] = 0;
                parent = (const char *const *)bsearch(&key, names, (size_t)Zip_Count(zip), sizeof *names, CompareArchiveNames);
                prefix[at] = '/';
                if (parent && (*parent)[strlen(*parent) - 1] != '/') goto done;
            }
    }
    for (i = 0; i < Zip_Count(zip); i++) {
        const char *name = Zip_Name(zip, i);
        size_t length = strlen(name);
        BYTE local[ZIP_LOCAL_BYTES];
        char localName[ZIP_NAME_MAX];
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
            StringCchCopyW(error, cch, TR(L"Validation cancelled."));
            goto done;
        }
        if (!ReadAt(zip->file, zip->base + zip->items[i].offset, local, sizeof local) || Get32(local) != ZIP_LOCAL_SIGNATURE ||
            (Get16(local + 6) & ZIP_ENCRYPTED) || Get16(local + 8) != zip->items[i].method || Get16(local + 26) != length ||
            !ReadAt(zip->file, zip->base + zip->items[i].offset + ZIP_LOCAL_BYTES, localName, (DWORD)length) || memcmp(name, localName, length) != 0 ||
            (!(Get16(local + 6) & 8) && (Get32(local + 14) != zip->items[i].crc || Get32(local + 18) != zip->items[i].packed ||
                                         Get32(local + 22) != zip->items[i].size)))
            goto done;
        if (!ItemIntact(zip, i)) {
            WCHAR bad[512];
            int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, bad, ARRAYSIZE(bad));
            StringCchPrintfW(error, cch, TR(L"Unreadable or corrupt archive item: %s"), wide ? bad : L"?");
            goto done;
        }
    }
    if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
        StringCchCopyW(error, cch, TR(L"Validation cancelled."));
        goto done;
    }
    ok = TRUE;
done:
    Util_Free(names);
    if (!ok && !error[0]) StringCchCopyW(error, cch, TR(L"The archive is incomplete, damaged, unsupported or exceeds the read limits."));
    return ok;
}

static BOOL ValidateArchive(const WCHAR *path, HANDLE cancel, int *files, WCHAR *error, size_t cch, int depth)
{
    ZipIn *zip = Zip_Open(path);
    DWORD size = 0;
    char *manifest = NULL;
    const char *value;
    size_t n;
    WCHAR format[80], temporary[MAX_PATH] = L"", folder[MAX_PATH];
    ULONGLONG version = 0, expected = 0;
    BOOL session = FALSE, ok = FALSE;
    int i, entries = 0, at = zip ? Zip_Find(zip, "manifest.json") : -1;
    if (!Zip_ValidateAll(zip, cancel, error, cch) || at < 0 || !(manifest = (char *)Zip_Read(zip, at, 65536, &size)) ||
        !Core_JsonIsValue(manifest, size) ||
        !Core_JsonMember(manifest, size, "format", &value, &n) || !Core_JsonString(value, n, format, ARRAYSIZE(format)) ||
        !Core_JsonMember(manifest, size, "version", &value, &n) || !Core_JsonNumber(value, n, &version))
        goto done;
    session = wcscmp(format, L"claude-desktop-profiles-manager-sessions") == 0;
    if (depth && !session) goto done;
    if ((!session && wcscmp(format, L"claude-desktop-profiles-manager-backup") != 0) || !version || version > (session ? 1u : 2u)) goto done;
    if (session && (!Core_JsonMember(manifest, size, "sessions", &value, &n) || !Core_JsonNumber(value, n, &expected))) goto done;
    for (i = 0; i < Zip_Count(zip); i++) {
        const char *name = Zip_Name(zip, i);
        size_t length = strlen(name);
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) {
            StringCchCopyW(error, cch, TR(L"Validation cancelled."));
            goto done;
        }
        if (session && strncmp(name, "entries/", 8) == 0 && name[length - 1] != '/') {
            void *data = Zip_Read(zip, i, ZIP_MAX_FILE, &size);
            if (!data || !Core_JsonIsValue((const char *)data, size) || Core_SessionEntryKind((const char *)data, size) != ENTRY_LOCAL) {
                Util_Free(data);
                goto done;
            }
            entries++;
            Util_Free(data);
        }
        if (!session && VaultTasks_IsCoverage(name) && !VaultTasks_ValidateCoverage(zip, name)) goto done;
        (*files)++;
        if (!session && strcmp(name, "sessions/archive.zip") == 0) {
            if (depth || !GetTempPathW(ARRAYSIZE(folder), folder) || !GetTempFileNameW(folder, L"cdv", 0, temporary) ||
                !Zip_Extract(zip, i, temporary) || !ValidateArchive(temporary, cancel, files, error, cch, depth + 1))
                goto done;
            DeleteFileW(temporary);
            temporary[0] = 0;
        }
    }
    if (session && expected != (ULONGLONG)entries) goto done;
    ok = TRUE;
done:
    if (temporary[0]) DeleteFileW(temporary);
    if (!ok && !error[0]) StringCchCopyW(error, cch, TR(L"The archive is incomplete, damaged, unsupported or exceeds the read limits."));
    Util_Free(manifest);
    Zip_Free(zip);
    return ok;
}

BOOL Zip_Validate(const WCHAR *path, HANDLE cancel, int *files, WCHAR *error, size_t cch)
{
    *files = 0;
    error[0] = 0;
    return ValidateArchive(path, cancel, files, error, cch, 0);
}
