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
#include "presets.h"
#include "vaulttools.h"
#include <objbase.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define PLAN_PREFIX           L"pending-sync-"
#define PLAN_MAX_BYTES        (16u * 1024u * 1024u)
#define PLAN_LINE_CCH         (SESSION_ID_CCH + SYNC_CONTENT_CCH + 96)   /* the kind, three numbers and the tabs */
#define CONTENT_MAX_BYTES     (16u * 1024u * 1024u)   /* an entry, or a list of archived sessions */
#define TOMBSTONE_PREFIX      L"deleted_"
#define LOCAL_PREFIX          L"local_"
#define ARCHIVED_INDEX        L"archived-sessions.idx"   /* beside the entries: Claude's list of the archived ones */
#define BACKUPS_DIR           L"backups"
#define BACKUPS_KEPT          20
#define BACKUP_COPIES_MAX     100     /* name~2 ... name~100 when a name is taken in a backup */
#define TIME_TEXT_BYTES       sizeof "18446744073709551615"
#define ID_JSON_BYTES         (SESSION_ID_CCH * 6 + 3)   /* an id as a JSON string, every character escaped at worst */
#define PATH_JSON_BYTES       (MAX_PATH * 6 + 3)

#define ARCHIVE_FORMAT        "claude-desktop-profiles-manager-sessions"
#define ARCHIVE_VERSION       1
#define ARCHIVE_MANIFEST      "manifest.json"
#define ARCHIVE_ENTRIES       "entries/"
#define ARCHIVE_FILES         "claude/"
#define ARCHIVE_PARTS         "conversations/"   /* conversations/<n>.json: the files of entries/<n>.json's conversation */
#define ARCHIVE_NAME_MAX      (LONG_PATH_CCH * 3)   /* claude/<a path under Claude Code's folder>, in UTF-8 */

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

/* `data` (a heap block) owned by the outbox from now on; NULL, freed, when it cannot be. */
static char *Keep(Outbox *box, char *data)
{
    char **grown;
    if (!data) return NULL;
    if ((grown = (char **)Util_Grow(box->owned, &box->ownedCapacity, box->ownedCount + 1, sizeof *box->owned)) == NULL) {
        Util_Free(data);
        return NULL;
    }
    box->owned = grown;
    box->owned[box->ownedCount++] = data;
    return data;
}

static BOOL AddSent(Sent *sent, const SyncOp *op, const char *content, size_t length)
{
    int capacity = sent->capacity;
    SyncOp *ops = (SyncOp *)Util_Grow(sent->ops, &capacity, sent->count + 1, sizeof *ops);
    const char **contents;
    size_t *lengths;
    if (!ops) return FALSE;
    sent->ops = ops;
    capacity = sent->capacity;
    if ((contents = (const char **)Util_Grow((void *)sent->contents, &capacity, sent->count + 1, sizeof *contents)) == NULL) return FALSE;
    sent->contents = contents;
    capacity = sent->capacity;
    if ((lengths = (size_t *)Util_Grow(sent->lengths, &capacity, sent->count + 1, sizeof *lengths)) == NULL) return FALSE;
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
    Util_Free(sent->ops);
    Util_Free((void *)sent->contents);
    Util_Free(sent->lengths);
    ZeroMemory(sent, sizeof *sent);
}

static void FreeOutbox(Outbox *box)
{
    int i;
    for (i = 0; i < MAX_PROFILES; i++) FreeSent(&box->sent[i]);
    for (i = 0; i < box->ownedCount; i++) Util_Free(box->owned[i]);
    Util_Free(box->owned);
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

/* `path` holds `data`, written beside it and then put in its place: a file
 * of ours (`profile` NULL) replaces the one there; an entry of `profile`'s
 * (or Claude's mark beside them) is written only while that profile is
 * closed, replacing the one there only with `replace`, else only where there
 * is none. GetLastError tells why it failed; ERROR_BUSY: its Claude runs. */
static BOOL WriteFileAt(const WCHAR *path, const char *data, size_t length, const Profile *profile, BOOL replace)
{
    WCHAR temporary[LONG_PATH_CCH];
    if (profile && SessionLink_Busy(profile)) {
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    if (!Util_WriteTemporary(path, data, length, temporary, ARRAYSIZE(temporary))) return FALSE;
    if (profile && SessionLink_Busy(profile)) {
        DeleteFileW(temporary);
        SetLastError(ERROR_BUSY);
        return FALSE;
    }
    return Util_PutInPlace(temporary, path, !profile ? UTIL_PUT_REPLACE : replace ? UTIL_PUT_OVER : UTIL_PUT_NEW);
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
        Util_Free(raw);
        return -1;
    }
    MultiByteToWideChar(CP_UTF8, 0, raw, (int)length, text, wide);
    text[wide] = 0;
    Util_Free(raw);
    for (line = text; line; line = next) {
        SyncOp op, *grown;
        size_t lineLength;
        next = wcschr(line, L'\n');
        if (next) *next++ = 0;
        lineLength = wcslen(line);
        if (lineLength && line[lineLength - 1] == L'\r') line[--lineLength] = 0;
        if (!lineLength || !Core_SyncOpParse(line, &op)) continue;
        if ((grown = (SyncOp *)Util_Grow(plan->ops, &plan->capacity, plan->count + 1, sizeof *plan->ops)) == NULL) {
            Util_Free(text);
            Util_Free(plan->ops);
            ZeroMemory(plan, sizeof *plan);
            return -1;
        }
        plan->ops = grown;
        plan->ops[plan->count++] = op;
    }
    Util_Free(text);
    return plan->count;
}

/* The content files of the plan's folder `dir` that no change of `ops`
 * names go, `ops` being the plan just written: a line LoadPlan could not
 * read is dropped with its file. Those of `replaced` (changes a newer one
 * replaced) stay for the caller, which deletes them once the plan holds. */
static void SweepContents(const WCHAR *dir, const SyncOp *ops, int count, const SyncOp *replaced, int replacedCount)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW found;
    HANDLE find;
    int i;
    if (FAILED(StringCchPrintfW(pattern, ARRAYSIZE(pattern), L"%s\\*.json", dir)) ||
        (find = FindFirstFileW(pattern, &found)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        for (i = 0; i < count && !Core_EqualsI(ops[i].content, found.cFileName); i++) {}
        if (i < count) continue;
        for (i = 0; i < replacedCount && !Core_EqualsI(replaced[i].content, found.cFileName); i++) {}
        if (i == replacedCount && SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, found.cFileName))) {
            if (DeleteFileW(path)) Util_Log(L"sessions sent: %s went, no change named it", path);
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
}

/* The plan written again as `ops` holds it, under the profile's edit lock;
 * none left: its file and folder go. The files of `replaced` are the
 * caller's to delete (SweepContents). */
static BOOL SavePlan(const Profile *p, const SyncOp *ops, int count, const SyncOp *replaced, int replacedCount)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH], line[PLAN_LINE_CCH];
    char *out;
    size_t used = 0, capacity;
    int i;
    BOOL ok;
    if (!SessionSync_PlanPath(p, path, ARRAYSIZE(path)) || !PlanDir(p, dir, ARRAYSIZE(dir))) return FALSE;
    if (count == 0) {
        ok = DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
        if (ok) SweepContents(dir, ops, 0, replaced, replacedCount);
        RemoveDirectoryW(dir);   /* when it is empty */
        return ok;
    }
    capacity = (size_t)count * ARRAYSIZE(line) * 3;
    if ((out = (char *)HeapAlloc(GetProcessHeap(), 0, capacity)) == NULL) return FALSE;
    for (i = 0; i < count; i++) {
        int bytes;
        if (!Core_SyncOpFormat(&ops[i], line, ARRAYSIZE(line)) || FAILED(StringCchCatW(line, ARRAYSIZE(line), L"\n")) ||
            (bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, out + used, (int)min(capacity - used, INT_MAX), NULL, NULL)) <= 0) {
            Util_Free(out);
            return FALSE;
        }
        used += (size_t)bytes - 1;
    }
    ok = WriteFileAt(path, out, used, NULL, TRUE);
    if (!ok) Util_Log(L"could not write %s (error %lu)", path, GetLastError());
    else SweepContents(dir, ops, count, replaced, replacedCount);   /* once the plan names none of them: a failed write keeps the old plan whole */
    Util_Free(out);
    return ok;
}

int SessionSync_PendingCount(const Profile *p)
{
    Plan plan;
    int count = LoadPlan(p, &plan);
    Util_Free(plan.ops);
    return max(count, 0);
}

static void DeleteContent(const WCHAR *dir, const SyncOp *op);

/* The changes waiting for `p` that would write or remove the session `key`
 * taken out of its plan: a session deleted everywhere must not come back
 * when `p` closes. FALSE when the plan cannot be read or written. */
BOOL SessionSync_Forget(const Profile *p, const WCHAR *key)
{
    WCHAR staging[MAX_PATH];
    HANDLE lock = NULL, syncLock = NULL;
    Plan plan;
    int n, i, kept = 0;
    BOOL ok = TRUE;
    if (!PlanDir(p, staging, ARRAYSIZE(staging)) || (syncLock = Util_SyncLock()) == NULL) return FALSE;
    if ((lock = SessionEdit_Lock(p)) == NULL) {
        Util_SyncUnlock(syncLock);
        return FALSE;
    }
    if ((n = LoadPlan(p, &plan)) > 0) {
        for (i = 0; i < plan.count; i++) {
            if ((plan.ops[i].kind == SYNC_PUT || plan.ops[i].kind == SYNC_REMOVE) && Core_EqualsI(plan.ops[i].key, key)) {
                DeleteContent(staging, &plan.ops[i]);
                continue;
            }
            plan.ops[kept++] = plan.ops[i];
        }
        if (kept < plan.count) {
            ok = SavePlan(p, plan.ops, kept, NULL, 0);
            Util_Log(L"session %s: %d change(s) waiting for %s dropped", key, plan.count - kept, p->folder);
        }
    }
    SessionEdit_Unlock(lock);
    Util_SyncUnlock(syncLock);
    Util_Free(plan.ops);
    return ok && n >= 0;
}

int SessionSync_Queued(const Profile *p, SyncOp **ops)
{
    Plan plan;
    int count = LoadPlan(p, &plan);
    *ops = count > 0 ? plan.ops : NULL;
    if (count <= 0) Util_Free(plan.ops);
    return count;
}


