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
  "https://query1.finance.yahoo.com/v8/finance/chart/%.12s?interval=30m&range=5d"

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

/* Numbers the reader's way: thousands separators, and the decimal mark the
   locale uses. A price is a number to read, not a float to dump. */
static void stock_format_number(double value, int decimals, const char *lang,
                                char *out, size_t cap) {
  char raw[64];
  snprintf(raw, sizeof(raw), "%.*f", decimals, value);
  int decimal_comma = 0;
  if (lang) {
    static const char *COMMA[] = {"de", "es", "it", "pt", "fr", "nl", "ru",
                                  "lv", "fi", "vi", "af"};
    for (size_t i = 0; i < sizeof(COMMA) / sizeof(COMMA[0]); i++)
      if (strncasecmp(lang, COMMA[i], 2) == 0)
        decimal_comma = 1;
  }
  char *dot = strchr(raw, '.');
  char whole[48], frac[16];
  if (dot) {
    size_t n = (size_t)(dot - raw);
    if (n >= sizeof(whole))
      n = sizeof(whole) - 1;
    memcpy(whole, raw, n);
    whole[n] = '\0';
    snprintf(frac, sizeof(frac), "%s", dot + 1);
  } else {
    snprintf(whole, sizeof(whole), "%s", raw);
    frac[0] = '\0';
  }
  /* group the integer part in threes, from the right */
  char grouped[64];
  size_t glen = 0, wlen = strlen(whole);
  for (size_t i = 0; i < wlen && glen + 2 < sizeof(grouped); i++) {
    if (i > 0 && (wlen - i) % 3 == 0)
      grouped[glen++] = decimal_comma ? '.' : ',';
    grouped[glen++] = whole[i];
  }
  grouped[glen] = '\0';
  if (frac[0])
    snprintf(out, cap, "%s%s%s", grouped, decimal_comma ? "," : ".", frac);
  else
    snprintf(out, cap, "%s", grouped);
}

/* "30 Oct, 16:00 UTC" - UTC because the provider's stamp is UTC and inventing
   a zone from the reader's locale would be a guess. */
static void stock_format_time(time_t t, char *out, size_t cap) {
  struct tm tm_utc;
  gmtime_r(&t, &tm_utc);
  strftime(out, cap, "%e %b, %H:%M UTC", &tm_utc);
}

/* K11: names to symbols, from the bundled SEC table.
 *
 * The table is a *name* map, not a quote source: it answers "nvidia" and
 * "apple" with no network and no leak, and its absence is not an error - a
 * name that is not in it is a name, not a guessed ticker. The file is read
 * once, because a 8,000-line table is 2 ms and the alternative is a request
 * per keystroke. */
typedef struct {
  char *name;
  char *ticker;
} SymbolRow;

static SymbolRow g_symbols[9000];
static int g_symbol_count;
static int g_symbols_loaded;

/* generic words that are also company names in the SEC file ("NEWS" is News
   Corp): resolving them would answer a query about the news with a quote */
static const char *NAME_STOPLIST[] = {
    "NEWS", "MARKET", "MARKETS", "STOCK", "STOCKS", "SHARE", "SHARES",
    "PRICE", "PRICES", "QUOTE", "WATCH", "TODAY", "MONEY", "CASH", "TRADE",
    "TRADING", "FUND", "FUNDS", "GOLD", "SILVER", "OIL", "GAS", "BANK",
    "BANKS", "CRYPTO", "BITCOIN", "DOLLAR", "EURO", "PHOTOS", "PHOTO",
    "IMAGES", "VIDEOS", "BEST", "GOOD", "CHEAP"};

static int in_stoplist(const char *name) {
  for (size_t i = 0; i < sizeof(NAME_STOPLIST) / sizeof(NAME_STOPLIST[0]); i++)
    if (strcmp(name, NAME_STOPLIST[i]) == 0)
      return 1;
  return 0;
}

/* upper-case, strip punctuation, drop a leading THE and the legal forms a
   company name carries and nobody types */
