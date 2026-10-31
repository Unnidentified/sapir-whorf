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
  context_set(&ctx, "title_font", get_title_font());
  /* one key: the templates want the multiplier, and nothing else read the
     percent form (the context has a fixed 32 slots) */
  context_set(&ctx, "page_scale_css", get_page_scale_css());
  CustomTheme custom_theme;
  get_custom_theme(&custom_theme);
  context_set_custom_theme(&ctx, &custom_theme);
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
