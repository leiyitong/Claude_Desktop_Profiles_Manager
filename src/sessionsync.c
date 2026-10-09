/*
 * Sessions sent between profiles in numbers: every profile's merged,
 * several shared, copied or taken out at once, sessions exported to an
 * archive or imported from one, and the pins and groups of Claude's sidebar
 * (for sessionvault.c).
 *
 * A profile lists a session through an entry of its own (sessionstore.c), so
 * a session sent to a profile is an entry written there as Claude writes its
 * own: local_<id>.json beside its other entries, in the folder of the
 * account signed in and of its organization. A running Claude keeps its
 * list in memory and writes it back, so entries are written only while their
 * profile is closed: what is sent to a profile goes to a plan of ours
 * (pending-sync-<folder>.txt, the entries in pending-sync-<folder>\), made
 * at once when the profile is closed, else when it closes, at the moments
 * sessionedit.c makes its waiting changes. Each change is checked again
 * then against the entries as they are: a session used there since keeps
 * its entry, and one Claude marked deleted there stays deleted unless the
 * user sent it on purpose. What a change replaces or takes away is copied
 * first to backups\<time>\<folder> in our state folder (the latest
 * BACKUPS_KEPT kept).
 *
 * An archive is a ZIP file, stored rather than compressed: manifest.json,
 * each session's entry (entries/<n>.json) and its conversation's files in
 * Claude Code's folder (claude/<path there>). Importing one adds the files
 * that are missing, only where a conversation keeps its own
 * (Core_ConversationFileName), and sends the entries to the profiles chosen.
 */
#include "app.h"
#include <objbase.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PLAN_PREFIX           L"pending-sync-"
#define PLAN_MAX_BYTES        (16u * 1024u * 1024u)
#define PLAN_LINE_CCH         (SESSION_ID_CCH + SYNC_CONTENT_CCH + 96)   /* the kind, three numbers and the tabs */
#define CONTENT_MAX_BYTES     (16u * 1024u * 1024u)   /* an entry, or a list of archived sessions */
#define TEMPORARY_SUFFIX      L".cdm-new"
#define TOMBSTONE_PREFIX      L"deleted_"
#define LOCAL_PREFIX          L"local_"
#define ARCHIVED_INDEX        L"archived-sessions.idx"   /* beside the entries: Claude's list of the archived ones */
#define BACKUPS_DIR           L"backups"
#define BACKUPS_KEPT          20
#define BACKUP_COPIES_MAX     100     /* name~2 ... name~100 when a name is taken in a backup */
#define WRITE_CHUNK_MAX       0x40000000u
#define COPY_CHUNK            (1024u * 1024u)
#define UNIX_EPOCH_TICKS      116444736000000000ULL
#define TICKS_PER_MILLISECOND (TICKS_PER_SECOND / 1000)
#define GUID_TEXT_CCH         39
#define TIME_TEXT_BYTES       sizeof "18446744073709551615"
#define ID_JSON_BYTES         (SESSION_ID_CCH * 6 + 3)   /* an id as a JSON string, every character escaped at worst */
#define PATH_JSON_BYTES       (MAX_PATH * 6 + 3)

#define ARCHIVE_FORMAT        "claude-desktop-profiles-manager-sessions"
#define ARCHIVE_VERSION       1
#define ARCHIVE_MANIFEST      "manifest.json"
#define ARCHIVE_ENTRIES       "entries/"
#define ARCHIVE_FILES         "claude/"
#define ZIP_LOCAL_SIGNATURE   0x04034B50u
#define ZIP_CENTRAL_SIGNATURE 0x02014B50u
#define ZIP_END_SIGNATURE     0x06054B50u
#define ZIP_LOCAL_BYTES       30
#define ZIP_CENTRAL_BYTES     46
#define ZIP_END_BYTES         22
#define ZIP_COMMENT_MAX       0xFFFFu
#define ZIP_VERSION           20      /* 2.0: stored files in folders */
#define ZIP_UTF8_NAMES        0x0800u
#define ZIP_ENCRYPTED         0x0001u
#define ZIP_STORED            0
#define ZIP_MAX_ITEMS         0xFFFEu
#define ZIP_MAX_OFFSET        0xFFFFFFFEull
#define ZIP_DIRECTORY_MAX     (64u * 1024u * 1024u)
#define ZIP_NAME_MAX          (LONG_PATH_CCH * 3)

/* What happened to one change. */
typedef enum OpResult {
    OP_MADE,
    OP_SAME,       /* nothing to do: as it should be already */
    OP_SKIPPED,    /* left as it was: newer there, used there since, or deleted there */
    OP_FAILED,
    OP_STOPPED     /* its Claude started: this change and the rest wait */
} OpResult;

/* What one profile is sent, before it goes to its plan. */
typedef struct Sent {
    SyncOp      *ops;
    const char **contents;   /* each change's entry or list, owned by the Outbox; NULL for none */
    size_t      *lengths;
    int          count, capacity;
} Sent;

typedef struct Outbox {
    Sent   sent[MAX_PROFILES];
    char **owned;
    int    ownedCount, ownedCapacity;
} Outbox;

typedef struct Plan {
    SyncOp *ops;
    int     count, capacity;
} Plan;

/* --------------------------------------------------------------- memory */

static void *Grow(void *items, int *capacity, int need, size_t size)
{
    void *bigger;
    int n;
    if (need <= *capacity) return items;
    if (*capacity > INT_MAX / 2) return NULL;
    n = *capacity ? *capacity * 2 : 16;
    while (n < need) n *= 2;
    bigger = items ? HeapReAlloc(GetProcessHeap(), 0, items, (size_t)n * size) : HeapAlloc(GetProcessHeap(), 0, (size_t)n * size);
    if (bigger) *capacity = n;
    return bigger;
}

static void Free(void *block)
{
    if (block) HeapFree(GetProcessHeap(), 0, block);
}

/* `data` (a heap block) owned by the outbox from now on; NULL, freed, when it cannot be. */
static char *Keep(Outbox *box, char *data)
{
    char **grown;
    if (!data) return NULL;
    if ((grown = (char **)Grow(box->owned, &box->ownedCapacity, box->ownedCount + 1, sizeof *box->owned)) == NULL) {
        Free(data);
        return NULL;
    }
    box->owned = grown;
    box->owned[box->ownedCount++] = data;
    return data;
}

static BOOL AddSent(Sent *sent, const SyncOp *op, const char *content, size_t length)
{
    int capacity = sent->capacity;
    SyncOp *ops = (SyncOp *)Grow(sent->ops, &capacity, sent->count + 1, sizeof *ops);
    const char **contents;
    size_t *lengths;
    if (!ops) return FALSE;
    sent->ops = ops;
    capacity = sent->capacity;
    if ((contents = (const char **)Grow((void *)sent->contents, &capacity, sent->count + 1, sizeof *contents)) == NULL) return FALSE;
    sent->contents = contents;
    capacity = sent->capacity;
    if ((lengths = (size_t *)Grow(sent->lengths, &capacity, sent->count + 1, sizeof *lengths)) == NULL) return FALSE;
    sent->lengths = lengths;
    sent->capacity = capacity;
    sent->ops[sent->count] = *op;
    sent->contents[sent->count] = content;
    sent->lengths[sent->count] = length;
    sent->count++;
    return TRUE;
}

static void FreeSent(Sent *sent)
{
    Free(sent->ops);
    Free((void *)sent->contents);
    Free(sent->lengths);
    ZeroMemory(sent, sizeof *sent);
}

static void FreeOutbox(Outbox *box)
{
    int i;
    for (i = 0; i < MAX_PROFILES; i++) FreeSent(&box->sent[i]);
    for (i = 0; i < box->ownedCount; i++) Free(box->owned[i]);
    Free(box->owned);
    ZeroMemory(box, sizeof *box);
}

/* -------------------------------------------------------------- reports */

/* The first failure, said in `report` (`format`, translated, takes the
 * path and the error), and every one logged. */
static void Failed(SyncReport *report, const WCHAR *format, const WCHAR *path, DWORD code)
{
    report->failed++;
    Util_Log(L"session sync: %s failed (error %lu)", path, code);
    if (!report->error[0]) StringCchPrintfW(report->error, ARRAYSIZE(report->error), format, path, code);
}

static void CannotWrite(SyncReport *report, const WCHAR *path, DWORD code)
{
    Failed(report, TR(L"%s could not be written (error %lu)."), path, code);
}

static void CannotRead(SyncReport *report, const WCHAR *path, DWORD code)
{
    Failed(report, TR(L"%s could not be read (error %lu)."), path, code);
}

/* ---------------------------------------------------------------- files */

static BOOL Extended(const WCHAR *path, WCHAR *out)
{
    return Util_ExtendedPath(path, out, LONG_PATH_CCH);
}

static BOOL Present(const WCHAR *path)
{
    WCHAR extended[LONG_PATH_CCH];
    return Extended(path, extended) && Util_QueryPath(extended, NULL) == PATH_PRESENT;
}

static BOOL WriteWhole(HANDLE file, const char *data, size_t length)
{
    while (length > 0) {
        DWORD written = 0, chunk = length > WRITE_CHUNK_MAX ? WRITE_CHUNK_MAX : (DWORD)length;
        if (!WriteFile(file, data, chunk, &written, NULL) || written == 0) return FALSE;
        data += written;
        length -= written;
    }
    return TRUE;
}

/* `path` holds `data`, written beside it and then put in its place: a file
 * of ours (`profile` NULL) replaces the one there; an entry of `profile`'s
 * (or Claude's mark beside them) is written only while that profile is
 * closed, replacing the one there only with `replace`, else only where there
 * is none. GetLastError tells why it failed; ERROR_BUSY: its Claude runs. */
static BOOL WriteFileAt(const WCHAR *path, const char *data, size_t length, const Profile *profile, BOOL replace)
{
    WCHAR target[LONG_PATH_CCH], temporary[LONG_PATH_CCH];
    HANDLE file;
    DWORD error = ERROR_SUCCESS;
    BOOL ok;
    if (!Extended(path, target) || FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s" TEMPORARY_SUFFIX, target))) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    if (profile && SessionLink_Busy(profile)) {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteWhole(file, data, length) && FlushFileBuffers(file);
    if (!ok) error = GetLastError();
    CloseHandle(file);
    if (ok && profile && SessionLink_Busy(profile)) {
        ok = FALSE;
        error = ERROR_BUSY;
    }
    if (ok) {
        if (profile && replace) {
            ok = ReplaceFileW(target, temporary, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL);
            if (!ok) error = GetLastError();
            /* ReplaceFileW took the old one away but could not put the new one in its place. */
            if (!ok && error == ERROR_UNABLE_TO_MOVE_REPLACEMENT && MoveFileExW(temporary, target, MOVEFILE_WRITE_THROUGH)) ok = TRUE;
        } else {
            ok = MoveFileExW(temporary, target, MOVEFILE_WRITE_THROUGH | (profile ? 0 : MOVEFILE_REPLACE_EXISTING));
            if (!ok) error = GetLastError();
        }
    }
    if (!ok) {
        DeleteFileW(temporary);
        SetLastError(error);
    }
    return ok;
}

/* Each missing folder of `path`'s parent made, at any length: the first
 * existing one down from `root`, which exists. */
static BOOL MakeParents(const WCHAR *root, const WCHAR *path)
{
    WCHAR folder[LONG_PATH_CCH], extended[LONG_PATH_CCH], *at;
    size_t start = wcslen(root);
    if (FAILED(StringCchCopyW(folder, ARRAYSIZE(folder), path))) return FALSE;
    for (at = folder + start + 1; (at = wcschr(at, L'\\')) != NULL; at++) {
        *at = 0;
        if (!Extended(folder, extended) || (!CreateDirectoryW(extended, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) return FALSE;
        *at = L'\\';
    }
    return TRUE;
}

/* ------------------------------------------------------------- the plan */

BOOL SessionSync_PlanPath(const Profile *p, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    return Util_StateDir(state, ARRAYSIZE(state)) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" PLAN_PREFIX L"%s.txt", state, p->folder));
}

static BOOL PlanDir(const Profile *p, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH];
    return Util_StateDir(state, ARRAYSIZE(state)) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" PLAN_PREFIX L"%s", state, p->folder));
}

/* The changes sent to `p` and not made yet, oldest first. -1 when the file
 * is there but cannot be read: rewriting it then would lose them. */
static int LoadPlan(const Profile *p, Plan *plan)
{
    WCHAR path[MAX_PATH], *text, *line, *next;
    DWORD length = 0;
    char *raw;
    int wide;
    ZeroMemory(plan, sizeof *plan);
    if (!SessionSync_PlanPath(p, path, ARRAYSIZE(path))) return -1;
    if ((raw = Util_ReadFile(path, PLAN_MAX_BYTES, FALSE, &length)) == NULL) {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &attributes))
            return attributes.nFileSizeHigh || attributes.nFileSizeLow ? -1 : 0;
        return Util_QueryPath(path, NULL) == PATH_MISSING ? 0 : -1;
    }
    wide = MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, NULL, 0);
    text = wide > 0 ? (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)wide + 1) * sizeof(WCHAR)) : NULL;
    if (!text) {
        Free(raw);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, text, wide);
    text[wide] = 0;
    Free(raw);
    for (line = text; line; line = next) {
        SyncOp op, *grown;
        size_t lineLength;
        next = wcschr(line, L'\n');
        if (next) *next++ = 0;
        lineLength = wcslen(line);
        if (lineLength && line[lineLength - 1] == L'\r') line[--lineLength] = 0;
        if (!lineLength || !Core_SyncOpParse(line, &op)) continue;
        if ((grown = (SyncOp *)Grow(plan->ops, &plan->capacity, plan->count + 1, sizeof *plan->ops)) == NULL) {
            Free(text);
            Free(plan->ops);
            ZeroMemory(plan, sizeof *plan);
            return -1;
        }
        plan->ops = grown;
        plan->ops[plan->count++] = op;
    }
    Free(text);
    return plan->count;
}

