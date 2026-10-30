#include "ImageProxy.h"
#include "../Cache/Cache.h"
#include "../Proxy/Proxy.h"
#include <arpa/inet.h>
#include <curl/curl.h>
#include <curl/urlapi.h>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#define MAX_IMAGE_SIZE (10 * 1024 * 1024)
#define DNS_CACHE_TTL 300
#define MAX_IMAGE_REDIRECTS 3
/* cached bodies are stored as "mime:type\n" + raw bytes */
#define IMG_MIME_MAX 64

static int is_private_ip(const char *ip_str);

/* Validating only the initial host left redirects and DNS rebinding open: a
   public URL that 302s to 127.0.0.1 was fetched happily. This callback runs on
   the real socket for every hop (including redirects), so the address that is
   about to be connected to is the address that gets checked. */
static curl_socket_t guard_opensocket(void *clientp, curlsocktype purpose,
                                      struct curl_sockaddr *address) {
  (void)clientp;
  (void)purpose;
  char ip[INET6_ADDRSTRLEN] = {0};

  if (address->family == AF_INET) {
    struct sockaddr_in *v4 = (struct sockaddr_in *)&address->addr;
    inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof(ip));
  } else if (address->family == AF_INET6) {
    struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address->addr;
    inet_ntop(AF_INET6, &v6->sin6_addr, ip, sizeof(ip));
  } else {
    return CURL_SOCKET_BAD;
  }

  if (is_private_ip(ip)) {
    return CURL_SOCKET_BAD;
  }

  curl_socket_t sock = socket(address->family, address->socktype,
                              address->protocol);
  if (sock == CURL_SOCKET_BAD)
    return CURL_SOCKET_BAD;
  return sock;
}

typedef struct DnsCacheEntry {
  char hostname[256];
  char ip_str[INET6_ADDRSTRLEN];
  time_t resolved_at;
  struct DnsCacheEntry *next;
} DnsCacheEntry;

static DnsCacheEntry *dns_cache = NULL;
static pthread_mutex_t dns_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
  char *data;
  size_t size;
  size_t capacity;
} MemoryBuffer;

static int dns_cache_lookup(const char *hostname, char *out_ip) {
  time_t now = time(NULL);
  pthread_mutex_lock(&dns_cache_mutex);
  for (DnsCacheEntry *e = dns_cache; e; e = e->next) {
    if (strcmp(e->hostname, hostname) == 0) {
      if ((now - e->resolved_at) < DNS_CACHE_TTL) {
        strcpy(out_ip, e->ip_str);
        pthread_mutex_unlock(&dns_cache_mutex);
        return 0;
      }
      break;
    }
  }
  pthread_mutex_unlock(&dns_cache_mutex);
  return -1;
}

static void dns_cache_insert(const char *hostname, const char *ip_str) {
  time_t now = time(NULL);
  pthread_mutex_lock(&dns_cache_mutex);

  DnsCacheEntry **cursor = &dns_cache;
  while (*cursor) {
    DnsCacheEntry *entry = *cursor;
    if ((now - entry->resolved_at) >= DNS_CACHE_TTL) {
      *cursor = entry->next;
      free(entry);
      continue;
    }
    if (strcmp(entry->hostname, hostname) == 0) {
      strcpy(entry->ip_str, ip_str);
      entry->resolved_at = now;
      pthread_mutex_unlock(&dns_cache_mutex);
      return;
    }
    cursor = &entry->next;
  }

  DnsCacheEntry *new_entry = malloc(sizeof(DnsCacheEntry));
  if (new_entry) {
    strncpy(new_entry->hostname, hostname, sizeof(new_entry->hostname) - 1);
    new_entry->hostname[sizeof(new_entry->hostname) - 1] = '\0';
    strcpy(new_entry->ip_str, ip_str);
    new_entry->resolved_at = now;
    new_entry->next = dns_cache;
    dns_cache = new_entry;
  }

  pthread_mutex_unlock(&dns_cache_mutex);
}

/* Anything that is not a global unicast address is refused. The old list only
   knew RFC1918 + loopback + link-local, so 0.0.0.0/8, CGNAT (100.64/10),
   benchmarking (198.18/15), multicast/reserved and every IPv6 form were
   reachable - including the cloud metadata address on some stacks. */
