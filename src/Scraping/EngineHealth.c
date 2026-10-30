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
static int cooldown_seconds = 900;
static char health_file[768];
static int records_since_persist = 0;

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
      if (sscanf(line, "%31s %d %d %ld %lu %lu %lu %lu %lf %d", id, &state,
                 &consecutive, &open_until, &req, &ok, &blocked, &errors,
                 &latency, &trips) < 4)
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
  pthread_mutex_lock(&health_mutex);
  EngineHealthEntry *entry = find_or_create(id);
  if (entry) {
    entry->total_requests++;
    entry->total_latency_ms += latency_ms;
    if (is_failure_status(status, http_status)) {
      entry->total_errors++;
      if (status == SCRAPE_STATUS_BLOCKED)
        entry->total_blocked++;
      entry->consecutive_failures++;
      if (entry->state == HEALTH_HALF_OPEN ||
          entry->consecutive_failures >= failure_threshold) {
        entry->state = HEALTH_OPEN;
        entry->trips++;
        entry->open_until =
            time(NULL) + backoff_seconds(entry->trips, retry_after_secs);
      }
    } else if (status == SCRAPE_STATUS_OK) {
      entry->total_ok++;
      entry->consecutive_failures = 0;
      entry->trips = 0;
      entry->state = HEALTH_CLOSED;
      entry->open_until = 0;
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

void engine_health_persist(void) {
  if (!health_file[0])
    return;

  pthread_mutex_lock(&health_mutex);
  FILE *file = fopen(health_file, "w");
  if (file) {
    for (int i = 0; i < entry_count; i++) {
      fprintf(file, "%s\t%d\t%d\t%ld\t%lu\t%lu\t%lu\t%lu\t%.1f\t%d\n",
              entries[i].id, entries[i].state,
              entries[i].consecutive_failures, (long)entries[i].open_until,
              entries[i].total_requests, entries[i].total_ok,
              entries[i].total_blocked, entries[i].total_errors,
              entries[i].total_latency_ms, entries[i].trips);
    }
    fclose(file);
  }
  pthread_mutex_unlock(&health_mutex);
}
