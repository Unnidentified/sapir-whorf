/* J5.1: an engine is a data file.

   The claim being tested is not "a JSON file parses" - it is that the file
   actually produces results through the shared parser, and that a file which
   could only ever produce nothing is refused at load time. */

#include "../src/Scraping/Scraping.h"
#include "../src/Utility/XmlHelper.h"
#include <libxml/HTMLparser.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static void check(int ok, const char *what) {
  if (!ok) {
    printf("FAIL: %s\n", what);
    failures++;
  }
}

static void write_file(const char *dir, const char *name, const char *body) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "w");
  if (!f)
    return;
  fputs(body, f);
  fclose(f);
}

static const SearchEngine *find(const char *id) {
  for (int i = 0; i < ENGINE_COUNT; i++)
    if (strcmp(ENGINE_REGISTRY[i].id, id) == 0)
      return &ENGINE_REGISTRY[i];
  return NULL;
}

static const char *GOOD =
    "{\"id\":\"tdef\",\"name\":\"Test Definitions\","
    "\"search_url\":\"https://example.test/search?q=%s\","
    "\"index_group\":\"testindex\",\"verticals\":[\"academic\"],"
    "\"weight\":0.5,\"pace_per_minute\":7,"
    "\"xpath_result\":\"//li[@class='hit']\","
    "\"xpath_link\":\".//a[@class='t']\","
    "\"xpath_title\":\".//a[@class='t']\","
    "\"xpath_snippet\":\".//p[@class='s']\"}";

static const char *HTML =
    "<html><body><ul>"
    "<li class='hit'><a class='t' href='https://example.test/1'>First</a>"
    "<p class='s'>one</p></li>"
    "<li class='hit'><a class='t' href='https://example.test/2'>Second</a>"
    "<p class='s'>two</p></li>"
    "<li class='hit'><span>no link here</span></li>"
    "</ul></body></html>";

