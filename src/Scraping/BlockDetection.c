#include "BlockDetection.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static int contains_ci(const char *haystack, const char *needle) {
  if (!haystack || !needle || !needle[0])
    return 0;
  size_t nlen = strlen(needle);
  for (const char *p = haystack; *p; p++) {
    if (strncasecmp(p, needle, nlen) == 0)
      return 1;
  }
  return 0;
}

static int title_contains(const char *response, const char *needle) {
  const char *t = strcasestr(response, "<title");
  if (!t)
    return 0;
  const char *gt = strchr(t, '>');
  if (!gt)
    return 0;
  const char *end = strcasestr(gt, "</title>");
  if (!end)
    return 0;
  size_t len = (size_t)(end - gt);
  if (len == 0 || len > 512)
    return 0;

  char title[513];
  size_t n = len - 1 < sizeof(title) - 1 ? len - 1 : sizeof(title) - 1;
  memcpy(title, gt + 1, n);
  title[n] = '\0';

  return contains_ci(title, needle);
}

/* Markers are looked for in the <title> only, never in the body: a genuine
   results page whose snippets mention "Just a moment..." or "Robot" used to be
   classified BLOCKED, dropped, counted against the breaker, and have its cookie
   jar deleted. Whole-body markers now also require a small page (a challenge
   shell has no results; a real SERP is large). Startpage and Ecosia are not in
   the registry, so their markers are gone. */
int response_is_blocked_page(const char *response) {
  if (!response)
    return 0;

  size_t len = strlen(response);
  int small = len < 20000;

  return title_contains(response, "captcha") ||
         title_contains(response, "just a moment") ||
         title_contains(response, "attention required") ||
         title_contains(response, "access denied") ||
         title_contains(response, "are you a robot") ||
         (small && contains_ci(response, "Just a moment...")) ||
         (small && contains_ci(response, ">Robot<")) ||
         /* engine-specific challenge shells: these classes/DOM hooks only exist
            on the interstitial, never on a results page */
         contains_ci(response, "anomaly-modal") ||
         contains_ci(response, "anomaly-modal__") ||
         contains_ci(response, "g-recaptcha") ||
         contains_ci(response, "h-captcha") ||
         (contains_ci(response, "rate_limit") &&
          contains_ci(response, "uptime")) ||
         contains_ci(response, "captcha-container") ||
         /* Brave's interstitial ships a set_token inside a challengeSet blob */
         contains_ci(response, "challengeSet:{set_token") ||
         contains_ci(response, "challengeset:{set_token");
}

/* The stronger form used by the scraper: a page is only "blocked" if it also
   produced no results. A real SERP can contain the word "captcha" in a snippet
   or a challenge markup fragment in an ad slot. */
int response_is_empty_body(const char *body, size_t len) {
  if (!body || len == 0)
    return 1;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)body[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      return 0;
  }
  return 1;
}

int response_is_blocked_for_engine(const char *response, int parsed_results) {
  if (parsed_results > 0)
    return 0;
  return response_is_blocked_page(response);
}
