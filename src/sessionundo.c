/*
 * What the manager changed in sessions, undone and redone (Ctrl+Z, Ctrl+Y in
 * the sessions view). An action, one menu choice or one key, is a list of
 * steps, each about one session in one profile: its star, its title, read or
 * unread, or its entry (listed or not, and as what). A step keeps both sides, so undoing
 * goes back to the first and redoing to the second whatever came between:
 * stars, titles and unread marks through sessionedit.c, entries through sessionsync.c,
 * the only writer of new entries, which also waits for a running profile to
 * close. The lists live as long as the manager.
 */
#include "app.h"

#define UNDO_DEPTH 50   /* actions kept each way */

typedef enum UndoKind { UNDO_STAR, UNDO_TITLE, UNDO_ENTRY, UNDO_READ } UndoKind;

typedef struct UndoStep {
    UndoKind kind;
    WCHAR    folder[FOLDER_CCH];             /* the profile's */
    WCHAR    key[SESSION_ID_CCH];            /* the session's */
    BOOL     starred[2];                     /* star: before, after */
    BOOL     unread[2];                      /* unread: before, after */
    WCHAR    title[2][SESSION_TITLE_CCH];    /* title: before, after; "" for none */
    BOOL     autoTitle[2];                   /* ... given by Claude itself */
    BOOL     listed[2];                      /* entry: listed before, after */
    char    *entry[2];                       /* ... as what; NULL where it is not listed or could not be read */
    size_t   length[2];
} UndoStep;

typedef struct UndoAction {
    UndoStep *steps;
    int       count, capacity;
} UndoAction;

typedef struct UndoList {
    UndoAction actions[UNDO_DEPTH];          /* the latest last */
    int        count;
} UndoList;

static UndoList          g_done, g_undone;
static UndoAction        g_recording;
static int               g_depth;
static BOOL              g_lost;             /* a step of the action could not be kept */
static const SessionSet *g_recordSet;

static void FreeAction(UndoAction *action)
{
    int i;
    for (i = 0; i < action->count; i++) {
        if (action->steps[i].entry[0]) HeapFree(GetProcessHeap(), 0, action->steps[i].entry[0]);
        if (action->steps[i].entry[1]) HeapFree(GetProcessHeap(), 0, action->steps[i].entry[1]);
    }
    if (action->steps) HeapFree(GetProcessHeap(), 0, action->steps);
    ZeroMemory(action, sizeof *action);
}

static void ClearList(UndoList *list)
{
    int i;
    for (i = 0; i < list->count; i++) FreeAction(&list->actions[i]);
    list->count = 0;
}

/* `action` the latest of `list`, which owns it from now on; the oldest goes when it is full. */
static void Push(UndoList *list, UndoAction *action)
{
    if (list->count == UNDO_DEPTH) {
        FreeAction(&list->actions[0]);
        MoveMemory(&list->actions[0], &list->actions[1], (UNDO_DEPTH - 1) * sizeof list->actions[0]);
        list->count--;
    }
    list->actions[list->count++] = *action;
    ZeroMemory(action, sizeof *action);
}

static char *ReadEntry(const WCHAR *file, size_t *length)
{
    DWORD read = 0;
    char *data = Util_ReadFile(file, SESSION_ENTRY_MAX_BYTES, FALSE, &read);
    *length = data ? read : 0;
    return data;
}

/* A new step of the action recorded; NULL outside one, or without memory (the action is then lost). */
static UndoStep *NewStep(UndoKind kind, const Profile *p, const WCHAR *key)
{
    UndoStep *step;
    if (!g_depth) return NULL;
    if (g_recording.count == g_recording.capacity) {
        int capacity = g_recording.capacity ? g_recording.capacity * 2 : 8;
        UndoStep *grown = g_recording.steps
                          ? (UndoStep *)HeapReAlloc(GetProcessHeap(), 0, g_recording.steps, (size_t)capacity * sizeof *grown)
                          : (UndoStep *)HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof *grown);
        if (!grown) {
            g_lost = TRUE;
            return NULL;
        }
        g_recording.steps = grown;
        g_recording.capacity = capacity;
    }
    step = &g_recording.steps[g_recording.count++];
    ZeroMemory(step, sizeof *step);
    step->kind = kind;
    StringCchCopyW(step->folder, ARRAYSIZE(step->folder), p->folder);
    StringCchCopyW(step->key, ARRAYSIZE(step->key), key);
    return step;
}

