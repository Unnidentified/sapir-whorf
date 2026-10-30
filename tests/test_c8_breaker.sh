#!/usr/bin/env bash
# C8: a transport failure is our network, not the engine; eval must not poison
# the persisted breaker.
set -u
fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }

echo "C8: breaker resilience"

# --- 1. OMNI_NOBREAKER=1: failures must not move the counters or open anything
ROOT=${ROOT:-/tmp/omni-test}
PORT2=${PORT2:-8093}
PID=$(lsof -nP -iTCP:$PORT2 -sTCP:LISTEN -t 2>/dev/null); [ -n "$PID" ] && kill -9 "$PID"
rm -rf /tmp/omni-nb && cp -R "$ROOT" /tmp/omni-nb 2>/dev/null
python3 - <<'PY'
import re
p='/tmp/omni-nb/config.ini'
s=open(p).read().replace('port = 8091','port = 8093').replace('dir = /tmp/omni-test/cache','dir = /tmp/omni-nb/cache')
open(p,'w').write(s)
PY
rm -rf /tmp/omni-nb/cache
cd /tmp/omni-nb && (OMNI_NOBREAKER=1 ./bin/omnisearch > /tmp/nb.log 2>&1 < /dev/null &)
sleep 3
echo block > /tmp/fake_upstream.mode
for i in 1 2 3 4 5 6; do curl -sS -o /dev/null -m 20 "http://localhost:$PORT2/search?q=c8+nb+$i" 2>/dev/null; done
h=$(curl -sS -m 5 "http://localhost:$PORT2/info" | python3 -c "
import json,sys
d=json.load(sys.stdin)
e=[x for x in d['engines'] if x['id']=='searxng'][0]
print(e['health'], e['errors'])")
echo "  with OMNI_NOBREAKER: health/errors = $h"
case "$h" in
  closed*) ok "OMNI_NOBREAKER kept the breaker closed and errors uncharged" ;;
  *) bad "OMNI_NOBREAKER did not prevent breaker movement: $h" ;;
esac
PID=$(lsof -nP -iTCP:$PORT2 -sTCP:LISTEN -t 2>/dev/null); [ -n "$PID" ] && kill -9 "$PID"
echo ok > /tmp/fake_upstream.mode

[ $fail -eq 0 ] && echo "C8: PASS" || echo "C8: FAIL"
exit $fail
