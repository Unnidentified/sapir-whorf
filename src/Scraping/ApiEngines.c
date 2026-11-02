#include "ApiEngines.h"
#include <pthread.h>
#include "../Utility/HttpClient.h"
#include "../Utility/JsonHelper.h"
#include "../Utility/Unescape.h"
#include "Config.h"
#include <jansson.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>

#define API_PAGE_SIZE 10

static void html_unescape_inplace(char *text) {
  if (!text)
    return;
  struct {
    const char *entity;
    char replacement;
  } map[] = {{"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''},
             {"&lt;", '<'},   {"&gt;", '>'},   {"&amp;", '&'}};

  for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
    char *pos;
    while ((pos = strstr(text, map[i].entity)) != NULL) {
      *pos = map[i].replacement;
      memmove(pos + 1, pos + strlen(map[i].entity),
              strlen(pos + strlen(map[i].entity)) + 1);
    }
  }
}

static char *dup_string(const json_t *object, const char *key) {
  json_t *value = json_object_get(object, key);
  if (!json_is_string(value))
    return NULL;
  return strdup(json_string_value(value));
}

static char *url_encode_terms(const char *text) {
  if (!text)
    return NULL;
  size_t len = strlen(text);
  char *out = malloc(len * 3 + 1);
  if (!out)
    return NULL;
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    char c = text[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
      out[o++] = c;
    } else if (c == ' ') {
      out[o++] = '+';
    } else {
      snprintf(out + o, 4, "%%%02X", (unsigned char)c);
      o += 3;
    }
  }
  out[o] = '\0';
  return out;
}

static void add_result(SearchResult *slots, int *count, int max,
                       const char *url, const char *title,
                       const char *snippet) {
  if (!slots || *count >= max || !url || !url[0])
    return;
  if (assign_result(&slots[*count], (char *)url, (char *)title,
                    (char *)snippet, 0))
    (*count)++;
}

/* ---- URL builders ------------------------------------------------ */

char *api_url_openalex(const char *encoded_query, int page, const char *lang) {
  (void)lang;
  char *url = malloc(1024);
  if (!url)
    return NULL;
  snprintf(url, 1024,
           "https://api.openalex.org/works?search=%s&per-page=%d&page=%d"
           "&select=id,title,display_name,doi,publication_year,"
           "cited_by_count,primary_location",
           encoded_query, API_PAGE_SIZE, page);
  return url;
}

char *api_url_crossref(const char *encoded_query, int page, const char *lang) {
  (void)lang;
  char *url = malloc(1024);
  if (!url)
    return NULL;
  snprintf(url, 1024,
           "https://api.crossref.org/works?query=%s&rows=%d&offset=%d"
           "&select=DOI,title,container-title,issued,author",
           encoded_query, API_PAGE_SIZE, (page - 1) * API_PAGE_SIZE);
  return url;
}

char *api_url_stackexchange(const char *encoded_query, int page,
                            const char *lang) {
  char *url = malloc(1024);
  if (!url)
    return NULL;
  snprintf(url, 1024,
           "https://api.stackexchange.com/2.3/search/advanced?order=desc"
           "&sort=relevance&q=%s&site=stackoverflow&pagesize=%d&page=%d"
           "&filter=default",
           encoded_query, API_PAGE_SIZE, page);
  (void)lang;
  return url;
}

char *api_url_hn(const char *encoded_query, int page, const char *lang) {
  (void)lang;
  char *url = malloc(1024);
  if (!url)
    return NULL;
  snprintf(url, 1024,
           "https://hn.algolia.com/api/v1/search?query=%s&tags=story"
           "&hitsPerPage=%d&page=%d",
           encoded_query, API_PAGE_SIZE, page - 1);
  return url;
}

char *api_url_wikipedia(const char *encoded_query, int page, const char *lang) {
  const char *wiki_lang = (lang && lang[0]) ? lang : "en";
  char *url = malloc(1024);
  if (!url)
    return NULL;
  if (page > 1) {
    /* the search API has no paging cursor in this simple form */
    url[0] = '\0';
    return url;
  }
  snprintf(url, 1024,
           "https://%s.wikipedia.org/w/api.php?action=query&generator=search"
           "&gsrnamespace=0&gsrlimit=5&gsrsearch=%s&prop=extracts|pageimages"
           "&exintro=1&explaintext=1&exsentences=2&pithumbsize=200"
           "&format=json&formatversion=2&redirects=1",
           wiki_lang, encoded_query);
  return url;
}

char *api_url_arxiv(const char *encoded_query, int page, const char *lang) {
  (void)lang;
  char *url = malloc(1024);
  if (!url)
    return NULL;
  snprintf(url, 1024,
           "https://export.arxiv.org/api/query?search_query=all:%s"
           "&start=%d&max_results=%d",
           encoded_query, (page - 1) * API_PAGE_SIZE, API_PAGE_SIZE);
  return url;
}

/* ---- parsers ----------------------------------------------------- */

