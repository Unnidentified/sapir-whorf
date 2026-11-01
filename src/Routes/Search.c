#include "Search.h"
#include "ImageProxy.h"
#include "../Cache/Cache.h"
#include "../Infobox/Calculator.h"
#include "../Infobox/Translate.h"
#include "../Infobox/ColourCode.h"
#include "../Infobox/CurrencyConversion.h"
#include "../Infobox/Dictionary.h"
#include "../Infobox/UnitConversion.h"
#include "../Infobox/Wikipedia.h"
#include "../Limiter/RateLimit.h"
#include "../Scraping/EngineHealth.h"
#include "../Infobox/AnswerBox.h"
#include "../Infobox/CalcKeys.h"
#include "../Query/Placeholders.h"
#include "../Providers/Providers.h"
#include "../Infobox/Descriptions.h"
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
  /* J3b: the generic Wikipedia panel is only wanted when the query is about
     something encyclopaedic. It used to be fetched for every query through
     generator=search (the best fuzzy match for any text), so "1+1" got the
     article "1+1" and "covid-19" got whatever matched best. */
  int answered_locally;
  /* K1.4: decided on the request thread. beaker's cookies are per request
     thread, so reading them inside the worker found nothing and the gate
     silently passed - which is why turning the dictionary off still fetched
     it. */
  int suppressed;
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
  /* K7: the eleventh field is the link the page uses to ask for a real
     description when the engines gave none - empty when there is nothing to
     ask for */
  RESULT_FIELD_COUNT = 11,
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