/* The plan written again as `ops` holds it; none left: its file and folder go. */
static BOOL SavePlan(const Profile *p, const SyncOp *ops, int count)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH], line[PLAN_LINE_CCH];
    char *out;
    size_t used = 0, capacity;
    int i;
    BOOL ok;
    if (!SessionSync_PlanPath(p, path, ARRAYSIZE(path)) || !PlanDir(p, dir, ARRAYSIZE(dir))) return FALSE;
    if (count == 0) {
        ok = DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
        RemoveDirectoryW(dir);   /* when it is empty */
        return ok;
    }
    capacity = (size_t)count * ARRAYSIZE(line) * 3;
    if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, capacity)) == NULL) return FALSE;
    for (i = 0; i < count; i++) {
        int bytes;
        if (!Core_SyncOpFormat(&ops[i], line, ARRAYSIZE(line)) || FAILED(StringCchCatW(line, ARRAYSIZE(line), L"\n")) ||
            (bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, out + used, (int)min(capacity - used, INT_MAX), NULL, NULL)) <= 0) {
            Free(out);
            return FALSE;
        }
        used += (size_t)bytes - 1;
    }
    ok = WriteFileAt(path, out, used, NULL, TRUE);
    if (!ok) Util_Log(L"could not write %s (error %lu)", path, GetLastError());
    Free(out);
    return ok;
}

int SessionSync_PendingCount(const Profile *p)
{
    Plan plan;
    int count = LoadPlan(p, &plan);
    Free(plan.ops);
    return max(count, 0);
}

int SessionSync_Queued(const Profile *p, SyncOp **ops)
{
    Plan plan;
    int count = LoadPlan(p, &plan);
    *ops = count > 0 ? plan.ops : NULL;
    if (count <= 0) Free(plan.ops);
    return count;
}

/* A content file's number: "<n>.json". */
static int ContentNumber(const WCHAR *name)
{
    int n = 0;
    for (; *name >= L'0' && *name <= L'9' && n < INT_MAX / 10 - 9; name++) n = n * 10 + (*name - L'0');
    return n;
}

static void DeleteContent(const WCHAR *dir, const SyncOp *op)
{
    WCHAR path[MAX_PATH];
    if (op->content[0] && SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, op->content))) DeleteFileW(path);
}

/* What `sent` holds added to the plan of `p`, each change replacing the
 * queued ones it supersedes (Core_SyncOpReplaces), its entry kept in a file
 * of the plan's folder. */
static BOOL StageSent(const Profile *p, Sent *sent)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    HANDLE lock = NULL;
    Plan plan;
    int next = 1, i, j, kept;
    BOOL ok = FALSE;
    if (!sent->count) return TRUE;
    if (!PlanDir(p, dir, ARRAYSIZE(dir)) || (lock = SessionEdit_Lock(p)) == NULL) return FALSE;
    if (LoadPlan(p, &plan) < 0) {
        Util_Log(L"sessions sent to %s: the plan cannot be read", p->folder);
        SessionEdit_Unlock(lock);
        return FALSE;
    }
    for (i = 0; i < plan.count; i++) next = max(next, ContentNumber(plan.ops[i].content) + 1);
    if (!Util_EnsureDir(dir)) goto done;
    for (i = 0; i < sent->count; i++) {
        SyncOp *op = &sent->ops[i], *grown;
        for (j = kept = 0; j < plan.count; j++) {
            if (Core_SyncOpReplaces(&plan.ops[j], op)) DeleteContent(dir, &plan.ops[j]);
            else plan.ops[kept++] = plan.ops[j];
        }
        plan.count = kept;
        op->content[0] = 0;
        if (sent->contents[i]) {
            if (FAILED(StringCchPrintfW(op->content, ARRAYSIZE(op->content), L"%d.json", next++)) ||
                FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, op->content)) ||
                !WriteFileAt(path, sent->contents[i], sent->lengths[i], NULL, TRUE)) {
                Util_Log(L"sessions sent to %s: %s not written (error %lu)", p->folder, op->content, GetLastError());
                goto done;
            }
        }
        if ((grown = (SyncOp *)Grow(plan.ops, &plan.capacity, plan.count + 1, sizeof *plan.ops)) == NULL) goto done;
        plan.ops = grown;
        plan.ops[plan.count++] = *op;
    }
    ok = SavePlan(p, plan.ops, plan.count);
done:
    SessionEdit_Unlock(lock);
    Free(plan.ops);
    return ok;
}

/* ------------------------------------------------------------- backups */

static int __cdecl CompareNames(const void *a, const void *b)
{
    return wcscmp((const WCHAR *)a, (const WCHAR *)b);
}

/* A folder of ours deleted with what it holds. */
static void DeleteTree(const WCHAR *dir)
{
    WCHAR path[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(dir, L"*", &found, FALSE);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0 ||
                FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName)) || !Extended(path, extended))
                continue;
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                DeleteTree(path);
            else if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(extended);
            else
                DeleteFileW(extended);
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    if (Extended(dir, extended)) RemoveDirectoryW(extended);
}

/* The oldest backups beyond the latest BACKUPS_KEPT deleted: their names
 * are their times. */
static void PruneBackups(const WCHAR *backups)
{
    WCHAR (*names)[MAX_PATH] = NULL, path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find = Util_FindFiles(backups, L"*", &found, TRUE);
    int count = 0, capacity = 0, i;
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR (*grown)[MAX_PATH];
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || found.cFileName[0] == L'.') continue;
        if ((grown = (WCHAR (*)[MAX_PATH])Grow(names, &capacity, count + 1, sizeof *names)) == NULL) break;
        names = grown;
        StringCchCopyW(names[count++], MAX_PATH, found.cFileName);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (count > BACKUPS_KEPT) {
        qsort(names, (size_t)count, sizeof *names, CompareNames);
        for (i = 0; i < count - BACKUPS_KEPT; i++)
            if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", backups, names[i]))) DeleteTree(path);
    }
    Free(names);
}

/* The folder `p`'s backups go to in this report's backup, made at its first use. */
static BOOL BackupDir(const Profile *p, SyncReport *report, WCHAR *out, size_t cch)
{
    WCHAR state[MAX_PATH], backups[MAX_PATH];
    if (!report->backup[0]) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        if (!Util_StateDir(state, ARRAYSIZE(state)) ||
            FAILED(StringCchPrintfW(backups, ARRAYSIZE(backups), L"%s\\" BACKUPS_DIR, state)) ||
            FAILED(StringCchPrintfW(report->backup, ARRAYSIZE(report->backup), L"%s\\%04u-%02u-%02u %02u.%02u.%02u", backups,
                                    now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond)) ||
            !Util_EnsureDir(report->backup)) {
            report->backup[0] = 0;
            return FALSE;
        }
        PruneBackups(backups);
    }
    return SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\%s", report->backup, p->folder)) && Util_EnsureDir(out);
}

/* `file` of profile `p` kept in the backup first: moved (`move`, it goes
 * from the profile) or copied (it is about to be replaced). */
static BOOL BackUp(const Profile *p, const WCHAR *file, BOOL move, SyncReport *report)
{
    WCHAR dir[MAX_PATH], target[LONG_PATH_CCH], source[LONG_PATH_CCH], extended[LONG_PATH_CCH];
    const WCHAR *name = wcsrchr(file, L'\\');
    int copy;
    BOOL ok;
    if (!name || !BackupDir(p, report, dir, ARRAYSIZE(dir)) || !Extended(file, source)) return FALSE;
    for (copy = 1; copy <= BACKUP_COPIES_MAX; copy++) {
        if (FAILED(copy == 1 ? StringCchPrintfW(target, ARRAYSIZE(target), L"%s%s", dir, name)
                             : StringCchPrintfW(target, ARRAYSIZE(target), L"%s%s~%d", dir, name, copy)) ||
            !Extended(target, extended))
            return FALSE;
        if (Util_QueryPath(extended, NULL) == PATH_MISSING) break;
    }
    if (copy > BACKUP_COPIES_MAX) return FALSE;
    ok = move ? MoveFileExW(source, extended, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH) : CopyFileW(source, extended, TRUE);
    if (!ok) Util_Log(L"could not back up %s (error %lu)", file, GetLastError());
    return ok;
}

/* ---------------------------------------------------------- applying */

static BOOL MemberText(const char *json, size_t length, const char *key, WCHAR *out, size_t cch)
{
    const char *value;
    size_t valueLength;
    out[0] = 0;
    return Core_JsonMember(json, length, key, &value, &valueLength) && Core_JsonString(value, valueLength, out, cch);
}

static ULONGLONG MemberNumber(const char *json, size_t length, const char *key)
{
    const char *value;
    size_t valueLength;
    ULONGLONG number = 0;
    if (Core_JsonMember(json, length, key, &value, &valueLength)) Core_JsonNumber(value, valueLength, &number);
    return number;
}

/* `json` with member `key` set to the JSON string of `text`: a heap block
 * (NULL without memory, or when it is no object). */
static char *WithString(const char *json, size_t length, const char *key, const WCHAR *text, size_t *outLength)
{
    char quoted[PATH_JSON_BYTES], *out;
    size_t capacity;
    if (!Core_JsonQuote(text, quoted, sizeof quoted)) return NULL;
    capacity = length + strlen(key) + strlen(quoted) + 8;
    if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, capacity)) == NULL) return NULL;
    if (Core_JsonSetMember(json, length, key, quoted, out, capacity, outLength)) return out;
    Free(out);
    return NULL;
}

/* An entry's own id it can be named by in a file: local_ then letters,
 * digits and dashes. */
static BOOL IsEntryId(const WCHAR *id)
{
    const WCHAR *c;
    size_t prefix = ARRAYSIZE(LOCAL_PREFIX) - 1;
    if (wcsncmp(id, LOCAL_PREFIX, prefix) != 0 || !id[prefix]) return FALSE;
    for (c = id + prefix; *c; c++)
        if (!((*c >= L'0' && *c <= L'9') || (*c >= L'a' && *c <= L'z') || (*c >= L'A' && *c <= L'Z') || *c == L'-' || *c == L'_'))
            return FALSE;
    return TRUE;
}

/* A mark's id, as Claude names them: an id without local_. */
static BOOL TombstonePath(const WCHAR *dir, const WCHAR *id, WCHAR *out, size_t cch)
{
    size_t prefix = ARRAYSIZE(LOCAL_PREFIX) - 1;
    if (wcsncmp(id, LOCAL_PREFIX, prefix) == 0) id += prefix;
    return Core_IsUuid(id) && SUCCEEDED(StringCchPrintfW(out, cch, L"%s\\" TOMBSTONE_PREFIX L"%s", dir, id));
}

/* A session sent to `p`: its entry added, or the one there replaced (with
 * the profile's own id kept) unless it is newer or used since. */
