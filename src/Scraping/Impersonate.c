#include "Impersonate.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "../Config.h"
#include "Scraping.h"

extern Config global_config;

/* One pinned target per setting. chrome136, firefox147 and safari260 are the
   ones this build's fixtures were recorded against; a later library release
   ships newer targets, and the point of pinning is that upgrading the library
   does not silently change the fingerprint the tests assert. */
static const struct {
  const char *setting;
  const char *target;
} TARGETS[] = {
    {"chrome", "chrome136"},
    {"firefox", "firefox147"},
    {"safari", "safari260"},
};

const char *impersonate_target_for(const char *setting) {
  if (!setting || !setting[0] || strcasecmp(setting, "none") == 0)
    return NULL;
  for (size_t i = 0; i < sizeof(TARGETS) / sizeof(TARGETS[0]); i++)
    if (strcasecmp(setting, TARGETS[i].setting) == 0)
      return TARGETS[i].target;
  return NULL;
}

const char *impersonate_banner(void) {
  static char line[128];
  const char *resolved =
      impersonate_target_for(global_config.impersonate[0]
                                 ? global_config.impersonate
                                 : "none");
#ifdef HAVE_CURL_IMPERSONATE
  if (resolved)
    snprintf(line, sizeof(line), "%s, default target %s",
             IMPERSONATION_LIBRARY, resolved);
  else
    snprintf(line, sizeof(line), "%s, impersonation off", IMPERSONATION_LIBRARY);
#else
  (void)resolved;
  snprintf(line, sizeof(line), "off (stock libcurl)");
#endif
  return line;
}

const char *impersonate_target_for_engine(const char *engine_override) {
#ifdef HAVE_CURL_IMPERSONATE
  const char *setting = engine_override && engine_override[0]
                            ? engine_override
                            : global_config.impersonate;
  return impersonate_target_for(setting && setting[0] ? setting : "chrome");
#else
  (void)engine_override;
  return NULL;
#endif
}
