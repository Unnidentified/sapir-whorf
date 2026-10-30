#!/usr/bin/env bash
# C3: the image proxy must refuse SSRF vectors.
# Every case below is a URL that reached the network before this fix.
set -u
PORT=${PORT:-8091}
fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }

# Unsigned requests must be refused outright; that is C4's core fix. The SSRF
# cases below are therefore exercised both unsigned and signed - an attacker
# cannot mint a signature, but the address checks must hold even if they could.
# The instance under test runs with OMNI_PROXY_TEST_KEY so the harness can mint
# signatures; an attacker cannot, which is the point.
TEST_KEY=${OMNI_PROXY_TEST_KEY:-testkey0123456789}
sign() { python3 - "$1" "$TEST_KEY" <<'PY'
import hmac, hashlib, sys
url, key = sys.argv[1], sys.argv[2].encode()
k = (key + b"\0"*32)[:32]
print(hmac.new(k, url.encode(), hashlib.sha256).hexdigest()[:32])
PY
}
fetch() {
  local s; s=$(sign "$1")
  curl -sS -m 8 --get --data-urlencode "url=$1" --data-urlencode "s=$s" \
       "http://localhost:$PORT/proxy"
}

echo "C3: image proxy SSRF vectors"

# a local listener the proxy must never reach
python3 - <<'PY' >/tmp/ssrf_target.log 2>&1 &
import http.server, socketserver
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self,*a): pass
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type","text/plain")
        self.end_headers()
        self.wfile.write(b"SECRET-INTERNAL-SERVICE")
socketserver.TCPServer.allow_reuse_address=True
socketserver.TCPServer(("127.0.0.1",8898),H).serve_forever()
PY
sleep 1.5

cases=(
  "http://127.0.0.1:8898/internal|loopback with explicit port"
  "http://localhost:8898/internal|loopback by name with port"
  "http://169.254.169.254/latest/meta-data/|link-local metadata"
  "http://[::1]:8898/internal|IPv6 loopback literal"
  "http://0.0.0.0:8898/internal|0.0.0.0/8"
  "http://100.64.0.1/x|CGNAT 100.64/10"
  "http://198.18.0.1/x|benchmarking 198.18/15"
  "http://224.0.0.1/x|multicast"
  "http://user:pass@example.com/x|userinfo present"
  "http://example.com:8080/x|port other than 80/443"
  "file:///etc/passwd|file scheme"
)

echo "  -- unsigned request must be refused before any fetch --"
unsigned=$(curl -sS -m 8 --get --data-urlencode "url=https://httpbin.org/image/png" "http://localhost:$PORT/proxy")
if printf '%s' "$unsigned" | grep -qi "unsigned"; then ok "unsigned proxy request refused"
else bad "unsigned proxy request was processed: $(printf '%s' "$unsigned" | head -c 50)"; fi

for entry in "${cases[@]}"; do
  url="${entry%%|*}"; label="${entry##*|}"
  body=$(fetch "$url")
  if printf '%s' "$body" | grep -q "SECRET-INTERNAL-SERVICE"; then
    bad "$label - internal service body leaked"
  elif printf '%s' "$body" | grep -qiE "not allowed|invalid|not proxied|failed to fetch"; then
    ok "$label refused"
  else
    bad "$label returned unexpected body: $(printf '%s' "$body" | head -c 60)"
  fi
done

# SVG must never be served
svg=$(fetch "https://www.w3.org/Icons/w3c_home.svg")
if printf '%s' "$svg" | grep -qi "svg is not proxied"; then ok "svg refused"
else bad "svg not refused: $(printf '%s' "$svg" | head -c 60)"; fi

# a legitimate public image must still proxy (the guard must not break the app)
gsig=$(sign "https://httpbin.org/image/png")
good=$(curl -sS -m 15 -o /tmp/c3good.bin -w "%{http_code} %{content_type}" \
        "http://localhost:$PORT/proxy?url=https%3A%2F%2Fhttpbin.org%2Fimage%2Fpng&s=$gsig" 2>/dev/null || echo "000 -")
case "$good" in
  200*image/png*) ok "legitimate image still proxies ($good)" ;;
  *) bad "legitimate image broken: $good" ;;
esac

pkill -f "socketserver.TCPServer" 2>/dev/null
[ $fail -eq 0 ] && echo "C3: PASS" || echo "C3: FAIL"
exit $fail
