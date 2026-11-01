#!/bin/sh
# K4: the Videos tab and /videos.
#
# What must hold without a network: the tab bar is the only navigation and it
# carries the four tabs in order; switching every video provider off gives the
# "no providers" page, not an error and not "no results"; and no card ever
# points the browser at a third party - every thumbnail goes through /proxy.
#
# The card fields are asserted only when a provider actually answered, so the
# test degrades honestly on a machine with no outbound network.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8173
DIR=/tmp/k4videos
fail=0
pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }
cleanup() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; wait 2>/dev/null; }
trap cleanup EXIT INT TERM

pkill -9 -f "$DIR/t.ini" 2>/dev/null
rm -rf "$DIR"; mkdir -p "$DIR/cache"
printf '[server]\nport = %s\nhost = 127.0.0.1\n[engines]\nengines = ddg\n[health]\nbreaker_threshold = 1000\n[cache]\ndir = %s/cache\n' \
    "$PORT" "$DIR" > "$DIR/t.ini"
./bin/omnisearch --config "$DIR/t.ini" > "$DIR/srv.log" 2>&1 &
SRV=$!
i=0
while [ $i -lt 40 ]; do
    curl -sS -m 1 "http://127.0.0.1:$PORT/info" >/dev/null 2>&1 && break
    sleep 0.25; i=$((i + 1))
done

get() { curl -sS -o "$DIR/page.html" -m 30 ${1:+-b "$1"} "http://127.0.0.1:$PORT/videos?q=rust+programming&nc=$RANDOM" 2>/dev/null; }

# 1. the tab bar: one nav, four tabs, Videos active on this page
get ""
grep -q 'class="nav-tabs"' "$DIR/page.html" || say_fail "no tab bar on /videos"
count=$(grep -c 'class="nav-tabs"' "$DIR/page.html")
[ "$count" = "1" ] || say_fail "expected one tab bar, saw $count"
order=$(python3 -c "
import re
h=open('$DIR/page.html').read()
m=re.search(r'<nav class=\"nav-tabs\">.*?</nav>', h, re.S)
block=m.group(0) if m else ''
print(' '.join(t.strip() for t in re.findall(r'>\s*([A-Za-z][A-Za-z ]*?)\s*<', block)) + ' ')
")
case "$order" in
"All Images Videos Settings ") say_pass ;;
*) say_fail "tab order is '$order', expected All Images Videos Settings" ;;
esac
grep -q 'href="/videos?q=rust%20programming" class="active"' "$DIR/page.html" \
    && say_pass || say_fail "Videos is not the active tab on /videos"
grep -q 'filter-tabs\|class="chip' "$DIR/page.html" && say_fail "chip markup on /videos" || say_pass

# 2. no card points the browser at a third party
if grep -q '<img src="https://' "$DIR/page.html"; then
    say_fail "a thumbnail is loaded straight from the platform"
else
    say_pass
fi

# 3. cards have the four things a video card needs, when a provider answered
if grep -q 'class="video-card"' "$DIR/page.html"; then
    grep -q 'class="video-title">[^<]' "$DIR/page.html" && say_pass || say_fail "a card has no title"
    grep -q 'class="video-duration">[0-9]' "$DIR/page.html" && say_pass || say_fail "a card has no duration"
    grep -q 'class="video-platform">' "$DIR/page.html" && say_pass || say_fail "a card names no platform"
    grep -q '/proxy?url=' "$DIR/page.html" && say_pass || say_fail "no thumbnail went through /proxy"
    grep -q 'iframe' "$DIR/page.html" && say_fail "the page embeds a player" || say_pass
else
    echo "  skip: no video provider answered (offline?)"
fi

# 4. every video provider off: the honest page, no cards, no error
get "engines=off=dailymotion"
code=$(curl -sS -o /dev/null -m 20 -w '%{http_code}' -b "engines=off=dailymotion" \
       "http://127.0.0.1:$PORT/videos?q=rust+programming&nc=$RANDOM")
[ "$code" = "200" ] && say_pass || say_fail "all providers off returned $code"
grep -q 'class="video-card"' "$DIR/page.html" && say_fail "cards rendered with every provider off" || say_pass
grep -q 'No video providers are switched on' "$DIR/page.html" \
    && say_pass || say_fail "all providers off did not say why"

echo "K4 videos: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
