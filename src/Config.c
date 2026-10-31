#include "Config.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum {
  CONFIG_STRING,
  CONFIG_INTEGER,
  CONFIG_BOOLEAN,
} ConfigValueType;

typedef struct {
  const char *section;
  const char *key;
  ConfigValueType type;
  size_t offset;
  size_t size;
} ConfigOption;

#define STRING_OPTION(section_name, key_name, field)                           \
  {section_name, key_name, CONFIG_STRING, offsetof(Config, field),             \
   sizeof(((Config *)0)->field)}
#define INTEGER_OPTION(section_name, key_name, field)                          \
  {section_name, key_name, CONFIG_INTEGER, offsetof(Config, field), 0}
#define BOOLEAN_OPTION(section_name, key_name, field)                          \
  {section_name, key_name, CONFIG_BOOLEAN, offsetof(Config, field), 0}

static const ConfigOption CONFIG_OPTIONS[] = {
    STRING_OPTION("server", "host", host),
    INTEGER_OPTION("server", "port", port),
    STRING_OPTION("server", "locale", default_locale),
    STRING_OPTION("proxy", "proxy", proxy),
    STRING_OPTION("proxy", "list_file", proxy_list_file),
    INTEGER_OPTION("proxy", "max_retries", max_proxy_retries),
    BOOLEAN_OPTION("proxy", "randomize_username", randomize_username),
    BOOLEAN_OPTION("proxy", "randomize_password", randomize_password),
    STRING_OPTION("cache", "dir", cache_dir),
    INTEGER_OPTION("cache", "ttl_search", cache_ttl_search),
    INTEGER_OPTION("cache", "ttl_infobox", cache_ttl_infobox),
    INTEGER_OPTION("cache", "ttl_image", cache_ttl_image),
    STRING_OPTION("engines", "engines", engines),
    STRING_OPTION("engines", "yacy_instance", yacy_instance),
    STRING_OPTION("engines", "google_cse_key", google_cse_key),
    STRING_OPTION("engines", "google_cse_cx", google_cse_cx),
    STRING_OPTION("engines", "google_cse_tok", google_cse_tok),
    STRING_OPTION("engines", "searxng_url", searxng_url),
    STRING_OPTION("engines", "searxng_engines", searxng_engines),
    STRING_OPTION("rate_limit", "trusted_proxies", trusted_proxies),
    STRING_OPTION("server", "infobox", infobox_mode),
    STRING_OPTION("server", "answers", answers_mode),
    STRING_OPTION("server", "answers_fanout", answers_fanout),
    INTEGER_OPTION("rate_limit", "search_requests", rate_limit_search_requests),
    INTEGER_OPTION("rate_limit", "search_interval", rate_limit_search_interval),
    INTEGER_OPTION("rate_limit", "images_requests", rate_limit_images_requests),
    INTEGER_OPTION("rate_limit", "images_interval", rate_limit_images_interval),
    INTEGER_OPTION("health", "breaker_threshold", breaker_threshold),
    INTEGER_OPTION("health", "cooldown_seconds", breaker_cooldown),
    /* G2: outbound pacing per engine */
    INTEGER_OPTION("health", "pace_requests_per_minute", pace_per_minute),
    INTEGER_OPTION("health", "pace_burst", pace_burst),
};

void config_set_defaults(Config *config) {
  if (!config) {
    return;
  }

  *config = (Config){
      .host = DEFAULT_HOST,
      .port = DEFAULT_PORT,
      .default_locale = "en_gb",
      .max_proxy_retries = DEFAULT_MAX_PROXY_RETRIES,
      .cache_dir = DEFAULT_CACHE_DIR,
      .cache_ttl_search = DEFAULT_CACHE_TTL_SEARCH,
      .cache_ttl_infobox = DEFAULT_CACHE_TTL_INFOBOX,
      .cache_ttl_image = DEFAULT_CACHE_TTL_IMAGE,
      .breaker_threshold = DEFAULT_BREAKER_THRESHOLD,
      .breaker_cooldown = DEFAULT_BREAKER_COOLDOWN,
      .answers_mode = DEFAULT_ANSWERS_MODE,
      .answers_fanout = DEFAULT_ANSWERS_FANOUT,
      .pace_per_minute = DEFAULT_PACE_PER_MINUTE,
      .pace_burst = DEFAULT_PACE_BURST,
      .rate_limit_search_requests = DEFAULT_RATE_LIMIT_SEARCH_REQUESTS,
      .rate_limit_search_interval = DEFAULT_RATE_LIMIT_SEARCH_INTERVAL,
      .rate_limit_images_requests = DEFAULT_RATE_LIMIT_IMAGES_REQUESTS,
      .rate_limit_images_interval = DEFAULT_RATE_LIMIT_IMAGES_INTERVAL,
      .infobox_mode = "all",
  };
}

static char *trim(char *value) {
  while (*value == ' ' || *value == '\t') {
    value++;
  }

  char *end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t')) {
    end--;
  }
  *end = '\0';
  return value;
}

static char *unquote(char *value) {
  size_t length = strlen(value);
  if (length >= 2 && (value[0] == '"' || value[0] == '\'') &&
      value[length - 1] == value[0]) {
    value[length - 1] = '\0';
    return value + 1;
  }
  return value;
}

