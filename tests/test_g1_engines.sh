#!/bin/sh
# G1 + L2: engines="*" means the registry defaults, and naming a disabled
# engine opts it in. Before this, "*" force-enabled every entry, so an engine
# shipped disabled (google, searxng) still ran on every query.
#
# L2 removed DuckDuckGo Lite: the id is now UNKNOWN, not merely disabled, so the
# test asserts that naming it changes nothing and does not stop the parse.
set -u
cd "$(dirname "$0")/.." || exit 1
BIN=./bin/omnisearch
[ -x "$BIN" ] || { echo "FAIL: $BIN missing"; exit 1; }

run_with() {
  { printf '[engines]\n'; printf '%s\n' "$1"; } > /tmp/g1-test-engines.ini
  timeout 5 $BIN --config /tmp/g1-test-engines.ini --check-config 2>&1 |
    sed -n 's/^  engines_enabled=//p'
}

fail=0
# 1. default "*": disabled entries stay disabled
out=$(run_with 'engines="*"')
for id in google searxng; do
  case ",$out," in
    *",$id,"*) echo "FAIL: engines=\"*\" enabled $id"; fail=1 ;;
  esac
done
case ",$out," in
  *",ddg,"*) : ;;
  *) echo "FAIL: engines=\"*\" left ddg disabled"; fail=1 ;;
esac

# 2. explicit opt-in still works
out=$(run_with 'engines="*,yacy"')
case ",$out," in
  *",yacy,"*) : ;;
  *) echo "FAIL: engines=\"*,yacy\" did not enable yacy"; fail=1 ;;
esac
# L2: ddg-lite was removed from the build. Naming it must be a no-op, not a
# parse failure and not a resurrected engine.
case ",$out," in
  *",ddg-lite,"*) echo "FAIL: engines=\"*,yacy\" resurrected ddg-lite"; fail=1 ;;
esac
out=$(run_with 'engines="*,-ddg-lite"')
case ",$out," in
  *",ddg,"*) : ;;
  *) echo "FAIL: engines=\"*,-ddg-lite\" broke the parse (ddg missing)"; fail=1 ;;
esac
case ",$out," in
  *",ddg-lite,"*) echo "FAIL: engines=\"*,-ddg-lite\" still lists ddg-lite"; fail=1 ;;
esac

# 3. an explicit list alone disables everything else
out=$(run_with 'engines="bing"')
case ",$out," in
  *",bing,"*) : ;;
  *) echo "FAIL: engines=\"bing\" did not enable bing"; fail=1 ;;
esac
case ",$out," in
  *",ddg,"*) echo "FAIL: engines=\"bing\" left ddg enabled"; fail=1 ;;
esac

rm -f /tmp/g1-test-engines.ini
if [ "$fail" -eq 0 ]; then
  echo "G1 engines config: PASS"
else
  exit 1
fi