int api_parse_openalex(const char *body, size_t len, const char *lang,
                       SearchResult **out, int max) {
  (void)lang;
  json_error_t error;
  json_t *root = json_loadb(body, len, 0, &error);
  if (!root)
    return 0;

  json_t *results = json_object_get(root, "results");
  int count = 0;
  if (json_is_array(results)) {
    *out = alloc_results_array((int)json_array_size(results), max);
    for (size_t i = 0; i < json_array_size(results) && count < max; i++) {
      json_t *item = json_array_get(results, i);
      char *title = dup_string(item, "display_name");
      if (!title)
        title = dup_string(item, "title");
      char *doi = dup_string(item, "doi");
      char *url = NULL;
      if (doi && strncmp(doi, "https://", 8) == 0) {
        url = strdup(doi);
      } else {
        json_t *location = json_object_get(item, "primary_location");
        if (json_is_object(location)) {
          json_t *landing = json_object_get(location, "landing_page_url");
          if (json_is_string(landing))
            url = strdup(json_string_value(landing));
        }
      }
      if (!url)
        url = dup_string(item, "id");

      char snippet[256] = "OpenAlex";
      json_t *year = json_object_get(item, "publication_year");
      json_t *cites = json_object_get(item, "cited_by_count");
      char extra[160] = "";
      if (json_is_integer(year))
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 " · %lld", (long long)json_integer_value(year));
      if (json_is_integer(cites))
        snprintf(extra + strlen(extra), sizeof(extra) - strlen(extra),
                 " · %lld citations", (long long)json_integer_value(cites));
      snprintf(snippet, sizeof(snippet), "OpenAlex%s", extra);
      add_result(*out, &count, max, url, title, snippet);
      free(title);
      free(doi);
      free(url);
    }
  }
  json_decref(root);
  return count;
}

int api_parse_crossref(const char *body, size_t len, const char *lang,
                       SearchResult **out, int max) {
  (void)lang;
  json_error_t error;
  json_t *root = json_loadb(body, len, 0, &error);
  if (!root)
    return 0;

  json_t *message = json_object_get(root, "message");
  json_t *items = message ? json_object_get(message, "items") : NULL;
  int count = 0;
  if (json_is_array(items)) {
    *out = alloc_results_array((int)json_array_size(items), max);
    for (size_t i = 0; i < json_array_size(items) && count < max; i++) {
      json_t *item = json_array_get(items, i);
      json_t *title_array = json_object_get(item, "title");
      const char *title = json_is_array(title_array) && json_array_size(title_array)
                              ? json_string_value(json_array_get(title_array, 0))
                              : NULL;
      char *doi = dup_string(item, "DOI");
      char *url = NULL;
      if (doi) {
        url = malloc(strlen(doi) + 32);
        snprintf(url, strlen(doi) + 32, "https://doi.org/%s", doi);
      }
      char snippet[256] = "Crossref";
      json_t *container = json_object_get(item, "container-title");
      if (json_is_array(container) && json_array_size(container)) {
        const char *journal = json_string_value(json_array_get(container, 0));
        if (journal)
          snprintf(snippet, sizeof(snippet), "%s · Crossref", journal);
      }
      add_result(*out, &count, max, url, title, snippet);
      free(doi);
      free(url);
    }
  }
  json_decref(root);
  return count;
}

int api_parse_stackexchange(const char *body, size_t len, const char *lang,
                            SearchResult **out, int max) {
  (void)lang;
  json_error_t error;
  json_t *root = json_loadb(body, len, 0, &error);
  if (!root)
    return 0;

  json_t *items = json_object_get(root, "items");
  int count = 0;
  if (json_is_array(items)) {
    *out = alloc_results_array((int)json_array_size(items), max);
    for (size_t i = 0; i < json_array_size(items) && count < max; i++) {
      json_t *item = json_array_get(items, i);
      char *title = dup_string(item, "title");
      char *url = dup_string(item, "link");
      if (title)
        html_unescape_inplace(title);
      char snippet[160];
      json_t *answers = json_object_get(item, "answer_count");
      json_t *score = json_object_get(item, "score");
      snprintf(snippet, sizeof(snippet), "Stack Overflow · %lld answers · score %lld",
               json_is_integer(answers) ? (long long)json_integer_value(answers) : 0LL,
               json_is_integer(score) ? (long long)json_integer_value(score) : 0LL);
      add_result(*out, &count, max, url, title, snippet);
      free(title);
      free(url);
    }
  }
  json_decref(root);
  return count;
}

