#include "Accounts.h"
#include "../Config.h"
#include "../Providers/Providers.h"
#include "../Utility/Effective.h"
#include "../Utility/Secrets.h"
#include "../Utility/HttpClient.h"
#include "../Utility/Utility.h"
#include "../Scraping/Reddit.h"
#include "Guard.h"
#include "../Scraping/CseId.h"
#include "../Scraping/ApiEngines.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* one card: a provider that takes configuration */
typedef struct {
  const char *id;        /* card id in the URL */
  const char *title;     /* what the page calls it */
  const char *why;       /* one line: what it is for */
  const char *fields[6]; /* SecretKey names, in display order */
  int secret[6];         /* 1 = write-only */
  const char *links;     /* one line of plain text under the title */
  const char *doc_url;   /* where to register the app, if anywhere */
  const char *doc_label; /* what that link is called */
} AccountCard;

static const AccountCard CARDS[] = {
    /* L5: three fields, and three is enough. The client id, the redirect URI
       the app was registered with, and the app's User-Agent. No secret: an
       installed app has none, and Reddit's app-only grant authenticates with
       the id alone. The username field stays for an app that has no UA of its
       own, where the documented UA format needs one. */
    {"reddit", "Reddit",
     "Search Reddit with the app you registered. Three fields, no secret.",
     {"reddit_client_id", "reddit_redirect_uri", "reddit_user_agent",
      "reddit_username", "reddit_client_secret", "reddit_api"},
     {0, 0, 0, 0, 1, 0},
     "Choose \"installed app\". The client id is the string under the app's "
     "name; the redirect URI must match the one you typed there; the "
     "User-Agent is your app's own, e.g. org.example.app/1.0."},
    {"searxng", "SearXNG",
     "A SearXNG instance is the documented low-risk route to Google's index.",
     {"searxng_url", "searxng_engines", NULL, NULL, NULL, NULL},
     {0, 0, 0, 0, 0, 0},
     "Run it with docker compose up searxng; the JSON format must be enabled "
     "there.", NULL, NULL},
    {"yacy", "YaCy",
     "A YaCy peer you run yourself. Off by default; most people do not have one.",
     {"yacy_instance", NULL, NULL, NULL, NULL, NULL},
     {0, 0, 0, 0, 0, 0},
     "A peer you run yourself.", "https://yacy.net/", "yacy.net"},
    /* M4: one field. The entry point accepts the id, the public URL or the
       embed code, extracts the cx, and enables the engine immediately. The API
       key is behind Advanced because the JSON API behind it closed to new
       customers in January 2026 and cannot be obtained now. */
    {"google", "Google",
     "Your Programmable Search Engine. Paste the id, the public URL or the embed code.",
     {"google_cse_cx", "google_cse_key", NULL, NULL, NULL, NULL},
     {0, 1, 0, 0, 0, 0},
     "Turn on \"Search the entire web\" in your engine's settings, or it will "
     "have nothing to search.",
     "https://programmablesearchengine.google.com/",
     "programmablesearchengine.google.com"},
};
enum { CARD_COUNT = sizeof(CARDS) / sizeof(CARDS[0]) };
/* one definition of "how many fields a card may have", used by the save path,
   the clear path and the row builder alike */
enum { ACCOUNT_FIELD_SLOTS = 6 };

/* M1: every refusal goes back to the page with a code, never to a blank body.
   send_status + send_response emits Content-Length: 0 in this beaker, which is
   why the owner saw the browser's own "Access to localhost was denied" page. */
static const char *guard_error_code(GuardResult r) {
  switch (r) {
  case GUARD_ERR_EXPIRED: return "expired";
  case GUARD_ERR_CROSS_SITE: return "cross";
  case GUARD_ERR_NEEDS_TOKEN: return "token";
  default: return "";
  }
}

static int refuse(GuardResult r) {
  char where[96];
  snprintf(where, sizeof(where), "/settings?err=%s#accounts", guard_error_code(r));
  send_redirect(where);
  return 0;
}

/* the one gate every write goes through */
static int require_write(UrlParams *params) {
  GuardResult r = guard_check_write(params);
  return r == GUARD_OK ? 1 : 0;
}

/* kept because Accounts.h declares it, but every route now goes through
   guard_check_write(); a second gate with different rules is how the two
   drifted apart in the first place */
