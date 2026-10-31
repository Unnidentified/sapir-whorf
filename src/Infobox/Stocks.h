#ifndef STOCKS_H
#define STOCKS_H

#include "AnswerBox.h"
#include <stddef.h>

/* J4d: 1 when the query asks for a share price, with `ticker` filled. Requires
   a word like "stock"/"quote" - an upper-case word alone is not a ticker. */
int stocks_parse(const char *query, char *ticker, size_t ticker_size);

/* 1 when a card applies; out->ok says whether a quote came back. */
int stocks_widget(const char *ticker, WidgetData *out);

#endif
