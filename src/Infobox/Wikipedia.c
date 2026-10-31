#include "Wikipedia.h"
#include "../Cache/Cache.h"
#include "../Scraping/Scraping.h"
#include "../Utility/HttpClient.h"
#include "Config.h"
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void shorten_summary(char **extract_ptr, int max_chars) {
  if (!extract_ptr || !*extract_ptr)
    return;

  char *text = *extract_ptr;
  int len = strlen(text);

  if (len <= max_chars)
    return;

  int end_pos = max_chars;
  for (int i = max_chars; i > (max_chars / 2); i--) {
    if (text[i] == '.' || text[i] == '!' || text[i] == '?') {
      end_pos = i + 1;
      break;
    }
  }

  char *new_text = (char *)malloc(end_pos + 4);

  if (new_text) {
    strncpy(new_text, text, end_pos);
    new_text[end_pos] = '\0';
    strcat(new_text, "...");
    free(*extract_ptr);
    *extract_ptr = new_text;
  }
}

static void extract_wiki_info(xmlNode *node, InfoBox *info) {
  xmlNode *cur_node = NULL;

  for (cur_node = node; cur_node; cur_node = cur_node->next) {
    if (cur_node->type == XML_ELEMENT_NODE) {
      if (strcmp((const char *)cur_node->name, "page") == 0) {
        xmlChar *title = xmlGetProp(cur_node, (const xmlChar *)"title");
        if (title) {
          if (info->title) {
            free(info->title);
          }
          info->title = strdup((const char *)title);

          const char *base_article_url = "https://en.wikipedia.org/wiki/";
          char *formatted_title = strdup((const char *)title);
          for (int i = 0; formatted_title[i]; i++) {
            if (formatted_title[i] == ' ')
              formatted_title[i] = '_';
          }

          if (info->url) {
            free(info->url);
          }
          info->url =
              malloc(strlen(base_article_url) + strlen(formatted_title) + 1);
          if (info->url) {
            strcpy(info->url, base_article_url);
            strcat(info->url, formatted_title);
          }
          free(formatted_title);
          xmlFree(title);
        }
      }

      if (strcmp((const char *)cur_node->name, "thumbnail") == 0) {
        xmlChar *source = xmlGetProp(cur_node, (const xmlChar *)"source");
        if (source) {
          if (info->thumbnail_url) {
            free(info->thumbnail_url);
          }
          info->thumbnail_url = strdup((const char *)source);
          xmlFree(source);
        }
      }

      if (strcmp((const char *)cur_node->name, "extract") == 0) {
        xmlChar *content = xmlNodeGetContent(cur_node);
        if (content) {
          if (info->extract) {
            free(info->extract);
          }
          info->extract = strdup((const char *)content);

          shorten_summary(&(info->extract), WIKI_SUMMARY_MAX_CHARS);
          xmlFree(content);
        }
      }
    }
    extract_wiki_info(cur_node->children, info);
  }
}

InfoBox fetch_wiki_data(char *api_url) {
  InfoBox info = {NULL, NULL, NULL, NULL};

  if (!api_url) {
    return info;
  }

  char *cache_key = cache_compute_key(api_url, 0, "wikipedia");
  if (cache_key && get_cache_ttl_infobox() > 0) {
    char *cached_data = NULL;
    size_t cached_size = 0;
    if (cache_get(cache_key, get_cache_ttl_infobox(), &cached_data,
                  &cached_size) == 0 &&
        cached_data && cached_size > 0) {
      xmlDocPtr doc =
          xmlReadMemory(cached_data, cached_size, NULL, NULL,
                        XML_PARSE_RECOVER | XML_PARSE_NOERROR |
                            XML_PARSE_NOWARNING);
      if (doc != NULL) {
        xmlNode *root_element = xmlDocGetRootElement(doc);
        extract_wiki_info(root_element, &info);
        xmlFreeDoc(doc);
      }
      free(cached_data);
      free(cache_key);
      return info;
    }
    free(cached_data);
  }
  free(cache_key);

  HttpResponse resp =
      http_get_budget(api_url, "libcurl-agent/1.0", INFOBOX_BUDGET_MS);
  if (resp.memory && resp.size > 0) {
    cache_key = cache_compute_key(api_url, 0, "wikipedia");
    if (cache_key && get_cache_ttl_infobox() > 0) {
      cache_set(cache_key, resp.memory, resp.size);
    }
    free(cache_key);

    xmlDocPtr doc =
        xmlReadMemory(resp.memory, resp.size, NULL, NULL,
                      XML_PARSE_RECOVER | XML_PARSE_NOERROR |
                          XML_PARSE_NOWARNING);
    if (doc != NULL) {
      xmlNode *root_element = xmlDocGetRootElement(doc);
      extract_wiki_info(root_element, &info);
      xmlFreeDoc(doc);
    }
  }

  http_response_free(&resp);
  return info;
}

/* One request instead of two: the search generator returns the best matching
   article together with its intro extract and thumbnail. The old code did a
   title lookup and then a second fetch for the extract. */
char *construct_wiki_url_lang(const char *search_term, const char *lang) {
  if (!search_term || search_term[0] == '\0')
    return NULL;

  char *escaped_term = curl_easy_escape(NULL, search_term, 0);
  if (!escaped_term)
    return NULL;

  const char *wiki_lang = (lang && lang[0]) ? lang : "en";
  char base[256];
  snprintf(base, sizeof(base),
           "https://%s.wikipedia.org/w/api.php?action=query&generator=search"
           "&gsrnamespace=0&gsrlimit=1&gsrsearch=",
           wiki_lang);
  const char *suffix = "&prop=extracts|pageimages&exintro&explaintext"
                       "&pithumbsize=400&format=xml&origin=*&redirects=1";

  char *full_url =
      malloc(strlen(base) + strlen(escaped_term) + strlen(suffix) + 1);
  if (full_url) {
    strcpy(full_url, base);
    strcat(full_url, escaped_term);
    strcat(full_url, suffix);
  }

  curl_free(escaped_term);
  return full_url;
}

char *construct_wiki_url(const char *search_term) {
  return construct_wiki_url_lang(search_term, "en");
}