static void normalise_name(const char *in, char *out, size_t cap) {
  static const char *SUFFIX[] = {
      " INCORPORATED", " INC", " CORPORATION", " CORP", " COMPANY", " CO",
      " LIMITED", " LTD", " LLC", " PLC", " HOLDINGS", " HOLDING", " GROUP",
      " SA", " NV", " AG", " SE", " AB", " TRUST", " FUND", " CLASS A",
      " CLASS B", " CLASS C", " ADR", " REIT"};
  size_t used = 0;
  if (!in || !out || cap == 0)
    return;
  out[0] = '\0';
  for (const char *c = in; *c && used + 1 < cap; c++) {
    if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
        (*c >= '0' && *c <= '9'))
      out[used++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
    else if (used > 0 && out[used - 1] != ' ')
      out[used++] = ' ';
  }
  while (used > 0 && out[used - 1] == ' ')
    used--;
  out[used] = '\0';
  if (strncmp(out, "THE ", 4) == 0)
    memmove(out, out + 4, strlen(out + 4) + 1);
  /* one pass is enough: the SEC writes a single legal form at the end */
  size_t len = strlen(out);
  for (size_t i = 0; i < sizeof(SUFFIX) / sizeof(SUFFIX[0]); i++) {
    size_t slen = strlen(SUFFIX[i]);
    if (len > slen && strcmp(out + len - slen, SUFFIX[i]) == 0) {
      out[len - slen] = '\0';
      break;
    }
  }
  len = strlen(out);
  while (len > 0 && out[len - 1] == ' ')
    out[--len] = '\0';
}

static void load_symbol_table(void) {
  if (g_symbols_loaded)
    return;
  g_symbols_loaded = 1;
  const char *path = getenv("OMNI_STOCK_TABLE");
  if (!path || !path[0])
    path = "static/stocks/company_tickers.tsv";
  FILE *fh = fopen(path, "r");
  if (!fh)
    return;
  char line[256];
  while (fgets(line, sizeof(line), fh) && g_symbol_count < 9000) {
    if (line[0] == '#')
      continue;
    char *tab = strchr(line, '\t');
    if (!tab)
      continue;
    *tab = '\0';
    char *ticker = tab + 1;
    ticker[strcspn(ticker, "\r\n")] = '\0';
    if (!line[0] || !ticker[0])
      continue;
    g_symbols[g_symbol_count].name = strdup(line);
    g_symbols[g_symbol_count].ticker = strdup(ticker);
    if (!g_symbols[g_symbol_count].name || !g_symbols[g_symbol_count].ticker) {
      free(g_symbols[g_symbol_count].name);
      free(g_symbols[g_symbol_count].ticker);
      break;
    }
    g_symbol_count++;
  }
  fclose(fh);
}

/* exact name match first, then the name with its leading words dropped
   ("berkshire hathaway class b" -> "BERKSHIRE HATHAWAY") */
int stocks_lookup_name(const char *name, char *out, size_t cap) {
  if (!name || !out || cap == 0)
    return 0;
  load_symbol_table();
  char key[128];
  normalise_name(name, key, sizeof(key));
  if (!key[0] || in_stoplist(key))
    return 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < g_symbol_count; i++) {
      if (strcmp(g_symbols[i].name, key) == 0) {
        snprintf(out, cap, "%s", g_symbols[i].ticker);
        return 1;
      }
    }
    char *space = strchr(key, ' ');
    if (!space)
      break;
    memmove(key, space + 1, strlen(space + 1) + 1);
    if (in_stoplist(key))
      return 0;
  }
  return 0;
}

/* is this an actual ticker in the table? "nvda stock" typed in lower case is a
   symbol the reader typed, not a name we are guessing at */
int stocks_symbol_known(const char *symbol) {
  if (!symbol || !symbol[0])
    return 0;
  load_symbol_table();
  for (int i = 0; i < g_symbol_count; i++)
    if (strcasecmp(g_symbols[i].ticker, symbol) == 0)
      return 1;
  return 0;
}

/* defined below: the parse uses it to decide whether a token was typed as a
   symbol, and the widget uses it to decide whether a failure earns a card */