static void clean_snippet(const char *in, char *out, size_t size) {
  out[0] = '\0';
  if (!in || size == 0)
    return;

  /* K7: a placeholder is not a description. Emptied here so the merge below
     can treat it as "no snippet" rather than as the longest one. */
  if (snippet_is_placeholder(in)) {
    if (size > 0)
      out[0] = '\0';
    return;
  }

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
    unescape_html_entities(decoded, once, sizeof(once));
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

/* K1.4: the widget resolver asks this before contacting a provider */
static int widget_provider_gate(const char *kind) {
  const char *provider = provider_id_for_widget_kind(kind);
  return provider && provider_is_off(provider);
}

static InfoBoxHandler handlers[] = {
    {"dictionary", is_dictionary_query, fetch_dict_wrapper, NULL},
    /* K8: there is no "arithmetic" handler. It was left behind when the
       calculator widget was added in J4, so 1+1 rendered a rail card AND the
       card - the same result twice. The widget is the only path: it is the one
       that works with the keypad, and with answers off there is no arithmetic
       answer at all, which is what "answers off" means. */
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
  /* K1.4: the same switch covers the infobox. Turning Wikipedia off must stop
     the panel as well as the engine, because both are the same destination.
     The answer was computed on the request thread; see the field's comment. */
  if (data->suppressed) {
    if (getenv("OMNI_PERF"))
      fprintf(stderr, "[INFOBOX] suppressed id=%s\n", h->id);
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

  /* J3b: a panel is relevant when the article title matches the query exactly,
     or when the title and first sentence cover at least 60 % of the query's
     normalised tokens. The old rule accepted any single token anywhere in the
     extract, which is why a query that had already been answered locally still
     got an article about something else in the rail. */
  int relevant = 0;
  if (h->fetch_fn == fetch_wiki_wrapper && data->answered_locally) {
    relevant = 0; /* arithmetic, units, colours: this is not a reference query */
  } else {
    char tokens[8][TOKEN_LEN];
    char folded_query[512];
    tokens_fold(folded_query, sizeof(folded_query), data->query);
    int token_count = tokenize_query(data->query, tokens, 8);
    if (token_count == 0) {
      relevant = !tokens_term_has_letter(data->query);
    } else {
      /* the first sentence, not the whole extract */
      char first_sentence[512];
      size_t used = 0;
      const char *p = data->result.extract ? data->result.extract : "";
      while (*p && used + 1 < sizeof(first_sentence)) {
        if (*p == '.' && (p[1] == ' ' || p[1] == '\0')) {
          first_sentence[used++] = '.';
          break;
        }
        first_sentence[used++] = *p++;
      }
      first_sentence[used] = '\0';

      /* fold the article's own text once, so accent-folded query tokens
         ("espana") match the article ("España") */
      char folded_title[256];
      char folded_lead[512];
      tokens_fold(folded_title, sizeof(folded_title),
                  data->result.title ? data->result.title : "");
      tokens_fold(folded_lead, sizeof(folded_lead), first_sentence);
      if (strcmp(folded_title, folded_query) == 0) {
        relevant = 1;
      } else {
        int hits = 0;
        for (int t = 0; t < token_count; t++)
          if (strstr(folded_title, tokens[t]) ||
              strstr(folded_lead, tokens[t]))
            hits++;
        relevant = (hits * 100 >= token_count * 60);
        /* A one-word query is a lookup, and the article for it may carry a
           translated title: es.wikipedia's best hit for "españa" is
           "España" and en.wikipedia's is "Spain". A strict title match would
           drop the correct panel whenever the detected language and the
           edition disagree (measured). For a single word with letters, the
           generator's own ranking is the evidence. Numeric and symbolic
           queries never get here: they have no letter. */
        if (!relevant && token_count == 1 &&
            tokens_term_has_letter(data->query))
          relevant = 1;
      }
    }
  }

  data->success = (data->result.title != NULL &&
                   infobox_text_is_meaningful(data->result.extract) && relevant);
  return NULL;
}

static int add_infobox_to_collection(InfoBox *infobox,
                                     StringMatrix *collection) {
  /* J6.2: the template emits the body through {{ … |safe}}, because the local
     generators produce HTML. A body that is NOT HTML has to be escaped here,
     and this is the only place that knows which is which: Wikipedia's
     explaintext extract is plain text containing & and <, and the Cambridge
     scrape is worse. Measured: {{ }} does escape by default, so |safe is what
     makes this necessary rather than optional. */
  char escaped[8192];
  const char *body = infobox->extract;
  if (body && !infobox->is_html) {
    html_escape(body, escaped, sizeof(escaped));
    body = escaped;
  }
  const char *values[INFOBOX_FIELD_COUNT] = {
      infobox->title, infobox->thumbnail_url, body,
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

/* J5.5: per-browser weight overrides from the engine_weights cookie.

   Thread-local rather than global: beaker serves requests concurrently, and a
   file-static table filled at request start would let one reader's policy leak
   into another's request. It is parsed once per request and read by the weight
   function below. */
#define WEIGHT_OVERRIDES_MAX 32
static _Thread_local struct {
  char id[32];
  double weight;
} tl_weights[WEIGHT_OVERRIDES_MAX];
static _Thread_local int tl_weight_count;

static void weights_load_from_cookie(void) {
  tl_weight_count = 0;
  char *cookie = get_cookie("engine_weights");
  if (!cookie || !cookie[0]) {
    free(cookie);
    return;
  }
  char *copy = strdup(cookie);
  free(cookie);
  if (!copy)
    return;
  char *saveptr = NULL;
  for (char *token = strtok_r(copy, "|", &saveptr);
       token && tl_weight_count < WEIGHT_OVERRIDES_MAX;
       token = strtok_r(NULL, ",", &saveptr)) {
    char *colon = strchr(token, ':');
    if (!colon)
      continue;
    *colon = '\0';
    char *end = NULL;
    double weight = strtod(colon + 1, &end);
    if (end == colon + 1 || weight < 0.5 || weight > 5.0 || !token[0])
      continue;
    snprintf(tl_weights[tl_weight_count].id, sizeof(tl_weights[0].id), "%s",
             token);
    tl_weights[tl_weight_count].weight = weight;
    tl_weight_count++;
  }
  free(copy);
}

static double weight_override_for(const char *id) {
  if (!id)
    return 0.0;
  for (int i = 0; i < tl_weight_count; i++)
    if (strcmp(tl_weights[i].id, id) == 0)
      return tl_weights[i].weight;
  return 0.0;
}

/* H2: selection weight, adjusted by measured quality. J5.5: and by this
   browser's own policy, when it set one. */
static double engine_effective_weight(const SearchEngine *e) {
  if (!e)
    return 0.0;
  double override = weight_override_for(e->id);
  return engine_health_effective_weight(e->id,
                                        override > 0.0 ? override : e->weight);
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
                               int page, int web, const char *cat) {
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
  if (web)
    needed += strlen("&web=1");
  if (cat && cat[0])
    needed += strlen("&cat=") + strlen(cat);

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
  /* J3b: an explicit "search the web" must survive the pager, or page 2 goes
     back to being an answer-only page. */
  if (web)
    strcat(href, "&web=1");
  /* J5.3: the category has to survive the pager, or page 2 fans out to every
     engine again and the tab was decorative. */
  if (cat && cat[0]) {
    strcat(href, "&cat=");
    strcat(href, cat);
  }

  return href;
}

typedef struct {
  const char *query;
  const char *engine_id;
  int web;
  const char *cat;
} SearchHrefData;

static char *search_href_builder(int page, void *data) {
  SearchHrefData *d = (SearchHrefData *)data;
  return build_search_href(d->query, d->engine_id, page, d->web, d->cat);
}

/* The context has 32 slots for a whole page, and these two caveats used to
   take one each. They are both "a line about where the results came from", so
   they share a slot and are joined when both apply. */
static void append_source_note(TemplateContext *ctx, const char *note) {
  if (!note || !note[0])
    return;
  const char *existing = NULL;
  for (int i = 0; i < ctx->count; i++) {
    if (strcmp(ctx->vars[i].key, "source_note") == 0) {
      existing = ctx->vars[i].value.string_val;
      break;
    }
  }
  if (!existing || !existing[0]) {
    context_set(ctx, "source_note", note);
    return;
  }
  char joined[400];
  snprintf(joined, sizeof(joined), "%s · %s", existing, note);
  context_set(ctx, "source_note", joined);
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
  /* J4.0: one keypad character per request, so the calculator card works with
     JavaScript off. This has to happen before the query is analysed or cached.
     raw_query initially points into the request params, which must not be
     written to, so an appended query is built in this buffer and raw_query is
     re-pointed at it. */
  /* J4.0: these are read before they are written when the parameter is
     absent, so they must start empty - an uninitialised widget_mode[0] decided
     whether the card rendered at all. */
  char key_pressed[16] = "";
  char keyed_buffer[512] = "";
  char widget_mode[32] = "";
  /* K10: the translate card's own form sends both menus explicitly */
  char widget_from[8] = "";
  char widget_to[8] = "";
  int swap_panes = 0;
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
      } else if (strcmp(params->params[i].key, "w") == 0) {
        snprintf(widget_mode, sizeof(widget_mode), "%s",
                 params->params[i].value);
      } else if (strcmp(params->params[i].key, "from") == 0) {
        snprintf(widget_from, sizeof(widget_from), "%s", params->params[i].value);
      } else if (strcmp(params->params[i].key, "to") == 0) {
        snprintf(widget_to, sizeof(widget_to), "%s", params->params[i].value);
      } else if (strcmp(params->params[i].key, "swap") == 0 &&
                 strcmp(params->params[i].value, "1") == 0) {
        swap_panes = 1;
      } else if (strcmp(params->params[i].key, "key") == 0) {
        /* the calculator keypad: append one character, capped at 24, and only
           from the set the keypad actually offers. Anything else is dropped
           rather than appended, because this value becomes the query. */
        snprintf(key_pressed, sizeof(key_pressed), "%s",
                 params->params[i].value);
      }
    }
  }

  /* K9: one implementation of the keypad protocol, shared with /w/<kind> */
  if (key_pressed[0] && raw_query[0]) {
    if (calc_apply_key(raw_query, key_pressed, keyed_buffer,
                       sizeof(keyed_buffer)))
      raw_query = keyed_buffer;
  }

  context_set(&ctx, "query", raw_query);

  char *theme = get_theme("");
  context_set(&ctx, "theme", theme);
  context_set(&ctx, "title_font", get_title_font());
  /* one key: the templates want the multiplier, and nothing else read the
     percent form (the context has a fixed 32 slots) */
  context_set(&ctx, "page_scale_css", get_page_scale_css());
  CustomTheme custom_theme;
  get_custom_theme(&custom_theme);
  context_set_custom_palette(&ctx, &custom_theme);
  free(theme);

  char *locale = get_locale(NULL);
  beaker_set_locale(&ctx, locale);

  const char *rate_limit_msg = beaker_get_locale_value(locale, "rate_limit");
  if (!rate_limit_msg)
    rate_limit_msg = "Slow down! Too many searches from you!";
  /* J3b: the note is rendered unconditionally by the template, so it must
     always exist - a missing key printed the literal tag (measured: nine
     "Key 'answer_source_note' not found in context" lines in the log). */
  context_set(&ctx, "answer_source_note",
              beaker_get_locale_value(locale, "answer_local"));
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

  char *search_href = build_search_href(raw_query, selected_engine_id, 1, 0, NULL);
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

  /* K1.4: one predicate for "may this request contact this engine". It was
     three separate conditions in three places, and the F2 recovery round only
     had one of them - measured: with an opt-in provider named and NOT opted
     in, the recovery round still fetched it, because it scanned the whole
     registry instead of the eligible set. */
  #define ENGINE_MAY_BE_CONTACTED(e)                                          \
    ((e)->enabled &&                                                          \
     engine_allowed_for_user((e), user_engines, user_engine_count,             \
                             has_user_pref) &&                                \
     !((e)->opt_in && !opt_in_enabled_for((e)->id)) &&                        \
     (!selected_engine || (e) == selected_engine))

  /* eligible engines for this request: filtered once so the counting and the
     job setup below can never disagree */
  const SearchEngine *eligible[MAX_ENGINES];
  int eligible_count = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (!ENGINE_MAY_BE_CONTACTED(&ENGINE_REGISTRY[i]))
      continue;
    eligible[eligible_count++] = &ENGINE_REGISTRY[i];
  }

  const SearchEngine *eligible_all[MAX_ENGINES];
  int eligible_all_count = eligible_count;
  for (int i = 0; i < eligible_count; i++)
    eligible_all[i] = eligible[i];

  /* skip engines whose breaker is open. K3: this used to keep the last
     engine even when it was suspended ("never leave the page without one"),
     which made the page say "Resumes in 4m" and then contact it on the next
     request - a suspension that is not honoured is not a suspension. When
     nothing is healthy the page shows the paused state, which is the truth. */
  if (eligible_count > 0) {
    int healthy[MAX_ENGINES];
    for (int i = 0; i < eligible_count; i++)
      healthy[i] = engine_health_allow(eligible[i]->id);
    {
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

    /* If every general engine is broken, probe the strongest *healthy* one so
       a general query is never answered by vertical APIs alone. K3: this used
       to ignore the breaker on purpose, and the consequence was that the page
       said "Resumes in 4m" while it contacted the engine immediately - a
       suspension that is not honoured is not a suspension. When nothing is
       healthy the page shows the paused state instead. */
    if (web_count == 0) {
      const SearchEngine *best = NULL;
      for (int i = 0; i < eligible_all_count; i++) {
        const SearchEngine *e = eligible_all[i];
        if (!(e->verticals & V_WEB) || !healthy[i])
          continue;
        if (!best || engine_effective_weight(e) > engine_effective_weight(best))
          best = e;
      }
      if (best && eligible_count < ENGINE_COUNT)
        eligible[eligible_count++] = best;
    }
  }
  int enabled_engine_count = eligible_count;

  /* J5.3: ?cat=<vertical> narrows the fan-out to engines that declare that
     vertical. It is a routing hint, not a filter on the results: an engine
     that answers a different vertical is simply not asked. Unknown values are
     ignored rather than 404ing, because a hand-edited URL should not look like
     a broken site. */
  int cat_mask = 0;
  const char *cat = "";
  for (int i = 0; i < params->count; i++) {
    if (strcmp(params->params[i].key, "cat") != 0)
      continue;
    const char *value = params->params[i].value;
    if (strcmp(value, "news") == 0) {
      cat_mask = V_NEWS;
      cat = "news";
    } else if (strcmp(value, "code") == 0) {
      cat_mask = V_CODE;
      cat = "code";
    } else if (strcmp(value, "academic") == 0) {
      cat_mask = V_ACADEMIC;
      cat = "academic";
    } else if (strcmp(value, "media") == 0) {
      cat_mask = V_MEDIA;
      cat = "media";
    } else if (strcmp(value, "entity") == 0) {
      cat_mask = V_ENTITY;
      cat = "entity";
    }
  }

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
    const SearchEngine *routed[MAX_ENGINES];
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
      break;
    }
    /* J5.3: an explicit category always beats the inferred intent - the visitor
       clicked the tab, so guessing again would ignore them. */
    if (cat_mask)
      want = cat_mask;

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
    } else if (cat_mask) {
      /* J5.3: no engine serves this category, so ask none. Falling back to the
         full web set would answer a News tab with web results and look like it
         had worked. */
      eligible_count = 0;
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
     query three search engines to be told it is 4.

     J3c reverses that default: the owner wants results under every answer, as
     Google shows them. The local answer is pure computation, so with
     answers_fanout=always the engines run as they would on any other query and
     the page waits no longer than a normal search. */
  AnswerBoxResult local_answer;
  int have_local_answer = 0;
  char answers_mode[16];
  char answers_fanout[16];
  get_answers_mode(global_config.answers_mode, answers_mode,
                   sizeof(answers_mode));
  get_answers_fanout(global_config.answers_fanout, answers_fanout,
                     sizeof(answers_fanout));
  int answers_off = strcmp(answers_mode, "off") == 0;
  if (!answers_off)
    have_local_answer =
        answer_box_resolve_local_lang(raw_query, lang, &local_answer);
  int skip_fanout = 0;
  /* J3b: ?web=1 means "I asked for the web". The page must then say so, and
     must not offer itself a link to do what it just did. */
  int web_explicit = 0;
  if (have_local_answer && local_answer.confidence >= 0.95) {
    if (strcmp(answers_fanout, "never") == 0) {
      /* the H4 behaviour, kept as an option */
      skip_fanout = 1;
    } else if (strcmp(answers_fanout, "auto") == 0) {
      /* only when there is nothing to search for: a numeric or symbolic
         expression with no words in it */
      skip_fanout = !analysis.has_alpha_term;
    }
  }

  /* ?web=1 asks for the web explicitly, which is what the "search the web for
     this" link does - a confident answer must not be a dead end. */
  for (int i = 0; i < params->count; i++) {
    if (strcmp(params->params[i].key, "web") == 0 &&
        strcmp(params->params[i].value, "1") == 0) {
      skip_fanout = 0;
      web_explicit = 1;
      /* J3a: an explicit request for the web skips the relevance floor too.
         This is the link on the answer card, so it exists precisely to
         override "we already answered that". */
      analysis.force_web = 1;
    }
  }

  /* K4: the chip strip is gone. It was a second navigation next to the tab
     bar, it duplicated the engine list on every results page, and it held a
     context slot that the page needs more. The evidence it carried - which
     engines answered, and with how many - is one line in the footer, built
     after the fan-out; the slot it used is now free. */
  /* J3b: the card offers a way to ask for the web - unless this request *is*
     that way. Keep ?web=1 on the links the page generates from here. */
  context_set(&ctx, "show_web_link", web_explicit ? "" : "1");
  {
    /* The href is /search?q=<encoded>&web=1, and query_encoded is already in
       the context: the template appends the literal suffix. That is one fewer
       of the 32 slots for a string nobody has to compute here. */
  }

  /* G4 + J4.0: the local answer and its card. These are pure computation, so
     they are resolved before the fan-out and set before the results are known.
     They used to be set inside "if (total_results > 0)", so when every engine
     was rate-limited the page showed an error instead of the arithmetic it had
     already worked out - which is a measured case, not a hypothetical one. */
  if (page == 1) {
    if (have_local_answer) {
      context_set(&ctx, "answer_text", local_answer.answer);
      if (local_answer.detail[0])
        context_set(&ctx, "answer_detail", local_answer.detail);
      context_set(&ctx, "answer_source_note",
                  beaker_get_locale_value(
                      locale, web_explicit ? "answer_showing_web"
                                           : "answer_local"));
      context_set(&ctx, "show_answer_strip", "1");
    }
    if (!answers_off) {
      WidgetData widget;
      answer_box_set_provider_gate(widget_provider_gate);
      int wres;
      /* K10: "w=translate" with explicit menus is a direct request, not
         something to parse out of the text */
      /* K10: the swap button submits the same text with the menus reversed,
         so the no-JS path needs no script and no second link */
      if (swap_panes && widget_from[0] && widget_to[0]) {
        char tmp[8];
        snprintf(tmp, sizeof(tmp), "%s", widget_from);
        snprintf(widget_from, sizeof(widget_from), "%s", widget_to);
        snprintf(widget_to, sizeof(widget_to), "%s", tmp);
        if (!widget_from[0])
          snprintf(widget_from, sizeof(widget_from), "%s", "en");
      }
      if (widget_mode[0] && strcmp(widget_mode, "translate") == 0 &&
          widget_to[0] && !widget_provider_gate("translate"))
        wres = translate_widget_explicit(raw_query, widget_from, widget_to,
                                         &widget, lang);
      else
        wres = widget_mode[0]
                     ? answer_box_widget_forced(widget_mode, raw_query, lang,
                                                &widget)
                     : answer_box_widget(raw_query, lang, NULL, &widget);
      if (wres) {
        /* two keys, not four: the context is a fixed 32 slots. The expression
           is what the user typed, and the result slot doubles as the error
           slot because a card shows one big value either way. */
        /* K1.4: a switch that only hides a card would still let the provider
           be contacted, and the card is the only reason it is contacted. So
           the gate is here, before anything is rendered or fetched. Local
           kinds (calculator, units, colour) have no provider to switch. */
        const char *widget_provider = provider_id_for_widget_kind(widget.kind);
        int widget_denied = widget_provider && provider_is_off(widget_provider);
        if (widget_denied) {
          if (getenv("OMNI_PERF"))
            fprintf(stderr, "[WIDGET] suppressed kind=%s provider=%s\n",
                    widget.kind, widget_provider);
          widget.ok = 0;
          widget.kind[0] = '\0';
        }
        context_set(&ctx, "show_answer_strip", "");
        context_set(&ctx, "widget_kind", widget.kind);
        /* four keys, and the context has 31 in use on this page: the result
           slot doubles as the error line, and the title and source come from
           the locale and the template rather than from here. */
        context_set(&ctx, "widget_result",
                    widget.ok && widget.result[0] ? widget.result
                                                  : widget.note);
        /* K4: a key is only worth a slot on the page that can read it. The
           expression belongs to the stock and translate cards, the href to a
           card that links out, and the code pair to translate alone; setting
           them empty on every other card cost slots the context does not have
           (it renders with 31 of beaker's 32). */
        if (widget.expression[0] &&
            (strcmp(widget.kind, "stock") == 0 ||
             strcmp(widget.kind, "translate") == 0))
          context_set(&ctx, "widget_expression", widget.expression);
        context_set(&ctx, "widget_note", widget.note);
        if (widget.href[0])
          context_set(&ctx, "widget_href", widget.href);
        /* K10: "from|to" for the translate card's two menus. One key, because
           the page renders with 31 of beaker's 32 slots. */
        if (widget.translate_codes[0])
          context_set(&ctx, "translate_codes", widget.translate_codes);
        context_set(&ctx, "widget_ok", widget.ok ? "1" : "");
        if (getenv("OMNI_PERF"))
          fprintf(stderr, "[WIDGET] kind=%s ok=%d expr='%s' note='%s'\n",
                  widget.kind, widget.ok, widget.expression, widget.note);
      }
    }
  }

  /* the answer box and the patient link both need it, on both branches */
  pthread_t infobox_threads[HANDLER_COUNT];
  InfoBoxThreadData infobox_data[HANDLER_COUNT];

  for (int i = 0; i < HANDLER_COUNT; i++) {
    infobox_data[i].query = raw_query;
    infobox_data[i].answered_locally = have_local_answer;
    snprintf(infobox_data[i].lang, sizeof(infobox_data[i].lang), "%s", lang);
    infobox_data[i].success = i;
    infobox_data[i].result = (InfoBox){NULL};
    /* K1.4: read the cookie here, on the request thread */
    infobox_data[i].suppressed =
        handlers[i].id && provider_is_off(provider_id_for_engine(handlers[i].id));
  }

  /* J5.1: sized by what this request will actually fire, on the heap. It used
     to be ENGINE_COUNT on the stack; with a growable registry that would be a
     VLA, and 1760 bytes times 192 engines is a quarter of a megabyte of stack
     on a thread the review already calls small. Each engine fires at most one
     job and the recovery round adds at most three, so the selected count plus
     a margin is the real bound. */
  const size_t job_capacity = (size_t)eligible_count + 8;
  ScrapeJob *jobs = calloc(job_capacity, sizeof(ScrapeJob));
  SearchResult **all_results = calloc(job_capacity, sizeof(SearchResult *));
  if (!jobs || !all_results) {
    /* nothing else has been allocated yet at this point */
    free(jobs);
    free(all_results);
    free(locale);
    free_user_engine_list(user_engines, user_engine_count);
    free_context(&ctx);
    send_status("500 Internal Server Error");
    send_response("<h1>Out of memory</h1>");
    return 0;
  }

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

  weights_load_from_cookie();

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
  int lane_job_ids[2] = {-1, -1};
  int lane_job_count = 0;
  /* K5.4: the lane used to require user_engine_count == 0, so anyone who had
     ever saved a provider choice never saw a Reddit thread - the likeliest
     reason the owner's searches showed none. The provider toggle is the real
     gate, and ENGINE_MAY_BE_CONTACTED already honours it. The triggers are the
     discussion shape, the word "reddit", an r/<sub> token, and a typed
     site:reddit.com operator (which is passed through unchanged). */
  int reddit_wanted = analysis.wants_discussion;
  if (!reddit_wanted && raw_query) {
    if (strcasestr(raw_query, "reddit") || strcasestr(raw_query, "r/"))
      reddit_wanted = 1;
  }
  if (reddit_wanted && page == 1 && engine_idx < ENGINE_COUNT &&
      !provider_is_off("reddit")) {
    /* K5.2 problem 2: one engine wide. DuckDuckGo is the most rate-limited
       engine in the set, and the two measurements on record disagree about
       who honours the operator (tools/queries-reddit.tsv: Yahoo 12 plain / 89
       site: hits against 0 for Bing and Brave; a later probe: DDG returns
       threads, Yahoo a zero-byte body). Rather than pick a winner from two
       contradictory measurements, the lane asks the two engines that have ever
       answered - two requests, one batch, no extra latency - and the log names
       which one delivered. */
    static const char *LANE_ORDER[] = {"ddg", "yahoo", "bing", "brave"};
    const SearchEngine *lane[2] = {NULL, NULL};
    int lane_count = 0;
    for (size_t p = 0;
         p < sizeof(LANE_ORDER) / sizeof(LANE_ORDER[0]) && lane_count < 2; p++) {
      for (int i = 0; i < ENGINE_COUNT; i++) {
        if (!(ENGINE_REGISTRY[i].verticals & V_WEB) ||
            !ENGINE_REGISTRY[i].supports_site ||
            !ENGINE_MAY_BE_CONTACTED(&ENGINE_REGISTRY[i]))
          continue;
        if (strcmp(ENGINE_REGISTRY[i].id, LANE_ORDER[p]) == 0) {
          if (engine_health_allow(ENGINE_REGISTRY[i].id)) {
            lane[lane_count++] = &ENGINE_REGISTRY[i];
          }
          break;
        }
      }
    }
    if (lane_count > 0) {
      char lane_query[512];
      if (analysis.discussion_site[0])
        snprintf(lane_query, sizeof(lane_query), "site:%s %s",
                 analysis.discussion_site, analysis.engine_core);
      else
        snprintf(lane_query, sizeof(lane_query), "site:reddit.com %s",
                 analysis.engine_core);

      for (int l = 0; l < lane_count && engine_idx < ENGINE_COUNT; l++) {
        all_results[engine_idx] = NULL;
        jobs[engine_idx].engine = lane[l];
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
        if (reddit_lane_job < 0)
          reddit_lane_job = engine_idx;
        lane_job_ids[lane_job_count++] = engine_idx;
        engine_idx++;
        if (getenv("OMNI_PERF"))
          fprintf(stderr, "[F3] reddit lane via %s (query length %zu)\n",
                  lane[l]->id, strlen(lane_query));
      }
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
      const char *used_groups[MAX_ENGINES];
      int used_group_count = 0;
      for (int j = 0; j < engine_idx && used_group_count < ENGINE_COUNT; j++) {
        const char *g = jobs[j].engine ? jobs[j].engine->index_group : NULL;
        if (!g)
          continue;
        used_groups[used_group_count++] = g;
      }

      int added = 0;
      for (int pass = 0; pass < 2 && added < 3; pass++) {
      for (int i = 0; i < ENGINE_COUNT && engine_idx < (int)job_capacity; i++) {
        const SearchEngine *cand = &ENGINE_REGISTRY[i];
        if (!(cand->verticals & V_WEB))
          continue;
        /* the same gate as the first pass: recovery must never reach a
           provider the first pass was not allowed to reach */
        if (!ENGINE_MAY_BE_CONTACTED(cand) || !engine_health_allow(cand->id))
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
        free(jobs);
        free(all_results);
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
    free(jobs);
    free(all_results);
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
    /* J1: the compact summary shows the first panel's title */
    context_set(&ctx, "panel_title",
                infobox_matrix.rows[0][0] ? infobox_matrix.rows[0][0] : "");
  }
  string_matrix_free(&infobox_matrix);

  /* H4: answers = all. If no local answerer fired, promote the best infobox
     the rail already fetched into the answer box. No extra request: the work
     was done in parallel anyway, and doing it twice would be the only way to
     make this slower than the rail it mirrors. */
  if (!have_local_answer && !answers_off && page == 1 &&
      strcmp(answers_mode, "all") == 0) {
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
      context_set(&ctx, "answer_source_note",
                  beaker_get_locale_value(locale, "answer_from_web"));
      break;
    }
  }

  int stale_count = 0;
  for (int i = 0; i < enabled_engine_count; i++) {
    if (jobs[i].stale)
      stale_count++;
  }
  if (stale_count > 0) {
    char stale_label[256];
    const char *fmt = beaker_get_locale_value(locale, "results_cached_note");
    char count[16];
    snprintf(count, sizeof(count), "%d", stale_count);
    if (fmt && strstr(fmt, "{count}")) {
      const char *mark = strstr(fmt, "{count}");
      snprintf(stale_label, sizeof(stale_label), "%.*s%s%s", (int)(mark - fmt),
               fmt, count, mark + 7);
    } else {
      snprintf(stale_label, sizeof(stale_label), "%s %s",
               fmt ? fmt : "", count);
    }
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
          /* J5.2: the first source that provides a rich field wins; later ones
             only fill a gap, so a duplicate row cannot blank a thumbnail. */
          if (!cands[existing].thumbnail && all_results[i][j].thumbnail)
            cands[existing].thumbnail = all_results[i][j].thumbnail;
          if (!cands[existing].date && all_results[i][j].date)
            cands[existing].date = all_results[i][j].date;
          if (!cands[existing].author && all_results[i][j].author)
            cands[existing].author = all_results[i][j].author;
          if (!cands[existing].source && all_results[i][j].source)
            cands[existing].source = all_results[i][j].source;
          if (!cands[existing].kind && all_results[i][j].kind)
            cands[existing].kind = all_results[i][j].kind;
          cands[existing].score += score;
          cands[existing].rrf += score;
          cands[existing].engine_mask |= (1u << i);
          if (i < 32) {
            /* J5.6: a bridge job carries hits from several indexes, so the
               hit's own group wins when it has one */
            cands[existing].engine_groups[i] =
                (all_results[i][j].index_group[0])
                    ? all_results[i][j].index_group
                    : (jobs[i].engine ? jobs[i].engine->index_group : NULL);
            cands[existing].engine_weights[i] = engine_weight;
          }
          /* K7: a real snippet always beats an empty one, and only then does
             the longer win. The old rule was "longer wins", so any boilerplate
             beat a short real description. */
          {
            const char *have = cands[existing].snippet ? cands[existing].snippet : "";
            int take = 0;
            if (cleaned[0] && !have[0])
              take = 1;
            else if (cleaned[0] && have[0] && strlen(cleaned) > strlen(have))
              take = 1;
            else if (!cleaned[0] && !have[0])
              take = 0;
            if (take) {
              free(cands[existing].snippet);
              cands[existing].snippet = strdup(cleaned);
            }
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
        cand->thumbnail = all_results[i][j].thumbnail;
        cand->date = all_results[i][j].date;
        cand->author = all_results[i][j].author;
        cand->source = all_results[i][j].source;
        cand->kind = all_results[i][j].kind;
        if (i < 32) {
          cand->engine_groups[i] =
              (all_results[i][j].index_group[0])
                  ? all_results[i][j].index_group
                  : (jobs[i].engine ? jobs[i].engine->index_group : NULL);
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
      /* J5.2: the proxy only fetches URLs it signed itself, so the thumbnail
         link is built here rather than in the template. The browser never
         contacts the third party. */
      char *thumb_href = NULL;
      if (cands[c].thumbnail && cands[c].thumbnail[0])
        thumb_href = image_proxy_sign_url(cands[c].thumbnail);
      /* K7: a blank snippet is the one case worth a second request, and the
         link is signed so the endpoint cannot be used to fetch an arbitrary
         URL. Only the first page asks: a row on page 2 is rarely the one a
         reader studies. */
      char *desc_href = NULL;
      /* K7: with no snippet at all, the URL's own path is a true description of
         what the link is - the same thing other engines show when a site's
         robots.txt stops them quoting it. The fetcher may replace it. */
      char breadcrumb[256] = "";
      if (!cands[c].snippet || !cands[c].snippet[0]) {
        const char *p = strstr(cands[c].url, "://");
        p = p ? p + 3 : cands[c].url;
        const char *slash = strchr(p, '/');
        size_t host_len = slash ? (size_t)(slash - p) : strlen(p);
        size_t used = 0;
        if (host_len > 0 && host_len < sizeof(breadcrumb)) {
          memcpy(breadcrumb, p, host_len);
          used = host_len;
          if (strncmp(breadcrumb, "www.", 4) == 0) {
            memmove(breadcrumb, breadcrumb + 4, used - 4 + 1);
            used -= 4;
          }
          const char *seg = slash;
          int taken = 0;
          while (seg && *seg == '/' && taken < 3 && used + 3 < sizeof(breadcrumb)) {
            const char *end = strchr(seg + 1, '/');
            size_t seg_len = end ? (size_t)(end - seg - 1) : strlen(seg + 1);
            if (seg_len == 0)
              break;
            if (used + seg_len + 8 >= sizeof(breadcrumb))
              break;
            memcpy(breadcrumb + used, " \xE2\x80\xBA ", 4);
            used += 4;
            memcpy(breadcrumb + used, seg + 1, seg_len);
            used += seg_len;
            breadcrumb[used] = '\0';
            taken++;
            seg = end;
          }
        }
      }
      if (page == 1 && (!cands[c].snippet || !cands[c].snippet[0]) &&
          (strncmp(cands[c].url, "http://", 7) == 0 ||
           strncmp(cands[c].url, "https://", 8) == 0))
        desc_href = description_href(cands[c].url);
      const char *values[RESULT_FIELD_COUNT] = {
          cands[c].url,
          pretty_url,
          cands[c].title,
          cands[c].snippet && cands[c].snippet[0] ? cands[c].snippet
                                                  : breadcrumb,
          base_url,
          cands[c].kind && cands[c].kind[0]
              ? cands[c].kind
              : (is_media_result(cands[c].url) ? "media" : ""),
          thumb_href ? thumb_href : "",
          cands[c].date ? cands[c].date : "",
          cands[c].source ? cands[c].source : "",
          cands[c].author ? cands[c].author : "",
          desc_href ? desc_href : "",
      };
      string_matrix_append(&results_matrix, values, RESULT_FIELD_COUNT);

      free(pretty_url);
      free(base_url);
      free(thumb_href);
      free(desc_href);
      rank_doc_free(&cands[c]);
    }

    if (results_matrix.count > 0)
      context_set_array_of_arrays(&ctx, "results", results_matrix.rows,
                                  results_matrix.count,
                                  results_matrix.field_counts);


    StringMatrix pager_matrix;
    SearchHrefData href_data = {.query = raw_query,
                                .engine_id = selected_engine_id,
                                .web = web_explicit,
                                .cat = cat};
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
                results_matrix.count > 0
                    ? "ok"
                    : ((have_local_answer || widget_mode[0]) ? "answered"
                                                            : "empty"));

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


    /* H1: if every result came from one index, say so. It is the difference
       between "this is what the web says" and "this is what one index says". */
    {
      const char *groups[MAX_ENGINES];
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
        char note[320];
        snprintf(note, sizeof(note), "%s",
                 beaker_get_locale_value(locale, "one_index_note"));
        {
          /* substitute {group} without a printf format from the locale file:
             a stray % in a translation must not become a format string */
          char filled[320];
          const char *mark = strstr(note, "{group}");
          if (mark) {
            snprintf(filled, sizeof(filled), "%.*s%s%s",
                     (int)(mark - note), note, groups[0], mark + 7);
            snprintf(note, sizeof(note), "%s", filled);
          }
        }
        append_source_note(&ctx, note);
      }
    }

    /* G2: a paused engine is not an outage, and it is not invisible either. */
    {
      char note[320];
      if (build_paused_note(note, sizeof note) > 0)
        append_source_note(&ctx, note);
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
        /* the funnel goes to the perf log, not the context: no template reads
           it, and the context has 32 slots for the whole page */
        fprintf(stderr, "[FUNNEL] %s\n", line);
      }
    }

    if (results_matrix.count == 0 && total_results > 0) {
      /* J6.1: the query text was written into every one of these lines, which
         is why the shipped log contained "1+1", "covid-19", "#123456" and a
         personal search. Part 0 says not to log the query; the counts and the
         per-engine breakdown are what actually diagnose this, so keep those
         and drop the text. A per-install-salted hash would allow correlating
         repeats; the length alone is enough to tell a one-word query from a
         sentence, and it is not reversible. */
      fprintf(stderr,
              "[WARN] %d raw hits collapsed to zero rendered results (query "
              "length %zu):",
              total_results, strlen(raw_query));
      for (int i = 0; i < enabled_engine_count; i++)
        fprintf(stderr, " %s=%d", jobs[i].engine->id, jobs[i].results_count);
      fprintf(stderr, "\n");
    }

    /* K4: "Results from Bing (12), Brave (9)" - the same counts the chips
       carried, in a line that does not compete with the tab bar. */
    {
      char line[512];
      size_t used = 0;
      line[0] = '\0';
      for (int i = 0; i < engine_idx; i++) {
        if (!jobs[i].engine || !jobs[i].engine->id || jobs[i].results_count <= 0)
          continue;
        int n = snprintf(line + used, sizeof(line) - used, "%s%s (%d)",
                         used ? ", " : "", jobs[i].engine->name,
                         jobs[i].results_count);
        if (n < 0 || (size_t)n >= sizeof(line) - used)
          break;
        used += (size_t)n;
      }
      if (used > 0) {
        char full[640];
        /* a locale that has not been translated yet must not print (null) */
        const char *label = beaker_get_locale_value(locale, "results_from");
        if (!label || !label[0])
          label = "Results from";
        snprintf(full, sizeof(full), "%s %s", label, line);
        context_set(&ctx, "answered_note", full);
      } else {
        context_set(&ctx, "answered_note", "");
      }
      if (getenv("OMNI_PERF"))
        fprintf(stderr, "[F4] answered note: %s\n", used ? line : "(none)");
    }

    /* set even when empty: the render check fails on a key the template reads
       and the context never defines */
    if (getenv("OMNI_PERF")) {
      fprintf(stderr, "[F4] context keys used: %d of %d:", ctx.count,
              MAX_CONTEXT_VARS);
      for (int kv = 0; kv < ctx.count; kv++)
        fprintf(stderr, " %s", ctx.vars[kv].key);
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
    context_set(&ctx, "results_state",
                (cat_mask && enabled_engine_count == 0) ? "empty_category"
                                                        : "empty");
    if (have_local_answer) {
      context_set(&ctx, "answer_text", local_answer.answer);
      if (local_answer.detail[0])
        context_set(&ctx, "answer_detail", local_answer.detail);
      context_set(&ctx, "answer_source_note",
                  beaker_get_locale_value(
                      locale, web_explicit ? "answer_showing_web"
                                           : "answer_local"));
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
      char pn[320];
      int paused_now = build_paused_note(pn, sizeof pn);
      if (paused_now > 0) {
        append_source_note(&ctx, pn);
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

  free(jobs);
  free(all_results);
  free(request_cache_key);

  if (page == 1)
    free_infobox_results(infobox_data);
  free(locale);
  free_user_engine_list(user_engines, user_engine_count);
  free_context(&ctx);

  return 0;
}
