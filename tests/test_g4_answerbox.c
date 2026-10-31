/* G4: the answer box, table-driven. Every row is a query and the answerer that
   must handle it. The local-only rows run offline and are the ones that matter
   most: a local answer must never lose to a network call. */
#include "../src/Infobox/AnswerBox.h"
#include "../src/Infobox/AnswerLocal.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

static int failures = 0;

typedef struct {
  const char *query;
  const char *want_source;
  const char *want_contains; /* substring of answer+detail, NULL = don't care */
  int want_local;            /* expected local flag */
} Case;

static int run_case(const Case *c) {
  AnswerBoxResult r;
  int got = answer_box_resolve(c->query, &r);
  if (!got) {
    printf("FAIL %-28s resolved nothing (wanted %s)\n", c->query,
           c->want_source);
    return 1;
  }
  if (strcmp(r.source, c->want_source) != 0) {
    printf("FAIL %-28s source=%s want=%s\n", c->query, r.source, c->want_source);
    return 1;
  }
  if (r.local != c->want_local) {
    printf("FAIL %-28s local=%d want=%d\n", c->query, r.local, c->want_local);
    return 1;
  }
  if (c->want_contains) {
    char both[1100];
    snprintf(both, sizeof both, "%s %s", r.answer, r.detail);
    if (!strstr(both, c->want_contains)) {
      printf("FAIL %-28s answer/detail lacks \"%s\" (got \"%s\")\n", c->query,
             c->want_contains, both);
      return 1;
    }
  }
  return 0;
}

