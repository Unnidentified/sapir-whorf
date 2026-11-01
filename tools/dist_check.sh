#!/bin/sh
# L6.4: a credential or a log in the release archive is a leak that travels
# with it. Run on the tarball `make dist` just built.
set -u
ARCHIVE="$1"
[ -f "$ARCHIVE" ] || { echo "dist: no archive at $ARCHIVE"; exit 1; }
BAD=$(tar tzf "$ARCHIVE" | grep -E 'secrets\.ini|\.err$|\.err\.old$|\.log$')
if [ -n "$BAD" ]; then
    echo "dist: FAIL - the archive contains a credential or a log:"
    printf '%s\n' "$BAD"
    exit 1
fi
echo "dist: no credentials and no logs in the archive"
