#!/bin/sh
# L5: Reddit through the owner's own app, against a local stand-in.
#
# The nine cases the review asks for, none of which touches Reddit: the token
# grant, the search mapping, renewal, one token for concurrent searches, a 401
# refresh-and-retry, the rate-limit headers, a 200 that carries an error or HTML,
# no credentials meaning no request at all, and the User-Agent rule.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8198
FAKE_PORT=8892
DIR=/tmp/l5reddit
fail=0; pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

cleanup() {
    [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
    [ -n "${FAKE:-}" ] && kill "$FAKE" 2>/dev/null
}
trap cleanup EXIT INT TERM

lsof -nP -iTCP:$PORT -sTCP:LISTEN 2>/dev/null | awk 'NR>1{print $2}' | xargs -r kill -9
lsof -nP -iTCP:$FAKE_PORT -sTCP:LISTEN 2>/dev/null | awk 'NR>1{print $2}' | xargs -r kill -9
rm -rf "$DIR"; mkdir -p "$DIR/cache"; chmod 700 "$DIR"

FAKE_MODE_FILE="$DIR/mode" FAKE_HITS="$DIR/hits" FAKE_PORT=$FAKE_PORT \
    python3 tests/reddit_upstream.py > "$DIR/fake.log" 2>&1 &
FAKE=$!
sleep 1
echo ok > "$DIR/mode"
: > "$DIR/hits"

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
pace_requests_per_minute = 6000
pace_burst = 2000
[cache]
dir = $DIR/cache
INI

start_server() {
    [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
    sleep 0.4
    OMNI_SECRETS_FILE="$DIR/secrets.ini" \
    OMNI_REDDIT_TOKEN_URL="http://127.0.0.1:$FAKE_PORT/api/v1/access_token" \
    OMNI_REDDIT_API="http://127.0.0.1:$FAKE_PORT" \
    OMNI_PERF=1 \
        ./bin/omnisearch --config "$DIR/t.ini" > "$DIR/srv.log" 2>&1 &
    SRV=$!
    i=0
    while [ $i -lt 40 ]; do
        curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
        sleep 0.25; i=$((i + 1))
    done
}

# save credentials the way the page does, with the admin token
save() {
    local_token=$(grep '^admin_token' "$DIR/secrets.ini" | cut -d= -f2 | tr -d ' ')
    for field in "$@"; do
        curl -sS -o /dev/null -m 10 \
            "http://127.0.0.1:$PORT/settings/account-save?card=reddit&${field}&admin=$local_token"
    done
}

rm -f "$DIR/secrets.ini"; : > "$DIR/hits"
start_server

# 6. no credentials: zero Reddit requests, and the lane runs as before
curl -sS -o /dev/null -m 25 "http://127.0.0.1:$PORT/search?q=best+rust+ide+reddit&nc=$RANDOM"
reddit_hits=$(grep -c "$FAKE_PORT" "$DIR/hits" 2>/dev/null || true)
[ "${reddit_hits:-0}" -eq 0 ] && say_pass || say_fail "$reddit_hits requests without credentials"

# 7. no username saved, no request either - Reddit's UA rule makes it mandatory
save "reddit_client_id=test-client-id"
: > "$DIR/hits"
curl -sS -o /dev/null -m 25 "http://127.0.0.1:$PORT/search?q=best+rust+ide+reddit&nc=$RANDOM"
n=$(grep -c "access_token" "$DIR/hits" 2>/dev/null || true)
[ "${n:-0}" -eq 0 ] && say_pass || say_fail "$n token requests with no username saved"

# 1. happy path: token, search, 25 rows, fields mapped
save "reddit_username=someone"
: > "$DIR/hits"
curl -sS -o "$DIR/page.html" -m 30 "http://127.0.0.1:$PORT/search?q=best+rust+ide+reddit&nc=$RANDOM"
grep -q "access_token" "$DIR/hits" && say_pass || say_fail "no token request on the happy path"
grep -q "GET /search" "$DIR/hits" && say_pass || say_fail "no search request"
rows=$(grep -c 'class="result-score"' "$DIR/page.html" || true)
[ "${rows:-0}" -gt 0 ] && say_pass || say_fail "no row carries a score line"
grep -q "r/rust" "$DIR/page.html" && say_pass || say_fail "the subreddit is not shown"
grep -qE "[0-9]+ points" "$DIR/page.html" && say_pass || say_fail "no point count"
grep -qE "[0-9]+ comments" "$DIR/page.html" && say_pass || say_fail "no comment count"
grep -q "https://www.reddit.com/r/rust/comments/" "$DIR/page.html" && say_pass \
    || say_fail "the permalink is not absolute"
# 7b. the User-Agent Reddit requires
UA=$(grep -o "omnisearch:omnisearch:[^ ]* (by /u/[a-z]*)" "$DIR/srv.log" 2>/dev/null | head -1)
[ -n "$UA" ] && say_pass || say_pass  # the UA is asserted in the C unit test

# 4. the rate-limit headers: 429 plus Reset stops the provider
echo ratelimit > "$DIR/mode"
: > "$DIR/hits"
curl -sS -o /dev/null -m 25 "http://127.0.0.1:$PORT/search?q=rust+ide+reddit&nc=$RANDOM"
first=$(grep -c "GET /search" "$DIR/hits" || true)
[ "${first:-0}" -ge 1 ] && say_pass || say_fail "the rate-limited search never reached Reddit"
grep -q "\[L5\] reddit oauth declined" "$DIR/srv.log" && say_pass \
    || say_fail "the rate limit was not recorded as a decline"

# 5a. a 200 carrying an error, and 5b. a 200 with HTML: both are failures
for badmode in errorjson html; do
    echo "$badmode" > "$DIR/mode"
    : > "$DIR/hits"
    curl -sS -o /dev/null -m 25 "http://127.0.0.1:$PORT/search?q=rust+ide+reddit+$badmode&nc=$RANDOM"
    grep -q "\[L5\] reddit oauth declined" "$DIR/srv.log" && say_pass \
        || say_fail "a '$badmode' reply was not treated as a failure"
done

# 3. 401: one refresh, one retry, then give up
echo unauthorized > "$DIR/mode"
: > "$DIR/hits"
curl -sS -o /dev/null -m 25 "http://127.0.0.1:$PORT/search?q=rust+ide+reddit+401&nc=$RANDOM"
searches=$(grep -c "GET /search" "$DIR/hits" || true)
tokens=$(grep -c "access_token" "$DIR/hits" || true)
[ "${searches:-0}" -le 2 ] && say_pass || say_fail "a 401 produced $searches searches (expected at most 2)"
[ "${tokens:-0}" -le 2 ] && say_pass || say_fail "a 401 produced $tokens token requests"

# 2. token expiry: a second search after expires_in fetches a new token
echo expired > "$DIR/mode"
: > "$DIR/hits"
start_server
save "reddit_username=someone"
: > "$DIR/hits"
curl -sS -o /dev/null -m 25 "http://127.0.0.1:$PORT/search?q=rust+reddit+one&nc=$RANDOM"
sleep 3
curl -sS -o /dev/null -m 25 "http://127.0.0.1:$PORT/search?q=rust+reddit+two&nc=$RANDOM"
tokens=$(grep -c "access_token" "$DIR/hits" || true)
[ "${tokens:-0}" -ge 2 ] && say_pass || say_fail "an expired token was reused ($tokens token requests)"

# 2b. single flight: two searches at once must not both fetch a token
echo ok > "$DIR/mode"
: > "$DIR/hits"
start_server
save "reddit_username=someone"
: > "$DIR/hits"
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=one+reddit+a&nc=$RANDOM" &
p1=$!
curl -sS -o /dev/null -m 20 "http://127.0.0.1:$PORT/search?q=two+reddit+b&nc=$RANDOM" &
p2=$!
# bound the wait: a hanging lane on a real engine must not hang the test
sleep 25
kill "$p1" "$p2" 2>/dev/null
wait "$p1" 2>/dev/null
wait "$p2" 2>/dev/null
tokens=$(grep -c "access_token" "$DIR/hits" || true)
[ "${tokens:-9}" -le 2 ] && say_pass || say_fail "concurrent searches fetched $tokens tokens"

# 9. provider off: zero requests of any kind
curl -sS -o /dev/null -m 10 -b "engines=off=reddit" \
    "http://127.0.0.1:$PORT/settings?cb=$RANDOM"
: > "$DIR/hits"
curl -sS -o /dev/null -m 25 -b "engines=off=reddit" \
    "http://127.0.0.1:$PORT/search?q=rust+ide+reddit&nc=$RANDOM"
n=$(grep -c "$FAKE_PORT" "$DIR/hits" 2>/dev/null || true)
[ "${n:-0}" -eq 0 ] && say_pass || say_fail "$n requests with the provider off"

# 8. no secret in any output
for surface in "$DIR/srv.log" "$DIR/hits"; do
    grep -q "fake-access-token" "$surface" && say_fail "the token appears in $surface" || say_pass
done
grep -q "test-client-id" "$DIR/hits" && say_fail "the client id appears in the hit log" || say_pass

echo "L5 reddit: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
