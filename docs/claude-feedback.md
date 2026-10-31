# claude-feedback — review of sp-engine / omnisearch

> Prompt for a coding agent (Claude Code or similar).
> Based on a read-through of `docs/SETUP.md` and the source in `omnisearch.zip`
> (git `3344fcb`, branch `sapir-whorf`), written 2026-10-30. **Round 2 (Part 4)** was
> added 2026-10-31 after reading `omnisearch_1.zip` (git `3f98753`) and SearXNG's
> current source (master, 2026-10-30). **Round 3 (Part 5)** reviews
> `omnisearch_e38410.zip` (git `f9828f7`, same day).
> I read the code; I did not build or run it. Items tagged **[verify]** could not
> be confirmed from source alone — confirm them before acting.

## Your job

You are picking up a self-hosted metasearch engine in C (~10k lines, beaker +
libcurl + libxml2 + jansson, no database, plain Makefile). `docs/SETUP.md` already
lists the sharp edges honestly. This document adds what the source review found
on top of it: bugs that SETUP.md does not mention, places where the docs and the
code disagree, and my view on what to do, what to avoid, and what to build next.

Part 0 is three problems the owner reported while using the engine. They outrank
everything except the P0 fixes. Work through the rest in the order given under
**Order of work**. Ignore anything that conflicts with a direct instruction from
the owner.

## Ground rules

- Keep it C11 with the current dependency set. No JS framework, no build system,
  no new runtime dependency unless an item says so and you explain why.
- Do not change ranking weights, thresholds or engine weights until the offline
  benchmark (A3) exists. Bug fixes that change ranking as a side effect (C10, C11,
  C13) are fine, but run the benchmark before and after once it exists.
- One logical change per commit. Run `make`, `make check`, `make smoke` before each.
- Don't hammer upstream engines while testing. `make eval` fans out 60 queries
  × up to 6 engines against live sources; use fixtures (A1) for anything you can.
- Do not start logging raw query text. The current `ACCESS` lines record paths
  only — that is a good property, keep it.
- For every item: confirm the finding in code first, then fix, then say what you
  ran to show it is fixed. If a finding is wrong, say so and move on.

## Order of work

1. **P0 — correctness and exposure:** C1, C2, C3, C4, C5. (F1 and F2 depend on C1.)
2. **Safety net:** A1 (fixtures + registry lint), then A3 (offline benchmark with
   replay). Add the F1 funnel instrumentation at the same time.
3. **Owner-reported:** F1, F2, F3 (Part 0). F3 starts with its measurement spike.
4. **P1:** C6–C13, C15, C19, A2, A4.
5. **P2:** C14, C16–C18, A5–A9.

**Round 3 (Part 5) supersedes everything below for new work: H1 and H3 first (they
are about whether the engine has more than one real source), then H2, then H4.**

**Round 2 (Part 4) supersedes this order for new work:** G1 (diagnose the three
failing engines) first, because it is cheap and everything else is judged against
it; then G2 (rate-limit policy), G3 (Google), G4 (answer box). Round-1 items that
are already committed are not to be redone — run `make verify` and move on.

---

## Part 0 — Reported by the owner (F)

### F1. Page 1 sometimes shows only a handful of results; pages 2–4 are full

*How I read the report:* on some searches the first page renders very few results
(about two), while the pages after it are normal. If you read it differently, say so.

**What the code does (confirmed in `Search.c` and `Scraping.c`):**
- The number of results on a page is whatever survives: engines that answered
  before the early exit → URL and title-Jaccard dedupe → coverage floor
  (`< 0.15` dropped) → per-domain cap (3). There is no page size, no minimum and
  no top-up.
- The early exit counts **raw** results: `total_so_far += results_count`, and an
  engine counts once it returned ≥ 1 result. Two engines with four results each
  satisfy "2 engines, ≥ 8 results" and end the batch at 1.2 s, even if dedupe,
  floor and cap leave three results. Engines cut off this way are
  `CANCELLED`: no warning, nothing cached, nothing shown. The user cannot tell why
  the page is thin.
- Only page 2+ has a fullness safeguard (the "sink rather than delete" comment:
  "an empty page 2 is worse than a repeat"). Page 1 has no equivalent.
- The coverage floor and IDF are computed from the candidate set (SETUP #5), so a
  thin set makes the floor noisier and drops more.
- Page 1 is also the cold request. Cookie jars are deleted after 429/202/5xx/
  blocked/zero-result responses, so the next search starts without one; DNS and TLS
  are rebuilt on every search (a new `curl_multi` and new easy handles each time,
  700 ms connect timeout); and the six infobox threads run only on page 1. Pages
  2–4 inherit the warm state page 1 left. **[verify]** — this is a hypothesis.

**Step 1 — diagnose before changing anything.** Add a per-stage funnel to
`OMNI_PERF` output and to a loopback-only `&debug=1`: per engine status, HTTP code,
ms, cached/stale/cancelled, raw count; then totals after dedupe, after floor, after
cap, and rendered. Run 30 queries cold (cache dir emptied, cookie jars deleted) and
warm. Report which stage loses the results, and how often, *before* writing a fix.

**Step 2 — fix (works whichever stage turns out to be the cause):**
1. **Exit on projected yield, not raw count.** Projected yield = Σ over domains of
   `min(MAX_PER_DOMAIN, distinct canonical URLs)`, counted across ≥ 2 independent
   sources (see F2). Take the fast exit (500–550 ms) only when projected yield ≥ a
   full page (10). Otherwise keep waiting for unfinished engines up to a soft cap
   (~3 s). Keep the hard cap.
2. **Top-up after the pipeline.** If fewer than MIN (6) results render: relax in
   order — per-domain cap 3 → 5, coverage floor 0.15 → 0.08 (never to zero), treat
   the page like page 2 (sunk results allowed back). If it is still under MIN, mark
   the page as partial.
3. **Say it out loud.** When engines were cut off or failed and the page is under
   target, show one line naming them, with a "wait for all sources" link
   (`&patient=1`: no early exit, ~6 s cap). Move the hardcoded English into locale
   files (A5).
4. **Keep connection state warm.** Use a shared `CURLSH` (DNS, TLS sessions,
   connection cache) or one long-lived multi handle, so page 1 stops paying
   handshake cost inside a 700 ms connect window.
5. **Decide with data:** whether to paginate over a stable fused list (fetch two
   SERP pages per engine once, cache the fused ranking for 10 minutes, slice by
   ten). It makes pages consistent and page 1 fuller, but doubles upstream requests
   per search — which is the scarce resource (C19). Only do it if steps 1–4 are not
   enough.

**Done when:** on 50 cold queries ≥ 95 % of page-1 renders have ≥ 8 results or show
the partial note naming the reason, p95 latency stays ≤ 3 s, and a replay test
(A3) reproduces a thin page and asserts the fix.

### F2. The aggregated engines "zone out" and I get no good results

Principle: one unhealthy or junk engine must not set the quality of the page, and
the page must never go blank silently.

1. **Count independent sources, not engines.** Add `index_group` to the registry.
   Yahoo's web results are largely Bing's, and DuckDuckGo's are partly Bing's
   **[verify]**; Brave has its own index. Right now Bing + Yahoo agreeing counts as
   consensus and as "two engines" for the early exit, although it is one index with
   one failure mode. Count groups in consensus and in the exit rule, and pick the
   six slots to maximise group diversity among healthy engines.
2. **Measure quality per request.** `good_yield` = rendered results with
   coverage ≥ 0.5. Feed it into A2 (per-engine quality EWMA) as well as health.
3. **Recover inside the request, bounded to one extra round (≤ ~1.5 s).** If
   `good_yield` < MIN after the batch: (a) hedge to backup engines that were not
   in the first round; (b) retry the engines that answered empty or junk with a
   query variant — raw instead of `engine_core`, or `engine_core` instead of raw,
   quotes and operators dropped; (c) fall back to stale results for the same
   query (C1) with a visible banner.
4. **Fail loudly and usefully.** If nothing good remains, show a per-engine outcome
   list (answered / cut off / blocked / junk) and a "retry with all sources" link
   instead of a blank page. Keep the nonsense-query floor, but show
   relaxed-floor results flagged as low confidence rather than nothing.
5. **Remove the self-inflicted causes first:** C1 (stale counted as healthy), C7
   (100–200 ms sleep), C8 (breaker trips on local network blips), C9 (false
   "blocked" pages), C12 (early exit with no web engine).
6. **Add independent sources, cooperatively (N3):** (a) the SearXNG bridge already
   in the code is the practical route to Google's index — document it and make it
   the recommended optional upstream; (b) evaluate the Brave Search API (own
   index; check its current free tier and terms) instead of scraping Brave's HTML;
   (c) evaluate Mojeek and Marginalia APIs. Treat the `google` CSE engine as
   dormant or delete it (N8).

**Done when:** replay tests (A3) with one injected failure per engine — empty, junk,
blocked, slow — still return ≥ MIN good results in ≥ 90 % of the benchmark queries,
and no query renders an empty page without an outcome list.

### F3. Find a way to index Reddit

**Goal:** Reddit threads should show up for the questions people take to Reddit
(recommendations, "X vs Y", troubleshooting, "is it worth it"), not only when an
upstream engine happens to include them. Today the `+0.22` boost reweights Reddit
results that already arrived and does nothing otherwise.

**Constraints (as of 2026-10-30; from news reports of Reddit's 2026-09-30
announcement — verify at source):** new public Data API access requests close on
2026-10-31; RSS feeds end 2026-11-13; unregistered apps lose API access from
2027-01-12; the remaining public API closes in March 2027; logged-out `.json`
requests reportedly return 403; Old Reddit is becoming login-only; Reddit is
suing scrapers. So **do not** build on the Reddit API, RSS or `.json`, do not
scrape reddit.com or old.reddit.com directly, and do not work around blocks. If
the owner wants official API access as a short-term supplement they would have to
request it by tomorrow; that is their call, and nothing here should depend on it.
Reddit also limits crawlers other than its partners **[verify]**, so some
engines may hold thin or stale Reddit coverage. Measure; don't assume.

**Plan — in this order:**
1. **Spike (half a day).** Write `tools/queries-reddit.tsv` with ~40 Reddit-shaped
   queries. For every web engine in the registry, run `site:reddit.com <q>` and the
   plain `<q>`. Record per engine: Reddit threads in the top 10, freshness, snippet
   quality, dead links. Report the table. Choose the lane's engines from this data
   and store the result as a registry field (or learn it as an EWMA).
2. **A Reddit lane.** New intent `QI_DISCUSSION`, triggered by the opinion words the
   engine currently strips and discards (best, cheap, top, vs, worth, recommend,
   review, experience), by a bare `reddit` word, or by `r/<sub>`. In that case, in
   the same parallel batch (so no extra latency), also send
   `site:reddit.com <engine_core>` to the 1–2 best lane engines; `reddit` or
   `r/<sub>` restricts to `site:reddit.com[/r/<sub>]`. Merge the results and
   reserve ≥ 2 slots (raise the per-domain cap for reddit.com to 4) for lane
   results that pass coverage. A failing lane must not add a user-visible warning
   beyond the normal engine handling.
3. **Canonicalise Reddit URLs.** Collapse `old.`, `np.`, `new.`, `i.`, `m.` and `www.`
   hosts and reduce `/r/<sub>/comments/<id>/<slug>/<comment>` to the thread id.
   `rank_canonical_url` strips only `www.`, `m.` and `amp.` today, so the same
   thread can appear twice.
4. **A local thread index — the form of "indexing" that is still possible.** Record
   every `reddit.com/r/*/comments/<id>` hit that any search returns (URL, title,
   snippet, subreddit, first/last seen, hit count) in a flat file (TSV or JSONL; no
   database), load it into memory at startup, and expose it as a local source
   `reddit-local` queried with the existing BM25 and coverage code. Rules: store
   search-result metadata only (no post bodies or comments); no query text and no
   client key; 90-day TTL and a size cap; drop an entry when the thread 404s or is
   removed; a `reddit_index = on|off` setting; local to the machine; `/info` shows
   a count at most. Do not import third-party Reddit dumps.
5. **UI and ranking.** Show subreddit and age on Reddit results and add a "Reddit"
   filter chip. Replace the one-domain special case with a data-driven "discussion
   tier" file (reddit, Stack Exchange, Hacker News, …) loaded like `authority.tsv`.

**Done when:** on `tools/queries-reddit.tsv`, the share of queries with ≥ 2 Reddit
threads in the top 10 is measured before and after (agree the target with the owner
once the baseline is known), p95 page latency is unchanged (±100 ms), and the
canonicaliser merges all Reddit host variants in a test.

---

## Part 1 — Things I would not do (N)

**N1. Don't add embeddings or a semantic layer yet.** SETUP #9 calls it an
architectural change; I'd go further: without a judged set you cannot tell whether
it helps. Build the benchmark first (A3). It may turn out the fusion math is fine
and the problem is engine quality (A2).

**N2. Don't add more hand-tuned signals or config knobs for weights.** There are
already ten weighted terms and no way to tell if any of them helps. Reddit gets
`+0.22`, the second-largest positive weight, for a single hardcoded domain. Don't
add a twelfth signal, and don't make weights configurable. Tune offline (A3), then
keep the result as compile-time constants.

**N3. Don't escalate anti-bot evasion to keep scraped engines alive.** *(Amended in
Round 2, see G1: the owner asked to follow what SearXNG does. Fingerprint hygiene is
fine; solving challenges is not.)* No UA
rotation, TLS impersonation, or proxy rotation as the fix for 429s and 202s. The
project is already moving the right way with API sources (OpenAlex, Crossref,
arXiv, StackExchange, Google CSE, SearXNG). Extend that; treat the scraped SERPs
as best-effort. Keep the breaker cooperative.

**N4. Don't "clean up" the discovered upstream quirks without a test.** Bing
without locale params, Yahoo with a cookie jar, forced HTTP/1.1, the stripped
`first=1` — they look odd and they are load-bearing. Move them into data (C15,
A1) and pin them with fixtures; don't delete them.

