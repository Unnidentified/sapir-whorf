#!/bin/bash
# K1.1: a toggle has to round-trip. This is the test that would have caught the
# comma: beaker drops a cookie value containing one, so two or more selected
# engines saved nothing at all while one worked.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8141
DIR=/tmp/k1p
rm -rf "$DIR" && mkdir -p "$DIR"
cat > /tmp/k1p.ini <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg,bing,yahoo,brave,wikipedia
[cache]
dir = $DIR
INI
pkill -9 -f "k1p.ini" 2>/dev/null
sleep 0.5
./bin/omnisearch --config /tmp/k1p.ini > /tmp/k1p.log 2>&1 &
sleep 3
BASE="http://127.0.0.1:$PORT"
FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }

# save three, keep the jar, read the page back
rm -f /tmp/k1p.jar
curl -sS -o /dev/null -m 20 -c /tmp/k1p.jar -D /tmp/k1p.h \
  "$BASE/save_settings?engines_seen=5&off_brave=1&off_wikipedia=1"
grep -qi 'set-cookie: engines=' /tmp/k1p.h || fail "saving three engines set no cookie"
cookie=$(grep -i 'set-cookie: engines=' /tmp/k1p.h | head -1 | sed 's/.*engines=//; s/;.*//')
echo "cookie after saving ddg,bing,yahoo: $cookie"
case "$cookie" in
  off=*) : ;;
  *) fail "the cookie is not the disabled set ($cookie)" ;;
esac
case "$cookie" in
  *brave*|*wikipedia*) : ;;
  *) fail "the disabled set does not name the engines that were left out" ;;
esac

curl -sS -o /tmp/k1p.html -m 20 -b /tmp/k1p.jar "$BASE/settings"
# ticked means OFF in this form
for e in brave wikipedia; do
  grep -q "name=\"off_$e\"[^>]*checked" /tmp/k1p.html \
    || fail "$e was switched off but did not come back off"
done
for e in ddg bing yahoo; do
  grep -q "name=\"off_$e\"[^>]*checked" /tmp/k1p.html && fail "$e came back off but was left on"
done

# and with two
rm -f /tmp/k1p.jar
curl -sS -o /dev/null -m 20 -c /tmp/k1p.jar -D /tmp/k1p2.h \
  "$BASE/save_settings?engines_seen=5&off_ddg=1&off_yahoo=1&off_wikipedia=1"
grep -qi 'set-cookie: engines=' /tmp/k1p2.h || fail "saving two engines set no cookie"
curl -sS -o /tmp/k1p2.html -m 20 -b /tmp/k1p.jar "$BASE/settings"
grep -q 'name="off_ddg"[^>]*checked' /tmp/k1p2.html || fail "two-provider save lost the ddg choice"
grep -q 'name="off_yahoo"[^>]*checked' /tmp/k1p2.html || fail "two-provider save lost the yahoo choice"

# a legacy comma cookie still works (it stored the ENABLED set)
curl -sS -o /tmp/k1p3.html -m 20 -b "engines=bing,yahoo" "$BASE/settings"
# a legacy comma cookie stored the ENABLED set: bing and yahoo on, the rest off
grep -q 'name="off_ddg"[^>]*checked' /tmp/k1p3.html || fail "a legacy cookie did not leave ddg off"
grep -q 'name="off_bing"[^>]*checked' /tmp/k1p3.html && fail "a legacy cookie turned bing off"

# older ids still map
curl -sS -o /tmp/k1p4.html -m 20 -b "engines=startpage" "$BASE/settings"
grep -q 'name="off_bing"[^>]*checked' /tmp/k1p4.html && fail "startpage no longer maps to bing"

pkill -9 -f "k1p.ini" 2>/dev/null
[ "$FAIL" = "0" ] && echo "K1.1 toggle persistence: PASS" || { echo "K1.1 toggle persistence: FAIL"; exit 1; }
