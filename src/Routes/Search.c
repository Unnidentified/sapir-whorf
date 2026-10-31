#include "Search.h"
#include "../Cache/Cache.h"
#include "../Infobox/Calculator.h"
#include "../Infobox/ColourCode.h"
#include "../Infobox/CurrencyConversion.h"
#include "../Infobox/Dictionary.h"
#include "../Infobox/UnitConversion.h"
#include "../Infobox/Wikipedia.h"
#include "../Limiter/RateLimit.h"
#include "../Scraping/EngineHealth.h"
#include "../Infobox/AnswerBox.h"
#include "../Query/QueryAnalysis.h"
#include "../Query/Tokens.h"
#include "../Ranking/Ranker.h"
#include "../Scraping/Scraping.h"
#include "../Utility/Display.h"
#include "../Utility/Unescape.h"
#include "../Utility/Utility.h"
#include "Config.h"
#include <ctype.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


typedef struct {
  const char *query;
  char lang[8];
  InfoBox result;
  int success;
} InfoBoxThreadData;

typedef struct {
  /* H4: the id ties a rail handler to the answer-box table, so the priority
     lives in one place instead of in a second hardcoded list here. */
  const char *id;
  int (*check_fn)(const char *query);
  InfoBox (*fetch_fn)(char *query);
  char *(*url_construct_fn)(const char *query);
} InfoBoxHandler;

enum {
  RESULT_FIELD_COUNT = 6,
};

static InfoBox fetch_wiki_wrapper(char *query) {
  char *url = construct_wiki_url(query);
  if (!url)
    return (InfoBox){NULL};
  InfoBox result = fetch_wiki_data(url);
  free(url);
  return result;
}

static InfoBox fetch_wiki_wrapper_lang(char *query, const char *lang) {
  char *url = construct_wiki_url_lang(query, lang);
  if (!url)
    return (InfoBox){NULL};
  InfoBox result = fetch_wiki_data(url);
  free(url);
  return result;
}

static int always_true(const char *query) {
  (void)query;
  return 1;
}

static InfoBox fetch_dict_wrapper(char *query) {
  return fetch_dictionary_data(query);
}
static InfoBox fetch_calc_wrapper(char *query) {
  return fetch_calc_data(query);
}
static InfoBox fetch_unit_wrapper(char *query) {
  return fetch_unit_conv_data(query);
}
static InfoBox fetch_currency_wrapper(char *query) {
  return fetch_currency_data(query);
}
char *get_base_url(const char *input) {
  if (!input)
    return NULL;

  const char *start = input;

  const char *protocol_pos = strstr(input, "://");
  if (protocol_pos) {
    start = protocol_pos + 3;
  }

  const char *end = start;
  while (*end && *end != '/' && *end != '?' && *end != '#') {
    end++;
  }

  size_t len = end - start;

  char *domain = (char *)malloc(len + 1);
  if (!domain)
    return NULL;

  strncpy(domain, start, len);
  domain[len] = '\0';

  return domain;
}
static int url_host_matches(const char *url, const char *host) {
  if (!url || !host)
    return 0;

  const char *start = url;
  const char *protocol_pos = strstr(url, "://");
  if (protocol_pos)
    start = protocol_pos + 3;

  size_t host_len = strlen(host);
  if (strncmp(start, host, host_len) == 0 &&
      (start[host_len] == '/' || start[host_len] == '\0' ||
       start[host_len] == '?' || start[host_len] == ':'))
    return 1;

  if (strncmp(start, "www.", 4) == 0 &&
      strncmp(start + 4, host, host_len) == 0 &&
      (start[4 + host_len] == '/' || start[4 + host_len] == '\0' ||
       start[4 + host_len] == '?' || start[4 + host_len] == ':'))
    return 1;

  return 0;
}

/* ---------- result ranking ----------
   Every engine contributes a position; results are merged by Reciprocal Rank
   Fusion so a hit that several engines agree on outranks one that only a
   single engine liked. Duplicates are matched on a normalised URL, and at
   most MAX_PER_DOMAIN results survive per host so one site cannot fill the
   page. */

#define MAX_CANDIDATES 128
#define MAX_PER_DOMAIN 3
#define MAX_PER_DOMAIN_RELAXED 5
#define MIN_WEB_ENGINES 3
#define RRF_K 60.0

typedef RankDoc ResultCandidate;

static int prefix_ci(const char *s, const char *prefix) {
  while (*prefix) {
    char a = *s++;
    char b = *prefix++;
    if (a >= 'A' && a <= 'Z')
      a += 32;
    if (b >= 'A' && b <= 'Z')
      b += 32;
    if (a != b)
      return 0;
  }
  return 1;
}

static int is_month_abbrev(const char *s) {
  static const char *MONTHS[] = {"jan", "feb", "mar", "apr", "may", "jun",
                                 "jul", "aug", "sep", "oct", "nov", "dec"};
  for (size_t i = 0; i < sizeof(MONTHS) / sizeof(MONTHS[0]); i++) {
    if (prefix_ci(s, MONTHS[i]))
      return 1;
  }
  return 0;
}

/* Bing and Brave prefix snippets with "May 1, 2017 · " or "3 weeks ago - " */
static const char *skip_date_prefix(const char *s) {
  const char *p = s;
  while (*p == ' ')
    p++;

  if (is_month_abbrev(p)) {
    const char *q = p;
    while (*q && *q != ' ')
      q++;
    while (*q == ' ')
      q++;
    if (*q < '0' || *q > '9')
      return s;
    while (*q >= '0' && *q <= '9')
      q++;
    if (*q == ',')
      q++;
    while (*q == ' ')
      q++;
    int digits = 0;
    while (*q >= '0' && *q <= '9') {
      q++;
      digits++;
    }
    if (digits < 4)
      return s;
    while (*q && !(*q >= '0' && *q <= '9') && !(*q >= 'a' && *q <= 'z') &&
           !(*q >= 'A' && *q <= 'Z'))
      q++;
    return q;
  }

  if (*p >= '0' && *p <= '9') {
    static const char *UNITS[] = {"day", "week", "month", "year",
                                  "hour", "min", "minute"};
    const char *q = p;
    while (*q >= '0' && *q <= '9')
      q++;
    while (*q == ' ')
      q++;
    for (size_t i = 0; i < sizeof(UNITS) / sizeof(UNITS[0]); i++) {
      if (prefix_ci(q, UNITS[i])) {
        const char *r = q;
        while (*r && *r != ' ')
          r++;
        while (*r && !(*r >= '0' && *r <= '9') && !(*r >= 'a' && *r <= 'z') &&
               !(*r >= 'A' && *r <= 'Z'))
          r++;
        if (*r)
          return r;
        return s;
      }
    }
  }
  return s;
}

/* engines occasionally hand back raw entities ("OpenBSD&apos;s") */
static void unescape_html(const char *in, char *out, size_t size) {
  out[0] = '\0';
  if (!in || size == 0)
    return;

  size_t o = 0;
  for (const char *p = in; *p && o + 1 < size; p++) {
    if (*p != '&') {
      out[o++] = *p;
      continue;
    }

    const char *semi = strchr(p, ';');
    if (!semi || semi - p > 10) {
      out[o++] = *p;
      continue;
    }

    char entity[12];
    size_t len = (size_t)(semi - p + 1);
    memcpy(entity, p, len);
    entity[len] = '\0';

    char decoded = 0;
    if (strcmp(entity, "&amp;") == 0) decoded = '&';
    else if (strcmp(entity, "&lt;") == 0) decoded = '<';
    else if (strcmp(entity, "&gt;") == 0) decoded = '>';
    else if (strcmp(entity, "&quot;") == 0) decoded = '"';
    else if (strcmp(entity, "&apos;") == 0) decoded = '\'';
    else if (strcmp(entity, "&nbsp;") == 0) decoded = ' ';
    else if (entity[1] == '#') {
      int value = 0, ok = 1;
      if (entity[2] == 'x' || entity[2] == 'X') {
        for (const char *q = entity + 3; *q != ';'; q++) {
          if (*q >= '0' && *q <= '9') value = value * 16 + (*q - '0');
          else if ((*q | 32) >= 'a' && (*q | 32) <= 'f') value = value * 16 + (*q | 32) - 'a' + 10;
          else { ok = 0; break; }
        }
      } else {
        for (const char *q = entity + 2; *q != ';'; q++) {
          if (*q >= '0' && *q <= '9') value = value * 10 + (*q - '0');
          else { ok = 0; break; }
        }
      }
      if (ok && value > 0 && value < 128)
        decoded = (char)value;
    }

    if (decoded) {
      out[o++] = decoded;
      p = semi;
    } else {
      out[o++] = *p;
    }
  }
  out[o] = '\0';
}

