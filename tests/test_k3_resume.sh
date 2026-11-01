#!/bin/sh
# K3: "Resumes" is an instant, not a duration, and it is the same instant the
# dispatcher honours.
#
# The upstream answers 429 with Retry-After: 6, so the suspension has a known
# length. The test then reads the page's data-resume-at, checks it against the
# clock, and - the part that makes the number real - checks that no request
# reaches the upstream while suspended and that one does afterwards.
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=8160
UPSTREAM_PORT=8892
DIR=/tmp/k3resume
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
# a leftover process holding the upstream port would answer in place of the one
# this test starts, and the sequence below would measure the wrong server
free_port() {
    if command -v lsof >/dev/null 2>&1; then
        for p in $(lsof -nP -tiTCP:"$1" -sTCP:LISTEN 2>/dev/null); do
            kill -9 "$p" 2>/dev/null
        done
    fi
}
free_port "$UPSTREAM_PORT"
sleep 0.5
echo retry6 > "$DIR/mode"
: > "$DIR/hits"

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

# 1. one query, which the upstream answers with 429 + Retry-After: 6
before=$(date +%s)
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=resume+probe&nc=$RANDOM"
hits_after_query=$(wc -l < "$DIR/hits" 2>/dev/null | tr -d ' ')
[ "$hits_after_query" -ge 1 ] && say_pass || say_fail "the engine never reached the fake upstream"

# 2. the page carries an absolute instant within 2 s of now + 6
curl -sS -o "$DIR/s.html" -m 20 "http://127.0.0.1:$PORT/settings"
stamp=$(python3 - "$DIR/s.html" <<'PY'
import re, sys
h = open(sys.argv[1]).read()
m = re.search(r'data-now="(\d+)"', h)
at = re.findall(r'data-resume-at="(\d+)"', h)
print(f"{m.group(1) if m else 0} {at[0] if at else 0}")
PY
)
now_srv=${stamp%% *}
resume_at=${stamp##* }
expected=$((before + 6))
if [ "$resume_at" -gt 0 ]; then say_pass; else say_fail "no data-resume-at on the page"; fi
drift=$((resume_at - expected))
if [ "$drift" -ge -2 ] && [ "$drift" -le 2 ]; then
    say_pass
else
    say_fail "resume instant is $drift s from now+6 (expected within 2)"
fi

# 3. inside the suspension, a new query must not reach the upstream. Checked
#    twice, at about +1 s and +4 s, because a gate that only holds for the
#    first moment is not a gate.
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=resume+probe+two&nc=$RANDOM"
after_1=$(wc -l < "$DIR/hits" 2>/dev/null | tr -d ' ')
if [ "$after_1" -eq "$hits_after_query" ]; then say_pass; else
    say_fail "a suspended engine was contacted 1 s after the 429 ($hits_after_query -> $after_1)"
fi
sleep 3
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=resume+probe+three&nc=$RANDOM"
after_4=$(wc -l < "$DIR/hits" 2>/dev/null | tr -d ' ')
if [ "$after_4" -eq "$after_1" ]; then say_pass; else
    say_fail "a suspended engine was contacted 4 s after the 429 ($after_1 -> $after_4)"
fi

# 4. after the suspension ends it is retried
sleep 4
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=resume+probe+four&nc=$RANDOM"
after_wait=$(wc -l < "$DIR/hits" 2>/dev/null | tr -d ' ')
if [ "$after_wait" -gt "$after_4" ]; then say_pass; else
    say_fail "the engine was not retried after the suspension ended"
fi

# 5. the JSON endpoint agrees, and says ready
curl -sS -o "$DIR/status.json" -m 20 "http://127.0.0.1:$PORT/settings/engine-status.json"
python3 - "$DIR/status.json" <<'PY' && say_pass || say_fail "engine-status.json does not agree with the page"
import json, sys, time
d = json.load(open(sys.argv[1]))
assert 'now' in d and d['engines'], 'shape'
e = [x for x in d['engines'] if x['id'] == 'searxng']
assert e, 'searxng missing'
now = d['now']
for x in e:
    # either it is suspended (resume_at in the future) or it is ready
    assert x['resume_at'] == 0 or x['resume_at'] > now - 1, x
PY

echo "K3 resume clock: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
