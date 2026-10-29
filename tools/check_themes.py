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
    return 1 if failed else 0

if __name__ == "__main__":
    sys.exit(main())
