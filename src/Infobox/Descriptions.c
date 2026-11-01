#include "Descriptions.h"
#include "../Cache/Cache.h"
#include "../Routes/ImageProxy.h"
#include "../Utility/Display.h"
#include "../Utility/Unescape.h"
#include "../Utility/Utility.h"
#include <ctype.h>
#include <libxml/HTMLparser.h>
#include <libxml/xpath.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DESC_BUDGET_MS 600
#define DESC_MAX_BYTES (64 * 1024)
#define DESC_MAX_LEN 200
#define DESC_CACHE_TTL (7 * 24 * 3600)

/* collapse whitespace, decode entities, and cut at 200 characters on a word
   boundary rather than mid-word */
static void tidy_text(const char *in, char *out, size_t cap) {
  char collapsed[1024];
  size_t used = 0;
  int space = 0;
  for (const char *c = in; *c && used + 1 < sizeof(collapsed); c++) {
    if (*c == '\n' || *c == '\r' || *c == '\t' || *c == ' ') {
      space = 1;
      continue;
    }
    if (space && used > 0)
      collapsed[used++] = ' ';
    space = 0;
    collapsed[used++] = *c;
  }
  collapsed[used] = '\0';
  char decoded[1024];
  unescape_html_entities(collapsed, decoded, sizeof(decoded));

  size_t len = strlen(decoded);
  if (len > DESC_MAX_LEN) {
    size_t cut = DESC_MAX_LEN;
    while (cut > 0 && decoded[cut] != ' ')
      cut--;
    if (cut < DESC_MAX_LEN / 2)
      cut = DESC_MAX_LEN;
    decoded[cut] = '\0';
    len = strlen(decoded);
    while (len > 0 && (decoded[len - 1] == ' ' || decoded[len - 1] == ','))
      decoded[--len] = '\0';
  }
  snprintf(out, cap, "%s", decoded);
}

static char *xpath_first(xmlDocPtr doc, const char *xpath) {
  if (!doc)
    return NULL;
  xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
  if (!ctx)
    return NULL;
  xmlXPathObjectPtr obj = xmlXPathEvalExpression((const xmlChar *)xpath, ctx);
  char *out = NULL;
  if (obj && obj->type == XPATH_NODESET && obj->nodesetval &&
      obj->nodesetval->nodeNr > 0) {
    xmlNodePtr node = obj->nodesetval->nodeTab[0];
    xmlChar *content = xmlNodeGetContent(node);
    if (content) {
      out = strdup((const char *)content);
      xmlFree(content);
    }
  }
  if (obj)
    xmlXPathFreeObject(obj);
  xmlXPathFreeContext(ctx);
  return out;
}

int description_extract(const char *html, size_t len, char *out, size_t cap) {
  if (!html || !out || cap == 0)
    return 0;
  out[0] = '\0';
  xmlDocPtr doc = htmlReadMemory(html, (int)len, NULL, NULL,
                                 HTML_PARSE_RECOVER | HTML_PARSE_NOERROR |
                                     HTML_PARSE_NOWARNING | HTML_PARSE_NOBLANKS);
  if (!doc)
    return 0;

  /* a page can say "do not quote me": nosnippet, or max-snippet:0 */
  {
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (ctx) {
      xmlXPathObjectPtr obj = xmlXPathEvalExpression(
          (const xmlChar *)"//meta[@name='robots']/@content", ctx);
      if (obj && obj->type == XPATH_NODESET && obj->nodesetval &&
          obj->nodesetval->nodeNr > 0) {
        xmlChar *content = xmlNodeGetContent(obj->nodesetval->nodeTab[0]);
        if (content) {
          char lowered[256];
          snprintf(lowered, sizeof(lowered), "%s", (const char *)content);
          for (char *c = lowered; *c; c++)
            if (*c >= 'A' && *c <= 'Z')
              *c = (char)(*c - 'A' + 'a');
          int denied = strstr(lowered, "nosnippet") != NULL ||
                       strstr(lowered, "max-snippet:0") != NULL ||
                       strstr(lowered, "max-snippet: 0") != NULL;
          xmlFree(content);
          if (denied) {
            if (obj)
              xmlXPathFreeObject(obj);
            xmlXPathFreeContext(ctx);
            xmlFreeDoc(doc);
            return 0;
          }
        }
      }
      if (obj)
        xmlXPathFreeObject(obj);
      xmlXPathFreeContext(ctx);
    }
  }

  /* the order is the one a reader would pick: what the site says it is for a
     social card, then its own description, then its opening paragraph */
  static const char *XPATH[] = {
      "//meta[@property='og:description']/@content",
      "//meta[@name='description']/@content",
      "//meta[@property='twitter:description']/@content",
  };
  for (size_t i = 0; i < sizeof(XPATH) / sizeof(XPATH[0]); i++) {
    xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
    if (!ctx)
      continue;
    xmlXPathObjectPtr obj =
        xmlXPathEvalExpression((const xmlChar *)XPATH[i], ctx);
    if (obj && obj->type == XPATH_NODESET && obj->nodesetval &&
        obj->nodesetval->nodeNr > 0) {
      xmlChar *content = xmlNodeGetContent(obj->nodesetval->nodeTab[0]);
      if (content) {
        char tidy[1024];
        tidy_text((const char *)content, tidy, sizeof(tidy));
        if (strlen(tidy) > 20)
          snprintf(out, cap, "%s", tidy);
        xmlFree(content);
      }
    }
    if (obj)
      xmlXPathFreeObject(obj);
    xmlXPathFreeContext(ctx);
    if (out[0])
      break;
  }

  if (!out[0]) {
    /* the first paragraph long enough to be a description */
    char *para = xpath_first(doc, "//p[not(ancestor::nav) and "
                                  "not(ancestor::footer)]");
    if (para) {
      char tidy[1024];
      tidy_text(para, tidy, sizeof(tidy));
      if (strlen(tidy) >= 60)
        snprintf(out, cap, "%s", tidy);
      free(para);
    }
  }
  xmlFreeDoc(doc);
  return out[0] ? 1 : 0;
}

