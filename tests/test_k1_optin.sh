#!/bin/sh
# K1.5: one opt-in switch per provider, and the switch is what decides whether
# the provider is contacted.
#
# There is no opt-in provider in the shipped build yet (K4 and K5 add them), so
# the test registers one from a definition file - the same path a real one
# takes - and measures it. The proxy records every host asked for, so "not
# contacted" is observed rather than inferred.
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=8151
PROXY_PORT=8894
DIR=/tmp/k1optin
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

rm -rf "$DIR"; mkdir -p "$DIR/defs" "$DIR/cache"
python3 - "$DIR/defs/probe.json" <<'PY'
import json, sys
d = json.load(open('static/engines.d/openlibrary.json'))
d['id'] = 'optin_probe'
d['name'] = 'Opt-in probe'
d['enabled'] = True
d['opt_in'] = True
json.dump(d, open(sys.argv[1], 'w'), indent=2)
PY

cat > "$DIR/t.ini" <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg,optin_probe
allow_files = 1
definitions_dir = $DIR/defs
[health]
breaker_threshold = 1000
pace_requests_per_minute = 6000
pace_burst = 2000
[cache]
dir = $DIR/cache
INI

# a server left behind by an interrupted run would hold the port, the new one
# would fail to bind, and the test would measure the stale one
pkill -9 -f "$DIR/t.ini" 2>/dev/null
# the proxy is started by name, so a leftover from an interrupted run is found
# by name too - its port would otherwise still be held and the new one would
# exit without a word
pkill -9 -f counting_proxy.py 2>/dev/null
# free the proxy port even if the process that held it is not findable by name
# (measured: a leftover held the port, the new proxy exited with "address in
# use", and the test then measured a server that was not using a proxy at all)
free_port() {
    if command -v lsof >/dev/null 2>&1; then
        for p in $(lsof -nP -tiTCP:"$1" -sTCP:LISTEN 2>/dev/null); do
            kill -9 "$p" 2>/dev/null
        done
    fi
}
free_port "$PROXY_PORT"
sleep 0.4

PROXY_LOG="$DIR/proxy.log" PROXY_PORT="$PROXY_PORT" python3 tests/counting_proxy.py > "$DIR/proxy.out" 2>&1 &
PROXY_PID=$!
sleep 0.7
HTTPS_PROXY="http://127.0.0.1:$PROXY_PORT" https_proxy="http://127.0.0.1:$PROXY_PORT" \
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
# our own server, not one left over: its log names the config we just wrote
grep -q "Starting Omnisearch on 127.0.0.1:$PORT" "$DIR/srv.log" || {
    echo "FAIL the server answering on $PORT is not the one this test started"
    exit 1; }

count() { grep -c "^openlibrary.org\$" "$DIR/proxy.log" 2>/dev/null || true; }

# 1. the page offers a per-provider switch, in an Opt-in group
curl -sS -o "$DIR/s.html" -m 20 "http://127.0.0.1:$PORT/settings"
grep -q 'kind-group">.*Opt-in' "$DIR/s.html" 2>/dev/null || \
    python3 - "$DIR/s.html" <<'PY' && say_pass || say_fail "no Opt-in group with a registered provider"
import re, sys
h = open(sys.argv[1]).read()
ok = bool(re.search(r'Opt-in', h)) and 'name="optin_optin_probe"' in h
sys.exit(0 if ok else 1)
PY
grep -q 'name="optin_optin_probe"' "$DIR/s.html" && say_pass || say_fail "the row is not named optin_<id>"

# 2. without the opt-in, naming it must not reach the provider
before=$(count)
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=linux+kernel+alpha&engine=optin_probe&nc=$RANDOM"
after=$(count)
[ "$after" -eq "$before" ] && say_pass || say_fail "not opted in, but openlibrary.org was contacted"

# 3. with the opt-in cookie it is reached
# a different query: the page caches by query, so reusing one would serve the
# second request from the first one's cache and hide whether it was contacted
curl -sS -o /dev/null -m 20 -b "optin=optin_probe" \
     "http://127.0.0.1:$PORT/search?q=linux+kernel+beta&engine=optin_probe&nc=$RANDOM"
after2=$(count)
[ "$after2" -gt "$after" ] && say_pass || say_fail "opted in, but the provider was never contacted"

# 4. the save path writes the list, and an emptied list clears it
curl -sS -o /dev/null -m 20 -D "$DIR/save.h" \
     "http://127.0.0.1:$PORT/save_settings?engines_seen=20&optin_optin_probe=1"
grep -qi "set-cookie: optin=optin_probe" "$DIR/save.h" && say_pass || say_fail "saving did not write optin=<id>"
curl -sS -o /dev/null -m 20 -D "$DIR/save2.h" \
     "http://127.0.0.1:$PORT/save_settings?engines_seen=20&optin_optin_probe=0"
grep -qi "set-cookie: optin=;" "$DIR/save2.h" && say_pass || say_fail "un-ticks did not clear the list"

echo "K1.5 opt-in: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
