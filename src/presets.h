#ifndef CDM_PRESETS_H
#define CDM_PRESETS_H
#include "app.h"
#define SESSION_PRESETS_FILE L"ccd-session-presets.json"
/* Current account only; heap blocks, HeapFree. No account identifiers leave the layout. */
char *Presets_Account(const char *json, size_t length, const char *account, size_t *outLength);
char *Presets_Merge(const char *json, size_t length, const char *account, const char *incoming, size_t incomingLength, size_t *outLength);
/* One account's presets `old` with `incoming` merged in, as Presets_Merge leaves them. */
char *Presets_MergeAccount(const char *old, size_t oldLength, const char *incoming, size_t incomingLength, size_t *outLength);
#endif
