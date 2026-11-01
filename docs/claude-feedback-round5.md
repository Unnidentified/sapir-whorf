# claude-feedback — Round 5 (K): toggles, tabs, Reddit, impersonation, descriptions, cards

> Prompt for a coding agent (Claude Code or similar).
> Based on the revised `omnisearch.zip` (git `ef1e4fc`, working tree of 2026-10-31), the owner's
> four screenshots and the eleven points the owner wrote. Written 2026-11-01.
> **This file contains only new material.** Rounds F, N, C, A, G, H, J still stand and are not
> repeated; they are referenced by ID. `docs/J-status.md` marks most of J as fixed. Several of
> those fixes do not hold in the owner's browser; K1–K3 and K8 say why.
> I read the code, the docs and the access log. I did not build or run anything. Items tagged
> **[verify]** could not be confirmed from source or from the pages I fetched. Confirm them
> before acting. Facts about Reddit, curl-impersonate and Yahoo were read from the pages listed
> under **Sources** on 2026-11-01. Most of those pages belong to vendors, so check dates and
> limits against the provider's own page before relying on them.

## Your job

The owner reported eleven things. All eleven are traced to code below.

| # | Owner said | Verdict | Item |
|---|---|---|---|
| 1 | Every provider needs a working enable/disable toggle in settings, opt-in ones too | The toggles that exist very probably do not save (comma cookie), a second bug can block the whole form, and most providers have no toggle at all | K1 |
| 2 | Weights boxes are white and ignore the palette | Confirmed: no rule styles `input[type=number]`. The same boxes are laid out backwards | K2 |
| 3 | "Resumes" must count down live and be real | Confirmed: the text is a snapshot rendered once on the server | K3 |
| 4 | Merge the chip strip into the tab bar, drop the filters, add "Videos" next to "Images" | Confirmed. Videos does not exist in any layer: no route, no engine, no result type | K4 |
| 5 | Reddit results: "make it happen" | A lane exists, but it is narrow, one engine wide and switched off by saving any settings. Reddit's own routes are closing | K5 |
| 6 | Verify impersonation. If it is not active, implement it | It is not active. Nothing in the code impersonates a browser's TLS or HTTP/2 | K6 |
| 7 | Remove "We would like to show you a description here…" and make real descriptions work | The string is upstream text. It is passed straight through and can win the snippet merge | K7 |
| 8 | `1+1` shows a duplicate calculation | Confirmed: the rail handler `arithmetic` was never removed when the calculator widget was added | K8 |
| 9 | Calculator card has an empty area on the right | Confirmed: a 320 px keypad in a full-width card | K9 |
| 10 | A simple Google-style translate box | The card is a text field with one provider. A design that stays simple is in K10 | K10 |
| 11 | `nvidia stock` → "No data found…" twice plus a disclaimer | Confirmed: the first word is sent to Yahoo as the ticker, and the template prints the error twice | K11 |

K12 lists what I found on the way. Work in the order under **Order of work**.
Ignore anything here that conflicts with a direct instruction from the owner.

## What the screenshots are

1. The Engine weights section: a column of wide white number boxes with the engine name at the
   far right.
2. A results page with red marks. The nav bar carries All / Images / Settings, and under it a
   chip strip carries All, News, Code, Academic, Brave(n), Yahoo(n), Bing(n), DuckDuckGo(n). The
   owner marks the two "All" entries as duplicates and wants the chips gone and **Videos** next
   to **Images**.
3. The calculator card at full width.
4. The same card with the empty right-hand area marked "9".

## Ground rules (additions to the earlier rounds')

- **No raw query text in logs.** This covers every new endpoint, probe and test.
- **The browser never contacts a third party.** Thumbnails, descriptions, videos, tiles and
  translations all go through own endpoints with fixed upstream hosts and the SSRF checks of C3.
- **Impersonation is for looking like an ordinary browser, not for beating limits.** The owner
  was explicit: "not circumventing API rate limits, that's different." Keep the token buckets,
  breaker, `Retry-After` handling and per-host pacing exactly as they are. No proxy rotation,
  no retry-until-it-works loops.
- **Paced probes** are unchanged: at most 3 per hour per host, 60 s apart, each logged in
  `docs/probe-log.tsv`. Prefer a local listener over a third-party echo service.
- **Template context is 32 slots and `Search.c` says 31 are in use** (comment at
  `Search.c:1408`). Anything that needs a slot must free one. K4 frees one.
- **One logical change per commit.** Report `make verify` as a single summary line.
- Themes: the nine base tokens plus derived semantic tokens. No inline hex. The lint for
  undefined `var(--x)` stays on, and K2 extends it.

## Order of work

1. **K1 first, in two commits.** Fix the cookie separator and the form-blocking validation before
   anything else. Until then no toggle the owner clicks can be trusted, and every later item is
   judged through that page.
2. K8, K11 (error path only), K7 (placeholder filter only). These are small, certain and visible.
3. K2, K3, K9. CSS and a small script; no new providers.
4. K6 before K5. Reddit's lane depends on engines that return a zero-byte body today. Impersonation
   may fix that and change what K5 has to build.
5. K4 (tab merge), then the Videos tab with providers.
6. K10, then the full K11 card, then K7's description fetcher.

---

## K1. A working toggle for every provider

### What the owner sees

Clicking provider checkboxes and saving either changes nothing or changes only one. Opt-in
providers have one global checkbox and nothing behind it. Widget providers (MyMemory, Yahoo
chart, Nominatim, tiles, Wikipedia, the dictionary) cannot be turned off at all.

### K1.1 The `engines` cookie is joined with commas, and beaker drops it

`src/Routes/SettingsSave.c:171-183` builds the cookie value by joining `selected_ids` with
`','`. `src/Utility/Utility.c` (`get_user_engines`, the `strtok_r(copy, ",", …)` calls near
lines 450-515) splits it on commas again. Your own `docs/J-status.md` records that beaker
silently drops a cookie value that contains a comma, and the weights cookie was moved to `|` for
that reason (commit `3d1bcb0`, "a cookie rule I had to measure"). The `engines` cookie was not.

Result: one engine selected saves fine, two or more save nothing. The cookie never arrives, the
server falls back to defaults, and the page shows the checkboxes as they were. This is exactly
"the toggle does not work".

*Confirmed from the code and from your own commit message. I did not run it.*

**Fix.**
- Join with `|` and accept both `|` and `,` when reading, so cookies already in browsers keep
  working.
- Store the **disabled** set, not the enabled set (`off=bing|brave`). A provider added in a later
  release then defaults to on without the cookie having to know about it, and the cookie stays
  short. The `char cookie_value[512]` buffer at `SettingsSave.c:172` is a truncation hazard once
  there are 40 providers. Check for overflow and fail loudly instead of cutting the list.
- Keep the legacy-id mapping (`startpage→bing`, `mojeek→brave`) on the read path.

