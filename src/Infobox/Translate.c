/* J4b: translation as a widget.

   The provider is a fixed, configured endpoint - never a host from the query -
   because the widget runs on the request path and a user-supplied URL there is
   an SSRF with a friendly name. The budget is the same class as the answer
   panels: if the provider does not answer in time the card says so instead of
   showing an empty translation.

   Measured before building: MyMemory answers "hello world" en|es in 706 ms
   with HTTP 200 and {"translatedText":"Hola Mundo","match":1}. An invalid
   target language comes back 403 with the error text inside translatedText,
   which is why the status is checked and not just the text. See
   docs/probe-log.tsv. */

#include "Translate.h"
#include "AnswerBox.h"
#include "../Utility/HttpClient.h"
#include "../Utility/Utility.h"
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#define TRANSLATE_BUDGET_MS 1200
#define TRANSLATE_ENDPOINT                                                  \
  "https://api.mymemory.translated.net/get?q=%.900s&langpair=%.12s%%7C%.12s"

/* The names a person types, not the ISO codes alone: "translate hello to
   spanish" is the common shape, and es/西班牙语 are not. */
static const struct {
  const char *name;
  const char *code;
} LANGUAGES[] = {
    {"English", "en"},  {"Inglés", "en"},   {"Ingles", "en"},
    {"Spanish", "es"},  {"Español", "es"},  {"Espanol", "es"},
    {"French", "fr"},   {"Francés", "fr"},  {"Frances", "fr"},
    {"German", "de"},   {"Alemán", "de"},   {"Aleman", "de"},
    {"Italian", "it"},  {"Italiano", "it"}, {"Portuguese", "pt"},
    {"Portugués", "pt"},{"Portugues", "pt"},{"Dutch", "nl"},
    {"Neerlandés", "nl"},{"Russian", "ru"}, {"Ruso", "ru"},
    {"Japanese", "ja"}, {"Japonés", "ja"},  {"Chinese", "zh"},
    {"Chino", "zh"},    {"Arabic", "ar"},   {"Árabe", "ar"},
    {"Swedish", "sv"},  {"Sueco", "sv"},    {"Polish", "pl"},
    {"Polaco", "pl"},   {"Turkish", "tr"},  {"Turco", "tr"},
    {"Greek", "el"},    {"Griego", "el"},   {"Catalan", "ca"},
    {"Catalán", "ca"},  {"Ukrainian", "uk"},{"Danish", "da"},
    {"Finnish", "fi"},  {"Norwegian", "no"},{"Romanian", "ro"},
    {"Czech", "cs"},    {"Korean", "ko"},   {"Hindi", "hi"},
};

static const char *language_code(const char *word) {
  if (!word || !word[0])
    return NULL;
  for (size_t i = 0; i < sizeof(LANGUAGES) / sizeof(LANGUAGES[0]); i++)
    if (strcasecmp(word, LANGUAGES[i].name) == 0)
      return LANGUAGES[i].code;
  /* a bare two-letter code is accepted when it is one we know */
  if (strlen(word) == 2)
    for (size_t i = 0; i < sizeof(LANGUAGES) / sizeof(LANGUAGES[0]); i++)
      if (strncasecmp(word, LANGUAGES[i].code, 2) == 0)
        return LANGUAGES[i].code;
  return NULL;
}

static const char *language_name(const char *code) {
  for (size_t i = 0; i < sizeof(LANGUAGES) / sizeof(LANGUAGES[0]); i++)
    if (strcmp(code, LANGUAGES[i].code) == 0)
      return LANGUAGES[i].name;
  return code;
}

static int is_translate_word(const char *word) {
  return strcasecmp(word, "translate") == 0 || strcasecmp(word, "traduce") == 0 ||
         strcasecmp(word, "traducir") == 0;
}

/* K10: the shapes a person actually types. Each rule is narrow on purpose:
   a wrong translation answers a question no one asked, which is worse than a
   search that does not show a card. */
