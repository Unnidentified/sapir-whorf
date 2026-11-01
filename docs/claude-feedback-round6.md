# claude-feedback — Round 6 (L): switches that read right, live bridges, Reddit with the owner's own app

> Prompt for a coding agent (Claude Code or similar).
> Based on the revised `omnisearch.zip` (git `7bb516b`, 310 commits, working tree of 2026-11-01),
> the owner's two screenshots of `/settings` and the owner's message. Written 2026-11-01.
> **This file contains only new material.** Earlier rounds are not repeated.
> **Do not restore** `docs/claude-feedback.md` or `docs/claude-feedback-round5.md`. The owner deleted them
> from the working tree (`git status` shows ` D` for both). Commit the deletion as its own commit and
> leave them gone. This is the only feedback file; if you keep a copy in the repo, keep this one.
> I read the code, the docs and the log. I did not build or run anything, and I do not have the owner's
> Reddit key and must not be given it (see L3). Items tagged **[verify]** could not be confirmed from
> source or from the pages I fetched. Reddit's OAuth rules were read on 2026-11-01 from Reddit's own
> archived wiki pages (listed under **Sources**).

## Your job

The owner made four requests and said the Reddit one is "a big one".

| # | Owner said | Verdict | Item |
|---|---|---|---|
| 1 | Every row says **On**, but no box is checked | By design, and the design is backwards: the box means "switch this **off**". Opt-in rows mean the opposite, so one list carries two meanings | L1 |
| 2 | If `lite.duckduckgo.com` serves nothing, remove it | It serves nothing (your own G1 probe: 14 results once, then an empty shell; the registry says it "stopped answering"). Remove it | L2 |
| 3 | Fill in the forms and links, make them editable, leave a form for the RedReader key | Nothing server-side is editable today. This needs secure storage, an admin gate and a forms section. It also serves L4 | L3 |
| 4 | "Activate the bridges and APIs" | Four rows are unavailable. Each needs something different, and one of them cannot be activated by anyone | L4 |
| 5 | With the key, Reddit search results should be possible | Yes. Reddit's app-only OAuth grant works for an app of "installed" type. The build has no OAuth rung; this is the big item | L5 |

L6 lists what I found on the way. Work in the order under **Order of work**.
Ignore anything here that conflicts with a direct instruction from the owner.

## What the screenshots are

1. `/settings` → "Search engines", Web and Academic-and-code groups. Every enabled row reads "On" with an empty
   box. DuckDuckGo Lite and YaCy read "Unavailable: disabled by the server administrator". The intro says "Tick a
   provider to switch it off." The Reddit row shows the host `reddit.com`.
2. The same page lower down: Reddit; Videos → Dailymotion only; Answers → Page descriptions (host "result
   sites"); Bridges and APIs → Google and Google (Programmable Search Element) "disabled by the server
   administrator", SearXNG "needs `searxng_url` in the config", Open Library "disabled by the server
   administrator". Then the default-engine select and Save.

## Ground rules (additions)

- **Secrets never leave the server's own memory and one file.** No client id, secret, token, `device_id` or
  authorization code in: logs, `omnisearch.err`, `/metrics`, `/settings/engine-status.json`, error messages,
  fixtures, test output, the template context dump, commit messages, or a cookie. Log "reddit token: ok, expires in
  3540 s", never the value. Today's access log writes the path without the query string (checked in
  `omnisearch.err`); keep it that way.
- **No default credentials, no borrowed ones.** Do not embed any client id. The G3 doc quotes a third party's
  `cx=partner-pub-…` from a probe; that value must not become a default anywhere.
- **Never lie about the User-Agent to Reddit.** Reddit's rules say so in as many words (L5). No RedReader, Sync or
  browser user-agent string on Reddit requests.
- Everything from earlier rounds still applies: no raw query text in logs, the browser never contacts a third
  party, paced probes (≤ 3 per hour per host, logged in `docs/probe-log.tsv`), one logical change per commit,
  `make verify` summary line, no inline hex, `[verify]` tags, and the 32-slot template context (K4 freed one slot;
  say how many are in use after your changes).

## Order of work

1. **L1** and **L2**. Small, certain, visible, and the owner's first two complaints.
2. **L3**, the credentials and server-config forms. L4 and L5 both need it.
3. **L4**, the bridges and APIs.
4. **L5**, Reddit with the owner's app. Do it last because it is the largest, but give it the most care.