**Test that would have caught it** (`tests/test_k1_toggle_persist.sh`): save three engines
through `/save_settings`, keep the cookie jar, GET `/settings`, and assert the same three boxes
are `checked`. Then repeat with two engines. Today the second case fails.

### K1.2 One invalid number blocks the entire form

`templates/settings.html:279` declares weights as
`<input type="number" … min="0.5" max="5" step="0.5">`, and `settings-form` (line 73) holds the
weights, the engine checkboxes, the opt-in box and the selects in **one** form with no
`novalidate`. HTML constraint validation uses `min` as the step base, so the only valid values are
0.5, 1, 1.5 … 5. The registry's default weights in this build include 0.95, 1.15, 1.2, 1.3 and 0.8,
and the page pre-fills them. A browser refuses to submit a form containing a value that fails its
step, so **saving is blocked, toggles included**, with a small tooltip on the weights box that the
owner may never have seen.

*By HTML validation semantics. Not run in a browser.* **[verify in Chrome and Firefox]**

**Fix.** `step="any"`, or `step="0.05"` with `min="0.5"`. Add `novalidate` to the form and keep the
server-side clamp, which is the real validation. Add the test below (K2).

### K1.3 The list shows only what is server-enabled

`src/Routes/Settings.c` builds `enabled_engines` only for `ENGINE_REGISTRY[i].enabled`
(lines 71, 84, 186, 228, 269). A provider the server config disabled, a provider that is
opt-in, and a provider that is down simply do not appear. The owner cannot tell "off by me" from
"off by the server" from "not built". Weights and status have the same filter.

**Fix.** One table, one row per provider, **every** provider in the registry, grouped:

| Group | Examples | Control |
|---|---|---|
| Web | Bing, Brave, Yahoo, DuckDuckGo, Mojeek, Google bridge | checkbox |
| Images | the image engines | checkbox |
| Videos (K4) | each video engine | checkbox |
| Answers (widgets) | calculator (local, no network), translate (MyMemory), stocks (Yahoo chart), places (Nominatim + tiles), dictionary, currency, Wikipedia infobox | checkbox |
| Bridges and APIs | SearXNG bridge, Open Library, Reddit (K5) | checkbox |
| Opt-in | every provider registered with `opt_in` | checkbox **and** a short disclosure each |

Each row shows one of three states, in words and not only colour:
- **On** and reachable.
- **Off (by you)**: the checkbox is unchecked, the provider is never contacted.
- **Unavailable**: the server config turned it off or the build lacks it. The box is disabled
  and the reason is printed, for example "Disabled by the server administrator" or "Needs
  `searxng_url`".

Show the destination host next to each row ("sends your query to `api.mymemory.translated.net`").
That is what a per-provider toggle is for, and it is one string per registry entry.

### K1.4 A toggle must change what leaves the machine

A checkbox that only changes what is displayed is worse than none. "Off" has to hold in **every**
path that can reach the provider:

- the main fan-out (`Search.c`), including the Reddit lane (`Search.c:1555`) and the SearXNG
  bridge's own sub-engines (`searxng_engines`);
- the images route and, in K4, the videos route;
- the `/w/<kind>` endpoints and `answer_box_widget` / `answer_box_widget_forced`
  (`Search.c:1398-1400`). A user who turns off the translate provider must not have
  `translate …` still go to MyMemory because the widget checks only the server config;
- the infobox `handlers[]` (`Search.c:417-424`): Wikipedia, dictionary, currency;
- autocomplete, if it asks a third party.

Local-only features (calculator, units, colour) have nothing to switch off. Show them as "runs
locally, no network" and give them no checkbox, so the owner does not expect one to matter.

**Test** (`tests/test_k1_toggle_effect.sh`, using `tests/fake_upstream.py`): for each provider in
the registry, set it off through the cookie, run a query that would use it, and assert the fake
upstream's per-host request counter for that provider stays at zero. Run it as a loop over the
registry so a provider added later is covered without anyone remembering to add a case.

### K1.5 Opt-in: one toggle per provider

`docs/J5-opt-in.md` says the gate is plumbing only: `SearchEngine.opt_in`, `engine_count_opt_in()`,
`opt_in_providers_allowed()`, and no provider registered under it. The settings page
(`settings.html:207-236`) shows **one** global checkbox plus a four-paragraph disclaimer. Two
gates run in series: server `[engines] allow_files` and the `opt_in` cookie.

**Fix.**
- Keep the server gate as the outer one. It is the administrator's switch.
- Replace the single cookie with a per-provider list, `optin=<id>|<id>`, read and written exactly
  like K1.1.
- Render one row per opt-in provider with its own checkbox and its own disclosure (what it sends,
  to whom, what it risks). The global paragraph becomes the section intro.
- A provider must be registered for this to mean anything. K4 and K5 add the first ones.
  Candidates that belong here are the ones with a real downside: Google via CSE or the bridge,
  Reddit archive providers, YouTube innertube.
- If `allow_files` is off, show the rows disabled with "Disabled by the server administrator",
  not hidden. That is the "Unavailable" state in K1.3.

### K1.6 Acceptance

- Selecting any subset of providers, saving, reloading and reading the checkboxes round-trips.
  This holds for 1, 2 and 20 selected.
- A provider that is off receives zero requests on every route (K1.4).
- Saving with default weights succeeds in a real browser.
- Every provider in `ENGINE_REGISTRY` and every widget provider appears in `/settings` exactly
  once, in one of the three states.

---

## K2. The weights boxes: palette, layout, validation

### Cause

`static/main.css:949-952` gives `input, button, select, textarea` only `font-family` and
`font-variant-ligatures`. `.settings-select` (line 1543) and `input[type="color"]` (line 1662) are
styled; nothing styles `input[type="number"]`. Browsers draw an unstyled number input white with
dark text whatever the theme says, and draw its spinner in light colours.

The row is also laid out the wrong way round. `.settings-field` (line 1521) is a grid of
`minmax(0, 1fr) 240px`, and the template puts the `<input>` **first** and the label **second**
(`settings.html:278-281`). The input takes the wide column and the label lands in the narrow one at
the right edge, which is what the screenshot shows.

### Fix

1. One global rule, token-based, so every future input follows the theme too:
   ```css
   input, select, textarea {
     background: var(--surface-2);
     color: var(--text-primary);
     border: 1px solid var(--border);
     border-radius: 0;            /* match the cards */
     color-scheme: inherit;
   }
   input:focus-visible, select:focus-visible, textarea:focus-visible { outline: 2px solid var(--focus); }
   ```
2. Native widgets (spinner arrows, date pickers, scrollbars inside inputs) follow `color-scheme`.
   Set `color-scheme: light` or `dark` once per theme file in `static/themes/*.css`. Without it a
   dark theme gets a light spinner. Add `accent-color: var(--accent)` on `:root` so checkboxes and
   radios follow the palette.
