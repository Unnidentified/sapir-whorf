#include "SettingsSave.h"
#include <stdlib.h>
#include <string.h>

int settings_save_handler(UrlParams *params) {
  const char *theme = "";
  const char *query = "";

  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "theme") == 0) {
        theme = params->params[i].value;
      } else if (strcmp(params->params[i].key, "q") == 0) {
        query = params->params[i].value;
      }
    }
  }

  if (strlen(theme) > 0) {
    set_cookie("theme", theme, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false, false);
  }

  char redirect_url[512];
  snprintf(redirect_url, sizeof(redirect_url), "/settings?q=%s", query);
  send_redirect(redirect_url);

  return 0;
}
