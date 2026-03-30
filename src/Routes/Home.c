#include "Home.h"
#include "../Utility/Utility.h"
#include <stdlib.h>

int home_handler(UrlParams *params) {
  (void)params;
  char *theme = get_theme("");

  TemplateContext ctx = new_context();
  context_set(&ctx, "theme", theme);
  char *rendered_html = render_template("home.html", &ctx);
  send_response(rendered_html);

  free(rendered_html);
  free_context(&ctx);
  free(theme);

  return 0;
}
