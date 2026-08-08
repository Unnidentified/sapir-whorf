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

int append_string_row(char ****matrix, int **inner_counts, int row_count,
                      const char *const *values, int field_count) {
  if (!matrix || !inner_counts || !values || field_count <= 0)
    return row_count;

  char **row = calloc((size_t)field_count, sizeof(*row));
  if (!row)
    return row_count;

  for (int i = 0; i < field_count; i++) {
    row[i] = strdup(values[i] ? values[i] : "");
    if (!row[i]) {
      for (int j = 0; j < i; j++)
        free(row[j]);
      free(row);
      return row_count;
    }
  }

  char ***new_matrix = malloc(sizeof(*new_matrix) * (row_count + 1));
  int *new_counts = malloc(sizeof(*new_counts) * (row_count + 1));
  if (!new_matrix || !new_counts) {
    free(new_matrix);
    free(new_counts);
    for (int i = 0; i < field_count; i++)
      free(row[i]);
    free(row);
    return row_count;
  }

  if (row_count > 0) {
    memcpy(new_matrix, *matrix, sizeof(*new_matrix) * row_count);
    memcpy(new_counts, *inner_counts, sizeof(*new_counts) * row_count);
  }

  new_matrix[row_count] = row;
  new_counts[row_count] = field_count;
  free(*matrix);
  free(*inner_counts);
  *matrix = new_matrix;
  *inner_counts = new_counts;
  return row_count + 1;
}

void free_string_matrix(char ***matrix, int *inner_counts, int row_count) {
  if (matrix) {
    for (int i = 0; i < row_count; i++) {
      int field_count = inner_counts ? inner_counts[i] : 0;
      for (int j = 0; j < field_count; j++)
        free(matrix[i][j]);
      free(matrix[i]);
    }
  }
  free(matrix);
  free(inner_counts);
}

int add_link_to_collection(const char *href, const char *label,
                           const char *class_name, char ****collection,
                           int **inner_counts, int current_count) {
  const char *values[LINK_FIELD_COUNT] = {href, label, class_name};
  return append_string_row(collection, inner_counts, current_count, values,
                           LINK_FIELD_COUNT);
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
