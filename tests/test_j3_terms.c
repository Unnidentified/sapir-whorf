/* J3a: a query term and a document token come from the same tokenizer, so a
   term containing punctuation or a non-ASCII letter can match.

   Before this, QueryAnalysis kept the space-separated words of length >= 3
   verbatim ("1+1", "covid-19", "espa\u00f1a") while the ranker split every
   document on bytes outside [A-Za-z0-9]. Document frequency for such a term
   was therefore 0, the coverage loop skipped it, idf_sum stayed 0 and
   coverage came out 0.0 for every document. Measured in the shipped log: six
   queries with 20-37 raw hits and zero rendered.

   The doc sets below are built to mirror what a real SERP looks like for the
   query (title and snippet contain it, as engines echo the query). The
   measured live counts for the same queries are in docs/J3-terms.md. */
#include "../src/Query/QueryAnalysis.h"
#include "../src/Query/Tokens.h"
#include "../src/Ranking/Ranker.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void doc_text(RankDoc *d, const char *title, const char *snippet,
                     const char *url) {
  memset(d, 0, sizeof(*d));
  d->title = strdup(title);
  d->snippet = strdup(snippet);
  d->url = strdup(url);
  snprintf(d->domain, sizeof(d->domain), "example.com");
  d->key = strdup(url);
  d->rrf = 1.0;
}

/* Ten documents shaped like a SERP for `query`, then the relevance floor that
   Search.c applies (coverage >= 0.15) counted. Returns how many survive.
   `echo` decides what the SERP looks like: with echo (a real match) eight
   documents carry the query in title and snippet, as engine results do; with
   no echo (a query nothing matches) every document is unrelated -- which is
   what an engine returns for a string that does not exist. */
static int survivors_for(const char *query, int echo) {
  SearchQuery q;
  RankDoc docs[10];
  int kept = 0;
  char title[256];
  char snippet[400];

  query_analyze(query, "en_gb", &q);
  for (int i = 0; i < 10; i++) {
    if (echo && i < 8) {
      snprintf(title, sizeof(title), "%s - reference page %d", q.engine_core,
               i);
      snprintf(snippet, sizeof(snippet),
               "Everything about %s, written for people looking up %s.", query,
               query);
    } else {
      /* two documents that genuinely have nothing to do with the query */
      snprintf(title, sizeof(title), "Unrelated page %d", i);
      snprintf(snippet, sizeof(snippet), "Cooking, gardening and tax forms.");
    }
    {
      char url[128];
      snprintf(url, sizeof(url), "https://example.com/page-%d", i);
      doc_text(&docs[i], title, snippet, url);
    }
  }
  rank_score(docs, 10, &q);
  for (int i = 0; i < 10; i++) {
    if (docs[i].coverage >= 0.15)
      kept++;
    else
      rank_doc_free(&docs[i]);
  }
  return kept;
}

static void expect_min(const char *query, int want) {
  int got = survivors_for(query, 1);
  checks++;
  if (got < want) {
    printf("FAIL %-28s rendered %d, wanted >= %d\n", query, got, want);
    failures++;
  } else {
    printf("ok   %-28s rendered %d\n", query, got);
  }
}

static void expect_none(const char *query) {
  int got = survivors_for(query, 0);
  checks++;
  if (got != 0) {
    printf("FAIL %-28s rendered %d with no candidate containing a term\n",
           query, got);
    failures++;
  } else {
    printf("ok   %-28s rendered 0\n", query);
  }
}
static void token_case(const char *text, const char *first) {
  char tok[32][TOKEN_LEN];
  int n = tokens_split(text, tok, 32, 0);
  checks++;
  if (n < 1 || strcmp(tok[0], first) != 0) {
    printf("FAIL tokenize %-20s gave [%s], wanted [%s]\n", text,
           n ? tok[0] : "", first);
    failures++;
  } else {
    printf("ok   tokenize %-20s -> [%s]\n", text, first);
  }
}

int main(void) {
  rank_init("static/authority.tsv", "static/spam.tsv");

  /* the six queries the shipped log shows collapsing to zero */
  expect_min("1+1", 5);
  expect_min("2+2", 5);
  expect_min("covid-19", 5);
  expect_min("#123456", 5);
  expect_min("google_cse", 5);
  expect_min("1.234,56 + 2", 5);

  /* the shapes the review listed as affected by the same path */
  expect_min("c++", 5);
  expect_min("c#", 5);
  expect_min("node.js", 5);
  expect_min(".net", 5);
  expect_min("wi-fi", 5);
  expect_min("gpt-4", 5);
  expect_min("3.14", 5);
  expect_min("50%", 5);
  expect_min("e-mail", 5);
  expect_min("o'brien", 5);
  expect_min("$100", 5);
  expect_min("user@example.com", 5);
  expect_min("espa\u00f1a", 5);
  expect_min("cami\u00f3n", 5);
  expect_min("ni\u00f1o", 5);
  expect_min("caf\u00e9", 5);
  expect_min("\u00fcber", 5);
  expect_min("\u043c\u043e\u0441\u043a\u0432\u0430", 5);
  expect_min("\u65e5\u672c\u8a9e", 5);

  /* two-word versions of the same shapes */
  expect_min("covid-19 vaccine", 5);
  expect_min("node.js tutorial", 5);
  expect_min("espa\u00f1a turismo", 5);
  expect_min("gpt-4 api", 5);

  /* The nonsense guard: an alphabetic query no candidate contains still
     renders nothing. (The review's two example strings are not nonsense on
     the live web -- #vbbnmm is a TikTok hashtag and "asdfghjkl" is common --
     so absent strings are used here; see docs/J3-terms.md.) */
  expect_none("xqztvw plmnko");
  expect_none("qwzxvbnm plkjhgfds");
  expect_none("zzqqxx vbbnmm");

  /* the tokenizer itself */
  token_case("covid-19", "covid");
  token_case("espa\u00f1a", "espana");
  token_case("cami\u00f3n", "camion");
  token_case("node.js", "node");
  token_case("google_cse", "google");
  token_case("\u65e5\u672c\u8a9e", "\u65e5");
  token_case("1+1", "1");
  token_case("3.14", "3");
  token_case("AT&T", "at");

  printf("\n%s: %d cases, %d failed\n", failures ? "FAIL" : "PASS", checks,
         failures);
  return failures ? 1 : 0;
}
