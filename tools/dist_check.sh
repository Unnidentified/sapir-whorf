#!/bin/sh
# A credential or a log in the release archive is a leak that travels with it,
# and a duplicated source file breaks the build the moment it is unpacked (the
# Makefile globs src/**.c, so "Foo 1.c" is compiled as a second definition).
#
# X2: this used to crash with "$1: unbound variable" when run with no argument.
set -u
ARCHIVE="${1:-}"
if [ -z "$ARCHIVE" ]; then
    # no argument: check the tarball make dist most recently built
    ARCHIVE=$(ls -t /tmp/omnisearch-*.tar.gz 2>/dev/null | head -1)
    if [ -z "$ARCHIVE" ]; then
        echo "dist: usage: $0 <archive.tar.gz>  (or run make dist first)"
        exit 1
    fi
    echo "dist: checking $ARCHIVE"
fi
[ -f "$ARCHIVE" ] || { echo "dist: no archive at $ARCHIVE"; exit 1; }

fail=0

# 1. secrets and logs
BAD=$(tar tzf "$ARCHIVE" | grep -E 'secrets\.ini|\.err$|\.err\.old$|\.log$')
if [ -n "$BAD" ]; then
    echo "dist: FAIL - the archive contains a credential or a log:"
    printf '%s\n' "$BAD"
    fail=1
fi

# 2. macOS Finder duplicates ("Foo 1.c", "Bar 2.html"). These came back as 489
#    byte-identical copies in a shared zip once; git archive cannot make them,
#    but a folder zip can, and the check belongs wherever an archive is examined.
DUPES=$(tar tzf "$ARCHIVE" | grep -E ' [0-9]+\.[A-Za-z]+$| [0-9]+$')
if [ -n "$DUPES" ]; then
    echo "dist: FAIL - duplicated files (Finder copies) would not build:"
    printf '%s\n' "$DUPES" | head -10
    fail=1
fi

# 3. build output and caches
JUNK=$(tar tzf "$ARCHIVE" | grep -E '(^|/)(obj|bin|cache)/|\.o$|\.d$|\.DS_Store$')
if [ -n "$JUNK" ]; then
    echo "dist: FAIL - build output or junk in the archive:"
    printf '%s\n' "$JUNK" | head -10
    fail=1
fi

[ "$fail" -eq 0 ] && echo "dist: clean - no credentials, no logs, no duplicates, no build output"
exit "$fail"
