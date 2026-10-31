#include "AnswerLocal.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- cities -> time zone ------------------------------------------------ */

#define MAX_ALIASES 8

typedef struct {
  char zone[48];
  char *alias[MAX_ALIASES];
  int alias_count;
} CityRow;

static CityRow *city_rows = NULL;
static int city_count = 0;
static pthread_once_t city_once = PTHREAD_ONCE_INIT;
static const char *CITY_FILE = "static/cities.tsv";

static void load_cities(void) {
  FILE *f = fopen(CITY_FILE, "r");
  if (!f)
    return;
  city_rows = calloc(256, sizeof(CityRow));
  if (!city_rows) {
    fclose(f);
    return;
  }
  char line[512];
  while (fgets(line, sizeof(line), f) && city_count < 256) {
    if (line[0] == '#')
      continue;
    char *nl = strpbrk(line, "\r\n");
    if (nl)
      *nl = '\0';
    char *save = NULL;
    char *zone = strtok_r(line, "\t", &save);
    if (!zone || !zone[0])
      continue;
    CityRow *row = &city_rows[city_count];
    snprintf(row->zone, sizeof(row->zone), "%s", zone);
    char *alias;
    while ((alias = strtok_r(NULL, "\t", &save)) && row->alias_count < MAX_ALIASES) {
      if (!alias[0])
        continue;
      row->alias[row->alias_count++] = strdup(alias);
    }
    if (row->alias_count > 0)
      city_count++;
  }
  fclose(f);
}

/* lowercase, collapse spaces, trim - so "New  York " still matches */
static void normalise(const char *in, char *out, size_t size) {
  size_t n = 0;
  int last_space = 1;
  for (const char *p = in; *p && n + 1 < size; p++) {
    unsigned char c = (unsigned char)*p;
    if (isspace(c)) {
      if (!last_space && n + 1 < size) {
        out[n++] = ' ';
        last_space = 1;
      }
      continue;
    }
    out[n++] = (char)tolower(c);
    last_space = 0;
  }
  while (n > 0 && out[n - 1] == ' ')
    n--;
  out[n] = '\0';
}

int answer_city_zone(const char *city, char *out, size_t size) {
  pthread_once(&city_once, load_cities);
  if (!city || !city[0] || !out || size == 0)
    return 0;
  char key[128];
  normalise(city, key, sizeof(key));
  if (!key[0])
    return 0;
  for (int i = 0; i < city_count; i++) {
    for (int a = 0; a < city_rows[i].alias_count; a++) {
      char alias[128];
      normalise(city_rows[i].alias[a], alias, sizeof(alias));
      if (strcmp(alias, key) == 0) {
        snprintf(out, size, "%s", city_rows[i].zone);
        return 1;
      }
    }
  }
  return 0;
}

/* ---- time in <city> ---------------------------------------------------- */

/* setenv("TZ") is process-global, so the conversion is serialised. It takes
   microseconds and the alternative is wrong answers for concurrent requests. */
static pthread_mutex_t tz_mutex = PTHREAD_MUTEX_INITIALIZER;

static int time_in_zone(const char *zone, struct tm *out_tm, int *offset_hours) {
  pthread_mutex_lock(&tz_mutex);
  char *old = getenv("TZ");
  char keep[128] = "";
  if (old)
    snprintf(keep, sizeof(keep), "%s", old);
  setenv("TZ", zone, 1);
  tzset();
  time_t now = time(NULL);
  int ok = localtime_r(&now, out_tm) != NULL;
  int offset = (int)(out_tm->tm_gmtoff / 3600);
  if (keep[0])
    setenv("TZ", keep, 1);
  else
    unsetenv("TZ");
  tzset();
  pthread_mutex_unlock(&tz_mutex);
  if (offset_hours)
    *offset_hours = offset;
  return ok;
}

static const char *TIME_MARKERS[] = {"what time is it in ", "time in ",
                                     "current time in ",   "hora en ",
                                     "que hora es en ",    "qué hora es en "};

int answer_time_zone(const char *query, AnswerBoxResult *out) {
  if (!query)
    return 0;
  char lower[256];
  snprintf(lower, sizeof(lower), "%s", query);
  for (char *p = lower; *p; p++)
    *p = (char)tolower((unsigned char)*p);

  const char *rest = NULL;
  for (size_t i = 0; i < sizeof(TIME_MARKERS) / sizeof(TIME_MARKERS[0]); i++) {
    const char *hit = strstr(lower, TIME_MARKERS[i]);
    if (hit) {
      rest = query + (hit - lower) + strlen(TIME_MARKERS[i]);
      break;
    }
  }
  if (!rest)
    return 0;
  while (*rest == ' ')
    rest++;
  if (!*rest)
    return 0;

  char zone[64];
  if (!answer_city_zone(rest, zone, sizeof(zone)))
    return 0;

  struct tm tm_now;
  int offset = 0;
  if (!time_in_zone(zone, &tm_now, &offset))
    return 0;

  static const char *DAYS[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                               "Thursday", "Friday", "Saturday"};
  static const char *MONTHS[] = {"January",   "February", "March",    "April",
                                 "May",       "June",     "July",     "August",
                                 "September", "October",  "November", "December"};
  snprintf(out->answer, sizeof(out->answer), "%02d:%02d", tm_now.tm_hour,
           tm_now.tm_min);
  snprintf(out->detail, sizeof(out->detail), "%s, %d %s %d — %s (UTC%+d)",
           DAYS[tm_now.tm_wday % 7], tm_now.tm_mday,
           MONTHS[tm_now.tm_mon % 12], tm_now.tm_year + 1900, zone, offset);
  out->confidence = 1.0;
  return 1;
}

