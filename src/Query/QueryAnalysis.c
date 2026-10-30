#include "QueryAnalysis.h"
#include "../Utility/Utility.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *STOPWORDS[] = {
    "the", "and", "for", "with", "how", "what", "when", "where", "who",
    "why", "that", "this", "from", "into", "your", "you", "are", "was",
    "were", "has", "have", "had", "not", "but", "all", "can", "will",
    "its", "his", "her", "they", "them", "then", "than", "about", "over",
    "does", "did", "use", "using", "best", "latest"};

static int is_stopword(const char *word) {
  for (size_t i = 0; i < sizeof(STOPWORDS) / sizeof(STOPWORDS[0]); i++) {
    if (strcmp(word, STOPWORDS[i]) == 0)
      return 1;
  }
  return 0;
}

static void lower_copy(char *dst, size_t size, const char *src) {
  size_t i = 0;
  for (; src && src[i] && i + 1 < size; i++)
    dst[i] = (char)((src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i]);
  dst[i] = '\0';
}

static void collapse_spaces(char *text) {
  size_t out = 0;
  int pending = 0;
  for (size_t i = 0; text[i]; i++) {
    if (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
        text[i] == '\r') {
      pending = out > 0;
      continue;
    }
    if (pending)
      text[out++] = ' ';
    pending = 0;
    text[out++] = text[i];
  }
  text[out] = '\0';
}

static void append_word(char *out, size_t size, const char *word) {
  size_t len = strlen(out);
  if (len > 0 && len + 1 < size)
    out[len++] = ' ';
  snprintf(out + len, size - len, "%s", word);
}

typedef struct {
  const char *word;
  int points;
} IntentTerm;

static const IntentTerm CODE_TERMS[] = {
    {"error", 2},      {"exception", 2}, {"traceback", 3}, {"compile", 2},
    {"compiler", 2},   {"segfault", 3},  {"regex", 2},     {"api", 1},
    {"undefined", 2},  {"null", 1},      {"syntax", 2},    {"stackoverflow", 3},
    {"docker", 2},     {"kubernetes", 2}, {"kubectl", 3},  {"sql", 1},
    {"python", 1},     {"rust", 1},      {"javascript", 1}, {"typescript", 2},
    {"golang", 2},     {"cmake", 2},     {"asyncio", 3},   {"foreach", 2}};

static const IntentTerm ACADEMIC_TERMS[] = {
    {"paper", 3},     {"study", 2},   {"journal", 2},   {"doi", 3},
    {"arxiv", 3},     {"citation", 3}, {"survey", 2},   {"dataset", 2},
    {"conference", 2}, {"ieee", 3},   {"acm", 2},       {"preprint", 3},
    {"meta-analysis", 3}, {"et", 1},  {"al", 1}};

static const IntentTerm ENTITY_TERMS[] = {
    {"who", 2},      {"born", 2},     {"height", 2},   {"net", 1},
    {"worth", 1},    {"filmography", 3}, {"discography", 3}, {"biography", 2},
    {"is", 1}};

static const IntentTerm NEWS_TERMS[] = {
    {"latest", 2}, {"news", 2},  {"today", 2},   {"announced", 2},
    {"breaking", 3}, {"election", 2}, {"2025", 1}, {"2026", 1}};

static const IntentTerm MEDIA_TERMS[] = {
    {"video", 2}, {"trailer", 3}, {"song", 2}, {"lyrics", 3},
    {"episode", 2}, {"watch", 1}, {"stream", 2}};

static int table_score(char tokens[][32], int count, const IntentTerm *table,
                       size_t table_size) {
  int score = 0;
  for (int i = 0; i < count; i++) {
    for (size_t t = 0; t < table_size; t++) {
      if (strcmp(tokens[i], table[t].word) == 0)
        score += table[t].points;
    }
  }
  return score;
}

