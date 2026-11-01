#ifndef BLOCK_DETECTION_H
#define BLOCK_DETECTION_H

#include <stddef.h>

/* Pure, dependency-free: used by Scraping.c and by the fixture tests. */
int response_is_blocked_page(const char *response);

/* Prefer this one: a page that yielded results is never "blocked", whatever
   words its snippets contain. */
int response_is_blocked_for_engine(const char *response, int parsed_results);

/* K12.6: a 200 with nothing in it is not "no results". Yahoo answers
   site:reddit.com with an empty body when it decides not to serve the query,
   and the old code recorded that as a clean zero-result search: no block
   signal, nothing for the breaker, and the next query asked again. Empty (or
   whitespace-only) counts as a block signal for any engine that normally
   returns content. */
int response_is_empty_body(const char *body, size_t len);

#endif