int api_parse_hn(const char *body, size_t len, const char *lang,
                 SearchResult **out, int max) {
  (void)lang;
  json_error_t error;
  json_t *root = json_loadb(body, len, 0, &error);
  if (!root)
    return 0;

  json_t *hits = json_object_get(root, "hits");
  int count = 0;
  if (json_is_array(hits)) {
    *out = alloc_results_array((int)json_array_size(hits), max);
    for (size_t i = 0; i < json_array_size(hits) && count < max; i++) {
      json_t *item = json_array_get(hits, i);
      char *title = dup_string(item, "title");
      char *url = dup_string(item, "url");
      if (!url) {
        json_t *id = json_object_get(item, "objectID");
        if (json_is_string(id)) {
          url = malloc(64);
          snprintf(url, 64, "https://news.ycombinator.com/item?id=%s",
                   json_string_value(id));
        }
      }
      char snippet[160];
      json_t *points = json_object_get(item, "points");
      json_t *comments = json_object_get(item, "num_comments");
      snprintf(snippet, sizeof(snippet), "Hacker News · %lld points · %lld comments",
               json_is_integer(points) ? (long long)json_integer_value(points) : 0LL,
               json_is_integer(comments) ? (long long)json_integer_value(comments) : 0LL);
      add_result(*out, &count, max, url, title, snippet);
      free(title);
      free(url);
    }
  }
  json_decref(root);
  return count;
}

int api_parse_wikipedia(const char *body, size_t len, const char *lang,
                        SearchResult **out, int max) {
  json_error_t error;
  json_t *root = json_loadb(body, len, 0, &error);
  if (!root)
    return 0;

  json_t *query = json_object_get(root, "query");
  json_t *pages = query ? json_object_get(query, "pages") : NULL;
  int count = 0;
  if (json_is_array(pages)) {
    *out = alloc_results_array((int)json_array_size(pages), max);
    const char *wiki_lang = (lang && lang[0]) ? lang : "en";
    for (size_t i = 0; i < json_array_size(pages) && count < max; i++) {
      json_t *page = json_array_get(pages, i);
      char *title = dup_string(page, "title");
      char *extract = dup_string(page, "extract");
      char *url = NULL;
      if (title) {
        url = malloc(strlen(title) + strlen(wiki_lang) + 40);
        char *slug = strdup(title);
        for (char *p = slug; p && *p; p++)
          if (*p == ' ')
            *p = '_';
        snprintf(url, strlen(title) + strlen(wiki_lang) + 40,
                 "https://%s.wikipedia.org/wiki/%s", wiki_lang, slug);
        free(slug);
      }
      add_result(*out, &count, max, url, title, extract);
      free(title);
      free(extract);
      free(url);
    }
  }
  json_decref(root);
  return count;
}

int api_parse_arxiv(const char *body, size_t len, const char *lang,
                    SearchResult **out, int max) {
  (void)lang;
  xmlDocPtr doc = xmlReadMemory(body, (int)len, NULL, NULL,
                                XML_PARSE_RECOVER | XML_PARSE_NOERROR |
                                    XML_PARSE_NOWARNING);
  if (!doc)
    return 0;

  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  xmlXPathObjectPtr entries =
      xmlXPathEvalExpression((const xmlChar *)"//entry", ctx);
  int count = 0;
  if (entries && entries->nodesetval && entries->nodesetval->nodeNr > 0) {
    int total = entries->nodesetval->nodeNr;
    *out = alloc_results_array(total, max);
    for (int i = 0; i < total && count < max; i++) {
      ctx->node = entries->nodesetval->nodeTab[i];
      xmlXPathObjectPtr title_obj =
          xmlXPathEvalExpression((const xmlChar *)"./title", ctx);
      xmlXPathObjectPtr id_obj =
          xmlXPathEvalExpression((const xmlChar *)"./id", ctx);
      xmlXPathObjectPtr summary_obj =
          xmlXPathEvalExpression((const xmlChar *)"./summary", ctx);
      char *title = title_obj && title_obj->nodesetval &&
                            title_obj->nodesetval->nodeNr > 0
                        ? xmlNodeGetContent(title_obj->nodesetval->nodeTab[0])
                        : NULL;
      char *id = id_obj && id_obj->nodesetval && id_obj->nodesetval->nodeNr > 0
                     ? xmlNodeGetContent(id_obj->nodesetval->nodeTab[0])
                     : NULL;
      char *summary = summary_obj && summary_obj->nodesetval &&
                              summary_obj->nodesetval->nodeNr > 0
                          ? xmlNodeGetContent(summary_obj->nodesetval->nodeTab[0])
                          : NULL;
      if (title) {
        /* arXiv titles are wrapped over several lines */
        char *p = title;
        while (*p) {
          if (*p == '\n' || *p == '\r')
            *p = ' ';
          p++;
        }
      }
      add_result(*out, &count, max, id, title, summary);
      xmlFree(title);
      xmlFree(id);
      xmlFree(summary);
      xmlXPathFreeObject(title_obj);
      xmlXPathFreeObject(id_obj);
      xmlXPathFreeObject(summary_obj);
    }
  }
  ctx->node = NULL;
  xmlXPathFreeObject(entries);
  xmlXPathFreeContext(ctx);
  xmlFreeDoc(doc);
  return count;
}

