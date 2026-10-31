# Round 4 (J) — what is in, what is not, and the evidence

`make verify` → `verify: all checks passed`; the smoke suite inside it prints
`22 passed, 0 failed`. Those are two different counts and a earlier revision of
this file quoted the smoke number as if it were the whole suite - it is not.
Live service on `http://localhost:8087/` runs the tip of `sapir-whorf`.

| ID | Status | Evidence |
|---|---|---|
| **J3a** | fixed | `src/Query/Tokens.[ch]` is the only tokenizer; folded, punctuation-aware, floor skipped for letterless queries. `tests/test_j3_terms.c`, 41 cases, PASS. Live, paced: `1+1` 15, `covid-19` 14, `#123456` 15, `c++` 10, `3.14` 16, `50%` 16, `camión` 15, `o'brien` 13 — all 0 before. `docs/J3-terms.md` |
| **J3b** | fixed, one gap named | Wikipedia panel gated (exact title / 60 % of tokens / single-word top hit); `1+1` and `#ff0000` no longer get an article. 700 ms fetch budget. `web=1` says "Showing web results" and stops linking to itself. `answer_source_note` always set; render check fails on `not found in context`. `docs/J3b-answer-page.md` |
| **J3b (deadline)** | partial | the wait is bounded by bounding the *fetch*, not by a render-side deadline. A condvar gate with a refcounted heap array would be the real fix. |
| **J3c** | fixed | `[server] answers_fanout = always\|auto\|never`, default `always`; per-user cookie; both controls on `/settings`. `2+2` → 4 and 17 results. `tests/test_j3_answers.sh` PASS. |
| **J3d** | fixed | one `url_encode()` inside the href builders and `query_encoded` in the chrome. `tests/test_j3_hrefs.sh` PASS (4 queries × 4 positions). |
| **J1** | fixed by text, not by geometry | one `.content-layout` with `grid-template-areas`; compact `<details>` panel below the breakpoint; `zoom` replaced by `--page-scale` (the default 90 % was making every breakpoint fire 11 % early). `tools/check_themes.py` "one layout" lint. |
| **J1 (measured)** | **fixed, and it found a real bug** | Playwright's own Chromium runs here even though the installed app-bundle browsers cannot. 32 checks across 4 themes × 8 widths, **0 failures**, 12 screenshots in `docs/layout/`. The measurement found that `.content-layout` had `grid-template-areas: "main rail"` and **no `display: grid`** — the grid was inert, so the rail sat *below* the results at every width. Fixed; a lint now fails on any `grid-template-*` whose base class never sets `display: grid` (proven by deleting the line and watching it fire). `make layout-check PYTHON=/tmp/pw/bin/python` |
| **J2** | fixed | `--surface` existed in no theme (the white card); nine semantic tokens derived in `:root`; no `style='…'` left in `src/Infobox/*.c` except the `--swatch` custom property; icon tiles dropped. Three lints green on 16 themes, WCAG contrast included (`catppuccin-latte` and `light` failed first and the token derivation was fixed, not the threshold). `docs/J1-J2-layout-theme.md` |
| **J2 (contact sheet)** | **done** | 16 themes rendered at 1300×1000 into `docs/themes/`; inspected: no white card on the dark themes, and the light theme is the one with a white card on purpose. It also caught `covid-19` rendering a **calculator card that said Error** — the trigger accepted any word with a digit and a minus. Now every alphabetic run must be a known function or constant. `make theme-shots PYTHON=/tmp/pw/bin/python` |
| **J4.0** | **partly** | the registry carries `widget` per kind and `answer_box_widget()` returns `WidgetData`, never markup; `templates/widget_<kind>.html` (flat, because beaker's include rejects subdirectories); the card is the first element of the main column and suppresses the text strip. **Not done:** `/w/<kind>` endpoints, the `provider_chain`/`provider_fetch` layer (no network kind exists to need it), and the `static/widgets.js` enhancement. `docs/J4-widgets.md` |
| **J4a** | **partly** | the card exists: expression, big result, 26-key GET keypad that appends one character per request, `w=calculator` mode, `AC`, mid-entry is not an error. `Infinity`, `Error`, `-2^2 = -4`, `2^-1`, `ln/log/sqrt/abs/π/e`. **62-expression table** (precedence, associativity, unary minus, the functions the keypad offers, constants, exponent literals, 15 malformed inputs that must be NaN) plus `make widget-check`. Found while expanding it: bare `pi` was not a constant — only the glyph `π` — so a keyboard user got NaN while the keypad button worked. **Not done:** `sin/cos/tan` (needs a Deg/Rad decision — buttons removed rather than shipped broken), `%` as percent, `!`, implicit multiplication, `mod`, `EXP`. |
| **J4b** | **mostly, with a named defect** | probed MyMemory first (706 ms, 200; invalid pair is 403 with the error text inside `translatedText`, so the status is checked). `translate hello world to spanish` → `Hola Mundo` live, `traduce good morning al frances` → `Bonjour`, verb required so `<x> in spanish` stays a search, honest failure states, 16 parser cases (`translate-check`). **Named defect:** the provider returns a polluted memory entry with HTTP 200 and a perfect match score (`hello` → `testvalue`), sporadically — the card now says it is an unchecked community translation, and `docs/J4c-J4d.md` records the measurement. **Not done:** source-language detection, a language selector, caching, a self-hosted provider. `docs/J4b-translate.md` |
| **J4c** | **mostly** | probed Nominatim first (240 ms, 1 req/s policy honoured by making it one lookup). `where is Madrid` → display name + `40.4167820, -3.7035070` + an OSM link, live. A **place card, not a map** — tiles would be a third-party request from the reader. 20 trigger cases (`places-check`). **Not done:** no disambiguation between same-named places, no map image. `docs/J4c-J4d.md` |
| **J4d** | **mostly** | Stooq's CSV endpoint is **gone** (404 on both domains), so Yahoo's undocumented chart endpoint is used; `MSFT stock price` → `521.9100 USD` and `MSFT · prev 519.34 · 2026-10-30 20:00 UTC` live, with a visible "delayed, unofficial, not investment advice" line. Ticker rule fixed twice by the test (`quote for tsla` took FOR; `stock market news` took MARKET). **Not done:** charts, historical ranges, currency conversion of the quote. |
| **J5.0–5.1** | **partly, registry done** | `ENGINE_COUNT` is runtime now (constructor counts the initialisers before `main`), the two per-request arrays are heap-sized by the selected count instead of by the registry, and `static/engines.d/*.json` defines engines as data: a URL template plus four XPaths run by one shared parser. 16 registry entries, 15 compiled in plus `openlibrary` as data. `tests/test_j5_defs.c` PASS, `defs-check`. **Not done:** a JSON-API definition still needs a C body parser, `ENGINE_REGISTRY` is a heap array grown by doubling (50-definition growth test), with `MAX_ENGINES` kept as an enforced cap because the request-path routing arrays are sized by it. `docs/J5-engines.md`, `static/engines.d/README.md` |
| **J5.3** | **partly** | `?cat=news\|code\|academic\|media\|entity` narrows the fan-out to engines that declare the vertical, survives the pager, offers only categories an enabled engine actually serves, and says "nothing was asked" instead of falling back to web results when none does. `tests/test_j5_categories.sh` PASS, `categories-check`. **Not done:** the tabs live on the results page rather than in the shared chrome, and there is no Images/Media split beyond the existing `/images`. |
| **J5.4** | **fixed** | `?engine=<id>` routes to one engine, and the page now says which engines were asked and what each returned: `All \| News \| Code \| Academic \| Yahoo (7) \| Bing (10)`. Categories and engines share **one** context key (a chip matrix of `{href, label, class, group}`) because the context is a hard 32 slots — last round's attempt to add a second key silently dropped `paused_note`, which is why it was reverted. Exactly one chip is active in all/category/engine modes; counts come from the jobs, not the rendered rows. `tests/test_j5_categories.sh` covers both groups. |
| **J5.5** | **fixed** | Per-engine weights, both halves: `[engines] weights = id:weight,...` on the server (clamped to (0, 5], applied count logged) and an editor on `/settings` — one input per enabled engine, saved per browser, applied through a `_Thread_local` table so concurrent requests cannot share it. The page shows back the weight routing will actually use. 6 cases in `test_j5_weights` plus 12 in `test_j5_defs`. Measured on the way: beaker **drops a cookie whose value contains a comma** (no error, header simply absent — pairs use `|`), and the context had 31 of 32 slots before three keys were merged away. |
| **J5.7** | **not started** | no per-engine process, no overlap report. As written this is a redesign of the fan-out rather than a fix, and it is not attempted unbidden. |
| **J5.1 prep** | measured, not built | `ENGINE_COUNT` is load-bearing at **114 sites**, three of them stack arrays sized by it (`ScrapeJob jobs[]` is ~26 KB today, ~300 KB at 150 engines, on a thread whose stack the review flags). The migration is four steps and needs a round of its own; the `BodyParserFn` signature carries no engine identity, so a generic parser needs a def pointer plus a thread-local. Open Library probed once (200, 1809 B, numFound=89) and its fixture saved. `docs/J5-engines.md` |
| **J5.2** | fixed | `SearchResult` carries `thumbnail/date/author/source/kind`, the ranker borrows them, the row template renders them, thumbnails go through the signed `/proxy` link. Measured end to end against a fake instance: thumbnail row, `source=brave`, `source=google`, a date. Per-kind templates and age formatting not done. |
| **J5.6** | **mostly** | per-hit `index_group` from each result's `engine` field, so a bridge answer is no longer all "google"; buffer 128→1024 and the config line buffer 1024→4096 with a loud rejection; bang→name map with dedupe (`!go,!ddg,br,qw` → `google,duckduckgo,brave,qwant`); startup probe for `format=json` and `/config` name validation, measured against a fake instance. **Not done:** per-result `index_group` and the richer JSON fields — both need `SearchResult` to carry them (J5.2). |
| **J5.7** | **not started** | no per-engine process, no overlap report. |
| **J5.5 tier 3** | fixed (the gate) | the owner asked for a switch for the opt-in-only providers with a legal notice: `[engines] allow_files` plus a per-browser cookie, the control hidden while the server refuses, four-paragraph notice in en/es, six-case test. No provider is registered under it. `docs/J5-opt-in.md` |
| **J6.1** | fixed | query text removed from the log; counts and per-engine breakdown kept. |
| **J6.2** | fixed | `InfoBox.is_html`, set only by local generators; `add_infobox_to_collection` escapes the rest. `{{ }}` verified to escape by default, so `|safe` was the reason. `AT&T` renders `AT&amp;T`. |
| **J6.3** | fixed | `format_number()` in `Calculator.c` is the only place a computed number becomes text; the strip and the card agree by construction rather than by two format strings happening to match. |
| **J6.4** | fixed via J3b | Wikipedia no longer asked for every query, and the short-query bypass is gone. |
| **J6.5** | fixed | one tokenizer. |
| **J6.6** | fixed | `-` is an exclusion only when a letter follows. |
| **J6.7** | fixed | `answers` and `answers_fanout` both have controls on `/settings`. |
| **J6.8** | **not done** | SETUP §3 has rows for the answer settings; not for translation, geocoding, tiles or stock providers, because those do not exist. |
| **J6.9** | fixed | `docs/claude-feedback.md` restored from HEAD and extended with Round 4 as Part 6. |
| **J6.10** | fixed | `make render-check` fails on `not found in context`. |

## [verify] items resolved with the exact request

- **H3-adjacent, J3b:** `GET https://en.wikipedia.org/w/api.php?action=query&generator=search&gsrsearch=españa&…`
  returns the article **"Spain"**, and `es.wikipedia` returns **"España"**.
  That is why a strict title match would drop a correct panel and why the
  single-word exception exists.
- **J5.6:** `GET <searxng>/config` returns `{"engines":[{"name":…,"shortcut":…,"enabled":…}]}`,
  which is what the startup validation reads.
- **J3a:** the review's two "nonsense" strings are not nonsense on the live web
  — `zzqqxx vbbnmm` returns `tiktok.com/tag/vbbnmm` — so the test uses absent
  strings instead. Recorded in `docs/J3-terms.md`.

## Things found that are not in the document

1. **`make verify` was never run by me in Round 1 and it was broken** (it
   depended on a nonexistent target). Fixed in Round 2; it is green now.
2. **`zoom` at the default 90 %** made every media query fire 11 % early, so
   the rail dropped below the results on a window that had room for two
   columns. This is a *cause* of the owner's report, not just a J1 detail.
3. **`search_href` argument order:** adding `web` before `page` pointed every
   pager link at page 1. Caught by `tests/test_j3_hrefs.sh`.
4. **The config reader's 1024-byte line buffer** was the real ceiling on
   `searxng_engines`; raising the field alone would have changed nothing.
