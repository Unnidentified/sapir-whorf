#!/bin/sh
# M1: writes from the page work with nothing pasted, and every refusal is a
# redirect back to the page with a reason - never an empty body.
#
# The owner's report was "every Accounts button returns 403, and the 403 page is
# blank". Both halves are asserted here.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8071
DIR=/tmp/m1gate
fail=0; pass=0
ok()  { pass=$((pass + 1)); }
bad() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }

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
[answers]
mode = off
[health]
breaker_threshold = 1000
[cache]
dir = $DIR/cache
INI
OMNI_SECRETS_FILE="$DIR/secrets.ini" ./bin/omnisearch --config "$DIR/t.ini" \
    > "$DIR/srv.log" 2>&1 &
SRV=$!
i=0
while [ $i -lt 40 ]; do
    curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
    sleep 0.25; i=$((i + 1))
done

nonce() {
    curl -sS -m 10 "http://127.0.0.1:$PORT/settings?cb=$RANDOM" \
        | grep -o 'name="n" value="[a-f0-9]*"' | head -1 | sed 's/.*value="//;s/"//'
}

# 1. the page renders a nonce, and the same one in every account form
N=$(nonce)
[ ${#N} -eq 32 ] && ok || bad "the page carries no 32-character nonce (got '$N')"
nonces=$(curl -sS -m 10 "http://127.0.0.1:$PORT/settings?cb=$RANDOM" | grep -c 'name="n"')
[ "$nonces" -ge 1 ] && ok || bad "an account form has no nonce"

# 2. the admin token field is NOT shown to a loopback request
if curl -sS -m 10 "http://127.0.0.1:$PORT/settings?cb=$RANDOM" | grep -q 'name="admin"'; then
    bad "the admin token field is still shown on loopback"
else
    ok
fi

# 3. loopback + valid nonce + same-origin = saved, with no token typed
code=$(curl -sS -o /dev/null -m 20 -H "Sec-Fetch-Site: same-origin" -w "%{http_code}" \
       "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=placeholder-id&n=$N")
loc=$(curl -sS -o /dev/null -m 20 -H "Sec-Fetch-Site: same-origin" -D - \
      "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=placeholder-id&n=$N" \
      2>/dev/null | grep -i "^location:" | tr -d '\r')
[ "$code" = "302" ] && ok || bad "a loopback save returned $code"
echo "$loc" | grep -q "err=" && bad "a valid save was refused ($loc)" || ok
grep -q "^reddit_client_id = placeholder-id" "$DIR/secrets.ini" && ok \
    || bad "the value was not stored"

# 4. every refusal is a redirect, and none has an empty body
refuse_case() {
    label="$1"; shift
    hdrs=$(curl -sS -o "$DIR/body" -m 20 -D - "$@" 2>/dev/null)
    status=$(echo "$hdrs" | head -1)
    clen=$(echo "$hdrs" | grep -i "^content-length:" | tr -d '\r' | awk '{print $2}')
    echo "$status" | grep -q " 30[123]" && ok || bad "$label did not redirect: $status"
    if [ -n "$clen" ] && [ "$clen" = "0" ] && ! echo "$status" | grep -q " 30[123]"; then
        bad "$label answered with an empty body"
    else
        ok
    fi
}

refuse_case "cross-site" -H "Sec-Fetch-Site: cross-site" \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=x&n=$N"
refuse_case "no nonce" -H "Sec-Fetch-Site: same-origin" \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=x"
refuse_case "bogus nonce" -H "Sec-Fetch-Site: same-origin" \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=x&n=deadbeef"

# 5. the refusal lands on a page that says why
N2=$(nonce)
page=$(curl -sS -L -m 20 -H "Sec-Fetch-Site: cross-site" \
       "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=x&n=$N2")
echo "$page" | grep -q "settings-write-error" && ok || bad "no banner on the refusal page"

# 6. every account route, not only save: none may return an empty 4xx/5xx
for route in account-save account-clear account-test account-authorize account-code; do
    hdrs=$(curl -sS -o "$DIR/b" -m 20 -D - -H "Sec-Fetch-Site: cross-site" \
           "http://127.0.0.1:$PORT/settings/$route?card=reddit&n=$N" 2>/dev/null)
    status=$(echo "$hdrs" | head -1)
    clen=$(echo "$hdrs" | grep -i "^content-length:" | tr -d '\r' | awk '{print $2}')
    case "$status" in
        *" 30"*) ok ;;
        *" 403"*|*" 500"*)
            if [ "${clen:-0}" = "0" ]; then bad "$route refused with an empty body"
            else ok; fi ;;
        *) bad "$route returned $status" ;;
    esac
done

# 7. Origin/Referer fallback: no Sec-Fetch-Site, matching Origin = allowed
code=$(curl -sS -o /dev/null -m 20 -H "Origin: http://127.0.0.1:$PORT" -w "%{http_code}" \
       "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=origin-ok&n=$N2")
[ "$code" = "302" ] && ok || bad "a same-origin Origin was refused ($code)"
# ...and a different host is not
N3=$(nonce)
h=$(curl -sS -o /dev/null -m 20 -D - -H "Origin: http://evil.example" \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=x&n=$N3" 2>/dev/null | grep -i "^location:")
echo "$h" | grep -q "err=cross" && ok || bad "a foreign Origin was not refused"
# ...and a request with no origin headers at all is refused rather than allowed
N4=$(nonce)
h=$(curl -sS -o /dev/null -m 20 -D - -H "Origin:" -H "Referer:" \
    "http://127.0.0.1:$PORT/settings/account-save?card=reddit&reddit_client_id=x&n=$N4" 2>/dev/null | grep -i "^location:")
echo "$h" | grep -q "err=cross" && ok || bad "a request with no origin at all was not refused"

echo "M1 gate: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