WCHAR *SessionSync_PendingText(const ProfileList *profiles)
{
    enum { TEXT_CCH = 65536 };
    WCHAR *text = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, TEXT_CCH * sizeof(WCHAR));
    PendingEdit *edits = (PendingEdit *)HeapAlloc(GetProcessHeap(), 0, SESSION_PENDING_MAX * sizeof *edits);
    WCHAR line[SESSION_TITLE_CCH + LABEL_CCH + 512], dir[MAX_PATH], path[LONG_PATH_CCH];
    int p, i, j;
    BOOL full = FALSE;
    if (!text || !edits) { Util_Free(text); Util_Free(edits); return NULL; }
    for (p = 0; p < profiles->count && !full; p++) {
        const Profile *profile = &profiles->items[p];
        SyncOp *ops = NULL;
        int count = SessionSync_Queued(profile, &ops), n = SessionStore_LoadPending(profile, edits, SESSION_PENDING_MAX);
        StringCchPrintfW(line, ARRAYSIZE(line), L"\r\n%s\r\n%s\r\n", profile->name,
                         Claude_IsRunning(profile) ? TR(L"Waiting for this profile to close") : TR(L"Applied on the next session update when the profile is available"));
        if (FAILED(StringCchCatW(text, TEXT_CCH, line))) full = TRUE;
        if (count < 0 || n < 0) StringCchCatW(text, TEXT_CCH, TR(L"A queue could not be read; its changes are unknown.\r\n"));
        if (!count && !n) StringCchCatW(text, TEXT_CCH, TR(L"No pending changes.\r\n"));
        for (i = 0; i < count && !full; i++) {
            WCHAR value[SESSION_TITLE_CCH] = L"";
            const WCHAR *action;
            for (j = i + 1; j < count; j++) if (Core_SyncOpReplaces(&ops[i], &ops[j])) break;
            if (j < count) continue;
            switch (ops[i].kind) {
            case SYNC_PUT: action = TR(L"Add or update session"); break;
            case SYNC_REMOVE: action = TR(L"Remove session"); break;
            case SYNC_MARK: action = TR(L"Mark session deleted"); break;
            case SYNC_UNMARK: action = TR(L"Clear deletion mark"); break;
            case SYNC_INDEX: action = TR(L"Replace archived index"); break;
            default: action = TR(L"Apply sidebar and settings"); break;
            }
            if (ops[i].content[0]) {
                DWORD length = 0;
                char *content = NULL;
                if (PlanDir(profile, dir, ARRAYSIZE(dir)) &&
                    SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", dir, ops[i].content)))
                    content = Util_ReadFile(path, SESSION_ENTRY_MAX_BYTES, FALSE, &length);
                if (!content) StringCchCopyW(value, ARRAYSIZE(value), TR(L"Content unavailable"));
                else if (ops[i].kind == SYNC_PUT) {
                    WCHAR title[SESSION_TITLE_CCH];
                    const char *star, *archived;
                    size_t starLength = 0, archivedLength = 0;
                    Core_JsonMemberString(content, length, "title", title, ARRAYSIZE(title));
                    Core_JsonMember(content, length, "isStarred", &star, &starLength);
                    Core_JsonMember(content, length, "isArchived", &archived, &archivedLength);
                    StringCchPrintfW(value, ARRAYSIZE(value), TR(L"Title: %s; starred=%d; archived=%d"), title,
                                     starLength == 4 && memcmp(star, "true", 4) == 0, archivedLength == 4 && memcmp(archived, "true", 4) == 0);
                }
                Util_Free(content);
            }
            StringCchPrintfW(line, ARRAYSIZE(line), L"1. %s | %s | %s\r\n", ops[i].key, action, value);
            if (wcslen(text) + wcslen(line) + 256 >= TEXT_CCH) full = TRUE;
            else StringCchCatW(text, TEXT_CCH, line);
        }
        for (i = 0; i < n && !full; i++) {
            const WCHAR *action;
            for (j = i + 1; j < n; j++) if (Core_PendingReplaces(&edits[i], &edits[j])) break;
            if (j < n) continue;
            switch (edits[i].op) {
            case PENDING_TITLE: case PENDING_TITLE_AUTO: action = TR(L"Set title"); break;
            case PENDING_STAR: action = TR(L"Set star"); break;
            case PENDING_UNREMOTE: action = TR(L"Convert to local session"); break;
            default: action = TR(L"Remove session after other edits"); break;
            }
            StringCchPrintfW(line, ARRAYSIZE(line), L"2. %s | %s | %s\r\n", edits[i].key, action, edits[i].value);
            if (wcslen(text) + wcslen(line) + 256 >= TEXT_CCH) full = TRUE;
            else StringCchCatW(text, TEXT_CCH, line);
        }
        Util_Free(ops);
    }
    if (full) StringCchCatW(text, TEXT_CCH, TR(L"\r\nThe display limit was reached; more changes may be queued."));
    Util_Free(edits);
    return text;
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

/* Each entry this thread sends, told to the manager's undo. */
typedef struct SentHook {
    SyncSentProc sent;
    void        *context;
} SentHook;
static __declspec(thread) SentHook g_onSent;

void SessionSync_OnEachSent(SyncSentProc sent, void *context)
{
    g_onSent.sent = sent;
    g_onSent.context = context;
}

static BOOL PlanKeepsContent(const Plan *plan, const SyncOp *op)
{
    int i;
    for (i = 0; i < plan->count; i++)
        if (Core_EqualsI(plan->ops[i].content, op->content)) return TRUE;
    return FALSE;
}

/* What `sent` holds added to the plan of `p`, each change replacing the
 * queued ones it supersedes (Core_SyncOpReplaces), its entry kept in a file
 * of the plan's folder. */
static BOOL StageSent(const Profile *p, Sent *sent)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH];
    HANDLE lock = NULL;
    Plan plan;
    SyncOp *before = NULL;
    int next = 1, i, j, kept, beforeCount, made = 0;
    DWORD error;
    BOOL ok = FALSE;
    if (!sent->count) return TRUE;
    if (!PlanDir(p, dir, ARRAYSIZE(dir)) || (lock = SessionEdit_Lock(p)) == NULL) return FALSE;
    if (LoadPlan(p, &plan) < 0) {
        Util_Log(L"sessions sent to %s: the plan cannot be read", p->folder);
        SessionEdit_Unlock(lock);
        return FALSE;
    }
    beforeCount = plan.count;
    if (beforeCount) {
        before = (SyncOp *)HeapAlloc(GetProcessHeap(), 0, (size_t)beforeCount * sizeof *before);
        if (!before) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            goto done;
        }
        memcpy(before, plan.ops, (size_t)beforeCount * sizeof *before);
    }
    for (i = 0; i < plan.count; i++) next = max(next, ContentNumber(plan.ops[i].content) + 1);
    if (!Util_EnsureDir(dir)) goto done;
    for (i = 0; i < sent->count; i++) {
        SyncOp *op = &sent->ops[i], *grown;
        for (j = kept = 0; j < plan.count; j++) {
            if (!Core_SyncOpReplaces(&plan.ops[j], op)) plan.ops[kept++] = plan.ops[j];
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
        made = i + 1;
        if ((grown = (SyncOp *)Util_Grow(plan.ops, &plan.capacity, plan.count + 1, sizeof *plan.ops)) == NULL) goto done;
        plan.ops = grown;
        plan.ops[plan.count++] = *op;
    }
    ok = SavePlan(p, plan.ops, plan.count, before, beforeCount);
done:
    error = GetLastError();
    /* The committed plan owns its files. Old content survives every failed
     * replacement; new content is discarded only when no committed plan names it. */
    for (i = 0; ok && i < beforeCount; i++)
        if (!PlanKeepsContent(&plan, &before[i])) DeleteContent(dir, &before[i]);
    for (i = 0; i < made; i++)
        if (!ok || !PlanKeepsContent(&plan, &sent->ops[i])) DeleteContent(dir, &sent->ops[i]);
    SessionEdit_Unlock(lock);
    Util_Free(before);
    Util_Free(plan.ops);
    for (i = 0; ok && g_onSent.sent && i < sent->count; i++)
        if (sent->ops[i].kind == SYNC_PUT || sent->ops[i].kind == SYNC_REMOVE)
            g_onSent.sent(g_onSent.context, p, &sent->ops[i], sent->contents[i], sent->lengths[i]);
    if (!ok) SetLastError(error);
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
        if ((grown = (WCHAR (*)[MAX_PATH])Util_Grow(names, &capacity, count + 1, sizeof *names)) == NULL) break;
        names = grown;
        StringCchCopyW(names[count++], MAX_PATH, found.cFileName);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (count > BACKUPS_KEPT) {
        qsort(names, (size_t)count, sizeof *names, CompareNames);
        for (i = 0; i < count - BACKUPS_KEPT; i++)
            if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", backups, names[i]))) DeleteTree(path);
    }
    Util_Free(names);
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
    Util_Free(out);
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

    if (Core_SessionEntryKind(content, contentLength) != ENTRY_LOCAL) {
        CannotWrite(report, op->key, ERROR_INVALID_DATA);
        return OP_FAILED;
    }
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
        if (!Core_JsonMemberString(content, contentLength, "sessionId", ownId, ARRAYSIZE(ownId)) || !IsEntryId(ownId) ||
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
        else report->added++;   /* told once for the batch (SessionSync_ApplyPending) */
    } else if (GetLastError() == ERROR_BUSY) {
        result = OP_STOPPED;
    } else {
        CannotWrite(report, path, GetLastError());
    }
done:
    Util_Free(data);
    Util_Free(current);
    return result;
}


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

/* The entry's file names another transcript than `key` now: the
 * conversation went on in it with a new one (or it was just replaced). */
static BOOL HoldsAnother(const SessionEntry *entry, const WCHAR *key)
{
    WCHAR cli[SESSION_ID_CCH];
    const char *value;
    size_t valueLength;
    DWORD size = 0;
    char *json = Util_ReadFile(entry->file, CONTENT_MAX_BYTES, FALSE, &size);
    BOOL another = json && Core_JsonMember(json, size, "cliSessionId", &value, &valueLength) &&
                   Core_JsonString(value, valueLength, cli, ARRAYSIZE(cli)) && Core_IsUuid(cli) && !Core_EqualsI(cli, key);
    Util_Free(json);
    return another;
}

/* A session taken away from `p`: its entries to the backup, unless it was
 * used there since, or an entry goes on with another transcript; with
 * SYNC_MARKED, Claude's marks that it was deleted there written too, so that
 * Claude does not take it in again. */
