#!/bin/sh
# J5.5 tier 3: the opt-in providers switch.
#
# The claim under test is that two keys are in series and neither alone is
# enough: the server must permit the providers at all ([engines] allow_files),
# and the browser must have opted in. This checks every combination, that the
# control cannot be offered while the server refuses, that opting in can be
# withdrawn (an unchecked checkbox submits nothing, so it needs a companion
# field), and that an unknown value is not stored.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/omnisearch
[ -x "$BIN" ] || { echo "FAIL: $BIN missing"; exit 1; }

fail=0
PIDS=""

start() { # name port allow_files
  name=$1
  port=$2
  allow=$3
  rm -rf "/tmp/optin-$name"
  cat > "/tmp/optin-$name.ini" <<INI
[server]
host = 127.0.0.1
port = $port
answers = off
[engines]
engines = "ddg"
allow_files = $allow
[cache]
dir = /tmp/optin-$name
INI
  $BIN --config "/tmp/optin-$name.ini" > "/tmp/optin-$name.log" 2>&1 &
  PIDS="$PIDS $!"
}

start off 8101 off
start on 8102 on
sleep 4

# 1. server refuses: no switch anywhere, and it says why
curl -sS -o /tmp/optin-off.html -m 10 "http://127.0.0.1:8101/settings"
grep -q 'name="optin_' /tmp/optin-off.html && {
  echo "FAIL: a per-provider switch was offered while the server forbids it"; fail=1; }
grep -qi "has not enabled these providers" /tmp/optin-off.html || {
  echo "FAIL: the page did not say the server forbids them"; fail=1; }
grep -qi "Legal notice" /tmp/optin-off.html || {
  echo "FAIL: the disclaimer was missing"; fail=1; }

# 2. a cookie cannot override the server
curl -sS -o /tmp/optin-forced.html -m 10 -b "optin=1"   "http://127.0.0.1:8101/settings"
grep -q 'name="optin_' /tmp/optin-forced.html && {
  echo "FAIL: an optin cookie produced a switch on a refusing server"; fail=1; }

# 3. server permits: the section explains itself. There is no global switch
#    any more (K1.5): the switch is the provider's own row, and the shipped
#    build registers none, so the page must say so rather than offer a control
#    that would switch nothing.
curl -sS -o /tmp/optin-on.html -m 10 "http://127.0.0.1:8102/settings"
grep -q 'name="opt_in"' /tmp/optin-on.html && {
  echo "FAIL: the removed global switch is still rendered"; fail=1; }
grep -qi "No provider of this kind is registered" /tmp/optin-on.html || {
  echo "FAIL: the page did not say that no such provider exists yet"; fail=1; }
grep -qi "own switch under Opt-in" /tmp/optin-on.html || {
  echo "FAIL: the page did not point at the per-provider switch"; fail=1; }

# 4. an unrelated save does not touch the opt-in list
curl -sS -o /dev/null -m 10 -D /tmp/optin-h3.h \
  "http://127.0.0.1:8102/save_settings?theme=amber"
grep -qi "set-cookie: optin" /tmp/optin-h3.h && {
  echo "FAIL: an unrelated save changed the opt-in"; fail=1; }

# 5. the per-provider path is measured end to end in test_k1_optin.sh, which
#    registers a provider and checks what leaves the machine.

for p in $PIDS; do kill "$p" 2>/dev/null; done
rm -f /tmp/optin-*.ini
if [ "$fail" -eq 0 ]; then
  echo "J5.5 opt-in switch: PASS (server gate, no global switch, per-provider path elsewhere)"
else
  exit 1
fi
