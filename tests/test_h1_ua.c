/* H1: the UA ladder and the profile that survives a restart. A challenge page
   is retried once with the next profile; the ladder order is the measured one
   (Brave served Safari and Firefox, refused every Chrome UA). */
#include "../src/Scraping/EngineHealth.h"
#include "../src/Scraping/Scraping.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static void expect_str(const char *what, const char *got, const char *want) {
  if (want == NULL) {
    if (got != NULL) {
      printf("FAIL: %s: got %s, want NULL\n", what, got);
      failures++;
    }
    return;
  }
  if (!got || strcmp(got, want) != 0) {
    printf("FAIL: %s: got %s, want %s\n", what, got ? got : "(null)", want);
    failures++;
  }
}

int main(void) {
  /* ladder order, from what was measured */
  expect_str("safari->firefox", get_engine_user_agent_ladder_next("safari"),
             "firefox");
  expect_str("firefox->chrome", get_engine_user_agent_ladder_next("firefox"),
             "chrome");
  expect_str("chrome->NULL", get_engine_user_agent_ladder_next("chrome"), NULL);
  /* an unknown profile starts at the top rather than looping forever */
  expect_str("unknown->safari", get_engine_user_agent_ladder_next("nonsense"),
             "safari");
  expect_str("NULL->safari", get_engine_user_agent_ladder_next(NULL),
             "safari");

  /* the profile survives a persist/load cycle, so the ladder resumes where it
     last succeeded instead of starting from a profile already refused */
  /* hermetic: drop anything a previous run persisted */
  remove("/tmp/h1-ua-test/engine_health.tsv");
  engine_health_init("/tmp/h1-ua-test");
  expect_str("no stored profile", engine_health_ua_profile("brave"), NULL);
  engine_health_set_ua_profile("brave", "firefox");
  expect_str("stored immediately", engine_health_ua_profile("brave"),
             "firefox");
  engine_health_persist();
  engine_health_init("/tmp/h1-ua-test");
  expect_str("survives restart", engine_health_ua_profile("brave"), "firefox");

  if (failures == 0)
    printf("H1 ua ladder: PASS\n");
  return failures ? 1 : 0;
}
