#include "ImageScraping.h"
#include "../Utility/HttpClient.h"
#include "Config.h"
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *build_proxy_url(const char *image_url) {
  if (!image_url)
    return NULL;

  char *proxy_url = NULL;
  CURL *curl = curl_easy_init();
  if (curl) {
    char *encoded = curl_easy_escape(curl, (char *)image_url, 0);
    if (encoded) {
      size_t len = strlen("/proxy?url=") + strlen(encoded) + 1;
      proxy_url = malloc(len);
      if (proxy_url)
        snprintf(proxy_url, len, "/proxy?url=%s", encoded);
      curl_free(encoded);
    }
    curl_easy_cleanup(curl);
  }

  return proxy_url;
}

static char *extract_json_string(const char *json, const char *key) {
  if (!json || !key)
    return NULL;

  char search_key[64];
  snprintf(search_key, sizeof(search_key), "\"%s\"", key);

  const char *key_pos = strstr(json, search_key);
  if (!key_pos)
    return NULL;

  const char *colon = strchr(key_pos + strlen(search_key), ':');
  if (!colon)
    return NULL;

  colon++;
  while (*colon == ' ' || *colon == '\t' || *colon == '\n' || *colon == '\r')
    colon++;

  if (*colon != '"')
    return NULL;
  colon++;

  size_t len = 0;
  const char *start = colon;
  while (*colon && *colon != '"') {
    if (*colon == '\\' && *(colon + 1))
      colon++;
    colon++;
    len++;
  }

  char *result = malloc(len + 1);
  if (!result)
    return NULL;

  colon = start;
  size_t i = 0;
  while (*colon && *colon != '"') {
    if (*colon == '\\' && *(colon + 1))
      colon++;
    result[i++] = *colon++;
  }
  result[i] = '\0';

  return result;
}

static int parse_iusc_node(xmlNodePtr node, ImageResult *result) {
  xmlChar *m_attr = xmlGetProp(node, (const xmlChar *)"m");
  if (!m_attr)
    return 0;

  char *turl = extract_json_string((const char *)m_attr, "turl");
  char *murl = extract_json_string((const char *)m_attr, "murl");
  char *purl = extract_json_string((const char *)m_attr, "purl");
  char *title = extract_json_string((const char *)m_attr, "t");

  int ok = (turl != NULL && strlen(turl) > 0);
  if (ok) {
    char *proxy_url = build_proxy_url(turl);
    result->thumbnail_url = proxy_url ? strdup(proxy_url) : strdup(turl);
    free(proxy_url);
    result->title =
        title && strlen(title) > 0 ? strdup(title) : strdup("Image");
    result->page_url = purl && strlen(purl) > 0 ? strdup(purl) : strdup("#");
    result->full_url = murl && strlen(murl) > 0 ? strdup(murl) : strdup("#");
  }

  free(turl);
  free(murl);
  free(purl);
  free(title);

  xmlFree(m_attr);
  return ok;
}

int scrape_images(const char *query, int page, ImageResult **out_results,
                  int *out_count) {
  *out_results = NULL;
  *out_count = 0;

  if (!query || strlen(query) == 0)
    return -1;

  CURL *tmp = curl_easy_init();
  if (!tmp)
    return -1;

  char *encoded_query = curl_easy_escape(tmp, query, 0);
  curl_easy_cleanup(tmp);

  if (!encoded_query)
    return -1;

  char url[BUFFER_SIZE_LARGE];
  int first = (page - 1) * IMAGE_RESULTS_PER_PAGE + 1;
  snprintf(
      url, sizeof(url),
      "https://www.bing.com/images/async?q=%s&async=content&first=%d&count=%d",
      encoded_query, first, 35);
  free(encoded_query);

  HttpResponse resp = http_get(
      url,
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
      "like Gecko) Chrome/120.0.0.0 Safari/537.36");
  if (!resp.memory) {
    return -1;
  }

  htmlDocPtr doc = htmlReadMemory(resp.memory, resp.size, NULL, NULL,
                                  HTML_PARSE_RECOVER | HTML_PARSE_NOERROR);
  if (!doc) {
    http_response_free(&resp);
    return -1;
  }

  xmlXPathContextPtr xpathCtx = xmlXPathNewContext(doc);
  if (!xpathCtx) {
    xmlFreeDoc(doc);
    http_response_free(&resp);
    return -1;
  }

  xmlXPathObjectPtr xpathObj =
      xmlXPathEvalExpression((const xmlChar *)"//a[@class='iusc']", xpathCtx);

  if (!xpathObj || !xpathObj->nodesetval) {
    if (xpathObj)
      xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);
    http_response_free(&resp);
    return 0;
  }

  int nodes = xpathObj->nodesetval->nodeNr;
  int max_images =
      (nodes < IMAGE_RESULTS_PER_PAGE) ? nodes : IMAGE_RESULTS_PER_PAGE;

  ImageResult *results = malloc(sizeof(ImageResult) * max_images);
  if (!results) {
    xmlXPathFreeObject(xpathObj);
    xmlXPathFreeContext(xpathCtx);
    xmlFreeDoc(doc);
    http_response_free(&resp);
    return -1;
  }

  int count = 0;
  for (int i = 0; i < nodes && count < IMAGE_RESULTS_PER_PAGE; i++) {
    xmlNodePtr node = xpathObj->nodesetval->nodeTab[i];
    if (parse_iusc_node(node, &results[count])) {
      count++;
    }
  }

  xmlXPathFreeObject(xpathObj);
  xmlXPathFreeContext(xpathCtx);
  xmlFreeDoc(doc);
  http_response_free(&resp);

  *out_results = results;
  *out_count = count;
  return 0;
}

void free_image_results(ImageResult *results, int count) {
  if (!results)
    return;

  for (int i = 0; i < count; i++) {
    free(results[i].thumbnail_url);
    free(results[i].title);
    free(results[i].page_url);
    free(results[i].full_url);
  }
  free(results);
}