static int is_private_ipv6(const struct in6_addr *a) {
  const unsigned char *b = a->s6_addr;
  if (IN6_IS_ADDR_LOOPBACK(a) || IN6_IS_ADDR_UNSPECIFIED(a) ||
      IN6_IS_ADDR_LINKLOCAL(a) || IN6_IS_ADDR_SITELOCAL(a) ||
      IN6_IS_ADDR_MULTICAST(a)) {
    return 1;
  }
  /* unique local fc00::/7 */
  if ((b[0] & 0xFE) == 0xFC) {
    return 1;
  }
  /* IPv4-mapped ::ffff:a.b.c.d - judge the embedded v4 address */
  static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0,
                                           0, 0, 0xFF, 0xFF};
  if (memcmp(b, mapped, 12) == 0) {
    char v4[INET_ADDRSTRLEN];
    snprintf(v4, sizeof(v4), "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
    return is_private_ip(v4);
  }
  return 0;
}

static int is_private_ip(const char *ip_str) {
  struct in_addr addr;
  if (inet_pton(AF_INET, ip_str, &addr) != 1) {
    struct in6_addr v6;
    if (inet_pton(AF_INET6, ip_str, &v6) == 1)
      return is_private_ipv6(&v6);
    return 1; /* unparseable: refuse rather than guess */
  }

  /* Octets, not shifted masks: the previous >>22 / >>15 constants were wrong
     and matched nothing (198.18/15 was never caught). */
  const unsigned char *o = (const unsigned char *)&addr.s_addr;
  unsigned a = o[0], b = o[1], c = o[2];

  if (a == 0) return 1;                       /* 0.0.0.0/8 */
  if (a == 10) return 1;                      /* 10.0.0.0/8 */
  if (a == 127) return 1;                     /* 127.0.0.0/8 */
  if (a == 169 && b == 254) return 1;         /* 169.254.0.0/16 */
  if (a == 172 && b >= 16 && b <= 31) return 1; /* 172.16.0.0/12 */
  if (a == 192 && b == 168) return 1;         /* 192.168.0.0/16 */
  if (a == 100 && b >= 64 && b <= 127) return 1; /* 100.64.0.0/10 CGNAT */
  if (a == 198 && (b == 18 || b == 19)) return 1; /* 198.18.0.0/15 bench */
  if (a == 192 && b == 0 && c == 0) return 1;  /* 192.0.0.0/24 */
  if (a == 192 && b == 0 && c == 2) return 1;  /* 192.0.2.0/24 doc */
  if (a == 198 && b == 51 && c == 100) return 1; /* 198.51.100.0/24 doc */
  if (a == 203 && b == 0 && c == 113) return 1;  /* 203.0.113.0/24 doc */
  if (a >= 224) return 1;                     /* multicast + reserved */
  return 0;
}

static const char *is_private_hostname(const char *hostname, char *out_ip) {
  if (dns_cache_lookup(hostname, out_ip) == 0) {
    return out_ip;
  }

  struct addrinfo hints, *res, *p;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  int err = getaddrinfo(hostname, NULL, &hints, &res);
  if (err != 0) {
    return NULL;
  }

  /* Refuse if ANY returned address is private: a host with a public A record
     and a private AAAA record must not be reachable either way. */
  char chosen[INET6_ADDRSTRLEN] = {0};
  for (p = res; p != NULL; p = p->ai_next) {
    char ip_str[INET6_ADDRSTRLEN] = {0};
    if (p->ai_family == AF_INET) {
      struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
      inet_ntop(AF_INET, &(ipv4->sin_addr), ip_str, sizeof(ip_str));
    } else if (p->ai_family == AF_INET6) {
      struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
      inet_ntop(AF_INET6, &(ipv6->sin6_addr), ip_str, sizeof(ip_str));
    } else {
      continue;
    }

    if (is_private_ip(ip_str)) {
      freeaddrinfo(res);
      return NULL;
    }
    if (chosen[0] == '\0')
      snprintf(chosen, sizeof(chosen), "%s", ip_str);
  }

  freeaddrinfo(res);
  if (chosen[0] == '\0')
    return NULL;

  snprintf(out_ip, INET6_ADDRSTRLEN, "%s", chosen);
  dns_cache_insert(hostname, chosen);
  return out_ip;
}