int main(void) {
  char dir[] = "/tmp/omnidefsXXXXXX";
  if (!mkdtemp(dir)) {
    printf("FAIL: cannot make a temp dir\n");
    return 1;
  }

  int before = ENGINE_COUNT;
  write_file(dir, "good.json", GOOD);
  /* every one of these is a real mistake someone will make in a definition */
  write_file(dir, "no-format.json",
             "{\"id\":\"tbad1\",\"name\":\"No Format\","
             "\"search_url\":\"https://example.test/search\","
             "\"xpath_result\":\"//li\",\"xpath_link\":\".//a\"}");
  write_file(dir, "no-link.json",
             "{\"id\":\"tbad2\",\"name\":\"No Link\","
             "\"search_url\":\"https://example.test/s?q=%s\","
             "\"xpath_result\":\"//li\"}");
  write_file(dir, "no-name.json",
             "{\"id\":\"tbad3\",\"search_url\":\"https://e.test/?q=%s\","
             "\"xpath_result\":\"//li\",\"xpath_link\":\".//a\"}");
  write_file(dir, "broken.json", "{ not json at all ");
  write_file(dir, "notjson.txt",
             "{\"id\":\"tbad4\",\"name\":\"Wrong Extension\","
             "\"search_url\":\"https://e.test/?q=%s\","
             "\"xpath_result\":\"//li\",\"xpath_link\":\".//a\"}");

  int loaded = engines_load_definitions(dir);
  printf("loaded %d of 1 valid definition\n", loaded);
  check(loaded == 1, "only the valid definition is accepted");
  check(ENGINE_COUNT == before + 1, "the registry grew by exactly one");

  const SearchEngine *engine = find("tdef");
  check(engine != NULL, "the engine is in the registry");
  if (engine) {
    check(strcmp(engine->name, "Test Definitions") == 0, "name is carried");
    check(strcmp(engine->index_group, "testindex") == 0, "index_group is carried");
    check(engine->verticals == V_ACADEMIC, "verticals are mapped");
    check(engine->weight == 0.5, "weight is carried");
    check(engine->pace_per_minute == 7, "pace is carried");
    check(engine->opt_in == 0, "opt_in is off unless the file says so");
    check(engine->parser != NULL, "the definition has a parser");
    /* off unless the file says on: see the enabled field's comment */
    check(engine->enabled == 0, "a definition starts disabled");
  }

  /* the point of the exercise: the file produces results */
  if (engine) {
    htmlDocPtr doc = htmlReadMemory(HTML, (int)strlen(HTML), NULL, NULL,
                                    HTML_PARSE_NOERROR | HTML_PARSE_NOWARNING);
    check(doc != NULL, "fixture parses as HTML");
    if (doc) {
      SearchResult *results = NULL;
      int n = engine->parser(engine->name, doc, &results, 10);
      printf("parser returned %d results\n", n);
      check(n == 2, "two of three blocks have a usable link");
      if (n == 2) {
        check(results[0].url && strcmp(results[0].url, "https://example.test/1") == 0,
              "first url comes from the href");
        check(results[0].title && strcmp(results[0].title, "First") == 0,
              "first title comes from the title xpath");
        check(results[0].snippet && strcmp(results[0].snippet, "one") == 0,
              "first snippet comes from the snippet xpath");
        check(results[1].url && strcmp(results[1].url, "https://example.test/2") == 0,
              "second url");
      }
      if (results)
        xml_result_free(results, n);
      xmlFreeDoc(doc);
    }
  }

  /* One file per directory, so the refusal is attributable to that file rather
     than to every other file in the directory also being refused. */
  char dir3[] = "/tmp/omnidefs3XXXXXX";
  if (mkdtemp(dir3)) {
    write_file(dir3, "dupe.json",
               "{\"id\":\"tdef\",\"name\":\"Another Name\","
               "\"search_url\":\"https://e.test/?q=%s\","
               "\"xpath_result\":\"//li\",\"xpath_link\":\".//a\"}");
    check(engines_load_definitions(dir3) == 0, "a duplicate id is refused");
  }

  /* the same display name: the shared parser looks engines up by name, so a
     second engine with this name would read the first one's selectors */
  char dir4[] = "/tmp/omnidefs4XXXXXX";
  if (mkdtemp(dir4)) {
    write_file(dir4, "dupe2.json",
               "{\"id\":\"tdef2\",\"name\":\"Test Definitions\","
               "\"search_url\":\"https://e.test/?q=%s\","
               "\"xpath_result\":\"//li\",\"xpath_link\":\".//a\"}");
    check(engines_load_definitions(dir4) == 0,
          "a duplicate display name is refused");
  }

  /* reloading the same directory is a no-op, not a second copy */
  check(engines_load_definitions(dir) == 0, "a reload adds nothing");
  check(find("tdef") != NULL, "the first definition still works after a reload");

  /* a missing directory is not an error, and loads nothing */
  check(engines_load_definitions("/tmp/definitely-not-here-omnidefs") == 0,
        "a missing directory loads nothing and does not crash");
  check(engines_load_definitions(NULL) == 0, "a NULL directory is not a crash");

  /* tier 3 stays opt-in only when the file asks for it */
  char dir2[] = "/tmp/omnidefs2XXXXXX";
  if (mkdtemp(dir2)) {
    write_file(dir2, "tier3.json",
               "{\"id\":\"ttier3\",\"name\":\"Tier Three Test\","
               "\"search_url\":\"https://e.test/?q=%s\","
               "\"xpath_result\":\"//li\",\"xpath_link\":\".//a\","
               "\"enabled\":true,\"opt_in\":true}");
    check(engines_load_definitions(dir2) == 1, "an opt-in definition loads");
    const SearchEngine *t3 = find("ttier3");
    check(t3 != NULL && t3->opt_in == 1, "opt_in is carried from the file");
    check(t3 != NULL && t3->enabled == 1, "a file can enable its own engine");
  }

  /* the registry is a heap array now: appending past its initial capacity has
     to grow it, and every definition has to survive the realloc */
  char dir5[] = "/tmp/omnidefs5XXXXXX";
  if (mkdtemp(dir5)) {
    int before_growth = ENGINE_COUNT;
    char body[512];
    for (int i = 0; i < 50; i++) {
      snprintf(body, sizeof(body),
               "{\"id\":\"grow%d\",\"name\":\"Grow %d\","
               "\"search_url\":\"https://e.test/?q=%%s&i=%d\","
               "\"xpath_result\":\"//li\",\"xpath_link\":\".//a\"}",
               i, i, i);
      char name[32];
      snprintf(name, sizeof(name), "g%d.json", i);
      write_file(dir5, name, body);
    }
    int grew = engines_load_definitions(dir5);
    printf("grown by %d of 50\n", grew);
    check(grew == 50, "50 definitions are appended");
    check(ENGINE_COUNT == before_growth + 50, "the registry grew by 50");
    int found_all = 1;
    for (int i = 0; i < 50; i++) {
      char id[32];
      snprintf(id, sizeof(id), "grow%d", i);
      const SearchEngine *e = find(id);
      if (!e || !e->search_url_fmt)
        found_all = 0;
    }
    check(found_all, "every grown definition survived the realloc");
    /* the first-seeded engines must still be there and still be themselves */
    const SearchEngine *first = find("ddg");
    check(first && first->name && strcmp(first->name, "DuckDuckGo") == 0,
          "the seeded engines survive the growth");
  }

  /* J5.5: weights from the config, with the refusals that matter */
  {
    const SearchEngine *bing = find("bing");
    double before = bing ? bing->weight : 0;
    check(apply_engine_weights("bing:2.5") == 1, "one override applies");
    check(find("bing")->weight == 2.5, "the weight is set");
    check(apply_engine_weights("bing:0") == 0, "a zero weight is refused");
    check(find("bing")->weight == 2.5, "a refused weight leaves it alone");
    check(apply_engine_weights("bing:9") == 0, "an absurd weight is refused");
    check(apply_engine_weights("nosuch:1.5") == 0, "an unknown id applies nothing");
    check(apply_engine_weights("garbage") == 0, "a malformed entry applies nothing");
    check(apply_engine_weights("bing:2.5") == 1, "re-applying is fine");
    check(apply_engine_weights("") == 0, "an empty spec applies nothing");
    check(apply_engine_weights(NULL) == 0, "NULL is not a crash");
    check(apply_engine_weights("bing:1.0,ddg:1.5") == 2,
          "a list applies every entry");
    check(find("ddg")->weight == 1.5, "the second entry applied too");
    for (int i = 0; i < ENGINE_COUNT; i++)
      if (bing && strcmp(ENGINE_REGISTRY[i].id, "bing") == 0)
        ENGINE_REGISTRY[i].weight = before;
  }

  printf("J5 engine definitions: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
