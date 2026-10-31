#include "../Utility/Unescape.h"
#include "../Utility/XmlHelper.h"
#include "Config.h"
#include "ApiEngines.h"
#include "Scraping.h"
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <stdlib.h>
#include <string.h>

xmlXPathContextPtr create_xpath_context(xmlDocPtr doc) {
  return xmlXPathNewContext(doc);
}

void free_xpath_objects(xmlXPathContextPtr ctx, xmlXPathObjectPtr obj) {
  if (obj)
    xmlXPathFreeObject(obj);
  if (ctx)
    xmlXPathFreeContext(ctx);
}

SearchResult *alloc_results_array(int capacity, int max_results) {
  int count = capacity < max_results ? capacity : max_results;
  return xml_result_alloc(capacity, count);
}

int assign_result(SearchResult *result, char *url, char *title, char *snippet,
                  int unescape) {
  result->url = unescape ? unescape_search_url(url) : strdup(url ? url : "");
  result->title = strdup(title ? title : "No Title");
  result->snippet = strdup(snippet ? snippet : "");

  /* a hit with no usable link is not a result: it used to be counted, which
     left the fusion stage with zero candidates and a blank page. */
  if (!result->url || result->url[0] == '\0') {
    free(result->url);
    free(result->title);
    free(result->snippet);
    result->url = NULL;
    result->title = NULL;
    result->snippet = NULL;
    return 0;
  }
  return 1;
}

void free_xml_node_list(char *title, char *url, char *snippet) {
  if (title)
    xmlFree(title);
  if (url)
    xmlFree(url);
  if (snippet)
    xmlFree(snippet);
}

/* depth-limited subtree search for the first element whose class contains cls */
typedef struct { const char *cls; char *found; } class_search;

static int class_search_walk(xmlNodePtr node, class_search *cs, int depth);

/* depth-limited subtree search for the first element whose class contains cls */
static char *find_class_text(xmlNodePtr node, const char *cls) {
  class_search cs = {cls, NULL};
  class_search_walk(node, &cs, 0);
  return cs.found;
}

static int class_search_walk(xmlNodePtr node, class_search *cs, int depth) {
  if (!node || cs->found || depth > 6)
    return 0;
  for (xmlNodePtr n = node; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE) {
      char *c = (char *)xmlGetProp(n, (const xmlChar *)"class");
      int hit = c && strstr(c, cs->cls);
      if (c)
        xmlFree(c);
      if (hit) {
        cs->found = (char *)xmlNodeGetContent(n);
        return 1;
      }
    }
    if (class_search_walk(n->children, cs, depth + 1))
      return 1;
  }
  return 0;
}

/* comments.duckduckgo.com/html/ markup: <a class="result__a"> plus a sibling
   <a class="result__snippet">. This endpoint is what SearXNG uses, and unlike
   lite. it keeps answering from this machine (docs/G1-engines.md). */
static int parse_ddg_html(const char *engine_name, xmlDocPtr doc,
                          SearchResult **out_results, int max_results) {
  (void)engine_name;
  xmlXPathContextPtr ctx = create_xpath_context(doc);
  if (!ctx)
    return 0;

  xmlXPathObjectPtr obj =
      xml_xpath_eval(ctx, "//a[contains(@class, 'result__a')]");
  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int num_links = obj->nodesetval->nodeNr;
  *out_results = alloc_results_array(num_links, max_results);
  if (!*out_results) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int count = 0;
  for (int i = 0; i < num_links && count < max_results; i++) {
    xmlNodePtr link = obj->nodesetval->nodeTab[i];
    char *href = (char *)xmlGetProp(link, (const xmlChar *)"href");
    char *title = (char *)xmlNodeGetContent(link);
    if (!href || !href[0]) {
      free(href);
      free(title);
      continue;
    }

    /* the snippet is the result__snippet inside the same result block; walk up
       at most two levels so one result's snippet can't leak into another */
    char *snippet = NULL;
    xmlNodePtr scope = link->parent ? link->parent->parent : NULL;
    for (int up = 0; scope && !snippet && up < 2; up++, scope = scope->parent)
      snippet = find_class_text(scope, "result__snippet");

    assign_result(&(*out_results)[count], href, title, snippet, 1);
    count++;
  }

  free_xpath_objects(ctx, obj);
  return count;
}

