#include "Reddit.h"
#include "../Utility/Secrets.h"
#include "../Utility/Utility.h"
#include <curl/curl.h>
#include <jansson.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The two hosts, overridable so a test can point them at a local stand-in. The
   override is read per call and the shipped configuration never sets it; a
   value here is a test harness's choice, not a default anyone can inherit. */
#define REDDIT_TOKEN_URL_DEFAULT "https://www.reddit.com/api/v1/access_token"
#define REDDIT_API_DEFAULT "https://oauth.reddit.com"

static const char *token_url(void) {
  const char *override = getenv("OMNI_REDDIT_TOKEN_URL");
  return (override && override[0]) ? override : REDDIT_TOKEN_URL_DEFAULT;
}

static const char *api_base(void) {
  const char *override = getenv("OMNI_REDDIT_API");
  return (override && override[0]) ? override : REDDIT_API_DEFAULT;
}
#define REDDIT_TIMEOUT_MS 6000L
#define REDDIT_VERSION "1.0"

/* the token, in memory only */
static struct {
  char value[512];
  time_t expires_at;      /* absolute; a token is renewed 60 s before this */
  int have;
} g_token;
static pthread_mutex_t g_token_lock = PTHREAD_MUTEX_INITIALIZER;

/* the provider's block state, so the status table can show it */
static time_t g_blocked_until;
static char g_why[192] = "needs setup";

typedef struct {
  char *data;
  size_t size;
  size_t cap;
} Buf;

static size_t buf_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
  Buf *buf = userdata;
  size_t total = size * nmemb;
  if (buf->size + total + 1 > buf->cap) {
    size_t want = (buf->size + total + 1) * 2;
    char *grown = realloc(buf->data, want);
    if (!grown)
      return 0;
    buf->data = grown;
    buf->cap = want;
  }
  memcpy(buf->data + buf->size, ptr, total);
  buf->size += total;
  buf->data[buf->size] = '\0';
  return total;
}

typedef struct {
  long remaining;
  long reset;
  int have_ratelimit;
} RateInfo;

static size_t header_cb(char *buffer, size_t size, size_t nitems,
                        void *userdata) {
  RateInfo *info = userdata;
  size_t total = size * nitems;
  if (strncasecmp(buffer, "X-Ratelimit-Remaining:", 22) == 0) {
    info->remaining = strtol(buffer + 22, NULL, 10);
    info->have_ratelimit = 1;
  } else if (strncasecmp(buffer, "X-Ratelimit-Reset:", 18) == 0) {
    info->reset = strtol(buffer + 18, NULL, 10);
  }
  (void)total;
  return total;
}

/* M2: identities that belong to other projects. Sending one is impersonation:
   Reddit's rules want an app you registered, and a borrowed identity gets
   throttled or blocked - which also hits that project's real users. Refused
   rather than sent. */
static const char *FOREIGN_AGENTS[] = {
    "redreader", "org.quantumbadger", "reddit is fun", "rif", "apollo",
    "narwhal", "slide", "relay for reddit", "sync for reddit",
};

int reddit_agent_is_foreign(const char *agent) {
  if (!agent || !agent[0])
    return 0;
  char lower[256];
  size_t n = strlen(agent);
  if (n >= sizeof(lower))
    n = sizeof(lower) - 1;
  for (size_t i = 0; i < n; i++)
    lower[i] = (char)((agent[i] >= 'A' && agent[i] <= 'Z')
                          ? agent[i] + 32
                          : agent[i]);
  lower[n] = '\0';
  for (size_t i = 0; i < sizeof(FOREIGN_AGENTS) / sizeof(FOREIGN_AGENTS[0]); i++)
    if (strstr(lower, FOREIGN_AGENTS[i]))
      return 1;
  return 0;
}

int reddit_configured(void) {
  const char *id = "";
  const char *user = "";
  const char *agent = "";
  secrets_get(SK_REDDIT_CLIENT_ID, &id);
  secrets_get(SK_REDDIT_USERNAME, &user);
  secrets_get(SK_REDDIT_USER_AGENT, &agent);
  /* a client id, and something that names the app honestly: either the app's
     own User-Agent string, or a username we can put in the format Reddit
     documents. Neither is optional, because the rules forbid lying about it. */
  return id[0] && (agent[0] || user[0]);
}

