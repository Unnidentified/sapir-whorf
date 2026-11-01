#include "AnswerBox.h"
#include <math.h>
#include "AnswerLocal.h"
#include "Calculator.h"
#include "Translate.h"
#include "Places.h"
#include "Stocks.h"
#include "ColourCode.h"
#include "CurrencyConversion.h"
#include "Dictionary.h"
#include "UnitConversion.h"
#include "Wikipedia.h"
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ---- local answerers ------------------------------------------------- */

static int any_query(const char *query) {
  (void)query;
  return 1;
}

/* H4: the arithmetic answerer must only fire on something that is actually an
   expression. "covid-19" passed the operator test (a '-' with a digit after
   it) and evaluated to 0, which is a confidently wrong answer - worse than no
   answer. Only digits, spaces, a decimal point and the operators may appear. */
/* English writes 1,234.56. A comma is a thousands separator only when it is
   followed by exactly three digits and then a non-digit; "2,5" is left alone so
   it fails to evaluate rather than silently becoming 25. */
static void strip_thousands_commas(const char *in, char *out, size_t size) {
  size_t n = 0;
  for (const char *p = in; *p && n + 1 < size; p++) {
    if (*p == ',') {
      int digits = 0;
      const char *q = p + 1;
      while (*q >= '0' && *q <= '9') {
        digits++;
        q++;
      }
      if (digits == 3 && !(*q >= '0' && *q <= '9')) {
        if (*q == '\0' || *q == ' ' || *q == '+' || *q == '-' || *q == '*' ||
            *q == '/' || *q == '^' || *q == ')' || *q == '.')
          continue; /* drop the comma */
      }
    }
    out[n++] = *p;
  }
  out[n] = '\0';
}

static int looks_like_expression(const char *query) {
  if (!query || !query[0])
    return 0;
  int digits = 0;
  for (const char *p = query; *p; p++) {
    if (*p >= '0' && *p <= '9')
      digits = 1;
    else if (*p == ' ' || *p == '.' || *p == '+' || *p == '-' || *p == '*' ||
             *p == '/' || *p == '^' || *p == '(' || *p == ')' || *p == ',')
      continue;
    else
      return 0;
  }
  return digits;
}

static int answer_arithmetic(const char *query, AnswerBoxResult *out) {
  if (!looks_like_expression(query))
    return 0;
  if (!is_calculator_query(query))
    return 0;
  /* the infobox extract is HTML for the card; the answer box wants the number.
     A confidently wrong answer is worse than none, so this is the same
     evaluator the card uses, just formatted. */
  char expression[512];
  strip_thousands_commas(query, expression, sizeof(expression));
  double value = evaluate(expression);
  if (value != value) /* NaN: the parse failed, so there is no answer */
    return 0;
  if (isinf(value)) {
    /* J4a: Google prints Infinity for 1/0; "inf" is not a word */
    snprintf(out->answer, sizeof(out->answer), "%s",
             value < 0 ? "-Infinity" : "Infinity");
    snprintf(out->detail, sizeof(out->detail), "%s", expression);
    out->confidence = 1.0;
    return 1;
  }
  format_number(value, out->answer, sizeof(out->answer));
  snprintf(out->detail, sizeof(out->detail), "%s", expression);
  out->confidence = 1.0; /* an evaluated expression is not a guess */
  return 1;
}

static int answer_units(const char *query, AnswerBoxResult *out) {
  if (!is_unit_conv_query(query))
    return 0;
  if (!unit_conv_answer(query, out->answer, sizeof(out->answer)))
    return 0;
  out->confidence = 1.0; /* a parsed conversion is not a guess */
  return 1;
}