static OpResult ApplyPut(const Profile *p, const SessionSet *entries, const WCHAR *dir, const SyncOp *op, const char *content,
                         size_t contentLength, SyncReport *report)
{
    WCHAR ownId[SESSION_ID_CCH], path[LONG_PATH_CCH], marks[2][LONG_PATH_CCH];
    char *data = NULL, *current = NULL;
    size_t length = 0;
    DWORD currentLength = 0;
    OpResult result = OP_FAILED;
    BOOL marked[2] = { FALSE, FALSE }, replacing = FALSE;
    int found = SessionStore_FindEntry(entries, 0, op->key, NULL), i;

    if (found >= 0) {
        const SessionEntry *entry = &entries->entries[found];
        if ((op->flags & SYNC_REPLACE) ? entry->lastActivity > max(op->time, op->seen) : entry->lastActivity >= op->time) {
            Util_Log(L"session %s in %s: kept, newer there", op->key, p->folder);
            return OP_SKIPPED;
        }
        if ((data = WithString(content, contentLength, "sessionId", entry->localId, &length)) == NULL) {
            CannotWrite(report, entry->file, ERROR_INVALID_DATA);
            return OP_FAILED;
        }
        StringCchCopyW(path, ARRAYSIZE(path), entry->file);
        replacing = TRUE;
    } else {
        if (!MemberText(content, contentLength, "sessionId", ownId, ARRAYSIZE(ownId)) || !IsEntryId(ownId) ||
            FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s.json", dir, ownId))) {
            CannotWrite(report, op->key, ERROR_INVALID_DATA);
            return OP_FAILED;
        }
        /* Claude's marks that the session was deleted here. */
        marked[0] = TombstonePath(dir, op->key, marks[0], ARRAYSIZE(marks[0])) && Present(marks[0]);
        marked[1] = !Core_EqualsI(ownId + ARRAYSIZE(LOCAL_PREFIX) - 1, op->key) &&
                    TombstonePath(dir, ownId, marks[1], ARRAYSIZE(marks[1])) && Present(marks[1]);
        if ((marked[0] || marked[1]) && !(op->flags & SYNC_UNDELETE)) {
            Util_Log(L"session %s in %s: kept out, deleted there", op->key, p->folder);
            return OP_SKIPPED;
        }
        if (Present(path)) {   /* an entry that could not be read, or of a session run elsewhere */
            if (!(op->flags & SYNC_REPLACE)) return OP_SKIPPED;
            replacing = TRUE;
        }
        if ((data = (char *)HeapAlloc(GetProcessHeap(), 0, contentLength + 1)) == NULL) {
            CannotWrite(report, path, ERROR_NOT_ENOUGH_MEMORY);
            return OP_FAILED;
        }
        memcpy(data, content, contentLength);
        length = contentLength;
    }
    if (replacing && (current = Util_ReadFile(path, SESSION_ENTRY_MAX_BYTES, FALSE, &currentLength)) != NULL &&
        currentLength == length && memcmp(current, data, length) == 0) {
        result = OP_SAME;
        goto done;
    }
    if (SessionLink_Busy(p)) {
        result = OP_STOPPED;
        goto done;
    }
    for (i = 0; i < 2; i++)
        if (marked[i] && !BackUp(p, marks[i], TRUE, report)) {
            CannotWrite(report, marks[i], GetLastError());
            goto done;
        }
    if (replacing && !BackUp(p, path, FALSE, report)) {
        CannotWrite(report, path, GetLastError());
        goto done;
    }
    if (WriteFileAt(path, data, length, p, replacing)) {
        result = OP_MADE;
        if (replacing) report->updated++;
        else report->added++;
        Util_Log(L"session %s in %s: %s", op->key, p->folder, replacing ? L"entry replaced" : L"entry added");
    } else if (GetLastError() == ERROR_BUSY) {
        result = OP_STOPPED;
    } else {
        CannotWrite(report, path, GetLastError());
    }
done:
    Free(data);
    Free(current);
    return result;
}

static ULONGLONG NowMs(void);

/* Claude's mark that session `id` (an id, or an entry's own) was deleted
 * from the entries folder `dir`, holding the time in ms. */
static BOOL WriteMark(const Profile *p, const WCHAR *dir, const WCHAR *id, ULONGLONG time)
{
    WCHAR path[LONG_PATH_CCH];
    char text[TIME_TEXT_BYTES];
    if (!TombstonePath(dir, id, path, ARRAYSIZE(path)) || Present(path)) return TRUE;
    StringCchPrintfA(text, sizeof text, "%I64u", time);
    return WriteFileAt(path, text, strlen(text), p, FALSE);
}

/* A session taken away from `p`: its entries to the backup, unless it was
 * used there since; with SYNC_MARKED, Claude's marks that it was deleted
 * there written too, so that Claude does not take it in again. */
static OpResult ApplyRemove(const Profile *p, const SessionSet *entries, const WCHAR *dir, const SyncOp *op, SyncReport *report)
{
    WCHAR ids[MAX_PROFILES][SESSION_ID_CCH];
    int entry = SessionStore_FindEntry(entries, 0, op->key, NULL), count = 0, i;
    if (entry < 0) return OP_SAME;
    if (entries->entries[entry].lastActivity > op->seen) {
        Util_Log(L"session %s in %s: kept, used there since", op->key, p->folder);
        return OP_SKIPPED;
    }
    for (; entry >= 0; entry = entries->entries[entry].duplicate) {
        if (SessionLink_Busy(p)) return OP_STOPPED;
        if (count < MAX_PROFILES) StringCchCopyW(ids[count++], SESSION_ID_CCH, entries->entries[entry].localId);
        if (!BackUp(p, entries->entries[entry].file, TRUE, report)) {
            CannotWrite(report, entries->entries[entry].file, GetLastError());
            return OP_FAILED;
        }
    }
    if (op->flags & SYNC_MARKED) {
        ULONGLONG now = NowMs();
        WriteMark(p, dir, op->key, now);
        for (i = 0; i < count; i++) WriteMark(p, dir, ids[i], now);
    }
    report->removed++;
    Util_Log(L"session %s in %s: entry removed", op->key, p->folder);
    return OP_MADE;
}

static OpResult ApplyMark(const Profile *p, const SessionSet *entries, const WCHAR *dir, const SyncOp *op, SyncReport *report)
{
    WCHAR path[LONG_PATH_CCH], local[SESSION_ID_CCH];
    char text[TIME_TEXT_BYTES];
    if (!TombstonePath(dir, op->key, path, ARRAYSIZE(path))) return OP_SAME;
    if (op->kind == SYNC_UNMARK) {
        if (!Present(path)) return OP_SAME;
        if (SessionLink_Busy(p)) return OP_STOPPED;
        if (BackUp(p, path, TRUE, report)) return OP_MADE;
        CannotWrite(report, path, GetLastError());
        return OP_FAILED;
    }
    /* Never over a session the profile lists. */
    StringCchPrintfW(local, ARRAYSIZE(local), LOCAL_PREFIX L"%s", op->key);
    if (Present(path) || SessionStore_FindEntry(entries, 0, op->key, NULL) >= 0 || SessionStore_FindEntry(entries, 0, local, NULL) >= 0)
        return OP_SAME;
    StringCchPrintfA(text, sizeof text, "%I64u", op->time);
    if (WriteFileAt(path, text, strlen(text), p, FALSE)) return OP_MADE;
    if (GetLastError() == ERROR_BUSY) return OP_STOPPED;
    CannotWrite(report, path, GetLastError());
    return OP_FAILED;
}

static OpResult ApplyIndex(const Profile *p, const WCHAR *dir, const char *content, size_t length, SyncReport *report)
{
    WCHAR path[LONG_PATH_CCH];
    DWORD currentLength = 0;
    char *current;
    BOOL there;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" ARCHIVED_INDEX, dir))) return OP_FAILED;
    current = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &currentLength);
    if (current && currentLength == length && memcmp(current, content, length) == 0) {
        Free(current);
        return OP_SAME;
    }
    Free(current);
    there = Present(path);
    if (SessionLink_Busy(p)) return OP_STOPPED;
    if (there && !BackUp(p, path, FALSE, report)) {
        CannotWrite(report, path, GetLastError());
        return OP_FAILED;
    }
    if (WriteFileAt(path, content, length, p, there)) return OP_MADE;
    if (GetLastError() == ERROR_BUSY) return OP_STOPPED;
    CannotWrite(report, path, GetLastError());
    return OP_FAILED;
}

/* ----------------------------------------------------- the sidebar's layout */

/* The pins, project order and filters of Claude's sidebar, in
 * claude_desktop_config.json under preferences.epitaxyPrefs: each part by its
 * name in a layout of ours, the name Claude gives it, and whether Claude keeps
 * it for every account or per account (the account's id after the name). */
typedef enum LayoutScope { LAYOUT_ANY, LAYOUT_ORGANIZATION, LAYOUT_ACCOUNT } LayoutScope;
static const struct { const char *part, *name; LayoutScope scope; } kLayoutParts[] = {
    { "starred", "starred-local-code-sessions", LAYOUT_ANY },
    { "slice", "dframe-local-slice", LAYOUT_ANY },
    { "starredGroups", "starred-session-groups", LAYOUT_ANY },
    { "order", "code-projects-order.", LAYOUT_ACCOUNT },
    { "statusFilter", "code-sessions-status-filter.", LAYOUT_ACCOUNT },             /* the filter's Status */
    { "environments", "code-sessions-selected-environments-v2.", LAYOUT_ACCOUNT },  /* ...its Environment */
    { "showEmpty", "code-sessions-show-empty-projects.", LAYOUT_ACCOUNT },          /* ...Show empty groups */
    { "showPrStatus", "code-sessions-show-pr-status.", LAYOUT_ACCOUNT },           /* ...Show PR status */
    { "projectsFilter", "ccd-sessions-filter", LAYOUT_ANY },                        /* the projects chosen */
};

/* The sidebar's groups and their sections, the "Edit sidebar" choices, the
 * groups folded, how sessions are grouped and sorted, and the editor's
 * settings: Claude's web UI keeps them in its web storage (webstore.c), most
 * under "state" of its value dframe-store. It keeps copies of the groups and
 * sections (dframe-group-scopes, dframe-code-sections) in the settings above
 * and in its web storage (LSS-persisted.<the same name>), and writes them from
 * dframe-store each time it starts: written in a copy alone, a change would
 * show nowhere. So these are read from the web storage and written to it and
 * to both copies alike. Each part: its name in a layout of ours, the value,
 * its member of "state" (NULL: the value whole), whether Claude keeps it per
 * account and organization (its member "<account>/<organization>"), and the
 * name of the copies. */
#define SIDEBAR_STORE   L"dframe-store"
#define COPY_PREFIX     L"LSS-persisted."
typedef struct WebPart {
    const char  *part;
    const WCHAR *value;
    const char  *member;
    LayoutScope  scope;
    const char  *copy;
} WebPart;
static const WebPart kWebParts[] = {
    { "groups", SIDEBAR_STORE, "customGroupsByScope", LAYOUT_ORGANIZATION, "dframe-group-scopes" },
    { "sections", SIDEBAR_STORE, "codeSidebarByScope", LAYOUT_ORGANIZATION, "dframe-code-sections" },
    { "navPins", SIDEBAR_STORE, "navPinnedIds", LAYOUT_ANY, NULL },               /* Edit sidebar */
    { "folded", SIDEBAR_STORE, "collapsedGroups", LAYOUT_ANY, NULL },
    { "groupBy", SIDEBAR_STORE, "groupByByMode", LAYOUT_ANY, NULL },
    { "sortBy", SIDEBAR_STORE, "sortByByMode", LAYOUT_ANY, NULL },
    { "recentsType", SIDEBAR_STORE, "recentsTypeFilter", LAYOUT_ANY, NULL },      /* the filter of Recents */
    { "recentsStatus", SIDEBAR_STORE, "recentsStatusFilter", LAYOUT_ANY, NULL },
    { "routines", SIDEBAR_STORE, "routinesSidebarPlacement", LAYOUT_ANY, NULL },
    { "sidebarWidth", SIDEBAR_STORE, "sidebarWidth", LAYOUT_ANY, NULL },
    { "interfaceFont", SIDEBAR_STORE, "interfaceFont", LAYOUT_ANY, NULL },
    { "systemFont", SIDEBAR_STORE, "systemFont", LAYOUT_ANY, NULL },
    { "editor", L"epitaxy-editor-prefs", NULL, LAYOUT_ANY, NULL },                 /* the editor's font, theme, text size */
};

