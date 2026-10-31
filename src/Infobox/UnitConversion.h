#ifndef UNITCONVERSION_H
#define UNITCONVERSION_H

#include "Infobox.h"
#include <stddef.h>

int is_unit_conv_query(const char *query);
InfoBox fetch_unit_conv_data(const char *query);
/* plain-text form of the same conversion, for the answer box */
int unit_conv_answer(const char *query, char *out, size_t size);

#endif