void reddit_user_agent(char *out, size_t cap) {
  if (!out || cap == 0)
    return;
  const char *agent = "";
  secrets_get(SK_REDDIT_USER_AGENT, &agent);
  if (agent[0]) {
    /* the app's own string, sent unchanged. Never a browser's: Reddit's rules
       say the agent must identify the app and must not lie about it. */
    snprintf(out, cap, "%s", agent);
    return;
  }
  const char *user = "";
  secrets_get(SK_REDDIT_USERNAME, &user);
  /* otherwise the format Reddit documents:
     <platform>:<app ID>:<version> (by /u/<user>) */
  snprintf(out, cap, "omnisearch:omnisearch:%s (by /u/%s)", REDDIT_VERSION,
           user[0] ? user : "none");
}

static const char *basic_auth(char *out, size_t cap, const char *id,
                              const char *secret) {
  char joined[640];
  snprintf(joined, sizeof(joined), "%s:%s", id ? id : "", secret ? secret : "");
  /* base64 */
  static const char *B64 =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  size_t len = strlen(joined), o = 0;
  for (size_t i = 0; i < len && o + 5 < cap; i += 3) {
    unsigned v = (unsigned char)joined[i] << 16;
    if (i + 1 < len)
      v |= (unsigned char)joined[i + 1] << 8;
    if (i + 2 < len)
      v |= (unsigned char)joined[i + 2];
    out[o++] = B64[(v >> 18) & 63];
    out[o++] = B64[(v >> 12) & 63];
    out[o++] = (i + 1 < len) ? B64[(v >> 6) & 63] : '=';
    out[o++] = (i + 2 < len) ? B64[v & 63] : '=';
  }
  out[o] = '\0';
  return out;
}

/* one token request. Caller holds g_token_lock. */
static int fetch_token_locked(char *why, size_t why_cap) {
  const char *id = "";
  const char *secret = "";
  secrets_get(SK_REDDIT_CLIENT_ID, &id);
  secrets_get(SK_REDDIT_CLIENT_SECRET, &secret);
  const char *user = "";
  secrets_get(SK_REDDIT_USERNAME, &user);
  if (!id[0]) {
    snprintf(why, why_cap, "no client id saved");
    return -1;
  }
  const char *agent = "";
  secrets_get(SK_REDDIT_USER_AGENT, &agent);
  if (!user[0] && !agent[0]) {
    snprintf(why, why_cap,
             "no User-Agent saved (Reddit's rules require the app to identify "
             "itself, so no request is made)");
    return -1;
  }

  /* the device id: 20-30 ASCII characters, generated once, stored, reused */
  const char *device = "";
  secrets_get(SK_REDDIT_DEVICE_ID, &device);
  if (!device[0]) {
    char fresh[40] = "";
    FILE *fh = fopen("/dev/urandom", "rb");
    unsigned char raw[24];
    if (fh) {
      if (fread(raw, 1, sizeof(raw), fh) == sizeof(raw)) {
        static const char *HEX = "0123456789abcdef";
        for (size_t i = 0; i < sizeof(raw); i++) {
          fresh[i * 2] = HEX[raw[i] >> 4];
          fresh[i * 2 + 1] = HEX[raw[i] & 15];
        }
        fresh[sizeof(raw) * 2] = '\0';
      }
      fclose(fh);
    }
    if (!fresh[0]) {
      snprintf(why, why_cap, "cannot generate a device id");
      return -1;
    }
    secrets_set(SK_REDDIT_DEVICE_ID, fresh);
    secrets_reload_after_save();
    secrets_get(SK_REDDIT_DEVICE_ID, &device);
  }

  char auth[700];
  basic_auth(auth, sizeof(auth), id, secret);
  char auth_header[800];
  snprintf(auth_header, sizeof(auth_header), "Authorization: Basic %s", auth);

  /* A stored refresh token is the better grant: it identifies the owner's app
     rather than an anonymous device, and Reddit's quota is per app either way.
     The app-only grant is the fallback for an app with no code flow done yet. */
  const char *refresh = "";
  secrets_get(SK_REDDIT_REFRESH_TOKEN, &refresh);
  char post[1024];
  if (refresh[0])
    snprintf(post, sizeof(post),
             "grant_type=refresh_token&refresh_token=%s", refresh);
  else
    snprintf(post, sizeof(post),
             "grant_type=https://oauth.reddit.com/grants/installed_client"
             "&device_id=%s",
             device);

  char ua[256];
  reddit_user_agent(ua, sizeof(ua));

  CURL *curl = curl_easy_init();
  if (!curl) {
    snprintf(why, why_cap, "curl init failed");
    return -1;
  }
  Buf buf = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, auth_header); /* never logged */
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
  curl_easy_setopt(curl, CURLOPT_URL, token_url());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REDDIT_TIMEOUT_MS);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    snprintf(why, why_cap, "Reddit did not answer (%s)", curl_easy_strerror(rc));
    free(buf.data);
    return -1;
  }
  if (status == 401 || status == 403) {
    snprintf(why, why_cap, "Reddit has refused this app's client id (%ld)", status);
    g_blocked_until = time(NULL) + 900;
    free(buf.data);
    return -1;
  }
  if (status != 200) {
    snprintf(why, why_cap, "Reddit answered %ld to the token request", status);
    free(buf.data);
    return -1;
  }
  json_error_t err;
  json_t *root = buf.data ? json_loads(buf.data, 0, &err) : NULL;
  free(buf.data);
  if (!root) {
    snprintf(why, why_cap, "Reddit sent unreadable JSON for the token");
    return -1;
  }
  /* a 200 carrying an error is a failure, not a result */
  const char *error_field =
      json_string_value(json_object_get(root, "error"));
  if (error_field && error_field[0]) {
    snprintf(why, why_cap, "Reddit refused the grant: %.120s", error_field);
    json_decref(root);
    return -1;
  }
  const char *access = json_string_value(json_object_get(root, "access_token"));
  json_t *expires = json_object_get(root, "expires_in");
  if (!access || !access[0]) {
    snprintf(why, why_cap, "Reddit's token reply has no access_token");
    json_decref(root);
    return -1;
  }
  snprintf(g_token.value, sizeof(g_token.value), "%s", access);
  long ttl = json_is_integer(expires) ? (long)json_integer_value(expires) : 3600;
  g_token.expires_at = time(NULL) + ttl;
  g_token.have = 1;
  g_blocked_until = 0;
  snprintf(g_why, sizeof(g_why), "ok");
  json_decref(root);
  return 0;
}