static int parse_ddg_lite(const char *engine_name, xmlDocPtr doc,
                          SearchResult **out_results, int max_results) {
  (void)engine_name;
  int found_count = 0;

  xmlXPathContextPtr ctx = create_xpath_context(doc);
  if (!ctx)
    return 0;

  xmlXPathObjectPtr obj =
      xml_xpath_eval(ctx, "//tr[not(contains(@class, "
                          "'result-sponsored'))]//a[@class='result-link']");

  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int num_links = obj->nodesetval->nodeNr;
  *out_results = alloc_results_array(num_links, max_results);
  if (!*out_results) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  for (int i = 0; i < num_links && found_count < max_results; i++) {
    xmlNodePtr link_node = obj->nodesetval->nodeTab[i];
    char *title = xml_node_content(link_node);
    char *url = (char *)xmlGetProp(link_node, (xmlChar *)"href");
    char *snippet_text = NULL;

    xmlNodePtr current = link_node->parent;
    while (current && xmlStrcasecmp(current->name, (const xmlChar *)"tr") != 0)
      current = current->parent;

    if (current && current->next) {
      xmlNodePtr snippet_row = current->next;
      while (snippet_row &&
             xmlStrcasecmp(snippet_row->name, (const xmlChar *)"tr") != 0)
        snippet_row = snippet_row->next;
      if (snippet_row) {
        ctx->node = snippet_row;
        xmlXPathObjectPtr s_obj =
            xml_xpath_eval(ctx, ".//td[@class='result-snippet']");
        if (s_obj && s_obj->nodesetval && s_obj->nodesetval->nodeNr > 0)
          snippet_text = xml_node_content(s_obj->nodesetval->nodeTab[0]);
        if (s_obj)
          xmlXPathFreeObject(s_obj);
        ctx->node = NULL;
      }
    }

    if (assign_result(&(*out_results)[found_count], url, title, snippet_text, 1))
      found_count++;
    free_xml_node_list(title, url, snippet_text);
  }

  free_xpath_objects(ctx, obj);
  return found_count;
}

static int parse_yahoo(const char *engine_name, xmlDocPtr doc,
                       SearchResult **out_results, int max_results) {
  (void)engine_name;
  int found_count = 0;

  xmlXPathContextPtr ctx = create_xpath_context(doc);
  if (!ctx)
    return 0;

  xmlXPathObjectPtr obj =
      xml_xpath_eval(ctx, "//div[contains(@class, 'algo-sr')]");

  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int num_results = obj->nodesetval->nodeNr;
  *out_results = alloc_results_array(num_results, max_results);
  if (!*out_results) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  for (int i = 0; i < num_results && found_count < max_results; i++) {
    xmlNodePtr result_node = obj->nodesetval->nodeTab[i];
    ctx->node = result_node;

    xmlXPathObjectPtr link_obj = xml_xpath_eval(
        ctx, ".//div[contains(@class, 'compTitle')]//a[@target='_blank']");
    char *url =
        (link_obj && link_obj->nodesetval && link_obj->nodesetval->nodeNr > 0)
            ? (char *)xmlGetProp(link_obj->nodesetval->nodeTab[0],
                                 (xmlChar *)"href")
            : NULL;

    xmlXPathObjectPtr title_obj =
        xml_xpath_eval(ctx, ".//h3[contains(@class, 'title')]");
    char *title = (title_obj && title_obj->nodesetval &&
                   title_obj->nodesetval->nodeNr > 0)
                      ? xml_node_content(title_obj->nodesetval->nodeTab[0])
                      : NULL;

    xmlXPathObjectPtr snippet_obj =
        xml_xpath_eval(ctx, ".//div[contains(@class, 'compText')]//p");
    char *snippet_text =
        (snippet_obj && snippet_obj->nodesetval &&
         snippet_obj->nodesetval->nodeNr > 0)
            ? xml_node_content(snippet_obj->nodesetval->nodeTab[0])
            : NULL;

    if (url && title &&
        assign_result(&(*out_results)[found_count], url, title, snippet_text, 1))
      found_count++;

    free_xml_node_list(title, url, snippet_text);
    if (link_obj)
      xmlXPathFreeObject(link_obj);
    if (title_obj)
      xmlXPathFreeObject(title_obj);
    if (snippet_obj)
      xmlXPathFreeObject(snippet_obj);
  }

  ctx->node = NULL;
  free_xpath_objects(ctx, obj);
  return found_count;
}

