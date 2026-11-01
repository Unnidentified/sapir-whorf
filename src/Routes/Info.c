#include "Info.h"

#include "../Scraping/Impersonate.h"
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

/* /info lists engine internals. It is useful on loopback and revealing on a
   LAN, so the detail is only served to the local host (unless the operator has
   bound to another address deliberately, in which case they get health state
   without the git remote). */
static int caller_is_loopback(void) {
  const char *addr = beaker_get_remote_addr();
  if (!addr)
    return 0;
  return strncmp(addr, "127.", 4) == 0 || strcmp(addr, "::1") == 0;
}

int info_handler(UrlParams *params) {
  (void)params;
  extern Config global_config;
  int local = caller_is_loopback();

  char json[INITIAL_BUFFER_SIZE];
  size_t used = 0;

  if (append_json(json, sizeof(json), &used, "{\"version\":\"%s\",", VERSION) !=
          0 ||
      append_json(json, sizeof(json), &used, "\"git_remote\":\"%s\",",
                  local ? GIT_REMOTE : "") != 0 ||
      append_json(json, sizeof(json), &used, "\"locale\":\"%s\",",
                  global_config.default_locale) != 0 ||
      /* K6: the same sentence the startup line prints, so the owner can see
         whether impersonation is active without reading the log */
      append_json(json, sizeof(json), &used, "\"impersonation\":\"%s\",",
                  impersonate_banner()) != 0 ||
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
    int paused_left = 0;
    engine_health_suspension(ENGINE_REGISTRY[i].id, &paused_left);
    if (append_json(json, sizeof(json), &used,
                    "%s{\"id\":\"%s\",\"name\":\"%s\","
                    "\"health\":\"%s\",\"requests\":%lu,\"ok\":%lu,"
                    "\"blocked\":%lu,\"errors\":%lu,"
                    "\"quality\":%.2f,\"quality_samples\":%lu,"
                    "\"never_succeeded\":%s,\"cancelled\":%lu,"
                    "\"suspend_reason\":\"%s\",\"paused_seconds_left\":%d,"
                    "\"last_ok\":%ld,\"last_error\":%ld,\"ua_profile\":\"%s\","
                    "\"impersonate\":\"%s\",\"junk\":%lu}",
                    first ? "" : ",", ENGINE_REGISTRY[i].id,
                    ENGINE_REGISTRY[i].name,
                    health ? engine_health_state_name(health->state) : "unknown",
                    health ? health->total_requests : 0UL,
                    health ? health->total_ok : 0UL,
                    health ? health->total_blocked : 0UL,
                    health ? health->total_errors : 0UL,
                    health && health->quality_samples ? health->quality_ewma : -1.0,
                    health ? health->quality_samples : 0UL,
                    (health && health->total_requests > 0 &&
                     health->total_ok == 0)
                        ? "true"
                        : "false",
                    health ? health->total_cancelled : 0UL,
                    engine_health_suspend_name(
                        health ? health->suspend_reason : SUSPEND_NONE),
                    paused_left,
                    health ? (long)health->last_ok : 0L,
                    health ? (long)health->last_error : 0L,
                    health && health->ua_profile[0] ? health->ua_profile : "-",
                    impersonate_target_for_engine(ENGINE_REGISTRY[i].impersonate)
                        ? impersonate_target_for_engine(
                              ENGINE_REGISTRY[i].impersonate)
                        : "stock",
                    health ? health->total_junk : 0UL) != 0) {
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
