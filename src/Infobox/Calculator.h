#ifndef CALCULATOR_H
#define CALCULATOR_H

#include "Infobox.h"

int is_calculator_query(const char *query);
double evaluate(const char *expr);
InfoBox fetch_calc_data(char *math_input);

#endif