static int parse_yahoo(const char *engine_name, xmlDocPtr doc,
                       SearchResult **out_results, int max_results);

static int parse_yacy(const char *engine_name, xmlDocPtr doc,
                      SearchResult **out_results, int max_results) {
  (void)engine_name;
  int found_count = 0;

  xmlXPathContextPtr ctx = create_xpath_context(doc);
  if (!ctx)
    return 0;

  xmlXPathObjectPtr obj = xml_xpath_eval(ctx, "//item");

  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int num_items = obj->nodesetval->nodeNr;
  *out_results = alloc_results_array(num_items, max_results);
  if (!*out_results) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  for (int i = 0; i < num_items && found_count < max_results; i++) {
    xmlNodePtr item_node = obj->nodesetval->nodeTab[i];
    ctx->node = item_node;

    xmlXPathObjectPtr title_obj = xml_xpath_eval(ctx, "./title");
    char *title = (title_obj && title_obj->nodesetval &&
                   title_obj->nodesetval->nodeNr > 0)
                      ? xml_node_content(title_obj->nodesetval->nodeTab[0])
                      : NULL;

    xmlXPathObjectPtr link_obj = xml_xpath_eval(ctx, "./link");
    char *url =
        (link_obj && link_obj->nodesetval && link_obj->nodesetval->nodeNr > 0)
            ? xml_node_content(link_obj->nodesetval->nodeTab[0])
            : NULL;

    xmlXPathObjectPtr desc_obj = xml_xpath_eval(ctx, "./description");
    char *snippet_text =
        (desc_obj && desc_obj->nodesetval && desc_obj->nodesetval->nodeNr > 0)
            ? xml_node_content(desc_obj->nodesetval->nodeTab[0])
            : NULL;

    if (url && title &&
        assign_result(&(*out_results)[found_count], url, title, snippet_text, 0))
      found_count++;

    free_xml_node_list(title, url, snippet_text);
    if (title_obj)
      xmlXPathFreeObject(title_obj);
    if (link_obj)
      xmlXPathFreeObject(link_obj);
    if (desc_obj)
      xmlXPathFreeObject(desc_obj);
  }

  ctx->node = NULL;
  free_xpath_objects(ctx, obj);
  return found_count;
}

static char yacy_base_url[BUFFER_SIZE_LARGE];

void configure_yacy_engine(const char *instance) {
  if (!instance || instance[0] == '\0')
    instance = DEFAULT_YACY_INSTANCE;

  snprintf(yacy_base_url, sizeof(yacy_base_url),
           "%s/"
           "yacysearch.rss?resource=global&urlmaskfilter=.*&prefermaskfilter=&"
           "nav=all&maximumRecords=%d&query=",
           instance, MAX_RESULTS_PER_ENGINE);

  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (strcmp(ENGINE_REGISTRY[i].id, "yacy") == 0) {
      ENGINE_REGISTRY[i].base_url = yacy_base_url;
      break;
    }
  }
}

/* ---- Bing ---------------------------------------------------------- */

/* Bing wraps result links in /ck/a?...&u=a1<base64url of the target> */
static char *bing_decode_target(const char *href) {
  if (!href)
    return NULL;

  const char *u = strstr(href, "u=a1");
  if (!u)
    return NULL;

  const char *start = u + 4;
  size_t len = 0;
  while (start[len] != '\0' && start[len] != '&' && start[len] != '#')
    len++;
  if (len == 0)
    return NULL;

  static const char tbl[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  char *out = malloc(len + 4);
  if (!out)
    return NULL;

  unsigned long long acc = 0;
  int bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    const char *p = strchr(tbl, start[i]);
    if (!p || start[i] == '\0') {
      free(out);
      return NULL;
    }
    acc = (acc << 6) | (unsigned long long)(p - tbl);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out[o++] = (char)((acc >> bits) & 0xFF);
    }
  }
  out[o] = '\0';

  if (strncmp(out, "http", 4) != 0) {
    free(out);
    return NULL;
  }
  return out;
}

