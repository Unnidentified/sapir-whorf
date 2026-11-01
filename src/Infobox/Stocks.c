/* J4d: a price card.

   Two probes decided this one. Stooq's CSV endpoint 404s on both stooq.com and
   stooq.pl, so the obvious free source is gone. Yahoo's chart endpoint answers
   AAPL in 325 ms with price, currency and previous close.

   That endpoint is undocumented and not a public API. It is used here because
   it is the only source that answered without a key, and the card says what it
   is: a delayed quote, from an unofficial endpoint, that is not investment
   advice. If it breaks, the card says the provider did not answer - it does not
   show a stale number as if it were live. */

#include "AnswerBox.h"
#include "../Utility/HttpClient.h"
#include "../Utility/Utility.h"
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define STOCK_BUDGET_MS 900
#define STOCK_ENDPOINT                                                     \
  "https://query1.finance.yahoo.com/v8/finance/chart/%.12s?interval=1d&range=5d"

/* A ticker is only recognised when the query says it is one. An upper-case word
   on its own is a name or an acronym far more often than a listed company. */
static int looks_like_ticker_word(const char *word) {
  if (!word || !word[0])
    return 0;
  /* K11: "$NVDA" and "^GSPC" are how people write a symbol, so the prefix is
     part of the notation, not part of the symbol. */
  const char *body = (word[0] == '$' || word[0] == '^') ? word + 1 : word;
  if (!body[0] || strlen(body) > 10)
    return 0;
  for (const char *c = body; *c; c++)
    if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
          *c == '.' || *c == '-' || (*c >= '0' && *c <= '9')))
      return 0;
  return 1;
}

static int is_stock_keyword(const char *word) {
  static const char *KEYWORDS[] = {"stock",  "stocks",     "share",
                                   "shares", "quote",      "price",
                                   "precio", "cotizacion", "cotización",
                                   "accion", "acción"};
  for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++)
    if (strcasecmp(word, KEYWORDS[i]) == 0)
      return 1;
  return 0;
}

/* "stock"/"quote"/"cotización" is what makes a query a price request. "price"
   alone is not: "price of gold" is a web query. */
static int is_price_intent(const char *word) {
  return strcasecmp(word, "stock") == 0 || strcasecmp(word, "stocks") == 0 ||
         strcasecmp(word, "share") == 0 || strcasecmp(word, "shares") == 0 ||
         strcasecmp(word, "quote") == 0 ||
         strcasecmp(word, "cotizacion") == 0 ||
         strcasecmp(word, "cotización") == 0 ||
         strcasecmp(word, "accion") == 0 || strcasecmp(word, "acción") == 0;
}

static int is_link_word(const char *word) {
  return strcasecmp(word, "of") == 0 || strcasecmp(word, "for") == 0 ||
         strcasecmp(word, "de") == 0 || strcasecmp(word, "del") == 0;
}

/* A candidate has to look like a symbol rather than a word. "STOCK MARKET
   NEWS" used to take MARKET as the ticker; the rules below are what stopped it:
   an explicit "of/for/de X", a dotted or dashed symbol, or the first token -
   and never a keyword. */
int stocks_parse(const char *query, char *ticker, size_t ticker_size) {
  if (!query || !ticker)
    return 0;
  ticker[0] = '\0';
  char copy[512];
  snprintf(copy, sizeof(copy), "%s", query);
  char *start = copy;
  while (*start == ' ')
    start++;
  size_t len = strlen(start);
  while (len > 0 && start[len - 1] == ' ')
    start[--len] = '\0';

  char *words[16];
  int count = 0;
  char *saveptr = NULL;
  for (char *w = strtok_r(start, " ", &saveptr); w && count < 16;
       w = strtok_r(NULL, " ", &saveptr))
    words[count++] = w;
  if (count == 0)
    return 0;

  int intent = 0;
  for (int i = 0; i < count; i++)
    if (is_price_intent(words[i])) {
      intent = 1;
      break;
    }
  if (!intent)
    return 0;

  const char *candidate = NULL;
  /* "of X" / "for X" / "de X" is the most explicit form */
  for (int i = 0; i + 1 < count && !candidate; i++)
    if (is_link_word(words[i]) && looks_like_ticker_word(words[i + 1]) &&
        !is_stock_keyword(words[i + 1]))
      candidate = words[i + 1];
  /* a dotted or dashed symbol is a symbol wherever it appears */
  if (!candidate)
    for (int i = 0; i < count && !candidate; i++)
      if (looks_like_ticker_word(words[i]) && !is_stock_keyword(words[i]) &&
          (strchr(words[i], '.') || strchr(words[i], '-')))
        candidate = words[i];
  /* otherwise the first word, when it is not itself the keyword */
  if (!candidate && looks_like_ticker_word(words[0]) &&
      !is_stock_keyword(words[0]) && !is_link_word(words[0]))
    candidate = words[0];

  if (!candidate || !candidate[0] || strlen(candidate) > 11)
    return 0;
  const char *bare =
      (candidate[0] == '$' || candidate[0] == '^') ? candidate + 1 : candidate;
  snprintf(ticker, ticker_size, "%s", bare);
  for (char *c = ticker; *c; c++)
    if (*c >= 'a' && *c <= 'z')
      *c = (char)(*c - 'a' + 'A');
  return 1;
}

