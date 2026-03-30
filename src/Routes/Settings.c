#include "Settings.h"
#include "../Utility/Utility.h"
#include <stdlib.h>
#include <string.h>

int settings_handler(UrlParams *params) {
  const char *query = "";
  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0) {
        query = params->params[i].value;
        break;
      }
    }
  }

  char *theme = get_theme("system");

  TemplateContext ctx = new_context();
  context_set(&ctx, "query", query);
  context_set(&ctx, "theme", theme);
  char *rendered_html = render_template("settings.html", &ctx);
  send_response(rendered_html);

  free(rendered_html);
  free(theme);
  free_context(&ctx);

  return 0;
}
