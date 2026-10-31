#ifndef ANSWERLOCAL_H
#define ANSWERLOCAL_H

#include "AnswerBox.h"

/* H4: local answer kinds that need no network and no new dependency.
   Each returns 1 and fills `out` when it can answer. */

int answer_time_zone(const char *query, AnswerBoxResult *out);
int answer_days_between(const char *query, AnswerBoxResult *out);
int answer_random(const char *query, AnswerBoxResult *out);

/* test hook: the city lookup, so a test can assert the table resolves */
int answer_city_zone(const char *city, char *out, size_t size);

#endif
