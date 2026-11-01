/* K11: which queries name a company, and which are just words.
 *
 * Two bugs sat here. The first word became the ticker, so "nvidia" was sent as
 * NVIDIA and the provider answered "symbol may be delisted" - the card the
 * owner saw. And a failure for a name we had guessed at printed an apology
 * where the web results belonged.
 *
 * These are the rules, with no network: an explicit notation is a symbol, the
 * bundled SEC table resolves names, and a bare lower-case word that is neither
 * is a word. */
#include "Infobox/Stocks.h"
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

static void expect_symbol(const char *query, const char *want) {
  char ticker[16];
  checks++;
  if (!stocks_parse(query, ticker, sizeof(ticker)) ||
      strcasecmp(ticker, want) != 0) {
    printf("FAIL %-28s -> %s (wanted %s)\n", query,
           stocks_parse(query, ticker, sizeof(ticker)) ? ticker : "(none)",
           want);
    failures++;
  }
}

static void expect_nothing(const char *query) {
  char ticker[16];
  checks++;
  if (stocks_parse(query, ticker, sizeof(ticker))) {
    printf("FAIL %-28s -> %s (wanted no card)\n", query, ticker);
    failures++;
  }
}

int main(void) {
  /* the table, with no request: the names people actually type */
  expect_symbol("nvidia stock", "NVDA");
  expect_symbol("nvidia share price", "NVDA");
  expect_symbol("apple stock", "AAPL");
  expect_symbol("tesla quote", "TSLA");
  expect_symbol("microsoft stock", "MSFT");
  expect_symbol("berkshire hathaway stock", "BRK-B");
  expect_symbol("bank of america stock", "BAC");

  /* notation the reader typed */
  expect_symbol("NVDA stock", "NVDA");
  expect_symbol("$NVDA", "NVDA");
  expect_symbol("nvda stock", "NVDA");
  expect_symbol("brk-b stock", "BRK-B");
  expect_symbol("^GSPC", "^GSPC");

  /* words that are not companies: no card, and no request either */
  expect_nothing("news today");
  expect_nothing("market watch");
  expect_nothing("stock photos");
  expect_nothing("stock market");
  expect_nothing("linux kernel");
  expect_nothing("price of gold");
  expect_nothing("nvidia");

  printf("K11 stocks: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
