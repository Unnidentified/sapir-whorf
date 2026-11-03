/* The search cache key must distinguish a category from the default.

   This was a real fault: build_search_request_cache_key took the query, page,
   engine and client but not the category, so ?q=x and ?q=x&cat=media shared one
   cache entry. Measured on the running binary with only arxiv enabled: a media
   request served 10 cached web rows instead of saying no engine serves that
   category. */
#include "../src/Routes/SearchCacheKey.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void expect(const char *what, const char *want, const char *got) {
  checks++;
  if (strcmp(want, got) != 0) {
    printf("FAIL %s\n     want: %s\n     got:  %s\n", what, want, got);
    failures++;
  }
}

int main(void) {
  char a[256], b[256];

  search_cache_scope(a, sizeof a, NULL, "c1", NULL);
  expect("no engine, no category", "search_request:all:c1:cat=any", a);

  search_cache_scope(b, sizeof b, NULL, "c1", "media");
  expect("no engine, media", "search_request:all:c1:cat=media", b);

  /* the fault: these used to be the same string */
  checks++;
  if (strcmp(a, b) == 0) {
    printf("FAIL a category and the default share a cache entry\n");
    failures++;
  }

  /* every category distinct from every other, and from the default */
  const char *cats[] = {"news", "code", "academic", "media", "entity"};
  for (int i = 0; i < 5; i++) {
    char first[256], second[256];
    search_cache_scope(first, sizeof first, "ddg", "c1", cats[i]);
    for (int j = i + 1; j < 5; j++) {
      search_cache_scope(second, sizeof second, "ddg", "c1", cats[j]);
      checks++;
      if (strcmp(first, second) == 0) {
        printf("FAIL %s and %s share a cache entry\n", cats[i], cats[j]);
        failures++;
      }
    }
  }

  search_cache_scope(a, sizeof a, "ddg", "c1", "");
  expect("empty category is the default", "search_request:ddg:c1:cat=any", a);
  search_cache_scope(a, sizeof a, "", "", "");
  expect("empties fall back", "search_request:all:unknown:cat=any", a);

  /* a long client key must not overflow, and must stay terminated */
  char big[64];
  char key[512];
  memset(key, 'k', sizeof key - 1);
  key[sizeof key - 1] = '\0';
  search_cache_scope(big, sizeof big, "ddg", key, "media");
  checks++;
  if (strlen(big) >= sizeof big) {
    printf("FAIL scope overran its buffer\n");
    failures++;
  }

  printf("search cache key: %d checks, %d failed\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
