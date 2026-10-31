#include "Calculator.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-thread: this file-level buffer was shared by every request thread, so
   two concurrent calculator queries interleaved their working. */
static __thread char logic_log[4096];

typedef struct {
  const char *buffer;
  int pos;
} Parser;

static double parse_expression(Parser *p);
static double parse_power(Parser *p);

static void skip_ws(Parser *p) {
  while (p->buffer[p->pos] == ' ')
    p->pos++;
}

static double parse_factor(Parser *p) {
  skip_ws(p);
  if (p->buffer[p->pos] == '-') {
    p->pos++;
    return -parse_factor(p);
  }
  if (p->buffer[p->pos] == '(') {
    p->pos++;
    double res = parse_expression(p);
    if (p->buffer[p->pos] == ')')
      p->pos++;
    return res;
  }
  char *endptr;
  double val = strtod(&p->buffer[p->pos], &endptr);
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
    double exponent = parse_power(p);
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
  double left = parse_power(p);
  while (1) {
    skip_ws(p);
    char op = p->buffer[p->pos];
    if (op == '*' || op == '/') {
      p->pos++;
      double right = parse_power(p);
      double old = left;
      left = (op == '*') ? left * right : left / right;

      char step[256];

      snprintf(step, sizeof(step), "<div>%g %c %g = <b>%g</b></div>", old, op,
               right, left);
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

      snprintf(step, sizeof(step), "<div>%g %c %g = <b>%g</b></div>", old, op,
               right, left);
      strncat(logic_log, step, sizeof(logic_log) - strlen(logic_log) - 1);
    } else
      break;
  }
  return left;
}

double evaluate(const char *expr) {
  logic_log[0] = '\0';
  if (!expr || strlen(expr) == 0)
    return 0.0;
  Parser p = {expr, 0};
  return parse_expression(&p);
}

InfoBox fetch_calc_data(char *math_input) {
  InfoBox info = {NULL, NULL, NULL, NULL};
  if (!math_input)
    return info;

  double result = evaluate(math_input);

  char html_output[5120];
  snprintf(html_output, sizeof(html_output),
           "<div class='calc-container'>"
           "%s"
           "<div class='calc-steps'>"
           "<b>%.12g</b>"
           "</div>"
           "</div>",
           strlen(logic_log) > 0 ? logic_log : "<div>Constant value</div>",
           result);

  info.title = strdup("Calculation");
  info.extract = strdup(html_output);
  /* J2: no image tile. It was a hard-coded #3498db glyph on white and an
     <img> cannot take currentColor, so it was wrong on every dark palette. */
  info.thumbnail_url = strdup("");
  info.url = strdup("#");

  return info;
}

/* G4: the matcher lives with the answer it guards, so the answer-box table
   is self-contained and the test can drive matcher and answerer together. */
int is_calculator_query(const char *query) {
  if (!query)
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