/* ---- days between two dates -------------------------------------------- */

static int parse_date(const char *s, struct tm *out) {
  int y = 0, m = 0, d = 0;
  if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
    return 0;
  if (y < 1900 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31)
    return 0;
  memset(out, 0, sizeof(*out));
  out->tm_year = y - 1900;
  out->tm_mon = m - 1;
  out->tm_mday = d;
  return 1;
}

int answer_days_between(const char *query, AnswerBoxResult *out) {
  if (!query)
    return 0;
  const char *first = strstr(query, "between ");
  const char *and_pos = first ? strstr(first, " and ") : NULL;
  if (first && and_pos) {
    char a[32], b[32];
    snprintf(a, sizeof(a), "%.*s", (int)(and_pos - (first + 8)), first + 8);
    snprintf(b, sizeof(b), "%s", and_pos + 5);
    struct tm da, db;
    if (!parse_date(a, &da) || !parse_date(b, &db))
      return 0;
    time_t ta = timegm(&da), tb = timegm(&db);
    long days = (long)((tb - ta) / 86400);
    if (days < 0)
      days = -days;
    snprintf(out->answer, sizeof(out->answer), "%ld days", days);
    snprintf(out->detail, sizeof(out->detail), "between %s and %s", a, b);
    out->confidence = 1.0;
    return 1;
  }
  /* "how many days until 2025-12-25" / "days until ..." */
  const char *until = strstr(query, "until ");
  if (until) {
    struct tm target;
    if (!parse_date(until + 6, &target))
      return 0;
    struct tm today_tm;
    time_t now = time(NULL);
    gmtime_r(&now, &today_tm);
    today_tm.tm_hour = today_tm.tm_min = today_tm.tm_sec = 0;
    long days = (long)((timegm(&target) - timegm(&today_tm)) / 86400);
    snprintf(out->answer, sizeof(out->answer), "%ld days", days);
    snprintf(out->detail, sizeof(out->detail), "until %s", until + 6);
    out->confidence = 0.95; /* depends on today's date, which moves */
    return 1;
  }
  return 0;
}

/* ---- random / dice / coin / uuid --------------------------------------- */

/* rand_r is per-thread and this runs on request threads (C17). */
static __thread unsigned int rnd_state = 0;
static unsigned int next_rand(void) {
  if (rnd_state == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    rnd_state = (unsigned int)(ts.tv_nsec ^ (ts.tv_sec << 16)) | 1u;
  }
  return (unsigned int)rand_r(&rnd_state);
}

/* A short imperative ("flip a coin", "uuid") is a request; a longer query is a
   question *about* the thing ("uuid v4 format spec", "flip a coin
   probability"). Only answer the former. */
static int word_count(const char *s) {
  int n = 0, in_word = 0;
  for (const char *p = s; *p; p++) {
    if (isspace((unsigned char)*p))
      in_word = 0;
    else if (!in_word) {
      in_word = 1;
      n++;
    }
  }
  return n;
}

int answer_random(const char *query, AnswerBoxResult *out) {
  if (!query)
    return 0;
  char lower[128];
  snprintf(lower, sizeof(lower), "%s", query);
  for (char *p = lower; *p; p++)
    *p = (char)tolower((unsigned char)*p);

  /* "random number between 1 and 10" is a request however long it is; the
     guard is for coin/dice/uuid, where a longer query is a question about the
     thing rather than a request for one. */
  if (!strstr(lower, "random number") && word_count(lower) > 5)
    return 0;

  if (strstr(lower, "flip a coin") || strstr(lower, "lanzar una moneda")) {
    snprintf(out->answer, sizeof(out->answer), "%s",
             (next_rand() & 1u) ? "Heads" : "Tails");
    out->confidence = 1.0;
    return 1;
  }
  if (strstr(lower, "roll a d6") || strstr(lower, "roll a dice") ||
      strstr(lower, "roll a die") || strstr(lower, "tirar un dado")) {
    snprintf(out->answer, sizeof(out->answer), "%u", 1 + (next_rand() % 6u));
    out->confidence = 1.0;
    return 1;
  }
  if (strstr(lower, "roll a d20")) {
    snprintf(out->answer, sizeof(out->answer), "%u", 1 + (next_rand() % 20u));
    out->confidence = 1.0;
    return 1;
  }
  if (strstr(lower, "random number")) {
    long lo = 1, hi = 100;
    if (strstr(lower, "between")) {
      const char *p = strstr(lower, "between");
      if (p && sscanf(p, "between %ld and %ld", &lo, &hi) != 2) {
        if (p)
          sscanf(p, "between %ld-%ld", &lo, &hi);
      }
    }
    if (hi < lo) {
      long t = lo;
      lo = hi;
      hi = t;
    }
    if (hi - lo > 0)
      snprintf(out->answer, sizeof(out->answer), "%ld",
               lo + (long)(next_rand() % (unsigned int)(hi - lo + 1)));
    else
      snprintf(out->answer, sizeof(out->answer), "%ld", lo);
    out->confidence = 1.0;
    return 1;
  }
  if (word_count(lower) <= 3 &&
      (strstr(lower, "uuid") || strstr(lower, "guid"))) {
    unsigned int a = next_rand(), b = next_rand(), c = next_rand(),
                 d = next_rand();
    snprintf(out->answer, sizeof(out->answer),
             "%08x-%04x-4%03x-%04x-%08x%04x", a, (b >> 16) & 0xffff,
             b & 0x0fff, (c & 0x3fff) | 0x8000, d,
             (c >> 16) & 0xffff);
    out->confidence = 1.0;
    return 1;
  }
  return 0;
}
