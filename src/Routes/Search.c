#include "Search.h"
#include "../Cache/Cache.h"
#include "../Infobox/Calculator.h"
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

#define URL_HASH_TABLE_SIZE 64

typedef struct UrlHashEntry {
  char *url;
  struct UrlHashEntry *next;
} UrlHashEntry;

typedef struct {
  UrlHashEntry *buckets[URL_HASH_TABLE_SIZE];
} UrlHashTable;

static void url_hash_init(UrlHashTable *ht) {
  for (int i = 0; i < URL_HASH_TABLE_SIZE; i++) {
    ht->buckets[i] = NULL;
  }
}

static unsigned int url_hash(const char *url) {
  unsigned char hash[EVP_MAX_MD_SIZE];
  unsigned int hash_len;
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx)
    return 0;
  EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
  EVP_DigestUpdate(ctx, url, strlen(url));
  EVP_DigestFinal_ex(ctx, hash, &hash_len);
  EVP_MD_CTX_free(ctx);
  unsigned int h = 0;
  for (unsigned int i = 0; i < hash_len; i++) {
    h = h * 31 + hash[i];
  }
  return h % URL_HASH_TABLE_SIZE;
}

static int url_hash_contains(UrlHashTable *ht, const char *url) {
  unsigned int idx = url_hash(url);
  for (UrlHashEntry *e = ht->buckets[idx]; e; e = e->next) {
    if (strcmp(e->url, url) == 0) {
      return 1;
    }
  }
  return 0;
}

static int url_hash_insert(UrlHashTable *ht, const char *url) {
  unsigned int idx = url_hash(url);
  for (UrlHashEntry *e = ht->buckets[idx]; e; e = e->next) {
    if (strcmp(e->url, url) == 0) {
      return 0;
    }
  }
  UrlHashEntry *new_entry = malloc(sizeof(UrlHashEntry));
  if (!new_entry)
    return -1;
  new_entry->url = strdup(url);
  if (!new_entry->url) {
    free(new_entry);
    return -1;
  }
  new_entry->next = ht->buckets[idx];
  ht->buckets[idx] = new_entry;
  return 0;
}

static void url_hash_free(UrlHashTable *ht) {
  for (int i = 0; i < URL_HASH_TABLE_SIZE; i++) {
    UrlHashEntry *e = ht->buckets[i];
    while (e) {
      UrlHashEntry *next = e->next;
      free(e->url);
      free(e);
      e = next;
    }
    ht->buckets[i] = NULL;
  }
}

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
    if (!input) return NULL;

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
    if (!domain) return NULL;

    strncpy(domain, start, len);
    domain[len] = '\0';

    return domain;
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

static InfoBoxHandler handlers[] = {
    {is_dictionary_query, fetch_dict_wrapper, NULL},
    {is_calculator_query, fetch_calc_wrapper, NULL},
    {is_unit_conv_query, fetch_unit_wrapper, NULL},
    {is_currency_query, fetch_currency_wrapper, NULL},
    {always_true, fetch_wiki_wrapper, construct_wiki_url},
};
enum { HANDLER_COUNT = sizeof(handlers) / sizeof(handlers[0]) };

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
  data->success = (data->result.title != NULL && data->result.extract != NULL &&
                   data->result.extract[0] != '\0');
  return NULL;
}

static int add_infobox_to_collection(InfoBox *infobox, char ****collection,
                                     int **inner_counts, int current_count) {
  *collection =
      (char ***)realloc(*collection, sizeof(char **) * (current_count + 1));
  *inner_counts =
      (int *)realloc(*inner_counts, sizeof(int) * (current_count + 1));

  (*collection)[current_count] =
      (char **)malloc(sizeof(char *) * INFOBOX_FIELD_COUNT);
  (*collection)[current_count][0] =
      infobox->title ? strdup(infobox->title) : NULL;
  (*collection)[current_count][1] =
      infobox->thumbnail_url ? strdup(infobox->thumbnail_url) : NULL;
  (*collection)[current_count][2] =
      infobox->extract ? strdup(infobox->extract) : NULL;
  (*collection)[current_count][3] = infobox->url ? strdup(infobox->url) : NULL;
  (*collection)[current_count][4] = infobox->url ? strdup(infobox->url) : NULL;
  (*inner_counts)[current_count] = INFOBOX_FIELD_COUNT;

  return current_count + 1;
}

