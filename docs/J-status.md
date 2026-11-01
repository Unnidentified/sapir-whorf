# Round 4 (J) — what is in, what is not, and the evidence

`make verify` → `verify: all checks passed`; the smoke suite inside it prints
`22 passed, 0 failed`. Those are two different counts and a earlier revision of
this file quoted the smoke number as if it were the whole suite - it is not.
Live service on `http://localhost:8087/` runs the tip of `sapir-whorf`.

| ID | Status | Verified how | Evidence |
|---|---|---|---|
| **J3a** | fixed | unit test (41 cases) + live paced queries | `src/Query/Tokens.[ch]` is the only tokenizer; folded, punctuation-aware, floor skipped for letterless queries. `tests/test_j3_terms.c`, 41 cases, PASS. Live, paced: `1+1` 15, `covid-19` 14, `#123456` 15, `c++` 10, `3.14` 16, `50%` 16, `camión` 15, `o'brien` 13 — all 0 before. `docs/J3-terms.md` |
| **J3b** | fixed, one gap named | replay + render check | Wikipedia panel gated (exact title / 60 % of tokens / single-word top hit); `1+1` and `#ff0000` no longer get an article. 700 ms fetch budget. `web=1` says "Showing web results" and stops linking to itself. `answer_source_note` always set; render check fails on `not found in context`. `docs/J3b-answer-page.md` |
| **J3b (deadline)** | fixed in effect, mechanism differs | code review: fetch budget, not a render wait | the 700 ms budget bounds the *fetch* rather than a render-side wait, so the panel cannot delay the page past it. A condvar gate with a refcounted heap array would be the purer form; the observable guarantee the review asked for holds. |
| **J3c** | fixed | test_j3_answers.sh | `[server] answers_fanout = always\|auto\|never`, default `always`; per-user cookie; both controls on `/settings`. `2+2` → 4 and 17 results. `tests/test_j3_answers.sh` PASS. |
| **J3d** | fixed | unit test (round-trip) | one `url_encode()` inside the href builders and `query_encoded` in the chrome. `tests/test_j3_hrefs.sh` PASS (4 queries × 4 positions). |
| **J1** | **fixed, and it found a real bug** | browser (layout_check.py, 32 checks) | Playwright's own Chromium runs here even though the installed app-bundle browsers cannot. 32 checks across 4 themes × 8 widths, **0 failures**, 12 screenshots in `docs/layout/`. The measurement found that `.content-layout` had `grid-template-areas: "main rail"` and **no `display: grid`** — the grid was inert, so the rail sat *below* the results at every width. Fixed; a lint now fails on any `grid-template-*` whose base class never sets `display: grid` (proven by deleting the line and watching it fire). `make layout-check PYTHON=/tmp/pw/bin/python` |
| **J2** | fixed | browser (theme sheet, 16 themes) + lint | `--surface` existed in no theme (the white card); nine semantic tokens derived in `:root`; no `style='…'` left in `src/Infobox/*.c` except the `--swatch` custom property; icon tiles dropped. Three lints green on 16 themes, WCAG contrast included (`catppuccin-latte` and `light` failed first and the token derivation was fixed, not the threshold). `docs/J1-J2-layout-theme.md` |
| **J2 (contact sheet)** | **done** | implemented, unverified | 16 themes rendered at 1300×1000 into `docs/themes/`; inspected: no white card on the dark themes, and the light theme is the one with a white card on purpose. It also caught `covid-19` rendering a **calculator card that said Error** — the trigger accepted any word with a digit and a minus. Now every alphabetic run must be a known function or constant. `make theme-shots PYTHON=/tmp/pw/bin/python` |
| **J4.0** | **fixed** | browser (both with and without JS) | the registry carries `widget` per kind and `answer_box_widget()` returns `WidgetData`, never markup; cards are `templates/widget_<kind>.html`; `/w/calculator`, `/w/translate`, `/w/place`, `/w/stock` serve one card under the inbound limiter, with the kind selecting a function so no request can name a host (SSRF is structural); `static/widgets.js` enhances without being required — measured both ways (`5`→`57` with no navigation; the plain GET with JS off). |
| **J4a** | **fixed** | unit test (62-expression table) | Pratt parser with functions and constants, real errors (`NaN` for malformed, `Infinity` for 1/0), one formatter (`format_number`), a 26-key GET keypad that appends one character per request, `w=calculator` explicit mode, and a **62-expression table**. Found while expanding it: bare `pi` was not a constant, only the glyph. |
| **J4b** | **mostly, with a named defect** | provider probe + unit test + live | probed MyMemory first (706 ms, 200; invalid pair is 403 with the error text inside `translatedText`, so the status is checked). `translate hello world to spanish` → `Hola Mundo` live, `traduce good morning al frances` → `Bonjour`, verb required so `<x> in spanish` stays a search, honest failure states, 16 parser cases (`translate-check`). **Named defect:** the provider returns a polluted memory entry with HTTP 200 and a perfect match score (`hello` → `testvalue`), sporadically — the card now says it is an unchecked community translation, and `docs/J4c-J4d.md` records the measurement. **Not done:** source-language detection, a language selector, caching, a self-hosted provider. `docs/J4b-translate.md` |
| **J4c** | **mostly** | provider probe + unit test + live | probed Nominatim first (240 ms, 1 req/s policy honoured by making it one lookup). `where is Madrid` → display name + `40.4167820, -3.7035070` + an OSM link, live. A **place card, not a map** — tiles would be a third-party request from the reader. 20 trigger cases (`places-check`). **Not done:** no disambiguation between same-named places, no map image. `docs/J4c-J4d.md` |
| **J4d** | **mostly** | provider probe + unit test + live | Stooq's CSV endpoint is **gone** (404 on both domains), so Yahoo's undocumented chart endpoint is used; `MSFT stock price` → `521.9100 USD` and `MSFT · prev 519.34 · 2026-10-30 20:00 UTC` live, with a visible "delayed, unofficial, not investment advice" line. Ticker rule fixed twice by the test (`quote for tsla` took FOR; `stock market news` took MARKET). **Not done:** charts, historical ranges, currency conversion of the quote. |
| **J5.0–5.1** | **partly, registry done** | implemented, unverified | `ENGINE_COUNT` is runtime now (constructor counts the initialisers before `main`), the two per-request arrays are heap-sized by the selected count instead of by the registry, and `static/engines.d/*.json` defines engines as data: a URL template plus four XPaths run by one shared parser. 16 registry entries, 15 compiled in plus `openlibrary` as data. `tests/test_j5_defs.c` PASS, `defs-check`. **Not done:** a JSON-API definition still needs a C body parser, `ENGINE_REGISTRY` is a heap array grown by doubling (50-definition growth test), with `MAX_ENGINES` kept as an enforced cap because the request-path routing arrays are sized by it. `docs/J5-engines.md`, `static/engines.d/README.md` |
| **J5.3** | **fixed** | test_j5_categories.sh | `?cat=...` narrows the fan-out to engines that declare the vertical, survives the pager, offers only categories an enabled engine serves, and answers "nothing was asked" rather than falling back to web results. Since J5.4 the category and engine chips share one strip. |
| **J5.4** | **fixed** | test_j5_categories.sh + browser | `?engine=<id>` routes to one engine, and the page now says which engines were asked and what each returned: `All \| News \| Code \| Academic \| Yahoo (7) \| Bing (10)`. Categories and engines share **one** context key (a chip matrix of `{href, label, class, group}`) because the context is a hard 32 slots — last round's attempt to add a second key silently dropped `paused_note`, which is why it was reverted. Exactly one chip is active in all/category/engine modes; counts come from the jobs, not the rendered rows. `tests/test_j5_categories.sh` covers both groups. |
| **J5.5** | **fixed** | test_j5_weights.sh + browser | Per-engine weights, both halves: `[engines] weights = id:weight,...` on the server (clamped to (0, 5], applied count logged) and an editor on `/settings` — one input per enabled engine, saved per browser, applied through a `_Thread_local` table so concurrent requests cannot share it. The page shows back the weight routing will actually use. 6 cases in `test_j5_weights` plus 12 in `test_j5_defs`. Measured on the way: beaker **drops a cookie whose value contains a comma** (no error, header simply absent — pairs use `|`), and the context had 31 of 32 slots before three keys were merged away. |
| **J5.7** | **not started** | implemented, unverified | no per-engine process, no overlap report. As written this is a redesign of the fan-out rather than a fix, and it is not attempted unbidden. |
| **J5.2** | fixed | unit test | `SearchResult` carries `thumbnail/date/author/source/kind`, the ranker borrows them, the row template renders them, thumbnails go through the signed `/proxy` link. Measured end to end against a fake instance: thumbnail row, `source=brave`, `source=google`, a date. Per-kind templates and age formatting not done. |
| **J5.6** | **fixed** | test_j5_bridge.sh | per-hit `index_group` from each result's engine field; rich JSON through `SearchResult`; buffer 128→1024 and the config line buffer 1024→4096 with a loud rejection; bang→name map with dedupe; startup probe for `format=json` and `/config` validation, measured against a fake instance. |
| **J5.5 tier 3** | fixed (the gate) | implemented, unverified | the owner asked for a switch for the opt-in-only providers with a legal notice: `[engines] allow_files` plus a per-browser cookie, the control hidden while the server refuses, four-paragraph notice in en/es, six-case test. No provider is registered under it. `docs/J5-opt-in.md` |
| **J6.1** | fixed | test suite | query text removed from the log; counts and per-engine breakdown kept. |
| **J6.2** | fixed | test suite | `InfoBox.is_html`, set only by local generators; `add_infobox_to_collection` escapes the rest. `{{ }}` verified to escape by default, so `|safe` was the reason. `AT&T` renders `AT&amp;T`. |
| **J6.3** | fixed | unit test (one formatter) | `format_number()` in `Calculator.c` is the only place a computed number becomes text; the strip and the card agree by construction rather than by two format strings happening to match. |
| **J6.4** | fixed via J3b | test suite | Wikipedia no longer asked for every query, and the short-query bypass is gone. |
| **J6.5** | fixed | test suite | one tokenizer. |
| **J6.6** | fixed | test suite | `-` is an exclusion only when a letter follows. |
| **J6.7** | fixed | test suite | `answers` and `answers_fanout` both have controls on `/settings`. |
| **J6.8** | fixed | test suite | SETUP has rows for the answer settings, the three widget providers (translation, geocoding, stock) with what each costs and what it is not, plus `definitions_dir`, `weights` and `allow_files`. Tiles are deliberately absent: this build fetches none. |
| **J6.9** | fixed | test suite | `docs/claude-feedback.md` restored from HEAD and extended with Round 4 as Part 6. |
| **J6.10** | fixed | test suite | `make render-check` fails on `not found in context`. |

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

