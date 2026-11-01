#ifndef STOCKS_H
#define STOCKS_H

#include "AnswerBox.h"
#include <stddef.h>

/* J4d: 1 when the query asks for a share price, with `ticker` filled. Requires
   a word like "stock"/"quote" - an upper-case word alone is not a ticker. */
int stocks_parse(const char *query, char *ticker, size_t ticker_size);
/* K11: 1 when the token the user wrote is unmistakably a symbol ($NVDA, NVDA,
   BRK-B), which is what decides whether a failed lookup gets an error card or
   no card at all. */
int stocks_symbol_was_explicit(const char *query, const char *ticker);

/* 1 when a card applies; out->ok says whether a quote came back. */
int stocks_widget(const char *ticker, int explicit_symbol, WidgetData *out);

#endif