static OpResult ApplyRemove(const Profile *p, const SessionSet *entries, const WCHAR *dir, const SyncOp *op, SyncReport *report)
{
    WCHAR ids[MAX_PROFILES][SESSION_ID_CCH];
    int entry = SessionStore_FindEntry(entries, 0, op->key, NULL), count = 0, i;
    BOOL marked;
    if (entry < 0) {
        /* Gone after a try that failed (it took the entry away, its mark not
         * written): the mark still is. Never for one the profile did not have. */
        if ((op->flags & SYNC_MARKED) && op->tries && !WriteMark(p, dir, op->key, Util_NowMs())) {
            CannotWrite(report, dir, GetLastError());
            return OP_FAILED;
        }
        return OP_SAME;
    }
    if (entries->entries[entry].lastActivity > op->seen) {
        Util_Log(L"session %s in %s: kept, used there since", op->key, p->folder);
        return OP_SKIPPED;
    }
    for (i = entry; i >= 0; i = entries->entries[i].duplicate)
        if (HoldsAnother(&entries->entries[i], op->key)) {
            Util_Log(L"session %s in %s: kept, its entry goes on with another transcript", op->key, p->folder);
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
        ULONGLONG now = Util_NowMs();
        marked = WriteMark(p, dir, op->key, now);
        for (i = 0; i < count; i++) marked = WriteMark(p, dir, ids[i], now) && marked;
        if (!marked) {
            /* Without its mark Claude could take the session in again: tried again. */
            CannotWrite(report, dir, GetLastError());
            return OP_FAILED;
        }
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
        Util_Free(current);
        return OP_SAME;
    }
    Util_Free(current);
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

/* What a profile shows the way it does, kept the same as a sidebar's "layout"
 * of ours: one JSON object, each part by its name. The parts come from three
 * places, each in a table below: Claude's settings, its window's web storage,
 * and two files of its own. */
typedef enum LayoutScope { LAYOUT_ANY, LAYOUT_ORGANIZATION, LAYOUT_ACCOUNT } LayoutScope;

/* Claude's settings, in claude_desktop_config.json under "preferences": the
 * part's name in a layout, the name Claude gives it, whether it is its
 * window's (under epitaxyPrefs), whether Claude keeps it for every account
 * or per account (the account's id after the name), and what it belongs to.
 * The window keeps a copy of its own in its web storage
 * (LSS-persisted.<name>), written alike where there is one. */
typedef struct SettingPart {
    const char *part, *name;
    BOOL        window;
    LayoutScope scope;
    DWORD       item;
} SettingPart;
static const SettingPart kSettingParts[] = {
    { "starred", "starred-local-code-sessions", TRUE, LAYOUT_ANY, SYNC_ITEM_SIDEBAR_PINS },
    { "slice", "dframe-local-slice", TRUE, LAYOUT_ANY, SYNC_ITEM_SIDEBAR_PINS },
    { "starredGroups", "starred-session-groups", TRUE, LAYOUT_ANY, SYNC_ITEM_SIDEBAR_PINS },
    { "order", "code-projects-order.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_SIDEBAR_ORDER },
    { "statusFilter", "code-sessions-status-filter.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_SIDEBAR_FILTERS },             /* the filter's Status */
    { "environments", "code-sessions-selected-environments-v2.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_SIDEBAR_FILTERS },  /* ...its Environment */
    { "showEmpty", "code-sessions-show-empty-projects.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_SIDEBAR_FILTERS },          /* ...Show empty groups */
    { "showPrStatus", "code-sessions-show-pr-status.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_SIDEBAR_FILTERS },           /* ...Show PR status */
    { "projectsFilter", "ccd-sessions-filter", TRUE, LAYOUT_ANY, SYNC_ITEM_SIDEBAR_FILTERS },                        /* the projects chosen */
    { "transcriptLinks", "epitaxy-transcript-links-in-preview", TRUE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_CODE },
    { "autoResume", "autoResumeRateLimitOptIn.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_SETTINGS_CODE },                 /* go on after a usage limit */
    { "folderModes", "epitaxy-folder-permission-mode.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_PERMISSIONS_MODES },       /* each folder's mode */
    { "modeAcks", "epitaxy-perm-mode-acks.", TRUE, LAYOUT_ACCOUNT, SYNC_ITEM_PERMISSIONS_MODES },                 /* ...its warning confirmed */
    { "autoArchive", "ccAutoArchiveOnPrClose", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_ARCHIVE },
    { "coworkWebSearch", "coworkWebSearchEnabled", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_COWORK },
    { "coworkBrowserTools", "coworkBrowserToolsEnabled", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_COWORK },
    { "coworkBrowser", "coworkPreferredBrowser", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_COWORK },
    { "coworkSchedules", "coworkScheduledTasksEnabled", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_COWORK },
    { "codeSchedules", "ccdScheduledTasksEnabled", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_CODE },
    { "remoteExcluded", "remoteControlExcludedFolders", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_REMOTE },
    { "remotePinned", "remoteControlPinnedFolders", FALSE, LAYOUT_ANY, SYNC_ITEM_SETTINGS_REMOTE },
    { "trustedFolders", "localAgentModeTrustedFolders", FALSE, LAYOUT_ANY, SYNC_ITEM_PERMISSIONS_TRUSTED },        /* Cowork's */
};

/* The sidebar's groups and their sections, the "Edit sidebar" choices, the
 * groups folded, how sessions are grouped and sorted, the editor's settings,
 * each session's model, side pane, unread mark and cost: Claude's window
 * keeps them in its web storage (webstore.c). It keeps copies of the groups
 * and sections (dframe-group-scopes, dframe-code-sections) in its settings
 * and in its web storage (LSS-persisted.<the same name>), and writes them from
 * dframe-store each time it starts: written in a copy alone, a change would
 * show nowhere. So these are read from the web storage and written to it and
 * to both copies alike. A value is one of Claude's stores ({"state": ...,
 * "version": ...}: the part is `member` of its state, NULL for the value
 * whole), any JSON value, plain text (a JSON string in a layout), or a family
 * of values named `value` and a session's own id (one object by that id).
 * Each part: its name, the value, its form, the member, whether Claude keeps
 * it per account and organization (its member "<account>/<organization>"),
 * the name of the copies, what it belongs to, and whether it is each
 * session's own (an object by session, made the same member by member). */
typedef enum WebForm { FORM_STORE, FORM_JSON, FORM_TEXT, FORM_FAMILY } WebForm;
typedef struct WebPart {
    const char  *part;
    const WCHAR *value;
    WebForm      form;
    const char  *member;
    LayoutScope  scope;
    const char  *copy;
    DWORD        item;
    BOOL         byKey;
} WebPart;
#define SIDEBAR_STORE   L"dframe-store"
#define UNREAD_STORE    L"epitaxy-unread-v1"   /* the sessions unread (Core_UnreadSet) */
#define PANE_STORE      L"epitaxy.sidePaneStore.v1"
#define COPY_PREFIX     L"LSS-persisted."
/* Claude's window keeps some of its stores per account on its server too
 * (ccd/<name>), and at each start puts the server's over what it holds here,
 * unless SYNC_PENDING<that> names the account it starts on
 * ("<account>/<organization>"): then it sends what it holds here to the
 * server first (and clears it). The account it reconciled last is
 * SYNC_OWNER. Those the sync writes in its web storage: the sidebar, the
 * editor's settings and the projects chosen (whose copies in its settings
 * it writes from there at each start, measured on 3.6556). */
#define SYNC_PENDING    L"ccd-sync-pending:"
#define SYNC_OWNER      L"ccd-sync-owner"
typedef struct SyncedStore {
    const WCHAR *name;
    const char  *setting;   /* the setting part (kSettingParts) that is the store whole, written where Claude made it; NULL: none */
} SyncedStore;
static const SyncedStore kSyncedStores[] = { { SIDEBAR_STORE, NULL }, { L"epitaxy-editor-prefs", NULL }, { L"ccd-sessions-filter", "projectsFilter" } };
/* Which sessions are starred: in its IndexedDB (idb-keyval's database and
 * store), {"state":{"starredIds":[...]},"version":...,"updatedAt":...}, and
 * on the server; the copies in its settings ("starred") are written from
 * there at each start (measured on 3.6556: a profile's eight stars back to
 * its own two). */
#define STARRED_DATABASE L"keyval-store"
#define STARRED_STORE    L"keyval"
#define STARRED_RECORD   L"store:pin-state:dframe-starred-code"
#define STARRED_SYNCED   L"ccd/dframe-starred-code"
#define WEB_NAME_CCH    160
static const WebPart kWebParts[] = {
    { "groups", SIDEBAR_STORE, FORM_STORE, "customGroupsByScope", LAYOUT_ORGANIZATION, "dframe-group-scopes", SYNC_ITEM_SIDEBAR_GROUPS, FALSE },
    { "sections", SIDEBAR_STORE, FORM_STORE, "codeSidebarByScope", LAYOUT_ORGANIZATION, "dframe-code-sections", SYNC_ITEM_SIDEBAR_GROUPS, FALSE },
    { "navPins", SIDEBAR_STORE, FORM_STORE, "navPinnedIds", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_PINS, FALSE },   /* Edit sidebar */
    { "folded", SIDEBAR_STORE, FORM_STORE, "collapsedGroups", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_GROUPS, FALSE },
    { "groupBy", SIDEBAR_STORE, FORM_STORE, "groupByByMode", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_FILTERS, FALSE },
    { "sortBy", SIDEBAR_STORE, FORM_STORE, "sortByByMode", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_FILTERS, FALSE },
    { "recentsType", SIDEBAR_STORE, FORM_STORE, "recentsTypeFilter", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_FILTERS, FALSE },   /* Recents' filter */
    { "recentsStatus", SIDEBAR_STORE, FORM_STORE, "recentsStatusFilter", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_FILTERS, FALSE },
    { "routines", SIDEBAR_STORE, FORM_STORE, "routinesSidebarPlacement", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_LAYOUT, FALSE },
    { "sidebarWidth", SIDEBAR_STORE, FORM_STORE, "sidebarWidth", LAYOUT_ANY, NULL, SYNC_ITEM_SIDEBAR_LAYOUT, FALSE },
    { "interfaceFont", SIDEBAR_STORE, FORM_STORE, "interfaceFont", LAYOUT_ANY, NULL, SYNC_ITEM_APPEARANCE_FONTS, FALSE },
    { "systemFont", SIDEBAR_STORE, FORM_STORE, "systemFont", LAYOUT_ANY, NULL, SYNC_ITEM_APPEARANCE_FONTS, FALSE },
    { "editor", L"epitaxy-editor-prefs", FORM_STORE, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_APPEARANCE_EDITOR, FALSE },   /* font, theme, text size */
    { "zoom", L"desktop-frame.mainViewZoomFactor.v1", FORM_JSON, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_APPEARANCE_ZOOM, FALSE },
    { "locale", L"spa:locale", FORM_TEXT, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_LANGUAGE, FALSE },
    { "localeSource", L"spa:locale-source", FORM_TEXT, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_LANGUAGE, FALSE },
    { "defaultModel", L"default-model", FORM_TEXT, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_MODEL, FALSE },
    { "coworkRecents", L"cowork-picker-recents", FORM_JSON, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_SETTINGS_RECENTS, FALSE },
    { "pills", L"code-session-pills", FORM_JSON, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_MODEL, FALSE },   /* each session's model, effort */
    { "unread", UNREAD_STORE, FORM_STORE, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_UNREAD, FALSE },
    { "railSections", L"epitaxy.sessionRailSections", FORM_STORE, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, FALSE },
    { "paneLayout", PANE_STORE, FORM_STORE, "tileLayout", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, FALSE },
    { "paneSizes", PANE_STORE, FORM_STORE, "lastPaneFlexByKind", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, FALSE },
    { "transcriptMode", PANE_STORE, FORM_STORE, "transcriptMode", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, FALSE },
    { "transcriptModeDefault", PANE_STORE, FORM_STORE, "transcriptModeIsDefault", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, FALSE },
    { "filesTree", PANE_STORE, FORM_STORE, "filesTreeShown", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, FALSE },
    { "diffTree", PANE_STORE, FORM_STORE, "diffShowTree", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, FALSE },
    { "paneLayouts", PANE_STORE, FORM_STORE, "tileLayoutBySession", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, TRUE },
    { "paneExpanded", PANE_STORE, FORM_STORE, "expandedTileBySession", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, TRUE },
    { "paneClosed", PANE_STORE, FORM_STORE, "closedPaneSlotsBySession", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, TRUE },
    { "transcriptModes", PANE_STORE, FORM_STORE, "transcriptModeBySession", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, TRUE },
    { "paneBands", PANE_STORE, FORM_STORE, "bandHeightsBySession", LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_PANE, TRUE },
    { "costs", L"epitaxy-session-result:", FORM_FAMILY, NULL, LAYOUT_ANY, NULL, SYNC_ITEM_DETAILS_COST, TRUE },
};

/* Two files of Claude's own: config.json (the app's state: its language, the
 * zoom of the window's controls) and Chromium's Preferences (the page's zoom,
 * spelling's languages), each part the member reached through `keys`. With
 * `own`, the last key is the profile's own (Electron names its zoom by a hash
 * of the profile's folder, Core_ZoomPartitionKey), or the only member there
 * when it has none: a Preferences copied from another folder holds that
 * folder's too, which Claude no longer reads. */
#define CHROMIUM_PREFERENCES L"Preferences"
typedef struct FilePart {
    const char  *part;
    const WCHAR *file;
    const char  *keys[3];
    int          depth;
    BOOL         own;
    DWORD        item;
} FilePart;
static const FilePart kFileParts[] = {
    { "appLocale", CLAUDE_APP_SETTINGS, { "locale", NULL, NULL }, 1, FALSE, SYNC_ITEM_LANGUAGE },
    { "controlsZoom", CLAUDE_APP_SETTINGS, { "windowControlsZoomFactor", NULL, NULL }, 1, FALSE, SYNC_ITEM_APPEARANCE_ZOOM },
    { "pageZoom", CHROMIUM_PREFERENCES, { "partition", "per_host_zoom_levels", NULL }, 3, TRUE, SYNC_ITEM_APPEARANCE_ZOOM },
    { "spelling", CHROMIUM_PREFERENCES, { "spellcheck", "dictionaries", NULL }, 2, FALSE, SYNC_ITEM_APPEARANCE_SPELLING },
};

/* Each part as the person sees it, in a conflict (a catalog key). */
static const struct { const char *part; const WCHAR *label; } kPartLabels[] = {
    { "starred", L"Starred sessions" }, { "slice", L"Pinned sessions" }, { "starredGroups", L"Starred groups" },
    { "order", L"Project order" }, { "statusFilter", L"Status filter" }, { "environments", L"Environment filter" },
    { "showEmpty", L"Show empty groups" }, { "showPrStatus", L"Show PR status" }, { "projectsFilter", L"Projects shown" },
    { "transcriptLinks", L"Links in previews" }, { "autoResume", L"Go on after a usage limit" },
    { "folderModes", L"Folders' permission modes" }, { "modeAcks", L"Permission warnings confirmed" },
    { "autoArchive", L"Archive when the PR closes" }, { "coworkWebSearch", L"Cowork web search" },
    { "coworkBrowserTools", L"Cowork browser tools" }, { "coworkBrowser", L"Cowork's browser" },
    { "coworkSchedules", L"Cowork scheduled tasks" }, { "codeSchedules", L"Code scheduled tasks" },
    { "remoteExcluded", L"Remote Control: excluded folders" }, { "remotePinned", L"Remote Control: pinned folders" },
    { "trustedFolders", L"Cowork's trusted folders" }, { "groups", L"Groups" }, { "sections", L"Sidebar sections" },
    { "navPins", L"Edit sidebar" }, { "folded", L"Folded groups" }, { "groupBy", L"Group by" }, { "sortBy", L"Sort by" },
    { "recentsType", L"Recents: type" }, { "recentsStatus", L"Recents: status" }, { "routines", L"Routines' place" },
    { "sidebarWidth", L"Sidebar width" }, { "interfaceFont", L"Interface font" }, { "systemFont", L"System font" },
    { "editor", L"Editor" }, { "zoom", L"Zoom" }, { "locale", L"Language" }, { "localeSource", L"Language source" },
    { "defaultModel", L"Default model" }, { "coworkRecents", L"Cowork's recent folders" },
    { "pills", L"Sessions' model and effort" }, { "unread", L"Unread marks" }, { "railSections", L"Session side bar" },
    { "paneLayout", L"Side pane layout" }, { "paneSizes", L"Side pane sizes" }, { "transcriptMode", L"Transcript view" },
    { "transcriptModeDefault", L"Transcript view by default" }, { "filesTree", L"Files tree" }, { "diffTree", L"Diff tree" },
    { "appLocale", L"App language" }, { "controlsZoom", L"Controls' zoom" }, { "pageZoom", L"Page zoom" },
    { "spelling", L"Spelling languages" }, { "sessionPresets", L"Session presets" },
};

static const WCHAR *PartLabel(const char *part)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(kPartLabels); i++)
        if (strcmp(kPartLabels[i].part, part) == 0) return kPartLabels[i].label;
    return NULL;
}

BOOL SessionSync_LayoutPart(int index, LayoutPart *part)
{
    ZeroMemory(part, sizeof *part);
    if (index < 0) return FALSE;
    if (index < (int)ARRAYSIZE(kSettingParts)) {
        part->name = kSettingParts[index].part;
        part->item = kSettingParts[index].item;
    } else if ((index -= (int)ARRAYSIZE(kSettingParts)) < (int)ARRAYSIZE(kWebParts)) {
        part->name = kWebParts[index].part;
        part->item = kWebParts[index].item;
        part->byKey = kWebParts[index].byKey;
    } else if ((index -= (int)ARRAYSIZE(kWebParts)) < (int)ARRAYSIZE(kFileParts)) {
        part->name = kFileParts[index].part;
        part->item = kFileParts[index].item;
        part->ownOnly = kFileParts[index].own;
    } else if (index == (int)ARRAYSIZE(kFileParts)) {
        part->name = "sessionPresets";
        part->item = SYNC_ITEM_DETAILS_MODEL;
        part->mergeOnly = TRUE;
    } else {
        return FALSE;
    }
    part->label = PartLabel(part->name);
    return TRUE;
}

char *SessionSync_MergePart(const LayoutPart *part, const char *base, size_t baseLength, const char *other, size_t otherLength,
                            size_t *outLength)
{
    *outLength = 0;
    return part->mergeOnly ? Presets_MergeAccount(base, baseLength, other, otherLength, outLength) : NULL;
}

/* What ApplyLayout would make of `value` where the member shows `shown`:
 * nothing over a value of the profile's own it does not have yet (ApplyFiles),
 * and the presets merged into its own (ApplyPresets), which can hold more
 * than `value`. Sent again, either would change nothing. */
BOOL SessionSync_PartHolds(const LayoutPart *part, const char *layout, size_t layoutLength, const char *shown, size_t shownLength,
                           const char *value, size_t valueLength)
{
    const char *absent, *mark;
    size_t absentLength, markLength;
    char *merged;
    size_t mergedLength = 0;
    BOOL holds;
    if (part->ownOnly && !shown) return TRUE;
    /* A part it cannot take (ApplyWeb, ApplyFiles write none there): sent again and again, it would change nothing. */
    if (!shown && layout && Core_JsonMember(layout, layoutLength, LAYOUT_CANNOT_TAKE, &absent, &absentLength) &&
        Core_JsonMember(absent, absentLength, part->name, &mark, &markLength))
        return TRUE;
    if (!shown || !value || !part->mergeOnly) return FALSE;
    if ((merged = Presets_MergeAccount(shown, shownLength, value, valueLength, &mergedLength)) == NULL) return FALSE;
    holds = mergedLength == shownLength && memcmp(merged, shown, shownLength) == 0;
    Util_Free(merged);
    return holds;
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

/* `*json` (a heap block) with the member reached through `keys` set to
 * `length` bytes of `raw`; FALSE, `*json` as it was, when it cannot be. */
static BOOL SetNested(char **json, size_t *used, const char *const *keys, int depth, const char *raw, size_t length)
{
    char *text, *out = NULL;
    size_t outLength = 0;
    if (*json && (text = Util_CopyBytes(raw, length)) != NULL) {
        out = Core_JsonSetNested(*json, *used, keys, depth, text, &outLength);
        Util_Free(text);
    }
    if (!out) return FALSE;
    Util_Free(*json);
    *json = out;
    *used = outLength;
    return TRUE;
}

char *SessionSync_SetMember(char *json, size_t *used, const char *key, const char *raw, size_t length)
{
    const char *keys[1];
    keys[0] = key;
    if (json && SetNested(&json, used, keys, 1, raw, length)) return json;
    Util_Free(json);
    return NULL;
}

static char *EmptyObject(size_t *used)
{
    *used = 2;
    return Util_CopyBytes("{}", 2);
}

/* A file's time of last write, in ms since 1970; 0 when unknown. */
static ULONGLONG WrittenAt(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA attributes;
    return GetFileAttributesExW(path, GetFileExInfoStandard, &attributes) ? Core_FileTimeMs(&attributes.ftLastWriteTime) : 0;
}

/* A session's own id in a value of Claude's ("local_<id>") that the profile
 * lists: an entry of that name in its entries folder `entriesDir`. */
static BOOL ListedSession(const WCHAR *entriesDir, const char *id, size_t length)
{
    WCHAR wide[SESSION_ID_CCH], path[LONG_PATH_CCH];
    size_t prefix = sizeof "local_" - 1, i;
    if (length <= prefix || length >= ARRAYSIZE(wide) || memcmp(id, "local_", prefix) != 0) return FALSE;
    for (i = 0; i < length; i++) {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_')) return FALSE;
        wide[i] = (WCHAR)c;
    }
    wide[length] = 0;
    return SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s.json", entriesDir, wide)) && Present(path);
}

/* An object by session being made: the members of the sessions it lists. */
typedef struct SessionObject {
    const WCHAR *entriesDir;
    char        *json;
    size_t       used;
} SessionObject;

static BOOL AddListed(void *context, const char *key, size_t keyLength, const char *value, size_t valueLength)
{
    SessionObject *object = (SessionObject *)context;
    char name[SESSION_ID_CCH];
    if (!ListedSession(object->entriesDir, key, keyLength) || FAILED(StringCchCopyNA(name, sizeof name, key, keyLength))) return TRUE;
    object->json = SessionSync_SetMember(object->json, &object->used, name, value, valueLength);
    return object->json != NULL;
}

/* A value family's member: "<family><id>" read as the member <id>. */
typedef struct FamilyRead {
    const WebStore *store;
    const WCHAR    *family;
    SessionObject   object;
} FamilyRead;

static void AddFamilyValue(void *context, const WCHAR *name)
{
    FamilyRead *read = (FamilyRead *)context;
    char id[SESSION_ID_CCH];
    size_t prefix = wcslen(read->family), length = wcslen(name), valueLength = 0, i;
    char *value;
    if (!read->object.json || length <= prefix || length - prefix >= sizeof id) return;
    for (i = prefix; i < length; i++) id[i - prefix] = (char)name[i];
    id[length - prefix] = 0;
    if (!ListedSession(read->object.entriesDir, id, length - prefix)) return;
    if ((value = WebStore_Get(read->store, name, &valueLength)) != NULL && Core_JsonIsValue(value, valueLength))
        read->object.json = SessionSync_SetMember(read->object.json, &read->object.used, id, value, valueLength);
    Util_Free(value);
}

/* Part `part` of the web storage value `text` (`length` bytes): its JSON text
 * (a heap block, NULL when it has none). */
static char *WebPartOf(const WebPart *part, const char *text, size_t length, const char *scope, const WCHAR *entriesDir, size_t *outLength)
{
    const char *state, *inner;
    size_t stateLength, innerLength;
    *outLength = 0;
    if (!text) return NULL;
    if (part->form == FORM_JSON) {
        if (!Core_JsonIsValue(text, length)) return NULL;
        *outLength = length;
        return Util_CopyBytes(text, length);
    }
    if (part->form == FORM_TEXT) {
        WCHAR *wide;
        char *quoted = NULL;
        int chars = length ? MultiByteToWideChar(CP_UTF8, 0, text, (int)length, NULL, 0) : 0;
        if ((length && chars <= 0) || (wide = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, ((size_t)chars + 1) * sizeof(WCHAR))) == NULL) return NULL;
        if (chars) MultiByteToWideChar(CP_UTF8, 0, text, (int)length, wide, chars);
        wide[chars] = 0;
        if ((quoted = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)chars * 12 + 3)) != NULL && Core_JsonQuote(wide, quoted, (size_t)chars * 12 + 3)) {
            *outLength = strlen(quoted);
        } else {
            Util_Free(quoted);
            quoted = NULL;
        }
        Util_Free(wide);
        return quoted;
    }
    if (!Core_JsonMember(text, length, "state", &state, &stateLength)) return NULL;
    if (!part->member) {
        *outLength = length;
        return Util_CopyBytes(text, length);
    }
    if (!Core_JsonMember(state, stateLength, part->member, &inner, &innerLength) ||
        (part->scope == LAYOUT_ORGANIZATION && !Core_JsonMember(inner, innerLength, scope, &inner, &innerLength)))
        return NULL;
    if (part->byKey) {
        SessionObject object;
        object.entriesDir = entriesDir;
        object.json = EmptyObject(&object.used);
        if (!object.json || !Core_JsonEachMember(inner, innerLength, AddListed, &object)) {
            Util_Free(object.json);
            return NULL;
        }
        *outLength = object.used;
        return object.json;
    }
    *outLength = innerLength;
    return Util_CopyBytes(inner, innerLength);
}

/* The parts of `layout` (a heap block, freed) read from the web storage of
 * `p`; `written` made the time its newest file was written, when later. */
/* `name` added to `absent`, the parts a profile cannot take (an object of
 * `*absentUsed` bytes, its members those parts' names). */
static void CannotTake(char *absent, size_t *absentUsed, size_t capacity, const char *name)
{
    size_t length = strlen(name);
    if (*absentUsed + length + 8 > capacity) return;
    StringCchPrintfA(absent + *absentUsed - 1, capacity - *absentUsed + 1, "%s\"%s\":1}", *absentUsed > 2 ? "," : "", name);
    *absentUsed += length + 4 + (*absentUsed > 2 ? 1 : 0);
}

static char *ReadWebParts(const Profile *p, const char *scope, const WCHAR *entriesDir, char *layout, size_t *used, ULONGLONG *written,
                          char *absent, size_t *absentUsed, size_t absentCapacity)
{
    WebStore *store = WebStore_Open(p);
    const WCHAR *read = NULL;
    char *text = NULL, *value;
    size_t length = 0, valueLength, i;
    if (!store) {   /* none yet (Claude makes it at its first start): ApplyWeb writes nothing there */
        for (i = 0; i < ARRAYSIZE(kWebParts); i++) CannotTake(absent, absentUsed, absentCapacity, kWebParts[i].part);
        return layout;
    }
    if (written) *written = max(*written, WebStore_Written(store));
    for (i = 0; i < ARRAYSIZE(kWebParts) && layout; i++) {
        if (kWebParts[i].form == FORM_FAMILY) {
            FamilyRead family;
            family.store = store;
            family.family = kWebParts[i].value;
            family.object.entriesDir = entriesDir;
            family.object.json = EmptyObject(&family.object.used);
            WebStore_EachName(store, kWebParts[i].value, AddFamilyValue, &family);
            if (family.object.json && family.object.used > 2)
                layout = SessionSync_SetMember(layout, used, kWebParts[i].part, family.object.json, family.object.used);
            Util_Free(family.object.json);
            continue;
        }
        if (!read || wcscmp(read, kWebParts[i].value) != 0) {
            Util_Free(text);
            text = WebStore_Get(store, kWebParts[i].value, &length);
            read = kWebParts[i].value;
        }
        /* A member of a store Claude has not made here (its side pane, never opened): SetWebValue makes none. */
        if (!text && kWebParts[i].member) CannotTake(absent, absentUsed, absentCapacity, kWebParts[i].part);
        if ((value = WebPartOf(&kWebParts[i], text, length, scope, entriesDir, &valueLength)) != NULL)
            layout = SessionSync_SetMember(layout, used, kWebParts[i].part, value, valueLength);
        Util_Free(value);
    }
    Util_Free(text);
    WebStore_Free(store);
    return layout;
}

/* The only member of an object, when it has one alone. */
typedef struct OnlyMember {
    const char *key, *value;
    size_t      keyLength, valueLength;
    int         count;
} OnlyMember;

static BOOL SeeOnly(void *context, const char *key, size_t keyLength, const char *value, size_t valueLength)
{
    OnlyMember *only = (OnlyMember *)context;
    only->key = key;
    only->keyLength = keyLength;
    only->value = value;
    only->valueLength = valueLength;
    return ++only->count == 1;
}

/* The member of `json` reached through `keys`: with `own`, the last key is
 * the profile's own (`*ownKey`, Core_ZoomPartitionKey of its folder), else
 * the only member of the object before it, whose name goes to `*ownKey`. */
static BOOL FileMember(const char *json, size_t length, const FilePart *part, const char **value, size_t *valueLength, char *ownKey, size_t ownCap)
{
    int level, last = part->own ? part->depth - 1 : part->depth;
    *value = json;
    *valueLength = length;
    for (level = 0; level < last; level++)
        if (!Core_JsonMember(*value, *valueLength, part->keys[level], value, valueLength)) return FALSE;
    if (part->own) {
        OnlyMember only;
        const char *mine;
        size_t mineLength;
        if (ownKey[0] && Core_JsonMember(*value, *valueLength, ownKey, &mine, &mineLength)) {
            *value = mine;
            *valueLength = mineLength;
            return TRUE;
        }
        ZeroMemory(&only, sizeof only);
        if (!Core_JsonEachMember(*value, *valueLength, SeeOnly, &only) || only.count != 1 || FAILED(StringCchCopyNA(ownKey, ownCap, only.key, only.keyLength)))
            return FALSE;
        *value = only.value;
        *valueLength = only.valueLength;
    }
    return TRUE;
}

static char *ReadFileParts(const Profile *p, char *layout, size_t *used, ULONGLONG *written, char *absent, size_t *absentUsed,
                           size_t absentCapacity)
{
    const WCHAR *read = NULL;
    WCHAR path[LONG_PATH_CCH];
    char *text = NULL, own[128];
    const char *value;
    DWORD length = 0;
    size_t valueLength, i;
    for (i = 0; i < ARRAYSIZE(kFileParts) && layout; i++) {
        if (!read || wcscmp(read, kFileParts[i].file) != 0) {
            Util_Free(text);
            text = NULL;
            read = kFileParts[i].file;
            if (SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", p->storageDir, read)))
                text = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &length);
            if (text && written) *written = max(*written, WrittenAt(path));
        }
        if (!text) {   /* a file Claude has not made: ApplyFiles writes none */
            CannotTake(absent, absentUsed, absentCapacity, kFileParts[i].part);
            continue;
        }
        if (!Core_ZoomPartitionKey(p->storageDir, own, sizeof own)) own[0] = 0;
        if (text && FileMember(text, length, &kFileParts[i], &value, &valueLength, own, sizeof own))
            layout = SessionSync_SetMember(layout, used, kFileParts[i].part, value, valueLength);
    }
    Util_Free(text);
    return layout;
}

char *SessionSync_ReadLayout(const Profile *p, const WCHAR *entriesDir, size_t *length, ULONGLONG *written)
{
    WCHAR path[LONG_PATH_CCH];
    char account[SESSION_ID_CCH * 3], scope[SESSION_ID_CCH * 6 + 2], name[128], *config, *layout, absent[1024] = "{}";
    const char *prefs, *epitaxy, *value;
    size_t prefsLength, epitaxyLength, valueLength, used = 0, i, absentUsed = 2;
    DWORD size = 0;
    *length = 0;
    if (written) *written = 0;
    if (!p->storageDir[0] || !LayoutScopeOf(entriesDir, account, sizeof account, scope, sizeof scope) ||
        FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir)) || (layout = EmptyObject(&used)) == NULL)
        return NULL;
    if ((config = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &size)) != NULL) {
        if (written) *written = WrittenAt(path);
        if (Core_JsonMember(config, size, "preferences", &prefs, &prefsLength)) {
            if (!Core_JsonMember(prefs, prefsLength, "epitaxyPrefs", &epitaxy, &epitaxyLength)) epitaxy = NULL;
            for (i = 0; i < ARRAYSIZE(kSettingParts) && layout; i++) {
                const SettingPart *part = &kSettingParts[i];
                if (part->window && !epitaxy) continue;
                if (SUCCEEDED(StringCchPrintfA(name, sizeof name, "%s%s", part->name, part->scope == LAYOUT_ACCOUNT ? account : "")) &&
                    (part->window ? Core_JsonMember(epitaxy, epitaxyLength, name, &value, &valueLength)
                                  : Core_JsonMember(prefs, prefsLength, name, &value, &valueLength)))
                    layout = SessionSync_SetMember(layout, &used, part->part, value, valueLength);
            }
        }
        Util_Free(config);
    }
    if (layout) layout = ReadWebParts(p, scope, entriesDir, layout, &used, written, absent, &absentUsed, sizeof absent);
    if (layout) layout = ReadFileParts(p, layout, &used, written, absent, &absentUsed, sizeof absent);
    if (layout && SUCCEEDED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" SESSION_PRESETS_FILE, p->storageDir))) {
        char *presets = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &size);
        if (presets) {
            size_t presetsLength = 0;
            char *own = Presets_Account(presets, size, account, &presetsLength);
            if (own) {
                layout = SessionSync_SetMember(layout, &used, "sessionPresets", own, presetsLength);
                if (written) *written = max(*written, WrittenAt(path));
            }
            Util_Free(own); Util_Free(presets);
        }
    }
    if (layout && used <= 2) {
        Util_Free(layout);
        layout = NULL;
    }
    if (layout && absentUsed > 2) layout = SessionSync_SetMember(layout, &used, LAYOUT_CANNOT_TAKE, absent, absentUsed);
    if (layout) *length = used;
    return layout;
}