/* ---- Google Programmable Search Element ----------------------------
   Clean-room, implemented from the protocol description in the review (H3 /
   Part 4 G3) and confirmed by probe on 2026-10-31; nothing is copied from
   SearXNG (AGPL) or translated from it.

   Two steps:
     1. GET www.google.com/cse/cse.js?cx=<CX>   (301 -> cse.google.com/...)
        The trailing `({ ... })` object carries cse_token, cselibVersion and
        exp. Cached for an hour, fetched under a lock so a burst of searches
        does not each mint one.
     2. GET cse.google.com/cse/element/v1?...&cse_tok=<token>&callback=_
        which answers JSONP: `_({ ... "results": [...] })`.

   A pasted token (google_cse_tok) overrides step 1 - useful when the token
   endpoint changes, and the only mode that worked before this fix. */

#define CSE_TOKEN_TTL_SECS 3600
#define CSE_TOKEN_URL_FMT                                                      \
  "https://www.google.com/cse/cse.js?cx=%s"

static char cse_element_cx[128];
static char cse_element_tok[512];   /* optional override */
static char cse_cached_tok[512];
static char cse_cached_lib[64];
static char cse_cached_exp[128];
static time_t cse_token_fetched = 0;
static time_t cse_token_failed = 0;
static pthread_mutex_t cse_token_mutex = PTHREAD_MUTEX_INITIALIZER;

void api_google_cse_element_configure(const char *cx, const char *tok) {
  pthread_mutex_lock(&cse_token_mutex);
  snprintf(cse_element_cx, sizeof(cse_element_cx), "%s", cx ? cx : "");
  snprintf(cse_element_tok, sizeof(cse_element_tok), "%s", tok ? tok : "");
  cse_token_fetched = 0;
  cse_token_failed = 0;
  pthread_mutex_unlock(&cse_token_mutex);
}

int api_google_cse_element_available(void) {
  return cse_element_cx[0] != '\0';
}

/* pull "key": "value" out of the JS object at the end of the token page */
static int json_field_string(const char *body, const char *key, char *out,
                             size_t size) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(body, needle);
  if (!p)
    return 0;
  p += strlen(needle);
  while (*p == ' ' || *p == '\t' || *p == ':')
    p++;
  if (*p != '"')
    return 0;
  p++;
  size_t n = 0;
  while (*p && *p != '"' && n + 1 < size) {
    if (*p == '\\' && p[1]) {
      p++;
      out[n++] = *p++;
    } else {
      out[n++] = *p++;
    }
  }
  out[n] = '\0';
  return n > 0;
}

/* exp is a JSON array of strings; flatten to "a,b,c" */
static void json_field_array(const char *body, const char *key, char *out,
                             size_t size) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\":", key);
  const char *p = strstr(body, needle);
  if (!p) {
    if (size)
      out[0] = '\0';
    return;
  }
  p += strlen(needle);
  const char *open = strchr(p, '[');
  const char *close = open ? strchr(open, ']') : NULL;
  if (!open || !close || close < open) {
    if (size)
      out[0] = '\0';
    return;
  }
  size_t n = 0;
  for (const char *q = open; q < close && n + 1 < size; q++) {
    if (*q == '"')
      continue;
    if (*q == ',' && n > 0 && out[n - 1] == ',')
      continue;
    if (*q == ' ' || *q == '\n')
      continue;
    if (*q == '[')
      continue;
    if (*q == '"' && n == 0)
      continue;
    out[n++] = *q;
  }
  /* trim leading/trailing commas */
  while (n > 0 && (out[n - 1] == ',' || out[n - 1] == ' '))
    n--;
  out[n] = '\0';
}

/* Fetch the token if the cache is cold. Returns 1 on success. Called under the
   mutex, so concurrent searches wait for one fetch instead of all racing. */
static int cse_fetch_token_locked(void) {
  char token_url[512];
  snprintf(token_url, sizeof(token_url), CSE_TOKEN_URL_FMT, cse_element_cx);
  HttpResponse resp = http_get(
      token_url,
      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
      "(KHTML, like Gecko) Version/17.2 Safari/605.1.15");
  if (!resp.memory || resp.size == 0) {
    http_response_free(&resp);
    return 0;
  }
  int ok = json_field_string(resp.memory, "cse_token", cse_cached_tok,
                             sizeof(cse_cached_tok)) &&
           json_field_string(resp.memory, "cselibVersion", cse_cached_lib,
                             sizeof(cse_cached_lib));
  json_field_array(resp.memory, "exp", cse_cached_exp, sizeof(cse_cached_exp));
  http_response_free(&resp);
  return ok;
}

/* Test hook: run the token-page extraction over a captured response, without
   any network. Returns 1 when cse_token and cselibVersion were both found. */
int api_cse_extract_token_for_test(const char *body, size_t len) {
  (void)len;
  char tok[512], lib[64], exp[128];
  int ok = body && json_field_string(body, "cse_token", tok, sizeof(tok)) &&
           json_field_string(body, "cselibVersion", lib, sizeof(lib));
  if (ok && body) {
    json_field_array(body, "exp", exp, sizeof(exp));
    if (!exp[0])
      ok = 0;
  }
  return ok;
}

