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

#include "AnswerBox.h"
#include "../Utility/HttpClient.h"
#include "../Utility/Utility.h"
#include <jansson.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#define TRANSLATE_BUDGET_MS 900
#define TRANSLATE_ENDPOINT                                                  \
  "https://api.mymemory.translated.net/get?q=%.900s&langpair=%.5s%%7C%.5s"

/* The names a person types, not the ISO codes alone: "translate hello to
   spanish" is the common shape, and es/西班牙语 are not. */
static const struct {
  const char *name;
  const char *code;
} LANGUAGES[] = {
    {"english", "en"},  {"inglés", "en"},   {"ingles", "en"},
    {"spanish", "es"},  {"español", "es"},  {"espanol", "es"},
    {"french", "fr"},   {"francés", "fr"},  {"frances", "fr"},
    {"german", "de"},   {"alemán", "de"},   {"aleman", "de"},
    {"italian", "it"},  {"italiano", "it"}, {"portuguese", "pt"},
    {"portugués", "pt"},{"portugues", "pt"},{"dutch", "nl"},
    {"neerlandés", "nl"},{"russian", "ru"}, {"ruso", "ru"},
    {"japanese", "ja"}, {"japonés", "ja"},  {"chinese", "zh"},
    {"chino", "zh"},    {"arabic", "ar"},   {"árabe", "ar"},
    {"swedish", "sv"},  {"sueco", "sv"},    {"polish", "pl"},
    {"polaco", "pl"},   {"turkish", "tr"},  {"turco", "tr"},
    {"greek", "el"},    {"griego", "el"},   {"catalan", "ca"},
    {"catalán", "ca"},  {"ukrainian", "uk"},{"danish", "da"},
    {"finnish", "fi"},  {"norwegian", "no"},{"romanian", "ro"},
    {"czech", "cs"},    {"korean", "ko"},   {"hindi", "hi"},
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

/* returns 1 when the widget applies (ok says whether it translated) */
int translate_widget(const char *text, const char *target, const char *source,
                     WidgetData *out, const char *lang) {
  if (!out || !text || !text[0] || !target || !target[0])
    return 0;
  memset(out, 0, sizeof(*out));
  snprintf(out->kind, sizeof(out->kind), "translate");
  snprintf(out->title, sizeof(out->title), "%s", "Translation");
  snprintf(out->expression, sizeof(out->expression), "%s", text);

  const char *from = (source && source[0]) ? source : "en";
  /* translating into the language you typed in is not a request anyone makes
     by accident; say so rather than echoing the input back as a translation */
  if (strcmp(from, target) == 0) {
    snprintf(out->note, sizeof(out->note), "Already in %s",
             language_name(target));
    return 1;
  }

  char encoded[1024];
  url_encode(text, encoded, sizeof(encoded));
  char url[1400];
  snprintf(url, sizeof(url), TRANSLATE_ENDPOINT, encoded, from, target);

  HttpResponse resp = http_get_budget(url, "omnisearch/1.0 (translation)", 
                                      TRANSLATE_BUDGET_MS);
  if (!resp.memory || resp.size == 0) {
    http_response_free(&resp);
    /* an honest failure: the card exists, the value does not */
    snprintf(out->note, sizeof(out->note), "Provider did not answer");
    return 1;
  }

  json_error_t error;
  json_t *root = json_loadb(resp.memory, resp.size, 0, &error);
  http_response_free(&resp);
  if (!root) {
    snprintf(out->note, sizeof(out->note), "Provider sent unreadable JSON");
    return 1;
  }

  json_t *status = json_object_get(root, "responseStatus");
  json_t *data = json_object_get(root, "responseData");
  const char *translated =
      data ? json_string_value(json_object_get(data, "translatedText")) : NULL;

  if (!json_is_integer(status) || json_integer_value(status) != 200 ||
      !translated || !translated[0]) {
    snprintf(out->note, sizeof(out->note), "No translation for %s",
             language_name(target));
    json_decref(root);
    return 1;
  }

  snprintf(out->result, sizeof(out->result), "%.200s", translated);
  out->ok = 1;
  snprintf(out->note, sizeof(out->note), "%.16s → %.16s · MyMemory",
           language_name(from), language_name(target));
  json_decref(root);
  (void)lang;
  return 1;
}
