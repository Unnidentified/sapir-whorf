#include "EngineHealth.h"
#include "Scraping.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAX_ENTRIES 32

static EngineHealthEntry entries[MAX_ENTRIES];
static int entry_count = 0;
static pthread_mutex_t health_mutex = PTHREAD_MUTEX_INITIALIZER;
static int failure_threshold = 3;
/* G2: outbound pacing, per engine. A burst of parallel queries is what trips
   an engine into a challenge in the first place, so the fix has to be on the
   way out, not just a cooldown afterwards. */
static int pace_per_minute = 40;
static int pace_burst = 6;
static double tokens[MAX_ENTRIES];
static double last_refill[MAX_ENTRIES];
static int cooldown_seconds = 900;
static char health_file[768];
static int records_since_persist = 0;

/* C8: set by the scraper when the whole batch failed the same way, i.e. when
   the evidence points at our network rather than at any one engine. */
static int batch_transport_outage = 0;

void engine_health_set_batch_outage(int is_outage) {
  pthread_mutex_lock(&health_mutex);
  batch_transport_outage = is_outage ? 1 : 0;
  pthread_mutex_unlock(&health_mutex);
}

/* G2: classify what happened before deciding how to react. The old code had one
   cooldown for every kind of failure, which meant a 429 with Retry-After: 30
   was punished for 15 minutes and a captcha was retried after 15 minutes as if
   it were a blip. */
static SuspendReason classify_failure(int status, long http_status,
                                      int retry_after) {
  if (status == SCRAPE_STATUS_BLOCKED)
    return SUSPEND_CHALLENGE;
  /* 429 and friends are the engine saying "not now" whether or not it bothered
     to say when. Retry-After only refines the duration. */
  if (http_status == 429 || http_status == 403 || http_status == 202 ||
      http_status == 503)
    return SUSPEND_RATE_LIMIT;
  if (status == SCRAPE_STATUS_FETCH_ERROR)
    return SUSPEND_TRANSPORT;
  if (status == SCRAPE_STATUS_PARSE_MISMATCH)
    return SUSPEND_PARSE;
  if (status == SCRAPE_STATUS_EMPTY && http_status >= 400)
    return SUSPEND_RATE_LIMIT;
  return SUSPEND_NONE;
}

/* First strike, by kind. Rate limits are the server telling us exactly when to
   come back, so honour it; a challenge will not clear by retrying harder, so
   stand down for longer; transport and parse get the benefit of the doubt and
   need repeated evidence. */
static int first_strike_seconds(SuspendReason reason, int retry_after) {
  switch (reason) {
  case SUSPEND_RATE_LIMIT: {
    int wait = retry_after > 0 ? retry_after : 60;
    if (wait < 30)
      wait = 30;
    if (wait > 3600)
      wait = 3600;
    return wait;
  }
  case SUSPEND_CHALLENGE:
    return 900;
  case SUSPEND_PARSE:
    return 300;
  case SUSPEND_TRANSPORT:
    return 120;
  case SUSPEND_NONE:
    break;
  }
  return 0;
}

/* how much evidence a kind needs before it pauses anything */
static int strike_threshold(SuspendReason reason) {
  switch (reason) {
  case SUSPEND_RATE_LIMIT:
  case SUSPEND_CHALLENGE:
    return 1; /* the server already said no; one is enough */
  case SUSPEND_TRANSPORT:
    return failure_threshold * 2; /* our network is the likelier cause */
  case SUSPEND_PARSE:
    return failure_threshold;
  case SUSPEND_NONE:
    break;
  }
  return failure_threshold;
}

static void engine_health_persist_locked(void);

static int is_failure_status(int status, long http_status) {
  if (status == SCRAPE_STATUS_FETCH_ERROR ||
      status == SCRAPE_STATUS_PARSE_MISMATCH ||
      status == SCRAPE_STATUS_BLOCKED)
    return 1;
  /* an "empty" answer behind a 202/429/4xx is a soft block, not a real miss */
  if (status == SCRAPE_STATUS_EMPTY &&
      (http_status == 202 || http_status == 429 || http_status >= 400))
    return 1;
  return 0;
}

