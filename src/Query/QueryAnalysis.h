#ifndef QUERY_ANALYSIS_H
#define QUERY_ANALYSIS_H

#include <stddef.h>

typedef enum {
  QI_GENERAL = 0,
  QI_CODE,
  QI_ACADEMIC,
  QI_ENTITY,
  QI_NEWS,
  QI_MEDIA
} QueryIntent;

typedef struct {
  char raw[512];            /* exactly what the user typed              */
  char clean[512];          /* operators removed, whitespace collapsed  */
  char terms[8][32];
  int term_count;
  char phrases[2][96];
  int phrase_count;
  char site[128];           /* site: filter, empty when absent          */
  char negatives[4][32];
  int negative_count;
  char filetype[16];
  QueryIntent intent;
  char lang[8];
  int is_question;
  int has_operators;
} SearchQuery;

void query_analyze(const char *raw, const char *locale, SearchQuery *out);
const char *query_intent_name(QueryIntent intent);

/* Build the text handed to one engine. Engines that understand site: and
   -term get them; the others receive the clean query only. */
void query_engine_text(const SearchQuery *query, int supports_site,
                       int supports_negatives, char *out, size_t size);

#endif
