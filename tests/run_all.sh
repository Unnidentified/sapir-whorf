#!/usr/bin/env bash
# Runs the offline/loopback test suite. Needs a test instance on PORT (default
# 8091) configured with engines=searxng and searxng_url pointed at
# tests/fake_upstream.py, plus that fake upstream running on FAKE_PORT.
set -u
PORT=${PORT:-8091}
CACHE_DIR=${CACHE_DIR:-/tmp/omni-test/cache}
here="$(cd "$(dirname "$0")" && pwd)"
fail=0
for t in "$here"/test_*.sh; do
  echo "=== $(basename "$t") ==="
  PORT="$PORT" CACHE_DIR="$CACHE_DIR" "$t" || fail=1
  echo
done
[ $fail -eq 0 ] && echo "ALL TESTS PASSED" || echo "SOME TESTS FAILED"
exit $fail
