# G3 / H3 — the Google CSE element, implemented properly

**Round 2 got this wrong and said so in writing.** The first version of this
document claimed the `cse_tok` "can only be minted by Google's obfuscated widget
JavaScript" and therefore had to be pasted by hand. That was not true. The
Round-2 probe requested `cse.google.com/cse.js` — missing the `/cse/` path
segment — got a 404, and the conclusion was drawn from a request that never
reached the endpoint.

Round 3 (H3) named the real protocol. This document now records what that
protocol actually returns.

## The exact requests, and what they return

One request each; see `docs/probe-log.tsv`.

**Step 1 — the token page.**

```
GET https://www.google.com/cse/cse.js?cx=partner-pub-8993703457585266:4862972284
  UA:     Safari/17.2 macOS
  Accept: */*
  Referer: https://cse.google.com/
→ 301, 273 bytes, Location: https://cse.google.com/cse/cse.js?cx=…
→ 200, 3324 bytes gzip (9053 inflated)
```

The response ends in a JS object that carries the token:

```js
"cse_token": "AHbIdTgr29BfRdqookhwubsNdtaY:1793449170388",
"exp": ["cc", "sps", "esbfa"],
"cselibVersion": "dc6f600a0d4404e5",
```

No JavaScript is executed and none needs to be. The values are strings in the
response body. The Round-2 claim was simply wrong.

**Step 2 — the results.**

```
GET https://cse.google.com/cse/element/v1
     ?rsz=filtered_cse&num=20&hl=en
     &cselibv=<cselibVersion>&cx=<CX>&q=linux+kernel&safe=off
     &cse_tok=<cse_token>&callback=_&rurl=&searchtype=&exp=<exp…>
  Referer: https://cse.google.com/
→ 200, 13094 bytes, 20 results
```

The body is JSONP: a `/*O_o*/` line, then `_({ … })`. Results are under
`results[]` with `unescapedUrl`, `titleNoFormatting`, `contentNoFormatting`.

An empty query answer is a 432-byte JSONP object with no `results` array. A 429
arrives as `{"error":{"code":429,…}}`. Fixtures for all four shapes are in
`tests/fixtures/gcse/`; the 429 one is synthetic and labelled as such.

## What was built

`google_cse_element`, clean-room from the protocol above — no SearXNG code
copied or translated (SearXNG is AGPL-3.0; this project is GPL-2.0).

- **Automatic token fetch**, cached for one hour, under a mutex so a burst of
  searches mints one token rather than one per request. A failed fetch is not
  retried for 60 s, and the last good token keeps being used.
- **`google_cse_tok` is an optional override**, not a requirement. It exists for
  the case where the token endpoint changes.
- **JSONP parser** reading `results[]`, with entity unescaping and entries
  without a link dropped.
- **Its own small bucket: 10 requests/min**, separate from the global pacing,
  because it borrows another site's public CX.
- **Disabled by default** and enabled only when `google_cse_cx` is set.
- The token is redacted from the `[URL]` diagnostic log the same way API keys
  are (`cse_tok=REDACTED`).

Measured after the change: two searches in one process produced **one** token
fetch and two results requests; a rendering search returned 10 results.

## Licence, terms, fragility

- The endpoint is undocumented and uses a third-party CX, so it can change or be
  cut without notice and sits outside the Programmable Search terms. That is why
  it ships disabled and is described as experimental. Enabling it is the
  owner's call.
- Whether the endpoint survives the JSON API shutdown on 2027-01-01 is unknown.
  Still open; check in January.
- `searxng_url` remains the documented lower-risk route to Google's index.

## What is still not verified

- The 429 path was not triggered deliberately (getting rate-limited on someone
  else's CX is not a good test). The parse path is covered by a synthetic
  fixture; the suspension is covered by `tests/test_g2_suspension.c`.
- The endpoint was exercised with one query, twice, not under load.
