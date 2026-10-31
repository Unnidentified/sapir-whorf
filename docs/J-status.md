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
| **J1 (measured)** | **not done** | `tools/layout_check.py` prints a skip: Playwright is not installed, so no width/theme was measured and `docs/layout/` is empty. |
| **J2** | fixed | `--surface` existed in no theme (the white card); nine semantic tokens derived in `:root`; no `style='…'` left in `src/Infobox/*.c` except the `--swatch` custom property; icon tiles dropped. Three lints green on 16 themes, WCAG contrast included (`catppuccin-latte` and `light` failed first and the token derivation was fixed, not the threshold). `docs/J1-J2-layout-theme.md` |
| **J2 (contact sheet)** | **not done** | `tools/theme_shot.py` prints the same Playwright skip; `docs/themes/` is empty. |
| **J4.0** | **not started** | no widget layer, no `templates/widgets/`, no `/w/<kind>` endpoints. `static/widgets.js` exists but only closes the compact panel. |
| **J4a** | **error handling only** | `evaluate()` returns NaN on a failed or partial parse, `1/0` is `Infinity`, `-2^2` is `-4`, `%.12g` in the step list. 13-expression table in `tests/test_g4_answerbox.c`. No keypad, no functions, no card. |
| **J4b, J4c, J4d** | **not started** | no translation, map or stock code, and no paced provider probes were run. |
| **J5.0–5.5** | **not started** | still 13 registry entries; no `static/engines.d`, no tabs beyond All/Images, no rich results. |
| **J5.6** | **partly** | buffer 128→1024 and the config line buffer 1024→4096 with a loud rejection; bang→name map with dedupe (`!go,!ddg,br,qw` → `google,duckduckgo,brave,qwant`); startup probe for `format=json` and `/config` name validation, measured against a fake instance. **Not done:** per-result `index_group` and the richer JSON fields — both need `SearchResult` to carry them (J5.2). |
| **J5.7** | **not started** | no per-engine process, no overlap report. |
| **J6.1** | fixed | query text removed from the log; counts and per-engine breakdown kept. |
| **J6.2** | **not started** | `{{info[2]|safe}}` still marks every panel body as safe. |
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
