#include "SearchCacheKey.h"
#include <stdio.h>

void search_cache_scope(char *out, size_t cap, const char *engine_id,
                        const char *client_key, const char *category) {
  if (!out || cap == 0)
    return;
  snprintf(out, cap, "search_request:%s:%s:cat=%s",
           (engine_id && engine_id[0]) ? engine_id : "all",
           (client_key && client_key[0]) ? client_key : "unknown",
           (category && category[0]) ? category : "any");
}
