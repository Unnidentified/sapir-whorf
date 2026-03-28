#include "ImageProxy.h"
#include "../Cache/Cache.h"
#include "../Proxy/Proxy.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <curl/urlapi.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#define MAX_IMAGE_SIZE (10 * 1024 * 1024)

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} MemoryBuffer;

static int is_private_ip(const char *ip_str) {
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_str, &addr) != 1) {
    return 0;
  }

  uint32_t ip = ntohl(addr.s_addr);

  // 10.0.0.0/8
  if ((ip >> 24) == 10) {
    return 1;
  }

  // 172.16.0.0/12
  if ((ip >> 20) == 0xAC) {
    uint8_t second = (ip >> 16) & 0xFF;
    if (second >= 16 && second <= 31) {
      return 1;
    }
  }

  // 192.168.0.0/16
  if ((ip >> 16) == 0xC0A8) {
    return 1;
  }

  // 127.0.0.0/8
  if ((ip >> 24) == 127) {
    return 1;
  }

  // 169.254.0.0/16
  if ((ip >> 16) == 0xA9FE) {
    return 1;
  }

  return 0;
}

static int is_private_hostname(const char *hostname) {
  struct addrinfo hints, *res, *p;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  int err = getaddrinfo(hostname, NULL, &hints, &res);
  if (err != 0) {
    return 0;
  }

  for (p = res; p != NULL; p = p->ai_next) {
    if (p->ai_family == AF_INET) {
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
      char ip_str[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, INET_ADDRSTRLEN);

      if (is_private_ip(ip_str)) {
        freeaddrinfo(res);
        return 1;
      }
    }
  }

  freeaddrinfo(res);
  return 0;
}

static int is_allowed_domain(const char *url) {
  CURLU *h = curl_url();
  if (!h) {
    return -1;
  }

  curl_url_set(h, CURLUPART_URL, url, 0);

  char *scheme = NULL;
  curl_url_get(h, CURLUPART_SCHEME, &scheme, 0);

  int valid_scheme = 0;
  if (scheme && (strcasecmp(scheme, "http") == 0 || strcasecmp(scheme, "https") == 0)) {
    valid_scheme = 1;
  }

  if (scheme)
    curl_free(scheme);

  if (!valid_scheme) {
    curl_url_cleanup(h);
    return -1;
  }

  const char *protocol = strstr(url, "://");
  if (!protocol) {
    protocol = url;
  } else {
    protocol += 3;
  }

  const char *path = strchr(protocol, '/');
  size_t host_len = path ? (size_t)(path - protocol) : strlen(protocol);

  char host[256] = {0};
  if (host_len >= sizeof(host)) {
    host_len = sizeof(host) - 1;
  }
  strncpy(host, protocol, host_len);

  char *colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
  }

  if (is_private_hostname(host)) {
    curl_url_cleanup(h);
    return 0;
  }

  curl_url_cleanup(h);
  return 1;
}

static size_t write_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  size_t realsize = size * nmemb;
  MemoryBuffer *buf = (MemoryBuffer *)userp;

  if (buf->size + realsize > MAX_IMAGE_SIZE) {
    return 0;
  }

  if (buf->size + realsize > buf->capacity) {
    size_t new_capacity = buf->capacity * 2;
    if (new_capacity < buf->size + realsize) {
      new_capacity = buf->size + realsize;
    }
    char *new_data = realloc(buf->data, new_capacity);
    if (!new_data)
      return 0;
    buf->data = new_data;
    buf->capacity = new_capacity;
  }

  memcpy(buf->data + buf->size, contents, realsize);
  buf->size += realsize;
  return realsize;
}