/* 1 when a token is ready (from the override, the cache, or a fresh fetch) */
static int cse_ensure_token(void) {
  if (cse_element_tok[0]) {
    /* an explicit override wins and needs no fetch */
    snprintf(cse_cached_tok, sizeof(cse_cached_tok), "%s", cse_element_tok);
    if (!cse_cached_lib[0])
      snprintf(cse_cached_lib, sizeof(cse_cached_lib), "dc6f600a0d4404e5");
    return 1;
  }
  pthread_mutex_lock(&cse_token_mutex);
  time_t now = time(NULL);
  if (cse_cached_tok[0] && now - cse_token_fetched < CSE_TOKEN_TTL_SECS) {
    pthread_mutex_unlock(&cse_token_mutex);
    return 1;
  }
  /* do not hammer the token endpoint when it is failing */
  if (cse_token_failed && now - cse_token_failed < 60) {
    pthread_mutex_unlock(&cse_token_mutex);
    return cse_cached_tok[0] != '\0';
  }
  if (!cse_fetch_token_locked()) {
    if (getenv("OMNI_PERF"))
      fprintf(stderr, "[CSE] token fetch failed\n");
    cse_token_failed = now;
    pthread_mutex_unlock(&cse_token_mutex);
    return cse_cached_tok[0] != '\0';
  }
  cse_token_fetched = now;
  cse_token_failed = 0;
  if (getenv("OMNI_PERF"))
    fprintf(stderr, "[CSE] token fetched (cached %dh)\n",
            CSE_TOKEN_TTL_SECS / 3600);
  pthread_mutex_unlock(&cse_token_mutex);
  return 1;
}

char *api_url_google_cse_element(const char *encoded_query, int page,
                                 const char *lang) {
  if (!api_google_cse_element_available() || !encoded_query)
    return NULL;
  if (!cse_ensure_token() || !cse_cached_tok[0] || !cse_cached_lib[0])
    return NULL;
  char *url = malloc(2048);
  if (!url)
    return NULL;
  int start = (page > 1 ? page - 1 : 0) * API_PAGE_SIZE;
  snprintf(url, 2048,
           "https://cse.google.com/cse/element/v1?rsz=filtered_cse&num=%d"
           "&hl=%s&cselibv=%s&cx=%s&q=%s&safe=off&cse_tok=%s&callback=_"
           "&rurl=&searchtype=%s&exp=%s&start=%d",
           API_PAGE_SIZE, (lang && lang[0]) ? lang : "en", cse_cached_lib,
           cse_element_cx, encoded_query, cse_cached_tok,
           (page > 0) ? "" : "", cse_cached_exp, start);
  return url;
}

/* The reply is JSONP: a `/*O_o*\/` comment, then `_({...})`. */
static const char *jsonp_payload(const char *body, size_t len) {
  if (!body || len == 0)
    return NULL;
  const char *p = strstr(body, "_(");
  if (!p)
    p = strchr(body, '{');
  else
    p += 2;
  if (!p)
    return NULL;
  while (*p == ' ' || *p == '\n' || *p == '\r')
    p++;
  if (*p != '{')
    return NULL;
  return p;
}

int api_parse_google_cse_element(const char *body, size_t len, const char *lang,
                                 SearchResult **out, int max) {
  (void)lang;
  const char *start = jsonp_payload(body, len);
  if (!start)
    return 0;
  const char *end = NULL;
  for (size_t i = len; i > (size_t)(start - body); i--) {
    if (body[i - 1] == '}') {
      end = body + i;
      break;
    }
  }
  if (!end || end <= start)
    return 0;

  json_error_t error;
  json_t *root = json_loadb(start, (size_t)(end - start), 0, &error);
  if (!root) {
    if (getenv("OMNI_CSE_DEBUG"))
      fprintf(stderr, "[CSE] json_loadb failed: %s at line %d\n", error.text,
              error.line);
    return 0;
  }

  json_t *results = json_object_get(root, "results");
  int count = 0;
  if (json_is_array(results)) {
    *out = alloc_results_array((int)json_array_size(results), max);
    for (size_t i = 0; i < json_array_size(results) && count < max; i++) {
      json_t *item = json_array_get(results, i);
      char *title = dup_string(item, "titleNoFormatting");
      if (!title)
        title = dup_string(item, "title");
      char *link = dup_string(item, "unescapedUrl");
      if (!link)
        link = dup_string(item, "url");
      char *snippet = dup_string(item, "contentNoFormatting");
      if (!snippet)
        snippet = dup_string(item, "content");
      if (link && link[0]) {
        html_unescape_inplace(title);
        html_unescape_inplace(snippet);
        assign_result(&(*out)[count++], link, title, snippet, count + 1);
      } else {
        free(title);
        free(snippet);
        free(link);
      }
    }
  }
  json_decref(root);
  return count;
}

/* ---- Google Programmable Search ------------------------------------ */

static char google_key[256];
static char google_cx[128];

void api_google_cse_configure(const char *key, const char *cx) {
  snprintf(google_key, sizeof(google_key), "%s", key ? key : "");
  snprintf(google_cx, sizeof(google_cx), "%s", cx ? cx : "");
}

