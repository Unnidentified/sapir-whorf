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

/* K11: the bundled SEC name table. A name resolves with no network and no
   leak; a name that is not in it is a name, not a guessed ticker. */
int stocks_lookup_name(const char *name, char *out, size_t cap);
int stocks_symbol_known(const char *symbol);

/* 1 when a card applies; out->ok says whether a quote came back. */
int stocks_widget(const char *ticker, int explicit_symbol, const char *lang,
                  const char *range, WidgetData *out);

#endif
