#!/bin/bash
# K11 (error path): a name is not a ticker, a failure is not an apology card,
# and the message appears once. The full card (name->symbol table, quote
# presentation, sparkline) is separate work.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8145
DIR=/tmp/k11
rm -rf "$DIR" && mkdir -p "$DIR"
cat > /tmp/k11.ini <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg
[cache]
dir = $DIR
INI
pkill -9 -f "k11.ini" 2>/dev/null
sleep 0.5
./bin/omnisearch --config /tmp/k11.ini > /tmp/k11.log 2>&1 &
sleep 3
BASE="http://127.0.0.1:$PORT"
FAIL=0
fail() { echo "FAIL: $1"; FAIL=1; }
body() { curl -sS -o /tmp/k11.html -m 40 "$BASE/search?q=$1&nc=$RANDOM"; }

# 1. a company name is a name until proven a ticker: no card at all, and the
#    provider's error text appears zero times
body "nvidia+stock"
grep -q 'widget widget-stock' /tmp/k11.html && fail "nvidia stock still renders a stock card"
n=$(grep -c "No data found" /tmp/k11.html)
[ "$n" = "0" ] || fail "nvidia stock printed the provider error $n times"

# 2. an explicit symbol that the provider does not know: one error, once
body "ZZZZ+stock"
grep -q 'widget widget-stock' /tmp/k11.html || fail "ZZZZ stock rendered no error card"
n=$(grep -c "No data found" /tmp/k11.html)
[ "$n" = "1" ] || fail "ZZZZ stock printed the error $n times, expected 1"

# 3. the disclaimer belongs to a quote, not to an error
grep -qi "investment advice" /tmp/k11.html && fail "the disclaimer printed on an error card"

# 4. the notation people use is accepted
for q in "NVDA+stock" "%24NVDA+stock" "brk-b+stock"; do
  body "$q"
  grep -q 'widget widget-stock' /tmp/k11.html || fail "$q rendered no card"
  grep -qi "investment advice" /tmp/k11.html || fail "$q showed a quote without the disclaimer"
done

pkill -9 -f "k11.ini" 2>/dev/null
[ "$FAIL" = "0" ] && echo "K11 stock errors: PASS" || { echo "K11 stock errors: FAIL"; exit 1; }
