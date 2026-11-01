#include <stddef.h>

#ifndef CALCULATOR_H
#define CALCULATOR_H

#include "Infobox.h"

int is_calculator_query(const char *query);
double evaluate(const char *expr);

/* K9: the value "Ans" evaluates to, set by the caller before evaluate() */
void calculator_set_ans(double value);

/* the one place a computed number becomes text */
void format_number(double value, char *out, size_t size);
InfoBox fetch_calc_data(char *math_input);

#endif
