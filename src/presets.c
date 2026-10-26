/* Account-scoped session presets. A conflicting preset keeps both versions,
 * under a deterministic imported id, so repeated syncs do not multiply it. */
#include "presets.h"
#include <string.h>
#include <wchar.h>

static BOOL Object(const char *p, size_t n)
{
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\r' || p[i] == '\n' || p[i] == '\t')) i++;
    return i < n && p[i] == '{' && Core_JsonIsValue(p, n);
}
static BOOL Set(char **json, size_t *length, const char *key, const char *value, size_t n)
{
    const char *keys[1] = { key };
    char *raw = Util_CopyBytes(value, n), *next;
    size_t used = 0;
    if (!raw) return FALSE;
    next = Core_JsonSetNested(*json, *length, keys, 1, raw, &used);
    Util_Free(raw);
    if (!next) return FALSE;
    Util_Free(*json); *json = next; *length = used;
    return TRUE;
}
char *Presets_Account(const char *json, size_t length, const char *account, size_t *outLength)
{
    const char *value;
    size_t n;
    *outLength = 0;
    if (!Object(json, length) || !Core_JsonMember(json, length, "byAccount", &value, &n) ||
        !Core_JsonMember(value, n, account, &value, &n) || !Object(value, n)) return NULL;
    *outLength = n;
    return Util_CopyBytes(value, n);
}

typedef struct MergeObject { char **json; size_t *length; BOOL replace, ok; int depth; } MergeObject;
static char *Merge(const char *base, size_t baseLength, const char *other, size_t otherLength, BOOL replace, int depth, size_t *length);
static BOOL MergeMember(void *context, const char *key, size_t keyLength, const char *value, size_t valueLength)
{
    MergeObject *m = (MergeObject *)context;
    char name[512], *merged = NULL;
    const char *old;
    size_t oldLength = 0, n = 0;
    if (FAILED(StringCchCopyNA(name, ARRAYSIZE(name), key, keyLength))) return m->ok = FALSE;
    if (Core_JsonMember(*m->json, *m->length, name, &old, &oldLength)) {
        if (m->depth < 16 && Object(old, oldLength) && Object(value, valueLength)) {
            merged = Merge(old, oldLength, value, valueLength, m->replace, m->depth + 1, &n);
            if (!merged) return m->ok = FALSE;
            value = merged; valueLength = n;
        } else if (!m->replace) return TRUE;
    }
    m->ok = Set(m->json, m->length, name, value, valueLength);
    Util_Free(merged);
    return m->ok;
}
static char *Merge(const char *base, size_t baseLength, const char *other, size_t otherLength, BOOL replace, int depth, size_t *length)
{
    MergeObject m;
    char *out;
    if (!Object(base, baseLength) || !Object(other, otherLength) || !(out = Util_CopyBytes(base, baseLength))) return NULL;
    *length = baseLength;
    m.json = &out; m.length = length; m.replace = replace; m.ok = TRUE; m.depth = depth;
    if (!Core_JsonEachMember(other, otherLength, MergeMember, &m) || !m.ok) { Util_Free(out); return NULL; }
    return out;
}