int accounts_require_admin(UrlParams *params) {
  return guard_check_write(params) == GUARD_OK;
}

/* A field's human label and a one-line hint, keyed by the secret name. The
   page used to print the raw key with underscores swapped ("reddit client id"),
   which reads like a variable and names no product. */
typedef struct {
  const char *key;
  const char *label;
  const char *hint;
} FieldText;

static const FieldText FIELD_TEXT[] = {
    {"reddit_client_id", "Client ID", "Under the app's name at reddit.com/prefs/apps"},
    {"reddit_redirect_uri", "Redirect URI", "Exactly as registered, e.g. myapp://callback"},
    {"reddit_user_agent", "User-Agent", "Your app's own string, e.g. org.example.app/1.0"},
    {"reddit_username", "Reddit username", "Optional: only if your app has no User-Agent of its own"},
    {"reddit_client_secret", "Client secret", "Not needed for an installed app"},
    {"reddit_api", "Use Reddit's API", "\"on\" (default) or \"off\" to use the site:reddit.com lane only"},
    {"searxng_url", "Instance URL", "e.g. http://127.0.0.1:8888"},
    {"searxng_engines", "Engines", "Comma-separated, e.g. google,brave,qwant"},
    {"yacy_instance", "Instance URL", "e.g. http://127.0.0.1:8090"},
    {"google_cse_cx", "Search engine ID (cx)", "From programmablesearchengine.google.com"},
    {"google_cse_key", "API key", "Only a key issued before January 2026 works"},
};

/* "is this value only punctuation/masks?" - "*", "****", "••••" and the like */
static int no_letters_or_digits(const char *v) {
  if (!v || !v[0])
    return 0;
  for (const char *p = v; *p; p++)
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
        (*p >= '0' && *p <= '9'))
      return 0;
  return 1;
}

static const FieldText *field_text(const char *name) {
  for (size_t i = 0; i < sizeof(FIELD_TEXT) / sizeof(FIELD_TEXT[0]); i++)
    if (strcmp(FIELD_TEXT[i].key, name) == 0)
      return &FIELD_TEXT[i];
  return NULL;
}

static const AccountCard *card_by_id(const char *id) {
  if (!id)
    return NULL;
  for (size_t i = 0; i < CARD_COUNT; i++)
    if (strcmp(CARDS[i].id, id) == 0)
      return &CARDS[i];
  return NULL;
}

/* the last test result, kept in memory (never on disk: it can name a status
   code, not a secret) */
typedef struct {
  char result[256];
  time_t at;
} TestResult;
static TestResult g_tests[CARD_COUNT];

static void remember_test(const AccountCard *card, const char *text) {
  size_t i = (size_t)(card - CARDS);
  if (i >= CARD_COUNT)
    return;
  snprintf(g_tests[i].result, sizeof(g_tests[i].result), "%s", text);
  g_tests[i].at = time(NULL);
}

static void test_result_text(const AccountCard *card, char *out, size_t cap) {
  size_t i = (size_t)(card - CARDS);
  out[0] = '\0';
  if (i >= CARD_COUNT || !g_tests[i].result[0])
    return;
  char when[32];
  struct tm tm_local;
  localtime_r(&g_tests[i].at, &tm_local);
  strftime(when, sizeof(when), "%d %b %H:%M", &tm_local);
  snprintf(out, cap, "%s · %s", g_tests[i].result, when);
}

/* One FLAT matrix, one row per field, because beaker's {{for}} cannot nest: a
   card's fields are rows too, with the card's own data repeated and a flag that
   says "start of a card", so the template is a single loop.
   Columns: card id | card title | why | links | test text | field name |
            field label | masked value | source | credential | first-in-card */
enum { ACCOUNT_COLS = 27 };
/* 0 card id | 1 title | 2 why | 3 links | 4 test text | 5 field name |
   6 label | 7 masked value | 8 source | 9 credential | 10 first-in-card |
   11 input type | 12 last-in-card | 13 hint | 14 optional marker |
   15 authorise link (Reddit only) | 16 doc URL | 17 doc label |
   18 "local"/"remote" | 19 "1" when the admin token must be asked for |
   20 the value to show (empty for a secret) |
   21 the state nonce this link carries | 22 why Authorise is unavailable |
   23 "1" when the Authorise link exists | 24 "1" when it must be disabled |
   25 "1" opens the Advanced block | 26 "1" closes it */

