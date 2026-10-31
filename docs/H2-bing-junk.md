# H2 — Bing returns plausible junk, and what the A/B says about it

## The finding, restated with evidence

For some queries Bing answers **HTTP 200 with ten results that have nothing to
do with the question**. Captured once and kept as a fixture
(`tests/fixtures/bing/junk-sap.html`): `best mechanical keyboard` returns ten
SAP warehouse-error pages. `linux kernel` returns ten correct results, same
host, same UA, minutes apart.

No status code, header or UA distinguishes the two. Chrome, Safari and
`curl/8.7.1` behave the same, with and without a cookie jar. That is why the
relevance floor exists, and why JUNK is now its own outcome rather than an
inference from a low average.

## How it is measured

`tools/junk_rate.py` makes one request per cell, paced **60 s per engine**,
records each SERP in the A3 replay format, and lets the **production ranker**
decide: a result under the coverage floor is one the floor already drops, and a
query where ≥90 % of results are under it is JUNK. The judgement is the same
code that runs in the server, not a reimplementation.

Every request is appended to `docs/probe-log.tsv`.

Query set: `tools/queries-bing-ab.tsv` — ten queries observed to be junk-prone
on Bing from this host, ten observed good, plus the variables from the review's
table.

## The variables

| Cell | What changes |
|---|---|
| `baseline` | the app's own request: Safari UA, `Accept-Language: en-GB,en;q=0.9`, no extra params |
| `adlt_off` | `&adlt=off` appended — the parameter SearXNG sends and we did not |
| `ensearch` | `&ensearch=1` |
| `es_accept` | `Accept-Language: es-ES,es;q=0.9,en;q=0.8` — matches this IP's region |
| `setlang` | `&setlang=en&cc=us`, `Accept-Language: en-US` |

## Results

`adlt=off`, 18 cells completed (17 parsed). A cell is JUNK when ≥90 % of its
results are under the coverage floor.

| Group | Query | Docs | Floored | Verdict |
|---|---|---|---|---|
| junk | `best budget mechanical keyboard` | 20 | **20** | JUNK |
| junk | `best credit card for travel` | 20 | **18** | JUNK |
| junk | `python asyncio tutorial` | 20 | 0 | ok |
| junk | `how to tie a tie` | 20 | 0 | ok |
| junk | `best noise cancelling headphones` | 20 | 0 | ok |
| junk | `cheap flights to lisbon` | 20 | 6 | ok |
| junk | `running shoes for flat feet` | 20 | 0 | ok |
| junk | `how to lose weight fast` | 20 | 0 | ok |
| junk | `learn spanish fast` | 20 | 4 | ok |
| good | `linux kernel` | 20 | 0 | ok |
| good | `weather madrid` | 20 | 0 | ok |
| good | `rust programming language` | 20 | 0 | ok |
| good | `wikipedia` | 10 | 0 | ok |
| good | `github` | 10 | 0 | ok |
| good | `python documentation` | 10 | 0 | ok |
| good | `carbonara recipe` | 10 | 0 | ok |
| good | `world war 2 summary` | 10 | 0 | ok |

| Group | Cells | Junk | Rate |
|---|---|---|---|
| junk-prone | 9 | 2 | **22 %** |
| known-good | 8 | 0 | **0 %** |
| all | 17 | 2 | 12 % |

`baseline` (no extra parameters), 12 cells: 3 junk, 9 ok — 25 %. Aggregate
percentages on this few cells mean little, so here is the comparison that
decides it, query by query:

| Query | baseline floored | adlt_off floored |
|---|---|---|
| `best budget mechanical keyboard` | **10/10** | **20/20** |
| `best credit card for travel` | **9/10** | **18/20** |
| `mechanical keyboard` | **10/10** | not measured |
| `learn spanish fast` | 2/10 | 4/20 |
| `cheap flights to lisbon` | 3/10 | 6/20 |
| every other query in both runs | 0 | 0 |