static int parse_bing(const char *engine_name, xmlDocPtr doc,
                      SearchResult **out_results, int max_results) {
  (void)engine_name;
  int found_count = 0;

  xmlXPathContextPtr ctx = create_xpath_context(doc);
  if (!ctx)
    return 0;

  xmlXPathObjectPtr obj =
      xml_xpath_eval(ctx, "//li[contains(@class,'b_algo')]");
  /* Bing rotates the result list class between experiments; fall back to any
     result list item that carries a heading link. */
  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    /* free the object only: free_xpath_objects also destroys the context we
       still need for the second query */
    if (obj)
      xmlXPathFreeObject(obj);
    obj = xml_xpath_eval(ctx, "//*[@id='b_results']//li[.//h2/a]");
  }
  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int num_results = obj->nodesetval->nodeNr;
  *out_results = alloc_results_array(num_results, max_results);
  if (!*out_results) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  for (int i = 0; i < num_results && found_count < max_results; i++) {
    ctx->node = obj->nodesetval->nodeTab[i];

    xmlXPathObjectPtr link_obj = xml_xpath_eval(ctx, ".//h2/a[1]");
    int has_link = link_obj && link_obj->nodesetval &&
                   link_obj->nodesetval->nodeNr > 0;

    char *href = has_link
                     ? (char *)xmlGetProp(link_obj->nodesetval->nodeTab[0],
                                          (xmlChar *)"href")
                     : NULL;
    char *title = has_link
                      ? xml_node_content(link_obj->nodesetval->nodeTab[0])
                      : NULL;

    xmlXPathObjectPtr snip_obj = xml_xpath_eval(
        ctx, ".//div[contains(@class,'b_caption')]//p[1]");
    if (!snip_obj || !snip_obj->nodesetval ||
        snip_obj->nodesetval->nodeNr == 0) {
      if (snip_obj)
        xmlXPathFreeObject(snip_obj);
      snip_obj = xml_xpath_eval(ctx, ".//p[1]");
    }
    char *snippet = (snip_obj && snip_obj->nodesetval &&
                     snip_obj->nodesetval->nodeNr > 0)
                        ? xml_node_content(snip_obj->nodesetval->nodeTab[0])
                        : NULL;

    char *decoded = bing_decode_target(href);
    const char *url = decoded ? decoded : href;

    if (url && title &&
        assign_result(&(*out_results)[found_count], (char *)url, title, snippet, 0))
      found_count++;

    free(decoded);
    free_xml_node_list(title, href, snippet);
    if (link_obj)
      xmlXPathFreeObject(link_obj);
    if (snip_obj)
      xmlXPathFreeObject(snip_obj);
  }

  ctx->node = NULL;
  free_xpath_objects(ctx, obj);
  return found_count;
}

/* ---- Brave --------------------------------------------------------- */

