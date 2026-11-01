#ifndef TRANSLATE_H
#define TRANSLATE_H

#include "AnswerBox.h"
#include <stddef.h>

/* J4b: split a translate request. Returns 1 and fills text/target when the
   query starts with the verb; the verb is required because "<x> in spanish"
   is a web query far more often than a translation. */
int translate_parse(const char *query, const char *default_target, char *text,
                    size_t text_size, char *target, size_t target_size);

/* K10: the request as the two-pane card needs it. `from` is "" when the source
   should be detected; `text` may be empty, which means "pre-set the menus and
   show an empty box" (bare "translate", or "english to spanish"). */
typedef struct {
  char text[900];
  char from[8];
  char to[8];
} TranslateRequest;

int translate_parse_request(const char *query, const char *default_target,
                            TranslateRequest *out);

/* K10: a two-letter code we know, or NULL. Callers validate `from`/`to` from
   the query string against this, never against free text. */
const char *translate_language_code(const char *word);

/* K10: the card's own form sends the text and both menus explicitly. `from`
   may be NULL or "" for detection. */
int translate_widget_explicit(const char *text, const char *from, const char *to,
                              WidgetData *out, const char *lang);

/* 1 when a card applies; out->ok says whether it actually translated */
int translate_widget(const char *text, const char *target, const char *source,
                     WidgetData *out, const char *lang);

#endif
