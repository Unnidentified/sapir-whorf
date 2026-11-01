#!/bin/sh
# K6: what impersonation state does this binary actually have, and what does
# our TLS handshake look like next to a real browser's?
#
# No third party is contacted. Local listeners record the ClientHello our fetch
# path sends and the one Chromium sends; the test compares them and the
# reported state.
#
# The library itself is a build-time choice (src/Scraping/Impersonate.h). When
# the binary has it, the test requires the two hellos to match the pinned
# target; when it does not, it requires the binary to SAY so and requires the
# measured difference to be the one that motivates the library.
set -u
cd "$(dirname "$0")/.." || exit 1

PORT=8166
LISTEN=8893
DIR=/tmp/k6imp
BIN=./bin/omnisearch
fail=0
pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

cleanup() {
    [ -n "${SRV_PID:-}" ] && kill "$SRV_PID" 2>/dev/null
    [ -n "${LST_PID:-}" ] && kill "$LST_PID" 2>/dev/null
    wait 2>/dev/null
}
trap cleanup EXIT INT TERM

rm -rf "$DIR"; mkdir -p "$DIR/cache" "$DIR/defs"

free_port() {
    if command -v lsof >/dev/null 2>&1; then
        for p in $(lsof -nP -tiTCP:"$1" -sTCP:LISTEN 2>/dev/null); do
            kill -9 "$p" 2>/dev/null
        done
    fi
}
free_port "$LISTEN"
free_port "$PORT"
sleep 0.5

# 1. what the binary says about itself
"$BIN" --config /dev/null > "$DIR/startup.log" 2>&1 &
sleep 2
pkill -f "config /dev/null" 2>/dev/null
line=$(grep -i "impersonation:" "$DIR/startup.log" | head -1)
case "$line" in
    *"off (stock libcurl)"*) say_pass ;;
    *"libcurl-impersonate"*) say_pass ;;
    *) say_fail "the startup line does not say which library: '$line'" ;;
esac
build_has_library=0
case "$line" in *"libcurl-impersonate"*) build_has_library=1 ;; esac

# 2. a definition pointing at the local listener
cat > "$DIR/defs/tls_probe.json" <<JSON
{
  "id": "tls_probe",
  "name": "TLS probe",
  "base_url": "https://127.0.0.1:$LISTEN",
  "search_url": "https://127.0.0.1:$LISTEN/search?q=%s",
  "xpath_result": "//result",
  "xpath_title": ".//title",
  "xpath_link": ".//link",
  "xpath_snippet": ".//snippet",
  "verticals": ["web"],
  "enabled": true
}
JSON

cat > "$DIR/t.ini" <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = tls_probe
definitions_dir = $DIR/defs
[cache]
dir = $DIR/cache
INI

# 3. our fetch path, against a listener that records one hello
python3 tests/tls_hello_capture.py --port "$LISTEN" --out "$DIR/hello-ours.json" \
    --count 1 --timeout 20 > "$DIR/listener-ours.out" 2>&1 &
LST_PID=$!
sleep 0.8

$BIN --config "$DIR/t.ini" > "$DIR/srv.log" 2>&1 &
SRV_PID=$!
sleep 2
curl -sS -o /dev/null -m 10 "http://127.0.0.1:$PORT/search?q=hello&nc=$RANDOM" 2>/dev/null
wait $LST_PID 2>/dev/null
sleep 0.5

# 4. a real browser, against a second listener (our path retries, so a shared
# capture would fill both slots with our own connections)
python3 tests/tls_hello_capture.py --port "$LISTEN" --out "$DIR/hello-browser.json" \
    --count 1 --timeout 20 > "$DIR/listener-browser.out" 2>&1 &
LST_PID=$!
sleep 0.8
/tmp/pw/bin/python - "$LISTEN" <<BROWSERPY 2>/dev/null || true
import asyncio, sys
try:
    from playwright.async_api import async_playwright
except ImportError:
    sys.exit(0)
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch()
        pg = await b.new_page()
        try:
            await pg.goto("https://127.0.0.1:%s/" % sys.argv[1], timeout=5000)
        except Exception:
            pass
        await b.close()
asyncio.run(main())
BROWSERPY
wait $LST_PID 2>/dev/null
sleep 0.5

if [ ! -s "$DIR/hello-ours.json" ] || [ ! -s "$DIR/hello-browser.json" ]; then
    say_fail "a listener captured no ClientHello"
else
    python3 - "$DIR/hello-ours.json" "$DIR/hello-browser.json" "$build_has_library" <<COMPAREPY && say_pass || fail=1
import json, sys

ours = json.load(open(sys.argv[1]))[0]
browser = json.load(open(sys.argv[2]))[0]
with_library = sys.argv[3] == "1"

def show(label, h):
    return (f"{label}: alpn={h['alpn']} ciphers={len(h['ciphers'])} "
            f"ext={len(h['extensions'])} sha={h['sha256'][:16]}")

print(show("ours   ", ours))
print(show("browser", browser))

if with_library:
    if ours["sha256"] != browser["sha256"]:
        print("FAIL: the binary links libcurl-impersonate but its ClientHello "
              "is not the browser's")
        sys.exit(1)
    if "h2" not in ours["alpn"]:
        print("FAIL: an impersonated hello must offer h2")
        sys.exit(1)
    sys.exit(0)

# stock libcurl: the gTAP the library exists to remove
if ours["sha256"] == browser["sha256"]:
    print("FAIL: stock libcurl sent the same ClientHello as Chromium - either "
          "the build secretly has the library, or the capture is wrong")
    sys.exit(1)
if "h2" not in browser["alpn"]:
    print("FAIL: Chromium did not offer h2; the comparison is not meaningful")
    sys.exit(1)
if ours["alpn"] != ["http/1.1"]:
    print(f"FAIL: this build forces HTTP/1.1, so the hello should offer only "
          f"http/1.1, not {ours['alpn']}")
    sys.exit(1)
COMPAREPY
    if [ "$fail" -ne 0 ]; then
        say_fail "the ClientHello comparison failed (see above)"
    else
        say_pass
    fi
fi

# 5. the page reports the same state as the startup line
curl -sS -o "$DIR/info.json" -m 10 "http://127.0.0.1:$PORT/info" 2>/dev/null
python3 - "$DIR/info.json" "$build_has_library" <<INFOPY && say_pass || say_fail "/info does not report the impersonation state"
import json, sys
d = json.load(open(sys.argv[1]))
state = d.get("impersonation", "")
with_library = sys.argv[2] == "1"
if not state:
    print("FAIL: /info has no impersonation field")
    sys.exit(1)
if with_library and "stock" in state:
    print(f"FAIL: /info says '{state}' but the binary has the library")
    sys.exit(1)
if not with_library and "stock" not in state:
    print(f"FAIL: /info says '{state}' but the binary has stock libcurl")
    sys.exit(1)
INFOPY

echo "K6 impersonation: $pass passed, $fail failed (library: $([ "$build_has_library" = 1 ] && echo yes || echo no))"
[ "$fail" -eq 0 ]
