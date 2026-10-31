#ifndef API_ENGINES_H
#define API_ENGINES_H

#include "Scraping.h"

/* JSON/Atom API engines: no HTML scraping, predictable schemas. */
int api_parse_openalex(const char *body, size_t len, const char *lang,
                       SearchResult **out, int max);
int api_parse_crossref(const char *body, size_t len, const char *lang,
                       SearchResult **out, int max);
int api_parse_stackexchange(const char *body, size_t len, const char *lang,
                            SearchResult **out, int max);
int api_parse_hn(const char *body, size_t len, const char *lang,
                 SearchResult **out, int max);
int api_parse_wikipedia(const char *body, size_t len, const char *lang,
                        SearchResult **out, int max);
int api_parse_arxiv(const char *body, size_t len, const char *lang,
                    SearchResult **out, int max);

char *api_url_openalex(const char *encoded_query, int page, const char *lang);
char *api_url_crossref(const char *encoded_query, int page, const char *lang);
char *api_url_stackexchange(const char *encoded_query, int page,
                            const char *lang);
char *api_url_hn(const char *encoded_query, int page, const char *lang);
char *api_url_wikipedia(const char *encoded_query, int page, const char *lang);
char *api_url_arxiv(const char *encoded_query, int page, const char *lang);

/* Google Programmable Search (official JSON API, needs a key and cx) */
void api_google_cse_configure(const char *key, const char *cx);
int api_google_cse_available(void);
int api_parse_google_cse(const char *body, size_t len, const char *lang,
                         SearchResult **out, int max);
char *api_url_google_cse(const char *encoded_query, int page, const char *lang);

/* G3: Google Programmable Search *Element*. Clean-room. Needs cx plus the
   cse_tok that only the widget JavaScript can mint, so it stays unavailable
   unless the operator supplies one. Disabled by default. */
void api_google_cse_element_configure(const char *cx, const char *tok);
int api_google_cse_element_available(void);
int api_parse_google_cse_element(const char *body, size_t len, const char *lang,
                                 SearchResult **out, int max);
char *api_url_google_cse_element(const char *encoded_query, int page,
                                 const char *lang);
/* test hook: no network, runs the token-page extraction on a captured body */
int api_cse_extract_token_for_test(const char *body, size_t len);

/* SearXNG instance acting as a bridge to Google and other engines */
void api_searxng_configure(const char *base_url, const char *engines);
int api_searxng_available(void);
int api_parse_searxng(const char *body, size_t len, const char *lang,
                      SearchResult **out, int max);
char *api_url_searxng(const char *encoded_query, int page, const char *lang);

#endif
