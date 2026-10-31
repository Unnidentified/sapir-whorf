# H1 — how many independent indexes actually answer, and what to do about it

## The finding, confirmed from state rather than from reading

`cache/engine_health.tsv` at the time of the review:

```
ddg     10 requests  10 ok
bing    11 requests  10 ok
yahoo   12 requests  11 ok
brave    2 requests   0 ok    state=open  trips=2  reason=rate-limited
```

`ddg`, `bing` and `yahoo` all carry `index_group = "bing"`. Yahoo's web results
are largely Bing's and DuckDuckGo's are partly so. **One index answered
everything.** G2c's hedging was correct and starved: there was nothing
independent left to hedge to. That is the real shape of "the engines zone out".

## What was built

### Brave: a UA ladder instead of one pinned profile

The Round-2 fix pinned Brave to Safari and it worked in that probe. It then
stopped working — which is what a single hard-coded fingerprint buys you.

Now each engine has a ladder, ordered by what was measured (Safari and Firefox
served, every Chrome UA refused): `safari → firefox → chrome`.

- on a challenge page the job is retried **once**, blocking, with the next
  profile, before the failure is recorded — so a cleared challenge counts as a
  success, not a strike;
- the profile that worked is written to the health file, so a restart resumes
  where it last succeeded instead of re-testing a profile already refused;
- the retry deliberately does not use the cookie jar: the jar is serialised per
  engine (C14) and a synchronous retry inside a batch would race the other jobs.
  A challenge is about the request fingerprint, not the cookies.

### Probes: paced, locked, and recorded

`tools/probe.sh` was a burst generator, and the Round-2 Brave matrix (five UAs
back to back) may itself have been what flagged this IP. It now refuses to run
without a lock, enforces **60 s between probes of the same engine**, allows
**3 per engine per hour**, and appends every probe to `docs/probe-log.tsv` with
the exact URL, UA, header mode and time.

### Independent sources

| Source | State here |
|---|---|
| `google_cse_element` | **Working** (H3). Disabled by default, needs only a `cx`, experimental. |
| SearXNG bridge | Supported and now documented with a one-command start plus `searxng_engines = google,brave,qwant` as the recommended value. This is the lower-risk route. |
| Brave Search API | **Deferred.** Needs a paid/registered key that does not exist in this environment, and the review itself flags an unverified monthly cap and billing beyond it. Shipping an engine that has never made a request would repeat exactly the mistake H3 caught. |
| Marginalia API | **Deferred**, same reason: needs a free key, and its endpoint and terms are marked **[verify]** in the review. |

Both deferred engines need a decision and a key from the owner. What is already
in place for when a key arrives: the registry entry shape, the per-engine token
bucket, the typed suspension, and the group accounting below, so adding one is a
parser plus a config line rather than new machinery.

### Saying it out loud

- `/settings` shows **how many independent index groups are answering now**, and
  warns when it is fewer than two.
- the results page notes **"Results from one index only (bing)"** whenever a
  single group contributed.

Measured on the live instance: `Independent indexes answering now: 1 (bing)`,
the warning, and the page note, while Brave was paused.

## Still open

- **Brave itself.** Its cooldown was still running during this round; the
  re-test must wait a full hour from the last probe and be a single Safari
  request, per the review. Until that happens, the honest statement is: the
  ladder is in place and unit-tested, the retry path is untested against live
  Brave, and the instance still has one index.
- Until a second independent source is enabled, the page note is the truthful
  outcome, not a defect.
