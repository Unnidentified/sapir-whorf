#!/bin/sh
# L2 + L6.10: the ids this project has shipped, in one table.
#
# DuckDuckGo Lite is gone from the build, but a browser or a config written by
# an older release still names it. Both must keep working without an error and
# without a warning on every request - and the exclusion case must NOT map, or
# "-ddg-lite" would switch off the engine that replaced it.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/omnisearch
PORT=8087
fail=0; pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

# 1. the grep the acceptance asks for: only the alias table and the G1 doc
# the alias line and its comment are the only permitted mentions, plus the one
# comment in the config parser explaining why an exclusion is not mapped
hits=$(grep -rn "ddg-lite\|lite\.duckduckgo" src templates static tools 2>/dev/null \
       | grep -v "^src/Utility/Utility.c" | grep -v "^src/Scraping/ScrapingParsers.c:9[0-9][0-9]:" \
       | wc -l | tr -d ' ')
[ "$hits" -eq 0 ] && say_pass || { say_fail "$hits stray ddg-lite references"; \
    grep -rn "ddg-lite\|lite\.duckduckgo" src templates static tools 2>/dev/null | head -4; }

# 2. no ddg-lite in the provider table
./bin/omnisearch --config /tmp/ol4.ini --providers 2>/dev/null | grep -q "ddg-lite" \
    && say_fail "ddg-lite is still in the provider table" || say_pass

# 3. every id this project has shipped, in a cookie: no error, the current
#    state is what the mapping says
if curl -sS -m 4 -o /dev/null "http://127.0.0.1:$PORT/settings" 2>/dev/null; then
    for legacy in startpage mojeek ddg-lite ddg_lite; do
        code=$(curl -sS -o /tmp/l2page.html -m 20 -w '%{http_code}' \
               -b "engines=off=$legacy" \
               "http://127.0.0.1:$PORT/settings?cb=$RANDOM" 2>/dev/null)
        [ "$code" = "200" ] && say_pass || say_fail "cookie off=$legacy returned $code"
    done
    # the old enabled-set cookie form, too
    code=$(curl -sS -o /tmp/l2page.html -m 20 -w '%{http_code}' \
           -b "engines=ddg-lite|bing" \
           "http://127.0.0.1:$PORT/settings?cb=$RANDOM" 2>/dev/null)
    [ "$code" = "200" ] && say_pass || say_fail "legacy enabled-set cookie returned $code"

    # off=ddg-lite|bing means: bing off, ddg on (the lite name is ddg's old name)
    curl -sS -o /tmp/l2page.html -m 20 -b "engines=off=ddg-lite|bing" \
         "http://127.0.0.1:$PORT/settings?cb=$RANDOM" 2>/dev/null
    python3 - <<'PY'
import re, sys
h = open('/tmp/l2page.html').read()
def checked(pid):
    m = re.search(r'id="prov_%s"(.*?)</label>' % pid, h, re.S)
    return 'checked' in m.group(1) if m else None
ok = checked('bing') is False and checked('ddg') is True
print('ok' if ok else 'bad', checked('bing'), checked('ddg'))
PY
    if [ "$(python3 -c "
import re
h=open('/tmp/l2page.html').read()
def c(p):
    m=re.search(r'id=\"prov_%s\"(.*?)</label>' % p, h, re.S)
    return 'checked' in m.group(1) if m else None
print('y' if (c('bing') is False and c('ddg') is True) else 'n')
")" = "y" ]; then
        say_pass
    else
        say_fail "off=ddg-lite|bing did not leave Bing off and DuckDuckGo on"
    fi
else
    echo "  skip: no server on $PORT (the cookie half needs one)"
fi

# 4. a config line naming the removed id: one note, and ddg stays on
printf '[engines]\nengines="*,-ddg-lite"\n' > /tmp/l2engines.ini
out=$(timeout 10 $BIN --config /tmp/l2engines.ini --check-config 2>&1)
printf '%s' "$out" | grep -q "NOTE.*ddg-lite" && say_pass \
    || say_fail "no startup note for the removed id"
notes=$(printf '%s' "$out" | grep -c "NOTE.*ddg-lite")
[ "$notes" -eq 1 ] && say_pass || say_fail "$notes notes for one removal (expected 1)"
printf '%s' "$out" | grep -q "WARN.*ddg-lite" && say_fail "the removal produced a WARN" || say_pass
printf '%s' "$out" | sed -n 's/^  engines_enabled=//p' | tr ',' '\n' | grep -qx "ddg" \
    && say_pass || say_fail "-ddg-lite also switched ddg off"

echo "L2 legacy ids: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
