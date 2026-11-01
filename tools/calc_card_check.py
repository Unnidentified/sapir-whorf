#!/usr/bin/env python3
"""K9: the calculator card is exactly as wide as its keypad.

The defect this measures: `.widget` was a block card filling the main column,
so at 1280 px the card was ~900 px and the keypad sat in its left 320 px. The
card looked like a marked square with a keypad in the corner.

Uses Playwright when it is available; prints a skip and exits 0 otherwise, like
layout_check.py, because Playwright is not a dependency of this project.

    python3 tools/calc_card_check.py
"""
import sys
import os

URL = os.environ.get("OMNI_BASE", "http://localhost:8087")
WIDTHS = [1280, 1024, 768, 390]


def main():
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("skip: playwright is not installed (pip install playwright)")
        return 0

    failures = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        for width in WIDTHS:
            page = browser.new_page(viewport={"width": width, "height": 900})
            page.goto(f"{URL}/search?q=2%2B2&w=calculator", wait_until="load")
            card = page.query_selector(".widget-calculator")
            keypad = page.query_selector(".widget-calc-keypad")
            if not card or not keypad:
                failures.append(f"{width}: no calculator card on the page")
                page.close()
                continue
            cbox = card.bounding_box()
            kbox = keypad.bounding_box()
            gap = cbox["x"] + cbox["width"] - (kbox["x"] + kbox["width"])
            overflow = page.evaluate(
                "() => { const k = document.querySelector('.widget-calc-keypad');"
                " return k ? k.scrollWidth - k.clientWidth : 0; }")
            print(f"{width:>5}: card {cbox['width']:.0f}px, keypad "
                  f"{kbox['width']:.0f}px, right gap {gap:.0f}px, "
                  f"keypad overflow {overflow}px")
            if abs(gap) > 16:
                failures.append(
                    f"{width}: the card's right edge is {gap:.0f}px away from "
                    f"the keypad's (limit 16)")
            if overflow > 0:
                failures.append(f"{width}: the keypad scrolls horizontally "
                                f"({overflow}px)")
            page.close()
        browser.close()

    if failures:
        for f in failures:
            print(f"FAIL {f}")
        return 1
    print(f"K9 calculator card: {len(WIDTHS)} widths, card matches its keypad")
    return 0


if __name__ == "__main__":
    sys.exit(main())
