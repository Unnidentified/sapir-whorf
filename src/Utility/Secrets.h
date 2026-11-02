/* L3: the server-side settings page, in one file the page owns.
 *
 * config.ini belongs to the owner and travels in the zip; a credential written
 * there goes wherever the archive goes. So the page writes
 * $HOME/.omnisearch/secrets.ini instead: mode 0600 in a 0700 directory, added
 * to .gitignore and to the release exclusions, never committed and never
 * logged.
 *
 * Load order, highest wins: environment > secrets.ini > config.ini. The page
 * says which one is in force, because "from the environment, not editable
 * here" is a sentence the owner needs to read rather than a puzzle. */
#ifndef SECRETS_H
#define SECRETS_H

#include <stddef.h>

typedef enum {
  SECRET_SOURCE_ABSENT = 0, /* nobody has set it */
  SECRET_SOURCE_CONFIG,     /* config.ini */
  SECRET_SOURCE_FILE,       /* secrets.ini, written by the page */
  SECRET_SOURCE_ENV         /* the environment, which the page cannot change */
} SecretSource;

/* The keys the page may write. Everything else in the file is ignored, so a
   hand-written section cannot inject a config option. */
typedef enum {
  SK_REDDIT_CLIENT_ID = 0,
  SK_REDDIT_CLIENT_SECRET,
  SK_REDDIT_USERNAME,
  SK_REDDIT_DEVICE_ID,
  SK_REDDIT_REDIRECT_URI,
  SK_REDDIT_REFRESH_TOKEN,
  /* the app's own User-Agent string, used verbatim when set. Reddit's rules
     only ask that it identifies the app and does not lie; an app that already
     ships one should send that rather than a synthesised string. Never another
     project's identity: that is impersonation and it is refused. */
  SK_REDDIT_USER_AGENT,
  SK_SEARXNG_URL,
  SK_SEARXNG_ENGINES,
  SK_YACY_INSTANCE,
  SK_GOOGLE_CSE_CX,
  SK_GOOGLE_CSE_KEY,
  SK_ADMIN_TOKEN,
  SK_EXTRA_ENGINES,
  SK_COUNT
} SecretKey;

/* Where the file is: $OMNI_SECRETS_FILE, else $HOME/.omnisearch/secrets.ini */
const char *secrets_path(void);

/* Read the file, creating the directory (0700) if needed. Refuses a file that
   is group- or world-readable, and says so once at startup. Returns 0 when the
   file is absent or readable, -1 when it exists and is unsafe. */
int secrets_load(void);
void secrets_reload_after_save(void);

/* The value for a key, and where it came from. *value is "" when absent. */
SecretSource secrets_get(SecretKey key, const char **value);
const char *secrets_key_name(SecretKey key);
SecretKey secrets_key_from_name(const char *name);

/* Write one value and persist the file at 0600. An empty value removes the
   key. Returns 0 on success. */
int secrets_set(SecretKey key, const char *value);

/* The masked form the page shows, for a stored secret: the last four
   characters only. Never the value itself. */
void secrets_mask(SecretKey key, char *out, size_t cap);

/* 1 when the key is a credential the page never renders and the log never
   prints. */
int secrets_is_credential(SecretKey key);

/* The admin token: generated on first start (32 random bytes, hex), stored in
   the file, printed once on stderr. Constant-time comparison. */
const char *secrets_admin_token(void);
int secrets_admin_token_matches(const char *candidate);

#endif
