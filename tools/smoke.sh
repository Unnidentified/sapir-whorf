#!/usr/bin/env bash
# Fast end-to-end check against a running instance.
#   ./tools/smoke.sh [base-url]      default http://localhost:8087
set -uo pipefail

BASE="${1:-http://localhost:8087}"
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); printf '  ok   %s\n' "$1"; }
bad()  { FAIL=$((FAIL+1)); printf '  FAIL %s\n' "$1"; }

# body_for <path> -> stdout, sets CODE
body_for() {
  BODY=$(curl -sS -m 45 -w '\n%{http_code}' "$BASE$1" 2>/dev/null)
  CODE="${BODY##*$'\n'}"
  BODY="${BODY%$'\n'*}"
}

expect_status() {
  local path="$1" want="${2:-200}"
  body_for "$path"
  if [ "$CODE" = "$want" ]; then ok "$path -> $CODE"; else bad "$path -> $CODE (want $want)"; fi
}

expect_body() {
  local path="$1" needle="$2" label="$3"
  body_for "$path"
  if printf '%s' "$BODY" | grep -q "$needle"; then ok "$label"; else bad "$label (missing: $needle)"; fi
}

# any theme has to render, so pick the default one
THEME=$(curl -sS -m 10 "$BASE/settings" 2>/dev/null | grep -oE '<option value="[a-z-]+"( selected)?>' | head -1 | sed -E 's/.*value="([a-z-]+)".*/\1/')
THEME="${THEME:-amber}"

echo "smoke test against $BASE (theme: $THEME)"

for p in / /history /settings /opensearch.xml /info \
         /static/main.css /static/autocomplete.js /static/history.js \
         /static/favicon.svg /static/settings.svg "/static/themes/$THEME.css"; do
  expect_status "$p"
done

expect_status "/search?q=linux"
expect_body   "/search?q=linux" 'class="result"\|no-results' "results page renders hits or empty state"
expect_body   "/search?q=linux" 'nav-tabs'          "shared chrome rendered"
expect_body   "/search?q=linux" 'nav-settings-icon' "settings icon present"
expect_status "/search?q=linux&p=2"
expect_status "/images?q=cat"
expect_body   "/images?q=cat" 'image-grid' "images grid renders"

# branding: every page must carry the sp-engine tab title
for path in / /history /settings; do
  body_for "$path"
  if printf '%s' "$BODY" | tr -d '\n' | grep -q '<title>[[:space:]]*sw'; then
    ok "tab title sw on $path"
  else
    bad "tab title sw on $path"
  fi
done

expect_body "/info" '"engine_health\|"health"' "engine health exposed in /info"

echo
printf '%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
