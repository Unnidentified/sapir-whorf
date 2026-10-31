# J1 + J2 — the rail, the palette, and the two lints

## J1: one layout

`.content-layout` was defined in seven places. The one that won at ≤1240 px set
`order: 0` on both children, overriding an earlier `order: -1`, so the rail
grid-placed itself **after** the pager. That is the reported behaviour, and it
fired on half a laptop screen.

Now:

- **One definition**, near the end of `main.css`, using `grid-template-areas`:
  `"rail" "main"` as the base (narrow: the panel above the results) and
  `"main rail"` at `min-width: 1241px`. No `order`, no `grid-column`.
- The three media-query blocks that used to redeclare `.content-layout` to
  change padding and gap now set `--layout-gap`, `--layout-gutter` and
  `--layout-block`, which the single block consumes.
- Deleted: the dead feed-layout grid (`1fr 37%`, 16.6 % gutters — overridden at
  equal specificity, never applied), `.sidebar-spacer` and its markup, the
  `order: -1` rule, and the `order: 0` rule.
- **Compact panel** below the breakpoint: a `<details class="panel-compact">`
  whose summary is the first panel's title and a "more" control. It is `open`
  in the markup — without JavaScript that is the only complete default — and
  `static/widgets.js` (deferred, 30 lines, no third party) closes it below the
  breakpoint. Above the breakpoint the summary is hidden by CSS.
- Spacing that used to be `order`/`padding` per breakpoint is now three
  variables, so the lint can require exactly one definition.

**The `zoom` bug, which made this worse than it looked.**
`get_page_scale()` returns `90` by default, and every template set
`style="zoom: 90%"` on `<html>`. `zoom` scales the layout but *not* the
viewport that media queries measure, so at a 1240 px window the content
occupied 1378 px — room for two columns — while the query said one column. The
rail dropped 11 % too early on every page, by default.

Replaced with `--page-scale` (0.9) feeding `html { font-size: ... }` and the
layout tokens. The breakpoints now measure what they think they measure.

Lint (`tools/check_themes.py`, check "one layout"): fails if `.content-layout`
is defined other than once, if it still uses `grid-column`, or if anything
still uses `order:`.

## J2: the palette

1. **`--surface` did not exist.** `.answer-box` was `var(--surface, #fff)`, so
   the `#fff` fallback always won and the card was white on every dark theme
   while the text inherited `--text-primary`. That is the pale `2` in the
   owner's screenshot. It is now `var(--bg-card)` with `var(--link)` links and
   a visited state.
2. **Semantic tokens** in `:root`, derived so no theme has to change:
   `--surface`, `--surface-2`, `--on-accent`, `--link`, `--link-visited`,
   `--pos`, `--neg`, `--focus`, `--map-filter`.
   `--pos`/`--neg` are mixed with the theme's own `--text-primary`
   (`color-mix(in srgb, #4fbf7b 55%, var(--text-primary))`), which darkens them
   on a light card and brightens them on a dark one — that is what makes both
   light themes pass the contrast check.
3. **No inline style in `src/Infobox/*.c`.** `Calculator`, `UnitConversion`,
   `CurrencyConversion`, `Dictionary` and `ColourCode` emitted `style='…'` with
   `#eee`, `#ddd`, `#666`, `#888`, `#555` and fixed layout. They emit classes
   now (`calc-steps`, `conv-primary`, `dict-word`, `colour-hex`, …) and the
   rules live in the panels section of `main.css`. The one dynamic value — a
   colour swatch — travels as `style='--swatch:#rrggbb'`, which the stylesheet
   consumes, so a strict `style-src 'self'` can allow exactly that shape.
4. **Icon tiles dropped.** `calculation.svg` was a hard-coded `#3498db` glyph on
   white and `dictionary.jpg` a 1200×1200 raster; both were reused as the
   thumbnail for currency and units. An `<img>` cannot take `currentColor`, and
   these are cards now, so the thumbnail is empty and `img[src=""]` hides it.
5. **Lints** (`make check-themes`), all passing on 16 themes:
   - *tokens referenced*: every `var(--x)` in `main.css` and the templates
     resolves to a `:root` definition, one of the nine base tokens, or a
     declared semantic token. This is the lint that would have caught
     `--surface`.
   - *no literal colours*: no hex or `rgb()` in `src/Infobox/*.c`, and none in
     `main.css` outside token definitions. Comments are blanked first (keeping
     line numbers), because the old greys are mentioned in the comments
     explaining this.
   - *contrast*: `--text-primary` on `--bg-card` ≥ 4.5, `--pos`/`--neg` on
     `--bg-card` ≥ 3, resolving `var()` and `color-mix()` percentages so the
     number is real. Two themes failed on the first run
     (`catppuccin-latte` 2.05, `light` 2.20) and the token derivation was
     changed rather than the threshold.

```
16/16 themes valid
ok   tokens referenced
ok   no literal colours
ok   one layout
ok   contrast
```

## What is NOT measured

`tools/layout_check.py` (the eight widths × four themes geometry assertions) and
`tools/theme_shot.py` (the contact sheet) both need Playwright, which is not
installed here, so **both print a skip**. No geometry was measured in a browser
and no contact sheet exists: `docs/layout/` and `docs/themes/` are empty. The
CSS invariants above are checked by text, which is weaker than the review asked
for; it is stated here rather than implied.

The headless Chromium on this machine (`Brave Origin Nightly`) hangs on
`--dump-dom` with a virtual time budget, so it was not used either.


## Measured, 2026-10-31

The earlier note in this file said the browsers here cannot run headless, so the
geometry was unverified. That was wrong in an important way: the **app-bundle**
browsers cannot, but Playwright's own Chromium can.

```
python3 -m venv /tmp/pw && /tmp/pw/bin/pip install playwright
/tmp/pw/bin/python -m playwright install chromium
make layout-check PYTHON=/tmp/pw/bin/python
```

32 checks, 4 themes x 8 widths (1600, 1300, 1241, 1240, 1100, 900, 600, 375),
**0 failures**, 12 screenshots in `docs/layout/`. The invariants are
relationship-based, not side-based: no overlap at any width, side by side above
1241, stacked below it. An earlier version of the tool asserted "panel to the
right, above when narrow" and reported 32 false failures against a correct
layout in the opposite order.

What the measurement found, that no amount of reading the CSS did:

`.content-layout` declared `grid-template-columns` and `grid-template-areas`
but **never set `display: grid`**. The declarations were inert, the children
fell back to block flow, and the rail rendered *below* the results at 1600px
while the stylesheet looked right. `tools/check_themes.py` now fails any
`grid-template-*` whose base class never sets `display: grid`, and the check was
proven by deleting the line and watching it fire.

The contact sheet (16 themes into `docs/themes/`) showed the same page in every
theme — and caught a second defect: `covid-19` rendered a **calculator card
saying "Error"**, because the trigger accepted any query with a digit and a
minus. Every alphabetic run must now be a known function or constant.
