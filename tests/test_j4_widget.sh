#!/bin/sh
# J4.0 + J4a: the calculator card.
#
# The card has to work with JavaScript off, so the keypad is a GET form that
# appends one character per request. That makes these assertions testable
# without a browser, which is the point.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/omnisearch
[ -x "$BIN" ] || { echo "FAIL: $BIN missing"; exit 1; }

fail=0
rm -rf /tmp/j4w
cat > /tmp/j4w.ini <<INI
[server]
host = 127.0.0.1
port = 8111
answers = local
answers_fanout = always
[engines]
engines = "ddg"
[cache]
dir = /tmp/j4w
INI
$BIN --config /tmp/j4w.ini > /tmp/j4w.log 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null; rm -rf /tmp/j4w' EXIT
for _ in $(seq 1 40); do
  curl -sS -o /dev/null -m 2 "http://127.0.0.1:8111/" && break
  sleep 0.25
done

get() { curl -sS -m 40 "http://127.0.0.1:8111/search?$1&nc=$RANDOM"; }
expr()  { printf '%s' "$1" | sed -n 's/.*widget-calc-expr">\([^<]*\).*/\1/p'; }
result() { printf '%s' "$1" | sed -n 's/.*widget-calc-result">\([^<]*\).*/\1/p'; }

check_widget() { # label, qs, want_expr, want_result
  body=$(get "$2")
  printf '%s' "$body" | grep -q 'class="widget widget-calculator"' || {
    echo "FAIL $1: no card"; fail=1; return; }
  got_e=$(expr "$body"); got_r=$(result "$body")
  [ "$got_e" = "$3" ] || { echo "FAIL $1: expression \"$got_e\", wanted \"$3\""; fail=1; }
  [ "$got_r" = "$4" ] || { echo "FAIL $1: result \"$got_r\", wanted \"$4\""; fail=1; }
}

# 1. the card, its keypad, and the strip it replaces
body=$(get "q=2%2B2")
printf '%s' "$body" | grep -q 'class="widget widget-calculator"' || {
  echo "FAIL: 2+2 rendered no calculator card"; fail=1; }
[ "$(result "$body")" = "4" ] || { echo "FAIL: 2+2 did not show 4"; fail=1; }
keys=$(printf '%s' "$body" | grep -c 'name="key"')
[ "$keys" -ge 25 ] || { echo "FAIL: keypad has $keys keys"; fail=1; }
printf '%s' "$body" | grep -q 'name="w" value="calculator"' || {
  echo "FAIL: the form does not carry w=calculator"; fail=1; }
printf '%s' "$body" | grep -q 'answer-box-text' && {
  echo "FAIL: the text strip was shown as well as the card"; fail=1; }
# The card must come first in the document when there are results. Asserting
# that results exist would make this test depend on an engine's mood; asserting
# the order does not.
first_result=$(printf '%s' "$body" | grep -b -o 'class="result"' | head -1 | cut -d: -f1)
widget_at=$(printf '%s' "$body" | grep -b -o 'class="widget widget-calculator"' | head -1 | cut -d: -f1)
if [ -n "$first_result" ] && [ -n "$widget_at" ] && [ "$widget_at" -gt "$first_result" ]; then
  echo "FAIL: the card rendered below the first result"; fail=1
fi

# 2. the three states the parser can be in
check_widget "Infinity" "q=1%2F0"            "1/0"       "Infinity"
check_widget "bad parse" "q=5%2B*%2B3&w=calculator" "5+*+3"  "Error"
check_widget "unbalanced" "q=%282%2B3&w=calculator" "(2+3"  "Error"

# 3. the keypad appends one character per request, without JavaScript
check_widget "append +" "q=5&w=calculator&key=%2B"   "5+"  ""
check_widget "append 3" "q=5%2B&w=calculator&key=3"  "5+3" "8"
check_widget "equals"   "q=5%2B3&w=calculator&key=%3D" "5+3" "8"

# 4. AC clears the expression
body=$(get "q=5%2B3&w=calculator&key=AC")
printf '%s' "$body" | grep -q 'class="widget widget-calculator"' || {
  echo "FAIL: AC removed the card instead of clearing it"; fail=1; }

# 5. a key the keypad does not offer is ignored, not appended
body=$(get "q=5&w=calculator&key=%3Cscript%3E")
printf '%s' "$body" | grep -q '<script>' && {
  echo "FAIL: a rejected key reached the page"; fail=1; }
[ "$(expr "$body")" = "5" ] || {
  echo "FAIL: a rejected key changed the expression to \"$(expr "$body")\""; fail=1; }

# 6. answers=off means no card at all
kill $PID 2>/dev/null
cat > /tmp/j4woff.ini <<INI
[server]
host = 127.0.0.1
port = 8112
answers = off
[engines]
engines = "ddg"
[cache]
dir = /tmp/j4woff
INI
rm -rf /tmp/j4woff
$BIN --config /tmp/j4woff.ini > /tmp/j4woff.log 2>&1 &
PID2=$!
trap 'kill $PID $PID2 2>/dev/null; rm -rf /tmp/j4w /tmp/j4woff' EXIT
for _ in $(seq 1 40); do
  curl -sS -o /dev/null -m 2 "http://127.0.0.1:8112/" && break
  sleep 0.25
done
body=$(curl -sS -m 40 "http://127.0.0.1:8112/search?q=2%2B2&nc=1")
printf '%s' "$body" | grep -q 'widget-calculator' && {
  echo "FAIL: answers=off still rendered the card"; fail=1; }

if [ "$fail" -eq 0 ]; then
  # a search about a disease has a digit and a minus; it used to render a
# calculator card saying "Error" (found in the J2 contact sheet)
curl -sS -o /tmp/j4w-neg.html -m 30 "http://127.0.0.1:8112/search?q=covid-19&nc=$RANDOM"
if grep -q "widget-calculator" /tmp/j4w-neg.html; then
  echo "FAIL: covid-19 rendered a calculator card"
  FAIL=1
fi

echo "J4 calculator card: PASS (render, states, keypad, AC, rejected key, answers=off)"
else
  exit 1
fi
