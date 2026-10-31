#!/bin/bash
# The template context has 32 slots, and beaker drops the 33rd with a single
# WARN line in the server log - the page just renders without it. That has
# silently removed an answer strip, a widget card and the empty-state hint at
# different points in this project, so the budget is now a test rather than
# something noticed later.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8125
DIR=/tmp/ctxbudget
LOG=/tmp/ctxbudget.log
rm -rf "$DIR" && mkdir -p "$DIR"
cat > /tmp/ctxbudget.ini <<INI
[server]
port = $PORT
host = 127.0.0.1
[engines]
engines = ddg
definitions_dir = static/engines.d
[cache]
dir = $DIR
INI
pkill -9 -f "ctxbudget.ini" 2>/dev/null
sleep 0.5
./bin/omnisearch --config /tmp/ctxbudget.ini > "$LOG" 2>&1 &
sleep 3

# every page that populates a lot of keys, including the widget kinds and the
# branches that only run when there are no results
for url in \
  "/search?q=2%2B2" \
  "/search?q=translate+hello+world+to+spanish" \
  "/search?q=zzqqxx+plkjhg" \
  "/search?q=linux+kernel&cat=academic&web=1" \
  "/search" \
  "/settings" \
  "/images?q=cat" \
  "/history" ; do
  curl -sS -o /dev/null -m 30 "http://127.0.0.1:$PORT$url"
done
sleep 1

pkill -9 -f "ctxbudget.ini" 2>/dev/null
if grep -q "TemplateContext is full" "$LOG"; then
  echo "FAIL: the template context overflowed:"
  grep -o "Cannot add key '[a-z_0-9]*'" "$LOG" | sort -u | head -8
  exit 1
fi
echo "J-template context budget: PASS (32 slots, nothing dropped)"
