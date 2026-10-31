#ifndef OMNISEARCH_TOKENS_H
#define OMNISEARCH_TOKENS_H

#include <stddef.h>

/* J3a: one tokenizer for the query, the ranker and the infobox gate.

   There used to be three, with three minimum lengths and three alphabets:
   QueryAnalysis split on spaces and kept [A-Za-z0-9_] of length >= 3, the
   ranker split on every byte outside [A-Za-z0-9] and kept length >= 2, and
   Search.c had a third copy. A query term and a document token could
   therefore never be equal for anything containing punctuation or a
   non-ASCII letter, which made document frequency 0 for that term, which
   made intended-IDF coverage 0 for *every* document, which blanked the page.
   Measured in the shipped log: 6 queries with 20-37 raw hits and 0 rendered
   (1+1, 2+2, covid-19, #123456, google_cse, 1.234,56 + 2). */

#define TOKEN_LEN 32

/* Split `text` into match tokens. Folding is applied (see tokens_fold).
   Kept: ASCII digit runs from length 1, everything else from length 2, and
   each CJK character on its own. `unique` drops repeats, keeping order. */
int tokens_split(const char *text, char out[][TOKEN_LEN], int max, int unique);

/* Lowercase + strip diacritics, punctuation preserved. Used for a query term,
   which is then matched literally before its sub-tokens are tried. */
void tokens_fold(char *dst, size_t size, const char *src);

/* 1 when `term` (already folded) matches `text` or its tokens: the literal
   substring path first (so "1+1", "c++", ".net", "node.js" work), then every
   sub-token must be present. Token comparison keeps the old tolerance:
   substring for terms of 5+ characters, one edit away for the same. */
int tokens_term_matches(const char *term, const char tokens[][TOKEN_LEN],
                        int count, const char *text);

/* 1 when the term contains a letter, ASCII or not. A query with no letter in
   any term is a numeric or symbolic expression: the engines judged it, so the
   relevance floor must not be applied (J3a task 3). */
int tokens_term_has_letter(const char *term);

#endif
