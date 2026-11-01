/* K7 step 1: two engines returning the same URL, one with a placeholder and
   one with a real description. The real one must win in both orders, which is
   the bug: the merge preferred the LONGER snippet, and boilerplate is long. */

#include "../src/Query/Placeholders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the rule as implemented in Search.c, copied so it can be tested without the
   whole request path */
static void merge(char **have, const char *cleaned) {
  int take = 0;
  const char *cur = *have ? *have : "";
  if (cleaned[0] && !cur[0])
    take = 1;
  else if (cleaned[0] && cur[0] && strlen(cleaned) > strlen(cur))
    take = 1;
  if (take) {
    free(*have);
    *have = strdup(cleaned);
  }
}

/* what clean_snippet does to a placeholder */
static const char *clean(const char *in) {
  return snippet_is_placeholder(in) ? "" : in;
}

static int failures = 0;
static void check(const char *what, const char *got, const char *want) {
  if (strcmp(got ? got : "", want) != 0) {
    printf("FAIL: %s -> '%s', wanted '%s'\n", what, got ? got : "", want);
    failures++;
  }
}

int main(void) {
  const char *placeholder = "We would like to show you a description here but "
                            "the site wo...";
  const char *real = "Linux is a family of Unix-like systems.";

  /* placeholder first, then the real one */
  char *a = NULL;
  merge(&a, clean(placeholder));
  merge(&a, clean(real));
  check("placeholder then real", a, real);
  free(a);

  /* real first, then the placeholder */
  char *b = NULL;
  merge(&b, clean(real));
  merge(&b, clean(placeholder));
  check("real then placeholder", b, real);
  free(b);

  /* two real ones: the longer still wins */
  char *c = NULL;
  merge(&c, "short");
  merge(&c, "a much longer real description");
  check("longer real wins", c, "a much longer real description");
  free(c);

  printf("K7 merge: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