---

## L1. "On" must mean the box is ticked

### Cause

`src/Routes/Settings.c:166-171` says it plainly in a comment: "the box is ticked when the provider is OFF, because
the form submits the disabled set." So `provider_data[prow][6]` is `"checked"` only when the state is `PS_OFF`. The
template (`templates/settings.html:323-325`) puts the word **On** beside a box that is empty for an on provider. The
intro (`locales/en_gb.ini:24`, `engines_off_explain`) tells the owner to "tick a provider to switch it off". It does
what it says, and it is the opposite of every other checkbox on the web.

It is also **inconsistent within the list**. For an opt-in provider the box is ticked when it is **on** (line 170,
`opted_in ? "checked"`), and the field name changes to `optin_` (line 179). Once an opt-in provider is registered, one
list will mix "tick = off" and "tick = on" under identical-looking boxes.

The reason is real: beaker keeps at most 16 URL parameters and silently drops the rest (`settings.html:306-313`), so
submitting the *on* set would break past 16 providers, and the page has about 25 rows. The design avoids that. But the
constraint is about what travels on the wire, not about what the box means on the screen.

### Fix

**The box means "on", always, including opt-in rows.** Ticked = this provider will be used. Unticked = off.
Unavailable rows are unticked and disabled.

Keep the wire format compact; that part is fine:

1. With scripts on: `static/settings.js` builds the single `off=` field from the boxes that are **unticked and not
   disabled**, and the named inputs are removed from the submission (`disabled` or no `name`). One parameter.
2. Without scripts: do not rely on the checkboxes. Give each row a small `<a>` or one-button form
   (`/save_settings?toggle=<id>`) that flips that one provider and redirects back with a 303. One parameter per request,
   no truncation, no JavaScript. Hide these links when scripts are on, or leave both; they do the same thing.
3. Opt-in rows use the same box and the same meaning. The field name differs on the server side only.
4. Rewrite `engines_off_explain` to say what the owner sees: "Untick a provider to switch it off. A provider that is
   off is never contacted…". Rename the state labels so they cannot disagree with the box: **On** only when ticked,
   **Off (by you)** only when unticked.
5. Instant toggling is optional but nice: with scripts on, a click can call `/save_settings?toggle=<id>` with `fetch`
   and update the row in place. If you do it, keep the Save button working for the other settings in the form.

### Tests

- A browser test (Playwright is in the container): for every row, `checkbox.checked === (state label === "On")`,
  `disabled` for every Unavailable row, and no row has any other combination. This is the test that was missing; the
  earlier ones checked the cookie, not what the owner sees.
- Click one provider off, save, reload: the box is unticked and the label reads "Off (by you)". Click it on, save,
  reload: ticked and "On". Repeat with 20 providers toggled at once (the 16-parameter case).
- Repeat both with scripts disabled, using the per-row links.
- The registry-wide "off means zero requests" test from K1.4 still passes unchanged.

---

## L2. Remove DuckDuckGo Lite

### Evidence

- `docs/G1-engines.md:36-40`: `lite.duckduckgo.com/lite/` returned 14 result links **once**, then 0 (a region-selector
  shell) for every later probe, with plain, Chrome, Firefox and Safari user agents.
- `src/Scraping/ScrapingParsers.c:571-586`: the registry entry has `.enabled = 0` and the comment says it "stopped
  answering from this host". It has been dead weight since G1. `ddg` (the `html.duckduckgo.com` endpoint) replaced it.

The owner's rule was "if it doesn't serve us anything, remove it." It does not. Remove it. Do not spend a probe
re-proving it. If you want one last data point, run **one** paced probe, log it, and remove it regardless of the
answer unless it returns 10 or more results.

### What to remove, and what to keep working

Remove:
- the `ddg-lite` entry (`ScrapingParsers.c:571-586`) and `parse_ddg_lite` (`ScrapingParsers.c:138`, and its
  declaration if any), plus any fixture used only by it;
- the `ddg-lite` branches in `src/Scraping/Scraping.c:66` and `:114`;
- `tests/test_g1_engines.sh:4,19,36`, which check that `ddg-lite` stays disabled: replace those with a check that the
  id is **unknown**, and keep the `google` and `searxng` cases;
