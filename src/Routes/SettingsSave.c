#include "SettingsSave.h"
#include "../Scraping/Scraping.h"
#include "../Utility/Utility.h"
#include <stdlib.h>
#include <string.h>

#define MAX_ENGINE_IDS ENGINE_COUNT

int settings_save_handler(UrlParams *params) {
  const char *theme = "";
  const char *locale = "";
  const char *query = "";
  const char *title_font = "";
  const char *default_engine = "all";
  int default_engine_present = 0;
  int engines_present = 0;
  char selected_ids[ENGINE_COUNT][32];
  int selected_count = 0;

  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "theme") == 0) {
        theme = params->params[i].value;
      } else if (strcmp(params->params[i].key, "locale") == 0) {
        locale = params->params[i].value;
      } else if (strcmp(params->params[i].key, "titlefont") == 0) {
        title_font = params->params[i].value;
      } else if (strcmp(params->params[i].key, "q") == 0) {
        query = params->params[i].value;
      } else if (strcmp(params->params[i].key, "default_engine") == 0) {
        default_engine = params->params[i].value;
        default_engine_present = 1;
      } else if (strcmp(params->params[i].key, "engines_present") == 0) {
        engines_present = 1;
      } else if (strncmp(params->params[i].key, "engine_", 7) == 0 &&
                 strcmp(params->params[i].value, "1") == 0) {
        const char *engine_id = params->params[i].key + 7;
        if (engine_id[0] != '\0' && is_engine_id_enabled(engine_id) &&
            selected_count < ENGINE_COUNT) {
          strncpy(selected_ids[selected_count], engine_id,
                  sizeof(selected_ids[selected_count]) - 1);
          selected_ids[selected_count][sizeof(selected_ids[selected_count]) - 1] =
              '\0';
          selected_count++;
        }
      }
    }
  }

  if (strlen(theme) > 0) {
    set_cookie("theme", theme, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false, false);
  }
  if (strcmp(title_font, "mono") == 0 || strcmp(title_font, "orbitron") == 0 ||
      strcmp(title_font, "blackout") == 0) {
    set_cookie("titlefont", title_font, "Fri, 31 Dec 2038 23:59:59 GMT", "/",
               false, false);
  }
  if (strlen(locale) > 0) {
    set_cookie("locale", locale, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false, false);
  }

  if (engines_present) {
    char cookie_value[512];
    cookie_value[0] = '\0';
    for (int i = 0; i < selected_count; i++) {
      if (i > 0)
        strcat(cookie_value, ",");
      strcat(cookie_value, selected_ids[i]);
    }
    set_cookie("engines", cookie_value, "Fri, 31 Dec 2038 23:59:59 GMT", "/",
               false, false);
  }

  if (default_engine_present) {
    const char *validated_default = "all";

    for (int i = 0; i < ENGINE_COUNT; i++) {
      if (!ENGINE_REGISTRY[i].enabled ||
          strcmp(default_engine, ENGINE_REGISTRY[i].id) != 0)
        continue;

      int selected = !engines_present;
      for (int j = 0; j < selected_count; j++) {
        if (strcmp(selected_ids[j], ENGINE_REGISTRY[i].id) == 0) {
          selected = 1;
          break;
        }
      }

      if (selected)
        validated_default = ENGINE_REGISTRY[i].id;
      break;
    }

    set_cookie("default_engine", validated_default,
               "Fri, 31 Dec 2038 23:59:59 GMT", "/", false, false);
  }

  char redirect_url[512];
  snprintf(redirect_url, sizeof(redirect_url), "/settings?q=%s", query);
  send_redirect(redirect_url);

  return 0;
}