/* L5 rung B: the authorisation URL. Reddit's read scope only - this client
   never posts, votes or messages, so asking for write would be more than the
   job needs. */
int reddit_authorize_url(const char *state, char *out, size_t cap,
                         char *why, size_t why_cap) {
  if (why && why_cap)
    why[0] = '\0';
  if (!out || cap < 64) {
    if (why)
      snprintf(why, why_cap, "no room for the URL");
    return -1;
  }
  const char *id = "";
  const char *redirect = "";
  secrets_get(SK_REDDIT_CLIENT_ID, &id);
  secrets_get(SK_REDDIT_REDIRECT_URI, &redirect);
  if (!id[0]) {
    if (why)
      snprintf(why, why_cap, "save the client id first");
    return -1;
  }
  if (!redirect[0]) {
    if (why)
      snprintf(why, why_cap, "save the redirect URI first");
    return -1;
  }
  char enc_redirect[512];
  url_encode(redirect, enc_redirect, sizeof(enc_redirect));
  snprintf(out, cap,
           "https://www.reddit.com/api/v1/authorize?client_id=%s"
           "&response_type=code&state=%s&redirect_uri=%s"
           "&duration=permanent&scope=read",
           id, (state && state[0]) ? state : "omnisearch", enc_redirect);
  return 0;
}

/* rung B, second half: the code becomes a token, and the refresh token is kept
   so later searches need no browser. Reddit's installed-app code flow uses
   Basic auth with an EMPTY secret - the id alone. */
/* M2: with a custom redirect (myapp://callback) the browser cannot open the
   redirect, so the owner copies the whole URL from the address bar. Pull the
   code and the state out of it, or take a bare code. */
static void extract_code_and_state(const char *input, char *code, size_t code_cap,
                                   char *state, size_t state_cap) {
  code[0] = '\0';
  state[0] = '\0';
  if (!input)
    return;
  const char *q = strstr(input, "code=");
  if (!q) {
    /* not a URL: take the text as the code, stripped of stray whitespace */
    const char *p = input;
    while (*p == ' ' || *p == '\t' || *p == '\n')
      p++;
    size_t n = strcspn(p, " \t\r\n");
    if (n >= code_cap)
      n = code_cap - 1;
    memcpy(code, p, n);
    code[n] = '\0';
    return;
  }
  q += 5;
  size_t n = strcspn(q, "&# \t\r\n");
  if (n >= code_cap)
    n = code_cap - 1;
  memcpy(code, q, n);
  code[n] = '\0';
  const char *st = strstr(input, "state=");
  if (st) {
    st += 6;
    size_t m = strcspn(st, "&# \t\r\n");
    if (m >= state_cap)
      m = state_cap - 1;
    memcpy(state, st, m);
    state[m] = '\0';
  }
}

