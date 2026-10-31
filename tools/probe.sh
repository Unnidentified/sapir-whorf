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
  ddg-lite) URL="https://lite.duckduckgo.com/lite/?q=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  ddg-html) URL="https://html.duckduckgo.com/html/?q=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  brave)    URL="https://search.brave.com/search?q=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  bing)     URL="https://www.bing.com/search?q=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  yahoo)    URL="https://search.yahoo.com/search?p=$(python3 -c "import urllib.parse,sys;print(urllib.parse.quote(sys.argv[1]))" "$QUERY")" ;;
  *) echo "unknown engine: $ENGINE" >&2; exit 2 ;;
esac

OUT=/tmp/probe-${ENGINE}-${HTTPV}-${HDRMODE}-${COOKIEMODE}
JAR="$OUT.cookies"
rm -f "$OUT.body" "$OUT.hdr" "$JAR"

ARGS=(-sS --max-time 20 -o "$OUT.body" -D "$OUT.hdr" -w "%{http_code}")
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
    ddg-lite|ddg-html)
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
  ddg-lite|ddg-html) contains "result-link\|result__a" && VERDICT="results" || { contains "anomaly\|challenge" && VERDICT="challenge"; }; ;;
  brave)    contains "snippet" && VERDICT="results(html)" || { contains "challengeSet\|challenge" && VERDICT="challenge"; }; ;;
  yahoo)    contains "compTitle" && VERDICT="results" || { contains "captcha\|challenge" && VERDICT="challenge"; }; ;;
esac
contains "captcha\|unusual traffic\|are you a robot\|turnstile" && VERDICT="$VERDICT +captcha-marker"

printf '%-10s %-7s %-7s %-9s http=%-4s bytes=%-7s %s\n' \
  "$ENGINE" "$HTTPV" "$HDRMODE" "$COOKIEMODE" "$CODE" "$SIZE" "$VERDICT"
printf '  body=%s hdr=%s\n' "$OUT.body" "$OUT.hdr"