3. Weights get their own compact layout: `.settings-weights` as a two-column list, `label` on the
   left (the engine name, readable), the input about 5 rem wide on the right. Do not stretch it.
   Fix the DOM order in the template or give this section its own grid.
4. `step`: see K1.2.
5. Extend the theme lint (`tools/check_themes.py`) so it fails if any `<input>`, `<select>`
   or `<textarea>` in `templates/` or in C-generated HTML has a computed background that is not a
   token. The easy version: grep the stylesheet and require the global rule above to exist, and
   reject `background: #fff`, `white` and `Canvas` anywhere in `static/`.

### Acceptance

For each of the themes: the weights boxes, the colour inputs and the selects use the same
background and border tokens as the cards around them. Screenshot each theme at 1280 and 390 px.
The spinner arrows are legible on dark themes.

---

## K3. "Resumes" must count down, and be real

### Cause

`src/Routes/Settings.c:262-311` computes `left` once from `engine_health_suspension(id, &left)`
and formats a string: `"in %dm %02ds"` (line 288), `"ready to retry"` (line 290) or `"-"` (line
286). `templates/settings.html:252-259` prints it as text. `static/settings.js` only syncs the
colour inputs. Nothing updates the number after the page loads, so it shows the value at render
time, forever. The header cells ("Engine", "State", "Why", "Resumes", "Answered", "Useful") are
also hard-coded English (K12).

### Fix

1. **Send an absolute time, not a duration.** `engine_health_suspension` already works from
   `open_until - time(NULL)` (`EngineHealth.c`). Emit `open_until` itself:
   `<time class="resume" data-resume-at="1762040000" datetime="…">`, and put the server's own
   "now" on the table (`data-now="…"`). The script computes
   `skew = data-now − Date.now()/1000` once, then each second shows
   `data-resume-at − (Date.now()/1000 + skew)`. This keeps the number correct on a client with a
   wrong clock.
2. **Tick once a second** in `static/settings.js`, with one `setInterval` for the whole table.
   Format `1h 05m`, `4m 07s`, `12s`. At zero show "retrying…" and then **ask the server**: a new
   `GET /settings/engine-status.json` returns the same rows (no query text, no cookies echoed) and
   the script re-renders State, Why and Resumes. Poll it every 10-15 s while any engine is
   suspended, and stop when none is.
3. **No JavaScript:** keep the server-rendered text as the fallback and add nothing that depends on
   script. A `<noscript>` line says the page does not update.
4. **Real means one source of truth.** The number must be read from the same field the dispatcher
   tests before it contacts the engine, and from nowhere else. Check that "ready to retry" means
   "the next request will actually be sent", and that "in 4m" means "no request before then". If
   the breaker and the `Retry-After` suspension keep separate clocks, show the **later** of the two.

### Test

`tests/test_k3_resume.sh`, with `fake_upstream.py` answering 429 with `Retry-After: 6`:
1. send a query, load `/settings`, parse `data-resume-at`, assert it is within ±2 s of
   `now + 6`;
2. assert **no** request reaches the fake upstream during the next 4 s;
3. assert one does after 7 s;
4. fetch `/settings/engine-status.json` and assert the state moved from suspended to ready.

Step 2 is the part that makes the number "real". It fails if the display and the gate disagree.

---

## K4. One tab bar, no chips, a Videos tab

### What exists

- `templates/_chrome.html:13-25`: tabs **All**, **Images**, **Settings** (a second Settings link is
  the gear at line 10; see K12).
- `templates/results.html`: a `{{if exists filter_links}}<nav class="filter-tabs">` strip built from
  one matrix in `Search.c:1296-1369` (categories News / Code / Academic) and appended again at
  `Search.c:2492-2501` (one chip per engine with its result count: `chip chip-eng`). The "All" chip
  is row 0 of that matrix. That is the duplicate the owner marked.
- CSS: `.filter-tabs` and `.chip*` around `main.css:2183-2250`.

### Fix

1. **Delete the strip.** Remove the `filter_links` matrix, both build sites, the `<nav
   class="filter-tabs">` block and its CSS. This **frees one context slot** (the comment at
   `Search.c:1408` says 31 of 32 are in use), which K5 and K10 can use.
2. **The tab bar is the only navigation.** `All · Images · Videos · Settings`. Order exactly that.
   Active state from `chrome_active`, as now.
3. Keep `?cat=` and `?engine=` working as URL parameters if tests or bookmarks use them, but give
   them no UI. If the owner later wants a per-engine breakdown, it belongs on the engine-status
   table in settings (the "Answered" column already counts it), not on every results page.
4. **Do not let "per-engine counts" vanish silently.** They were the only on-page evidence of which
   engines answered. Replace them with one line in the existing footer area of the results page
   ("Results from Bing, Brave, Yahoo") and, if you want detail, a closed `<details>` listing the
   counts. Closed by default, so it does not compete with the tab bar.

### The Videos tab

There is nothing under it today: no `/videos` route (`Main.c:324` registers `/images` only), no
video engine, no video result template, no thumbnail-with-duration card. It needs three things.

**Route and template.** Copy the shape of `src/Routes/Images.c` and `templates/images.html`:
`/videos?q=…`, same chrome, same paging, same provider toggles (K1). Result card: 16:9 thumbnail
through `/image_proxy` (never a direct third-party `<img src>`), duration badge, title, channel,
date, platform name. Clicking opens the watch page on the source site. **Do not embed players.** An
iframe makes the browser contact the platform, which breaks the ground rule.

**A result type.** `SearchResult` carries `thumbnail`, `date`, `author`, `source` and `kind` since
J5.2. A video also needs `duration_s` (int) and a platform label. Add them next to the others and
use the same first-writer-wins merge at `Search.c:1940-1952`.

**Providers.** Build them in this order. Items marked **[verify]** are from my memory of the
services and not from fetched pages. Probe each once under the paced-probe rule and record the
result in `docs/probe-log.tsv` before writing a parser.

| Provider | Route | Notes |
|---|---|---|
| Bing videos | HTML scrape of `bing.com/videos/search?q=` | Same family as the image scraper in `ImageScraping.c`, so it reuses pacing, breaker and UA. Gives YouTube, Vimeo and others in one place. **[verify]** markup |
| Brave videos | HTML scrape of `search.brave.com/videos?q=` | **[verify]** |
| Yahoo videos | `video.search.yahoo.com/search/video?p=` | **[verify]** |
| Dailymotion | public JSON API, `api.dailymotion.com/videos?search=…&fields=…` | No key needed for search **[verify]** |
| PeerTube via SepiaSearch | `sepiasearch.org/api/v1/search/videos?search=…` | JSON, federated, no key **[verify]** |
| Odysee / LBRY | `lighthouse.odysee.tv/search?s=…` | JSON **[verify]** |
| YouTube direct | innertube `youtubei/v1/search` | Fragile and the platform's terms apply. Make it **opt-in** (K1.5), last in the list, and only after the others work |
| Invidious / Piped | instances | Unreliable public instances. Skip unless the owner runs one |

