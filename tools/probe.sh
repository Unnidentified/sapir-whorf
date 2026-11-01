#!/usr/bin/env bash
# One request. No retries, no cache, no breaker. Dumps status, headers and body
# so an engine can be diagnosed instead of guessed at.
#
#   tools/probe.sh <engine> <query> [http1.1|http2] [plain|browser] [nocookie|engine]
#
# Prints a one-line verdict plus the paths of the dump files. Pace calls: one
# per cell, a minute apart - the probe must not become the thing that gets us
# blocked.
set -u

ENGINE=${1:-}
QUERY=${2:-linux kernel}
HTTPV=${3:-http2}
HDRMODE=${4:-plain}
COOKIEMODE=${5:-nocookie}

[ -z "$ENGINE" ] && { echo "usage: $0 <engine> <query> [http1.1|http2] [plain|browser] [nocookie|engine]" >&2; exit 2; }

UA_PLAIN="Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.2 Safari/605.1.15"
UA_BROWSER="Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

case "$ENGINE" in
  ddg-html) URL="https://html.duckduckgo.com/html/?q=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  brave)    URL="https://search.brave.com/search?q=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  bing)     URL="https://www.bing.com/search?q=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  yahoo)    URL="https://search.yahoo.com/search?p=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  *) echo "unknown engine: $ENGINE" >&2; exit 2 ;;
esac

# H1/H5: never burst live engines. One probe per engine per 60s, at most three
# per engine per hour, enforced with a lock so parallel runs cannot slip through.
LOCK=/tmp/omnisearch-probe.lock
STATE_DIR=/tmp/omnisearch-probe-state
mkdir -p "$STATE_DIR"
exec 9>"$LOCK"
if command -v flock >/dev/null 2>&1; then
  flock -w 120 9 || { echo "probe: another probe is running" >&2; exit 3; }
fi
STAMP="$STATE_DIR/$ENGINE.stamp"
HOUR_LOG="$STATE_DIR/$ENGINE.hour"
now=$(date +%s)
if [ -f "$STAMP" ]; then
  last=$(cat "$STAMP" 2>/dev/null || echo 0)
  wait_for=$((60 - (now - last)))
  if [ "$wait_for" -gt 0 ]; then
    echo "probe: pacing $ENGINE, sleeping ${wait_for}s (H1)" >&2
    sleep "$wait_for"
    now=$(date +%s)
  fi
fi
if [ -f "$HOUR_LOG" ]; then
  # keep only stamps from the last hour
  awk -v cut=$((now - 3600)) '$1 >= cut' "$HOUR_LOG" > "$HOUR_LOG.tmp" 2>/dev/null
  mv "$HOUR_LOG.tmp" "$HOUR_LOG" 2>/dev/null || true
  count=$(wc -l < "$HOUR_LOG" | tr -d ' ')
  if [ "${count:-0}" -ge 3 ]; then
    echo "probe: $ENGINE already probed 3 times this hour; refusing (H1)" >&2
    exit 4
  fi
fi
echo "$now" > "$STAMP"
echo "$now" >> "$HOUR_LOG"

OUT=/tmp/probe-${ENGINE}-${HTTPV}-${HDRMODE}-${COOKIEMODE}
JAR="$OUT.cookies"
rm -f "$OUT.body" "$OUT.hdr" "$JAR"

# The app follows redirects (Brave 302s to safe.search.brave.com), so the probe
# must too - otherwise it reports a 302 and calls the cell "unknown", which is a
# probe defect masquerading as an engine finding.
ARGS=(-sS -L -m 20 -o "$OUT.body" -D "$OUT.hdr" -w "%{http_code}")
[ "$HTTPV" = "http1.1" ] && ARGS+=(--http1.1)
[ "$HTTPV" = "http2" ] && ARGS+=(--http2)