int stocks_symbol_was_explicit(const char *query, const char *ticker);

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
  for (int i = 0; i < count; i++) {
    if (is_price_intent(words[i])) {
      intent = 1;
      break;
    }
    /* "$NVDA" and "^GSPC" are how people write a symbol, and the notation
       carries the intent: nobody types a dollar sign for a web search */
    if ((words[i][0] == '$' || words[i][0] == '^') &&
        looks_like_ticker_word(words[i])) {
      intent = 1;
      break;
    }
  }
  if (!intent)
    return 0;

  /* K11: a bare lower-case word is a NAME until something proves it a ticker.
     The old rule took the first word, so "nvidia" became the ticker NVIDIA and
     the chart endpoint answered "symbol may be delisted" - a card for a company
     that does not trade under that symbol. The order below is: notation first
     ($NVDA, ^GSPC, brk-b, an all-caps token), then the bundled name table, then
     - only for a symbol the table knows - the word itself. */
  const char *candidate = NULL;
  char resolved[16];
  /* an explicit notation is a symbol wherever it appears */
  for (int i = 0; i < count && !candidate; i++) {
    if (is_stock_keyword(words[i]) || is_link_word(words[i]))
      continue;
    if (words[i][0] == '$' || words[i][0] == '^' || strchr(words[i], '.') ||
        strchr(words[i], '-'))
      if (looks_like_ticker_word(words[i]))
        /* "$" is notation and is stripped; the caret is part of an index
           symbol (^GSPC), so it is kept */
        candidate = (words[i][0] == '$') ? words[i] + 1 : words[i];
  }
  /* an all-caps token the reader typed is a symbol too */
  if (!candidate)
    for (int i = 0; i < count && !candidate; i++) {
      if (is_stock_keyword(words[i]) || is_link_word(words[i]))
        continue;
      const char *w = words[i];
      int upper = 1, lower = 0;
      for (const char *c = w; *c; c++) {
        if (*c >= 'a' && *c <= 'z')
          lower = 1;
        else if (*c < 'A' || *c > 'Z')
          upper = 0;
      }
      if (upper && !lower && strlen(w) >= 2 && strlen(w) <= 10)
        candidate = w;
    }
  /* "of X" / "for X" / "de X" */
  if (!candidate)
    for (int i = 0; i + 1 < count && !candidate; i++)
      if (is_link_word(words[i]) && looks_like_ticker_word(words[i + 1]) &&
          !is_stock_keyword(words[i + 1]))
        candidate = words[i + 1];

  /* the name table: first the whole phrase the reader typed without the
     keyword ("bank of america" in "bank of america stock"), then the first
     word on its own ("nvidia" in "nvidia share price") */
  if (!candidate || !stocks_symbol_was_explicit(query, candidate)) {
    char phrase[128];
    size_t used = 0;
    phrase[0] = '\0';
    for (int i = 0; i < count; i++) {
      /* only the keyword goes: "bank of america" keeps its "of", because that
         is the name, while "of X" above is the link form */
      if (is_stock_keyword(words[i]))
        continue;
      int n = snprintf(phrase + used, sizeof(phrase) - used, "%s%s",
                       used ? " " : "", words[i]);
      if (n < 0 || (size_t)n >= sizeof(phrase) - used)
        break;
      used += (size_t)n;
    }
    if (used > 0 && stocks_lookup_name(phrase, resolved, sizeof(resolved))) {
      snprintf(ticker, ticker_size, "%s", resolved);
      return 1;
    }
    /* "nvda stock": the word is not a company name, but it is a ticker the
       table lists, so it is a symbol the reader typed, not a guess */
    if (used > 0 && stocks_symbol_known(phrase)) {
      size_t out = 0;
      for (const char *c = phrase; *c && out + 1 < ticker_size; c++)
        ticker[out++] = (char)((*c >= 'a' && *c <= 'z') ? *c - 32 : *c);
      ticker[out] = '\0';
      return 1;
    }
    if (candidate && strlen(candidate) > 0 &&
        stocks_lookup_name(candidate, resolved, sizeof(resolved))) {
      snprintf(ticker, ticker_size, "%s", resolved);
      return 1;
    }
    /* a lower-case word that is only a word: no card, and no request either */
    if (candidate && !stocks_symbol_was_explicit(query, candidate) &&
        !stocks_symbol_known(candidate))
      candidate = NULL;
  }
  if (!candidate || !candidate[0] || strlen(candidate) > 11)
    return 0;
  /* a lower-case candidate that the table does not know is a name, not a
     symbol: answering it with an error card is the bug this replaces */
  if (!stocks_symbol_was_explicit(query, candidate) &&
      !stocks_symbol_known(candidate))
    return 0;
  const char *bare = (candidate[0] == '$') ? candidate + 1 : candidate;
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
    const char *bare = token[0] == '$' ? token + 1 : token;
    const char *sym = ticker[0] == '$' ? ticker + 1 : ticker;
    if (strcasecmp(bare, sym) != 0)
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

