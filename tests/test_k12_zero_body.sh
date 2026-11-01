#!/bin/sh
# K12.6: a 200 with an empty body is a block signal, not a clean zero-result
# search.
#
# Yahoo answers site:reddit.com with an empty body when it decides not to serve
# the query. That used to be recorded as "no results": no block signal, nothing
# for the breaker, and the very next request asked again. The test drives the
# empty body through the fake upstream and asserts three things: the engine is
# marked blocked, the health table says why, and the next query does not reach
# the upstream at all.
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=8165
UPSTREAM_PORT=8890
DIR=/tmp/k12zero
BIN=./bin/omnisearch
fail=0
pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    [ -n "${UP_PID:-}" ] && kill "$UP_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

rm -rf "$DIR"; mkdir -p "$DIR/cache"
echo zero > "$DIR/mode"
: > "$DIR/hits"

free_port() {
    if command -v lsof >/dev/null 2>&1; then
        for p in $(lsof -nP -tiTCP:"$1" -sTCP:LISTEN 2>/dev/null); do
            kill -9 "$p" 2>/dev/null
        done
    fi
}
free_port "$UPSTREAM_PORT"
sleep 0.4

FAKE_MODE_FILE="$DIR/mode" FAKE_HITS="$DIR/hits" FAKE_PORT="$UPSTREAM_PORT" \
    python3 tests/fake_upstream.py > "$DIR/upstream.out" 2>&1 &
UP_PID=$!
sleep 0.7

cat > "$DIR/t.ini" <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = searxng
searxng_url = http://127.0.0.1:$UPSTREAM_PORT
[health]
breaker_threshold = 1000
pace_requests_per_minute = 6000
pace_burst = 2000
[cache]
dir = $DIR/cache
INI

pkill -9 -f "$DIR/t.ini" 2>/dev/null
sleep 0.4
$BIN --config "$DIR/t.ini" > "$DIR/srv.log" 2>&1 &
SRV_PID=$!
i=0
while [ $i -lt 40 ]; do
    curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
    sleep 0.25
    i=$((i + 1))
done
curl -sS -m 2 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 || {
    echo "FAIL server did not start"; tail -5 "$DIR/srv.log"; exit 1; }

# 1. the empty body is recorded as a block, not as an empty search
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=site%3Areddit.com+probe&nc=$RANDOM"
curl -sS -o "$DIR/status.json" -m 20 "http://127.0.0.1:$PORT/settings/engine-status.json"
python3 - "$DIR/status.json" <<'PY' && say_pass || say_fail "the empty body was not recorded as a block"
import json, sys
d = json.load(open(sys.argv[1]))
e = [x for x in d['engines'] if x['id'] == 'searxng']
if not e:
    sys.exit(1)
x = e[0]
# blocked counts, and a suspension with the challenge reason
assert x['blocked'] >= 1, x
assert x['why'] == 'challenged', x
assert x['resume_at'] > 0, x
PY

# 2. the breaker now holds: the next query does not reach the upstream
before=$(wc -l < "$DIR/hits" 2>/dev/null | tr -d ' ')
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=site%3Areddit.com+again&nc=$RANDOM"
after=$(wc -l < "$DIR/hits" 2>/dev/null | tr -d ' ')
if [ "$after" -eq "$before" ]; then say_pass; else
    say_fail "the suspended engine was contacted again ($before -> $after)"
fi

echo "K12.6 zero-byte body: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