int reddit_exchange_code(const char *input, const char *expected_state, char *why,
                         size_t why_cap, char *who, size_t who_cap) {
  if (why && why_cap)
    why[0] = '\0';
  if (who && who_cap)
    who[0] = '\0';
  char code[1024];
  char state[128];
  extract_code_and_state(input, code, sizeof(code), state, sizeof(state));
  if (!code[0]) {
    if (why)
      snprintf(why, why_cap, "no code given");
    return -1;
  }
  /* M2: the state Reddit echoes must be the one this page issued. A mismatch
     means the reply belongs to a different authorisation - refuse it in plain
     words rather than exchanging it. */
  if (expected_state && expected_state[0] && state[0] &&
      strcmp(expected_state, state) != 0) {
    if (why)
      snprintf(why, why_cap,
               "That reply does not match the request this page made - press "
               "Authorise again and paste the new one.");
    return -1;
  }
  const char *id = "";
  secrets_get(SK_REDDIT_CLIENT_ID, &id);
  const char *redirect = "";
  secrets_get(SK_REDDIT_REDIRECT_URI, &redirect);
  if (!id[0] || !redirect[0]) {
    if (why)
      snprintf(why, why_cap, "client id and redirect URI are both needed");
    return -1;
  }

  char auth[700];
  basic_auth(auth, sizeof(auth), id, "");
  char auth_header[800];
  snprintf(auth_header, sizeof(auth_header), "Authorization: Basic %s", auth);

  char enc_redirect[512];
  url_encode(redirect, enc_redirect, sizeof(enc_redirect));
  char enc_code[1024];
  url_encode(code, enc_code, sizeof(enc_code));

  char post[1800];
  snprintf(post, sizeof(post),
           "grant_type=authorization_code&code=%s&redirect_uri=%s",
           enc_code, enc_redirect);

  char ua[256];
  reddit_user_agent(ua, sizeof(ua));

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (why)
      snprintf(why, why_cap, "curl init failed");
    return -1;
  }
  Buf buf = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, auth_header);
  headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
  curl_easy_setopt(curl, CURLOPT_URL, token_url());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REDDIT_TIMEOUT_MS);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    if (why)
      snprintf(why, why_cap, "Reddit did not answer (%s)",
               curl_easy_strerror(rc));
    free(buf.data);
    return -1;
  }
  json_error_t err;
  json_t *root = buf.data ? json_loads(buf.data, 0, &err) : NULL;
  free(buf.data);
  if (!root) {
    if (why)
      snprintf(why, why_cap, "Reddit sent unreadable JSON (%ld)", status);
    return -1;
  }
  const char *error_field = json_string_value(json_object_get(root, "error"));
  if (error_field && error_field[0]) {
    if (why)
      snprintf(why, why_cap, "Reddit refused the code: %.120s", error_field);
    json_decref(root);
    return -1;
  }
  const char *access = json_string_value(json_object_get(root, "access_token"));
  const char *refresh = json_string_value(json_object_get(root, "refresh_token"));
  if (!access || !access[0]) {
    if (why)
      snprintf(why, why_cap, "Reddit's reply has no access_token");
    json_decref(root);
    return -1;
  }
  if (refresh && refresh[0]) {
    secrets_set(SK_REDDIT_REFRESH_TOKEN, refresh);
    secrets_reload_after_save();
  }
  pthread_mutex_lock(&g_token_lock);
  snprintf(g_token.value, sizeof(g_token.value), "%s", access);
  json_t *expires = json_object_get(root, "expires_in");
  long ttl = json_is_integer(expires) ? (long)json_integer_value(expires) : 3600;
  g_token.expires_at = time(NULL) + ttl;
  g_token.have = 1;
  g_blocked_until = 0;
  snprintf(g_why, sizeof(g_why), "ok");
  pthread_mutex_unlock(&g_token_lock);
  json_decref(root);
  return 0;
}