**N5. Don't expose this beyond a trusted LAN, and don't hand-roll auth.** If it
ever needs to be reachable from outside, put a reverse proxy in front. The fixes
below (C3, C4, C6, C19) are what makes LAN exposure acceptable, not what makes
internet exposure acceptable.

**N6. Don't refactor `Search.c` before there are tests** (C18), don't add a
server-side search history (the Reddit thread index in F3 stores result metadata only,
never queries), and don't translate all 14 locales' intent tables
(A5). Start with the locales that are actually used.

**N7. Don't leave dead knobs and dead branches around.** `[ranking]` in
`config.ini` is parsed by nothing; `mojeek`, `startpage` and `ecosia` are named in
code and comments but are not in the registry. Delete or implement; don't document
them as if they work.

**N8. Don't build on Reddit's API, RSS or `.json`, on scraping reddit.com directly,
or on Google Custom Search.** Reddit announced on 2026-09-30 that new public API
requests close on 2026-10-31, RSS ends 2026-11-13 and the remaining public API
closes in March 2027. Google's Custom Search JSON API has been closed to new
customers since 2026-01-20 and is discontinued on 2027-01-01. Both are gone within
months. Details and the alternative are in F3, F2 and G3 (SearXNG's "Google CSE" is
a different, undocumented route — see G3 before deciding anything about Google). (I read secondary reports of
the Reddit announcement and Google's notices — **[verify]** against the primary
sources.)

**N9. Don't fix thin pages by raising the hard cap or removing the early exit.**
That trades a thin page for a slow one. Fix what the exit measures (F1).

---

## Part 2 — Things I would change (C)

### P0

**C1. Stale cache hits are recorded as successes.**
`Scraping.c`, `process_response()`: when a fetch fails, `load_stale_results()`
sets `job->status = SCRAPE_STATUS_OK` and then `engine_health_record()` is called
with that status. A blocked or erroring engine that happens to have a body in the
7-day stale cache is logged as healthy: the breaker closes, `trips` resets,
`total_ok++`, and the cookie jar survives a `BLOCKED`. The breaker can never open
for an engine that has ever worked.
*Fix:* record health from the live fetch result, then substitute stale results;
keep `job->stale` and have the template say the results are stale (it already has
a "answered from cache" note).
*Done when:* a test forces a `BLOCKED` response with a stale entry present and
`/info` shows the error counter rising and the breaker opening at the threshold.

**C2. Startup cache cleanup deletes the caches that are meant to outlive it, and nothing prunes at runtime.**
`Main.c` calls `cache_cleanup(cfg.cache_ttl_search)`, which removes every
`*.cache` older than the *search* TTL (1 h). That also wipes the stale
last-known-good entries (7 d), infobox (24 h) and image (7 d) caches on every
restart — so the stale fallback does not survive a restart. At runtime, expired
files are only removed when the same key is read again, so the directory grows
without bound.
Related: `cache_set()` writes in place (`fopen "wb"`), so a concurrent reader can
get a truncated body (for the image proxy, a truncated image).
*Fix:* tag entries by class (e.g. `s.`, `stale.`, `info.`, `img.` in the filename
or a sidecar) and sweep each with its own TTL; sweep periodically, not just at
boot; enforce a size budget; write to a temp file and `rename()`.
*Done when:* restart keeps stale/image entries within their TTL; a soak of
10k distinct queries stays under the configured size.

**C3. Image proxy SSRF: redirects and port handling bypass the private-address check.**
`ImageProxy.c`: only the *initial* host is validated, then curl runs with
`FOLLOWLOCATION=1` and no `MAXREDIRS`/`REDIR_PROTOCOLS`. A public URL that 302s to
`127.0.0.1`, `169.254.169.254` or a LAN host is fetched. The `CURLOPT_RESOLVE` pin
is built as `host:80|443:ip` from the scheme only, so a URL with an explicit port
(`http://example.com:8080/`) is not pinned at all. Other gaps:
- IPv4 only (`AF_INET`); no `0.0.0.0/8`, `100.64.0.0/10`, `198.18.0.0/15`,
  multicast or reserved ranges, and no IPv6 rules.
- The host is parsed by hand (`strstr("://")`, `strchr('/')`, `strchr(':')`) while
  the scheme comes from `CURLU`. That is a parser differential (userinfo, `[::1]`,
  backslashes). Take host and port from the same `CURLU` that is used for the
  fetch.
*Fix (preferred):* validate at connect time with `CURLOPT_OPENSOCKETFUNCTION` (or
`SOCKOPTFUNCTION`) so the *actual* peer address is checked on every hop, including
redirects and rebinding. Also set `CURLOPT_PROTOCOLS`/`REDIR_PROTOCOLS` to
HTTP(S), `MAXREDIRS` ≤ 3, `MAXFILESIZE`, and restrict ports to 80/443.
*Done when:* tests (against a local listener) show redirect-to-loopback,
explicit-port, userinfo and IPv6-literal URLs are all refused.

**C4. The proxy serves attacker-controlled content from the app's origin, to anyone who can reach it.**
- Any `image/*` is accepted, including `image/svg+xml`. `/proxy?url=https://x/y.svg`
  runs attacker script on the app origin; none of the cookies are `HttpOnly`.
  On a cache hit the content type is guessed from the URL extension, not stored.
- There is no `Content-Security-Policy`, `X-Content-Type-Options` or
  `Referrer-Policy` anywhere in the app (grep finds none).
- With `0.0.0.0` bind and the rate limiter off, `/proxy` is an open fetch relay for
  the LAN.
*Fix:* refuse SVG (or serve with `Content-Security-Policy: sandbox; default-src 'none'`),
store the content type next to the cached body, send `nosniff`; add a global
CSP (`default-src 'self'; img-src 'self' data:; frame-ancestors 'none'`) and
`Referrer-Policy: no-referrer`; and **sign the proxy URLs**: when rendering
results, append an HMAC of the URL using a per-process secret, and refuse
unsigned or mismatched requests. That turns the proxy from "fetch anything" into
"fetch only what a result page contained".

**C5. Response splitting and unvalidated cookies in settings.**
`SettingsSave.c`: `snprintf(redirect_url, ..., "/settings?q=%s", query)` puts the
decoded query straight into `Location` — URL-encode it, and verify what beaker does
with CR/LF in header values **[verify]**. `theme` and `locale` are written to
cookies without checking them against the known theme/locale lists (title font,
scale and engines are checked). Cookies have no `SameSite`, and the state change
happens over GET, so any page can change someone's settings.
*Fix:* validate every value against its allowlist before `set_cookie`; encode the
redirect; `SameSite=Lax`; accept POST only for `/save_settings` if the form can
be changed to POST without breaking `smoke.sh`.

### P1

**C6. `X-Forwarded-For` is trusted unconditionally.**
`RateLimit.c` takes the first XFF value as the client key even when there is no
proxy. A client can dodge the limiter by varying the header, and because the key is
part of the request-cache key (`build_search_request_cache_key`) and the
seen-URL key, it also multiplies cache files. Add `trusted_proxies` config; use the
socket peer unless the peer is in that list.

**C7. Every search sleeps 100–200 ms for no reason.**
`Scraping.c` calls `http_delay()` (`usleep` 100–200 ms) after the handles are
set up and before the first `curl_multi_perform`, even when every job was served
from cache. The log in the zip shows `/search` at roughly 520–630 ms, so this is a
quarter or more of the page latency, and the jitter does nothing when all engines
start together. Remove it. (`rand()` is also not thread-safe; see C17.)

**C8. The breaker can't tell "my network blinked" from "the engine is blocking me", and eval can poison it.**
Connect timeout is 700 ms and any curl error is `FETCH_ERROR`, which counts as a
failure. A Wi-Fi blip or a wake from sleep can trip every engine, and the
exponential backoff (up to 6 h) is persisted. The snapshot in the zip
(`cache/engine_health.tsv`) shows `ddg` open with 7 consecutive failures, 0 OK and
3 trips, and `brave` at 0 OK / 3 errors. **[verify]** whether that is real
breakage or the result of running smoke/eval against live engines.
*Fix:* classify failures (DNS/connect/timeout vs HTTP block vs parse); do not
count transport failures against the breaker when most engines in the same batch
failed the same way; give `make eval` a pacing option and a way to run without
tripping persistent breakers (e.g. `OMNI_NOBREAKER=1`). Surface "never succeeded"
in `/info`.

**C9. The blocked-page detector can reject good pages.**
`response_is_blocked_page()` substring-searches the whole body for `Just a
moment...`, `>Robot<`, `<title>Captcha</title>` and similar *before* parsing. A
real results page whose snippets contain that text is classed `BLOCKED`, dropped,
counted against the breaker, and its cookie jar is deleted. Run it only when the
parser found zero results, or scope the markers to `<title>`/challenge DOM per
engine and keep the markers in the registry. Drop the `Startpage`/`Ecosia`
strings.

**C10. Canonical URL lowercases the whole path.**
`rank_canonical_url()` lowercases every character up to `?`. Paths are
case-sensitive: `youtu.be/<id>`, Wikipedia titles after the first character, paste
and short-link IDs all collide, and the dedupe silently drops one of them. Lowercase
the host only.

**C11. Registrable domain is a 14-entry hand list.**
`rank_domain_of()` knows `co.uk`, `com.au`, … but not `org.uk`, `gov.uk`,
`com.ar`, `com.pl`, or hosted-subdomain suffixes like `github.io`, `blogspot.com`,
`pages.dev`. That skews the per-domain cap (3) and the authority lookup — every
`*.github.io` site counts as one domain. Load a trimmed public-suffix list from
`static/` the way `authority.tsv` is loaded (preferred, no new dependency), or use
libpsl as an optional dependency.

**C12. The early-exit "trusted" test isn't vertical-aware, and engine weight is not used for ranking.**
`weight >= 1.1` counts Wikipedia (1.2), StackExchange (1.2) and OpenAlex (1.3) as
trusted. For a code, academic or entity query, one fast API engine returning ≥ 6
results at 550 ms can end the batch before any web engine answers, and the web
engines are cancelled. Meanwhile fusion uses `1/(k+rank)` with no engine weight,
so the float's only real jobs are routing and this heuristic. Replace the magic
number with explicit registry flags (e.g. `anchor_for_early_exit`), and require at
least one `V_WEB` result before exiting unless the user chose a single engine.
See F1 and F2 for the yield-based replacement. Also: engines that always take > 2 s
are cancelled every time, never cached, and never counted. Record cancellations per engine and demote engines that are
habitually cut.

**C13. Ranking inputs have three mismatches.**
- Freshness is a constant: `W_FRESHNESS * 0.5` for every document. SETUP §8
  describes "date fragments in snippet". Implement it (`skip_date_prefix` in
  `Search.c` already parses those fragments and then discards them) or remove the
  weight and the documentation.
- `df` is computed over title+snippet, but `tf` and coverage also look at URL
  tokens. A term present only in the URL gets `df == 0` and is dropped from
  coverage. Use the same fields everywhere.
- Coverage (`-0.65·(1-coverage)`) sees only the snippet the engine returned, and
  snippet length differs by engine. Results from engines with short snippets are
  penalised for the engine's truncation, not the page. Treat a term missing from a
  *truncated* snippet (ends with `…`/`...`) as unknown rather than absent.
- Title-Jaccard dedupe (`rank_dedupe`) merges different pages across domains when
  titles are ≥ 80 % similar. "How to install X on Ubuntu 22.04" from two different
  sites is not a duplicate. Require snippet similarity too, or limit it to known
  syndication hosts.

