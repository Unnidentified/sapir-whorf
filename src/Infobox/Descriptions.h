/* K7 step 2: a real description for a result the search engine would not
 * describe.
 *
 * When a site's robots.txt stops an engine reading the page, the engine prints
 * a placeholder instead of a description ("We would like to show you a
 * description here…"). Step 1 dropped those. This fetches the page ourselves,
 * narrowly: only for a blank row, 600 ms, 64 KB, three redirects, text/html,
 * through the image proxy's socket guard so a redirect cannot reach the
 * loopback address - and only when the site's own robots.txt allows us.
 *
 * If the site says no, the caller shows the URL's path as a breadcrumb, which
 * is a true description of what the link is. */
#ifndef DESCRIPTIONS_H
#define DESCRIPTIONS_H

#include <stddef.h>

/* The extraction alone, with no network: og:description, then meta
   description, then the first paragraph over 60 characters. Entities decoded,
   whitespace collapsed, cut at 200 characters on a word boundary. Returns 1
   when a description was found. */
int description_extract(const char *html, size_t len, char *out, size_t cap);

/* Fetch and extract, with robots.txt obeyed and the answer cached. Returns 1
   with a description in out; 0 when the site disallows us, the fetch failed or
   there was nothing to extract (the negative answer is cached too). */
int description_for_url(const char *url, char *out, size_t cap);

/* The signed link the results page carries for a row with no snippet:
   "/d?u=<base64url>&s=<hmac>". Returns malloc'd, or NULL when it cannot sign. */
char *description_href(const char *url);

/* The endpoint's side: verify the signature and fetch. Returns 1 with the text,
   0 when the signature is wrong or the site says no. */
int description_from_token(const char *encoded_url, const char *signature,
                           char *out, size_t cap);

/* Exposed for the test: does this robots.txt allow a path for our agent? */
int robots_allows(const char *robots_txt, const char *path);
#endif