- the `ddg-lite` cases in `tools/probe.sh` (lines 24, 95, 114) and the row in `docs/G1-engines.md` (keep the doc, add
  one line: "removed in Round 6");
- `docs/SETUP.md:132`, which still describes `ddg` as the "lite endpoint". It is the HTML endpoint.

Keep working (old browsers still hold the id):
- A cookie with `off=ddg-lite`, or the older `engines=…ddg-lite…`, must not produce an error or an "Unknown engine"
  warning on every request. In `src/Utility/Utility.c:550`, where `startpage→bing` and `mojeek→brave` are mapped,
  add `ddg-lite→ddg`. In the *disabled* set that means "the old name is now `ddg`", which is the same rule the existing
  comment states.
- `config.ini` lines such as `engines = "*,-ddg-lite"` should be accepted for one release with one startup notice, not
  one `[WARN]` per request.

### Acceptance

`grep -rn "ddg-lite\|lite.duckduckgo" src templates static tools` returns nothing except the alias line and the
G1 doc. `/settings` has no DuckDuckGo Lite row. A request with `Cookie: off=ddg-lite|bing` returns 200 and treats
Bing as off.

---

## L3. Editable server-side settings, and a form for the Reddit key

### Why this is its own item

Today everything the page edits is a **per-browser cookie**. The things the owner is asking about are **server
settings**: `searxng_url`, `google_cse_cx`, `yacy_instance`, and now a Reddit client id. They live in `config.ini` and
are read at startup (`src/Config.c:49-55`). Nothing lets the owner edit them from the page, and a cookie is the wrong
place for a credential: it is sent with every request, stored in the browser, and not shared with other browsers that
use the same instance.

### Storage

- A separate file, **not** `config.ini`, and **not** inside the project directory: `$HOME/.omnisearch/secrets.ini`,
  next to the cache directory the config already uses (`config.ini:30` points at `/Users/gefaass/.omnisearch/cache`).
  Create it with mode `0600`, in a directory with `0700`. Refuse to read it if it is group- or world-readable and say
  so at startup.
- Why not `config.ini`: it ships in the zip. The zip you were given contains the owner's `config.ini` with their home
  path in it, and it is carried around with the project. A key written there goes wherever the zip goes. Add
  `secrets.ini` to `.gitignore`, `.dockerignore` and the zip/release script.
- Non-secret server settings the form edits (`searxng_url`, `searxng_engines`, `yacy_instance`, `google_cse_cx`, the
  list of extra enabled engines) go in the same file under their own sections. One file the page owns, one file the
  owner owns (`config.ini`). If a key is in both, say which wins in `/settings` and in the startup log.
- Load order: `config.ini` → `secrets.ini` (overrides) → environment (`OMNI_REDDIT_CLIENT_ID` and so on, for Docker,
  overrides the file). The page shows the *effective* value's source ("from the environment, not editable here").

### The page

A new section in `/settings`: **Accounts and servers**, below Engines, one card per provider that takes
configuration. Each card shows: a one-line what-for, the exact fields, the helper links, a **Save** button, a
**Clear** button, a **Test** button, and the last test result with its time. Fields are editable text inputs styled by
the K2 token rule.

- **Secrets are write-only.** A saved secret shows as `•••••• saved (…ab12)`: last four characters only, never the
  value. Leaving the field blank on save keeps the old value. A separate **Clear** removes it. A client id for an
  installed app is not confidential in Reddit's sense, but treat it as private here: it ties the owner's searches to
  the owner's app.
- **Test** makes the smallest real call, shows a plain-language result and the HTTP status, and never prints a token,
  header or body. Rate-limit it (one test per 30 s) so a stuck button cannot hammer Reddit or a bridge.
- **Provider rows read from the same state.** A provider whose card is incomplete is **Unavailable: needs <field>**
  with a link that scrolls to its card. See L4.

### Who may edit: an admin gate

`/settings` has no authentication (the config comment says so), and `docker-compose.yml:6-7` publishes
`"8087:8087"`, which on Docker binds **all** host interfaces. Before the page can write a credential or turn a bridge
on, the write needs a gate:

- Require `admin_token` (random, 24+ bytes) for every write to the server-side settings. Generate it on first start if
  absent, write it into `secrets.ini`, and print it **once** on stderr at startup. The form asks for it in a password
  field; the server compares in constant time and rate-limits failures.