int main(void) {
  /* table order: local answerers must all precede network ones */
  size_t n = 0;
  const Answerer *table = answer_box_answerers(&n);
  int seen_network = 0;
  for (size_t i = 0; i < n; i++) {
    if (!table[i].local)
      seen_network = 1;
    else if (seen_network) {
      printf("FAIL: local answerer \"%s\" ordered after a network one\n",
             table[i].id);
      failures++;
    }
  }

  static const Case cases[] = {
      /* arithmetic: local, no network */
      {"2+2", "arithmetic", "4", 1},
      {"12 * 12", "arithmetic", "144", 1},
      {"2^10", "arithmetic", "1024", 1},
      {"7-3", "arithmetic", "4", 1},
      {"100 / 4", "arithmetic", "25", 1},
      {"3.5 * 2", "arithmetic", "7", 1},
      {"(2+3)*4", "arithmetic", "20", 1},
      {"2 ^ 3 ^ 2", "arithmetic", "512", 1},
      {"-5 + 8", "arithmetic", "3", 1},
      {"0.1 + 0.2", "arithmetic", "0.3", 1},
      {"1 + 1 + 1", "arithmetic", "3", 1},
      /* units: local */
      {"10 km to miles", "units", "6.21 miles", 1},
      {"100 f to c", "units", "37.78", 1},
      {"1 kg to lb", "units", NULL, 1},
      {"60 mph to kmh", "units", NULL, 1},
      {"5 feet in cm", "units", NULL, 1},
      {"2 litres in gallons", "units", NULL, 1},
      {"30 c to f", "units", NULL, 1},
      {"12 inches to cm", "units", NULL, 1},
      {"1 mile in km", "units", NULL, 1},
      {"512 mb to gb", "units", NULL, 1},
      /* colour: local */
      {"#ff0000", "colour", "#ff0000", 1},
      {"rgb(0, 128, 255)", "colour", "#0080FF", 1},
      {"#16a34a", "colour", "#16a34a", 1},
      {"#000", "colour", "#000", 1},
      {"#FFFFFF", "colour", "#FFFFFF", 1},
      {"rgb(255, 255, 255)", "colour", "#FFFFFF", 1},
      {"rgba(1, 2, 3, 0.5)", "colour", "#010203", 1},
      {"#abc", "colour", "#abc", 1},
      {"  #123456", "colour", "#123456", 1},
      {"rgb(1,2,3)", "colour", "#010203", 1},
  };

  /* Negatives: one table, every kind. A local answerer that fires on these is
     worse than one that misses - it is confidently wrong. The review named
     four of these (H4). */
  static const char *negatives[] = {
      /* arithmetic look-alikes */
      "2 broke girls", "python 3.12 vs 3.11", "10 downing street",
      "covid-19", "area 51", "24 season 1", "windows 10",
      "iphone 15 pro max", "1 direction", "3 body problem",
      /* unit words without a conversion */
      "convert to judaism", "convert pdf to word", "miles davis",
      "pounds sterling", "football", "celsius band", "kgb", "litre of tears",
      "inches away", "ounce of prevention",
      /* colour words that are not colours */
      "rgb gaming", "red dead redemption", "blue whale", "hashing",
      "sharp tv", "black friday", "white house", "green day", "grey's anatomy",
      "colour theory",
      /* spanish: the same traps */
      "2 chicas sin blanca", "convertir a pdf", "10 de mayo",
      "kilogramo de café", "pies de foto", "millas a ninguna parte",
      "rojo pasión", "verde que te quiero verde", "azul celeste",
      "negro como la noche",
  };
  for (size_t i = 0; i < sizeof(negatives) / sizeof(negatives[0]); i++) {
    AnswerBoxResult neg;
    if (answer_box_resolve_local(negatives[i], &neg)) {
      printf("FAIL negative %-24s answered locally by %s (\"%s\")\n",
             negatives[i], neg.source, neg.answer);
      failures++;
    }
  }
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    failures += run_case(&cases[i]);

  /* a plain word must not be answered locally at all: the local-only path is
     what the page calls, and it must never reach the network */
  AnswerBoxResult r;
  if (answer_box_resolve_local("linux kernel", &r)) {
    printf("FAIL: plain query answered locally by %s\n", r.source);
    failures++;
  }
  /* the deep resolver may use the network, but a local query must not */
  if (answer_box_resolve("linux kernel", &r) && r.local) {
    printf("FAIL: deep resolve answered a plain query locally via %s\n",
           r.source);
    failures++;
  }
  /* the local path answers every local case without touching the network */
  AnswerBoxResult loc;
  if (!answer_box_resolve_local("2+2", &loc) || !loc.local) {
    printf("FAIL: local resolve did not answer 2+2\n");
    failures++;
  }
  if (!answer_box_resolve_local("10 km to miles", &loc) || !loc.local) {
    printf("FAIL: local resolve did not answer a unit query\n");
    failures++;
  }

  /* H4: time / dates / random. These answers move, so the assertions are on
     the source and the shape, not on the value. */
  {
    static const struct {
      const char *query;
      const char *source;
    } dynamic[] = {
        /* time: ten */
        {"time in tokyo", "time"},
        {"what time is it in new york", "time"},
        {"que hora es en madrid", "time"},
        {"qué hora es en madrid", "time"},
        {"current time in lisbon", "time"},
        {"time in sydney", "time"},
        {"time in chicago", "time"},
        {"time in sao paulo", "time"},
        {"time in hong kong", "time"},
        {"time in buenos aires", "time"},
        /* days: ten (date maths) */
        {"days between 2024-01-01 and 2024-03-01", "days"},
        {"how many days until 2025-12-25", "days"},
        {"days between 2024-02-01 and 2024-03-01", "days"},
        {"days between 2025-01-01 and 2025-12-31", "days"},
        {"how many days until 2026-01-01", "days"},
        {"days between 2023-06-15 and 2023-06-20", "days"},
        {"days between 2024-03-01 and 2024-01-01", "days"},
        {"how many days until 2027-06-30", "days"},
        {"days between 2020-02-29 and 2020-03-01", "days"},
        {"days between 1999-12-31 and 2000-01-01", "days"},
        /* random: ten */
        {"flip a coin", "random"},
        {"roll a dice", "random"},
        {"random number between 1 and 10", "random"},
        {"uuid", "random"},
        {"roll a d20", "random"},
        {"roll a die", "random"},
        {"lanzar una moneda", "random"},
        {"random number between 5 and 5", "random"},
        {"random number between 100 and 1", "random"},
        {"guid", "random"},
    };
    for (size_t i = 0; i < sizeof(dynamic) / sizeof(dynamic[0]); i++) {
      AnswerBoxResult d;
      if (!answer_box_resolve_local(dynamic[i].query, &d)) {
        printf("FAIL dynamic %-38s did not answer\n", dynamic[i].query);
        failures++;
        continue;
      }
      if (strcmp(d.source, dynamic[i].source) != 0) {
        printf("FAIL dynamic %-38s source=%s want=%s\n", dynamic[i].query,
               d.source, dynamic[i].source);
        failures++;
      }
      if (!d.answer[0]) {
        printf("FAIL dynamic %-38s empty answer\n", dynamic[i].query);
        failures++;
      }
      if (strcmp(dynamic[i].source, "time") == 0 &&
          !(isdigit((unsigned char)d.answer[0]) &&
            isdigit((unsigned char)d.answer[1]) && d.answer[2] == ':')) {
        printf("FAIL dynamic %-38s time answer \"%s\" is not HH:MM\n",
               dynamic[i].query, d.answer);
        failures++;
      }
    }

    /* and the negatives for these kinds, in both languages */
    static const char *dyn_neg[] = {
        "time magazine", "time after time", "tempo", "dicey",
        "random access memory", "uuid v4 format spec",
        "days of our lives", "between the lines", "cities in spain",
        "hora de comer", "time zone map", "time complexity of quicksort",
        "days inn madrid", "how many days in a leap year",
        "time series analysis", "time travel movies", "roll of honour",
        "random forest classifier", "coin cell battery", "dice game rules",
        "uuid primary key postgres", "guid vs uuid", "tokyo drift",
        "new york times", "madrid weather",
    };
    for (size_t i = 0; i < sizeof(dyn_neg) / sizeof(dyn_neg[0]); i++) {
      AnswerBoxResult d;
      if (answer_box_resolve_local(dyn_neg[i], &d)) {
        printf("FAIL negative %-28s answered by %s (\"%s\")\n", dyn_neg[i],
               d.source, d.answer);
        failures++;
      }
    }
  }

  /* H4 step 2b: locale numbers and Spanish aliases. Same answerers, different
     notation. */
  {
    static const struct {
      const char *query;
      const char *lang;
      const char *source;
      const char *contains;
    } localised[] = {
        {"1.234,56 + 2", "es", "arithmetic", "1236.56"},
        {"1,5 + 1", "es", "arithmetic", "2.5"},
        {"10,25 * 2", "es", "arithmetic", "20.5"},
        {"1.234 + 1", "es", "arithmetic", NULL}, /* ambiguous: leave alone */
        {"1,234.56 + 2", "en", "arithmetic", "1236.56"},
        {"10 millas a km", "es", "units", "16.09"},
        {"1,5 km a millas", "es", "units", "0.93"},
        {"100 grados f a c", "es", "units", "37.78"},
        {"2 litros a galones", "es", "units", NULL},
        {"5 pies a pulgadas", "es", "units", NULL},
        {"30 kilos a libras", "es", "units", NULL},
        {"10 pulgadas a centimetros", "es", "units", NULL},
    };
    for (size_t i = 0; i < sizeof(localised) / sizeof(localised[0]); i++) {
      AnswerBoxResult r;
      if (!answer_box_resolve_local_lang(localised[i].query, localised[i].lang,
                                         &r)) {
        printf("FAIL localised %-26s did not answer\n", localised[i].query);
        failures++;
        continue;
      }
      if (strcmp(r.source, localised[i].source) != 0) {
        printf("FAIL localised %-26s source=%s want=%s\n", localised[i].query,
               r.source, localised[i].source);
        failures++;
      }
      if (localised[i].contains && !strstr(r.answer, localised[i].contains)) {
        printf("FAIL localised %-26s answer \"%s\" lacks %s\n",
               localised[i].query, r.answer, localised[i].contains);
        failures++;
      }
    }

    /* and the locale guard: the same text must NOT be reordered in English */
    AnswerBoxResult en;
    if (answer_box_resolve_local_lang("1.234,56 + 2", "en", &en) &&
        strstr(en.answer, "1236")) {
      printf("FAIL: comma-decimal text normalised under en\n");
      failures++;
    }
    /* the normaliser itself, directly */
    char out[64];
    if (!answer_normalise_locale_numbers("1.234,56", "es", out, sizeof out) ||
        strcmp(out, "1234.56") != 0) {
      printf("FAIL: normalise 1.234,56 -> \"%s\"\n", out);
      failures++;
    }
    answer_normalise_locale_numbers("1.234,56", "en", out, sizeof out);
    if (strcmp(out, "1.234,56") != 0) {
      printf("FAIL: en must be left alone, got \"%s\"\n", out);
      failures++;
    }
    answer_normalise_locale_numbers("1.234.567", "es", out, sizeof out);
    if (strcmp(out, "1234567") != 0) {
      printf("FAIL: thousands grouping got \"%s\"\n", out);
      failures++;
    }
  }

  if (failures == 0)
    printf("G4 answer box: PASS\n");
  return failures ? 1 : 0;
}