**C15. Config and docs drift.**
- `config.ini` documents `[health] threshold`; the parser key is
  `breaker_threshold`. Unknown keys and unparsable integers are ignored silently
  (`set_option`, `parse_integer`'s return value is unused), so a typo does
  nothing and says nothing. Log `config.ini:LINE unknown key`, reject bad values,
  and range-check (ports, TTLs, thresholds). Add `--config PATH` / env override
  and `--check-config`.
- `[ranking]` (see N7), the engine list comment (`startpage`, `mojeek`), and
  `engine_wants_no_cookies("mojeek")` refer to things that don't exist.
- SETUP §7 says `research_requests=0`; the key is `search_requests`.
- Docker: `Dockerfile` exposes 5000, compose maps `8087:8087`, and the binary
  loads `config.ini` from the working directory (`/app`) while compose mounts
  `example-config.ini` at `/etc/omnisearch/config.ini`. The mounted config is never
  read, so the container listens on the default port. **[verify]** by running it.
  Pin the beaker clone to a commit or tag instead of HEAD.
- Move the string-keyed per-engine special cases (`response_looks_like_results_page`
  matches on `engine->name`, others on `engine->id`) into registry fields: cookie
  policy, locale style, results marker, blocked markers.

**C19. Safer defaults.**
`host` defaults to `0.0.0.0` in `Config.h` and in the shipped config, rate limits
default to off, and `/info` is public. The one scarce resource is this host's
egress reputation: one noisy LAN client gets every engine blocked and then
breaker-banned for hours. Default to `127.0.0.1`, make LAN exposure an explicit
setting, default the search limiter to something modest, and keep `/info`
loopback-only. `GIT_REMOTE` is baked into the binary and served from `/info`;
strip userinfo at build time or leave it out.

### P2

**C14. Cookie jar races.** Concurrent searches share `cookies/<engine>.txt` with
`COOKIEFILE`+`COOKIEJAR` (curl rewrites the file at handle cleanup), and a failure
path `remove()`s it under other in-flight requests. Use a `CURLSH` with
`CURL_LOCK_DATA_COOKIE` and a mutex, persisting occasionally. Only Yahoo
really needs it.

**C16. Secrets in logs.** `Main.c` prints `proxy_url` verbatim, which may include
credentials; with `OMNI_PERF=1`, `[URL]` lines print full request URLs, which for
Google CSE include `key=`. Redact userinfo and `key=`.

**C17. Thread-safety and UB nits.** `rand()`/`srand()` are used from request
threads (`http_delay`, backoff jitter) — use `rand_r` or `arc4random`.
`Calculator.c` has a file-level `static char logic_log[4096]` shared by all
threads. `1 << bit` for `bit == 31` in the consensus loop is UB. `strcpy`,
`strcat` and `sprintf` remain in `ImageProxy.c` and `SettingsSave.c`; they are
bounded today by construction — make them bounded by code.

**C18. `Search.c` is 1,542 lines with a ~790-line `results_handler`.** After A1
exists, split it into stages (analyse → select engines → scrape → fuse →
paginate → infobox → render) around a `SearchContext` so each stage can be
fixture-tested. Not a rewrite.

---

## Part 3 — Things I would add (A)

**A1. Fixture-based parser tests and a registry lint (do this first).**
Use `OMNI_DUMP=<engine>` to save real SERPs into `tests/fixtures/<engine>/`, plus a
captcha/challenge page and an "empty results" page per engine. Add a small C test
binary that links `ScrapingParsers.c` and prints results as JSON; assert result
count, no empty titles/URLs, expected first host, and that the blocked detector
fires on the challenge fixture and *not* on the good ones (C9). Extend
`make check` with a registry lint: unique ids, non-empty `verticals`, sane
weight/timeouts, `build_url` or `base_url` present, explicit `supports_site`.
Parser drift then fails the build instead of reaching a user.

**A2. A per-engine quality signal ("healthy but junk").**
The data already exists after fusion: `doc->coverage` and `engine_mask`. For each
engine and query, compute the share of its top-5 results that (a) pass the
coverage floor and (b) are also returned by at least one other engine. Keep an
EWMA in `EngineHealthEntry`, expose it in `/info`, log a line when an engine's
results are mostly below the floor, and *demote* (reduce slots, don't open the
breaker) when the EWMA stays low. This targets the Bing-returns-hotel-brands case
directly, and it is the detection half of F2.

**A3. An offline relevance benchmark with a replay mode.**
- Record each engine's parsed results per query into `tests/replay/*.json`.
- `make eval-offline` re-ranks from those files with no network, deterministic.
- Hand-judge top-10 for ~100 queries on a 0/1/2 scale in `tools/judgments.tsv`
  (a day of work; start from `tools/queries.tsv`, which has 59 queries and only
  an "expected host" hint on some). Report MRR@10, nDCG@10 and hit@3 per category,
  with a `--compare <git-ref>` option.
- Only then add `tools/tune.py` (coordinate descent over the `W_*` constants) and
  commit the result as constants (N2).
Without this, every ranking edit is a guess.

**A4. Structured logs, a metrics endpoint, and log rotation that actually runs.**
One `key=value` line per request: request id, per-engine status/time/count, total
time, **no query text** (a hash at most). A loopback-only `/metrics` (Prometheus
text) with per-engine p50/p95 latency, status counts, cancellations and the A2
score — `total_latency_ms` alone hides the tail. `rotate-logs.sh` is not
scheduled; `omnisearch.err` was already ~400 KB after a day in the zip. Add a
launchd/systemd timer, or size-based rotation in the binary.

**A5. Data-driven language tables.**
Intent keywords, stopwords and opinion fillers are English tables in C
(`QueryAnalysis.c`). Move them to `static/lang/<lang>.tsv` (fillers, stopwords,
code/academic/news terms) loaded like `authority.tsv`, so a language is a data
file, not a patch. Do two locales first, the ones that are really used. Move the
hardcoded English strings in `results.html` ("No results found…", "Some
sources…") into the locale files at the same time.

**A6. Say where queries go, and let the user switch it off.**
Every query goes to up to 6 engines, *and* to the Wikipedia API (the infobox
`always_true` handler fires for every query), plus Cambridge Dictionary
(dictionary handler), exchange-rate/crypto APIs when a currency query matches, and
Google/SearXNG when configured. SETUP does not list this. Document it, and add
`infobox = all | local | off` (local = calculator, units, colour only, nothing
leaves the box), exposed in `/settings`.

**A7. A sanitizer build and one `make verify`.**
`make asan` (`-fsanitize=address,undefined -Wshadow -Wformat=2`), then
`make verify` = build + `check` + fixtures + registry lint + smoke (optionally
under ASAN). Run it before every commit. It is the substitute for the CI the
project doesn't have.

**A8. A clean handoff bundle.**
`omnisearch.zip` contained `obj/`, the compiled `bin/omnisearch`, logs, the live
`cache/engine_health.tsv`, `.DS_Store`, `__MACOSX`, and a `config.ini` with a home
path. Add `make dist` (or use `git archive`) that ships source, docs, tools and
`example-config.ini` only, and add `.DS_Store` to `.gitignore`.

**A9. A backup remote for the fork.**
`origin` is the upstream repository, and the sapir-whorf/ui work exists only as
local branches. Add a private remote, or at least a periodic `git bundle`.
Decide the product name too — SETUP #16 lists five of them (`sw`, `sapir-whorf`,
`omnisearch`, `_sapir-whorf` version suffix, `sp-engine`).

---

## Part 4 — Round 2: owner reports after the fixes (G)

*Based on `omnisearch_1.zip` (git `3f98753`, 26 commits after `3344fcb`), a read of
SearXNG's current source (read to understand behaviour; no code is to be copied —
see the licence note in G3), and web research on 2026-10-31. Not run live. Everything
tagged **[verify]** needs a check on the owner's machine.*

**The owner's report.** The results page now says: *"Some sources did not answer.
Brave is rate-limiting queries right now and will be retried automatically (HTTP
429). DuckDuckGo Lite is rate-limiting queries right now and will be retried
automatically. Bing returned search results in a format OmniSearch could not
parse."* The owner also wants (a) a Google-style answer box at the top for
conversions, maths, translation, stocks and similar, (b) a study of how other
aggregators handle rate limiting, (c) a Google CSE engine modelled on SearXNG's, and
(d) SearXNG's per-engine settings used as the starting point.

### What the new tree shows

- Round 1 landed as commits (C1–C19, A1–A8, F1–F3). Don't redo them. Run
  `make verify` first and report anything red.
- **DuckDuckGo Lite and Brave have never succeeded on this machine.** The persisted
  health file in the first zip showed ddg 0/7 and brave 0/3 OK; in this zip ddg is
  0/5 and brave 0/5, both breaker-open, while bing (8/8) and yahoo (7/7) worked.
  That is not load: two engines refuse this request shape every time. (Bing may well
  have worked until recently; the owner now reports a parse failure.)
- `docs/SETUP.md` is deleted in the working tree (`git status`: ` D docs/SETUP.md`;
  `docs/` is empty) but `config.ini` still says "see docs/SETUP.md". Restore it
  (updated) or fix the comment.
- `config.ini` is stale again: the engine list comment still names `startpage` and
  `mojeek`, and the Google block still advertises the JSON API as "free 100
  queries/day" (closed to new customers, discontinued 2027-01-01 — N8).
- The old log has 20+ `WARN context_set_array_of_arrays: TemplateContext is full.
  Cannot add array of arrays key 'results'` (also `'pagination_links'`) lines. The
  template context has a fixed capacity, and when it is full the **results** key is
  dropped silently. The new log has none, but the answer box (G4) adds context keys.
  Add a worst-case render test (infobox + answer card + engine warnings + stale
  banner + pagination + results) that fails on any `TemplateContext is full`.
- The old log also has 99 × `noname.xml:1: parser error : Start tag expected, '<' not
  found`: libxml2 is being handed a non-XML body (JSON, an empty body or an error
  page) by something. Silence libxml2's generic error handler *and* find the caller;
  it may be the same bodies that become "could not parse".

### G1. Why Brave, DuckDuckGo Lite and Bing fail — diagnose with an A/B matrix, then fix

**What SearXNG does now (read from its source; this is the biggest difference):**
- Its HTTP client is `curl_cffi` with **browser impersonation** (TLS/HTTP2
  fingerprint and default headers) on every engine: Chrome by default, Firefox for
  DuckDuckGo, a Chrome-Android profile for Google. It uses HTTP/2 by default.
  OmniSearch uses plain libcurl, forced to HTTP/1.1, with its own header set.
- **DuckDuckGo:** the engine enabled by default POSTs to
  `https://html.duckduckgo.com/html/` (not `lite.`), with `Sec-Fetch-*` navigation
  headers, `Referer`, `Accept-Language`, cookies `kl`/`df`/`ad`/`ah`/`l`, and a
  **static User-Agent**, because the `vqd` token is derived from the query and the
  UA. Its own comments say requests with curl-like headers are filtered. A second
  engine (`duckduckgo web`, disabled by default) uses the JS API
  (`duckduckgo.com/?q=…` → `deep_preload_link` → `links.duckduckgo.com/d.js?o=json…`)
  and contains code that solves DDG's JS challenge. **Do not implement challenge
  solving.** If DDG challenges, treat it as blocked (G2).
- **Brave:** it reads the SvelteKit data payload embedded in the page (`data: [...]`,
  including IIFE-wrapped objects), not HTML selectors, and sends cookies
  `safesearch`, `useLocation=0`, `summarizer=0`, `country`, `ui_lang`. OmniSearch's
  `parse_brave` looks for `div.snippet[data-type=web]`. Even without the 429 it may
  find nothing. **[verify]** with a dump.
- **Bing:** SearXNG's request is minimal — `q`, `adlt`, `setlang`, `cc` (omitting
  `cc` for us/cn/ru) — and it parses `ol#b_results > li.b_algo` with `h2/a`, decoding
  `www.bing.com/ck/a?…&u=a1<base64url>`. OmniSearch already decodes `ck/a` and has
  an XPath fallback. Its "could not parse" status means the body contained
  `b_algo`/`b_results` **but the parser found zero items**. That looks like a soft
  block (a results shell with no organic items) or a markup change, not a missing
  page. Test it; don't guess.

**Task 1 — probe, don't guess.** Add `tools/probe.sh <engine> <query>`: one request,
no retries, no cache, no breaker, dumping status, headers and body (`OMNI_DUMP`).
Run this matrix per engine on the owner's machine and record it in `docs/`:

| Dimension | Values |
|---|---|
| HTTP version | 1.1, 2 |
| TLS/HTTP fingerprint | default libcurl; libcurl-impersonate (chrome, firefox) **[verify availability on macOS]** |
| Headers | current set; browser-like set incl. `Sec-Fetch-*`, `Accept-Language` |
| Cookies | none; engine-specific (Brave and DDG lists above) |
| Endpoint | DDG `lite.` vs `html.`; Brave HTML vs payload |

Pace it: one request per cell, a minute apart, so the probe does not become the
cause. Report which dimension flips 429 → 200. Do not draw conclusions from one
request.

**Task 2 — Bing triage table (from the dump):**

| In the body | Meaning | Action |
|---|---|---|
| `b_algo` items exist, parser returns 0 | markup drift | fix the XPath, add a fixture |
| `b_results` but no `b_algo`, "no results" text (`b_no`) | Bing says nothing matched | status `EMPTY`, not `PARSE_MISMATCH`; check query shaping |
| consent wall, "unusual traffic", Turnstile/captcha | block | status `BLOCKED`, suspend by the G2 table |
| JS/JSON only | client-rendered | different endpoint needed |

Change `PARSE_MISMATCH` so it only fires when the page really is a different
layout, and keep the dump of the first N mismatches in `cache/mismatch/` (size
capped, loopback-readable only) so the next one can be diagnosed without a
reproduction.

**Task 3 — act on the result:**
- If a browser-like fingerprint is what flips it: add a per-engine registry field
  `impersonate = none | chrome | firefox`, implemented with libcurl-impersonate or
  an equivalent, default `none`, enabled only for engines the probe shows need it,
  and documented. This is the same mechanism SearXNG ships by default. It is also
  evading bot detection, which the engines' terms prohibit — **the owner's call;
  say so in the docs and the settings page**, and keep every other pacing rule in G2.
- DDG: switch from `lite.` to `html.` with the header/cookie/static-UA rules above
  if the probe shows it helps. Otherwise treat DDG as best-effort.
- Brave: implement the embedded-payload parser (not new XPaths) with a fixture, but
  only if the 200 path works after Task 1. Prefer the Brave Search API (G2) if it
  does not.

**Done when:** the probe matrix is in `docs/`, each of the three engines is either
returning results on the owner's machine or is documented as best-effort with a
reason, and the page message says what is actually happening (G2 task 4).

### G2. Rate limiting: what other aggregators do, and what to copy

**Findings.** *SearXNG values are read from `searx/settings.yml` and
`settings_defaults.py` (master, 2026-10-30). Other facts are from news and docs read
on 2026-10-31 — **[verify]** prices and terms before relying on them.*

1. **Typed, short suspensions that start on the first strike.** SearXNG suspends an
   engine for 180 s on HTTP 429 / "too many requests", 180 s on 402/403, 3,600 s on a
   CAPTCHA, 7 days on reCAPTCHA, 15 days on a Cloudflare CAPTCHA; other errors ban for
   a flat 5 s (`ban_time_on_fail`, with a 120 s maximum setting), cleared on the
   first success. OmniSearch needs **5
   consecutive failures** (five user-visible bad searches) before it opens a 15-minute
   breaker that doubles up to 6 h. So it hammers a 429-ing engine five times and
   then stays away for far longer than the engine's own penalty. Copy the model: trip
   on the **first** 429/challenge, respect `Retry-After` (capped), use a table of
   suspension times per failure type in `config.ini`, and keep the exponential
   backoff only for repeated trips.
2. **Pacing and a small default fan-out.** SearXNG enables only a handful of web
   engines by default (Brave, DuckDuckGo, and — until recently — Google and
   Startpage). Bing, Google, Yahoo, Qwant and Yandex are `disabled: true` by default,
   and Startpage and Mojeek are `inactive: true` because they now use proof-of-work
   CAPTCHAs. Operators report that the stock engines routinely block, CAPTCHA or
   rate-limit a single server IP, and that datacenter IPs fare far worse than
   residential ones. For one user on a home connection the cheapest lever is **demand
   control**: a per-engine token bucket (for example ≤ 20 requests/min and a daily
   ceiling), no upstream calls for autocomplete, history or prefetch, and the answer
   box (G4) skipping the fan-out for questions it can answer locally.
