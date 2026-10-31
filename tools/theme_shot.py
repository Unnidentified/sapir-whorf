#!/usr/bin/env python3
"""J2: the same page in every theme, as one contact sheet.

The acceptance question is "no white card on a dark theme, nothing
unreadable". That needs pixels, so this uses Playwright if present and prints a
clear skip otherwise (it is not a dependency of the project).
"""
import argparse
import pathlib
import sys


def themes(root):
    return sorted(p.stem for p in (root / "static" / "themes").glob("*.css"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8087)
    ap.add_argument("--query", default="covid-19")
    args = ap.parse_args()

    root = pathlib.Path(__file__).resolve().parent.parent
    names = themes(root)
    out = root / "docs" / "themes"
    out.mkdir(parents=True, exist_ok=True)

    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("theme_shot: SKIP -- Playwright is not installed, so no contact sheet was made.")
        print(f"theme_shot: it would have shot {len(names)} themes: {' '.join(names)}")
        print("theme_shot: the token checks that do not need a browser are in "
              "tools/check_themes.py (tokens resolve, no literal colours, WCAG contrast).")
        return 0

    shots = []
    with sync_playwright() as p:
        browser = p.chromium.launch()
        for name in names:
            ctx = browser.new_context(viewport={"width": 1300, "height": 1000})
            ctx.add_cookies([{"name": "theme", "value": name,
                              "url": f"http://127.0.0.1:{args.port}"}])
            page = ctx.new_page()
            page.goto(f"http://127.0.0.1:{args.port}/search?q={args.query}",
                      wait_until="networkidle")
            path = out / f"{name}.png"
            page.screenshot(path=str(path))
            shots.append(path)
            ctx.close()
        browser.close()

    print(f"theme_shot: {len(shots)} screenshots in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