/* `path`, a file of `p`, made `json` (`used` bytes) unless it is that
 * already (`current`), backed up first. */
static OpResult WriteLayoutFile(const Profile *p, const WCHAR *path, const char *current, DWORD currentLength, const char *json, size_t used,
                                const WCHAR *what, SyncReport *report)
{
    if (current && used == currentLength && memcmp(current, json, used) == 0) return OP_SAME;
    if (SessionLink_Busy(p)) return OP_STOPPED;
    if (current && !BackUp(p, path, FALSE, report)) {
        CannotWrite(report, path, GetLastError());
        return OP_FAILED;
    }
    if (WriteFileAt(path, json, used, p, current != NULL)) {
        Util_Log(L"%s in %s: replaced", what, p->folder);
        return OP_MADE;
    }
    if (GetLastError() == ERROR_BUSY) return OP_STOPPED;
    CannotWrite(report, path, GetLastError());
    return OP_FAILED;
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
    char *current, *json;
    BOOL ok = TRUE;
    OpResult result;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" CLAUDE_DESKTOP_SETTINGS, p->storageDir))) {
        CannotWrite(report, p->folder, ERROR_INVALID_DATA);
        return OP_FAILED;
    }
    current = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &currentLength);
    if (current) {
        json = Util_CopyBytes(current, currentLength);
        used = currentLength;
    } else {
        json = EmptyObject(&used);
    }
    for (i = 0; i < ARRAYSIZE(kSettingParts) && json && ok; i++) {
        const char *window[3] = { "preferences", "epitaxyPrefs", name }, *app[2] = { "preferences", name };
        if (Core_JsonMember(content, contentLength, kSettingParts[i].part, &value, &valueLength) &&
            SUCCEEDED(StringCchPrintfA(name, sizeof name, "%s%s", kSettingParts[i].name, kSettingParts[i].scope == LAYOUT_ACCOUNT ? account : "")))
            ok = kSettingParts[i].window ? SetNested(&json, &used, window, 3, value, valueLength) : SetNested(&json, &used, app, 2, value, valueLength);
    }
    for (i = 0; i < ARRAYSIZE(kWebParts) && json && ok; i++) {
        const char *keys[4] = { "preferences", "epitaxyPrefs", kWebParts[i].copy, scope };
        if (kWebParts[i].copy && Core_JsonMember(content, contentLength, kWebParts[i].part, &value, &valueLength))
            ok = SetNested(&json, &used, keys, 4, value, valueLength);
    }
    if (!json || !ok) {
        CannotWrite(report, path, ERROR_INVALID_DATA);
        result = OP_FAILED;
    } else {
        result = WriteLayoutFile(p, path, current, currentLength, json, used, L"the sidebar's settings", report);
    }
    Util_Free(json);
    Util_Free(current);
    return result;
}

/* A member `key` of the object `*json` reached through `keys` (`depth` of
 * them) set to `value`, the objects on the way made where missing. */
static BOOL SetMemberOf(char **json, size_t *used, const char *const *keys, int depth, const char *key, const char *value, size_t valueLength)
{
    const char *path[4];
    int level;
    if (depth > 3) return FALSE;
    for (level = 0; level < depth; level++) path[level] = keys[level];
    path[depth] = key;
    return SetNested(json, used, path, depth + 1, value, valueLength);
}