int api_google_cse_available(void) {
  return google_key[0] != '\0' && google_cx[0] != '\0';
}

char *api_url_google_cse(const char *encoded_query, int page, const char *lang) {
  if (!api_google_cse_available() || !encoded_query)
    return NULL;
  char *url = malloc(1024);
  if (!url)
    return NULL;
  int start = (page > 1 ? page - 1 : 0) * API_PAGE_SIZE + 1;
  snprintf(url, 1024,
           "https://www.googleapis.com/customsearch/v1?key=%s&cx=%s&q=%s"
           "&num=%d&start=%d&hl=%s",
           google_key, google_cx, encoded_query, API_PAGE_SIZE, start,
           (lang && lang[0]) ? lang : "en");
  return url;
}

int api_parse_google_cse(const char *body, size_t len, const char *lang,
                         SearchResult **out, int max) {
  (void)lang;
  json_error_t error;
  json_t *root = json_loadb(body, len, 0, &error);
  if (!root)
    return 0;

  json_t *items = json_object_get(root, "items");
  int count = 0;
  if (json_is_array(items)) {
    *out = alloc_results_array((int)json_array_size(items), max);
    for (size_t i = 0; i < json_array_size(items) && count < max; i++) {
      json_t *item = json_array_get(items, i);
      char *title = dup_string(item, "title");
      char *link = dup_string(item, "link");
      char *snippet = dup_string(item, "snippet");
      if (link) {
        html_unescape_inplace(title);
        html_unescape_inplace(snippet);
        add_result(*out, &count, max, link, title, snippet);
      }
      free(title);
      free(link);
      free(snippet);
    }
  }

  json_decref(root);
  return count;
}

/* ---- SearXNG bridge ------------------------------------------------ */

static char searxng_base[256];
static char searxng_engines[1024];

/* J5.6: SearXNG's own engine list is written with bangs ("!go", "!ddg") while
   the `engines=` parameter takes names ("google", "duckduckgo"). Accept either
   form. The names and shortcuts here are facts from SearXNG's settings.yml,
   not code from its repository. */
typedef struct {
  const char *shortcut;
  const char *name;
} SearxngBang;

static const SearxngBang SEARXNG_BANGS[] = {
    {"go", "google"},         {"g", "google"},          {"ddg", "duckduckgo"},
    {"b", "bing"},            {"bi", "bing images"},    {"br", "brave"},
    {"qw", "qwant"},          {"yh", "yahoo"},          {"sp", "startpage"},
    {"mw", "mojeek"},         {"wp", "wikipedia"},      {"wn", "wikinews"},
    {"wb", "wikibooks"},      {"wq", "wikiquote"},      {"ws", "wikisource"},
    {"wv", "wikivoyage"},     {"wy", "wiktionary"},     {"gd", "github"},
    {"gl", "gitlab"},         {"so", "stackoverflow"},  {"se", "stackexchange"},
    {"ap", "arch linux"},     {"wpkg", "npm"},          {"cr", "crates"},
    {"mdn", "mdn"},           {"yx", "yandex"},         {"bd", "baidu"},
    {"sc", "soundcloud"},     {"bc", "bandcamp"},       {"mg", "marginalia"},
    {"mn", "mwmbl"},          {"sx", "seznam"},         {"pp", "photon"},
    {"osm", "openstreetmap"}, {"ol", "openlibrary"},    {"pm", "pubmed"},
    {"ar", "arxiv"},          {"sd", "semantic scholar"},
    {"do", "docker hub"},     {"hg", "huggingface"},    {"fm", "film"},
    {"py", "pypi"},           {"hex", "hex"},           {"pc", "packagist"},
};

/* Writes the SearXNG engine name for a configured token, which may be a bare
   shortcut, a "!shortcut" bang, or a name. */
static void searxng_normalise_name(const char *token, char *out, size_t size) {
  while (*token == '!')
    token++;
  for (size_t i = 0; i < sizeof(SEARXNG_BANGS) / sizeof(SEARXNG_BANGS[0]); i++) {
    if (strcasecmp(token, SEARXNG_BANGS[i].shortcut) == 0) {
      snprintf(out, size, "%s", SEARXNG_BANGS[i].name);
      return;
    }
  }
  snprintf(out, size, "%s", token);
}