int translate_parse_request(const char *query, const char *default_target,
                            TranslateRequest *out) {
  if (!query || !out)
    return 0;
  memset(out, 0, sizeof(*out));

  char copy[512];
  snprintf(copy, sizeof(copy), "%s", query);
  char *start = copy;
  while (*start == ' ')
    start++;

  /* "how do you say <text> in <language>" */
  if (strncasecmp(start, "how do you say ", 15) == 0) {
    char *text = start + 15;
    char *in = NULL;
    for (char *scan = text; *scan; scan++)
      if (strncasecmp(scan, " in ", 4) == 0)
        in = scan;
    if (in) {
      const char *code = language_code(in + 4);
      if (code) {
        *in = '\0';
        snprintf(out->text, sizeof(out->text), "%s", text);
        snprintf(out->to, sizeof(out->to), "%s", code);
        return out->to[0] != '\0';
      }
    }
  }

  /* <language> to <language>: the empty box, menus pre-set */
  {
    char first[32];
    char second[32];
    if (sscanf(start, "%31s to %31s", first, second) == 2) {
      const char *from = language_code(first);
      const char *to = language_code(second);
      if (from && to && strlen(start) <= strlen(first) + strlen(second) + 4) {
        snprintf(out->from, sizeof(out->from), "%s", from);
        snprintf(out->to, sizeof(out->to), "%s", to);
        return 1;
      }
    }
  }

  /* the verb forms, which keep working as they did */
  {
    char text[512];
    char target[8];
    if (translate_parse(query, default_target, text, sizeof(text), target,
                        sizeof(target))) {
      snprintf(out->text, sizeof(out->text), "%s", text);
      snprintf(out->to, sizeof(out->to), "%s", target);
      return 1;
    }
  }

  /* the bare verb: an empty box in the reader's language */
  if (is_translate_word(start)) {
    if (default_target && default_target[0] &&
        strcmp(default_target, "en") != 0) {
      snprintf(out->to, sizeof(out->to), "%s", default_target);
      return 1;
    }
    return 0;
  }

  /* "<text> in <language>", only when it cannot be something else: the target
     must be a language we know, the text must be short, and a digit anywhere
     makes it a query ("1 in binary") rather than a phrase */
  {
    char *in = NULL;
    for (char *scan = start; *scan; scan++)
      if (strncasecmp(scan, " in ", 4) == 0)
        in = scan;
    if (in && in > start) {
      const char *code = language_code(in + 4);
      if (code) {
        int words = 0;
        int has_digit = 0;
        for (char *w = start; w < in; w++) {
          if (*w == ' ')
            words++;
          if (*w >= '0' && *w <= '9')
            has_digit = 1;
        }
        words++;
        if (!has_digit && words <= 6) {
          *in = '\0';
          snprintf(out->text, sizeof(out->text), "%s", start);
          snprintf(out->to, sizeof(out->to), "%s", code);
          return 1;
        }
      }
    }
  }

  return 0;
}

/* Split "translate <text> to <language>" / "traduce <text> al <language>".
   Returns 1 when the query is a translate request and fills text+target.
   The verb is required: "<something> in spanish" is a web query far more often
   than it is a translation request, and guessing would answer the wrong one. */
int translate_parse(const char *query, const char *default_target, char *text,
                    size_t text_size, char *target, size_t target_size) {
  if (!query || !text || !target)
    return 0;
  text[0] = '\0';
  target[0] = '\0';

  char copy[512];
  snprintf(copy, sizeof(copy), "%s", query);
  /* trim */
  char *start = copy;
  while (*start == ' ')
    start++;
  size_t len = strlen(start);
  while (len > 0 && start[len - 1] == ' ')
    start[--len] = '\0';

  char *first_end = strchr(start, ' ');
  if (!first_end)
    return 0;
  *first_end = '\0';
  if (!is_translate_word(start))
    return 0;

  char *rest = first_end + 1;
  while (*rest == ' ')
    rest++;
  if (!rest[0])
    return 0;

  /* the last " to <word>" / " al <word>" wins: the text itself may contain
     "to" ("translate to be or not to be to spanish") */
  char *marker = NULL;
  for (char *scan = rest; *scan; scan++) {
    if (scan > rest && scan[-1] != ' ')
      continue;
    if (strncasecmp(scan, "to ", 3) == 0 || strncasecmp(scan, "al ", 3) == 0 ||
        strncasecmp(scan, "into ", 5) == 0)
      marker = scan;
  }

  if (marker) {
    size_t prefix = (strncasecmp(marker, "into ", 5) == 0) ? 5 : 3;
    char *lang_word = marker + prefix;
    while (*lang_word == ' ')
      lang_word++;
    /* only a known language ends a sentence here; otherwise the word belongs
       to the text and there is no target */
    const char *code = language_code(lang_word);
    if (code) {
      *marker = '\0';
      snprintf(target, target_size, "%s", code);
    }
  }
  if (!target[0] && default_target && default_target[0] &&
      strcmp(default_target, "en") != 0) {
    /* no target named: translate into the reader's language, which is the only
       sensible default and the one the caller passed in */
    snprintf(target, target_size, "%s", default_target);
  }
  if (!target[0])
    return 0;

  snprintf(text, text_size, "%s", rest);
  /* trailing spaces from the removed marker */
  size_t used = strlen(text);
  while (used > 0 && text[used - 1] == ' ')
    text[--used] = '\0';
  return text[0] != '\0';
}