static int parse_integer(const char *value, int *result) {
  char *end;
  errno = 0;
  long parsed = strtol(value, &end, 10);

  if (value == end || *end != '\0' || errno == ERANGE || parsed < INT_MIN ||
      parsed > INT_MAX) {
    return -1;
  }

  *result = (int)parsed;
  return 0;
}

static int parse_boolean(const char *value, int *result) {
  if (strcmp(value, "1") == 0 || strcasecmp(value, "true") == 0 ||
      strcasecmp(value, "yes") == 0 || strcasecmp(value, "on") == 0) {
    *result = 1;
    return 0;
  }
  if (strcmp(value, "0") == 0 || strcasecmp(value, "false") == 0 ||
      strcasecmp(value, "no") == 0 || strcasecmp(value, "off") == 0) {
    *result = 0;
    return 0;
  }
  return -1;
}

/* Returns 0 when applied, -1 when the key is unknown, -2 when the value is
   unusable. The old version returned nothing, so "breaker_threshold = 5"
   written as "threshold = 5" silently did nothing whatever - the config lied
   about itself and the operator had no way to know. */
static int set_option(Config *config, const char *section, const char *key,
                      const char *value, int line_no) {
  size_t option_count = sizeof(CONFIG_OPTIONS) / sizeof(CONFIG_OPTIONS[0]);

  for (size_t i = 0; i < option_count; i++) {
    const ConfigOption *option = &CONFIG_OPTIONS[i];
    if (strcmp(section, option->section) != 0 ||
        strcmp(key, option->key) != 0) {
      continue;
    }

    void *destination = (char *)config + option->offset;
    if (option->type == CONFIG_STRING) {
      /* J5.6: snprintf truncated silently, so a long searxng_engines list
         lost names without a word. Say so instead. */
      if (strlen(value) >= option->size) {
        fprintf(stderr,
                "[WARN] config.ini:%d [%s] %s is %zu bytes and the limit is "
                "%zu; the value was rejected and the default kept. Raise the "
                "limit in Config.h or shorten the list\n",
                line_no, section, key, strlen(value), option->size - 1);
        return -2;
      }
      snprintf(destination, option->size, "%s", value);
      return 0;
    }
    if (option->type == CONFIG_INTEGER) {
      if (parse_integer(value, destination) != 0) {
        fprintf(stderr,
                "[WARN] config.ini:%d [%s] %s = \"%s\" is not an integer; "
                "keeping the default\n",
                line_no, section, key, value);
        return -2;
      }
      return 0;
    }
    if (parse_boolean(value, destination) != 0) {
      fprintf(stderr,
              "[WARN] config.ini:%d [%s] %s = \"%s\" is not a boolean; "
              "keeping the default\n",
              line_no, section, key, value);
      return -2;
    }
    return 0;
  }

  /* [ranking] is a known-dead section: say so once rather than pretending */
  if (strcmp(section, "ranking") == 0) {
    fprintf(stderr,
            "[WARN] config.ini:%d section [ranking] is not read by anything; "
            "ranking weights are compile-time constants in "
            "src/Ranking/Ranker.c\n",
            line_no);
    return -1;
  }

  fprintf(stderr, "[WARN] config.ini:%d unknown key [%s] %s = \"%s\"\n",
          line_no, section, key, value);
  return -1;
}

int load_config(const char *filename, Config *config) {
  if (!filename || !config) {
    return -1;
  }

  FILE *file = fopen(filename, "r");
  if (!file) {
    return -1;
  }

  /* J5.6: 1024 bytes here was the real limit on any single config value, so
     raising searxng_engines alone would have changed nothing. A line longer
     than this is reported rather than silently split. */
  char line[4096];
  char section[64] = "";
  int line_no = 0;
  int unknown_or_bad = 0;

  while (fgets(line, sizeof(line), file)) {
    line_no++;
    if (!strchr(line, '\n') && !feof(file)) {
      fprintf(stderr,
              "[WARN] config.ini:%d is longer than %zu bytes and was split; "
              "shorten the value\n",
              line_no, sizeof(line) - 1);
      int ch;
      while ((ch = fgetc(file)) != '\n' && ch != EOF)
        ;
      continue;
    }
    line[strcspn(line, "\r\n")] = '\0';
    char *content = trim(line);

    if (*content == '\0' || *content == '#' || *content == ';') {
      continue;
    }

    if (*content == '[') {
      char *end = strchr(content + 1, ']');
      if (end) {
        *end = '\0';
        snprintf(section, sizeof(section), "%s", trim(content + 1));
      }
      continue;
    }

    char *delimiter = strchr(content, '=');
    if (!delimiter) {
      continue;
    }

    *delimiter = '\0';
    char *key = trim(content);
    char *value = unquote(trim(delimiter + 1));
    int rc = set_option(config, section, key, value, line_no);
    if (rc < 0)
      unknown_or_bad++;
  }

  if (unknown_or_bad > 0)
    fprintf(stderr,
            "[WARN] config.ini: %d key(s) were rejected above; those settings "
            "are NOT in effect\n",
            unknown_or_bad);

  int result = ferror(file) ? -1 : 0;
  if (unknown_or_bad > 0 && result == 0)
    result = 2; /* loaded, but some settings are not in effect */
  fclose(file);
  return result;
}
