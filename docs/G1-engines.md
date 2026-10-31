# G1 — why Brave, DuckDuckGo Lite and Bing failed

Measured 2026-10-31 on the owner's machine with `tools/probe.sh` plus targeted
`curl` runs. Not guessed: every row below is a request that was actually made.

**The headline: the discriminator is the User-Agent, not TLS and not rate limits.**

## Brave — the User-Agent decides it

`search.brave.com` 302s to `safe.search.brave.com` when no SafeSearch cookie is
present, then serves either results or a JavaScript captcha depending on the UA.

| UA | HTTP | `data-type="web"` | Body |
|---|---|---|---|
| Chrome/120 Windows | **429** | 0 | 74 KB captcha shell (`challengeSet`) |
| Chrome/119 macOS | **429** | 0 | 74 KB captcha shell |
| Chrome/120 Linux | **429** | 0 | 74 KB captcha shell |
| Firefox/121 Windows | 200 | 3 | 398 KB results |
| Safari/17.2 macOS | 200 | 3 | 406 KB results |

All three Chrome UAs are refused; both non-Chrome UAs are served. This is not
load, and it is not a 429 from pacing — it is permanent for that fingerprint.

Why OmniSearch always failed: `get_engine_user_agent()` picks a UA by hashing the
engine id. Brave hashed to `UA[2]` — Chrome/120 on Linux — the exact profile that
is refused. The assignment was arbitrary, so this was chance, not design.

**Action taken:** the UA is now chosen per engine from the registry
(`user_agent` field) instead of by hash, and Brave is pinned to Safari. The rest
keep Chrome, which Bing and Yahoo accept.

## DuckDuckGo — `lite.` is worse than `html.`

| Endpoint | Headers | HTTP | Results |
|---|---|---|---|
| `lite.duckduckgo.com/lite/` | plain UA | 200 | 14 result links **once**, then 0 (shell) |
| `lite.` | Chrome / Firefox / Safari | 200 | 0 — region-selector shell |
| `html.duckduckgo.com/html/` | browser headers + `kl`/`df`/`ad`/`ah` cookies + warm jar | 200 | **11 results** |

`lite.` is inconsistent: one early probe found 14 results, every later one found
a shell. `html.` — the endpoint SearXNG uses — returns results consistently with
`Sec-Fetch-*` navigation headers, an `Accept-Language`, the DDG locale cookies and
a cookie jar. Its markup also differs (`class="result__a"`, not `result-link`).

**Action taken:** a `ddg-html` engine replaces `ddg` at the `html.` endpoint with
a parser for `result__a`/`result__snippet`, browser navigation headers and the
locale cookies. The old `lite.` parser is kept for reference.

## Bing — it was never broken

| Request | HTTP | `li class="b_algo"` |
|---|---|---|
| Chrome/120, stateless | 200 | **10** |
| Chrome/120, warm cookie jar | 200 | **10** |
| Safari/17.2 | 200 | 10 |

Bing returns ten organic results with `li.b_algo` in every configuration
tested, stateless or not. The earlier "returned search results in a format
OmniSearch could not parse" was a **measurement error on my side**:
`grep -c '<li class="b_algo"'` counts *matching lines*, and Bing's HTML is one
long line, so ten results counted as one. The parser was not at fault.

**But Bing does have a real problem, and it is worse than a parse failure.**
For a subset of queries it answers HTTP 200 with ten plausible, unrelated
results:

| Query | Titles | Relevant |
|---|---|---|
| `linux kernel` | 10 | 10 |
| `weather madrid` | 10 | 10 |
| `rust programming language` | 10 | 10 |
| `mechanical keyboard` | 10 | **0** — SAP warehouse errors |
| `python asyncio tutorial` | 10 | **0** |

Same for Chrome, Safari and `curl/8.7.1` UAs, with and without a cookie jar. The
junk is query-dependent and stable, which is the signature of a soft block that
serves a de-personalised or poisoned SERP rather than refusing outright. No
status-code check can see it.

**Action taken:**
- the relevance floor already drops these (measured: `raw=10 floored=0`), which
  is the correct outcome — the results are junk, not answers;
- Bing's genuine "no results" shell (`b_no`, no `b_algo`) is now classified
  `EMPTY` instead of `PARSE_MISMATCH`, so a real no-result no longer reads as a
  parser bug;
- the empty page now says *which* of the three it was. "The engines may be
  busy" was being shown when the engines had answered perfectly well and every
  hit had been judged irrelevant. It now distinguishes `filtered` (rephrase),
  `blocked` (wait), and `unavailable` (network).

Bing therefore stays enabled but is not trustworthy on its own from this host;
that is what the A2 quality signal is for.

## Yahoo

307 to a bot-check gif that sets `YBV`, then back to the search. Only a cookie
jar completes the round trip. Already the case since Round 1; unchanged.

## What this means for the fingerprint question

The review asked whether a browser-like TLS/HTTP fingerprint is what flips these
engines. On this machine the answer is **no**: the same system libcurl, the same
HTTP/1.1, the same headers, and only the User-Agent changed — and that alone
flips Brave from a captcha to results. `curl-impersonate` is not installed here
(`which curl-impersonate` finds nothing), so the TLS-fingerprint dimension was
not tested and is not needed for these three engines.

## Terms

Choosing a UA that an engine happens to accept is still evading the engine's bot
detection, and that is against their terms. It is the owner's call. It is
recorded here and in `config.ini` so it is a decision, not an accident.