AccountCards accounts_build(const char *locale_id) {
  (void)locale_id;
  AccountCards cards;
  cards.count = 0;
  int total = 0;
  for (size_t c = 0; c < CARD_COUNT; c++)
    for (int f = 0; f < 6 && CARDS[c].fields[f]; f++)
      total++;
  cards.rows = calloc((size_t)total + 1, sizeof(char **));
  cards.inner = calloc((size_t)total + 1, sizeof(int));
  if (!cards.rows || !cards.inner) {
    free(cards.rows);
    free(cards.inner);
    cards.count = 0;
    return cards;
  }

  int row = 0;
  for (size_t c = 0; c < CARD_COUNT; c++) {
    const AccountCard *card = &CARDS[c];
    char test_text[320];
    test_result_text(card, test_text, sizeof(test_text));
    int first = 1;
    for (int f = 0; f < 6 && card->fields[f]; f++) {
      SecretKey key = secrets_key_from_name(card->fields[f]);
      if (key == SK_COUNT)
        continue;
      const char *value = "";
      SecretSource src = secrets_get(key, &value);
      char masked[64] = "";
      /* X4: only a SECRET is masked. A client id, a redirect URI, a User-Agent
         and an instance URL are not secrets - the client id travels in a URL
         Reddit documents - so masking them hid a typo without protecting
         anything, and the owner saw **** where they needed to check a value. */
      if (value[0]) {
        if (card->secret[f])
          secrets_mask(key, masked, sizeof(masked));
        else
          snprintf(masked, sizeof(masked), "%s", value);
      }
      /* X4: a value made only of mask characters is junk that an earlier
         submission saved; never show it back, and never treat it as set. */
      if (value[0] && !card->secret[f] && no_letters_or_digits(value))
        masked[0] = '\0';
      const FieldText *ft = field_text(card->fields[f]);
      char label[96];
      snprintf(label, sizeof(label), "%s",
               ft && ft->label ? ft->label : card->fields[f]);
      EffectiveSource eff = src == SECRET_SOURCE_ENV
                                ? EFFECTIVE_FROM_ENV
                            : src == SECRET_SOURCE_FILE ? EFFECTIVE_FROM_FILE
                                                        : EFFECTIVE_FROM_DEFAULT;

      /* every column index this builder writes must be < ACCOUNT_COLS. A
         marker added at 25/26 while ACCOUNT_COLS was 25 wrote two pointers past
         the allocation, and the {{if}} that read them silently never fired. */
      _Static_assert(ACCOUNT_COLS >= 27,
                     "raise ACCOUNT_COLS when adding a column");
      cards.rows[row] = calloc(ACCOUNT_COLS + 1, sizeof(char *));
      if (!cards.rows[row])
        break;
      cards.rows[row][0] = strdup(card->id);
      cards.rows[row][1] = strdup(card->title);
      cards.rows[row][2] = strdup(card->why);
      cards.rows[row][3] = strdup(card->links);
      cards.rows[row][4] = strdup(test_text);
      cards.rows[row][5] = strdup(card->fields[f]);
      cards.rows[row][6] = strdup(label);
      cards.rows[row][7] = strdup(masked);
      cards.rows[row][8] = strdup(effective_source_label(eff));
      cards.rows[row][9] = strdup(card->secret[f] ? "1" : "");
      cards.rows[row][10] = strdup(first ? "1" : "");
      /* 11: the input type, decided here so the template needs no nested if */
      cards.rows[row][11] = strdup(card->secret[f] ? "password" : "text");
      /* 12: the last field of a card closes the card's markup */
      cards.rows[row][12] =
          strdup(card->fields[f + 1] ? "" : "1");
      /* 13: the hint under the field; 14: "optional" when a blank is fine */
      cards.rows[row][13] = strdup(ft && ft->hint ? ft->hint : "");
      cards.rows[row][14] = strdup(card->secret[f] ? "optional" : "");
      /* 15: the Authorise link, built here rather than routed through this
         server - the browser goes straight to Reddit. Empty when the fields it
         needs are not saved yet, and 22 then says why. */
      cards.rows[row][15] = strdup("");
      cards.rows[row][22] = strdup("");
      cards.rows[row][23] = strdup(""); /* "1" when Authorise can be offered */
      cards.rows[row][24] = strdup(""); /* "1" when it must be drawn disabled */
      if (first && strcmp(card->id, "reddit") == 0) {
        char url[1200];
        char state[40] = "";
        char why[192] = "";
        guard_new_nonce(state, sizeof(state)); /* a state nonce, same TTL store */
        if (reddit_authorize_url(state, url, sizeof(url), why, sizeof(why)) == 0) {
          free(cards.rows[row][15]);
          cards.rows[row][15] = strdup(url);
          free(cards.rows[row][21]);
          cards.rows[row][21] = strdup(state);
          free(cards.rows[row][23]);
          cards.rows[row][23] = strdup("1");
          free(cards.rows[row][24]);
          cards.rows[row][24] = strdup(""); /* no disabled twin */
        } else {
          free(cards.rows[row][22]);
          cards.rows[row][22] = strdup(why);
          free(cards.rows[row][24]);
          cards.rows[row][24] = strdup("1");
        }
      }
      /* 16/17: the doc link, repeated on every row of the card so the template
         needs no second lookup. Only the first row renders it. */
      cards.rows[row][16] = strdup(card->doc_url ? card->doc_url : "");
      cards.rows[row][17] = strdup(card->doc_label ? card->doc_label : "");
      cards.rows[row][18] =
          strdup(guard_peer_is_loopback() ? "local" : "remote");
      /* 19: "1" when this card must ask for the admin token, i.e. not loopback */
      cards.rows[row][19] = strdup(guard_peer_is_loopback() ? "" : "1");
      /* M2: the Reddit card shows three fields by default and puts the rest
         behind "Advanced". 25/26 mark the first and last advanced row. */
      cards.rows[row][25] = strdup("");
      cards.rows[row][26] = strdup("");
      {
        /* each card names how many of its fields are primary */
        int primary = (strcmp(card->id, "reddit") == 0) ? 3
                    : (strcmp(card->id, "google") == 0) ? 1 : ACCOUNT_FIELD_SLOTS;
        if (f == primary && card->fields[f]) {
          free(cards.rows[row][25]);
          cards.rows[row][25] = strdup("1"); /* opens the block */
        }
        if (f > primary && !card->fields[f + 1]) {
          free(cards.rows[row][26]);
          cards.rows[row][26] = strdup("1"); /* closes it */
        }
      }
      /* 20: what the input displays. A secret leaves this empty and shows the
         mask as a placeholder, so the real value never enters the HTML. */
      cards.rows[row][20] = strdup(card->secret[f] ? "" : masked);
      cards.inner[row] = ACCOUNT_COLS;
      first = 0;
      row++;
    }
  }
  cards.count = row;
  return cards;
}