static QueryIntent detect_intent(const SearchQuery *query) {
  int code = table_score(query->terms, query->term_count, CODE_TERMS,
                         sizeof(CODE_TERMS) / sizeof(CODE_TERMS[0]));
  int academic = table_score(query->terms, query->term_count, ACADEMIC_TERMS,
                             sizeof(ACADEMIC_TERMS) / sizeof(ACADEMIC_TERMS[0]));
  int entity = table_score(query->terms, query->term_count, ENTITY_TERMS,
                           sizeof(ENTITY_TERMS) / sizeof(ENTITY_TERMS[0]));
  int news = table_score(query->terms, query->term_count, NEWS_TERMS,
                         sizeof(NEWS_TERMS) / sizeof(NEWS_TERMS[0]));
  int media = table_score(query->terms, query->term_count, MEDIA_TERMS,
                          sizeof(MEDIA_TERMS) / sizeof(MEDIA_TERMS[0]));

  /* code looking punctuation is a strong signal on its own */
  if (strstr(query->clean, "::") || strchr(query->clean, '(') ||
      strchr(query->clean, '_'))
    code += 2;

  if (news > 0 && news >= code && news >= academic && news >= entity &&
      news >= media && news >= 3)
    return QI_NEWS;
  if (code >= 2 && code >= academic && code >= entity && code >= media)
    return QI_CODE;
  if (academic >= 3 && academic >= entity && academic >= media)
    return QI_ACADEMIC;
  if (media >= 3 && media >= entity)
    return QI_MEDIA;
  if (entity >= 2)
    return QI_ENTITY;
  return QI_GENERAL;
}

static void detect_language(SearchQuery *query, const char *locale) {
  static const char *EN[] = {"the", "and", "how", "what", "best", "with",
                             "for", "my", "is", "to"};
  static const char *ES[] = {"el", "la", "de", "que", "como", "para",
                             "con", "los", "una", "del"};
  static const char *FR[] = {"le", "la", "de", "et", "pour", "avec",
                             "comment", "les", "une", "des"};
  static const char *DE[] = {"der", "die", "und", "was", "wie", "mit",
                             "für", "ein", "eine", "ist"};
  static const char *PT[] = {"o", "a", "de", "que", "como", "para",
                             "com", "os", "uma", "do"};
  static const char *IT[] = {"il", "la", "di", "che", "come", "per",
                             "con", "gli", "una", "del"};

  struct {
    const char *lang;
    const char *const *words;
    int count;
    int score;
  } votes[] = {
      {"en", EN, 10, 0}, {"es", ES, 10, 0}, {"fr", FR, 10, 0},
      {"de", DE, 10, 0}, {"pt", PT, 10, 0}, {"it", IT, 10, 0},
  };

  char tokens[32][32];
  int token_count = 0;
  char lower[512];
  lower_copy(lower, sizeof(lower), query->clean);

  char *saveptr = NULL;
  char *token = strtok_r(lower, " ", &saveptr);
  while (token && token_count < 32) {
    snprintf(tokens[token_count++], 32, "%s", token);
    token = strtok_r(NULL, " ", &saveptr);
  }

  for (size_t v = 0; v < sizeof(votes) / sizeof(votes[0]); v++) {
    for (int i = 0; i < token_count; i++) {
      for (int w = 0; w < votes[v].count; w++) {
        if (strcmp(tokens[i], votes[v].words[w]) == 0) {
          votes[v].score++;
          break;
        }
      }
    }
  }

  int best = -1;
  for (size_t v = 0; v < sizeof(votes) / sizeof(votes[0]); v++) {
    if (votes[v].score > best) {
      best = votes[v].score;
    }
  }

  if (best >= 2) {
    for (size_t v = 0; v < sizeof(votes) / sizeof(votes[0]); v++) {
      if (votes[v].score == best) {
        snprintf(query->lang, sizeof(query->lang), "%s", votes[v].lang);
        return;
      }
    }
  }

  char fallback_lang[8], region[8];
  locale_parts(locale, fallback_lang, sizeof(fallback_lang), region,
               sizeof(region));
  snprintf(query->lang, sizeof(query->lang), "%s",
           fallback_lang[0] ? fallback_lang : "en");
}

