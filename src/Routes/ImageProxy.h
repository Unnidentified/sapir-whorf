#ifndef IMAGE_PROXY_HANDLER_H
#define IMAGE_PROXY_HANDLER_H

#include <beaker.h>

int image_proxy_handler(UrlParams *params);

/* Sign a remote URL so /proxy only fetches what a rendered page contained.
   Every generated proxy link must go through this. */
char *image_proxy_sign_url(const char *remote_url);

/* K7: one GET with the same socket guard the proxy uses (the address about to
   be connected to is the address that gets checked, on every hop), for the
   description fetcher. The body is malloc'd and NUL-terminated; returns 0 on a
   successful fetch, -1 otherwise. */
/* K7: the same HMAC the proxy puts on an image URL, exposed so the description
   endpoint can accept a URL only when this server signed it. */
char *proxy_sign_token(const char *payload);
int proxy_verify_token(const char *payload, const char *signature);

int proxy_guarded_get(const char *url, long timeout_ms, size_t max_bytes,
                      char **body_out, size_t *len_out, long *status_out);

/* The loopback escape, for the test harness only: it exists so a test can fetch
   its own local server, and it is read from the environment at call time, so a
   production process has no way to set it accidentally. */
int proxy_loopback_allowed(void);

#endif