void accounts_free(AccountCards *cards) {
  if (!cards || !cards->rows)
    return;
  for (int c = 0; c < cards->count; c++) {
    if (!cards->rows[c])
      continue;
    for (int f = 0; f < ACCOUNT_COLS + 1 && cards->rows[c][f]; f++)
      free(cards->rows[c][f]);
    free(cards->rows[c]);
  }
  free(cards->rows);
  free(cards->inner);
  cards->rows = NULL;
  cards->inner = NULL;
  cards->count = 0;
}

static void reddit_test_now(char *out, size_t cap);

int accounts_save_handler(UrlParams *params) {
  if (!require_write(params))
    return refuse(guard_check_write(params));

  const char *card_id = "";
  const char *action = "";
  for (int i = 0; params && i < params->count; i++) {
    const char *key = params->params[i].key;
    const char *value = params->params[i].value;
    if (strcmp(key, "card") == 0) {
      card_id = value;
      continue;
    }
    /* M1.3: Test is a submit button inside the card's form so that it carries
       the nonce; the action decides what runs after the fields are read. */
    if (strcmp(key, "action") == 0) {
      action = value;
      continue;
    }
    if (strcmp(key, "n") == 0)
      continue;
    if (strcmp(key, "admin") == 0 || strcmp(key, "cb") == 0)
      continue;
    /* a field is only accepted when the card owns it, so this endpoint cannot
       write an arbitrary key */
    const AccountCard *card = card_by_id(card_id);
    if (!card)
      continue;
    int owned = 0;
    /* X1: this was a literal 4 while a card may declare six fields, so the
       fifth and sixth could never be saved. Use the array's own size. */
    for (int f = 0; f < ACCOUNT_FIELD_SLOTS && card->fields[f]; f++)
      if (strcmp(card->fields[f], key) == 0)
        owned = 1;
    if (!owned)
      continue;
    SecretKey sk = secrets_key_from_name(key);
    if (sk == SK_COUNT)
      continue;
    /* blank means "keep the old value", which is what a masked field shows */
    if (!value[0])
      continue;
    /* X4: secrets.ini held a one-character reddit_username, which is a mask
       character from an earlier submission. Saving that again would store junk
       the owner cannot see. Refuse it. */
    {
      int is_secret = 0;
      for (int sf = 0; sf < ACCOUNT_FIELD_SLOTS && card->fields[sf]; sf++)
        if (strcmp(card->fields[sf], key) == 0)
          is_secret = card->secret[sf];
      if (!is_secret && no_letters_or_digits(value)) {
        remember_test(card, "That value is punctuation only - it looks like a "
                            "mask, not something you typed. Nothing was saved.");
        continue;
      }
    }
    /* M2: a User-Agent naming another project is impersonation. Refuse it here,
       where it is typed, rather than sending it to Reddit. */
    if (sk == SK_REDDIT_USER_AGENT && reddit_agent_is_foreign(value)) {
      remember_test(card, "That User-Agent belongs to another app. Use your own "
                          "- register one at reddit.com/prefs/apps.");
      continue;
    }
    /* M4.1: one field, three accepted forms. Extract the cx before storing, so
       a pasted URL or embed tag is not stored verbatim as a broken id. */
    char cse_id[128];
    if (sk == SK_GOOGLE_CSE_CX) {
      if (cse_extract_id(value, cse_id, sizeof(cse_id)) != 0) {
        remember_test(card, "That does not look like a search engine id. Paste "
                            "the Search engine ID, its public URL, or the "
                            "embed code.");
        continue;
      }
      value = cse_id;
    }
    if (secrets_set(sk, value) != 0)
      fprintf(stderr, "[WARN] account save failed for %s\n", key);
  }
  secrets_reload_after_save();
  {
    extern Config global_config;
    apply_effective_settings(&global_config);
    /* M4.4: saving a cx must take effect while the process runs. The engine was
       configured only at startup, so an owner who saved one saw nothing and had
       no way to know a restart was needed. Do the same work here, and undo it
       on clear. */
    extern void omnisearch_apply_google_cse(const char *cx, const char *tok);
    omnisearch_apply_google_cse(global_config.google_cse_cx,
                                global_config.google_cse_tok);
  }
  /* M1.3: the same form posts Save and Test. Test runs after the values are
     stored, so it tests what was just entered rather than what was there. */
  if (strcmp(action, "test") == 0 && card_id[0]) {
    const AccountCard *card = card_by_id(card_id);
    if (card) {
      char text[256];
      size_t idx = (size_t)(card - CARDS);
      if (strcmp(card->id, "reddit") == 0)
        reddit_test_now(text, sizeof(text));
      else if (strcmp(card->id, "google") == 0) {
        const char *cx = "";
        secrets_get(SK_GOOGLE_CSE_CX, &cx);
        api_google_cse_element_selftest(cx, text, sizeof(text));
      } else
        snprintf(text, sizeof(text), "Saved. Press Test again to check it.");
      remember_test(card, text);
      (void)idx;
    }
  }
  send_redirect("/settings#accounts");
  return 0;
}

