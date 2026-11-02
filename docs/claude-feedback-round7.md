# claude-feedback-round7 — Accounts and servers, Reddit, Google CSE

> Prompt for a coding agent. Written 2026-11-02 after reading `omnisearch1_1.zip`
> (git `8484205`, "The palette pickers work again, Reddit takes three fields, and the
> accounts cards look like the rest of the page") and five screenshots from the owner.
> I read the code and `docs/`; I could not build or run it (beaker is not installed
> where I worked). Items tagged **[verify]** need a check on the owner's machine.
>
> **This file covers only what is new since round 6.** Do not recreate the older
> feedback files (`claude-feedback.md`, `claude-feedback-round6.md`); the owner removed
> them on purpose. Leave any other file in `docs/` alone.

## What the owner reported

1. The **Authorise with Reddit** button opens an error page (screenshot: `localhost:8087/settings/account-authorize?card=reddit&admin=` → "Access to localhost was denied, HTTP ERROR 403").
2. The **Save** buttons don't work; they open an error page.
3. The **Save** buttons are not styled with the current palette.
4. The owner has a Google Programmable Search Engine ("GOOG", search-the-entire-web ON, image search ON) and does not know how to set it up. Make it easy.

Plus everything found along the way (Part 2).

## Ground rules

- One logical change per commit; run `make verify` before each and paste its summary
  line in the report.
- **No secrets in the repo, the docs, the fixtures or the probe log.** That includes the
  Reddit values the owner supplied for a manual test (see M2) and any admin token.
- Tests use the fake upstreams (`tests/reddit_upstream.py`, `tests/fake_upstream.py`) and
  placeholder ids such as `test-client-id`. No live requests in tests.
- When you probe a live service by hand, make at most three requests, a minute apart, and
  log only the status codes in `docs/probe-log.tsv`.
- Use only the existing theme tokens (`--bg-card`, `--border`, `--text-primary`,
  `--text-secondary`, …). `tools/check_themes.py` must stay green.

## Order of work

M1 → M2 → M3 → M4, then Part 2 (X1–X4). M1 is the cause of reports 1 and 2.

---

## Part 1 — What the owner asked for

### M1. Every Accounts button returns 403, and the 403 page is blank

**Cause (confirmed in source, three separate faults):**

1. **The links can never carry the token.** Every `accounts_*` handler starts with
   `accounts_require_admin(params)`, which reads `admin` from the query string. The
   Authorise button is a plain anchor `…/account-authorize?card=reddit&admin=` and the
   Test button is `…/account-test?card=…&admin=`. Both have an empty `admin=` baked into
   the HTML, and `static/*.js` never mentions `admin`, so nothing fills it in. They fail
   every time, even for an owner who knows the token.
2. **Nobody can find the token.** It is a 64-character string created at first start and
   printed once to stderr. Under launchd that is `omnisearch.err`, and the page tells the
   owner to read `~/.omnisearch/secrets.ini`. The per-card "Admin token" field is empty
   for everyone who has not done that, so Save also returns 403.
3. **The explanation is lost.** The handlers do `send_status("403 Forbidden")` and then
   `send_response("<h1>Forbidden</h1>…")`. In this beaker the two do not stack: the
   response goes out with an empty body, so the browser shows its own "Access to
   localhost was denied" page. `SettingsSave.c` (the `toomany` branch) already records
   exactly this bug and works around it with `send_redirect`. The five Accounts handlers
   (`save`, `clear`, `authorize`, `code`, `test`) repeat it.

**Also wrong:** because beaker does not parse POST bodies (`docs/probe-log.tsv`,
`beaker-post`), these forms are `method="GET"`. The admin token, and the Reddit client
secret when there is one, therefore travel in the URL and stay in browser history.

**Fix:**

1. **Remove the paste-a-token step for the normal case.** What the token protects against
   is another machine or another web page changing settings. Replace it with three
   checks that need nothing typed:
   - the socket peer is loopback (`127.0.0.1` or `::1`). `RateLimit.c` already reads the
     peer and the raw request buffer;
   - the request is same-origin: `Sec-Fetch-Site: same-origin`, or where that header is
     absent, `Origin`/`Referer` host equal to `Host`. A page on another site cannot forge
     this, which also closes cross-site GET requests that change settings **[verify]**
     with a test that sends `Sec-Fetch-Site: cross-site`;
   - a **form nonce**: `/settings` renders a hidden `n` field (16 random bytes, in memory,
     15-minute TTL), and every write requires it. The owner never sees it.
2. **Keep the admin token only for non-loopback access** (someone using the instance from
   another device). Enter it once, then keep it in a cookie (`HttpOnly; SameSite=Strict`)
   instead of in every form and URL. `secrets.ini` keeps storing it.
3. **Turn the buttons into real controls.** Authorise is a link to Reddit's authorise URL
   built when the page is rendered (client id, redirect URI, a `state` nonce), opening in
   a new tab, with no round trip through this server. If the three fields are not saved
   yet, render it disabled with the reason beside it. Test and Clear are `<button>`s with
   `formaction` inside the card's form, so they carry the nonce.
4. **Errors stay on the page.** Every refusal on these routes becomes
   `send_redirect("/settings?err=<code>#accounts")`, and the settings page shows a banner
   in the theme's own colours: "That form had expired — reload the page and try again",
   "Writing settings from another device needs the admin token". Never emit an empty
   body. Add a test that fails if any of these handlers returns a non-redirect with
   `Content-Length: 0`.
5. **Get secrets out of the URL if beaker allows it.** Spike (one hour): can a handler read
   a POST body from the raw request buffer, the same way `RateLimit.c` reads headers? If
   yes, make the account routes POST-only with a bounded (8 KB) in-app body parser. If
   not, keep GET, send `Cache-Control: no-store`, answer with a redirect to a clean URL,
   and say in the page and in `docs/` that a secret typed here ends up in browser
   history. Recommend `secrets.ini` or an environment variable for the client secret and
   any API key.

**Done when:** on a fresh install with nothing pasted, from the loopback address, Save,
Test, Authorise, Finish and Clear all work from the page. From a non-loopback address the
same actions show the in-page "needs the admin token" banner. A request with
`Sec-Fetch-Site: cross-site` is refused. No account route ever returns an empty body.

### M2. Reddit: three fields, a flow that works with a custom redirect, and a caution

**What the owner asked for:** only Client ID, Redirect URI and User-Agent.

- The card still shows five fields plus an admin token (screenshot). Show the three by
  default. Put "Reddit username" and "Client secret" in a collapsed "Advanced" `<details>`
  inside the card. The admin token is gone from the card after M1.
- The "code" row has a second, truncated input ("paste the token to enabl…"): that was the
  admin token. After M1 the row is `code` + Finish.
- **Accept the whole pasted URL.** With a redirect such as `redreader://rr_oauth_redir` the
  browser cannot open the redirect, so the owner has to copy it from the address bar. Let
  the "code" box take either the bare code or the entire `…?code=…&state=…` string, extract
  the code, and **verify `state`** against the value created at Authorise. Refuse a mismatch
  with a plain sentence.
- **Say what happened.** After Finish show "Connected as u/<name>" (one `GET /api/v1/me`)
  instead of "Authorised." Add **Disconnect**, which revokes the token at Reddit
  (`/api/v1/revoke_token`) and not only deletes it locally.
- **Never log the token or the client id.** After a fake-upstream run, a test greps stderr,
  `omnisearch.err` and `cache/` for the refresh token and fails if it appears.

**Caution on the values the owner supplied for testing.** The client ID `yH0a…`, the
redirect `redreader://rr_oauth_redir` and the User-Agent `org.quantumbadger.redreader/1.27`
are **RedReader's** app identity (an open-source Android client), not the owner's. Reddit's
rules want an app you registered and a User-Agent that identifies you. Using another app's
identity from this program is impersonation, and the likely outcome is throttling or a
block that also hits RedReader's real users and the owner's account. Separately, from the
round-1 research: Reddit closed new API access requests on 2026-10-31 and the remaining
public API ends in March 2027 **[verify]**. So:

1. Do not write these values anywhere: no source, tests, fixtures, docs, example config,
   probe log or commit message. The owner may type them into the page once for a manual
   check; make no more than one Authorise and two searches, spaced a minute apart, and log
   status codes only.
2. Ship no default client id and no default User-Agent.
3. Plan for the shutdown. If Reddit answers 401/403 repeatedly, or says API access ended,
   the card shows "Reddit ended API access for this app — using the Reddit lane
   (`site:reddit.com`) instead", and the K5 lane carries on. Add `reddit_api = on|off`.
4. Say in `docs/k5-reddit.md` that the owner's test used a borrowed identity and that the
   durable route is the lane.

**Done when:** three fields visible by default; pasting a full redirect URL works; a wrong
`state` is refused; the card says who it is connected as; Disconnect revokes; the secret
scan test passes; no Reddit value from this report appears in `git grep`.

### M3. Save buttons are not in the palette

**Cause (from the CSS; I could not render the page):** the account forms use
`<button class="btn-primary">`, but `.btn-primary` is only styled inside scopes —
`.settings-actions .btn-primary`, `.view-home .btn-primary` — and `.settings-account
.btn-primary` sets only padding and font size (`main.css` ~2823). So the account Save falls
through to the generic `input, button, select, textarea` rule. `.btn-secondary` has the same
problem and has been copied six times to get around it (`.view-home`, `.settings-actions`,
`.settings-colours-actions`, `.settings-account`, `.settings-account-code`, …).

**Fix:**

1. One button family defined at top level, with tokens only: base, hover, active, focus
   ring, disabled, `a.btn-*` and `button.btn-*` identical. Delete the scope-specific copies.
   Keep the project's look (dim border, `--bg-card`), not a new one.
2. Two different "Save" buttons live on one page: a bottom "Save settings" (browser, stored in
   cookies) and per-card "Save changes" (server). Label them for what they do — "Save browser
   settings" and "Save server settings" — and visually separate the two groups.
3. Extend `tools/check_themes.py`: every `<button>` and `.btn-*` in `templates/` must resolve
   to the family; fail on a second definition of `.btn-primary`/`.btn-secondary` selector
   outside it. Extend `tools/theme_shot.py` to capture the settings page, accounts open,
   normal/hover/focus/disabled, for three themes, and attach the images to the report.

**Done when:** one definition of each button class, `check_themes.py` green with the new
rules, and the screenshots show identical Save/Test/Authorise metrics in three themes.

### M4. Google Programmable Search: one field, no restart, a Test that explains itself

**What the owner has (screenshots):** engine "GOOG", ID `a25d329ae5f514a70`, **Search the
entire web** ON, **Image search** ON. The Overview page offers three things people copy: the
**Search engine ID**, the **Public URL** `https://cse.google.com/cse?cx=…`, and the **Get
code** embed `<script async src="https://cse.google.com/cse.js?cx=…"></script>`.

**What is wrong today:**

- The Google card asks for "Search engine ID (cx)" **and** "API key". The JSON API behind the
  key closed to new customers in January 2026 and ends 2027-01-01, so a new engine can never
  have one. The hint "Only a key issued before January 2026 works" is the only clue. The
  round-3 work (`docs/G3-google-cse.md`) shows the Element endpoint needs only the cx.
- Saving a cx does not turn the provider on while the app is running. `google_cse_element`
  is configured and enabled in `Main.c` at startup (`api_google_cse_element_configure`,
  `engine_set_enabled`), and `apply_effective_settings()` only copies the strings
  **[verify by saving a cx and searching without restarting]**. The owner would save, see
  nothing, and not know why.
- The `Main.c` comment says the element engine is "Disabled by default either way", while
  the code enables it whenever a cx is set. Fix the comment.

**Fix:**

1. **One field.** "Paste your search engine ID, its public URL, or the embed code." Extract
   the cx with a tolerant parser: `cx=([A-Za-z0-9:_-]+)` from a URL or script tag, or a bare
   id. Show it back: "Using engine a25d329a…". Reject text without a recognisable id.
2. **Three lines of help in the card** (not a link to a manual): (1) open
   programmablesearchengine.google.com and click your engine; (2) copy **Search engine ID**
   from the Overview page; (3) paste it here, Save, press Test. Add the one setting that
   matters: **Search the entire web** must be on.
3. **API key under "Advanced"**, collapsed, labelled "only for keys issued before January
   2026".
4. **Saving enables and configures it immediately**: call `api_google_cse_element_configure`,
   `engine_set_enabled("google_cse_element", 1)` and the pace setting from the save path, and
   do the reverse on Clear. No restart. If something truly needs a restart, say so on the card.
5. **Test answers in plain words**, from one token fetch and one query ("linux kernel"):
   - "Working — 20 results in 0.9 s"
   - token page 404 → "Google does not know that ID — check you copied the Search engine ID"
   - 0 results → "Your engine returned nothing — switch on 'Search the entire web'"
   - 429 → "Google is rate-limiting this engine; paused for N minutes"
   - network error → "Could not reach google.com"
6. **Images.** The owner's engine has Image search on. Use it for the Images tab
   (`searchtype=image` on the element request) **[verify the parameter on the owner's
   engine]**, with its own fixture; if it does not work, say so in the card.
7. Keep the "experimental" label: it is the owner's own engine and quota, which is much
   closer to the intended use than a borrowed CX, but the endpoint is still undocumented.
   Ship no default cx. Test through the UI with the owner's ID; do not commit it.

**Done when:** on a fresh instance, pasting the public URL (or the embed code, or the bare id)
and pressing Save makes Google results appear in the next search without a restart; Test shows
one of the five sentences above; the card shows no API key field unless "Advanced" is opened.

---

## Part 2 — Found along the way

**X1. `reddit_client_secret` is silently dropped.** The Reddit card has five fields
(`fields[6]`), but `accounts_save_handler` and `accounts_clear_handler` check ownership with
`for (int f = 0; f < 4 …)`. The fifth field can never be saved or cleared, and nothing says so.
Use the array size. Add a test that saves and clears every field of every card.

**X2. The uploaded archive carries things it should not.** `omnisearch1_1.zip` contained:
- `secrets.ini` with the **64-character admin token**. Treat it as exposed: delete the
  `admin_token` line and restart to mint a new one (after M1 it matters only for non-loopback
  access). Do not paste it anywhere;
- 489 byte-identical copies of tracked files named `Foo 1.c`, `Foo 1.html`, … (macOS Finder
  copies). The Makefile does `SRCS := $(shell find src -name '*.c')`, so with these present
  `make` sees duplicate definitions and file names with spaces. Check the working tree, and
  make `SRCS` and `tools/dist_check.sh` reject names matching ` [0-9]\.`;
- `omnisearch.err`, `cache/`, `bin/` and `obj/`.
`tools/dist_check.sh` also crashes with `$1: unbound variable` when run without an argument;
give it a default. Share archives with `make dist` / `git archive`, never a folder zip.

**X3. Open Library shows "unreachable", 12 errors, 0 OK (settings screenshot).** The code
comment in `EngineDefs.c` says it answers in about 1.7 s against three engines and is
cancelled against eleven, because of the 2 s page deadline. That is a deadline problem, not a
reachability problem, yet it is recorded as `SUSPEND_TRANSPORT` and opens the breaker.
**[verify]** with `OMNI_PERF=1`. Fix: a deadline-cancel is not an error; give slow vertical
providers their own deadline and run them only for the matching intent or tab (books); show
"too slow for the main page" instead of "unreachable".

**X4. Non-secret fields are masked, and one is junk.** The screenshot shows `Reddit username`
as `****` while the Client ID, Redirect URI and User-Agent are empty. Only
`reddit_client_secret` is flagged secret, so show the saved value of every other field in
plain text, and mask only secrets. `secrets.ini` holds a **one-character** `reddit_username`;
it is probably a mask character saved by an earlier submission. Reject values made only of
mask characters, and show the real value so a bad one is visible.

---

## Report back

One line per ID (M1–M4, X1–X4): `fixed | not reproducible | deferred (why)` with the command,
test or commit that shows it. Then list anything you found that is not in this document, and
every **[verify]** you resolved, with the result.
