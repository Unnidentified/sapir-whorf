#include <stddef.h>

#ifndef CALCULATOR_H
#define CALCULATOR_H

#include "Infobox.h"

int is_calculator_query(const char *query);
double evaluate(const char *expr);

/* the one place a computed number becomes text */
void format_number(double value, char *out, size_t size);
InfoBox fetch_calc_data(char *math_input);

#endif