typedef struct KeyedWrite {
    char       **json;
    size_t      *used;
    const char  *keys[3];
    int          depth;
    BOOL         ok;
} KeyedWrite;

/* A session's member of a part by session: set under its own id, which a
 * profile that does not list that session has none of. */
static BOOL SetKeyed(void *context, const char *key, size_t keyLength, const char *value, size_t valueLength)
{
    KeyedWrite *write = (KeyedWrite *)context;
    char name[SESSION_ID_CCH];
    if (keyLength <= sizeof "local_" - 1 || memcmp(key, "local_", sizeof "local_" - 1) != 0 ||
        FAILED(StringCchCopyNA(name, sizeof name, key, keyLength)))
        return TRUE;
    if (!SetMemberOf(write->json, write->used, write->keys, write->depth, name, value, valueLength)) write->ok = FALSE;
    return TRUE;
}

/* Web storage value `name` of `store` with the parts of `content` it keeps
 * set in it: a value whole made where there is none, a member of one only
 * where Claude made that value; `*changed` when it changed. A member Claude
 * keeps as something else than an object stays as it is. */
static BOOL SetWebValue(WebStore *store, const WCHAR *name, const char *scope, const char *content, size_t contentLength, BOOL *changed)
{
    static const char *const kSliceMembers[] = { "pinnedOrder", "homeProjectsPinnedOrder" };
    const char *value;
    size_t valueLength, length = 0, used, i;
    char *text = WebStore_Get(store, name, &length), *json = text ? Util_CopyBytes(text, length) : NULL;
    BOOL ok = TRUE, set = FALSE;
    used = length;
    for (i = 0; i < ARRAYSIZE(kWebParts); i++) {
        const WebPart *part = &kWebParts[i];
        const char *keys[3] = { "state", part->member, scope };
        if (part->form == FORM_FAMILY || wcscmp(part->value, name) != 0 || !Core_JsonMember(content, contentLength, part->part, &value, &valueLength))
            continue;
        if (part->form == FORM_TEXT) {
            WCHAR *wide = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (valueLength + 1) * sizeof(WCHAR));
            int bytes;
            if (wide && Core_JsonString(value, valueLength, wide, valueLength + 1) &&
                (bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL)) > 0) {
                Util_Free(json);
                if ((json = (char *)HeapAlloc(GetProcessHeap(), 0, (size_t)bytes)) != NULL) {
                    WideCharToMultiByte(CP_UTF8, 0, wide, -1, json, bytes, NULL, NULL);
                    used = (size_t)bytes - 1;
                    set = TRUE;
                }
            }
            Util_Free(wide);
        } else if (part->form == FORM_JSON || !part->member) {
            Util_Free(json);
            json = Util_CopyBytes(value, valueLength);
            used = valueLength;
            set = TRUE;
        } else if (json && part->byKey) {
            KeyedWrite write;
            write.json = &json;
            write.used = &used;
            write.keys[0] = "state";
            write.keys[1] = part->member;
            write.depth = 2;
            write.ok = TRUE;
            if (!Core_JsonEachMember(value, valueLength, SetKeyed, &write) || !write.ok)
                Util_Log(L"web storage %s: its %S kept, not an object there", name, part->part);
        } else if (json && !SetNested(&json, &used, keys, part->scope == LAYOUT_ORGANIZATION ? 3 : 2, value, valueLength)) {
            Util_Log(L"web storage %s: its %S kept, not an object there", name, part->part);
        }
    }
    /* The sidebar's pins: Claude's window keeps them in its store too, and
     * at each start writes its settings' copy (dframe-local-slice) from there,
     * so pins written in the copy alone were gone once it started (measured
     * on 3.6556: a profile's two pins back to its eight 14 s after its start). */
    if (json && wcscmp(name, SIDEBAR_STORE) == 0 && Core_JsonMember(content, contentLength, "slice", &value, &valueLength))
        for (i = 0; i < ARRAYSIZE(kSliceMembers); i++) {
            const char *keys[2] = { "state", kSliceMembers[i] }, *member;
            size_t memberLength;
            if (Core_JsonMember(value, valueLength, kSliceMembers[i], &member, &memberLength) &&
                !SetNested(&json, &used, keys, 2, member, memberLength))
                Util_Log(L"web storage %s: its %S kept, not an object there", name, kSliceMembers[i]);
        }
    if (json && (set || text) && (!text || used != length || memcmp(json, text, used) != 0)) {
        ok = WebStore_Set(store, name, json, used);
        *changed = TRUE;
    }
    Util_Free(text);
    Util_Free(json);
    return ok;
}

/* The values of family part `part` of `content`: each "<family><id>" made
 * its member <id>, where the profile lists that session. */
typedef struct FamilyWrite {
    WebStore    *store;
    const WCHAR *family;
    BOOL        *changed;
    BOOL         ok;
} FamilyWrite;

static BOOL SetFamilyValue(void *context, const char *key, size_t keyLength, const char *value, size_t valueLength)
{
    FamilyWrite *write = (FamilyWrite *)context;
    WCHAR name[WEB_NAME_CCH];
    size_t prefix = wcslen(write->family), i, length = 0;
    char *current;
    if (keyLength <= sizeof "local_" - 1 || memcmp(key, "local_", sizeof "local_" - 1) != 0 || prefix + keyLength >= ARRAYSIZE(name))
        return TRUE;
    StringCchCopyW(name, ARRAYSIZE(name), write->family);
    for (i = 0; i < keyLength; i++) name[prefix + i] = (WCHAR)(unsigned char)key[i];
    name[prefix + keyLength] = 0;
    current = WebStore_Get(write->store, name, &length);
    if (!current || length != valueLength || memcmp(current, value, valueLength) != 0) {
        if (WebStore_Set(write->store, name, value, valueLength)) *write->changed = TRUE;
        else write->ok = FALSE;
    }
    Util_Free(current);
    return TRUE;
}

/* The copy `name` (LSS-persisted.<...>) of a part `value` of a layout: its
 * "value" (its member `scope`, with one) made it, and its time now. One
 * there is none of is made only with `make`. */
static BOOL SetWebCopy(WebStore *store, const WCHAR *name, const char *scope, const char *value, size_t valueLength, BOOL make, BOOL *changed)
{
    const char *inner;
    const char *keys[2] = { "value", scope }, *timeKey[1] = { "timestamp" };
    size_t innerLength, length = 0, used;
    char *text, *json, now[TIME_TEXT_BYTES];
    BOOL ok = TRUE;
    text = WebStore_Get(store, name, &length);
    if (text && Core_JsonMember(text, length, "value", &inner, &innerLength) &&
        (!scope || Core_JsonMember(inner, innerLength, scope, &inner, &innerLength)) && innerLength == valueLength && memcmp(inner, value, valueLength) == 0) {
        Util_Free(text);
        return TRUE;
    }
    if (!text && !make) return TRUE;
    if (text) {
        json = text;
        used = length;
    } else {
        static const char kEmpty[] = "{\"value\":{},\"tabId\":\"\",\"timestamp\":0}";
        json = Util_CopyBytes(kEmpty, sizeof kEmpty - 1);
        used = sizeof kEmpty - 1;
    }
    StringCchPrintfA(now, sizeof now, "%I64u", Util_NowMs());
    if (json && SetNested(&json, &used, keys, scope ? 2 : 1, value, valueLength) && SetNested(&json, &used, timeKey, 1, now, strlen(now))) {
        ok = WebStore_Set(store, name, json, used);
        *changed = TRUE;
    } else {
        Util_Log(L"web storage %s: kept, not Claude's copy", name);
    }
    Util_Free(json);
    return ok;
}

/* JSON texts alike but for the white space between their tokens. */
static BOOL SameJson(const char *a, size_t aLength, const char *b, size_t bLength)
{
    size_t i = 0, j = 0;
    BOOL quoted = FALSE, escaped = FALSE;
    for (;;) {
        if (!quoted) {
            while (i < aLength && (a[i] == ' ' || a[i] == '\t' || a[i] == '\r' || a[i] == '\n')) i++;
            while (j < bLength && (b[j] == ' ' || b[j] == '\t' || b[j] == '\r' || b[j] == '\n')) j++;
        }
        if (i == aLength || j == bLength) return i == aLength && j == bLength;
        if (a[i] != b[j]) return FALSE;
        if (escaped) escaped = FALSE;
        else if (quoted && a[i] == '\\') escaped = TRUE;
        else if (a[i] == '"') quoted = !quoted;
        i++;
        j++;
    }
}

/* Claude sends its store `synced` (ccd/<name>) to this account's server at
 * its next start, instead of putting the server's over it (`*marked`); not
 * where its Claude last showed another account. */
static BOOL MarkSynced(WebStore *store, const Profile *p, const char *account, const char *scope, const WCHAR *synced, BOOL *marked)
{
    WCHAR name[WEB_NAME_CCH];
    size_t ownerLength = 0;
    char *owner = WebStore_Get(store, SYNC_OWNER, &ownerLength);
    BOOL ok = TRUE;
    if (owner && (ownerLength != strlen(account) || memcmp(owner, account, ownerLength) != 0)) {
        Util_Log(L"%s of %s: its Claude last showed another account; not sent to the server at its next start", synced, p->folder);
    } else {
        ok = SUCCEEDED(StringCchPrintfW(name, ARRAYSIZE(name), SYNC_PENDING L"%s", synced)) && WebStore_Set(store, name, scope, strlen(scope));
        if (ok) *marked = TRUE;
    }
    Util_Free(owner);
    return ok;
}

/* Which sessions are starred, in Claude's IndexedDB of `p`: the layout's
 * `value` where Claude made the record, its time now, the database copied
 * to the backup first. */
static OpResult ApplyStarred(const Profile *p, const char *value, size_t valueLength, SyncReport *report)
{
    static const char *const kIds[2] = { "state", "starredIds" };
    static const char *const kTime[1] = { "updatedAt" };
    WCHAR backup[MAX_PATH];
    WebStore *store;
    const char *ids;
    char *text, *json = NULL, now[TIME_TEXT_BYTES];
    size_t length = 0, idsLength, used = 0;
    OpResult result = OP_SAME;
    if ((store = WebStore_OpenIndexed(p, STARRED_DATABASE, STARRED_STORE, STARRED_RECORD)) == NULL) return OP_SAME;
    /* None yet (Claude makes it once it shows the sidebar), or as it is. */
    if ((text = WebStore_GetIndexed(store, &length)) == NULL ||
        (Core_JsonMember(text, length, "state", &ids, &idsLength) && Core_JsonMember(ids, idsLength, "starredIds", &ids, &idsLength) &&
         SameJson(ids, idsLength, value, valueLength)))
        goto done;
    StringCchPrintfA(now, sizeof now, "%I64u", Util_NowMs());
    if ((json = Util_CopyBytes(text, length)) != NULL) used = length;
    if (!json || !SetNested(&json, &used, kIds, 2, value, valueLength) || !SetNested(&json, &used, kTime, 1, now, strlen(now)) ||
        !WebStore_SetIndexed(store, json, used)) {
        Util_Log(L"the starred sessions in the IndexedDB of %s: kept, not Claude's record", p->folder);
    } else if (SessionLink_Busy(p)) {
        result = OP_STOPPED;
    } else if (!BackupDir(p, report, backup, ARRAYSIZE(backup)) || FAILED(StringCchCatW(backup, ARRAYSIZE(backup), L"\\" CLAUDE_INDEXED_DB)) ||
               !WebStore_Commit(store, backup)) {
        CannotWrite(report, p->folder, GetLastError());
        result = OP_FAILED;
    } else {
        Util_Log(L"the starred sessions in the IndexedDB of %s: replaced", p->folder);
        result = OP_MADE;
    }
done:
    Util_Free(json);
    Util_Free(text);
    WebStore_Free(store);
    return result;
}

/* The parts of `content` kept in the web storage of `p` written there, with
 * the copies of the groups, sections and settings, and the sessions starred
 * in its IndexedDB; each database copied to the backup first. */
