#include "Providers/Providers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "Cache/Cache.h"
#include "Config.h"
#include "Scraping/Scraping.h"
#include "Utility/Utility.h"

/* Providers that are not engines. The host is the destination the query is
   sent to, which is the thing a per-provider switch is for; NULL means the
   answer is computed locally and no request leaves the machine. */
static const ProviderRow EXTRA[] = {
    {"translate", "Translate", "api.mymemory.translated.net", PG_ANSWERS, 0, 1},
    {"stocks", "Share prices", "query1.finance.yahoo.com", PG_ANSWERS, 0, 1},
    {"places", "Places", "nominatim.openstreetmap.org", PG_ANSWERS, 0, 1},
    {"dictionary", "Dictionary", "dictionary.cambridge.org", PG_ANSWERS, 0, 1},
    {"currency", "Currency", "api.exchangerate-api.com", PG_ANSWERS, 0, 1},
    {"units", "Unit conversion", NULL, PG_ANSWERS, 0, 0},
    {"colour", "Colour codes", NULL, PG_ANSWERS, 0, 0},
    {"calculator", "Calculator", NULL, PG_ANSWERS, 0, 0},
    /* K5.3: the Reddit lane is its own provider - a row the reader can switch
       off without switching off the engine that fetches it. Its destination is
       reddit.com, reached through a site: operator, so no new third party. */
    {"reddit", "Reddit", "reddit.com", PG_WEB, 0, 1},
    /* K4: video providers are their own group, because a reader may want
       videos on and every other vertical off. */
    {"dailymotion", "Dailymotion", "api.dailymotion.com", PG_VIDEOS, 0, 1},
    /* K7: this one contacts the result sites themselves, so its disclosure is
       the sentence that matters most in the table. */
    {"descriptions", "Page descriptions", "result sites", PG_ANSWERS, 0, 1},
};
enum { EXTRA_COUNT = sizeof(EXTRA) / sizeof(EXTRA[0]) };

int providers_capacity(void) { return ENGINE_COUNT + EXTRA_COUNT; }

const char *provider_group_label(ProviderGroup g) {
  switch (g) {
  case PG_WEB:
    return "Web";
  case PG_ACADEMIC:
    return "Academic and code";
  case PG_ANSWERS:
    return "Answers";
  case PG_VIDEOS:
    return "Videos";
  case PG_BRIDGES:
    return "Bridges and APIs";
  case PG_OPTIN:
    return "Opt-in";
  default:
    return "";
  }
}

const char *provider_id_for_engine(const char *engine_id) {
  if (!engine_id)
    return NULL;
  /* the infobox panel and the engine share one destination */
  if (strcasecmp(engine_id, "wikipedia") == 0)
    return "wikipedia";
  return engine_id;
}

const char *provider_id_for_widget_kind(const char *kind) {
  if (!kind)
    return NULL;
  if (strcmp(kind, "translate") == 0)
    return "translate";
  if (strcmp(kind, "stock") == 0)
    return "stocks";
  if (strcmp(kind, "place") == 0)
    return "places";
  if (strcmp(kind, "calculator") == 0)
    return "calculator";
  return NULL;
}

/* Which group an engine belongs to. The review's groups are functional, not
   verticals: a bridge is a bridge even when it serves web results. */
static ProviderGroup engine_group(const SearchEngine *e) {
  if (e->opt_in)
    return PG_OPTIN;
  if (strcmp(e->id, "searxng") == 0 || strcmp(e->id, "google") == 0 ||
      strcmp(e->id, "google_cse_element") == 0 ||
      e->xpath_result != NULL /* J5.1: a definition is an API, not a scraper */
  )
    return PG_BRIDGES;
  if (e->verticals & (V_ACADEMIC | V_CODE))
    return PG_ACADEMIC;
  return PG_WEB;
}

/* destination host from a URL: strips the scheme and keeps up to the first
   slash. Written into a static table because the row keeps a pointer. */
static char derived_hosts[256][96];

static const char *derive_host(int slot, const char *url) {
  if (!url || !url[0] || slot < 0 || slot >= 256)
    return NULL;
  const char *start = strstr(url, "://");
  start = start ? start + 3 : url;
  size_t len = strcspn(start, "/?#");
  if (len == 0 || len >= sizeof(derived_hosts[0]))
    return NULL;
  memcpy(derived_hosts[slot], start, len);
  derived_hosts[slot][len] = '\0';
  return derived_hosts[slot];
}