3. **Egress diversity and retry on a different path.** SearXNG has per-engine
   `proxies` (round-robin), `source_ips`, `using_tor_proxy`, `retries` (each retry
   uses another proxy/IP) and `retry_on_http_error` (e.g. `[403, 429]`). OmniSearch
   has a proxy list, but retries don't switch egress per engine. Add
   `proxy = …`/`retry_on = 429,403` per engine, and never retry the same engine on
   the same path inside one request.
4. **Per-engine request profile.** Settings such as `send_accept_language_header`,
   `timeout`, `weight`, `disabled`, `inactive`, `display_error_messages`,
   `enable_http2`, `max_connections`, `retries`, `proxies`, `network` (reuse another
   engine's connection settings) and `tokens` (private engines). Copy the idea, not
   the YAML: registry fields plus a small `[engine.<id>]` section in `config.ini`
   with `enabled`, `timeout`, `weight`, `rate_per_min`, `impersonate`, `proxy`,
   `suspend_429`, `suspend_captcha`, `suspend_error`. Validate unknown keys (C15).
5. **Cache tokens and session state.** SearXNG caches DDG's first-page link for 2 h
   and the Google CSE token for 1 h, and reuses a pooled HTTP client. OmniSearch builds
   a new `curl_multi` per search (see F1 step 4 — verify that was done).
6. **Fallback tiers.** Typical stacks put a paid or official API behind the scrapers
   and call it only when the scrapers return nothing (for example SearXNG first,
   then a paid API, then an empty page). Brave's Search API now has no free plan:
   $5 per 1,000 requests with $5 of credit each month (about 1,000 free queries, card
   required, **check whether a spending cap can be set** before enabling it). Google's
   JSON API is closing (N8). Bing's self-serve API was retired on 2025-08-11.
7. **Non-self-hosted aggregators do not beat rate limits; they pay for access.**
   Yahoo has served Bing's index since 2010, DuckDuckGo kept a private Bing contract
   after the self-serve API was retired, and Ecosia/Qwant are building a shared
   European index. Brave and Mojeek run their own indexes. The realistic levers for a
   free personal instance are therefore: ask less, cache more, pace per engine, spread
   egress, and keep an independent fallback.
8. **Inbound limiter.** SearXNG's limiter (bot detection, link-token, per-IP limits,
   `public_instance` mode) exists to protect the instance's upstream reputation —
   which OmniSearch's C19 already does at loopback + 120/min.

**Tasks.**
1. **Suspension table + first-strike trip + `Retry-After`.** Implement the table in
   point 1 with the SearXNG values as defaults, and tests (`test_c8_breaker.sh`
   extended): 1× 429 opens the breaker for 180 s; a captcha for 3,600 s; transport
   errors keep the C8 rules.
2. **Per-engine token bucket** and a daily ceiling, with the counters in `/metrics`.
3. **Per-engine egress and retry policy** (point 3), off unless a proxy is configured.
4. **Make the message true.** "Will be retried automatically" is wrong when the
   engine is suspended for minutes. Say "paused for N min (rate-limited)" with the
   time left, show the cause class (rate-limited / blocked / changed layout /
   offline) and put an engine status panel in `/settings` (state, last error, next
   retry, requests in the last hour, a "try once now" button that respects the
   bucket).
5. **Fallback chain**, in this order and each optional: scraped engines → SearXNG
   bridge → Brave Search API (only when yield < MIN, with a monthly request cap in
   `config.ini`) → stale cache with a banner. Define "yield < MIN" with F1's
   projected yield.
6. **Recommendation to the owner (put it in the docs):** the strongest "copy what
   they do" move is to run SearXNG on the same machine and point `searxng_url` at it
   (one `docker run`, already supported). Its engine code is updated by many
   maintainers within days of an upstream change, and OmniSearch inherits that
   without maintaining scrapers for Google, Brave and DuckDuckGo itself. SearXNG
   itself still suffers 429s from the same engines, so keep G1 and G2 either way.

**Done when:** a scripted run of 40 queries in two minutes against fake upstreams
(`tests/fake_upstream.py`) that return 429 for one engine produces exactly one
request to it per suspension window; the page names the pause and its remaining
time; and `/metrics` shows the buckets and suspensions.

### G3. A Google engine modelled on SearXNG's "Google CSE" — what it really is

**Correction to the premise.** SearXNG's `google_cse` engine is **not** the official
Custom Search JSON API (which is closed to new customers and ends 2027-01-01, N8).
It uses the Programmable Search *Element* endpoints with a public third-party CX
(blackle.com's), no API key. This is the protocol, described from its source (read
2026-10-30):
1. `GET https://www.google.com/cse/cse.js?cx=<CX>` — the response ends in a JS object
   `({ … })`. Extract `cse_token`, `cselibVersion` and `exp` (a list, joined with
   commas). Cache for about 1 hour. The CX SearXNG uses is
   `partner-pub-8993703457585266:4862972284`.
2. `GET https://cse.google.com/cse/element/v1?rsz=filtered_cse&num=20&hl=<lang>&cselibv=<cselibVersion>&cx=<CX>&q=<query>&safe=<off|active>&cse_tok=<cse_token>&callback=_&rurl=&searchtype=`
   with `Referer: https://cse.google.com/`; add `gl=<country>` unless unspecified,
   `exp=<exp>`, `start=<(page-1)*20>`, and `sort=date:r:YYYYMMDD:YYYYMMDD` for a time
   range. At most 5 pages of 20.
