#include "Settings.h"
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

  TemplateContext ctx = new_context();
  beaker_set_locale(&ctx, locale);
  context_set(&ctx, "query", query);
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "locale", locale);
  context_set_array_of_arrays(&ctx, "locales", locale_data, locale_count, inner_counts);

  for (int i = 0; i < locale_count; i++) {
    free(locale_data[i]);
  }

  char *rendered_html = render_template("settings.html", &ctx);
  send_response(rendered_html);

  free(rendered_html);
  free(theme);
  free(locale);
  free_context(&ctx);

  return 0;
}
