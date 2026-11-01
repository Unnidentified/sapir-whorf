#ifndef ENGINE_HEALTH_H
#define ENGINE_HEALTH_H

#include <time.h>

typedef enum {
  HEALTH_CLOSED = 0,
  HEALTH_OPEN,
  HEALTH_HALF_OPEN
} BreakerState;

/* G2: why an engine is paused. A 429 and a captcha are not the same event and
   must not get the same sentence on the page or the same cooldown. */
typedef enum {
  SUSPEND_NONE = 0,
  SUSPEND_RATE_LIMIT, /* 429 / 202 / 503 with Retry-After: honour the server */
  SUSPEND_CHALLENGE,  /* captcha or interstitial: it will not clear on retry */
  SUSPEND_PARSE,      /* answered, but not in a shape we understand        */
  SUSPEND_TRANSPORT   /* we could not reach it at all                      */
} SuspendReason;

typedef struct {
  char id[32];
  BreakerState state;
  SuspendReason suspend_reason;
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
  /* H2: answers that arrived, parsed, and were junk - >=90% of what it
     returned fell below the relevance floor. Distinct from EMPTY (nothing
     found) and from a failure (nothing arrived). */
  unsigned long total_junk;
  int pace_override;        /* 0 = use the global rate                  */
  /* H1: when did this engine last succeed, and when did it last fail? A
     persisted "0/2" cannot be tied to a build or to a moment without these. */
  time_t last_ok;
  time_t last_error;
  /* the UA profile that last worked, so a restart does not go back to a
     profile the engine has already refused */
  char ua_profile[16];
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

/* G2: the reason this engine is paused and how long is left, both for the
   page copy and for the /settings panel. Returns SUSPEND_NONE when open. */
SuspendReason engine_health_suspension(const char *id, int *seconds_left);

/* K3: the absolute instant the engine may be contacted again, 0 when it may be
   contacted now. This is the same field the dispatcher tests before it sends
   anything, so a page that counts down from it cannot disagree with the gate.
   The breaker and an upstream Retry-After share it: the later of the two
   wins, because either one can hold the engine. */
time_t engine_health_open_until(const char *id);
const char *engine_health_suspend_name(SuspendReason reason);

/* G2: per-engine outbound token bucket. 0 when the engine has no allowance
   right now, so the caller can skip it instead of piling on. */
int engine_health_take_token(const char *id);
void engine_health_set_pacing(int requests_per_minute, int burst);
/* Per-engine override of the global rate: an engine that borrows someone
   else's public endpoint (google_cse_element) gets a smaller one. */
void engine_health_set_engine_pace(const char *id, int requests_per_minute);

/* H1: remember which UA profile this engine answered under, so the ladder
   starts where it last succeeded. Returns the stored profile or NULL. */
void engine_health_set_ua_profile(const char *id, const char *profile);
const char *engine_health_ua_profile(const char *id);

/* H2: the routing weight adjusted by what this engine's results have been
   worth. A2 recorded quality but nothing read it, so "healthy but junk" cost
   nothing. 0.5x at quality 0, 1.0x at quality 1, and no penalty until there is
   enough evidence to deserve one. */
double engine_health_effective_weight(const char *id, double base_weight);

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

/* H2: a JUNK answer - 200, results present, >=90% below the relevance floor. */
void engine_health_record_junk(const char *id);

#endif
