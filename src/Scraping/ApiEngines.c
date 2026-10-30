#include "ApiEngines.h"
#include "../Utility/JsonHelper.h"
#include "../Utility/Unescape.h"
#include "Config.h"
#include <jansson.h>
#include <stdio.h>
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
static char searxng_engines[128];

void api_searxng_configure(const char *base_url, const char *engines) {
  snprintf(searxng_base, sizeof(searxng_base), "%s", base_url ? base_url : "");
  size_t n = strlen(searxng_base);
  while (n > 0 && searxng_base[n - 1] == '/')
    searxng_base[--n] = '\0';
  snprintf(searxng_engines, sizeof(searxng_engines), "%s",
           (engines && engines[0]) ? engines : "google");
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
