#include "Home.h"
#include "../Utility/Utility.h"
#include <beaker.h>
#include <stdlib.h>
#include <string.h>

int home_handler(UrlParams *params) {
  (void)params;
  char *theme = get_theme("");
  char *locale = get_locale(NULL);

  char **themes = NULL;
  int themes_count = 0;
  get_available_themes(&themes, &themes_count);

  TemplateContext ctx = new_context();
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "version", VERSION);
  context_set(&ctx, "git_remote", GIT_REMOTE);
  beaker_set_locale(&ctx, locale);
  char *rendered_html = render_template("home.html", &ctx);
  send_response(rendered_html);

  free(rendered_html);
  free_context(&ctx);
  free(theme);
  free(locale);

  return 0;
}
