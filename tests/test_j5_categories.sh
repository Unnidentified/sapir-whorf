#!/bin/bash
# J5.3 + K4: a category narrows which engines are asked, keeps itself on the
# pager, and says so when nothing serves it rather than silently answering with
# web results. Since K4 the category is a URL parameter with no UI of its own:
# the tab bar is the only navigation, and the per-engine counts live in one
# footer line instead of a chip strip.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8123
DIR=/tmp/j5cat
rm -rf "$DIR" && mkdir -p "$DIR"
cat > /tmp/j5cat.ini <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg,openlibrary,openalex,arxiv
definitions_dir = static/engines.d
[cache]
dir = $DIR
INI
pkill -9 -f "j5cat.ini" 2>/dev/null
sleep 0.5
./bin/omnisearch --config /tmp/j5cat.ini > /tmp/j5cat.log 2>&1 &
SRV=$!
sleep 3

FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }

body() { curl -sS -m 40 "http://127.0.0.1:$PORT/search?$1&nc=$RANDOM" -o /tmp/j5cat.html; }
# K4: none of this may come back. The strip was a second navigation.
no_chips() {
  grep -q 'filter-tabs' /tmp/j5cat.html && fail "the filter strip is back"
  grep -q 'class="chip' /tmp/j5cat.html && fail "chip markup is back"
  return 0
}
# the footer line, if the page has one
note() { grep -o '<p class="results-source-note">[^<]*</p>' /tmp/j5cat.html | head -1; }

# 1. no strip, no chips, and the tab bar is there exactly once
body "q=linux+kernel"
no_chips
[ "$(grep -c 'class="nav-tabs"' /tmp/j5cat.html)" = "1" ] \
  || fail "the tab bar is not the single navigation"

# 2. the category page still renders and still has no chips
body "q=linux+kernel&cat=academic"
no_chips

# 4. the pager keeps the category. A page with no results has no pager either
#    - offline, that is the normal case here, and asserting a pager on a page
#    that has none tests the network rather than the code.
body "q=linux+kernel&cat=academic&p=2"
if grep -q 'class="result"' /tmp/j5cat.html; then
    grep -q 'cat=academic' /tmp/j5cat.html || fail "pager dropped the category"
else
    echo "  note: no results this run, so the pager is not asserted"
fi

# K4: the counts the chips carried are one footer line. Offline this test may
# have no answer at all, so the rule is: a page with results names its engines,
# a page without results has no line rather than an empty one.
body "q=linux+kernel"
no_chips
if grep -q 'class="result"' /tmp/j5cat.html; then
  note > /tmp/j5cat.note
  [ -s /tmp/j5cat.note ] || fail "results without the answered-by line"
  grep -q '(' /tmp/j5cat.note || fail "the answered-by line carries no count"
else
  [ -z "$(note)" ] || fail "an empty page printed an empty source line"
fi
# ?engine= is still a URL parameter, and still must not bring a chip back
body "q=linux+kernel&engine=ddg"
no_chips

# 5. an unknown category is ignored, not fatal
code=$(curl -sS -m 30 -o /tmp/j5cat.html -w '%{http_code}' \
  "http://127.0.0.1:$PORT/search?q=linux+kernel&cat=bogus&nc=$RANDOM")
[ "$code" = "200" ] || fail "unknown category returned $code"
no_chips
# the point is that the page still RENDERS and still has no chips; asking for
# results as well would make this test depend on an engine answering, and this
# network refuses some of them (recorded in docs/probe-log.tsv)
grep -q 'class="results-view"\|class="no-results"' /tmp/j5cat.html \
  || fail "an unknown category cost the page its body"
if ! grep -q 'class="result"' /tmp/j5cat.html; then
  echo "  note: no engine answered (offline or refused), so the result rows are not asserted"
fi

# 6. a category with no enabled engine answers honestly instead of with web
#    results: this build has no media index
sed -i '' 's/^engines = .*/engines = arxiv/' /tmp/j5cat.ini
pkill -9 -f "j5cat.ini" 2>/dev/null
sleep 0.5
./bin/omnisearch --config /tmp/j5cat.ini > /tmp/j5cat2.log 2>&1 &
sleep 3
body "q=linux+kernel&cat=media"
grep -q 'no-engines\|No enabled engine serves this category' /tmp/j5cat.html \
  || fail "empty category did not say why"
grep -q 'class="result"' /tmp/j5cat.html \
  && fail "empty category rendered results anyway"

pkill -9 -f "j5cat.ini" 2>/dev/null
[ "$FAIL" = "0" ] && echo "J5 categories: PASS" || { echo "J5 categories: FAIL"; exit 1; }
