#include "EngineStatus.h"

#include <beaker.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../Scraping/EngineHealth.h"
#include "../Scraping/Scraping.h"

int engine_status_handler(UrlParams *params) {
  (void)params;
  char *body = malloc(16384);
  if (!body) {
    send_status("500 Internal Server Error");
    send_response("{\"error\":\"out of memory\"}");
    return 0;
  }

  int written = snprintf(body, 16384, "{\"now\":%lld,\"engines\":[",
                         (long long)time(NULL));
  int first = 1;
  for (int i = 0; i < ENGINE_COUNT && written < 16000; i++) {
    if (!ENGINE_REGISTRY[i].enabled)
      continue;
    EngineHealthEntry *h = engine_health_get(ENGINE_REGISTRY[i].id);
    int left = 0;
    SuspendReason reason =
        engine_health_suspension(ENGINE_REGISTRY[i].id, &left);
    time_t open_until =
        reason == SUSPEND_NONE ? 0 : engine_health_open_until(ENGINE_REGISTRY[i].id);
    if (open_until > 0 && open_until <= (time_t)time(NULL)) {
      /* the instant passed but the entry has not been reaped yet: report ready,
         because that is what the dispatcher will do on the next request */
      open_until = 0;
      left = 0;
    }

    /* names come from the registry, but a definition file supplies one, so it
       is escaped before it lands in JSON */
    char escaped[256];
    {
      const char *src = ENGINE_REGISTRY[i].name ? ENGINE_REGISTRY[i].name
                                                : ENGINE_REGISTRY[i].id;
      size_t e = 0;
      for (size_t c = 0; src[c] && e + 3 < sizeof(escaped); c++) {
        if (src[c] == '"' || src[c] == '\\') {
          escaped[e++] = '\\';
          escaped[e++] = src[c];
        } else if ((unsigned char)src[c] < 0x20) {
          continue;
        } else {
          escaped[e++] = src[c];
        }
      }
      escaped[e] = '\0';
    }
    written += snprintf(body + written, (size_t)(16384 - written),
                        "%s{\"id\":\"%s\",\"name\":\"%s\",\"state\":\"%s\","
                        "\"why\":\"%s\",\"resume_at\":%lld,\"seconds\":%d,"
                        "\"answered\":%lu,\"blocked\":%lu,\"errors\":%lu}",
                        first ? "" : ",", ENGINE_REGISTRY[i].id, escaped,
                        engine_health_state_name(h ? h->state : HEALTH_CLOSED),
                        engine_health_suspend_name(reason),
                        (long long)open_until, left, h ? h->total_ok : 0UL,
                        h ? h->total_blocked : 0UL,
                        h ? h->total_errors : 0UL);
    first = 0;
  }
  snprintf(body + written, (size_t)(16384 - written), "]}");

  serve_data(body, strlen(body), "application/json");
  free(body);
  return 0;
}
