#!/bin/sh
# J3c: results belong under the answer. Reverses H4's default: a confident
# local answer no longer skips the fan-out unless answers_fanout says so.
#
# The H4 assertions live on in the `never` mode below, so both behaviours are
# still covered.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/omnisearch
[ -x "$BIN" ] || { echo "FAIL: $BIN missing"; exit 1; }

fail=0
PIDS=""

start() { # mode-port-name, port, fanout
  name=$1
  port=$2
  fanout=$3
  rm -rf "/tmp/j3b-$name"
  cat > "/tmp/j3b-$name.ini" <<INI
[server]
host = 127.0.0.1
port = $port
answers = local
answers_fanout = $fanout
[cache]
dir = /tmp/j3b-$name
[engines]
engines = "ddg,bing"
INI
  OMNI_PERF=1 $BIN --config "/tmp/j3b-$name.ini" > "/tmp/j3b-$name.log" 2>&1 &
  PIDS="$PIDS $!"
}

# "ever" is not a real answer-engine name: use ddg only so one engine line is
# enough to prove the fan-out ran.
start always 8096 always
start never 8097 never
start auto 8098 auto
sleep 4

# 1. always (the new default): the answer AND results, one upstream engine
curl -sS -o /tmp/j3b-always.html -m 60 "http://localhost:8096/search?q=2%2B2"
grep -q "answer-box-text" /tmp/j3b-always.html || {
  echo "FAIL: answers_fanout=always dropped the answer"; fail=1; }
grep -q ">4<" /tmp/j3b-always.html || {
  echo "FAIL: answers_fanout=always did not answer 2+2 with 4"; fail=1; }
grep -q 'class="result"' /tmp/j3b-always.html || {
  echo "FAIL: answers_fanout=always rendered no results under the answer"; fail=1; }
grep -q "\[ENGINE\]" /tmp/j3b-always.log || {
  echo "FAIL: answers_fanout=always did not query an engine"; fail=1; }

# 2. never: the H4 behaviour, kept as an option
curl -sS -o /tmp/j3b-never.html -m 40 "http://localhost:8097/search?q=2%2B2"
grep -q "answer-box-text" /tmp/j3b-never.html || {
  echo "FAIL: answers_fanout=never dropped the answer"; fail=1; }
grep -q 'class="result"' /tmp/j3b-never.html && {
  echo "FAIL: answers_fanout=never still searched the web"; fail=1; }
grep -q "\[ENGINE\]" /tmp/j3b-never.log && {
  echo "FAIL: answers_fanout=never still queried an engine"; fail=1; }

# 3. auto: wordless certain answer skips, a query with words does not
curl -sS -o /dev/null -m 40 "http://localhost:8098/search?q=2%2B2"
grep -q "\[ENGINE\]" /tmp/j3b-auto.log && {
  echo "FAIL: answers_fanout=auto searched for a wordless answer"; fail=1; }
curl -sS -o /dev/null -m 60 "http://localhost:8098/search?q=12%20inches%20in%20cm"
grep -q "\[ENGINE\]" /tmp/j3b-auto.log || {
  echo "FAIL: answers_fanout=auto skipped a query that has words in it"; fail=1; }

# 4. the per-user cookie overrides the server default
curl -sS -o /tmp/j3b-cookie.html -m 40 -H "Cookie: answers_fanout=never" \
  "http://localhost:8096/search?q=3%2B3"
grep -q 'class="result"' /tmp/j3b-cookie.html && {
  echo "FAIL: the answers_fanout cookie did not override the server default"; fail=1; }

# 5. an unknown value is ignored, not stored
curl -sS -o /dev/null -m 10 -D /tmp/j3b-hdr.txt \
  "http://localhost:8096/save_settings?answers=bogus&answers_fanout=bogus"
grep -qi "set-cookie: answers=" /tmp/j3b-hdr.txt && {
  echo "FAIL: save_settings accepted an unknown answers value"; fail=1; }

for p in $PIDS; do kill "$p" 2>/dev/null; done
rm -f /tmp/j3b-*.ini
if [ "$fail" -eq 0 ]; then
  echo "J3c answers_fanout: PASS (always, never, auto, cookie override, validation)"
else
  exit 1
fi
