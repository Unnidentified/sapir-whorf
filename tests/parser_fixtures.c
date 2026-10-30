/* A1: parser fixture tests.
   Each fixture pins a layout a parser depends on. A silent upstream redesign
   (or a bad refactor) then fails here instead of reaching a user. */
#include "../src/Scraping/Scraping.h"
#include "../src/Utility/XmlHelper.h"
#include "../src/Scraping/BlockDetection.h"
#include <libxml/HTMLparser.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern SearchEngine ENGINE_REGISTRY[];
extern const int ENGINE_COUNT;

static int failures = 0;

static void fail(const char *engine, const char *case_name, const char *why) {
  printf("  FAIL %-8s %-14s %s\n", engine, case_name, why);
  failures++;
}

static int parse_file(const SearchEngine *e, const char *path,
                      SearchResult **out) {
  htmlDocPtr doc = htmlReadFile(path, NULL,
                                HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                    HTML_PARSE_NOWARNING | HTML_PARSE_NONET);
  if (!doc)
    return -1;
  int n = e->parser(e->name, doc, out, 10);
  xmlFreeDoc(doc);
  return n;
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = malloc((size_t)n + 1);
  if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf);
    buf = NULL;
  }
  if (buf)
    buf[n] = '\0';
  fclose(f);
  return buf;
}

static int file_exists(const char *p) {
  FILE *f = fopen(p, "rb");
  if (f)
    fclose(f);
  return f != NULL;
}

int main(void) {
  const char *fixtures = "tests/fixtures";
  char path[512];

  struct {
    const char *engine;
    const char *file;
    int min_results;
    int expect_blocked; /* the blocked-page detector must fire */
  } cases[] = {
      {"yahoo", "good.html", 3, 0},
      {"bing", "empty-shell.html", 0, 0},
      {"ddg", "challenge.html", 0, 1},
      {"brave", "challenge.html", 0, 1},
      {"ddg", "good.html", 3, 0},
      {"brave", "good.html", 3, 0},
      {"bing", "good.html", 3, 0},
  };

  printf("parser fixtures\n");

  for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
    snprintf(path, sizeof(path), "%s/%s/%s", fixtures, cases[c].engine,
             cases[c].file);
    if (!file_exists(path)) {
      printf("  skip %-8s %-14s (no fixture yet)\n", cases[c].engine,
             cases[c].file);
      continue;
    }

    const SearchEngine *e = NULL;
    for (int i = 0; i < ENGINE_COUNT; i++) {
      if (strcmp(ENGINE_REGISTRY[i].id, cases[c].engine) == 0) {
        e = &ENGINE_REGISTRY[i];
        break;
      }
    }
    if (!e) {
      fail(cases[c].engine, cases[c].file, "engine missing from registry");
      continue;
    }
    if (!e->parser) {
      fail(cases[c].engine, cases[c].file, "no HTML parser");
      continue;
    }

    /* C9: the blocked detector must fire on challenge pages and NOT on real
       results pages whose snippets merely mention the marker words. */
    if (cases[c].expect_blocked) {
      char *body = read_file(path);
      int blocked = body ? response_is_blocked_page(body) : 0;
      free(body);
      if (!blocked)
        fail(cases[c].engine, cases[c].file, "blocked detector did not fire");
      else
        printf("  ok   %-8s %-14s detected as blocked\n", cases[c].engine,
               cases[c].file);
    }

    SearchResult *results = NULL;
    int n = parse_file(e, path, &results);

    if (n < cases[c].min_results) {
      char why[128];
      snprintf(why, sizeof(why), "parsed %d, expected >= %d", n,
               cases[c].min_results);
      fail(cases[c].engine, cases[c].file, why);
    } else {
      if (!cases[c].expect_blocked) {
        char *body2 = read_file(path);
        int blocked = body2 ? response_is_blocked_page(body2) : 0;
        free(body2);
        if (blocked)
          fail(cases[c].engine, cases[c].file,
               "blocked detector fired on a good page");
        else
          printf("  ok   %-8s %-14s %d results, not blocked\n",
                 cases[c].engine, cases[c].file, n);
      }
    }

    /* invariants that must hold for every parsed result */
    for (int i = 0; i < n; i++) {
      if ((!results[i].title || !results[i].title[0]) &&
          (!results[i].url || !results[i].url[0])) {
        fail(cases[c].engine, cases[c].file, "result with no title and no url");
        break;
      }
      if (results[i].url && results[i].url[0] &&
          strncmp(results[i].url, "http://", 7) != 0 &&
          strncmp(results[i].url, "https://", 8) != 0) {
        fail(cases[c].engine, cases[c].file, "result with non-http url");
        break;
      }
    }

    if (results)
      xml_result_free(results, n);
  }

  printf("%s\n", failures == 0 ? "parser fixtures: PASS" : "parser fixtures: FAIL");
  return failures == 0 ? 0 : 1;
}
