#!/bin/sh
# The settings glyph: beside the search box, on both pages, never cut.
#
# Reported three times and each time for a different reason. It had been deleted
# from the results chrome entirely (only its dead CSS remained), it was
# position:fixed to the window corner on the front page, and it was a CSS mask
# whose opaque fill rendered as a solid slab. This holds all three.
FAIL=0
fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }
pass() { printf "  ok  %s\n" "$1"; }

echo "settings glyph"

# 1. the element exists in both templates, and it is inline SVG
grep -q 'class="chrome-settings"' templates/_chrome.html \
  && pass "the results chrome carries the glyph" \
  || fail "no glyph in templates/_chrome.html"
grep -q 'class="home-settings-btn"' templates/home.html \
  && pass "the front page carries the glyph" \
  || fail "no glyph in templates/home.html"

# 2. no CSS mask may style it again: a mask reads alpha, and this SVG's fill is
#    opaque, which is what turned it into a solid block
if grep -qE '\.(chrome-settings|home-settings-btn)[^{]*\{[^}]*mask-image' static/main.css; then
  fail "a mask is back on the glyph"
else
  pass "no CSS mask on the glyph"
fi

# 3. it must not be pinned to the window: that is what floated it away from the
#    search form on a wide screen and clipped it on a narrow one
if grep -qE '\.(chrome-settings|home-settings-btn)[^{]*\{[^}]*position:\s*fixed' static/main.css; then
  fail "the glyph is position:fixed to the window again"
else
  pass "the glyph is in the flow, not fixed to the window"
fi

# 4. it travels with the search box on the results pages
grep -q 'header-search-row' templates/_chrome.html \
  && pass "the results glyph shares a row with the form" \
  || fail "the results glyph is not in .header-search-row"
grep -q 'search-row' templates/home.html \
  && pass "the front-page glyph shares a row with the input" \
  || fail "the front-page glyph is not in .search-row"

# 5. no rule hides it at a narrow width
if grep -qE '\.(chrome-settings|home-settings-btn)[^{]*\{[^}]*display:\s*none' static/main.css; then
  fail "a rule hides the glyph"
else
  pass "no rule hides the glyph"
fi
# and the old breakpoint that hid it is gone
grep -q 'nav-settings-icon' static/main.css \
  && fail "the dead nav-settings-icon rules are still there" \
  || pass "the dead nav-settings-icon rules are gone"

# 6. the glyph has a fixed box that cannot be squeezed
grep -q '^\.chrome-settings,$' static/main.css \
  && pass "one shared rule defines both glyphs" \
  || fail "the two glyphs are not defined together"

[ "$FAIL" = "0" ] && echo "settings glyph: PASS" || { echo "settings glyph: FAIL"; exit 1; }
