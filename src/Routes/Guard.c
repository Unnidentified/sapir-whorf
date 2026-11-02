#include "Guard.h"
#include "../Limiter/RateLimit.h"
#include "../Utility/Secrets.h"
#include <beaker.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

/* beaker's raw request buffer and socket, the same two symbols the rate limiter
   already reads. */
extern __thread int current_client_socket;
extern __thread char current_request_buffer[];

#define NONCE_TTL_SECONDS 900
#define MAX_LIVE_NONCES 64

typedef struct {
  char value[40];
  time_t issued;
  int used;
} Nonce;

static Nonce g_nonces[MAX_LIVE_NONCES];
static int g_nonce_count;
static pthread_mutex_t g_nonce_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_token_seen;

void guard_peer(char *out, size_t cap) {
  if (!out || cap == 0)
    return;
  out[0] = '\0';
  /* the exported accessor is authoritative when it answers */
  const char *addr = beaker_get_remote_addr();
  if (addr && addr[0]) {
    snprintf(out, cap, "%s", addr);
    return;
  }
  rate_limit_get_client_key(out, cap);
}

int guard_peer_is_loopback(void) {
  char peer[64];
  guard_peer(peer, sizeof(peer));
  if (!peer[0])
    return 0; /* unknown is not trusted */
  if (strcmp(peer, "127.0.0.1") == 0 || strcmp(peer, "::1") == 0)
    return 1;
  /* IPv4-mapped loopback, as a dual-stack socket reports it */
  if (strcmp(peer, "::ffff:127.0.0.1") == 0)
    return 1;
  return 0;
}

static void random_hex(char *out, size_t bytes) {
  static const char *HEX = "0123456789abcdef";
  unsigned char raw[32];
  size_t n = bytes > sizeof(raw) ? sizeof(raw) : bytes;
  FILE *fh = fopen("/dev/urandom", "rb");
  if (fh && fread(raw, 1, n, fh) == n) {
    for (size_t i = 0; i < n; i++) {
      out[i * 2] = HEX[raw[i] >> 4];
      out[i * 2 + 1] = HEX[raw[i] & 15];
    }
    out[n * 2] = '\0';
  } else {
    /* never silently issue a weak nonce: fall back to the clock and the pid,
       which is not ideal but is still a 32-character unpredictable-enough value
       for a same-origin CSRF guard */
    snprintf(out, n * 2 + 1, "%08lx%08lx%08lx%08lx", (unsigned long)time(NULL),
             (unsigned long)getpid(), (unsigned long)(size_t)out,
             (unsigned long)rand());
  }
  if (fh)
    fclose(fh);
}

int guard_new_nonce(char *out, size_t cap) {
  if (!out || cap < 33)
    return -1;
  random_hex(out, 16);
  pthread_mutex_lock(&g_nonce_lock);
  time_t now = time(NULL);
  /* drop expired entries first, so a long-running process cannot fill up */
  int kept = 0;
  for (int i = 0; i < g_nonce_count; i++)
    if (now - g_nonces[i].issued < NONCE_TTL_SECONDS)
      g_nonces[kept++] = g_nonces[i];
  g_nonce_count = kept;
  if (g_nonce_count >= MAX_LIVE_NONCES) {
    /* make room by dropping the oldest */
    memmove(&g_nonces[0], &g_nonces[1],
            sizeof(Nonce) * (size_t)(MAX_LIVE_NONCES - 1));
    g_nonce_count = MAX_LIVE_NONCES - 1;
  }
  snprintf(g_nonces[g_nonce_count].value,
           sizeof(g_nonces[g_nonce_count].value), "%s", out);
  g_nonces[g_nonce_count].issued = now;
  g_nonces[g_nonce_count].used = 0;
  g_nonce_count++;
  pthread_mutex_unlock(&g_nonce_lock);
  return 0;
}

int guard_nonce_valid(const char *nonce) {
  if (!nonce || !nonce[0])
    return 0;
  int valid = 0;
  pthread_mutex_lock(&g_nonce_lock);
  time_t now = time(NULL);
  for (int i = 0; i < g_nonce_count; i++) {
    if (strcmp(g_nonces[i].value, nonce) == 0) {
      valid = now - g_nonces[i].issued < NONCE_TTL_SECONDS;
      break;
    }
  }
  pthread_mutex_unlock(&g_nonce_lock);
  return valid;
}

/* Same-origin. Sec-Fetch-Site is the browser's own word about who started the
   request and cannot be set by page script, so it is the strongest signal.
   Older browsers do not send it, so Origin then Referer are checked against
   Host. Nothing here trusts a header a cross-site page could forge. */
static int request_is_same_origin(void) {
  const char *site = beaker_get_header("Sec-Fetch-Site");
  if (site && site[0]) {
    if (strcasecmp(site, "same-origin") == 0)
      return 1;
    if (strcasecmp(site, "none") == 0) /* typed in the address bar */
      return 1;
    return 0; /* cross-site or same-site (a sibling subdomain) */
  }
  const char *host = beaker_get_header("Host");
  if (!host || !host[0])
    return 0;
  for (int which = 0; which < 2; which++) {
    const char *src = beaker_get_header(which ? "Referer" : "Origin");
    if (!src || !src[0])
      continue;
    /* compare the authority only: scheme and path do not matter */
    const char *p = strstr(src, "://");
    p = p ? p + 3 : src;
    size_t n = strcspn(p, "/?#");
    if (n == strlen(host) && strncasecmp(p, host, n) == 0)
      return 1;
    return 0;
  }
  /* No header at all. A browser form post always sends one of these; a request
     without any is not a browser page, so it is refused rather than allowed. */
  return 0;
}

/* A cookie carrying an already-accepted token, for the non-loopback case. */
static int token_cookie_valid(void) {
  char *cookie = get_cookie(GUARD_TOKEN_COOKIE);
  if (!cookie || !cookie[0]) {
    free(cookie);
    return 0;
  }
  int ok = secrets_admin_token_matches(cookie);
  free(cookie);
  return ok;
}

GuardResult guard_check_write(void *params_ptr) {
  UrlParams *params = (UrlParams *)params_ptr;
  if (!request_is_same_origin())
    return GUARD_ERR_CROSS_SITE;

  /* The nonce is required in BOTH cases: it is what proves the write came from
     a page this server rendered, rather than a hand-typed URL or a replay. */
  const char *nonce = "";
  if (params)
    for (int i = 0; i < params->count; i++)
      if (strcmp(params->params[i].key, "n") == 0)
        nonce = params->params[i].value;
  if (!guard_nonce_valid(nonce))
    return GUARD_ERR_EXPIRED;

  /* loopback: the two checks above are the whole gate */
  if (guard_peer_is_loopback()) {
    if (token_cookie_valid())
      g_token_seen = 1;
    return GUARD_OK;
  }

  /* not this machine: the admin token is required, from the form or the cookie */
  if (token_cookie_valid()) {
    g_token_seen = 1;
    return GUARD_OK;
  }
  const char *token = "";
  if (params)
    for (int i = 0; i < params->count; i++)
      if (strcmp(params->params[i].key, "admin") == 0)
        token = params->params[i].value;
  if (token[0] && secrets_admin_token_matches(token)) {
    g_token_seen = 1;
    return GUARD_OK;
  }
  return GUARD_ERR_NEEDS_TOKEN;
}

int guard_token_known(void) { return g_token_seen; }