static int answer_colour(const char *query, AnswerBoxResult *out) {
  if (!is_colour_code_query(query))
    return 0;
  /* plain hex is the answer; the card carries the full breakdown */
  const char *p = query;
  while (*p == ' ')
    p++;
  if (strncasecmp(p, "rgb(", 4) == 0 || strncasecmp(p, "rgba(", 5) == 0) {
    int r = -1, g = -1, b = -1;
    const char *open = strchr(p, '(');
    if (!open ||
        (sscanf(open + 1, "%d ,%d ,%d", &r, &g, &b) != 3 &&
         sscanf(open + 1, "%d, %d, %d", &r, &g, &b) != 3))
      return 0;
    snprintf(out->answer, sizeof(out->answer), "#%02X%02X%02X", r, g, b);
    snprintf(out->detail, sizeof(out->detail), "RGB(%d, %d, %d)", r, g, b);
    out->confidence = 1.0;
    return 1;
  }
  char hex[16];
  size_t n = 0;
  p++;
  while (*p == ' ')
    p++;
  while ((isxdigit((unsigned char)*p)) && n < sizeof(hex) - 1)
    hex[n++] = *p++;
  hex[n] = '\0';
  snprintf(out->answer, sizeof(out->answer), "#%s", hex);
  out->confidence = 1.0;
  return 1;
}

/* ---- network answerers ----------------------------------------------- */

static int answer_dictionary(const char *query, AnswerBoxResult *out) {
  if (!is_dictionary_query(query))
    return 0;
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", query);
  InfoBox box = fetch_dictionary_data(buf);
  if (!box.extract || !box.extract[0]) {
    free_infobox(&box);
    return 0;
  }
  snprintf(out->answer, sizeof(out->answer), "%s",
           box.title ? box.title : "Definition");
  snprintf(out->detail, sizeof(out->detail), "%s", box.extract);
  snprintf(out->source, sizeof(out->source), "dictionary");
  out->confidence = 0.7;
  free_infobox(&box);
  return 1;
}

static int answer_currency(const char *query, AnswerBoxResult *out) {
  if (!is_currency_query(query))
    return 0;
  InfoBox box = fetch_currency_data(query);
  if (!box.title && !box.extract) {
    free_infobox(&box);
    return 0;
  }
  snprintf(out->answer, sizeof(out->answer), "%s",
           box.extract ? box.extract : (box.title ? box.title : ""));
  if (!out->answer[0]) {
    free_infobox(&box);
    return 0;
  }
  snprintf(out->detail, sizeof(out->detail), "%s", box.title ? box.title : "");
  snprintf(out->source, sizeof(out->source), "currency");
  out->confidence = 0.8;
  free_infobox(&box);
  return 1;
}

static int answer_wikipedia(const char *query, AnswerBoxResult *out) {
  char *built = construct_wiki_url(query);
  if (!built)
    return 0;
  InfoBox box = fetch_wiki_data(built);
  free(built);
  if (!box.extract || !box.extract[0]) {
    free_infobox(&box);
    return 0;
  }
  snprintf(out->answer, sizeof(out->answer), "%s",
           box.title ? box.title : "Wikipedia");
  snprintf(out->detail, sizeof(out->detail), "%s", box.extract);
  snprintf(out->source, sizeof(out->source), "wikipedia");
  out->confidence = 0.6;
  free_infobox(&box);
  return 1;
}

/* ---- the table ------------------------------------------------------- */

static const Answerer answerers[] = {
    /* local first: instant, deterministic, no network. Order among these is
       intentional - a bare expression is arithmetic, not a unit conversion. */
    /* H4: the local kinds that need neither network nor a new dependency.
       time and dates come before arithmetic because "10 days until
       2025-12-25" also looks like an expression to the matcher. */
    {"time", 1, NULL, answer_time_zone, NULL},
    {"days", 1, NULL, answer_days_between, NULL},
    {"random", 1, NULL, answer_random, NULL},
    /* J4.0: arithmetic is the first kind with a card. Units and colour keep
       the text answer for now; each gets a widget by adding a template and
       naming it here, not by adding a second table. */
    {"arithmetic", 1, is_calculator_query, answer_arithmetic, "calculator"},
    {"units", 1, is_unit_conv_query, answer_units, NULL},
    {"colour", 1, is_colour_code_query, answer_colour, NULL},
    /* then the network, cheapest first */
    {"currency", 0, is_currency_query, answer_currency, NULL},
    {"dictionary", 0, is_dictionary_query, answer_dictionary, NULL},
    {"wikipedia", 0, any_query, answer_wikipedia, NULL},
};