/* K10: the strings MyMemory puts INSIDE translatedText when it is not
   translating. A status-200 reply carrying one of these must not be shown as a
   translation. */
static int looks_like_error_text(const char *text) {
  static const char *const MARKERS[] = {
      "MYMEMORY WARNING", "QUOTA", "INVALID LANGUAGE",
      "PLEASE SELECT TWO DISTINCT", "QUERY LENGTH LIMIT", "NO QUERY SPECIFIED",
      "PLEASE SPECIFY A QUERY", "AUTODETECT FAILED"};
  if (!text)
    return 1;
  for (size_t i = 0; i < sizeof(MARKERS) / sizeof(MARKERS[0]); i++)
    if (strcasestr(text, MARKERS[i]))
      return 1;
  return 0;
}

/* K10: the provider echoing the input with something appended ("bonjour tout
   le monde" -> "bonjour tout le monde !!!") is the same failure as an exact
   echo, and the exact comparison missed it. Measured with the live card. */
static int echoes_input(const char *translated, const char *input) {
  if (!translated || !input)
    return 0;
  char a[512], b[512];
  size_t an = 0, bn = 0;
  for (const char *p = translated; *p && an + 1 < sizeof(a); p++)
    if (isalnum((unsigned char)*p) || *p == ' ')
      a[an++] = (char)tolower((unsigned char)*p);
  a[an] = '\0';
  for (const char *p = input; *p && bn + 1 < sizeof(b); p++)
    if (isalnum((unsigned char)*p) || *p == ' ')
      b[bn++] = (char)tolower((unsigned char)*p);
  b[bn] = '\0';
  if (bn < 4)
    return 0;
  return strstr(a, b) != NULL;
}

/* equal after trimming and folding case: the provider echoing the input is
   the Lingva symptom, not a translation */
static int same_text(const char *a, const char *b) {
  if (!a || !b)
    return 0;
  while (*a == ' ')
    a++;
  while (*b == ' ')
    b++;
  size_t la = strlen(a);
  size_t lb = strlen(b);
  while (la > 0 && a[la - 1] == ' ')
    la--;
  while (lb > 0 && b[lb - 1] == ' ')
    lb--;
  if (la != lb)
    return 0;
  for (size_t i = 0; i < la; i++)
    if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
      return 0;
  return la > 0;
}

/* One attempt against MyMemory. Returns 1 when the reply is a usable
   translation; `note` says why when it is not (the caller decides whether to
   try the next rung or show it). */
static int translate_once(const char *text, const char *from, const char *to,
                          char *result, size_t result_size, char *detected,
                          size_t detected_size, const char **note) {
  *note = "No translation";
  char encoded[1024];
  url_encode(text, encoded, sizeof(encoded));
  char url[1400];
  snprintf(url, sizeof(url), TRANSLATE_ENDPOINT, encoded, from, to);

  HttpResponse resp = http_get_budget(url, "omnisearch/1.0 (translation)",
                                      TRANSLATE_BUDGET_MS);
  if (!resp.memory || resp.size == 0) {
    http_response_free(&resp);
    *note = "Provider did not answer";
    return 0;
  }

  json_error_t error;
  json_t *root = json_loadb(resp.memory, resp.size, 0, &error);
  http_response_free(&resp);
  if (!root) {
    *note = "Provider sent unreadable JSON";
    return 0;
  }

  json_t *status = json_object_get(root, "responseStatus");
  json_t *data = json_object_get(root, "responseData");
  const char *translated =
      data ? json_string_value(json_object_get(data, "translatedText")) : NULL;

  if (!json_is_integer(status) || json_integer_value(status) != 200 ||
      !translated || !translated[0] || looks_like_error_text(translated) ||
      same_text(translated, text) || echoes_input(translated, text)) {
    /* K10: the replies that are not translations. Measured: an invalid target
       comes back 403 with the error INSIDE translatedText; a spent quota comes
       back 200 with a warning there; and the community memory sometimes holds
       an entry that translates "hello" to "testvalue" with a perfect match
       score, so the score cannot be the only guard. */
    *note = "No translation";
    json_decref(root);
    return 0;
  }

  snprintf(result, result_size, "%.200s", translated);
  const char *det = data ? json_string_value(json_object_get(data,
                                                             "detectedLanguage"))
                         : NULL;
  snprintf(detected, detected_size, "%s", det && det[0] ? det : from);
  json_decref(root);
  return 1;
}

