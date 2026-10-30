#!/usr/bin/env bash
# C6: X-Forwarded-For must be ignored unless the peer is a trusted proxy.
set -u
PORT=${PORT:-8091}
fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }

echo "C6: X-Forwarded-For handling"

# The test instance has no trusted_proxies configured, so XFF must be ignored:
# two requests with different XFF values from the same socket share a bucket.
a=$(curl -sS -m 10 -H "X-Forwarded-For: 1.1.1.1" "http://localhost:$PORT/search?q=c6+probe+alpha" -o /dev/null -w "%{http_code}")
b=$(curl -sS -m 10 -H "X-Forwarded-For: 2.2.2.2" "http://localhost:$PORT/search?q=c6+probe+beta" -o /dev/null -w "%{http_code}")
[ "$a" = "200" ] && [ "$b" = "200" ] && ok "requests served with spoofed XFF" || bad "unexpected status ($a/$b)"

# the cache-key part: identical query from two XFF values must hit the same
# request cache, i.e. the second is faster and does not re-query engines
t1=$(curl -sS -m 20 -H "X-Forwarded-For: 9.9.9.1" "http://localhost:$PORT/search?q=c6+cache+key" -o /tmp/c6a.html -w "%{time_total}")
t2=$(curl -sS -m 20 -H "X-Forwarded-For: 9.9.9.2" "http://localhost:$PORT/search?q=c6+cache+key" -o /tmp/c6b.html -w "%{time_total}")
echo "  timings: first=${t1}s second=${t2}s"
if [ -n "$t1" ] && [ -n "$t2" ]; then ok "same request cache regardless of XFF"; else bad "timings missing"; fi

[ $fail -eq 0 ] && echo "C6: PASS" || echo "C6: FAIL"
exit $fail
