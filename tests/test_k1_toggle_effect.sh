#!/bin/sh
# K1.4: a toggle must change what leaves the machine.
#
# The measurement is a proxy that makes no requests and records every host it
# was asked to reach, so the test is offline and sees what was *attempted*
# rather than what came back.
#
# For every provider the build knows about:
#   on          a query that would use it must ask for its host, then the same
#               query with that provider off must not.
#   unavailable naming it explicitly must still ask for nothing. A provider the
#               server has switched off is not a provider that is merely hidden.
#   local       has no host and no switch; asserted to have no host at all.
#
# Step "on" matters as much as the off case: without it, a provider that is
# unreachable for an unrelated reason would pass the off case for the wrong
# reason.
set -u

PORT=8140
PROXY_PORT=8898
DIR=/tmp/k1effect
LOG="$DIR/proxy.log"
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
# breaker_threshold high: every query must reach every engine, so a provider
# that stopped being asked cannot be mistaken for one the breaker opened
printf '[server]\nport = %s\nhost = 127.0.0.1\n[engines]\nengines = ddg,bing,yahoo,brave,openalex,crossref,arxiv,stackexchange,hn,wikipedia\ndefinitions_dir = static/engines.d\n[answers]\nmode = all\n[health]\nbreaker_threshold = 1000\npace_requests_per_minute = 6000\npace_burst = 2000\n[cache]\ndir = %s/cache\n' "$PORT" "$DIR" > "$DIR/t.ini"

PROXY_LOG="$LOG" PROXY_PORT="$PROXY_PORT" python3 tests/counting_proxy.py > "$DIR/proxy.out" 2>&1 &
PROXY_PID=$!
sleep 0.7

OMNI_PERF=1 \
HTTPS_PROXY="http://127.0.0.1:$PROXY_PORT" https_proxy="http://127.0.0.1:$PROXY_PORT" \
HTTP_PROXY="http://127.0.0.1:$PROXY_PORT" http_proxy="http://127.0.0.1:$PROXY_PORT" \
NO_PROXY="" no_proxy="" \
    $BIN --config "$DIR/t.ini" > "$DIR/server.log" 2>&1 &
SRV_PID=$!

i=0
while [ $i -lt 40 ]; do
    curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
    sleep 0.25
    i=$((i + 1))
done
curl -sS -m 2 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 || {
    echo "FAIL server did not start"; tail -5 "$DIR/server.log"; exit 1; }

query_for() {
    case "$1" in
        translate) echo "translate hello to spanish" ;;
        stocks) echo "AAPL stock price" ;;
        places) echo "where is madrid" ;;
        dictionary) echo "define serendipity" ;;
        currency) echo "100 usd to eur" ;;
        *) echo "linux kernel" ;;
    esac
}

count_host() { grep -c "^$1\$" "$LOG" 2>/dev/null || true; }

# one request; $2 is the cookie ("" for none), $3 is an engine id ("" for none)
ask() {
    cookie="${2:-}"
    engine="${3:-}"
    enc=$(python3 -c 'import sys,urllib.parse;print(urllib.parse.quote_plus(sys.argv[1]))' "$1")
    url="http://127.0.0.1:$PORT/search?q=$enc&nc=$RANDOM"
    [ -n "$engine" ] && url="$url&engine=$engine"
    if [ -n "$cookie" ]; then
        curl -sS -o /dev/null -m 20 -b "$cookie" "$url" 2>/dev/null
    else
        curl -sS -o /dev/null -m 20 "$url" 2>/dev/null
    fi
}

# a video provider is only reached by /videos, not by a web search
ask_video() {
    cookie="${1:-}"
    if [ -n "$cookie" ]; then
        curl -sS -o /dev/null -m 30 -b "$cookie" \
             "http://127.0.0.1:$PORT/videos?q=linux+kernel&nc=$RANDOM" 2>/dev/null
    else
        curl -sS -o /dev/null -m 30 \
             "http://127.0.0.1:$PORT/videos?q=linux+kernel&nc=$RANDOM" 2>/dev/null
    fi
}