static int parse_brave(const char *engine_name, xmlDocPtr doc,
                       SearchResult **out_results, int max_results) {
  (void)engine_name;
  int found_count = 0;

  xmlXPathContextPtr ctx = create_xpath_context(doc);
  if (!ctx)
    return 0;

  xmlXPathObjectPtr obj = xml_xpath_eval(
      ctx, "//div[contains(@class,'snippet') and @data-type='web']");
  if (!obj || !obj->nodesetval || obj->nodesetval->nodeNr == 0) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  int num_results = obj->nodesetval->nodeNr;
  *out_results = alloc_results_array(num_results, max_results);
  if (!*out_results) {
    free_xpath_objects(ctx, obj);
    return 0;
  }

  for (int i = 0; i < num_results && found_count < max_results; i++) {
    ctx->node = obj->nodesetval->nodeTab[i];

    xmlXPathObjectPtr link_obj = xml_xpath_eval(ctx, ".//a[@href][1]");
    int has_link = link_obj && link_obj->nodesetval &&
                   link_obj->nodesetval->nodeNr > 0;

    char *url = has_link
                    ? (char *)xmlGetProp(link_obj->nodesetval->nodeTab[0],
                                         (xmlChar *)"href")
                    : NULL;

    xmlXPathObjectPtr title_obj =
        xml_xpath_eval(ctx, ".//div[contains(@class,'search-snippet-title')]");
    char *title = (title_obj && title_obj->nodesetval &&
                   title_obj->nodesetval->nodeNr > 0)
                      ? xml_node_content(title_obj->nodesetval->nodeTab[0])
                      : NULL;

    xmlXPathObjectPtr snip_obj = xml_xpath_eval(
        ctx, ".//div[contains(@class,'generic-snippet')]//div[contains(@class,'content')]");
    char *snippet = (snip_obj && snip_obj->nodesetval &&
                     snip_obj->nodesetval->nodeNr > 0)
                        ? xml_node_content(snip_obj->nodesetval->nodeTab[0])
                        : NULL;

    if (url && title &&
        assign_result(&(*out_results)[found_count], url, title, snippet, 0))
      found_count++;

    free_xml_node_list(title, url, snippet);
    if (link_obj)
      xmlXPathFreeObject(link_obj);
    if (title_obj)
      xmlXPathFreeObject(title_obj);
    if (snip_obj)
      xmlXPathFreeObject(snip_obj);
  }

  ctx->node = NULL;
  free_xpath_objects(ctx, obj);
  return found_count;
}

