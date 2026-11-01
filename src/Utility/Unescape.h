#ifndef UNESCAPE_H
#define UNESCAPE_H

#include <stddef.h>

char *unescape_search_url(const char *input);
char *url_decode_query(const char *src);
/* escape plain text for HTML output; see J6.2 in Search.c */
void html_escape(const char *in, char *out, size_t size);

#endif

/* the five named entities, decimal and hex references - shared by the snippet
   cleaner and the description extractor */
void unescape_html_entities(const char *in, char *out, size_t size);
