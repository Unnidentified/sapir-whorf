#!/bin/sh
# K5.4: when does the Reddit lane run?
#
# It used to require that the reader had never saved a provider choice
# (user_engine_count == 0), which meant the owner - who has saved settings -
# never got a Reddit result. The provider toggle is the real gate now, and the
# triggers are the discussion shape, the word "reddit", an r/<sub> token and a
# typed site:reddit.com operator.
#
# The outbound requests go to a proxy that makes none, so the test is offline;
# the evidence is the lane's own log line.
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=8167
PROXY_PORT=8888
DIR=/tmp/k5lane
BIN=./bin/omnisearch
fail=0
pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    [ -n "${PROXY_PID:-}" ] && kill "$PROXY_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

rm -rf "$DIR"; mkdir -p "$DIR/cache"

free_port() {
    if command -v lsof >/dev/null 2>&1; then
        for p in $(lsof -nP -tiTCP:"$1" -sTCP:LISTEN 2>/dev/null); do
            kill -9 "$p" 2>/dev/null
        done
    fi
}
free_port "$PROXY_PORT"
free_port "$PORT"
sleep 0.5

printf '[server]\nport = %s\nhost = 127.0.0.1\n[engines]\nengines = ddg,bing,yahoo,brave\n[health]\nbreaker_threshold = 1000\npace_requests_per_minute = 6000\npace_burst = 2000\n[cache]\ndir = %s/cache\n' \
    "$PORT" "$DIR" > "$DIR/t.ini"

PROXY_LOG="$DIR/proxy.log" PROXY_PORT="$PROXY_PORT" python3 tests/counting_proxy.py > "$DIR/proxy.out" 2>&1 &
PROXY_PID=$!
sleep 0.7

OMNI_PERF=1 HTTPS_PROXY="http://127.0.0.1:$PROXY_PORT" \
HTTPS_PROXY="http://127.0.0.1:$PROXY_PORT" \
https_proxy="http://127.0.0.1:$PROXY_PORT" \
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

lanes() { grep -c "\[F3\] reddit lane" "$DIR/srv.log" 2>/dev/null || true; }

# $1 query, $2 cookie ("" for none), $3 expected new lane lines (0 or 1), $4 why
check() {
    local_q=$1
    local_cookie=${2:-}
    local_want=$3
    local_why=$4
    before=$(lanes)
    if [ -n "$local_cookie" ]; then
        curl -sS -o /dev/null -m 20 -b "$local_cookie" \
             "http://127.0.0.1:$PORT/search?q=$local_q&nc=$RANDOM" 2>/dev/null
    else
        curl -sS -o /dev/null -m 20 \
             "http://127.0.0.1:$PORT/search?q=$local_q&nc=$RANDOM" 2>/dev/null
    fi
    sleep 0.3
    after=$(lanes)
    got=$((after - before))
    if [ "$got" -eq "$local_want" ]; then
        say_pass
    else
        say_fail "$local_why: expected $local_want lane(s), saw $got"
    fi
}

check "linux+kernel+probe1" "" 0 "a plain query must not start the lane"
# K5.2 problem 2: one engine wide is not a lane. Two site:-capable engines are
# asked per query, so the lane survives one of them being rate-limited.
check "best+rust+ide+reddit+probe2" "" 2 "the word reddit (two engines)"
check "r%2Frust+ide+probe3" "" 2 "an r/<sub> token"
check "site%3Areddit.com+rust+probe4" "" 2 "a typed site:reddit.com operator"
# L6.1: "r/" matched inside words, so each of these bought two lane requests
# nobody asked for. None of them is a subreddit.
check "and%2For+probe9" "" 0 "and/or is not a subreddit"
check "color%2Fcolour+probe10" "" 0 "color/colour is not a subreddit"
check "hammer%2Fanvil+probe11" "" 0 "hammer/anvil is not a subreddit"
check "either%2For+probe12" "" 0 "either/or is not a subreddit"
check "index.php%3Fpage%3D2+probe13" "" 0 "a URL with r/ buried in it is not a subreddit"
# K12.4: a saved provider choice must not disable the lane
check "best+rust+ide+reddit+probe5" "engines=off=brave" 2 "a saved provider choice"
# ...but switching the lane engines off must
check "best+rust+ide+probe6" "engines=off=ddg|yahoo|bing|brave" 0 \
      "every lane engine switched off"
check "best+rust+ide+reddit+probe7" "engines=off=reddit" 0 \
      "the Reddit provider switched off on its own"
# one of the two lane engines switched off: the lane still runs, on the others,
# and never through the one that is off
check "best+rust+ide+reddit+probe8" "engines=off=ddg" 2 \
      "DDG switched off, the lane runs on the others"
if tail -2 "$DIR/srv.log" | grep -q "reddit lane via ddg"; then
    say_fail "DDG is off, but the lane asked DDG"
else
    say_pass
fi

echo "K5.4 reddit lane: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