const char *SessionSync_LayoutPart(int index)
{
    if (index < 0) return NULL;
    if (index < (int)ARRAYSIZE(kLayoutParts)) return kLayoutParts[index].part;
    index -= (int)ARRAYSIZE(kLayoutParts);
    return index < (int)ARRAYSIZE(kWebParts) ? kWebParts[index].part : NULL;
}

/* The account and organization an entries folder is for (its last two
 * folders), as UTF-8, and Claude's name for both: "<account>/<organization>". */
static BOOL LayoutScopeOf(const WCHAR *entriesDir, char *account, size_t accountCap, char *scope, size_t scopeCap)
{
    const WCHAR *last = wcsrchr(entriesDir, L'\\'), *before;
    WCHAR accountName[SESSION_ID_CCH];
    char organization[SESSION_ID_CCH * 3];
    if (!last || last == entriesDir || !last[1]) return FALSE;
    for (before = last - 1; before > entriesDir && *before != L'\\'; before--) {}
    if (*before != L'\\' || before + 1 == last || FAILED(StringCchCopyNW(accountName, ARRAYSIZE(accountName), before + 1, (size_t)(last - before - 1))))
        return FALSE;
    return WideCharToMultiByte(CP_UTF8, 0, accountName, -1, account, (int)accountCap, NULL, NULL) > 0 &&
           WideCharToMultiByte(CP_UTF8, 0, last + 1, -1, organization, (int)sizeof organization, NULL, NULL) > 0 &&
           SUCCEEDED(StringCchPrintfA(scope, scopeCap, "%s/%s", account, organization));
}

/* `json` (a heap block, freed) with its member `key` set to `raw` (`length`
 * bytes); NULL, freed, when it cannot be. */
static char *SetRaw(char *json, size_t *used, const char *key, const char *raw, size_t length)
{
    size_t capacity, written = 0;
    char *text = (char *)HeapAlloc(GetProcessHeap(), 0, length + 1), *out = NULL;
    if (json && text) {
        memcpy(text, raw, length);
        text[length] = 0;
        capacity = *used + length + strlen(key) + 8;
        if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, capacity + 1)) != NULL && Core_JsonSetMember(json, *used, key, text, out, capacity, &written)) {
            out[written] = 0;
            *used = written;
        } else {
            Free(out);
            out = NULL;
        }
    }
    Free(text);
    Free(json);
    return out;
}

/* `*json` (a heap block) with the member reached through `keys` set to
 * `length` bytes of `raw`; FALSE, `*json` as it was, when it cannot be. */
static BOOL SetNested(char **json, size_t *used, const char *const *keys, int depth, const char *raw, size_t length)
{
    char *text, *out = NULL;
    size_t outLength = 0;
    if (*json && (text = (char *)HeapAlloc(GetProcessHeap(), 0, length + 1)) != NULL) {
        memcpy(text, raw, length);
        text[length] = 0;
        out = Core_JsonSetNested(*json, *used, keys, depth, text, &outLength);
        Free(text);
    }
    if (!out) return FALSE;
    Free(*json);
    *json = out;
    *used = outLength;
    return TRUE;
}

static char *Duplicate(const char *text, size_t length)
{
    char *copy = (char *)HeapAlloc(GetProcessHeap(), 0, length + 1);
    if (copy) {
        memcpy(copy, text, length);
        copy[length] = 0;
    }
    return copy;
}

static char *EmptyObject(size_t *used)
{
    char *json = (char *)HeapAlloc(GetProcessHeap(), 0, 3);
    if (json) memcpy(json, "{}", 3);
    *used = 2;
    return json;
}

/* Part `part` of the web storage value `text`: where it is. A value whole is
 * one of Claude's stores ({"state": ..., "version": ...}) or none. */
static BOOL WebPartOf(const WebPart *part, const char *text, size_t length, const char *scope, const char **value, size_t *valueLength)
{
    const char *state, *inner;
    size_t stateLength, innerLength;
    if (!text || !Core_JsonMember(text, length, "state", &state, &stateLength)) return FALSE;
    if (!part->member) {
        *value = text;
        *valueLength = length;
        return TRUE;
    }
    if (!Core_JsonMember(state, stateLength, part->member, &inner, &innerLength) ||
        (part->scope == LAYOUT_ORGANIZATION && !Core_JsonMember(inner, innerLength, scope, &inner, &innerLength)))
        return FALSE;
    *value = inner;
    *valueLength = innerLength;
    return TRUE;
}

/* The parts of `layout` (a heap block, freed) read from the web storage of
 * `p`; `written` made the time its newest file was written, when later. */
static char *ReadWebParts(const Profile *p, const char *scope, char *layout, size_t *used, ULONGLONG *written)
{
    WebStore *store = WebStore_Open(p);
    const WCHAR *read = NULL;
    char *text = NULL;
    const char *value;
    size_t length = 0, valueLength, i;
    if (!store) return layout;
    if (written) *written = max(*written, WebStore_Written(store));
    for (i = 0; i < ARRAYSIZE(kWebParts) && layout; i++) {
        if (!read || wcscmp(read, kWebParts[i].value) != 0) {
            Free(text);
            text = WebStore_Get(store, kWebParts[i].value, &length);
            read = kWebParts[i].value;
        }
        if (WebPartOf(&kWebParts[i], text, length, scope, &value, &valueLength))
            layout = SetRaw(layout, used, kWebParts[i].part, value, valueLength);
    }
    Free(text);
    WebStore_Free(store);
    return layout;
}

char *SessionSync_ReadLayout(const Profile *p, const WCHAR *entriesDir, size_t *length, ULONGLONG *written)
{
    WCHAR path[LONG_PATH_CCH];
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    char account[SESSION_ID_CCH * 3], scope[SESSION_ID_CCH * 6 + 2], name[128], *config, *layout;
    const char *prefs, *epitaxy, *value;
    size_t prefsLength, epitaxyLength, valueLength, used = 0, i;
    DWORD size = 0;
    *length = 0;
    if (written) *written = 0;
    if (!p->storageDir[0] || !LayoutScopeOf(entriesDir, account, sizeof account, scope, sizeof scope) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir)) || (layout = EmptyObject(&used)) == NULL)
        return NULL;
    if ((config = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &size)) != NULL) {
        if (written && GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) {
            ULONGLONG ticks = ((ULONGLONG)attributes.ftLastWriteTime.dwHighDateTime << 32) | attributes.ftLastWriteTime.dwLowDateTime;
            *written = ticks > UNIX_EPOCH_TICKS ? (ticks - UNIX_EPOCH_TICKS) / TICKS_PER_MILLISECOND : 0;
        }
        if (Core_JsonMember(config, size, "preferences", &prefs, &prefsLength) &&
            Core_JsonMember(prefs, prefsLength, "epitaxyPrefs", &epitaxy, &epitaxyLength))
            for (i = 0; i < ARRAYSIZE(kLayoutParts) && layout; i++)
                if (SUCCEEDED(StringCchPrintfA(name, sizeof name, "%s%s", kLayoutParts[i].name, kLayoutParts[i].scope == LAYOUT_ACCOUNT ? account : "")) &&
                    Core_JsonMember(epitaxy, epitaxyLength, name, &value, &valueLength))
                    layout = SetRaw(layout, &used, kLayoutParts[i].part, value, valueLength);
        Free(config);
    }
    if (layout) layout = ReadWebParts(p, scope, layout, &used, written);
    if (layout && used <= 2) {
        Free(layout);
        layout = NULL;
    }
    if (layout) *length = used;
    return layout;
}

/* The parts of `content` kept in claude_desktop_config.json, and the copies
 * of the groups and sections there, put in that file of `p`. */
static OpResult ApplySettings(const Profile *p, const char *account, const char *scope, const char *content, size_t contentLength,
                              SyncReport *report)
{
    WCHAR path[LONG_PATH_CCH];
    char name[128];
    const char *value;
    size_t valueLength, used = 0, i;
    DWORD currentLength = 0;
    char *current, *json = NULL;
    BOOL there;
    OpResult result = OP_FAILED;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir))) {
        CannotWrite(report, p->folder, ERROR_INVALID_DATA);
        return OP_FAILED;
    }
    current = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &currentLength);
    there = current != NULL;
    if (current) {
        if ((json = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)currentLength + 1)) != NULL) {
            memcpy(json, current, currentLength);
            json[currentLength] = 0;
            used = currentLength;
        }
    } else {
        json = EmptyObject(&used);
    }
    for (i = 0; i < ARRAYSIZE(kLayoutParts) && json; i++) {
        const char *keys[3] = { "preferences", "epitaxyPrefs", name };
        if (Core_JsonMember(content, contentLength, kLayoutParts[i].part, &value, &valueLength) &&
            SUCCEEDED(StringCchPrintfA(name, sizeof name, "%s%s", kLayoutParts[i].name, kLayoutParts[i].scope == LAYOUT_ACCOUNT ? account : "")))
            if (!SetNested(&json, &used, keys, 3, value, valueLength)) break;
    }
    for (i = 0; i < ARRAYSIZE(kWebParts) && json; i++) {
        const char *keys[4] = { "preferences", "epitaxyPrefs", kWebParts[i].copy, scope };
        if (kWebParts[i].copy && Core_JsonMember(content, contentLength, kWebParts[i].part, &value, &valueLength) &&
            !SetNested(&json, &used, keys, 4, value, valueLength))
            break;
    }
    if (!json || i < ARRAYSIZE(kWebParts)) {
        CannotWrite(report, path, ERROR_INVALID_DATA);
        goto done;
    }
    if (current && used == currentLength && memcmp(current, json, used) == 0) {
        result = OP_SAME;
        goto done;
    }
    if (SessionLink_Busy(p)) {
        result = OP_STOPPED;
        goto done;
    }
    if (there && !BackUp(p, path, FALSE, report)) {
        CannotWrite(report, path, GetLastError());
        goto done;
    }
    if (WriteFileAt(path, json, used, p, there)) {
        result = OP_MADE;
        Util_Log(L"the sidebar's settings in %s: replaced", p->folder);
    } else if (GetLastError() == ERROR_BUSY) {
        result = OP_STOPPED;
    } else {
        CannotWrite(report, path, GetLastError());
    }
done:
    Free(json);
    Free(current);
    return result;
}

/* Web storage value `name` of `store` with the parts of `content` it keeps
 * set in it: a value whole made where there is none, a member of one only
 * where Claude made that value; `*changed` when it changed. A member Claude
 * keeps as something else than an object stays as it is. */
static BOOL SetWebValue(WebStore *store, const WCHAR *name, const char *scope, const char *content, size_t contentLength, BOOL *changed)
{
    const char *value;
    size_t valueLength, length = 0, used, i;
    char *text = WebStore_Get(store, name, &length), *json = text ? Duplicate(text, length) : NULL;
    BOOL ok = TRUE;
    used = length;
    for (i = 0; i < ARRAYSIZE(kWebParts); i++) {
        const char *keys[3] = { "state", kWebParts[i].member, scope };
        if (wcscmp(kWebParts[i].value, name) != 0 || !Core_JsonMember(content, contentLength, kWebParts[i].part, &value, &valueLength))
            continue;
        if (!kWebParts[i].member) {
            Free(json);
            json = Duplicate(value, valueLength);
            used = valueLength;
        } else if (json && !SetNested(&json, &used, keys, kWebParts[i].scope == LAYOUT_ORGANIZATION ? 3 : 2, value, valueLength)) {
            Util_Log(L"web storage %s: its %S kept, not an object there", name, kWebParts[i].part);
        }
    }
    if (json && (!text || used != length || memcmp(json, text, used) != 0)) {
        ok = WebStore_Set(store, name, json, used);
        *changed = TRUE;
    }
    Free(text);
    Free(json);
    return ok;
}

/* The copy LSS-persisted.<copy> of part `part` of `content`: its member of
 * "value" for `scope`, and its time now. */
