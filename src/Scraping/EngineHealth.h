#ifndef ENGINE_HEALTH_H
#define ENGINE_HEALTH_H

#include <time.h>

typedef enum {
  HEALTH_CLOSED = 0,
  HEALTH_OPEN,
  HEALTH_HALF_OPEN
} BreakerState;

typedef struct {
  char id[32];
  BreakerState state;
  int consecutive_failures;
  int trips;              /* times the breaker has opened without a success */
  time_t open_until;
  unsigned long total_requests;
  unsigned long total_ok;
  unsigned long total_blocked;
  unsigned long total_errors;
  double total_latency_ms;
  /* A2: quality of what the engine returns, independent of whether it answered
     at all. An engine can be perfectly healthy (200, ten results, no warning)
     and still be returning junk that the coverage floor throws away - that is
     what Bing did for "best podcast app". */
  unsigned long total_cancelled;  /* cut off by our own batch deadline */
  unsigned long quality_samples;
  double quality_ewma;      /* 0..1, share of its results that survived      */
} EngineHealthEntry;

void engine_health_init(const char *cache_dir);
void engine_health_set_limits(int failure_threshold, int cooldown_seconds);

/* false when the breaker is open and the cooldown has not elapsed */
int engine_health_allow(const char *id);

/* status: SCRAPE_STATUS_* from Scraping.h (kept as int to avoid a cycle) */
void engine_health_record(const char *id, int status, long http_status,
                          double latency_ms, int retry_after_secs);

EngineHealthEntry *engine_health_get(const char *id);
const char *engine_health_state_name(BreakerState state);
void engine_health_persist(void);

/* C8: when most of a batch failed for transport reasons, the cause is our
   network. Engines still count requests but no failure is charged to them. */
void engine_health_set_batch_outage(int is_outage);

/* A2: feed back how many of this engine's results survived ranking. `useful`
   is the count that passed the floor and were backed by another index. */
void engine_health_record_quality(const char *id, int returned, int useful);

/* C12: an engine that is cancelled every time is either consistently slow or
   consistently a bad fit for the query mix. Recording it makes that visible. */
void engine_health_record_cancelled(const char *id);

#endif
