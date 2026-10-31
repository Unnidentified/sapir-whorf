#include "Utility.h"
#include "../Scraping/Scraping.h"
#include <beaker.h>
#include <curl/curl.h>
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

  char themes_dir[512];
  snprintf(themes_dir, sizeof(themes_dir), "%s/themes", static_path);

  DIR *dir = opendir(themes_dir);
  if (!dir)
    return;

  struct dirent *entry;
  int capacity = 4;
  themes_list = malloc(sizeof(char *) * capacity);
  if (!themes_list) {
    closedir(dir);
    return;
  }
  themes_count = 0;

  while ((entry = readdir(dir)) != NULL) {
    size_t len = strlen(entry->d_name);
    if (len > 4 && strcmp(entry->d_name + len - 4, ".css") == 0) {
      if (themes_count >= capacity) {
        int new_capacity = capacity * 2;
        char **new_list = realloc(themes_list, sizeof(char *) * new_capacity);
        if (!new_list)
          break;
        themes_list = new_list;
        capacity = new_capacity;
      }
      char *theme = strndup(entry->d_name, len - 4);
      if (!theme)
        break;
      themes_list[themes_count] = theme;
      themes_count++;
    }
  }
  closedir(dir);
  themes_initialized = 1;

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

/* J3d: the query reached every link verbatim, so /search?q=1+1 decoded as
   "1 1", AT&T became "AT" and C# became "C". One encoder, used by every href
   builder and by the templates. curl_easy_escape is already a dependency
   (the engine queries go through it). */
int url_encode(const char *in, char *out, size_t size) {
  CURL *curl;
  char *esc;
  if (!out || size == 0)
    return 0;
  out[0] = '\0';
  if (!in)
    return 0;
  curl = curl_easy_init();
  if (!curl)
    return 0;
  esc = curl_easy_escape(curl, in, 0);
  if (!esc) {
    curl_easy_cleanup(curl);
    return 0;
  }
  snprintf(out, size, "%s", esc);
  curl_free(esc);
  curl_easy_cleanup(curl);
  return (int)strlen(out);
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
                                                           : "amber");
}

/* J3c: the answer settings are per user, like the theme and the locale, so a
   cookie wins over the server default. Only known values are accepted: the
   value reaches a comparison, and an arbitrary cookie must not turn into one. */
static void user_choice(const char *cookie_name, const char *fallback,
                        const char *const *allowed, int allowed_count,
                        char *out, size_t size) {
  char *cookie = get_cookie(cookie_name);
  const char *pick = NULL;
  if (cookie) {
    for (int i = 0; i < allowed_count; i++) {
      if (strcmp(cookie, allowed[i]) == 0) {
        pick = allowed[i];
        break;
      }
    }
    free(cookie);
  }
  if (!pick)
    pick = (fallback && fallback[0]) ? fallback : allowed[0];
  snprintf(out, size, "%s", pick);
}

int answers_mode_ok(const char *value) {
  return value && (!strcmp(value, "local") || !strcmp(value, "all") ||
                   !strcmp(value, "off"));
}

int answers_fanout_ok(const char *value) {
  return value && (!strcmp(value, "always") || !strcmp(value, "auto") ||
                   !strcmp(value, "never"));
}

void get_answers_mode(const char *fallback, char *out, size_t size) {
  static const char *const ALLOWED[] = {"local", "all", "off"};
  user_choice("answers", fallback, ALLOWED, 3, out, size);
}

void get_answers_fanout(const char *fallback, char *out, size_t size) {
  static const char *const ALLOWED[] = {"always", "auto", "never"};
  user_choice("answers_fanout", fallback, ALLOWED, 3, out, size);
}

/* ---------- user-defined palette ---------- */

static const char *CUSTOM_DEFAULTS[CUSTOM_COLOUR_COUNT] = {
    "0f0f0f", "272a27", "333633", "e6e3e0",
    "b0b0ad", "8f8f8d", "c99a75", "7d7d7b"};

static char *custom_slot(CustomTheme *t, int index) {
  switch (index) {
  case 0: return t->bg_main;
  case 1: return t->bg_card;
  case 2: return t->border;
  case 3: return t->text_primary;
  case 4: return t->text_secondary;
  case 5: return t->text_muted;
  case 6: return t->accent;
  default: return t->text_dim;
  }
}

static int is_hex6(const char *s) {
  if (!s)
    return 0;
  if (*s == '#')
    s++;
  for (int i = 0; i < 6; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F')))
      return 0;
  }
  return s[6] == '\0';
}

