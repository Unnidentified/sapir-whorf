#!/usr/bin/env bash
# J3d: the query used to be pasted unescaped into every link, so /search?q=1+1
# decoded as "1 1", AT&T became "AT" and C# became "C". This checks the three
# link families the review named (tabs, pager, settings) for the four queries
# it named, against a private instance so the assertions are deterministic.
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=8095
CACHE=/tmp/j3d-cache
INI=/tmp/j3d.ini
rm -rf "$CACHE"
cat > "$INI" <<CONF
[server]
port = $PORT
host = 127.0.0.1
answers = off
infobox = off
[engines]
engines = "bing"
[cache]
dir = $CACHE
CONF

./bin/omnisearch --config "$INI" >/tmp/j3d.log 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null; rm -rf "$CACHE"' EXIT
for _ in $(seq 1 40); do
  curl -sS -o /dev/null -m 2 "http://127.0.0.1:$PORT/" && break
  sleep 0.25
done

fail=0
check() { # query, encoded
  local q="$1" enc="$2" body
  body=$(curl -sS -m 30 "http://127.0.0.1:$PORT/search?q=$enc&web=1&nc=$RANDOM")
  for want in "/search?q=$enc" "/images?q=$enc" "/settings?q=$enc"; do
    if ! printf '%s' "$body" | grep -qF "href=\"$want"; then
      echo "FAIL $q: no link to $want"
      fail=1
    fi
  done
  # the pager must keep the same encoding, not the decoded text
  if ! printf '%s' "$body" | grep -qF "/search?q=$enc&amp;p=2"; then
    echo "FAIL $q: pager lost the encoding"
    fail=1
  fi
}

check "1+1" "1%2B1"
check "AT&T" "AT%26T"
check "C#" "C%23"
check "50%" "50%25"

if [ "$fail" -eq 0 ]; then
  echo "PASS: 4 queries x 4 link positions keep the percent-encoding"
fi
exit "$fail"
