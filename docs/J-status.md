# Round 4 (J) — what is in, what is not, and the evidence

`make verify` → `22 passed, 0 failed` / `verify: all checks passed`.
Live service on `http://localhost:8087/` runs the tip of `sapir-whorf`.

| ID | Status | Evidence |
|---|---|---|
| **J3a** | fixed | `src/Query/Tokens.[ch]` is the only tokenizer; folded, punctuation-aware, floor skipped for letterless queries. `tests/test_j3_terms.c`, 41 cases, PASS. Live, paced: `1+1` 15, `covid-19` 14, `#123456` 15, `c++` 10, `3.14` 16, `50%` 16, `camión` 15, `o'brien` 13 — all 0 before. `docs/J3-terms.md` |
| **J3b** | fixed, one gap named | Wikipedia panel gated (exact title / 60 % of tokens / single-word top hit); `1+1` and `#ff0000` no longer get an article. 700 ms fetch budget. `web=1` says "Showing web results" and stops linking to itself. `answer_source_note` always set; render check fails on `not found in context`. `docs/J3b-answer-page.md` |
| **J3b (deadline)** | partial | the wait is bounded by bounding the *fetch*, not by a render-side deadline. A condvar gate with a refcounted heap array would be the real fix. |
| **J3c** | fixed | `[server] answers_fanout = always\|auto\|never`, default `always`; per-user cookie; both controls on `/settings`. `2+2` → 4 and 17 results. `tests/test_j3_answers.sh` PASS. |
| **J3d** | fixed | one `url_encode()` inside the href builders and `query_encoded` in the chrome. `tests/test_j3_hrefs.sh` PASS (4 queries × 4 positions). |
| **J1** | fixed by text, not by geometry | one `.content-layout` with `grid-template-areas`; compact `<details>` panel below the breakpoint; `zoom` replaced by `--page-scale` (the default 90 % was making every breakpoint fire 11 % early). `tools/check_themes.py` "one layout" lint. |
| **J1 (measured)** | **blocked by the environment** | `tools/layout_check.py` prints a skip and `docs/layout/` is empty. Measured why: the browsers on this machine cannot run headless here (permission-denied messages, no `--dump-dom` output), so this is a capability gap rather than a missing package. |
| **J2** | fixed | `--surface` existed in no theme (the white card); nine semantic tokens derived in `:root`; no `style='…'` left in `src/Infobox/*.c` except the `--swatch` custom property; icon tiles dropped. Three lints green on 16 themes, WCAG contrast included (`catppuccin-latte` and `light` failed first and the token derivation was fixed, not the threshold). `docs/J1-J2-layout-theme.md` |
| **J2 (contact sheet)** | **blocked by the environment** | same gap; `docs/themes/` is empty. |
| **J4.0** | **partly** | the registry carries `widget` per kind and `answer_box_widget()` returns `WidgetData`, never markup; `templates/widget_<kind>.html` (flat, because beaker's include rejects subdirectories); the card is the first element of the main column and suppresses the text strip. **Not done:** `/w/<kind>` endpoints, the `provider_chain`/`provider_fetch` layer (no network kind exists to need it), and the `static/widgets.js` enhancement. `docs/J4-widgets.md` |
| **J4a** | **partly** | the card exists: expression, big result, 26-key GET keypad that appends one character per request, `w=calculator` mode, `AC`, mid-entry is not an error. `Infinity`, `Error`, `-2^2 = -4`, `2^-1`, `ln/log/sqrt/abs/π/e`. **Not done:** `sin/cos/tan` (needs a Deg/Rad decision — buttons removed rather than shipped broken), `%` as percent, `!`, implicit multiplication, `mod`, `EXP`. 21-expression table + `make widget-check`. |
| **J4b, J4c, J4d** | **not started** | no translation, map or stock code, and no paced provider probes were run. |
| **J5.0–5.5** | **not started** | still 13 registry entries; no `static/engines.d`, no tabs beyond All/Images. |
| **J5.1 prep** | measured, not built | `ENGINE_COUNT` is load-bearing at **114 sites**, three of them stack arrays sized by it (`ScrapeJob jobs[]` is ~26 KB today, ~300 KB at 150 engines, on a thread whose stack the review flags). The migration is four steps and needs a round of its own; the `BodyParserFn` signature carries no engine identity, so a generic parser needs a def pointer plus a thread-local. Open Library probed once (200, 1809 B, numFound=89) and its fixture saved. `docs/J5-engines.md` |
| **J5.2** | fixed | `SearchResult` carries `thumbnail/date/author/source/kind`, the ranker borrows them, the row template renders them, thumbnails go through the signed `/proxy` link. Measured end to end against a fake instance: thumbnail row, `source=brave`, `source=google`, a date. Per-kind templates and age formatting not done. |
| **J5.6** | **mostly** | per-hit `index_group` from each result's `engine` field, so a bridge answer is no longer all "google"; buffer 128→1024 and the config line buffer 1024→4096 with a loud rejection; bang→name map with dedupe (`!go,!ddg,br,qw` → `google,duckduckgo,brave,qwant`); startup probe for `format=json` and `/config` name validation, measured against a fake instance. **Not done:** per-result `index_group` and the richer JSON fields — both need `SearchResult` to carry them (J5.2). |
| **J5.7** | **not started** | no per-engine process, no overlap report. |
| **J5.5 tier 3** | fixed (the gate) | the owner asked for a switch for the opt-in-only providers with a legal notice: `[engines] allow_files` plus a per-browser cookie, the control hidden while the server refuses, four-paragraph notice in en/es, six-case test. No provider is registered under it. `docs/J5-opt-in.md` |
| **J6.1** | fixed | query text removed from the log; counts and per-engine breakdown kept. |
| **J6.2** | fixed | `InfoBox.is_html`, set only by local generators; `add_infobox_to_collection` escapes the rest. `{{ }}` verified to escape by default, so `|safe` was the reason. `AT&T` renders `AT&amp;T`. |
| **J6.3** | partly | `%g`→`%.12g` here, but the calculator card and the answer box still format independently. |
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
