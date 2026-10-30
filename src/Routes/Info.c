#include "Info.h"
#include "../Config.h"
#include "../Scraping/EngineHealth.h"
#include "../Scraping/Scraping.h"
#include "../Utility/Utility.h"
#include <beaker.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static int append_json(char *buf, size_t size, size_t *used, const char *format,
                       ...) {
  if (*used >= size) {
    return -1;
  }

  va_list args;
  va_start(args, format);
  int written = vsnprintf(buf + *used, size - *used, format, args);
  va_end(args);

  if (written < 0 || (size_t)written >= size - *used) {
    return -1;
  }

  *used += (size_t)written;
  return 0;
}

int info_handler(UrlParams *params) {
  (void)params;
  extern Config global_config;

  char json[INITIAL_BUFFER_SIZE];
  size_t used = 0;

  if (append_json(json, sizeof(json), &used, "{\"version\":\"%s\",", VERSION) !=
          0 ||
      append_json(json, sizeof(json), &used, "\"git_remote\":\"%s\",",
                  GIT_REMOTE) != 0 ||
      append_json(json, sizeof(json), &used, "\"locale\":\"%s\",",
                  global_config.default_locale) != 0 ||
      append_json(json, sizeof(json), &used, "\"engines\":[") != 0) {
    send_response("Failed to build instance info");
    return -1;
  }

  int first = 1;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (!ENGINE_REGISTRY[i].enabled) {
      continue;
    }
    EngineHealthEntry *health = engine_health_get(ENGINE_REGISTRY[i].id);
    if (append_json(json, sizeof(json), &used,
                    "%s{\"id\":\"%s\",\"name\":\"%s\","
                    "\"health\":\"%s\",\"requests\":%lu,\"ok\":%lu,"
                    "\"blocked\":%lu,\"errors\":%lu}",
                    first ? "" : ",", ENGINE_REGISTRY[i].id,
                    ENGINE_REGISTRY[i].name,
                    health ? engine_health_state_name(health->state) : "unknown",
                    health ? health->total_requests : 0UL,
                    health ? health->total_ok : 0UL,
                    health ? health->total_blocked : 0UL,
                    health ? health->total_errors : 0UL) != 0) {
      send_response("Failed to build instance info");
      return -1;
    }
    first = 0;
  }

  if (append_json(json, sizeof(json), &used, "]}") != 0) {
    send_response("Failed to build instance info");
    return -1;
  }

  serve_data(json, used, "application/json");

  return 0;
}
