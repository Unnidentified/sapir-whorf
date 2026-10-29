#include <beaker.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#include "Cache/Cache.h"
#include "Config.h"
#include "Infobox/Wikipedia.h"
#include "Proxy/Proxy.h"
#include "Routes/History.h"
#include "Routes/Home.h"
#include "Routes/ImageProxy.h"
#include "Routes/Images.h"
#include "Routes/Info.h"
#include "Routes/Search.h"
#include "Routes/Settings.h"
#include "Routes/SettingsSave.h"
#include "Scraping/Scraping.h"
#include "Utility/Utility.h"

Config global_config;

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

int main() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &mask, NULL);

  LIBXML_TEST_VERSION
  xmlInitParser();

  curl_global_init(CURL_GLOBAL_DEFAULT);

  Config cfg;
  config_set_defaults(&cfg);

  if (load_config("config.ini", &cfg) != 0) {
    fprintf(stderr, "[WARN] Could not load config file, using defaults\n");
  }

  set_default_locale(cfg.default_locale);
  init_themes("static");

  global_config = cfg;

  int loaded = beaker_load_locales();
  if (loaded > 0) {
    fprintf(stderr, "[INFO] Loaded %d locales\n", loaded);
  } else {
    fprintf(stderr, "[WARN] No locales loaded (make sure to run from "
                    "omnisearch directory)\n");
  }

  apply_engines_config(cfg.engines);
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
    fprintf(stderr, "[INFO] Using proxy: %s\n", proxy_url);
  } else if (proxy_count > 0) {
    fprintf(stderr, "[INFO] Using %d proxies from %s\n", proxy_count,
            cfg.proxy_list_file);
  }

  set_handler("/", home_handler);
  set_handler("/opensearch.xml", handle_opensearch);
  set_handler("/search", results_handler);
  set_handler("/images", images_handler);
  set_handler("/proxy", image_proxy_handler);
  set_handler("/info", info_handler);
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

  curl_global_cleanup();
  xmlCleanupParser();
  beaker_free_locales();
  free_proxy_list();
  cache_shutdown();
  return EXIT_SUCCESS;
}
