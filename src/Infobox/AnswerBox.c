#include "AnswerBox.h"
#include "AnswerLocal.h"
#include "Calculator.h"
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
  if (value != value) /* NaN */
    return 0;
  if (value == (long long)value && value < 1e15 && value > -1e15)
    snprintf(out->answer, sizeof(out->answer), "%lld", (long long)value);
  else
    snprintf(out->answer, sizeof(out->answer), "%.10g", value);
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
    {"time", 1, NULL, answer_time_zone},
    {"days", 1, NULL, answer_days_between},
    {"random", 1, NULL, answer_random},
    {"arithmetic", 1, is_calculator_query, answer_arithmetic},
    {"units", 1, is_unit_conv_query, answer_units},
    {"colour", 1, is_colour_code_query, answer_colour},
    /* then the network, cheapest first */
    {"currency", 0, is_currency_query, answer_currency},
    {"dictionary", 0, is_dictionary_query, answer_dictionary},
    {"wikipedia", 0, any_query, answer_wikipedia},
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
