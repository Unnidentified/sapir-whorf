#ifndef PLACEHOLDERS_H
#define PLACEHOLDERS_H

#include <stddef.h>

/* K7: the line a search engine prints when its own crawler was not allowed to
   read the page:

     "We would like to show you a description here but the site won't allow us."

   It is upstream text, not ours, and it is long - so the snippet merge (which
   prefers the longer snippet) promoted it over a shorter real description from
   another engine. Returns 1 when the text is one of these, including the
   truncated form engines actually emit ("...the site wo..."). */
int snippet_is_placeholder(const char *text);

#endif
