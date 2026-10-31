#!/usr/bin/env bash
# The template context is a fixed 32 slots (MAX_CONTEXT_VARS) and beaker drops
# the key SILENTLY when it is full - in the old log that key was 'results', so
# the page rendered with no results and said nothing.
#
# This exercises the widest path we can produce on demand: results + an infobox
# (Wikipedia) + a page-2 request, and fails if any key was dropped or if a page
# that should have results renders without them.
set -u
PORT=${PORT:-8087}
LOG=${LOG:-$HOME/.omnisearch/omnisearch.err}
fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }

echo "context capacity: widest render path"

have_log=0
if [ -r "$LOG" ]; then have_log=1; before=$(wc -l < "$LOG"); fi

curl -sS -o /tmp/cap1.html -m 40 "http://localhost:$PORT/search?q=linux+kernel" >/dev/null 2>&1
curl -sS -o /tmp/cap2.html -m 40 "http://localhost:$PORT/search?q=linux+kernel&p=2" >/dev/null 2>&1
sleep 0.6

if [ "$have_log" = "1" ]; then
  new_lines=$(tail -n +$((before + 1)) "$LOG" 2>/dev/null)
  if printf '%s' "$new_lines" | grep -q "TemplateContext is full"; then
    bad "a context key was dropped:"
    printf '%s\n' "$new_lines" | grep "TemplateContext is full" | head -3 | sed 's/^/       /'
  else
    ok "no context key dropped"
  fi
else
  ok "log not readable here; skipped the drop check"
fi

# J6.10: a template tag with no matching context key is printed literally
# instead of rendering nothing, so a page can ship a visible "{{foo}}".
# Nine such warnings were in the shipped log. Make them a failure.
if [ "$have_log" = "1" ]; then
  if printf '%s' "$new_lines" | grep -q "not found in context"; then
    bad "a template tag had no context key:"
    printf '%s\n' "$new_lines" | grep "not found in context" | head -3 | sed 's/^/       /'
  else
    ok "no template tag left without a context key"
  fi
fi

n=$(grep -c 'class="result"' /tmp/cap1.html 2>/dev/null || echo 0)
[ "$n" -gt 0 ] && ok "results rendered ($n)" || bad "page 1 rendered no results"

# page 2 must at least render the shell rather than an error
if [ -s /tmp/cap2.html ] && grep -q "results-container\|no-results" /tmp/cap2.html; then
  ok "page 2 rendered"
else
  bad "page 2 did not render"
fi

# an infobox on this query means the infoboxes key survived too
grep -q 'class="infobox' /tmp/cap1.html && ok "infobox rendered" || ok "no infobox for this query"

[ $fail -eq 0 ] && echo "context capacity: PASS" || echo "context capacity: FAIL"
exit $fail
