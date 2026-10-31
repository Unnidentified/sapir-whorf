# Google CSE element fixtures

Captured 2026-10-31 from the protocol in Round 3 H3, with the public CX
SearXNG uses (`partner-pub-8993703457585266:4862972284`).

| File | What it is |
|---|---|
| `token-page.js` | `https://www.google.com/cse/cse.js?cx=…` followed to `cse.google.com/cse/cse.js`. 200, 3324 bytes. The trailing `({ … })` object carries `cse_token`, `cselibVersion` and `exp`. |
| `results.jsonp` | `cse/element/v1` for `linux kernel`, 200, 13094 bytes, 20 results. |
| `empty.jsonp` | same endpoint for a query with no matches, 200, 432 bytes, 0 results. |
| `error-429.jsonp` | **Synthetic.** The 429 shape is from the documented error envelope; it was not captured live (getting rate-limited on a third-party CX is not something to do deliberately). |

Request shape that worked (one request each, not a burst):

```
GET https://www.google.com/cse/cse.js?cx=<CX>
  → 301 → https://cse.google.com/cse/cse.js?cx=<CX>   (200)
GET https://cse.google.com/cse/element/v1?rsz=filtered_cse&num=20&hl=en
    &cselibv=<cselibVersion>&cx=<CX>&q=linux+kernel&safe=off
    &cse_tok=<cse_token>&callback=_&rurl=&searchtype=&exp=<exp…>
  Referer: https://cse.google.com/
```

The tokens in these fixtures are expired (they are minted per request); the
parser tests only read the shape.