static char *url_encode_key(const char *url) {
  char *hash = malloc(33);
  if (!hash)
    return NULL;

  unsigned char md5hash[16];
  EVP_MD_CTX *ctx = EVP_MD_CTX_new();
  if (!ctx) {
    free(hash);
    return NULL;
  }

  EVP_DigestInit_ex(ctx, EVP_md5(), NULL);
  EVP_DigestUpdate(ctx, url, strlen(url));
  EVP_DigestFinal_ex(ctx, md5hash, NULL);
  EVP_MD_CTX_free(ctx);

  for (int i = 0; i < 16; i++) {
    sprintf(hash + (i * 2), "%02x", md5hash[i]);
  }
  hash[32] = '\0';

  return hash;
}

int image_proxy_handler(UrlParams *params) {
  const char *url = NULL;
  for (int i = 0; i < params->count; i++) {
    if (strcmp(params->params[i].key, "url") == 0) {
      url = params->params[i].value;
      break;
    }
  }

  if (!url || strlen(url) == 0) {
    send_response("Missing 'url' parameter");
    return 0;
  }

  int domain_check = is_allowed_domain(url);
  if (domain_check == -1) {
    send_response("Invalid URL scheme");
    return 0;
  }
  if (domain_check == 0) {
    send_response("Private addresses are not allowed");
    return 0;
  }

  char *cache_key = url_encode_key(url);
  if (!cache_key) {
    send_response("Failed to generate cache key");
    return 0;
  }

  char *cached_data = NULL;
  size_t cached_size = 0;
  int cache_ttl = get_cache_ttl_image();

  if (cache_get(cache_key, cache_ttl, &cached_data, &cached_size) == 0) {
    char content_type[64] = {0};

    const char *ext = strrchr(url, '.');
    if (ext) {
      if (strcasecmp(ext, ".png") == 0) {
        strncpy(content_type, "image/png", sizeof(content_type) - 1);
      } else if (strcasecmp(ext, ".gif") == 0) {
        strncpy(content_type, "image/gif", sizeof(content_type) - 1);
      } else if (strcasecmp(ext, ".webp") == 0) {
        strncpy(content_type, "image/webp", sizeof(content_type) - 1);
      } else if (strcasecmp(ext, ".svg") == 0) {
        strncpy(content_type, "image/svg+xml", sizeof(content_type) - 1);
      } else if (strcasecmp(ext, ".ico") == 0) {
        strncpy(content_type, "image/x-icon", sizeof(content_type) - 1);
      } else if (strcasecmp(ext, ".bmp") == 0) {
        strncpy(content_type, "image/bmp", sizeof(content_type) - 1);
      }
    }

    if (strlen(content_type) == 0) {
      strncpy(content_type, "image/jpeg", sizeof(content_type) - 1);
    }

    serve_data(cached_data, cached_size, content_type);
    free(cached_data);
    free(cache_key);
    return 0;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    free(cache_key);
    send_response("Failed to initialize curl");
    return 0;
  }

  MemoryBuffer buf = {.data = malloc(8192), .size = 0, .capacity = 8192};

  if (!buf.data) {
    curl_easy_cleanup(curl);
    free(cache_key);
    send_response("Memory allocation failed");
    return 0;
  }

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/120.0.0.0 Safari/537.36");
  apply_proxy_settings(curl);

  CURLcode res = curl_easy_perform(curl);

  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  char *content_type_ptr = NULL;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_ptr);

  char content_type[64] = {0};
  if (content_type_ptr) {
    strncpy(content_type, content_type_ptr, sizeof(content_type) - 1);
  }

  curl_easy_cleanup(curl);

  if (res != CURLE_OK || response_code != 200) {
    free(buf.data);
    free(cache_key);
    send_response("Failed to fetch image");
    return 0;
  }

  if (strlen(content_type) == 0 ||
      strncmp(content_type, "image/", 6) != 0) {
    free(buf.data);
    free(cache_key);
    send_response("Invalid content type");
    return 0;
  }

  const char *mime_type =
      strlen(content_type) > 0 ? content_type : "image/jpeg";

  cache_set(cache_key, buf.data, buf.size);

  serve_data(buf.data, buf.size, mime_type);

  free(buf.data);
  free(cache_key);
  return 0;
}