static OpResult ApplyWeb(const Profile *p, const char *account, const char *scope, const char *content, size_t contentLength, SyncReport *report)
{
    WCHAR backup[MAX_PATH], copy[128], synced[WEB_NAME_CCH];
    WebStore *store;
    const WCHAR *names[ARRAYSIZE(kWebParts)];
    const char *value;
    char name[128], *before[ARRAYSIZE(kSyncedStores)];
    size_t valueLength, i, j, count = 0, beforeLength[ARRAYSIZE(kSyncedStores)];
    BOOL changed = FALSE, ok = TRUE, any = FALSE;
    OpResult result = OP_FAILED, starred = OP_SAME;
    for (i = 0; i < ARRAYSIZE(kWebParts) && !any; i++)
        if (Core_JsonMember(content, contentLength, kWebParts[i].part, &value, &valueLength)) any = TRUE;
    for (i = 0; i < ARRAYSIZE(kSettingParts) && !any; i++)
        if (kSettingParts[i].window && Core_JsonMember(content, contentLength, kSettingParts[i].part, &value, &valueLength)) any = TRUE;
    /* None yet (Claude makes it at its first start): the next sync sends them again. */
    if (!any || (store = WebStore_Open(p)) == NULL) return OP_SAME;
    for (i = 0; i < ARRAYSIZE(kWebParts); i++) {
        if (kWebParts[i].form == FORM_FAMILY) continue;
        for (j = 0; j < count && wcscmp(names[j], kWebParts[i].value) != 0; j++) {}
        if (j == count) names[count++] = kWebParts[i].value;
    }
    for (i = 0; i < ARRAYSIZE(kSyncedStores); i++) before[i] = WebStore_Get(store, kSyncedStores[i].name, &beforeLength[i]);
    for (i = 0; i < count && ok; i++) ok = SetWebValue(store, names[i], scope, content, contentLength, &changed);
    /* A store that is a setting whole, where Claude made it. */
    for (i = 0; i < ARRAYSIZE(kSyncedStores) && ok; i++)
        if (kSyncedStores[i].setting && before[i] && Core_JsonMember(content, contentLength, kSyncedStores[i].setting, &value, &valueLength) &&
            !SameJson(before[i], beforeLength[i], value, valueLength)) {
            ok = WebStore_Set(store, kSyncedStores[i].name, value, valueLength);
            changed = TRUE;
        }
    /* A store the server keeps too changed: Claude sends it to this
     * account's server at its next start, instead of putting the server's
     * old one over it. */
    for (i = 0; i < ARRAYSIZE(kSyncedStores) && ok; i++) {
        size_t afterLength = 0;
        char *after = WebStore_Get(store, kSyncedStores[i].name, &afterLength);
        if (after && (!before[i] || afterLength != beforeLength[i] || memcmp(after, before[i], afterLength) != 0))
            ok = SUCCEEDED(StringCchPrintfW(synced, ARRAYSIZE(synced), L"ccd/%s", kSyncedStores[i].name)) &&
                 MarkSynced(store, p, account, scope, synced, &changed);
        Util_Free(after);
    }
    for (i = 0; i < ARRAYSIZE(kSyncedStores); i++) Util_Free(before[i]);
    if (ok && Core_JsonMember(content, contentLength, "starred", &value, &valueLength)) {
        starred = ApplyStarred(p, value, valueLength, report);
        if (starred == OP_MADE) ok = MarkSynced(store, p, account, scope, STARRED_SYNCED, &changed);
    }
    for (i = 0; i < ARRAYSIZE(kWebParts) && ok; i++) {
        FamilyWrite write;
        if (kWebParts[i].form != FORM_FAMILY || !Core_JsonMember(content, contentLength, kWebParts[i].part, &value, &valueLength)) continue;
        write.store = store;
        write.family = kWebParts[i].value;
        write.changed = &changed;
        write.ok = TRUE;
        ok = Core_JsonEachMember(value, valueLength, SetFamilyValue, &write) && write.ok;
    }
    /* The copies of the groups and sections only where the value they copy is. */
    for (i = 0; i < ARRAYSIZE(kWebParts) && ok; i++) {
        size_t length = 0;
        char *text;
        if (!kWebParts[i].copy || !Core_JsonMember(content, contentLength, kWebParts[i].part, &value, &valueLength) ||
            FAILED(StringCchPrintfW(copy, ARRAYSIZE(copy), COPY_PREFIX L"%S", kWebParts[i].copy)))
            continue;
        text = WebStore_Get(store, kWebParts[i].value, &length);
        if (text) ok = SetWebCopy(store, copy, scope, value, valueLength, TRUE, &changed);
        Util_Free(text);
    }
    /* The window's own copies of its settings, where it has them. */
    for (i = 0; i < ARRAYSIZE(kSettingParts) && ok; i++) {
        if (!kSettingParts[i].window || !Core_JsonMember(content, contentLength, kSettingParts[i].part, &value, &valueLength) ||
            FAILED(StringCchPrintfA(name, sizeof name, "%s%s", kSettingParts[i].name, kSettingParts[i].scope == LAYOUT_ACCOUNT ? account : "")) ||
            FAILED(StringCchPrintfW(copy, ARRAYSIZE(copy), COPY_PREFIX L"%S", name)))
            continue;
        ok = SetWebCopy(store, copy, NULL, value, valueLength, FALSE, &changed);
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
    if (result != OP_FAILED && (starred == OP_FAILED || starred == OP_STOPPED)) return starred;
    return result == OP_SAME ? starred : result;
}

/* The sessions of entries `ids` (their own ids, local_<id>) read, or marked
 * unread as Claude's Mark as unread does, in the web storage of closed
 * profile `p`, what it replaces backed up first. TRUE when made, or when
 * nothing was to change (no web storage yet: Claude shows none unread). */
BOOL SessionSync_SetUnread(const Profile *p, const WCHAR *const *ids, int count, BOOL unread)
{
    WebStore *store;
    SyncReport *report;
    WCHAR backup[MAX_PATH];
    char *value, *next;
    size_t length = 0, nextLength;
    BOOL ok = TRUE, changed = FALSE;
    int i;
    if ((store = WebStore_Open(p)) == NULL) return TRUE;
    value = WebStore_Get(store, UNREAD_STORE, &length);
    for (i = 0; i < count && ok; i++) {
        if (!unread && !Core_UnreadHas(value, length, ids[i])) continue;
        if ((next = Core_UnreadSet(value, length, ids[i], unread, &nextLength)) == NULL) {
            ok = FALSE;
            break;
        }
        if (!value || nextLength != length || memcmp(next, value, length) != 0) changed = TRUE;
        Util_Free(value);
        value = next;
        length = nextLength;
    }
    if (ok && changed) ok = WebStore_Set(store, UNREAD_STORE, value, length);
    if (ok && changed) {
        report = (SyncReport *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *report);
        ok = report && !SessionLink_Busy(p) && BackupDir(p, report, backup, ARRAYSIZE(backup)) &&
             SUCCEEDED(StringCchCatW(backup, ARRAYSIZE(backup), L"\\" CLAUDE_WEB_STORAGE)) && WebStore_Commit(store, backup);
        Util_Free(report);
        Util_Log(L"the sessions unread in the web storage of %s: %d %s%s", p->folder, count, unread ? L"marked unread" : L"read",
                 ok ? L"" : L" FAILED");
    }
    Util_Free(value);
    WebStore_Free(store);
    return ok;
}

/* The parts of `content` kept in config.json and Chromium's Preferences put
 * in those files of `p`. */
static OpResult ApplyFiles(const Profile *p, const char *content, size_t contentLength, SyncReport *report)
{
    static const WCHAR *const kFiles[] = { CLAUDE_APP_SETTINGS, CHROMIUM_PREFERENCES };
    OpResult result = OP_SAME;
    size_t f, i;
    for (f = 0; f < ARRAYSIZE(kFiles) && result != OP_STOPPED && result != OP_FAILED; f++) {
        WCHAR path[LONG_PATH_CCH];
        DWORD currentLength = 0;
        char *current, *json, own[128];
        const char *value, *found;
        size_t valueLength, foundLength, used;
        BOOL any = FALSE;
        OpResult one;
        for (i = 0; i < ARRAYSIZE(kFileParts) && !any; i++)
            if (wcscmp(kFileParts[i].file, kFiles[f]) == 0 && Core_JsonMember(content, contentLength, kFileParts[i].part, &value, &valueLength)) any = TRUE;
        /* Only a file Claude made: a profile not started yet gets them at the next sync. */
        if (!any || FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\%s", p->storageDir, kFiles[f])) ||
            (current = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &currentLength)) == NULL)
            continue;
        json = Util_CopyBytes(current, currentLength);
        used = currentLength;
        for (i = 0; i < ARRAYSIZE(kFileParts) && json; i++) {
            const FilePart *part = &kFileParts[i];
            const char *keys[3];
            int level;
            if (wcscmp(part->file, kFiles[f]) != 0 || !Core_JsonMember(content, contentLength, part->part, &value, &valueLength)) continue;
            for (level = 0; level < part->depth; level++) keys[level] = part->keys[level];
            if (part->own) {
                if (!Core_ZoomPartitionKey(p->storageDir, own, sizeof own)) own[0] = 0;
                if (!FileMember(json, used, part, &found, &foundLength, own, sizeof own)) continue;   /* none of its own yet */
                keys[part->depth - 1] = own;
            }
            if (!SetNested(&json, &used, keys, part->depth, value, valueLength))
                Util_Log(L"%s of %s: its %S kept, not an object there", kFiles[f], p->folder, part->part);
        }
        one = json ? WriteLayoutFile(p, path, current, currentLength, json, used, kFiles[f], report) : OP_FAILED;
        if (one != OP_SAME) result = one;
        Util_Free(json);
        Util_Free(current);
    }
    return result;
}

/* Presets carry only the source account's object. The destination account is
 * selected from its entries path, never copied from the source's id. */
static OpResult ApplyPresets(const Profile *p, const char *account, const char *content, size_t contentLength, SyncReport *report)
{
    const char *incoming;
    size_t incomingLength = 0, mergedLength = 0;
    DWORD currentLength = 0;
    char *current, *merged;
    WCHAR path[LONG_PATH_CCH];
    OpResult result;
    if (!Core_JsonMember(content, contentLength, "sessionPresets", &incoming, &incomingLength)) return OP_SAME;
    if (FAILED(StringCchPrintfW(path, ARRAYSIZE(path), L"%s\\" SESSION_PRESETS_FILE, p->storageDir))) return OP_FAILED;
    current = Util_ReadFile(path, CONTENT_MAX_BYTES, FALSE, &currentLength);
    if (!current && Util_QueryPath(path, NULL) != PATH_MISSING) {
        CannotWrite(report, path, GetLastError()); return OP_FAILED;
    }
    merged = Presets_Merge(current ? current : "{}", current ? currentLength : 2, account, incoming, incomingLength, &mergedLength);
    if (!merged) { Util_Free(current); CannotWrite(report, path, ERROR_INVALID_DATA); return OP_FAILED; }
    result = WriteLayoutFile(p, path, current, currentLength, merged, mergedLength, L"Session presets", report);
    Util_Free(merged); Util_Free(current);
    return result;
}

/* The layout `content` (a layout of ours) put in `p`, for the account and
 * organization of its entries folder `dir`: in its settings, its web storage
 * and its two files. What else they hold stays as it is. */
static OpResult ApplyLayout(const Profile *p, const WCHAR *dir, const char *content, size_t contentLength, SyncReport *report)
{
    char account[SESSION_ID_CCH * 3], scope[SESSION_ID_CCH * 6 + 2];
    OpResult results[4];
    int i;
    BOOL made = FALSE;
    if (!p->storageDir[0] || !LayoutScopeOf(dir, account, sizeof account, scope, sizeof scope)) {
        CannotWrite(report, p->folder, ERROR_INVALID_DATA);
        return OP_FAILED;
    }
    results[0] = ApplySettings(p, account, scope, content, contentLength, report);
    if (results[0] == OP_STOPPED || results[0] == OP_FAILED) return results[0];
    results[1] = ApplyWeb(p, account, scope, content, contentLength, report);
    if (results[1] == OP_STOPPED || results[1] == OP_FAILED) return results[1];
    results[2] = ApplyFiles(p, content, contentLength, report);
    if (results[2] == OP_STOPPED || results[2] == OP_FAILED) return results[2];
    results[3] = ApplyPresets(p, account, content, contentLength, report);
    if (results[3] == OP_STOPPED || results[3] == OP_FAILED) return results[3];
    for (i = 0; i < 4; i++) made |= results[i] == OP_MADE;
    return made ? OP_MADE : OP_SAME;
}

char *SessionSync_FilterLayout(const char *layout, size_t length, DWORD items, size_t *outLength)
{
    char *out;
    LayoutPart part;
    int p;
    size_t used = length;
    *outLength = 0;
    if (!layout || (out = (char *)HeapAlloc(GetProcessHeap(), 0, length + 1)) == NULL) return NULL;
    memcpy(out, layout, length);
    out[used] = 0;
    for (p = 0; SessionSync_LayoutPart(p, &part); p++)
        if (!(part.item & items) && !Core_JsonRemoveMember(out, used, part.name, out, used + 1, &used)) {
            HeapFree(GetProcessHeap(), 0, out);
            return NULL;
        }
    if (!Core_JsonRemoveMember(out, used, LAYOUT_CANNOT_TAKE, out, used + 1, &used)) {   /* this profile's alone */
        HeapFree(GetProcessHeap(), 0, out);
        return NULL;
    }
    out[used] = 0;
    *outLength = used;
    return out;
}

BOOL SessionSync_RestoreLayout(const Profile *p, const WCHAR *entriesDir, const char *layout, size_t length, SyncReport *report)
{
    OpResult result;
    SyncReport ignored;
    if (!report) {
        ZeroMemory(&ignored, sizeof ignored);
        report = &ignored;
    }
    result = ApplyLayout(p, entriesDir, layout, length, report);
    return result == OP_MADE || result == OP_SAME;
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
    Util_Free(content);
    return result;
}

/* Makes the changes sent to `p` while its Claude is closed, in order; one
 * its Claude overtakes waits with those after it. One that fails stays for
 * the next time, SYNC_TRIES_MAX times at most: until it is made, the vault
 * takes it for the profile's entry (a waiting change), so what the profile
 * still shows is never taken for a change of its own. Returns how many left
 * the plan. A profile with no entries folder yet keeps them: made once it has. */
int SessionSync_ApplyPending(const Profile *p, SyncReport *report)
{
    SyncReport own;
    SessionSet entries;
    WCHAR staging[MAX_PATH];
    HANDLE lock = NULL, syncLock = NULL;
    Plan plan;
    int i = 0, n = 0, kept = 0, made = 0, added, updated;
    BOOL busy;
    ZeroMemory(&plan, sizeof plan);
    if (!report) {
        ZeroMemory(&own, sizeof own);
        report = &own;
    }
    added = report->added;
    updated = report->updated;
    if (!PlanDir(p, staging, ARRAYSIZE(staging)) || (syncLock = Util_SyncLock()) == NULL) return 0;
    if ((lock = SessionEdit_Lock(p)) == NULL) {
        Util_SyncUnlock(syncLock);
        return 0;
    }
    SessionLink_BeginBatch();
    if ((busy = SessionLink_Busy(p)) != FALSE || (n = LoadPlan(p, &plan)) <= 0) {
        if (!busy && n < 0) Util_Log(L"sessions sent to %s: the plan cannot be read", p->folder);
        SessionLink_EndBatch();
        SessionEdit_Unlock(lock);
        Util_SyncUnlock(syncLock);
        Util_Free(plan.ops);
        return 0;
    }
    if (!SessionStore_LoadEntries(&entries, p)) Util_Log(L"sessions sent to %s: some of its entries cannot be read", p->folder);
    if (!entries.source[0].entriesDir[0]) {
        Util_Log(L"sessions sent to %s: kept, it has no session entries yet", p->folder);
    } else {
        for (i = 0; i < plan.count; i++) {
            OpResult result = ApplyOp(p, &entries, entries.source[0].entriesDir, staging, &plan.ops[i], report);
            if (result == OP_STOPPED) {
                Util_Log(L"sessions sent to %s: the rest waits, its Claude started", p->folder);
                break;
            }
            if (result == OP_FAILED && ++plan.ops[i].tries < SYNC_TRIES_MAX) {
                Util_Log(L"sessions sent to %s: session %s waits for another try (%lu)", p->folder, plan.ops[i].key, plan.ops[i].tries);
                plan.ops[kept++] = plan.ops[i];
                continue;
            }
            if (result == OP_FAILED) Util_Log(L"sessions sent to %s: session %s given up after %d tries", p->folder, plan.ops[i].key, SYNC_TRIES_MAX);
            DeleteContent(staging, &plan.ops[i]);
            made++;
        }
        /* What failed, then what was not reached, in their order. */
        if (kept < i) MoveMemory(plan.ops + kept, plan.ops + i, (size_t)(plan.count - i) * sizeof *plan.ops);
        if ((made || kept) && !SavePlan(p, plan.ops, kept + plan.count - i, NULL, 0))
            Util_Log(L"sessions sent to %s: the plan could not be written", p->folder);
    }
    SessionLink_EndBatch();
    SessionEdit_Unlock(lock);
    Util_SyncUnlock(syncLock);
    SessionStore_Free(&entries);
    Util_Free(plan.ops);
    i = made;
    if (i) Util_Log(L"%d change(s) sent to %s made (%d session(s) added, %d replaced)", i, p->folder, report->added - added, report->updated - updated);
    return i;
}