- Reads of the page do not need it, but the cards show only masked values without it.
- Do not use "is the peer loopback" as the gate. Inside Docker the peer is the bridge address, not loopback, so the
  check is wrong in exactly the deployment where it matters.
- Send these pages and responses with `Cache-Control: no-store` and `Referrer-Policy: no-referrer`.

### Transport: POST, if beaker has it

Every form in this project is `method="GET"`, and I found no code that reads a request body (`grep` for `body`,
`method`, `POST` in `src/Main.c` and `src/Routes/` finds none). **[verify]** whether beaker parses `POST` bodies: a
two-line test handler and `curl -d` settles it. If it does, the credential forms use POST. If it does not, the
fallback is a GET with fields in the query string, **and then** the save handler must answer `303` to a clean
`/settings`, set `no-store` and `no-referrer`, and the form must carry `autocomplete="off"`, so the secret does not
stay in history, `Referer` or the cache. The access log omits query strings today; add a test that fails if that
changes. Tell the owner, in the report, which transport was used.

### Applying changes

Saving must either apply live or say "restart needed". `apply_engines_config` mutates `ENGINE_REGISTRY[i].enabled`
(`ScrapingParsers.c:946-995`) while requests read it, so a live change is a data race unless it is done under a lock or
by swapping a copy. Choose the smaller thing: apply on the next restart and show a banner "Saved. Restart to apply."
for engine-list changes, and apply **immediately** only to values read per request (Reddit credentials, URLs).

### Tests

- Save a client id and a secret, GET `/settings`, assert neither appears in the HTML except as `…ab12`.
- `grep` `omnisearch.err`, `/metrics` and `/settings/engine-status.json` for the saved values: zero hits.
- A write without the admin token returns 403 and changes nothing.
- The file mode is `0600` after save and after a second save.
- The cookie jar after a save contains no credential.

---

## L4. Activate the bridges and APIs

The page says "disabled by the server administrator" for rows that the owner, who **is** the administrator, cannot
change from the page. That message is wrong for these rows and gives no way forward. Each row needs its own, true
reason and its own fix. What each one actually needs:

| Row | Why it is unavailable | What activates it | Can it be activated? |
|---|---|---|---|
| **Google (JSON API)**, host `www.googleapis.com` | needs `google_cse_key` and `google_cse_cx`. `config.ini:53-58` says the API has been **closed to new customers since 2026-01-20 and ends 2027-01-01**; the path "is dormant and will not be revived" | a key issued before 2026-01-20, plus a cx | **Only if the owner already holds such a key.** Give it the two fields in L3. Otherwise print "Needs a key. Google no longer issues them." and stop offering it |
| **Google (Programmable Search Element)**, `cse.google.com` | `ScrapingParsers.c` `.enabled = 0`, and G3 enables it "only when `google_cse_cx` is set" | a cx. The G3 doc says the page-scraped token is fetched automatically | Yes, if the owner has a cx **[verify]** that Google still lets a new Programmable Search Engine be created, and that the Element endpoint still answers (G3 is experimental). The owner supplies the cx; never ship one |
| **SearXNG** | `needs searxng_url in the config` | a running SearXNG instance and its URL | Yes. See below |
| **Open Library** | `static/engines.d/openlibrary.json` has `"enabled": false` and the comment "enable it in [engines] only after measuring it against the 2 s page deadline" | measure, then set `true` | Yes, and it needs no credentials |
| **YaCy** (in the Web group) | a YaCy peer at `yacy_instance` (default `127.0.0.1:8090`) must be running | a peer, and `yacy_instance` | Only if the owner runs one. Show "Needs a YaCy peer at <url>" and whether it is reachable |

### SearXNG: give the owner a working one

SearXNG is the documented low-risk route to Google's index, and it needs a second process. Make that one command:

- Add a `searxng` service to `docker-compose.yml` with a `settings.yml` **mounted from the repo** that sets
  `server.secret_key` (generate at first run, do not commit), `search.formats: [html, json]` and `server.limiter:
  false` for a private instance. Without `json` in `formats` SearXNG answers JSON requests with 403 **[verify]**, which
  looks exactly like "bridge broken". Bind it to `127.0.0.1` only.