/* K11: was this unmistakably a symbol, or a company name we guessed at? Only
   the first earns an error card when the lookup fails. */
int stocks_symbol_was_explicit(const char *query, const char *ticker) {
  if (!query || !ticker || !ticker[0])
    return 0;
  char copy[512];
  snprintf(copy, sizeof(copy), "%s", query);
  char *saveptr = NULL;
  for (char *w = strtok_r(copy, " ", &saveptr); w; w = strtok_r(NULL, " ", &saveptr)) {
    char token[32];
    snprintf(token, sizeof(token), "%s", w);
    size_t len = strlen(token);
    while (len > 0 && (token[len - 1] == '?' || token[len - 1] == '.' ||
                       token[len - 1] == ','))
      token[--len] = '\0';
    if (!token[0])
      continue;
    const char *bare = token[0] == '$' || token[0] == '^' ? token + 1 : token;
    if (strcasecmp(bare, ticker) != 0)
      continue;
    if (token[0] == '$' || token[0] == '^')
      return 1;
    if (strchr(bare, '.') || strchr(bare, '-'))
      return 1;
    int has_upper = 0, has_lower = 0;
    for (const char *c = bare; *c; c++) {
      if (*c >= 'A' && *c <= 'Z')
        has_upper = 1;
      else if (*c >= 'a' && *c <= 'z')
        has_lower = 1;
    }
    return has_upper && !has_lower;
  }
  return 0;
}

int stocks_widget(const char *ticker, int explicit_symbol, WidgetData *out) {
  if (!out || !ticker || !ticker[0])
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->kind, sizeof(out->kind), "stock");
  snprintf(out->title, sizeof(out->title), "Share price");
  snprintf(out->expression, sizeof(out->expression), "%s", ticker);

  char url[1200];
  snprintf(url, sizeof(url), STOCK_ENDPOINT, ticker);
  HttpResponse resp = http_get_budget(
      url, "Mozilla/5.0 (compatible; omnisearch/1.0; "
           "+https://codeberg.org/gabrielf/omnisearch)",
      STOCK_BUDGET_MS);
  if (!resp.memory || resp.size == 0) {
    http_response_free(&resp);
    /* K11: an apology card for a name we guessed at is worse than none - the
       web results are the right answer for "nvidia". */
    if (!explicit_symbol)
      return 0;
    snprintf(out->note, sizeof(out->note), "Provider did not answer");
    return 1;
  }
  json_error_t error;
  json_t *root = json_loadb(resp.memory, resp.size, 0, &error);
  http_response_free(&resp);
  if (!root) {
    snprintf(out->note, sizeof(out->note), "Provider sent unreadable JSON");
    return 1;
  }
  json_t *chart = json_object_get(root, "chart");
  json_t *error_obj = chart ? json_object_get(chart, "error") : NULL;
  json_t *results = chart ? json_object_get(chart, "result") : NULL;
  if (!json_is_array(results) || json_array_size(results) == 0) {
    const char *why = error_obj ? json_string_value(
                                      json_object_get(error_obj, "description"))
                                : NULL;
    if (!explicit_symbol) {
      json_decref(root);
      return 0;
    }
    snprintf(out->note, sizeof(out->note), "%.80s",
             why && why[0] ? why : "No quote for that symbol");
    json_decref(root);
    return 1;
  }
  json_t *meta = json_object_get(json_array_get(results, 0), "meta");
  json_t *price = meta ? json_object_get(meta, "regularMarketPrice") : NULL;
  json_t *prev = meta ? json_object_get(meta, "chartPreviousClose") : NULL;
  const char *currency =
      meta ? json_string_value(json_object_get(meta, "currency")) : NULL;
  const char *symbol =
      meta ? json_string_value(json_object_get(meta, "symbol")) : NULL;
  json_t *stamp = meta ? json_object_get(meta, "regularMarketTime") : NULL;

  if (!json_is_number(price)) {
    if (!explicit_symbol) {
      json_decref(root);
      return 0;
    }
    snprintf(out->note, sizeof(out->note), "No quote for that symbol");
    json_decref(root);
    return 1;
  }
  snprintf(out->result, sizeof(out->result), "%.4f %s", json_number_value(price),
           currency ? currency : "");

  char when[32] = "";
  if (json_is_integer(stamp)) {
    time_t t = (time_t)json_integer_value(stamp);
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M UTC", &tm_utc);
  }
  if (json_is_number(prev)) {
    snprintf(out->note, sizeof(out->note), "%.10s · prev %.4f · %.28s",
             symbol ? symbol : ticker, json_number_value(prev), when);
  } else {
    snprintf(out->note, sizeof(out->note), "%.10s · %.28s",
             symbol ? symbol : ticker, when);
  }
  out->ok = 1;
  json_decref(root);
  return 1;
}
