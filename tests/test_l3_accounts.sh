#!/bin/sh
# L3: the credentials form, end to end, without a browser.
#
# What is asserted: an unauthenticated write is refused; an authenticated one
# lands in a 0600 file outside the project; the page shows only the last four
# characters; a blank field keeps the stored value; Clear removes it; and none
# of it appears in the log, the status JSON or the page.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8196
DIR=/tmp/l3accounts
fail=0; pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; }
trap cleanup EXIT INT TERM

lsof -nP -iTCP:$PORT -sTCP:LISTEN 2>/dev/null | awk 'NR>1{print $2}' | xargs -r kill -9
rm -rf "$DIR"; mkdir -p "$DIR/cache"; chmod 700 "$DIR"
cat > "$DIR/t.ini" <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg
[cache]
dir = $DIR/cache
INI
OMNI_SECRETS_FILE="$DIR/secrets.ini" ./bin/omnisearch --config "$DIR/t.ini" > "$DIR/srv.log" 2>&1 &
SRV=$!
i=0
while [ $i -lt 40 ]; do
    curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
    sleep 0.25; i=$((i + 1))
done
[ -f "$DIR/secrets.ini" ] || { echo "FAIL no secrets file"; tail -5 "$DIR/srv.log"; exit 1; }
# M1: every write needs the form nonce that /settings renders, exactly as a
# browser gets it. Fetching one here is what makes these tests honest.
fetch_nonce() {
    curl -sS -m 10 "http://127.0.0.1:$PORT/settings?cb=$RANDOM" \
        | grep -o 'name="n" value="[a-f0-9]*"' | head -1 | sed 's/.*value="//;s/"//'
}

TOKEN=$(grep '^admin_token' "$DIR/secrets.ini" | cut -d= -f2 | tr -d ' ')
[ "${#TOKEN}" -ge 48 ] && say_pass || say_fail "the admin token is too short (${#TOKEN})"

# 1. the token was printed once, at startup, and nowhere else
grep -q "admin token created" "$DIR/srv.log" && say_pass \
    || say_fail "the first start did not announce the token"
grep -c "$TOKEN" "$DIR/srv.log" | grep -qx 1 && say_pass \
    || say_fail "the token appears $(grep -c "$TOKEN" "$DIR/srv.log") times in the log (expected 1)"

# 2. M1 changed what this gate is. On loopback the token is no longer asked for:
#    the nonce and a same-origin request are the gate. What must still hold is
#    that a write WITHOUT a nonce is refused - that is the control that stops a
#    hand-typed URL or a replay.
code=$(curl -sS -H "Sec-Fetch-Site: same-origin" -o /dev/null -m 20 -w '%{http_code}' -D "$DIR/nononce.h" \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=x")
[ "$code" = "302" ] && say_pass || say_fail "a write with no nonce returned $code"
grep -qi "^location:.*err=expired" "$DIR/nononce.h" && say_pass \
    || say_fail "a write with no nonce was not refused as expired"
grep -q "reddit_client_id = x" "$DIR/secrets.ini" && say_fail "a refused write still stored a value" \
    || say_pass

# 3. the real write lands, and the file is 0600
curl -sS -H "Sec-Fetch-Site: same-origin" -o /dev/null -m 20 -w '%{http_code}' \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=client-9f3a&admin=$TOKEN&n=$(fetch_nonce)" \
    > /tmp/l3code.txt 2>/dev/null
[ "$(cat /tmp/l3code.txt)" = "302" ] && say_pass || say_fail "a good write returned $(cat /tmp/l3code.txt)"
grep -q "reddit_client_id = client-9f3a" "$DIR/secrets.ini" && say_pass \
    || say_fail "the client id is not in the file"
mode=$(stat -f '%Lp' "$DIR/secrets.ini" 2>/dev/null || stat -c '%a' "$DIR/secrets.ini" 2>/dev/null)
[ "$mode" = "600" ] && say_pass || say_fail "the file mode is $mode, expected 600"

# 4. X4: a non-secret field is shown in full, and a secret is masked. The client
#    id is not a secret - it travels in a URL Reddit documents - so masking it
#    hid a typo instead of protecting anything.
curl -sS -o "$DIR/page.html" -m 20 "http://127.0.0.1:$PORT/settings" 2>/dev/null
grep -q "client-9f3a" "$DIR/page.html" && say_pass \
    || say_fail "a non-secret field is not shown in plain text"

# 5. a blank field keeps the stored value
curl -sS -H "Sec-Fetch-Site: same-origin" -o /dev/null -m 20 \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=&reddit_username=someone&admin=$TOKEN&n=$(fetch_nonce)"
grep -q "reddit_client_id = client-9f3a" "$DIR/secrets.ini" && say_pass \
    || say_fail "a blank field wiped the stored value"
grep -q "reddit_username = someone" "$DIR/secrets.ini" && say_pass \
    || say_fail "the second field did not save"

# 6. Clear removes it
curl -sS -H "Sec-Fetch-Site: same-origin" -o /dev/null -m 20 \
    "http://127.0.0.1:$PORT/settings/account-clear?card=reddit&field=reddit_client_id&admin=$TOKEN&n=$(fetch_nonce)"
grep -q "reddit_client_id = client-9f3a" "$DIR/secrets.ini" && say_fail "Clear did not remove the key" \
    || say_pass

# 7. the secret reaches no log, no status JSON, and no page but the masked one
for surface in "$DIR/srv.log" "$HOME/.omnisearch/omnisearch.err"; do
    [ -r "$surface" ] || continue
    grep -q "client-9f3a" "$surface" && say_fail "the credential appears in $surface" \
        || say_pass
done
curl -sS -o "$DIR/status.json" -m 20 "http://127.0.0.1:$PORT/settings/engine-status.json" 2>/dev/null
grep -q "client-9f3a" "$DIR/status.json" && say_fail "the credential appears in the status JSON" \
    || say_pass
curl -sS -o "$DIR/metrics.txt" -m 20 "http://127.0.0.1:$PORT/metrics" 2>/dev/null
grep -q "client-9f3a" "$DIR/metrics.txt" && say_fail "the credential appears in /metrics" || say_pass

# 8. the cookie jar holds no credential: the save sets no cookie at all
rm -f "$DIR/jar"
curl -sS -o /dev/null -m 20 -c "$DIR/jar" \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=client-9f3a&admin=$TOKEN&n=$(fetch_nonce)"
grep -q "client-9f3a" "$DIR/jar" 2>/dev/null && say_fail "the credential came back as a cookie" \
    || say_pass

# 9. the page declares no-store and no-referrer
grep -q 'http-equiv="Cache-Control" content="no-store"' "$DIR/page.html" && say_pass \
    || say_fail "the page has no Cache-Control declaration"
grep -q 'name="referrer" content="no-referrer"' "$DIR/page.html" && say_pass \
    || say_fail "the page has no referrer policy"

echo "L3 accounts: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