static void clean_snippet(const char *in, char *out, size_t size) {
  out[0] = '\0';
  if (!in || size == 0)
    return;

  const char *p = skip_date_prefix(in);
  size_t o = 0;
  int pending_space = 0;
  for (; *p && o + 1 < size; p++) {
    char c = *p;
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      pending_space = 1;
      continue;
    }
    if (pending_space && o > 0)
      out[o++] = ' ';
    pending_space = 0;
    out[o++] = c;
  }
  out[o] = '\0';

  char decoded[BUFFER_SIZE_LARGE];
  snprintf(decoded, sizeof(decoded), "%s", out);
  for (int pass = 0; pass < 3; pass++) {
    char once[BUFFER_SIZE_LARGE];
    unescape_html(decoded, once, sizeof(once));
    if (strcmp(once, decoded) == 0)
      break;
    snprintf(decoded, sizeof(decoded), "%s", once);
  }
  snprintf(out, size, "%s", decoded);
}

/* ---------- query terms ---------- */

static const char *QUERY_STOPWORDS[] = {
    "the", "and", "for", "with", "how", "what", "when", "where", "who",
    "why", "that", "this", "from", "into", "your", "you", "are", "was",
    "were", "has", "have", "had", "not", "but", "all", "can", "will",
    "its", "his", "her", "they", "them", "then", "than", "about", "over"};

static int is_stopword(const char *word) {
  for (size_t i = 0; i < sizeof(QUERY_STOPWORDS) / sizeof(QUERY_STOPWORDS[0]);
       i++) {
    if (strcmp(word, QUERY_STOPWORDS[i]) == 0)
      return 1;
  }
  return 0;
}

/* J3a: the third tokenizer is gone; this is the shared one plus the stopword
   filter the infobox gate wants. */
static int tokenize_query(const char *query, char tokens[][32], int max) {
  char raw[16][TOKEN_LEN];
  int n = tokens_split(query, raw, 16, 1);
  int count = 0;
  for (int i = 0; i < n && count < max; i++) {
    if (is_stopword(raw[i]))
      continue;
    snprintf(tokens[count++], TOKEN_LEN, "%s", raw[i]);
  }
  return count;
}

static int contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || !*needle)
    return 0;

  size_t n = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    size_t i = 0;
    while (i < n && p[i]) {
      char a = p[i];
      char b = needle[i];
      if (a >= 'A' && a <= 'Z')
        a += 32;
      if (b >= 'A' && b <= 'Z')
        b += 32;
      if (a != b)
        break;
      i++;
    }
    if (i == n)
      return 1;
  }
  return 0;
}

/* strip characters that break the scrapers and collapse whitespace */
static void sanitize_query(const char *in, char *out, size_t size) {
  out[0] = '\0';
  if (!in || size == 0)
    return;

  size_t o = 0;
  int pending_space = 0;
  for (const char *p = in; *p && o + 1 < size; p++) {
    unsigned char c = (unsigned char)*p;
    if (c < 0x20) {
      pending_space = 1;
      continue;
    }
    if (strchr("<>{}[]|`^~", c))
      continue;
    if (c == ' ') {
      pending_space = 1;
      continue;
    }
    if (pending_space && o > 0)
      out[o++] = ' ';
    pending_space = 0;
    out[o++] = (char)c;
  }
  out[o] = '\0';
}

static int candidate_cmp(const void *a, const void *b) {
  const RankDoc *x = (const RankDoc *)a;
  const RankDoc *y = (const RankDoc *)b;
  if (x->score > y->score)
    return -1;
  if (x->score < y->score)
    return 1;
  return x->order - y->order;
}

static int is_media_result(const char *url) {
  static const char *MEDIA_HOSTS[] = {"youtube.com",      "m.youtube.com",
                                      "youtu.be",         "vimeo.com",
                                      "dailymotion.com",  "twitch.tv",
                                      "soundcloud.com",   "spotify.com",
                                      "music.youtube.com"};

  for (size_t i = 0; i < sizeof(MEDIA_HOSTS) / sizeof(MEDIA_HOSTS[0]); i++) {
    if (url_host_matches(url, MEDIA_HOSTS[i]))
      return 1;
  }
  return 0;
}

static InfoBox fetch_colour_wrapper(char *query) {
  return fetch_colour_data(query);
}

static InfoBoxHandler handlers[] = {
    {"dictionary", is_dictionary_query, fetch_dict_wrapper, NULL},
    {"arithmetic", is_calculator_query, fetch_calc_wrapper, NULL},
    {"units", is_unit_conv_query, fetch_unit_wrapper, NULL},
    {"currency", is_currency_query, fetch_currency_wrapper, NULL},
    {"colour", is_colour_code_query, fetch_colour_wrapper, NULL},
    {"wikipedia", always_true, fetch_wiki_wrapper, construct_wiki_url},
};
enum { HANDLER_COUNT = sizeof(handlers) / sizeof(handlers[0]) };

static int infobox_text_is_meaningful(const char *text) {
  if (!text)
    return 0;

  const char *start = text;
  while (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r')
    start++;

  size_t len = strlen(start);
  while (len > 0) {
    char c = start[len - 1];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
      len--;
    else
      break;
  }

  /* Disambiguation stubs such as "Kirby may refer to:" carry no summary, so
   * the infobox would render as an empty card. Require a real sentence. */
  if (len < 60)
    return 0;

  return 1;
}

static void *infobox_thread_func(void *arg) {
  InfoBoxThreadData *data = (InfoBoxThreadData *)arg;
  int handler_idx = data->success;
  if (handler_idx < 0 || handler_idx >= HANDLER_COUNT)
    return NULL;

  InfoBoxHandler *h = &handlers[handler_idx];
  if (h->check_fn && !h->check_fn(data->query)) {
    data->success = 0;
    return NULL;
  }

  /* A6: these handlers contact other services (Wikipedia, Cambridge
     Dictionary, exchange-rate APIs) on every matching query. "local" keeps the
     calculator, units and colour lookups and sends nothing off the machine. */
  {
    extern Config global_config;
    const char *mode = global_config.infobox_mode;
    if (mode && strcmp(mode, "off") == 0) {
      data->success = 0;
      return NULL;
    }
    int remote = (h->fetch_fn == fetch_wiki_wrapper) ||
                 (h->fetch_fn == fetch_dict_wrapper) ||
                 (h->fetch_fn == fetch_currency_wrapper);
    if (remote && mode && strcmp(mode, "local") == 0) {
      data->success = 0;
      return NULL;
    }
  }

  if (h->fetch_fn == fetch_wiki_wrapper)
    data->result = fetch_wiki_wrapper_lang((char *)data->query, data->lang);
  else
    data->result = h->fetch_fn((char *)data->query);

  int relevant = 1;
  {
    char tokens[8][32];
    int token_count = tokenize_query(data->query, tokens, 8);
    if (token_count > 0) {
      relevant = 0;
      for (int t = 0; t < token_count; t++) {
        if (contains_ci(data->result.title, tokens[t]) ||
            contains_ci(data->result.extract, tokens[t])) {
          relevant = 1;
          break;
        }
      }
    }
  }

  data->success = (data->result.title != NULL &&
                   infobox_text_is_meaningful(data->result.extract) && relevant);
  return NULL;
}

static int add_infobox_to_collection(InfoBox *infobox,
                                     StringMatrix *collection) {
  const char *values[INFOBOX_FIELD_COUNT] = {
      infobox->title, infobox->thumbnail_url, infobox->extract,
      infobox->url,   infobox->url,
  };
  return string_matrix_append(collection, values, INFOBOX_FIELD_COUNT);
}

static int add_warning_to_collection(const char *engine_name,
                                     const char *warning_message,
                                     StringMatrix *collection) {
  const char *values[] = {engine_name, warning_message};
  return string_matrix_append(collection, values, 2);
}

static void free_user_engine_list(char **user_engines, int user_engine_count) {
  for (int i = 0; i < user_engine_count; i++)
    free(user_engines[i]);
  free(user_engines);
}

static void free_search_results(SearchResult **all_results, ScrapeJob *jobs,
                                int job_count) {
  for (int i = 0; i < job_count; i++) {
    for (int j = 0; all_results[i] && j < jobs[i].results_count; j++) {
      free(all_results[i][j].url);
      free(all_results[i][j].title);
      free(all_results[i][j].snippet);
    }
    free(all_results[i]);
  }
}

static void free_infobox_results(InfoBoxThreadData *data) {
  for (int i = 0; i < HANDLER_COUNT; i++) {
    free_infobox(&data[i].result);
  }
}

static const char *warning_message_for_job(const ScrapeJob *job,
                                           const char *locale) {
  /* rate limits and challenge pages are temporary and self-healing: say so
     instead of reporting a generic failure */
  if (job->http_status == 429 || job->http_status == 202) {
    const char *limited =
        beaker_get_locale_value(locale, "warning_rate_limited");
    if (limited)
      return limited;
  }
  switch (job->status) {
  case SCRAPE_STATUS_FETCH_ERROR: {
    if (job->elapsed_ms >= 4000.0) {
      const char *timeout_msg =
          beaker_get_locale_value(locale, "warning_timeout");
      if (timeout_msg)
        return timeout_msg;
    }
    const char *msg = beaker_get_locale_value(locale, "warning_fetch_error");
    return msg ? msg
               : "request failed before OmniSearch could read search results.";
  }
  case SCRAPE_STATUS_PARSE_MISMATCH: {
    const char *msg = beaker_get_locale_value(locale, "warning_parse_mismatch");
    return msg ? msg
               : "returned search results in a format OmniSearch could not "
                 "parse.";
  }
  case SCRAPE_STATUS_CANCELLED:
    /* we stopped waiting; the source may well have answered a moment later */
    return NULL;
  case SCRAPE_STATUS_BLOCKED: {
    const char *msg = beaker_get_locale_value(locale, "warning_blocked");
    return msg ? msg
               : "returned a captcha or another blocking page instead of "
                 "search results.";
  }
  default:
    return NULL;
  }
}

static int engine_id_matches(const char *left, const char *right) {
  if (!left || !right)
    return 0;

  while (*left && *right) {
    char l = *left;
    char r = *right;

    if (l >= 'A' && l <= 'Z')
      l = l - 'A' + 'a';
    if (r >= 'A' && r <= 'Z')
      r = r - 'A' + 'a';

    if (l != r)
      return 0;

    left++;
    right++;
  }

  return *left == *right;
}

/* G2: describe the engines the breaker has paused, so neither a thin page nor
   an empty one leaves the user guessing. Returns how many are paused. */
static int build_paused_note(char *note, size_t size) {
  int paused = 0;
  int soonest = 0;
  const char *reason_txt = NULL;
  int all_same = 1;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (!ENGINE_REGISTRY[i].enabled)
      continue;
    int left = 0;
    SuspendReason why =
        engine_health_suspension(ENGINE_REGISTRY[i].id, &left);
    if (why == SUSPEND_NONE)
      continue;
    paused++;
    if (!reason_txt)
      reason_txt = engine_health_suspend_name(why);
    else if (strcmp(reason_txt, engine_health_suspend_name(why)) != 0)
      all_same = 0;
    if (soonest == 0 || (left > 0 && left < soonest))
      soonest = left;
  }
  if (paused == 0)
    return 0;
  const char *why = all_same && reason_txt ? reason_txt : "several reasons";
  if (soonest > 0)
    snprintf(note, size,
             "%d source%s paused (%s); earliest retry in %dm %02ds.", paused,
             paused == 1 ? "" : "s", why, soonest / 60, soonest % 60);
  else
    snprintf(note, size,
             "%d source%s paused (%s) and will be retried on the next search.",
             paused, paused == 1 ? "" : "s", why);
  return paused;
}

