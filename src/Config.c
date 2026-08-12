#include "Config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim(char *value) {
  while (*value == ' ' || *value == '\t')
    value++;

  char *end = value + strlen(value);
  while (end > value && (end[-1] == ' ' || end[-1] == '\t'))
    end--;
  *end = '\0';
  return value;
}

int load_config(const char *filename, Config *config) {
  FILE *file = fopen(filename, "r");
  if (!file) {
    return -1;
  }

  char line[512];
  char section[64] = "";

  while (fgets(line, sizeof(line), file)) {
    line[strcspn(line, "\r\n")] = 0;

    if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
      continue;
    }

    if (line[0] == '[') {
      char *end = strchr(line, ']');
      if (end) {
        *end = '\0';
        snprintf(section, sizeof(section), "%.*s", (int)(sizeof(section) - 1),
                 line + 1);
        section[sizeof(section) - 1] = '\0';
      }
      continue;
    }

    char *delimiter = strchr(line, '=');
    if (delimiter) {
      *delimiter = '\0';
      char *key = trim(line);
      char *value = trim(delimiter + 1);

      char *value_end = value + strlen(value);
      while (value_end > value &&
             (value_end[-1] == '"' || value_end[-1] == '\''))
        *--value_end = '\0';
      while (*value == '"' || *value == '\'')
        value++;

      if (strcmp(section, "server") == 0) {
        if (strcmp(key, "host") == 0) {
          snprintf(config->host, sizeof(config->host), "%s", value);
        } else if (strcmp(key, "port") == 0) {
          config->port = atoi(value);
        } else if (strcmp(key, "locale") == 0) {
          snprintf(config->default_locale, sizeof(config->default_locale), "%s",
                   value);
        }
      } else if (strcmp(section, "proxy") == 0) {
        if (strcmp(key, "proxy") == 0) {
          snprintf(config->proxy, sizeof(config->proxy), "%s", value);
        } else if (strcmp(key, "list_file") == 0) {
          snprintf(config->proxy_list_file, sizeof(config->proxy_list_file),
                   "%s", value);
        } else if (strcmp(key, "max_retries") == 0) {
          config->max_proxy_retries = atoi(value);
        } else if (strcmp(key, "randomize_username") == 0) {
          config->randomize_username = atoi(value);
        } else if (strcmp(key, "randomize_password") == 0) {
          config->randomize_password = atoi(value);
        }
      } else if (strcmp(section, "cache") == 0) {
        if (strcmp(key, "dir") == 0) {
          snprintf(config->cache_dir, sizeof(config->cache_dir), "%s", value);
        } else if (strcmp(key, "ttl_search") == 0) {
          config->cache_ttl_search = atoi(value);
        } else if (strcmp(key, "ttl_infobox") == 0) {
          config->cache_ttl_infobox = atoi(value);
        } else if (strcmp(key, "ttl_image") == 0) {
          config->cache_ttl_image = atoi(value);
        }
      } else if (strcmp(section, "engines") == 0) {
        if (strcmp(key, "engines") == 0) {
          snprintf(config->engines, sizeof(config->engines), "%s", value);
        } else if (strcmp(key, "yacy_instance") == 0) {
          snprintf(config->yacy_instance, sizeof(config->yacy_instance), "%s",
                   value);
        }
      } else if (strcmp(section, "rate_limit") == 0) {
        if (strcmp(key, "search_requests") == 0) {
          config->rate_limit_search_requests = atoi(value);
        } else if (strcmp(key, "search_interval") == 0) {
          config->rate_limit_search_interval = atoi(value);
        } else if (strcmp(key, "images_requests") == 0) {
          config->rate_limit_images_requests = atoi(value);
        } else if (strcmp(key, "images_interval") == 0) {
          config->rate_limit_images_interval = atoi(value);
        }
      }
    }
  }

  fclose(file);
  return 0;
}
