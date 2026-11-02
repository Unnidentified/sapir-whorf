/* M1: who may write server settings.
 *
 * The owner's report was that every Accounts button returned 403. Three faults
 * stacked: the links baked an EMPTY admin= into the HTML that nothing ever
 * filled, the token itself was a 64-character string printed once to a log, and
 * the handlers answered a refusal with send_status + send_response, which in
 * this beaker emits an empty body - so the browser showed its own error page.
 *
 * A pasted token is the wrong thing to ask for. What the token was protecting
 * against is another machine, or another web page, changing settings. Three
 * checks cover that and need nothing typed:
 *
 *   1. the socket peer is loopback - another machine cannot originate it;
 *   2. the request is same-origin - Sec-Fetch-Site: same-origin, or (where that
 *      header is absent) Origin/Referer whose host equals Host. A page on
 *      another site cannot forge either;
 *   3. a form nonce rendered into /settings, valid 15 minutes.
 *
 * The admin token survives for the case the checks do not cover: a request
 * arriving from a non-loopback address. There it is required, and once entered
 * it is kept in a cookie rather than pasted into every form.
 */
#ifndef GUARD_H
#define GUARD_H

#include <stddef.h>

/* What went wrong, so the page can say it in one sentence. */
typedef enum {
  GUARD_OK = 0,
  GUARD_ERR_EXPIRED,    /* the nonce is missing, unknown, or older than its TTL */
  GUARD_ERR_CROSS_SITE, /* the request came from somewhere else */
  GUARD_ERR_NEEDS_TOKEN /* non-loopback, and no valid token supplied */
} GuardResult;

/* The peer address, "127.0.0.1" / "::1" for the machine itself. Empty when the
   socket is unknown. */
void guard_peer(char *out, size_t cap);

/* 1 when the peer is this machine. */
int guard_peer_is_loopback(void);

/* A fresh nonce, stored in memory with a TTL. The buffer must hold 33 bytes.
   Returns 0 on success. */
int guard_new_nonce(char *out, size_t cap);

/* 1 when the nonce is one this process issued and it has not expired. */
int guard_nonce_valid(const char *nonce);

/* The whole decision for a write. params carries the submitted "n" nonce and,
   for the non-loopback case, "admin". */
GuardResult guard_check_write(void *params);

/* The cookie name carrying an accepted admin token. */
#define GUARD_TOKEN_COOKIE "omni_admin"

/* 1 when a token has been accepted at least once in this process (so the page
   can say "already authorised" instead of asking again). */
int guard_token_known(void);

#endif