if [ "$HDRMODE" = "browser" ]; then
  ARGS+=(-A "$UA_BROWSER")
  ARGS+=(-H "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8")
  ARGS+=(-H "Accept-Language: en-GB,en;q=0.9")
  ARGS+=(-H "Sec-Fetch-Dest: document" -H "Sec-Fetch-Mode: navigate" -H "Sec-Fetch-Site: none" -H "Sec-Fetch-User: ?1")
  ARGS+=(-H "Upgrade-Insecure-Requests: 1")
  ARGS+=(-H "Sec-CH-UA: \"Chromium\";v=\"131\", \"Not_A Brand\";v=\"24\"")
  ARGS+=(-H "Sec-CH-UA-Mobile: ?0" -H "Sec-CH-UA-Platform: \"Windows\"")
else
  ARGS+=(-A "$UA_PLAIN")
  ARGS+=(-H "Accept-Language: en-GB,en;q=0.9")
fi

# engine-specific cookies, from SearXNG's per-engine request profiles
if [ "$COOKIEMODE" = "engine" ]; then
  case "$ENGINE" in
    brave)
      ARGS+=(-b "safesearch=0; useLocation=0; summarizer=0; country=gb; ui_lang=en-gb") ;;
    ddg-html)
      ARGS+=(-c "$JAR" -b "$JAR" -b "kl=gb-en; df=en-GB; ad=0; ah=0; l=en-GB")
      ARGS+=(-H "Referer: https://duckduckgo.com/") ;;
    bing)
      ARGS+=(-c "$JAR" -b "$JAR") ;;
    yahoo)
      ARGS+=(-c "$JAR" -b "$JAR") ;;
  esac
fi

CODE=$(curl "${ARGS[@]}" "$URL" 2>"$OUT.err" || echo "000")
SIZE=$(wc -c < "$OUT.body" 2>/dev/null | tr -d ' ')

# What does the body actually contain? These markers are the ones the parsers
# key on, so "did we get a results page" is answered from the body, not the code.
contains() { grep -qi -- "$1" "$OUT.body" 2>/dev/null; }
VERDICT="unknown"
case "$ENGINE" in
  bing)     contains "b_algo" && VERDICT="results(b_algo)" || { contains "b_results" && VERDICT="shell(b_results, no b_algo)"; }; ;;
  ddg-html) contains "result__a\|result__snippet" && VERDICT="results" || { contains "anomaly\|challenge" && VERDICT="challenge"; }; ;;
  brave)    contains "snippet" && VERDICT="results(html)" || { contains "challengeSet\|challenge" && VERDICT="challenge"; }; ;;
  yahoo)    contains "compTitle" && VERDICT="results" || { contains "captcha\|challenge" && VERDICT="challenge"; }; ;;
esac
# Only call it a captcha when no results were found: a 325 KB results page
# contains the word "captcha" in its JavaScript, which made every good Brave
# probe read as half-blocked.
case "$VERDICT" in
  results*) ;;
  *) contains "captcha\|unusual traffic\|are you a robot\|turnstile" && VERDICT="$VERDICT +captcha-marker" ;;
esac

printf '%-10s %-7s %-7s %-9s http=%-4s bytes=%-7s %s\n' \
  "$ENGINE" "$HTTPV" "$HDRMODE" "$COOKIEMODE" "$CODE" "$SIZE" "$VERDICT"
printf '  body=%s hdr=%s\n' "$OUT.body" "$OUT.hdr"

# H5: every measurement claim in docs/ names the exact request. Append one row
# per probe so the evidence exists before the claim does.
LOG="$(dirname "$0")/../docs/probe-log.tsv"
if [ ! -f "$LOG" ]; then
  printf 'utc	engine	query	http	bytes	verdict	ua	headers	url\n' > "$LOG"
fi
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$ENGINE" "$QUERY" "$CODE" "$SIZE" \
  "$VERDICT" "$([ "$HDRMODE" = browser ] && echo chrome || echo safari)" \
  "$HDRMODE" "$URL" >> "$LOG"
