#!/bin/bash
# J5.5: per-engine weights edited in the browser.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8135
DIR=/tmp/j5w
rm -rf "$DIR" && mkdir -p "$DIR"
cat > /tmp/j5w.ini <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg,bing
[cache]
dir = $DIR
INI
pkill -9 -f "j5w.ini" 2>/dev/null
sleep 0.5
./bin/omnisearch --config /tmp/j5w.ini > /tmp/j5w.log 2>&1 &
sleep 3
FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
BASE="http://127.0.0.1:$PORT"

# the editor exists, one input per enabled engine
curl -sS -o /tmp/j5w.html -m 20 "$BASE/settings"
grep -q 'name="w_ddg"' /tmp/j5w.html || fail "no weight input for ddg"
grep -q 'min="0.5" max="5"' /tmp/j5w.html || fail "the input is unbounded"
grep -q 'weights_present' /tmp/j5w.html || fail "no presence flag, so a save cannot tell"

# a save stores them, and refuses values outside the range
curl -sS -o /dev/null -m 20 -D /tmp/j5w.h \
  "$BASE/save_settings?weights_present=1&w_ddg=2.5&w_bing=9&w_nope=abc"
grep -qi 'set-cookie: engine_weights=ddg:2.5' /tmp/j5w.h \
  || fail "the valid weight was not stored"
grep -qi 'bing' /tmp/j5w.h && fail "an out-of-range weight was stored"
grep -qi 'nope' /tmp/j5w.h && fail "an unparseable weight was stored"

# and the page shows back what routing will use
curl -sS -o /tmp/j5w2.html -m 20 -b "engine_weights=ddg:2.5" "$BASE/settings"
grep -q 'name="w_ddg" value="2.5"' /tmp/j5w2.html \
  || fail "the saved weight is not shown back"
grep -q 'name="w_bing" value="0.95"' /tmp/j5w2.html \
  || fail "an engine without an override lost its configured weight"

# a save that does not carry the editor must not wipe the reader's policy
curl -sS -o /dev/null -m 20 -D /tmp/j5w3.h "$BASE/save_settings?theme=amber"
grep -qi 'set-cookie: engine_weights' /tmp/j5w3.h \
  && fail "an unrelated save cleared the weights"

pkill -9 -f "j5w.ini" 2>/dev/null
[ "$FAIL" = "0" ] && echo "J5.5 weight editor: PASS" || { echo "J5.5 weight editor: FAIL"; exit 1; }