/* H2: selection weight, adjusted by measured quality. */
static double engine_effective_weight(const SearchEngine *e) {
  if (!e)
    return 0.0;
  return engine_health_effective_weight(e->id, e->weight);
}

static const SearchEngine *find_enabled_engine(const char *engine_id) {
  if (!engine_id || engine_id[0] == '\0' || engine_id_matches(engine_id, "all"))
    return NULL;

  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled &&
        engine_id_matches(ENGINE_REGISTRY[i].id, engine_id)) {
      return &ENGINE_REGISTRY[i];
    }
  }

  return NULL;
}

static int engine_allowed_for_user(const SearchEngine *eng, char **user_ids,
                                   int user_count, int has_pref) {
  if (!has_pref)
    return 1;
  return user_engines_contains(eng->id, user_ids, user_count);
}

static char *build_search_href(const char *query, const char *engine_id,
                               int page) {
  const char *safe_query = query ? query : "";
  int use_engine =
      engine_id && engine_id[0] != '\0' && !engine_id_matches(engine_id, "all");
  /* J3d: encode here, not at the call sites, so no caller can forget */
  char encoded_query[BUFFER_SIZE_MEDIUM];
  url_encode(safe_query, encoded_query, sizeof(encoded_query));
  size_t needed = strlen("/search?q=") + strlen(encoded_query) + 1;

  if (use_engine)
    needed += strlen("&engine=") + strlen(engine_id);
  if (page > 1)
    needed += strlen("&p=") + 16;

  char *href = (char *)malloc(needed);
  if (!href)
    return NULL;

  snprintf(href, needed, "/search?q=%s", encoded_query);

  if (use_engine) {
    strcat(href, "&engine=");
    strcat(href, engine_id);
  }

  if (page > 1) {
    char page_buf[16];
    snprintf(page_buf, sizeof(page_buf), "%d", page);
    strcat(href, "&p=");
    strcat(href, page_buf);
  }

  return href;
}

typedef struct {
  const char *query;
  const char *engine_id;
} SearchHrefData;

static char *search_href_builder(int page, void *data) {
  SearchHrefData *d = (SearchHrefData *)data;
  return build_search_href(d->query, d->engine_id, page);
}

static char *build_search_request_cache_key(const char *query,
                                            const char *engine_id, int page,
                                            const char *client_key) {
  char scope_key[BUFFER_SIZE_MEDIUM];
  snprintf(scope_key, sizeof(scope_key), "search_request:%s:%s",
           engine_id ? engine_id : "all", client_key ? client_key : "unknown");
  return cache_compute_key(query, page, scope_key);
}

