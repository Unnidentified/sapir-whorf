#!/bin/sh
# X1 and X4: every field of every card saves and clears, a non-secret shows its
# own value, and a value made only of mask characters is refused.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8073
DIR=/tmp/x4fields
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
save_field() {  # card field value
    curl -sS -H "Sec-Fetch-Site: same-origin" -o /dev/null -m 20 \
        "http://127.0.0.1:$PORT/settings/account-save?card=$1&$2=$(printf %s "$3" | sed 's/:/%3A/g;s|/|%2F|g')&n=$(nonce)"
}
clear_field() { # card field
    curl -sS -H "Sec-Fetch-Site: same-origin" -o /dev/null -m 20 \
        "http://127.0.0.1:$PORT/settings/account-clear?card=$1&field=$2&n=$(nonce)"
}
stored() { grep -q "^$1 = " "$DIR/secrets.ini"; }

# X1: the FIFTH field of the Reddit card. It could never be saved before this,
# because ownership was checked with a literal `f < 4`.
save_field reddit reddit_client_secret csecret-value-77
stored "reddit_client_secret" && ok || bad "the fifth field did not save"
clear_field reddit reddit_client_secret
stored "reddit_client_secret" && bad "the fifth field did not clear" || ok

# every declared field of every card round-trips
for pair in \
    "reddit:reddit_client_id:cid-123" \
    "reddit:reddit_redirect_uri:myapp://cb" \
    "reddit:reddit_user_agent:com.example.app/1.0" \
    "reddit:reddit_username:someone" \
    "searxng:searxng_url:http://127.0.0.1:8888" \
    "searxng:searxng_engines:google,brave" \
    "yacy:yacy_instance:http://127.0.0.1:8090" \
    "google:google_cse_cx:0123456789abcdef" \
    "google:google_cse_key:key-abc" ; do
    card=${pair%%:*}; rest=${pair#*:}; field=${rest%%:*}; value=${rest#*:}
    save_field "$card" "$field" "$value"
    stored "$field" && ok || bad "$field did not save"
    clear_field "$card" "$field"
    stored "$field" && bad "$field did not clear" || ok
done

# X4: a non-secret shows its value in the input; a secret does not
save_field reddit reddit_client_id shown-in-full-abc
page=$(curl -sS -m 15 "http://127.0.0.1:$PORT/settings?cb=$RANDOM")
echo "$page" | grep -q 'value="shown-in-full-abc"' && ok \
    || bad "a non-secret field is not shown in plain text"
save_field reddit reddit_client_secret top-secret-xyz
page=$(curl -sS -m 15 "http://127.0.0.1:$PORT/settings?cb=$RANDOM")
echo "$page" | grep -q "top-secret-xyz" && bad "a secret is printed on the page" || ok
# ...and a mask-only value is refused rather than stored
clear_field reddit reddit_username
save_field reddit reddit_username '***'
stored "reddit_username" && bad "a mask-only value was stored" || ok
page=$(curl -sS -m 15 "http://127.0.0.1:$PORT/settings?cb=$RANDOM")
echo "$page" | grep -q "punctuation only" && ok || bad "no explanation for the refusal"

echo "X1/X4 fields: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
