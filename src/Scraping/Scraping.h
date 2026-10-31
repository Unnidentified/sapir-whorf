#ifndef SCRAPING_H
#define SCRAPING_H

#include <curl/curl.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <time.h>

typedef struct {
  char *url;
  char *title;
  char *snippet;
} SearchResult;

typedef enum {
  V_WEB = 1u << 0,
  V_CODE = 1u << 1,
  V_ACADEMIC = 1u << 2,
  V_ENTITY = 1u << 3,
  V_NEWS = 1u << 4,
  V_MEDIA = 1u << 5
} EngineVertical;

typedef int (*ParserFunc)(const char *engine_name, xmlDocPtr doc,
                          SearchResult **out_results, int max_results);

/* JSON/Atom APIs: parse the raw body directly */
typedef int (*BodyParserFn)(const char *body, size_t len, const char *lang,
                            SearchResult **out_results, int max_results);

/* custom URL template for engines that are not "?q=...&page=" shaped */
typedef char *(*UrlBuilderFn)(const char *encoded_query, int page,
                              const char *lang);

typedef struct {
  const char *id;
  const char *name;
  const char *base_url;
  const char *host_header;
  const char *referer;

  const char *page_param;
  int page_multiplier;
  int page_base;
  ParserFunc parser;
  BodyParserFn body_parser;
  UrlBuilderFn build_url;
  int enabled;
  int is_xml;
  int timeout_secs;    /* 0 = CURL_TIMEOUT_SECS */
  int verticals;       /* EngineVertical bitmask */
  double weight;       /* routing weight, 1.0 default */
  int supports_site;   /* passes site:<host> through */
  /* Which search index actually answers this engine. Yahoo's web results are
     largely Bing's and DuckDuckGo's partly so; counting them as independent
     opinions inflates consensus and lets one index's failure look like three
     engines agreeing. Engines sharing a group are one vote. */
  const char *index_group;
  /* Which User-Agent this engine gets: "chrome" (default), "firefox",
     "safari", or NULL to keep the old per-id hash. Picking by hash was
     arbitrary, and Brave refuses every Chrome UA it was being handed while
     serving Firefox and Safari normally - see docs/G1-engines.md. */
  const char *user_agent;
  /* send the Sec-Fetch-* navigation headers a browser sends. Measured need:
     DDG's html. endpoint answers a shell without them and results with them. */
  int browser_headers;
  /* per-engine outbound rate limit, 0 = the global [health] setting */
  int pace_per_minute;
  /* cookies to seed the jar with on first use (locale selectors and the like) */
  const char *init_cookies;
} SearchEngine;

typedef struct {
  char *memory;
  size_t size;
  size_t capacity;
} MemoryBuffer;

/* C8: why a fetch failed matters. A DNS blip or a connect timeout is our
   network, not the engine refusing us, and must not count against its breaker -
   otherwise one Wi-Fi hiccup opens every breaker and the exponential backoff
   persists that verdict for hours. */
typedef enum {
  FETCH_FAIL_NONE = 0,
  FETCH_FAIL_TRANSPORT,  /* DNS, connect, TLS, timeout: probably our side    */
  FETCH_FAIL_REFUSED,    /* HTTP 429/202/403/5xx or a challenge page         */
  FETCH_FAIL_PARSE       /* we got bytes and could not read them             */
} FetchFailureKind;

typedef enum {
  SCRAPE_STATUS_PENDING,
  SCRAPE_STATUS_OK,
  SCRAPE_STATUS_EMPTY,
  SCRAPE_STATUS_FETCH_ERROR,
  SCRAPE_STATUS_PARSE_MISMATCH,
  SCRAPE_STATUS_BLOCKED,
  /* the batch deadline stopped us before the engine could answer: not a
     failure, and it must not count against the engine's health */
  SCRAPE_STATUS_CANCELLED,
  /* G2: our own pacing left this engine out of the batch. Not a failure of
     the engine and not a failure of the network - it never got asked. */
  SCRAPE_STATUS_SKIPPED,
} ScrapeStatus;

typedef struct {
  const SearchEngine *engine;
  char *query;
  char query_text[512];
  char lang[8];
  char region[8];
  char accept_language[64];
  SearchResult **out_results;
  int max_results;
  int page;
  CURL *handle;
  MemoryBuffer response;
  int results_count;
  long http_status;
  int retry_after;
  FetchFailureKind failure_kind;
  int stale;                 /* served from the last-known-good cache */
  double elapsed_ms;
  struct timespec started;
  ScrapeStatus status;
} ScrapeJob;

extern SearchEngine ENGINE_REGISTRY[];
extern const int ENGINE_COUNT;
void apply_engines_config(const char *engines_str);
void engine_set_enabled(const char *engine_id, int enabled);
void configure_yacy_engine(const char *instance);

size_t write_memory_callback(void *contents, size_t size, size_t nmemb,
                             void *userp);
const char *get_random_user_agent(void);
void configure_curl_handle(CURL *curl, const char *full_url,
                           MemoryBuffer *chunk, struct curl_slist *headers,
                           long timeout_secs, const char *engine_id,
                           const char *ua_profile, int browser_headers);
const char *get_engine_user_agent(const char *engine_id);
const char *get_engine_user_agent_profile(const char *profile,
                                          const char *engine_id);
char *build_search_url(const char *base_url, const char *page_param,
                       int page_multiplier, int page_base,
                       const char *encoded_query, int page);
struct curl_slist *build_request_headers(const char *host_header,
                                         const char *referer,
                                         const char *accept_language,
                                         int browser_headers);
void http_delay(void);

/* set per request: wait for every engine instead of taking the early exit */
extern __thread int request_is_patient;

xmlXPathContextPtr create_xpath_context(xmlDocPtr doc);
void free_xpath_objects(xmlXPathContextPtr ctx, xmlXPathObjectPtr obj);
SearchResult *alloc_results_array(int capacity, int max_results);
int assign_result(SearchResult *result, char *url, char *title, char *snippet,
                  int unescape);
void free_xml_node_list(char *title, char *url, char *snippet);

int scrape_engine(const SearchEngine *engine, const char *query,
                  SearchResult **out_results, int max_results);

int scrape_engines_parallel(ScrapeJob *jobs, int num_jobs);

#endif
