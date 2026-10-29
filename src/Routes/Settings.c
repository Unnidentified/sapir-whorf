#include "Settings.h"
#include "../Scraping/Scraping.h"
#include "../Utility/Utility.h"
#include <beaker.h>
#include <stdlib.h>
#include <string.h>

static char *format_theme_name(const char *theme) {
  char *label = strdup(theme ? theme : "");
  if (!label)
    return NULL;

  int capitalise_next = 1;
  for (size_t i = 0; label[i] != '\0'; i++) {
    if (label[i] == '-') {
      label[i] = ' ';
      capitalise_next = 1;
      continue;
    }
    if (capitalise_next) {
      if (label[i] >= 'a' && label[i] <= 'z')
        label[i] = label[i] - 'a' + 'A';
      capitalise_next = 0;
    }
  }
  return label;
}

int settings_handler(UrlParams *params) {
  const char *query = "";
  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0) {
        query = params->params[i].value;
      }
    }
  }

  char *theme = get_theme("");
  char *locale = get_locale(NULL);

  LocaleInfo locales[32];
  int locale_count = beaker_get_all_locales(locales, 32);

  char **locale_data[32];
  int inner_counts[32];
  for (int i = 0; i < locale_count; i++) {
    locale_data[i] = malloc(sizeof(char *) * 2);
    locale_data[i][0] = locales[i].meta.id;
    locale_data[i][1] = locales[i].meta.name;
    inner_counts[i] = 2;
  }

  char **user_engines = NULL;
  int user_engine_count = 0;
  int has_user_pref =
      (get_user_engines(&user_engines, &user_engine_count) == 0);
  char *default_engine = get_default_search_engine();

  if (strcmp(default_engine, "all") != 0 && has_user_pref &&
      !user_engines_contains(default_engine, user_engines,
                             user_engine_count)) {
    free(default_engine);
    default_engine = strdup("all");
  }

  int enabled_count = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled)
      enabled_count++;
  }

  char ***engine_data = NULL;
  int *engine_inner = NULL;

  if (enabled_count > 0) {
    engine_data = malloc(sizeof(char **) * enabled_count);
    engine_inner = malloc(sizeof(int) * enabled_count);

    int idx = 0;
    for (int i = 0; i < ENGINE_COUNT; i++) {
      if (!ENGINE_REGISTRY[i].enabled)
        continue;

      int is_selected = !has_user_pref;
      if (has_user_pref) {
        for (int j = 0; j < user_engine_count; j++) {
          if (strcmp(user_engines[j], ENGINE_REGISTRY[i].id) == 0) {
            is_selected = 1;
            break;
          }
        }
      }

      engine_data[idx] = malloc(sizeof(char *) * 4);
      engine_data[idx][0] = (char *)ENGINE_REGISTRY[i].id;
      engine_data[idx][1] = (char *)ENGINE_REGISTRY[i].name;
      engine_data[idx][2] = is_selected ? "checked" : "";
      engine_data[idx][3] =
          strcmp(default_engine, ENGINE_REGISTRY[i].id) == 0 ? "selected" : "";
      engine_inner[idx] = 4;

      idx++;
    }
  }

  TemplateContext ctx = new_context();
  beaker_set_locale(&ctx, locale);
  context_set(&ctx, "query", query);
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "title_font", get_title_font());
  context_set(&ctx, "page_scale", get_page_scale());
  CustomTheme custom_theme;
  get_custom_theme(&custom_theme);
  context_set_custom_theme(&ctx, &custom_theme);
  context_set(&ctx, "locale", locale);
  context_set(&ctx, "default_engine", default_engine);
  context_set_array_of_arrays(&ctx, "locales", locale_data, locale_count,
                              inner_counts);

  char **themes = NULL;
  int themes_count = 0;
  get_available_themes(&themes, &themes_count);

  if (themes_count > 0) {
    char ***theme_ptrs = malloc(sizeof(char **) * themes_count);
    int *theme_inner = malloc(sizeof(int) * themes_count);
    for (int i = 0; i < themes_count; i++) {
      theme_ptrs[i] = malloc(sizeof(char *) * 2);
      theme_ptrs[i][0] = themes[i];
      theme_ptrs[i][1] = format_theme_name(themes[i]);
      theme_inner[i] = 2;
    }
    context_set_array_of_arrays(&ctx, "themes", theme_ptrs, themes_count,
                                theme_inner);
    for (int i = 0; i < themes_count; i++) {
      free(theme_ptrs[i][1]);
      free(theme_ptrs[i]);
    }
    free(theme_ptrs);
    free(theme_inner);
  }

  if (enabled_count > 0) {
    context_set_array_of_arrays(&ctx, "enabled_engines", engine_data,
                                enabled_count, engine_inner);
    context_set(&ctx, "has_enabled_engines", "1");
  }

  /* the template engine keeps these pointers, so they are freed after render */

  char *rendered_html = render_template("settings.html", &ctx);
  send_response(rendered_html);

  for (int i = 0; i < locale_count; i++) {
    free(locale_data[i]);
  }
  if (engine_data) {
    for (int i = 0; i < enabled_count; i++)
      free(engine_data[i]);
    free(engine_data);
  }
  free(engine_inner);
  if (has_user_pref) {
    for (int i = 0; i < user_engine_count; i++)
      free(user_engines[i]);
    free(user_engines);
  }

  free(rendered_html);
  free(default_engine);
  free(theme);
  free(locale);
  free_context(&ctx);

  return 0;
}
