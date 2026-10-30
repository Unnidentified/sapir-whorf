#!/usr/bin/env bash
# C5: settings must not store unvalidated values or split the redirect header.
set -u
PORT=${PORT:-8091}
fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }

echo "C5: settings validation and redirect encoding"

# --- arbitrary theme / locale must not be written to cookies ---
hdr=$(curl -sS -i -m 8 "http://localhost:$PORT/save_settings?theme=..%2F..%2Fetc%2Fpasswd&locale=zz_ZZ" 2>&1)
if printf '%s' "$hdr" | grep -qi "set-cookie: theme="; then
  bad "hostile theme written to a cookie"
else
  ok "hostile theme rejected"
fi
if printf '%s' "$hdr" | grep -qi "set-cookie: locale=zz_ZZ"; then
  bad "unknown locale written to a cookie"
else
  ok "unknown locale rejected"
fi

# --- a real theme must still be accepted ---
hdr2=$(curl -sS -i -m 8 "http://localhost:$PORT/save_settings?theme=amber" 2>&1)
if printf '%s' "$hdr2" | grep -qi "set-cookie: theme=amber"; then ok "valid theme accepted"
else bad "valid theme was rejected"; fi

# --- CR/LF in the query must not reach the Location header ---
# beaker rejects a raw CR/LF in the request line with 400 (defence in depth).
# Either a 400 or a Location without the injected header is acceptable; an
# injected header is not.
code=$(curl -sS -o /tmp/c5body.txt -D /tmp/c5hdr.txt -m 8 -w "%{http_code}" \
       "http://localhost:8091/save_settings?q=hello%0d%0aX-Injected:%20yes" 2>&1)
if grep -qi "x-injected" /tmp/c5hdr.txt; then
  bad "response splitting: injected header present in Location"
else
  ok "CR/LF never reaches the header (http $code)"
fi

# --- characters that DO get through must be encoded in Location ---
curl -sS -D /tmp/c5hdr2.txt -o /dev/null -m 8 \
     "http://localhost:8091/save_settings?q=a%26b%3Dc%20d" 2>&1
loc=$(grep -i "^location:" /tmp/c5hdr2.txt | head -1 | tr -d '\r')
case "$loc" in
  *"q=a%26b%3Dc%20d"*) ok "redirect query is percent-encoded: $loc" ;;
  *"&b="*)             bad "redirect query unescaped, params split: $loc" ;;
  *)                   bad "unexpected redirect: ${loc:-<none>}" ;;
esac

hdrlines=$(wc -l < /tmp/c5hdr2.txt | tr -d ' ')
[ "$hdrlines" -lt 40 ] && ok "response header block sane ($hdrlines lines)" || bad "suspicious header growth ($hdrlines lines)"

[ $fail -eq 0 ] && echo "C5: PASS" || echo "C5: FAIL"
exit $fail
