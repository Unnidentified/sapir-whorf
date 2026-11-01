/* K6: which libcurl the binary is linked against, and what "impersonation" can
   mean here.

   libcurl-impersonate is a drop-in libcurl with BoringSSL, so the TLS
   ClientHello, ALPN, header order and HTTP/2 settings match a real browser
   instead of libcurl's. It is not in Homebrew and the macOS release ships only
   executables, so it is a build-time choice:

       make CURL_IMPERSONATE=1 CURL_IMPERSONATE_PREFIX=/usr/local/curl-impersonate

   Without it the binary says "off (stock libcurl)" and every engine uses the
   stock handshake. That is the honest state, and it is the one this build has. */
#ifndef OMNISEARCH_IMPERSONATE_H
#define OMNISEARCH_IMPERSONATE_H

#ifdef HAVE_CURL_IMPERSONATE
#define IMPERSONATION_LIBRARY "libcurl-impersonate"
#else
#define IMPERSONATION_LIBRARY "stock libcurl"
#endif

#ifdef HAVE_CURL_IMPERSONATE
#include <curl/curl.h>
/* the extra entry point the fork adds; not in curl's own headers */
extern CURLcode curl_easy_impersonate(CURL *curl, const char *target,
                                      int default_headers);
#endif

/* One table: the config value maps to one pinned target. Pinning matters:
   "chrome" that silently becomes a new build is not reproducible, and the
   test's fixtures are recorded against a specific target. */
const char *impersonate_target_for(const char *setting);

/* The banner the startup line and /info print. */
const char *impersonate_banner(void);

/* The target in use for one engine: the registry override, else the configured
   default, else NULL when impersonation is off. */
const char *impersonate_target_for_engine(const char *engine_override);

#endif