int parse_custom_theme_cookie(const char *value, CustomTheme *out) {
  if (!out)
    return -1;

  for (int i = 0; i < CUSTOM_COLOUR_COUNT; i++)
    snprintf(custom_slot(out, i), 7, "%s", CUSTOM_DEFAULTS[i]);

  if (!value || value[0] == '\0')
    return -1;

  char *copy = strdup(value);
  if (!copy)
    return -1;

  int index = 0;
  char *saveptr = NULL;
  char *token = strtok_r(copy, "-", &saveptr);
  while (token && index < CUSTOM_COLOUR_COUNT) {
    while (*token == ' ' || *token == '#')
      token++;
    if (is_hex6(token))
      snprintf(custom_slot(out, index), 7, "%s", token);
    index++;
    token = strtok_r(NULL, "-", &saveptr);
  }

  free(copy);
  return index == CUSTOM_COLOUR_COUNT ? 0 : -1;
}

void get_custom_theme(CustomTheme *out) {
  char *cookie = get_cookie("customtheme");
  parse_custom_theme_cookie(cookie, out);
  free(cookie);
}

void context_set_custom_theme(TemplateContext *ctx, const CustomTheme *theme) {
  if (!ctx || !theme)
    return;

  static const char *KEYS[CUSTOM_COLOUR_COUNT + 1] = {
      "custom_bg_main", "custom_bg_card", "custom_border",
      "custom_text_primary", "custom_text_secondary", "custom_text_muted",
      "custom_accent", "custom_text_dim", "custom_accent_glow"};

  /* beaker keeps the pointer, so these must outlive the call */
  static __thread char values[CUSTOM_COLOUR_COUNT + 1][16];

  for (int i = 0; i < CUSTOM_COLOUR_COUNT; i++)
    snprintf(values[i], sizeof(values[i]), "#%s",
             custom_slot((CustomTheme *)theme, i));

  snprintf(values[CUSTOM_COLOUR_COUNT], sizeof(values[0]), "#%s1a",
           theme->accent);

  for (int i = 0; i < CUSTOM_COLOUR_COUNT + 1; i++)
    context_set(ctx, KEYS[i], values[i]);
}

const char *get_title_font(void) {
  char *cookie = get_cookie("titlefont");
  const char *value = "mono";

  if (cookie) {
    if (strcmp(cookie, "orbitron") == 0)
      value = "orbitron";
    else if (strcmp(cookie, "blackout") == 0)
      value = "blackout";
    free(cookie);
  }

  return value;
}

int page_scale_is_valid(const char *value) {
  if (!value)
    return 0;
  return strcmp(value, "80") == 0 || strcmp(value, "85") == 0 ||
         strcmp(value, "90") == 0 || strcmp(value, "95") == 0 ||
         strcmp(value, "100") == 0 || strcmp(value, "110") == 0;
}

