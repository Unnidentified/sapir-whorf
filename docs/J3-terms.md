# J3a — one tokenizer, and the six queries that rendered nothing

## What was wrong

Three tokenizers, three minimum lengths, three alphabets:

| Where | Split on | Kept |
|---|---|---|
| `QueryAnalysis.c` term building | spaces | word of length ≥ 3, ASCII-lowercased |
| `Ranker.c` `tokenize_into` (documents) | every byte outside `[A-Za-z0-9]` | token of length ≥ 2 |
| `Search.c` `tokenize_query` (infobox gate) | every byte outside `[A-Za-z0-9]` | token of length ≥ 3 |

A query term containing punctuation or a non-ASCII letter was therefore never
equal to any document token. `df` stayed 0, the coverage loop skipped the term
("a term no candidate contains cannot be matched"), `idf_sum` stayed 0 and
`coverage` came out `0.0` for **every** document. `Search.c` drops anything
under `coverage >= 0.15`, so the page was empty and the hint said "none of what
came back was relevant enough".

The shipped `omnisearch.err` has six queries with 20–37 raw hits and zero
rendered: `1+1`, `2+2`, `covid-19`, `#123456`, `google_cse`, `1.234,56 + 2`.

## What it is now

`src/Query/Tokens.[ch]` is the only tokenizer. `QueryAnalysis.c`, `Ranker.c`
and `Search.c` all call it.

- **Folding.** Lowercase plus NFKD-derived diacritic stripping for every
  precomposed Latin letter in U+00C0–U+024F and U+1E00–U+1EFF (508 entries,
  generated from Unicode data, not typed by hand). `españa` → `espana` on both
  sides, so an accented query matches an unaccented page and the reverse. The
  **engine query is never altered** — folding happens for matching only.
- **Lengths.** A digit run is a word at length 1 (`3.14` → `3`, `14`); anything
  else needs 2; each CJK character is its own token.
- **Punctuated terms.** A term matches if its folded literal is a substring of
  title, snippet or URL — but **only when the term carries punctuation**. For
  a plain word the literal path is not tried, because it also matches the
  SERP's own echo of the query; with it unrestricted, `zzqqxx vbbnmm` rendered
  11 results (measured). Plain words match through tokens.
- **Floor exemption.** `SearchQuery.has_alpha_term` is 0 when no term contains
  a letter. Those queries (`1+1`, `#123456`, `50%`) skip the coverage floor and
  rank by RRF: the engines already judged relevance, and this is precisely the
  case the old floor got wrong. `web=1` ("search the web for this") sets
  `force_web` and skips the floor too — that link exists to override "we
  already answered that".

## Measured

Deterministic regression test — `tests/test_j3_terms.c`, 41 cases,
`make terms-check`:

```
PASS: 41 cases, 0 failed
```

25 queries built as a SERP-shaped doc set (8 of 10 documents carry the query,
as real engine results do) each render ≥ 5 of 10; three absent strings render
0.

Live, against a private instance on `127.0.0.1:8091` with `answers = off`
(so the ranker path is exercised with no local answer in the way), one query
every 6 s, cache directory emptied first:

| query | results |
|---|---|
| `1+1` | 15 |
| `covid-19` | 14 |
| `#123456` | 15 |
| `c++` | 10 |
| `3.14` | 16 |
| `50%` | 16 |
| `camión` | 15 |
| `o'brien` | 13 |

Before the change all of these rendered 0 through the same path.

### Two things worth knowing before re-measuring

1. **Pacing is not optional.** A 28-request burst at one per second made the
   second half of the run return zero-result pages at 0.6 ms each: the engines
   were blocked and the empty pages were cached. The same queries render
   10–16 results when asked 6 s apart. This is the H5 rule, measured.
2. **The review's two nonsense strings are not nonsense on the live web.**
   `zzqqxx vbbnmm` returns a TikTok hashtag page (`tiktok.com/tag/vbbnmm`) and
   an Instagram profile; `qwpoeiruty asdfghjkl` returns pages too. A candidate
   really does contain the term, so the guard correctly does not fire. The test
   uses strings that are actually absent (`xqztvw plmnko`,
   `qwzxvbnm plkjhgfds`) and both render 0.

## Also fixed here

**J6.6** — the `-` exclusion operator now requires a letter after the sign.
`10 -3`, `covid -19` and `vi -editor` were losing their second term because
any word of length > 1 starting with `-` was stripped.

**J6.5** — one tokenizer instead of three; `Search.c`'s `tokenize_query` is now
a stopword filter over `tokens_split`.

## Still open

`3-2-1 song`, `2 broke girls`, `iphone-15` and `10 downing street` are the
J4a negative cases; they are alphabetic enough to rank, and their behaviour is
covered by the J4a matcher tables, not here.
