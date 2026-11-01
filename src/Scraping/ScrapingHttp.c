#include "../Proxy/Proxy.h"
#include "Config.h"
#include "Impersonate.h"
#include "Scraping.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HTTP_DELAY_MIN_US 100000
#define HTTP_DELAY_RANGE_US 100000

static const char *USER_AGENTS[] = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, "
    "like Gecko) Chrome/120.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/119.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like "
    "Gecko) Chrome/120.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:109.0) Gecko/20100101 "
    "Firefox/121.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 "
    "(KHTML, like Gecko) Version/17.2 Safari/605.1.15"};

#define USER_AGENT_COUNT (sizeof(USER_AGENTS) / sizeof(USER_AGENTS[0]))

size_t write_memory_callback(void *contents, size_t size, size_t nmemb,
                             void *userp) {
  size_t realsize = size * nmemb;
  MemoryBuffer *mem = (MemoryBuffer *)userp;

  if (mem->size + realsize + 1 > mem->capacity) {
    size_t new_cap =
        mem->capacity == 0 ? INITIAL_BUFFER_SIZE : mem->capacity * 2;
    while (new_cap < mem->size + realsize + 1)
      new_cap *= 2;

    char *ptr = (char *)realloc(mem->memory, new_cap);
    if (!ptr)
      return 0;
    mem->memory = ptr;
    mem->capacity = new_cap;
  }

  memcpy(&(mem->memory[mem->size]), contents, realsize);
  mem->size += realsize;
  mem->memory[mem->size] = 0;

  return realsize;
}

const char *get_random_user_agent(void) {
  static __thread unsigned int user_agent_seed = 0;
  if (user_agent_seed == 0) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    user_agent_seed = (unsigned int)(ts.tv_nsec ^ (ts.tv_sec << 16)) | 1u;
  }
  return USER_AGENTS[rand_r(&user_agent_seed) % USER_AGENT_COUNT];
}

/* Pick the UA by explicit profile when the registry names one, and fall back to
   the per-id hash only when it does not. Measured, not assumed: Brave refuses
   all three Chrome UAs and serves Firefox and Safari (docs/G1-engines.md). */
const char *get_engine_user_agent_profile(const char *profile,
                                          const char *engine_id) {
  if (profile && profile[0]) {
    if (strcasecmp(profile, "safari") == 0)
      return USER_AGENTS[4];
    if (strcasecmp(profile, "firefox") == 0)
      return USER_AGENTS[3];
    if (strcasecmp(profile, "chrome") == 0)
      return USER_AGENTS[0];
  }
  return get_engine_user_agent(engine_id);
}

/* H1: the measured ladder. Brave refused every Chrome UA and served Safari and
   Firefox; the others accept Chrome, so the ladder is ordered by what was
   actually observed to work rather than by market share. */
static const char *UA_LADDER[] = {"safari", "firefox", "chrome"};

const char *get_engine_user_agent_ladder_next(const char *current) {
  const size_t count = sizeof(UA_LADDER) / sizeof(UA_LADDER[0]);
  /* no profile, or one that is not in the ladder: start at the top rather than
     guessing where we are. Anything else would skip the profile most likely to
     work, which is the point of having a ladder. */
  if (!current || !current[0])
    return UA_LADDER[0];
  for (size_t i = 0; i < count; i++) {
    if (strcasecmp(current, UA_LADDER[i]) != 0)
      continue;
    return (i + 1 < count) ? UA_LADDER[i + 1] : NULL;
  }
  return UA_LADDER[0];
}

/* one stable identity per engine: rotating the UA on every request is a
   fingerprint of its own, and some engines challenge the mismatch */
const char *get_engine_user_agent(const char *engine_id) {
  unsigned long hash = 5381;
  for (const char *p = engine_id ? engine_id : ""; *p; p++)
    hash = hash * 33 + (unsigned char)*p;
  return USER_AGENTS[hash % USER_AGENT_COUNT];
}

void configure_curl_handle(CURL *curl, const char *full_url,
                           MemoryBuffer *chunk, struct curl_slist *headers,
                           long timeout_secs, const char *engine_id,
                           const char *ua_profile, int browser_headers,
                           const char *impersonate_override) {
  curl_easy_setopt(curl, CURLOPT_URL, full_url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)chunk);
  const char *impersonate_target =
      impersonate_target_for_engine(impersonate_override);
#ifdef HAVE_CURL_IMPERSONATE
  if (impersonate_target) {
    /* K6: the library sets the UA, Accept, Accept-Language, sec-ch-ua*,
       Sec-Fetch-* and the header ORDER, and negotiates HTTP/2. Layering our
       own headers on top would break the point of it, so nothing else is set
       here - not even the UA - and HTTP/1.1 is not forced. */
    curl_easy_impersonate(curl, impersonate_target, 1);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
    headers = NULL;
  } else
#endif
  {
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                     get_engine_user_agent_profile(ua_profile, engine_id));
    /* HTTP/1.1 on purpose: Yahoo (and some other front ends) abort the
       transfer with "Error in the HTTP2 framing layer", which reads as an
       engine failure */
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  }
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, CURL_DNS_TIMEOUT_SECS);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,
                   timeout_secs > 0 ? timeout_secs : CURL_TIMEOUT_SECS);
  /* a dead endpoint (silent TLS handshake) must not hold the batch */
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 700L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");

  apply_proxy_settings(curl);
}

char *build_search_url(const char *base_url, const char *page_param,
                       int page_multiplier, int page_base,
                       const char *encoded_query, int page) {
  int page_value = (page < 1 ? 1 : page - 1) * page_multiplier + page_base;
  char *url = malloc(BUFFER_SIZE_LARGE);
  if (!url)
    return NULL;
  snprintf(url, BUFFER_SIZE_LARGE, "%s%s&%s=%d", base_url, encoded_query,
           page_param, page_value);
  return url;
}

struct curl_slist *build_request_headers(const char *host_header,
                                         const char *referer,
                                         const char *accept_language,
                                         int browser_headers) {
  struct curl_slist *headers = NULL;
  char host_buf[BUFFER_SIZE_MEDIUM], ref_buf[BUFFER_SIZE_MEDIUM];

  if (host_header && host_header[0] != '\0') {
    snprintf(host_buf, sizeof(host_buf), "Host: %s", host_header);
    headers = curl_slist_append(headers, host_buf);
  }
  if (referer && referer[0] != '\0') {
    snprintf(ref_buf, sizeof(ref_buf), "Referer: %s", referer);
    headers = curl_slist_append(headers, ref_buf);
  }
  headers = curl_slist_append(
      headers,
      "Accept: "
      "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
  char accept_buf[BUFFER_SIZE_MEDIUM];
  snprintf(accept_buf, sizeof(accept_buf), "Accept-Language: %s",
           accept_language && accept_language[0] ? accept_language
                                                 : "en-US,en;q=0.5");
  headers = curl_slist_append(headers, accept_buf);
  /* DNT: 1 was a reliable automation tell; a browser does not send it unless
     asked, and dropping it costs nothing. */
  if (browser_headers) {
    headers = curl_slist_append(headers, "Sec-Fetch-Dest: document");
    headers = curl_slist_append(headers, "Sec-Fetch-Mode: navigate");
    headers = curl_slist_append(headers, "Sec-Fetch-Site: none");
    headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
    headers =
        curl_slist_append(headers, "Upgrade-Insecure-Requests: 1");
  }

  return headers;
}

/* Unused since the 100-200ms pre-fetch sleep was removed (C7). Kept as a
   no-op for any out-of-tree caller. */
void http_delay(void) {}