/* the "Clear" button: one field, no value at all */
int accounts_clear_handler(UrlParams *params) {
  if (!require_write(params))
    return refuse(guard_check_write(params));
  const char *card_id = "", *field = "";
  for (int i = 0; params && i < params->count; i++) {
    if (strcmp(params->params[i].key, "card") == 0)
      card_id = params->params[i].value;
    else if (strcmp(params->params[i].key, "field") == 0)
      field = params->params[i].value;
  }
  const AccountCard *card = card_by_id(card_id);
  if (card) {
    for (int f = 0; f < ACCOUNT_FIELD_SLOTS && card->fields[f]; f++)
      if (strcmp(card->fields[f], field) == 0)
        secrets_set(secrets_key_from_name(field), "");
  }
  secrets_reload_after_save();
  {
    /* M4.4: clearing the cx turns the engine off live, the same way saving it
       turns it on. Without this the engine kept answering with a cx that the
       page no longer showed. */
    extern Config global_config;
    extern void omnisearch_apply_google_cse(const char *cx, const char *tok);
    apply_effective_settings(&global_config);
    omnisearch_apply_google_cse(global_config.google_cse_cx,
                                global_config.google_cse_tok);
  }
  send_redirect("/settings#accounts");
  return 0;
}

/* The smallest real call each card can make. Nothing here prints a token, a
   header or a body: only a sentence and an HTTP status. One per 30 seconds per
   card, so a stuck button cannot hammer Reddit or a bridge. */
