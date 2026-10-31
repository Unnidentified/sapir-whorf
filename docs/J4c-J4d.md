# J4c place card, J4d share price — built after probes

Both follow the J4.0 pattern: a `WidgetData` filled in C, a flat template that
renders it, and an honest failure state. Neither contacts a third party from the
reader's browser.

## J4c — place (`src/Infobox/Places.c`)

Probe (docs/probe-log.tsv): Nominatim answers "Madrid" in **240 ms** with
lat/lon and a display name, and its usage policy is explicit — 1 request per
second, an identifying User-Agent, no bulk geocoding. So the card is **one**
lookup for a query that asked for a place, and the widget can never become the
thing that makes a search loop.

It is a **place card, not a map**. Drawing tiles would make the reader's browser
contact a tile server on our behalf; the card shows the display name, the
coordinates and a link that the reader chooses to follow.

```
where is Madrid   →  Madrid, Community of Madrid, Spain
                     40.4167820, -3.7035070 · OpenStreetMap  [Open in OpenStreetMap]
```

Detection needs a phrase that only makes sense as a lookup ("where is", "map of",
"coordinates of", "dónde está", "mapa de", "coordenadas de"). A bare `Madrid` is
a search.

## J4d — share price (`src/Infobox/Stocks.c`)

Two probes decided this. **Stooq's CSV endpoint is gone** (404 on both
stooq.com and stooq.pl), so the obvious free source no longer exists. Yahoo's
chart endpoint answers AAPL in **325 ms** with price, currency, previous close
and a market timestamp.

That endpoint is **undocumented and not a public API**. It is used because it is
the only source that answered without a key, and the card says what it is:
*"Delayed quote from an unofficial endpoint. Not investment advice."* If it
breaks, the card says the provider did not answer rather than showing a stale
number as if it were live.

```
MSFT stock price  →  521.9100 USD
                     MSFT · prev 519.34 · 2026-10-30 20:00 UTC
```

The trigger took two bug fixes, both caught by the test rather than by me:
`quote for tsla` took **FOR** as the ticker, and `stock market news` took
**MARKET**. The rule is now: a price-intent keyword must be present, and the
candidate is an explicit `of/for/de X`, or a dotted/dashed symbol, or the first
token — never a keyword. `price of gold` stays a search, because "price" alone
is not a price intent.

## The translate card's honesty problem, found here

While testing these I re-probed the J4b provider and found:

```
GET .../get?q=hello&langpair=en|es  →  200  {"translatedText":"testvalue","match":1}
```

A corrupted community-memory entry, returned with HTTP 200, a perfect match
score and the same quality value as the good entries in the same payload. No
gate can catch it: `goodbye`, `cat`, `thank you` and `where is the station` all
came back correct in the same minute, so the pollution is sporadic rather than
structural.

That is why the translate card now carries a visible caveat — *"Community
translation from MyMemory, not checked by this site. Short or common words can
be wrong."* — instead of presenting a community memory as an authoritative
translation. Lingva was also probed and returned the **untranslated** text for
en→es, so it is not a better fallback; LibreTranslate's public instances
returned 502.

The real fix is a self-hosted LibreTranslate behind `[engines] translate_url`,
which is not built. Until then the card is honest about what it is.

## Tests

- `tests/test_j4_places_stocks.c` — 20 trigger cases, `make places-check`.
- The cards themselves are exercised live by the smoke run; `where is Madrid`,
  `MSFT stock price` and `translate good morning to french` are all verified in
  this round's report.

## Not done

- no map image (deliberate: it would be a third-party request from the reader)
- no stock chart, no historical range, no currency conversion of the quote
- the place card does not disambiguate between two places with the same name
  (Nominatim's `limit=1` picks by importance, and the card does not say so)