static int token(char *out, size_t cap, char *why, size_t why_cap);

/* M2: one GET /api/v1/me, so the card can say who it is connected as instead
   of a bare "Authorised". Returns 0 and fills who on success. */
int reddit_whoami(char *who, size_t cap, char *why, size_t why_cap) {
  if (who && cap)
    who[0] = '\0';
  if (why && why_cap)
    why[0] = '\0';
  char access[512];
  if (token(access, sizeof(access), why, why_cap) != 0)
    return -1;
  char url[512];
  snprintf(url, sizeof(url), "%s/api/v1/me", api_base());
  char auth_header[600];
  snprintf(auth_header, sizeof(auth_header), "Authorization: bearer %s", access);
  char ua[256];
  reddit_user_agent(ua, sizeof(ua));

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (why)
      snprintf(why, why_cap, "curl init failed");
    return -1;
  }
  Buf buf = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, auth_header);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REDDIT_TIMEOUT_MS);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    if (why)
      snprintf(why, why_cap, "Reddit did not answer (%s)",
               curl_easy_strerror(rc));
    free(buf.data);
    return -1;
  }
  json_error_t err;
  json_t *root = buf.data ? json_loads(buf.data, 0, &err) : NULL;
  free(buf.data);
  if (!root) {
    if (why)
      snprintf(why, why_cap, "Reddit sent unreadable JSON (%ld)", status);
    return -1;
  }
  const char *name = json_string_value(json_object_get(root, "name"));
  if (!name || !name[0])
    name = json_string_value(json_object_get(root, "id"));
  if (!name || !name[0]) {
    if (why)
      snprintf(why, why_cap, "Reddit did not say which account this is (%ld)",
               status);
    json_decref(root);
    return -1;
  }
  if (who)
    snprintf(who, cap, "%s", name);
  json_decref(root);
  return 0;
}

/* M2: Disconnect revokes at Reddit, not only locally - otherwise the token
   stays valid until it expires and nothing about the page is true. */
int reddit_revoke(char *why, size_t why_cap) {
  if (why && why_cap)
    why[0] = '\0';
  const char *refresh = "";
  secrets_get(SK_REDDIT_REFRESH_TOKEN, &refresh);
  const char *id = "";
  const char *secret = "";
  secrets_get(SK_REDDIT_CLIENT_ID, &id);
  secrets_get(SK_REDDIT_CLIENT_SECRET, &secret);
  int revoked = 0;
  if (refresh[0] && id[0]) {
    char auth[700];
    basic_auth(auth, sizeof(auth), id, secret);
    char auth_header[800];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Basic %s", auth);
    char post[1200];
    snprintf(post, sizeof(post), "token=%s", refresh);
    char url[512];
    snprintf(url, sizeof(url), "%s/api/v1/revoke_token", api_base());
    char ua[256];
    reddit_user_agent(ua, sizeof(ua));
    CURL *curl = curl_easy_init();
    if (curl) {
      struct curl_slist *headers = NULL;
      headers = curl_slist_append(headers, auth_header);
      headers = curl_slist_append(
          headers, "Content-Type: application/x-www-form-urlencoded");
      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REDDIT_TIMEOUT_MS);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
      Buf buf = {0};
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
      curl_easy_perform(curl);
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      free(buf.data);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      revoked = (status == 200 || status == 204);
    }
  }
  /* locally the token goes regardless: a revoked-but-still-stored token is
     worse than a deleted one */
  reddit_forget_refresh_token();
  if (!revoked && why)
    snprintf(why, why_cap,
             "Disconnected here. Reddit did not confirm the revocation, so the "
             "token may stay valid until it expires.");
  return revoked ? 0 : 1;
}

int reddit_has_refresh_token(void) {
  const char *refresh = "";
  secrets_get(SK_REDDIT_REFRESH_TOKEN, &refresh);
  return refresh[0] != '\0';
}

void reddit_forget_refresh_token(void) {
  secrets_set(SK_REDDIT_REFRESH_TOKEN, "");
  secrets_reload_after_save();
  /* the cached access token came from that refresh token, so it goes too */
  pthread_mutex_lock(&g_token_lock);
  g_token.have = 0;
  g_token.value[0] = '\0';
  pthread_mutex_unlock(&g_token_lock);
}

/* the token, refreshed when it is within 60 s of expiry. One lock, so N
   concurrent searches make one token request. */