static void reddit_test(const char *client_id, const char *secret, char *out,
                        size_t cap);
static void reddit_test_now(char *out, size_t cap);

/* L5: the Authorise button. Sends the browser to Reddit's authorisation page
   built from the saved client id and redirect URI - the browser talks to
   Reddit, this server never sees the login. */
int accounts_authorize_handler(UrlParams *params) {
  if (!require_write(params))
    return refuse(guard_check_write(params));
  char url[1200];
  char why[192] = "";
  if (reddit_authorize_url("", url, sizeof(url), why, sizeof(why)) != 0) {
    send_redirect("/settings?authwhy=1#accounts");
    return 0;
  }
  send_redirect(url);
  return 0;
}

/* L5: the code Reddit appends to the redirect URI comes back here (the owner
   pastes it, or the app forwards it). It becomes a refresh token, so every
   later search runs without a browser. */
int accounts_code_handler(UrlParams *params) {
  if (!require_write(params))
    return refuse(guard_check_write(params));
  const char *code = "";
  const char *state = "";
  for (int i = 0; params && i < params->count; i++) {
    if (strcmp(params->params[i].key, "code") == 0)
      code = params->params[i].value;
    else if (strcmp(params->params[i].key, "state") == 0)
      state = params->params[i].value;
  }
  char why[256] = "";
  char who[64] = "";
  char text[320];
  size_t idx = 0;
  for (size_t c = 0; c < CARD_COUNT; c++)
    if (strcmp(CARDS[c].id, "reddit") == 0)
      idx = c;
  if (reddit_exchange_code(code, state, why, sizeof(why), who, sizeof(who)) == 0) {
    /* M2: say who, not just "authorised" */
    if (!who[0])
      reddit_whoami(who, sizeof(who), why, sizeof(why));
    if (who[0])
      snprintf(text, sizeof(text), "Connected as u/%s", who);
    else
      snprintf(text, sizeof(text), "Connected. Searches now use your app.");
  } else {
    snprintf(text, sizeof(text), "Could not finish: %.200s", why);
  }
  remember_test(&CARDS[idx], text);
  send_redirect("/settings#accounts");
  return 0;
}

int accounts_test_handler(UrlParams *params) {
  if (!require_write(params))
    return refuse(guard_check_write(params));
  const char *card_id = "";
  for (int i = 0; params && i < params->count; i++)
    if (strcmp(params->params[i].key, "card") == 0)
      card_id = params->params[i].value;
  const AccountCard *card = card_by_id(card_id);
  if (!card) {
    send_status("404 Not Found");
    send_response("<h1>Unknown card</h1>");
    return 0;
  }

  char text[256];
  size_t idx = (size_t)(card - CARDS);
  time_t now = time(NULL);
  if (g_tests[idx].at && now - g_tests[idx].at < 30) {
    snprintf(text, sizeof(text), "Just tested. Wait %d s before testing again.",
             (int)(30 - (now - g_tests[idx].at)));
    remember_test(card, text);
  } else if (strcmp(card->id, "reddit") == 0) {
    const char *id = "";
    const char *secret = "";
    secrets_get(SK_REDDIT_CLIENT_ID, &id);
    secrets_get(SK_REDDIT_CLIENT_SECRET, &secret);
    reddit_test(id, secret, text, sizeof(text));
    remember_test(card, text);
  } else if (strcmp(card->id, "searxng") == 0) {
    const char *url = "";
    secrets_get(SK_SEARXNG_URL, &url);
    if (!url[0])
      snprintf(text, sizeof(text), "No URL saved yet.");
    else {
      char reply[256];
      if (searxng_test(url, reply, sizeof(reply)) == 0)
        snprintf(text, sizeof(text), "%s", reply);
      else
        snprintf(text, sizeof(text), "Can't reach %s", url);
    }
    remember_test(card, text);
  } else if (strcmp(card->id, "yacy") == 0) {
    const char *url = "";
    secrets_get(SK_YACY_INSTANCE, &url);
    char probe[512];
    snprintf(probe, sizeof(probe), "%s/Status.html", url[0] ? url : DEFAULT_YACY_INSTANCE);
    int reachable = url_reachable(probe, 2500);
    snprintf(text, sizeof(text), reachable ? "A YaCy peer answers at %s"
                                           : "No peer answers at %s",
             url[0] ? url : DEFAULT_YACY_INSTANCE);
    remember_test(card, text);
  } else if (strcmp(card->id, "google") == 0) {
    const char *cx = "";
    secrets_get(SK_GOOGLE_CSE_CX, &cx);
    snprintf(text, sizeof(text),
             cx[0] ? "A cx is saved. The Element endpoint is experimental."
                   : "No cx saved. Google's JSON API no longer issues keys.");
    remember_test(card, text);
  } else {
    snprintf(text, sizeof(text), "Nothing to test.");
    remember_test(card, text);
  }

  send_redirect("/settings#accounts");
  return 0;
}