static int is_allowed_domain(const char *url, char *resolved_ip) {
  CURLU *h = curl_url();
  if (!h) {
    return -1;
  }

  curl_url_set(h, CURLUPART_URL, url, 0);

  char *scheme = NULL;
  curl_url_get(h, CURLUPART_SCHEME, &scheme, 0);

  int valid_scheme = 0;
  int is_https = 0;
  if (scheme && (strcasecmp(scheme, "http") == 0 || strcasecmp(scheme, "https") == 0)) {
    valid_scheme = 1;
    is_https = (strcasecmp(scheme, "https") == 0);
  }

  if (scheme)
    curl_free(scheme);
  scheme = NULL;

  if (!valid_scheme) {
    curl_url_cleanup(h);
    return -1;
  }

  /* Host, port and userinfo all come from the CURLU handle that is used for
     the fetch. The previous hand parser (strstr("://"), strchr(':')) disagreed
     with curl on userinfo, IPv6 literals and explicit ports, so a URL could be
     validated as one host and fetched as another. */
  char *host = NULL;
  if (curl_url_get(h, CURLUPART_HOST, &host, 0) != CURLUE_OK || !host) {
    if (getenv("OMNI_PROXY_DEBUG")) fprintf(stderr,"[PROXYDBG] no host\n");
    curl_url_cleanup(h);
    return 0;
  }

  /* credentials in the URL are never needed for a thumbnail. curl_url_get
     requires a real out pointer: passing NULL is undefined, not a probe. */
  char *user = NULL;
  CURLUcode uc = curl_url_get(h, CURLUPART_USER, &user, 0);
  int has_user = (uc == CURLUE_OK && user && user[0]);
  if (user)
    curl_free(user);
  if (has_user) {
    if (getenv("OMNI_PROXY_DEBUG")) fprintf(stderr,"[PROXYDBG] userinfo\n");
    curl_free(host);
    curl_url_cleanup(h);
    return 0;
  }

  char *port_str = NULL;
  long port = 0;
  if (curl_url_get(h, CURLUPART_PORT, &port_str, 0) == CURLUE_OK && port_str) {
    port = strtol(port_str, NULL, 10);
    curl_free(port_str);
  } else {
    port = is_https ? 443 : 80;
  }
  if (port != 80 && port != 443) {
    if (getenv("OMNI_PROXY_DEBUG")) fprintf(stderr,"[PROXYDBG] port=%ld\n",port);
    curl_free(host);
    curl_url_cleanup(h);
    return 0;
  }

  int ok = 0;
  if (is_private_hostname(host, resolved_ip)) {
    ok = 1;
  }
  if (getenv("OMNI_PROXY_DEBUG"))
    fprintf(stderr, "[PROXYDBG] host=%s port=%ld ok=%d ip=%s\n", host, port, ok,
            resolved_ip[0] ? resolved_ip : "(none)");
  curl_free(host);
  curl_url_cleanup(h);
  return ok;
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


/* --- proxy URL signing -----------------------------------------------------
   Without this, /proxy is an open fetch relay for anyone who can reach the
   app. Result pages get links carrying an HMAC of the remote URL, keyed by a
   per-process secret, and the handler refuses anything unsigned. The proxy
   then fetches only URLs that this process itself put on a page. */
static unsigned char proxy_secret[32];
static pthread_once_t proxy_secret_once = PTHREAD_ONCE_INIT;

static void init_proxy_secret(void) {
  /* A fixed key is only for the test harness, which has to mint valid links.
     Production always uses a per-process random secret. */
  const char *fixed = getenv("OMNI_PROXY_TEST_KEY");
  if (fixed && strlen(fixed) > 0) {
    /* pad/truncate to the exact key length so the harness can reproduce it */
    memset(proxy_secret, 0, sizeof(proxy_secret));
    memcpy(proxy_secret, fixed,
           strlen(fixed) < sizeof(proxy_secret) ? strlen(fixed)
                                                : sizeof(proxy_secret));
    return;
  }
  if (RAND_bytes(proxy_secret, sizeof(proxy_secret)) != 1) {
    /* fall back to something unguessable-enough rather than running unsigned */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf((char *)proxy_secret, sizeof(proxy_secret), "%ld.%ld.%d",
             (long)ts.tv_sec, (long)ts.tv_nsec, getpid());
  }
}

static void proxy_hmac_hex(const char *url, char *out, size_t out_size) {
  pthread_once(&proxy_secret_once, init_proxy_secret);
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned int mac_len = 0;
  HMAC(EVP_sha256(), proxy_secret, (int)sizeof(proxy_secret),
       (const unsigned char *)url, strlen(url), mac, &mac_len);
  size_t n = mac_len < 16 ? mac_len : 16;
  if (out_size < n * 2 + 1)
    return;
  for (size_t i = 0; i < n; i++)
    sprintf(out + (i * 2), "%02x", mac[i]);
  out[n * 2] = '\0';
}

char *image_proxy_sign_url(const char *remote_url) {
  if (!remote_url)
    return NULL;
  char sig[64] = {0};
  proxy_hmac_hex(remote_url, sig, sizeof(sig));
  if (!sig[0])
    return NULL;

  CURL *curl = curl_easy_init();
  if (!curl)
    return NULL;
  char *encoded = curl_easy_escape(curl, (char *)remote_url, 0);
  char *out = NULL;
  if (encoded) {
    size_t len = strlen("/proxy?url=") + strlen(encoded) + strlen("&s=") +
                 strlen(sig) + 1;
    out = malloc(len);
    if (out)
      snprintf(out, len, "/proxy?url=%s&s=%s", encoded, sig);
    curl_free(encoded);
  }
  curl_easy_cleanup(curl);
  return out;
}

static int proxy_signature_valid(const char *url, const char *given) {
  if (!url || !given)
    return 0;
  char expected[64] = {0};
  proxy_hmac_hex(url, expected, sizeof(expected));
  if (!expected[0])
    return 0;
  /* constant-time compare */
  size_t a = strlen(expected), b = strlen(given);
  if (a != b)
    return 0;
  unsigned char diff = 0;
  for (size_t i = 0; i < a; i++)
    diff |= (unsigned char)(expected[i] ^ given[i]);
  return diff == 0;
}

int image_proxy_handler(UrlParams *params) {
  const char *url = NULL;
  const char *sig = NULL;
  for (int i = 0; i < params->count; i++) {
    if (strcmp(params->params[i].key, "url") == 0) {
      url = params->params[i].value;
    } else if (strcmp(params->params[i].key, "s") == 0) {
      sig = params->params[i].value;
    }
  }

  if (!url || strlen(url) == 0) {
    send_response("Missing 'url' parameter");
    return 0;
  }

  if (!proxy_signature_valid(url, sig)) {
    send_response("Unsigned proxy request");
    return 0;
  }

  char resolved_ip[INET6_ADDRSTRLEN] = {0};
  int domain_check = is_allowed_domain(url, resolved_ip);
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
    if (!cached_data || cached_size == 0) {
      free(cached_data);
      free(cache_key);
      send_response("Empty cached image response");
      return 0;
    }

    char content_type[IMG_MIME_MAX] = {0};
    char *body = cached_data;
    size_t body_size = cached_size;

    /* Entries written by this version carry their real MIME type. Older
       entries fall back to the extension guess below. */
    if (cached_size > 5 && strncmp(cached_data, "mime:", 5) == 0) {
      char *nl = memchr(cached_data, '\n', cached_size);
      if (nl) {
        size_t tlen = (size_t)(nl - cached_data) - 5;
        if (tlen > 0 && tlen < sizeof(content_type)) {
          memcpy(content_type, cached_data + 5, tlen);
          content_type[tlen] = '\0';
        }
        body = nl + 1;
        body_size = cached_size - (size_t)(body - cached_data);
      }
    }

    if (strncasecmp(content_type, "image/svg", 9) == 0) {
      free(cached_data);
      free(cache_key);
      send_response("SVG is not proxied");
      return 0;
    }

    if (strlen(content_type) == 0) {
      const char *ext = strrchr(url, '.');
      if (ext) {
        if (strcasecmp(ext, ".png") == 0) {
          strncpy(content_type, "image/png", sizeof(content_type) - 1);
        } else if (strcasecmp(ext, ".gif") == 0) {
          strncpy(content_type, "image/gif", sizeof(content_type) - 1);
        } else if (strcasecmp(ext, ".webp") == 0) {
          strncpy(content_type, "image/webp", sizeof(content_type) - 1);
        } else if (strcasecmp(ext, ".svg") == 0) {
          free(cached_data);
          free(cache_key);
          send_response("SVG is not proxied");
          return 0;
        } else if (strcasecmp(ext, ".ico") == 0) {
          strncpy(content_type, "image/x-icon", sizeof(content_type) - 1);
        } else if (strcasecmp(ext, ".bmp") == 0) {
          strncpy(content_type, "image/bmp", sizeof(content_type) - 1);
        }
      }
    }

    if (strlen(content_type) == 0) {
      strncpy(content_type, "image/jpeg", sizeof(content_type) - 1);
    }

    serve_data(body, body_size, content_type);
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
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, (long)MAX_IMAGE_REDIRECTS);
  curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                   (curl_off_t)MAX_IMAGE_SIZE);
  curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, guard_opensocket);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                   "AppleWebKit/537.36 (KHTML, like Gecko) "
                   "Chrome/120.0.0.0 Safari/537.36");
  apply_proxy_settings(curl);

  struct curl_slist *resolves = NULL;
  if (resolved_ip[0] != '\0') {
    CURLU *u = curl_url();
    if (u) {
      curl_url_set(u, CURLUPART_URL, url, 0);
      char *rhost = NULL;
      curl_url_get(u, CURLUPART_HOST, &rhost, 0);
      if (rhost) {
        char *rscheme = NULL;
        curl_url_get(u, CURLUPART_SCHEME, &rscheme, 0);
        int port = (rscheme && strcasecmp(rscheme, "https") == 0) ? 443 : 80;
        if (rscheme)
          curl_free(rscheme);

        char resolve_str[512];
        snprintf(resolve_str, sizeof(resolve_str), "%s:%d:%s", rhost, port,
                 resolved_ip);
        resolves = curl_slist_append(NULL, resolve_str);
        curl_easy_setopt(curl, CURLOPT_RESOLVE, resolves);
        curl_free(rhost);
      }
      curl_url_cleanup(u);
    }
  }

  CURLcode res = curl_easy_perform(curl);

  long response_code;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

  char *content_type_ptr = NULL;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type_ptr);

  char content_type[64] = {0};
  if (content_type_ptr) {
    strncpy(content_type, content_type_ptr, sizeof(content_type) - 1);
  }

  if (resolves)
    curl_slist_free_all(resolves);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK || response_code != 200) {
    if (getenv("OMNI_PROXY_DEBUG"))
      fprintf(stderr, "[PROXYDBG] fetch failed: %s (http=%ld)\n",
              curl_easy_strerror(res), response_code);
    free(buf.data);
    free(cache_key);
    send_response("Failed to fetch image");
    return 0;
  }

  if (buf.size == 0) {
    free(buf.data);
    free(cache_key);
    send_response("Empty image response");
    return 0;
  }

  if (strlen(content_type) == 0 ||
      strncmp(content_type, "image/", 6) != 0) {
    free(buf.data);
    free(cache_key);
    send_response("Invalid content type");
    return 0;
  }

  /* SVG is a script container, and this proxy serves from the app's origin to
     anything that can reach it. Refuse it rather than execute it. */
  if (strncasecmp(content_type, "image/svg", 9) == 0) {
    free(buf.data);
    free(cache_key);
    send_response("SVG is not proxied");
    return 0;
  }

  const char *mime_type =
      strlen(content_type) > 0 ? content_type : "image/jpeg";

  {
    char *prefixed = malloc(strlen(mime_type) + 2 + buf.size);
    if (prefixed) {
      int n = snprintf(prefixed, IMG_MIME_MAX + 2, "mime:%s\n", mime_type);
      if (n > 0) {
        memcpy(prefixed + n, buf.data, buf.size);
        cache_set(cache_key, prefixed, (size_t)n + buf.size);
      }
      free(prefixed);
    }
  }

  serve_data(buf.data, buf.size, mime_type);

  free(buf.data);
  free(cache_key);
  return 0;
}
