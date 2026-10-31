/* G3: the element parser and its availability rule. The fixture is the shape
   the endpoint documents (a cb({...}) wrapper around a results[] array); it is
   a shape fixture, not a captured live response - see docs/G3-google-cse.md
   for why a live one could not be captured here. */
#include "../src/Scraping/ApiEngines.h"
#include "../src/Scraping/Scraping.h"
#include "../src/Utility/XmlHelper.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(int cond, const char *what) {
  if (!cond) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

int main(void) {
  /* availability requires BOTH cx and token: the endpoint 403s without a
     token, so claiming to be available would only produce a fake failure */
  api_google_cse_element_configure("012345678901234567890:abcdefghijk", NULL);
  expect(!api_google_cse_element_available(),
         "cx without token must not be available");
  expect(api_url_google_cse_element("linux", 1, "en") == NULL,
         "no url without a token");
  api_google_cse_element_configure(NULL, "tok");
  expect(!api_google_cse_element_available(),
         "token without cx must not be available");
  api_google_cse_element_configure("012345678901234567890:abcdefghijk", "tok");
  expect(api_google_cse_element_available(), "cx+token is available");
  char *url = api_url_google_cse_element("linux+kernel", 1, "en");
  expect(url != NULL, "url built");
  if (url) {
    expect(strstr(url, "cse/element/v1") != NULL, "element endpoint");
    expect(strstr(url, "cse_tok=tok") != NULL, "token carried");
    expect(strstr(url, "start=0") != NULL, "first page starts at 0");
    free(url);
  }
  char *p2 = api_url_google_cse_element("linux+kernel", 2, "en");
  expect(p2 && strstr(p2, "start=10") != NULL, "page 2 starts at 10");
  free(p2);

  const char *fixture =
      "cb({\"results\":["
      "{\"titleNoFormatting\":\"Linux kernel\",\"unescapedUrl\":"
      "\"https://kernel.org/\",\"content\":\"The Linux kernel "
      "architecture.\"},"
      "{\"title\":\"Kernel &amp; modules\",\"url\":\"https://example.org/k\","
      "\"content\":\"docs\"},"
      "{\"titleNoFormatting\":\"No link\",\"unescapedUrl\":\"\"}"
      "]})";
  SearchResult *out = NULL;
  int n = api_parse_google_cse_element(fixture, strlen(fixture), "en", &out, 10);
  if (n != 2) {
    printf("  (parsed %d results, len=%zu)\n", n, strlen(fixture));
    printf("  first 80: %.80s\n", fixture);
    printf("  avail=%d\n", api_google_cse_element_available());
  }
  expect(n == 2, "two results with a link, one without is dropped");
  if (n >= 1) {
    expect(strcmp(out[0].url, "https://kernel.org/") == 0, "url 0");
    expect(out[0].title && strstr(out[0].title, "Linux kernel") != NULL,
           "title 0");
    expect(out[0].snippet && strstr(out[0].snippet, "architecture") != NULL,
           "snippet 0");
  }
  if (n >= 2)
    expect(out[1].title && strstr(out[1].title, "&amp;") == NULL,
           "entities unescaped");
  if (out)
    xml_result_free(out, n);

  /* a 403 body is not JSON: zero, not a crash or a bogus result */
  const char *denied = "<html><head><title>Sorry...</title></head></html>";
  SearchResult *none = NULL;
  expect(api_parse_google_cse_element(denied, strlen(denied), "en", &none, 10) ==
             0,
         "html body yields zero");
  if (none)
    xml_result_free(none, 0);

  if (failures == 0)
    printf("G3 cse element: PASS\n");
  return failures ? 1 : 0;
}