SearchEngine ENGINE_REGISTRY[] = {
    {.id = "ddg",
     .name = "DuckDuckGo",
     /* html. rather than lite.: lite. answered a region-selector shell to every
        request after the first, while html. keeps returning results with the
        navigation headers and locale cookies below (docs/G1-engines.md). */
     .base_url = "https://html.duckduckgo.com/html/?q=",
     .host_header = "html.duckduckgo.com",
     .referer = "https://html.duckduckgo.com/",
     .page_param = "s",
     .page_multiplier = 30,
     .page_base = 0,
     .parser = parse_ddg_html,
     .enabled = 1,
     .timeout_secs = 8,
     .supports_site = 1, .verticals = V_WEB, .weight = 1.0,
     .index_group = "bing", .user_agent = "safari",
     .browser_headers = 1,
     .init_cookies = "kl=gb-en; df=en-GB; ad=0; ah=0" },
    /* kept for reference and for anyone who has a good experience with it;
       disabled because it stopped answering from this host. */
    {.id = "ddg-lite",
     .name = "DuckDuckGo Lite (old endpoint)",
     .base_url = "https://lite.duckduckgo.com/lite/?q=",
     .host_header = "lite.duckduckgo.com",
     .referer = "https://lite.duckduckgo.com/",
     .page_param = "s",
     .page_multiplier = 30,
     .page_base = 0,
     .parser = parse_ddg_lite,
     .enabled = 0,
     .timeout_secs = 5,
     .supports_site = 1, .verticals = V_WEB, .weight = 1.0,
     .index_group = "bing" },
    {.id = "bing",
     .name = "Bing",
     .base_url = "https://www.bing.com/search?q=",
     .host_header = "www.bing.com",
     .referer = "https://www.bing.com/",
     .page_param = "first",
     .page_multiplier = 10,
     .page_base = 1,
     .parser = parse_bing,
     .enabled = 1,
     .timeout_secs = 5,
     .supports_site = 1, .verticals = V_WEB, .weight = 0.95, .index_group = "bing" },
    {.id = "yahoo",
     .name = "Yahoo",
     .base_url = "https://search.yahoo.com/search?p=",
     .host_header = "search.yahoo.com",
     .referer = "https://search.yahoo.com/",
     .page_param = "b",
     .page_multiplier = 10,
     .page_base = 1,
     .parser = parse_yahoo,
     .enabled = 1,
     .timeout_secs = 5,
     .supports_site = 1, .verticals = V_WEB, .weight = 1.2, .index_group = "bing" },
    {.id = "brave",
     .name = "Brave",
     .base_url = "https://search.brave.com/search?q=",
     .host_header = "search.brave.com",
     .referer = "https://search.brave.com/",
     .page_param = "offset",
     .page_multiplier = 10,
     .page_base = 0,
     .parser = parse_brave,
     .enabled = 1,
     .timeout_secs = 5,
     .supports_site = 1, .verticals = V_WEB, .weight = 1.15,
     .index_group = "brave", .user_agent = "safari" },

    {.id = "openalex",
     .name = "OpenAlex",
     .base_url = "",
     .host_header = "api.openalex.org",
     .referer = NULL,
     .page_param = "page",
     .page_multiplier = 10,
     .page_base = 1,
     .parser = NULL,
     .body_parser = api_parse_openalex,
     .build_url = api_url_openalex,
     .enabled = 1,
     .is_xml = 0,
     .timeout_secs = 3,
     .verticals = V_ACADEMIC,
     .weight = 1.3,
     .supports_site = 0, .index_group = "openalex" },
    {.id = "crossref",
     .name = "Crossref",
     .base_url = "",
     .host_header = "api.crossref.org",
     .referer = NULL,
     .page_param = "offset",
     .page_multiplier = 10,
     .page_base = 0,
     .parser = NULL,
     .body_parser = api_parse_crossref,
     .build_url = api_url_crossref,
     .enabled = 1,
     .is_xml = 0,
     .timeout_secs = 3,
     .verticals = V_ACADEMIC,
     .weight = 1.0,
     .supports_site = 0, .index_group = "crossref" },
    {.id = "arxiv",
     .name = "arXiv",
     .base_url = "",
     .host_header = "export.arxiv.org",
     .referer = NULL,
     .page_param = "start",
     .page_multiplier = 10,
     .page_base = 0,
     .parser = NULL,
     .body_parser = api_parse_arxiv,
     .build_url = api_url_arxiv,
     .enabled = 1,
     .is_xml = 1,
     .timeout_secs = 4,
     .verticals = V_ACADEMIC,
     .weight = 1.0,
     .supports_site = 0, .index_group = "arxiv" },
    {.id = "stackexchange",
     .name = "Stack Overflow",
     .base_url = "",
     .host_header = "api.stackexchange.com",
     .referer = NULL,
     .page_param = "page",
     .page_multiplier = 10,
     .page_base = 1,
     .parser = NULL,
     .body_parser = api_parse_stackexchange,
     .build_url = api_url_stackexchange,
     .enabled = 1,
     .is_xml = 0,
     .timeout_secs = 3,
     .verticals = V_CODE,
     .weight = 1.2,
     .supports_site = 0, .index_group = "stackexchange" },
    {.id = "hn",
     .name = "Hacker News",
     .base_url = "",
     .host_header = "hn.algolia.com",
     .referer = NULL,
     .page_param = "page",
     .page_multiplier = 10,
     .page_base = 0,
     .parser = NULL,
     .body_parser = api_parse_hn,
     .build_url = api_url_hn,
     .enabled = 1,
     .is_xml = 0,
     .timeout_secs = 3,
     .verticals = V_CODE | V_NEWS,
     .weight = 0.8,
     .supports_site = 0, .index_group = "hn" },
    {.id = "wikipedia",
     .name = "Wikipedia",
     .base_url = "",
     .host_header = "en.wikipedia.org",
     .referer = NULL,
     .page_param = "sroffset",
     .page_multiplier = 10,
     .page_base = 0,
     .parser = NULL,
     .body_parser = api_parse_wikipedia,
     .build_url = api_url_wikipedia,
     .enabled = 1,
     .is_xml = 0,
     .timeout_secs = 3,
     .verticals = V_ENTITY,
     .weight = 1.2,
     .supports_site = 0, .index_group = "wikipedia" },
    {.id = "yacy",
     .name = "YaCy",
     .base_url = "",
     .host_header = NULL,
     .referer = NULL,
     .page_param = "startRecord",
     .page_multiplier = 10,
     .page_base = 0,
     .parser = parse_yacy,
     .enabled = 0,
     .is_xml = 1,
     .supports_site = 0, .verticals = V_WEB, .weight = 0.6, .index_group = "yacy" },
    {.id = "google",
     .name = "Google",
     .base_url = "",
     .host_header = "www.googleapis.com",
     .referer = NULL,
     .page_param = "start",
     .page_multiplier = 10,
     .page_base = 1,
     .parser = NULL,
     .body_parser = api_parse_google_cse,
     .build_url = api_url_google_cse,
     .enabled = 0,
     .is_xml = 0,
     .timeout_secs = 6,
     .supports_site = 1,
     .verticals = V_WEB,
     .weight = 1.3, .index_group = "google" },
    {.id = "searxng",
     .name = "SearXNG",
     .base_url = "",
     .host_header = NULL,
     .referer = NULL,
     .page_param = "pageno",
     .page_multiplier = 1,
     .page_base = 1,
     .parser = NULL,
     .body_parser = api_parse_searxng,
     .build_url = api_url_searxng,
     .enabled = 0,
     .is_xml = 0,
     .timeout_secs = 6,
     .supports_site = 1,
     .verticals = V_WEB,
     .weight = 1.25, .index_group = "google" }};

