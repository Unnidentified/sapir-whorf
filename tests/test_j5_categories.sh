#!/bin/bash
# J5.3: a category tab narrows which engines are asked, keeps itself on the
# pager, and says so when nothing serves it rather than silently answering with
# web results.
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
# the strip only: the chrome's own All tab is also class="active", and counting
# it made this test pass for the wrong reason until it was scoped
strip() { python3 -c "
import re, sys
h = open('/tmp/j5cat.html').read()
m = re.search(r'<nav class=\"filter-tabs\".*?</nav>', h, re.S)
print(m.group(0) if m else '')
"; }

# 1. the strip exists, and it names only what an enabled engine declares: this
#    build is the three engines above, so academic is there and code is not
body "q=linux+kernel"
grep -q 'class="filter-tabs"' /tmp/j5cat.html || fail "no filter strip"
grep -q "cat=academic" /tmp/j5cat.html || fail "strip has no academic"
grep -q "cat=code" /tmp/j5cat.html \
  && fail "strip offers code, but no enabled engine declares it"

# 2. exactly one row in the strip is active
strip > /tmp/j5cat.strip
active=$(grep -o 'chip[^"]*active' /tmp/j5cat.strip | wc -l | tr -d ' ')
[ "$active" = "1" ] || fail "expected one active row, saw $active"
grep -q 'q=linux%20kernel" class="chip chip-all active"' /tmp/j5cat.strip \
  || fail "All is not active on an unfiltered search"

# 3. a category is active on its own page and nothing else is
body "q=linux+kernel&cat=academic"
strip > /tmp/j5cat.strip
grep -q 'cat=academic" class="chip chip-cat active"' /tmp/j5cat.strip \
  || fail "academic is not active on its own page"
grep -q 'q=linux%20kernel" class="chip chip-all active"' /tmp/j5cat.strip \
  && fail "All is active while a category is selected"

# 4. the pager keeps the category
body "q=linux+kernel&cat=academic&p=2"
grep -q 'cat=academic' /tmp/j5cat.html || fail "pager dropped the category"

# J5.4: every engine that was asked appears with its count, and selecting one
# makes exactly that chip active - one active row across both groups
body "q=linux+kernel"
strip > /tmp/j5cat.strip
eng=$(grep -c 'chip-eng' /tmp/j5cat.strip)
if [ "$eng" -lt 1 ]; then
  echo "  skip: no engine was asked (every candidate is paused or unavailable)"
else
grep -q 'chip-eng[^>]*>.*(' /tmp/j5cat.strip || fail "an engine chip has no count"
act=$(grep -o 'chip[^"]*active' /tmp/j5cat.strip | wc -l | tr -d ' ')
[ "$act" = "1" ] || fail "expected one active chip across both groups, saw $act"
fi

# an engine that is enabled and routed: selecting it makes its chip active. An
# id that is not available falls back to All, which is why this uses ddg.
body "q=linux+kernel&engine=ddg"
strip > /tmp/j5cat.strip
if grep -q 'chip-eng' /tmp/j5cat.strip; then
  grep -q 'chip-eng active' /tmp/j5cat.strip \
    || fail "the selected engine is not active"
fi
grep -q 'chip-all active' /tmp/j5cat.strip \
  && fail "All is active while an engine is selected"

# 5. an unknown category is ignored, not fatal
code=$(curl -sS -m 30 -o /tmp/j5cat.html -w '%{http_code}' \
  "http://127.0.0.1:$PORT/search?q=linux+kernel&cat=bogus&nc=$RANDOM")
[ "$code" = "200" ] || fail "unknown category returned $code"
grep -q 'class="filter-tabs"' /tmp/j5cat.html \
  || fail "unknown category removed the strip"

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
