#include "SettingsSave.h"
#include "../Scraping/Scraping.h"
#include "../Utility/Utility.h"
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#define MAX_ENGINE_IDS ENGINE_COUNT

int settings_save_handler(UrlParams *params) {
  const char *theme = "";
  const char *locale = "";
  const char *query = "";
  const char *title_font = "";
  const char *page_scale = "";
  const char *custom[CUSTOM_COLOUR_COUNT] = {NULL};
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
      } else if (strcmp(params->params[i].key, "pagescale") == 0) {
        page_scale = params->params[i].value;
      } else if (strncmp(params->params[i].key, "c_", 2) == 0) {
        static const char *SLOT_KEYS[CUSTOM_COLOUR_COUNT] = {
            "c_bg_main", "c_bg_card", "c_border", "c_text_primary",
            "c_text_secondary", "c_text_muted", "c_accent", "c_text_dim"};
        for (int k = 0; k < CUSTOM_COLOUR_COUNT; k++) {
          if (strcmp(params->params[i].key, SLOT_KEYS[k]) == 0) {
            custom[k] = params->params[i].value;
            break;
          }
        }
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

  /* validate against the real lists before writing: these values were stored
     verbatim, so a crafted link could set an arbitrary theme/locale cookie. */
  if (is_known_theme(theme)) {
    set_cookie("theme", theme, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false, false);
  }
  int have_custom = 1;
  for (int c = 0; c < CUSTOM_COLOUR_COUNT; c++) {
    if (!custom[c])
      have_custom = 0;
  }
  if (have_custom) {
    char palette[80];
    palette[0] = '\0';
    for (int c = 0; c < CUSTOM_COLOUR_COUNT; c++) {
      const char *hex = custom[c][0] == '#' ? custom[c] + 1 : custom[c];
      if (strlen(hex) != 6)
        continue;
      size_t used = strlen(palette);
      if (c > 0 && used + 1 < sizeof(palette))
        palette[used++] = '-', palette[used] = '\0';
      if (used + strlen(hex) < sizeof(palette))
        memcpy(palette + used, hex, strlen(hex) + 1);
    }
    if (strlen(palette) == 8 * 6 + 7)
      set_cookie("customtheme", palette, "Fri, 31 Dec 2038 23:59:59 GMT", "/",
                 false, false);
  }

  if (strcmp(title_font, "mono") == 0 || strcmp(title_font, "orbitron") == 0 ||
      strcmp(title_font, "blackout") == 0) {
    set_cookie("titlefont", title_font, "Fri, 31 Dec 2038 23:59:59 GMT", "/",
               false, false);
  }
  if (page_scale_is_valid(page_scale)) {
    set_cookie("pagescale", page_scale, "Fri, 31 Dec 2038 23:59:59 GMT", "/",
               false, false);
  }
  if (is_known_locale(locale)) {
    set_cookie("locale", locale, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false, false);
  }

  if (engines_present) {
    char cookie_value[512];
    cookie_value[0] = '\0';
    for (int i = 0; i < selected_count; i++) {
      size_t used = strlen(cookie_value);
      if (i > 0 && used + 1 < sizeof(cookie_value))
        cookie_value[used++] = ',', cookie_value[used] = '\0';
      if (used + strlen(selected_ids[i]) < sizeof(cookie_value))
        memcpy(cookie_value + used, selected_ids[i],
               strlen(selected_ids[i]) + 1);
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

  /* The decoded query went straight into Location. CR/LF in a header value is
     response splitting, and unescaped characters break the redirect. Encode
     everything, and drop any control character first. */
  char safe_query[256];
  size_t qlen = strlen(query);
  if (qlen >= sizeof(safe_query))
    qlen = sizeof(safe_query) - 1;
  size_t qo = 0;
  for (size_t i = 0; i < qlen; i++) {
    unsigned char ch = (unsigned char)query[i];
    if (ch < 0x20 || ch == 0x7F)
      continue; /* strip CR, LF, NUL and friends */
    safe_query[qo++] = (char)ch;
  }
  safe_query[qo] = '\0';

  char encoded_query[768];
  encoded_query[0] = '\0';
  CURL *curl = curl_easy_init();
  if (curl) {
    char *esc = curl_easy_escape(curl, safe_query, 0);
    if (esc) {
      snprintf(encoded_query, sizeof(encoded_query), "%s", esc);
      curl_free(esc);
    }
    curl_easy_cleanup(curl);
  }

  char redirect_url[1024];
  snprintf(redirect_url, sizeof(redirect_url), "/settings?q=%s", encoded_query);
  send_redirect(redirect_url);

  return 0;
}