static int token(char *out, size_t cap, char *why, size_t why_cap) {
  pthread_mutex_lock(&g_token_lock);
  time_t now = time(NULL);
  if (g_token.have && now < g_token.expires_at - 60) {
    snprintf(out, cap, "%s", g_token.value);
    pthread_mutex_unlock(&g_token_lock);
    return 0;
  }
  if (fetch_token_locked(why, why_cap) != 0) {
    pthread_mutex_unlock(&g_token_lock);
    return -1;
  }
  snprintf(out, cap, "%s", g_token.value);
  pthread_mutex_unlock(&g_token_lock);
  return 0;
}

static void forget_token(void) {
  pthread_mutex_lock(&g_token_lock);
  g_token.have = 0;
  g_token.value[0] = '\0';
  pthread_mutex_unlock(&g_token_lock);
}

int reddit_search(const char *terms, const char *subreddit, RedditPost **out,
                  int *count, char *why, size_t why_cap) {
  if (out)
    *out = NULL;
  if (count)
    *count = 0;
  if (why && why_cap)
    why[0] = '\0';
  if (!reddit_configured()) {
    if (why)
      snprintf(why, why_cap, "not configured");
    return -1;
  }
  if (g_blocked_until && time(NULL) < g_blocked_until) {
    if (why)
      snprintf(why, why_cap, "blocked until %ld", (long)g_blocked_until);
    return -1;
  }

  char access[512];
  if (token(access, sizeof(access), why, why_cap) != 0)
    return -1;

  char encoded[1024];
  url_encode(terms ? terms : "", encoded, sizeof(encoded));
  char url[1600];
  if (subreddit && subreddit[0])
    snprintf(url, sizeof(url),
             "%s/r/%s/search?q=%s&restrict_sr=1&type=link"
                        "&sort=relevance&t=all&limit=25&raw_json=1",
             api_base(),
             subreddit, encoded);
  else
    snprintf(url, sizeof(url),
             "%s/search?q=%s&type=link&sort=relevance&t=all"
                        "&limit=25&raw_json=1",
             api_base(), encoded);

  char auth_header[600];
  snprintf(auth_header, sizeof(auth_header), "Authorization: bearer %s", access);
  char ua[256];
  reddit_user_agent(ua, sizeof(ua));

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (why)
      snprintf(why, why_cap, "curl init failed");
    return -1;
  }
  Buf buf = {0};
  RateInfo rate = {0};
  struct curl_slist *headers = curl_slist_append(NULL, auth_header);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &rate);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REDDIT_TIMEOUT_MS);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rate.have_ratelimit && rate.remaining < 5 && rate.reset > 0) {
    g_blocked_until = time(NULL) + rate.reset;
    if (why)
      snprintf(why, why_cap, "quota nearly spent (%ld left), pausing %ld s",
               rate.remaining, rate.reset);
  }

  if (rc != CURLE_OK) {
    if (why)
      snprintf(why, why_cap, "Reddit did not answer (%s)",
               curl_easy_strerror(rc));
    free(buf.data);
    return -1;
  }
  if (status == 401) {
    /* one refresh, one retry: the token may have been invalidated */
    forget_token();
    free(buf.data);
    if (token(access, sizeof(access), why, why_cap) != 0)
      return -1;
    snprintf(auth_header, sizeof(auth_header), "Authorization: bearer %s",
             access);
    curl = curl_easy_init();
    if (!curl)
      return -1;
    buf.data = NULL;
    buf.size = 0;
    buf.cap = 0;
    headers = curl_slist_append(NULL, auth_header);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, buf_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, REDDIT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, ua);
    rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK || status != 200) {
      if (why)
        snprintf(why, why_cap, "Reddit refused the retry (%ld)", status);
      free(buf.data);
      return -1;
    }
  }
  if (status == 429) {
    g_blocked_until = time(NULL) + (rate.reset > 0 ? rate.reset : 60);
    if (why)
      snprintf(why, why_cap, "rate limited (%ld)", status);
    free(buf.data);
    return -1;
  }
  if (status != 200) {
    if (why)
      snprintf(why, why_cap, "Reddit answered %ld", status);
    free(buf.data);
    return -1;
  }
  if (!buf.data) {
    if (why)
      snprintf(why, why_cap, "Reddit sent an empty body");
    return -1;
  }
  /* HTML where JSON was expected is a block page, not a result - the parse
     below catches it, and the message says so rather than "no results" */
  json_error_t err;
  json_t *root = json_loads(buf.data, 0, &err);
  free(buf.data);
  if (!root) {
    if (why)
      snprintf(why, why_cap,
               "Reddit answered 200 with something that is not JSON");
    g_blocked_until = time(NULL) + 300;
    return -1;
  }
  const char *error_field = json_string_value(json_object_get(root, "error"));
  if (error_field && error_field[0]) {
    if (why)
      snprintf(why, why_cap, "Reddit reported: %.100s", error_field);
    json_decref(root);
    return -1;
  }
  json_t *data = json_object_get(root, "data");
  json_t *children = data ? json_object_get(data, "children") : NULL;
  if (!json_is_array(children)) {
    if (why)
      snprintf(why, why_cap, "Reddit's reply has no data.children");
    json_decref(root);
    return -1;
  }
  size_t n = json_array_size(children);
  RedditPost *posts = n ? calloc(n, sizeof(RedditPost)) : NULL;
  if (n && !posts) {
    json_decref(root);
    return -1;
  }
  int made = 0;
  for (size_t i = 0; i < n; i++) {
    json_t *child = json_array_get(children, i);
    json_t *d = json_object_get(child, "data");
    if (!json_is_object(d))
      continue;
    const char *title = json_string_value(json_object_get(d, "title"));
    const char *permalink = json_string_value(json_object_get(d, "permalink"));
    if (!title || !permalink)
      continue;
    RedditPost *p = &posts[made];
    snprintf(p->title, sizeof(p->title), "%s", title);
    snprintf(p->permalink, sizeof(p->permalink), "https://www.reddit.com%s",
             permalink);
    const char *sub = json_string_value(
        json_object_get(d, "subreddit_name_prefixed"));
    snprintf(p->subreddit, sizeof(p->subreddit), "%s", sub ? sub : "");
    const char *author = json_string_value(json_object_get(d, "author"));
    snprintf(p->author, sizeof(p->author), "%s", author ? author : "");
    const char *self = json_string_value(json_object_get(d, "selftext"));
    snprintf(p->selftext, sizeof(p->selftext), "%s", self ? self : "");
    const char *domain =
        json_string_value(json_object_get(d, "domain"));
    snprintf(p->link_domain, sizeof(p->link_domain), "%s", domain ? domain : "");
    json_t *score = json_object_get(d, "score");
    p->score = json_is_integer(score) ? (int)json_integer_value(score) : 0;
    json_t *comments = json_object_get(d, "num_comments");
    p->comments =
        json_is_integer(comments) ? (int)json_integer_value(comments) : 0;
    json_t *created = json_object_get(d, "created_utc");
    p->created_utc =
        json_is_number(created) ? (time_t)json_number_value(created) : 0;
    json_t *nsfw = json_object_get(d, "over_18");
    p->over_18 = json_is_true(nsfw) ? 1 : 0;
    made++;
  }
  json_decref(root);
  *out = posts;
  *count = made;
  return 0;
}

