#!/bin/sh
# H4: the answer box modes, and the fan-out skip. The important claim is that a
# confident local answer costs no upstream request - so the assertion is that
# the engine line is absent, not that the page looks right.
#
# J3c reversed the default: a confident local answer now searches the web too,
# unless answers_fanout = never. This test keeps its assertions by asking for
# that mode explicitly; the new default is covered by test_j3_answers.sh.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/omnisearch
[ -x "$BIN" ] || { echo "FAIL: $BIN missing"; exit 1; }

fail=0

run_mode() {
  mode=$1
  port=$2
  cache=/tmp/h4-answers-$mode
  log=/tmp/h4-answers-$mode.log
  rm -rf "$cache"; mkdir -p "$cache"
  cat > /tmp/h4-answers-$mode.ini <<INI
[server]
host = 127.0.0.1
port = $port
answers = $mode
answers_fanout = never
[cache]
dir = $cache
[engines]
engines = "ddg,bing"
INI
  OMNI_PERF=1 $BIN --config /tmp/h4-answers-$mode.ini > "$log" 2>&1 &
  echo $!
}

PID_OFF=$(run_mode off 8088)
PID_LOCAL=$(run_mode local 8087)
sleep 3
# the local instance needs its own port; 8087 is the real service
pkill -f "h4-answers-local.ini" 2>/dev/null
cat > /tmp/h4-answers-local.ini <<INI
[server]
host = 127.0.0.1
port = 8089
answers = local
answers_fanout = never
[cache]
dir = /tmp/h4-answers-local
[engines]
engines = "ddg,bing"
INI
OMNI_PERF=1 $BIN --config /tmp/h4-answers-local.ini > /tmp/h4-answers-local.log 2>&1 &
PID_LOCAL=$!
sleep 2

# answers=off must not show a card at all
curl -sS -o /tmp/h4-off.html -m 30 "http://localhost:8088/search?q=2%2B2"
if grep -q "answer-box-text" /tmp/h4-off.html; then
  echo "FAIL: answers=off still rendered an answer card"; fail=1
fi

# answers=local must answer, and must not ask an engine
curl -sS -o /tmp/h4-local.html -m 30 "http://localhost:8089/search?q=2%2B2"
if ! grep -q "answer-box-text" /tmp/h4-local.html &&
   ! grep -q "widget-calc-result" /tmp/h4-local.html; then
  echo "FAIL: answers=local did not render the answer"; fail=1
fi
if ! grep -q ">4<" /tmp/h4-local.html &&
   ! grep -q 'widget-calc-result">4<' /tmp/h4-local.html; then
  echo "FAIL: answers=local did not answer 2+2 with 4"; fail=1
fi
if grep -q "\[ENGINE\]" /tmp/h4-answers-local.log; then
  echo "FAIL: a confident local answer still queried an engine:"
  grep "\[ENGINE\]" /tmp/h4-answers-local.log | head -2
  fail=1
fi
# and the web=1 override must restore the fan-out
curl -sS -o /tmp/h4-web.html -m 40 "http://localhost:8089/search?q=2%2B2&web=1"
if ! grep -q "\[ENGINE\]" /tmp/h4-answers-local.log; then
  echo "FAIL: web=1 did not re-enable the fan-out"; fail=1
fi

kill $PID_OFF $PID_LOCAL 2>/dev/null
rm -f /tmp/h4-answers-*.ini
if [ "$fail" -eq 0 ]; then
  echo "H4 answers modes: PASS"
else
  exit 1
fi