/* An entry sessionsync.c sends while an action is recorded: what the
 * profile listed for it before, from the sessions the action began with. */
static void Sent(void *context, const Profile *p, const SyncOp *op, const char *content, size_t length)
{
    const SessionSet *set = g_recordSet;
    int index = set ? Profiles_Find(&set->profiles, p->folder) : -1, found = -1;
    BOOL listed;
    UndoStep *step;
    (void)context;
    if (index >= 0) found = SessionStore_FindEntry(set, index, op->key, NULL);
    listed = found >= 0 && !set->entries[found].pendingRemove;
    if (op->kind == SYNC_REMOVE && !listed) return;   /* nothing listed, nothing taken away */
    if ((step = NewStep(UNDO_ENTRY, p, op->key)) == NULL) return;
    step->listed[0] = listed;
    step->listed[1] = op->kind == SYNC_PUT;
    if (listed) step->entry[0] = ReadEntry(set->entries[found].file, &step->length[0]);
    if (op->kind == SYNC_PUT) {
        step->entry[1] = content ? Util_CopyBytes(content, length) : NULL;
        step->length[1] = step->entry[1] ? length : 0;
    }
}

void SessionUndo_Begin(const SessionSet *set)
{
    if (g_depth++) return;
    FreeAction(&g_recording);
    g_lost = FALSE;
    g_recordSet = set;
    SessionSync_OnEachSent(Sent, NULL);
}

void SessionUndo_End(void)
{
    if (!g_depth || --g_depth) return;
    SessionSync_OnEachSent(NULL, NULL);
    g_recordSet = NULL;
    if (g_lost) {
        /* Not all of it kept: undoing it, or what came before it, would not give back what was. */
        Util_Log(L"session undo: an action could not be kept, earlier ones are forgotten");
        FreeAction(&g_recording);
        ClearList(&g_done);
        ClearList(&g_undone);
        return;
    }
    if (!g_recording.count) return;
    Push(&g_done, &g_recording);
    ClearList(&g_undone);
}

void SessionUndo_Star(const Profile *p, const WCHAR *key, BOOL before, BOOL after)
{
    UndoStep *step;
    if (!before == !after || (step = NewStep(UNDO_STAR, p, key)) == NULL) return;
    step->starred[0] = before;
    step->starred[1] = after;
}

void SessionUndo_Read(const Profile *p, const WCHAR *key, BOOL unreadBefore, BOOL unreadAfter)
{
    UndoStep *step;
    if (!unreadBefore == !unreadAfter || (step = NewStep(UNDO_READ, p, key)) == NULL) return;
    step->unread[0] = unreadBefore;
    step->unread[1] = unreadAfter;
}

void SessionUndo_Title(const Profile *p, const WCHAR *key, const WCHAR *before, BOOL beforeAuto, const WCHAR *after)
{
    UndoStep *step;
    if ((step = NewStep(UNDO_TITLE, p, key)) == NULL) return;
    StringCchCopyW(step->title[0], ARRAYSIZE(step->title[0]), before);
    StringCchCopyW(step->title[1], ARRAYSIZE(step->title[1]), after);
    step->autoTitle[0] = beforeAuto || !before[0];   /* untitled: Claude names it */
}

