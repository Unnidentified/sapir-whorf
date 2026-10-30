#!/usr/bin/env bash
# C1: a stale-cache substitution must not be recorded as a live success.
#
# Warm the per-query stale entry for ONE query, flip the upstream to a challenge
# page, then re-ask that query from a *different client key* (X-Forwarded-For)
# so the rendered-request cache misses but the per-engine stale cache still
# matches. The body is then served from stale cache.
#   buggy code: health records OK  -> ok rises, errors flat, breaker stays shut
#   fixed code: health records the live failure -> errors rise
set -u
PORT=${PORT:-8091}
MODE=/tmp/fake_upstream.mode
ENC="linux+kernel"
CACHE_DIR=${CACHE_DIR:-/tmp/omni-test/cache}

fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }
mode(){ echo "$1" > "$MODE"; }
field(){ curl -sS -m 5 "http://localhost:$PORT/info" | python3 -c "
import json,sys
d=json.load(sys.stdin)
e=[x for x in d.get('engines',[]) if x.get('id')=='searxng']
print(e[0].get('$1','?') if e else '?')"; }

echo "C1: stale substitution must not mask a blocked engine"

rm -f "$CACHE_DIR/engine_health.tsv"
find "$CACHE_DIR" -name "*.cache" -delete 2>/dev/null

mode ok
curl -sS -o /tmp/c1warm.html -m 30 -H "X-Forwarded-For: 10.9.9.1" \
  "http://localhost:$PORT/search?q=$ENC&engine=searxng" >/dev/null
sleep 0.4
o1=$(field ok); e1=$(field errors)
echo "  warmed: ok=$o1 errors=$e1 rendered=$(grep -c 'class="result"' /tmp/c1warm.html)"

mode block
curl -sS -o /tmp/c1block.html -m 30 -H "X-Forwarded-For: 10.9.9.2" \
  "http://localhost:$PORT/search?q=$ENC&engine=searxng" >/dev/null
sleep 0.4
o2=$(field ok); e2=$(field errors)
cached_note=$(grep -c 'cached-note' /tmp/c1block.html || true)
echo "  blocked fetch, fresh client: ok=$o2 errors=$e2 cached-note=$cached_note"

if [ "$cached_note" -gt 0 ]; then ok "body was served from the stale cache"
else bad "stale path not exercised (no cached-note) - test proves nothing"; fi
if [ "$e2" -gt "$e1" ]; then ok "error counter rose ($e1 -> $e2)"
else bad "error counter flat ($e1 -> $e2) - stale reported as success"; fi
if [ "$o2" -le "$o1" ]; then ok "ok counter did not rise ($o1 -> $o2)"
else bad "ok counter rose ($o1 -> $o2) - stale reported as success"; fi

mode ok
[ $fail -eq 0 ] && echo "C1: PASS" || echo "C1: FAIL"
exit $fail
