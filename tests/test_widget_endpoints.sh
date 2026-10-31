#!/bin/bash
# J4.0: /w/<kind> serves one card and nothing else, and the keypad's one
# character per request works through it exactly as it does through the page.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8127
DIR=/tmp/widg
rm -rf "$DIR" && mkdir -p "$DIR"
cat > /tmp/widg.ini <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg
definitions_dir = static/engines.d
[cache]
dir = $DIR
INI
pkill -9 -f "widg.ini" 2>/dev/null
sleep 0.5
./bin/omnisearch --config /tmp/widg.ini > /tmp/widg.log 2>&1 &
sleep 3

FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
BASE="http://127.0.0.1:$PORT"

# a fragment is a fragment: it must be a card, not a page
body=$(curl -sS -m 20 "$BASE/w/calculator?q=2%2B2")
case "$body" in
  *"<!DOCTYPE"*|*"<html"*) fail "the calculator fragment is a whole page" ;;
esac
case "$body" in
  *"widget-calculator"*) : ;;
  *) fail "no calculator card in the fragment" ;;
esac
echo "$body" | grep -q 'widget-calc-result">4' || fail "2+2 did not evaluate to 4"

# the keypad protocol, one character per request
curl -sS -o /tmp/widg1.html -m 20 "$BASE/w/calculator?q=5&key=7"
grep -q 'widget-calc-expr">57' /tmp/widg1.html || fail "key 7 did not append"
grep -q 'widget-calc-result">57' /tmp/widg1.html || fail "5 then 7 did not evaluate"

# /w/calculator is explicit mode, the same as ?w=calculator on the page: it
# renders the card even mid-entry, which is what makes the keypad usable. The
# *page* is what decides a query is a search, so covid-19 has no card there.
curl -sS -o /tmp/widg4.html -m 20 "$BASE/w/calculator?q=covid-19"
grep -q 'widget-calculator' /tmp/widg4.html || fail "explicit fragment rendered no card"
curl -sS -o /tmp/widg5.html -m 30 "$BASE/search?q=covid-19"
grep -q 'widget-calculator' /tmp/widg5.html && fail "covid-19 still gets a card on the page"

# a kind with no matching request refuses rather than rendering an empty card
code=$(curl -sS -o /dev/null -w '%{http_code}' -m 20 "$BASE/w/place?q=hello")
[ "$code" = "204" ] || fail "a non-place query returned $code, expected 204"

# and the two network kinds answer when the query is theirs
curl -sS -o /tmp/widg2.html -m 30 "$BASE/w/place?q=where+is+Madrid"
grep -q 'widget-place' /tmp/widg2.html || fail "no place card"
grep -q 'widget-title">Place' /tmp/widg2.html || fail "locale string did not render (got a key?)"

# an unknown kind is a 404, not a fallback
code=$(curl -sS -o /dev/null -w '%{http_code}' -m 20 "$BASE/w/nonsense?q=2%2B2")
[ "$code" = "404" ] || fail "unknown kind returned $code, expected 404"

# the page still works without this endpoint being involved
curl -sS -o /tmp/widg3.html -m 30 "$BASE/search?q=2%2B2&w=calculator&key%3D1=1"
grep -q 'widget-calculator' /tmp/widg3.html || fail "the page lost its card"

pkill -9 -f "widg.ini" 2>/dev/null
[ "$FAIL" = "0" ] && echo "J4.0 widget endpoints: PASS" || { echo "J4.0 widget endpoints: FAIL"; exit 1; }
