#include "../src/presets.h"
#include <stdio.h>
#include <string.h>
static int checks, failed;
static void Check(const char *name, BOOL ok) { checks++; if (!ok) { failed++; printf("FAIL %s\n", name); } }
static void Free(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }
static BOOL Count(void *context, const char *value, size_t length) { (void)value; (void)length; (*(int *)context)++; return TRUE; }
static int PresetCount(const char *json, size_t length, const char *account)
{
    size_t n = 0, arrayLength = 0; int count = 0; const char *array;
    char *own = json ? Presets_Account(json, length, account, &n) : NULL;
    if (!own || !Core_JsonMember(own, n, "presets", &array, &arrayLength) || !Core_JsonEachElement(array, arrayLength, Count, &count)) count = -1;
    Free(own); return count;
}
int main(void)
{
    static const char base[] = "{\"futureRoot\":1,\"byAccount\":{\"target\":{\"future\":{\"keep\":true},\"presets\":[{\"id\":\"p1\",\"name\":\"A\",\"target\":{\"kind\":\"local\",\"cwd\":\"C:\\\\Work\"},\"createdAt\":100,\"lastUsedAt\":200}],\"builtIns\":{\"local\":{\"future\":5,\"settings\":{\"effort\":\"high\"}}}},\"other\":{\"presets\":[{\"id\":\"other\"}]}}}";
    static const char newer[] = "{\"presets\":[{\"target\":{\"cwd\":\"C:\\\\Work\",\"kind\":\"local\"},\"lastUsedAt\":400,\"createdAt\":50,\"name\":\"A\",\"id\":\"p1\"}],\"builtIns\":{\"local\":{\"settings\":{\"modelId\":\"model\",\"effort\":\"medium\"}}}}";
    static const char different[] = "{\"presets\":[{\"id\":\"p1\",\"name\":\"Different\",\"target\":{\"kind\":\"local\",\"cwd\":\"D:\\\\Other\"},\"lastUsedAt\":500}]}";
    char *out, *again, *source, *back; size_t n = 0, m = 0, k = 0, backLength = 0;
    out = Presets_Merge(base, sizeof base - 1, "target", newer, sizeof newer - 1, &n);
    Check("metadata and object key order never create another preset", out && PresetCount(out, n, "target") == 1);
    Check("last use advances and creation keeps oldest time", out && strstr(out, "\"lastUsedAt\":400") && strstr(out, "\"createdAt\":50"));
    Check("other accounts and future fields remain", out && strstr(out, "\"other\":{\"presets\":[{\"id\":\"other\"}]") && strstr(out, "\"futureRoot\":1") && strstr(out, "\"keep\":true") && strstr(out, "\"future\":5"));
    Check("built in nested settings merge", out && strstr(out, "\"effort\":\"medium\"") && strstr(out, "\"modelId\":\"model\""));
    again = out ? Presets_Merge(out, n, "target", different, sizeof different - 1, &m) : NULL;
    Check("genuine configuration conflicts keep both", again && PresetCount(again, m, "target") == 2 && strstr(again, "cdm-"));
    Free(out); out = again; n = m;
    again = out ? Presets_Merge(out, n, "target", different, sizeof different - 1, &m) : NULL;
    Check("reimport of a conflicting preset is idempotent", again && PresetCount(again, m, "target") == 2);
    {
        /* A sync sends a group's presets; the account keeps its own beside them, so what it holds is not what was sent. */
        LayoutPart presets = { "sessionPresets" }, zoom = { "pageZoom" }, other = { "appLocale" };
        size_t ownLength = 0, mergedLength = 0;
        char *own = again ? Presets_Account(again, m, "target", &ownLength) : NULL, *merged;
        zoom.ownOnly = TRUE;
        presets.mergeOnly = TRUE;
        {
            size_t unionLength = 0;
            char *both = own ? SessionSync_MergePart(&presets, own, ownLength, different, sizeof different - 1, &unionLength) : NULL;
            Check("presets merge: a part merged keeps what each has", both && unionLength >= ownLength && strstr(both, "\"Different\""));
            Check("presets merge: no other part merges", !SessionSync_MergePart(&zoom, "{}", 2, "{}", 2, &unionLength) && !unionLength);
            Free(both);
        }
        merged = own ? Presets_MergeAccount(own, ownLength, different, sizeof different - 1, &mergedLength) : NULL;
        Check("an account's merge with presets it already took leaves it as it is",
              merged && mergedLength == ownLength && memcmp(merged, own, ownLength) == 0);
        Check("... so sending them again is no change", own && SessionSync_PartHolds(&presets, NULL, 0, own, ownLength, different, sizeof different - 1));
        static const char added[] = "{\"presets\":[{\"id\":\"p9\",\"name\":\"New\",\"target\":{\"kind\":\"local\",\"cwd\":\"E:\\\\New\"}}]}";
        Check("... unlike presets it does not have", own && !SessionSync_PartHolds(&presets, NULL, 0, own, ownLength, added, sizeof added - 1) &&
              !SessionSync_PartHolds(&presets, NULL, 0, NULL, 0, added, sizeof added - 1));
        Check("a value only the profile's own can take is no change where it has none",
              SessionSync_PartHolds(&zoom, NULL, 0, NULL, 0, "{\"a\":1}", 7) && !SessionSync_PartHolds(&zoom, NULL, 0, "{\"a\":2}", 7, "{\"a\":1}", 7));
        Check("any other part differing is a change", !SessionSync_PartHolds(&other, NULL, 0, "\"en-US\"", 7, "\"zh-CN\"", 7) &&
              !SessionSync_PartHolds(&other, NULL, 0, NULL, 0, "\"zh-CN\"", 7));
        {
            static const char cannot[] = "{\"cannotTake\":{\"appLocale\":1}}", others[] = "{\"cannotTake\":{\"pageZoom\":1}}";
            Check("a part the profile cannot take (a value Claude has not made there) is no change where it has none",
                  SessionSync_PartHolds(&other, cannot, sizeof cannot - 1, NULL, 0, "\"zh-CN\"", 7) &&
                  !SessionSync_PartHolds(&other, others, sizeof others - 1, NULL, 0, "\"zh-CN\"", 7) &&
                  !SessionSync_PartHolds(&other, cannot, sizeof cannot - 1, "\"en-US\"", 7, "\"zh-CN\"", 7));
        }
        Free(merged); Free(own);
    }
    source = again ? Presets_Account(again, m, "target", &k) : NULL;
    back = source ? Presets_Merge("{}", 2, "source", source, k, &backLength) : NULL;
    Check("account mapping does not copy source account IDs", back && PresetCount(back, backLength, "source") == 2 && !strstr(back, "\"target\":{\"presets\""));
    Free(source); source = back ? Presets_Account(back, backLength, "source", &k) : NULL;
    Free(back); back = source && again ? Presets_Merge(again, m, "target", source, k, &backLength) : NULL;
    Check("two way synchronization stays at two presets", back && PresetCount(back, backLength, "target") == 2);
    Free(source); Free(back); Free(again); Free(out);
    out = Presets_Merge("{}", 2, "target", "{\"presets\":{}}", 14, &n);
    Check("malformed preset array fails", !out); Free(out);
    out = Presets_Merge("{\"byAccount\":[]}", 16, "target", "{}", 2, &n);
    Check("invalid account map is not overwritten", !out); Free(out);
    printf("Preset tests: %d checks, %d failure(s).\n", checks, failed);
    return failed ? 1 : 0;
}
