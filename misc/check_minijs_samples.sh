#!/usr/bin/env bash
# Run every mini-js sample through the front end and through `node`, and
# report the ones that disagree. The samples are the front end's oracle:
# each is plain JavaScript, so `node` says what it has to print.
set -u
cd "$(dirname "$0")/.."
CULEBRA=${CULEBRA:-./build-dev/culebra}
# Which engine to run the front end -- and its culebra-side oracle -- on.
ENGINE=${ENGINE:---vm}
LIB=${LIB:-examples/languages/mini-js/fmt.js}
fail=0
for f in examples/languages/mini-js/samples/*.js; do
  got=$("$CULEBRA" "$ENGINE" examples/languages/mini-js/mini_js.cul --lib "$LIB" "$f" 2>&1)
  want=$(node -e "$(cat "$LIB"; cat "$f")" 2>&1)
  if [ "$got" = "$want" ]; then
    echo "OK   $(basename "$f")"
  else
    echo "DIFF $(basename "$f")"
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -20
    fail=1
  fi
done
exit $fail
