#include <beaker.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "Cache/Cache.h"
#include "Config.h"
#include "Scraping/ApiEngines.h"
#include "Infobox/Wikipedia.h"
#include "Proxy/Proxy.h"
#include "Routes/History.h"
#include "Routes/Home.h"
#include "Routes/ImageProxy.h"
#include "Routes/Images.h"
#include "Routes/Info.h"
#include "Routes/Metrics.h"
#include "Routes/Search.h"
#include "Limiter/RateLimit.h"
#include "Routes/Settings.h"
#include "Routes/SettingsSave.h"
#include "Ranking/Ranker.h"
#include "Scraping/EngineHealth.h"
#include "Scraping/Scraping.h"
#include "Utility/Utility.h"

/* Runtime sweeping: expired entries were only removed when the same key was
   read again, so a long-lived process grew the cache without bound. One thread
   wakes hourly and sweeps each class at its own TTL. */
static pthread_mutex_t sweeper_mutex = PTHREAD_MUTEX_INITIALIZER;
static int sweeper_started = 0;

static void *cache_sweeper_thread(void *arg) {
  time_t search_ttl = (time_t)(intptr_t)arg;
  for (;;) {
    for (int i = 0; i < 60; i++) {
      sleep(60);
      /* stop cleanly on shutdown */
      pthread_mutex_lock(&sweeper_mutex);
      int run = sweeper_started;
      pthread_mutex_unlock(&sweeper_mutex);
      if (!run)
        return NULL;
    }
    cache_cleanup(search_ttl);
  }
  return NULL;
}

static void start_cache_sweeper(int search_ttl) {
  pthread_t tid;
  pthread_mutex_lock(&sweeper_mutex);
  if (sweeper_started) {
    pthread_mutex_unlock(&sweeper_mutex);
    return;
  }
  sweeper_started = 1;
  pthread_mutex_unlock(&sweeper_mutex);

  if (pthread_create(&tid, NULL, cache_sweeper_thread,
                     (void *)(intptr_t)(search_ttl > 0 ? search_ttl : 60)) !=
      0) {
    pthread_mutex_lock(&sweeper_mutex);
    sweeper_started = 0;
    pthread_mutex_unlock(&sweeper_mutex);
    fprintf(stderr, "[WARN] Could not start cache sweeper thread\n");
    return;
  }
  pthread_detach(tid);
  fprintf(stderr, "[INFO] Cache sweeper running hourly\n");
}

extern Config global_config;

int handle_opensearch(UrlParams *params) {
  (void)params;
  TemplateContext ctx = new_context();

  const char *http_host = beaker_get_header("Host");
  if (http_host == NULL) {
    http_host = "localhost";
  }

  const char *req_scheme =
      "https"; // not sure if it's a good idea to just assume https, but you
               // should probably be using https for anything other than testing
               // or local network anyways.

  if (strncmp(http_host, "localhost", 9) == 0 ||
      strncmp(http_host, "127.", 4) == 0 ||
      strncmp(http_host, "192.168.", 8) == 0 ||
      strncmp(http_host, "10.", 3) == 0) {
    req_scheme = "http";
  }

  context_set(&ctx, "domain", http_host);
  context_set(&ctx, "scheme", req_scheme);

  char *rendered = render_template("opensearch.xml", &ctx);
  serve_data(rendered, strlen(rendered),
             "application/opensearchdescription+xml");

  free(rendered);
  free_context(&ctx);

  return 0;
}