/* Each profile's part to its plan, made at once where it is closed. */
static BOOL OutOfMemory(SyncReport *report);

static SyncProfileResult *ProfileResult(SyncReport *report, const Profile *profile)
{
    SyncProfileResult *detail;
    int d;
    for (d = 0; d < report->profileCount; d++)
        if (Core_EqualsI(report->profiles[d].folder, profile->folder)) return &report->profiles[d];
    if (report->profileCount >= MAX_PROFILES) return NULL;
    detail = &report->profiles[report->profileCount++];
    ZeroMemory(detail, sizeof *detail);
    StringCchCopyW(detail->folder, ARRAYSIZE(detail->folder), profile->folder);
    return detail;
}

static void ResultTargets(const SessionSet *set, DWORD targets, DWORD takers, SyncReport *report)
{
    int p;
    for (p = 0; p < set->profiles.count; p++)
        if (targets & (1u << p)) {
            SyncProfileResult *detail = ProfileResult(report, &set->profiles.items[p]);
            if (detail) detail->unavailable = !(takers & (1u << p));
        }
}

static void ResultSkipped(const SessionSet *set, int p, SyncReport *report)
{
    SyncProfileResult *detail = ProfileResult(report, &set->profiles.items[p]);
    report->skipped++;
    if (detail) detail->skipped++;
}

static void ResultPart(const Profile *profile, const SyncReport *part, SyncReport *report)
{
    SyncProfileResult *detail = ProfileResult(report, profile);
    report->added += part->added;
    report->updated += part->updated;
    report->removed += part->removed;
    report->skipped += part->skipped;
    report->failed += part->failed;
    if (!report->error[0]) StringCchCopyW(report->error, ARRAYSIZE(report->error), part->error);
    if (part->backup[0]) StringCchCopyW(report->backup, ARRAYSIZE(report->backup), part->backup);
    if (detail) {
        detail->added += part->added;
        detail->updated += part->updated;
        detail->removed += part->removed;
        detail->skipped += part->skipped;
        detail->failed += part->failed;
        detail->waiting = part->waiting != 0;
        if (!detail->error[0]) StringCchCopyNW(detail->error, ARRAYSIZE(detail->error), part->error, ARRAYSIZE(detail->error) - 1);
    }
}

static void Send(Outbox *box, const SessionSet *set, SyncReport *report)
{
    HANDLE lock = Util_SyncLock();
    SyncReport *part = NULL;
    int p;
    if (!lock) {
        report->failed++;
        return;
    }
    part = (SyncReport *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *part);
    if (!part) {
        OutOfMemory(report);
        Util_SyncUnlock(lock);
        return;
    }
    for (p = 0; p < set->profiles.count; p++) {
        const Profile *profile = &set->profiles.items[p];
        if (!box->sent[p].count) continue;
        ZeroMemory(part, sizeof *part);
        StringCchCopyW(part->backup, ARRAYSIZE(part->backup), report->backup);
        if (!StageSent(profile, &box->sent[p])) {
            WCHAR path[MAX_PATH];
            if (!SessionSync_PlanPath(profile, path, ARRAYSIZE(path))) StringCchCopyW(path, ARRAYSIZE(path), profile->folder);
            CannotWrite(part, path, GetLastError());
        } else {
            SessionSync_ApplyPending(profile, part);
            if (SessionSync_PendingCount(profile) > 0) {
                report->waiting |= 1u << p;
                part->waiting = 1;
            }
        }
        ResultPart(profile, part, report);
    }
    Util_Free(part);
    Util_SyncUnlock(lock);
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
    ResultTargets(set, profiles, takers, report);
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
        ResultTargets(set, 1u << change->profile, takers, report);
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

BOOL SessionSync_Remove(const SessionSet *set, DWORD profiles, const int *rows, int rowCount, SyncReport *report)
{
    Outbox box;
    int i, p;
    ZeroMemory(&box, sizeof box);
    profiles = ValidProfiles(set, profiles);
    ResultTargets(set, profiles, SessionSync_Takers(set), report);
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
    ResultTargets(set, targets, takers, report);
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
                ResultSkipped(set, t, report);
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
    WCHAR id[SESSION_ID_CCH];
    return Util_NewId(id, ARRAYSIZE(id), NULL) && SUCCEEDED(StringCchPrintfW(out, cch, LOCAL_PREFIX L"%s", id));
}

/* The entry of a copy: the original's, under its own id and transcript
 * `copyId`, in the working folder it got (a session without a folder), and
 * without the original's other transcripts: the copy goes on from its own.
 * A heap block; NULL when it cannot be made. */
static char *CopyEntry(const char *original, size_t length, const WCHAR *copyId, size_t *outLength)
{
    WCHAR ownId[SESSION_ID_CCH], cwd[MAX_PATH];
    const char *value;
    size_t valueLength;
    char *data, *next;
    if (!NewEntryId(ownId, ARRAYSIZE(ownId)) || (data = WithString(original, length, "sessionId", ownId, &length)) == NULL) return NULL;
    if ((next = WithString(data, length, "cliSessionId", copyId, &length)) == NULL) goto failed;
    Util_Free(data);
    data = next;
    if (SessionEdit_CopiedCwd(copyId, cwd, ARRAYSIZE(cwd))) {
        if ((next = WithString(data, length, "cwd", cwd, &length)) == NULL) goto failed;
        Util_Free(data);
        data = next;
        if (Core_JsonMember(data, length, "originCwd", &value, &valueLength)) {
            if ((next = WithString(data, length, "originCwd", cwd, &length)) == NULL) goto failed;
            Util_Free(data);
            data = next;
        }
    }
    if (!Core_EntryDropHistory(data, &length)) goto failed;
    *outLength = length;
    return data;
failed:
    Util_Free(data);
    return NULL;
}

CopyResult SessionSync_Copy(HWND owner, const SessionSet *set, int from, const int *rows, int rowCount, DWORD targets, SyncReport *report)
{
    WCHAR copyId[SESSION_ID_CCH], error[LONG_PATH_CCH], left[LONG_PATH_CCH];
    DWORD takers, sent = 0;
    CopyResult result = COPY_MADE;
    SyncReport *part = (SyncReport *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *part);
    int i, t, p;
    if (!part) { OutOfMemory(report); return COPY_FAILED; }
    targets = ValidProfiles(set, targets) & ~(from >= 0 ? 1u << from : 0);
    takers = targets & SessionSync_Takers(set);
    report->unavailable |= targets & ~takers;
    ResultTargets(set, targets, takers, report);
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
                ResultSkipped(set, t, report);
                continue;
            }
            copied = SessionEdit_CopyConversation(owner, set, rows[i], t, copyId, ARRAYSIZE(copyId), error, ARRAYSIZE(error));
            if (copied == COPY_CANCELLED) {
                result = COPY_CANCELLED;
                break;
            }
            if (copied == COPY_FAILED) {
                SyncProfileResult *detail = ProfileResult(report, &set->profiles.items[t]);
                report->failed++;
                if (!report->error[0]) StringCchCopyW(report->error, ARRAYSIZE(report->error), error);
                if (detail) {
                    detail->failed++;
                    if (!detail->error[0]) StringCchCopyNW(detail->error, ARRAYSIZE(detail->error), error, ARRAYSIZE(detail->error) - 1);
                }
                continue;
            }
            ZeroMemory(part, sizeof *part);
            ZeroMemory(&one, sizeof one);
            MakeOp(&op, SYNC_PUT, SYNC_UNDELETE, copyId, Core_JsonMemberNumber(original, length, "lastActivityAt"), 0);
            entry = CopyEntry(original, length, copyId, &entryLength);
            /* Not sent, the copy is listed nowhere: what it made goes again. */
            if (!entry || !AddSent(&one, &op, entry, entryLength) || !StageSent(&set->profiles.items[t], &one)) {
                CannotWrite(part, set->profiles.items[t].folder, entry ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY);
                if (!SessionEdit_RemoveCopy(set, rows[i], t, copyId, left, ARRAYSIZE(left)) && left[0])
                    Util_Log(L"session copy %s: %s left behind", copyId, left);
            } else {
                sent |= 1u << t;
            }
            FreeSent(&one);
            Util_Free(entry);
            ResultPart(&set->profiles.items[t], part, report);
        }
        Util_Free(original);
    }
    for (p = 0; p < set->profiles.count; p++) {
        if (!(sent & (1u << p))) continue;
        ZeroMemory(part, sizeof *part);
        StringCchCopyW(part->backup, ARRAYSIZE(part->backup), report->backup);
        SessionSync_ApplyPending(&set->profiles.items[p], part);
        if (SessionSync_PendingCount(&set->profiles.items[p]) > 0) {
            report->waiting |= 1u << p;
            part->waiting = 1;
        }
        ResultPart(&set->profiles.items[p], part, report);
    }
    Util_Free(part);
    return result;
}

/* ------------------------------------------------------------- archives */

/* `path` under `root` as an archive's name, claude/<its path under root>, a
 * folder's ending in '/'. */
static BOOL ArchiveName(const WCHAR *root, const WCHAR *path, BOOL folder, char *name, size_t cch)
{
    size_t rootLength = wcslen(root), prefix = sizeof ARCHIVE_FILES - 1, i, length;
    if (cch < prefix + 3 || !Core_PathUnder(path, root) || Core_PathEquals(path, root)) return FALSE;
    memcpy(name, ARCHIVE_FILES, prefix);
    if (WideCharToMultiByte(CP_UTF8, 0, path + rootLength + 1, -1, name + prefix, (int)(cch - prefix - 1), NULL, NULL) <= 1) return FALSE;
    for (i = prefix; name[i]; i++)
        if (name[i] == '\\') name[i] = '/';
    if (folder) {
        length = strlen(name);
        name[length] = '/';
        name[length + 1] = 0;
    }
    return TRUE;
}

/* `path` (a file, or a folder and what it holds) under `root`, as
 * claude/<its path under root>; one Claude Code still writes is taken up to
 * where it is. */
static BOOL ZipAddTree(ZipOut *zip, const WCHAR *root, const WCHAR *path, SyncReport *report)
{
    WCHAR extended[LONG_PATH_CCH], child[LONG_PATH_CCH];
    char name[ARCHIVE_NAME_MAX];
    WIN32_FIND_DATAW found;
    DWORD attributes = 0;
    HANDLE find;
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
    if (!ArchiveName(root, path, FALSE, name, sizeof name)) {
        CannotRead(report, path, ERROR_FILENAME_EXCED_RANGE);
        return FALSE;
    }
    if (Zip_Has(zip, name)) return TRUE;
    if (Zip_AddFile(zip, name, path)) return TRUE;
    if (Zip_Error(zip) != ERROR_FILE_TOO_LARGE) CannotRead(report, path, Zip_Error(zip));
    return FALSE;
}

/* Which of an archive's conversation files are entry `n`'s, as
 * conversations/<n>.json: the archive names of `paths` (a folder's ending in
 * '/'), so that an import of some of its entries takes only their files. */
static BOOL AddConversationMap(ZipOut *zip, const WCHAR *root, int n, WCHAR (*paths)[LONG_PATH_CCH], int count)
{
    enum { QUOTED_CAP = ARCHIVE_NAME_MAX * 6 + 4 };
    char name[ARCHIVE_NAME_MAX + 2], mapName[64], *quoted = (char *)Util_Alloc(QUOTED_CAP), *json = (char *)Util_Alloc(256), *grown;
    WCHAR extended[LONG_PATH_CCH], wide[ARCHIVE_NAME_MAX + 2];
    size_t used = 1, cap = 256, length;
    DWORD attributes;
    int i, names = 0;
    BOOL ok = quoted && json;
    if (ok) json[0] = '[';
    for (i = 0; ok && i < count; i++) {
        attributes = 0;
        if (!Extended(paths[i], extended) || Util_QueryPath(extended, &attributes) != PATH_PRESENT) continue;   /* gone meanwhile */
        if (!ArchiveName(root, paths[i], (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, name, sizeof name) ||
            MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, ARRAYSIZE(wide)) <= 0 || !Core_JsonQuote(wide, quoted, QUOTED_CAP))
            continue;
        length = strlen(quoted);
        if (used + length + 2 > cap) {   /* room for a comma and the closing bracket */
            cap = (used + length + 2) * 2;
            if ((grown = (char *)HeapReAlloc(GetProcessHeap(), 0, json, cap)) == NULL) {
                ok = FALSE;
                break;
            }
            json = grown;
        }
        if (names++) json[used++] = ',';
        memcpy(json + used, quoted, length);
        used += length;
    }
    if (ok) {
        json[used++] = ']';
        ok = SUCCEEDED(StringCchPrintfA(mapName, sizeof mapName, ARCHIVE_PARTS "%d.json", n)) && Zip_AddData(zip, mapName, json, used);
    }
    Util_Free(json);
    Util_Free(quoted);
    return ok;
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
    WCHAR root[LONG_PATH_CCH], listError[LONG_PATH_CCH];
    WCHAR (*paths)[LONG_PATH_CCH] = NULL;
    char name[64], manifest[256];
    SyncReport report;
    ZipOut *zip;
    DWORD code = ERROR_SUCCESS;
    int i, j, pathCount = 0;
    BOOL ok = TRUE;
    *exported = 0;
    error[0] = 0;
    ZeroMemory(&report, sizeof report);
    if (!ClaudeCodeRoot(root, ARRAYSIZE(root))) {
        StringCchCopyW(error, errorCch, TR(L"The path is too long."));
        return FALSE;
    }
    if ((zip = Zip_Create(archive, &code)) == NULL) {
        StringCchPrintfW(error, errorCch, TR(L"%s could not be written (error %lu)."), archive, code);
        return FALSE;
    }
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
        ok = Zip_AddData(zip, name, entry, length);
        Util_Free(entry);
        if (ok && !SessionEdit_ListConversation(set, rows[i], &paths, &pathCount, listError, ARRAYSIZE(listError))) {
            StringCchCopyW(report.error, ARRAYSIZE(report.error), listError);
            ok = FALSE;
        }
        if (ok) ok = AddConversationMap(zip, root, *exported + 1, paths, pathCount);
        for (j = 0; ok && j < pathCount; j++) ok = ZipAddTree(zip, root, paths[j], &report);
        Util_Free(paths);
        paths = NULL;
        if (ok) (*exported)++;
    }
    if (ok) {
        StringCchPrintfA(manifest, sizeof manifest, "{\"format\":\"" ARCHIVE_FORMAT "\",\"version\":%d,\"sessions\":%d,\"exported\":%I64u}",
                         ARCHIVE_VERSION, *exported, Util_NowMs());
        ok = Zip_AddData(zip, ARCHIVE_MANIFEST, manifest, strlen(manifest));
    }
    code = Zip_Error(zip);
    if (!ok && !report.error[0]) {
        if (code == ERROR_FILE_TOO_LARGE) StringCchCopyW(report.error, ARRAYSIZE(report.error), TR(L"The archive is too large."));
        else StringCchPrintfW(report.error, ARRAYSIZE(report.error), TR(L"%s could not be written (error %lu)."), archive, code);
    }
    if (!Zip_Close(zip, ok) && ok) {
        StringCchPrintfW(report.error, ARRAYSIZE(report.error), TR(L"%s could not be written (error %lu)."), archive, GetLastError());
        ok = FALSE;
    }
    StringCchCopyW(error, errorCch, report.error);
    Util_Log(L"%d session(s) exported to %s%s", *exported, archive, ok ? L"" : L" FAILED");
    return ok;
}