static void Trim(const char **p, size_t *n)
{
    while (*n && (**p == ' ' || **p == '\r' || **p == '\n' || **p == '\t')) { (*p)++; (*n)--; }
    while (*n && ((*p)[*n - 1] == ' ' || (*p)[*n - 1] == '\r' || (*p)[*n - 1] == '\n' || (*p)[*n - 1] == '\t')) (*n)--;
}
static BOOL Metadata(const char *key, size_t n)
{
    return (n == 2 && memcmp(key, "id", 2) == 0) || (n == 9 && memcmp(key, "createdAt", 9) == 0) ||
           (n == 10 && memcmp(key, "lastUsedAt", 10) == 0);
}
static ULONGLONG SemanticHash(const char *json, size_t n, int depth);
typedef struct SemanticHashState { ULONGLONG hash; int count, depth; BOOL object; } SemanticHashState;
static BOOL HashMember(void *context, const char *key, size_t n, const char *value, size_t valueLength)
{
    SemanticHashState *h = (SemanticHashState *)context;
    ULONGLONG child;
    if (h->depth == 0 && Metadata(key, n)) return TRUE;
    child = SemanticHash(value, valueLength, h->depth + 1);
    h->hash += Core_HashBytes(Core_HashBytes(CORE_HASH_START, key, n), &child, sizeof child); h->count++;
    return TRUE;
}
static BOOL HashElement(void *context, const char *value, size_t length)
{
    SemanticHashState *h = (SemanticHashState *)context;
    ULONGLONG child = SemanticHash(value, length, h->depth + 1);
    h->hash = Core_HashBytes(h->hash, &child, sizeof child); h->count++; return TRUE;
}
static ULONGLONG SemanticHash(const char *json, size_t n, int depth)
{
    SemanticHashState h;
    Trim(&json, &n);
    h.hash = CORE_HASH_START; h.count = 0; h.depth = depth; h.object = n && json[0] == '{';
    if (depth < 32 && n && json[0] == '{' && Core_JsonEachMember(json, n, HashMember, &h))
        return Core_HashBytes(h.hash, &h.count, sizeof h.count);
    if (depth < 32 && n && json[0] == '[' && Core_JsonEachElement(json, n, HashElement, &h))
        return Core_HashBytes(h.hash, &h.count, sizeof h.count);
    if (n && json[0] == '"') {
        WCHAR *text = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
        if (text && Core_JsonString(json, n, text, n + 1)) h.hash = Core_HashBytes(CORE_HASH_START, text, wcslen(text) * sizeof(WCHAR));
        else h.hash = Core_HashBytes(CORE_HASH_START, json, n);
        Util_Free(text); return h.hash;
    }
    return Core_HashBytes(CORE_HASH_START, json, n);
}
static BOOL SemanticEqual(const char *a, size_t an, const char *b, size_t bn, int depth);
typedef struct EqualState { const char *other; size_t length; int count, depth, at; BOOL equal; } EqualState;
static BOOL EqualMember(void *context, const char *key, size_t n, const char *value, size_t length)
{
    EqualState *e = (EqualState *)context;
    char name[512]; const char *other; size_t otherLength;
    if (e->depth == 0 && Metadata(key, n)) return TRUE;
    e->count++;
    if (FAILED(StringCchCopyNA(name, sizeof name, key, n)) || !Core_JsonMember(e->other, e->length, name, &other, &otherLength) ||
        !SemanticEqual(value, length, other, otherLength, e->depth + 1)) return e->equal = FALSE;
    return TRUE;
}
static BOOL AtElement(void *context, const char *value, size_t length)
{
    EqualState *e = (EqualState *)context;
    if (e->count++ == e->at) { e->other = value; e->length = length; }
    return TRUE;
}
static BOOL EqualElement(void *context, const char *value, size_t length)
{
    EqualState *e = (EqualState *)context, at;
    ZeroMemory(&at, sizeof at); at.at = e->count++;
    if (!Core_JsonEachElement(e->other, e->length, AtElement, &at) || !at.other ||
        !SemanticEqual(value, length, at.other, at.length, e->depth + 1)) return e->equal = FALSE;
    return TRUE;
}
static BOOL SemanticEqual(const char *a, size_t an, const char *b, size_t bn, int depth)
{
    EqualState x, y;
    Trim(&a, &an); Trim(&b, &bn);
    if (depth > 32 || !an || !bn || a[0] != b[0]) return FALSE;
    ZeroMemory(&x, sizeof x); ZeroMemory(&y, sizeof y); x.other = b; x.length = bn; x.depth = depth; x.equal = TRUE;
    y.other = a; y.length = an; y.depth = depth; y.equal = TRUE;
    if (a[0] == '{') return Core_JsonEachMember(a, an, EqualMember, &x) && x.equal &&
        Core_JsonEachMember(b, bn, EqualMember, &y) && y.equal && x.count == y.count;
    if (a[0] == '[') {
        if (!Core_JsonEachElement(a, an, EqualElement, &x) || !x.equal) return FALSE;
        y.at = -1;
        return Core_JsonEachElement(b, bn, AtElement, &y) && x.count == y.count;
    }
    if (a[0] == '"') {
        WCHAR *aw = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (an + 1) * sizeof(WCHAR));
        WCHAR *bw = (WCHAR *)HeapAlloc(GetProcessHeap(), 0, (bn + 1) * sizeof(WCHAR));
        BOOL equal = aw && bw && Core_JsonString(a, an, aw, an + 1) && Core_JsonString(b, bn, bw, bn + 1) && wcscmp(aw, bw) == 0;
        Util_Free(aw); Util_Free(bw); return equal;
    }
    return an == bn && memcmp(a, b, an) == 0;
}

