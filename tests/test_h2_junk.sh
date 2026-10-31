#!/bin/sh
# H2: a JUNK answer is its own outcome. HTTP 200, ten results, none of them
# relevant - the fake upstream serves exactly that, so this is deterministic.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/omnisearch
[ -x "$BIN" ] || { echo "FAIL: $BIN missing"; exit 1; }

PORT=8093
CACHE=/tmp/h2-junk-cache
MODE=/tmp/h2-fake.mode
LOG=/tmp/h2-junk.log
rm -rf "$CACHE"; mkdir -p "$CACHE"
echo junk > "$MODE"

FAKE_MODE_FILE="$MODE" FAKE_PORT=8898 python3 tests/fake_upstream.py &
FAKE_PID=$!
cat > /tmp/h2-junk.ini <<INI
[server]
host = 127.0.0.1
port = $PORT
[cache]
dir = $CACHE
[engines]
engines = "searxng"
searxng_url = http://127.0.0.1:8898
INI
OMNI_PERF=1 $BIN --config /tmp/h2-junk.ini > "$LOG" 2>&1 &
SRV_PID=$!
sleep 3

curl -sS -o /dev/null -m 30 "http://localhost:$PORT/search?q=linux+kernel" 2>/dev/null
sleep 1

fail=0
if ! grep -q "\[JUNK\] searxng returned 10" "$LOG"; then
  echo "FAIL: no JUNK outcome for ten irrelevant results"
  grep -E "FUNNEL|ENGINE" "$LOG" | head -3
  fail=1
fi
JUNK=$(curl -sS -m 5 "http://127.0.0.1:$PORT/metrics" 2>/dev/null |
       sed -n 's/^omnisearch_engine_junk{engine="searxng"} \([0-9]*\)$/\1/p')
if [ "${JUNK:-0}" != "1" ]; then
  echo "FAIL: metrics junk gauge is '${JUNK:-missing}', want 1"
  fail=1
fi
# and the same results must NOT be reported as OK
OK=$(curl -sS -m 5 "http://127.0.0.1:$PORT/metrics" 2>/dev/null |
     sed -n 's/^omnisearch_engine_ok{engine="searxng"} \([0-9]*\)$/\1/p')
if [ "${OK:-0}" != "1" ]; then
  echo "FAIL: engine_ok is '${OK:-missing}' - the engine did answer, but junk"
  fail=1
fi

kill $SRV_PID $FAKE_PID 2>/dev/null
rm -f /tmp/h2-junk.ini
if [ "$fail" -eq 0 ]; then
  echo "H2 junk outcome: PASS"
else
  exit 1
fi
