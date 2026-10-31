/* J4b: the translate request parser. No network - this is the part that decides
   whether a query is a translation at all, and getting it wrong means either
   answering a search with a translation or a translation with a search. */

#include "../src/Infobox/Translate.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_default(const char *query, const char *reader_lang,
                           int want_ok, const char *want_text,
                           const char *want_target) {
  char text[256] = {0};
  char target[8] = {0};
  int ok = translate_parse(query, reader_lang, text, sizeof(text), target,
                           sizeof(target));
  int bad = 0;
  if (ok != want_ok)
    bad = 1;
  if (want_ok && strcmp(text, want_text) != 0)
    bad = 1;
  if (want_ok && strcmp(target, want_target) != 0)
    bad = 1;
  if (bad) {
    printf("FAIL: '%s' (reader %s) -> ok=%d text='%s' target='%s' (wanted "
           "ok=%d '%s' '%s')\n",
           query, reader_lang, ok, text, target, want_ok, want_text, want_target);
    failures++;
  }
}

/* the common case: an English reader */
static void expect(const char *query, int want_ok, const char *want_text,
                   const char *want_target) {
  expect_default(query, "en", want_ok, want_text, want_target);
}

int main(void) {
  expect("translate hello world to spanish", 1, "hello world", "es");
  expect("Translate Hello World To Spanish", 1, "Hello World", "es");
  expect("translate good morning into german", 1, "good morning", "de");
  expect("traduce buenos dias al frances", 1, "buenos dias", "fr");
  expect("traducir hola a italiano", 0, "", "");   /* "a " is not a marker */
  expect("translate hola al italiano", 1, "hola", "it");
  /* the text itself contains "to": the target is the last marker, and only if
     it names a language */
  expect("translate to be or not to be to spanish", 1, "to be or not to be", "es");
  /* a bare two-letter code that we know */
  expect("translate hello world to fr", 1, "hello world", "fr");
  /* not a translate request at all */
  expect("hello world in spanish", 0, "", "");
  expect("translation services", 0, "", "");
  expect("translate", 0, "", "");
  expect("", 0, "", "");
  /* no target named, and the reader's language is the default: a Spanish
     reader gets the card, an English one does not, because it would be a
     no-op */
  expect_default("translate hello world", "de", 1, "hello world", "de");
  expect_default("translate hello world", "es", 1, "hello world", "es");
  /* ...unless the reader's language is English, which would be a no-op */
  {
    char text[256] = {0};
    char target[8] = {0};
    int ok = translate_parse("translate hello world", "en", text, sizeof(text),
                             target, sizeof(target));
    if (ok != 0) {
      printf("FAIL: an untargeted request defaulting to English should not "
             "become a card (ok=%d target='%s')\n", ok, target);
      failures++;
    }
  }
  /* a language the table does not know is not a target, so the phrase stays
     part of the text */
  expect("translate hello world to klingon", 0, "", "");

  printf("J4b translate parser: %s\n", failures == 0 ? "PASS" : "FAIL");
  return failures == 0 ? 0 : 1;
}