int robots_allows(const char *robots_txt, const char *path) {
  if (!robots_txt || !path)
    return 1;
  /* a deliberately small reader: the groups that name us or "*" decide, and a
     Disallow whose prefix matches the path is a no. Anything else is allowed,
     which is what the standard says when no group matches. */
  int in_ours = 0, in_any = 0, any_seen = 0;
  char line[512];
  const char *cursor = robots_txt;
  while (*cursor) {
    size_t n = 0;
    while (*cursor && *cursor != '\n' && n + 1 < sizeof(line))
      line[n++] = *cursor++;
    if (*cursor == '\n')
      cursor++;
    line[n] = '\0';
    char *hash = strchr(line, '#');
    if (hash)
      *hash = '\0';
    char *colon = strchr(line, ':');
    if (!colon)
      continue;
    *colon = '\0';
    char *key = line;
    while (*key == ' ' || *key == '\t')
      key++;
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t')
      value++;
    size_t vlen = strlen(value);
    while (vlen > 0 && (value[vlen - 1] == ' ' || value[vlen - 1] == '\r'))
      value[--vlen] = '\0';

    if (strcasecmp(key, "user-agent") == 0) {
      in_ours = strcasestr(value, "omnisearch") != NULL;
      in_any = strcmp(value, "*") == 0;
      if (in_any)
        any_seen = 1;
      continue;
    }
    if (strcasecmp(key, "disallow") == 0 && value[0] && (in_ours || in_any)) {
      size_t plen = strlen(value);
      if (strncmp(path, value, plen) == 0)
        return 0;
    }
    if (strcasecmp(key, "allow") == 0 && value[0] && (in_ours || in_any)) {
      size_t plen = strlen(value);
      if (strncmp(path, value, plen) == 0)
        return 1;
    }
  }
  (void)any_seen;
  return 1;
}

