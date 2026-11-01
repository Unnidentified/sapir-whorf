/* K7: one table, one file.

   The strings here are the ones that appear in this project's own fixtures and
   replay corpus rather than from memory: tests/fixtures/yahoo/good.html carries
   "We would like to show you a description here but the site wo...".

   Matching is a case-insensitive PREFIX test against each entry, because the
   engines truncate with "..." and the truncation point is not stable. A prefix
   test on a distinctive opening sentence is safe; a substring test would not
   be, since a page could legitimately quote the sentence. */

#include "Placeholders.h"
#include <ctype.h>
#include <string.h>

static const char *PLACEHOLDERS[] = {
    /* Yahoo and Bing, truncated and full */
    "we would like to show you a description here",
    /* Bing's robots variant */
    "a description for this result is not available",
    "a description for this page is not available",
    /* Google's wording on the same condition */
    "no information is available for this page",
    "no information is available for this result",
    /* DuckDuckGo's shell when it has no abstract */
    "no results found for that query",
};

/* compare `text` against `prefix` ignoring case and leading space */
static int starts_with_folded(const char *text, const char *prefix) {
  while (*text == ' ' || *text == '\t')
    text++;
  while (*prefix) {
    if (!*text)
      return 0;
    unsigned char a = (unsigned char)*text;
    unsigned char b = (unsigned char)*prefix;
    if (tolower(a) != tolower(b))
      return 0;
    text++;
    prefix++;
  }
  return 1;
}

int snippet_is_placeholder(const char *text) {
  if (!text || !text[0])
    return 0;
  for (size_t i = 0; i < sizeof(PLACEHOLDERS) / sizeof(PLACEHOLDERS[0]); i++)
    if (starts_with_folded(text, PLACEHOLDERS[i]))
      return 1;
  return 0;
}
