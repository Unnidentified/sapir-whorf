#!/usr/bin/env python3
"""K1.6 in a real browser: saving with the default weights must work, and the
engine checkboxes must round-trip.

The unit tests check the cookie; this one checks what the owner does - click,
save, reload. It needs Playwright, and skips loudly without it (the project does
not depend on it).
"""
import sys
import urllib.parse

BASE = "http://127.0.0.1:8087"


def main():
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("K1 browser check: SKIP -- Playwright is not installed")
        return 0

    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)
            print("FAIL:", what)

    with sync_playwright() as p:
        browser = p.chromium.launch()
        context = browser.new_context()
        page = context.new_page()

        # 1. the page saves with the weights it ships with. Each section is its
        #    own form now (K1.2b: beaker keeps at most 16 parameters), so the
        #    test uses the whole settings form's button.
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        before_url = page.url
        # the save redirects back to /settings?q=, so "did it submit" is "did the
        # URL change", not "does it still say save_settings"
        page.click("#settings-form .settings-actions button")
        page.wait_for_timeout(1500)
        check(page.url != before_url, "the save button did not submit the form")
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        invalid = page.evaluate(
            """() => [...document.querySelectorAll('#settings-form input')]
                     .filter(i => !i.checkValidity()).map(i => i.name)""")
        check(not invalid, f"inputs still invalid after the fix: {invalid[:4]}")

        # 2. tick two providers OFF, save that section, reload, read back.
        #    A ticked box means "off" in the engines form (K1.2b).
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        boxes = page.query_selector_all("#engines-form input[type=checkbox][id^=engine_]")
        chosen = [b.get_attribute("id") for b in boxes[:2]]
        for box in boxes:
            if box.get_attribute("id") in chosen and not box.is_checked():
                box.click()
        page.click("#engines-form .settings-actions button")
        page.wait_for_timeout(1500)
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        for ident in chosen:
            el = page.query_selector(f"#{ident}")
            check(el is not None and el.is_checked(),
                  f"{ident} was switched off but came back on")
        still_on = [b.get_attribute("id") for b in
                    page.query_selector_all("#engines-form input[type=checkbox][id^=engine_]")
                    if b.is_checked() and b.get_attribute("id") not in chosen]
        check(not still_on, f"providers came back off though they were left on: {still_on[:4]}")

        # 3. the cookie really is the disabled set, joined with '|'
        cookies = {c["name"]: c["value"] for c in context.cookies()}
        value = cookies.get("engines", "")
        check(value.startswith("off="), f"engines cookie is not the disabled set: {value!r}")
        check("," not in value, "engines cookie still contains a comma")
        print("engines cookie:", value)

        context.close()
        browser.close()

    print("K1 browser check:", "PASS" if not failures else "FAIL")
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())