/* is anything listening at this URL? HEAD first, GET as a fallback, 2.5 s */
int url_reachable(const char *url, long timeout_ms) {
  if (!url || !url[0])
    return 0;
  CURL *curl = curl_easy_init();
  if (!curl)
    return 0;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (compatible; omnisearch/1.0)");
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  return rc == CURLE_OK && status > 0 && status < 500;
}

/* SearXNG: one JSON search. A 403 here almost always means the instance does
   not have `json` in search.formats, which is worth saying in words. */
int searxng_test(const char *base_url, char *out, size_t cap) {
  char url[600];
  snprintf(url, sizeof(url), "%s/search?q=test&format=json", base_url);
  CURL *curl = curl_easy_init();
  if (!curl)
    return -1;
  char *body = NULL;
  size_t len = 0;
  FILE *mem = tmpfile();
  if (!mem) {
    curl_easy_cleanup(curl);
    return -1;
  }
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, mem);
  curl_easy_setopt(curl, CURLOPT_USERAGENT,
                   "Mozilla/5.0 (compatible; omnisearch/1.0)");
  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  if (rc != CURLE_OK) {
    fclose(mem);
    return -1;
  }
  if (status == 403) {
    snprintf(out, cap,
             "SearXNG refused JSON (403): enable \"json\" under "
             "search.formats");
    fclose(mem);
    return 0;
  }
  if (status != 200) {
    snprintf(out, cap, "SearXNG answered %ld", status);
    fclose(mem);
    return 0;
  }
  fseek(mem, 0, SEEK_END);
  long size = ftell(mem);
  fseek(mem, 0, SEEK_SET);
  if (size > 0 && size < 1024 * 1024) {
    body = malloc((size_t)size + 1);
    if (body) {
      len = fread(body, 1, (size_t)size, mem);
      body[len] = '\0';
    }
  }
  fclose(mem);
  if (!body) {
    snprintf(out, cap, "SearXNG answered 200 with an unreadable body");
    return 0;
  }
  /* count the results without a JSON parser dependency here: the field is a
     top-level array of objects, and the count is what the owner needs */
  int results = 0;
  for (const char *p = body; (p = strstr(p, "\"url\"")) != NULL; p++)
    results++;
  snprintf(out, cap, "SearXNG answered: %d result%s in JSON", results,
           results == 1 ? "" : "s");
  free(body);
  return 0;
}

/* Reddit: fetch an app-only token and ask one question. This is the evidence
   the owner runs once with their own key; it prints a sentence and a status,
   never the token, the header or the body. */
/* reads the stored credentials and tests them; used by the card's Test button */
static void reddit_test_now(char *out, size_t cap) {
  const char *id = "";
  const char *secret = "";
  secrets_get(SK_REDDIT_CLIENT_ID, &id);
  secrets_get(SK_REDDIT_CLIENT_SECRET, &secret);
  extern void reddit_test(const char *client_id, const char *secret, char *out,
                          size_t cap);
  reddit_test(id, secret, out, cap);
}

static void reddit_test(const char *client_id, const char *secret, char *out,
                        size_t cap) {
  if (!client_id || !client_id[0]) {
    snprintf(out, cap, "No client id saved yet.");
    return;
  }
  extern int reddit_test_credentials(const char *client_id,
                                     const char *client_secret,
                                     char *out, size_t cap);
  reddit_test_credentials(client_id, secret, out, cap);
}