int providers_build(ProviderRow *rows, int capacity) {
  int n = 0;
  extern Config global_config;
  for (int g = 0; g < (int)PG_GROUP_COUNT; g++) {
    for (int i = 0; i < ENGINE_COUNT && n < capacity; i++) {
      SearchEngine *e = &ENGINE_REGISTRY[i];
      if ((int)engine_group(e) != g)
        continue;
      rows[n].id = e->id;
      rows[n].label = e->name ? e->name : e->id;
      /* the disclosure is the host only, not a URL template: a YaCy query
         string on a settings row is unreadable and says less */
      if (e->host_header && e->host_header[0])
        rows[n].host = e->host_header;
      else if (strcmp(e->id, "searxng") == 0 && global_config.searxng_url[0])
        rows[n].host = derive_host(n, global_config.searxng_url);
      else if (e->base_url && e->base_url[0])
        rows[n].host = derive_host(n, e->base_url);
      else
        rows[n].host = derive_host(n, e->search_url_fmt);
      rows[n].group = g;
      rows[n].opt_in = e->opt_in;
      rows[n].toggleable = 1;
      n++;
    }
    if (g == PG_ANSWERS) {
      for (int i = 0; i < EXTRA_COUNT && n < capacity; i++)
        rows[n++] = EXTRA[i];
    }
  }
  /* the host field can be a whole URL template; the page shows the host only */
  return n;
}

int provider_id_known(const char *id) {
  if (!id || !id[0])
    return 0;
  for (int i = 0; i < ENGINE_COUNT; i++)
    if (strcasecmp(ENGINE_REGISTRY[i].id, id) == 0)
      return 1;
  for (int i = 0; i < EXTRA_COUNT; i++)
    if (strcasecmp(EXTRA[i].id, id) == 0)
      return 1;
  return 0;
}

int provider_widget_denied(const char *kind) {
  const char *provider = provider_id_for_widget_kind(kind);
  return provider && provider_is_off(provider);
}

int provider_is_lane(const char *id) {
  return id && strcasecmp(id, "reddit") == 0;
}

int provider_uses_network(const char *id) {
  for (int i = 0; i < EXTRA_COUNT; i++)
    if (strcasecmp(EXTRA[i].id, id) == 0)
      return EXTRA[i].host != NULL;
  return 1;
}

static int id_in_list(const char *id, const char *body) {
  char *copy = body ? strdup(body) : NULL;
  if (!copy)
    return 0;

  int found = 0;
  char *saveptr = NULL;
  for (char *tok = strtok_r(copy, "|,", &saveptr); tok;
       tok = strtok_r(NULL, "|,", &saveptr)) {
    while (*tok == ' ' || *tok == '\t')
      tok++;
    if (strcasecmp(tok, id) == 0) {
      found = 1;
      break;
    }
  }
  free(copy);
  return found;
}

int provider_is_off(const char *id) {
  if (!id || !id[0])
    return 0;

  char *cookie = get_cookie("engines");
  if (!cookie || cookie[0] == '\0') {
    free(cookie);
    return 0;
  }

  int off;
  if (strncmp(cookie, "off=", 4) == 0) {
    off = id_in_list(id, cookie + 4);
  } else {
    /* a cookie written before K1.1 is the ENABLED set: absent means off */
    off = !id_in_list(id, cookie);
    /* the old engine names that were renamed in the F round */
    if (off && (strcasecmp(id, "bing") == 0 || strcasecmp(id, "brave") == 0)) {
      const char *was = strcasecmp(id, "bing") == 0 ? "startpage" : "mojeek";
      off = !id_in_list(was, cookie);
    }
  }

  free(cookie);
  return off;
}

ProviderState provider_state(const ProviderRow *row) {
  if (!row)
    return PS_UNAVAILABLE;
  if (row->toggleable && provider_is_off(row->id))
    return PS_OFF;
  if (!row->toggleable)
    return PS_ON;
  /* an opt-in provider is available when the *server* permits it; whether
     this browser has switched it on is a different question, and the page
     shows that as the switch's own state rather than as unavailability */
  if (row->opt_in && !opt_in_providers_available())
    return PS_UNAVAILABLE;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (strcmp(ENGINE_REGISTRY[i].id, row->id) != 0)
      continue;
    if (!ENGINE_REGISTRY[i].enabled)
      return PS_UNAVAILABLE;
    break;
  }
  return PS_ON;
}

int provider_needs_config(const ProviderRow *row) {
  extern Config global_config;
  return row && strcmp(row->id, "searxng") == 0 &&
         global_config.searxng_url[0] == '\0';
}

const char *provider_state_note(const ProviderRow *row) {
  switch (provider_state(row)) {
  case PS_OFF:
    return "Off (by you)";
  case PS_ON:
    return "On";
  default:
    break;
  }
  if (provider_needs_config(row))
    return "Needs searxng_url";
  return "Disabled by the server administrator";
}

const char *provider_opt_in_disclosure(const char *id) {
  (void)id;
  return "Sends your query to this provider and returns material that may be "
         "copyrighted or licensed. Whether you may use it is governed by the "
         "law where you live and by the provider's own terms.";
}
