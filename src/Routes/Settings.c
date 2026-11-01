#include "Settings.h"
#include "Accounts.h"
#include "../Config.h"
#include "../Scraping/Scraping.h"
#include "../Scraping/EngineHealth.h"
#include "../Providers/Providers.h"
#include "../Scraping/Impersonate.h"
#include "../Utility/Utility.h"
#include <beaker.h>
#include <stdlib.h>
#include <string.h>

static char *format_theme_name(const char *theme) {
  char *label = strdup(theme ? theme : "");
  if (!label)
    return NULL;

  int capitalise_next = 1;
  for (size_t i = 0; label[i] != '\0'; i++) {
    if (label[i] == '-') {
      label[i] = ' ';
      capitalise_next = 1;
      continue;
    }
    if (capitalise_next) {
      if (label[i] >= 'a' && label[i] <= 'z')
        label[i] = label[i] - 'a' + 'A';
      capitalise_next = 0;
    }
  }
  return label;
}

int settings_handler(UrlParams *params) {
  const char *query = "";
  const char *toomany = "";
  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0) {
        query = params->params[i].value;
      } else if (strcmp(params->params[i].key, "toomany") == 0) {
        toomany = params->params[i].value;
      }
    }
  }

  char *theme = get_theme("");
  char *locale = get_locale(NULL);

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

  char **user_engines = NULL;
  int user_engine_count = 0;
  int has_user_pref =
      (get_user_engines(&user_engines, &user_engine_count) == 0);
  char *default_engine = get_default_search_engine();

  if (strcmp(default_engine, "all") != 0 && has_user_pref &&
      !user_engines_contains(default_engine, user_engines,
                             user_engine_count)) {
    free(default_engine);
    default_engine = strdup("all");
  }

  int enabled_count = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled)
      enabled_count++;
  }

  char ***engine_data = NULL;
  int *engine_inner = NULL;

  if (enabled_count > 0) {
    engine_data = malloc(sizeof(char **) * enabled_count);
    engine_inner = malloc(sizeof(int) * enabled_count);

    int idx = 0;
    for (int i = 0; i < ENGINE_COUNT; i++) {
      if (!ENGINE_REGISTRY[i].enabled)
        continue;

      int is_selected = !has_user_pref;
      if (has_user_pref) {
        for (int j = 0; j < user_engine_count; j++) {
          if (strcmp(user_engines[j], ENGINE_REGISTRY[i].id) == 0) {
            is_selected = 1;
            break;
          }
        }
      }

      engine_data[idx] = malloc(sizeof(char *) * 4);
      engine_data[idx][0] = (char *)ENGINE_REGISTRY[i].id;
      engine_data[idx][1] = (char *)ENGINE_REGISTRY[i].name;
      /* K1.2b: the box is ticked when the provider is OFF, because the form
         submits only the disabled set - one param for a provider that is off,
         none for one that is on, which keeps the submission under beaker's
         16-parameter limit however large the registry grows. */
      engine_data[idx][2] = is_selected ? "" : "checked";
      engine_data[idx][3] =
          strcmp(default_engine, ENGINE_REGISTRY[i].id) == 0 ? "selected" : "";
      engine_inner[idx] = 4;

      idx++;
    }
  }

  /* K1.3: one table, one row per provider - every entry in the registry plus
     the answer providers that are not engines. The old list only showed what
     the server had enabled, so "off by me", "off by the administrator" and
     "not in this build" all looked the same: absent. */
  ProviderRow provider_rows[MAX_ENGINES + 16];
  int provider_row_count = providers_build(provider_rows, (int)(sizeof(provider_rows) / sizeof(provider_rows[0])));

  int matrix_rows = 0;
  for (int i = 0; i < provider_row_count; i++)
    matrix_rows++; /* the row itself */
  for (int i = 0; i < provider_row_count; i++)
    if (i == 0 || provider_rows[i].group != provider_rows[i - 1].group)
      matrix_rows++; /* a group heading */

  char ***provider_data = malloc(sizeof(char **) * (size_t)matrix_rows);
  int *provider_inner = malloc(sizeof(int) * (size_t)matrix_rows);
  int prow = 0;
  for (int i = 0; i < provider_row_count; i++) {
    ProviderRow *r = &provider_rows[i];
    if (i == 0 || r->group != provider_rows[i - 1].group) {
      provider_data[prow] = malloc(sizeof(char *) * 15);
      provider_data[prow][0] = strdup("group");
      provider_data[prow][1] = strdup("group");
      provider_data[prow][2] = strdup(provider_group_label(r->group));
      for (int c = 3; c < 15; c++)
        provider_data[prow][c] = strdup("");
      /* a heading row draws no control: it is disabled so it never submits,
         and the stylesheet hides the box */
      provider_data[prow][4] = strdup("group");
      provider_data[prow][6] = strdup("");
      provider_data[prow][7] = strdup("disabled");
      /* L1: a heading row carries no data- attributes, so the page's own
         script and tests cannot mistake it for a provider */
      provider_data[prow][12] = strdup("");
      /* 13: the no-script toggle link, empty on a heading */
      provider_data[prow][13] = strdup("");
      provider_data[prow][14] = strdup("");
      provider_inner[prow] = 15;
      prow++;
    }

    ProviderState state = provider_state(r);
    provider_data[prow] = malloc(sizeof(char *) * 15);
    provider_data[prow][0] = strdup("row");
    provider_data[prow][1] = strdup(r->id);
    provider_data[prow][2] = strdup(r->label);
    provider_data[prow][3] = strdup(r->host ? r->host : "");
    /* the state kind, not the sentence: the words live in the locales, and
       colour alone is not a state (the review asked for words) */
    int opted_in = r->opt_in && opt_in_enabled_for(r->id);
    provider_data[prow][4] =
        strdup(!r->toggleable ? "local"
               : state == PS_UNAVAILABLE ? (provider_needs_config(r) ? "needs_config" : "unavailable")
               : r->opt_in ? (opted_in ? "on" : "off")
               : state == PS_ON ? "on"
                                : "off");
    provider_data[prow][5] = strdup("");
    /* L1: the box means "on", always - the same meaning every other checkbox
       on the web has, and the same meaning for an opt-in row as for an engine.
       It used to be the opposite ("tick to switch off"), because the form
       submits the disabled set; but that is a fact about the wire, not about
       what the owner sees. Ticked = this provider will be used.
       Unavailable rows are unticked and disabled. */
    provider_data[prow][6] =
        strdup(r->opt_in ? (opted_in ? "checked" : "")
                         : state == PS_ON ? "checked" : "");
    provider_data[prow][7] = strdup((state == PS_UNAVAILABLE || !r->toggleable) ? "disabled" : "");
    provider_data[prow][8] = strdup(r->toggleable ? "" : "1");
    provider_data[prow][9] = strdup(r->opt_in ? "1" : "");
    provider_data[prow][10] = strdup(r->opt_in ? provider_opt_in_disclosure(r->id) : "");
    /* K1.5: an opt-in provider has ONE control, and it is the opt-in switch -
       a second "off" checkbox for the same row would be two gates with the same
       meaning. The field name says which gate it writes. */
    provider_data[prow][11] = strdup(r->opt_in ? "optin_" : "off_");
    /* L1: the attributes the script and the tests read, built here so a
       heading row can carry none */
    {
      char attrs[192];
      /* beaker's context escapes a value, so the attribute's own quotes would
         arrive as &quot;. Bare words need no quotes at all, and every value
         here is an id or a fixed state name. */
      snprintf(attrs, sizeof(attrs),
               "data-provider=%s data-optin=%s data-on=%s",
               r->id, r->opt_in ? "1" : "",
               !r->toggleable ? "local"
               : state == PS_UNAVAILABLE
                   ? (provider_needs_config(r) ? "needs_config" : "unavailable")
               : r->opt_in ? (opted_in ? "on" : "off")
               : state == PS_ON ? "on"
                                : "off");
      provider_data[prow][12] = strdup(attrs);
    }
    /* 13: the no-script toggle, as the three pieces the template needs - the
       href, the word and whether the row gets one at all. Built here (and not
       as markup) because the context escapes a value, so an embedded <a> would
       arrive as text; and not with a nested {{if}}, because beaker's {{if}}
       does not nest. */
    if (r->toggleable && state != PS_UNAVAILABLE) {
      char href[128];
      snprintf(href, sizeof(href), "/save_settings?toggle=%s", r->id);
      provider_data[prow][13] = strdup(href);
      int is_on = (state == PS_ON) || (r->opt_in && opted_in);
      provider_data[prow][5] =
          strdup(beaker_get_locale_value(locale, is_on ? "toggle_switch_off"
                                                       : "toggle_switch_on"));
    } else {
      provider_data[prow][13] = strdup("");
    }
    /* L4: the sentence beside the box, built here from the reason enum so the
       template does not need five mutually exclusive conditionals */
    {
      ProviderReason reason = provider_reason(r);
      const char *word = provider_state_note(r);
      char sentence[320];
      if (state == PS_ON && word && strcmp(word, "On") != 0) {
        /* on, but not by the best route. The state column stays one word; the
           sentence the reader sees under the label says which route runs. */
        snprintf(sentence, sizeof(sentence),
                 "%s (%s)", beaker_get_locale_value(locale, "provider_state_on"),
                 word + 4 /* skip "On, " */);
      } else if (state == PS_ON) {
        snprintf(sentence, sizeof(sentence), "%s",
                 beaker_get_locale_value(locale, "provider_state_on"));
      }
      else if (state == PS_OFF)
        snprintf(sentence, sizeof(sentence), "%s",
                 beaker_get_locale_value(locale, "provider_state_off"));
      else if (reason == PR_LOCAL)
        snprintf(sentence, sizeof(sentence), "%s",
                 beaker_get_locale_value(locale, "provider_state_local"));
      else
        snprintf(sentence, sizeof(sentence), "%s: %s",
                 beaker_get_locale_value(locale, "provider_state_unavailable"),
                 word ? word : "");
      provider_data[prow][14] = strdup(sentence);
      /* the state column is a word, not a sentence: the template has one slot
         for it and a paragraph does not fit */
      if (state == PS_ON && word && strcmp(word, "On") != 0) {
        char short_state[160];
        snprintf(short_state, sizeof(short_state), "%s",
                 beaker_get_locale_value(locale, "provider_state_on"));
        free(provider_data[prow][14]);
        provider_data[prow][14] = strdup(short_state);
        if (provider_data[prow][10] && provider_data[prow][10][0] == '\0') {
          free(provider_data[prow][10]);
          provider_data[prow][10] = strdup(sentence);
        }
      }
    }
    provider_inner[prow] = 15;
    prow++;
  }

  TemplateContext ctx = new_context();
  beaker_set_locale(&ctx, locale);
  context_set(&ctx, "query", query);
  /* L1/K1.2b: the save refused because the submission was cut off by the
     server's parameter limit. Say so on the page the owner is sent back to,
     instead of the blank 500 they used to get. */
  context_set(&ctx, "too_many_notice", (toomany && toomany[0]) ? "1" : "");
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "title_font", get_title_font());
  /* one key: the templates want the multiplier, and nothing else read the
     percent form (the context has a fixed 32 slots) */
  context_set(&ctx, "page_scale_css", get_page_scale_css());
  CustomTheme custom_theme;
  get_custom_theme(&custom_theme);
  /* the page has nine colour pickers, so it needs the colours individually -
     set as one array key, because nine separate keys did not fit the budget */
  context_set_custom_theme_compact(&ctx, &custom_theme);
  context_set_custom_colours(&ctx, &custom_theme);
  context_set(&ctx, "locale", locale);
  {
    extern Config global_config;
    char mode[16];
    char fanout[16];
    get_answers_mode(global_config.answers_mode, mode, sizeof(mode));
    get_answers_fanout(global_config.answers_fanout, fanout, sizeof(fanout));
    context_set(&ctx, "answers_mode", mode);
    context_set(&ctx, "answers_fanout", fanout);
    /* J5.5 tier 3: three states, because "offered but impossible" is a lie.
         locked  - the server forbids it; the control is not rendered
         off/on  - the server permits it and this browser has/has not opted in */
    /* Two keys instead of three flags: the control's state and the sentence
       that explains it. The page is at the hard context limit (measured 31 of
       32) and three keys for one control is how a page runs out of room.
       "locked" means the server forbids it, so the control is not rendered at
       all; the note is empty unless nothing is registered behind the switch. */
    int permitted = opt_in_providers_available();
    /* one key, four states: the page was at the hard context limit and the
       fourth (a separate note key) pushed engine_status out of it - measured:
       the status table rendered with an empty tbody. "empty" means the server
       permits opt-in providers and this build registers none. */
    context_set(&ctx, "opt_in_state",
                !permitted ? "locked"
                : opt_in_providers_allowed() ? "on"
                : engine_count_opt_in() == 0 ? "empty"
                                             : "off");
  }
  context_set(&ctx, "default_engine", default_engine);
  {
    char now[24];
    snprintf(now, sizeof(now), "%lld", (long long)time(NULL));
    context_set(&ctx, "now_epoch", now);
  }
  context_set_array_of_arrays(&ctx, "locales", locale_data, locale_count,
                              inner_counts);

  char **themes = NULL;
  int themes_count = 0;
  get_available_themes(&themes, &themes_count);

  char ***theme_ptrs = NULL;
  int *theme_inner = NULL;
  if (themes_count > 0) {
    theme_ptrs = malloc(sizeof(char **) * themes_count);
    theme_inner = malloc(sizeof(int) * themes_count);
    for (int i = 0; i < themes_count; i++) {
      theme_ptrs[i] = malloc(sizeof(char *) * 2);
      theme_ptrs[i][0] = themes[i];
      theme_ptrs[i][1] = format_theme_name(themes[i]);
      theme_inner[i] = 2;
    }
    context_set_array_of_arrays(&ctx, "themes", theme_ptrs, themes_count,
                                theme_inner);
  }

  /* one key: the whole provider table, grouped rows and headings together */
  context_set_array_of_arrays(&ctx, "providers", provider_data, matrix_rows,
                              provider_inner);
  {
    int toggleable = 0;
    for (int i = 0; i < provider_row_count; i++)
      if (provider_rows[i].toggleable)
        toggleable++;
    char seen[16];
    snprintf(seen, sizeof(seen), "%d", toggleable);
    context_set(&ctx, "provider_count", seen);
  }

  /* L3: the Accounts and servers cards. One matrix, one context key, and the
     rows are built in C because beaker's {{if}} does not nest. */
  {
    AccountCards cards = accounts_build(locale);
    context_set_array_of_arrays(&ctx, "accounts", cards.rows, cards.count,
                                cards.inner);
    /* the fields inside a card are a "name|label|masked|source|credential"
       string; the template splits nothing, so the fields arrive pre-split */
    accounts_free(&cards);
  }

  if (enabled_count > 0) {
    context_set_array_of_arrays(&ctx, "enabled_engines", engine_data,
                                enabled_count, engine_inner);
    /* K3: the "has lists" flag is gone. The page renders with 32 of the 32
       context slots once beaker's four __locale keys are counted, and adding
       the absolute resume instant pushed engine_status out of it - the table
       rendered with an empty tbody and no error. The status section is now
       guarded by index_groups, which is empty exactly when no engine answered,
       so it carries no extra key. */

    /* J5.5: the editor. One row per engine with its effective weight - the
       configured one, or this browser's override when it has one - so the form
       always shows what routing will actually use. */
    {
      char ***weight_ptrs = malloc(sizeof(char **) * (size_t)enabled_count);
      int *weight_inner = malloc(sizeof(int) * (size_t)enabled_count);
      int rows = 0;
      if (weight_ptrs && weight_inner) {
        char *cookie = get_cookie("engine_weights");
        for (int i = 0; i < ENGINE_COUNT && rows < enabled_count; i++) {
          if (!ENGINE_REGISTRY[i].enabled)
            continue;
          double weight = ENGINE_REGISTRY[i].weight;
          if (cookie && cookie[0]) {
            char needle[48];
            snprintf(needle, sizeof(needle), "%s:", ENGINE_REGISTRY[i].id);
            const char *hit = strstr(cookie, needle);
            if (hit) {
              double parsed = strtod(hit + strlen(needle), NULL);
              if (parsed >= 0.5 && parsed <= 5.0)
                weight = parsed;
            }
          }
          char *value = malloc(16);
          if (!value)
            break;
          snprintf(value, 16, "%g", weight);
          weight_ptrs[rows] = malloc(sizeof(char *) * 2);
          if (!weight_ptrs[rows]) {
            free(value);
            break;
          }
          weight_ptrs[rows][0] = strdup(ENGINE_REGISTRY[i].id);
          weight_ptrs[rows][1] = value;
          weight_inner[rows] = 2;
          rows++;
        }
        free(cookie);
        if (rows > 0)
          context_set_array_of_arrays(&ctx, "engine_weights", weight_ptrs, rows,
                                      weight_inner);
      }
    }
  }

  /* H1: how many independent indexes can answer right now. Three engines that
     resell one index are one source, and a page that only ever hears from one
     index should say so. */
  {
    const char *groups[MAX_ENGINES];
    int group_count = 0;
    for (int i = 0; i < ENGINE_COUNT; i++) {
      if (!ENGINE_REGISTRY[i].enabled)
        continue;
      if (!(ENGINE_REGISTRY[i].verticals & V_WEB))
        continue;
      if (!engine_health_allow(ENGINE_REGISTRY[i].id))
        continue;
      const char *g = ENGINE_REGISTRY[i].index_group;
      if (!g || !g[0])
        continue;
      int seen = 0;
      for (int k = 0; k < group_count; k++)
        if (strcmp(groups[k], g) == 0)
          seen = 1;
      if (!seen && group_count < ENGINE_COUNT)
        groups[group_count++] = g;
    }
    char summary[256];
    int used = snprintf(summary, sizeof(summary), "%d", group_count);
    for (int k = 0; k < group_count && used < (int)sizeof(summary) - 3; k++)
      used += snprintf(summary + used, sizeof(summary) - (size_t)used, "%s%s",
                       k ? ", " : " (", groups[k]);
    if (group_count > 0 && used < (int)sizeof(summary) - 1)
      snprintf(summary + used, sizeof(summary) - (size_t)used, ")");
    context_set(&ctx, "index_groups", summary);
    if (group_count < 2)
      context_set(&ctx, "index_groups_warning",
                  "Only one independent index is answering. Everything you see "
                  "comes from a single source, and its failures are not covered "
                  "by anything else. Add an independent engine (see "
                  "docs/SETUP.md) to change that.");
  }

  /* G2: what the breaker is actually doing, per engine. A pause the user cannot
     see is indistinguishable from a broken search engine - that was the
     complaint behind the rate-limit work. Row: name, state, why, resumes,
     counters, quality, and (K3) the absolute instant it may be retried.
     Declared at function scope because the template reads these pointers after
     the block ends, and so the strings can be freed after the render. */
  char ***status_rows = malloc(sizeof(char **) * enabled_count);
  int *status_inner = malloc(sizeof(int) * enabled_count);
  int status_count = 0;
  {
    for (int i = 0; i < ENGINE_COUNT && status_count < enabled_count; i++) {
      if (!ENGINE_REGISTRY[i].enabled)
        continue;
      EngineHealthEntry *h = engine_health_get(ENGINE_REGISTRY[i].id);
      int left = 0;
      SuspendReason reason =
          engine_health_suspension(ENGINE_REGISTRY[i].id, &left);
      char *why = malloc(48), *resumes = malloc(48), *stats = malloc(96),
           *quality = malloc(24);
      if (!why || !resumes || !stats || !quality) {
        free(why);
        free(resumes);
        free(stats);
        free(quality);
        break;
      }
      snprintf(why, 48, "%s", engine_health_suspend_name(reason));
      /* K3: the absolute instant, formatted once here as the no-script
         fallback. The script reads data-resume-at and counts down; the number
         it counts from is the field the dispatcher tests, not a copy of it. */
      time_t open_until = engine_health_open_until(ENGINE_REGISTRY[i].id);
      char resume_at[24] = "";
      if (reason == SUSPEND_NONE)
        snprintf(resumes, 48, "-");
      else if (left > 0) {
        /* K12.2: the words come from the locale; the numbers are numbers */
        const char *in_word = beaker_get_locale_value(locale, "status_in");
        snprintf(resumes, 48, "%s %dm %02ds", in_word ? in_word : "in",
                 left / 60, left % 60);
        snprintf(resume_at, sizeof(resume_at), "%lld", (long long)open_until);
      } else {
        const char *ready = beaker_get_locale_value(locale, "status_ready");
        snprintf(resumes, 48, "%s", ready ? ready : "ready to retry");
      }
      snprintf(stats, 96, "%lu ok / %lu blocked / %lu errors",
               h ? h->total_ok : 0UL, h ? h->total_blocked : 0UL,
               h ? h->total_errors : 0UL);
      if (h && h->quality_samples > 0)
        snprintf(quality, 24, "%.0f%%", h->quality_ewma * 100.0);
      else
        snprintf(quality, 24, "-");

      char **row = malloc(sizeof(char *) * 9);
      row[0] = strdup(ENGINE_REGISTRY[i].name);
      row[1] = strdup(engine_health_state_name(h ? h->state : HEALTH_CLOSED));
      row[2] = why;
      row[3] = resumes;
      row[4] = stats;
      row[5] = quality;
      row[6] = strdup(resume_at);
      row[7] = strdup(reason == SUSPEND_NONE ? "" : "1");
      /* K6: which TLS fingerprint this engine is using. "stock" is the honest
         answer on a build without libcurl-impersonate, and it is different
         from "chrome136" in a way the owner can read. */
      {
        const char *target =
            impersonate_target_for_engine(ENGINE_REGISTRY[i].impersonate);
        row[8] = strdup(target ? target : "stock");
      }
      status_rows[status_count] = row;
      status_inner[status_count] = 9;
      status_count++;
    }
    if (status_count > 0) {
      context_set_array_of_arrays(&ctx, "engine_status", status_rows,
                                  status_count, status_inner);

    }
  }

  /* the template engine keeps these pointers, so they are freed after render */

  if (getenv("OMNI_PERF")) {
    fprintf(stderr, "[CTX] settings page uses %d of %d context slots:",
            ctx.count, MAX_CONTEXT_VARS);
    for (int kv = 0; kv < ctx.count; kv++)
      fprintf(stderr, " %s", ctx.vars[kv].key);
    fprintf(stderr, "\n");
  }

  char *rendered_html = render_template("settings.html", &ctx);
  send_response(rendered_html);

  for (int i = 0; i < locale_count; i++) {
    free(locale_data[i]);
  }
  if (engine_data) {
    for (int i = 0; i < enabled_count; i++)
      free(engine_data[i]);
    free(engine_data);
  }
  if (status_rows) {
    for (int i = 0; i < status_count; i++) {
      if (!status_rows[i])
        continue;
      for (int c = 0; c < status_inner[i]; c++)
        free(status_rows[i][c]);
      free(status_rows[i]);
    }
    free(status_rows);
  }
  free(status_inner);
  free(engine_inner);
  if (has_user_pref) {
    for (int i = 0; i < user_engine_count; i++)
      free(user_engines[i]);
    free(user_engines);
  }

  if (theme_ptrs) {
    for (int i = 0; i < themes_count; i++) {
      free(theme_ptrs[i][1]);
      free(theme_ptrs[i]);
    }
    free(theme_ptrs);
    free(theme_inner);
  }

  free(rendered_html);
  free(default_engine);
  free(theme);
  free(locale);
  free_context(&ctx);

  return 0;
}