static BOOL SetWebCopy(WebStore *store, const WebPart *part, const char *scope, const char *content, size_t contentLength, BOOL *changed)
{
    WCHAR name[128];
    const char *value, *inner;
    const char *keys[2] = { "value", scope }, *timeKey[1] = { "timestamp" };
    size_t valueLength, innerLength, length = 0, used;
    char *text, *json, now[TIME_TEXT_BYTES];
    BOOL ok = TRUE;
    if (!Core_JsonMember(content, contentLength, part->part, &value, &valueLength) ||
        FAILED(StringCchPrintfW(name, ARRAYSIZE(name), COPY_PREFIX L"%S", part->copy)))
        return TRUE;
    text = WebStore_Get(store, name, &length);
    if (text && Core_JsonMember(text, length, "value", &inner, &innerLength) && Core_JsonMember(inner, innerLength, scope, &inner, &innerLength) &&
        innerLength == valueLength && memcmp(inner, value, valueLength) == 0) {
        Free(text);
        return TRUE;
    }
    if (text) {
        json = text;
        used = length;
    } else {
        static const char kEmpty[] = "{\"value\":{},\"tabId\":\"\",\"timestamp\":0}";
        json = Duplicate(kEmpty, sizeof kEmpty - 1);
        used = sizeof kEmpty - 1;
    }
    StringCchPrintfA(now, sizeof now, "%I64u", NowMs());
    if (json && SetNested(&json, &used, keys, 2, value, valueLength) && SetNested(&json, &used, timeKey, 1, now, strlen(now))) {
        ok = WebStore_Set(store, name, json, used);
        *changed = TRUE;
    } else {
        Util_Log(L"web storage %s: kept, not Claude's copy", name);
    }
    Free(json);
    return ok;
}

/* The parts of `content` kept in the web storage of `p` written there, with
 * its copies; the database copied to the backup first. */
static OpResult ApplyWeb(const Profile *p, const char *scope, const char *content, size_t contentLength, SyncReport *report)
{
    WCHAR backup[MAX_PATH];
    WebStore *store;
    const WCHAR *names[ARRAYSIZE(kWebParts)];
    const char *value;
    size_t valueLength, i, j, count = 0;
    BOOL changed = FALSE, ok = TRUE, any = FALSE;
    OpResult result = OP_FAILED;
    for (i = 0; i < ARRAYSIZE(kWebParts); i++)
        if (Core_JsonMember(content, contentLength, kWebParts[i].part, &value, &valueLength)) any = TRUE;
    /* None yet (Claude makes it at its first start): the next sync sends them again. */
    if (!any || (store = WebStore_Open(p)) == NULL) return OP_SAME;
    for (i = 0; i < ARRAYSIZE(kWebParts); i++) {
        for (j = 0; j < count && wcscmp(names[j], kWebParts[i].value) != 0; j++) {}
        if (j == count) names[count++] = kWebParts[i].value;
    }
    for (i = 0; i < count && ok; i++) ok = SetWebValue(store, names[i], scope, content, contentLength, &changed);
    /* The copies only where the value they copy is. */
    for (i = 0; i < ARRAYSIZE(kWebParts) && ok; i++) {
        size_t length = 0;
        char *text;
        if (!kWebParts[i].copy) continue;
        text = WebStore_Get(store, kWebParts[i].value, &length);
        if (text) ok = SetWebCopy(store, &kWebParts[i], scope, content, contentLength, &changed);
        Free(text);
    }
    if (!ok) {
        CannotWrite(report, p->folder, ERROR_INVALID_DATA);
    } else if (!changed) {
        result = OP_SAME;
    } else if (SessionLink_Busy(p)) {
        result = OP_STOPPED;
    } else if (!BackupDir(p, report, backup, ARRAYSIZE(backup)) || FAILED(StringCchCatW(backup, ARRAYSIZE(backup), L"\\" CLAUDE_WEB_STORAGE))) {
        CannotWrite(report, p->folder, GetLastError());
    } else if (WebStore_Commit(store, backup)) {
        result = OP_MADE;
        Util_Log(L"the sidebar's groups and settings in the web storage of %s: replaced", p->folder);
    } else {
        CannotWrite(report, p->folder, GetLastError());
    }
    WebStore_Free(store);
    return result;
}

/* The layout `content` (a layout of ours) put in the sidebar of `p`, for the
 * account and organization of its entries folder `dir`: in its settings, then
 * in its web storage. What else they hold stays as it is. */
static OpResult ApplyLayout(const Profile *p, const WCHAR *dir, const char *content, size_t contentLength, SyncReport *report)
{
    char account[SESSION_ID_CCH * 3], scope[SESSION_ID_CCH * 6 + 2];
    OpResult settings, web;
    if (!p->storageDir[0] || !LayoutScopeOf(dir, account, sizeof account, scope, sizeof scope)) {
        CannotWrite(report, p->folder, ERROR_INVALID_DATA);
        return OP_FAILED;
    }
    settings = ApplySettings(p, account, scope, content, contentLength, report);
    if (settings == OP_STOPPED || settings == OP_FAILED) return settings;
    web = ApplyWeb(p, scope, content, contentLength, report);
    if (web == OP_STOPPED || web == OP_FAILED) return web;
    return settings == OP_MADE || web == OP_MADE ? OP_MADE : OP_SAME;
}

/* Each change this thread makes, told to a progress bar. */
typedef struct ChangeHook {
    void (*step)(void *context);
    void *context;
} ChangeHook;
static __declspec(thread) ChangeHook g_onChange;

void SessionSync_OnEachChange(void (*step)(void *context), void *context)
{
    g_onChange.step = step;
    g_onChange.context = context;
}

static OpResult ApplyOp(const Profile *p, const SessionSet *entries, const WCHAR *dir, const WCHAR *staging, const SyncOp *op,
                        SyncReport *report)
{
    WCHAR path[MAX_PATH];
    DWORD length = 0;
    char *content = NULL;
    OpResult result;
    if (op->kind == SYNC_PUT || op->kind == SYNC_INDEX || op->kind == SYNC_LAYOUT) {
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", staging, op->content)) ||
            (content = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &length)) == NULL) {
            CannotRead(report, path, GetLastError());
            return OP_FAILED;
        }
    }
    switch (op->kind) {
    case SYNC_PUT:    result = ApplyPut(p, entries, dir, op, content, length, report); break;
    case SYNC_REMOVE: result = ApplyRemove(p, entries, dir, op, report); break;
    case SYNC_INDEX:  result = ApplyIndex(p, dir, content, length, report); break;
    case SYNC_LAYOUT: result = ApplyLayout(p, dir, content, length, report); break;
    default:          result = ApplyMark(p, entries, dir, op, report); break;
    }
    if (g_onChange.step) g_onChange.step(g_onChange.context);
    if (result == OP_SKIPPED && (op->kind == SYNC_PUT || op->kind == SYNC_REMOVE)) report->skipped++;
    Free(content);
    return result;
}

/* Makes the changes sent to `p` while its Claude is closed, in order; one
 * its Claude overtakes waits with those after it. Returns how many left the
 * plan. A profile with no entries folder yet keeps them: made once it has. */
int SessionSync_ApplyPending(const Profile *p, SyncReport *report)
{
    SyncReport own;
    SessionSet entries;
    WCHAR staging[MAX_PATH];
    HANDLE lock = NULL;
    Plan plan;
    int i = 0, n = 0;
    ZeroMemory(&plan, sizeof plan);
    if (!report) {
        ZeroMemory(&own, sizeof own);
        report = &own;
    }
    if (!PlanDir(p, staging, ARRAYSIZE(staging)) || (lock = SessionEdit_Lock(p)) == NULL) return 0;
    if (SessionLink_Busy(p) || (n = LoadPlan(p, &plan)) <= 0) {
        if (!SessionLink_Busy(p) && n < 0) Util_Log(L"sessions sent to %s: the plan cannot be read", p->folder);
        SessionEdit_Unlock(lock);
        Free(plan.ops);
        return 0;
    }
    SessionStore_LoadEntries(&entries, p);
    if (!entries.source[0].entriesDir[0]) {
        Util_Log(L"sessions sent to %s: kept, it has no session entries yet", p->folder);
    } else {
        for (i = 0; i < plan.count; i++) {
            if (ApplyOp(p, &entries, entries.source[0].entriesDir, staging, &plan.ops[i], report) == OP_STOPPED) {
                Util_Log(L"sessions sent to %s: the rest waits, its Claude started", p->folder);
                break;
            }
            DeleteContent(staging, &plan.ops[i]);
        }
        if (i && !SavePlan(p, plan.ops + i, plan.count - i)) Util_Log(L"sessions sent to %s: the plan could not be written", p->folder);
    }
    SessionEdit_Unlock(lock);
    SessionStore_Free(&entries);
    Free(plan.ops);
    if (i) Util_Log(L"%d change(s) sent to %s made", i, p->folder);
    return i;
}

/* Each profile's part to its plan, made at once where it is closed. */
static void Send(Outbox *box, const SessionSet *set, SyncReport *report)
{
    int p;
    for (p = 0; p < set->profiles.count; p++) {
        const Profile *profile = &set->profiles.items[p];
        if (!box->sent[p].count) continue;
        if (!StageSent(profile, &box->sent[p])) {
            WCHAR path[MAX_PATH];
            if (!SessionSync_PlanPath(profile, path, ARRAYSIZE(path))) StringCchCopyW(path, ARRAYSIZE(path), profile->folder);
            CannotWrite(report, path, GetLastError());
            continue;
        }
        SessionSync_ApplyPending(profile, report);
        if (SessionSync_PendingCount(profile) > 0) report->waiting |= 1u << p;
    }
}

/* ----------------------------------------------------------- planning */

DWORD SessionSync_Takers(const SessionSet *set)
{
    DWORD takers = 0;
    int p;
    for (p = 0; p < set->profiles.count; p++)
        if (set->source[p].entriesDir[0]) takers |= 1u << p;
    return takers;
}

static const SessionEntry *EntryIn(const SessionSet *set, const SessionRow *row, int p)
{
    return p >= 0 && row->entry[p] >= 0 ? &set->entries[row->entry[p]] : NULL;
}

static const SessionEntry *Kept(const SessionSet *set, const SessionRow *row, int p)
{
    const SessionEntry *entry = EntryIn(set, row, p);
    return entry && !entry->pendingRemove ? entry : NULL;
}

/* The profile whose entry of `row` is sent: `from`'s when it keeps the
 * session, else the latest of `among`; -1 for none. */
static int SourceOf(const SessionSet *set, const SessionRow *row, int from, DWORD among)
{
    int p, best = -1;
    if (Kept(set, row, from)) return from;
    for (p = 0; p < set->profiles.count; p++)
        if ((among & (1u << p)) && Kept(set, row, p) &&
            (best < 0 || set->entries[row->entry[p]].lastActivity > set->entries[row->entry[best]].lastActivity))
            best = p;
    return best;
}

/* An entry's file read whole, owned by `box`; NULL (the failure in `report`) when it cannot be. */
static char *ReadEntry(Outbox *box, const WCHAR *file, size_t *length, SyncReport *report)
{
    DWORD read = 0;
    char *data = Util_ReadFile(file, SESSION_ENTRY_MAX_BYTES, FALSE, &read);
    *length = read;
    if (!data) {
        CannotRead(report, file, GetLastError());
        return NULL;
    }
    return Keep(box, data);
}

/* `content` as profile `t` gets it (SessionVault_ForProfile), owned by
 * `box`: in a "no folder" area, its own. */
static const char *ContentFor(Outbox *box, const SessionSet *set, int t, const char *content, size_t length, size_t *outLength)
{
    char *mapped = SessionVault_ForProfile(set, t, content, length, outLength);
    if (mapped && Keep(box, mapped)) return mapped;
    *outLength = length;
    return content;
}

static void MakeOp(SyncOp *op, SyncOpKind kind, DWORD flags, const WCHAR *key, ULONGLONG time, ULONGLONG seen)
{
    ZeroMemory(op, sizeof *op);
    op->kind = kind;
    op->flags = flags;
    op->time = time;
    op->seen = seen;
    StringCchCopyW(op->key, ARRAYSIZE(op->key), key);
}

