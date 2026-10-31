/* J5.6: the SearXNG bridge.
 *
 * Two things are tested here and neither needs the network: the engine list
 * accepts bangs and de-duplicates, and each hit carries the index that
 * actually produced it. The second one matters because the bridge is one job
 * but many engines, so attributing every hit to the bridge's own index_group
 * made /settings claim one independent index when three had answered.
 */
#include "../src/Scraping/ApiEngines.h"
#include "../src/Scraping/Scraping.h"
#include "../src/Utility/XmlHelper.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_engines(const char *input, const char *want) {
  api_searxng_configure("http://127.0.0.1:8888", input);
  const char *got = api_searxng_engines();
  if (strcmp(got, want) != 0) {
    printf("FAIL engines(\"%s\") = \"%s\", wanted \"%s\"\n", input, got, want);
    failures++;
  } else {
    printf("ok   engines(\"%s\") -> %s\n", input, got);
  }
}

int main(void) {
  /* bangs, names and both together; duplicates dropped */
  expect_engines("!go,!ddg,br,qw", "google,duckduckgo,brave,qwant");
  expect_engines("google,brave", "google,brave");
  expect_engines("!go,google", "google");
  expect_engines("", "google");
  expect_engines("!se,!so", "stackexchange,stackoverflow");

  /* per-result index group: the bridge is one job, many indexes */
  {
    const char *json =
        "{\"results\":["
        "{\"title\":\"A\",\"url\":\"https://a.example/\",\"content\":\"x\","
        "\"engine\":\"brave\"},"
        "{\"title\":\"B\",\"url\":\"https://b.example/\",\"content\":\"y\","
        "\"engine\":\"qwant\"},"
        "{\"title\":\"C\",\"url\":\"https://c.example/\",\"content\":\"z\","
        "\"engine\":\"startpage\"},"
        "{\"title\":\"D\",\"url\":\"https://d.example/\",\"content\":\"w\","
        "\"engine\":\"something_new\"},"
        "{\"title\":\"E\",\"url\":\"https://e.example/\",\"content\":\"v\"}]}";
    SearchResult *out = NULL;
    int n = api_parse_searxng(json, strlen(json), "en", &out, 10);
    const char *want[] = {"brave", "qwant", "google", "other", ""};
    if (n != 5) {
      printf("FAIL parsed %d results, wanted 5\n", n);
      failures++;
    } else {
      int ok = 1;
      for (int i = 0; i < 5; i++) {
        if (strcmp(out[i].index_group, want[i]) != 0) {
          printf("FAIL hit %d group \"%s\", wanted \"%s\"\n", i,
                 out[i].index_group, want[i]);
          failures++;
          ok = 0;
        }
      }
      if (ok)
        printf("ok   per-hit groups: brave qwant google(startpage) other, "
               "and empty where the instance says nothing\n");
    }
    xml_result_free(out, n);
  }

  /* a bridge hit with no engine field leaves the group empty, so the caller
     falls back to the engine entry's group instead of inheriting "other" */
  {
    const char *json = "{\"results\":[{\"title\":\"F\",\"url\":"
                       "\"https://f.example/\",\"content\":\"u\"}]}";
    SearchResult *out = NULL;
    int n = api_parse_searxng(json, strlen(json), "en", &out, 10);
    if (n != 1 || out[0].index_group[0] != '\0') {
      printf("FAIL a hit without an engine field got group \"%s\"\n",
             n ? out[0].index_group : "(none)");
      failures++;
    } else {
      printf("ok   no engine field -> no group, so the engine entry decides\n");
    }
    xml_result_free(out, n);
  }

  /* J5.2: the rich fields the instance really sends, and nothing invented */
  {
    const char *json =
        "{\"results\":["
        "{\"title\":\"Video\",\"url\":\"https://v.example/1\","
        "\"content\":\"c\",\"engine\":\"brave\","
        "\"thumbnail\":\"https://img.example/t.jpg\","
        "\"publishedDate\":\"2026-10-01T10:00:00\",\"category\":\"videos\"},"
        "{\"title\":\"Bare\",\"url\":\"https://b.example/2\","
        "\"content\":\"c\",\"engine\":\"google\"},"
        "{\"title\":\"Bad thumb\",\"url\":\"https://t.example/3\","
        "\"content\":\"c\",\"engine\":\"google\","
        "\"thumbnail\":\"javascript:alert(1)\"}]}";
    SearchResult *out = NULL;
    int n = api_parse_searxng(json, strlen(json), "en", &out, 10);
    if (n != 3) {
      printf("FAIL rich: parsed %d, wanted 3\n", n);
      failures++;
    } else {
      if (!out[0].thumbnail || !out[0].date || !out[0].kind || !out[0].source) {
        printf("FAIL rich: first hit lost thumbnail/date/kind/source\n");
        failures++;
      }
      if (strcmp(out[0].kind, "videos") != 0) {
        printf("FAIL rich: kind \"%s\", wanted videos\n", out[0].kind);
        failures++;
      }
      /* absent stays absent rather than being filled with a guess */
      if (out[1].thumbnail || out[1].date || out[1].author || out[1].kind) {
        printf("FAIL rich: a bare hit grew fields\n");
        failures++;
      }
      /* a non-http thumbnail is dropped, not proxied */
      if (out[2].thumbnail) {
        printf("FAIL rich: javascript: thumbnail was kept\n");
        failures++;
      }
      printf("ok   rich fields: thumbnail/date/kind/source, absent stays "
             "absent, non-http refused\n");
    }
    xml_result_free(out, n);
  }

  printf("\n%s: J5.6 searxng bridge\n", failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