void api_searxng_configure(const char *base_url, const char *engines) {
  snprintf(searxng_base, sizeof(searxng_base), "%s", base_url ? base_url : "");
  size_t n = strlen(searxng_base);
  while (n > 0 && searxng_base[n - 1] == '/')
    searxng_base[--n] = '\0';

  searxng_engines[0] = '\0';
  if (!engines || !engines[0]) {
    snprintf(searxng_engines, sizeof(searxng_engines), "google");
    return;
  }
  char work[1024];
  snprintf(work, sizeof(work), "%s", engines);
  char *saveptr = NULL;
  char *token = strtok_r(work, ", \t", &saveptr);
  while (token) {
    char name[64];
    size_t used = strlen(searxng_engines);
    searxng_normalise_name(token, name, sizeof(name));
    int dup = 0;
    if (used) {
      char probe[1100];
      snprintf(probe, sizeof(probe), ",%s,", name);
      char haystack[1100];
      snprintf(haystack, sizeof(haystack), ",%s,", searxng_engines);
      dup = strstr(haystack, probe) != NULL;
    }
    if (name[0] && !dup && used + strlen(name) + 2 < sizeof(searxng_engines)) {
      snprintf(searxng_engines + used, sizeof(searxng_engines) - used, "%s%s",
               used ? "," : "", name);
    }
    token = strtok_r(NULL, ", \t", &saveptr);
  }
  if (!searxng_engines[0])
    snprintf(searxng_engines, sizeof(searxng_engines), "google");
}

const char *api_searxng_engines(void) { return searxng_engines; }

/* J5.6: two failures were silent before. An instance with format=json turned
   off answers 403, and an engine name that does not exist on that instance
   answers 200 with no results. Both looked exactly like "SearXNG is broken".
   One request each, at startup, with one clear line per problem. */
void api_searxng_validate(void) {
  if (!searxng_base[0])
    return;

  char url[1024];
  snprintf(url, sizeof(url), "%s/search?q=test&format=json&engines=google",
           searxng_base);
  HttpResponse resp = http_get_budget(url, "omnisearch/1.0 (search bridge)", 4000);
  if (!resp.memory) {
    fprintf(stderr,
            "[WARN] SearXNG bridge: %s did not answer. The engine stays "
            "enabled but will fail like this on every search\n",
            searxng_base);
    return;
  }
  json_error_t error;
  json_t *root = json_loadb(resp.memory, resp.size, 0, &error);
  http_response_free(&resp);
  if (!root) {
    fprintf(stderr,
            "[WARN] SearXNG bridge: %s/search?q=test&format=json did not return "
            "JSON. JSON output is off by default in SearXNG; add\n"
            "         search:\n           formats:\n             - html\n"
            "             - json\n"
            "       to the instance's settings.yml (or set SEARXNG_SETTINGS_PATH) "
            "and restart it. Until then the bridge cannot work\n",
            searxng_base);
    return;
  }
  if (!json_object_get(root, "results")) {
    fprintf(stderr,
            "[WARN] SearXNG bridge: the JSON answer has no \"results\" array; "
            "check that this URL is a SearXNG instance\n");
    json_decref(root);
    return;
  }
  json_decref(root);

  /* the configured names must exist on this instance */
  snprintf(url, sizeof(url), "%s/config", searxng_base);
  resp = http_get_budget(url, "omnisearch/1.0 (search bridge)", 4000);
  if (!resp.memory) {
    fprintf(stderr, "[WARN] SearXNG bridge: %s/config did not answer; engine "
                    "names were not checked\n", searxng_base);
    return;
  }
  root = json_loadb(resp.memory, resp.size, 0, &error);
  http_response_free(&resp);
  if (!root)
    return;
  json_t *engines = json_object_get(root, "engines");
  if (!json_is_array(engines)) {
    json_decref(root);
    return;
  }
  char list[1024];
  snprintf(list, sizeof(list), "%s", searxng_engines);
  char *saveptr = NULL;
  char *token = strtok_r(list, ",", &saveptr);
  while (token) {
    int found = 0;
    int enabled = 0;
    for (size_t i = 0; i < json_array_size(engines); i++) {
      json_t *e = json_array_get(engines, i);
      const char *name = json_string_value(json_object_get(e, "name"));
      if (name && strcasecmp(name, token) == 0) {
        found = 1;
        json_t *en = json_object_get(e, "enabled");
        enabled = !json_is_boolean(en) || json_is_true(en);
        break;
      }
    }
    if (!found)
      fprintf(stderr,
              "[WARN] SearXNG bridge: no engine named \"%s\" on this instance; "
              "it will be ignored. Check GET %s/config for the names\n",
              token, searxng_base);
    else if (!enabled)
      fprintf(stderr,
              "[WARN] SearXNG bridge: engine \"%s\" is disabled on the "
              "instance, so it will return nothing\n",
              token);
    token = strtok_r(NULL, ",", &saveptr);
  }
  json_decref(root);
}

int api_searxng_available(void) { return searxng_base[0] != '\0'; }

char *api_url_searxng(const char *encoded_query, int page, const char *lang) {
  if (!api_searxng_available() || !encoded_query)
    return NULL;
  char *url = malloc(1024);
  if (!url)
    return NULL;
  snprintf(url, 1024,
           "%s/search?q=%s&format=json&pageno=%d&language=%s&engines=%s",
           searxng_base, encoded_query, page > 0 ? page : 1,
           (lang && lang[0]) ? lang : "en", searxng_engines);
  return url;
}

