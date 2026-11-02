/* M4.1: the cx comes out of the id, the public URL, or the embed code. */
#include "../src/Scraping/CseId.h"
#include <stdio.h>
#include <string.h>

static int failures;

static void expect(const char *what, const char *input, const char *want) {
  char out[128];
  int rc = cse_extract_id(input, out, sizeof(out));
  if (want == NULL) {
    if (rc == 0) {
      printf("  FAIL %-22s accepted '%.40s' as '%s'\n", what, input, out);
      failures++;
    }
    return;
  }
  if (rc != 0 || strcmp(out, want) != 0) {
    printf("  FAIL %-22s got '%s' want '%s'\n", what, rc == 0 ? out : "(none)",
           want);
    failures++;
  }
}

int main(void) {
  /* the three things the Overview page offers */
  expect("bare id", "a25d329ae5f514a70", "a25d329ae5f514a70");
  expect("public URL",
         "https://cse.google.com/cse?cx=a25d329ae5f514a70", "a25d329ae5f514a70");
  expect("embed code",
         "<script async src=\"https://cse.google.com/cse.js?cx=a25d329ae5f514a70\"></script>",
         "a25d329ae5f514a70");
  expect("URL with extra params",
         "https://cse.google.com/cse?cx=a25d329ae5f514a70&ie=UTF-8",
         "a25d329ae5f514a70");
  expect("whitespace around", "   a25d329ae5f514a70  ", "a25d329ae5f514a70");
  expect("id with a colon", "0123456789012345:abc", "0123456789012345:abc");
  /* junk is refused, not stored */
  expect("empty", "", NULL);
  expect("no id", "https://example.com/nothing-here", NULL);
  expect("too short", "abc", NULL);
  expect("a sentence", "I could not find my id anywhere", NULL);

  if (failures == 0)
    printf("M4 cse id: PASS\n");
  return failures ? 1 : 0;
}
