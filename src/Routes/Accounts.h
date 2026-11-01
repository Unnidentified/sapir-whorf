/* L3: the Accounts and servers cards, and the write path behind them.
 *
 * Everything here is a *server* setting, not a per-browser one: the Reddit
 * app's credentials, the SearXNG URL, the YaCy peer, a Google cx. They live in
 * $HOME/.omnisearch/secrets.ini (see Utility/Secrets.h) and writing them needs
 * the admin token, because /settings has no login and Docker publishes the
 * port on every interface.
 *
 * Beaker does not parse POST bodies (measured, docs/probe-log.tsv), so the
 * forms are GETs: one parameter per field, a 303 to a clean URL afterwards,
 * no-store and no-referrer on the response, and autocomplete off on the form,
 * so a secret does not linger in history or a Referer. */
#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include <beaker.h>

/* Fills the "accounts" matrix for the settings page: one row per card, with the
   fields it needs, their current masked values and their source. */
typedef struct {
  char ***rows;
  int *inner;
  int count;
} AccountCards;

AccountCards accounts_build(const char *locale_id);
void accounts_free(AccountCards *cards);

/* GET /settings/account-save?card=reddit&field=...&admin=... */
int accounts_save_handler(UrlParams *params);
/* GET /settings/account-test?card=reddit&admin=... - the smallest real call.
   Rate-limited to one per 30 s per card, and it never prints a token. */
int accounts_test_handler(UrlParams *params);

/* The admin gate's answer, kept here so both handlers use one rule. */
int accounts_require_admin(UrlParams *params);

/* GET /settings/account-clear?card=...&field=...&admin=... */
int accounts_clear_handler(UrlParams *params);

/* The Test buttons' probes, exposed for the shell test. */
int url_reachable(const char *url, long timeout_ms);
int searxng_test(const char *base_url, char *out, size_t cap);

#endif