int stocks_widget(const char *ticker, int explicit_symbol, const char *lang,
                  const char *range, WidgetData *out) {
  if (!out || !ticker || !ticker[0])
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->kind, sizeof(out->kind), "stock");
  snprintf(out->title, sizeof(out->title), "Share price");
  snprintf(out->expression, sizeof(out->expression), "%s", ticker);

  /* K11: the window. The card's range links send range=<yahoo window>; an
     unknown or absent one falls back to the five days the plan asks for. */
  static const struct {
    const char *id;
    const char *interval;
    const char *range;
  } WINDOWS[] = {{"1d", "5m", "1d"},   {"5d", "30m", "5d"},
                 {"1mo", "1d", "1mo"}, {"6mo", "1d", "6mo"},
                 {"ytd", "1d", "ytd"}, {"1y", "1d", "1y"},
                 {"5y", "1wk", "5y"}};
  const char *interval = "30m", *window = "5d";
  for (size_t w = 0; w < sizeof(WINDOWS) / sizeof(WINDOWS[0]); w++)
    if (range && strcmp(range, WINDOWS[w].id) == 0) {
      interval = WINDOWS[w].interval;
      window = WINDOWS[w].range;
      break;
    }
  char url[1200];
  snprintf(url, sizeof(url),
           "https://query1.finance.yahoo.com/v8/finance/chart/%.12s?"
           "interval=%s&range=%s",
           ticker, interval, window);
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
  /* K11: the quote is presented, not dumped. Two decimals for a share price,
     four for a sub-dollar one, thousands separators and the reader's decimal
     mark; the name and the exchange under the number; the change with an arrow
     so colour is never the only signal. An index is quoted in points, not in a
     currency it does not have. */
  {
    const char *quote_type =
        meta ? json_string_value(json_object_get(meta, "quoteType")) : NULL;
    const char *long_name =
        meta ? json_string_value(json_object_get(meta, "longName")) : NULL;
    if (!long_name && meta)
      long_name = json_string_value(json_object_get(meta, "shortName"));
    const char *exchange = meta
                               ? json_string_value(json_object_get(
                                     meta, "fullExchangeName"))
                               : NULL;
    if (!exchange && meta)
      exchange = json_string_value(json_object_get(meta, "exchangeName"));

    char number[64];
    int decimals = json_number_value(price) < 1.0 ? 4 : 2;
    stock_format_number(json_number_value(price), decimals, lang, number,
                        sizeof(number));
    const char *unit = ((quote_type && strcasecmp(quote_type, "INDEX") == 0) ||
                        (symbol && symbol[0] == '^'))
                           ? "points"
                           : (currency && currency[0] ? currency : "");
    snprintf(out->result, sizeof(out->result), "%s %s", number, unit);

    char when[64] = "";
    if (json_is_integer(stamp)) {
      time_t t = (time_t)json_integer_value(stamp);
      stock_format_time(t, when, sizeof(when));
    }

    /* the change: absolute and percent, with the arrow before the number */
    char change_text[96] = "";
    if (json_is_number(prev) && json_number_value(prev) > 0) {
      double before = json_number_value(prev);
      double delta = json_number_value(price) - before;
      char abs_text[32], pct_text[32], abs_num[32];
      stock_format_number(delta < 0 ? -delta : delta, decimals, lang, abs_num,
                          sizeof(abs_num));
      snprintf(abs_text, sizeof(abs_text), "%s%s", delta < 0 ? "-" : "",
               abs_num);
      stock_format_number(delta / before * 100.0, 2, lang, pct_text,
                          sizeof(pct_text));
      snprintf(change_text, sizeof(change_text), "%s %s (%s%%)",
               delta > 0 ? "\xE2\x96\xB2" : delta < 0 ? "\xE2\x96\xBC" : "\xC2\xB7",
               abs_text, pct_text);
    }

    char head[96] = "";
    if (long_name && long_name[0])
      snprintf(head, sizeof(head), "%s", long_name);
    char listing[96] = "";
    if (exchange && exchange[0])
      snprintf(listing, sizeof(listing), "%s: %s", exchange,
               symbol ? symbol : ticker);
    else
      snprintf(listing, sizeof(listing), "%s", symbol ? symbol : ticker);

    snprintf(out->note, sizeof(out->note), "%s%s%s%s%s%s%s",
             head[0] ? head : listing, head[0] ? " · " : "",
             head[0] ? listing : "", change_text[0] ? " · " : "", change_text,
             when[0] ? " · " : "", when);
  }
  /* K11: the sparkline. The chart endpoint already returns the series in
     indicators.quote[0].close; the card draws it as path data in a 240x60 box,
     so the page needs no third-party script and no image request. */
  {
    json_t *indicators =
        json_object_get(json_array_get(results, 0), "indicators");
    json_t *quote = indicators ? json_object_get(indicators, "quote") : NULL;
    json_t *series =
        json_is_array(quote) ? json_object_get(json_array_get(quote, 0), "close")
                             : NULL;
    json_t *stamps = json_object_get(json_array_get(results, 0), "timestamp");
    if (json_is_array(series)) {
      size_t n = json_array_size(series);
      double lo = 0, hi = 0;
      int have = 0;
      for (size_t i = 0; i < n; i++) {
        json_t *v = json_array_get(series, i);
        if (!json_is_number(v))
          continue;
        double d = json_number_value(v);
        if (!have || d < lo)
          lo = d;
        if (!have || d > hi)
          hi = d;
        have = 1;
      }
      if (have && n > 1 && hi > lo) {
        /* beaker's context keeps 255 characters per value, and a truncated
           path draws a wrong chart. So: at most 28 points, integers, relative
           moves ("l3 -4"), which is about seven characters a point. A
           sparkline does not need one point per pixel. */
        size_t stride = (n + 27) / 28;
        if (stride < 1)
          stride = 1;
        size_t used = 0;
        int point = 0, prev_x = 0, prev_y = 0;
        for (size_t i = 0; i < n; i += stride) {
          json_t *v = json_array_get(series, i);
          if (!json_is_number(v))
            continue;
          double d = json_number_value(v);
          int x = (int)((double)i * 240.0 / (double)(n - 1));
          int y = 58 - (int)((d - lo) / (hi - lo) * 56.0);
          int wrote;
          if (point == 0)
            wrote = snprintf(out->chart_path + used,
                             sizeof(out->chart_path) - used, "M%d %d", x, y);
          else
            wrote = snprintf(out->chart_path + used,
                             sizeof(out->chart_path) - used, "l%d %d",
                             x - prev_x, y - prev_y);
          if (wrote < 0 || (size_t)wrote >= sizeof(out->chart_path) - used)
            break;
          used += (size_t)wrote;
          prev_x = x;
          prev_y = y;
          point++;
        }
        /* a path that does not fit is not drawn at all: a truncated "d" would
           draw a chart that is not the price */
        if (strlen(out->chart_path) > 240)
          out->chart_path[0] = '\0';
        (void)stamps;
      }
    }
  }

  out->ok = 1;
  json_decref(root);
  return 1;
}
