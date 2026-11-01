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

        # 2. L1: a ticked box means ON. Untick two, save, reload, read back.
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        boxes = page.query_selector_all(
            "#engines-form input[type=checkbox][data-provider]:not([disabled])")
        check(len(boxes) >= 2, f"only {len(boxes)} switchable rows on the page")
        chosen = [b.get_attribute("data-provider") for b in boxes[:2]]
        for box in boxes:
            if box.get_attribute("data-provider") in chosen and box.is_checked():
                box.scroll_into_view_if_needed()
                box.click()
        page.click("#engines-form .settings-actions button")
        page.wait_for_timeout(1500)
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        for ident in chosen:
            el = page.query_selector(f'#prov_{ident}')
            check(el is not None and not el.is_checked(),
                  f"{ident} was switched off but came back ticked")
            word = page.query_selector(
                f'#prov_{ident}').evaluate(
                    "e => e.closest('.settings-provider').querySelector('.settings-provider-state').textContent.trim()")
            check(word.startswith("Off"), f"{ident} came back reading {word!r}")
        still_off = [b.get_attribute("id") for b in
                     page.query_selector_all("#engines-form input[type=checkbox][data-provider]")
                     if not b.is_checked() and b.get_attribute("data-provider") not in chosen
                     and not b.is_disabled()]
        check(not still_off, f"providers came back unticked though they were left on: {still_off[:4]}")

        # 2b. L1: the assertion that was missing all along - for every row, the
        #     box agrees with the state word beside it, and an unavailable row
        #     is unticked and disabled. Checked with scripts on and off.
        ROWS_JS = """() => [...document.querySelectorAll('.settings-provider')]
            .map(r => {
                const box = r.querySelector('.settings-checkbox');
                if (!box) return null;
                const word = r.querySelector('.settings-provider-state');
                return {id: box.getAttribute('data-provider'),
                        checked: box.checked, disabled: box.disabled,
                        label: word ? word.textContent.trim() : ''};
            }).filter(Boolean)"""

        def agree(rows, when):
            bad = []
            for r in rows:
                if r["label"].startswith("Unavailable"):
                    if r["checked"] or not r["disabled"]:
                        bad.append((r["id"], "unavailable row is not unticked+disabled"))
                    continue
                if r["label"] == "On" and not r["checked"]:
                    bad.append((r["id"], "says On but the box is empty"))
                if r["label"].startswith("Off") and r["checked"]:
                    bad.append((r["id"], "says Off but the box is ticked"))
            check(not bad, f"box/label disagree with {when}: {bad[:3]}")

        rows = page.evaluate(ROWS_JS)
        agree(rows, "scripts on")
        print(f"rows checked with scripts on: {len(rows)}")

        nojs = browser.new_context(java_script_enabled=False)
        np = nojs.new_page()
        np.goto(BASE + "/settings", wait_until="domcontentloaded")
        rows_off = np.evaluate(ROWS_JS)
        agree(rows_off, "scripts off")
        print(f"rows checked with scripts off: {len(rows_off)}")
        link = np.query_selector(".settings-provider-toggle")
        check(link is not None, "no no-script toggle link on the page")
        if link is not None:
            href = link.get_attribute("href")
            before = link.evaluate("e => e.closest('.settings-provider').querySelector('.settings-provider-state').textContent.trim()")
            np.goto(BASE + href, wait_until="domcontentloaded")
            np.goto(BASE + "/settings", wait_until="domcontentloaded")
            after = np.evaluate(ROWS_JS)
            print("no-script toggle used:", href, "| before:", before)
            agree(after, "after the no-script toggle")
        nojs.close()

        # 2c. L1: every switchable row switched off at once, then all of them
        #     back. The twenty-provider case that the single off= field exists
        #     for (beaker keeps 16 parameters) is asserted in
        #     tests/test_l1_toggle.sh, where the ids need not be clickable.
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        allboxes = page.query_selector_all(
            "#engines-form input[type=checkbox][data-provider]:not([disabled])")
        many = [b.get_attribute("data-provider") for b in allboxes]
        # a real user unticking twenty rows waits for each save to land; the
        # script serialises them, so the test does too - clicking all twenty at
        # once would race the queue, not the server
        page.evaluate("""(ids) => {
            const list = [...document.querySelectorAll('.settings-checkbox[data-provider]')]
                .filter(b => ids.includes(b.getAttribute('data-provider')) && b.checked);
            list.forEach(b => b.click());
        }""", many)
        page.wait_for_timeout(4000)
        page.click("#engines-form .settings-actions button")
        page.wait_for_timeout(1500)
        page.goto(BASE + "/settings", wait_until="domcontentloaded")
        back_on = []
        for ident in many:
            el = page.query_selector("#prov_" + ident)
            if el is None or el.is_checked():
                back_on.append(ident)
        check(not back_on, f"with 20 switched off, these came back on: {back_on[:5]}")
        print(f"all-switchable save: {len(many)} off, {len(back_on)} lost")
        page.evaluate("""(ids) => {
            const list = [...document.querySelectorAll('.settings-checkbox[data-provider]')]
                .filter(b => ids.includes(b.getAttribute('data-provider')) && !b.checked && !b.disabled);
            list.forEach(b => b.click());
        }""", many)
        page.wait_for_timeout(4000)
        page.click("#engines-form .settings-actions button")
        page.wait_for_timeout(1500)


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