/* returns 1 when the widget applies (ok says whether it translated) */
int translate_widget(const char *text, const char *target, const char *source,
                     WidgetData *out, const char *lang) {
  if (!out || !text || !target || !target[0])
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->kind, sizeof(out->kind), "translate");
  snprintf(out->title, sizeof(out->title), "%s", "Translation");
  snprintf(out->expression, sizeof(out->expression), "%s", text);

  /* an empty box (bare "translate", or "<language> to <language>") is a card
     that asks for the text; it must not contact anything */
  if (!text[0]) {
    snprintf(out->note, sizeof(out->note), "%s → %s",
             language_name(source && source[0] ? source : "en"),
             language_name(target));
    snprintf(out->translate_codes, sizeof(out->translate_codes), "%.6s|%.6s",
             source && source[0] ? source : "", target);
    return 1;
  }

  const char *from = (source && source[0]) ? source : "Autodetect";
  /* translating into the language you typed in is not a request anyone makes
     by accident; say so rather than echoing the input back as a translation */
  if (strcasecmp(from, target) == 0) {
    snprintf(out->note, sizeof(out->note), "Already in %s",
             language_name(target));
    return 1;
  }

  char result[256] = "";
  char detected[16] = "";
  const char *note = "No translation";
  int ok = translate_once(text, from, target, result, sizeof(result), detected,
                          sizeof(detected), &note);

  /* rung 2: the same request with detection. Measured: "good morning" en|de
     came back with an empty translatedText while Autodetect|de answered, so
     the pair the user typed is an attempt, not a promise. */
  if (!ok && strcmp(from, "Autodetect") != 0) {
    const char *retry_note = "No translation";
    if (translate_once(text, "Autodetect", target, result, sizeof(result),
                       detected, sizeof(detected), &retry_note)) {
      ok = 1;
      note = retry_note;
    } else if (strcmp(note, "Provider did not answer") == 0 &&
               strcmp(retry_note, "Provider did not answer") != 0) {
      note = retry_note;
    }
  }

  if (!ok) {
    snprintf(out->note, sizeof(out->note), "%s for %s", note,
             language_name(target));
    snprintf(out->translate_codes, sizeof(out->translate_codes), "%.6s|%.6s",
             source && source[0] ? source : "", target);
    return 1;
  }

  snprintf(out->result, sizeof(out->result), "%.200s", result);
  out->ok = 1;
  {
    const char *shown = detected[0] ? detected : from;
    if (strcmp(shown, "Autodetect") == 0)
      shown = "en";
    if (strcmp(from, "Autodetect") == 0 || strcmp(shown, from) != 0)
      snprintf(out->note, sizeof(out->note), "%.16s → %.16s · %s · MyMemory",
               language_name(shown), language_name(target), "source detected");
    else
      snprintf(out->note, sizeof(out->note), "%.16s → %.16s · MyMemory",
               language_name(shown), language_name(target));
    snprintf(out->translate_codes, sizeof(out->translate_codes), "%.6s|%.6s",
             shown, target);
  }
  (void)lang;
  return 1;
}

const char *translate_language_code(const char *word) {
  return language_code(word);
}

int translate_widget_explicit(const char *text, const char *from,
                              const char *to, WidgetData *out,
                              const char *lang) {
  const char *code = language_code(to);
  if (!code)
    return 0;
  const char *src = NULL;
  if (from && from[0]) {
    src = language_code(from);
    if (!src)
      return 0; /* a code we do not know is not a source language */
  }
  return translate_widget(text, code, src, out, lang);
}

/* returns 1 when the widget applies (ok says whether it translated) */
