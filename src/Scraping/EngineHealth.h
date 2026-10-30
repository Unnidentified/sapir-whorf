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
  time_t open_until;
  unsigned long total_requests;
  unsigned long total_ok;
  unsigned long total_blocked;
  unsigned long total_errors;
  double total_latency_ms;
} EngineHealthEntry;

void engine_health_init(const char *cache_dir);
void engine_health_set_limits(int failure_threshold, int cooldown_seconds);

/* false when the breaker is open and the cooldown has not elapsed */
int engine_health_allow(const char *id);

/* status: SCRAPE_STATUS_* from Scraping.h (kept as int to avoid a cycle) */
void engine_health_record(const char *id, int status, long http_status,
                          double latency_ms);

EngineHealthEntry *engine_health_get(const char *id);
const char *engine_health_state_name(BreakerState state);
void engine_health_persist(void);

#endif
