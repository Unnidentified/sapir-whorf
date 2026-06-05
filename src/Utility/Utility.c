#include "Utility.h"
#include "../Scraping/Scraping.h"
#include <beaker.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char global_default_locale[32] = "en_gb";
static char **themes_list = NULL;
static int themes_count = 0;
static int themes_initialized = 0;

void init_themes(const char *static_path) {
  if (themes_initialized)
    return;
  themes_initialized = 1;

  char themes_dir[512];
  snprintf(themes_dir, sizeof(themes_dir), "%s/themes", static_path);

  DIR *dir = opendir(themes_dir);
  if (!dir)
    return;

  struct dirent *entry;
  int capacity = 4;
  themes_list = malloc(sizeof(char *) * capacity);
  themes_count = 0;

  while ((entry = readdir(dir)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (len > 4 && strcmp(entry->d_name + len - 4, ".css") == 0) {
      if (themes_count >= capacity) {
        capacity *= 2;
        themes_list = realloc(themes_list, sizeof(char *) * capacity);
      }
      themes_list[themes_count] = strndup(entry->d_name, len - 4);
      themes_count++;
    }
  }
  closedir(dir);

  for (int i = 0; i < themes_count; i++) {
    for (int j = i + 1; j < themes_count; j++) {
      int priority_i = 0, priority_j = 0;
      if (strcmp(themes_list[i], "system") == 0)
        priority_i = 0;
      else if (strcmp(themes_list[i], "light") == 0)
        priority_i = 1;
      else if (strcmp(themes_list[i], "dark") == 0)
        priority_i = 2;
      else
        priority_i = 3;
      if (strcmp(themes_list[j], "system") == 0)
        priority_j = 0;
      else if (strcmp(themes_list[j], "light") == 0)
        priority_j = 1;
      else if (strcmp(themes_list[j], "dark") == 0)
        priority_j = 2;
      else
        priority_j = 3;
      if (priority_i > priority_j ||
          (priority_i == priority_j &&
           strcmp(themes_list[i], themes_list[j]) > 0)) {
        char *tmp = themes_list[i];
        themes_list[i] = themes_list[j];
        themes_list[j] = tmp;
      }
    }
  }
}

void get_available_themes(char ***out_themes, int *out_count) {
  *out_themes = themes_list;
  *out_count = themes_count;
}

void set_default_locale(const char *locale) {
  if (locale && strlen(locale) > 0) {
    strncpy(global_default_locale, locale, sizeof(global_default_locale) - 1);
    global_default_locale[sizeof(global_default_locale) - 1] = '\0';
  }
}

int hex_to_int(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

char *get_theme(const char *default_theme) {
  char *cookie = get_cookie("theme");
  if (cookie && strlen(cookie) > 0) {
    for (int i = 0; i < themes_count; i++) {
      if (strcmp(cookie, themes_list[i]) == 0) {
        return cookie;
      }
    }
  }
  free(cookie);
  return strdup(default_theme && strlen(default_theme) > 0 ? default_theme
                                                           : "system");
}

char *get_locale(const char *default_locale) {
  char *cookie = get_cookie("locale");
  if (cookie && beaker_get_locale_meta(cookie) != NULL) {
    return cookie;
  }
  free(cookie);
  const char *fallback =
      default_locale ? default_locale : global_default_locale;
  return strdup(fallback);
}

static int engine_id_casecmp(const char *a, const char *b) {
  while (*a && *b) {
    char la = *a;
    char lb = *b;
    if (la >= 'A' && la <= 'Z')
      la = la - 'A' + 'a';
    if (lb >= 'A' && lb <= 'Z')
      lb = lb - 'A' + 'a';
    if (la != lb)
      return 0;
    a++;
    b++;
  }
  return *a == *b;
}

int is_engine_id_enabled(const char *engine_id) {
  if (!engine_id)
    return 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled &&
        engine_id_casecmp(ENGINE_REGISTRY[i].id, engine_id)) {
      return 1;
    }
  }
  return 0;
}

