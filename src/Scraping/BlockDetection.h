#ifndef BLOCK_DETECTION_H
#define BLOCK_DETECTION_H

/* Pure, dependency-free: used by Scraping.c and by the fixture tests. */
int response_is_blocked_page(const char *response);

/* Prefer this one: a page that yielded results is never "blocked", whatever
   words its snippets contain. */
int response_is_blocked_for_engine(const char *response, int parsed_results);

#endif
