#include "History.h"
#include "../Utility/Utility.h"
#include <beaker.h>
#include <stdlib.h>
#include <string.h>

int history_handler(UrlParams *params) {
  char *query = "";
  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0)
        query = params->params[i].value;
    }
  }

  char *theme = get_theme("");
  char *locale = get_locale(NULL);

  TemplateContext ctx = new_context();
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "title_font", get_title_font());
  CustomTheme custom_theme;
  get_custom_theme(&custom_theme);
  context_set_custom_theme(&ctx, &custom_theme);
  context_set(&ctx, "query", query);
  context_set(&ctx, "chrome_action", "/search");
  context_set(&ctx, "chrome_active", "");
  context_set(&ctx, "chrome_engine", "");
  context_set(&ctx, "chrome_all_href", "/");
  context_set(&ctx, "version", VERSION);
  context_set(&ctx, "git_remote", GIT_REMOTE);
  beaker_set_locale(&ctx, locale);
  char *rendered_html = render_template("history.html", &ctx);
  send_response(rendered_html);

  free(rendered_html);
  free_context(&ctx);
  free(theme);
  free(locale);

  return 0;
}