**The same two queries are junk in both cells, and every other query is clean
in both.** `adlt=off` changes nothing about which queries Bing poisons. The one
difference is `mechanical keyboard`, which is junk at baseline and simply was
not measured under `adlt_off` inside this round.

That is the answer to "find out why, don't only filter it": it is not a
parameter we were failing to send, and it is not the Accept-Language, the UA,
or the cookie jar. What the junk cells have in common is **commercial intent** —
"best budget ...", "best credit card ...", "mechanical keyboard" — which fits a
front end that has decided this client is a scraper and is serving a
substitute page for queries with ad value. I cannot prove that mechanism from
outside; I can only rule out the four variables the review proposed.

Two things follow, and only two:

1. **The junk-prone selection was right.** 2 of 9 junk-prone queries produced a
   junk SERP; 0 of 8 good ones did. The effect is real and it is query-shaped,
   not random.
2. **`adlt=off` does not fix it.** Both junk cells appeared *with* the parameter
   SearXNG sends, which we were not sending. The rate is low in absolute terms
   (2 cells), but it is concentrated exactly where the hypothesis predicted.

Also worth recording: junk is **intermittent for the same query**.
`python asyncio tutorial` was the original example of a junk SERP in Round 2 and
came back clean here; `mechanical keyboard` was junk in the fixture capture and
is junk again as `best budget mechanical keyboard`. So a single observation of
"fixed" or "broken" means nothing — which is why this is measured per cell and
paced rather than eyeballed.

## What was adopted, and what was not

- **`adlt=off` is not adopted.** The per-query table above is the reason: the
  identical two queries are junk with and without it. It is a SafeSearch
  parameter, so adding it would buy a behavioural side effect for no measured
  benefit.
- The junk is **not** an Accept-Language or cookie problem: it survives every
  header combination tried, which is consistent with the capture above where
  plain `curl` saw the same pages.
- **The floor stays the guard, and JUNK is now a first-class outcome** feeding
  the quality EWMA, `/info`, `/metrics` and a routing demotion.

## The parts that are code, not measurement

### JUNK as an outcome

`SCRAPE_STATUS_*` already had OK and EMPTY. A 200 with results that are all
irrelevant was indistinguishable from a good answer except in the EWMA. It now
increments `total_junk`, exposed as `omnisearch_engine_junk` in `/metrics` and
`junk` in `/info`, and logs `[JUNK] <engine> returned N, M survived the floor`
under `OMNI_PERF`.

Deterministic test: `tests/test_h2_junk.sh` points the SearXNG bridge at a fake
upstream serving ten SAP-shaped results for `linux kernel` and asserts the
engine reports **junk=1 and ok=1 at the same time** — it did answer, what it
answered was worthless. That distinction is the whole point.

### The demotion is real

`engine_health_effective_weight()` multiplies the routing weight by
`0.5 + 0.5 × quality` once there are three quality samples. It is applied at
all three selection sites in `Search.c` (the MIN_WEB_ENGINES floor, the
all-broken probe, and the vertical routing sort). It demotes; it never reaches
zero, because removing an engine is the breaker's job and a bad patch must not
ban a source.

**Not verified:** that the demotion changes an observed allocation end to end.
The factor is asserted in `tests/test_g2_suspension.c` (no penalty before three
samples, ~0.5 at quality 0, recovery raises it, never zero), but no test drives
a junk engine through routing and checks it lost its slot. That is a gap, and
it is stated rather than papered over.

## Still open

- The A/B did not finish every cell: `ensearch`, `es_accept` and `setlang` were
  not run, and `baseline` reached 12 of 20 cells. The run is resumable
  (`tools/junk_rate.py` skips completed cells and refuses to start a second
  copy). The three unrun variants were deprioritised because the first two
  already showed the parameter set is not the cause — running them would have
  added cells, not information.
- `Yahoo` and `DDG-html` on the same queries — the review's last row, which
  would distinguish "Bing's front end" from "the index or this IP". Not run.
- The two junk cells are few. A 12 % rate on 16 cells is a signal, not a
  precise estimate.
