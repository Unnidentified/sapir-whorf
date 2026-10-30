/* C10/F3: canonical URL merging. Paths are case sensitive; a page must not be
   silently merged with a different page, and Reddit hosts must collapse. */
#include "../src/Ranking/Ranker.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect(const char *url, const char *want) {
  char got[1024];
  rank_canonical_url(url, got, sizeof(got));
  if (strcmp(got, want) != 0) {
    printf("  FAIL %-58s -> %-34s want %s\n", url, got, want);
    failures++;
  } else {
    printf("  ok   %-58s -> %s\n", url, got);
  }
}

static void expect_differ(const char *a, const char *b) {
  char ca[1024], cb[1024];
  rank_canonical_url(a, ca, sizeof(ca));
  rank_canonical_url(b, cb, sizeof(cb));
  if (strcmp(ca, cb) == 0) {
    printf("  FAIL %s and %s collapsed to %s (must differ)\n", a, b, ca);
    failures++;
  } else {
    printf("  ok   %s != %s\n", a, b);
  }
}

int main(void) {
  printf("canonical urls\n");

  /* C10: host lowercased, path preserved */
  expect("https://Example.COM/Path/Case", "example.com/Path/Case");
  expect("https://en.wikipedia.org/wiki/CamelCase",
         "en.wikipedia.org/wiki/CamelCase");
  expect_differ("https://youtu.be/AbCdEf", "https://youtu.be/abcdef");
  expect_differ("https://example.com/A", "https://example.com/a");

  /* www / scheme / fragment / query */
  expect("http://www.example.com/x?utm=1#frag", "example.com/x");

  /* F3: reddit hosts collapse to one thread */
  expect("https://old.reddit.com/r/rust/comments/abc123/some_slug/",
         "reddit.com/r/rust/comments/abc123");
  expect("https://www.reddit.com/r/rust/comments/abc123/other_slug/def456/",
         "reddit.com/r/rust/comments/abc123");
  expect("https://m.reddit.com/r/rust/comments/abc123/x/",
         "reddit.com/r/rust/comments/abc123");
  expect_differ("https://reddit.com/r/rust/comments/abc123/x/",
                "https://reddit.com/r/rust/comments/zzz999/x/");

  /* C11: registrable domain, from static/public_suffixes.txt */
  printf("\nregistrable domains\n");
  struct { const char *url, *want; } domains[] = {
      {"https://example.co.uk/x", "example.co.uk"},
      {"https://news.bbc.co.uk/x", "bbc.co.uk"},
      {"https://example.org.uk/x", "example.org.uk"},
      {"https://user1.github.io/a", "user1.github.io"},
      {"https://user2.github.io/b", "user2.github.io"},
      {"https://site.pages.dev/x", "site.pages.dev"},
      {"https://foo.blogspot.com/x", "foo.blogspot.com"},
      {"https://en.wikipedia.org/wiki/X", "wikipedia.org"},
      {"https://a.b.example.com/x", "example.com"},
  };
  for (size_t i = 0; i < sizeof(domains) / sizeof(domains[0]); i++) {
    char got[256];
    rank_domain_of(domains[i].url, got, sizeof(got));
    if (strcmp(got, domains[i].want) != 0) {
      printf("  FAIL %-42s -> %-24s want %s\n", domains[i].url, got,
             domains[i].want);
      failures++;
    } else {
      printf("  ok   %-42s -> %s\n", domains[i].url, got);
    }
  }

  /* two different github.io sites must not collapse into one domain */
  {
    char a[256], b[256];
    rank_domain_of("https://alice.github.io/x", a, sizeof(a));
    rank_domain_of("https://bob.github.io/y", b, sizeof(b));
    if (strcmp(a, b) == 0) {
      printf("  FAIL alice.github.io and bob.github.io share a domain (%s)\n", a);
      failures++;
    } else {
      printf("  ok   alice.github.io != bob.github.io\n");
    }
  }

  printf("%s\n", failures == 0 ? "canonical urls: PASS" : "canonical urls: FAIL");
  return failures == 0 ? 0 : 1;
}