const int ENGINE_COUNT = sizeof(ENGINE_REGISTRY) / sizeof(SearchEngine);

static int engine_id_compare(const char *engine_id, const char *config_id) {
  while (*engine_id && *config_id) {
    char e = *engine_id;
    char c = *config_id;
    if (e >= 'A' && e <= 'Z')
      e = e - 'A' + 'a';
    if (c >= 'A' && c <= 'Z')
      c = c - 'A' + 'a';
    if (e != c)
      return 0;
    engine_id++;
    config_id++;
  }
  return *engine_id == *config_id;
}

void engine_set_enabled(const char *engine_id, int enabled) {
  for (int i = 0; i < ENGINE_COUNT; i++) {
    if (engine_id_compare(ENGINE_REGISTRY[i].id, engine_id)) {
      ENGINE_REGISTRY[i].enabled = enabled ? 1 : 0;
      return;
    }
  }
}

/* "*" used to mean "enable everything", which silently resurrected engines the
   registry ships disabled - a disabled engine ran, failed, and spent a request
   slot on every query. It now means "the engines the registry ships enabled";
   naming a disabled engine explicitly ("*,yacy") still opts it in, which is
   what docs/SETUP.md tells people to do. */
static int shipped_enabled[ENGINE_COUNT];
static int shipped_enabled_captured = 0;

static void capture_shipped_enabled(void) {
  if (shipped_enabled_captured)
    return;
  for (int i = 0; i < ENGINE_COUNT; i++)
    shipped_enabled[i] = ENGINE_REGISTRY[i].enabled;
  shipped_enabled_captured = 1;
}

void apply_engines_config(const char *engines_str) {
  capture_shipped_enabled();
  if (!engines_str || engines_str[0] == '\0') {
    for (int i = 0; i < ENGINE_COUNT; i++)
      ENGINE_REGISTRY[i].enabled = shipped_enabled[i];
    return;
  }

  for (int i = 0; i < ENGINE_COUNT; i++) {
    ENGINE_REGISTRY[i].enabled = 0;
  }

  char *copy = strdup(engines_str);
  if (!copy)
    return;

  char *saveptr;
  char *token = strtok_r(copy, ",", &saveptr);

  while (token) {
    while (*token == ' ' || *token == '\t')
      token++;

    if (strcmp(token, "*") == 0) {
      for (int i = 0; i < ENGINE_COUNT; i++)
        ENGINE_REGISTRY[i].enabled = shipped_enabled[i];
    } else if (token[0] == '-' && token[1] != '\0') {
      char *engine_id = token + 1;
      int found = 0;
      for (int i = 0; i < ENGINE_COUNT; i++) {
        if (engine_id_compare(ENGINE_REGISTRY[i].id, engine_id)) {
          ENGINE_REGISTRY[i].enabled = 0;
          found = 1;
          break;
        }
      }
      if (!found) {
        fprintf(stderr, "[WARN] Unknown engine: %s\n", engine_id);
      }
    } else {
      int found = 0;
      for (int i = 0; i < ENGINE_COUNT; i++) {
        if (engine_id_compare(ENGINE_REGISTRY[i].id, token)) {
          ENGINE_REGISTRY[i].enabled = 1;
          found = 1;
          break;
        }
      }
      if (!found) {
        fprintf(stderr, "[WARN] Unknown engine: %s\n", token);
      }
    }

    token = strtok_r(NULL, ",", &saveptr);
  }

  free(copy);
}