void reddit_free_posts(RedditPost *posts, int count) {
  (void)count;
  free(posts);
}

const char *reddit_status_word(void) {
  if (!reddit_configured())
    return "needs setup";
  if (g_blocked_until && time(NULL) < g_blocked_until)
    return "blocked";
  if (g_token.have && time(NULL) < g_token.expires_at)
    return "ok";
  return g_why;
}

time_t reddit_blocked_until(void) { return g_blocked_until; }

int reddit_test_credentials(const char *client_id, const char *client_secret,
                            char *out, size_t cap) {
  (void)client_id;
  (void)client_secret;
  /* the credentials used are the saved ones: the card saves first, then tests,
     so what is tested is what will be used */
  forget_token();
  g_blocked_until = 0;
  char why[192] = "";
  RedditPost *posts = NULL;
  int count = 0;
  char probe[8];
  snprintf(probe, sizeof(probe), "test");
  if (reddit_search(probe, NULL, &posts, &count, why, sizeof(why)) != 0) {
    snprintf(out, cap, "Reddit refused: %.150s", why);
    return -1;
  }
  reddit_free_posts(posts, count);
  snprintf(out, cap,
           "Reddit accepted this app. %d result%s. Remaining quota: %s.",
           count, count == 1 ? "" : "s", "read from the response headers");
  return 0;
}