- Default `searxng_url` in the compose file to the service name (`http://searxng:8080`) through the environment, so
  the owner does nothing.
- Keep `searxng_engines = google,brave,qwant` as the default list, editable in L3.
- **Test** in the card: a `GET <url>/search?q=test&format=json` and the count of results, plus a specific message for
  403 ("SearXNG refused JSON: enable `json` under `search.formats`") and for a connection error.

### Open Library: measure it, and consider the API

The shipped definition scrapes Open Library's HTML search page with XPath. Open Library also has a JSON search
endpoint (`openlibrary.org/search.json?q=…`) **[verify]**; a JSON engine is less brittle than an XPath over markup.
Do this: run the existing definition against the 2 s page deadline with five queries, record the timings in
`docs/probe-log.tsv`, and if it fits, set `"enabled": true`. If it does not fit, make it asynchronous like the page
descriptions rather than leaving it off. Either way, a book query (an author, a title, an ISBN) must return Open
Library rows in the results.

### Make "unavailable" tell the truth

Replace the single `disabled by the server administrator` text with a reason from a short enum, each with its fix:

| Reason | Text on the row |
|---|---|
| `needs_field` | "Needs <field>. Fill it in below." (link to the card) |
| `unreachable` | "Can't reach <host>." plus the last error class and time |
| `closed_upstream` | "The provider no longer accepts new users." |
| `off_in_config` | "Turned off in `config.ini` (`engines = …`). Remove `-id` or tick it below." |
| `not_in_build` | "Not part of this build." |

For rows the owner can fix from the page, the **tick box is enabled**: ticking it writes the extra-enabled list in
`secrets.ini` (L3). For rows that are blocked on a missing field, the box stays disabled until the field is saved and
tested. This also removes the "On/Off" confusion from L1 for these rows.

### Acceptance

- Open Library is On and returns book results.
- With a SearXNG instance up, the SearXNG row is On after the owner saves its URL and presses Test, and a query
  returns rows attributed to it. With it down, the row says "Can't reach http://…" and the search page still renders.
- Google (Element) turns On after a cx is saved and tested, and stays off otherwise.
- No row says "disabled by the server administrator" unless the cause is `off_in_config`.

---

## L5. Reddit results with the owner's registered app

This is the big one. The plan below is built on Reddit's own OAuth documentation, read today.

### What the owner has, and what the form must not assume

The owner registered an app on Reddit before app creation closed, for use with RedReader and the ReVanced build of
Sync for Reddit. That is an app of type **"installed app"** (RedReader's setup instructions use that type, and its
redirect URI is `redreader://rr_oauth_redir` **[verify against the owner's registration at
`https://www.reddit.com/prefs/apps`]**). An installed app has a **client id and no secret**.