int get_user_engines(char ***out_ids, int *out_count) {
  *out_ids = NULL;
  *out_count = 0;

  char *cookie = get_cookie("engines");
  if (!cookie || cookie[0] == '\0') {
    free(cookie);
    return -1;
  }

  char **ids = NULL;
  int count = 0;

  char *copy = strdup(cookie);
  if (!copy) {
    free(cookie);
    return -1;
  }

  char *saveptr;
  char *token = strtok_r(copy, ",", &saveptr);
  while (token) {
    while (*token == ' ' || *token == '\t')
      token++;

    if (token[0] != '\0' && is_engine_id_enabled(token)) {
      char **new_ids = realloc(ids, sizeof(char *) * (count + 1));
      if (new_ids) {
        ids = new_ids;
        ids[count] = strdup(token);
        count++;
      }
    }

    token = strtok_r(NULL, ",", &saveptr);
  }

  free(copy);
  free(cookie);

  if (count == 0) {
    free(ids);
    return -1;
  }

  *out_ids = ids;
  *out_count = count;
  return 0;
}

int user_engines_contains(const char *engine_id, char **ids, int count) {
  if (!engine_id || !ids)
    return 0;
  for (int i = 0; i < count; i++) {
    if (engine_id_casecmp(ids[i], engine_id))
      return 1;
  }
  return 0;
}

int add_link_to_collection(const char *href, const char *label,
                           const char *class_name, char ****collection,
                           int **inner_counts, int current_count) {
  char ***old_collection = *collection;
  int *old_inner_counts = *inner_counts;
  char ***new_collection =
      (char ***)malloc(sizeof(char **) * (current_count + 1));
  int *new_inner_counts = (int *)malloc(sizeof(int) * (current_count + 1));

  if (!new_collection || !new_inner_counts) {
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  if (*collection && current_count > 0) {
    memcpy(new_collection, *collection, sizeof(char **) * current_count);
  }
  if (*inner_counts && current_count > 0) {
    memcpy(new_inner_counts, *inner_counts, sizeof(int) * current_count);
  }

  *collection = new_collection;
  *inner_counts = new_inner_counts;

  (*collection)[current_count] =
      (char **)malloc(sizeof(char *) * LINK_FIELD_COUNT);
  if (!(*collection)[current_count]) {
    *collection = old_collection;
    *inner_counts = old_inner_counts;
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  (*collection)[current_count][0] = strdup(href ? href : "");
  (*collection)[current_count][1] = strdup(label ? label : "");
  (*collection)[current_count][2] = strdup(class_name ? class_name : "");

  if (!(*collection)[current_count][0] || !(*collection)[current_count][1] ||
      !(*collection)[current_count][2]) {
    free((*collection)[current_count][0]);
    free((*collection)[current_count][1]);
    free((*collection)[current_count][2]);
    free((*collection)[current_count]);
    *collection = old_collection;
    *inner_counts = old_inner_counts;
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  (*inner_counts)[current_count] = LINK_FIELD_COUNT;

  free(old_collection);
  free(old_inner_counts);
  return current_count + 1;
}

int build_pagination(int page, char *(*href_builder)(int page, void *data),
                     void *data, char ****out_matrix, int **out_inner_counts) {
  enum { PAGER_WINDOW_SIZE = 5 };

  *out_matrix = NULL;
  *out_inner_counts = NULL;
  int count = 0;

  int pager_start = page <= 3 ? 1 : page - 2;
  int pager_end = pager_start + PAGER_WINDOW_SIZE - 1;

  if (page > 1) {
    char *href = href_builder(page - 1, data);
    count = add_link_to_collection(href, "←", "pagination-btn prev", out_matrix,
                                   out_inner_counts, count);
    free(href);
  }

  for (int i = pager_start; i <= pager_end; i++) {
    char label[16];
    snprintf(label, sizeof(label), "%d", i);
    char *href = href_builder(i, data);
    count = add_link_to_collection(
        href, label,
        i == page ? "pagination-btn pagination-current" : "pagination-btn",
        out_matrix, out_inner_counts, count);
    free(href);
  }

  char *href = href_builder(page + 1, data);
  count = add_link_to_collection(href, "→", "pagination-btn next", out_matrix,
                                 out_inner_counts, count);
  free(href);

  return count;
}
