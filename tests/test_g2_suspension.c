/* G2: the first-strike table, exercised directly. Each row is a claim the page
   copy and the cooldown both depend on. */
#include "../src/Scraping/EngineHealth.h"
#include "../src/Scraping/Scraping.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(const char *what, int got, int want) {
  if (got != want) {
    printf("FAIL: %s: got %d want %d\n", what, got, want);
    failures++;
  }
}

static void expect_paused(const char *id, int status, long http, int retry,
                          int strike, SuspendReason want_reason,
                          int want_min, int want_max) {
  char name[64];
  snprintf(name, sizeof name, "%s/strike%d", id, strike);
  engine_health_record(id, status, http, 10.0, retry);
  int left = 0;
  SuspendReason r = engine_health_suspension(id, &left);
  if (want_reason == SUSPEND_NONE) {
    check(name, r, SUSPEND_NONE);
    check(name, engine_health_allow(id), 1);
    return;
  }
  if (r != want_reason) {
    printf("FAIL: %s: reason %s want %s\n", name,
           engine_health_suspend_name(r),
           engine_health_suspend_name(want_reason));
    failures++;
  }
  if (left < want_min || left > want_max) {
    printf("FAIL: %s: %ds left, want %d..%d\n", name, left, want_min, want_max);
    failures++;
  }
  check(name, engine_health_allow(id), 0);
}

int main(void) {
  engine_health_init("/tmp/g2-suspension-test");
  engine_health_set_limits(3, 900);

  /* 429 with Retry-After: suspended on the FIRST strike, for what it asked */
  expect_paused("rl429", SCRAPE_STATUS_EMPTY, 429, 42, 1, SUSPEND_RATE_LIMIT,
                40, 44);
  /* 202 soft block with Retry-After is the same conversation */
  expect_paused("rl202", SCRAPE_STATUS_EMPTY, 202, 30, 1, SUSPEND_RATE_LIMIT,
                28, 32);
  /* 202 without Retry-After still stands down, for a default minute */
  expect_paused("rl202n", SCRAPE_STATUS_EMPTY, 202, 0, 1, SUSPEND_RATE_LIMIT,
                58, 62);
  /* a captcha is first-strike and longer than a rate limit */
  expect_paused("ch1", SCRAPE_STATUS_BLOCKED, 200, 0, 1, SUSPEND_CHALLENGE,
                898, 902);
  /* parse mismatch needs the configured three strikes */
  expect_paused("pm", SCRAPE_STATUS_PARSE_MISMATCH, 200, 0, 1, SUSPEND_NONE,
                0, 0);
  expect_paused("pm", SCRAPE_STATUS_PARSE_MISMATCH, 200, 0, 2, SUSPEND_NONE,
                0, 0);
  expect_paused("pm", SCRAPE_STATUS_PARSE_MISMATCH, 200, 0, 3, SUSPEND_PARSE,
                298, 302);
  /* transport is our network's usual suspect: needs six, double the threshold */
  for (int i = 1; i <= 5; i++) {
    engine_health_record("tx", SCRAPE_STATUS_FETCH_ERROR, 0, 10.0, 0);
    check("tx/before6", engine_health_allow("tx"), 1);
  }
  expect_paused("tx", SCRAPE_STATUS_FETCH_ERROR, 0, 0, 6, SUSPEND_TRANSPORT,
                118, 122);
  /* a success clears everything, including the reason */
  engine_health_record("ch1", SCRAPE_STATUS_OK, 200, 10.0, 0);
  int left = 0;
  check("ch1/recovered", engine_health_suspension("ch1", &left), SUSPEND_NONE);
  check("ch1/allowed", engine_health_allow("ch1"), 1);

  /* H2: quality demotes, but only once there is evidence. A2 recorded the
     number and nothing read it; this is the factor the router now uses. */
  engine_health_record_quality("q-new", 10, 0); /* one sample: not enough */
  if (engine_health_effective_weight("q-new", 1.0) != 1.0) {
    printf("FAIL: one low sample already demoted the engine\n");
    failures++;
  }
  for (int i = 0; i < 4; i++)
    engine_health_record_quality("q-new", 10, 0);
  double low = engine_health_effective_weight("q-new", 1.0);
  if (low < 0.49 || low > 0.51) {
    printf("FAIL: all-junk engine weight %.3f, want ~0.5\n", low);
    failures++;
  }
  engine_health_record_quality("q-new", 10, 10);
  engine_health_record_quality("q-new", 10, 10);
  engine_health_record_quality("q-new", 10, 10);
  double better = engine_health_effective_weight("q-new", 1.0);
  if (better <= low) {
    printf("FAIL: quality recovery did not raise the weight (%.3f)\n", better);
    failures++;
  }
  /* it demotes, it does not ban */
  if (engine_health_effective_weight("q-new", 1.0) <= 0.0) {
    printf("FAIL: junk engine reduced to zero weight\n");
    failures++;
  }

  if (failures == 0)
    printf("G2 suspension table: PASS\n");
  return failures ? 1 : 0;
}
