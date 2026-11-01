#!/bin/sh
# K7 step 2: the fetch path itself - robots, the budget and the signed link.
#
# A local server stands in for a result site. OMNI_FETCH_ALLOW_LOOPBACK=1 is the
# harness's own switch (the socket guard blocks loopback in production, which is
# the point of it) and nothing else in this test depends on the flag.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=${PORT:-8191}
DIR=/tmp/k7fetch
fail=0; pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }
cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; }
trap cleanup EXIT INT TERM

pkill -9 -f "k7srv.py $PORT" 2>/dev/null
rm -rf "$DIR"; mkdir -p "$DIR/cache"
python3 /tmp/k7srv.py "$PORT" > "$DIR/upstream.log" 2>&1 &
SRV=$!
sleep 1

cat > "$DIR/t.ini" <<INI
[server]
port = 8192
host = 127.0.0.1
[engines]
engines = ddg
[answers]
mode = all
[cache]
dir = $DIR/cache
INI
pkill -9 -f "$DIR/t.ini" 2>/dev/null
OMNI_FETCH_ALLOW_LOOPBACK=1 ./bin/omnisearch --config "$DIR/t.ini" > "$DIR/srv.log" 2>&1 &
APP=$!
i=0
while [ $i -lt 40 ]; do
    curl -sS -m 1 "http://127.0.0.1:8192/info" >/dev/null 2>&1 && break
    sleep 0.25; i=$((i + 1))
done

# the endpoint refuses an unsigned or forged link
code=$(curl -sS -o "$DIR/out.txt" -m 10 -w '%{http_code}' \
       "http://127.0.0.1:8192/d?u=aHR0cDovL2V4YW1wbGUuY29tL21ldGEuaHRtbA&s=deadbeef")
[ "$code" = "204" ] && say_pass || say_fail "unsigned link returned $code, expected 204"

# and with the provider switched off
code=$(curl -sS -o /dev/null -m 10 -w '%{http_code}' -b "engines=off=descriptions" \
       "http://127.0.0.1:8192/d?u=x&s=y")
[ "$code" = "204" ] && say_pass || say_fail "provider off returned $code, expected 204"

# the library half, against the same local server
K7_LIVE_BASE="http://127.0.0.1:$PORT" OMNI_FETCH_ALLOW_LOOPBACK=1 \
    ./tests/test_k7_descriptions > "$DIR/lib.out" 2>&1
if [ $? -eq 0 ]; then
    say_pass
else
    say_fail "library fetch: $(tail -2 "$DIR/lib.out" | tr '\n' ' ')"
fi

kill "$APP" 2>/dev/null
[ "$fail" -eq 0 ] && echo "K7 fetch: PASS ($pass checks)" || { echo "K7 fetch: FAIL"; exit 1; }