int main(int argc, char **argv) {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &mask, NULL);

  LIBXML_TEST_VERSION
  xmlInitParser();

  curl_global_init(CURL_GLOBAL_DEFAULT);

  Config cfg;
  config_set_defaults(&cfg);

  /* --config PATH overrides the file, --check-config validates and exits */
  const char *config_path = getenv("OMNISEARCH_CONFIG");
  int check_only = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else if (strcmp(argv[i], "--check-config") == 0) {
      check_only = 1;
    }
  }
  if (!config_path || !config_path[0])
    config_path = "config.ini";

  int config_rc = load_config(config_path, &cfg);
  if (config_rc != 0 && config_rc != 2) {
    fprintf(stderr, "[WARN] Could not load %s, using defaults\n", config_path);
  }

  if (check_only) {
    /* report the effective engine set, so resolve the list first */
    apply_engines_config(cfg.engines);
    printf("config: %s\n", config_path);
    printf("  host=%s port=%d locale=%s\n", cfg.host, cfg.port,
           cfg.default_locale);
    printf("  cache=%s ttl_search=%d ttl_infobox=%d ttl_image=%d\n",
           cfg.cache_dir, cfg.cache_ttl_search, cfg.cache_ttl_infobox,
           cfg.cache_ttl_image);
    printf("  breaker_threshold=%d cooldown=%ds\n", cfg.breaker_threshold,
           cfg.breaker_cooldown);
    printf("  rate_limit: search=%d/%ds trusted_proxies=\"%s\"\n",
           cfg.rate_limit_search_requests, cfg.rate_limit_search_interval,
           cfg.trusted_proxies);
    printf("  engines=\"%s\"\n", cfg.engines);
    /* the effective set, not just the string: this is where "*" quietly meant
       "every entry including the disabled ones" */
    printf("  engines_enabled=");
    {
      int printed = 0;
      for (int i = 0; i < ENGINE_COUNT; i++) {
        if (!ENGINE_REGISTRY[i].enabled)
          continue;
        printf("%s%s", printed ? "," : "", ENGINE_REGISTRY[i].id);
        printed++;
      }
      if (!printed)
        printf("(none)");
      printf("\n");
    }
    printf("config: %s\n",
           config_rc == 0 ? "OK" : "loaded with warnings (see above)");
    return config_rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
  }

  set_default_locale(cfg.default_locale);
  init_themes("static");
  rank_init("static/authority.tsv", "static/spam_hosts.txt");

  global_config = cfg;

  int loaded = beaker_load_locales();
  if (loaded > 0) {
    fprintf(stderr, "[INFO] Loaded %d locales\n", loaded);
  } else {
    fprintf(stderr, "[WARN] No locales loaded (make sure to run from "
                    "omnisearch directory)\n");
  }

  apply_engines_config(cfg.engines);

  /* Google and SearXNG stay off until they are actually configured: a
     credential-less API engine would only add a failed source per query */
  engine_set_enabled("google", 0);
  engine_set_enabled("searxng", 0);
  if (cfg.google_cse_key[0] != '\0' && cfg.google_cse_cx[0] != '\0') {
    api_google_cse_configure(cfg.google_cse_key, cfg.google_cse_cx);
    engine_set_enabled("google", 1);
    fprintf(stderr, "[INFO] Google Programmable Search enabled\n");
  }
  /* H3: the element protocol mints its own token from
     www.google.com/cse/cse.js, so a cx alone is enough. A pasted
     google_cse_tok is an override for when that endpoint changes. Disabled by
     default either way: it is an undocumented endpoint and it borrows another
     site's CX, which is the owner's call. */
  if (cfg.google_cse_cx[0] != '\0') {
    api_google_cse_element_configure(cfg.google_cse_cx, cfg.google_cse_tok);
    engine_set_enabled("google_cse_element", 1);
    engine_health_set_engine_pace("google_cse_element", 10);
    fprintf(stderr,
            "[INFO] Google Programmable Search Element enabled for cx=%s%s "
            "(experimental, 10 req/min)\n",
            cfg.google_cse_cx,
            cfg.google_cse_tok[0] ? " using the supplied token" : "");
  }
  if (cfg.searxng_url[0] != '\0') {
    api_searxng_configure(cfg.searxng_url, cfg.searxng_engines);
    engine_set_enabled("searxng", 1);
    fprintf(stderr, "[INFO] SearXNG bridge enabled at %s, engines: %s\n",
            cfg.searxng_url, api_searxng_engines());
    /* J5.6: a silently dead bridge is worse than a noisy one */
    api_searxng_validate();
  }
  configure_yacy_engine(cfg.yacy_instance);

  if (cfg.yacy_instance[0] != '\0') {
    int yacy_enabled = 0;
    for (int i = 0; i < ENGINE_COUNT; i++) {
      if (strcmp(ENGINE_REGISTRY[i].id, "yacy") == 0) {
        yacy_enabled = ENGINE_REGISTRY[i].enabled;
        break;
      }
    }
    if (!yacy_enabled) {
      fprintf(
          stderr,
          "[INFO] YaCy instance configured but the yacy engine is not "
          "enabled (add it to the engines list, e.g. engines=\"*,yacy\")\n");
    }
  }

  if (cache_init(cfg.cache_dir) != 0) {
    fprintf(stderr,
            "[WARN] Failed to initialize cache, continuing without caching\n");
  } else {
    fprintf(stderr, "[INFO] Cache initialized at %s\n", cfg.cache_dir);
    cache_cleanup(cfg.cache_ttl_search);
    start_cache_sweeper(cfg.cache_ttl_search);
    engine_health_set_limits(cfg.breaker_threshold, cfg.breaker_cooldown);
    engine_health_set_pacing(cfg.pace_per_minute, cfg.pace_burst);
    engine_health_init(cfg.cache_dir);
    if (getenv("OMNI_PERF")) {
      for (int i = 0; i < ENGINE_COUNT; i++)
        fprintf(stderr, "[REG] %-12s enabled=%d web=%d site=%d ua=%s\n",
                ENGINE_REGISTRY[i].id, ENGINE_REGISTRY[i].enabled,
                !!(ENGINE_REGISTRY[i].verticals & V_WEB),
                ENGINE_REGISTRY[i].supports_site,
                ENGINE_REGISTRY[i].user_agent ? ENGINE_REGISTRY[i].user_agent
                                              : "-");
    }
  }

  set_cache_ttl_search(cfg.cache_ttl_search);
  set_cache_ttl_infobox(cfg.cache_ttl_infobox);
  set_cache_ttl_image(cfg.cache_ttl_image);

  if (cfg.proxy_list_file[0] != '\0') {
    if (load_proxy_list(cfg.proxy_list_file) < 0) {
      fprintf(stderr,
              "[WARN] Failed to load proxy list, continuing without proxies\n");
    }
  }

  max_proxy_retries = cfg.max_proxy_retries;
  set_proxy_config(cfg.proxy, cfg.randomize_username, cfg.randomize_password);

  if (proxy_url[0] != '\0') {
    /* proxy URLs routinely carry credentials: never write them verbatim */
    char redacted[512];
    snprintf(redacted, sizeof(redacted), "%s", proxy_url);
    char *at = strchr(redacted, '@');
    if (at) {
      char *scheme = strstr(redacted, "://");
      if (scheme && scheme < at) {
        char tail[512];
        snprintf(tail, sizeof(tail), "%s", at + 1);
        snprintf(scheme + 3, (size_t)(redacted + sizeof(redacted) - scheme - 3),
                 "***:***@%s", tail);
      }
    }
    fprintf(stderr, "[INFO] Using proxy: %s\n", redacted);
  } else if (proxy_count > 0) {
    fprintf(stderr, "[INFO] Using %d proxies from %s\n", proxy_count,
            cfg.proxy_list_file);
  }

  /* C6: only a proxy we name may set X-Forwarded-For */
  rate_limit_set_trusted_proxies(cfg.trusted_proxies);

  set_handler("/", home_handler);
  set_handler("/opensearch.xml", handle_opensearch);
  set_handler("/search", results_handler);
  set_handler("/images", images_handler);
  set_handler("/proxy", image_proxy_handler);
  set_handler("/info", info_handler);
  set_handler("/metrics", metrics_handler);
  set_handler("/settings", settings_handler);
  set_handler("/history", history_handler);
  set_handler("/save_settings", settings_save_handler);

  fprintf(stderr, "[INFO] Starting Omnisearch on %s:%d\n", cfg.host, cfg.port);

  int result = beaker_run(cfg.host, cfg.port);

  if (result != 0) {
    fprintf(stderr, "[ERROR] Beaker server failed to start.\n");
    curl_global_cleanup();
    xmlCleanupParser();
    return EXIT_FAILURE;
  }

  engine_health_persist();
  curl_global_cleanup();
  xmlCleanupParser();
  beaker_free_locales();
  free_proxy_list();
  cache_shutdown();
  return EXIT_SUCCESS;
}