Fix the order of work inside this item: route and card with **one** provider first (Dailymotion or
SepiaSearch, since they are JSON and need no markup parsing), then the HTML scrapers.

**Data-defined providers.** `static/engines.d/*.json` exists and only `openlibrary` uses it. The
two JSON providers above are exactly what it is for. Add them there instead of writing C, and
prove the loader handles a `videos` vertical.

### Acceptance

- One tab bar, no `<nav class="filter-tabs">`, no chip classes anywhere in `templates/` or CSS.
- `/videos?q=…` returns a grid of video cards with thumbnails, durations and source names, and
  turning every video provider off in settings gives the "no providers enabled" page, not an error.
- A test that counts context keys on `/search` and asserts the page now uses at most 30 of 32.

---

## K5. Reddit results

The owner's instruction is "I don't want a no for an answer." The honest position is that
**no single route is guaranteed**, because Reddit has been closing its own. So build several,
rank them, and prove with a live canary that at least one works. Here is the state as I found it.

### K5.1 What Reddit allows now, per the sources I read **[verify each against Reddit]**

- Unauthenticated `.json` on `www.reddit.com` was announced for shutdown on 2026-05-28 and returns
  403 (one source retested on 2026-07-11, another on 2026-10-27: three of three requests blocked;
  `old.reddit.com/*.json` answered with a 302).
- RSS still answered 200 on 2026-10-27, but one vendor reports Reddit ends RSS support on
  **2026-11-13**.
- The official OAuth Data API is described as free for non-commercial use at about 100 requests per
  minute, but **new requests are reported to stop on 2026-10-31** and approvals have been "rarely"
  granted since November 2025. Public Data API access is reported to end in March 2027.
- "Logged-in and authenticated access won't be impacted" is Reddit's quoted wording.

So: do not build on Reddit's JSON, do not build on Reddit's RSS, and treat the official API as
something the owner can use **only if they already hold a registered app**.

### K5.2 What the code does today

`Search.c:1548-1612` is the "F3 Reddit lane". It runs only when
`analysis.wants_discussion && page == 1 && engine_idx < ENGINE_COUNT && user_engine_count == 0`
(`Search.c:1555`), asks DuckDuckGo Lite for `site:reddit.com …` and falls back through
`{"ddg","yahoo","bing","brave"}`. `tools/queries-reddit.tsv` and the comment at lines 1557-1561
record the spike: only DDG honoured the operator; Yahoo returned a **zero-byte body**; Bing and
Brave returned nothing. Results are capped at 4 per domain (`Search.c:2168-2202`) and re-weighted
by `reddit_relevance` (`Ranker.c:425`, `W_REDDIT 0.22`).

Four problems:

1. **Saving any settings turns the lane off.** `user_engine_count == 0` means "the user has chosen
   no engines." The moment the owner clicks a checkbox and saves, the cookie is non-empty and the
   lane never runs again. This is probably why the owner sees none.
2. **One engine wide.** DuckDuckGo Lite is the most rate-limited engine in the set (202 and anomaly
   pages). When it is paused there is no Reddit.
3. **A zero-byte body is a block, not "no results."** Yahoo returning an empty body for a query
   with an operator looks like the same fingerprint problem as K6. Do not record it as "Yahoo
   cannot do site:".
4. **Only `wants_discussion` queries get it.** A query that says "reddit" or "r/rust" explicitly,
   or uses `site:reddit.com`, should always get it.

### K5.3 Build a Reddit provider with a ladder of routes

A provider `reddit` (K1: its own toggle, destination hosts listed) that tries these in order,
stops at the first that returns results, and records which one answered. Every route normalises to
the same fields: title, URL, subreddit, score, comment count, age, snippet.

| Rung | Route | Why | Caveat |
|---|---|---|---|
| 1 | `site:reddit.com` through **every** web engine that honours it, not only DDG | Needs no Reddit access at all. It is the part that works whatever Reddit does | Depends on K6 for Yahoo and Bing |
| 2 | Query-variant ladder: `"<terms>" reddit`, `<terms> site:reddit.com/r/<sub>`, `<terms> inurl:comments` | Operators are handled differently by each engine | Keep each variant to one request. This is variety, not retry |
| 3 | Archive search: **Arctic Shift** and **PullPush** (public APIs over a Reddit archive) | Does not touch Reddit. Gives threads and comments by text search | Data lags and coverage is partial. Both are volunteer services. **[verify]** the exact endpoints and that they answer today; one search result tied Arctic Shift to a March 2025 project post |
| 4 | Google via the existing CSE-element or SearXNG bridge, restricted to reddit.com | `google_cse_element` is already built (`config.ini:53`) and the bridge exists | Off by default. Opt-in (K1.5) |
| 5 | Reddit's OAuth API, **only** when `config.ini` has a client id and secret | For an owner who already holds one | Read-only. Do not build a registration flow, the programme is closing |
| 6 | Reddit RSS (`search.rss`) | Still answered on 2026-10-27 | **Ends 2026-11-13 by one report.** Add it only if it costs under 20 lines, and tag it expiring |
| — | Redlib instances | Mirrors Reddit with a clean UI | They themselves depend on Reddit access and have been failing. **[verify]** before building. I would skip |

Rungs 1-3 are the ones that need no credentials and do not depend on Reddit's goodwill. Build them
first. Rungs 4-6 are opt-in or config-gated.

### K5.4 When the lane runs

- Remove `user_engine_count == 0` from the condition. Honour the **provider toggle** instead (K1.4).
- Run it for: `wants_discussion`, any query containing the word "reddit", a `r/<sub>` token, and a
  `site:reddit.com` operator the user typed. For the operator, **pass it through unchanged** to the
  engines and do not strip it.
- Page 1 only is fine. A "more discussions" link is not needed now.
- Show subreddit, score and comment count in the result row's meta line when the provider supplied
  them (the J5.2 result fields already hold `source`, `author`, `date`).

### K5.5 Prove it, and keep proving it

- `tests/test_k5_reddit.sh` against `fake_upstream.py`: each rung, one at a time, answers a query
  and the page shows Reddit rows, with the other rungs off.
- A **canary**, not a test that depends on Reddit: a small paced job (once per 6 hours, inside the
  3-per-hour probe rule) that runs one fixed discussion query through the whole ladder, writes the
  winning rung or "none" into the engine-status table (K3) and `docs/probe-log.tsv`. When it says
  "none", the owner finds out from the settings page, not from a bad search.
- Re-run `tools/queries-reddit.tsv` after K6 and record the new per-engine counts. If Yahoo goes from
  0 bytes to results, K5.3 rung 1 is most of the answer.

Do not write "no" in the report. If every rung fails on the day, report which rungs failed, the
status codes and the body sizes, and what the canary shows.

---

## K6. Impersonation: it is not active

### Verification

I searched the code, `Makefile` and docs. Findings:

