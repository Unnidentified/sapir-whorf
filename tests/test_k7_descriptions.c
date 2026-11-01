/* K7 step 2: the description extractor, with no network.
 *
 * The page is the thing being parsed, so these are real page shapes: an og
 * card, a plain meta description, a paragraph-only page, a page with nothing,
 * entities, and a description longer than the cut. robots.txt gets the same
 * treatment - a disallowed path is a "no" and the caller then shows the URL
 * path instead. */
#include "Infobox/Descriptions.h"
#include <stdio.h>
#include <string.h>

static int checks, failures;

static void expect_desc(const char *html, const char *want_part) {
  char out[512];
  checks++;
  int found = description_extract(html, strlen(html), out, sizeof(out));
  if (!found || !strstr(out, want_part)) {
    printf("FAIL extraction: found=%d out='%s' wanted '%s'\n", found, out,
           want_part);
    failures++;
  }
}

static void expect_empty(const char *html) {
  char out[512];
  checks++;
  if (description_extract(html, strlen(html), out, sizeof(out))) {
    printf("FAIL extraction: expected nothing, got '%s'\n", out);
    failures++;
  }
}

static void robots(const char *txt, const char *path, int want) {
  checks++;
  if (robots_allows(txt, path) != want) {
    printf("FAIL robots: '%s' for %s -> %d (wanted %d)\n", txt, path,
           robots_allows(txt, path), want);
    failures++;
  }
}

int main(void) {
  expect_desc("<html><head><meta property=\"og:description\" "
              "content=\"A static analyser for the Linux kernel.\"></head>"
              "<body></body></html>",
              "static analyser");
  expect_desc("<html><head><meta name=\"description\" content=\"Docs and "
              "release notes for the project.\"></head></html>",
              "release notes");
  expect_desc("<html><body><p>This is the opening paragraph of a page that has "
              "no meta description at all, and it is long enough to be used as "
              "one.</p></body></html>",
              "opening paragraph");
  /* the cut is on a word boundary and inside the limit */
  {
    char big[1024];
    snprintf(big, sizeof(big),
             "<html><head><meta name=\"description\" content=\"%s\"></head>"
             "</html>",
             "word word word word word word word word word word word word "
             "word word word word word word word word word word word word "
             "word word word word word word word word word word word word "
             "word word word word word word word word word word word word");
    char out[512];
    checks++;
    description_extract(big, strlen(big), out, sizeof(out));
    if (strlen(out) > 200) {
      printf("FAIL extraction: %zu characters, over the 200 limit\n",
             strlen(out));
      failures++;
    } else if (out[strlen(out) - 1] != 'd') {
      printf("FAIL extraction: cut mid-word ('%s')\n", out);
      failures++;
    }
  }
  /* entities are decoded, not shown as source */
  expect_desc("<html><head><meta name=\"description\" content=\"Fish &amp; "
              "chips &quot;recipe&quot;\"></head></html>",
              "Fish & chips \"recipe\"");
  expect_empty("<html><head><title>Nothing here</title></head><body></body></html>");
  expect_empty("");
  /* a short paragraph is not a description */
  expect_empty("<html><body><p>Short.</p></body></html>");

  robots("User-agent: *\nDisallow: /private\n", "/private/x", 0);
  robots("User-agent: *\nDisallow: /private\n", "/public/x", 1);
  robots("User-agent: omnisearch\nDisallow: /\n", "/anything", 0);
  robots("User-agent: googlebot\nDisallow: /\n", "/anything", 1);
  robots("", "/anything", 1);

  printf("K7 descriptions: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
