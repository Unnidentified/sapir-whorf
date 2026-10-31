#ifndef ANSWERBOX_H
#define ANSWERBOX_H

#include <stddef.h>

/* G4: the answer box. A Google-style direct answer at the top of the page,
   resolved from an explicit ordered table rather than from whichever infobox
   thread happens to finish first.

   The order is the policy: local answerers first (arithmetic, units, colour),
   then network ones (dictionary, currency, Wikipedia). A local answer is
   instant, deterministic and costs nothing, so it must never lose to a network
   call that might be slow, wrong, or unavailable. */

typedef struct {
  char answer[512];   /* the one-line answer */
  char detail[512];   /* optional second line: working, source, caveat */
  char source[64];    /* which answerer produced it */
  int local;          /* 1 = answered without the network */
} AnswerBoxResult;

typedef struct {
  const char *id;
  int local;
  /* 1 when this answerer wants the query at all */
  int (*matches)(const char *query);
  /* 1 and out filled when it can answer; 0 to fall through to the next */
  int (*answer)(const char *query, AnswerBoxResult *out);
} Answerer;

/* Resolve `query`. Tries every local answerer before any network one.
   Returns 1 when something answered, 0 otherwise. */
int answer_box_resolve(const char *query, AnswerBoxResult *out);

/* Local answerers only - instant, no network, safe to call on the request
   path. This is what the page calls: a local answer is shown immediately, and
   if there is none the existing infobox card carries the network answer. */
int answer_box_resolve_local(const char *query, AnswerBoxResult *out);

/* the table itself, exposed so the tests can assert its order */
const Answerer *answer_box_answerers(size_t *count);

#endif