/* J5.6: one SearXNG job carries results from several indexes. These are the
   names known to share an index with something already in the registry; an
   engine we do not recognise counts as "other" rather than being attributed to
   whatever the bridge's own group says, because over-counting independence is
   how the H1 one-index note becomes a lie. */
static const char *searxng_group_for(const char *engine) {
  static const struct {
    const char *engine;
    const char *group;
  } MAP[] = {
      {"google", "google"},     {"startpage", "google"},
      {"brave", "brave"},       {"duckduckgo", "ddg"},
      {"bing", "bing"},         {"yahoo", "bing"},
      {"qwant", "qwant"},       {"mojeek", "mojeek"},
      {"yandex", "yandex"},     {"baidu", "baidu"},
      {"wikipedia", "wikipedia"}, {"marginalia", "marginalia"},
  };
  if (engine) {
    for (size_t i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
      if (strcasecmp(engine, MAP[i].engine) == 0)
        return MAP[i].group;
  }
  return "other";
}

int api_parse_searxng(const char *body, size_t len, const char *lang,
                      SearchResult **out, int max) {
  (void)lang;
  json_error_t error;
  json_t *root = json_loadb(body, len, 0, &error);
  if (!root)
    return 0;

  json_t *results = json_object_get(root, "results");
  int count = 0;
  if (json_is_array(results)) {
    *out = alloc_results_array((int)json_array_size(results), max);
    for (size_t i = 0; i < json_array_size(results) && count < max; i++) {
      json_t *item = json_array_get(results, i);
      char *title = dup_string(item, "title");
      char *link = dup_string(item, "url");
      char *snippet = dup_string(item, "content");
      if (link) {
        html_unescape_inplace(title);
        html_unescape_inplace(snippet);
        int before = count;
        add_result(*out, &count, max, link, title, snippet);
        if (count > before && *out) {
          SearchResult *hit = &(*out)[before];
          /* the result's own engine, so the group is the index that answered.
             When the instance does not say, leave it empty: the caller then
             falls back to the engine entry's group rather than inheriting
             "other" for every hit. */
          const char *engine = json_string_value(json_object_get(item, "engine"));
          if (engine && engine[0]) {
            snprintf(hit->index_group, sizeof(hit->index_group), "%s",
                     searxng_group_for(engine));
            hit->source = strdup(engine);
          }
          /* J5.2: the fields the instance really sends. A name that is absent
             stays NULL rather than being filled with a guess. */
          {
            const char *thumb = json_string_value(
                json_object_get(item, "thumbnail"));
            if (!thumb)
              thumb = json_string_value(json_object_get(item, "img_src"));
            if (thumb && (strncmp(thumb, "http://", 7) == 0 ||
                          strncmp(thumb, "https://", 8) == 0))
              hit->thumbnail = strdup(thumb);
            const char *date = json_string_value(
                json_object_get(item, "publishedDate"));
            if (!date)
              date = json_string_value(json_object_get(item, "pubdate"));
            if (date && date[0])
              hit->date = strdup(date);
            const char *category = json_string_value(
                json_object_get(item, "category"));
            if (category && category[0] &&
                strcmp(category, "general") != 0)
              hit->kind = strdup(category);
          }
        }
      }
      free(title);
      free(link);
      free(snippet);
    }
  }

  json_decref(root);
  return count;
}

/* M4.5: the Test button, in plain words. One token fetch and one query, and
   the five outcomes the review asks for. Never prints a token or a header. */
int api_google_cse_element_selftest(const char *cx, char *out, size_t cap) {
  if (!out || cap == 0)
    return -1;
  if (!cx || !cx[0]) {
    snprintf(out, cap, "Paste your search engine id first.");
    return -1;
  }
  api_google_cse_element_configure(cx, "");
  if (!api_google_cse_element_available()) {
    snprintf(out, cap,
             "Google does not know that id - check you copied the Search "
             "engine ID.");
    return -1;
  }
  char *url = api_url_google_cse_element("linux+kernel", 1, "en");
  if (!url) {
    snprintf(out, cap,
             "Could not get a token for that engine. Check the id, then try "
             "again.");
    return -1;
  }
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  HttpResponse resp = http_get(url, "omnisearch/1.0");
  clock_gettime(CLOCK_MONOTONIC, &t1);
  free(url);
  double secs = (double)(t1.tv_sec - t0.tv_sec) +
                (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
  if (!resp.memory || resp.size == 0) {
    snprintf(out, cap, "Could not reach google.com.");
    free(resp.memory);
    return -1;
  }
  SearchResult *results = NULL;
  int n = api_parse_google_cse_element(resp.memory, resp.size, "en", &results,
                                       API_PAGE_SIZE);
  free(resp.memory);
  if (n <= 0) {
    snprintf(out, cap,
             "Your engine returned nothing - switch on \"Search the entire "
             "web\" in its settings.");
    return -1;
  }
  snprintf(out, cap, "Working - %d results in %.1f s", n, secs);
  for (int i = 0; i < n; i++) {
    free(results[i].url);
    free(results[i].title);
    free(results[i].snippet);
  }
  free(results);
  return 0;
}
