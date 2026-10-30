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
    STRING_OPTION("engines", "searxng_url", searxng_url),
    STRING_OPTION("engines", "searxng_engines", searxng_engines),
    STRING_OPTION("rate_limit", "trusted_proxies", trusted_proxies),
    INTEGER_OPTION("rate_limit", "search_requests", rate_limit_search_requests),
    INTEGER_OPTION("rate_limit", "search_interval", rate_limit_search_interval),
    INTEGER_OPTION("rate_limit", "images_requests", rate_limit_images_requests),
    INTEGER_OPTION("rate_limit", "images_interval", rate_limit_images_interval),
    INTEGER_OPTION("health", "breaker_threshold", breaker_threshold),
    INTEGER_OPTION("health", "cooldown_seconds", breaker_cooldown),
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

static void set_option(Config *config, const char *section, const char *key,
                       const char *value) {
  size_t option_count = sizeof(CONFIG_OPTIONS) / sizeof(CONFIG_OPTIONS[0]);

  for (size_t i = 0; i < option_count; i++) {
    const ConfigOption *option = &CONFIG_OPTIONS[i];
    if (strcmp(section, option->section) != 0 ||
        strcmp(key, option->key) != 0) {
      continue;
    }

    void *destination = (char *)config + option->offset;
    if (option->type == CONFIG_STRING) {
      snprintf(destination, option->size, "%s", value);
    } else if (option->type == CONFIG_INTEGER) {
      parse_integer(value, destination);
    } else {
      parse_boolean(value, destination);
    }
    return;
  }
}

int load_config(const char *filename, Config *config) {
  if (!filename || !config) {
    return -1;
  }

  FILE *file = fopen(filename, "r");
  if (!file) {
    return -1;
  }

  char line[1024];
  char section[64] = "";

  while (fgets(line, sizeof(line), file)) {
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
    set_option(config, section, key, value);
  }

  int result = ferror(file) ? -1 : 0;
  fclose(file);
  return result;
}
