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
  /* H4: how sure the answerer is. Only a confident local answer is allowed to
     skip the engine fan-out - a shaky one is shown but the web is still asked. */
  double confidence;  /* 0..1 */
} AnswerBoxResult;

/* J4.0: the interactive kinds. A widget is DATA, not markup - the template in
   templates/widgets/<kind>.html decides how it looks, so a card can be
   restyled or themed without touching C, and every colour comes from a token.
   The text answer (AnswerBoxResult) stays as it is: one line for the strip
   above the results, and the widget below it when the kind has one. */
typedef struct {
  char kind[32];             /* "calculator"; also the template name */
  char title[64];            /* "Calculation" */
  char expression[256];      /* what was evaluated, as the user typed it */
  char result[64];           /* the headline value */
  char note[96];             /* "Error", "Infinity", or a short caveat */
  int ok;                    /* 1 when result is a number */
} WidgetData;

typedef struct {
  const char *id;
  int local;
  /* 1 when this answerer wants the query at all */
  int (*matches)(const char *query);
  /* 1 and out filled when it can answer; 0 to fall through to the next */
  int (*answer)(const char *query, AnswerBoxResult *out);
  /* J4.0: the widget template for this kind, or NULL when the answer is only
     ever a line of text. One answerer, one kind, one template. */
  const char *widget;
} Answerer;

/* Resolve `query`. Tries every local answerer before any network one.
   Returns 1 when something answered, 0 otherwise. */
int answer_box_resolve(const char *query, AnswerBoxResult *out);

/* Local answerers only - instant, no network, safe to call on the request
   path. This is what the page calls: a local answer is shown immediately, and
   if there is none the existing infobox card carries the network answer. */
int answer_box_resolve_local(const char *query, AnswerBoxResult *out);

/* H4: locale-aware form. Comma-decimal locales ("1.234,56", "es") are
   normalised before the answerers see the query. */
int answer_box_resolve_local_lang(const char *query, const char *lang,
                                  AnswerBoxResult *out);

/* J4.0: resolve the interactive widget for `query`, if this kind has one.
   Returns 1 and fills `out` when a widget applies. `lang` selects the number
   formatting, and `append` (may be NULL) is one keypad character to add to the
   expression before evaluating, which is how the keypad works without
   JavaScript. */
int answer_box_widget(const char *query, const char *lang, const char *append,
                      WidgetData *out);

/* The same, when the page is explicitly in this widget's mode (w=<kind> in the
   URL, which is what the keypad form carries). Without it a keypad could not
   exist: "5" and "53" are not expressions, so the card would vanish mid-entry.
   Returns 1 whenever `kind` names a widget, with `out->ok` saying whether the
   expression evaluated. */
int answer_box_widget_forced(const char *kind, const char *query,
                             const char *lang, WidgetData *out);

/* the widget template for an answerer id, or NULL: so the page asks the table
   instead of keeping a second list of kind names */
const char *answer_box_widget_for(const char *answerer_id);

/* the table itself, exposed so the tests can assert its order */
const Answerer *answer_box_answerers(size_t *count);

/* H4: the network answerers in priority order, by id. The page uses this to
   promote an already-fetched result into the card, so the priority is defined
   once, in the table. */
const char *const *answer_box_network_order(int *count);

#endif
