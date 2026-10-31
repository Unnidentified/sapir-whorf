#include "Metrics.h"
#include "../Config.h"
#include "../Scraping/EngineHealth.h"
#include "../Scraping/Scraping.h"
#include <stdio.h>
#include <string.h>

extern SearchEngine ENGINE_REGISTRY[];
extern const int ENGINE_COUNT;

/* Prometheus text format, loopback only. The review's point was that
   total_latency_ms alone hides the tail and that there was no way to see the
   per-engine picture without grepping stderr. */
int metrics_handler(UrlParams *params) {
  (void)params;

  const char *addr = beaker_get_remote_addr();
  int local = addr && (strncmp(addr, "127.", 4) == 0 ||
                       strcmp(addr, "::1") == 0);
  if (!local) {
    send_response("metrics are available on loopback only");
    return 0;
  }

  char out[16384];
  size_t used = 0;
  int n = snprintf(out, sizeof(out),
                   "# HELP omnisearch_up 1 when the server is serving\n"
                   "# TYPE omnisearch_up gauge\n"
                   "omnisearch_up 1\n\n");
  if (n > 0)
    used = (size_t)n;

  for (int i = 0; i < ENGINE_COUNT && used < sizeof(out) - 400; i++) {
    if (!ENGINE_REGISTRY[i].enabled)
      continue;
    EngineHealthEntry *h = engine_health_get(ENGINE_REGISTRY[i].id);
    const char *id = ENGINE_REGISTRY[i].id;

    n = snprintf(
        out + used, sizeof(out) - used,
        "omnisearch_engine_requests{engine=\"%s\"} %lu\n"
        "omnisearch_engine_ok{engine=\"%s\"} %lu\n"
        "omnisearch_engine_errors{engine=\"%s\"} %lu\n"
        "omnisearch_engine_blocked{engine=\"%s\"} %lu\n"
        "omnisearch_engine_cancelled{engine=\"%s\"} %lu\n"
        "omnisearch_engine_breaker_open{engine=\"%s\"} %d\n"
        "omnisearch_engine_junk{engine=\"%s\"} %lu\n"
        "omnisearch_engine_quality{engine=\"%s\"} %.3f\n"
        "omnisearch_engine_latency_ms_avg{engine=\"%s\"} %.1f\n",
        id, h ? h->total_requests : 0UL, id, h ? h->total_ok : 0UL, id,
        h ? h->total_errors : 0UL, id, h ? h->total_blocked : 0UL, id,
        h ? h->total_cancelled : 0UL, id,
        (h && h->state == HEALTH_OPEN) ? 1 : 0, id,
        h ? h->total_junk : 0UL, id,
        (h && h->quality_samples) ? h->quality_ewma : -1.0, id,
        (h && h->total_requests) ? h->total_latency_ms / h->total_requests
                                 : 0.0);
    if (n > 0)
      used += (size_t)n;
  }

  serve_data(out, used, "text/plain; version=0.0.4; charset=utf-8");
  return 0;
}