The form must therefore take:
- **Client id** (required);
- **Client secret** (optional, empty for an installed app; only needed if the owner's app is a "script" or "web" type);
- **Redirect URI** (editable, prefilled with the value above, used only for the optional logged-in mode below);
- **Reddit username**, used in the User-Agent (Reddit's rule, below);
- **Mode**: *App only (no login)*, or *Logged in* (second stage).

It must show, beside the fields, the links the owner needs: `https://www.reddit.com/prefs/apps` (to read the client
id: it is the string under the app's name), Reddit's OAuth2 documentation, and a short note: "Use a client id **you
registered yourself**. Do not paste RedReader's own built-in id, and do not share yours."

### Rung A, first and enough on its own: app-only access

Reddit's OAuth wiki defines a grant for installed apps that needs no user login:

```
POST https://www.reddit.com/api/v1/access_token
Authorization: Basic base64(<client_id>:<client_secret or empty>)
Content-Type: application/x-www-form-urlencoded
grant_type=https://oauth.reddit.com/grants/installed_client&device_id=<20-30 ASCII chars>
```

Facts from the wiki:
- `device_id` is "a unique, per-device ID generated by your client", 20-30 ASCII characters, **reused** when
  renewing. Generate it once from a CSPRNG, store it in `secrets.ini`, and do not derive it from anything personal.
  (`DO_NOT_TRACK_THIS_DEVICE` is the documented opt-out value; do not use it, since Reddit asks for a stable id.)
- "All bearer tokens expire after 1 hour." App-only tokens "never receive a refresh_token", so renew by asking again.
- API calls go to **`https://oauth.reddit.com`**, never `www.reddit.com`, with `Authorization: bearer <token>`.
- The wiki is ambiguous about the Basic-auth password for the app-only grant (it says installed apps have no secret
  and, elsewhere, that the password is an empty string). Use `client_id:` with an empty password, and if the owner's
  app has a secret, send it. **[verify with the owner's real id]**: this is the one call to test first.

Search call (public listing endpoint; the `read` scope is the one this needs **[verify]** that the app-only token
may call `/search`):

```
GET https://oauth.reddit.com/search?q=<terms>&type=link&sort=relevance&t=all&limit=25&raw_json=1
GET https://oauth.reddit.com/r/<sub>/search?q=<terms>&restrict_sr=1&…      (for an r/<sub> query)
```

Map each `data.children[].data` to a `SearchResult`: `title`; URL = `https://www.reddit.com` + `permalink`;
snippet = the first 200 characters of `selftext` (empty for link posts: use the linked domain instead);
`subreddit_name_prefixed`, `score`, `num_comments`, `created_utc` and `author` go into the result fields the J5.2 work
already carries (`source`, `author`, `date`) plus the two counters in the row's meta line; `over_18` follows the
project's safe-search setting **[verify it exists]**. Group `index_group = "reddit"`. **Link posts only**: the search
endpoint does not search comments, and the report should say so.

### The rules Reddit sets, and how to obey them

- **User-Agent:** `<platform>:<app ID>:<version> (by /u/<username>)`, for example
  `linux:omnisearch:1.0 (by /u/<owner's name>)`. "NEVER lie about your user-agent." That includes impersonating a
  browser or "another bot"; Reddit says it bans liars. So the Reddit client **must not** use the browser
  profiles from `ScrapingHttp.c`, and it must not borrow RedReader's string. Build the UA from the form's username
  field; refuse to call Reddit with an empty one and say why.
- **Rate limit:** the archived wiki says 60 requests per minute for OAuth clients, one vendor page says about 100;
  neither is authoritative today. Read `X-Ratelimit-Used`, `X-Ratelimit-Remaining` and `X-Ratelimit-Reset` from
  **every** response and pace from them. Cap yourself well below: one search is one request, plus a token refresh
  an hour. At `Remaining < 5` stop until `Reset`, and show the time on the engine-status table (K3 already does
  absolute resume times).
- **Honest scope:** this is for the owner's own instance. `config.ini` binds to `127.0.0.1` for a reason. If the
  instance is ever reachable by other people, the owner's app, quota and Reddit terms are carrying them. Say so in
  the Reddit card when `host` is not loopback.
- **Disclosure** on the row (K1.5 style): "Sends your search terms to Reddit together with your app's client id."

### Token handling

- Fetch lazily on the first Reddit search, and again when `expires_in − 60 s` has passed, or once on a 401.
- **Single flight:** concurrent searches must not each fetch a token. One mutex, one in-flight refresh; the others wait
  or use the old token until it expires.
- Keep the token in memory only. Never write it to disk, the log, `/metrics` or the engine-status JSON.
- A failed token fetch is a **block signal** for the provider (feeds the breaker, shows on the status page), not "no
  results". A 200 with an `error` field is a failure; so is HTML where JSON was expected. K5.1 already learned both
  lessons for the archive providers; use the same helpers.

### Rung B (optional, after A works): logged in

Reddit's own wording, quoted by a vendor page, is that "logged-in and authenticated access won't be impacted" by the
changes. If the owner wants that mode (permanent token, higher limits, personalised results) **[verify]**, the flow is
the standard code flow, and the RedReader redirect makes it a copy-and-paste:

1. The card shows an **Authorize** link:
   `https://www.reddit.com/api/v1/authorize?client_id=<id>&response_type=code&state=<random>&redirect_uri=<uri>&duration=permanent&scope=read`.
   Generate `state` per click and remember it for ten minutes.
2. The owner approves in the browser. The browser then tries to open `redreader://rr_oauth_redir?code=…&state=…`,
   which a desktop browser cannot do, so the owner copies the full URL from the address bar or the error.
3. A **"Paste the address you were sent to"** field takes that URL. The server checks `state`, extracts `code`,
   exchanges it at the token endpoint with `grant_type=authorization_code`, stores the **refresh token** in
   `secrets.ini` (write-only, masked), and renews access tokens from it.
4. Codes are single-use. A reused or expired code gives a plain error and a fresh Authorize link, not a stack trace.

Build rung A first, ship it, and only then B.

### How it joins the existing lane

Today's Reddit provider (`Search.c:1517-1610`) is a **lane**: it asks DuckDuckGo and Yahoo for
`site:reddit.com …`. Keep that, as the fallback. The order becomes:

1. **OAuth search**, when credentials are saved, a token is held and the breaker is closed. This returns Reddit's own
   ranking with real scores and counts.
2. **The lane**, when OAuth is not configured, is suspended, or returned fewer than 3 rows. It needs no credentials and
   keeps working if Reddit later ends the Data API (reported as March 2027, **[verify]**).

When OAuth answers, skip the lane for that query: two more requests to two rate-limited engines for the same threads
is waste. The result rows say which route answered; the engine-status table says the same.

The provider row (`Providers.c:28`) now has a truthful destination. It currently prints `reddit.com`, but the query
goes to `duckduckgo.com` and `search.yahoo.com`. The distinction "lane" exists in the code (`Providers.c:165`,
`Main.c:178`) and is **not shown on the settings page**. Print, for the Reddit row: `oauth.reddit.com` when
configured, and "via DuckDuckGo and Yahoo (`site:reddit.com`)" when not.

### What Reddit may do, and the plan if it does

The vendor pages I read (all secondary; **[verify against Reddit]**) say: new API requests close on 2026-10-31
(yesterday); approvals have been rare since late 2025; access for apps that registered earlier continues until
roughly 2027-01-12, and the public Data API ends in March 2027. The owner registered before the closure, so the app
should keep working, but **not forever**. Therefore:

- The lane stays as the fallback, and the card says so.
- The engine-status table shows "Reddit (OAuth): ok / blocked since <time> / needs setup", so a quiet end of the app
  is visible within a day, not when the owner notices the results are thinner.
- A Test that fails with 401/403 says "Reddit has refused this app's client id" and links to the apps page.

### Tests

Extend `tests/fake_upstream.py` with `/api/v1/access_token` and `/search`; point the Reddit hosts at it through a test
config. Cases:

1. Happy path: token, search, 25 rows, fields mapped, permalinks absolute.
2. Token expiry: second search after `expires_in` fetches a new token; two concurrent searches fetch **one**.
3. 401 on search: one refresh, one retry, then give up.
4. 429 with `X-Ratelimit-Remaining: 0`, `Reset: 30`: no further Reddit request for 30 s; the status table shows it.
5. 200 with `{"error": …}`, and 200 with an HTML body: both recorded as failure, the lane takes over.
6. No credentials: zero Reddit requests; the lane runs as before.
7. The User-Agent on every Reddit request matches the regex of Reddit's format and contains the saved username; with
   no username saved, **no** request is made.
8. **No secret in any output** (L3's grep test), including with `OMNI_PERF` debug on.
9. Provider off in settings: zero Reddit requests of any kind, token fetches included.

And one **live** check the owner runs, once, from the Test button: it fetches a token, runs `search?q=test&limit=1`,
and prints "Reddit accepted this app. 1 result. Remaining 59 of 60, resets in 58 s." That is the evidence that the
real key works, and I cannot produce it.

### Acceptance

- With the key saved: `best rust ide reddit`, `r/rust async`, and `how to flash a router reddit` each show Reddit
  rows with subreddit, score and comment count, from the OAuth route.
- With the key cleared: the same queries still show Reddit rows through the lane, as before.
- No Reddit request is made when the provider is off or no username is saved.
- The Reddit card's Test result, the status-table line and the settings row agree with each other.

---

## L6. Found along the way

1. **The `r/` test matches inside words.** `Search.c:1533` is `strcasestr(raw_query, "r/")`. It fires on `and/or`,
   `color/colour`, `hammer/anvil`, `either/or` and any query with a word ending in "r" followed by a slash. Each one
   adds a `site:reddit.com` lane request. `QueryAnalysis.c:482-483` does it correctly (start of string or preceded by
   a space). Use that, and add the false-positive cases to the test.
2. **The Reddit row's host is wrong** (see L5): it says `reddit.com`; the request goes to two search engines. The
   `Providers.h` comment "the distinction is printed" holds for the startup log (`Main.c:178`) and not of the page.
3. **`docker-compose.yml:6-7` publishes `8087:8087` on every host interface**, while the config says "loopback by
   default" and "it has no authentication". Use `127.0.0.1:8087:8087`. This matters more now that the page will hold
   credentials.
4. **`config.ini` is in the zip** with the owner's home path (`/Users/gefaass/.omnisearch/cache`, `config.ini:30`),
   and so are `omnisearch.log`, `omnisearch.err` and `omnisearch.err.old`. All are gitignored and none is tracked, but
   they travel with an archive. Exclude them in the release/zip step, and add a pre-share check that fails if the
   archive contains `secrets.ini`, `*.err` or `*.log`.
5. **`docs/J-status.md:88` is stale.** It says K4, K5 and K6 are "not done", but the log has K4 steps 1-2, K5.1-K5.4
   and a K6 commit. Fix the table, and keep the
   "verified how" column honest: "ran in a browser" for the checkbox rows would have exposed L1.
6. **`docs/k5-reddit.md` calls the OAuth rung "not built, ~80 lines"** and decides it is not worth it, on the basis
   that the owner had no key. The owner has one. Update that row when L5 lands.
7. **Open Library's disabled-by-default reason is a measurement nobody made** (`openlibrary.json` comment). L4 asks
   for the five-query timing.
8. **The Videos group has one provider** (Dailymotion). K4 asked for several. Not this round's request; listed so it
   does not drop off.
9. **Page descriptions** shows host "result sites" and is a fetch-anywhere provider. When the owner turns it off the
   K7 placeholder filter must still hide the "We would like to show you a description…" text; confirm the filter runs
   whether or not the fetcher does.
10. **Legacy ids in cookies.** After L2, add a test that feeds the cookie parser every id this project has ever
    shipped (`startpage`, `mojeek`, `ddg-lite`, …) and asserts no warning and no error. The mapping table is the
    third copy of this idea in the code; make it one table.

---

## Report back

Reply with:

1. **Status per item L1–L5**: done / partial / not done, with the commit for each, and **how it was verified**
   (browser run, unit test, fake-upstream test, or the owner's real key). Say which of these you could **not** do.
2. **L1.** A screenshot of the Engines card at 1280 and 390 px in two themes, showing ticked = On. The Playwright
   result for "every row's box agrees with its label", with scripts on and off.
3. **L2.** The `grep` result, and the cookie test.
4. **L3.** Where credentials are stored, the file mode, the transport (POST or GET), whether beaker parses POST, how the
   admin token is created and shown, and the output of the "no secret in any output" grep.
5. **L4.** Per row: the state it ended in and the reason text. The Open Library timings. Whether SearXNG came up from
   `docker compose up` with JSON enabled.
6. **L5.** The result of tests 1-9, the transport used for the token, how many context slots the page now uses, and
   the point at which the owner has to press **Test** with the real key. Do not ask the owner to paste the key into
   chat or a file: it goes into the form only.
7. **A one-line `make verify` summary**, the things you could not verify (each tagged), and anything you changed that
   this file did not ask for, with the reason.

## Sources

Read 2026-11-01.

- Reddit OAuth2 (archived wiki): https://github.com/reddit-archive/reddit/wiki/OAuth2 — the `installed_client` grant,
  `device_id` rules, one-hour tokens, `oauth.reddit.com`, the code flow, scopes.
- Reddit API rules (archived wiki): https://github.com/reddit-archive/reddit/wiki/API — User-Agent format, "never lie
  about your user-agent", 60 requests per minute for OAuth clients, the three `X-Ratelimit-*` headers.
- BigIdeasDB, "Reddit API Shutdown 2027" (a vendor; one IP, one day): https://bigideasdb.com/reddit-api-shutdown-2027 —
  the closure dates and what still works. Treat the dates as **[verify]** against Reddit.
- Crawlora, "Why Reddit Blocked Unauthenticated JSON in 2026": https://crawlora.net/blog/reddit-json-api-blocked-2026
  — the quoted "logged-in and authenticated access won't be impacted".
- Not fetched and tagged **[verify]**: SearXNG's `search.formats` JSON requirement, Open Library's `search.json`,
  whether Google still lets a new Programmable Search Engine be created, and beaker's POST support.
