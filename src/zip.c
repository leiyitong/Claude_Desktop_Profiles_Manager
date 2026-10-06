/*
 * ZIP archives, written and read: files stored or deflated (methods 0 and
 * 8, Core_Deflate and Core_Inflate), names in UTF-8, without ZIP64: every
 * file and the archive under 4 GB, at most 65535 files. A file goes in as a
 * whole, read into memory, and is stored when deflating does not make it
 * smaller. An archive is written through a temporary file that takes its
 * name only once complete, so a failure leaves no half archive.
 */
#include "app.h"
#include <string.h>

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
#define ZIP_MAX_FILE          (512u * 1024u * 1024u)   /* a file read whole into memory */
#define ZIP_NAME_MAX          1024
#define ZIP_DIRECTORY_MAX     (64u * 1024u * 1024u)
#define TEMPORARY_SUFFIX      L".cdm-new"

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
    HANDLE   file;
    ZipItem *items;
    int      count;
};

static void *Alloc(size_t bytes) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, bytes); }
static void Free(void *memory) { if (memory) HeapFree(GetProcessHeap(), 0, memory); }

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
    ZipOut *zip = (ZipOut *)Alloc(sizeof *zip);
    *error = ERROR_SUCCESS;
    if (!zip) {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        return NULL;
    }
    if (FAILED(StringCchCopyW(zip->path, ARRAYSIZE(zip->path), path)) ||
        FAILED(StringCchPrintfW(zip->temporary, ARRAYSIZE(zip->temporary), L"%s" TEMPORARY_SUFFIX, path))) {
        *error = ERROR_FILENAME_EXCED_RANGE;
        Free(zip);
        return NULL;
    }
    zip->file = CreateFileW(zip->temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (zip->file == INVALID_HANDLE_VALUE) {
        *error = GetLastError();
        Free(zip);
        return NULL;
    }
    return zip;
}

DWORD Zip_Error(const ZipOut *zip) { return zip ? zip->error : ERROR_INVALID_PARAMETER; }

static BOOL Emit(ZipOut *zip, const void *data, size_t length)
{
    DWORD done = 0;
    if (zip->error) return FALSE;
    if (zip->written + length > ZIP_MAX_OFFSET) {
        zip->error = ERROR_FILE_TOO_LARGE;
        return FALSE;
    }
    if (length && (!WriteFile(zip->file, data, (DWORD)length, &done, NULL) || done != length)) {
        zip->error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        return FALSE;
    }
    zip->written += length;
    return TRUE;
}