const char *get_page_scale(void) {
  char *cookie = get_cookie("pagescale");
  const char *value = "90";

  if (cookie) {
    if (page_scale_is_valid(cookie))
      value = cookie;
    /* the cookie buffer outlives this call only until freed, so copy out */
    if (strcmp(value, "80") == 0)
      value = "80";
    else if (strcmp(value, "85") == 0)
      value = "85";
    else if (strcmp(value, "95") == 0)
      value = "95";
    else if (strcmp(value, "100") == 0)
      value = "100";
    else if (strcmp(value, "110") == 0)
      value = "110";
    else
      value = "90";
    free(cookie);
  }

  return value;
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

void locale_parts(const char *locale, char *lang, size_t lang_size, char *region,
                  size_t region_size) {
  if (lang && lang_size)
    lang[0] = '\0';
  if (region && region_size)
    region[0] = '\0';
  if (!locale || !locale[0])
    return;

  size_t len = 0;
  while (locale[len] && locale[len] != '_' && locale[len] != '-' &&
         len + 1 < (lang ? lang_size : 0))
    len++;
  if (lang && lang_size)
    snprintf(lang, lang_size, "%.*s", (int)len, locale);

  const char *sep = strchr(locale, '_');
  if (!sep)
    sep = strchr(locale, '-');
  if (sep && region && region_size) {
    size_t out = 0;
    for (const char *p = sep + 1; *p && out + 1 < region_size; p++)
      region[out++] = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
    region[out] = '\0';
  }
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

    /* saved preferences from before the engine swap keep working */
    if (strcmp(token, "startpage") == 0)
      token = "bing";
    else if (strcmp(token, "mojeek") == 0)
      token = "brave";

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

char *get_default_search_engine(void) {
  char *cookie = get_cookie("default_engine");
  if (cookie && (strcmp(cookie, "all") == 0 || is_engine_id_enabled(cookie)))
    return cookie;

  free(cookie);
  return strdup("all");
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

void string_matrix_init(StringMatrix *matrix) {
  if (matrix)
    *matrix = (StringMatrix){0};
}

int string_matrix_append(StringMatrix *matrix, const char *const *values,
                         int field_count) {
  if (!matrix || !values || field_count <= 0)
    return -1;

  char **row = calloc((size_t)field_count, sizeof(*row));
  if (!row)
    return -1;

  for (int i = 0; i < field_count; i++) {
    row[i] = strdup(values[i] ? values[i] : "");
    if (!row[i]) {
      for (int j = 0; j < i; j++)
        free(row[j]);
      free(row);
      return -1;
    }
  }

  if (matrix->count == matrix->capacity) {
    int new_capacity = matrix->capacity ? matrix->capacity * 2 : 8;
    char ***new_rows = malloc(sizeof(*new_rows) * (size_t)new_capacity);
    int *new_counts = malloc(sizeof(*new_counts) * (size_t)new_capacity);
    if (!new_rows || !new_counts) {
      free(new_rows);
      free(new_counts);
      for (int i = 0; i < field_count; i++)
        free(row[i]);
      free(row);
      return -1;
    }
    if (matrix->count > 0) {
      memcpy(new_rows, matrix->rows, sizeof(*new_rows) * matrix->count);
      memcpy(new_counts, matrix->field_counts,
             sizeof(*new_counts) * matrix->count);
    }
    free(matrix->rows);
    free(matrix->field_counts);
    matrix->rows = new_rows;
    matrix->field_counts = new_counts;
    matrix->capacity = new_capacity;
  }

  matrix->rows[matrix->count] = row;
  matrix->field_counts[matrix->count] = field_count;
  matrix->count++;
  return 0;
}

void string_matrix_free(StringMatrix *matrix) {
  if (!matrix)
    return;
  free_string_matrix(matrix->rows, matrix->field_counts, matrix->count);
  *matrix = (StringMatrix){0};
}

int string_matrix_build_pagination(int page,
                                   char *(*href_builder)(int page, void *data),
                                   void *data, StringMatrix *out_matrix) {
  enum { PAGER_WINDOW_SIZE = 5 };
  if (!out_matrix || !href_builder)
    return -1;

  string_matrix_init(out_matrix);
  int pager_start = page <= 3 ? 1 : page - 2;
  int pager_end = pager_start + PAGER_WINDOW_SIZE - 1;

  for (int i = page > 1 ? pager_start - 1 : pager_start; i <= pager_end + 1;
       i++) {
    const char *label = NULL;
    const char *class_name = "pagination-btn";
    char page_label[16];
    int target_page = i;

    if (page > 1 && i == pager_start - 1) {
      label = "←";
      class_name = "pagination-btn prev";
      target_page = page - 1;
    } else if (i == pager_end + 1) {
      label = "→";
      class_name = "pagination-btn next";
      target_page = page + 1;
    } else {
      snprintf(page_label, sizeof(page_label), "%d", i);
      label = page_label;
      if (i == page)
        class_name = "pagination-btn pagination-current";
    }

    char *href = href_builder(target_page, data);
    const char *values[LINK_FIELD_COUNT] = {href, label, class_name};
    int result = string_matrix_append(out_matrix, values, LINK_FIELD_COUNT);
    free(href);
    if (result != 0) {
      string_matrix_free(out_matrix);
      return -1;
    }
  }
  return 0;
}

/* Allowlist checks used when a cookie is *written*. Reading already validated
   the theme; writing did not, so this closed the gap in both directions. */
int is_known_theme(const char *theme) {
  if (!theme || !theme[0])
    return 0;
  for (int i = 0; i < themes_count; i++) {
    if (strcmp(theme, themes_list[i]) == 0)
      return 1;
  }
  return 0;
}

int is_known_locale(const char *locale) {
  if (!locale || !locale[0])
    return 0;
  LocaleInfo locales[32];
  int n = beaker_get_all_locales(locales, 32);
  for (int i = 0; i < n; i++) {
    if (strcmp(locale, locales[i].meta.id) == 0)
      return 1;
  }
  return 0;
}