int description_for_url(const char *url, char *out, size_t cap) {
  if (!url || !out || cap == 0)
    return 0;
  out[0] = '\0';

  char cache_key[512];
  snprintf(cache_key, sizeof(cache_key), "desc:v1:%s", url);
  char *cached = NULL;
  size_t cached_len = 0;
  if (cache_get(cache_key, DESC_CACHE_TTL, &cached, &cached_len) == 0 &&
      cached) {
    int found = cached_len > 0 && cached[0] == '1';
    if (found && cached_len > 2)
      snprintf(out, cap, "%s", cached + 2);
    free(cached);
    return found;
  }
  free(cached);

  /* robots first: a site that disallows us gets no request at all */
  char scheme[8] = "https", host[256] = "", path[512] = "/";
  {
    const char *p = strstr(url, "://");
    const char *rest = url;
    if (p) {
      size_t slen = (size_t)(p - url);
      if (slen < sizeof(scheme)) {
        memcpy(scheme, url, slen);
        scheme[slen] = '\0';
      }
      rest = p + 3;
    }
    const char *slash = strchr(rest, '/');
    size_t hlen = slash ? (size_t)(slash - rest) : strlen(rest);
    if (hlen >= sizeof(host))
      return 0;
    memcpy(host, rest, hlen);
    host[hlen] = '\0';
    if (slash)
      snprintf(path, sizeof(path), "%s", slash);
  }
  if (!host[0])
    return 0;

  char robots_url[600];
  snprintf(robots_url, sizeof(robots_url), "%s://%s/robots.txt", scheme, host);
  char robots_key[600];
  snprintf(robots_key, sizeof(robots_key), "robots:v1:%s", host);
  char *robots = NULL;
  size_t robots_len = 0;
  if (cache_get(robots_key, 24 * 3600, &robots, &robots_len) != 0 || !robots) {
    free(robots);
    robots = NULL;
    char *body = NULL;
    size_t body_len = 0;
    long status = 0;
    if (proxy_guarded_get(robots_url, DESC_BUDGET_MS, 32 * 1024, &body,
                          &body_len, &status) == 0) {
      robots = body;
      if (status >= 200 && status < 300)
        cache_set(robots_key, body ? body : "", body ? body_len : 0);
    }
  }
  int allowed = robots_allows(robots, path);
  free(robots);
  if (!allowed) {
    cache_set(cache_key, "0", 1);
    return 0;
  }

  char *body = NULL;
  size_t body_len = 0;
  long status = 0;
  if (proxy_guarded_get(url, DESC_BUDGET_MS, DESC_MAX_BYTES, &body, &body_len,
                        &status) != 0 ||
      !body) {
    free(body);
    cache_set(cache_key, "0", 1);
    return 0;
  }
  int found = description_extract(body, body_len, out, cap);
  free(body);
  if (found) {
    size_t need = strlen(out) + 3;
    char *entry = malloc(need);
    if (entry) {
      snprintf(entry, need, "1 %s", out);
      cache_set(cache_key, entry, strlen(entry));
      free(entry);
    }
  } else {
    cache_set(cache_key, "0", 1);
  }
  return found;
}

/* base64url without padding: the URL travels in a query parameter, so it must
   not contain anything a browser would escape */
static void b64url_encode(const char *in, char *out, size_t cap) {
  static const char *ALPHABET =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t o = 0;
  const unsigned char *p = (const unsigned char *)in;
  size_t len = strlen(in);
  for (size_t i = 0; i + 2 < len && o + 5 < cap; i += 3) {
    out[o++] = ALPHABET[p[i] >> 2];
    out[o++] = ALPHABET[((p[i] & 3) << 4) | (p[i + 1] >> 4)];
    out[o++] = ALPHABET[((p[i + 1] & 15) << 2) | (p[i + 2] >> 6)];
    out[o++] = ALPHABET[p[i + 2] & 63];
  }
  size_t rem = len % 3;
  if (rem == 1 && o + 3 < cap) {
    out[o++] = ALPHABET[p[len - 1] >> 2];
    out[o++] = ALPHABET[(p[len - 1] & 3) << 4];
  } else if (rem == 2 && o + 4 < cap) {
    const unsigned char *t = p + len - 2;
    out[o++] = ALPHABET[t[0] >> 2];
    out[o++] = ALPHABET[((t[0] & 3) << 4) | (t[1] >> 4)];
    out[o++] = ALPHABET[(t[1] & 15) << 2];
  }
  out[o] = '\0';
}

static int b64url_decode(const char *in, char *out, size_t cap) {
  static const char ALPHABET[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  int lookup[256];
  for (int i = 0; i < 256; i++)
    lookup[i] = -1;
  for (int i = 0; i < 64; i++)
    lookup[(unsigned char)ALPHABET[i]] = i;
  size_t o = 0;
  int bits = 0, value = 0;
  for (const char *c = in; *c; c++) {
    int v = lookup[(unsigned char)*c];
    if (v < 0)
      return 0;
    value = (value << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (o + 1 >= cap)
        return 0;
      out[o++] = (char)((value >> bits) & 0xFF);
    }
  }
  out[o] = '\0';
  return o > 0;
}

char *description_href(const char *url) {
  if (!url || !url[0])
    return NULL;
  char encoded[1024];
  b64url_encode(url, encoded, sizeof(encoded));
  if (!encoded[0])
    return NULL;
  char *signature = proxy_sign_token(encoded);
  if (!signature)
    return NULL;
  size_t need = strlen("/d?u=") + strlen(encoded) + strlen("&s=") +
                strlen(signature) + 1;
  char *href = malloc(need);
  if (href)
    snprintf(href, need, "/d?u=%s&s=%s", encoded, signature);
  free(signature);
  return href;
}

int description_from_token(const char *encoded_url, const char *signature,
                           char *out, size_t cap) {
  if (!encoded_url || !signature || !out || cap == 0)
    return 0;
  out[0] = '\0';
  if (!proxy_verify_token(encoded_url, signature))
    return 0;
  char url[1024];
  if (!b64url_decode(encoded_url, url, sizeof(url)))
    return 0;
  return description_for_url(url, out, cap);
}