int results_handler(UrlParams *params) {
  extern Config global_config;
  struct timespec perf_start, perf_engines, perf_infobox, perf_done;
  clock_gettime(CLOCK_MONOTONIC, &perf_start);
  TemplateContext ctx = new_context();
  char *raw_query = "";
  char selected_engine_id[32];
  char *default_engine = get_default_search_engine();
  snprintf(selected_engine_id, sizeof(selected_engine_id), "%s",
           default_engine);
  free(default_engine);
  int page = 1;
  int btnI = 0;

  char **user_engines = NULL;
  int user_engine_count = 0;
  int has_user_pref =
      (get_user_engines(&user_engines, &user_engine_count) == 0);

  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0) {
        raw_query = params->params[i].value;
      } else if (strcmp(params->params[i].key, "p") == 0) {
        int parsed = atoi(params->params[i].value);
        if (parsed > 1)
          page = parsed;
      } else if (strcmp(params->params[i].key, "engine") == 0) {
        snprintf(selected_engine_id, sizeof(selected_engine_id), "%s",
                 params->params[i].value);
      } else if (strcmp(params->params[i].key, "btnI") == 0) {
        btnI = atoi(params->params[i].value);
      }
    }
  }

  context_set(&ctx, "query", raw_query);

  char *theme = get_theme("");
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "title_font", get_title_font());
  context_set(&ctx, "page_scale", get_page_scale());
  CustomTheme custom_theme;
  get_custom_theme(&custom_theme);
  context_set_custom_theme(&ctx, &custom_theme);
  free(theme);

  char *locale = get_locale(NULL);
  beaker_set_locale(&ctx, locale);

  const char *rate_limit_msg = beaker_get_locale_value(locale, "rate_limit");
  if (!rate_limit_msg)
    rate_limit_msg = "Slow down! Too many searches from you!";
  const char *no_results_msg = beaker_get_locale_value(locale, "no_results");
  if (!no_results_msg)
    no_results_msg = "No results found";


  if (!raw_query || strlen(raw_query) == 0) {
    send_redirect("/");
    free(locale);
    free_user_engine_list(user_engines, user_engine_count);
    free_context(&ctx);
    return -1;
  }

  const SearchEngine *selected_engine = find_enabled_engine(selected_engine_id);
  if (selected_engine &&
      !engine_allowed_for_user(selected_engine, user_engines, user_engine_count,
                               has_user_pref)) {
    selected_engine = NULL;
  }
  if (selected_engine) {
    snprintf(selected_engine_id, sizeof(selected_engine_id), "%s",
             selected_engine->id);
  } else {
    snprintf(selected_engine_id, sizeof(selected_engine_id), "all");
  }

  char *search_href = build_search_href(raw_query, selected_engine_id, 1);
  context_set(&ctx, "chrome_action", "/search");
  context_set(&ctx, "chrome_active", "all");
  {
    char enc[BUFFER_SIZE_MEDIUM];
    url_encode(raw_query ? raw_query : "", enc, sizeof(enc));
    context_set(&ctx, "query_encoded", enc);
  }
  context_set(&ctx, "chrome_engine",
              selected_engine_id ? selected_engine_id : "");
  context_set(&ctx, "chrome_all_href",
              search_href ? search_href : "/search");
  free(search_href);

  /* eligible engines for this request: filtered once so the counting and the
     job setup below can never disagree */
  const SearchEngine *eligible[ENGINE_COUNT];
  int eligible_count = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (!ENGINE_REGISTRY[i].enabled)
      continue;
    if (selected_engine && &ENGINE_REGISTRY[i] != selected_engine)
      continue;
    if (!engine_allowed_for_user(&ENGINE_REGISTRY[i], user_engines,
                                 user_engine_count, has_user_pref))
      continue;
    eligible[eligible_count++] = &ENGINE_REGISTRY[i];
  }

  const SearchEngine *eligible_all[ENGINE_COUNT];
  int eligible_all_count = eligible_count;
  for (int i = 0; i < eligible_count; i++)
    eligible_all[i] = eligible[i];

  /* skip engines whose breaker is open, but never leave the page without one */
  if (eligible_count > 0) {
    int healthy[ENGINE_COUNT];
    int healthy_count = 0;
    for (int i = 0; i < eligible_count; i++) {
      healthy[i] = engine_health_allow(eligible[i]->id);
      if (healthy[i])
        healthy_count++;
    }
    if (healthy_count > 0 && healthy_count < eligible_count) {
      int kept = 0;
      for (int i = 0; i < eligible_count; i++)
        if (healthy[i])
          eligible[kept++] = eligible[i];
      eligible_count = kept;
    }

    /* Keep a floor of three web engines. A breaker that took out every general
       engine used to leave the page ranked on academic APIs alone. */
    int web_count = 0;
    for (int i = 0; i < eligible_count; i++)
      if (eligible[i]->verticals & V_WEB)
        web_count++;
    /* Only healthy engines are added back: firing at a breaker that just
       tripped only produces a failure notice, not results. */
    for (int want = MIN_WEB_ENGINES; web_count < want; ) {
      const SearchEngine *best = NULL;
      for (int i = 0; i < eligible_all_count; i++) {
        const SearchEngine *e = eligible_all[i];
        if (!(e->verticals & V_WEB) || !healthy[i])
          continue;
        int already = 0;
        for (int j = 0; j < eligible_count; j++)
          if (eligible[j] == e)
            already = 1;
        if (already)
          continue;
        if (!best || engine_effective_weight(e) > engine_effective_weight(best))
          best = e;
      }
      if (!best || eligible_count >= ENGINE_COUNT)
        break;
      eligible[eligible_count++] = best;
      web_count++;
    }

    /* If every general engine is broken, still probe the strongest one so a
       general query is never answered by vertical APIs alone. */
    if (web_count == 0) {
      const SearchEngine *best = NULL;
      for (int i = 0; i < eligible_all_count; i++) {
        const SearchEngine *e = eligible_all[i];
        if (!(e->verticals & V_WEB))
          continue;
        if (!best || engine_effective_weight(e) > engine_effective_weight(best))
          best = e;
      }
      if (best && eligible_count < ENGINE_COUNT)
        eligible[eligible_count++] = best;
    }
  }
  int enabled_engine_count = eligible_count;

  char lang[8], region[8];
  locale_parts(locale, lang, sizeof(lang), region, sizeof(region));
  if (!lang[0])
    snprintf(lang, sizeof(lang), "en");

  char accept_language[64];
  if (region[0])
    snprintf(accept_language, sizeof(accept_language),
             "%s-%s,%s;q=0.9,en;q=0.4", lang, region, lang);
  else
    snprintf(accept_language, sizeof(accept_language), "%s;q=0.9,en;q=0.4",
             lang);

  /* operators (site:, -term, "phrase", filetype:) and intent */
  SearchQuery analysis;
  query_analyze(raw_query, locale, &analysis);

  /* vertical routing: the intent selects specialised engines first, then
     general web engines fill the remaining slots so recall never depends on a
     single API */
  {
    const SearchEngine *routed[ENGINE_COUNT];
    int routed_count = 0;
    int max_engines = 6;
    int want = V_WEB;
    switch (analysis.intent) {
    case QI_CODE:
      want = V_CODE;
      break;
    case QI_ACADEMIC:
      want = V_ACADEMIC;
      break;
    case QI_ENTITY:
      want = V_ENTITY;
      break;
    case QI_NEWS:
      want = V_NEWS;
      break;
    default:
      want = V_WEB;
      break;
    }

    /* A site: query is a web query restricted to one host: vertical APIs
       cannot honour it, and neither can engines that drop the operator. */
    int site_locked = analysis.site[0] != '\0';

    for (int pass = 0; pass < 2 && routed_count < max_engines; pass++) {
      for (int i = 0; i < eligible_count && routed_count < max_engines; i++) {
        int verticals = eligible[i]->verticals;
        if (pass == 0 && (verticals & want) == 0)
          continue;
        if (pass == 1 && (verticals & V_WEB) == 0)
          continue;
        if (site_locked && (!(verticals & V_WEB) || !eligible[i]->supports_site))
          continue;
        int duplicate = 0;
        for (int k = 0; k < routed_count; k++) {
          if (routed[k] == eligible[i]) {
            duplicate = 1;
            break;
          }
        }
        if (duplicate)
          continue;
        routed[routed_count++] = eligible[i];
      }
    }

    /* strongest first */
    for (int i = 1; i < routed_count; i++) {
      const SearchEngine *key = routed[i];
      int j = i - 1;
      while (j >= 0 && engine_effective_weight(routed[j]) < engine_effective_weight(key)) {
        routed[j + 1] = routed[j];
        j--;
      }
      routed[j + 1] = key;
    }

    if (routed_count > 0) {
      for (int i = 0; i < routed_count; i++)
        eligible[i] = routed[i];
      eligible_count = routed_count;
    }

    if (getenv("OMNI_PERF")) {
      fprintf(stderr, "[ROUTE] intent=%s engines=", query_intent_name(analysis.intent));
      for (int i = 0; i < eligible_count; i++)
        fprintf(stderr, "%s%s", i ? "," : "", eligible[i]->id);
      fprintf(stderr, "\n");
    }
  }


  /* engines choke on some punctuation; keep the typed text for display only */
  char engine_query[BUFFER_SIZE_LARGE];
  sanitize_query(analysis.clean[0] ? analysis.clean : raw_query, engine_query,
                 sizeof(engine_query));
  if (engine_query[0] == '\0')
    snprintf(engine_query, sizeof(engine_query), "%s", raw_query);

  /* H4: resolve the local answer BEFORE deciding whether to fan out. A
     confident local answer needs no upstream request at all - "2+2" should not
     query three search engines to be told it is 4. */
  AnswerBoxResult local_answer;
  int have_local_answer = 0;
  int answers_off = strcmp(global_config.answers_mode, "off") == 0;
  if (!answers_off)
    have_local_answer =
        answer_box_resolve_local_lang(raw_query, lang, &local_answer);
  int skip_fanout = have_local_answer && local_answer.confidence >= 0.95;

  /* ?web=1 asks for the web explicitly, which is what the "search the web for
     this" link does - a confident answer must not be a dead end. */
  for (int i = 0; i < params->count; i++) {
    if (strcmp(params->params[i].key, "web") == 0 &&
        strcmp(params->params[i].value, "1") == 0) {
      skip_fanout = 0;
      /* J3a: an explicit request for the web skips the relevance floor too.
         This is the link on the answer card, so it exists precisely to
         override "we already answered that". */
      analysis.force_web = 1;
    }
  }

  /* the answer box and the patient link both need it, on both branches */
  pthread_t infobox_threads[HANDLER_COUNT];
  InfoBoxThreadData infobox_data[HANDLER_COUNT];

  for (int i = 0; i < HANDLER_COUNT; i++) {
    infobox_data[i].query = raw_query;
    snprintf(infobox_data[i].lang, sizeof(infobox_data[i].lang), "%s", lang);
    infobox_data[i].success = i;
    infobox_data[i].result = (InfoBox){NULL};
  }

  ScrapeJob jobs[ENGINE_COUNT];
  SearchResult *all_results[ENGINE_COUNT];
  memset(jobs, 0, sizeof(jobs));
  memset(all_results, 0, sizeof(all_results));

  int engine_idx = 0;
  if (skip_fanout)
    eligible_count = 0; /* answered locally; do not spend a request */
  for (int i = 0; i < eligible_count; i++) {
    {
      all_results[engine_idx] = NULL;
      jobs[engine_idx].engine = eligible[i];
      jobs[engine_idx].query = jobs[engine_idx].query_text;
      query_engine_text(&analysis, eligible[i]->supports_site, 1,
                        jobs[engine_idx].query_text,
                        sizeof(jobs[engine_idx].query_text));
      if (jobs[engine_idx].query_text[0] == '\0')
        snprintf(jobs[engine_idx].query_text,
                 sizeof(jobs[engine_idx].query_text), "%s", engine_query);
      snprintf(jobs[engine_idx].lang, sizeof(jobs[engine_idx].lang), "%s",
               lang);
      snprintf(jobs[engine_idx].region, sizeof(jobs[engine_idx].region), "%s",
               region);
      snprintf(jobs[engine_idx].accept_language,
               sizeof(jobs[engine_idx].accept_language), "%s",
               accept_language);
      jobs[engine_idx].out_results = &all_results[engine_idx];
      jobs[engine_idx].max_results = MAX_RESULTS_PER_ENGINE;
      jobs[engine_idx].results_count = 0;
      jobs[engine_idx].page = page;
      jobs[engine_idx].handle = NULL;
      jobs[engine_idx].response.memory = NULL;
      jobs[engine_idx].response.size = 0;
      jobs[engine_idx].response.capacity = 0;
      jobs[engine_idx].http_status = 0;
      jobs[engine_idx].status = SCRAPE_STATUS_PENDING;
      engine_idx++;
    }
  }
  /* the router narrows the engine list after this count is taken, so the real
     job count is engine_idx: anything larger walks past jobs[].engine */
  enabled_engine_count = engine_idx;

  char client_key[BUFFER_SIZE_SMALL];
  rate_limit_get_client_key(client_key, sizeof(client_key));

  /* &patient=1: do not take the early exit, wait for every source (F1) */
  request_is_patient = 0;
  for (int i = 0; i < params->count; i++) {
    if (strcmp(params->params[i].key, "patient") == 0 &&
        strcmp(params->params[i].value, "1") == 0)
      request_is_patient = 1;
  }

  char *request_cache_key = build_search_request_cache_key(
      raw_query, selected_engine_id, page, client_key);
  int request_is_cached = 0;

  if (request_cache_key && get_cache_ttl_search() > 0) {
    char *cached_marker = NULL;
    size_t cached_marker_size = 0;

    if (cache_get(request_cache_key, (time_t)get_cache_ttl_search(),
                  &cached_marker, &cached_marker_size) == 0) {
      request_is_cached = 1;
    }

    free(cached_marker);
  }

  if (engine_idx > 0 && !request_is_cached) {
    RateLimitConfig rate_limit_config = {
        .max_requests = global_config.rate_limit_search_requests,
        .interval_seconds = global_config.rate_limit_search_interval,
    };
    RateLimitResult rate_limit_result =
        rate_limit_check("search", &rate_limit_config);
    if (rate_limit_result.limited) {
      char response[256];
      snprintf(response, sizeof(response), "<h1>%s</h1>", rate_limit_msg);
      send_response(response);
      free(request_cache_key);
      free(locale);
      free_user_engine_list(user_engines, user_engine_count);
      free_context(&ctx);
      return -1;
    }

    if (request_cache_key && get_cache_ttl_search() > 0) {
      cache_set(request_cache_key, "1", 1);
    }
  }

  /* F3 Reddit lane: for discussion-shaped queries, also ask the one engine
     that actually surfaces Reddit threads (Yahoo, per the spike: 12 plain /
     89 site: hits vs 0 for Bing, Brave and DDG) for site:reddit.com results.
     It joins the same parallel batch, so it costs no extra latency, and it is
     a separate job so its results merge and rank like any other source. */
  int reddit_lane_job = -1;
  if (analysis.wants_discussion && page == 1 && engine_idx < ENGINE_COUNT &&
      user_engine_count == 0) {
    /* Measured, not assumed (tools/queries-reddit.tsv): of the scraped
       engines only DuckDuckGo Lite actually honours site:reddit.com - it
       returns ~10 Reddit threads, while Yahoo returns a zero-byte body for the
       same operator and Bing and Brave return none. The lane therefore asks
       DDG, and falls back to any other site:-capable web engine only if DDG is
       unavailable. */
    static const char *LANE_ORDER[] = {"ddg", "yahoo", "bing", "brave"};
    const SearchEngine *lane = NULL;
    for (size_t p = 0; p < sizeof(LANE_ORDER) / sizeof(LANE_ORDER[0]) && !lane;
         p++) {
      for (int i = 0; i < ENGINE_COUNT; i++) {
        if (!ENGINE_REGISTRY[i].enabled ||
            !(ENGINE_REGISTRY[i].verticals & V_WEB) ||
            !ENGINE_REGISTRY[i].supports_site)
          continue;
        if (strcmp(ENGINE_REGISTRY[i].id, LANE_ORDER[p]) == 0) {
          lane = &ENGINE_REGISTRY[i];
          break;
        }
      }
    }
    if (lane && engine_health_allow(lane->id)) {
      char lane_query[512];
      if (analysis.discussion_site[0])
        snprintf(lane_query, sizeof(lane_query), "site:%s %s",
                 analysis.discussion_site, analysis.engine_core);
      else
        snprintf(lane_query, sizeof(lane_query), "site:reddit.com %s",
                 analysis.engine_core);

      all_results[engine_idx] = NULL;
      jobs[engine_idx].engine = lane;
      jobs[engine_idx].query = jobs[engine_idx].query_text;
      snprintf(jobs[engine_idx].query_text,
               sizeof(jobs[engine_idx].query_text), "%s", lane_query);
      snprintf(jobs[engine_idx].lang, sizeof(jobs[engine_idx].lang), "%s",
               lang);
      snprintf(jobs[engine_idx].region, sizeof(jobs[engine_idx].region), "%s",
               region);
      snprintf(jobs[engine_idx].accept_language,
               sizeof(jobs[engine_idx].accept_language), "%s",
               accept_language);
      jobs[engine_idx].out_results = &all_results[engine_idx];
      jobs[engine_idx].max_results = MAX_RESULTS_PER_ENGINE;
      jobs[engine_idx].results_count = 0;
      jobs[engine_idx].page = page;
      jobs[engine_idx].handle = NULL;
      jobs[engine_idx].response.memory = NULL;
      jobs[engine_idx].response.size = 0;
      jobs[engine_idx].response.capacity = 0;
      jobs[engine_idx].http_status = 0;
      jobs[engine_idx].status = SCRAPE_STATUS_PENDING;
      reddit_lane_job = engine_idx;
      engine_idx++;
      if (getenv("OMNI_PERF"))
        fprintf(stderr, "[F3] reddit lane via %s: %s\n", lane->id, lane_query);
    }
  }

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      pthread_create(&infobox_threads[i], NULL, infobox_thread_func,
                     &infobox_data[i]);
    }
  }

  if (engine_idx > 0) {
    scrape_engines_parallel(jobs, engine_idx);
  }

  /* F2 recovery, bounded to one extra round: if the batch under-delivered and
     there are healthy engines that were not queried, hedge to them rather than
     render a thin page. The round is only entered when the first pass clearly
     failed, so the normal path pays nothing. */
  {
    int delivered = 0;
    for (int i = 0; i < engine_idx; i++)
      if (jobs[i].results_count > 0)
        delivered++;

    /* H4: a confident local answer must not be hedged around. Without this the
       recovery round saw "0 engines delivered" and quietly re-added two of
       them, which is exactly the request the skip exists to avoid. */
    if (!skip_fanout && delivered < 2 && user_engine_count == 0 &&
        engine_idx < ENGINE_COUNT && page == 1) {
      /* G2 fallback chain: prefer an engine backed by an index we have NOT
         already heard from and that is not the index that just failed. Three
         engines that all resell the same index are one source, so hedging
         within a group buys nothing - see index_group in the registry. */
      const char *used_groups[ENGINE_COUNT];
      int used_group_count = 0;
      for (int j = 0; j < engine_idx && used_group_count < ENGINE_COUNT; j++) {
        const char *g = jobs[j].engine ? jobs[j].engine->index_group : NULL;
        if (!g)
          continue;
        used_groups[used_group_count++] = g;
      }

      int added = 0;
      for (int pass = 0; pass < 2 && added < 3; pass++) {
      for (int i = 0; i < ENGINE_COUNT && engine_idx < ENGINE_COUNT; i++) {
        const SearchEngine *cand = &ENGINE_REGISTRY[i];
        if (!cand->enabled || !(cand->verticals & V_WEB))
          continue;
        if (!engine_health_allow(cand->id))
          continue;
        int already = 0;
        for (int j = 0; j < engine_idx; j++)
          if (jobs[j].engine == cand)
            already = 1;
        if (already)
          continue;
        /* pass 0 wants a fresh index, pass 1 takes whatever is left */
        if (pass == 0 && cand->index_group) {
          int seen = 0;
          for (int k = 0; k < used_group_count; k++)
            if (strcmp(used_groups[k], cand->index_group) == 0)
              seen = 1;
          if (seen)
            continue;
        }

        all_results[engine_idx] = NULL;
        jobs[engine_idx].engine = cand;
        jobs[engine_idx].query = jobs[engine_idx].query_text;
        snprintf(jobs[engine_idx].query_text,
                 sizeof(jobs[engine_idx].query_text), "%s", engine_query);
        snprintf(jobs[engine_idx].lang, sizeof(jobs[engine_idx].lang), "%s",
                 lang);
        snprintf(jobs[engine_idx].region, sizeof(jobs[engine_idx].region), "%s",
                 region);
        snprintf(jobs[engine_idx].accept_language,
                 sizeof(jobs[engine_idx].accept_language), "%s",
                 accept_language);
        jobs[engine_idx].out_results = &all_results[engine_idx];
        jobs[engine_idx].max_results = MAX_RESULTS_PER_ENGINE;
        jobs[engine_idx].results_count = 0;
        jobs[engine_idx].page = page;
        jobs[engine_idx].handle = NULL;
        jobs[engine_idx].response.memory = NULL;
        jobs[engine_idx].response.size = 0;
        jobs[engine_idx].response.capacity = 0;
        jobs[engine_idx].http_status = 0;
        jobs[engine_idx].status = SCRAPE_STATUS_PENDING;
        engine_idx++;
        added++;
        if (cand->index_group && used_group_count < ENGINE_COUNT)
          used_groups[used_group_count++] = cand->index_group;
        if (added >= 3)
          break;
      }
      }

      if (added > 0) {
        if (getenv("OMNI_PERF"))
          fprintf(stderr, "[F2] recovery round: hedging to %d engines\n", added);
        scrape_engines_parallel(&jobs[engine_idx - added], added);
      }
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &perf_engines);

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      pthread_join(infobox_threads[i], NULL);
    }
  }
  clock_gettime(CLOCK_MONOTONIC, &perf_infobox);


  if (btnI) {
    for (int i = 0; i < engine_idx; i++) {
      if (jobs[i].results_count > 0 && all_results[i][0].url) {
        char *redirect_url = strdup(all_results[i][0].url);
        free_search_results(all_results, jobs, engine_idx);
        if (page == 1)
          free_infobox_results(infobox_data);
        free(request_cache_key);
        free(locale);
        free_user_engine_list(user_engines, user_engine_count);
        free_context(&ctx);
        if (redirect_url) {
          send_redirect(redirect_url);
          free(redirect_url);
        }
        return 0;
      }
    }
    free_search_results(all_results, jobs, engine_idx);
    if (page == 1)
      free_infobox_results(infobox_data);
    free(request_cache_key);
    free(locale);
    free_user_engine_list(user_engines, user_engine_count);
    free_context(&ctx);
    char no_results_html[128];
    snprintf(no_results_html, sizeof(no_results_html), "<h1>%s</h1>",
             no_results_msg);
    send_response(no_results_html);
    return 0;
  }

  StringMatrix infobox_matrix;
  string_matrix_init(&infobox_matrix);

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      if (infobox_data[i].success) {
        add_infobox_to_collection(&infobox_data[i].result, &infobox_matrix);
      }
    }
  }

  if (infobox_matrix.count > 0) {
    context_set_array_of_arrays(&ctx, "infoboxes", infobox_matrix.rows,
                                infobox_matrix.count,
                                infobox_matrix.field_counts);
  }
  string_matrix_free(&infobox_matrix);

  /* H4: answers = all. If no local answerer fired, promote the best infobox
     the rail already fetched into the answer box. No extra request: the work
     was done in parallel anyway, and doing it twice would be the only way to
     make this slower than the rail it mirrors. */
  if (!have_local_answer && !answers_off && page == 1 &&
      strcmp(global_config.answers_mode, "all") == 0) {
    /* the priority comes from the answer-box table itself, so adding a network
       kind is one edit, not two */
    int net_count = 0;
    const char *const *net_ids = answer_box_network_order(&net_count);
    for (int k = 0; k < net_count; k++) {
      int idx = -1;
      for (int h = 0; h < HANDLER_COUNT; h++)
        if (handlers[h].id && strcmp(handlers[h].id, net_ids[k]) == 0)
          idx = h;
      if (idx < 0)
        continue;
      if (!infobox_data[idx].success || !infobox_data[idx].result.extract)
        continue;
      if (!infobox_text_is_meaningful(infobox_data[idx].result.extract))
        continue;
      context_set(&ctx, "answer_text",
                  infobox_data[idx].result.title
                      ? infobox_data[idx].result.title
                      : net_ids[k]);
      context_set(&ctx, "answer_detail", infobox_data[idx].result.extract);
      context_set(&ctx, "answer_source_note", "From the web");
      context_set(&ctx, "answer_is_web", "1");
      break;
    }
  }

  int stale_count = 0;
  for (int i = 0; i < enabled_engine_count; i++) {
    if (jobs[i].stale)
      stale_count++;
  }
  if (stale_count > 0) {
    char stale_label[32];
    snprintf(stale_label, sizeof(stale_label), "%d", stale_count);
    context_set(&ctx, "results_cached", stale_label);
  }

  int warning_count = 0;
  for (int i = 0; i < enabled_engine_count; i++) {
    if (warning_message_for_job(&jobs[i], locale))
      warning_count++;
  }

  if (warning_count > 0) {
    StringMatrix warning_matrix;
    string_matrix_init(&warning_matrix);

    for (int i = 0; i < enabled_engine_count; i++) {
      const char *warning_message = warning_message_for_job(&jobs[i], locale);
      if (!warning_message)
        continue;

      if (jobs[i].http_status >= 400) {
        char warning_with_status[BUFFER_SIZE_LARGE];
        snprintf(warning_with_status, sizeof(warning_with_status),
                 "%s (HTTP %ld)", warning_message, jobs[i].http_status);
        add_warning_to_collection(jobs[i].engine->name, warning_with_status,
                                  &warning_matrix);
      } else {
        add_warning_to_collection(jobs[i].engine->name, warning_message,
                                  &warning_matrix);
      }
    }

    if (warning_matrix.count > 0) {
      context_set_array_of_arrays(&ctx, "engine_warnings", warning_matrix.rows,
                                  warning_matrix.count,
                                  warning_matrix.field_counts);
    }

    string_matrix_free(&warning_matrix);
  }

  int total_results = 0;
  for (int i = 0; i < enabled_engine_count; i++) {
    total_results += jobs[i].results_count;
  }

  /* results_state is derived from what is actually rendered, below */

  if (total_results > 0) {
    StringMatrix results_matrix;
    string_matrix_init(&results_matrix);

    ResultCandidate cands[MAX_CANDIDATES];
    int cand_count = 0;
    memset(cands, 0, sizeof(cands));

    int max_per_engine = 0;
    for (int i = 0; i < enabled_engine_count; i++) {
      if (jobs[i].results_count > max_per_engine)
        max_per_engine = jobs[i].results_count;
    }

    for (int j = 0; j < max_per_engine; j++) {
      for (int i = 0; i < enabled_engine_count; i++) {
        if (j >= jobs[i].results_count)
          continue;

        char *raw_url = all_results[i][j].url;
        char *raw_title = all_results[i][j].title;
        char *raw_snippet = all_results[i][j].snippet;

        if (!raw_url) {
          free(raw_url);
          free(raw_title);
          free(raw_snippet);
          continue;
        }

        char key[BUFFER_SIZE_LARGE];
        rank_canonical_url(raw_url, key, sizeof(key));

        char cleaned[BUFFER_SIZE_LARGE];
        clean_snippet(raw_snippet, cleaned, sizeof(cleaned));

        /* C12: engine weight was only ever used for routing and the "trusted"
           heuristic; fusion ignored it. Weighting the RRF vote means a source
           the project rates higher (Yahoo, Brave) actually counts more than a
           weaker one, which is what the weight was always for. */
        double engine_weight = jobs[i].engine ? jobs[i].engine->weight : 1.0;
        double score = engine_weight / (RRF_K + (double)j);

        int existing = -1;
        for (int c = 0; c < cand_count; c++) {
          if (strcmp(cands[c].key, key) == 0) {
            existing = c;
            break;
          }
        }

        if (existing >= 0) {
          cands[existing].score += score;
          cands[existing].rrf += score;
          cands[existing].engine_mask |= (1u << i);
          if (i < 32) {
            cands[existing].engine_groups[i] =
                jobs[i].engine ? jobs[i].engine->index_group : NULL;
            cands[existing].engine_weights[i] = engine_weight;
          }
          if (strlen(cleaned) > strlen(cands[existing].snippet)) {
            free(cands[existing].snippet);
            cands[existing].snippet = strdup(cleaned);
          }
          free(raw_url);
          free(raw_title);
          free(raw_snippet);
          continue;
        }

        if (cand_count >= MAX_CANDIDATES) {
          free(raw_url);
          free(raw_title);
          free(raw_snippet);
          continue;
        }

        ResultCandidate *cand = &cands[cand_count];
        cand->key = strdup(key);
        cand->url = strdup(raw_url);
        cand->title = strdup(raw_title ? raw_title : "Untitled");
        cand->snippet = strdup(cleaned);
        cand->score = score;
        cand->rrf = score;
        cand->engine_mask = (1u << i);
        if (i < 32) {
          cand->engine_groups[i] =
              jobs[i].engine ? jobs[i].engine->index_group : NULL;
          cand->engine_weights[i] = engine_weight;
        }
        cand->order = cand_count;
        rank_domain_of(cand->url, cand->domain, sizeof(cand->domain));

        if (cand->key && cand->url && cand->title && cand->snippet) {
          cand_count++;
        } else {
          free(cand->key);
          free(cand->url);
          free(cand->title);
          free(cand->snippet);
        }

        free(raw_url);
        free(raw_title);
        free(raw_snippet);
      }
    }

    for (int i = 0; i < enabled_engine_count; i++) {
      free(all_results[i]);
    }

    /* collapse syndicated copies, then score with the ranker */
    int funnel_raw = cand_count;
    cand_count = rank_dedupe(cands, cand_count);
    int funnel_deduped = cand_count;
    rank_score(cands, cand_count, &analysis);

    /* Relevance floor: if a result shares almost none of the query's
       information mass, it is a "relaxed" engine hit, not an answer. Without
       this, a nonsense query renders whatever popular pages an engine fell
       back to.

       F1 top-up: when the surviving set cannot fill a page and the engines
       were cut short, retry with a lower floor rather than shipping a thin
       page. The floor never reaches zero, so a nonsense query still renders
       nothing. */
    {
      int any_cancelled = 0;
      for (int i = 0; i < enabled_engine_count; i++)
        if (jobs[i].status == SCRAPE_STATUS_CANCELLED)
          any_cancelled = 1;

      /* Decide membership over the ORIGINAL array before moving anything:
         compacting first and then re-reading the tail is what caused a double
         free (the tail had already been moved to the front). */
      char *keep = calloc((size_t)cand_count + 1, 1);
      if (keep) {
        int kept = 0;
        for (int c = 0; c < cand_count; c++) {
          if (cands[c].coverage >= 0.15) {
            keep[c] = 1;
            kept++;
          }
        }

        if (kept < 6 && any_cancelled) {
          int relaxed = 0;
          for (int c = 0; c < cand_count; c++) {
            if (!keep[c] && cands[c].coverage >= 0.08) {
              keep[c] = 1;
              kept++;
              relaxed++;
            }
          }
          if (relaxed > 0 && getenv("OMNI_PERF"))
            fprintf(stderr, "[F1] top-up floor relaxed: +%d results\n", relaxed);
        }

        int out = 0;
        for (int c = 0; c < cand_count; c++) {
          if (!keep[c]) {
            rank_doc_free(&cands[c]);
            continue;
          }
          if (out != c)
            cands[out] = cands[c];
          out++;
        }
        cand_count = out;
        free(keep);
      }
    }
    int funnel_floored = cand_count;

    /* drop anything already shown on an earlier page of this query */
    /* cache keys are hashed; a raw "seen|<client>|<query>" string is rejected */
    char *seen_key = cache_compute_key(raw_query, 0, client_key);

    char *seen_data = NULL;
    size_t seen_size = 0;
    int have_seen = (seen_key && page > 1 &&
                     cache_get(seen_key, 600, &seen_data, &seen_size) == 0 &&
                     seen_data && seen_size > 0);

    /* sink, rather than delete: an empty page 2 is worse than a repeat */
    if (have_seen) {
      for (int c = 0; c < cand_count; c++) {
        if (!cands[c].key || !cands[c].key[0])
          continue;
        char needle[BUFFER_SIZE_LARGE];
        snprintf(needle, sizeof(needle), "\n%s\n", cands[c].key);
        if (strstr(seen_data, needle))
          cands[c].score -= 2.0;
      }
    }

    size_t seen_cap = 16384;
    char *seen_out = malloc(seen_cap);
    if (seen_out) {
      size_t len = 0;
      seen_out[len++] = '\n';
      if (have_seen && seen_size + 1 < seen_cap) {
        memcpy(seen_out + len, seen_data, seen_size);
        len += seen_size;
      }
      for (int c = 0; c < cand_count && len + 120 < seen_cap; c++) {
        if (!cands[c].key || !cands[c].key[0])
          continue;
        int wrote = snprintf(seen_out + len, seen_cap - len, "%s\n",
                             cands[c].key);
        if (wrote > 0)
          len += (size_t)wrote;
      }
      if (seen_key)
        cache_set(seen_key, seen_out, len);
      free(seen_out);
    }
    if (seen_data)
      free(seen_data);
    free(seen_key);

    /* A2: per-engine quality. Count, per engine, how many of the results it
       returned survived the floor and the join with the candidate pool. An
       engine that answered 200 with ten results that all got dropped scores 0
       here while its health stays "closed". */
    {
      int returned[32] = {0};
      int useful[32] = {0};
      int junk_engines = 0;
      for (int i = 0; i < enabled_engine_count; i++)
        returned[i] = jobs[i].results_count;
      for (int c = 0; c < cand_count; c++) {
        for (int bit = 0; bit < 32 && bit < enabled_engine_count; bit++) {
          if (cands[c].engine_mask & (1u << bit))
            useful[bit]++;
        }
      }
      for (int i = 0; i < enabled_engine_count; i++) {
        if (!jobs[i].engine || returned[i] <= 0)
          continue;
        engine_health_record_quality(jobs[i].engine->id, returned[i],
                                     useful[i]);
        /* H2: JUNK is its own outcome. HTTP 200, results parsed, and almost
           all of them below the floor - that is not OK and not EMPTY, and it
           should be visible as such rather than inferred from a low EWMA. */
        if (returned[i] >= 5 && useful[i] * 10 <= returned[i]) {
          engine_health_record_junk(jobs[i].engine->id);
          junk_engines++;
          if (getenv("OMNI_PERF"))
            fprintf(stderr, "[JUNK] %s returned %d, %d survived the floor\n",
                    jobs[i].engine->id, returned[i], useful[i]);
        }
      }
    }

    qsort(cands, cand_count, sizeof(ResultCandidate), candidate_cmp);

    char host_seen[64][128];
    int host_counts[64];
    int host_total = 0;

    /* F1 top-up: if the page would still be thin, allow more results per host
       rather than shipping five results when fifteen were available. */
    int domain_cap = MAX_PER_DOMAIN;
    /* F3: reddit.com carries several distinct threads per query by design, so
       the generic 3-per-domain cap would throw the lane away */
    int reddit_cap = analysis.wants_discussion ? 4 : MAX_PER_DOMAIN;
    {
      int cancelled_here = 0;
      for (int i = 0; i < enabled_engine_count; i++)
        if (jobs[i].status == SCRAPE_STATUS_CANCELLED)
          cancelled_here = 1;
      if (cancelled_here) {
        int cap_probe = 0;
        for (int c = 0; c < cand_count && c < 40; c++)
          cap_probe++;
        if (cap_probe < 12)
          domain_cap = MAX_PER_DOMAIN_RELAXED;
      }
    }

    for (int c = 0; c < cand_count; c++) {
      char host[128];
      snprintf(host, sizeof(host), "%s", cands[c].domain);

      int slot = -1;
      for (int h = 0; h < host_total; h++) {
        if (strcmp(host_seen[h], host) == 0) {
          slot = h;
          break;
        }
      }
      if (slot < 0 && host_total < 64) {
        slot = host_total++;
        snprintf(host_seen[slot], sizeof(host_seen[slot]), "%s", host);
        host_counts[slot] = 0;
      }

      int this_cap = (strstr(host, "reddit.com") != NULL) ? reddit_cap
                                                          : domain_cap;
      if (slot >= 0 && host_counts[slot] >= this_cap) {
        rank_doc_free(&cands[c]);
        continue;
      }
      if (slot >= 0)
        host_counts[slot]++;

      char *pretty_url = pretty_display_url(cands[c].url);
      char *base_url = get_base_url(cands[c].url);
      const char *values[RESULT_FIELD_COUNT] = {
          cands[c].url,
          pretty_url,
          cands[c].title,
          cands[c].snippet,
          base_url,
          is_media_result(cands[c].url) ? "media" : "",
      };
      string_matrix_append(&results_matrix, values, RESULT_FIELD_COUNT);

      free(pretty_url);
      free(base_url);
      rank_doc_free(&cands[c]);
    }

    if (results_matrix.count > 0)
      context_set_array_of_arrays(&ctx, "results", results_matrix.rows,
                                  results_matrix.count,
                                  results_matrix.field_counts);


    StringMatrix pager_matrix;
    SearchHrefData href_data = {.query = raw_query,
                                .engine_id = selected_engine_id};
    int pagination_result = 0;
    if (results_matrix.count > 0)
      pagination_result = string_matrix_build_pagination(
          page, search_href_builder, &href_data, &pager_matrix);
    else
      string_matrix_init(&pager_matrix);

    if (pagination_result == 0 && pager_matrix.count > 0) {
      context_set_array_of_arrays(&ctx, "pagination_links", pager_matrix.rows,
                                  pager_matrix.count,
                                  pager_matrix.field_counts);
    }


    clock_gettime(CLOCK_MONOTONIC, &perf_done);
    if (getenv("OMNI_PERF")) {
      double to_eng = (perf_engines.tv_sec - perf_start.tv_sec) * 1e3 +
                      (perf_engines.tv_nsec - perf_start.tv_nsec) / 1e6;
      double to_box = (perf_infobox.tv_sec - perf_engines.tv_sec) * 1e3 +
                      (perf_infobox.tv_nsec - perf_engines.tv_nsec) / 1e6;
      double to_render = (perf_done.tv_sec - perf_infobox.tv_sec) * 1e3 +
                         (perf_done.tv_nsec - perf_infobox.tv_nsec) / 1e6;
      fprintf(stderr, "[PERF] engines=%.0fms infobox=%.0fms fusion=%.0fms\n",
              to_eng, to_box, to_render);
    }

    context_set(&ctx, "results_state",
                results_matrix.count > 0 ? "ok" : "empty");

    /* Why is the page empty? "The engines may be busy" is wrong when they
       answered fine and every hit was judged irrelevant - that is what Bing
       does from this host for some queries (docs/G1-engines.md). Saying which
       of the three it was is the difference between "retry" and "rephrase". */
    if (results_matrix.count == 0) {
      const char *reason = "none";
      int any_ok = 0;
      char pause_note[320];
      int paused_now = build_paused_note(pause_note, sizeof pause_note);
      for (int i = 0; i < enabled_engine_count; i++)
        if (jobs[i].status == SCRAPE_STATUS_OK)
          any_ok = 1;
      if (paused_now > 0 && !any_ok)
        reason = "paused";
      else if (any_ok || funnel_raw > 0)
        reason = "filtered";
      else {
        int any_blocked = 0, any_failed = 0;
        for (int i = 0; i < enabled_engine_count; i++) {
          if (jobs[i].status == SCRAPE_STATUS_BLOCKED)
            any_blocked = 1;
          if (jobs[i].status == SCRAPE_STATUS_FETCH_ERROR)
            any_failed = 1;
        }
        if (any_blocked)
          reason = "blocked";
        else if (any_failed)
          reason = "unavailable";
      }
      context_set(&ctx, "empty_reason", reason);
      context_set(&ctx, "empty_hint",
                  strcmp(reason, "filtered") == 0
                      ? "The sources answered, but none of what came back was "
                        "relevant enough to show. Try rephrasing, or a more "
                        "specific query."
                  : strcmp(reason, "paused") == 0
                      ? "Every source that could answer is paused; the note "
                        "above says when the next one retries."
                  : strcmp(reason, "blocked") == 0
                      ? "Every source we asked refused the request. Waiting a "
                        "few minutes usually clears it."
                  : strcmp(reason, "unavailable") == 0
                      ? "We could not reach the sources — your network may be "
                        "down."
                      : "The search engines may be busy — try again in a "
                        "moment.");
    }

    /* G4: the answer box. Local answerers only, resolved on the request path -
       they are pure computation, so this costs nothing measurable. If nothing
       local answers, the infobox card carries the network answer instead. */
    if (have_local_answer) {
      context_set(&ctx, "answer_text", local_answer.answer);
      if (local_answer.detail[0])
        context_set(&ctx, "answer_detail", local_answer.detail);
      context_set(&ctx, "answer_source", local_answer.source);
      context_set(&ctx, "answer_source_note", "Answered locally");
      /* "2+2" answered 4: telling the user nothing was found on top of that
         would be silly. "answered" is not a state the empty block matches. */
      if (results_matrix.count == 0)
        context_set(&ctx, "results_state", "answered");
    }

    /* H1: if every result came from one index, say so. It is the difference
       between "this is what the web says" and "this is what one index says". */
    {
      const char *groups[ENGINE_COUNT];
      int group_count = 0;
      for (int i = 0; i < enabled_engine_count; i++) {
        if (jobs[i].results_count <= 0 || !jobs[i].engine)
          continue;
        const char *g = jobs[i].engine->index_group;
        if (!g || !g[0])
          continue;
        int seen = 0;
        for (int k = 0; k < group_count; k++)
          if (strcmp(groups[k], g) == 0)
            seen = 1;
        if (!seen && group_count < ENGINE_COUNT)
          groups[group_count++] = g;
      }
      if (group_count == 1) {
        char note[192];
        snprintf(note, sizeof(note),
                 "Results from one index only (%s). No other independent "
                 "source answered this search.",
                 groups[0]);
        context_set(&ctx, "one_index_note", note);
      }
    }

    /* G2: a paused engine is not an outage, and it is not invisible either. */
    {
      char note[320];
      if (build_paused_note(note, sizeof note) > 0)
        context_set(&ctx, "paused_note", note);
    }

    /* F1/F2: a thin page must say why it is thin, and offer the patient view.
       Silence here is what made "page 1 is empty but page 2 is fine" look like
       a bug the user could not act on. */
    if (results_matrix.count > 0 && results_matrix.count < 6) {
      int cut = 0, broke = 0;
      for (int i = 0; i < enabled_engine_count; i++) {
        if (jobs[i].status == SCRAPE_STATUS_CANCELLED)
          cut++;
        else if (jobs[i].status == SCRAPE_STATUS_BLOCKED ||
                 jobs[i].status == SCRAPE_STATUS_FETCH_ERROR)
          broke++;
      }
      if (cut > 0 || broke > 0) {
        char note[320];
        snprintf(note, sizeof(note),
                 "Only %d results: %d source%s %s still answering and %d "
                 "could not be reached.",
                 results_matrix.count, cut, cut == 1 ? "" : "s",
                 cut == 1 ? "was" : "were", broke);
        context_set(&ctx, "partial_note", note);
      }
    }



    /* F1 funnel: where did the results go? Without this a thin page is
       indistinguishable from a slow one or a blocked one. */
    {
      int cancelled = 0, blocked = 0, failed = 0, empty_eng = 0, ok_eng = 0;
      int paced = 0;
      for (int i = 0; i < enabled_engine_count; i++) {
        switch (jobs[i].status) {
        case SCRAPE_STATUS_CANCELLED: cancelled++; break;
        case SCRAPE_STATUS_BLOCKED: blocked++; break;
        case SCRAPE_STATUS_FETCH_ERROR: failed++; break;
        case SCRAPE_STATUS_EMPTY: empty_eng++; break;
        case SCRAPE_STATUS_OK: ok_eng++; break;
        case SCRAPE_STATUS_SKIPPED: paced++; break;
        default: break;
        }
      }

      if (getenv("OMNI_PERF")) {
        fprintf(stderr,
                "[FUNNEL] raw=%d deduped=%d floored=%d capped=%d rendered=%d "
                "engines=%d ok=%d empty=%d blocked=%d failed=%d cancelled=%d "
                "paced=%d stale=%d\n",
                funnel_raw, funnel_deduped, funnel_floored, total_results,
                results_matrix.count, enabled_engine_count, ok_eng, empty_eng,
                blocked, failed, cancelled, paced, stale_count);
        for (int i = 0; i < enabled_engine_count; i++) {
          fprintf(stderr, "[FUNNEL-E] %-14s status=%d results=%d http=%ld %.0fms%s\n",
                  jobs[i].engine->id, (int)jobs[i].status,
                  jobs[i].results_count, jobs[i].http_status, jobs[i].elapsed_ms,
                  jobs[i].stale ? " stale" : "");
        }
      }

      /* loopback-only debug view: never expose engine internals to the LAN */
      const char *dbg = NULL;
      for (int i = 0; i < params->count; i++) {
        if (strcmp(params->params[i].key, "debug") == 0)
          dbg = params->params[i].value;
      }
      if (dbg && strcmp(dbg, "1") == 0 &&
          strncmp(client_key, "127.", 4) == 0) {
        char line[512];
        snprintf(line, sizeof(line),
                 "raw=%d deduped=%d floored=%d capped=%d rendered=%d "
                 "engines=%d ok=%d empty=%d blocked=%d failed=%d cancelled=%d "
                 "stale=%d",
                 funnel_raw, funnel_deduped, funnel_floored, total_results,
                 results_matrix.count, enabled_engine_count, ok_eng, empty_eng,
                 blocked, failed, cancelled, stale_count);
        context_set(&ctx, "debug_funnel", line);
      }
    }

    if (results_matrix.count == 0 && total_results > 0) {
      fprintf(stderr,
              "[WARN] %d raw hits collapsed to zero rendered results (query "
              "\"%s\"):",
              total_results, raw_query);
      for (int i = 0; i < enabled_engine_count; i++)
        fprintf(stderr, " %s=%d", jobs[i].engine->id, jobs[i].results_count);
      fprintf(stderr, "\n");
    }

    char *html = render_template("results.html", &ctx);
    if (html) {
      send_response(html);
      free(html);
    } else {
      send_status("500 Internal Server Error");
      send_response("<h1>Template rendering failed</h1>");
    }

    string_matrix_free(&pager_matrix);
    string_matrix_free(&results_matrix);
  } else {
    context_set(&ctx, "results_state", "empty");
    if (have_local_answer) {
      context_set(&ctx, "answer_text", local_answer.answer);
      if (local_answer.detail[0])
        context_set(&ctx, "answer_detail", local_answer.detail);
      context_set(&ctx, "answer_source", local_answer.source);
      context_set(&ctx, "answer_source_note", "Answered locally");
      context_set(&ctx, "results_state", "answered");
    }
    {
      const char *reason = "none";
      int any_blocked = 0, any_failed = 0;
      for (int i = 0; i < engine_idx; i++) {
        if (jobs[i].status == SCRAPE_STATUS_BLOCKED)
          any_blocked = 1;
        if (jobs[i].status == SCRAPE_STATUS_FETCH_ERROR)
          any_failed = 1;
      }
      if (any_blocked)
        reason = "blocked";
      else if (any_failed)
        reason = "unavailable";
      context_set(&ctx, "empty_reason", reason);
      char pn[320];
      int paused_now = build_paused_note(pn, sizeof pn);
      if (paused_now > 0) {
        context_set(&ctx, "paused_note", pn);
        reason = "paused";
      }
      context_set(&ctx, "empty_hint",
                  strcmp(reason, "paused") == 0
                      ? "Every source that could answer is paused; the note "
                        "above says when the next one retries."
                  : strcmp(reason, "blocked") == 0
                      ? "Every source we asked refused the request. Waiting a "
                        "few minutes usually clears it."
                  : strcmp(reason, "unavailable") == 0
                      ? "We could not reach the sources — your network may be "
                        "down."
                      : "The search engines may be busy — try again in a "
                        "moment.");
    }
    char *html = render_template("results.html", &ctx);
    if (html) {
      send_response(html);
      free(html);
    } else {
      send_status("500 Internal Server Error");
      send_response("<h1>Template rendering failed</h1>");
    }

    free_search_results(all_results, jobs, engine_idx);
  }

  free(request_cache_key);

  if (page == 1)
    free_infobox_results(infobox_data);
  free(locale);
  free_user_engine_list(user_engines, user_engine_count);
  free_context(&ctx);

  return 0;
}