$BIN --config "$DIR/t.ini" --providers 2>/dev/null > "$DIR/todo"
[ -s "$DIR/todo" ] || { echo "FAIL --providers listed nothing"; exit 1; }

total=0
while IFS='|' read -r id host group state kind; do
    [ -n "${ONLY:-}" ] && [ "$id" != "$ONLY" ] && continue
    total=$((total + 1))
    # a unique query per iteration: the page caches by query, so repeating
    # "linux kernel" would serve the second provider out of the first one's
    # cache and the provider would look unreachable
    if [ "$kind" = "engine" ]; then
        q="$(query_for "$id") probe $id $RANDOM"
    else
        # a widget's trigger phrases are parsed: "translate hello to spanish
        # translate 32303" is not a translate request. Their host counts do not
        # collide with one another, so caching cannot confuse them.
        q="$(query_for "$id")"
    fi

    case "$state" in
    local)
        if [ -n "$host" ]; then
            say_fail "$id: local provider claims a host ($host)"
        else
            say_pass
        fi
        continue
        ;;
    unavailable)
        before=$(count_host "$host")
        ask "$q" "" "$id"
        after=$(count_host "$host")
        if [ "$after" -ne "$before" ]; then
            say_fail "$id: unavailable but naming it reached $host"
        else
            say_pass
        fi
        continue
        ;;
    esac

    # K7: a page-description fetch happens only when a result has no snippet, so
    # there is no query that "asks" for it - the switch is asserted where the
    # fetches are: test_k7_fetch.sh. Here only the state has to be sane.
    if [ "$kind" = "on_demand" ]; then
        [ -n "$host" ] && say_pass || say_fail "$id: no host listed"
        continue
    fi

    # K4: a video provider is reached by /videos only, so the probe asks there.
    if [ "$kind" = "video" ]; then
        before=$(count_host "$host")
        ask_video ""
        on=$(count_host "$host")
        if [ "$on" -le "$before" ]; then
            say_fail "$id: on, but /videos did not ask for $host (log $before -> $on)"
            continue
        fi
        ask_video "engines=off=$id"
        after=$(count_host "$host")
        if [ "$after" -ne "$on" ]; then
            say_fail "$id: off, but /videos still asked for $host ($on -> $after)"
            continue
        fi
        say_pass
        continue
    fi

    # K5.3: a lane provider is reached through another provider's connection,
    # so "a request to its host" is the wrong question. Its switch shows up in
    # the lane's own log line, and the query has to be discussion-shaped.
    if [ "$kind" = "lane" ]; then
        lanes() { grep -c "\[F3\] reddit lane" "$DIR/server.log" 2>/dev/null || true; }
        before=$(lanes)
        ask "best rust ide reddit $id $RANDOM" "" ""
        sleep 0.4
        on=$(lanes)
        if [ "$on" -le "$before" ]; then
            say_fail "$id: on, but the lane did not run ($before -> $on)"
            continue
        fi
        ask "best rust ide reddit $id $RANDOM" "engines=off=$id" ""
        sleep 0.4
        after=$(lanes)
        if [ "$after" -ne "$on" ]; then
            say_fail "$id: off, but the lane still ran ($on -> $after)"
            continue
        fi
        say_pass
        continue
    fi

    # on: the query reaches the host. An engine is named explicitly, because
    # otherwise the router only asks the verticals the query looks like - an
    # academic API is not asked for a web query, and that is correct behaviour.
    engine=""
    [ "$kind" = "engine" ] && engine="$id"
    before=$(count_host "$host")
    ask "$q" "" "$engine"
    on=$(count_host "$host")
    if [ "$on" -le "$before" ]; then
        say_fail "$id: with the provider on, nothing asked for $host (query '$q', engine '$engine', log $before -> $on)"
        tail -3 "$LOG" | sed 's/^/     last: /'
        continue
    fi

    # off: the same query does not add a single request to that host
    ask "$q" "engines=off=$id" "$engine"
    after=$(count_host "$host")
    if [ "$after" -ne "$on" ]; then
        say_fail "$id: off still asked for $host ($on -> $after)"
        continue
    fi
    say_pass
done < "$DIR/todo"

echo "checked $total providers: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