void SessionUndo_Listed(const Profile *p, const WCHAR *key, BOOL listed, char *content, size_t length)
{
    UndoStep *step = NewStep(UNDO_ENTRY, p, key);
    if (!step) {
        if (content) HeapFree(GetProcessHeap(), 0, content);
        return;
    }
    step->listed[0] = !listed;
    step->listed[1] = listed;
    step->entry[listed ? 1 : 0] = content;
    step->length[listed ? 1 : 0] = content ? length : 0;
}

void SessionUndo_Replaced(const Profile *p, const WCHAR *key, char *before, size_t beforeLength, char *after, size_t afterLength)
{
    UndoStep *step = NewStep(UNDO_ENTRY, p, key);
    if (!step) {
        if (before) HeapFree(GetProcessHeap(), 0, before);
        if (after) HeapFree(GetProcessHeap(), 0, after);
        return;
    }
    step->listed[0] = step->listed[1] = TRUE;   /* listed throughout: only its content changed */
    step->entry[0] = before;
    step->length[0] = before ? beforeLength : 0;
    step->entry[1] = after;
    step->length[1] = after ? afterLength : 0;
}

BOOL SessionUndo_CanUndo(void)
{
    return g_done.count > 0;
}

BOOL SessionUndo_CanRedo(void)
{
    return g_undone.count > 0;
}

static BOOL NamesSession(const UndoAction *action, const WCHAR *key)
{
    int i;
    for (i = 0; i < action->count; i++)
        if (Core_EqualsI(action->steps[i].key, key)) return TRUE;
    return FALSE;
}

static void ForgetIn(UndoList *list, const WCHAR *key)
{
    int i, kept = 0;
    for (i = 0; i < list->count; i++) {
        if (NamesSession(&list->actions[i], key)) FreeAction(&list->actions[i]);
        else list->actions[kept++] = list->actions[i];
    }
    list->count = kept;
}

void SessionUndo_Forget(const WCHAR *key)
{
    ForgetIn(&g_done, key);
    ForgetIn(&g_undone, key);
}

void SessionUndo_Clear(void)
{
    ClearList(&g_done);
    ClearList(&g_undone);
    FreeAction(&g_recording);
}

/* ------------------------------------------------------------ applying */

typedef struct Applying {
    HWND              owner;
    const SessionSet *set;
    SessionSet       *now[MAX_PROFILES];     /* each profile's entries as they are, read once (SessionStore_LoadEntries) */
    SyncSend         *sends;                 /* the entries sent, all at once at the end */
    int               sendCount, sendCapacity;
    BOOL              ok;
    WCHAR            *error;
    size_t            errorCch;
} Applying;

static void Fail(Applying *a, const WCHAR *why)
{
    a->ok = FALSE;
    if (a->errorCch && !a->error[0] && why && why[0]) StringCchCopyW(a->error, a->errorCch, why);
}

static const SessionSet *EntriesNow(Applying *a, int p)
{
    if (!a->now[p]) {
        if ((a->now[p] = (SessionSet *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *a->now[p])) == NULL) return NULL;
        SessionStore_LoadEntries(a->now[p], &a->set->profiles.items[p]);   /* what could be read */
    }
    return a->now[p];
}

static ULONGLONG Activity(const char *json, size_t length)
{
    const char *value;
    size_t valueLength;
    ULONGLONG time = 0;
    if (json && Core_JsonMember(json, length, "lastActivityAt", &value, &valueLength)) Core_JsonNumber(value, valueLength, &time);
    return time;
}

/* A change of kind `kind` to session `key` waiting in the plan of `p`. */
static BOOL Queued(const Profile *p, SyncOpKind kind, const WCHAR *key)
{
    SyncOp *ops = NULL;
    int count = SessionSync_Queued(p, &ops), i;
    BOOL found = FALSE;
    for (i = 0; i < count && !found; i++) found = ops[i].kind == kind && Core_EqualsI(ops[i].key, key);
    if (ops) HeapFree(GetProcessHeap(), 0, ops);
    return found;
}