void query_analyze(const char *raw, const char *locale, SearchQuery *out) {
  memset(out, 0, sizeof(*out));
  if (!raw)
    raw = "";
  snprintf(out->raw, sizeof(out->raw), "%s", raw);

  char clean[512];
  clean[0] = '\0';

  const char *p = raw;
  while (*p) {
    while (*p == ' ' || *p == '\t' || *p == '\n')
      p++;
    if (!*p)
      break;

    const char *start = p;
    const char *end = p;
    int quoted = 0;
    if (*p == '"') {
      quoted = 1;
      start = ++p;
      while (*p && *p != '"')
        p++;
      end = p;
      if (*p == '"')
        p++;
    } else {
      while (*p && *p != ' ' && *p != '\t' && *p != '\n')
        p++;
      end = p;
    }

    size_t len = (size_t)(end - start);
    if (len == 0)
      continue;

    if (len > 5 && strncasecmp(start, "site:", 5) == 0) {
      snprintf(out->site, sizeof(out->site), "%.*s", (int)(len - 5),
               start + 5);
      out->has_operators = 1;
      continue;
    }
    if (len > 9 && strncasecmp(start, "filetype:", 9) == 0) {
      snprintf(out->filetype, sizeof(out->filetype), "%.*s", (int)(len - 9),
               start + 9);
      out->has_operators = 1;
      continue;
    }
    if (!quoted && len > 1 && start[0] == '-' && out->negative_count < 4) {
      char neg[32];
      snprintf(neg, sizeof(neg), "%.*s", (int)(len - 1), start + 1);
      lower_copy(out->negatives[out->negative_count++], sizeof(neg), neg);
      out->has_operators = 1;
      continue;
    }
    if (quoted && out->phrase_count < 2) {
      snprintf(out->phrases[out->phrase_count++], 96, "%.*s", (int)len, start);
      append_word(clean, sizeof(clean), out->phrases[out->phrase_count - 1]);
      continue;
    }

    char word[128];
    snprintf(word, sizeof(word), "%.*s", (int)len, start);
    append_word(clean, sizeof(clean), word);
  }

  collapse_spaces(clean);
  snprintf(out->clean, sizeof(out->clean), "%s", clean);

  char lower[512];
  lower_copy(lower, sizeof(lower), clean);
  char *saveptr = NULL;
  char *token = strtok_r(lower, " ", &saveptr);
  while (token && out->term_count < 8) {
    if (strlen(token) >= 3 && !is_stopword(token))
      snprintf(out->terms[out->term_count++], 32, "%s", token);
    token = strtok_r(NULL, " ", &saveptr);
  }

  out->is_question = strncasecmp(out->clean, "how ", 4) == 0 ||
                     strncasecmp(out->clean, "what ", 5) == 0 ||
                     strncasecmp(out->clean, "why ", 4) == 0 ||
                     strncasecmp(out->clean, "who ", 4) == 0 ||
                     strncasecmp(out->clean, "which ", 6) == 0 ||
                     strncasecmp(out->clean, "when ", 5) == 0 ||
                     strncasecmp(out->clean, "where ", 6) == 0;

  detect_language(out, locale);
  out->intent = detect_intent(out);
}

const char *query_intent_name(QueryIntent intent) {
  switch (intent) {
  case QI_CODE:
    return "code";
  case QI_ACADEMIC:
    return "academic";
  case QI_ENTITY:
    return "entity";
  case QI_NEWS:
    return "news";
  case QI_MEDIA:
    return "media";
  case QI_GENERAL:
    break;
  }
  return "general";
}

void query_engine_text(const SearchQuery *query, int supports_site,
                       int supports_negatives, char *out, size_t size) {
  out[0] = '\0';
  if (!query)
    return;

  snprintf(out, size, "%s", query->clean);

  if (supports_site && query->site[0]) {
    size_t used = strlen(out);
    snprintf(out + used, size - used, " site:%s", query->site);
  }
  if (query->filetype[0]) {
    size_t used = strlen(out);
    snprintf(out + used, size - used, " filetype:%s", query->filetype);
  }
  if (supports_negatives) {
    for (int i = 0; i < query->negative_count; i++) {
      size_t used = strlen(out);
      snprintf(out + used, size - used, " -%s", query->negatives[i]);
    }
  }
}