/* What could not be sent at all: no memory. */
static BOOL OutOfMemory(SyncReport *report)
{
    CannotWrite(report, L"", ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
}

static DWORD ValidProfiles(const SessionSet *set, DWORD profiles)
{
    return set->profiles.count >= 32 ? profiles : profiles & ((1u << set->profiles.count) - 1);
}

BOOL SessionSync_Merge(const SessionSet *set, DWORD profiles, SyncReport *report)
{
    Outbox box;
    DWORD takers;
    int r, t;
    ZeroMemory(&box, sizeof box);
    profiles = ValidProfiles(set, profiles);
    takers = profiles & SessionSync_Takers(set);
    report->unavailable |= profiles & ~takers;
    for (r = 0; r < set->rowCount; r++) {
        const SessionRow *row = &set->rows[r];
        int best = SourceOf(set, row, -1, profiles);
        const SessionEntry *newest;
        const char *content = NULL;
        size_t length = 0;
        if (best < 0) continue;
        newest = &set->entries[row->entry[best]];
        for (t = 0; t < set->profiles.count; t++) {
            const SessionEntry *there = EntryIn(set, row, t);
            const char *mapped;
            size_t mappedLength = 0;
            SyncOp op;
            if (t == best || !(takers & (1u << t))) continue;
            if (there && (there->pendingRemove || there->lastActivity >= newest->lastActivity)) continue;
            if (!content && (content = ReadEntry(&box, newest->file, &length, report)) == NULL) break;
            mapped = ContentFor(&box, set, t, content, length, &mappedLength);
            MakeOp(&op, SYNC_PUT, 0, row->key, newest->lastActivity, there ? there->lastActivity : 0);
            if (!AddSent(&box.sent[t], &op, mapped, mappedLength)) {
                FreeOutbox(&box);
                return OutOfMemory(report);
            }
        }
    }
    Send(&box, set, report);
    FreeOutbox(&box);
    return TRUE;
}

BOOL SessionSync_Send(const SessionSet *set, const SyncSend *changes, int count, SyncReport *report)
{
    Outbox box;
    DWORD takers = SessionSync_Takers(set);
    int i;
    ZeroMemory(&box, sizeof box);
    for (i = 0; i < count; i++) {
        const SyncSend *change = &changes[i];
        if (change->profile < 0 || change->profile >= set->profiles.count) continue;
        if (!(takers & (1u << change->profile))) {
            report->unavailable |= 1u << change->profile;
            continue;
        }
        if (!AddSent(&box.sent[change->profile], &change->op, change->content, change->length)) {
            FreeOutbox(&box);
            return OutOfMemory(report);
        }
    }
    Send(&box, set, report);
    FreeOutbox(&box);
    return TRUE;
}

static ULONGLONG NowMs(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime) - UNIX_EPOCH_TICKS) / TICKS_PER_MILLISECOND;
}

BOOL SessionSync_Remove(const SessionSet *set, DWORD profiles, const int *rows, int rowCount, SyncReport *report)
{
    Outbox box;
    int i, p;
    ZeroMemory(&box, sizeof box);
    profiles = ValidProfiles(set, profiles);
    for (i = 0; i < rowCount; i++) {
        const SessionRow *row;
        if (rows[i] < 0 || rows[i] >= set->rowCount) continue;
        row = &set->rows[rows[i]];
        for (p = 0; p < set->profiles.count; p++) {
            const SessionEntry *there = EntryIn(set, row, p);
            SyncOp op;
            if (!(profiles & (1u << p)) || !there || there->pendingRemove) continue;
            MakeOp(&op, SYNC_REMOVE, SYNC_MARKED, row->key, 0, there->lastActivity);
            if (!AddSent(&box.sent[p], &op, NULL, 0)) {
                FreeOutbox(&box);
                return OutOfMemory(report);
            }
        }
    }
    Send(&box, set, report);
    FreeOutbox(&box);
    return TRUE;
}

BOOL SessionSync_Share(const SessionSet *set, int from, const int *rows, int rowCount, DWORD targets, SyncReport *report)
{
    Outbox box;
    DWORD takers;
    int i, t;
    ZeroMemory(&box, sizeof box);
    targets = ValidProfiles(set, targets) & ~(from >= 0 ? 1u << from : 0);
    takers = targets & SessionSync_Takers(set);
    report->unavailable |= targets & ~takers;
    for (i = 0; i < rowCount; i++) {
        const SessionRow *row;
        const char *content = NULL;
        size_t length = 0;
        int source;
        if (rows[i] < 0 || rows[i] >= set->rowCount) continue;
        row = &set->rows[rows[i]];
        if ((source = SourceOf(set, row, from, (DWORD)-1)) < 0) {
            report->skipped++;
            continue;
        }
        for (t = 0; t < set->profiles.count; t++) {
            const SessionEntry *there = EntryIn(set, row, t);
            const char *mapped;
            size_t mappedLength = 0;
            SyncOp op;
            if (!(takers & (1u << t)) || (there && !there->pendingRemove)) continue;   /* it has it already */
            if (there) {   /* its removal waits there: it goes when that profile closes */
                report->skipped++;
                continue;
            }
            if (!content && (content = ReadEntry(&box, set->entries[row->entry[source]].file, &length, report)) == NULL) break;
            mapped = ContentFor(&box, set, t, content, length, &mappedLength);
            MakeOp(&op, SYNC_PUT, SYNC_UNDELETE, row->key, set->entries[row->entry[source]].lastActivity, 0);
            if (!AddSent(&box.sent[t], &op, mapped, mappedLength)) {
                FreeOutbox(&box);
                return OutOfMemory(report);
            }
        }
    }
    Send(&box, set, report);
    FreeOutbox(&box);
    return TRUE;
}

static BOOL NewEntryId(WCHAR *out, size_t cch)
{
    GUID guid;
    WCHAR text[GUID_TEXT_CCH];
    size_t i;
    if (FAILED(CoCreateGuid(&guid)) || StringFromGUID2(&guid, text, ARRAYSIZE(text)) != ARRAYSIZE(text)) return FALSE;
    text[ARRAYSIZE(text) - 2] = 0;   /* the closing brace */
    for (i = 1; text[i]; i++) text[i] = (WCHAR)towlower(text[i]);
    return SUCCEEDED(StringCchPrintfW(out, cch, LOCAL_PREFIX L"%s", text + 1));
}

/* The entry of a copy: the original's, under its own id and transcript
 * `copyId`, in the working folder it got (a session without a folder), and
 * without the original's other transcripts: the copy goes on from its own.
 * A heap block; NULL when it cannot be made. */
static char *CopyEntry(const char *original, size_t length, const WCHAR *copyId, size_t *outLength)
{
    static const char *const kOthers[] = { "priorCliSessionIds", "preClearCliSessionId", "unarchivedCliSessionId", "stagedTranscriptPath" };
    WCHAR ownId[SESSION_ID_CCH], cwd[MAX_PATH];
    const char *value;
    size_t valueLength, i;
    char *data, *next;
    if (!NewEntryId(ownId, ARRAYSIZE(ownId)) || (data = WithString(original, length, "sessionId", ownId, &length)) == NULL) return NULL;
    if ((next = WithString(data, length, "cliSessionId", copyId, &length)) == NULL) goto failed;
    Free(data);
    data = next;
    if (SessionEdit_CopiedCwd(copyId, cwd, ARRAYSIZE(cwd))) {
        if ((next = WithString(data, length, "cwd", cwd, &length)) == NULL) goto failed;
        Free(data);
        data = next;
        if (Core_JsonMember(data, length, "originCwd", &value, &valueLength)) {
            if ((next = WithString(data, length, "originCwd", cwd, &length)) == NULL) goto failed;
            Free(data);
            data = next;
        }
    }
    for (i = 0; i < ARRAYSIZE(kOthers); i++) {
        size_t shorter;
        if (!Core_JsonRemoveMember(data, length, kOthers[i], data, length, &shorter)) goto failed;
        length = shorter;
    }
    *outLength = length;
    return data;
failed:
    Free(data);
    return NULL;
}

CopyResult SessionSync_Copy(HWND owner, const SessionSet *set, int from, const int *rows, int rowCount, DWORD targets, SyncReport *report)
{
    WCHAR copyId[SESSION_ID_CCH], error[LONG_PATH_CCH], left[LONG_PATH_CCH];
    DWORD takers, sent = 0;
    CopyResult result = COPY_MADE;
    int i, t, p;
    targets = ValidProfiles(set, targets) & ~(from >= 0 ? 1u << from : 0);
    takers = targets & SessionSync_Takers(set);
    report->unavailable |= targets & ~takers;
    for (i = 0; i < rowCount && result != COPY_CANCELLED; i++) {
        const SessionRow *row;
        DWORD length = 0;
        char *original;
        int source;
        if (rows[i] < 0 || rows[i] >= set->rowCount) continue;
        row = &set->rows[rows[i]];
        if ((source = SourceOf(set, row, from, (DWORD)-1)) < 0 || !row->transcript || !Core_IsUuid(row->key)) {
            report->skipped++;
            continue;
        }
        if ((original = Util_ReadFile(set->entries[row->entry[source]].file, SESSION_ENTRY_MAX_BYTES, FALSE, &length)) == NULL) {
            CannotRead(report, set->entries[row->entry[source]].file, GetLastError());
            continue;
        }
        for (t = 0; t < set->profiles.count && result != COPY_CANCELLED; t++) {
            Sent one;
            SyncOp op;
            char *entry;
            size_t entryLength = 0;
            CopyResult copied;
            if (!(takers & (1u << t))) continue;
            if (set->groups[row->group].scratchOf >= 0 && !set->source[t].scratchDir[0]) {
                report->skipped++;
                continue;
            }
            copied = SessionEdit_CopyConversation(owner, set, rows[i], t, copyId, ARRAYSIZE(copyId), error, ARRAYSIZE(error));
            if (copied == COPY_CANCELLED) {
                result = COPY_CANCELLED;
                break;
            }
            if (copied == COPY_FAILED) {
                report->failed++;
                if (!report->error[0]) StringCchCopyW(report->error, ARRAYSIZE(report->error), error);
                continue;
            }
            ZeroMemory(&one, sizeof one);
            MakeOp(&op, SYNC_PUT, SYNC_UNDELETE, copyId, MemberNumber(original, length, "lastActivityAt"), 0);
            entry = CopyEntry(original, length, copyId, &entryLength);
            /* Not sent, the copy is listed nowhere: what it made goes again. */
            if (!entry || !AddSent(&one, &op, entry, entryLength) || !StageSent(&set->profiles.items[t], &one)) {
                CannotWrite(report, set->profiles.items[t].folder, entry ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY);
                if (!SessionEdit_RemoveCopy(set, rows[i], t, copyId, left, ARRAYSIZE(left)) && left[0])
                    Util_Log(L"session copy %s: %s left behind", copyId, left);
            } else {
                sent |= 1u << t;
            }
            FreeSent(&one);
            Free(entry);
        }
        Free(original);
    }
    for (p = 0; p < set->profiles.count; p++) {
        if (!(sent & (1u << p))) continue;
        SessionSync_ApplyPending(&set->profiles.items[p], report);
        if (SessionSync_PendingCount(&set->profiles.items[p]) > 0) report->waiting |= 1u << p;
    }
    return result;
}

/* ------------------------------------------------------------- archives */

static void Put16(unsigned char *at, DWORD value)
{
    at[0] = (unsigned char)value;
    at[1] = (unsigned char)(value >> 8);
}

static void Put32(unsigned char *at, DWORD value)
{
    Put16(at, value & 0xFFFF);
    Put16(at + 2, value >> 16);
}

static DWORD Get16(const unsigned char *at)
{
    return (DWORD)at[0] | ((DWORD)at[1] << 8);
}

static DWORD Get32(const unsigned char *at)
{
    return Get16(at) | (Get16(at + 2) << 16);
}

typedef struct ZipItem {
    char     *name;          /* UTF-8, '/' between folders; a heap block (writer) or in the directory (reader) */
    size_t    nameLength;
    DWORD     crc, size, offset;
    DWORD     method, flags, packed;
} ZipItem;

typedef struct ZipWriter {
    HANDLE    file;
    ZipItem  *items;
    int       count, capacity;
    ULONGLONG offset;
    WORD      time, date;     /* MS-DOS's, for every file */
    BOOL      tooBig;
} ZipWriter;

static BOOL ZipWrite(ZipWriter *zip, const void *data, size_t length)
{
    if (zip->offset + length > ZIP_MAX_OFFSET) {
        zip->tooBig = TRUE;
        return FALSE;
    }
    if (!WriteWhole(zip->file, (const char *)data, length)) return FALSE;
    zip->offset += length;
    return TRUE;
}

/* Whether `name` is in the archive already. */
static BOOL ZipHas(const ZipWriter *zip, const char *name, size_t length)
{
    int i;
    for (i = 0; i < zip->count; i++)
        if (zip->items[i].nameLength == length && _strnicmp(zip->items[i].name, name, length) == 0)
            return TRUE;
    return FALSE;
}

/* A file's header, its data then written by the caller: its CRC and size
 * are filled in once it is (ZipEnd). */
