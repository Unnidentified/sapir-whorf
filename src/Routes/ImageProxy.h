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
int proxy_guarded_get(const char *url, long timeout_ms, size_t max_bytes,
                      char **body_out, size_t *len_out, long *status_out);

#endif