static BOOL AddItem(ZipOut *zip, const char *name, const void *data, size_t size, const FILETIME *when)
{
    BYTE header[ZIP_LOCAL_BYTES];
    size_t nameLength = strlen(name), packedSize = 0;
    BYTE *packed = NULL;
    const void *body = data;
    ZipItem *item;
    if (zip->error) return FALSE;
    if (!nameLength || nameLength > ZIP_NAME_MAX || size > ZIP_MAX_FILE || zip->count >= ZIP_MAX_ITEMS) {
        zip->error = size > ZIP_MAX_FILE ? ERROR_FILE_TOO_LARGE : ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    if (zip->count == zip->capacity) {
        int capacity = zip->capacity ? zip->capacity * 2 : 64;
        ZipItem *grown = zip->items ? (ZipItem *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, zip->items, (size_t)capacity * sizeof *grown)
                                    : (ZipItem *)Alloc((size_t)capacity * sizeof *grown);
        if (!grown) {
            zip->error = ERROR_NOT_ENOUGH_MEMORY;
            return FALSE;
        }
        zip->items = grown;
        zip->capacity = capacity;
    }
    item = &zip->items[zip->count];
    ZeroMemory(item, sizeof *item);
    if ((item->name = (char *)Alloc(nameLength + 1)) == NULL) {
        zip->error = ERROR_NOT_ENOUGH_MEMORY;
        return FALSE;
    }
    memcpy(item->name, name, nameLength);
    item->crc = Core_Crc32(0, data, size);
    item->size = (DWORD)size;
    item->method = ZIP_STORED;
    item->offset = (DWORD)zip->written;
    DosTime(when, &item->time, &item->date);
    /* Deflated when that makes it smaller. */
    if (size >= 64 && (packed = (BYTE *)HeapAlloc(GetProcessHeap(), 0, Core_DeflateBound(size))) != NULL &&
        Core_Deflate(data, size, packed, Core_DeflateBound(size), &packedSize) && packedSize < size) {
        body = packed;
        item->method = ZIP_DEFLATED;
    } else {
        packedSize = size;
    }
    item->packed = (DWORD)packedSize;
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
    if (!Emit(zip, header, sizeof header) || !Emit(zip, name, nameLength) || !Emit(zip, body, packedSize)) {
        Free(packed);
        return FALSE;
    }
    Free(packed);
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
    if ((ULONGLONG)size.QuadPart > ZIP_MAX_FILE) {
        zip->error = ERROR_FILE_TOO_LARGE;
        CloseHandle(file);
        return FALSE;
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
    Free(data);
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
    if (ok && !MoveFileExW(zip->temporary, zip->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        zip->error = GetLastError();
        ok = FALSE;
    }
    if (!ok) DeleteFileW(zip->temporary);
    for (i = 0; i < zip->count; i++) Free(zip->items[i].name);
    Free(zip->items);
    if (!ok) SetLastError(zip->error ? zip->error : ERROR_CANCELLED);
    Free(zip);
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

ZipIn *Zip_Open(const WCHAR *path)
{
    ZipIn *zip = (ZipIn *)Alloc(sizeof *zip);
    LARGE_INTEGER size;
    BYTE *tail = NULL, *directory = NULL;
    DWORD tailLength, at, directorySize, directoryOffset, count, i;
    LONG found = -1;
    if (!zip) return NULL;
    zip->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, NULL);
    if (zip->file == INVALID_HANDLE_VALUE || !GetFileSizeEx(zip->file, &size) || size.QuadPart < ZIP_END_BYTES ||
        (ULONGLONG)size.QuadPart > 0xFFFFFFFFull)
        goto failed;
    tailLength = (DWORD)min((ULONGLONG)size.QuadPart, (ULONGLONG)ZIP_END_BYTES + ZIP_COMMENT_MAX);
    if ((tail = (BYTE *)Alloc(tailLength)) == NULL || !ReadAt(zip->file, (ULONGLONG)size.QuadPart - tailLength, tail, tailLength)) goto failed;
    for (at = tailLength - ZIP_END_BYTES + 1; at-- > 0;)
        if (Get32(tail + at) == ZIP_END_SIGNATURE) {
            found = (LONG)at;
            break;
        }
    if (found < 0) goto failed;
    count = Get16(tail + found + 10);
    directorySize = Get32(tail + found + 12);
    directoryOffset = Get32(tail + found + 16);
    if (directorySize > ZIP_DIRECTORY_MAX || (ULONGLONG)directoryOffset + directorySize > (ULONGLONG)size.QuadPart) goto failed;
    if ((directory = (BYTE *)Alloc(directorySize + 1)) == NULL || !ReadAt(zip->file, directoryOffset, directory, directorySize)) goto failed;
    if ((zip->items = (ZipItem *)Alloc((size_t)max(count, 1) * sizeof *zip->items)) == NULL) goto failed;
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
        if ((item->name = (char *)Alloc(nameLength + 1)) == NULL) goto failed;
        memcpy(item->name, directory + at + ZIP_CENTRAL_BYTES, nameLength);
        zip->count++;
        at += ZIP_CENTRAL_BYTES + nameLength + extra + comment;
    }
    Free(tail);
    Free(directory);
    return zip;
failed:
    Free(tail);
    Free(directory);
    Zip_Free(zip);
    return NULL;
}

void Zip_Free(ZipIn *zip)
{
    int i;
    if (!zip) return;
    if (zip->file && zip->file != INVALID_HANDLE_VALUE) CloseHandle(zip->file);
    for (i = 0; i < zip->count; i++) Free(zip->items[i].name);
    Free(zip->items);
    Free(zip);
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

void *Zip_Read(const ZipIn *zip, int index, DWORD maxBytes, DWORD *size)
{
    const ZipItem *item;
    BYTE local[ZIP_LOCAL_BYTES], *packed = NULL, *data = NULL;
    ULONGLONG start;
    size_t done = 0;
    BOOL ok = FALSE;
    if (size) *size = 0;
    if (!zip || index < 0 || index >= zip->count) return NULL;
    item = &zip->items[index];
    if (item->size > maxBytes || item->size > ZIP_MAX_FILE || item->packed > ZIP_MAX_FILE ||
        (item->method != ZIP_STORED && item->method != ZIP_DEFLATED))
        return NULL;
    if (!ReadAt(zip->file, item->offset, local, sizeof local) || Get32(local) != ZIP_LOCAL_SIGNATURE) return NULL;
    start = (ULONGLONG)item->offset + ZIP_LOCAL_BYTES + Get16(local + 26) + Get16(local + 28);
    if ((data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)item->size + 1)) == NULL) return NULL;
    if (item->method == ZIP_STORED) {
        ok = item->packed == item->size && (!item->size || ReadAt(zip->file, start, data, item->size));
        done = item->size;
    } else if ((packed = (BYTE *)HeapAlloc(GetProcessHeap(), 0, (size_t)item->packed + 1)) != NULL) {
        ok = (!item->packed || ReadAt(zip->file, start, packed, item->packed)) &&
             Core_Inflate(packed, item->packed, data, item->size, &done) && done == item->size;
    }
    Free(packed);
    if (!ok || Core_Crc32(0, data, done) != item->crc) {
        Free(data);
        return NULL;
    }
    data[done] = 0;
    if (size) *size = (DWORD)done;
    return data;
}

BOOL Zip_Extract(const ZipIn *zip, int index, const WCHAR *path)
{
    WCHAR longPath[LONG_PATH_CCH];
    DWORD size = 0, done = 0;
    void *data = Zip_Read(zip, index, ZIP_MAX_FILE, &size);
    HANDLE file;
    BOOL ok;
    if (!data) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if (!Util_ExtendedPath(path, longPath, ARRAYSIZE(longPath))) StringCchCopyW(longPath, ARRAYSIZE(longPath), path);
    file = CreateFileW(longPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        Free(data);
        return FALSE;
    }
    ok = (!size || (WriteFile(file, data, size, &done, NULL) && done == size)) && FlushFileBuffers(file);
    CloseHandle(file);
    Free(data);
    if (!ok) DeleteFileW(longPath);
    return ok;
}