typedef struct FindPreset { WCHAR id[256]; const char *value; size_t length; BOOL valid; } FindPreset;
static BOOL Find(void *context, const char *value, size_t length)
{
    FindPreset *find = (FindPreset *)context;
    const char *raw;
    size_t n;
    WCHAR id[256];
    if (!Object(value, length) || !Core_JsonMember(value, length, "id", &raw, &n) || !Core_JsonString(raw, n, id, ARRAYSIZE(id)) || !id[0]) {
        find->valid = FALSE; return FALSE;
    }
    if (wcscmp(id, find->id) == 0) { find->value = value; find->length = length; }
    return TRUE;
}
typedef struct MergePresets { char *array; size_t length; BOOL ok; } MergePresets;
static BOOL RefreshMetadata(MergePresets *m, const FindPreset *find, const char *incoming, size_t incomingLength)
{
    static const char *const times[] = { "createdAt", "lastUsedAt" };
    size_t n = find->length, i, rawLength;
    char *changed = Util_CopyBytes(find->value, find->length), *next;
    const char *raw;
    if (!changed) return FALSE;
    for (i = 0; i < ARRAYSIZE(times); i++) {
        ULONGLONG old = 0, fresh = 0;
        if (!Core_JsonMember(incoming, incomingLength, times[i], &raw, &rawLength) || !Core_JsonNumber(raw, rawLength, &fresh)) continue;
        if (Core_JsonMember(find->value, find->length, times[i], &raw, &rawLength)) Core_JsonNumber(raw, rawLength, &old);
        if (!old || (i == 0 ? fresh < old : fresh > old)) {
            char number[32]; StringCchPrintfA(number, sizeof number, "%I64u", fresh);
            if (!Set(&changed, &n, times[i], number, strlen(number))) { Util_Free(changed); return FALSE; }
        }
    }
    next = (char *)HeapAlloc(GetProcessHeap(), 0, m->length - find->length + n + 1);
    if (!next) { Util_Free(changed); return FALSE; }
    i = (size_t)(find->value - m->array);
    memcpy(next, m->array, i); memcpy(next + i, changed, n);
    memcpy(next + i + n, find->value + find->length, m->length - i - find->length);
    m->length = m->length - find->length + n; next[m->length] = 0;
    Util_Free(m->array); m->array = next; Util_Free(changed); return TRUE;
}
static BOOL AddPreset(void *context, const char *value, size_t length)
{
    MergePresets *m = (MergePresets *)context;
    FindPreset find;
    const char *raw;
    size_t n, changedLength = 0;
    char *changed = NULL, *next;
    WCHAR original[256];
    char quoted[1024];
    int collision;
    ZeroMemory(&find, sizeof find); find.valid = TRUE;
    if (!Object(value, length) || !Core_JsonMember(value, length, "id", &raw, &n) || !Core_JsonString(raw, n, original, ARRAYSIZE(original)) || !original[0])
        return m->ok = FALSE;
    StringCchCopyW(find.id, ARRAYSIZE(find.id), original);
    if (!Core_JsonEachElement(m->array, m->length, Find, &find) || !find.valid) return m->ok = FALSE;
    if (find.value && SemanticEqual(find.value, find.length, value, length, 0)) return m->ok = RefreshMetadata(m, &find, value, length);
    if (find.value) {
        ULONGLONG hash = SemanticHash(value, length, 0);
        for (collision = 0; collision < 1024; collision++) {
            if (collision == 0) StringCchPrintfW(find.id, ARRAYSIZE(find.id), L"cdm-%016I64x", hash);
            else StringCchPrintfW(find.id, ARRAYSIZE(find.id), L"cdm-%016I64x-%d", hash, collision);
            find.value = NULL;
            if (!Core_JsonEachElement(m->array, m->length, Find, &find) || !find.valid) return m->ok = FALSE;
            if (!find.value) break;
            if (SemanticEqual(find.value, find.length, value, length, 0)) return m->ok = RefreshMetadata(m, &find, value, length);
        }
        if (collision == 1024 || !Core_JsonQuote(find.id, quoted, sizeof quoted)) return m->ok = FALSE;
        changed = Util_CopyBytes(value, length); changedLength = length;
        if (!changed || !Set(&changed, &changedLength, "id", quoted, strlen(quoted))) { Util_Free(changed); return m->ok = FALSE; }
        value = changed; length = changedLength;
    }
    next = (char *)HeapAlloc(GetProcessHeap(), 0, m->length + length + 2);
    if (!next) { Util_Free(changed); return m->ok = FALSE; }
    n = m->length;
    if (!n || m->array[n - 1] != ']') { Util_Free(next); Util_Free(changed); return m->ok = FALSE; }
    memcpy(next, m->array, n - 1); n--;
    if (n > 1) next[n++] = ',';
    memcpy(next + n, value, length); n += length;
    next[n++] = ']'; next[n] = 0;
    Util_Free(m->array); m->array = next; m->length = n;
    Util_Free(changed);
    return TRUE;
}
char *Presets_MergeAccount(const char *old, size_t oldLength, const char *incoming, size_t incomingLength, size_t *outLength)
{
    const char *a, *b, *built;
    size_t aLength, bLength, builtLength, n = 0;
    char *merged, *builtMerge = NULL;
    MergePresets m;
    *outLength = 0;
    if (!Object(incoming, incomingLength)) return NULL;
    /* Unknown target members win; unknown source members are retained when new. */
    merged = Merge(old, oldLength, incoming, incomingLength, FALSE, 0, outLength);
    if (!merged) return NULL;
    if (Core_JsonMember(incoming, incomingLength, "builtIns", &built, &builtLength)) {
        a = "{}"; aLength = 2;
        Core_JsonMember(old, oldLength, "builtIns", &a, &aLength);
        builtMerge = Merge(a, aLength, built, builtLength, TRUE, 0, &n);
        if (!builtMerge || !Set(&merged, outLength, "builtIns", builtMerge, n)) goto failed;
    }
    a = "[]"; aLength = 2; b = "[]"; bLength = 2;
    Core_JsonMember(old, oldLength, "presets", &a, &aLength);
    Core_JsonMember(incoming, incomingLength, "presets", &b, &bLength);
    m.array = Util_CopyBytes("[]", 2); m.length = 2; m.ok = m.array != NULL;
    if (!m.ok || !Core_JsonEachElement(a, aLength, AddPreset, &m) || !m.ok ||
        !Core_JsonEachElement(b, bLength, AddPreset, &m) || !m.ok || !Set(&merged, outLength, "presets", m.array, m.length)) {
        Util_Free(m.array); goto failed;
    }
    Util_Free(m.array);
    Util_Free(builtMerge);
    return merged;
failed:
    Util_Free(builtMerge); Util_Free(merged);
    *outLength = 0;
    return NULL;
}
char *Presets_Merge(const char *json, size_t length, const char *account, const char *incoming, size_t incomingLength, size_t *outLength)
{
    const char *accounts, *old;
    size_t accountsLength, oldLength, mergedLength = 0;
    char *merged, *root;
    const char *keys[2] = { "byAccount", account };
    *outLength = 0;
    if (!Object(json, length) || !Object(incoming, incomingLength) || !account || !account[0]) return NULL;
    old = "{}"; oldLength = 2;
    if (Core_JsonMember(json, length, "byAccount", &accounts, &accountsLength)) {
        if (!Object(accounts, accountsLength)) return NULL;
        Core_JsonMember(accounts, accountsLength, account, &old, &oldLength);
    }
    if ((merged = Presets_MergeAccount(old, oldLength, incoming, incomingLength, &mergedLength)) == NULL) return NULL;
    root = Core_JsonSetNested(json, length, keys, 2, merged, outLength);
    Util_Free(merged);
    return root;
}
