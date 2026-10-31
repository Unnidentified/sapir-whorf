# J3b — the answer page says what it did

## 1. The rail panel is now gated

`infobox = all` fetched Wikipedia for **every** query through
`generator=search`, which returns the best fuzzy match for any text, and the
relevance check accepted any single token anywhere in the whole extract. The
gate was also skipped entirely when the tokenizer produced no tokens — which,
before J3a, was every query containing punctuation. That is where the article
"1+1" in the owner's screenshot came from: not a search result, an infobox
thread.

Now the Wikipedia panel renders only when:

- the folded article title equals the folded query exactly ("españa" →
  "España"), or
- the title and the **first sentence** cover at least 60 % of the query's
  normalised tokens, or
- the query is a single word with letters. This exception is deliberate and
  measured: `en.wikipedia`'s top hit for `españa` is the article **"Spain"**,
  because language detection falls back to the locale for one-word queries.
  A strict title match throws the correct panel away. A one-word query is a
  lookup; the generator's own ranking is the evidence.

A query that was answered locally never gets the generic panel at all. Measured
with `answers=all`:

| query | panels |
|---|---|
| `1+1` | Calculation |
| `12 inches in cm` | Unit Conversion |
| `#ff0000` | Colour |
| `españa` | Spain |
| `python` | Python (programming language) |
| `covid-19` | COVID-19 |
| `xyzzy` | Xyzzy (computing) — the known cost of the single-word exception |

## 2. A budget, not an unbounded wait

The panel fetches (`Wikipedia.c`, `Dictionary.c`, `CurrencyConversion.c`) now go
through `http_get_budget()` with `INFOBOX_BUDGET_MS = 700`. Before, they used
the shared 15 s curl timeout, and the handler joins every infobox thread before
rendering — the owner measured `1+1` at 1607 ms with no fan-out at all, against
a ~600 ms median.

**Gap, stated plainly:** this bounds the wait by bounding the upstream call; it
is not a render-side deadline. A thread that overruns for a non-network reason
still delays the render, because the data array lives on the handler's stack
and a render-side deadline would need the array, the results and the join to
move to a refcounted heap structure with a condvar gate. That is the correct
fix and it is not in this round.

## 3. State lines that match reality

- `?web=1` now sets `web_explicit`. The card then reads **"Showing web
  results"** and does **not** offer "search the web for this" — a link to what
  the page just did.
- `web=1` survives the pager (`/search?q=…&p=2&web=1`), so page 2 does not
  silently become an answer-only page.
- `answer_source_note` is set once, early, before any branch. The template
  prints it unconditionally, and it was missing on some paths — nine
  `Key 'answer_source_note' not found in context` lines in the shipped log,
  which renders as a literal `{{answer_source_note}}` on the page.

### A regression found while testing this

Adding the `web` argument to `build_search_href` put it **before** `page` in the
signature while the call site still passed `page` first, so every pager link
pointed at page 1 with `&web=1` appended. `tests/test_j3_hrefs.sh` caught it
because it asserts `/search?q=…&p=2` exists. Signature is now
`(query, engine_id, page, web)`. Pager links:

```
/search?q=linux%20kernel                 (current page)
/search?q=linux%20kernel&p=2&web=1
/search?q=linux%20kernel&p=3&web=1
```

## 4. Strings

`results.html` no longer contains English prose: "Direct answer", "search the
web for this", "No results found for", the cache note, the one-index note, the
partial-failure heading and "Wait for all sources" all come from
`locales/en_gb.ini`, with `es_es.ini` translated. A `{group}` / `{count}`
placeholder is substituted in C, not passed to `printf`, so a `%` in a
translation cannot become a format string.

## 5. The render check fails on a missing key

`tests/test_context_capacity.sh` (make render-check) now also fails when the
log contains `not found in context`, not just when the context is full. That is
the check that would have caught `{{answer_source_note}}` shipping as literal
text. Current run:

```
context capacity: widest render path
  ok   no context key dropped
  ok   no template tag left without a context key
  ok   results rendered (11)
  ok   page 2 rendered
  ok   infobox rendered
context capacity: PASS
```