/* `content` kept by the step owning it, sent once every step is through. */
static void Send(Applying *a, int p, SyncOpKind kind, DWORD flags, const WCHAR *key, ULONGLONG time, ULONGLONG seen,
                 const char *content, size_t length)
{
    SyncSend *change;
    if (a->sendCount == a->sendCapacity) {
        int capacity = a->sendCapacity ? a->sendCapacity * 2 : 16;
        SyncSend *grown = a->sends ? (SyncSend *)HeapReAlloc(GetProcessHeap(), 0, a->sends, (size_t)capacity * sizeof *grown)
                                   : (SyncSend *)HeapAlloc(GetProcessHeap(), 0, (size_t)capacity * sizeof *grown);
        if (!grown) {
            Fail(a, L"");
            return;
        }
        a->sends = grown;
        a->sendCapacity = capacity;
    }
    change = &a->sends[a->sendCount++];
    ZeroMemory(change, sizeof *change);
    change->profile = p;
    change->op.kind = kind;
    change->op.flags = flags;
    change->op.time = time;
    change->op.seen = seen;
    StringCchCopyW(change->op.key, ARRAYSIZE(change->op.key), key);
    change->content = content;
    change->length = length;
}

static void Change(Applying *a, const Profile *p, const SessionEntry *entry, PendingOp op, const WCHAR *key, const WCHAR *value)
{
    PendingEdit edit;
    WCHAR reason[LONG_PATH_CCH];
    BOOL waiting;
    ZeroMemory(&edit, sizeof edit);
    edit.op = op;
    StringCchCopyW(edit.key, ARRAYSIZE(edit.key), key);
    StringCchCopyW(edit.value, ARRAYSIZE(edit.value), value);
    if (!SessionEdit_Change(a->owner, p, entry, &edit, &waiting, reason, ARRAYSIZE(reason))) Fail(a, reason);
}

/* The removal of session `row` of `now` (one profile's entries) waiting for that profile to close, dropped. */
static void CancelRemoval(Applying *a, const Profile *p, const SessionSet *now, int row)
{
    PendingEdit keys[MAX_PROFILES + 1];
    int entry, count = 0;
    ZeroMemory(keys, sizeof keys);
    keys[count].op = PENDING_REMOVE;
    StringCchCopyW(keys[count++].key, ARRAYSIZE(keys->key), now->rows[row].key);
    for (entry = now->rows[row].entry[0]; entry >= 0 && count < (int)ARRAYSIZE(keys); entry = now->entries[entry].duplicate) {
        keys[count].op = PENDING_REMOVE;
        StringCchCopyW(keys[count++].key, ARRAYSIZE(keys->key), now->entries[entry].localId);
    }
    if (!SessionEdit_Cancel(p, keys, count)) Fail(a, L"");
}

