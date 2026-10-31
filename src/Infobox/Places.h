#ifndef PLACES_H
#define PLACES_H

#include "AnswerBox.h"
#include <stddef.h>

/* J4c: 1 when the query asks where something is, with `place` filled. */
int places_parse(const char *query, char *place, size_t place_size);

/* 1 when a card applies; out->ok says whether the lookup answered. */
int places_widget(const char *place, WidgetData *out);

#endif
