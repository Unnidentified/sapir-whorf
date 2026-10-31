#ifndef TRANSLATE_H
#define TRANSLATE_H

#include "AnswerBox.h"
#include <stddef.h>

/* J4b: split a translate request. Returns 1 and fills text/target when the
   query starts with the verb; the verb is required because "<x> in spanish"
   is a web query far more often than a translation. */
int translate_parse(const char *query, const char *default_target, char *text,
                    size_t text_size, char *target, size_t target_size);

/* 1 when a card applies; out->ok says whether it actually translated */
int translate_widget(const char *text, const char *target, const char *source,
                     WidgetData *out, const char *lang);

#endif