/* Step `step` brought to its side `side` (0: as before the action, 1: as after). */
static void ApplyStep(Applying *a, UndoStep *step, int side)
{
    const Profile *profile;
    const SessionSet *now;
    const SessionEntry *entry;
    int p = Profiles_Find(&a->set->profiles, step->folder), found, row = -1;
    if (p < 0) {
        Fail(a, TR(L"The profile is gone."));
        return;
    }
    profile = &a->set->profiles.items[p];
    if ((now = EntriesNow(a, p)) == NULL) {
        Fail(a, L"");
        return;
    }
    found = SessionStore_FindEntry(now, 0, step->key, &row);
    entry = found >= 0 ? &now->entries[found] : NULL;
    if (step->kind != UNDO_ENTRY && !entry) {
        Fail(a, TR(L"A session is no longer listed there."));
        return;
    }
    switch (step->kind) {
    case UNDO_STAR:
        Change(a, profile, entry, PENDING_STAR, step->key, step->starred[side] ? L"1" : L"0");
        break;
    case UNDO_READ:
        Change(a, profile, entry, PENDING_READ, step->key, step->unread[side] ? L"0" : L"1");
        break;
    case UNDO_TITLE:
        Change(a, profile, entry, step->autoTitle[side] ? PENDING_TITLE_AUTO : PENDING_TITLE, step->key, step->title[side]);
        break;
    case UNDO_ENTRY:
        if (!step->listed[side]) {
            if (!entry) {
                /* Not there, unless it waits in the plan of a running profile: a removal takes its place. */
                if (Queued(profile, SYNC_PUT, step->key)) Send(a, p, SYNC_REMOVE, SYNC_MARKED, step->key, 0, 0, NULL, 0);
                break;
            }
            /* Taken away as it is now, which is what goes back. */
            if (step->entry[!side]) HeapFree(GetProcessHeap(), 0, step->entry[!side]);
            step->entry[!side] = ReadEntry(entry->file, &step->length[!side]);
            Send(a, p, SYNC_REMOVE, SYNC_MARKED, step->key, 0, entry->lastActivity, NULL, 0);
        } else if (entry && !step->listed[!side]) {
            /* Listed still: what would take it away stops. */
            CancelRemoval(a, profile, now, row);
            if (Queued(profile, SYNC_REMOVE, step->key)) {
                if (step->entry[side]) HeapFree(GetProcessHeap(), 0, step->entry[side]);
                if ((step->entry[side] = ReadEntry(entry->file, &step->length[side])) != NULL)
                    Send(a, p, SYNC_PUT, SYNC_REPLACE, step->key, entry->lastActivity, entry->lastActivity, step->entry[side],
                         step->length[side]);
                else Fail(a, L"");
            }
        } else if (!step->entry[side]) {
            Fail(a, TR(L"A session's entry could not be read when it changed."));
        } else {
            Send(a, p, SYNC_PUT, SYNC_UNDELETE | (entry ? SYNC_REPLACE : 0), step->key, Activity(step->entry[side], step->length[side]),
                 entry ? entry->lastActivity : 0, step->entry[side], step->length[side]);
        }
        break;
    }
}

/* The latest action of `from` brought to `side`, then the latest of `to`. */
static BOOL Apply(UndoList *from, UndoList *to, int side, HWND owner, const SessionSet *set, WCHAR *error, size_t errorCch)
{
    Applying a;
    UndoAction action;
    SyncReport report;
    int i, p;
    if (errorCch) error[0] = 0;
    if (!from->count || g_depth) return FALSE;
    action = from->actions[--from->count];
    ZeroMemory(&a, sizeof a);
    a.owner = owner;
    a.set = set;
    a.ok = TRUE;
    a.error = error;
    a.errorCch = errorCch;
    /* Undone from the last step back; made again from the first. */
    for (i = 0; i < action.count; i++) ApplyStep(&a, &action.steps[side ? i : action.count - 1 - i], side);
    if (a.sendCount) {
        ZeroMemory(&report, sizeof report);
        if (!SessionSync_Send(set, a.sends, a.sendCount, &report) || report.failed) Fail(&a, report.error);
        if (report.skipped) Fail(&a, TR(L"Sessions used since then in a profile stay there as they are."));
        if (report.unavailable) Fail(&a, TR(L"A profile is not signed in to Claude any more."));
    }
    Util_Log(L"session undo: %d change(s) %s%s", action.count, side ? L"made again" : L"undone", a.ok ? L"" : L", not all of them");
    for (p = 0; p < MAX_PROFILES; p++) {
        if (!a.now[p]) continue;
        SessionStore_Free(a.now[p]);
        HeapFree(GetProcessHeap(), 0, a.now[p]);
    }
    if (a.sends) HeapFree(GetProcessHeap(), 0, a.sends);
    Push(to, &action);
    return a.ok;
}

BOOL SessionUndo_Undo(HWND owner, const SessionSet *set, WCHAR *error, size_t errorCch)
{
    return Apply(&g_done, &g_undone, 0, owner, set, error, errorCch);
}

BOOL SessionUndo_Redo(HWND owner, const SessionSet *set, WCHAR *error, size_t errorCch)
{
    return Apply(&g_undone, &g_done, 1, owner, set, error, errorCch);
}