## Round K (docs/claude-feedback-round5.md)

| ID | Status | Verified how | Evidence |
|---|---|---|---|
| **K1.1** | fixed | test_k1_toggle_persist.sh + browser | the cookie is `off=<ids>` joined with `|`, written with a 1024-byte buffer and an overflow check that refuses rather than writes a partial list. Saving three engines: before `engines=` (empty, never written), after `off=bing|translate|yahoo`. |
| **K1.2** | fixed | browser (Playwright) | `step`/`novalidate`: the weight inputs no longer block the form, measured by clicking the real Save button with an out-of-range value present. |
| **K1.2b** | fixed | browser + curl | beaker keeps 16 URL parameters and stops parsing silently. Four forms, one `off=` field built by settings.js, `engines_seen` so a truncated submission is refused instead of written. |
| **K1.3** | fixed | browser + test_k1_toggle_effect.sh | one provider table, 32 rows for 16 engines + 8 answer providers + 8 definitions, grouped, with each destination host and the three states in words. `bin/omnisearch --providers` prints the same rows for tests. |
| **K1.4** | fixed | test_k1_toggle_effect.sh (24/24) | a counting proxy records every host asked for: each provider is contacted when on, not when off, and not when the server disabled it. The F2 recovery round was reaching engines the first pass could not - one predicate now guards the first pass, recovery and the Reddit lane. |
| **K1.5** | fixed | test_k1_optin.sh (6/6) + test_opt_in.sh | one switch per opt-in provider, `optin=<ids>` cookie, server gate outermost, and no provider is registered in this build yet so the page says so. |
| **K2** | fixed | check_themes + browser (6 shots) | form-control tokens, `color-scheme` per theme, weights row label-first, lint `check_form_control_tokens`. |
| **K3** | fixed | test_k3_resume.sh (7/7) + browser | `data-resume-at` is `engine_health_open_until()`, the same field the dispatcher tests; the page counts down and polls `/settings/engine-status.json`. The last-engine exception that contacted a suspended engine is gone. |
| **K7.1** | fixed | test_k7_placeholders.c + test_k7_merge.c | one placeholder table, prefix-matched, and a merge that prefers a real snippet over an empty one. |
| **K7.2** | not done | - | the `/d` description fetcher with robots respect. |
| **K8** | fixed | test_k8_single_answer.sh | the arithmetic rail handler is gone; `1+1` renders one answer. |
| **K9** | fixed | tools/calc_card_check.py (4 widths) + browser | the card fits its keypad (11 px, no overflow), `%`, `Ans` and `⌫` are understood by the evaluator, and one `calc_apply_key()` is shared by the page and `/w/calculator` - two copies had made AC produce `2+27AC`. |
| **K10** | fixed, two rungs of three | parser test (13 K10 cases) + browser | two panes with two menus, swap (a submit button, so it works without JS), a textarea and an output pane; `translate_codes` preselects the menus and `widgets.js` updates 400 ms after typing stops. The parser now takes `<text> in <language>`, `how do you say …`, a bare `translate`, and `<language> to <language>`, with the false positives tested (`meaning in tamil`, `1 in binary`). Guards measured against the live provider: echo (exact and "with !!! appended"), quota/error text inside a 200, and a detection retry when the asked pair returns nothing ("good morning" en|de was empty; Autodetect|de answered). Not done: the LibreTranslate rung, the cache, and the Google rung - it returned a challenge page from this network when probed, so it is not shipped. |
| **K11 (error path)** | fixed | test_k11_stock_errors.sh | no dead card, one error line, disclaimer only with a quote. The name-to-symbol table and the sparkline are not done. |
| **K12.1** | fixed | browser | one Settings link: the tab. The header gear is gone from both chromes. |
| **K12.2** | fixed | browser (es_es) | the status header and the two phrases come from `locales/`; Spanish renders Motor / Estado / Motivo / Se reanuda. |
| **K12.6** | fixed | test_k12_zero_body.sh (2/2) | a 200 with an empty body is a block signal: recorded, surfaced as "challenged", and the next query does not reach the upstream. |
| **K12.11** | fixed | this table | the "Verified how" column above. |
| **K4 / K5 / K6** | not done | - | the tab bar with Videos, the Reddit ladder, and curl-impersonate. |
