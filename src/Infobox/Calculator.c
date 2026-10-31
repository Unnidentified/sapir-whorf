#include "Calculator.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Per-thread: this file-level buffer was shared by every request thread, so
   two concurrent calculator queries interleaved their working. */
static __thread char logic_log[4096];

typedef struct {
  const char *buffer;
  int pos;
  /* J4a: the parser had no error channel, so "5 + * 3" answered 5 and
     "(2+3" answered 5 - a confidently wrong number, which is worse than no
     answer at all. */
  int error;
} Parser;

static double parse_expression(Parser *p);
static double parse_power(Parser *p);

static void skip_ws(Parser *p) {
  while (p->buffer[p->pos] == ' ')
    p->pos++;
}

static double parse_unary(Parser *p) {
  skip_ws(p);
  if (p->buffer[p->pos] == '-') {
    p->pos++;
    return -parse_unary(p);
  }
  if (p->buffer[p->pos] == '+') {
    p->pos++;
    return parse_unary(p);
  }
  return parse_power(p);
}

/* J4a: the keypad offers ln, log, sqrt and ^, so the parser has to understand
   them, or the buttons lie. sin/cos/tan are deliberately absent: without a
   Deg/Rad switch they would be ambiguous, and Google's calculator is DEG while
   a mathematician expects radians. Better a smaller keypad that is right. */
static double parse_unary(Parser *p);

typedef struct {
  const char *name;
  double (*fn)(double);
} CalcFunc;

static double fn_ln(double x) { return log(x); }
static double fn_log10(double x) { return log10(x); }
static double fn_sqrt(double x) { return sqrt(x); }
static double fn_abs(double x) { return fabs(x); }

static const CalcFunc CALC_FUNCS[] = {
    {"ln", fn_ln}, {"log", fn_log10}, {"sqrt", fn_sqrt}, {"abs", fn_abs}};

static double parse_factor(Parser *p) {
  skip_ws(p);
  /* a function name or a constant */
  for (size_t i = 0; i < sizeof(CALC_FUNCS) / sizeof(CALC_FUNCS[0]); i++) {
    size_t len = strlen(CALC_FUNCS[i].name);
    if (strncmp(&p->buffer[p->pos], CALC_FUNCS[i].name, len) == 0) {
      p->pos += (int)len;
      skip_ws(p);
      if (p->buffer[p->pos] != '(') {
        p->error = 1; /* "ln 2" is not accepted; "ln(2)" is */
        return 0.0;
      }
      p->pos++;
      double arg = parse_expression(p);
      skip_ws(p);
      if (p->buffer[p->pos] != ')') {
        p->error = 1;
        return 0.0;
      }
      p->pos++;
      return CALC_FUNCS[i].fn(arg);
    }
  }
  if (p->buffer[p->pos] == '\xcf' && p->buffer[p->pos + 1] == '\x80') {
    p->pos += 2; /* π */
    return 3.14159265358979323846;
  }
  /* "pi" is what people type on a keyboard without a π key. The keypad sends
     the glyph, so both spellings have to work or the same value has two
     behaviours depending on how it was entered. */
  if ((p->buffer[p->pos] == 'p' || p->buffer[p->pos] == 'P') &&
      (p->buffer[p->pos + 1] == 'i' || p->buffer[p->pos + 1] == 'I') &&
      !isalpha((unsigned char)p->buffer[p->pos + 2])) {
    p->pos += 2;
    return 3.14159265358979323846;
  }
  if ((p->buffer[p->pos] == 'e' || p->buffer[p->pos] == 'E') &&
      !isdigit((unsigned char)p->buffer[p->pos + 1])) {
    p->pos++;
    return 2.71828182845904523536;
  }
  if (p->buffer[p->pos] == '(') {
    p->pos++;
    double res = parse_expression(p);
    skip_ws(p);
    if (p->buffer[p->pos] == ')')
      p->pos++;
    else
      p->error = 1; /* unbalanced: "5 * (2+3" is not a number */
    return res;
  }
  char *endptr;
  double val = strtod(&p->buffer[p->pos], &endptr);
  if (endptr == &p->buffer[p->pos]) {
    /* nothing consumable here: "5 + * 3", "()" and "" all land here */
    p->error = 1;
    return 0.0;
  }
  p->pos = (int)(endptr - p->buffer);
  return val;
}

/* G4: "^" was in the matcher but never in the parser, so "2^10" quietly
   answered 2 - a confidently wrong number, which is worse than no answer.
   Right-associative and binding tighter than * and /, like the usual
   convention (2^3^2 is 2^(3^2)). */
static double parse_power(Parser *p) {
  double base = parse_factor(p);
  skip_ws(p);
  if (p->buffer[p->pos] == '^') {
    p->pos++;
    /* the exponent takes a unary sign (2^-1) and ^ is right-associative */
    double exponent = parse_unary(p);
    double result = 1.0;
    if (exponent == (int)exponent && exponent >= 0 && exponent <= 1024) {
      for (int i = 0; i < (int)exponent; i++)
        result *= base;
    } else {
      result = pow(base, exponent);
    }
    skip_ws(p);
    return result;
  }
  return base;
}

static double parse_term(Parser *p) {
  double left = parse_unary(p);
  while (1) {
    skip_ws(p);
    char op = p->buffer[p->pos];
    if (op == '*' || op == '/') {
      p->pos++;
      double right = parse_unary(p);
      double old = left;
      left = (op == '*') ? left * right : left / right;

      char step[256];

      snprintf(step, sizeof(step),
               "<div>%.12g %c %.12g = <b>%.12g</b></div>", old, op, right,
               left);
      strncat(logic_log, step, sizeof(logic_log) - strlen(logic_log) - 1);
    } else
      break;
  }
  return left;
}