static BOOL ItemNamed(const char *name, const char *prefix)
{
    return strncmp(name, prefix, strlen(prefix)) == 0;
}

/* An entry of the archive: entries/<n>.json, not a folder. */
static BOOL EntryItem(const ZipIn *zip, int index)
{
    const char *name = Zip_Name(zip, index);
    size_t length = strlen(name);
    return ItemNamed(name, ARCHIVE_ENTRIES) && length && name[length - 1] != '/';
}

/* The archive's own: a manifest of ours, of a version this one reads. */
static BOOL IsSessionArchive(const ZipIn *zip)
{
    WCHAR format[64];
    DWORD length = 0;
    int at = Zip_Find(zip, ARCHIVE_MANIFEST);
    char *manifest = at >= 0 ? (char *)Zip_Read(zip, at, SESSION_ENTRY_MAX_BYTES, &length) : NULL;
    BOOL ours = manifest && Core_JsonMemberString(manifest, length, "format", format, ARRAYSIZE(format)) && wcscmp(format, L"" ARCHIVE_FORMAT) == 0 &&
                Core_JsonMemberNumber(manifest, length, "version") <= ARCHIVE_VERSION;
    Util_Free(manifest);
    return ours;
}

BOOL SessionSync_IsArchive(const WCHAR *archive)
{
    ZipIn *zip = Zip_Open(archive);
    BOOL ours = zip && IsSessionArchive(zip);
    Zip_Free(zip);
    return ours;
}

/* An entry's session: its transcript's id, or its own before its first
 * message; FALSE for one that names neither as Claude writes them. */
static BOOL EntryKey(const char *entry, size_t length, WCHAR *own, size_t ownCch, WCHAR *key, size_t keyCch)
{
    if (Core_SessionEntryKind(entry, length) != ENTRY_LOCAL || !Core_JsonMemberString(entry, length, "sessionId", own, ownCch) || !IsEntryId(own))
        return FALSE;
    if (!Core_JsonMemberString(entry, length, "cliSessionId", key, keyCch) || !key[0]) StringCchCopyW(key, keyCch, own);
    return Core_IsUuid(key) || IsEntryId(key);
}

int SessionSync_ArchiveSessions(const ZipIn *zip, ArchivedSession **out)
{
    ArchivedSession *list = NULL, *grown;
    int i, count = 0, capacity = 0;
    *out = NULL;
    if (!zip || !IsSessionArchive(zip)) return -1;
    for (i = 0; i < Zip_Count(zip); i++) {
        WCHAR own[SESSION_ID_CCH];
        DWORD length = 0;
        char *entry;
        size_t c;
        if (!EntryItem(zip, i)) continue;
        if ((entry = (char *)Zip_Read(zip, i, SESSION_ENTRY_MAX_BYTES, &length)) == NULL) {
            Util_Free(list);
            return -1;
        }
        if (count == capacity) {
            capacity = capacity ? capacity * 2 : 32;
            grown = list ? (ArchivedSession *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, list, (size_t)capacity * sizeof *list)
                         : (ArchivedSession *)Util_Alloc((size_t)capacity * sizeof *list);
            if (!grown) {
                Util_Free(entry);
                Util_Free(list);
                return -1;
            }
            list = grown;
        }
        /* One that names its session as Claude never does is left out, as its import leaves it. */
        if (EntryKey(entry, length, own, ARRAYSIZE(own), list[count].key, ARRAYSIZE(list[count].key))) {
            if (!Core_JsonMemberString(entry, length, "title", list[count].title, ARRAYSIZE(list[count].title)) || !list[count].title[0])
                StringCchCopyW(list[count].title, ARRAYSIZE(list[count].title), list[count].key);
            for (c = 0; list[count].title[c]; c++)
                if (list[count].title[c] < L' ') list[count].title[c] = L' ';
            list[count].lastActivity = Core_JsonMemberNumber(entry, length, "lastActivityAt");
            StringCchCopyW(list[count].own, ARRAYSIZE(list[count].own), own);
            count++;
        }
        Util_Free(entry);
    }
    *out = list;
    return count;
}

/* Whether `pick` takes the session `key`: every one without keys. */
static BOOL Picked(const SessionPick *pick, const WCHAR *key)
{
    int i;
    if (!pick || !pick->keys) return TRUE;
    for (i = 0; i < pick->count; i++)
        if (_wcsicmp(pick->keys[i], key) == 0) return TRUE;
    return FALSE;
}

/* The names the conversation maps of the entries `pick` takes list
 * (AddConversationMap), gathered as Core_JsonEachElement goes. */
typedef struct PickedFiles {
    WCHAR **names;
    int     count, capacity;
    BOOL    ok;
} PickedFiles;

static BOOL AddPickedFile(void *context, const char *value, size_t valueLength)
{
    PickedFiles *files = (PickedFiles *)context;
    WCHAR name[ARCHIVE_NAME_MAX + 2];
    if (!Core_JsonString(value, valueLength, name, ARRAYSIZE(name))) return TRUE;
    if (files->count == files->capacity) {
        int capacity = files->capacity ? files->capacity * 2 : 64;
        WCHAR **grown = files->names ? (WCHAR **)HeapReAlloc(GetProcessHeap(), 0, files->names, (size_t)capacity * sizeof *grown)
                                     : (WCHAR **)Util_Alloc((size_t)capacity * sizeof *grown);
        if (!grown) return files->ok = FALSE;
        files->names = grown;
        files->capacity = capacity;
    }
    if ((files->names[files->count] = (WCHAR *)Util_CopyBytes((const char *)name, (wcslen(name) + 1) * sizeof(WCHAR))) == NULL) return files->ok = FALSE;
    files->count++;
    return TRUE;
}

static void FreePickedFiles(PickedFiles *files)
{
    int i;
    for (i = 0; i < files->count; i++) Util_Free(files->names[i]);
    Util_Free(files->names);
    ZeroMemory(files, sizeof *files);
}

/* The conversation files of the entries `pick` takes, from their maps;
 * FALSE (every file taken) when one of them has none: an archive of an
 * earlier version. */
static BOOL GatherPickedFiles(const ZipIn *zip, const SessionPick *pick, PickedFiles *files)
{
    int i;
    ZeroMemory(files, sizeof *files);
    files->ok = TRUE;
    for (i = 0; i < Zip_Count(zip) && files->ok; i++) {
        WCHAR own[SESSION_ID_CCH], key[SESSION_ID_CCH];
        char mapName[64];
        DWORD length = 0;
        char *entry, *map;
        int at;
        if (!EntryItem(zip, i)) continue;
        entry = (char *)Zip_Read(zip, i, SESSION_ENTRY_MAX_BYTES, &length);
        if (!entry || !EntryKey(entry, length, own, ARRAYSIZE(own), key, ARRAYSIZE(key)) || !Picked(pick, key)) {
            Util_Free(entry);
            continue;
        }
        Util_Free(entry);
        if (FAILED(StringCchPrintfA(mapName, sizeof mapName, ARCHIVE_PARTS "%s", Zip_Name(zip, i) + sizeof ARCHIVE_ENTRIES - 1)) ||
            (at = Zip_Find(zip, mapName)) < 0 || (map = (char *)Zip_Read(zip, at, SESSION_ENTRY_MAX_BYTES, &length)) == NULL) {
            files->ok = FALSE;
            break;
        }
        if (!Core_JsonEachElement(map, length, AddPickedFile, files)) files->ok = FALSE;
        Util_Free(map);
    }
    if (!files->ok) FreePickedFiles(files);
    return files->ok;
}

/* Whether archive name `name` is one of `files`, or under one of its folders. */
static BOOL InPickedFiles(const PickedFiles *files, const char *name)
{
    WCHAR wide[ARCHIVE_NAME_MAX + 2];
    int i;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, ARRAYSIZE(wide)) <= 0) return FALSE;
    for (i = 0; i < files->count; i++) {
        size_t length = wcslen(files->names[i]);
        if (length && files->names[i][length - 1] == L'/' ? _wcsnicmp(wide, files->names[i], length) == 0 : _wcsicmp(wide, files->names[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

/* The conversation files of archive `zip` put under Claude Code's folder
 * `root`, never over one there; only those of `files` when given; the count
 * added. A failure is in `report`. */
static int ExtractConversations(ZipIn *zip, const WCHAR *root, const PickedFiles *only, SyncReport *report)
{
    WCHAR path[LONG_PATH_CCH], relative[LONG_PATH_CCH];
    int i, files = 0;
    for (i = 0; i < Zip_Count(zip); i++) {
        const char *name = Zip_Name(zip, i);
        size_t length = strlen(name), prefix = sizeof ARCHIVE_FILES - 1, c;
        int wide;
        if (!ItemNamed(name, ARCHIVE_FILES) || length == prefix || name[length - 1] == '/') continue;
        if (only && !InPickedFiles(only, name)) continue;
        if (!Core_ArchiveNameSafe(name + prefix, length - prefix) ||
            (wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name + prefix, (int)(length - prefix), relative,
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
        if (Zip_ExtractTo(zip, i, path, UTIL_PUT_NEW)) {
            files++;
        } else {
            DWORD code = GetLastError();
            if (code == ERROR_INVALID_DATA) {
                StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The archive is damaged."));
                report->failed++;
            } else if (code != ERROR_ALREADY_EXISTS && code != ERROR_FILE_EXISTS) {
                CannotWrite(report, path, code);
            }
        }
    }
    return files;
}

BOOL SessionSync_Import(const SessionSet *set, const WCHAR *archive, DWORD targets, int *sessions, SyncReport *report)
{
    return SessionSync_ImportPicked(set, archive, targets, NULL, sessions, report);
}

BOOL SessionSync_ImportPicked(const SessionSet *set, const WCHAR *archive, DWORD targets, const SessionPick *pick, int *sessions, SyncReport *report)
{
    WCHAR root[LONG_PATH_CCH];
    ZipIn *zip;
    Outbox box;
    PickedFiles only;
    DWORD takers;
    BOOL mapped = FALSE;
    int i, t, files, checked = 0;
    *sessions = 0;
    ZeroMemory(&box, sizeof box);
    targets = ValidProfiles(set, targets);
    takers = targets & SessionSync_Takers(set);
    report->unavailable |= targets & ~takers;
    ResultTargets(set, targets, takers, report);
    if (!ClaudeCodeRoot(root, ARRAYSIZE(root))) {
        StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The path is too long."));
        return FALSE;
    }
    if ((zip = Zip_Open(archive)) == NULL || !IsSessionArchive(zip)) {
        if (zip || Util_QueryPath(archive, NULL) == PATH_PRESENT)
            StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"This file is not a session archive of " APP_NAME L"."));
        else
            StringCchPrintfW(report->error, ARRAYSIZE(report->error), TR(L"%s could not be read (error %lu)."), archive, GetLastError());
        Zip_Free(zip);
        return FALSE;
    }
    if (!Zip_Validate(archive, NULL, &checked, report->error, ARRAYSIZE(report->error))) {
        Zip_Free(zip);
        return FALSE;
    }
    /* The conversations first: an entry is sent only once they are there; of some entries, only theirs. */
    if (pick && pick->keys) mapped = GatherPickedFiles(zip, pick, &only);
    files = ExtractConversations(zip, root, mapped ? &only : NULL, report);
    if (mapped) FreePickedFiles(&only);
    /* A failed extraction must not leave entries pointing at conversations
     * which never arrived. Successfully extracted files can be reused on retry. */
    if (report->failed) {
        Zip_Free(zip);
        FreeOutbox(&box);
        return FALSE;
    }
    for (i = 0; i < Zip_Count(zip); i++) {
        WCHAR key[SESSION_ID_CCH], ownId[SESSION_ID_CCH];
        DWORD length = 0;
        char *entry;
        if (!EntryItem(zip, i)) continue;
        if ((entry = Keep(&box, (char *)Zip_Read(zip, i, SESSION_ENTRY_MAX_BYTES, &length))) == NULL) {
            StringCchCopyW(report->error, ARRAYSIZE(report->error), TR(L"The archive is damaged."));
            report->failed++;
            continue;
        }
        /* One that names its session as Claude never does would make the whole plan unwritable. */
        if (!EntryKey(entry, length, ownId, ARRAYSIZE(ownId), key, ARRAYSIZE(key))) {
            report->failed++;
            continue;
        }
        if (!Picked(pick, key)) continue;
        (*sessions)++;
        for (t = 0; t < set->profiles.count; t++) {
            int there = (takers & (1u << t)) ? SessionStore_FindEntry(set, t, key, NULL) : -1;
            const char *content;
            size_t contentLength = 0;
            SyncOp op;
            if (!(takers & (1u << t))) continue;
            if (there >= 0 && set->entries[there].pendingRemove) {
                ResultSkipped(set, t, report);
                continue;
            }
            /* In a "no folder" area, the profile's own, as Share and Merge send it. */
            content = ContentFor(&box, set, t, entry, length, &contentLength);
            MakeOp(&op, SYNC_PUT, SYNC_UNDELETE, key, Core_JsonMemberNumber(entry, length, "lastActivityAt"),
                   there >= 0 ? set->entries[there].lastActivity : 0);
            if (!AddSent(&box.sent[t], &op, content, contentLength)) {
                Zip_Free(zip);
                FreeOutbox(&box);
                return OutOfMemory(report);
            }
        }
    }
    Zip_Free(zip);
    Send(&box, set, report);
    FreeOutbox(&box);
    Util_Log(L"%d session(s) imported from %s, %d conversation file(s) added", *sessions, archive, files);
    return TRUE;
}
