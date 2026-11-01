#include "Effective.h"
#include "Secrets.h"
#include "Utility.h"
#include "Config.h"
#include <stdio.h>
#include <string.h>

extern Config global_config;

/* the settings the page may edit, and where config.ini keeps them. A key with
   no config field is file-or-environment only. */
typedef struct {
  const char *key;
  SecretKey secret;
  int config_field; /* 0 = the secrets file is the only owner */
} EffectiveKey;

enum {
  EF_NONE = 0,
  EF_SEARXNG_URL,
  EF_SEARXNG_ENGINES,
  EF_YACY_INSTANCE,
  EF_GOOGLE_CSE_CX,
  EF_GOOGLE_CSE_KEY
};

static const EffectiveKey KEYS[] = {
    {"searxng_url", SK_SEARXNG_URL, EF_SEARXNG_URL},
    {"searxng_engines", SK_SEARXNG_ENGINES, EF_SEARXNG_ENGINES},
    {"yacy_instance", SK_YACY_INSTANCE, EF_YACY_INSTANCE},
    {"google_cse_cx", SK_GOOGLE_CSE_CX, EF_GOOGLE_CSE_CX},
    {"google_cse_key", SK_GOOGLE_CSE_KEY, EF_GOOGLE_CSE_KEY},
    {"reddit_client_id", SK_REDDIT_CLIENT_ID, EF_NONE},
    {"reddit_client_secret", SK_REDDIT_CLIENT_SECRET, EF_NONE},
    {"reddit_username", SK_REDDIT_USERNAME, EF_NONE},
    {"reddit_redirect_uri", SK_REDDIT_REDIRECT_URI, EF_NONE},
    {"reddit_refresh_token", SK_REDDIT_REFRESH_TOKEN, EF_NONE},
    {"extra_engines", SK_EXTRA_ENGINES, EF_NONE},
};
enum { KEY_COUNT = sizeof(KEYS) / sizeof(KEYS[0]) };

static const char *config_value(int which) {
  switch (which) {
  case EF_SEARXNG_URL:
    return global_config.searxng_url;
  case EF_SEARXNG_ENGINES:
    return global_config.searxng_engines;
  case EF_YACY_INSTANCE:
    return global_config.yacy_instance;
  case EF_GOOGLE_CSE_CX:
    return global_config.google_cse_cx;
  case EF_GOOGLE_CSE_KEY:
    return global_config.google_cse_key;
  default:
    return "";
  }
}

EffectiveSource effective_get(const char *key, char *out, size_t cap) {
  if (!out || cap == 0)
    return EFFECTIVE_FROM_DEFAULT;
  out[0] = '\0';
  if (!key)
    return EFFECTIVE_FROM_DEFAULT;
  for (int i = 0; i < KEY_COUNT; i++) {
    if (strcmp(KEYS[i].key, key) != 0)
      continue;
    const char *value = "";
    SecretSource src = secrets_get(KEYS[i].secret, &value);
    if (src == SECRET_SOURCE_ENV) {
      snprintf(out, cap, "%s", value);
      return EFFECTIVE_FROM_ENV;
    }
    if (src == SECRET_SOURCE_FILE) {
      snprintf(out, cap, "%s", value);
      return EFFECTIVE_FROM_FILE;
    }
    const char *from_config = config_value(KEYS[i].config_field);
    if (from_config && from_config[0]) {
      snprintf(out, cap, "%s", from_config);
      return EFFECTIVE_FROM_CONFIG;
    }
    return EFFECTIVE_FROM_DEFAULT;
  }
  return EFFECTIVE_FROM_DEFAULT;
}

void apply_effective_settings(void *config_ptr) {
  Config *cfg = (Config *)config_ptr;
  if (!cfg)
    return;
  /* nullable fields: the file wins when the page has written one, otherwise
     config.ini's value stays as loaded */
  struct {
    const char *key;
    char *field;
    size_t size;
  } FIELDS[] = {
      {"searxng_url", cfg->searxng_url, sizeof(cfg->searxng_url)},
      {"searxng_engines", cfg->searxng_engines, sizeof(cfg->searxng_engines)},
      {"yacy_instance", cfg->yacy_instance, sizeof(cfg->yacy_instance)},
      {"google_cse_cx", cfg->google_cse_cx, sizeof(cfg->google_cse_cx)},
      {"google_cse_key", cfg->google_cse_key, sizeof(cfg->google_cse_key)},
  };
  for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
    const char *value = "";
    SecretSource src = secrets_get(secrets_key_from_name(FIELDS[i].key), &value);
    if ((src == SECRET_SOURCE_FILE || src == SECRET_SOURCE_ENV) && value[0])
      snprintf(FIELDS[i].field, FIELDS[i].size, "%s", value);
  }
}

const char *effective_source_label(EffectiveSource src) {
  switch (src) {
  case EFFECTIVE_FROM_ENV:
    return "From the environment (not editable here)";
  case EFFECTIVE_FROM_FILE:
    return "Saved here";
  case EFFECTIVE_FROM_CONFIG:
    return "From config.ini";
  default:
    return "Not set";
  }
}
