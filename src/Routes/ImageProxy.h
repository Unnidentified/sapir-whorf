#ifndef IMAGE_PROXY_HANDLER_H
#define IMAGE_PROXY_HANDLER_H

#include <beaker.h>

int image_proxy_handler(UrlParams *params);

/* Sign a remote URL so /proxy only fetches what a rendered page contained.
   Every generated proxy link must go through this. */
char *image_proxy_sign_url(const char *remote_url);

#endif