static BOOL ZipBegin(ZipWriter *zip, const char *name, size_t length)
{
    unsigned char header[ZIP_LOCAL_BYTES];
    ZipItem *items, *item;
    if (zip->count >= (int)ZIP_MAX_ITEMS || length > 0xFFFF) {
        zip->tooBig = TRUE;
        return FALSE;
    }
    if ((items = (ZipItem *)Grow(zip->items, &zip->capacity, zip->count + 1, sizeof *zip->items)) == NULL) return FALSE;
    zip->items = items;
    item = &zip->items[zip->count];
    ZeroMemory(item, sizeof *item);
    if ((item->name = (char *)HeapAlloc(GetProcessHeap(), 0, length + 1)) == NULL) return FALSE;
    memcpy(item->name, name, length);
    item->name[length] = 0;
    item->nameLength = length;
    item->offset = (DWORD)zip->offset;
    zip->count++;
    ZeroMemory(header, sizeof header);
    Put32(header, ZIP_LOCAL_SIGNATURE);
    Put16(header + 4, ZIP_VERSION);
    Put16(header + 6, ZIP_UTF8_NAMES);
    Put16(header + 8, ZIP_STORED);
    Put16(header + 10, zip->time);
    Put16(header + 12, zip->date);
    Put16(header + 26, (DWORD)length);
    return ZipWrite(zip, header, sizeof header) && ZipWrite(zip, name, length);
}

/* The latest file's CRC and size, written into its header. */
static BOOL ZipEnd(ZipWriter *zip, DWORD crc, DWORD size)
{
    ZipItem *item = &zip->items[zip->count - 1];
    unsigned char sizes[12];
    LARGE_INTEGER at, end;
    item->crc = crc;
    item->size = size;
    Put32(sizes, crc);
    Put32(sizes + 4, size);
    Put32(sizes + 8, size);
    at.QuadPart = (LONGLONG)item->offset + 14;
    end.QuadPart = (LONGLONG)zip->offset;
    return SetFilePointerEx(zip->file, at, NULL, FILE_BEGIN) && WriteWhole(zip->file, (const char *)sizes, sizeof sizes) &&
           SetFilePointerEx(zip->file, end, NULL, FILE_BEGIN);
}

static BOOL ZipAddData(ZipWriter *zip, const char *name, const char *data, size_t length)
{
    return length <= ZIP_MAX_OFFSET && ZipBegin(zip, name, strlen(name)) && ZipWrite(zip, data, length) &&
           ZipEnd(zip, Core_Crc32(0, data, length), (DWORD)length);
}

/* The file `path` as `name`, read as it is now: one Claude Code still writes
 * is taken up to where it is. */
static BOOL ZipAddFile(ZipWriter *zip, const char *name, size_t nameLength, const WCHAR *path, SyncReport *report)
{
    WCHAR extended[LONG_PATH_CCH];
    HANDLE in;
    char *buffer;
    DWORD got = 0, crc = 0;
    ULONGLONG size = 0;
    BOOL ok;
    if (ZipHas(zip, name, nameLength)) return TRUE;
    if (!Extended(path, extended)) {
        CannotRead(report, path, ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    in = CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                     FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (in == INVALID_HANDLE_VALUE) {
        CannotRead(report, path, GetLastError());
        return FALSE;
    }
    if ((buffer = (char *)HeapAlloc(GetProcessHeap(), 0, COPY_CHUNK)) == NULL) {
        CloseHandle(in);
        return FALSE;
    }
    ok = ZipBegin(zip, name, nameLength);
    while (ok && ReadFile(in, buffer, COPY_CHUNK, &got, NULL) && got > 0) {
        crc = Core_Crc32(crc, buffer, got);
        size += got;
        ok = ZipWrite(zip, buffer, got);
    }
    if (ok && size > ZIP_MAX_OFFSET) {
        zip->tooBig = TRUE;
        ok = FALSE;
    }
    if (ok) ok = ZipEnd(zip, crc, (DWORD)size);
    Free(buffer);
    CloseHandle(in);
    return ok;
}

/* `path` (a file, or a folder and what it holds) under `root`, as
 * claude/<its path under root>. */
static BOOL ZipAddTree(ZipWriter *zip, const WCHAR *root, const WCHAR *path, SyncReport *report)
{
    WCHAR extended[LONG_PATH_CCH], child[LONG_PATH_CCH];
    char name[ZIP_NAME_MAX];
    WIN32_FIND_DATAW found;
    DWORD attributes = 0;
    size_t rootLength = wcslen(root), prefix = sizeof ARCHIVE_FILES - 1, i;
    HANDLE find;
    int bytes;
    if (!Core_PathUnder(path, root) || Core_PathEquals(path, root) || !Extended(path, extended) ||
        Util_QueryPath(extended, &attributes) != PATH_PRESENT)
        return TRUE;   /* gone meanwhile */
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return TRUE;   /* a link: not followed */
        if ((find = Util_FindFiles(path, L"*", &found, FALSE)) == INVALID_HANDLE_VALUE) return TRUE;
        do {
            if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) continue;
            if (FAILED(StringCchPrintfW(child, ARRAYSIZE(child), L"%s\\%s", path, found.cFileName)) || !ZipAddTree(zip, root, child, report)) {
                FindClose(find);
                return FALSE;
            }
        } while (FindNextFileW(find, &found));
        FindClose(find);
        return TRUE;
    }
    memcpy(name, ARCHIVE_FILES, prefix);
    bytes = WideCharToMultiByte(CP_UTF8, 0, path + rootLength + 1, -1, name + prefix, (int)(sizeof name - prefix), NULL, NULL);
    if (bytes <= 1) {
        CannotRead(report, path, ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    for (i = prefix; name[i]; i++)
        if (name[i] == '\\') name[i] = '/';
    return ZipAddFile(zip, name, prefix + (size_t)bytes - 1, path, report);
}

static BOOL ZipFinish(ZipWriter *zip)
{
    unsigned char header[ZIP_CENTRAL_BYTES], end[ZIP_END_BYTES];
    ULONGLONG start = zip->offset;
    int i;
    for (i = 0; i < zip->count; i++) {
        const ZipItem *item = &zip->items[i];
        ZeroMemory(header, sizeof header);
        Put32(header, ZIP_CENTRAL_SIGNATURE);
        Put16(header + 4, ZIP_VERSION);
        Put16(header + 6, ZIP_VERSION);
        Put16(header + 8, ZIP_UTF8_NAMES);
        Put16(header + 10, ZIP_STORED);
        Put16(header + 12, zip->time);
        Put16(header + 14, zip->date);
        Put32(header + 16, item->crc);
        Put32(header + 20, item->size);
        Put32(header + 24, item->size);
        Put16(header + 28, (DWORD)item->nameLength);
        Put32(header + 42, item->offset);
        if (!ZipWrite(zip, header, sizeof header) || !ZipWrite(zip, item->name, item->nameLength)) return FALSE;
    }
    ZeroMemory(end, sizeof end);
    Put32(end, ZIP_END_SIGNATURE);
    Put16(end + 8, (DWORD)zip->count);
    Put16(end + 10, (DWORD)zip->count);
    Put32(end + 12, (DWORD)(zip->offset - start));
    Put32(end + 16, (DWORD)start);
    return ZipWrite(zip, end, sizeof end) && FlushFileBuffers(zip->file);
}

static void FreeZipWriter(ZipWriter *zip)
{
    int i;
    for (i = 0; i < zip->count; i++) Free(zip->items[i].name);
    Free(zip->items);
    if (zip->file != INVALID_HANDLE_VALUE && zip->file) CloseHandle(zip->file);
    ZeroMemory(zip, sizeof *zip);
}

/* Claude Code's folder, where an archive's claude/ goes. */
static BOOL ClaudeCodeRoot(WCHAR *out, size_t cch)
{
    WCHAR *slash;
    if (!SessionStore_ProjectsDir(out, cch) || (slash = wcsrchr(out, L'\\')) == NULL) return FALSE;
    *slash = 0;
    return TRUE;
}

BOOL SessionSync_Export(const SessionSet *set, int profile, const int *rows, int rowCount, const WCHAR *archive, int *exported,
                        WCHAR *error, size_t errorCch)
{
    WCHAR target[LONG_PATH_CCH], temporary[LONG_PATH_CCH], root[LONG_PATH_CCH], listError[LONG_PATH_CCH];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    char name[64], manifest[256];
    SyncReport report;
    ZipWriter zip;
    SYSTEMTIME now;
    FILETIME local;
    int i, j, pathCount = 0;
    BOOL ok = TRUE;
    *exported = 0;
    error[0] = 0;
    ZeroMemory(&report, sizeof report);
    ZeroMemory(&zip, sizeof zip);
    if (!Extended(archive, target) || FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s" TEMPORARY_SUFFIX, target)) ||
        !ClaudeCodeRoot(root, ARRAYSIZE(root))) {
        StringCchCopyW(error, errorCch, TR(L"The path is too long."));
        return FALSE;
    }
    zip.file = CreateFileW(temporary, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (zip.file == INVALID_HANDLE_VALUE) {
        StringCchPrintfW(error, errorCch, TR(L"%s could not be written (error %lu)."), archive, GetLastError());
        return FALSE;
    }
    GetLocalTime(&now);
    if (SystemTimeToFileTime(&now, &local)) FileTimeToDosDateTime(&local, &zip.date, &zip.time);
    for (i = 0; i < rowCount && ok; i++) {
        const SessionRow *row;
        DWORD length = 0;
        char *entry;
        int source;
        if (rows[i] < 0 || rows[i] >= set->rowCount) continue;
        row = &set->rows[rows[i]];
        if ((source = SourceOf(set, row, profile, (DWORD)-1)) < 0) continue;
        if ((entry = Util_ReadFile(set->entries[row->entry[source]].file, SESSION_ENTRY_MAX_BYTES, FALSE, &length)) == NULL) {
            CannotRead(&report, set->entries[row->entry[source]].file, GetLastError());
            ok = FALSE;
            break;
        }
        StringCchPrintfA(name, sizeof name, ARCHIVE_ENTRIES "%d.json", *exported + 1);
        ok = ZipAddData(&zip, name, entry, length);
        Free(entry);
        if (ok && !SessionEdit_ListConversation(set, rows[i], &paths, &pathCount, listError, ARRAYSIZE(listError))) {
            StringCchCopyW(report.error, ARRAYSIZE(report.error), listError);
            ok = FALSE;
        }
        for (j = 0; ok && j < pathCount; j++) ok = ZipAddTree(&zip, root, paths[j], &report);
        Free(paths);
        paths = NULL;
        if (ok) (*exported)++;
    }
    if (ok) {
        StringCchPrintfA(manifest, sizeof manifest, "{\"format\":\"" ARCHIVE_FORMAT "\",\"version\":%d,\"sessions\":%d,\"exported\":%I64u}",
                         ARCHIVE_VERSION, *exported, NowMs());
        ok = ZipAddData(&zip, ARCHIVE_MANIFEST, manifest, strlen(manifest)) && ZipFinish(&zip);
    }
    if (!ok && !report.error[0]) {
        if (zip.tooBig) StringCchCopyW(report.error, ARRAYSIZE(report.error), TR(L"The archive is too large."));
        else StringCchPrintfW(report.error, ARRAYSIZE(report.error), TR(L"%s could not be written (error %lu)."), archive, GetLastError());
    }
    CloseHandle(zip.file);
    zip.file = INVALID_HANDLE_VALUE;
    if (ok && !MoveFileExW(temporary, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        StringCchPrintfW(report.error, ARRAYSIZE(report.error), TR(L"%s could not be written (error %lu)."), archive, GetLastError());
        ok = FALSE;
    }
    if (!ok) DeleteFileW(temporary);
    FreeZipWriter(&zip);
    StringCchCopyW(error, errorCch, report.error);
    Util_Log(L"%d session(s) exported to %s%s", *exported, archive, ok ? L"" : L" FAILED");
    return ok;
}

typedef struct ZipReader {
    HANDLE         file;
    ULONGLONG      size;
    unsigned char *directory;
    ZipItem       *items;
    int            count;
} ZipReader;

static BOOL ReadAt(HANDLE file, ULONGLONG offset, void *buffer, DWORD length)
{
    LARGE_INTEGER at;
    DWORD got = 0, n;
    at.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, at, NULL, FILE_BEGIN)) return FALSE;
    while (got < length && ReadFile(file, (char *)buffer + got, length - got, &n, NULL) && n > 0) got += n;
    return got == length;
}

/* The archive's directory read, each file's place in it checked. */
static BOOL ZipOpen(ZipReader *zip, const WCHAR *path)
{
    WCHAR extended[LONG_PATH_CCH];
    LARGE_INTEGER size;
    unsigned char *tail = NULL;
    DWORD tailLength, i, count, directorySize, directoryOffset, at;
    BOOL ok = FALSE;
    ZeroMemory(zip, sizeof *zip);
    if (!Extended(path, extended)) return FALSE;
    zip->file = CreateFileW(extended, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (zip->file == INVALID_HANDLE_VALUE || !GetFileSizeEx(zip->file, &size)) return FALSE;
    zip->size = (ULONGLONG)size.QuadPart;
    if (zip->size < ZIP_END_BYTES || zip->size > ZIP_MAX_OFFSET + 1) return FALSE;
    tailLength = (DWORD)min(zip->size, (ULONGLONG)ZIP_END_BYTES + ZIP_COMMENT_MAX);
    if ((tail = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, tailLength)) == NULL ||
        !ReadAt(zip->file, zip->size - tailLength, tail, tailLength))
        goto done;
    for (i = tailLength - ZIP_END_BYTES + 1; i-- > 0;)
        if (Get32(tail + i) == ZIP_END_SIGNATURE && i + ZIP_END_BYTES + Get16(tail + i + 20) == tailLength) break;
    if (i == (DWORD)-1) goto done;
    count = Get16(tail + i + 10);
    directorySize = Get32(tail + i + 12);
    directoryOffset = Get32(tail + i + 16);
    if (Get16(tail + i + 8) != count || directorySize > ZIP_DIRECTORY_MAX ||
        (ULONGLONG)directoryOffset + directorySize > zip->size - tailLength + i)
        goto done;
    if ((zip->directory = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, (size_t)directorySize + 1)) == NULL ||
        (zip->items = (ZipItem *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (size_t)max(count, 1) * sizeof *zip->items)) == NULL ||
        !ReadAt(zip->file, directoryOffset, zip->directory, directorySize))
        goto done;
    for (at = 0; (DWORD)zip->count < count; zip->count++) {
        ZipItem *item = &zip->items[zip->count];
        DWORD nameLength, extra, comment;
        if (at + ZIP_CENTRAL_BYTES > directorySize || Get32(zip->directory + at) != ZIP_CENTRAL_SIGNATURE) goto done;
        nameLength = Get16(zip->directory + at + 28);
        extra = Get16(zip->directory + at + 30);
        comment = Get16(zip->directory + at + 32);
        if (at + ZIP_CENTRAL_BYTES + nameLength + extra + comment > directorySize) goto done;
        item->flags = Get16(zip->directory + at + 8);
        item->method = Get16(zip->directory + at + 10);
        item->crc = Get32(zip->directory + at + 16);
        item->packed = Get32(zip->directory + at + 20);
        item->size = Get32(zip->directory + at + 24);
        item->offset = Get32(zip->directory + at + 42);
        item->name = (char *)zip->directory + at + ZIP_CENTRAL_BYTES;
        item->nameLength = nameLength;
        if ((ULONGLONG)item->offset + ZIP_LOCAL_BYTES + item->packed > directoryOffset) goto done;
        at += ZIP_CENTRAL_BYTES + nameLength + extra + comment;
    }
    ok = TRUE;
done:
    Free(tail);
    return ok;
}

static void ZipClose(ZipReader *zip)
{
    if (zip->file && zip->file != INVALID_HANDLE_VALUE) CloseHandle(zip->file);
    Free(zip->directory);
    Free(zip->items);
    ZeroMemory(zip, sizeof *zip);
}

static BOOL ItemNamed(const ZipItem *item, const char *prefix)
{
    size_t length = strlen(prefix);
    return item->nameLength >= length && memcmp(item->name, prefix, length) == 0;
}

/* Where a stored, unencrypted file's data starts; 0 for one this reader cannot take. */
static ULONGLONG DataOffset(const ZipReader *zip, const ZipItem *item)
{
    unsigned char header[ZIP_LOCAL_BYTES];
    ULONGLONG data;
    if (item->method != ZIP_STORED || (item->flags & ZIP_ENCRYPTED) || item->packed != item->size ||
        !ReadAt(zip->file, item->offset, header, sizeof header) || Get32(header) != ZIP_LOCAL_SIGNATURE)
        return 0;
    data = (ULONGLONG)item->offset + ZIP_LOCAL_BYTES + Get16(header + 26) + Get16(header + 28);
    return data + item->size <= zip->size ? data : 0;
}

/* A small file of the archive read whole and checked: a heap block, NULL when it cannot be. */
static char *ZipReadItem(const ZipReader *zip, const ZipItem *item, DWORD maxBytes)
{
    ULONGLONG data = DataOffset(zip, item);
    char *buffer;
    if (!data || item->size > maxBytes || (buffer = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)item->size + 1)) == NULL) return NULL;
    if (!ReadAt(zip->file, data, buffer, item->size) || Core_Crc32(0, buffer, item->size) != item->crc) {
        Free(buffer);
        return NULL;
    }
    buffer[item->size] = 0;
    return buffer;
}