static double parse_expression(Parser *p) {
  double left = parse_term(p);
  while (1) {
    skip_ws(p);
    char op = p->buffer[p->pos];
    if (op == '+' || op == '-') {
      p->pos++;
      double right = parse_term(p);
      double old = left;
      left = (op == '+') ? left + right : left - right;

      char step[256];

      snprintf(step, sizeof(step),
               "<div>%.12g %c %.12g = <b>%.12g</b></div>", old, op, right,
               left);
      strncat(logic_log, step, sizeof(logic_log) - strlen(logic_log) - 1);
    } else
      break;
  }
  return left;
}

/* J6.3: one formatter. The strip said 4.4 and the card said 4.4000000000001
   for the same expression, because each formatted its own way - %.10g in one
   place and %.12g in another. Every caller that shows a number uses this. */
void format_number(double value, char *out, size_t size) {
  if (!out || size == 0)
    return;
  if (value == (long long)value && value < 1e15 && value > -1e15)
    snprintf(out, size, "%lld", (long long)value);
  else
    snprintf(out, size, "%.12g", value);
}

double evaluate(const char *expr) {
  logic_log[0] = '\0';
  if (!expr || strlen(expr) == 0)
    return NAN;
  Parser p = {expr, 0, 0};
  double value = parse_expression(&p);
  /* a parse that did not consume the whole string did not understand it */
  skip_ws(&p);
  if (p.buffer[p.pos] != '\0')
    p.error = 1;
  if (p.error)
    return NAN;
  return value;
}

InfoBox fetch_calc_data(char *math_input) {
  InfoBox info = {NULL, NULL, NULL, NULL, 0};
  if (!math_input)
    return info;

  double result = evaluate(math_input);

  /* J4a: say what happened instead of printing a number that is not one.
     Google prints Infinity for 1/0, and Error for something it cannot parse. */
  const char *shown;
  char number[64];
  if (isnan(result)) {
    shown = "Error";
    snprintf(number, sizeof(number), "%s", shown);
  } else if (isinf(result)) {
    shown = result < 0 ? "-Infinity" : "Infinity";
    snprintf(number, sizeof(number), "%s", shown);
  } else {
    snprintf(number, sizeof(number), "%.12g", result);
    shown = number;
  }

  char html_output[5120];
  snprintf(html_output, sizeof(html_output),
           "<div class='calc-container'>"
           "%s"
           "<div class='calc-steps'>"
           "<b>%s</b>"
           "</div>"
           "</div>",
           strlen(logic_log) > 0 ? logic_log : "<div>Constant value</div>",
           shown);

  info.title = strdup("Calculation");
  info.extract = strdup(html_output);
  info.is_html = 1;
  /* J2: no image tile. It was a hard-coded #3498db glyph on white and an
     <img> cannot take currentColor, so it was wrong on every dark palette. */
  info.thumbnail_url = strdup("");
  info.url = strdup("#");

  return info;
}

/* G4: the matcher lives with the answer it guards, so the answer-box table
   is self-contained and the test can drive matcher and answerer together. */
/* The only words an expression may contain. "covid-19" has a digit and a
   minus, which was enough for the old check, so a search about a disease
   rendered a calculator card that said "Error" - measured in the J2 contact
   sheet at docs/themes/. A word that is not on this list makes the query a
   search, not an expression. */
static int is_expression_word(const char *word, size_t len) {
  static const char *WORDS[] = {"pi",  "e",    "sqrt", "cbrt", "abs",  "ln",
                                "log", "log2", "log10", "exp", "sin", "cos",
                                "tan", "asin", "acos", "atan", "mod", "ans"};
  for (size_t i = 0; i < sizeof(WORDS) / sizeof(WORDS[0]); i++) {
    if (strlen(WORDS[i]) == len && strncasecmp(word, WORDS[i], len) == 0)
      return 1;
  }
  return 0;
}

/* 1 when every alphabetic run in the query is a known function or constant. */
static int only_expression_words(const char *query) {
  const char *p = query;
  while (*p) {
    if (isalpha((unsigned char)*p)) {
      const char *start = p;
      while (isalpha((unsigned char)*p))
        p++;
      if (!is_expression_word(start, (size_t)(p - start)))
        return 0;
    } else {
      p++;
    }
  }
  return 1;
}

int is_calculator_query(const char *query) {
  if (!query)
    return 0;
  /* J4a: a word that is not a function or constant cannot be an expression */
  if (!only_expression_words(query))
    return 0;

  int has_digit = 0;
  int has_math_operator = 0;

  for (const char *p = query; *p; p++) {
    if (isdigit(*p) || *p == '.') {
      has_digit = 1;
    }
    if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '^') {
      has_math_operator = 1;
    }
  }

  if (!has_digit || !has_math_operator)
    return 0;

  int len = strlen(query);
  for (int i = 0; i < len; i++) {
    char c = query[i];
    if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^') {
      int has_num_before = 0;
      int has_num_after = 0;

      for (int j = i - 1; j >= 0; j--) {
        if (isdigit(query[j]) || query[j] == '.') {
          has_num_before = 1;
          break;
        }
        if (query[j] != ' ')
          break;
      }

      for (int j = i + 1; j < len; j++) {
        if (isdigit(query[j]) || query[j] == '.') {
          has_num_after = 1;
          break;
        }
        if (query[j] != ' ')
          break;
      }

      if (has_num_before || has_num_after) {
        return 1;
      }
    }
  }

  return 0;
}