- `src/Scraping/ScrapingHttp.c` `configure_curl_handle` sets the User-Agent, `CURL_HTTP_VERSION_1_1`,
  `CURLOPT_ACCEPT_ENCODING ""`, the cookie engine and proxy options. `build_request_headers` adds
  `Sec-Fetch-*` headers only when `browser_headers` is set. `USER_AGENTS[]` and
  `UA_LADDER {"safari","firefox","chrome"}` choose the string.
- `Makefile:26`: `LIBS := -lbeaker -lcurl …`. Stock libcurl. `Dockerfile` installs `curl-dev` from
  the distribution.
- `grep -ri impersonat` over `src/`, `Makefile` and `docs/` finds it only in review documents
  (`docs/claude-feedback.md`, `docs/G1-engines.md:104`, which says curl-impersonate "is not installed
  here … the TLS-fingerprint dimension was not tested").

So the engines see: an OpenSSL ClientHello with libcurl's cipher and extension order, **HTTP/1.1**
(Chrome and Safari speak HTTP/2 to these sites), libcurl's header order, and a User-Agent that
claims to be Chrome, Firefox or Safari. A User-Agent that disagrees with the TLS handshake is itself
a signal. `G1` found that a UA change alone flipped Brave from a challenge to results. That shows
the UA matters; it does not show the fingerprint does not. Yahoo's zero-byte bodies (K5.2) and the
202 pages are the symptoms to re-test.

### Implement it

**Library.** Use `libcurl-impersonate` from the **lexiforest/curl-impersonate** fork. Per its README
it is MIT-licensed, ships prebuilt binaries and Docker images, builds as a drop-in libcurl, and
supports many named targets: Chrome (through `chrome152` at the time of the README), Safari
(through `safari270`, including iOS), Firefox (through `firefox155`) and Edge. It lists
curl-impersonate 2.3.0 on curl 8.23.0, and its own table is marked "needs to be updated", so read
`browsers.json` for the real list. **[verify]** the C entry point: I believe it is
`curl_easy_impersonate(CURL *, const char *target, int default_headers)` as in the earlier lwthiker
fork. The README did not show it.

**Wiring.**
1. Link against it instead of stock libcurl. Change `Makefile:26` and the `Dockerfile` (build or
   download a release; pin the version and the checksum). Keep the stock-libcurl build working
   behind a flag, so the project builds on a machine without it, and have the binary report which
   one it was built with.
2. `impersonate = none | chrome | firefox | safari` in `[engines]`, plus an optional per-engine
   override in the registry. Default `chrome`. Map each value to one pinned target (for example
   `chrome152`) in one table.
3. In `configure_curl_handle`, call `curl_easy_impersonate` **instead of** setting the UA and the
   header set by hand when impersonation is on. The library sets the matching UA, `Accept`,
   `Accept-Language`, `sec-ch-ua*`, `Sec-Fetch-*` and header order, and negotiates HTTP/2. Do not
   layer your own headers on top. Remove the forced `CURL_HTTP_VERSION_1_1` for impersonated
   requests.
4. The **UA ladder must follow the profile**. `UA_LADDER {"safari","firefox","chrome"}` picks a UA
   string without changing the handshake. With impersonation, a ladder step means "switch target",
   not "switch string". Otherwise you send a Safari UA with a Chrome handshake.
5. **Cookies and sessions.** Keep the cookie engine on. Keep one handle per engine per profile so a
   session cookie from the first request is sent on the second.
6. Nothing else about the request path changes: same pacing, same breaker, same `Retry-After`. See
   the ground rule.

### Make it visible

- Print one line at startup: `impersonation: libcurl-impersonate 2.x, default target chrome152` or
  `impersonation: off (stock libcurl)`.
- Show the same in `/settings` (engine status header) and `/metrics`, per engine: the target in
  use. The owner asked whether it is active. The page should answer.

### Verify it works, without a third party

Use a local listener so no probe leaves the machine:

1. `tests/tls_hello_echo.py`: a small TLS server that accepts a handshake, logs the raw ClientHello,
   computes **JA3** and **JA4** and the ALPN list, and answers 200. A second listener with HTTP/2
   logs the SETTINGS frame values and the order of pseudo-headers (`:method :authority :scheme
   :path` for Chrome).
2. `tests/test_k6_impersonation.sh`: start the listener with a locally generated cert (the test
   config trusts it), run the project's own fetch path once per target, and assert the JA4, ALPN
   (`h2,http/1.1`) and pseudo-header order match values **recorded once from a real browser** and
   committed under `tests/fixtures/tls/`. Also assert that with `impersonate = none` the JA4 is
   libcurl's and not Chrome's, so the test cannot pass by accident.
3. One **paced** probe to a public fingerprint echo service (for example `tls.peet.ws/api/all`
   **[verify it is up]**) as a manual cross-check, logged in `docs/probe-log.tsv`. Do not make the
   test suite depend on it.
4. Re-run the engine matrix from `docs/G1-engines.md` with impersonation off and on, and record
   status code and body size per engine. This is the evidence for whether it mattered.

### Acceptance

The binary says which library it is built with and which target each engine uses. The local
listener test passes for Chrome, Firefox and Safari targets and fails for `none`. The G1 matrix
shows, per engine, what changed. If it changed nothing for an engine, say so.

---

## K7. The "We would like to show you a description here…" line

### Where it comes from

It is **not in your code**. I searched `src/`, `templates/`, `locales/` and `static/` and found no
such string. It is in the upstream pages: `tests/fixtures/yahoo/good.html` contains it, truncated as
"We would like to show you a description here but the site wo..." in Yahoo's sitelink
sub-descriptions. It is the text a search engine shows when a site's `robots.txt` stops that engine
from reading the page. Bing and Brave have their own versions.

Your pipeline passes it through:

- The parsers (`ScrapingParsers.c`) take the snippet node's text as it comes.
- `clean_snippet` (`Search.c:297`) removes whitespace and entities and the date prefix
  (`skip_date_prefix`, `Search.c:180`). Nothing recognises boilerplate.
- The merge at `Search.c:1956` keeps **the longer** snippet when two engines return the same URL:
  `if (strlen(cleaned) > strlen(cands[existing].snippet))`. A placeholder is long. It will replace a
  shorter, real description from another engine.
- The row is printed as it is.

*Confirmed from the code and the fixture. I did not check which parsers promote the sitelink text
to a main snippet.* **[verify in the Yahoo parser]**

### Fix, in two steps

**Step 1: stop showing it** (small, do first).

- Add `snippet_is_placeholder(const char *)`: case-insensitive match on a short table of known
  strings, and on the **truncated** form (a prefix of the phrase followed by `...`). Start with
  "We would like to show you a description here but the site won't allow us", "A description for
  this result is not available because of this site's robots.txt", "No information is available
  for this page", and the other languages the engines return for the configured locales.
  **[verify]** the exact strings per engine by grepping the fixtures and the replay corpus
  (`tests/replay`), not from memory. Store the table in one file.
- Apply it in `clean_snippet`'s caller: a placeholder becomes the **empty** string.
- Change the merge rule to: a non-empty real snippet always beats an empty one, then the longer of
  two real ones wins. Add a test with two engines returning the same URL, one placeholder and one
  real, in both orders.

**Step 2: make the description real.** A page that blocks the search engine's crawler has still got
a description, in its own `<meta name="description">`, `og:description` or first paragraph. Fetch
it ourselves, but narrowly:

- **Only for rows with an empty snippet** after step 1, and only on the first results page.
- **Progressively.** Render the page without it. A small script (the same pattern as
  `static/widgets.js`) asks an own endpoint, `GET /d?u=<result id>`, for each blank row, after the
  page has loaded, and fills the line in. The browser never contacts the site. The endpoint takes a
  **result id** from the page, not a free URL, so it is not an open fetcher. Keep the SSRF checks
  from C3 and re-check on every redirect hop.
- **Budget and limits.** 600 ms per fetch, at most 5 per page, 3 redirects, read at most 64 KB,
  `text/html` only, never follow to a private address. Per-host pacing like any other upstream.
- **Respect the site.** Fetch `robots.txt` for our own user agent and obey it. A page that disallows
  *us* gets no description fetched. Honour `<meta name="robots" content="nosnippet">` and
  `max-snippet:0`. If the site has said no, show the **URL path as a breadcrumb** (`kernel.org ›
  pub › linux`), which is a true description of what the link is and what other engines show.
- **Cache** by canonical URL with a long TTL in the existing cache, including a negative entry, so a
  popular blocked site is fetched once.
- **Extraction order:** `og:description`, `meta name=description`, then the first paragraph over 60
  characters in the main text. Decode entities, collapse whitespace, cut at 200 characters on a
  word boundary.
- **Per-site shortcuts** for the cases that matter: Wikipedia and Wiktionary through their summary
  APIs (also the right fix for K12's note on infobox duplication), GitHub repositories through the
  `description` of the page's meta tag, YouTube through `og:description`. These are the sites that
  most often show the placeholder.
- **Toggle.** A provider row in K1 ("Page descriptions: fetches the first KB of a result page when
  a search engine gives no description"), on by default, because it contacts result sites. Say so
  in its disclosure.

### Acceptance

- No result row anywhere shows the placeholder or its truncated form (`grep` the rendered page in
  the replay corpus).
- The two-engine merge test passes in both orders.
- A blocked-by-robots result shows a real description within the budget, or the breadcrumb, never an
  empty line and never the apology text.
- Log lines for `/d` contain the host and the status, not the full URL's query string.

---

## K8. `1+1` shows the answer twice

### Cause

The widget layer was added in J4 and the old rail handlers were left in. `Search.c:417-424`:

```
{"dictionary", …}, {"arithmetic", is_calculator_query, fetch_calc_wrapper, NULL},
{"units", …}, {"currency", …}, {"colour", …}, {"wikipedia", always_true, …}
```

`1+1` matches `arithmetic`, so the rail builds a "Calculation" card, **and** the widget path
(`Search.c:1396-1420`) renders the full calculator. `show_answer_strip` is switched off when a
widget renders (`Search.c:1406`), so the strip is not the third copy in this build, but nothing
stops the rail card. On a narrow window J1 puts the rail above the results, so the owner sees "a
simple box with the result" first and "the full calculator" second.

*Confirmed from the code. I did not render the page.*

### Fix

- A handler whose kind has a widget must be skipped **when the widget renders**. Do it as a rule,
  not a one-off: the widget layer reports `widget.kind`, and the handler loop skips any handler
  whose name equals it. `arithmetic` is the case today. Map explicitly (widget `calculator` ↔
  handler `arithmetic`; later, `currency` if it becomes a widget).
- Better still, **delete** the `arithmetic` handler and `fetch_calc_wrapper`. They have nothing left
  to do once the widget exists, and a dead handler is how this happened. If the widget is turned
  off (K1) the calculator still has to answer, so check what `answers_off` does and keep one path
  for that case, not two.
- Wikipedia's card for the article "1+1" (what the owner saw in Round 4) is a separate, correct
  question: for a **widget-answered** query the Wikipedia handler should not run unless the query
  contains something beyond the expression. That is the J3 rule. Confirm it still holds with a test.

### Test

`tests/test_k8_single_answer.sh`: for each widget query (`1+1`, `2^10`, `12*(3+4)`,
`translate hello to spanish`, `nvidia stock`, a place query), fetch `/search?q=…` and assert, from the
HTML: exactly one element with class `widget`, **zero** rail panels carrying the same `kind`, zero
`answer-box`, and the result value present in the page exactly once outside the keypad's `name="key"`
buttons. Run it in the replay harness with the network fixtures, so it is deterministic.

---

## K9. The calculator card: fill the space or shrink the card

### Cause

`static/main.css:2048-2053` makes `.widget` a block-level card: full width of the main column. The
keypad is `grid-template-columns: repeat(4, minmax(0, 1fr)); max-width: 320px` (line 2075-2080). On a
900 px main column the card is 900 px wide and the keypad sits in its left 320 px. The rest is empty.
That is the marked square.

### Fix: one of two, and I recommend the first

1. **Match Google: a compact card that fits its content.**
   - `.widget-calculator { width: fit-content; max-width: 100%; min-width: min(100%, 18rem); }`
   - Keypad `width: 100%`, no `max-width`, so the card is exactly as wide as the keypad plus padding.
   - The expression line and the result line go **inside** the same width, right-aligned like a
     display.
2. **Use the space.** Put the display and a short history on the left and the keypad on the right,
   two columns from the `min-width: 40rem` breakpoint, one column below it. More work, and it only
   pays off if you add history.

Do (1) now.

### Make the keypad match Google's

The current keypad (`widget_calculator.html`) has 26 keys: `AC ( ) ln / log abs √ xʸ / 7 8 9 ÷ / 4 5 6 × / 1 2 3 − / 0 . e + / π =`. Google's has, beyond that: `Rad`/`Deg`, `Inv`, `sin cos tan`, `x!`, `%`, `EXP`, `Ans` and a backspace. Add `⌫` and `%` and `Ans` first, since they are the ones people reach for, then the trig set behind an `Inv`/`Rad` toggle. Check each against `src/Infobox/Calc*.c`; a key the evaluator cannot parse is worse than a missing key. Make "no `=`, `=` is a key" consistent with how the 62-case table in `J4a` is tested.

A keypad where every press is a full page load (`widget_calculator.html` submits a GET form with
`name="key"`) is the no-JS fallback. Check that `static/widgets.js` intercepts it. The key press
should append to the expression in the page and call `/w/calculator?q=…` (the `/w` endpoint exists,
`Widget.c:44-76`), not reload. Google's calculator updates on each key.

### Acceptance

At widths 1280, 1024, 768 and 390: the card's right edge is within 16 px of the keypad's right edge
(Playwright `getBoundingClientRect`), and the keypad has no horizontal scroll. Screenshots in the
report.

---

## K10. A Google-style translate box

### What exists

`src/Infobox/Translate.c`: `translate_parse` needs a verb (translate / traduce / traducir) and takes
the **last** ` to|al|into <known language>`; `translate_widget` calls MyMemory
(`api.mymemory.translated.net/get?q=%.900s&langpair=%.5s|%.5s`, 900 ms budget) with the source
language defaulting to `en`. `templates/widget_translate.html` is a title, the from-text, the to-text
and one input with a button. `docs/J4b-translate.md` lists as not done: source detection, language
selector, caching. `docs/J4c-J4d.md` records MyMemory's polluted entry (`hello` → `testvalue`), Lingva
returning text untranslated and LibreTranslate's public instances answering 502.

### The shape to build

Google's card is two panes: on the left a source-language menu ("English – detected") over a text
area, on the right a target-language menu over the translation, a swap button between them, and the
translation updates as you type. That is easy to do **without** a new provider if you do four things.

**1. The markup** (`widget_translate.html`), no JS needed to be usable:
```
[ source ▾ ]  ⇄  [ target ▾ ]
[ textarea     ]  [ output   ]
```
Two `<select>`s fed by the existing `LANGUAGES[]` table (name → code), a swap link that is just the
same page with `from` and `to` exchanged, a `<textarea>` and a button. It is a GET form to
`/search?q=translate <text> to <language>` so it works with scripts off. Everything uses tokens.

**2. The parser takes what Google takes.** Add: `<text> in <language>` (the owner's own Google
example, `furthermore in spanish`), `how do you say <text> in <language>`, `<text> to <language>`
when the tail is a known language, `<language> to <language>` alone (an empty box, pre-set), and the
bare word `translate` (an empty box, English to the browser's language). Keep the verb requirement
off for `in <language>` only when the text is short (say up to 6 words) and contains no operator or
digit, or `1 in binary` and `meaning in tamil` will start translating. Add tests for the false
positives.

**3. Live update.** A debounce of 400 ms on the textarea calls
`/w/translate?q=<text>&from=<code>&to=<code>` (extend the endpoint to accept `from` and `to`,
validated against the table, never free text) and writes the result into the output pane. Reuse the
`refresh()` pattern in `static/widgets.js`.

**4. A provider ladder with detection.** One provider is the weak point:

| Rung | Route | Notes |
|---|---|---|
| 1 | MyMemory, `langpair=Autodetect|<to>` | Detects the source **[verify the `Autodetect` token]**. 500-byte limit per `q`, so split on sentence boundaries |
| 2 | An unofficial Google endpoint, `translate.googleapis.com/translate_a/single?client=gtx&sl=auto&tl=…&dt=t&q=…` | Returns the detected source language. No key. Unofficial, so treat it like the stock endpoint: flagged as such, **opt-in** (K1.5) and last **[verify it still answers]** |
| 3 | A LibreTranslate instance the owner configures (`libretranslate_url`) | Public instances were failing in J4d. The owner's own is reliable |

For every rung:
- **Reject untranslated output** (translation equals source after trimming) and move to the next
  rung. That is the Lingva symptom.
- **Reject MyMemory's low-quality matches.** The `testvalue` case is one entry from a user memory.
  The response lists `matches[]` with `quality` and `match`; take `responseData.translatedText`
  only if its match score is high, otherwise try the next rung **[verify field names]**.
- **Reject quota text.** MyMemory returns a warning string in place of a translation when the free
  daily quota is spent **[verify exact text]**; a status-200 reply carrying that text must count as
  a failure, not be shown as the translation.
- **Cache** by (text hash, from, to) with a 24-hour TTL. Never log the text.
- Show the detected language in the source menu ("English – detected") and put a small "via
  MyMemory" line under the card, as Google does with its own name.

### Acceptance

`furthermore in spanish`, `translate hello to french`, `bonjour in english` and a bare `translate`
each render the two-pane card. Changing either menu, or pressing swap, re-translates. With JS
disabled the form still works. With MyMemory forced to return the quota text, rung 2 or 3 answers or
the card says the service is unavailable **once**. A translation equal to its source is never shown.

---

## K11. `nvidia stock`

### Cause

Three defects stack.

1. **The first word becomes the ticker.** `src/Infobox/Stocks.c` `stocks_parse` (lines 69-123): when
   no other word looks like a ticker, `candidate = words[0]` (lines 113-116) if it is "not itself
   the keyword." `nvidia` passes (letters, under 10 characters), is upper-cased to `NVIDIA` and sent to
   `…/v8/finance/chart/NVIDIA` (line 24). NVIDIA's symbol is `NVDA`. Yahoo answers with
   `chart.error.description = "No data found, symbol may be delisted"`.
2. **The error is shown twice.** Lines 153-161 set only `out->note` from Yahoo's text and return with
   `ok = 0`. `Search.c:1411-1412` sets `widget_result = widget.ok && widget.result[0] ? widget.result :
   widget.note`, and `widget_note` is also set. `templates/widget_stock.html` prints `widget_note` in
   the head (`.widget-source`) and `widget_result` in `.widget-error`. Same string, two places. That
   is the owner's "No data found… / No data found…".
3. **The disclaimer prints on failure.** The last line of the template (`stock_disclaimer`) is outside
   the `{{if widget_ok}}`. "Delayed quote from an unofficial endpoint. Not investment advice." belongs
   to a quote, not to an error.

*Confirmed from the code. I did not run it.*

### Fix, in this order

1. **Never show a dead card.** If the symbol does not resolve, return "no widget" and let the normal
   results (the NVIDIA investor page, Yahoo Finance, news) take the top. An apology card for an
   unknown company is worse than none. Show an error card **only** when the user clearly typed a
   ticker (`$NVDA`, an all-caps token, or `NVDA stock`) and the provider failed, and then print the
   message **once**.
2. **Resolve names to symbols.** Before calling the chart endpoint:
   - a small **bundled table** of company names and aliases to symbols (a few hundred of the most
     searched, generated from a public listing such as the SEC's `company_tickers.json`
     **[verify the URL and its licence]**, committed as a data file with its date). This answers
     `nvidia`, `apple`, `tesla`, `microsoft` with no network and no leak;
   - then Yahoo's symbol search, `query1.finance.yahoo.com/v1/finance/search?q=<name>&quotesCount=5&newsCount=0`
     **[verify the endpoint, parameters and response fields]**, restricted to `quoteType == EQUITY`
     (plus `ETF`, `INDEX`, `CRYPTOCURRENCY` when the words say so), cached for 7 days by name;
   - if two candidates are close (Alphabet's two classes, a company with an ADR), pick the primary
     listing and print the exchange.
3. **A second guard on the ticker rule.** A bare lower-case first word is a **name** until proven a
   ticker. Only treat it as a symbol if it is all caps in the original query, has a `.` or `-` suffix,
   starts with `$` or `^`, or the name lookup found nothing and the chart endpoint then confirms it.
4. **Print the disclaimer only with a quote.**
5. **Present the quote properly.** The current result is `"%.4f %s"` (line 171), so it prints
   `180.0000 USD`. Use 2 decimals (4 for sub-dollar instruments), thousands separators and the
   locale's decimal mark; show the **name** ("NVIDIA Corporation") and `NASDAQ: NVDA` under it; show the
   change in absolute and percent, coloured with two new **derived tokens** `--up` and `--down` that
   pass contrast on every theme, and also marked with ▲ / ▼ so colour is not the only signal; show
   market state and time ("Closed · 30 Oct, 16:00 EDT").
6. **The chart.** The endpoint already returns the series. Render a server-side inline SVG sparkline
   from `indicators.quote[0].close` with range links `1D 5D 1M 6M YTD 1Y 5Y` (each a link carrying
   `range=…`, or a `/w/stock` fetch). No third-party script and no image request. Use the colour tokens
   and a `<title>` for accessibility. Details such as open, high, low, volume, 52-week range and
   previous close come from the same `meta` object where present **[verify field names:
   `regularMarketDayHigh`, `regularMarketDayLow`, `regularMarketVolume`, `fiftyTwoWeekHigh`,
   `fiftyTwoWeekLow`, `longName`, `exchangeName`]**.

### Tests

- `stocks_parse`: `nvidia stock` and `nvidia share price` resolve to `NVDA` through the table with no
  HTTP call; `NVDA stock`, `$NVDA`, `nvda` and `brk-b stock` still resolve; `news today`,
  `market watch` and `stock photos` resolve to nothing. (The earlier `MARKET NEWS` fix is in the
  comment at lines 66-68. Keep it.)
- With the chart endpoint returning the 404 JSON from `tests/fixtures`, `nvidia stock` renders **no**
  stock card and the error string appears **zero** times. With a ticker the user typed in capitals,
  the string appears **once**.
- The disclaimer appears only when a price is present.

---

## K12. Found along the way

1. **Two Settings links in the nav.** `templates/_chrome.html:10` has a gear icon linking to
   `/settings` and line 20 has a "Settings" tab to the same place. The owner asked for one tab bar;
   decide which stays (the tab, I suggest, and keep the gear as the icon inside it, or drop the gear)
   and remove the other.
2. **Hard-coded English in the engine-status header** (`settings.html:252`: "Engine / State / Why /
   Resumes / Answered / Useful") and in the strings from `Settings.c:286-290` ("in %dm %02ds", "ready
   to retry"). Move them to `locales/`. K3 touches both.
3. **`char cookie_value[512]`** in `SettingsSave.c:172` truncates silently once enough providers are
   on. K1.1 covers the cookie; keep the overflow check.
4. **The Reddit lane is off for anyone who has saved settings** (`Search.c:1555`, `user_engine_count ==
   0`). This is the likeliest reason the owner sees no Reddit. K5.4.
5. **`Search.c:1956` prefers the longer snippet**, so any boilerplate beats a short real snippet. K7.
6. **Zero-byte bodies are being read as "no results."** Yahoo's reply to `site:reddit.com` (comment at
   `Search.c:1557`) is the example. `BlockDetection.c` should classify an empty 200 as a block signal
   for any engine that normally returns content, record it, and feed the breaker and the status page.
   Add a fixture and a test.
7. **The stock price format** `%.4f` prints `180.0000 USD`. K11.
8. **The Yahoo fixture shows sitelink sub-results** (`alternative-links`) with their own descriptions.
   Check the parser neither promotes them to the main snippet nor drops the main snippet when they
   come first. **[verify]** `tests/fixtures/yahoo/good.html` against the current parser output.
9. **`widget_calculator.html` submits a form per key.** If `widgets.js` does not intercept it, every
   key press reloads the page and re-runs the web search behind the card. K9.
10. **`omnisearch.err`, `omnisearch.err.old` and `omnisearch.log` are in the zip.** `.gitignore`
    excludes `*.log` and `*.err`, so they are not tracked, but they travel with the archive. They
    contain access lines with request paths. Before sharing a build, exclude them and review them
    for query text (the ground rule).
11. **`docs/J-status.md` overstates.** It lists the J items as done. K1, K8 and K11 are J items the
    owner still sees broken. Add a **"verified how"** column to that table: unit test, replay test,
    or browser run. A row with none of those is "implemented, unverified".
12. **No end-to-end check that a toggle reaches the wire.** Every settings test I found checks the
    cookie or the page, not the outbound request. K1.4's registry-wide test is the missing one.

---

## Report back

Reply with:

1. **Status per item K1–K11**: done / partial / not done, with the commit for each. Say "how verified"
   per row: unit test, replay test, local TLS listener, or real browser.
2. **K1.** The exact cookie string before and after saving three engines. The result of
   `test_k1_toggle_persist.sh` and `test_k1_toggle_effect.sh`. The number of providers listed in
   `/settings` against the number in the registry.
3. **K3.** The `data-resume-at` value for a forced 429 and the time the engine was next contacted.
4. **K5.** The result of re-running `tools/queries-reddit.tsv`: per engine, status, body size and
   Reddit hits, with impersonation off and on. Which rungs of the ladder produced results, and what
   the canary shows. If a rung failed, the status codes and body sizes.
5. **K6.** The startup line, the JA3/JA4 of the local-listener test per target, and the G1 matrix
   before and after. If impersonation made no difference for an engine, say so.
6. **K7.** The placeholder table, the grep result over the replay corpus, and a screenshot of a
   robots-blocked result with a fetched description.
7. **K4, K9.** Screenshots at 1280 / 1024 / 390 px for the tab bar and the calculator, in two themes.
8. **A one-line `make verify` summary**, and the list of things you could not do or could not verify,
   each tagged.
9. **Anything you changed that this file did not ask for**, and why.

## Sources

Read 2026-11-01. Most are vendor pages. Check each claim against the provider before relying on it.

- Crawlora, "Why Reddit Blocked Unauthenticated JSON in 2026": https://crawlora.net/blog/reddit-json-api-blocked-2026
- BigIdeasDB, "Reddit API Shutdown 2027: Every Deadline, What Still Works" (a vendor; its own tests are one IP, one day): https://bigideasdb.com/reddit-api-shutdown-2027
- GitHub issue, `www.reddit.com/search.json` returning 403 and `api.reddit.com/search` as a fallback: https://github.com/public-clis/rdt-cli/issues/27
- lexiforest/curl-impersonate (README: MIT, 2.3.0 on curl 8.23.0, target list): https://github.com/lexiforest/curl-impersonate
- Search results consulted but not opened: Arctic Shift and PullPush archive status pages, Reddit data API pricing pages (search listing only; **[verify]**).
