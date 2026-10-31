/* G4: the answer box, table-driven. Every row is a query and the answerer that
   must handle it. The local-only rows run offline and are the ones that matter
   most: a local answer must never lose to a network call. */
#include "../src/Infobox/AnswerBox.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

typedef struct {
  const char *query;
  const char *want_source;
  const char *want_contains; /* substring of answer+detail, NULL = don't care */
  int want_local;            /* expected local flag */
} Case;

static int run_case(const Case *c) {
  AnswerBoxResult r;
  int got = answer_box_resolve(c->query, &r);
  if (!got) {
    printf("FAIL %-28s resolved nothing (wanted %s)\n", c->query,
           c->want_source);
    return 1;
  }
  if (strcmp(r.source, c->want_source) != 0) {
    printf("FAIL %-28s source=%s want=%s\n", c->query, r.source, c->want_source);
    return 1;
  }
  if (r.local != c->want_local) {
    printf("FAIL %-28s local=%d want=%d\n", c->query, r.local, c->want_local);
    return 1;
  }
  if (c->want_contains) {
    char both[1100];
    snprintf(both, sizeof both, "%s %s", r.answer, r.detail);
    if (!strstr(both, c->want_contains)) {
      printf("FAIL %-28s answer/detail lacks \"%s\" (got \"%s\")\n", c->query,
             c->want_contains, both);
      return 1;
    }
  }
  return 0;
}

int main(void) {
  /* table order: local answerers must all precede network ones */
  size_t n = 0;
  const Answerer *table = answer_box_answerers(&n);
  int seen_network = 0;
  for (size_t i = 0; i < n; i++) {
    if (!table[i].local)
      seen_network = 1;
    else if (seen_network) {
      printf("FAIL: local answerer \"%s\" ordered after a network one\n",
             table[i].id);
      failures++;
    }
  }

  static const Case cases[] = {
      /* arithmetic: local, no network */
      {"2+2", "arithmetic", "4", 1},
      {"12 * 12", "arithmetic", "144", 1},
      {"2^10", "arithmetic", "1024", 1},
      /* units: local */
      {"10 km to miles", "units", "6.21 miles", 1},
      {"100 f to c", "units", "37.78", 1},
      /* colour: local */
      {"#ff0000", "colour", "#ff0000", 1},
      {"rgb(0, 128, 255)", "colour", "#0080FF", 1},
      /* a query with no operator is not arithmetic */
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    failures += run_case(&cases[i]);

  /* a plain word must not be answered locally at all: the local-only path is
     what the page calls, and it must never reach the network */
  AnswerBoxResult r;
  if (answer_box_resolve_local("linux kernel", &r)) {
    printf("FAIL: plain query answered locally by %s\n", r.source);
    failures++;
  }
  /* the deep resolver may use the network, but a local query must not */
  if (answer_box_resolve("linux kernel", &r) && r.local) {
    printf("FAIL: deep resolve answered a plain query locally via %s\n",
           r.source);
    failures++;
  }
  /* the local path answers every local case without touching the network */
  AnswerBoxResult loc;
  if (!answer_box_resolve_local("2+2", &loc) || !loc.local) {
    printf("FAIL: local resolve did not answer 2+2\n");
    failures++;
  }
  if (!answer_box_resolve_local("10 km to miles", &loc) || !loc.local) {
    printf("FAIL: local resolve did not answer a unit query\n");
    failures++;
  }

  if (failures == 0)
    printf("G4 answer box: PASS\n");
  return failures ? 1 : 0;
}
