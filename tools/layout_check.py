#!/usr/bin/env python3
"""J1: assert the layout at the widths and in the themes the review named.

Wide  -> the panel's left edge is to the right of the results' right edge.
Narrow -> the panel's bottom is above the first result's top.

Uses Playwright if it is installed. It is not a dependency of this project, so
when it is missing the tool prints a clear skip and exits 0 rather than
pretending to have checked anything.

    python3 tools/layout_check.py            # against the running service
    python3 tools/layout_check.py --port 8091
"""
import argparse
import pathlib
import sys

WIDTHS = [1600, 1300, 1241, 1240, 1100, 900, 600, 375]
THEMES = ["amber", "light", "dark", "system"]
BREAKPOINT = 1241

MEASURE = """() => {
  const r = (sel) => {
    const el = document.querySelector(sel);
    if (!el) return null;
    const b = el.getBoundingClientRect();
    return {left: b.left, right: b.right, top: b.top, bottom: b.bottom,
            width: b.width, height: b.height};
  };
  return {
    layout: document.querySelector('.content-layout')
              ? getComputedStyle(document.querySelector('.content-layout')).gridTemplateAreas : null,
    panel: r('.infobox-sidebar'),
    main: r('.results-container'),
    firstResult: r('.results-container .result'),
  };
}"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8087)
    ap.add_argument("--query", default="covid-19")
    args = ap.parse_args()

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("layout_check: SKIP -- Playwright is not installed, so no geometry "
              "was measured.")
        print("layout_check: the CSS invariants that can be checked without a "
              "browser are in tools/check_themes.py (one .content-layout, no "
              "order hacks, no grid-column overrides).")
        print("layout_check: install with `pip install playwright && "
              "playwright install chromium` to enable the geometry assertions.")
        return 0

    failures = 0
    shots = pathlib.Path(__file__).resolve().parent.parent / "docs" / "layout"
    shots.mkdir(parents=True, exist_ok=True)
    url = f"http://127.0.0.1:{args.port}/search?q={args.query}"

    with sync_playwright() as p:
        browser = p.chromium.launch()
        for theme in THEMES:
            context = browser.new_context(viewport={"width": 1600, "height": 1000})
            context.add_cookies([{"name": "theme", "value": theme,
                                  "url": f"http://127.0.0.1:{args.port}"}])
            page = context.new_page()
            for width in WIDTHS:
                page.set_viewport_size({"width": width, "height": 1000})
                page.goto(url, wait_until="networkidle")
                m = page.evaluate(MEASURE)
                if not m["panel"] or not m["firstResult"]:
                    print(f"SKIP {theme} {width}: no panel or no results")
                    continue
                if width >= BREAKPOINT:
                    ok = m["panel"]["left"] >= m["main"]["right"] - 1
                    what = f"panel left {m['panel']['left']:.0f} vs results right {m['main']['right']:.0f}"
                else:
                    ok = m["panel"]["bottom"] <= m["firstResult"]["top"] + 1
                    what = f"panel bottom {m['panel']['bottom']:.0f} vs first result top {m['firstResult']['top']:.0f}"
                print(("ok   " if ok else "FAIL ") + f"{theme:8s} {width:5d}  {what}")
                if not ok:
                    failures += 1
            page.set_viewport_size({"width": 1241, "height": 1000})
            page.goto(url, wait_until="networkidle")
            page.screenshot(path=str(shots / f"{theme}-1241.png"), full_page=False)
            page.set_viewport_size({"width": 1240, "height": 1000})
            page.goto(url, wait_until="networkidle")
            page.screenshot(path=str(shots / f"{theme}-1240.png"), full_page=False)
            page.set_viewport_size({"width": 900, "height": 1000})
            page.goto(url, wait_until="networkidle")
            page.screenshot(path=str(shots / f"{theme}-900.png"), full_page=False)
            context.close()
        browser.close()

    print(f"\nlayout_check: {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
