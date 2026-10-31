# sp-engine / omnisearch — setup and architecture handoff

Written 2026-10-30 against commit `26067a6` (+ the Brave registry fix in the same tree).
Purpose: give a fresh reader (or another model) enough of the real picture to give
useful advice — including the parts that are odd, brittle, or half-finished.

---

## 1. What it is

A self-hosted metasearch engine in C (~11k lines) that fans a query out to several
upstream sources, fuses and re-ranks the results, and renders server-side HTML.
No database, no JavaScript framework, no build system beyond a plain Makefile.

- **Language / runtime:** C11, POSIX threads, one process.
- **HTTP server:** [beaker](https://git.bwaaa.space/beaker/) — routing, cookies,
  templating (`{{...}}`), sessions. Not a general web framework.
- **HTTP client:** libcurl multi interface (one handle per engine, all in flight).
- **HTML parsing:** libxml2 XPath. **JSON:** jansson. **TLS:** OpenSSL.
- **Frontend:** server-rendered templates + ~2.2k lines of vanilla CSS/JS
  (autocomplete, settings, themes, zoom, image grid).

Deployment on this machine: launchd agent `com.omnisearch`, binary at
`~/.omnisearch/bin/omnisearch`, listening on `0.0.0.0:8087`, logs to
`~/.omnisearch/omnisearch.{log,err}`.

---

## 2. Layout

```
src/
  Main.c                 startup: config, engines allowlist, cache, proxy, beaker serve
  Config.[ch]            config.ini parser (fixed option table, typed)
  Routes/                Home, Search, Images, Settings(+Save), History, Info, ImageProxy
  Query/QueryAnalysis.[ch]  operator parsing, intent classification, engine query text
  Scraping/              Scraping (curl multi, cache, cookies, deadlines), ScrapingParsers
                         (per-engine XPath parsers + registry), ApiEngines (JSON APIs),
                         ImageScraping, EngineHealth (circuit breaker), ScrapingHttp
  Ranking/Ranker.[ch]    dedupe, RRF fusion, BM25, authority, spam, coverage, reddit boost
  Infobox/               Wikipedia, calculator, unit/currency/colour, dictionary
  Cache/Cache.[ch]       file-backed key/value cache with TTL
  Limiter/RateLimit.c    inbound per-client limiter (disabled by default)
  Proxy/Proxy.c          outbound proxy list support (configured, currently unset)
  Utility/               strings, display URLs, JSON/XML helpers, HTML entity decoding
templates/               6 pages: home, results, images, settings, history, _chrome
static/                  main.css, autocomplete.js, settings.js, history.js,
                         authority.tsv (domain tiers), spam_hosts.txt, themes/, icons
locales/                 14 .ini translation files (en_gb default)
tools/                   smoke.sh, eval.py + queries.tsv, check_themes.py, rotate-logs.sh
docs/                    this file
```

Build: `make` → `bin/omnisearch`. `make check` (theme integrity), `make smoke`
(22 endpoint checks against a running instance), `make eval` (quality/latency
benchmark), `make install-{launchd,systemd,openrc,runit,dinit}`.

---

## 3. Configuration (`config.ini`)

Parsed against a fixed table in `src/Config.c`; unknown keys are ignored silently.
Defaults in `src/Config.h`.

| Section | Key | Default | Meaning |
|---|---|---|---|
| server | host / port | 0.0.0.0 / 5000 | listen address (this install: 8087) |
| server | locale | en_gb | default UI + result language |
| proxy | proxy / list_file | empty | outbound proxy or proxy pool |
| proxy | max_retries | 3 | retries when a proxy is configured |
| cache | dir | /tmp/omnisearch_cache | cache root (this install: ~/.omnisearch/cache) |
| cache | ttl_search / ttl_infobox / ttl_image | 3600 / 86400 / 604800 | seconds |
| engines | engines | all | allowlist, supports `*` and `-id` |
| engines | yacy_instance | 127.0.0.1:8090 | YaCy peer |
| engines | google_cse_key / google_cse_cx | empty | Google JSON API. Existing keys only: Google stopped issuing new ones in 2026-01 and the service ends 2027-01-01 |
| engines | google_cse_cx / google_cse_tok | empty | `google_cse_element`, clean-room and **disabled by default**. Needs a pasted `cse_tok`; the endpoint 403s without one. See docs/G3-google-cse.md |
| engines | searxng_url / searxng_engines | empty / google | enables SearXNG bridge |
| rate_limit | search_requests / search_interval | 0 / 0 | inbound limit per client; 0 disables |
| rate_limit | trusted_proxies | empty | addresses whose X-Forwarded-For may be believed (`loopback`, `lan`, or a list); empty trusts nobody |
| rate_limit | images_requests / images_interval | 0 / 0 | same for images |
| health | breaker_threshold / cooldown_seconds | 5 / 900 | circuit breaker |
| health | pace_requests_per_minute / pace_burst | 40 / 6 | per-engine outbound token bucket |

Note: `[ranking]` used to exist in `config.ini` but **nothing parsed it** —
ranking weights are compile-time constants in `src/Ranking/Ranker.c`. It has
been removed; unknown keys now report `config.ini:LINE` and `--check-config`
exits non-zero.

### Where a query actually goes

This is worth stating plainly, because it is more than "the six engines":

| Destination | When | Can be turned off |
|---|---|---|
| up to 6 search engines | every search | yes (`engines=` allowlist) |
| Yahoo, `site:reddit.com` | discussion-shaped queries | yes (`infobox`/lane) |
| Wikipedia API | every query (the infobox `always_true` handler) | yes |
| Cambridge Dictionary | looks like a dictionary query | yes |
| exchange-rate / crypto APIs | looks like a currency query | yes |
| thumbnail hosts (via `/proxy`) | every image search | yes |

`[server] infobox = all | local | off` controls the middle three: `local` keeps
the calculator, unit and colour boxes (which compute locally) and sends nothing
off the machine; `off` disables infoboxes entirely.

---

## 4. Engines

`ENGINE_REGISTRY` in `src/Scraping/ScrapingParsers.c` is the single source of truth.
`verticals` decides which intent a source serves; `weight` is used both for routing
priority and for the batch's "trusted source" test.

| id | verticals | weight | enabled | notes |
|---|---|---|---|---|
| yahoo | web | 1.20 | yes | needs a cookie jar: 307 → bot-check gif sets `YBV` |
| brave | web | 1.15 | yes | 429s under load; keeps cookies |
| bing | web | 0.95 | yes | **no locale params** (setlang/cc/mkt → empty shell); stateless |
| ddg | web | 1.00 | yes | lite endpoint; 202 challenge when hammered |
| google | web | 1.30 | no | CSE JSON API only; needs key + cx |
| searxng | web | 1.25 | no | self-hosted bridge, relays Google etc. |
| openalex | academic | 1.30 | yes | JSON API |
| crossref | academic | 1.00 | yes | JSON API |
| arxiv | academic | 1.00 | yes | Atom XML |
| stackexchange | code | 1.20 | yes | JSON API |
| hn | code+news | 0.80 | yes | Algolia JSON |
| wikipedia | entity | 1.20 | yes | MediaWiki API; also feeds the infobox |
| yacy | web | 0.60 | no | needs a peer |

`index_group` groups engines that answer from the same index (Bing, Yahoo and
DuckDuckGo are `bing`). Consensus and the batch's early exit count groups, not
engines, so one index's failure cannot look like three engines agreeing.

Rules applied to the registry at request time:

- `engines="*"` enables everything except yacy; `-id` removes.
- Google/SearXNG are force-disabled at startup unless credentials exist.
- Health breaker filters engines; a floor keeps at least 3 **healthy** web engines
  in play, and ≥1 web engine is forced in even if all are open (probe).

---

## 5. Request pipeline (GET /search?q=…)

```
beaker route
  └─ inbound rate limit (per client, off by default)
  └─ request cache lookup (client+query+page+engine, TTL 1 h) → hit: render
  └─ query analysis
       ├─ operators: site:, filetype:, "phrase", -term
       ├─ tokens, stopwords, phrases
       ├─ intent: general | code | academic | entity | news | media (keyword tables)
       ├─ language detection (locale)
       └─ engine_core: clean query minus opinion fillers (best/cheap/top/…) when
          ≥2 topic words remain — "best podcast app" searches as "podcast app"
  └─ engine selection
       ├─ user allowlist + selected engine (engine= param)
       ├─ breaker filter + healthy web floor
       ├─ vertical routing: intent picks V_CODE/V_ACADEMIC/V_ENTITY/V_NEWS first,
       │  then general web engines fill up to 6 slots
       └─ site: queries are locked to web engines that honour the operator
  └─ parallel scrape (curl multi, one handle per engine)
       ├─ per-engine URL build (page params, locale params, site/negatives)
       ├─ UA: stable per engine (hash), not random per request
       ├─ cookies: per-engine jar, except bing/ddg/mojeek (stateless)
       ├─ engine HTML cache (TTL 1 h) + last-known-good stale cache (7 days)
       ├─ batch rules: stop at 2 trusted sources w/ 8 results @500 ms,
       │  1 trusted @550 ms, 2 any @1200 ms, hard cap 2000 ms
       ├─ uncut engines get SCRAPE_STATUS_CANCELLED (not a failure, no warning)
       └─ one retry only for single-engine requests that came back empty
  └─ parse per engine (XPath for HTML, jansson for APIs)
       ├─ blocked-page detector, layout fallbacks (Bing b_algo → any h2 result)
       └─ classify: OK / EMPTY / FETCH_ERROR / PARSE_MISMATCH / BLOCKED / CANCELLED
  └─ rank (src/Ranking/Ranker.c)
       ├─ canonical URL dedupe + syndication collapse
       ├─ RRF (k=60) over per-engine ranks
       ├─ score = 0.50·RRF + 0.18·BM25 + 0.12·authority + 0.06·phrase
       │        + 0.05·consensus + 0.04·url_quality + 0.03·freshness
       │        − 0.15·spam + 0.22·reddit_relevance − 0.65·(1−coverage)
       ├─ coverage = IDF-weighted share of query terms found in title/url/snippet;
       │   terms no candidate contains are ignored; one-edit typos still match
       └─ relevance floor: coverage < 0.15 → dropped (nonsense query → empty state)
  └─ per-domain cap (3), seen-on-previous-page results sink (−2.0) not disappear
  └─ infobox (parallel threads): dictionary, calculator, units, currency, colour,
     Wikipedia — only rendered when a handler matches and the query is relevant
  └─ render results.html, write request cache, log ACCESS line
```

Pagination: `p=<n>`; cross-page dedupe stores seen canonical URLs for 10 minutes
and sinks repeats rather than deleting them, so page 2 stays full.

Images: `/images` scrapes Bing Images only (separate parser/cache, TTL 7 days) and
proxies thumbnails through `/image-proxy`.

---

## 6. Caching and state (all under `cache/`)

| What | Key | TTL |
|---|---|---|
| Rendered request results | client+query+page+engine | 1 h |
| Per-engine raw HTML/JSON | engine+query+page | 1 h |
| Last-known-good per engine | same + `|stale` | 7 days |
| Previously seen URLs (pagination) | client+query | 10 min |
| Cookie jars | `cache/cookies/<engine>.txt` | until purged |
| Breaker state | `cache/engine_health.tsv` | persisted, 10 columns |
| Infobox answers | query | 24 h |

Cookie jars are deleted on 429 / 202 / 5xx / blocked page / a 200 with zero web
results — a poisoned session is the single most common cause of "engine silently
returns nothing".

---

## 7. Health breaker and rate limits

`EngineHealth.c`: per-engine circuit breaker.

- Every pause has a **kind**, and the kind decides the reaction. The first
  strike is enough for a rate limit or a challenge; transport and parse
  failures need evidence.

  | Kind | Trigger | First strike | Evidence |
  |---|---|---|---|
  | `rate-limited` | 429 / 403 / 202 / 503 | `Retry-After`, clamped to 30 s–1 h (60 s if absent) | 1 |
  | `challenged` | captcha / interstitial shell | 900 s | 1 |
  | `unparseable` | `PARSE_MISMATCH` | 300 s | `breaker_threshold` |
  | `unreachable` | `FETCH_ERROR` | 120 s | 2 × `breaker_threshold` |

- A local outage that fails the whole batch the same way is not charged to any
  engine (`OMNI_NOBREAKER` also exists for evaluation runs).
- After the first strike, repeated trips double the wait (max 4 shifts → ≤6 h)
  with ±20 % jitter, and never less than an upstream `Retry-After`.
- One success closes the breaker and clears the reason.
- **Outbound pacing** (`pace_requests_per_minute` 40, `pace_burst` 6): a token
  bucket per engine, spent before a request leaves and refilled continuously.
  A cache hit costs no token. An engine out of allowance is `SKIPPED` for that
  batch — not charged, because the pause is our own doing.
- `/info` exposes state, the suspend reason, seconds left, and the counters.
  `/settings` shows the same as a table: state, why, when it retries, and the
  quality percentage.
- A paused engine is named on the results page ("1 source paused
  (rate-limited); earliest retry in 0m 42s."), including on empty pages, where
  the empty hint defers to the pause instead of blaming the network.
- `CANCELLED` (our own deadline) never counts.

Inbound limiting (`Limiter/RateLimit.c`) exists but is off by default
(`research_requests=0`). It is a fixed window per client key.

---

## 8. Ranking signals (Ranker.c)

| Signal | Weight | Source |
|---|---|---|
| RRF | 0.50 | rank position per engine, k=60 |
| BM25 | 0.18 | title×3, snippet×1, url×0.5, IDF over candidates |
| authority | 0.12 | `static/authority.tsv` tier for the registrable domain |
| phrase | 0.06 | quoted phrases in title/snippet |
| consensus | 0.05 | number of engines returning the URL |
| url_quality | 0.04 | depth/params/host heuristics |
| freshness | 0.03 | date fragments in snippet |
| spam | −0.15 | `static/spam_hosts.txt` + keyword heuristics |
| reddit | +0.22 | reddit.com only, and only if query terms appear in title/url |
| coverage | −0.65·(1−coverage) | IDF-weighted term coverage, typo-tolerant |

`MAX_PER_DOMAIN=3`, `MAX_CANDIDATES=128`, `MAX_RESULTS_PER_ENGINE=10`.

---

## 9. Tests and tooling

- `make check` → `tools/check_themes.py`: every theme defines every palette slot.
- `make smoke` → `tools/smoke.sh`: 22 checks (status codes, tab title, opensearch
  link, engine health in `/info`, settings round-trip).
- `make eval` → `tools/eval.py` + `tools/queries.tsv`: zero-result rate, unique
  sites, title coverage, duplicate rate, authoritative hits, latency percentiles.
  Thresholds live in `eval.py`; `--strict` exits non-zero.
- `tools/rotate-logs.sh`: truncates/rotates `omnisearch.log` (not scheduled).
- Debug env vars: `OMNI_PERF=1` (per-engine timings, routing, URLs),
  `OMNI_DUMP=<engine>` (dump that engine's raw response to `/tmp`),
  `OMNI_LEGACY=1` (restore the pre-optimisation batch policy, for A/B runs).

---

## 10. Things that are odd, brittle, or worth a second opinion

Ordered roughly by how much they cost you.

1. **Upstream quirks are encoded as code, not data.** "Bing must not receive
   locale params", "Yahoo needs a cookie jar", "force HTTP/1.1" are all discovered
   behaviour, documented only in comments. Any of them can silently invert.
2. **Junk that parses is invisible to health.** When Bing answers `best podcast
   app` with Spanish hotel brands, the engine is healthy: 200, 10 results, no
   warning. Only the coverage floor saves the page. There is no per-engine
   "result quality" signal, so a source can rot for weeks.
3. **The early-exit trusts a magic weight.** `weight >= 1.1` means "trusted" for
   the batch deadline. Brave sat at 1.0 with `supports_site=0` for several
   revisions (fixed in this tree) — nothing tests the registry, so drift is silent.
4. **Ranking weights are hand-tuned constants** with no offline evaluation. The
   eval harness measures coverage/dupes/latency, not ranking quality (no nDCG,
   no judged set). `[ranking]` in config.ini is dead weight.
5. **Coverage math has sharp edges.** It uses the candidate set's own IDF, so its
   behaviour changes with which engines answered; the 0.15 floor and the
   one-edit tolerance are empirical. It is the single most fragile part of
   relevance.
6. **Intent detection is English keyword tables.** Non-English queries almost
   always land in `general`, so verticals (arXiv, StackExchange) never fire.
7. **Opinion-filler stripping is English-only** (`best/cheap/top/…`). `mejor`,
   `beste`, `moins cher` are searched literally, with the upstream brand-junk
   failure mode that motivated the strip.
8. **No spelling correction and no query expansion.** Typo recovery relies on the
   upstream engine plus the coverage floor's edit tolerance.
9. **No semantic layer.** RRF + BM25 + handcrafted signals. Adding embeddings
   would mean a model runtime and a vector store — a real architectural change,
   not a tweak.
10. **Per-query fan-out is up to 6 engines + 6 infobox threads**, capped at 2 s.
    There is no streaming: the page is fully computed before first byte.
11. **Cache is file-per-key, per client.** Two clients searching the same term
    don't share the request cache (the key includes the client key); only the
    per-engine HTML cache is shared.
12. **Security posture:** binds `0.0.0.0`, no auth, `/info` exposes version, git
    remote and engine health, settings are cookie-driven. Fine on a trusted LAN,
    not safe to expose.
13. **Observability is freeform stderr.** No request ids, no metrics endpoint, no
    structured logs; perf data only behind `OMNI_PERF`. The breaker counters are
    the closest thing to telemetry.
14. **i18n is partial.** Settings/UI strings are translated (14 locales), but the
    results template hardcodes English ("No results found…", "Some sources did
    not answer"). Locale also drives upstream language params, so a mismatch is
    user-visible twice.
15. **beaker's array-of-arrays setter crashes on `count == 0` / NULL rows.** This
    bit twice; every new matrix in a handler needs a `count > 0` guard.
16. **Branding is inconsistent:** tab title `sw`, wordmark `sapir-whorf`, binary
    `omnisearch`, version suffix `_sapir-whorf`, repo `sp-engine`.
17. **Proxy support exists and is unused.** `proxy`/`list_file` are plumbed into
    every curl handle. It is the intended answer to per-IP rate limits, but it is
    a pool of your own egress points — not a way around an upstream's terms.
18. **Images only use Bing**, so image search has the same single point of
    failure as the web engine mix once had.
19. **`make eval` needs a live server** and its thresholds are constants inside
    the script; there is no CI, no unit tests, no fixture-based parser tests.
    Parser drift is caught by users, not by the build.

---

## 11. If you are handing this to another model

Useful context bundle, in order of density:

1. this file;
2. `src/Scraping/ScrapingParsers.c` (engine registry + parsers — where upstream
   reality lives);
3. `src/Scraping/Scraping.c` (batch policy, cache, cookies, deadlines);
4. `src/Ranking/Ranker.c` (the whole relevance model in one file);
5. `src/Query/QueryAnalysis.c` (operators, intents, filler stripping);
6. `src/Routes/Search.c` (orchestration, routing, warnings, matrices);
7. `config.ini` and `tools/queries.tsv` (what is configured and what "good" means).

Questions worth asking that model, given the above: how to replace the hand-tuned
score with something measurable; whether a per-engine quality metric can catch
"healthy but junk"; how to make upstream quirks config-driven; and what a minimal
offline relevance benchmark would look like for a project with no judged set.
