#ifndef RANKER_H
#define RANKER_H

#include "../Query/QueryAnalysis.h"
#include <stddef.h>

typedef struct {
  char *url;
  char *title;
  char *snippet;
  char *key;              /* canonical URL, used for de-duplication */
  char domain[128];       /* registrable domain                    */
  double rrf;             /* reciprocal rank fusion score          */
  double score;           /* final blended score                   */
  double coverage;        /* IDF-weighted share of query terms hit */
  int order;              /* first-seen order, tie breaker         */
  int engine_mask;        /* bit i set when engine i returned it   */
  /* J5.2: borrowed from the parsed result set; NULL when absent */
  const char *thumbnail;
  const char *date;
  const char *author;
  const char *source;
  const char *kind;
  /* L5: a discussion's two counters, carried through the merge so the row can
     show them. 0 means the source did not say. */
  int score_points;
  int comment_count;
  const char *engine_groups[32]; /* index group of each contributing engine */
  double engine_weights[32];     /* routing weight of each contributing engine */
} RankDoc;

/* canonical URL / registrable domain (public suffix subset) */
void rank_canonical_url(const char *url, char *out, size_t size);
void rank_domain_of(const char *url, char *out, size_t size);

/* authority tier 0..1 for a registrable domain */
double rank_authority(const char *domain);

/* loads static/authority.tsv and static/spam_hosts.txt once */
void rank_init(const char *authority_path, const char *spam_path);

/* score every document in place (BM25, authority, phrase, consensus, spam) */
void rank_score(RankDoc *docs, int count, const SearchQuery *query);

/* collapse near-duplicate titles, keeping the most authoritative copy;
   returns the number of surviving documents (compacted to the front) */
int rank_dedupe(RankDoc *docs, int count);

/* free the strings owned by one document */
void rank_doc_free(RankDoc *doc);

#endif
