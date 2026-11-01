#include "SettingsSave.h"
#include "../Scraping/Scraping.h"
#include "../Providers/Providers.h"
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
  int engines_seen = 0;
  const char *off_list = NULL; /* K1.3: the single field settings.js writes */
  /* K1.5: the opt-in list, one entry per provider this browser switched on */
  char optin_ids[64][32];
  int optin_count = 0;
  int optin_seen = 0;
  int answers_present = 0;
  int opt_in_present = 0;
  const char *opt_in = NULL;
  const char *answers = NULL;
  const char *answers_fanout = NULL;
  /* must hold the union of engine ids and answer provider ids */
  enum { SELECTED_CAPACITY = MAX_ENGINES + 32 };
  char selected_ids[SELECTED_CAPACITY][32];
  int selected_count = 0;
  /* J5.5: per-engine weights from the form, as "id:weight,id:weight" */
  char weight_spec[512] = "";
  size_t weight_len = 0;
  int weights_present = 0;

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
      } else if (strncmp(params->params[i].key, "w_", 2) == 0) {
        /* one input per engine. An unparseable or out-of-range value is
           dropped rather than clamped: silently changing 99 into 5 would be a
           policy change the reader never asked for. */
        weights_present = 1;
        const char *id = params->params[i].key + 2;
        const char *value = params->params[i].value;
        char *end = NULL;
        double weight = strtod(value, &end);
        if (!id[0] || end == value || weight < 0.5 || weight > 5.0)
          continue;
        if (weight_len + strlen(id) + 16 < sizeof(weight_spec)) {
          weight_len += (size_t)snprintf(weight_spec + weight_len,
                                         sizeof(weight_spec) - weight_len,
                                         "%s%s:%g",
                                         weight_len ? "|" : "", id, weight);
        }
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
      } else if (strcmp(params->params[i].key, "answers") == 0) {
        answers = params->params[i].value;
        answers_present = 1;
      } else if (strcmp(params->params[i].key, "opt_in") == 0) {
        opt_in = params->params[i].value;
      } else if (strcmp(params->params[i].key, "opt_in_present") == 0) {
        opt_in_present = 1;
      } else if (strcmp(params->params[i].key, "answers_fanout") == 0) {
        answers_fanout = params->params[i].value;
      } else if (strcmp(params->params[i].key, "engines_present") == 0) {
        engines_present = 1;
      } else if (strcmp(params->params[i].key, "engines_seen") == 0) {
        /* K1.2b: how many rows the page rendered. If the number of parameters
           reached beaker's limit the submission may have been truncated, and a
           truncated submission would silently re-enable providers. */
        engines_present = 1;
        engines_seen = atoi(params->params[i].value);
      } else if (strncmp(params->params[i].key, "optin_", 6) == 0) {
        /* K1.5: written only when the form was on the page and the server
           permits opt-in providers at all */
        optin_seen = 1;
        if (strcmp(params->params[i].value, "1") == 0 &&
            optin_count < 64) {
          const char *pid = params->params[i].key + 6;
          if (provider_id_known(pid)) {
            strncpy(optin_ids[optin_count], pid,
                    sizeof(optin_ids[optin_count]) - 1);
            optin_ids[optin_count][sizeof(optin_ids[optin_count]) - 1] = '\0';
            optin_count++;
          }
        }
      } else if (strcmp(params->params[i].key, "off") == 0) {
        off_list = params->params[i].value;
      } else if (strncmp(params->params[i].key, "off_", 4) == 0 &&
                 strcmp(params->params[i].value, "1") == 0) {
        /* K1.2b: the engines form submits the providers that are OFF, so the
           parameter count stays flat as the registry grows. beaker keeps only
           MAX_URL_PARAMS (16) and stops parsing silently beyond that. */
        const char *engine_id = params->params[i].key + 4;
        if (engine_id[0] != '\0' && provider_id_known(engine_id) &&
            selected_count < SELECTED_CAPACITY) {
          strncpy(selected_ids[selected_count], engine_id,
                  sizeof(selected_ids[selected_count]) - 1);
          selected_ids[selected_count][sizeof(selected_ids[selected_count]) - 1] =
              '\0';
          selected_count++;
        }
      }
    }
  }

  /* K1.3: the single off= field. Same format the cookie uses, so the read path
     is the same one. Parsed here rather than in the loop above because one
     value carries the whole list. */
  if (off_list && off_list[0]) {
    char *copy = strdup(off_list);
    if (copy) {
      char *saveptr = NULL;
      for (char *tok = strtok_r(copy, "|,", &saveptr); tok;
           tok = strtok_r(NULL, "|,", &saveptr)) {
        while (*tok == ' ' || *tok == '\t')
          tok++;
        if (tok[0] == '\0' || !provider_id_known(tok))
          continue;
        int already = 0;
        for (int j = 0; j < selected_count; j++)
          if (strcmp(selected_ids[j], tok) == 0)
            already = 1;
        if (already)
          continue;
        if (selected_count < SELECTED_CAPACITY) {
          strncpy(selected_ids[selected_count], tok,
                  sizeof(selected_ids[selected_count]) - 1);
          selected_ids[selected_count][sizeof(selected_ids[selected_count]) - 1] = '\0';
          selected_count++;
        } else {
          /* refuse to write a partial policy: the buffer sizes must grow with
             the registry, not the other way round */
          off_list = NULL;
          selected_count = 0;
          set_cookie("engines", "", "Fri, 31 Dec 2038 23:59:59 GMT", "/", false,
                     false);
          fprintf(stderr, "[WARN] engines save refused: more than %d providers "
                          "disabled at once\n", SELECTED_CAPACITY);
          break;
        }
      }
      free(copy);
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
  /* J3c: what leaves the machine is a per-user choice, so these are cookies.
     Both values are validated here -- they end up in string comparisons. */
  /* J5.5 tier 3: the per-user half of the opt-in gate. A form that does not
     carry the control (the server forbids it, so it was not rendered) must not
     silently clear an existing choice, and nothing here can override the
     server -- opt_in_providers_allowed() asks the config as well. */
  if (opt_in_present) {
    /* the hidden opt_in_present says the control was on the page; the checkbox
       says what it now is. Its absence means unchecked, which means "off". */
    const char *value = (opt_in && strcmp(opt_in, "1") == 0) ? "1" : "0";
    set_cookie("opt_in", value, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false,
               false);
  }
  if (answers_present && answers_mode_ok(answers)) {
    set_cookie("answers", answers, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false,
               false);
  }
  if (answers_present && answers_fanout_ok(answers_fanout)) {
    set_cookie("answers_fanout", answers_fanout,
               "Fri, 31 Dec 2038 23:59:59 GMT", "/", false, false);
  }

  /* J5.5: written only when the form carried weight inputs, so an unrelated
     save does not wipe the reader's policy. An empty spec is a real request -
     it means "back to the configured weights". */
  /* beaker drops a cookie whose value contains a comma (measured: the header
     never appeared), so pairs are joined with '|' rather than ','. */
  if (weights_present)
    set_cookie("engine_weights", weight_spec, "Fri, 31 Dec 2038 23:59:59 GMT",
               "/", false, false);

  if (engines_present && engines_seen > 0 && params &&
      params->count >= 16 && engines_seen > params->count) {
    /* the form was cut off by the server's parameter limit; refuse rather
       than write a partial policy that turns providers back on */
    fprintf(stderr,
            "[WARN] engines save refused: %d parameters reached a limit of 16 "
            "while %d providers were shown\n",
            params->count, engines_seen);
    send_status("500 Internal Server Error");
    send_response(
        "<h1>Too many settings at once</h1><p>The form exceeded the server's "
        "parameter limit, so nothing was saved. Reload the settings page and "
        "save fewer providers at a time.</p>");
    return 0;
  }

  if (engines_present) {
    /* K1.1: two changes, both of them consequences of the same report.

       1. The cookie is joined with '|'. Beaker silently drops a cookie whose
          value contains a comma - measured while building the weights editor -
          so "two engines selected" saved nothing at all while one worked. The
          read path still accepts commas, because cookies already in browsers
          use them.
       2. It stores the DISABLED set, not the enabled one. A provider added in
          a later release then defaults to on without the cookie having to know
          it exists, and the value stays short as the registry grows. */
    /* selected_ids now carries the providers that were ticked OFF, so the
       cookie is simply that list. */
    char cookie_value[1024];
    int written = snprintf(cookie_value, sizeof(cookie_value), "off=");
    int overflow = 0;
    for (int i = 0; i < selected_count && !overflow; i++) {
      size_t need = strlen(selected_ids[i]) + 2; /* separator + NUL */
      if ((size_t)written + need > sizeof(cookie_value)) {
        overflow = 1;
        break;
      }
      if (i > 0)
        cookie_value[written++] = '|';
      written += (int)snprintf(cookie_value + written,
                               sizeof(cookie_value) - (size_t)written, "%s",
                               selected_ids[i]);
      if (written < 0)
        overflow = 1;
    }
    if (overflow) {
      /* fail loudly: a truncated list would silently re-enable providers the
         owner turned off, which is the same class of bug as the comma */
      fprintf(stderr,
              "[WARN] engines cookie would overflow (%d providers); not saved. "
              "Raise the buffer before adding more providers.\n",
              selected_count);
    } else {
      set_cookie("engines", cookie_value, "Fri, 31 Dec 2038 23:59:59 GMT", "/",
                 false, false);
    }
  }

  if (optin_seen && opt_in_providers_available()) {
    char value[1024];
    int written = 0;
    for (int i = 0; i < optin_count; i++) {
      int need = (int)strlen(optin_ids[i]) + (i > 0 ? 1 : 0);
      if (written + need + 1 > (int)sizeof(value))
        break;
      if (i > 0)
        value[written++] = '|';
      written += snprintf(value + written, sizeof(value) - (size_t)written,
                          "%s", optin_ids[i]);
    }
    value[written] = '\0';
    set_cookie("optin", value, "Fri, 31 Dec 2038 23:59:59 GMT", "/", false,
               false);
    /* the old single cookie said "all of them"; leaving it would re-enable
       everything the moment this list is emptied */
    set_cookie("opt_in", "0", "Thu, 01 Jan 1970 00:00:00 GMT", "/", false,
               false);
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
          selected = 0; /* listed as off */
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