/* A file of the archive written to `path` (not there yet), checked before
 * it takes its name. */
static BOOL ZipExtract(const ZipReader *zip, const ZipItem *item, const WCHAR *path, SyncReport *report)
{
    WCHAR target[LONG_PATH_CCH], temporary[LONG_PATH_CCH];
    ULONGLONG data = DataOffset(zip, item);
    HANDLE out;
    char *buffer;
    DWORD left = item->size, crc = 0;
    BOOL ok;
    if (!data) {
        StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The archive is damaged."));
        report->failed++;
        return FALSE;
    }
    if (!Extended(path, target) || FAILED(StringCchPrintfW(temporary, ARRAYSIZE(temporary), L"%s" TEMPORARY_SUFFIX, target))) {
        CannotWrite(report, path, ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    out = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        CannotWrite(report, path, GetLastError());
        return FALSE;
    }
    if ((buffer = (char *)HeapAlloc(GetProcessHeap(), 0, COPY_CHUNK)) == NULL) {
        CloseHandle(out);
        DeleteFileW(temporary);
        return FALSE;
    }
    ok = TRUE;
    while (ok && left > 0) {
        DWORD chunk = min(left, COPY_CHUNK);
        ok = ReadAt(zip->file, data, buffer, chunk) && WriteWhole(out, buffer, chunk);
        crc = Core_Crc32(crc, buffer, chunk);
        data += chunk;
        left -= chunk;
    }
    ok = ok && FlushFileBuffers(out);
    CloseHandle(out);
    Free(buffer);
    if (ok && crc != item->crc) {
        DeleteFileW(temporary);
        StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The archive is damaged."));
        report->failed++;
        return FALSE;
    }
    if (!ok || !MoveFileExW(temporary, target, MOVEFILE_WRITE_THROUGH)) {
        DWORD code = GetLastError();
        DeleteFileW(temporary);
        if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) CannotWrite(report, path, code);
        return FALSE;
    }
    return TRUE;
}

/* The archive's own: a manifest of ours, of a version this one reads. */
static BOOL IsSessionArchive(const ZipReader *zip)
{
    WCHAR format[64];
    char *manifest = NULL;
    BOOL ours = FALSE;
    int i;
    for (i = 0; i < zip->count && !manifest; i++)
        if (zip->items[i].nameLength == sizeof ARCHIVE_MANIFEST - 1 && ItemNamed(&zip->items[i], ARCHIVE_MANIFEST))
            manifest = ZipReadItem(zip, &zip->items[i], SESSION_ENTRY_MAX_BYTES);
    if (manifest) {
        size_t length = strlen(manifest);
        ours = MemberText(manifest, length, "format", format, ARRAYSIZE(format)) && wcscmp(format, L"" ARCHIVE_FORMAT) == 0 &&
               MemberNumber(manifest, length, "version") <= ARCHIVE_VERSION;
        Free(manifest);
    }
    return ours;
}

BOOL SessionSync_IsArchive(const WCHAR *archive)
{
    ZipReader zip;
    BOOL ours = ZipOpen(&zip, archive) && IsSessionArchive(&zip);
    ZipClose(&zip);
    return ours;
}

BOOL SessionSync_Import(const SessionSet *set, const WCHAR *archive, DWORD targets, int *sessions, SyncReport *report)
{
    WCHAR root[LONG_PATH_CCH], path[LONG_PATH_CCH], relative[LONG_PATH_CCH];
    ZipReader zip;
    Outbox box;
    DWORD takers;
    int i, t, files = 0;
    *sessions = 0;
    ZeroMemory(&box, sizeof box);
    targets = ValidProfiles(set, targets);
    takers = targets & SessionSync_Takers(set);
    report->unavailable |= targets & ~takers;
    if (!ClaudeCodeRoot(root, ARRAYSIZE(root))) {
        StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The path is too long."));
        return FALSE;
    }
    if (!ZipOpen(&zip, archive) || !IsSessionArchive(&zip)) {
        StringCchCopyW(report->error, ARRAYSIZE(report->error), zip.file && zip.file != INVALID_HANDLE_VALUE
                       ? TR(L"This file is not a session archive of " APP_NAME L".") : L"");
        if (!report->error[0]) StringCchPrintfW(report->error, ARRAYSIZE(report->error), TR(L"%s could not be read (error %lu)."), archive,
                                                GetLastError());
        ZipClose(&zip);
        return FALSE;
    }
    /* The conversations first: an entry is sent only once they are there. */
    for (i = 0; i < zip.count; i++) {
        const ZipItem *item = &zip.items[i];
        size_t prefix = sizeof ARCHIVE_FILES - 1, c;
        int wide;
        if (!ItemNamed(item, ARCHIVE_FILES) || item->nameLength == prefix || item->name[item->nameLength - 1] == '/') continue;
        if (!Core_ArchiveNameSafe(item->name + prefix, item->nameLength - prefix) ||
            (wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, item->name + prefix, (int)(item->nameLength - prefix), relative,
                                        ARRAYSIZE(relative) - 1)) <= 0) {
            StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The archive is damaged."));
            report->failed++;
            continue;
        }
        relative[wide] = 0;
        for (c = 0; relative[c]; c++)
            if (relative[c] == L'/') relative[c] = L'\\';
        if (!Core_ConversationFileName(relative)) {
            Util_Log(L"session import: %s left out, not part of a conversation", relative);
            continue;
        }
        if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", root, relative))) {
            CannotWrite(report, relative, ERROR_FILENAME_EXCED_RANGE);
            continue;
        }
        if (Present(path)) continue;   /* a conversation here is never replaced */
        if (!MakeParents(root, path)) {
            CannotWrite(report, path, GetLastError());
            continue;
        }
        if (ZipExtract(&zip, item, path, report)) files++;
    }
    for (i = 0; i < zip.count; i++) {
        const ZipItem *item = &zip.items[i];
        WCHAR key[SESSION_ID_CCH], ownId[SESSION_ID_CCH];
        char *entry;
        size_t length;
        if (!ItemNamed(item, ARCHIVE_ENTRIES) || item->name[item->nameLength - 1] == '/') continue;
        if ((entry = Keep(&box, ZipReadItem(&zip, item, SESSION_ENTRY_MAX_BYTES))) == NULL) {
            StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The archive is damaged."));
            report->failed++;
            continue;
        }
        length = item->size;
        if (Core_SessionEntryKind(entry, length) != ENTRY_LOCAL || !MemberText(entry, length, "sessionId", ownId, ARRAYSIZE(ownId)) ||
            !IsEntryId(ownId)) {
            report->failed++;
            continue;
        }
        if (!MemberText(entry, length, "cliSessionId", key, ARRAYSIZE(key)) || !key[0]) StringCchCopyW(key, ARRAYSIZE(key), ownId);
        (*sessions)++;
        for (t = 0; t < set->profiles.count; t++) {
            int there = (takers & (1u << t)) ? SessionStore_FindEntry(set, t, key, NULL) : -1;
            SyncOp op;
            if (!(takers & (1u << t))) continue;
            if (there >= 0 && set->entries[there].pendingRemove) {
                report->skipped++;
                continue;
            }
            MakeOp(&op, SYNC_PUT, SYNC_UNDELETE, key, MemberNumber(entry, length, "lastActivityAt"),
                   there >= 0 ? set->entries[there].lastActivity : 0);
            if (!AddSent(&box.sent[t], &op, entry, length)) {
                ZipClose(&zip);
                FreeOutbox(&box);
                return OutOfMemory(report);
            }
        }
    }
    ZipClose(&zip);
    Send(&box, set, report);
    FreeOutbox(&box);
    Util_Log(L"%d session(s) imported from %s, %d conversation file(s) added", *sessions, archive, files);
    return TRUE;
}