3. The reply is JSONP (`_({…})`). Strip the wrapper, then read `results[]` →
   `unescapedUrl`, `titleNoFormatting`, `contentNoFormatting`
   (`richSnippet.cseThumbnail.src` for a thumbnail). `error.code == 429` means too
   many requests (feed it into G2's suspension).

**Constraints — read before building.**
- **Licence.** SearXNG is AGPL-3.0-or-later. OmniSearch's `LICENSE` is the GNU GPL
  version 2 **[verify the exact header]**. GPL-2.0-only code cannot incorporate
  AGPL-3.0 code. **Do not copy or translate SearXNG's source.** Implement from the
  protocol description above (a clean-room reimplementation), and say so in the
  commit message.
- **Terms and fragility.** The endpoint is undocumented, and borrows another site's
  CX, so it can change or be cut at any time and sits outside the Programmable Search
  terms. It is the owner's call to enable it. Ship it **disabled by default**, label
  it experimental in `/settings`, and give it a small bucket (≤ 10 requests/min).
- **Whether it survives** the JSON API shutdown on 2027-01-01 is unknown. **[verify]**
  in January.

**Task.** Add `google_cse_element` (`index_group = "google"`, `V_WEB`,
`supports_site = 1`) as an API-style engine in `ApiEngines.c` with a two-step
`build_url` (token fetch, cached 1 h, single-flight so concurrent searches do not
each fetch a token), a JSONP-stripping `body_parser`, jansson for the parse, and
fixtures for the token page, a normal result, an empty result and a 429. Remove or
mark dormant the old `google_cse_key/cx` JSON-API path and update `config.ini`.
Keep the `searxng_url` bridge as the documented lower-risk route to Google.

**Done when:** with the engine enabled and fake upstreams, one search issues at
most one token request per hour and one results request, the results rank and dedupe
like any other engine's, and a 429 suspends the engine per G2.

### G4. A Google-style answer box at the top of the page

**What exists.** `Search.c` has a fixed array of six "infobox" handlers (calculator,
units, currency, colour, dictionary, Wikipedia) producing one struct
`{title, thumbnail_url, extract, url}`, rendered in a side rail (`with-rail`).
Wikipedia and the Cambridge scrape run for any query that matches `always_true` or
`is_dictionary_query` (A6). This is a knowledge panel, not an answer box. Keep the
rail for the Wikipedia panel; build the answer box as a separate thing above the
results, as Google does.

**Design.**
- **One answer card**, full width above the results: a big primary line (the value),
  a smaller line (the expression or interpretation), the source and "as of" time,
  an optional "details" disclosure, and a copy button. Converters and the calculator
  have editable fields that **work without JavaScript** as plain GET forms that
  resubmit `q=`.
- **A separate answerer pipeline**, not more `InfoBox` entries. Each answerer is
  `{kind, match(q, locale) → confidence 0..1, compute()|fetch(), render()}`. Pick the
  single highest-confidence match as the primary card (confidence ≥ 0.8); a second
  card only for clear dual intents.
- **Local answerers need no network and consume no upstream budget.** When a local
  answerer matches with confidence ≥ 0.95, **skip the engine fan-out** and show a
  "search the web for this" link. That is both a privacy win and a rate-limit win
  (G2). Network answerers run in parallel with the engines, with a soft deadline of
  ~800 ms; if late, omit the card rather than delay the page.
- **Locale-aware parsing and formatting** (`1.234,56 €` vs `1,234.56`) and phrase
  aliases in data files (`static/lang/<lang>.tsv`, the A5 mechanism), so
  Spanish phrasing ("dólares a euros", "cuánto es 5 millas en km", "hora en Tokio",
  "tiempo en Madrid") works as well as English.
- **Conservative matching.** A bare number or a lone word must not trigger anything.
  "2 broke girls", "python 3.12 vs 3.11", "10 downing street" and "convert" must not.
  Require known unit/currency/city tokens.
- **Setting:** extend `infobox = all|local|off` into `answers = all|local|off`, plus
  per-kind toggles in `/settings`. Anything that sends the query to a third party
  (translate, currency, weather, stocks, definitions, Wikipedia) is `all` only.
- **Failure is quiet**, except for an unambiguous query: "100 usd to eur" with the
  rates API down shows the last cached rate with its timestamp, not an error.

**Kinds for v1, with data sources.** Endpoints and terms below are from memory and
**[verify]** each (availability, key requirements, rate limits, licence/terms) before
building; put each behind a small provider interface so a source can be swapped.

| Kind | Examples | Source | Cache |
|---|---|---|---|
| Maths | `3 + 4*2`, `sqrt(144)`, `15% of 80`, `20% off 59.99`, `0xff + 1`, `5 km + 300 m` | local evaluator (extend `Calculator.c`: functions, constants, `^`, `!`, percentages, bases, unit-aware sums; depth cap) | — |
| Units | `5 km in miles`, `72 f in c`, `3 gb in mib`, `30 mpg in l/100km` | local table in `static/units.tsv` (length, mass, volume, temperature, speed, area, data with KB vs KiB, time, energy, pressure, fuel economy); show both directions | — |
| Date maths | `days until 25 dec`, `30 days after today`, `weekday of 2027-03-01`, `week number` | local | — |
| Time / zones | `time in tokyo`, `9am madrid in new york`, `utc+2 now` | local: OS tz database + `static/cities_tz.tsv` (cities with population ≥ 15,000 from GeoNames, CC-BY — add attribution) with es/en aliases | — |
| Currency | `100 usd to eur`, `5 € en libras` | ECB reference rates (Frankfurter API) for fiat; a crypto source such as CoinGecko's public API for crypto | 1–6 h, with last-good fallback and "as of" |
| Weather | `weather madrid`, `tiempo en sevilla` | Open-Meteo forecast API (lat/lon from the cities file, so no geocoding call and no user IP sent) | 15 min |
| Stocks | `aapl stock`, `tesla share price`, `ibex 35` | provider interface: Stooq CSV quotes by default (no key, delayed); optional keyed provider (Finnhub or Alpha Vantage free tiers); ticker from a small local name→symbol table with a Wikidata fallback; label "delayed", never imply real time | 1–5 min |
| Translate | `translate good morning to spanish`, `how do you say thank you in french` | MyMemory API (free anonymous quota, optional email for more) or a self-hosted LibreTranslate URL from `config.ini`; ≤ 500 chars; show source/target and a link out | indefinitely, keyed by hash |
| Define | `define serendipity`, `qué significa efímero` | a stable dictionary API (Wiktionary REST or dictionaryapi.dev) instead of scraping Cambridge; two senses + part of speech | 24 h+ |
| Random | `roll d20`, `flip a coin`, `random number 1-100`, `uuid` | local | — |
| Colour | `#ff8800`, `rgb(255,136,0)` | existing | — |
| Featured snippet | `who wrote don quixote`, `how many ounces in a pound` | no new source: when the top fused result is from a high-authority host, has coverage ≥ 0.8 and ≥ 2 independent index groups agree on it, show its snippet (1–2 sentences, with the source) above the results. No generated text | — |

Out of scope for v1: sports scores, flights, package tracking, anything needing a
paid or unofficial scraped source. Say so in the docs rather than adding fragile
scrapers.

**Process.**
1. **Table-driven tests first:** `tests/answers/*.tsv` with `query → kind, primary
   text`, ≥ 10 positive and ≥ 10 negative queries per kind, in English and Spanish,
   including locale number formats. Add the worst-case render test from the
   "TemplateContext is full" item above.
2. Build in this order: Maths + Units + Date + Time (all local), then Currency,
   Weather, Translate, Define, Stocks, then the featured snippet.
3. Budgets: local answerers ≤ 2 ms; the page latency p95 must not move (±100 ms)
   because network answerers run in parallel with the engines.
4. Every answerer's network calls go through the same token-bucket and suspension
   code as engines (G2), and honour `answers = local|off`.

**Done when:** the v1 kinds each pass their query tables, a local answer skips the
engine fan-out, a late or failing provider never delays or breaks the page, the
card works with JavaScript disabled, and `/metrics` counts answers by kind.

---

## Part 5 — Round 3: review of `omnisearch_e38410.zip` (H)

*Based on commits `b771d47..f9828f7` (git `f9828f7`, 2026-10-31). I read the code, the
docs the agent wrote and the persisted health file; I did not run it.*

### Where things stand

| Item | State | Evidence |
|---|---|---|
| G1 engines | **Mostly fixed.** DDG moved to `html.` (health: ddg 10/10 OK), Bing and Yahoo healthy (10/11, 11/12). **Brave is still 0/2, breaker open, 2 trips.** | `cache/engine_health.tsv`, `docs/G1-engines.md` |
| G1 "Bing can't parse" | A measurement error (`grep -c` counts lines, Bing's HTML is one line). The real Bing problem is junk results (H2). | `docs/G1-engines.md` |
| G2 rate limits | Landed: typed first-strike suspension, token buckets, a paused-engine panel in `/settings`, docs. G2c hedges to a fresh index group first. | commits `020d83f`, `44c5690`, `f9828f7` |
| G3 Google element | Shipped **disabled and unable to work as delivered** (H3). | `docs/G3-google-cse.md` |
| G4 answer box | **A first slice only** (H4). | `src/Infobox/AnswerBox.c` |

What was done well, and should continue: the docs say what was measured and what was
not; the UA finding for Brave is a real discovery; building the answer table exposed
two real bugs (`2^10` evaluated to `2`; `rgb(...)` missed) that were fixed.

### H1. Right now OmniSearch has one real source, and the fallback has nowhere to go

`ddg`, `bing` and `yahoo` all carry `index_group = "bing"`. Brave is the only other
web index, and it is failing. So today every web result comes from Bing's index, which
(H2) returns junk for some queries. G2c's hedging is correct but starved: there is no
independent engine left to hedge to. This is the real cause of "the engines zone out".

**Tasks**
1. **Make Brave durable.**
   - Don't rely on a single pinned UA. Add a per-engine UA-profile ladder
     (`safari → firefox → chrome` as measured). On a Brave challenge page
     (`challengeSet:{set_token`) retry **once** with the next profile inside the same
     request, and persist the last profile that worked in the health file.
   - Pace the probes. The G1 matrix sent five UAs to Brave back to back, which may
     itself have flagged this IP; `tools/probe.sh` has no pacing or lock. Add a
     per-engine lock, ≥ 60 s between requests, ≤ 3 probes per engine per hour, and
     re-test Brave only after a full cool-down (≥ 1 h) with a single Safari request.
   - Confirm the running binary has the UA pin: add `last_ok` and `last_error`
     timestamps to the health file and to `/info`, so "0/2" can be tied to a build.
2. **Add independent sources.** Each ships disabled until configured, has a probe, and
   has a captured fixture. The owner picks which to enable:
   - the SearXNG bridge (already supported; add a one-command setup note and
     `searxng_engines = google,brave,qwant` as the recommended value),
   - `google_cse_element` (H3),
   - the Brave Search API with a **monthly request cap in `config.ini`** (the free
     credit is about 1,000 queries a month; the card is billed beyond it — **[verify]**
     whether Brave's dashboard can set a spending limit before enabling),
   - Marginalia's search API (its own small index; needs a free key — **[verify]**
     current endpoint and terms), as `index_group = "marginalia"`.
3. **Say it when it is true.** `/settings` shows how many independent index groups
   are currently healthy and warns when it is fewer than 2. The results page notes
   "results from one index only" when just one group answered.

**Done when:** with Brave healthy or at least one other independent engine
configured, ≥ 2 index groups answer on ≥ 90 % of `tools/queries.tsv` (add a
`--groups` report to `tools/eval.py`); otherwise the page note appears.

### H2. Bing returns plausible junk for some queries — find out why, don't only filter it

From `docs/G1-engines.md`: `mechanical keyboard` returns ten SAP-warehouse-error
pages, `python asyncio tutorial` returns ten irrelevant pages, while `linux kernel`
and `weather madrid` are fine. Chrome, Safari and `curl` UAs, with or without a
cookie jar, behave the same. The relevance floor drops the junk, which is the right
outcome, but Bing, Yahoo and DDG-html (one index group) all lose their slots.

**Test, don't theorise.** Run an A/B over ~10 junk-prone and ~10 good queries, one
request per cell, ≥ 60 s apart, judged automatically with the ranker's coverage (a
`tools/junk_rate.py` that reports the share of results under the floor):

| Variable | Values |
|---|---|
| `Accept-Language` vs the IP's region | current; `es-ES,es;q=0.9,en;q=0.8`; `en-US` |
| Bing params | none (current); `adlt=off` (SearXNG sends it, we don't); `ensearch=1`; `setlang`/`cc` (the code comment says these return an empty shell — record the exact failing request, don't just assert it) |
| Cookies | none; market cookies such as `_EDGE_CD`/`_EDGE_S` **[verify]** |
| Query shape | current; with `&form=QBLH` |
| Same queries on | Yahoo and DDG-html (if they are fine, the problem is Bing's front end; if all are junk, it is the index or the IP) |

Put the table in `docs/`, adopt the best parameter set, and save one junk SERP as a
fixture.

**Also:** add a `JUNK` outcome (HTTP 200, ≥ N results, ≥ 90 % below the floor),
distinct from `OK` and `EMPTY`. It feeds the A2 quality EWMA (already exposed in
`/info` and `/metrics`), the page message ("filtered"), and a light demotion. Confirm
that the A2 EWMA really changes slot allocation, not just a metric.

### H3. The Google element probe didn't test the protocol it was given

`docs/G3-google-cse.md` says the token "is minted by Google's obfuscated widget
JavaScript at runtime" and so must be pasted from devtools. The probe table behind that
used `cse.google.com/cse.js` and "a `cx` I could construct". The protocol in G3 of
Part 4 is different: `GET https://www.google.com/cse/cse.js?cx=<CX>` (host
`www.google.com`, path `/cse/cse.js`) with the public CX
`partner-pub-8993703457585266:4862972284`. The token (`cse_token`, `cselibVersion`,
`exp`) sits in the trailing JS object of that response, and SearXNG's engine reads
it from there without running any JavaScript. So "the token can't be obtained
server-side" is **unproven**, not shown.

**Tasks**
1. Repeat the probe once with exactly that URL and that CX. Record status, size and
   whether the trailing `({...})` object contains the three fields. Save the
   sanitised response as a fixture.
2. If it does, implement automatic token fetch (cached ~1 h, single flight) and make
   the pasted token an optional override rather than a requirement.
3. If it does not, record the real response and keep the engine disabled.
4. Keep it disabled by default and experimental in `/settings` either way: it uses
   another site's CX through an undocumented endpoint, which is the owner's call.
5. Don't write "has not been verified" claims into docs without the exact request
   that was made (see H5).

### H4. The answer box is a first slice; finish it

**What exists.** `AnswerBox.c` answers arithmetic, units and colour locally and shows
plain text on a card. Currency, dictionary and Wikipedia are in the table, but the
page only calls `answer_box_resolve_local()`, so the network half is dead code as far
as the page is concerned and those answers still render as infobox cards in the side
rail.

**Gaps against Part 4 G4:**
- No confidence value, and **no skipping of the engine fan-out**. `2+2` still queried
  Yahoo, DDG and Bing (the log shows `27 raw hits collapsed to zero rendered results`).
  A local answer should cost no upstream request and send nothing out.
- No `answers = all | local | off` setting (only `infobox`).
- No locale-aware numbers (`1.234,56`) and no Spanish phrasing; no alias data files.
- Missing kinds: date maths, time and time zones (plus the cities file), random/dice/
  coin/UUID, weather, stocks, translate, a better definition source (the Cambridge
  scrape is still the one), and the featured snippet.
- Converters have no no-JavaScript form.
- Tests have few negatives. Add per-kind tables with ≥ 10 positives and ≥ 10 negatives
  in English and Spanish (`2 broke girls`, `python 3.12 vs 3.11`, `10 downing street`,
  `convert`).

**Order.**
1. Confidence + fan-out skip for local answers ≥ 0.95, a "search the web for this"
   link, `answers = all|local|off`.
2. Local kinds: time/zones/date maths/random, then locale numbers and Spanish aliases
   in data files, with the negative tables.
3. Network kinds as the same card: currency, weather, translate, definitions — each
   behind a provider interface, using the engine token bucket and suspension code
   (G2) and honouring `answers = local`.
4. Stocks (Stooq CSV by default, optional keyed provider, "delayed" label).
5. The featured snippet.

Extend `tests/test_context_capacity.sh` to render the answer card in the worst case.

### H5. Process

- The agent's habit of recording what was and was not measured is right; keep it. Add
  one rule: every measurement claim in `docs/` names the exact request (URL, UA,
  headers, time). Add `docs/probe-log.tsv` that `tools/probe.sh` appends to.
- No burst probing of live engines (H1). It is how a clean IP becomes a flagged one.
- Run `make verify` before reporting and paste its summary line.

---

## Report back

For each ID (F, N, C, A, G and H items), one line: `ID — fixed | not reproducible | deferred (why) — evidence
(command/test/commit)`. Then list anything you found that is not in this document,
and anything marked **[verify]** that you resolved, with the result.

---

# Part 6 — Round 4 (J)

# claude-feedback — Round 4 (J): answer cards, the `1+1` page, theme and layout, SearXNG-class providers

> Prompt for a coding agent (Claude Code or similar).
> Based on `omnisearch.zip` (git `2ae96ec`, branch `sapir-whorf`, working tree of
> 2026-10-31), the owner's five screenshots, `searxng-aggregated.md` (the engine list)
> and `omnisearch.err`. Written 2026-10-31.
> **This file contains only new material.** Parts 0–5 of the previous review
> (IDs F, N, C, A, G, H) still stand and are not repeated; they are referenced by ID.
> (`docs/claude-feedback.md` shows as deleted in the working tree — this file continues it.)
> I read the code and the logs; I did not build or run anything. Items tagged
> **[verify]** could not be confirmed from source or from the documents I fetched —
> confirm them before acting. Three provider policies were read from their own
> pages today and are marked **[read 2026-10-31]**.

## Your job

The owner reported four things and made two requests. All six are traced to code below.

| # | Owner said | Verdict | Item |
|---|---|---|---|
| 1 | Quick-info boxes drop **below the results** when the window narrows | Confirmed: CSS cascade + DOM order, breakpoint at 1240 px | J1 |
| 2 | Those boxes do not follow the theme palette | Confirmed: an undefined token, hard-coded hex in C-generated HTML, a blue icon | J2 |
| 3 | `1+1`: "not web-searched", no results under the calculation, but the side box has Wikipedia text | Confirmed, and it is three different defects | J3 |
| 4 | "search the web for this" returns nothing, "affecting other searches" | Confirmed, and it **is** affecting other searches: one tokenizer bug blanks any single-term query with punctuation or non-ASCII letters | J3a |
| 5 | Stocks, maps, calculator, translation as native, Google-like cards, results underneath | Nothing exists for maps, stocks or translation; the calculator is text only | J4 |
| 6 | Build SearXNG-class providers | The code has 13 engines and one bridge that reads three fields | J5 |

Part J6 lists what I found on the way. Work in the order under **Order of work**.
Ignore anything here that conflicts with a direct instruction from the owner.

## What the screenshots are

Screenshots 1–4 are Google's cards, supplied as the target: translate (`furthermore in
spanish`), calculator (`1+1`), maps (an address query), stocks (`nvidia stock price chart`).
Screenshot 5 is OmniSearch's current `1+1` page: a white card with a pale `2`, the line
"Answered locally — search the web for this", the line "Results from one index only
(bing)…", an empty main column, and a right rail with a blue `=` tile ("Calculation") and a
Wikipedia box for the article "1+1".

## Ground rules (additions to the previous round's)

- Keep C11 and the current dependency set. New browser JS is optional, vanilla, one file
  (`static/widgets.js`), ≤ ~15 KB in total, no framework, no inline script or style.
  **Every card must work with JavaScript off.**
- **The browser never contacts a third party.** Tiles, translations, quotes, thumbnails
  all go through OmniSearch endpoints with fixed upstream hosts (no user-supplied URL ever
  becomes a fetch target; reuse the SSRF guards in `ImageProxy.c`).
- Every new outbound host goes through the G2 token bucket and suspension code, honours
  `answers = local|off` and `infobox = local|off` (all the network cards are network ⇒
  they are off in `local`), and gets a row in SETUP §3 "Where a query actually goes".
- Do not log raw query text, tickers, addresses or coordinates (see J6.1 — it is
  currently violated).
- No inline colour anywhere (J2). Every colour comes from a theme token.
- SearXNG engines are built **clean-room** from each provider's own docs and captured
  responses (J5.0). Do not port SearXNG's Python.
- Measure, don't assert (H5): every measurement claim in `docs/` names the exact request;
  probes are paced (≥ 60 s between requests to one host, ≤ 3 per engine per hour) and
  appended to `docs/probe-log.tsv`.
- **J3c reverses H4 task 1's default** ("a confident local answer skips the fan-out"). The
  owner wants results under every answer, as Google does. Keep the skip as an option.
- One logical change per commit. Run `make verify` and paste its summary line.

## Order of work

1. **J3a** (tokenizer) and **J3d** (URL encoding): small, correct more searches than anything
   else here, and every later item is judged on queries these break.
2. **J3b, J3c**: make the answer page honest, and put results under answers.
3. **J1 + J2** together (both rewrite parts of `main.css`), with the two lints.
4. Widget layer + **J4a** calculator (no network, so it settles the architecture).
5. **J5.6** (bridge fixes, cheap) → **J5.0–5.3** (framework, rich results, tabs) → J5 tiers.
6. **J4b** translate → **J4c** maps → **J4d** stocks (each needs paced provider probes first).

---

## J1. The quick-info rail ends up below the results

**What the code does (confirmed):**
- `templates/results.html:36-39,115`: DOM order is `sidebar-spacer`, `<main>`, `<aside
  class="infobox-sidebar">`. The rail is last in the source.
- `static/main.css` defines `.content-layout` in **seven places** with conflicting grid,
  padding and ordering: lines 320, 438, 506, 587 (three of them inside media queries), 1133,
  1145 (media) and 1250 (the `.with-rail` block). The 1200 px block sets
  `.infobox-sidebar { order: -1 }` (line 458: rail first), and a later
  `@media (max-width: 1240px)` block (line 1391, "keep the results first") sets `order: 0`
  on both (line 1394), which wins on specificity.
- Net effect: at ≥ 1241 CSS px, two columns with the rail at 360 px (`--rail-width`). At
  ≤ 1240 px, one column and the rail comes after the last result, the pager and the warnings
  (lines 1283-1292). Half a laptop screen is already under 1240 px. The answer box lives
  inside `<main>`, so it stays on top; the rail does not. That is the reported behaviour.
- Dead rules left behind: `.infobox { grid-column: 3 }` (line 354: the infobox is not a grid
  child), `.sidebar-spacer { display:none }`, `.results-container { grid-column: 2 }`.
- Every template sets `style="zoom: {{page_scale}}%"` on `<html>` (`results.html:2`). Media
  queries do not see that zoom, so at `page_scale` ≠ 100 the breakpoints fire at the wrong
  window width. **[verify]** in the browsers the owner uses.

**Target behaviour (Google's):**
- Wide (≥ ~1100 px): results column, rail on the right.
- Narrow: the knowledge panel becomes a compact card **above** the results, collapsed to
  2–3 lines with a "more" control, never after the pager. Answer cards (J4) are always the
  first thing in the main column at every width.

**Tasks**
1. Replace the seven `.content-layout` blocks with one, using `grid-template-areas`
   (`"main rail"` wide, `"rail" "main"` narrow) instead of `order` hacks. One breakpoint
   token. Delete the dead rules.
2. Narrow-width panel: `<details class="panel-compact">` (no JS), summary = title + first
   sentence, body = the full card. Closed by default under the breakpoint, open on wide
   (a tiny `matchMedia` enhancement is allowed, but the CSS-only default must be sane).
3. Replace `zoom` with a scale that media queries and layout agree on (root `font-size`
   in `rem`, or `--scale`). Apply to all templates, not only results. **[verify]** the
   settings preview still works.
4. Test: `tools/layout_check.py` using Playwright **if present** (print a clear skip
   otherwise): render a fixture page (answer card + panel + 10 results + pager) at 1600,
   1300, 1241, 1240, 1100, 900, 600 and 375 px, in four themes; assert wide ⇒ panel's left
   edge is right of the results' right edge; narrow ⇒ panel's bottom is above the first
   result's top. Plus a text lint (in `make check`) that fails if `.content-layout` is
   defined in more than one place.

**Done when:** the assertions pass at all eight widths and four themes, and a screenshot
contact sheet at 1241/1240/900 is committed under `docs/`.

---

## J2. The cards do not follow the theme palette

**What the code does (confirmed):**
1. `main.css:720`: `.answer-box { background: var(--surface, #fff) }`. **`--surface` is
   defined in no theme**, so the fallback `#fff` always applies: a white card on every dark
   theme. No text colour is set, so the text inherits `--text-primary` — light on white. That
   is the pale `2` in screenshot 5.
2. `.answer-box-source a` has no style: browser-default purple/blue link (screenshot 5).
3. The card also differs in shape from everything else: `border-radius: 10px`; `.infobox`
   is square with a 1 px `--border`.
4. HTML built in C carries fixed greys: `Calculator.c:130` (`#eee`), `ColourCode.c:159-214`
   (`#ddd`, `#666`), `CurrencyConversion.c:364` (`#666`), `Dictionary.c:68-80` (`#666`,
   `#888`, `#555`). They disappear or clash on dark, amber and custom themes.
5. `static/calculation.svg` is hard-coded `#3498db` with a white glyph (the blue tile in
   screenshot 5), and it is reused as the thumbnail for currency (`CurrencyConversion.c:450`)
   and units (`UnitConversion.c:613`). `static/dictionary.jpg` is a 1200×1200 raster.
   An `<img>` cannot take `currentColor`.
6. Themes define exactly nine tokens (`check_themes.py` `REQUIRED`). There is no semantic
   token for anything J4 needs: up/down, selected, key caps, text on an accent fill, map
   filter. The `custom` theme injects the same nine inline in `results.html:12-26`, and
   duplicated in the other templates.

**Tasks**
1. **Add semantic tokens, derived by default so no theme has to change:** in `main.css`'s
   `:root`, `--surface`, `--surface-2` (key caps, inset), `--on-accent`, `--link`, `--pos`,
   `--neg`, `--focus`, `--map-filter`, defined with `color-mix(in srgb, …)` from the nine
   base tokens. A theme may override any of them. Custom properties resolve at computed-value
   time, so a derived default follows the theme loaded after it. Add the same keys to the
   custom-theme injection in one shared include (not copied into each template).
2. `.answer-box` / widget cards: `--bg-card`, 1 px `--border`, no radius (match `.infobox`),
   text `--text-primary`, links `--link` with a visited state.
3. **No `style='…'` in `src/Infobox/*.c`.** Emit classes (`calc-expr`, `calc-result`,
   `muted`, …) and put the rules in `main.css`. The one legitimate dynamic colour (a colour
   swatch) uses `style="--swatch:#rrggbb"` with a validated hex, or an SVG `fill`. This is also
   what makes a strict `Content-Security-Policy: style-src 'self'` possible later.
4. Replace `calculation.svg` and `dictionary.jpg` with inline SVG using `currentColor`
   (or drop the image column for answer cards — they are cards now, not Wikipedia stubs).
5. **Two lints in `make check`:** (a) every `var(--x)` used in `main.css` and the templates
   resolves to a token defined in every theme or defaulted in `:root` — this exact lint
   would have caught `--surface`; (b) no hex/`rgb()` literal in `src/Infobox/*.c` or in
   `main.css` outside token definitions. Extend `check_themes.py` with WCAG contrast
   (text on `--bg-card` ≥ 4.5; `--pos`/`--neg` on `--bg-card` ≥ 3). **Report the themes
   that fail; do not weaken the check to make them pass.**
6. `tools/theme_shot.py`: the same fixture page in every theme as a contact sheet in
   `docs/`. The acceptance review is "no white card on a dark theme, nothing unreadable".

**Done when:** both lints and the contrast check pass, the contact sheet shows every card in
every theme (including `system` in both colour schemes), and `grep -n "style='" src/Infobox`
returns nothing.

---

## J3. The `1+1` page: three defects, and one of them blanks other searches

The log settles what happened. `omnisearch.err` at 16:27:45:
`[WARN] 27 raw hits collapsed to zero rendered results (query "1+1"): yahoo=7 brave=0 ddg=10 bing=10`.
So "search the web for this" **did** query the engines, and the ranker threw all 27 hits away.

### J3a. A query term and a document token are produced by different tokenizers (root cause)

- `QueryAnalysis.c:421-431`: query terms are the **space-separated words**, length ≥ 3, not a
  stopword, lower-cased ASCII-only (`lower_copy`, line 103). So `1+1` is one term `1+1`;
  `covid-19` is `covid-19`; `#123456`; `google_cse`; `1.234,56`; `españa` stays raw UTF-8.
- `Ranker.c:349-370` `tokenize_into` (used for every document): splits on **every byte that is
  not `[A-Za-z0-9]`** and keeps tokens ≥ 2. A document can never contain `1+1`, `covid-19`
  or `españa` as a token.
- `Ranker.c` document frequency (~570-596) is therefore 0 for such a term; the coverage loop
  skips terms with `df == 0` ("a term no candidate contains cannot be matched", line 691), so
  `idf_sum == 0` and `coverage = 0.0` (line 719) **for every document**; `Search.c:1636`
  drops everything under 0.15 (the 0.08 top-up at 1645 is also under it). Blank page, hint
  "none of what came back was relevant enough".
- A third tokenizer, `Search.c:341-376` `tokenize_query` (min length 3, ASCII), feeds the
  infobox relevance check (J3b).

**Evidence:** the log has 6 distinct queries with 20–37 raw hits and 0 rendered:
`1+1`, `2+2`, `covid-19`, `#123456`, `google_cse`, `1.234,56 + 2`. Every one has exactly one
term containing punctuation. By the same path (not in the log; **[verify]** with the tests
below): any single-word query with non-ASCII letters (`españa`, `camión`, `niño`, `москва`,
`日本`), `c++`, `c#`, `node.js`, `.net`, `wi-fi`, `gpt-4`, `e-mail`, `o'brien`, `3.14`, `$100`,
`50%`, `user@example.com`. In a multi-word query the term is silently ignored (so the
ranking is worse, not blank). The owner searches in Spanish; accents are not an edge case.

The "unknown terms are ignored" rule (typos) and "all terms unknown ⇒ coverage 0" (so
`zzqqxx vbbnmm` renders nothing) are both right **for words**. The bug is that tokenization
makes legitimate terms unmatchable.

**Tasks**
1. **One tokenizer, one file** (`src/Query/Tokens.[ch]`), used by `QueryAnalysis`, `Ranker`
   and `Search.c`. UTF-8 aware: treat any byte ≥ 0x80 as a word byte at minimum; prefer real
   letter classes if available on macOS and Linux **[verify]**. For *matching only*, fold
   case and strip diacritics (`españa` ≈ `espana`, `camión` ≈ `camion`); the engine query
   is never altered. Digits count at length 1; letters at 2; CJK per character or bigram.
2. **Punctuated terms:** a term matches a document if the case-folded literal is a substring
   of title, snippet or URL, **or** all its sub-tokens (`covid`, `19`) appear. Symbol-heavy
   terms (`c++`, `c#`, `.net`) match through the literal path.
3. **Never turn "no matchable term" into "everything is junk".** If a query has no term the
   tokenizer can ever match (pure symbols, numeric expressions), skip the coverage floor and
   rank by RRF: the engines already judged relevance. Keep the nonsense-query guard for
   alphabetic terms that no candidate contains. Also skip the floor when the user pressed
   "search the web for this" — explicit intent.
4. **Tests** (`tests/test_j3_terms`): ≥ 25 queries run against captured fixture result sets —
   include a sanitised copy of the `1+1` and `covid-19` SERPs — each asserting ≥ 5 rendered
   results; and the nonsense set (`zzqqxx vbbnmm`, `qwpoeiruty asdfghjkl`) still rendering 0.
   Cover: `1+1`, `2+2`, `covid-19`, `#123456`, `google_cse`, `c++`, `c#`, `node.js`, `.net`,
   `wi-fi`, `gpt-4`, `3.14`, `50%`, `españa`, `camión`, `niño`, `café`, `über`, `москва`,
   `日本語`, and two-word versions of each.

**Done when:** every listed query renders a non-empty page against its fixture, the nonsense
set stays empty, and `grep -c "collapsed to zero"` over a 40-query smoke run is 0 for them.

### J3b. The page says "answered locally" and then contradicts itself

**What the code does (confirmed):**
- `Search.c:1250-1254`: the infobox threads start on every page-1 request, **including when
  `skip_fanout` is set**. The Wikipedia handler is `always_true` (handler table, ~line 459)
  and uses `generator=search` (`Wikipedia.c:170-173`): the best fuzzy match for any text. The
  relevance check (`Search.c:527-541`) only runs when the query has a token of length ≥ 3, so
  for `1+1` it defaults to "relevant". Hence the article "1+1" in the rail. That is what the
  owner read as "it actually had some results": it is not a result, it is an infobox thread.
- The threads are joined before render (`Search.c:1351-1355`), so the "instant" local answer
  waits for Wikipedia. The log has `1+1` at 16:26:06 taking **1607 ms** with no fan-out
  (median `/search` is ~600 ms). **[verify]** by timing with `infobox = off`.
- `results.html:46` prints "Answered locally — search the web for this" even on the `?web=1`
  page it links to, then `one_index_note` (`Search.c:1930-1946`) adds "Results from one index
  only (bing)…" under an empty column. Three statements, none true.
- Hard-coded English in the new strings: "Direct answer", "Answered locally", "search the web
  for this", "Some sources did not answer", "No results found for" (`results.html`, SETUP #14).
- `omnisearch.err` has 9× `render_template_segment: Key 'answer_source_note' not found in
  context. Appending tag as-is` — the literal `{{answer_source_note}}` reached a page. The
  current source sets it at all three call sites (1436, 1915, 2063), so this may be an older
  build **[verify]**.

**Tasks**
1. **Gate the Wikipedia panel.** It renders only if the article title or first sentence
   matches ≥ 60 % of the *normalised* query tokens (J3a's tokenizer) or the title matches
   exactly. A query that is a numeric expression, a unit conversion or a colour code never
   gets the generic panel.
2. **A deadline for slow threads:** wait ≤ ~700 ms for infobox threads, render without a
   late one. Optionally lazy-load the panel via `GET /w/panel?q=` (fragment) with a tiny
   `fetch()` enhancement and a `<noscript>` link.
3. State lines match reality: `web=1` ⇒ the answer note reads "Showing web results" (no
   link to itself); `web=1` is carried through pagination, tab and pager hrefs; the
   one-index note is never printed when no engine was asked or when the page is empty for
   another reason.
4. Move every English string in `results.html` into `locales/*.ini` (en_gb + es_es at
   minimum; the rest may fall back).
5. Render check fails on any `not found in context` warning.

### J3c. Results belong under the answer (owner decision)

Google shows the calculator **and** the web results below it. Screenshot 5 shows an answer
and an empty page.

**Tasks**
1. New `[server] answers_fanout = always | auto | never`, default **`always`**: the local
   answer is computed first (it is pure computation) and the engines run in parallel as on
   any other query, so the page waits no longer than a normal search.
   `never` is the current H4 behaviour. `auto` skips only for confidence ≥ 0.95 **and** a
   query with no words in it.
2. `answers` currently has no UI at all (`grep answers` → 0 hits in `settings.html`,
   `Settings.c`, `SettingsSave.c`). Add both settings to the page (cookie, same pattern as
   the others), with one line saying what leaves the machine in each mode.
3. Update `tests/test_h4_answers.sh` and the SETUP `answers` row: `2+2` now queries the
   engines by default; the test that asserted otherwise moves to the `never` mode.

### J3d. The query is pasted unescaped into links

`build_search_href` (`Search.c:720-751`) and `build_images_href` (`Images.c:23-40`) write
`raw_query` into `/search?q=%s` / `/images?q=%s` as is; `_chrome.html:17,20` use
`/images?q={{query}}` and `/settings?q={{query}}`. `1+1` becomes `q=1+1`, which decodes as
`1 1`; `AT&T` becomes `q=AT`; `C#` becomes `q=C`. This hits the pager, the All/Images tabs and
the settings link, so page 2 of an operator query searches something else. **[verify]**
whether beaker's `{{ }}` encodes anything beyond HTML (it likely does not).

**Task:** one `url_encode()` helper (`curl_easy_escape`, already used for `query_encoded` at
`Search.c:1066-1076`) used by every href builder; pass `query_encoded` to `_chrome.html`.
**Test:** `q=1%2B1`, `AT%26T`, `C%23`, `50%25` round-trip unchanged through the tab links,
the pager links and the settings link.

---

## J4. Native cards: calculator, translation, maps, stocks

**Where things are.** The "answer box" is two text lines (`answer`, `detail`). The rail is
HTML strings built with `snprintf` in C. Nothing here is an interactive card. There is no
code for stocks, translation or geocoding at all (grep for `stock`, `translate`, `weather`,
`geocode`, `ticker` finds nothing relevant), and the only "map" is the currency-rate type.

### J4.0 The widget layer (build this first, then the cards)

- One registry for answer kinds (extend the `Answerer` table in `AnswerBox.c`, don't add a
  second): `{id, matches(query, ctx) → confidence, resolve(query, ctx, &data), template,
  provider_chain, ttl}`. `resolve` returns **structured data**, not markup. Templates live in
  `templates/widgets/<kind>.html`; CSS in a `widgets` section of `main.css`. This removes the
  C-built HTML that caused J2.
- Placement: the primary widget is the first element of the main column at every width; the
  knowledge panel is the rail (J1). One primary widget per query.
- No-JS baseline: controls are GET forms that re-render `/search?q=…&w=<kind>&…`. JS only
  enhances (J4a keypad, J4b translate-as-you-type, J4d chart hover, J4c pan/zoom).
- Endpoints `GET /w/<kind>` return JSON or an HTML fragment for the enhancement. They share
  the inbound limiter, fixed upstream hosts, size caps (query ≤ 512 B, text ≤ 5 000 chars),
  and are cached in the file cache with a per-kind TTL and single-flight per key.
- **Providers:** `provider_fetch(kind, params) → normalised struct`, an ordered chain per
  kind in config (`[widgets] translate = lingva,mozhi,mymemory`), keyless defaults first,
  keyed ones disabled until a key exists. Provider health is shown in the `/settings` table
  next to the engines. The page always says which provider answered and how old the data is.
- Matching: each kind gets a table of ≥ 10 positives and ≥ 10 negatives in **English and
  Spanish** (the owner's locale and queries are Spanish), as in H4's process.
- Exposure: `example-config.ini` binds `0.0.0.0` with no authentication. New proxy endpoints
  (`/tile`, `/w/*`) are an open relay for anyone on the LAN unless limited — apply the
  inbound limiter and the same guards as `/image-proxy`.

### J4a. Calculator (screenshot 2)

**Today:** `Calculator.c` is a recursive-descent parser for `+ - * / ^` and parentheses.
The matcher needs a digit and an operator. Confirmed gaps:
- No functions or constants: none of `sin cos tan ln log √ π e ! % EXP Ans Deg/Rad` from the
  keypad exist.
- Errors are silent: `parse_factor` returns `0` when `strtod` consumes nothing (`5 + * 3`,
  `(2+3`); `1/0` gives `inf`, which `answer_arithmetic` lets through (it only checks NaN,
  `AnswerBox.c:77`). **[verify]** by running them.
- Two formatters disagree: the answer box prints up to 10 significant digits
  (`AnswerBox.c:79-82`), the card prints `%g` (`Calculator.c:~137`), so `1234567+1` shows
  `1.23457e+06` in the card.
- The card prints each binary operation as a step list; Google shows the expression, a big
  result, and a keypad.

**Tasks**
1. Replace the evaluator with a tokenizer + precedence parser (shunting-yard or Pratt):
   `+ - * / ^ **`, unicode `× ÷ − ·`, implicit multiplication (`2(3+4)`, `2π`), unary minus
   (`-2^2 = -4`, `2^-1`), right-associative `^` (`2^3^2 = 512`), `%`, `!`, `sqrt cbrt abs
   floor ceil round mod`, `sin cos tan asin acos atan sinh cosh tanh`, `ln log log2 exp`,
   `π e`, `EXP` (×10ⁿ), Deg/Rad. Use `long double`; trim to ≤ 12 significant digits;
   thousands separators and decimal mark by locale (reuse `answer_normalise_locale_numbers`).
   One formatter used by both the answer and the card.
2. Real errors: `Error`, `Infinity` (Google prints `Infinity` for `1/0`), unbalanced
   parentheses. Never answer on a parse that did not consume the whole string.
3. Card = expression line (`1 + 1 =`), large result, keypad in the screenshot's 5×7 layout:
   `Deg|Rad, x!, (, ), %, AC / Inv, sin, ln, 7 8 9 ÷ / π, cos, log, 4 5 6 × / e, tan, √,
   1 2 3 − / Ans, EXP, xʸ, 0 . = +`. No-JS: each key is `<button name="k" value="7">`
   in a GET form that appends to an `expr` param and re-renders; with JS the keypad works
   locally and the history is `localStorage` only (no server-side history; privacy).
   Omit "Math Solver".
4. Decide and document the semantics of `%` (Google reads `100 + 10%` as 110) and `mod`.
5. **Test:** ≥ 60 expressions in a table (precedence, associativity, unary minus, implicit
   multiplication, `0.1 + 0.2` shown as `0.3`, `1/0`, `5 + * 3`, `(2+3`, huge and tiny
   magnitudes, `1,234.5 + 1` and `1.234,5 + 1` in `es`), plus negatives (`covid-19`,
   `2 broke girls`, `10 downing street`, `iphone-15`, `3-2-1 song`).

### J4b. Translation (screenshot 1)

**What Google shows:** source selector ("English - detected") ⇄ target selector; source
text with a phonetic line and speaker; target text with copy, speaker and "Open in Google
Translate"; then "Translations of *furthermore*" grouped by part of speech (`además — also,
besides, further, moreover…`, `más — more, most, other, else…`).

**Triggers (EN + ES):** `X in spanish`, `translate X to french`, `how do you say X in
german`, `X en inglés`, `traduce X al español`, `cómo se dice X en inglés`. A single word
also yields the "Translations of" list. Negatives: `spanish in america`, `in spanish class`,
`translate.google.com`, `how to speak spanish`.

**Providers (owner's list: Lingva, Mozhi, MyMemory, Dictzone):**
- **Lingva** — a public front end that returns Google-Translate-derived JSON including
  pronunciation and dictionary info. Public instances come and go and it depends on an
  upstream it does not control: make the instance list configurable, probe before trusting
  one, treat it as best-effort. **[verify]** the response shape and current instances.
- **Mozhi** — front end over several translation back ends. **[verify]** API and instances.
- **MyMemory** — `GET api.mymemory.translated.net/get?q=…&langpair=en|es`, optional `de=`
  (an email for support/volume). **[read 2026-10-31]** `q` is limited to **500 bytes**; the
  daily quota is on a separate "usage limits" page I did not read **[verify]**. Alternatives
  come back as `matches[]` — useful for the alternatives list. Chunk long text at sentence
  boundaries ≤ 450 B.
- **Dictzone / Wiktionary** — for the "Translations of" lists; Dictzone is HTML
  (**[verify]** terms), Wiktionary has a keyless API.
- **LibreTranslate** — optional, self-hosted via `libretranslate_url`.

**Tasks:** provider chain per J4.0; two `<select>`s fed by a data file
(`static/lang/names.tsv`, code → name per UI locale); swap = a link with swapped params;
editable source `<textarea>` in a GET form; JS enhancement debounces ~400 ms to `/w/translate`.
Copy via the Clipboard API. Speaker via the browser's `speechSynthesis` (local voices; hide
if unsupported). **No microphone/speech-recognition** by default: browsers implement it by
sending audio to their vendor's cloud, which contradicts the project's privacy stance — the
owner's call. "Open in <provider>" links to the provider that answered. Detected-language
label only when the provider reports one (or `QueryAnalysis` detection is confident). Footer
names the provider; Settings says plainly that the text is sent to a third party.

**Test:** parse fixtures per provider; matcher tables EN+ES; a 600-byte input proves
chunking; provider A failing falls to B with the breaker state visible.

### J4c. Maps (screenshot 3)

**What Google shows:** a dark map with a pin and labels, a street-level photo thumbnail, the
normalised address as title (`C. del Peñasco, 8` — Google snapped the typed "avenida" to a
different street type), the postcode, city, region and country as subtitle, and Directions /
Share buttons; a Maps tab appears.

**Triggers:** address-shaped queries (street-type words and a number in en/es/fr/de/it/pt:
`calle, avenida, av., paseo, plaza, carrer, rue, strasse, street, road`; postcode forms),
`mapa de X`, `map of X`, `where is X`, `X on map`, `dónde está X`. Place names only when the
geocoder is confident (`importance` above a threshold) **and** the Wikipedia panel does not
already own the query. No "near me" server-side (no geolocation); offer a browser geolocation
button only on click.

**Providers (owner's list: OpenStreetMap, Apple Maps, Photon):**
- **Geocoder — Nominatim.** **[read 2026-10-31]** absolute maximum 1 request/second; an
  identifying `User-Agent` or `Referer` is required (library defaults do not count); cache
  results (repeating a query can get a client flagged); **autocomplete / search-as-you-type is
  forbidden** (so `/autocomplete` must never call it); show attribution (ODbL); no bulk or
  systematic queries. One request per search, cached ≥ 24 h, paced through the engine token
  bucket at ≤ 1/s with a contact UA, is within that. `geocoder_url` is configurable so the
  owner can point at a self-hosted Nominatim or Photon. Photon's own usage terms
  **[verify]**; Apple Maps needs a token (optional, off by default).
- **Tiles — OpenStreetMap standard raster.** **[read 2026-10-31]** HTTPS only; a distinct,
  stable `User-Agent` naming the app with contact; honour `Cache-Control`/`ETag` or keep each
  tile ≥ 7 days; no bulk/prefetch/offline, no scanning at high zoom; no `no-cache` headers;
  don't strip the Referer; attribution "© OpenStreetMap contributors" visible bottom-right.
  The policy **does not address static-map composition** and does not recommend caching
  proxies; if the card is served through `/tile`, the proxy must meet every rule above, and
  the owner should confirm with the OSM Foundation before relying on it. Make `tile_url`
  configurable so a self-hosted or commercial tile server drops in.

**Design:** no Leaflet and no framework. The card is a 3×3 grid of 256 px `<img>` tiles from
`/tile/{z}/{x}/{y}` (own cache, fixed upstream host, SSRF-safe) with a CSS-positioned pin;
the browser never contacts OSM. Optional JS: drag-pan and zoom over the same endpoint
(~150 lines). Dark themes: `filter: var(--map-filter)` on the tile layer (an invert/hue
filter token from J2) rather than a second tile provider. **Street-level photo: omit.**
There is no keyless equivalent; do not fake one. Directions: a link built from a configurable
template (`maps_directions_url`, default an OSM directions URL) and a `geo:lat,lon` link for
phones. Share: `navigator.share` with copy-link fallback.

**Address normalisation:** accent-insensitive (`penasco` ≈ `peñasco`); free-form `q=` first,
structured fallback; when the returned street type differs from the typed one, say "Showing
closest match". If the geocoder is rate-limited or down: text-only card, "map unavailable",
never a blank page.

The Maps tab is J5.3 (`/search?cat=maps`): Nominatim / Photon result list + one map with
markers.

**Test:** matcher tables EN+ES (`avenida del penasco 8`, `calle mayor 5 madrid`, `10 downing
street`, `mapa de madrid`; negatives `5th avenue shoes`, `avenida` alone, `calle 13 band`);
geocoder fixture parse; `/tile` rejects any host/zoom out of range; log contains no query,
address or coordinates.

### J4d. Stocks (screenshot 4)

**What Google shows:** logo, name, `NASDAQ: NVDA`; price, change pill and absolute change;
market status line (`Closed: Oct 30, 7:59 PM EDT`) and extended-hours quote; range tabs
`1D 5D 1M 6M YTD 1Y 5Y Max`; a chart with a dotted previous-close line (1D) and a grey
extended-hours segment; a stats grid (Open, High, Low, Mkt cap, P/E, 52-wk high/low,
Dividend); a Related list of tickers with price pills; news, Earnings and Quarterly
financials cards. **Omit:** Follow (no accounts), "Why is NVDA down" and "Analyze" (AI).

**Triggers (EN + ES):** `nvda stock`, `nvidia stock price chart`, `NASDAQ:NVDA`, `$NVDA`,
`nvidia acciones`, `cotización nvidia`, indices (`^GSPC`, `ibex 35`), crypto via the existing
currency/crypto path. Require a cue word (`stock, share price, acciones, cotización,
ticker`) or an `EXCHANGE:SYM` form; resolve names → tickers from a local
`static/tickers.tsv` (top ~500 names/aliases, like `cities.tsv`) so common queries need no
lookup. Negatives: `nvidia drivers`, `apple pie`, `stock photos`, `chicken stock`, `tesla coil`.

**Providers — all unverified, measure each with paced probes (H5) and record them:**
- **Stooq** CSV (quote and daily history, no key, delayed): fits 5D–Max; no fundamentals
  **[verify]** intraday availability and terms.
- **Yahoo Finance chart endpoint** (unofficial, no key as far as I know): would give previous
  close, 52-week range, pre/post-market and the intraday series for 1D. **[verify]** current
  behaviour: crumb/cookie requirements, the EU consent redirect (the owner is in Madrid),
  User-Agent expectations, and terms. Best-effort provider with breaker, falling back to Stooq.
- **Keyed, optional, off by default** (Finnhub, Twelve Data, Alpha Vantage, FMP): quote,
  profile, ratios, earnings, peers (→ "Related"). Free-tier limits **[verify]** each; one
  that allows only a handful of calls a day cannot back a search page.
- Label everything "delayed, source X, as of <time in the exchange's zone>"; never show a
  price without its age; "not investment advice".

**Rendering:** server-side inline **SVG** chart — no JS required: area/line polyline,
3–4 grid lines, dashed previous-close line with label, extended-hours segment in
`--text-dim`, colour by sign via `--pos`/`--neg` (J2), x labels in the exchange time zone,
≤ 300 points (downsample). Range tabs are plain links (`&range=5d`). JS adds a hover
crosshair reading from a `data-points` attribute (≤ 3 KB). Missing stats are shown as `—`,
never invented. Related tickers link to `/search?q=<name> stock`. The news card reuses the
J5 News category (optional).

**Caching:** quote 60 s while open / 15 min closed; chart 1D 60 s, 5D 5 min, ≥ 1M 1 h, Max
24 h; stale-while-error; single-flight per symbol so ten clients make one upstream call.
Market state from the provider; otherwise weekday + exchange zone (and say "Closed"), with
holidays as a known gap.

**Test:** sanitised chart-JSON and CSV fixtures; golden SVG test (point count, the path
starts at the first close, the previous-close line's y matches); pill colouring; every theme
(J2); matcher tables EN+ES.

**Done when (J4):** calculator, translate, maps and stocks each render in every theme at all
widths with JS disabled, each has provider fixtures and a matcher table, `answers = local`
makes all three network cards disappear without error, and SETUP §3 lists every new host.

---

## J5. SearXNG-class providers

The owner attached SearXNG's engine list (~220 engines in General, News, Images, Videos,
Music, IT, Science, Files, Social, Maps) and asked to build them. I recommend **building the
framework once so an engine is ~10 lines of data, then adding engines in tiers** — not 220
hand-written parsers. Many in the list are front ends for the same index, many need keys, some
are hostile to automated use.

**Where the code is:** 13 registry entries; verticals `V_WEB/CODE/ACADEMIC/ENTITY/NEWS/MEDIA`
exist but only `hn` has `V_NEWS` and nothing has `V_MEDIA`; the chrome has `All | Images`;
`SearchResult` is `{url, title, snippet}` (`Scraping.h:10-14`); `results.html` renders four
fields.

### J5.0 Licence and clean-room

`LICENSE` is the GNU GPL v2 text (1989/1991); I found no per-file notice saying "or later"
**[verify]**. SearXNG is AGPL-3.0, and GPL-2-only and AGPL-3 are not compatible. Porting or
translating its Python engine modules would make a derivative. Build
each engine from the provider's own documentation and captured responses (as G3 did). The
engine names and bangs in the attached list are facts and fine to use. **The owner should
confirm the licence intent** (README also asks that the code not be hosted on proprietary
platforms).

### J5.1 Declarative engine definitions

- Load definitions at startup from `static/engines.d/*.ini` (or one TSV) into a **heap**
  registry. Kinds, mirroring the *idea* of SearXNG's generic engines: `json` (JSON paths
  via jansson), `feed` (RSS/Atom/XML), `html` (libxml2 XPath), and `c` (existing hand-coded
  parsers for Bing, Brave, DDG).
- Fields: `id, name, categories, url` template (`{q} {page} {offset} {lang} {region}
  {safe}`), `method`, `headers`, `ua`, `cookies`, result paths (`results, url, title,
  content, thumbnail, date, author, extra.*`), `index_group`, `weight`, `pace`, `timeout`,
  `requires_key`, `enabled_default`, `terms_note`, `rate_note`.
- `tests/registry_lint` additionally requires: unique ids, a fixture, a `docs/probe-log.tsv`
  entry, a declared `index_group`, and a `terms_note` for anything keyed or rate-limited.
- **Constraint:** `ENGINE_REGISTRY[]` and `ENGINE_COUNT` are compile-time. `results_handler`
  sizes `eligible[ENGINE_COUNT]` (`Search.c:862`), `jobs[ENGINE_COUNT]` and
  `all_results[ENGINE_COUNT]` (`Search.c:1089-1090`) on the stack, and uses `1u << i` masks
  (`Search.c:1550,1580`). With
  100+ engines move per-request arrays to the heap, sized by the **selected** engines (≤ 6–8
  plus hedges), keep the masks on job indices, and stop using `ENGINE_COUNT` as a request
  bound. **[verify]** beaker's thread stack size on macOS (secondary threads default to a
  small stack); `ScrapeJob` is ~2 KB, so 150 of them on the stack is ~300 KB before `cands[128]`.

### J5.2 Rich results

Extend `SearchResult` with `kind` and optional fields (`thumbnail`, `date`, `duration`,
`author`, `source`, `size`, `seeds`, `lat/lon`, `doi`, plus key/value `extra`). Templates per
kind: web (today), image grid (exists for Bing), video (thumbnail, duration, channel), news
(source, age), paper (authors, year, venue, DOI/PDF), package (name, version, downloads),
repository (stars, language, updated), file, place, audio. All thumbnails through
`/image-proxy`. Update the `results.html` row from positional `result[0..3]` to named
fields.

### J5.3 Tabs and categories

Chrome becomes `All | Images | Videos | News | Maps | More ▾` (a no-JS `<details>`:
Science, Code, Music, Files, Social), horizontally scrollable at narrow widths. Each tab is
`/search?q=…&cat=<id>`; the engine set comes from the registry's `categories`. Per-category
fusion: RRF within the category; the web-only signals (authority, spam, coverage floor)
**do not apply to non-web kinds** — in particular the floor must not drop media or
non-English results. Settings: engines grouped by category, with a filter box.

### J5.4 Independence accounting (continues H1)

Every web engine declares `index_group`. Genuinely separate indexes in the list: Brave
(exists), Mwmbl, YaCy (exists), Yandex, Baidu, Naver, Seznam, Sogou/360/Quark (regional),
Google (via the bridge or the CSE element); Qwant and Yep are mixed or uncertain
**[verify]**. Many entries are front ends over Bing, Google or Brave (`fireball, fynd,
privacywall, reloado, zapmeta, tusksearch, vuhuv, resulthunter, gmx` and others — **[verify]
each**): ship them disabled, labelled with the *parent's* `index_group` so consensus and the
H1 note don't double-count, and measure overlap with a new `tools/overlap.py` (top-10 URL
Jaccard against each index over `tools/queries.tsv`). Enable by default only what adds a new
index.

### J5.5 Tiers

| Tier | What | Parser | Notes |
|---|---|---|---|
| 0 | the framework + one reference engine per kind | json, feed, html | proves J5.1–5.3 end to end |
| 1 | **Wikimedia family:** wikibooks, wikiquote, wikisource, wikiversity, wikivoyage, wikinews, wikispecies, wikidata, Commons (files/images/videos/audio); **MediaWiki wikis:** Arch, Gentoo, NixOS, Free Software Directory | json | one definition + a `site` parameter per wiki |
| 1 | **Reference/science:** OpenLibrary, PubMed, Europe PMC, OpenAIRE, Semantic Scholar, PDB | json/xml | NCBI wants `tool`/`email` parameters **[verify]** |
| 1 | **Code hosting:** GitHub, GitLab, Codeberg/Gitea, Bitbucket, Hugging Face (models/datasets/spaces), Docker Hub | json | GitHub unauthenticated search is tightly limited **[verify]** |
| 1 | **Packages:** npm, crates.io, Hex, MetaCPAN, Packagist, pub.dev, RubyGems, Anaconda, Alpine, Void | json | crates.io requires a real UA **[verify]**; PyPI and pkg.go.dev have no search API → html |
| 1 | **Docs/Q&A:** StackExchange family (one entry, `site=` per community: askubuntu, superuser…), MDN, Microsoft Learn, Hoogle, mankier, NVD, Lobsters | json | |
| 1 | **Media:** Deezer, Radio Browser, Mixcloud, MediathekViewWeb, SepiaSearch/PeerTube, Odysee, Dailymotion, Openverse, Art Institute of Chicago, Frinkiac, Apple App Store (iTunes Search) | json | all documented or public APIs as far as I know — **[verify]** each against its own terms before coding |
| 1 | **News:** Tagesschau, Wikinews; **maps:** Photon, Nominatim, OSM | json | maps share J4c's pacing |
| 2 | **Feeds:** Google News RSS search, Bing News RSS | feed | **[verify]** both still serve |
| 2 | **HTML, fragile (each needs a fixture and a breaker):** Brave News/Images/Videos, DDG Images/Videos/News (needs a `vqd` token), Bing Videos/News, Mojeek, Mwmbl, Wiby, searchmysite, Qwant (bot-challenged), Yandex, Baidu, Naver, Seznam, Sogou, 360, Quark, YouTube, Vimeo, Rumble, Bilibili, Niconico, Pinterest, Flickr, Imgur, Artstation, SoundCloud, Bandcamp, Genius, F-Droid, APK Mirror | html | order by value to the owner; build on request after a probe |
| 3 | **Opt-in only, off by default, owner's explicit call:** torrent indexers (1337x, Piratebay, Kickass, Nyaa, Solidtorrents, Btdigg, Bt4g, Tokyotoshokan) and shadow-library searches (Anna's Archive, Library Genesis) | html | much of this content is infringing and domains rotate. I would **not ship** Anna's/LibGen in a default build; if the owner wants any, gate them behind `[engines] allow_files = on`, set `legal_note`, and never fan out to them from the All tab |

Skip unless asked: the small regional or unknown engines in the list (`abcnyheter, bpb, ayo,
gabanza, fastbot, crowdview, 9gag…`) — I cannot judge their value without a probe, so build
them only after one.

### J5.6 Fix the existing SearXNG bridge first (cheap, and it stays useful)

- `searxng_engines` is a 128-byte buffer (`Config.h:73`, `ApiEngines.c:762`); `snprintf`
  truncates a long list silently. The owner will list 20+ names. Raise it to ≥ 1 024 and
  fail loudly on overflow.
- The attached list uses **bangs** (`!go`, `!ddg`); the `engines=` parameter takes engine
  **names**. Ship a shortcut → name map generated from the list and accept either form.
  Validate the configured names at startup against the instance's `GET /config` (it lists
  each engine's name, shortcut and categories **[verify]**) and warn on unknown or disabled ones.
- `config.ini` recommends `docker run searxng/searxng` as the route to Google, but the JSON
  output (`format=json`) must be enabled in the instance's settings or it answers 403
  **[verify] for the current image**. Add a startup probe (`/search?q=test&format=json`)
  that prints one clear error instead of a silently dead engine.
- The registry gives the bridge `index_group = "google"` (`ScrapingParsers.c:~791`), so
  results from Brave and Qwant through the bridge are counted as Google. Use each result's
  `engine` field to pick the group.
- `api_parse_searxng` reads only `title/url/content`. The JSON also carries `engine`,
  `engines`, `positions`, `thumbnail`/`img_src`, `publishedDate`, `category`, and top-level
  `answers`, `infoboxes`, `suggestions`, `corrections`, `unresponsive_engines`
  **[verify names against the instance]**. Use `unresponsive_engines` in the health panel,
  `suggestions`/`corrections` for "did you mean", and pass `categories=` for the J5.3 tabs.

### J5.7 Process

Per engine: sanitised fixture (no cookies, no IPs), a parse test in `tests/parser_fixtures`,
a `registry_lint` pass, a paced probe logged in `docs/probe-log.tsv`, an identifying
`User-Agent` where the provider's policy asks for one (Nominatim, OSM tiles, crates.io,
Wikimedia **[verify]**), and a `rate_note` giving the provider's documented limit that
feeds the token bucket. The J5 probe suite is sequential and spread out; with ~100 hosts a
burst would be the H1 mistake at scale. The `JUNK` outcome (H2) applies to web kinds only.
Per-request fan-out stays capped (6–8); the All tab never queries 100 engines.

**Done when:** Tier 0 and Tier 1 are in with fixtures; ≥ 40 engines across ≥ 8 categories;
Videos, News and Maps tabs work end to end; the registry lint passes; `docs/` has the overlap
report; no engine is enabled by default without a logged probe; SETUP §3/§4 are updated.

---

## J6. Found along the way

1. **Raw query text is logged.** `Search.c:2036-2043` writes `raw_query` into every
   `[WARN] … collapsed to zero rendered results (query "…")` line, unconditionally. The
   shipped `omnisearch.err` contains `1+1`, `covid-19`, `google_cse` and `#123456`. This breaks
   the "do not start logging raw query text" ground rule from Part 0. Drop the text (or log a
   per-install-salted hash) and keep the counts. The zip also contained `omnisearch.err.old`
   (433 KB), `cache/cookies/*.txt`, `.git`, `obj/` and `.DS_Store`/`__MACOSX`: fine for a
   hand-off, but strip them before sharing (`make dist` exists for this, A7).
2. **`{{info[2]|safe}}`** (`results.html:127`) marks every infobox body as safe, including
   Wikipedia's plain-text `explaintext` extract (which can contain `<` and `&`) and the
   Cambridge scrape. Add `InfoBox.is_html`, set only by local generators, and escape the
   rest. **[verify]** beaker's default escaping for `{{ }}`.
3. **`%g` in the calculator card** truncates to 6 digits (J4a); the answer box and the card
   disagree.
4. **Wikipedia is asked about every query** under `infobox = all` (always-true handler, fuzzy
   `generator=search`), and its relevance gate is bypassed for short queries (J3b).
5. **Three tokenizers** with three minimum lengths and three alphabets (J3a).
6. **`-` prefix = exclusion** (`QueryAnalysis.c`, the `negatives` branch): any word starting
   with `-` and longer than one character is removed from the engine query, so
   `10 -3` or `covid -19` loses its second term. **[verify]**; require the `-` to be followed
   by a letter.
7. **`answers` has no settings control** (J3c) and the default (`local`) is documented only in
   `example-config.ini` and SETUP.
8. **SETUP §3 is stale for this round:** add rows for translate, geocoder, tiles, stock
   providers and every new engine; SETUP §4 needs the new registry model.
9. **`docs/claude-feedback.md` is deleted in the working tree** (` D` in `git status`). If that
   was not intended, restore it from `HEAD`.
10. **Template warnings can be failures:** 9× `Key '…' not found in context` in the log means a
    literal `{{tag}}` shipped to a page. Make the render check fail on any such warning.

---

## Report back

For each ID (J1, J2, J3a–d, J4.0, J4a–d, J5.0–5.7, J6.1–10), one line: `ID — fixed | not
reproducible | deferred (why) — evidence (command/test/commit)`. Then list anything you
found that is not in this document, every **[verify]** you resolved with the exact request
that resolved it, and the `make verify` summary line.