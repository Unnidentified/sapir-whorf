#!/usr/bin/env python3
"""J1: measure the layout at the widths and in the themes the review named.

The invariants are about the relationship, not about which side the panel is on
- an earlier version of this file asserted "panel to the right, and above when
narrow", and it reported 32 failures against a layout that was correct in the
opposite order. What actually matters:

  no overlap   at every width, the panel and the results must not intersect
  wide         they are side by side: separated horizontally
  narrow       they are stacked: separated vertically

The measured numbers are printed so a failure says what happened rather than
only that something did.

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


def overlap(a, b, tolerance=1.0):
    """True when the two boxes intersect by more than `tolerance` pixels."""
    horizontal = min(a["right"], b["right"]) - max(a["left"], b["left"])
    vertical = min(a["bottom"], b["bottom"]) - max(a["top"], b["top"])
    return horizontal > tolerance and vertical > tolerance


def side_by_side(a, b):
    return a["right"] <= b["left"] + 1 or b["right"] <= a["left"] + 1


def stacked(a, b):
    return a["bottom"] <= b["top"] + 1 or b["bottom"] <= a["top"] + 1


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
        print("layout_check: to measure it: python3 -m venv /tmp/pw && "
              "/tmp/pw/bin/pip install playwright && "
              "/tmp/pw/bin/python -m playwright install chromium, then "
              "make layout-check PYTHON=/tmp/pw/bin/python")
        print("layout_check: note for the record -- the browsers that ship on "
              "this machine (Brave/Chrome app bundles) cannot run headless in "
              "this environment; Playwright's own Chromium can, and that is how "
              "the numbers in docs/layout/ were produced.")
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
                panel, main = m["panel"], m["main"]
                problems = []
                if overlap(panel, main):
                    problems.append(
                        f"overlap x={min(panel['right'], main['right']) - max(panel['left'], main['left']):.0f}"
                        f" y={min(panel['bottom'], main['bottom']) - max(panel['top'], main['top']):.0f}")
                if width >= BREAKPOINT:
                    if not side_by_side(panel, main):
                        problems.append(
                            f"not side by side: panel x {panel['left']:.0f}-{panel['right']:.0f}, "
                            f"results x {main['left']:.0f}-{main['right']:.0f}")
                else:
                    if not stacked(panel, main):
                        problems.append(
                            f"not stacked: panel y {panel['top']:.0f}-{panel['bottom']:.0f}, "
                            f"results y {main['top']:.0f}-{main['bottom']:.0f}")
                ok = not problems
                detail = (
                    f"panel {panel['width']:.0f}x{panel['height']:.0f} at "
                    f"({panel['left']:.0f},{panel['top']:.0f}), results "
                    f"{main['width']:.0f}x{main['height']:.0f}, areas="
                    f"{str(m['layout'])[:34]}"
                )
                print(("ok   " if ok else "FAIL ") + f"{theme:8s} {width:5d}  {detail}"
                      + ("" if ok else "  <- " + "; ".join(problems)))
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
