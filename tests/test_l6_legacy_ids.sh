#!/bin/sh
# L6.10: every engine id this project has ever shipped, fed to the cookie
# parser. None of them may warn, error, or 500 - a browser that saved its choice
# months ago is not a mistake the owner can act on.
#
# The list is prose below rather than derived, deliberately: it is the record of
# what shipped, and a derived list would silently shrink when an engine goes.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8201
DIR=/tmp/l6legacy
fail=0; pass=0
ok() { pass=$((pass + 1)); }
bad() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; }
trap cleanup EXIT INT TERM

lsof -nP -iTCP:$PORT -sTCP:LISTEN 2>/dev/null | awk 'NR>1{print $2}' | xargs -r kill -9
rm -rf "$DIR"; mkdir -p "$DIR/cache"
cat > "$DIR/t.ini" <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg,bing,yahoo,brave,wikipedia
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

# startpage and mojeek were renamed to bing and brave; ddg-lite was removed in
# round 6. Everything else here is an id the settings page could have written.
OLD="startpage mojeek ddg-lite ddg_lite google_cse_element searxng yacy qwant
     ecosia marginalia wiby stract right dao youchat perplexity phind kagi
     brave_search startpage_eu html_duckduckgo duckduckgo_lite"

for id in $OLD; do
    # as part of the disabled set (the current format)
    code=$(curl -sS -o "$DIR/p.html" -m 20 -b "engines=off=$id|bing" \
           -w "%{http_code}" "http://127.0.0.1:$PORT/search?q=linux+kernel&nc=$RANDOM")
    [ "$code" = "200" ] && ok || bad "off=$id returned $code"
    # a real engine can warn (rate limit, empty body); an *id* never may. The
    # page names a failed source, so the test looks for the id beside its own
    # complaint rather than for the word "warning" anywhere.
    if grep -qiE "unknown engine[^<]*$id|$id[^<]*unknown engine|no such engine[^<]*$id" \
            "$DIR/p.html"; then
        bad "off=$id produced an unknown-engine complaint"
    else
        ok
    fi

    # as a bare legacy list (the format before K1.1)
    code=$(curl -sS -o /dev/null -m 20 -b "engines=$id,ddg" -w "%{http_code}" \
           "http://127.0.0.1:$PORT/search?q=linux+kernel&nc=$RANDOM")
    [ "$code" = "200" ] && ok || bad "legacy list '$id,ddg' returned $code"
done

# the renamed ones really are renamed, not dropped
for pair in "startpage:bing" "mojeek:brave" "ddg-lite:ddg" "ddg_lite:ddg"; do
    was=${pair%%:*}; now=${pair##*:}
    body=$(curl -sS -o /dev/null -m 20 -b "engines=off=$was" -w "%{http_code}" \
           "http://127.0.0.1:$PORT/search?q=linux+kernel&nc=$RANDOM")
    [ "$body" = "200" ] && ok || bad "$was -> $now returned $body"
done

# and an id that never existed is skipped without a word, not treated as an error
code=$(curl -sS -o "$DIR/q.html" -m 20 -b "engines=off=never-existed|bing" \
       -w "%{http_code}" "http://127.0.0.1:$PORT/search?q=linux+kernel&nc=$RANDOM")
[ "$code" = "200" ] && ok || bad "an unknown id returned $code"
grep -qiE "unknown engine|no such engine" "$DIR/q.html" \
    && bad "an unknown id rendered an unknown-engine complaint" || ok

# nothing on stderr about an unknown engine, either
if grep -qiE "unknown engine|no such engine|invalid engine" "$DIR/srv.log"; then
    bad "the server logged an unknown-engine warning"
else
    ok
fi

echo "L6 legacy ids: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
