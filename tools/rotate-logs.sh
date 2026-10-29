#!/usr/bin/env bash
# Keep the launchd log files bounded.
#
# The service is started with StandardOutPath/StandardErrorPath, so the
# process holds an open descriptor: the files must be truncated in place,
# not moved, or the server keeps writing to the unlinked inode.
set -uo pipefail

DATA="${OMNISEARCH_DATA:-$HOME/.omnisearch}"
MAX_BYTES="${OMNISEARCH_LOG_MAX:-5242880}"   # 5 MB

for name in omnisearch.err omnisearch.log; do
  f="$DATA/$name"
  [ -f "$f" ] || continue

  size=$(stat -f%z "$f" 2>/dev/null || stat -c%s "$f" 2>/dev/null || echo 0)
  [ "$size" -gt "$MAX_BYTES" ] || continue

  cp -f "$f" "$f.old"
  : > "$f"
  printf 'rotated %s (%s bytes) at %s\n' "$name" "$size" "$(date -u +%FT%TZ)" >> "$DATA/omnisearch.log"
done
