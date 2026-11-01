#!/bin/sh
# Every template must have balanced {{if}}/{{endif}} and {{for}}/{{endfor}}.
#
# This is a test because the failure is silent in the other direction: beaker
# reports "Unclosed '{{if}}' tag. Skipping if block." to the server log and
# renders the rest of the page without it, so a page can lose a whole section
# while still answering 200. It also parses tags inside HTML comments, which is
# how a comment explaining a removed condition can unbalance a file.
set -u
cd "$(dirname "$0")/.." || exit 1
fail=0
for f in templates/*.html; do
    depth_if=$(python3 - "$f" <<'PY'
import re, sys
src = open(sys.argv[1], encoding='utf-8').read()
# strip nothing: beaker does not honour HTML comments here
stack = []
for m in re.finditer(r'{{(if|endif|for|endfor|else)\b', src):
    t = m.group(1)
    if t in ('if', 'for'):
        stack.append(t)
    elif t == 'else':
        if not stack or stack[-1] != 'if':
            print('else-without-if')
            break
    else:
        want = 'if' if t == 'endif' else 'for'
        if not stack or stack[-1] != want:
            print('mismatch-' + t)
            break
        stack.pop()
else:
    if stack:
        print('unclosed-' + ','.join(stack))
PY
)
    if [ -n "$depth_if" ]; then
        printf 'FAIL %s: %s\n' "$f" "$depth_if"
        fail=$((fail + 1))
    fi
done
if [ "$fail" -ne 0 ]; then
    exit 1
fi
printf 'templates balanced: %s files\n' "$(ls templates/*.html | wc -l | tr -d ' ')"