const Answerer *answer_box_answerers(size_t *count) {
  if (count)
    *count = sizeof(answerers) / sizeof(answerers[0]);
  return answerers;
}

int answer_box_resolve_local(const char *query, AnswerBoxResult *out) {
  return answer_box_resolve_local_lang(query, NULL, out);
}

int answer_box_resolve_local_lang(const char *query, const char *lang,
                                  AnswerBoxResult *out) {
  if (!query || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  /* H4: normalise locale numbers once, here, so every answerer sees the same
     canonical text. */
  char canonical[512];
  const char *q = query;
  if (lang && answer_lang_comma_decimal(lang) &&
      answer_normalise_locale_numbers(query, lang, canonical,
                                      sizeof(canonical)))
    q = canonical;
  size_t count = 0;
  const Answerer *table = answer_box_answerers(&count);
  for (size_t i = 0; i < count; i++) {
    const Answerer *a = &table[i];
    if (!a->local)
      continue;
    if (a->matches && !a->matches(q))
      continue;
    AnswerBoxResult candidate;
    memset(&candidate, 0, sizeof(candidate));
    if (!a->answer(q, &candidate) || !candidate.answer[0])
      continue;
    *out = candidate;
    out->local = 1;
    if (!out->source[0])
      snprintf(out->source, sizeof(out->source), "%s", a->id);
    return 1;
  }
  return 0;
}

const char *const *answer_box_network_order(int *count) {
  static const char *order[8];
  static int built = 0;
  static int n = 0;
  if (!built) {
    size_t total = 0;
    const Answerer *table = answer_box_answerers(&total);
    for (size_t i = 0; i < total && n < 8; i++)
      if (!table[i].local)
        order[n++] = table[i].id;
    built = 1;
  }
  if (count)
    *count = n;
  return order;
}

int answer_box_resolve(const char *query, AnswerBoxResult *out) {
  if (!query || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  size_t count = 0;
  const Answerer *table = answer_box_answerers(&count);

  /* two passes so the ordering is a guarantee, not a coincidence of table
     layout: no local answerer can ever be beaten by a network one */
  for (int pass = 1; pass >= 0; pass--) {
    for (size_t i = 0; i < count; i++) {
      const Answerer *a = &table[i];
      if (a->local != pass)
        continue;
      if (a->matches && !a->matches(query))
        continue;
      AnswerBoxResult candidate;
      memset(&candidate, 0, sizeof(candidate));
      if (!a->answer(query, &candidate))
        continue;
      if (!candidate.answer[0])
        continue;
      *out = candidate;
      out->local = a->local;
      if (!out->source[0])
        snprintf(out->source, sizeof(out->source), "%s", a->id);
      return 1;
    }
  }
  return 0;
}

/* ---- J4.0: the widget layer ------------------------------------------ */

/* The calculator card. This is deliberately a thin mapper: it evaluates with
   the same engine the answer box uses and hands the pieces to the template.
   Nothing here builds HTML, which is what made the old cards impossible to
   theme (see J2). */
int answer_box_widget(const char *query, const char *lang, const char *append,
                      WidgetData *out) {
  char work[320];
  char expression[256];
  double value;

  if (!query || !out)
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(work, sizeof(work), "%s", query);
  if (append && append[0]) {
    size_t used = strlen(work);
    /* one keypad character per request: that is the whole no-JS protocol */
    if (used + strlen(append) + 1 < sizeof(work))
      snprintf(work + used, sizeof(work) - used, "%s", append);
  }

  /* J4b: an explicit translate request. Checked before the calculator so
     "translate 2+2 to spanish" asks the translator, not the evaluator. */
  {
    char text[256];
    char target[8];
    if (translate_parse(work, lang, text, sizeof(text), target,
                        sizeof(target))) {
      translate_widget(text, target, NULL, out, lang);
      return 1;
    }
  }

  /* J4c/J4d: place and price requests, also before the calculator */
  {
    char place[256];
    if (places_parse(work, place, sizeof(place))) {
      places_widget(place, out);
      return 1;
    }
  }
  {
    char ticker[16];
    if (stocks_parse(work, ticker, sizeof(ticker))) {
      /* K11: 0 means "no card", and then the query is a normal search */
      return stocks_widget(ticker,
                           stocks_symbol_was_explicit(work, ticker), out);
    }
  }

  if (!is_calculator_query(work))
    return 0;
  return answer_box_widget_forced("calculator", work, lang, out);
}

int answer_box_widget_forced(const char *kind, const char *query,
                             const char *lang, WidgetData *out) {
  char expression[256];
  double value;
  if (!out || !kind)
    return 0;
  memset(out, 0, sizeof(*out));
  /* J4b: w=translate carries the text to translate in q, and the target is
     either "to <language>" inside it or the reader's own language. */
  if (strcmp(kind, "translate") == 0) {
    char text[256];
    char target[8];
    if (translate_parse(query, lang, text, sizeof(text), target,
                        sizeof(target)))
      return translate_widget(text, target, NULL, out, lang);
    return 0;
  }
  if (strcmp(kind, "place") == 0) {
    char place[256];
    if (places_parse(query, place, sizeof(place)))
      return places_widget(place, out);
    return 0;
  }
  if (strcmp(kind, "stock") == 0) {
    char ticker[16];
    if (stocks_parse(query, ticker, sizeof(ticker)))
      return stocks_widget(ticker,
                           stocks_symbol_was_explicit(query, ticker), out);
    return 0;
  }
  if (strcmp(kind, "calculator") != 0)
    return 0;
  if (!query)
    query = "";
  answer_normalise_locale_numbers(query, lang && lang[0] ? lang : "en",
                                  expression, sizeof(expression));
  snprintf(out->kind, sizeof(out->kind), "calculator");
  snprintf(out->title, sizeof(out->title), "Calculation");
  snprintf(out->expression, sizeof(out->expression), "%s", expression);
  if (!expression[0])
    return 1;

  /* Mid-entry is not an error. Typing "5+" and seeing "Error" is what a
     calculator looks like when it is broken, so an expression that ends on an
     operator (or on the "=" the keypad just sent) simply has no value yet. */
  {
    size_t len = strlen(expression);
    while (len > 0 && expression[len - 1] == ' ')
      len--;
    if (len > 0 && expression[len - 1] == '=') {
      expression[len - 1] = '\0';
      len--;
      while (len > 0 && expression[len - 1] == ' ')
        len--;
    }
    snprintf(out->expression, sizeof(out->expression), "%s", expression);
    if (len == 0)
      return 1;
    switch (expression[len - 1]) {
    case '+': case '-': case '*': case '/': case '^': case '(':
    case '\xc3': /* the first byte of ÷, × and −: also incomplete */
      return 1;
    default:
      break;
    }
  }

  value = evaluate(expression);
  if (value != value) {
    snprintf(out->note, sizeof(out->note), "Error");
    return 1;
  }
  if (value == (double)INFINITY || value == -(double)INFINITY) {
    snprintf(out->result, sizeof(out->result), "%s",
             value < 0 ? "-Infinity" : "Infinity");
    out->ok = 1;
    return 1;
  }
  format_number(value, out->result, sizeof(out->result));
  out->ok = 1;
  return 1;
}

/* the widget name for an answerer id, so the page can ask without a second
   list of kind names */
const char *answer_box_widget_for(const char *answerer_id) {
  size_t count = 0;
  const Answerer *table = answer_box_answerers(&count);
  for (size_t i = 0; i < count; i++)
    if (table[i].id && answerer_id && strcmp(table[i].id, answerer_id) == 0)
      return table[i].widget;
  return NULL;
}
