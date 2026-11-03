#!/usr/bin/env python3
"""Validate every theme in static/themes/.

The UI reads nine CSS variables; a theme that omits one, or that has a
malformed declaration, silently renders with fallback colours (this bit us
twice: a missing semicolon in catppuccin-latte and --text-dim missing from
six themes). Run from `make check` or directly.
"""
import re
import sys
from pathlib import Path

REQUIRED = ["--bg-main", "--bg-card", "--border", "--text-primary",
            "--text-secondary", "--text-muted", "--accent", "--accent-glow",
            "--text-dim"]

COLOUR = re.compile(r"^#[0-9a-fA-F]{3,8}$|^rgba?\([\d\s.,%]+\)$|^hsla?\([\d\s.,%]+\)$")
HEX = re.compile(r"#(?:[0-9a-fA-F]{3}){1,2}\b")

# J2: the tokens a component may reference. Everything here is either one of
# the nine base tokens, a semantic token derived in main.css :root, or a
# property set at runtime. --surface was used for a year without existing in
# any theme, and the #fff fallback is what made the answer card white on dark.
SEMANTIC = ["--surface", "--surface-2", "--on-accent", "--link", "--link-visited",
            "--pos", "--neg", "--focus", "--map-filter", "--rail-width",
            "--rail-breakpoint", "--page-gutter", "--page-scale", "--swatch"]


def rel_luminance(value, tokens=None):
    """Resolve a token value and return its relative luminance (WCAG 2.x)."""
    r, g, b = _resolve(value, tokens)
    def channel(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.03928 else ((c + 0.055) / 1.055) ** 2.4
    return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)


def contrast(a, b, tokens=None):
    tokens = tokens or {}
    la, lb = rel_luminance(a, tokens), rel_luminance(b, tokens)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def _parse(value):
    value = value.strip()
    m = HEX.match(value)
    if m and m.start() == 0:
        h = m.group(0)[1:]
        if len(h) == 3:
            h = "".join(c * 2 for c in h)
        return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
    m = re.match(r"rgba?\(([\d\s.,%]+)\)", value)
    if m:
        parts = [p for p in re.split(r"[,\s]+", m.group(1).strip()) if p]
        if len(parts) >= 3:
            return tuple(int(float(p.replace("%", ""))) for p in parts[:3])
    return None


