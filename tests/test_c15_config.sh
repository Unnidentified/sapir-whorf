#!/usr/bin/env bash
# C15: a config file must never lie about itself.
set -u
BIN=${BIN:-./bin/omnisearch}
fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }

echo "C15: config validation"

$BIN --config config.ini --check-config >/tmp/c15good.txt 2>&1
[ $? -eq 0 ] && ok "shipped config validates" || bad "shipped config does not validate"

cat > /tmp/c15bad.ini <<'INI'
[server]
port = 8087
porrt = 9999
[health]
threshold = 3
cooldown_seconds = abc
[ranking]
authority_file = static/authority.tsv
INI

$BIN --config /tmp/c15bad.ini --check-config >/tmp/c15out.txt 2>&1
rc=$?
[ $rc -ne 0 ] && ok "invalid config exits non-zero ($rc)" || bad "invalid config exited 0"
for want in "porrt" "threshold" "not an integer" "ranking"; do
  if grep -q "$want" /tmp/c15out.txt; then ok "reported: $want"
  else bad "did not report: $want"; fi
done

grep -q "NOT in effect" /tmp/c15out.txt && ok "summary line present" || bad "no summary line"

[ $fail -eq 0 ] && echo "C15: PASS" || echo "C15: FAIL"
exit $fail
