/* J4c/J4d: which queries ask for a place or a share price. No network - the
   point is the trigger, because a false positive here answers a search with a
   map or a quote nobody asked for. */

#include "../src/Infobox/Places.h"
#include "../src/Infobox/Stocks.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void place(const char *query, int want_ok, const char *want) {
  char out[256] = {0};
  int ok = places_parse(query, out, sizeof(out));
  if (ok != want_ok || (want_ok && strcmp(out, want) != 0)) {
    printf("FAIL(place): '%s' -> ok=%d '%s' (wanted ok=%d '%s')\n", query, ok,
           out, want_ok, want);
    failures++;
  }
}

static void stock(const char *query, int want_ok, const char *want) {
  char out[16] = {0};
  int ok = stocks_parse(query, out, sizeof(out));
  if (ok != want_ok || (want_ok && strcmp(out, want) != 0)) {
    printf("FAIL(stock): '%s' -> ok=%d '%s' (wanted ok=%d '%s')\n", query, ok,
           out, want_ok, want);
    failures++;
  }
}

int main(void) {
  place("where is Madrid", 1, "Madrid");
  place("Where is Reykjavik?", 1, "Reykjavik?");
  place("map of the algarve", 1, "the algarve");
  place("coordinates of 10 Downing Street", 1, "10 Downing Street");
  place("dónde está Lisboa", 1, "Lisboa");
  place("donde esta Lima", 1, "Lima");
  place("mapa de Colombia", 1, "Colombia");
  /* not place lookups: a bare name is a search, and so is a question that only
     contains a place */
  place("Madrid", 0, "");
  place("where to buy a laptop", 0, "");
  place("where is", 0, "");
  place("", 0, "");

  stock("AAPL stock price", 1, "AAPL");
  stock("aapl stock", 1, "AAPL");
  stock("stock price of msft", 1, "MSFT");
  stock("quote for tsla", 1, "TSLA");
  stock("cotizacion de san.mc", 1, "SAN.MC");
  /* "price" alone is not a stock: "price of gold" is a web query */
  stock("price of gold", 0, "");
  stock("AAPL", 0, "");
  stock("stock market news", 0, "");
  stock("", 0, "");

  printf("J4c/J4d triggers: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