def _resolve(value, tokens=None, depth=0):
    """Resolve a token value to rgb: hex, rgb(), var(--x) or color-mix(),
    with the mix percentages actually applied, so the contrast number means
    something. Cycles and unknown names fall back to mid grey."""
    tokens = tokens or {}
    value = (value or "").strip()
    if depth > 6:
        return (128, 128, 128)
    m = re.match(r"^var\(\s*(--[a-z0-9-]+)\s*\)$", value)
    if m:
        return _resolve(tokens.get(m.group(1), ""), tokens, depth + 1)
    m = re.match(r"color-mix\(\s*in\s+srgb\s*,\s*(.+?)\s+(\d+(?:\.\d+)?)%\s*,"
                 r"\s*(.+?)\s*\)$", value)
    if m:
        a = _resolve(m.group(1), tokens, depth + 1)
        b = _resolve(m.group(3), tokens, depth + 1)
        p = float(m.group(2)) / 100.0
        return tuple(int(round(a[i] * p + b[i] * (1 - p))) for i in range(3))
    m = re.match(r"color-mix\(", value)
    if m:
        parts = [x for x in re.findall(r"(#[0-9a-fA-F]{3,8}|rgba?\([^)]*\))", value)]
        a = _resolve(parts[0], tokens, depth + 1) if parts else (128, 128, 128)
        b = _resolve(parts[1], tokens, depth + 1) if len(parts) > 1 else a
        return tuple((a[i] + b[i]) // 2 for i in range(3))
    return _parse(value) or (128, 128, 128)


def check_tokens_referenced(root):
    """Lint (a): every var(--x) in main.css and the templates must resolve."""
    css = (root / "static" / "main.css").read_text(encoding="utf-8")
    defined = set(re.findall(r"(--[a-z0-9-]+)\s*:", css))
    themes = sorted((root / "static" / "themes").glob("*.css"))
    theme_tokens = {}
    for theme in themes:
        theme_tokens[theme.name] = set(re.findall(r"(--[a-z0-9-]+)\s*:", theme.read_text(encoding="utf-8")))

    used = set(re.findall(r"var\(\s*(--[a-z0-9-]+)", css))
    for tpl in (root / "templates").rglob("*.html"):
        used |= set(re.findall(r"var\(\s*(--[a-z0-9-]+)", tpl.read_text(encoding="utf-8")))

    problems = []
    known = defined | set(SEMANTIC) | set(REQUIRED)
    for var in sorted(used):
        if var in known:
            continue
        problems.append(
            f"{var} is used but not defined in main.css :root and not a base token")
    # a theme may not reference a token it did not define itself
    for name, tokens in theme_tokens.items():
        text = (root / "static" / "themes" / name).read_text(encoding="utf-8")
        for var in sorted(set(re.findall(r"var\(\s*(--[a-z0-9-]+)", text))):
            if var not in tokens and var not in known:
                problems.append(f"{name} references undefined {var}")
    return problems


def strip_comments(text, kind):
    """Blank out comments but keep the line structure, so reported line
    numbers still point at the real file."""
    def blank(m):
        return "\n" * m.group(0).count("\n")
    text = re.sub(r"/\*.*?\*/", blank, text, flags=re.S)
    if kind == "c":
        text = re.sub(r"//[^\n]*", "", text)
    return text


def check_no_literal_colour(root):
    """Lint (b): colours live in tokens, not in C or in the middle of a rule."""
    problems = []
    for src in sorted((root / "src" / "Infobox").glob("*.c")):
        text = strip_comments(src.read_text(encoding="utf-8"), "c")
        # line numbers shift when comments are removed; report the text instead
        for i, line in enumerate(text.splitlines(), 1):
            s = line.strip()
            if not s:
                continue
            if "style='" in s and "--swatch" not in s:
                problems.append(f"{src.name}:{i} inline style: {s[:70]}")
            for m in HEX.finditer(s):
                problems.append(f"{src.name}:{i} literal colour {m.group(0)}")
            # a literal rgb() in C only matters if it reaches the page as a
            # fixed colour, and anything that does carries style= above.
            if re.search(r"\brgba?\(", s) and 'style=' in s:
                problems.append(f"{src.name}:{i} literal rgb() in emitted style")

    css = strip_comments((root / "static" / "main.css").read_text(encoding="utf-8"), "css")
    for i, line in enumerate(css.splitlines(), 1):
        s = line.strip()
        # token definitions are where colours are allowed to be literal
        if not s or s.startswith("--"):
            continue
        if "var(--swatch" in s:
            continue
        for m in HEX.finditer(s):
            problems.append(f"main.css:{i} literal colour {m.group(0)}")
        if re.search(r"\brgba?\(", s) and "color-mix" not in s:
            problems.append(f"main.css:{i} literal rgb()")
    return problems


ROOT_TOKENS = {}


def check_form_control_tokens(root):
    """Lint (c): form controls use theme tokens (K2).

    A number input with no rule is drawn white by the browser on every dark
    theme, and `background: white`/`#fff`/`Canvas` paints the same box
    regardless of the palette. Require the global rule in main.css and reject
    those literals anywhere under static/."""
    problems = []
    css = strip_comments((root / "static" / "main.css").read_text(encoding="utf-8"), "css")
    m = re.search(r"(?m)^\s*input\s*,\s*select\s*,\s*textarea\s*\{([^}]*)\}", css)
    if not m:
        problems.append("main.css: missing the global `input, select, textarea` rule (K2)")
    else:
        for prop, want in (
                ("background", "var(--surface-2)"),
                ("color", "var(--text-primary)"),
                ("border", "1px solid var(--border)"),
                ("border-radius", "0"),
                ("color-scheme", "inherit")):
            if not re.search(rf"(?m)^\s*{re.escape(prop)}\s*:\s*{re.escape(want)}\s*;",
                             m.group(1)):
                problems.append(
                    f"main.css: global input rule needs `{prop}: {want}` (K2)")

    for path in sorted((root / "static").rglob("*")):
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        if path.suffix == ".css":
            text = strip_comments(text, "css")
        for i, line in enumerate(text.splitlines(), 1):
            s = line.strip()
            if not s:
                continue
            # one declaration at a time: `background: #fff`, `background: white`
            literal_bg = re.search(
                r"background(?:-color)?\s*:[^;{}]*(?:#fff(?:fff)?\b|\bwhite\b)", s,
                re.IGNORECASE)
            # `Canvas` is a system colour whatever property it appears on
            canvas = re.search(r"\bCanvas\b", s)
            if literal_bg or canvas:
                problems.append(
                    f"{path.relative_to(root)}:{i} hard-coded white/Canvas: {s[:70]}")
    return problems


def check_layout_once(root):
    """J1: .content-layout must be defined in one place.

    It was in seven, with conflicting grid, padding and ordering. At <=1240px
    the last one won and put the rail after the pager, which is the reported
    bug. A second definition is now a test failure, not a judgement call."""
    css = strip_comments((root / "static" / "main.css").read_text(encoding="utf-8"), "css")
    problems = []
    blocks = len(re.findall(r"(?m)^\s*\.content-layout\s*\{", css))
    if blocks != 1:
        problems.append(f".content-layout is defined {blocks} times (want exactly 1)")
    for dead in (r"\.content-layout[^,{]*\{[^}]*grid-column", r"(?<![a-z-])order:\s*-?\d"):
        for m in re.finditer(dead, css):
            line = css[:m.start()].count("\n") + 1
            problems.append(f"main.css:{line} layout still uses {m.group(0)[:40]!r}")

    # A grid-template-* declaration in a rule that never sets display:grid is
    # inert, and the element silently falls back to block flow. That shipped:
    # .content-layout had grid-template-areas "main rail" and display:block, so
    # the rail sat below the results at every width while the CSS looked
    # correct. tools/layout_check.py caught it by measuring; this catches it
    # without a browser.
    for m in re.finditer(r"([^{}]+)\{([^{}]*)\}", css):
        selector, body = m.group(1).strip().splitlines()[-1].strip(), m.group(2)
        if "grid-template" not in body:
            continue
        if re.search(r"display\s*:\s*(grid|inline-grid)", body):
            continue
        # A compound selector inherits the display of its base class
        # (".result.result-with-thumb" from ".result"), and a media query may
        # set it in another rule for the same selector. Only flag a base class
        # that never gets display:grid anywhere.
        base = selector.split()[0].split(":")[0].split(".")[1]
        base = "." + base if base else selector
        escaped = re.escape(base)
        elsewhere = re.findall(rf"{escaped}\s*(?:,[^{{}}]*)?\{{([^{{}}]*)", css)
        if any(re.search(r"display\s*:\s*(grid|inline-grid)", b) for b in elsewhere):
            continue
        line = css[:m.start()].count("\n") + 1
        problems.append(
            f"main.css:{line} '{selector[:40]}' sets grid-template-* with no "
            f"display:grid, so the grid is inert")
    return problems


def check_contrast(root):
    """J2 task 5: text on --bg-card >= 4.5, --pos/--neg on --bg-card >= 3.
    Report the themes that fail; never weaken the check to make them pass."""
    global ROOT_TOKENS
    css = (root / "static" / "main.css").read_text(encoding="utf-8")
    block = css.split(":root {", 1)[1].split("}", 1)[0]
    ROOT_TOKENS = dict(re.findall(r"(--[a-z-]+)\s*:\s*([^;]+);", block))
    failures = []
    for theme in sorted((root / "static" / "themes").glob("*.css")):
        tokens = dict(re.findall(r"(--[a-z-]+)\s*:\s*([^;]+);", theme.read_text(encoding="utf-8")))
        card = tokens.get("--bg-card", "").strip()
        if not card:
            continue
        merged = {**ROOT_TOKENS, **tokens}
        text = tokens.get("--text-primary", "").strip()
        if text:
            ratio = contrast(text, card, merged)
            if ratio < 4.5:
                failures.append(f"{theme.name}: --text-primary on --bg-card is {ratio:.2f} (< 4.5)")
        for name, want in (("--pos", 3.0), ("--neg", 3.0)):
            # derived in main.css :root when the theme does not override it
            value = tokens.get(name, "").strip() or ROOT_TOKENS.get(name, "")
            if not value:
                continue
            ratio = contrast(value, card, merged)
            if ratio < want:
                failures.append(f"{theme.name}: {name} on --bg-card is {ratio:.2f} (< {want})")
    return failures

def check(path):
    text = path.read_text(encoding="utf-8")
    problems = []

    for line in text.splitlines():
        s = line.strip()
        if s.startswith("--") and not s.endswith(";") and ":" in s:
            problems.append(f"missing semicolon: {s}")

    found = dict(re.findall(r"(--[a-z-]+)\s*:\s*([^;]+);", text))
    for var in REQUIRED:
        if var not in found:
            problems.append(f"missing variable {var}")
        elif not COLOUR.match(found[var].strip()):
            problems.append(f"invalid colour for {var}: {found[var].strip()}")

    if text.count("{") != text.count("}"):
        problems.append("unbalanced braces")

    return problems


def check_button_family(root):
    """M3: .btn-primary and .btn-secondary are defined once, at top level.

    They used to live inside a dozen scopes (.settings-actions, .view-home,
    .settings-colours-actions, .settings-account, ...), each section copying the
    last because the class was not global. A button in a section that had not
    been added to the list silently fell through to the generic input rule -
    which is why the Accounts Save buttons were not in the palette. A scoped
    definition is now a failure."""
    css = strip_comments((root / "static" / "main.css").read_text(encoding="utf-8"), "css")
    problems = []
    # a selector block that mentions a btn class but is scoped by a prefix
    for m in re.finditer(r"(?m)^([^{}\n]*\.btn-(?:primary|secondary)[^{}\n]*)\{", css):
        selector = m.group(1).strip()
        line = css[:m.start()].count("\n") + 1
        for part in selector.split(","):
            part = part.strip()
            if not part:
                continue
            if part.startswith(".btn-primary") or part.startswith(".btn-secondary"):
                continue
            if ":hover" in part or ":active" in part or ":focus" in part or ":disabled" in part:
                continue  # state rules on the family itself are fine
            # A scoped rule that only adjusts type or spacing is fine: the family
            # owns the paint, a section may ask for slightly smaller type.
            body_m = re.match(r"[^{}]*\{([^}]*)\}", css[m.end() - 1:])
            body = body_m.group(1) if body_m else ""
            if body and not any(
                    re.search(rf"(?m)^\s*{k}[\w-]*\s*:", body)
                    for k in ("background", "border", "color", "box-shadow")):
                continue
            problems.append(
                f"main.css:{line} scoped button definition {part[:50]!r} "
                f"(define it once, at top level)")
    # An ancestor selector that repaints buttons also beats the family on
    # specificity: `.view-home button` (0,1,1) outranks `.btn-primary` (0,1,0),
    # which is how the front page's Search button lost its fill and its edge and
    # read as loose text. An ancestor may set cursor, spacing or type size; it
    # must not set background, border or colour on a button.
    PAINT = ("background", "border", "color")
    for m in re.finditer(r"(?m)^([^{}\n]*\bbutton\b[^{}\n]*)\{([^}]*)\}", css):
        selector = m.group(1).strip()
        body = m.group(2)
        line = css[:m.start()].count("\n") + 1
        if ".btn-" in selector:
            continue
        if not any(re.search(rf"(?m)^\s*{k}[\w-]*\s*:", body) for k in PAINT):
            continue
        problems.append(
            f"main.css:{line} {selector[:46]!r} repaints buttons, which overrides "
            f"the .btn-* family on specificity (set only cursor/size here)")

    # A button that acts on a form - Save, Test, Finish, Clear - must use the
    # family. Widget keys, the calculator keypad and the history clear control
    # are their own controls by design and are not in scope.
    ACTION_WORDS = ("save", "test", "finish", "clear", "authorise", "authorize")
    stray = []
    for tpl in sorted((root / "templates").glob("*.html")):
        text = tpl.read_text(encoding="utf-8")
        for m in re.finditer(r"<button\b[^>]*>([^<]*)", text):
            tag, label = m.group(0), m.group(1)
            # a button with its own styled class is its own control; only one
            # with no class at all falls through to the generic rule
            if 'class=' in tag:
                continue
            blob = (tag + " " + label).lower()
            if any(w in blob for w in ACTION_WORDS):
                line = text[:m.start()].count("\n") + 1
                stray.append(f"{tpl.name}:{line} action button without the "
                             f"button family: {tag[:50]}")
    if stray:
        problems.extend(stray[:6])
    return problems

def main():
    root = Path(__file__).resolve().parent.parent
    themes = sorted((root / "static" / "themes").glob("*.css"))
    if not themes:
        print("no themes found", file=sys.stderr)
        return 1

    failed = 0
    for theme in themes:
        problems = check(theme)
        if problems:
            failed += 1
            print(f"FAIL {theme.name}")
            for p in problems:
                print(f"     {p}")
        else:
            print(f"ok   {theme.name}")

    print(f"\n{len(themes) - failed}/{len(themes)} themes valid")

    lint_failed = 0
    for name, problems in (
            ("tokens referenced", check_tokens_referenced(root)),
            ("no literal colours", check_no_literal_colour(root)),
            ("form controls", check_form_control_tokens(root)),
            ("one layout", check_layout_once(root)),
            ("one button family", check_button_family(root)),
            ("contrast", check_contrast(root))):
        if problems:
            lint_failed += 1
            print(f"\nFAIL {name}")
            for p in problems[:25]:
                print(f"     {p}")
            if len(problems) > 25:
                print(f"     ... and {len(problems) - 25} more")
        else:
            print(f"ok   {name}")

    return 1 if (failed or lint_failed) else 0

if __name__ == "__main__":
    sys.exit(main())
