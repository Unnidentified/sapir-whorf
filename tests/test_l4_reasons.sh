#!/bin/sh
# L4: every unavailable row says something true and actionable.
#
# "Disabled by the server administrator" was false for the owner, who IS the
# administrator, and it gave no way forward. Each row now carries its own
# reason, and a row that waits for a field names that field. This test reads the
# rendered page and the provider table and asserts both.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8087
fail=0; pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

if ! curl -sS -m 4 -o /dev/null "http://127.0.0.1:$PORT/settings" 2>/dev/null; then
    echo "L4: SKIP -- no server on $PORT"
    exit 0
fi

curl -sS -o /tmp/l4page.html -m 20 "http://127.0.0.1:$PORT/settings?cb=$RANDOM" 2>/dev/null

# 1. no row says "disabled by the server administrator" any more
if grep -q "disabled by the server administrator" /tmp/l4page.html; then
    say_fail "a row still says 'disabled by the server administrator'"
    grep -o 'Unavailable[^<]*' /tmp/l4page.html | grep -i "administrator" | head -3
else
    say_pass
fi

# 2. each unavailable row names a reason from the enum, and each reason has a fix
python3 - <<'PY' > /tmp/l4reasons.txt
import re
h = open('/tmp/l4page.html').read()
states = re.findall(r'settings-provider-name">([^<]*)</span>\s*<span class="settings-provider-state">([^<]*)', h)
seen = {}
for name, state in states:
    state = state.strip()
    if not state.startswith('Unavailable'):
        continue
    seen.setdefault(state, []).append(name.strip())
for state, names in seen.items():
    print(f'{state}\t{", ".join(names)}')
PY
unavailable=$(wc -l < /tmp/l4reasons.txt | tr -d ' ')
[ "$unavailable" -gt 0 ] && say_pass || say_fail "no unavailable rows found to check"

# every unavailable sentence must carry a fix: a field, a reachable host, or a
# statement of what is closed
bad=0
while IFS='	' read -r state names; do
    case "$state" in
    *"Needs "*|*"Fill it in below"*|*"Enter a working one"*|*"no longer issues"*|*"Remove the -id"*|*"Not part of this build"*) : ;;
    *) bad=$((bad + 1)); printf 'FAIL no fix offered: %s (%s)\n' "$state" "$names" ;;
    esac
done < /tmp/l4reasons.txt
[ "$bad" -eq 0 ] && say_pass || fail=$((fail + bad))

# 3. the one row that can be fixed from the page names its field
grep -q "Needs searxng_url" /tmp/l4page.html && say_pass \
    || say_fail "SearXNG does not name the field it needs"
# 4. Google is honest about the closed API
grep -q "Google no longer issues keys" /tmp/l4page.html && say_pass \
    || say_fail "the Google row does not say the API is closed"
# 5. YaCy names the peer it cannot reach
# the apostrophe arrives escaped in the HTML, so match the escaped form
grep -qE "Can(&apos;|')t reach 127.0.0.1:8090" /tmp/l4page.html && say_pass \
    || say_fail "the YaCy row does not name the peer it tried"

# 6. Open Library is in the build and enabled, and it is a JSON definition
grep -q '"enabled": true' static/engines.d/openlibrary.json && say_pass \
    || say_fail "Open Library is still disabled by default"
grep -q '"json_results"' static/engines.d/openlibrary.json && say_pass \
    || say_fail "Open Library is not a JSON definition"

# 7. the compose file is loopback-only on both services, and hands the app a URL
grep -q '127.0.0.1:8087:8087' docker-compose.yml && say_pass \
    || say_fail "the app is still published on every interface"
grep -q '127.0.0.1:8888:8080' docker-compose.yml && say_pass \
    || say_fail "SearXNG is published on every interface"
grep -q 'OMNI_SEARXNG_URL' docker-compose.yml && say_pass \
    || say_fail "the compose file does not give the app the bridge URL"
grep -q 'json' searxng/settings.yml && say_pass \
    || say_fail "the SearXNG settings do not enable the JSON format"

echo "L4 reasons: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