static EngineHealthEntry *find_or_create(const char *id) {
  if (!id || !id[0])
    return NULL;
  for (int i = 0; i < entry_count; i++) {
    if (strcmp(entries[i].id, id) == 0)
      return &entries[i];
  }
  if (entry_count >= MAX_ENTRIES)
    return NULL;
  EngineHealthEntry *entry = &entries[entry_count++];
  memset(entry, 0, sizeof(*entry));
  snprintf(entry->id, sizeof(entry->id), "%s", id);
  entry->state = HEALTH_CLOSED;
  return entry;
}

void engine_health_set_pacing(int requests_per_minute, int burst) {
  pthread_mutex_lock(&health_mutex);
  if (requests_per_minute > 0)
    pace_per_minute = requests_per_minute;
  if (burst > 0)
    pace_burst = burst;
  pthread_mutex_unlock(&health_mutex);
}

/* Token bucket, one per engine, refilled at the configured rate. Returns 0
   when the allowance is spent so the caller can leave the engine out of this
   batch rather than pile on and earn a challenge. */
int engine_health_take_token(const char *id) {
  if (!id || !id[0])
    return 1;
  pthread_mutex_lock(&health_mutex);
  int slot = -1;
  for (int i = 0; i < entry_count; i++) {
    if (strcmp(entries[i].id, id) == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    if (entry_count >= MAX_ENTRIES) {
      pthread_mutex_unlock(&health_mutex);
      return 1;
    }
    slot = entry_count;
    find_or_create(id);
    tokens[slot] = pace_burst;
    last_refill[slot] = (double)time(NULL);
  }
  double now = (double)time(NULL);
  double elapsed = now - last_refill[slot];
  int rate = entries[slot].pace_override > 0 ? entries[slot].pace_override
                                             : pace_per_minute;
  int cap = rate < pace_burst ? rate : pace_burst;
  if (elapsed > 0) {
    tokens[slot] += elapsed * ((double)rate / 60.0);
    if (tokens[slot] > cap)
      tokens[slot] = cap;
    last_refill[slot] = now;
  }
  if (tokens[slot] > cap)
    tokens[slot] = cap;
  int allowed = tokens[slot] >= 1.0;
  if (allowed)
    tokens[slot] -= 1.0;
  pthread_mutex_unlock(&health_mutex);
  return allowed;
}

void engine_health_set_ua_profile(const char *id, const char *profile) {
  if (!id || !id[0] || !profile)
    return;
  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  if (entry && strcmp(entry->ua_profile, profile) != 0) {
    snprintf(entry->ua_profile, sizeof(entry->ua_profile), "%s", profile);
    engine_health_persist_locked();
  }
  pthread_mutex_unlock(&health_mutex);
}

const char *engine_health_ua_profile(const char *id) {
  static __thread char buf[16];
  buf[0] = '\0';
  if (!id)
    return NULL;
  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  if (entry && entry->ua_profile[0])
    snprintf(buf, sizeof(buf), "%s", entry->ua_profile);
  pthread_mutex_unlock(&health_mutex);
  return buf[0] ? buf : NULL;
}

void engine_health_set_engine_pace(const char *id, int requests_per_minute) {
  if (!id || !id[0] || requests_per_minute <= 0)
    return;
  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  if (entry)
    entry->pace_override = requests_per_minute;
  pthread_mutex_unlock(&health_mutex);
}

void engine_health_set_limits(int threshold, int cooldown) {
  if (threshold > 0)
    failure_threshold = threshold;
  if (cooldown > 0)
    cooldown_seconds = cooldown;
}

void engine_health_init(const char *cache_dir) {
  pthread_mutex_lock(&health_mutex);
  entry_count = 0;
  memset(entries, 0, sizeof(entries));
  snprintf(health_file, sizeof(health_file), "%s/engine_health.tsv",
           cache_dir ? cache_dir : ".");

  FILE *file = fopen(health_file, "r");
  if (file) {
    char line[512];
    while (fgets(line, sizeof(line), file)) {
      char id[32];
      int state = 0, consecutive = 0;
      long open_until = 0;
      unsigned long req = 0, ok = 0, blocked = 0, errors = 0;
      double latency = 0;
      int trips = 0;
      int reason = 0;
      long last_ok = 0, last_error = 0;
      char ua[16] = "";
      if (sscanf(line,
                 "%31s %d %d %ld %lu %lu %lu %lu %lf %d %d %ld %ld %15s", id,
                 &state, &consecutive, &open_until, &req, &ok, &blocked,
                 &errors, &latency, &trips, &reason, &last_ok, &last_error,
                 ua) < 4)
        continue;
      EngineHealthEntry *entry = find_or_create(id);
      if (!entry)
        continue;
      entry->state = (BreakerState)state;
      entry->consecutive_failures = consecutive;
      entry->open_until = (time_t)open_until;
      entry->total_requests = req;
      entry->total_ok = ok;
      entry->total_blocked = blocked;
      entry->total_errors = errors;
      entry->total_latency_ms = latency;
      entry->trips = trips;
      entry->suspend_reason = (SuspendReason)reason;
      entry->last_ok = (time_t)last_ok;
      entry->last_error = (time_t)last_error;
      if (ua[0] && strcmp(ua, "-") != 0)
        snprintf(entry->ua_profile, sizeof(entry->ua_profile), "%s", ua);
    }
    fclose(file);
  }

  /* a breaker open from a previous run can be retried once the cooldown passed */
  time_t now = time(NULL);
  for (int i = 0; i < entry_count; i++) {
    if (entries[i].state == HEALTH_OPEN && entries[i].open_until <= now)
      entries[i].state = HEALTH_HALF_OPEN;
  }
  pthread_mutex_unlock(&health_mutex);
}

EngineHealthEntry *engine_health_get(const char *id) {
  return find_or_create(id);
}

SuspendReason engine_health_suspension(const char *id, int *seconds_left) {
  if (seconds_left)
    *seconds_left = 0;
  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  SuspendReason reason = SUSPEND_NONE;
  int left = 0;
  if (entry && entry->state != HEALTH_CLOSED) {
    reason = entry->suspend_reason;
    if (entry->open_until > (time_t)time(NULL))
      left = (int)(entry->open_until - time(NULL));
  }
  pthread_mutex_unlock(&health_mutex);
  if (seconds_left)
    *seconds_left = left;
  return reason;
}

const char *engine_health_suspend_name(SuspendReason reason) {
  switch (reason) {
  case SUSPEND_RATE_LIMIT:
    return "rate-limited";
  case SUSPEND_CHALLENGE:
    return "challenged";
  case SUSPEND_PARSE:
    return "unparseable";
  case SUSPEND_TRANSPORT:
    return "unreachable";
  case SUSPEND_NONE:
    return "none";
  }
  return "none";
}

const char *engine_health_state_name(BreakerState state) {
  switch (state) {
  case HEALTH_CLOSED:
    return "closed";
  case HEALTH_OPEN:
    return "open";
  case HEALTH_HALF_OPEN:
    return "half-open";
  }
  return "unknown";
}

int engine_health_allow(const char *id) {
  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  int allow = 1;
  if (entry) {
    if (entry->state == HEALTH_OPEN) {
      if (entry->open_until > 0 && time(NULL) >= entry->open_until) {
        entry->state = HEALTH_HALF_OPEN;
        allow = 1;
      } else {
        allow = 0;
      }
    }
  }
  pthread_mutex_unlock(&health_mutex);
  return allow;
}

/* rand() is not thread-safe and this runs on request threads (C17) */
static __thread unsigned int jitter_state = 0;

static unsigned int backoff_seed(void) {
  if (jitter_state == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    jitter_state = (unsigned int)(ts.tv_nsec ^ (ts.tv_sec << 16)) | 1u;
  }
  return (unsigned int)rand_r(&jitter_state);
}

/* Repeated trips mean the source is still unhappy: wait longer each time,
   with jitter so parallel engines do not retry in lockstep. */
static int backoff_seconds(int trips, int retry_after) {
  int cooldown = cooldown_seconds > 0 ? cooldown_seconds : 900;
  int shift = trips > 1 ? trips - 1 : 0;
  if (shift > 4)
    shift = 4;
  long wait = (long)cooldown << shift;
  if (wait > 6 * 3600)
    wait = 6 * 3600;
  if (retry_after > wait)
    wait = retry_after;
  if (wait > 60) {
    int spread = (int)(wait / 5);
    wait += (backoff_seed() % (2 * spread + 1)) - spread;
  }
  return (int)wait;
}

void engine_health_record(const char *id, int status, long http_status,
                          double latency_ms, int retry_after_secs) {
  /* evaluation runs and local outages must not poison persisted breakers */
  if (getenv("OMNI_NOBREAKER"))
    return;

  pthread_mutex_lock(&health_mutex);

  /* a transport failure while the whole batch is failing transport-wise says
     nothing about this engine: skip it entirely */
  if (is_failure_status(status, http_status) && batch_transport_outage) {
    EngineHealthEntry *e = find_or_create(id);
    if (e) {
      e->total_requests++;
      e->total_latency_ms += latency_ms;
    }
    pthread_mutex_unlock(&health_mutex);
    return;
  }
  EngineHealthEntry *entry = find_or_create(id);
  if (entry) {
    entry->total_requests++;
    entry->total_latency_ms += latency_ms;
    if (is_failure_status(status, http_status)) {
      SuspendReason kind =
          classify_failure(status, http_status, retry_after_secs);
      entry->total_errors++;
      entry->last_error = time(NULL);
      if (status == SCRAPE_STATUS_BLOCKED)
        entry->total_blocked++;
      entry->consecutive_failures++;
      if (kind != SUSPEND_NONE && entry->suspend_reason == SUSPEND_NONE)
        entry->suspend_reason = kind;
      if (entry->state == HEALTH_HALF_OPEN ||
          entry->consecutive_failures >= strike_threshold(kind)) {
        if (entry->state != HEALTH_OPEN)
          entry->trips++;
        entry->state = HEALTH_OPEN;
        entry->suspend_reason = kind;
        int wait = entry->trips > 1
                       ? backoff_seconds(entry->trips, retry_after_secs)
                       : first_strike_seconds(kind, retry_after_secs);
        if (wait <= 0)
          wait = cooldown_seconds > 0 ? cooldown_seconds : 900;
        entry->open_until = time(NULL) + wait;
      }
    } else if (status == SCRAPE_STATUS_OK) {
      entry->total_ok++;
      entry->last_ok = time(NULL);
      entry->consecutive_failures = 0;
      entry->trips = 0;
      entry->state = HEALTH_CLOSED;
      entry->open_until = 0;
      entry->suspend_reason = SUSPEND_NONE;
    }
  }
  if (++records_since_persist >= 25) {
    records_since_persist = 0;
    pthread_mutex_unlock(&health_mutex);
    engine_health_persist();
    return;
  }
  pthread_mutex_unlock(&health_mutex);
}

/* Exponentially weighted, alpha 0.2: five bad queries move it ~70% of the way,
   so a temporary bad patch does not condemn an engine but a persistent one
   shows up within a handful of queries. */
void engine_health_record_quality(const char *id, int returned, int useful) {
  if (!id || returned <= 0)
    return;
  if (useful < 0)
    useful = 0;
  if (useful > returned)
    useful = returned;

  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  if (entry) {
    double sample = (double)useful / (double)returned;
    double alpha = 0.2;
    if (entry->quality_samples == 0)
      entry->quality_ewma = sample;
    else
      entry->quality_ewma = (1.0 - alpha) * entry->quality_ewma + alpha * sample;
    entry->quality_samples++;
  }
  pthread_mutex_unlock(&health_mutex);
}

void engine_health_record_cancelled(const char *id) {
  if (!id)
    return;
  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  if (entry)
    entry->total_cancelled++;
  pthread_mutex_unlock(&health_mutex);
}

static void engine_health_persist_locked(void) {
  if (!health_file[0])
    return;
  FILE *file = fopen(health_file, "w");
  if (file) {
    for (int i = 0; i < entry_count; i++) {
      fprintf(file,
              "%s\t%d\t%d\t%ld\t%lu\t%lu\t%lu\t%lu\t%.1f\t%d\t%d\t%ld"
              "\t%ld\t%s\n",
              entries[i].id, entries[i].state,
              entries[i].consecutive_failures, (long)entries[i].open_until,
              entries[i].total_requests, entries[i].total_ok,
              entries[i].total_blocked, entries[i].total_errors,
              entries[i].total_latency_ms, entries[i].trips,
              (int)entries[i].suspend_reason, (long)entries[i].last_ok,
              (long)entries[i].last_error,
              entries[i].ua_profile[0] ? entries[i].ua_profile : "-");
    }
    fclose(file);
  }
}

void engine_health_persist(void) {
  if (!health_file[0])
    return;
  pthread_mutex_lock(&health_mutex);
  engine_health_persist_locked();
  pthread_mutex_unlock(&health_mutex);
}
