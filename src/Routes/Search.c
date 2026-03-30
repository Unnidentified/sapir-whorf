#include "Search.h"
#include "../Infobox/Calculator.h"
#include "../Infobox/CurrencyConversion.h"
#include "../Infobox/Dictionary.h"
#include "../Infobox/UnitConversion.h"
#include "../Infobox/Wikipedia.h"
#include "../Scraping/Scraping.h"
#include "../Utility/Display.h"
#include "../Utility/Unescape.h"
#include "../Utility/Utility.h"
#include "Config.h"
#include <ctype.h>
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
  LINK_FIELD_COUNT = 3,
  PAGER_WINDOW_SIZE = 5,
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
                   strlen(data->result.extract) > 10);
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

static int add_link_to_collection(const char *href, const char *label,
                                  const char *class_name, char ****collection,
                                  int **inner_counts, int current_count) {
  char ***old_collection = *collection;
  int *old_inner_counts = *inner_counts;
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

  *collection = new_collection;
  *inner_counts = new_inner_counts;

  (*collection)[current_count] =
      (char **)malloc(sizeof(char *) * LINK_FIELD_COUNT);
  if (!(*collection)[current_count]) {
    *collection = old_collection;
    *inner_counts = old_inner_counts;
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  (*collection)[current_count][0] = strdup(href ? href : "");
  (*collection)[current_count][1] = strdup(label ? label : "");
  (*collection)[current_count][2] = strdup(class_name ? class_name : "");

  if (!(*collection)[current_count][0] || !(*collection)[current_count][1] ||
      !(*collection)[current_count][2]) {
    free((*collection)[current_count][0]);
    free((*collection)[current_count][1]);
    free((*collection)[current_count][2]);
    free((*collection)[current_count]);
    *collection = old_collection;
    *inner_counts = old_inner_counts;
    free(new_collection);
    free(new_inner_counts);
    return current_count;
  }

  (*inner_counts)[current_count] = LINK_FIELD_COUNT;

  free(old_collection);
  free(old_inner_counts);
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

static const char *warning_message_for_job(const ScrapeJob *job) {
  switch (job->status) {
  case SCRAPE_STATUS_FETCH_ERROR:
    return "request failed before OmniSearch could read search results.";
  case SCRAPE_STATUS_PARSE_MISMATCH:
    return "returned search results in a format OmniSearch could not parse.";
  case SCRAPE_STATUS_BLOCKED:
    return "returned a captcha or another blocking page instead of search "
           "results.";
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

int results_handler(UrlParams *params) {
  TemplateContext ctx = new_context();
  char *raw_query = "";
  const char *selected_engine_id = "all";
  int page = 1;
  int btnI = 0;

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

  char page_str[16];
  snprintf(page_str, sizeof(page_str), "%d", page);
  context_set(&ctx, "page", page_str);

  if (!raw_query || strlen(raw_query) == 0) {
    send_redirect("/");
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
        (!selected_engine || &ENGINE_REGISTRY[i] == selected_engine)) {
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
        (!selected_engine || &ENGINE_REGISTRY[i] == selected_engine)) {
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

  int filter_engine_count = 0;
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (ENGINE_REGISTRY[i].enabled)
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
      if (!ENGINE_REGISTRY[i].enabled)
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
    free_context(&ctx);
    send_response("<h1>No results found</h1>");
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
    if (warning_message_for_job(&jobs[i]))
      warning_count++;
  }

  if (warning_count > 0) {
    char ***warning_matrix = NULL;
    int *warning_inner_counts = NULL;
    int warning_index = 0;

    for (int i = 0; i < enabled_engine_count; i++) {
      const char *warning_message = warning_message_for_job(&jobs[i]);
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
    char **seen_urls = (char **)malloc(sizeof(char *) * total_results);
    if (!results_matrix || !results_inner_counts || !seen_urls) {
      if (results_matrix)
        free(results_matrix);
      if (results_inner_counts)
        free(results_inner_counts);
      if (seen_urls)
        free(seen_urls);
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
      free_context(&ctx);
      return 0;
    }
    int unique_count = 0;

    for (int i = 0; i < enabled_engine_count; i++) {
      for (int j = 0; j < jobs[i].results_count; j++) {
        char *display_url = all_results[i][j].url;

        int is_duplicate = 0;
        for (int k = 0; k < unique_count; k++) {
          if (strcmp(seen_urls[k], display_url) == 0) {
            is_duplicate = 1;
            break;
          }
        }

        if (is_duplicate) {
          free(all_results[i][j].url);
          free(all_results[i][j].title);
          free(all_results[i][j].snippet);
          continue;
        }

        seen_urls[unique_count] = strdup(display_url);
        if (!seen_urls[unique_count]) {
          free(all_results[i][j].url);
          free(all_results[i][j].title);
          free(all_results[i][j].snippet);
          continue;
        }
        results_matrix[unique_count] =
            (char **)malloc(sizeof(char *) * RESULT_FIELD_COUNT);
        if (!results_matrix[unique_count]) {
          free(seen_urls[unique_count]);
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
    int pager_count = 0;
    int pager_start = page <= 3 ? 1 : page - 2;
    int pager_end = pager_start + PAGER_WINDOW_SIZE - 1;

    if (page > 3) {
      char *first_href = build_search_href(raw_query, selected_engine_id, 1);
      pager_count = add_link_to_collection(first_href, "First", "pagination-btn",
                                           &pager_matrix, &pager_inner_counts,
                                           pager_count);
      free(first_href);
    }

    if (page > 1) {
      char *prev_href =
          build_search_href(raw_query, selected_engine_id, page - 1);
      pager_count = add_link_to_collection(prev_href, "Prev", "pagination-btn",
                                           &pager_matrix, &pager_inner_counts,
                                           pager_count);
      free(prev_href);
    }

    for (int i = pager_start; i <= pager_end; i++) {
      char label[16];
      snprintf(label, sizeof(label), "%d", i);
      char *page_href = build_search_href(raw_query, selected_engine_id, i);
      pager_count = add_link_to_collection(
          page_href, label,
          i == page ? "pagination-btn pagination-current" : "pagination-btn",
          &pager_matrix, &pager_inner_counts, pager_count);
      free(page_href);
    }

    char *next_href = build_search_href(raw_query, selected_engine_id, page + 1);
    pager_count = add_link_to_collection(next_href, "Next", "pagination-btn",
                                         &pager_matrix, &pager_inner_counts,
                                         pager_count);
    free(next_href);

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
      free(seen_urls[i]);
    }
    free(seen_urls);
    free(results_matrix);
    free(results_inner_counts);
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

  if (page == 1) {
    for (int i = 0; i < HANDLER_COUNT; i++) {
      if (infobox_data[i].success) {
        free_infobox(&infobox_data[i].result);
      }
    }
  }
  free_context(&ctx);

  return 0;
}
