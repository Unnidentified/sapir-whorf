# K5: Reddit results — what was measured, what is built

Every row below was measured from this machine on **2026-11-01**, one request per
host, paced (see `docs/probe-log.tsv`, rows tagged `round 5 K5`). Nothing here is
copied from a vendor note.

## The routes, as they answered today

| Route | Result | Reading |
|---|---|---|
| `www.reddit.com/search.json?q=…` | **403**, 190 KB of HTML | Blocked. The body is a page, not JSON, so a JSON parse would have failed silently — check the content type, not just the status |
| `old.reddit.com/search.json?q=…` | **302** → `/login/?reason=lor2` | Blocked too. A redirect to login is a block, not a result |
| `api.pullpush.io/reddit/search/submission/?q=…` | **522** | Cloudflare could not reach the origin. Archive service is down today |
| `arctic-shift.photon-reddit.com/api/posts/search?subreddit=rust&limit=2` | **200**, 2 posts | Works: the archive's *listing* endpoint answers |
| `…?subreddit=all&query=rust ide` | 200, `error: null`, **0 posts** | `all` is not a subreddit; a silent empty answer |
| `…?subreddit=rust&query=ide` | 200, `{"error":"Timeout. Maybe slow down a bit"}` | The *text search* is throttled hard — and it says so **inside a 200**. A provider that reads only the status records this as "no results" |
| `…?query=rust ide` (no subreddit) | 200, `error: "query parameter requires one of: author, subreddit"` | There is no whole-of-Reddit text search on this API |

Two lessons are worth keeping in the code, and both are now in it:

1. **A 200 with an `error` field is a failure.** Arctic Shift returns one.
2. **A 302 to a login page is a block.** So is a 403 whose body is HTML.

## What is built (rung 1), and why

The lane asks two `site:`-capable engines — DuckDuckGo and Yahoo, in one batch —
for `site:reddit.com <terms>`, or `site:r/<sub> <terms>` when the query names a
subreddit. It needs no Reddit access at all, which is the point: it works
whatever Reddit does to its own endpoints.

- It runs for a discussion-shaped query, the word `reddit`, an `r/<sub>` token,
  or a typed `site:reddit.com` operator, on page 1 only.
- It is gated by its own provider switch (`off=reddit`), not by "no engines
  chosen". Before this round, saving *any* setting turned it off for good.
- Two engines, not one: DuckDuckGo is the most rate-limited engine in the set,
  and the two measurements on record disagree about who honours the operator
  (`tools/queries-reddit.tsv` says Yahoo; a later probe says DuckDuckGo). Asking
  both costs one extra request in a batch that is already running and removes
  the guess.
- Live check: `best rust ide reddit` returns `r/rust` threads.

## What is not built, and why

| Rung | State | Why |
|---|---|---|
| 2. query-variant ladder | not built | Rung 1 already returns threads; variants would be extra requests per query for a small gain |
| 3. archive search | not usable as a general rung | Measured above: no whole-of-Reddit search, and text search is throttled. The *listing* endpoint works, so it can serve `r/<sub>` queries — worth adding as an opt-in provider, not as a general rung |
| 4. Google CSE / SearXNG | config-gated, off | Needs credentials or a bridge instance the owner has not configured |
| 5. Reddit OAuth | **built (round 6 L5)** | This row used to say "not built, ~80 lines, not worth it" because the owner had no key. The owner has one, and it is now the *first* rung rather than a fallback: installed-app token, `/search` through `oauth.reddit.com`, real scores and comment counts. Without credentials it makes no request at all and the two-engine lane below carries the query. `tests/test_l5_reddit.sh`, 22 checks against a local stand-in. |
| 6. Reddit RSS | not built, and now unnecessary | Reported to end 2026-11-13. With L5 built there is nothing this rung would add |
| — Redlib | skipped | It depends on Reddit access itself |

## The canary (next step)

`tests/test_k5_reddit_lane.sh` proves the lane offline. What is missing is the
live canary the review asks for: once every six hours, one fixed discussion query
through the ladder, writing the winning rung — or `none` — into the engine-status
table and `docs/probe-log.tsv`. The pieces exist (the engine-status table, the
probe log, the paced-probe rule); what is missing is the scheduled job.
