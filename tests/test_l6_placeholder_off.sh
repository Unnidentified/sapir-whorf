#!/bin/sh
# L6.9: the "We would like to show you a description here..." placeholder must
# never be rendered, whether or not the description fetcher is running.
#
# The concern is real: the fetcher is a provider with a switch, and the filter
# lives near it in the code, so switching the fetcher off could plausibly have
# taken the filter with it. It does not - the filter is in clean_snippet(), on
# the merge path - but that is a claim, so here it is measured.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8202
DIR=/tmp/l6ph
fail=0; pass=0
ok() { pass=$((pass + 1)); }
bad() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; }
trap cleanup EXIT INT TERM

lsof -nP -iTCP:$PORT -sTCP:LISTEN 2>/dev/null | awk 'NR>1{print $2}' | xargs -r kill -9
rm -rf "$DIR"; mkdir -p "$DIR/cache"
# one engine, served from a fixture: the reply is a page whose snippets are the
# placeholder, so the filter is the only thing standing between it and the page
cat > "$DIR/t.ini" <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg
[answers]
mode = off
[health]
breaker_threshold = 1000
[cache]
dir = $DIR/cache
INI

start() {
    [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
    sleep 0.4
    OMNI_SECRETS_FILE="$DIR/secrets.ini" OMNI_PERF=1 \
        ./bin/omnisearch --config "$DIR/t.ini" > "$DIR/srv-$1.log" 2>&1 &
    SRV=$!
    i=0
    while [ $i -lt 40 ]; do
        curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
        sleep 0.25; i=$((i + 1))
    done
}

# two runs: the description provider on, and off
for mode in on off; do
    start "$mode"
    if [ "$mode" = "off" ]; then
        cookie="engines=off=page_description"
    else
        cookie="engines="
    fi
    # the query is the fixture trigger the replay layer already uses
    curl -sS -o "$DIR/p-$mode.html" -m 25 -b "$cookie" \
        "http://127.0.0.1:$PORT/search?q=linux+kernel+placeholder&nc=$RANDOM"

    if grep -qi "we would like to show you a description" "$DIR/p-$mode.html"; then
        bad "the placeholder rendered with the fetcher $mode"
    else
        ok
    fi
    # and no snippet slot was left holding it in a hidden attribute either
    if grep -qiE 'data-snippet="[^"]*we would like to show' "$DIR/p-$mode.html"; then
        bad "the placeholder reached a data attribute with the fetcher $mode"
    else
        ok
    fi
done

# the unit-level truth too: the predicate itself, straight from the source
if grep -q "snippet_is_placeholder(in)" src/Routes/Search.c; then
    ok
else
    bad "clean_snippet no longer calls the placeholder filter"
fi

echo "L6 placeholder filter: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
