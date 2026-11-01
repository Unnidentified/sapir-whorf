/* K7 step 1: the placeholder table and the merge rule, with no network. */

#include "../src/Query/Placeholders.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(const char *text, int want) {
  int got = snippet_is_placeholder(text);
  if (got != want) {
    printf("FAIL: '%s' -> %d, wanted %d\n", text, got, want);
    failures++;
  }
}

int main(void) {
  /* the exact string this project's own Yahoo fixture carries, truncated */
  expect("We would like to show you a description here but the site wo...", 1);
  expect("We would like to show you a description here but the site won't allow us.", 1);
  expect("we would like to SHOW YOU a description here", 1);
  expect("A description for this result is not available because of this "
         "site's robots.txt", 1);
  expect("No information is available for this page.", 1);
  /* real descriptions must survive */
  expect("Linux is a family of open-source Unix-like operating systems.", 0);
  expect("A page that merely quotes the sentence is not a placeholder: "
         "the phrase appears in the middle", 0);
  expect("", 0);
  expect(NULL, 0);
  /* the prefix test must not fire on a different opening */
  expect("We would like to say thank you for your help", 0);

  printf("K7 placeholders: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
