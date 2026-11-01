#!/bin/sh
# K4: the results page must fit the template context - beaker has 32 slots and
# drops a key SILENTLY once they are full, which is how a page once rendered
# without results and said nothing.
#
# Deleting the chip strip freed one slot; the widget keys that only one card
# can read were being set on every page, so they now appear only on the card
# that reads them. This asserts the number, not the absence of a symptom.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8171
DIR=/tmp/k4ctx
fail=0
pass=0

cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT INT TERM

pkill -9 -f "$DIR/t.ini" 2>/dev/null
rm -rf "$DIR"; mkdir -p "$DIR/cache"
printf '[server]\nport = %s\nhost = 127.0.0.1\n[engines]\nengines = ddg,bing,yahoo,brave,wikipedia\n[answers]\nmode = all\n[health]\nbreaker_threshold = 1000\npace_requests_per_minute = 6000\npace_burst = 2000\n[cache]\ndir = %s/cache\n' \
    "$PORT" "$DIR" > "$DIR/t.ini"

OMNI_PERF=1 ./bin/omnisearch --config "$DIR/t.ini" > "$DIR/srv.log" 2>&1 &
SRV=$!
i=0
while [ $i -lt 40 ]; do
    curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
    sleep 0.25; i=$((i + 1))
done

# the widest paths: plain results, a calculator card, a translate card, a
# stock card, a place card - each sets a different set of widget keys
for q in "linux+kernel" "2%2B2" "translate+hello+to+german" "AAPL+stock" \
         "where+is+madrid" "define+serendipity"; do
    curl -sS -o /dev/null -m 30 "http://127.0.0.1:$PORT/search?q=$q&nc=$RANDOM" 2>/dev/null
done
sleep 0.5

worst=$(grep -o "context keys used: [0-9]*" "$DIR/srv.log" | awk '{print $4}' | sort -n | tail -1)
worst=${worst:-0}
if [ "$worst" -le 30 ] && [ "$worst" -gt 0 ]; then
    pass=$((pass + 1))
else
    echo "FAIL widest page used $worst of 32 slots"; fail=$((fail + 1))
fi

if grep -qi "TemplateContext is full\|Cannot add" "$DIR/srv.log"; then
    echo "FAIL beaker reported a dropped key"; fail=$((fail + 1))
else
    pass=$((pass + 1))
fi

echo "K4 context budget: $pass passed, $fail failed (worst $worst of 32)"
[ "$fail" -eq 0 ]
