#include "Search.h"
#include "../Cache/Cache.h"
#include "../Infobox/Calculator.h"
#include "../Infobox/ColourCode.h"
#include "../Infobox/CurrencyConversion.h"
#include "../Infobox/Dictionary.h"
#include "../Infobox/UnitConversion.h"
#include "../Infobox/Wikipedia.h"
#include "../Limiter/RateLimit.h"
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
  InfoBox result;
  int success;
} InfoBoxThreadData;

typedef struct {
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
#define RRF_K 60.0

typedef struct {
  char *url;
  char *title;
  char *snippet;
  char *key;
  double score;
  int order;
} ResultCandidate;

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

static const char *url_body(const char *url) {
  const char *p = url;
  if (prefix_ci(p, "https://"))
    p += 8;
  else if (prefix_ci(p, "http://"))
    p += 7;
  if (prefix_ci(p, "www."))
    p += 4;
  return p;
}

static void normalize_url_key(const char *url, char *out, size_t size) {
  out[0] = '\0';
  if (!url || size == 0)
    return;

  const char *p = url_body(url);
  size_t o = 0;
  for (; *p && o + 1 < size; p++) {
    if (*p == '#')
      break;
    char c = *p;
    if (c >= 'A' && c <= 'Z')
      c += 32;
    out[o++] = c;
  }
  out[o] = '\0';
  while (o > 0 && out[o - 1] == '/')
    out[--o] = '\0';
}

static void extract_host(const char *url, char *out, size_t size) {
  out[0] = '\0';
  if (!url || size == 0)
    return;

  const char *p = url_body(url);
  size_t o = 0;
  for (; *p && *p != '/' && *p != '?' && o + 1 < size; p++) {
    char c = *p;
    if (c >= 'A' && c <= 'Z')
      c += 32;
    out[o++] = c;
  }
  out[o] = '\0';
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

/* split a query into distinct content words (lowercase, 3+ chars) */
static int tokenize_query(const char *query, char tokens[][32], int max) {
  int count = 0;
  char buf[32];
  int len = 0;

  if (!query)
    return 0;

  for (const char *p = query;; p++) {
    char c = *p;
    int is_word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9');

    if (is_word) {
      if (len < 31)
        buf[len++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    } else {
      if (len >= 3) {
        buf[len] = '\0';
        if (!is_stopword(buf)) {
          int dup = 0;
          for (int i = 0; i < count; i++) {
            if (strcmp(tokens[i], buf) == 0)
              dup = 1;
          }
          if (!dup && count < max)
            snprintf(tokens[count++], 32, "%s", buf);
        }
      }
      len = 0;
      if (c == '\0')
        break;
    }
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
  const ResultCandidate *x = (const ResultCandidate *)a;
  const ResultCandidate *y = (const ResultCandidate *)b;
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

static int is_calculator_query(const char *query) {
  if (!query)
    return 0;

  int has_digit = 0;
  int has_math_operator = 0;

  for (const char *p = query; *p; p++) {
    if (isdigit(*p) || *p == '.') {
      has_digit = 1;
    }
    if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '^') {
      has_math_operator = 1;
    }
  }

  if (!has_digit || !has_math_operator)
    return 0;

  int len = strlen(query);
  for (int i = 0; i < len; i++) {
    char c = query[i];
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^') {
      int has_num_before = 0;
      int has_num_after = 0;

      for (int j = i - 1; j >= 0; j--) {
        if (isdigit(query[j]) || query[j] == '.') {
          has_num_before = 1;
          break;
        }
        if (query[j] != ' ')
          break;
      }

      for (int j = i + 1; j < len; j++) {
        if (isdigit(query[j]) || query[j] == '.') {
          has_num_after = 1;
          break;
        }
        if (query[j] != ' ')
          break;
      }

      if (has_num_before || has_num_after) {
        return 1;
      }
    }
  }

  return 0;
}

static InfoBox fetch_colour_wrapper(char *query) {
  return fetch_colour_data(query);
}

static InfoBoxHandler handlers[] = {
    {is_dictionary_query, fetch_dict_wrapper, NULL},
    {is_calculator_query, fetch_calc_wrapper, NULL},
    {is_unit_conv_query, fetch_unit_wrapper, NULL},
    {is_currency_query, fetch_currency_wrapper, NULL},
    {is_colour_code_query, fetch_colour_wrapper, NULL},
    {always_true, fetch_wiki_wrapper, construct_wiki_url},
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
  switch (job->status) {
  case SCRAPE_STATUS_FETCH_ERROR: {
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
  size_t needed = strlen("/search?q=") + strlen(safe_query) + 1;

  if (use_engine)
    needed += strlen("&engine=") + strlen(engine_id);
  if (page > 1)
    needed += strlen("&p=") + 16;

  char *href = (char *)malloc(needed);
  if (!href)
    return NULL;

  snprintf(href, needed, "/search?q=%s", safe_query);

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

  char page_str[16];
  snprintf(page_str, sizeof(page_str), "%d", page);
  context_set(&ctx, "page", page_str);

  char prev_str[16], next_str[16], two_prev_str[16], two_next_str[16];
  snprintf(prev_str, sizeof(prev_str), "%d", page > 1 ? page - 1 : 0);
  snprintf(next_str, sizeof(next_str), "%d", page + 1);
  snprintf(two_prev_str, sizeof(two_prev_str), "%d", page > 2 ? page - 2 : 0);
  snprintf(two_next_str, sizeof(two_next_str), "%d", page + 2);
  context_set(&ctx, "prev_page", prev_str);
  context_set(&ctx, "next_page", next_str);
  context_set(&ctx, "two_prev_page", two_prev_str);
  context_set(&ctx, "two_next_page", two_next_str);

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

  context_set(&ctx, "selected_engine", selected_engine_id);
  char *search_href = build_search_href(raw_query, selected_engine_id, 1);
  context_set(&ctx, "search_href", search_href ? search_href : "/search");
  context_set(&ctx, "chrome_action", "/search");
  context_set(&ctx, "chrome_active", "all");
  context_set(&ctx, "chrome_engine",
              selected_engine_id ? selected_engine_id : "");
  context_set(&ctx, "chrome_all_href",
              search_href ? search_href : "/search");
  free(search_href);

  int enabled_engine_count = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled &&
        (!selected_engine || &ENGINE_REGISTRY[i] == selected_engine) &&
        engine_allowed_for_user(&ENGINE_REGISTRY[i], user_engines,
                                user_engine_count, has_user_pref)) {
      enabled_engine_count++;
    }
  }

  /* engines choke on some punctuation; keep the typed text for display only */
  char engine_query[BUFFER_SIZE_LARGE];
  sanitize_query(raw_query, engine_query, sizeof(engine_query));
  if (engine_query[0] == '\0')
    snprintf(engine_query, sizeof(engine_query), "%s", raw_query);

  pthread_t infobox_threads[HANDLER_COUNT];
  InfoBoxThreadData infobox_data[HANDLER_COUNT];

  for (int i = 0; i < HANDLER_COUNT; i++) {
    infobox_data[i].query = raw_query;
    infobox_data[i].success = i;
    infobox_data[i].result = (InfoBox){NULL};
  }

  ScrapeJob jobs[ENGINE_COUNT];
  SearchResult *all_results[ENGINE_COUNT];

  int engine_idx = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled &&
        (!selected_engine || &ENGINE_REGISTRY[i] == selected_engine) &&
        engine_allowed_for_user(&ENGINE_REGISTRY[i], user_engines,
                                user_engine_count, has_user_pref)) {
      all_results[engine_idx] = NULL;
      jobs[engine_idx].engine = &ENGINE_REGISTRY[i];
      jobs[engine_idx].query = engine_query;
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

  char client_key[BUFFER_SIZE_SMALL];
  rate_limit_get_client_key(client_key, sizeof(client_key));

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

  int filter_engine_count = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled &&
        engine_allowed_for_user(&ENGINE_REGISTRY[i], user_engines,
                                user_engine_count, has_user_pref))
      filter_engine_count++;
  }

  if (filter_engine_count > 1) {
    StringMatrix filter_matrix;
    string_matrix_init(&filter_matrix);
    char *all_href = build_search_href(raw_query, "all", 1);
    const char *all_values[LINK_FIELD_COUNT] = {
        all_href, "All",
        selected_engine ? "engine-filter" : "engine-filter active"};

    string_matrix_append(&filter_matrix, all_values, LINK_FIELD_COUNT);
    free(all_href);

    for (int i = 0; i < ENGINE_COUNT; i++) {
      if (!ENGINE_REGISTRY[i].enabled ||
          !engine_allowed_for_user(&ENGINE_REGISTRY[i], user_engines,
                                   user_engine_count, has_user_pref))
        continue;

      char *filter_href =
          build_search_href(raw_query, ENGINE_REGISTRY[i].id, 1);
      const char *filter_class =
          (selected_engine && &ENGINE_REGISTRY[i] == selected_engine)
              ? "engine-filter active"
              : "engine-filter";

      const char *filter_values[LINK_FIELD_COUNT] = {
          filter_href, ENGINE_REGISTRY[i].name, filter_class};
      string_matrix_append(&filter_matrix, filter_values, LINK_FIELD_COUNT);
      free(filter_href);
    }

    if (filter_matrix.count > 0) {
      context_set_array_of_arrays(&ctx, "engine_filters", filter_matrix.rows,
                                  filter_matrix.count,
                                  filter_matrix.field_counts);
    }
    string_matrix_free(&filter_matrix);
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

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      pthread_join(infobox_threads[i], NULL);
    }
  }

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

  /* let the template render an explicit empty state instead of a blank page */
  context_set(&ctx, "results_state", total_results > 0 ? "ok" : "empty");

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
        normalize_url_key(raw_url, key, sizeof(key));

        char cleaned[BUFFER_SIZE_LARGE];
        clean_snippet(raw_snippet, cleaned, sizeof(cleaned));

        double score = 1.0 / (RRF_K + (double)j);

        int existing = -1;
        for (int c = 0; c < cand_count; c++) {
          if (strcmp(cands[c].key, key) == 0) {
            existing = c;
            break;
          }
        }

        if (existing >= 0) {
          cands[existing].score += score;
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
        cand->order = cand_count;

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

    /* prefer titles that actually contain the query terms */
    char query_tokens[8][32];
    int query_token_count = tokenize_query(raw_query, query_tokens, 8);
    if (query_token_count > 0) {
      for (int c = 0; c < cand_count; c++) {
        int title_hits = 0;
        int snippet_hits = 0;
        for (int t = 0; t < query_token_count; t++) {
          if (contains_ci(cands[c].title, query_tokens[t]))
            title_hits++;
          if (contains_ci(cands[c].snippet, query_tokens[t]))
            snippet_hits++;
        }
        cands[c].score +=
            0.05 * ((double)title_hits / query_token_count) +
            0.02 * ((double)snippet_hits / query_token_count);
      }
    }

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

    qsort(cands, cand_count, sizeof(ResultCandidate), candidate_cmp);

    char host_seen[64][128];
    int host_counts[64];
    int host_total = 0;

    for (int c = 0; c < cand_count; c++) {
      char host[128];
      extract_host(cands[c].url, host, sizeof(host));

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

      if (slot >= 0 && host_counts[slot] >= MAX_PER_DOMAIN) {
        free(cands[c].key);
        free(cands[c].url);
        free(cands[c].title);
        free(cands[c].snippet);
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
      free(cands[c].key);
      free(cands[c].url);
      free(cands[c].title);
      free(cands[c].snippet);
    }

    context_set_array_of_arrays(&ctx, "results", results_matrix.rows,
                                results_matrix.count,
                                results_matrix.field_counts);

    StringMatrix pager_matrix;
    SearchHrefData href_data = {.query = raw_query,
                                .engine_id = selected_engine_id};
    int pagination_result = string_matrix_build_pagination(
        page, search_href_builder, &href_data, &pager_matrix);

    if (pagination_result == 0 && pager_matrix.count > 0) {
      context_set_array_of_arrays(&ctx, "pagination_links", pager_matrix.rows,
                                  pager_matrix.count,
                                  pager_matrix.field_counts);
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
