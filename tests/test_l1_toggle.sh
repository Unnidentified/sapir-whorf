#!/bin/sh
# L1: the box means "on", and the wire stays compact.
#
# Three things are asserted here, none of which needs a browser:
#   1. every row's box agrees with its state word, both directions;
#   2. the no-script link flips one provider and nothing else;
#   3. twenty providers switched off in one submission survive - the case the
#      single off= field exists for, because beaker keeps 16 parameters and
#      stops parsing silently past that.
set -u
cd "$(dirname "$0")/.." || exit 1
PORT=8087
JAR=/tmp/l1jar.txt
fail=0; pass=0
say_fail() { printf 'FAIL %s\n' "$1"; fail=$((fail + 1)); }
say_pass() { pass=$((pass + 1)); }

if ! curl -sS -m 4 -o /dev/null "http://127.0.0.1:$PORT/settings" 2>/dev/null; then
    echo "L1: SKIP -- no server on $PORT (start it, or run this after make run)"
    exit 0
fi

rm -f "$JAR"
get() { curl -sS -o /tmp/l1page.html -m 20 -b "$JAR" -c "$JAR" \
        "http://127.0.0.1:$PORT/settings?cb=$RANDOM" 2>/dev/null; }

# 1. box vs label, both directions, for every row
get
python3 - <<'PY' > /tmp/l1agree.txt || true
import re, sys
h = open('/tmp/l1page.html').read()
blocks = re.findall(r'<div class="settings-provider kind-([a-z_]+)">(.*?)(?=<div class="settings-provider |</div>\s*</div>\s*</section>)', h, re.S)
bad = []
seen = 0
for kind, body in blocks:
    m = re.search(r'class="settings-checkbox"([^>]*)>', body)
    if not m:
        continue
    attrs = m.group(1)
    checked = 'checked' in attrs
    disabled = 'disabled' in attrs
    name = re.search(r'settings-provider-name">([^<]*)', body)
    state = re.search(r'settings-provider-state">([^<]*)', body)
    label = (state.group(1).strip() if state else '')
    ident = (name.group(1).strip() if name else '?')
    seen += 1
    if label.startswith('Unavailable'):
        if checked or not disabled:
            bad.append(f'{ident}: unavailable but checked={checked} disabled={disabled}')
    elif label == 'On' and not checked:
        bad.append(f'{ident}: says On, box empty')
    elif label.startswith('Off') and checked:
        bad.append(f'{ident}: says Off, box ticked')
print(f'rows={seen}')
for b in bad:
    print('BAD ' + b)
PY
rows=$(grep -o 'rows=[0-9]*' /tmp/l1agree.txt | cut -d= -f2)
bad=$(grep -c '^BAD ' /tmp/l1agree.txt || true)
[ "${rows:-0}" -gt 20 ] && say_pass || say_fail "only ${rows:-0} rows found on the page"
[ "${bad:-1}" -eq 0 ] && say_pass || { say_fail "$bad row(s) disagree:"; grep '^BAD ' /tmp/l1agree.txt | head -4; }

# 2. the no-script link flips exactly one provider
before=$(grep -c 'kind-off' /tmp/l1page.html || true)
curl -sS -o /dev/null -m 20 -b "$JAR" -c "$JAR" \
     "http://127.0.0.1:$PORT/save_settings?toggle=bing" 2>/dev/null
get
after=$(grep -c 'kind-off' /tmp/l1page.html || true)
[ "$after" -eq "$((before + 1))" ] && say_pass \
    || say_fail "toggle changed $before -> $after off rows (wanted +1)"
python3 - <<'PY' >> /tmp/l1agree.txt
import re
h = open('/tmp/l1page.html').read()
m = re.search(r'id="prov_bing"(.*?)</label>', h, re.S)
body = m.group(1) if m else ''
print('bing_label=' + (re.search(r'settings-provider-state">([^<]*)', body).group(1).strip()
                       if re.search(r'settings-provider-state">([^<]*)', body) else '?'))
print('bing_checked=' + str('checked' in body))
PY
grep -q 'bing_checked=False' /tmp/l1agree.txt && say_pass \
    || say_fail "bing stayed ticked after the toggle"
grep -q 'bing_label=Off' /tmp/l1agree.txt && say_pass \
    || say_fail "bing's label is not Off after the toggle"
# and back
curl -sS -o /dev/null -m 20 -b "$JAR" -c "$JAR" \
     "http://127.0.0.1:$PORT/save_settings?toggle=bing" 2>/dev/null
get
grep -q 'kind-off' /tmp/l1page.html && n=$(grep -c 'kind-off' /tmp/l1page.html) || n=0
[ "$n" -eq "$before" ] && say_pass || say_fail "toggling back left $n off rows, wanted $before"

# 3. twenty providers off in one submission. The cookie is the only thing that
#    has to carry them, and the off= field is one parameter.
ids=$(python3 -c "
import re
h=open('/tmp/l1page.html').read()
rows=re.findall(r'data-provider=\"([a-z0-9-]+)\"', h)
print('|'.join(rows[:20]))
")
count=$(printf '%s' "$ids" | tr '|' '\n' | grep -c .)
curl -sS -o /dev/null -m 20 -b "$JAR" -c "$JAR" \
     "http://127.0.0.1:$PORT/save_settings?engines_seen=40&off=$ids" 2>/dev/null
value=$(grep -o 'off=[^	]*' "$JAR" | tail -1 | cut -d'	' -f2)
kept=$(printf '%s' "$value" | tr '|' '\n' | grep -c .)
[ "$kept" -ge "$count" ] && say_pass \
    || say_fail "$count providers submitted, $kept in the cookie"
get
still_on=0
for id in $(printf '%s' "$ids" | tr '|' ' '); do
    python3 - "$id" <<'PY' || still_on=$((still_on + 1))
import re, sys
h = open('/tmp/l1page.html').read()
m = re.search(r'id="prov_%s"(.*?)</label>' % sys.argv[1], h, re.S)
body = m.group(1) if m else ''
if 'checked' in body:
    sys.exit(1)
PY
done
[ "$still_on" -eq 0 ] && say_pass || say_fail "$still_on of $count came back ticked"
# put the browser's policy back
curl -sS -o /dev/null -m 20 -b "$JAR" -c "$JAR" \
     "http://127.0.0.1:$PORT/save_settings?engines_seen=40&off=" 2>/dev/null

echo "L1 toggle: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
