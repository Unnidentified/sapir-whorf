#!/usr/bin/env bash
# C2: cache entries must be swept per class, and writes must be atomic.
#
# 1. create a stale-class file and a search-class file, both older than the
#    search TTL but inside the stale TTL
# 2. restart the server (which sweeps at boot with the search TTL)
# 3. the stale file must survive, the search file must be gone
set -u
PORT=${PORT:-8091}
CACHE_DIR=${CACHE_DIR:-/tmp/omni-test/cache}
ROOT=${ROOT:-/tmp/omni-test}

fail=0
ok()  { printf '  ok   %s\n' "$1"; }
bad() { printf '  FAIL %s\n' "$1"; fail=1; }

echo "C2: per-class cache sweeping"

# build two 2-hour-old entries: one stale-class (7d) one plain (1h)
sub="$CACHE_DIR/a"
mkdir -p "$sub"
stale_file="$sub/stale_c2probe.cache"
plain_file="$sub/c2probe.cache"
printf 'stale-body' > "$stale_file"
printf 'plain-body' > "$plain_file"
# 2 hours old: past the 1h search TTL, well inside the 7d stale/infobox/image TTLs
touch -t "$(date -v-2H +%Y%m%d%H%M)" "$stale_file" "$plain_file"
echo "  planted: stale=$(stat -f %Sm -t %H:%M "$stale_file") plain=$(stat -f %Sm -t %H:%M "$plain_file")"

# also plant an image-class entry and an infobox-class entry
img_file="$sub/img_c2probe.cache"; printf 'img-body' > "$img_file"
info_file="$sub/info_c2probe.cache"; printf 'info-body' > "$info_file"
touch -t "$(date -v-2H +%Y%m%d%H%M)" "$img_file" "$info_file"

# restart the server so boot cleanup runs
PID=$(lsof -nP -iTCP:$PORT -sTCP:LISTEN -t 2>/dev/null)
[ -n "$PID" ] && kill -9 "$PID"
sleep 1
cd "$ROOT" || exit 1
nohup env OMNI_PERF=1 ./bin/omnisearch > /tmp/omni-c2.log 2>&1 < /dev/null &
disown
sleep 3

[ -f "$stale_file" ] && ok "stale entry survived boot sweep" || bad "stale entry deleted by boot sweep"
[ ! -f "$plain_file" ] && ok "expired search entry swept" || bad "expired search entry not swept"
[ -f "$img_file" ] && ok "image entry survived boot sweep" || bad "image entry deleted by boot sweep"
[ -f "$info_file" ] && ok "infobox entry survived boot sweep" || bad "infobox entry deleted by boot sweep"

# no temp files left behind (atomic writes must rename, not linger)
tmp_count=$(find "$CACHE_DIR" -name "*.tmp.*" 2>/dev/null | wc -l | tr -d ' ')
[ "$tmp_count" = "0" ] && ok "no leftover .tmp files" || bad "$tmp_count leftover .tmp files"

# the sweeper thread should announce itself
grep -q "Cache sweeper running" /tmp/omni-c2.log && ok "runtime sweeper started" || bad "runtime sweeper did not start"

[ $fail -eq 0 ] && echo "C2: PASS" || echo "C2: FAIL"
exit $fail
