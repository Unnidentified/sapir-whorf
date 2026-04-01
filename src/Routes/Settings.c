#include "Settings.h"
#include "../Scraping/Scraping.h"
#include "../Utility/Utility.h"
#include <beaker.h>
#include <stdlib.h>
#include <string.h>

int settings_handler(UrlParams *params) {
  const char *query = "";
  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0) {
        query = params->params[i].value;
      }
    }
  }

  char *theme = get_theme("system");
  char *locale = get_locale("en_uk");

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
  int has_user_pref = (get_user_engines(&user_engines, &user_engine_count) == 0);

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

      engine_data[idx] = malloc(sizeof(char *) * 3);
      engine_data[idx][0] = (char *)ENGINE_REGISTRY[i].id;
      engine_data[idx][1] = (char *)ENGINE_REGISTRY[i].name;
      engine_data[idx][2] = is_selected ? "checked" : "";
      engine_inner[idx] = 3;

      idx++;
    }
  }

  TemplateContext ctx = new_context();
  beaker_set_locale(&ctx, locale);
  context_set(&ctx, "query", query);
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "locale", locale);
  context_set_array_of_arrays(&ctx, "locales", locale_data, locale_count, inner_counts);

  if (enabled_count > 0) {
    context_set_array_of_arrays(&ctx, "enabled_engines", engine_data,
                                enabled_count, engine_inner);
    context_set(&ctx, "has_enabled_engines", "1");
  }

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

  char *rendered_html = render_template("settings.html", &ctx);
  send_response(rendered_html);

  free(rendered_html);
  free(theme);
  free(locale);
  free_context(&ctx);

  return 0;
}