static int add_warning_to_collection(const char *engine_name,
                                     const char *warning_message,
                                     char ****collection, int **inner_counts,
                                     int current_count) {
  char ***new_collection =
      (char ***)malloc(sizeof(char **) * (current_count + 1));
  int *new_inner_counts =
      (int *)malloc(sizeof(int) * (current_count + 1));

  if (!new_collection || !new_inner_counts) {
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  if (*collection && current_count > 0) {
    memcpy(new_collection, *collection, sizeof(char **) * current_count);
  }
  if (*inner_counts && current_count > 0) {
    memcpy(new_inner_counts, *inner_counts, sizeof(int) * current_count);
  }

  free(*collection);
  free(*inner_counts);

  *collection = new_collection;
  *inner_counts = new_inner_counts;

  (*collection)[current_count] = (char **)malloc(sizeof(char *) * 2);
  if (!(*collection)[current_count])
    return current_count;

  (*collection)[current_count][0] = strdup(engine_name ? engine_name : "");
  (*collection)[current_count][1] =
      strdup(warning_message ? warning_message : "");

  if (!(*collection)[current_count][0] || !(*collection)[current_count][1]) {
    free((*collection)[current_count][0]);
    free((*collection)[current_count][1]);
    free((*collection)[current_count]);
    return current_count;
  }

  (*inner_counts)[current_count] = 2;
  return current_count + 1;
}

static const char *warning_message_for_job(const ScrapeJob *job, const char *locale) {
  switch (job->status) {
  case SCRAPE_STATUS_FETCH_ERROR: {
    const char *msg = beaker_get_locale_value(locale, "warning_fetch_error");
    return msg ? msg : "request failed before OmniSearch could read search results.";
  }
  case SCRAPE_STATUS_PARSE_MISMATCH: {
    const char *msg = beaker_get_locale_value(locale, "warning_parse_mismatch");
    return msg ? msg : "returned search results in a format OmniSearch could not parse.";
  }
  case SCRAPE_STATUS_BLOCKED: {
    const char *msg = beaker_get_locale_value(locale, "warning_blocked");
    return msg ? msg : "returned a captcha or another blocking page instead of search results.";
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
  int use_engine = engine_id && engine_id[0] != '\0' &&
                   !engine_id_matches(engine_id, "all");
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
  const char *selected_engine_id = "all";
  int page = 1;
  int btnI = 0;

  char **user_engines = NULL;
  int user_engine_count = 0;
  int has_user_pref = (get_user_engines(&user_engines, &user_engine_count) == 0);

  if (params) {
    for (int i = 0; i < params->count; i++) {
      if (strcmp(params->params[i].key, "q") == 0) {
        raw_query = params->params[i].value;
      } else if (strcmp(params->params[i].key, "p") == 0) {
        int parsed = atoi(params->params[i].value);
        if (parsed > 1)
          page = parsed;
      } else if (strcmp(params->params[i].key, "engine") == 0) {
        selected_engine_id = params->params[i].value;
      } else if (strcmp(params->params[i].key, "btnI") == 0) {
        btnI = atoi(params->params[i].value);
      }
    }
  }

  context_set(&ctx, "query", raw_query);

  char *theme = get_theme("");
  context_set(&ctx, "theme", theme);
  free(theme);

  char *locale = get_locale("en_uk");
  beaker_set_locale(&ctx, locale);

  const char *rate_limit_msg = beaker_get_locale_value(locale, "rate_limit");
  if (!rate_limit_msg) rate_limit_msg = "Slow down! Too many searches from you!";
  const char *no_results_msg = beaker_get_locale_value(locale, "no_results");
  if (!no_results_msg) no_results_msg = "No results found";

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
    if (has_user_pref) {
      for (int i = 0; i < user_engine_count; i++)
        free(user_engines[i]);
      free(user_engines);
    }
    free_context(&ctx);
    return -1;
  }

  const SearchEngine *selected_engine = find_enabled_engine(selected_engine_id);
  if (!selected_engine)
    selected_engine_id = "all";

  context_set(&ctx, "selected_engine", selected_engine_id);
  char *search_href = build_search_href(raw_query, selected_engine_id, 1);
  context_set(&ctx, "search_href", search_href ? search_href : "/search");
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

  pthread_t infobox_threads[HANDLER_COUNT];
  InfoBoxThreadData infobox_data[HANDLER_COUNT];

  for (int i = 0; i < HANDLER_COUNT; i++) {
    infobox_data[i].query = raw_query;
    infobox_data[i].success = i;
    infobox_data[i].result = (InfoBox){NULL};
  }

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      pthread_create(&infobox_threads[i], NULL, infobox_thread_func,
                     &infobox_data[i]);
    }
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
      jobs[engine_idx].query = raw_query;
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
      if (has_user_pref) {
        for (int i = 0; i < user_engine_count; i++)
          free(user_engines[i]);
        free(user_engines);
      }
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
    char ***filter_matrix = NULL;
    int *filter_inner_counts = NULL;
    int filter_count = 0;
    char *all_href = build_search_href(raw_query, "all", 1);

    filter_count = add_link_to_collection(
        all_href, "All",
        selected_engine ? "engine-filter" : "engine-filter active",
        &filter_matrix, &filter_inner_counts, filter_count);
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

      filter_count = add_link_to_collection(filter_href, ENGINE_REGISTRY[i].name,
                                            filter_class, &filter_matrix,
                                            &filter_inner_counts, filter_count);
      free(filter_href);
    }

    if (filter_count > 0) {
      context_set_array_of_arrays(&ctx, "engine_filters", filter_matrix,
                                  filter_count, filter_inner_counts);
      for (int i = 0; i < filter_count; i++) {
        for (int j = 0; j < LINK_FIELD_COUNT; j++)
          free(filter_matrix[i][j]);
        free(filter_matrix[i]);
      }
      free(filter_matrix);
      free(filter_inner_counts);
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
        for (int j = 0; j < enabled_engine_count; j++) {
          for (int k = 0; k < jobs[j].results_count; k++) {
            free(all_results[j][k].url);
            free(all_results[j][k].title);
            free(all_results[j][k].snippet);
          }
          free(all_results[j]);
        }
        if (page == 1) {
          for (int j = 0; j < HANDLER_COUNT; j++) {
            if (infobox_data[j].success) {
              free_infobox(&infobox_data[j].result);
            }
          }
        }
        free(request_cache_key);
        if (has_user_pref) {
          for (int i = 0; i < user_engine_count; i++)
            free(user_engines[i]);
          free(user_engines);
        }
        free_context(&ctx);
        if (redirect_url) {
          send_redirect(redirect_url);
          free(redirect_url);
        }
        return 0;
      }
    }
    for (int i = 0; i < enabled_engine_count; i++) {
      free(all_results[i]);
    }
    if (page == 1) {
      for (int i = 0; i < HANDLER_COUNT; i++) {
        if (infobox_data[i].success) {
          free_infobox(&infobox_data[i].result);
        }
      }
    }
    free(request_cache_key);
    if (has_user_pref) {
      for (int i = 0; i < user_engine_count; i++)
        free(user_engines[i]);
      free(user_engines);
    }
    free_context(&ctx);
    char no_results_html[128];
    snprintf(no_results_html, sizeof(no_results_html), "<h1>%s</h1>", no_results_msg);
    send_response(no_results_html);
    return 0;
  }
  
  char ***infobox_matrix = NULL;
  int *infobox_inner_counts = NULL;
  int infobox_count = 0;

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      if (infobox_data[i].success) {
        infobox_count =
            add_infobox_to_collection(&infobox_data[i].result, &infobox_matrix,
                                      &infobox_inner_counts, infobox_count);
      }
    }
  }

  if (infobox_count > 0) {
    context_set_array_of_arrays(&ctx, "infoboxes", infobox_matrix,
                                infobox_count, infobox_inner_counts);
    for (int i = 0; i < infobox_count; i++) {
      for (int j = 0; j < INFOBOX_FIELD_COUNT; j++)
        free(infobox_matrix[i][j]);
      free(infobox_matrix[i]);
    }
    free(infobox_matrix);
    free(infobox_inner_counts);
  }

  int warning_count = 0;
  for (int i = 0; i < enabled_engine_count; i++) {
    if (warning_message_for_job(&jobs[i], locale))
      warning_count++;
  }

  if (warning_count > 0) {
    char ***warning_matrix = NULL;
    int *warning_inner_counts = NULL;
    int warning_index = 0;

    for (int i = 0; i < enabled_engine_count; i++) {
      const char *warning_message = warning_message_for_job(&jobs[i], locale);
      if (!warning_message)
        continue;

      warning_index = add_warning_to_collection(
          jobs[i].engine->name, warning_message, &warning_matrix,
          &warning_inner_counts, warning_index);
    }

    if (warning_index > 0) {
      context_set_array_of_arrays(&ctx, "engine_warnings", warning_matrix,
                                  warning_index, warning_inner_counts);
    }

    if (warning_matrix) {
      for (int i = 0; i < warning_index; i++) {
        free(warning_matrix[i][0]);
        free(warning_matrix[i][1]);
        free(warning_matrix[i]);
      }
      free(warning_matrix);
    }
    if (warning_inner_counts)
      free(warning_inner_counts);
  }

  int total_results = 0;
  for (int i = 0; i < enabled_engine_count; i++) {
    total_results += jobs[i].results_count;
  }

  if (total_results > 0) {
    char ***results_matrix = (char ***)malloc(sizeof(char **) * total_results);
    int *results_inner_counts = (int *)malloc(sizeof(int) * total_results);
    if (!results_matrix || !results_inner_counts) {
      char *html = render_template("results.html", &ctx);
      if (html) {
        send_response(html);
        free(html);
      }
      for (int i = 0; i < enabled_engine_count; i++)
        free(all_results[i]);
      if (page == 1) {
        for (int i = 0; i < HANDLER_COUNT; i++) {
          if (infobox_data[i].success) {
            free_infobox(&infobox_data[i].result);
          }
        }
      }
      free(request_cache_key);
      if (has_user_pref) {
        for (int i = 0; i < user_engine_count; i++)
          free(user_engines[i]);
        free(user_engines);
      }
      free_context(&ctx);
      return 0;
    }
    int unique_count = 0;
    UrlHashTable url_table;
    url_hash_init(&url_table);

    for (int i = 0; i < enabled_engine_count; i++) {
      for (int j = 0; j < jobs[i].results_count; j++) {
        char *display_url = all_results[i][j].url;

        if (url_hash_contains(&url_table, display_url)) {
          free(all_results[i][j].url);
          free(all_results[i][j].title);
          free(all_results[i][j].snippet);
          continue;
        }

        url_hash_insert(&url_table, display_url);

        results_matrix[unique_count] =
            (char **)malloc(sizeof(char *) * RESULT_FIELD_COUNT);
        if (!results_matrix[unique_count]) {
          free(all_results[i][j].url);
          free(all_results[i][j].title);
          free(all_results[i][j].snippet);
          continue;
        }
        char *pretty_url = pretty_display_url(display_url);
        char *base_url = get_base_url(display_url);

        results_matrix[unique_count][0] = strdup(display_url);
        results_matrix[unique_count][1] = strdup(pretty_url);
        results_matrix[unique_count][2] = all_results[i][j].title
                                              ? strdup(all_results[i][j].title)
                                              : strdup("Untitled");
        results_matrix[unique_count][3] =
            all_results[i][j].snippet ? strdup(all_results[i][j].snippet)
                                      : strdup("");
        results_matrix[unique_count][4] = strdup(base_url ? base_url : "");
        results_matrix[unique_count][5] = strdup("");

        results_inner_counts[unique_count] = RESULT_FIELD_COUNT;

        free(pretty_url);
        free(base_url);
        free(all_results[i][j].url);
        free(all_results[i][j].title);
        free(all_results[i][j].snippet);

        unique_count++;
      }
      free(all_results[i]);
    }

    context_set_array_of_arrays(&ctx, "results", results_matrix, unique_count,
                                results_inner_counts);

    char ***pager_matrix = NULL;
    int *pager_inner_counts = NULL;
    SearchHrefData href_data = { .query = raw_query, .engine_id = selected_engine_id };
    int pager_count = build_pagination(page, search_href_builder,
                                       &href_data, &pager_matrix,
                                       &pager_inner_counts);

    if (pager_count > 0) {
      context_set_array_of_arrays(&ctx, "pagination_links", pager_matrix,
                                  pager_count, pager_inner_counts);
      for (int i = 0; i < pager_count; i++) {
        for (int j = 0; j < LINK_FIELD_COUNT; j++)
          free(pager_matrix[i][j]);
        free(pager_matrix[i]);
      }
      free(pager_matrix);
      free(pager_inner_counts);
    }

    char *html = render_template("results.html", &ctx);
    if (html) {
      send_response(html);
      free(html);
    }

    for (int i = 0; i < unique_count; i++) {
      for (int j = 0; j < RESULT_FIELD_COUNT; j++)
        free(results_matrix[i][j]);
      free(results_matrix[i]);
    }
    free(results_matrix);
    free(results_inner_counts);
    url_hash_free(&url_table);
  } else {
    char *html = render_template("results.html", &ctx);
    if (html) {
      send_response(html);
      free(html);
    }

    for (int i = 0; i < enabled_engine_count; i++) {
      free(all_results[i]);
    }
  }

  free(request_cache_key);

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      if (infobox_data[i].success) {
        free_infobox(&infobox_data[i].result);
      }
    }
  }
  free(locale);
  if (has_user_pref) {
    for (int i = 0; i < user_engine_count; i++)
      free(user_engines[i]);
    free(user_engines);
  }
  free_context(&ctx);

  return 0;
}
