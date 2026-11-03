#!/bin/sh
# L5b: Reddit's redirect lands on this server and finishes itself.
#
# The owner pressed Allow on reddit.com and nothing happened: the redirect URI
# was redreader://rr_oauth_redir, a scheme no browser delivers to this server,
# so the page simply came back unchanged. A URI that points at this instance
# means the ?code= arrives here instead, and this covers that path against a
# local stand-in for Reddit.
FAIL=0
fail() { echo "FAIL: $1"; FAIL=$((FAIL+1)); }
PORT=8212
STUB=8902
DIR=/tmp/l5bcb
KEY=FAKE_MODE

pass() { printf "  ok  %s\n" "$1"; }

kill_port() { lsof -nP -iTCP:$1 -sTCP:LISTEN 2>/dev/null | awk 'NR>1{print $2}' | xargs -r kill -9; }

kill_port $PORT; kill_port $STUB
rm -rf $DIR; mkdir -p $DIR/cache; chmod 700 $DIR
: > $DIR/hits

# a stand-in for Reddit: token endpoint, /api/v1/me, and a listing
cat > $DIR/fake.py <<'PY'
import http.server, json, sys
HITS = sys.argv[1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, obj):
        b = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header('Content-Type','application/json')
        self.send_header('Content-Length', str(len(b)))
        self.end_headers(); self.wfile.write(b)
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(n).decode()
        open(HITS,'a').write("POST %s %s\n" % (self.path, body))
        self._send({"access_token":"AT","refresh_token":"RT","expires_in":3600,
                    "token_type":"bearer","scope":"read"})
    def do_GET(self):
        open(HITS,'a').write("GET %s\n" % self.path)
        if '/me' in self.path:
            self._send({"name":"probeuser","id":"p1"})
        else:
            self._send({"data":{"children":[]}})
http.server.HTTPServer(('127.0.0.1', int(sys.argv[2])), H).serve_forever()
PY
python3 $DIR/fake.py $DIR/hits $STUB &
sleep 1
[ "$(lsof -nP -iTCP:$STUB -sTCP:LISTEN 2>/dev/null | wc -l | tr -d ' ')" -gt 0 ] \
  || { echo "SKIP: the stand-in did not start"; kill_port $STUB; exit 0; }

cat > $DIR/t.ini <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg
definitions_dir = static/engines.d
[health]
breaker_threshold = 1000
[cache]
dir = $DIR/cache
INI

OMNI_SECRETS_FILE=$DIR/secrets.ini \
OMNI_REDDIT_TOKEN_URL="http://127.0.0.1:$STUB/api/v1/access_token" \
OMNI_REDDIT_API="http://127.0.0.1:$STUB" \
  ./bin/omnisearch --config $DIR/t.ini > $DIR/s.log 2>&1 &
for _ in $(seq 1 40); do
  curl -sS -o /dev/null -m 1 "http://127.0.0.1:$PORT/" 2>/dev/null && break
  sleep 0.5
done
curl -sS -o /dev/null -m 1 "http://127.0.0.1:$PORT/" 2>/dev/null \
  || { echo "SKIP: the server did not start"; kill_port $PORT; kill_port $STUB; exit 0; }

page() { curl -sS -m 10 "http://127.0.0.1:$PORT/settings?cb=$RANDOM" -o $DIR/p.html; }
nonce() { page; grep -o 'name="n" value="[a-f0-9]*"' $DIR/p.html | head -1 | sed 's/.*value="//;s/"//'; }

save() {
  N=$(nonce)
  curl -sS -o /dev/null -m 15 -H "Sec-Fetch-Site: same-origin" -X POST \
    --data-urlencode "card=reddit" --data-urlencode "n=$N" \
    --data-urlencode "reddit_client_id=probeclient" \
    --data-urlencode "reddit_redirect_uri=$1" \
    --data-urlencode "reddit_user_agent=omnisearch/1.0" \
    "http://127.0.0.1:$PORT/settings/account-save"
}

echo "L5b reddit callback"

# 1. a custom scheme is called out, because no browser can deliver it here
save "redreader://rr_oauth_redir"
page
grep -q 'cannot come back to this server' $DIR/p.html \
  && pass "a custom scheme is named as the reason Allow did nothing" \
  || fail "the card did not explain that redreader:// cannot return"

# 2. the callback route exists and answers a redirect, not a 404
code=$(curl -sS -o /dev/null -m 10 -w "%{http_code}" \
  "http://127.0.0.1:$PORT/settings/reddit-callback?code=x&state=y")
[ "$code" = "302" ] && pass "the callback route answers 302" \
  || fail "the callback route answered $code"

# 3. Reddit's ?error=access_denied is reported, not swallowed
loc=$(curl -sS -o /dev/null -m 10 -w "%{redirect_url}" \
  "http://127.0.0.1:$PORT/settings/reddit-callback?error=access_denied&state=y")
case "$loc" in
  *codeerr=denied*) pass "declining at Reddit is reported" ;;
  *) fail "access_denied produced '$loc'" ;;
esac

# 4. a URI that points at us lets Authorise finish on its own
save "http://127.0.0.1:$PORT/settings/reddit-callback"
page
grep -q 'comes back here' $DIR/p.html \
  && pass "a callback URI is recognised as ours" \
  || fail "the card did not recognise its own callback URI"

# 5. the whole round trip: code in, token out, name back, banner shown
STATE=$(grep -o 'name="state" value="[a-f0-9]*"' $DIR/p.html | head -1 | sed 's/.*value="//;s/"//')
if [ -n "$STATE" ]; then
  loc=$(curl -sS -o /dev/null -m 20 -w "%{redirect_url}" \
    "http://127.0.0.1:$PORT/settings/reddit-callback?code=GOODCODE&state=$STATE")
  case "$loc" in
    *codeok=probeuser*) pass "the code was exchanged and the account named" ;;
    *codeok*) pass "the code was exchanged" ;;
    *) fail "the callback produced '$loc'" ;;
  esac
  case "$loc" in
    *codeok*)
      curl -sS -m 10 "$loc" 2>/dev/null | grep -q 'Connected as u/probeuser' \
        && pass "the page says who is connected" \
        || fail "the page did not name the connected account" ;;
  esac
  grep -q 'grant_type=authorization_code' $DIR/hits \
    && pass "the exchange used the authorization_code grant" \
    || fail "no authorization_code exchange reached the token endpoint"
  grep -q 'code=GOODCODE' $DIR/hits \
    && pass "the code was sent as given" \
    || fail "the code did not reach the token endpoint"
else
  fail "no state nonce was rendered, so Authorise cannot be matched"
fi

# 6. a state from another request is refused rather than exchanged
loc=$(curl -sS -o /dev/null -m 20 -w "%{redirect_url}" \
  "http://127.0.0.1:$PORT/settings/reddit-callback?code=X&state=notours")
case "$loc" in
  *codeerr=failed*) pass "a foreign state is refused" ;;
  *) fail "a foreign state produced '$loc'" ;;
esac

kill_port $PORT; kill_port $STUB
[ "$FAIL" = "0" ] && echo "L5b callback: PASS" || { echo "L5b callback: FAIL"; exit 1; }
