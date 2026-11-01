/* L5: Reddit, through the owner's own registered app.
 *
 * Rung A is the app-only ("installed_client") grant: no login, no redirect, no
 * refresh token - one POST to /api/v1/access_token with a client id and a
 * device id, then ordinary reads against oauth.reddit.com. Rung B (a real login
 * with a refresh token) is designed but not built; see docs/k5-reddit.md.
 *
 * The rules this file keeps, because Reddit states them:
 *   - the User-Agent is `<platform>:<app>:<version> (by /u/<username>)` and is
 *     never a browser's, never another app's. No username saved, no request;
 *   - every response's X-Ratelimit-Remaining / -Reset is read, and the client
 *     stops before the quota does;
 *   - tokens live in memory only, for their stated hour, refreshed under one
 *     lock so N concurrent searches make one token request;
 *   - a failure is a block signal for the provider, not "no results" - and a
 *     200 carrying an {"error": ...} body is a failure.
 */
#ifndef REDDIT_H
#define REDDIT_H

#include "Scraping.h"
#include <time.h>

typedef struct {
  char title[256];
  char permalink[512];
  char subreddit[64];
  char author[64];
  char selftext[512];
  char link_domain[128];
  int score;
  int comments;
  time_t created_utc;
  int over_18;
} RedditPost;

/* 1 when both a client id and a username are saved (app-only needs no secret).
   The username is not optional: it goes in the User-Agent, and Reddit's rules
   say the agent must not lie. */
int reddit_configured(void);

/* The User-Agent this client sends. Exposed so a test can assert the format. */
void reddit_user_agent(char *out, size_t cap);

/* Search. subreddit may be NULL (whole of Reddit) or "rust" (r/rust, with
   restrict_sr=1). Returns 0 on success and fills *out/*count, or -1 with a
   reason in why[] - never a silent empty list. */
int reddit_search(const char *terms, const char *subreddit, RedditPost **out,
                  int *count, char *why, size_t why_cap);

void reddit_free_posts(RedditPost *posts, int count);

/* The card's Test button: fetch a token, run one search of limit=1, and say
   what happened in one sentence. Never prints the token or a header. */
int reddit_test_credentials(const char *client_id, const char *client_secret,
                            char *out, size_t cap);

/* For the status table: "ok", "needs setup", or "blocked since <time>". */
const char *reddit_status_word(void);
time_t reddit_blocked_until(void);

#endif
