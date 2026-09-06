#!/usr/bin/env bash
# Run every PL/0 sample through the compiler front end and through the
# tree-walking interpreter in the same file, and report the ones that
# disagree. The interpreter is the oracle: it is the independent second
# implementation of the same language.
set -u
cd "$(dirname "$0")/.."
CULEBRA=${CULEBRA:-./build-dev/culebra}
# Which engine to run the front end -- and its culebra-side oracle -- on.
ENGINE=${ENGINE:---vm}
fail=0
for f in examples/languages/pl0/samples/*.pas; do
  stdin=${f%.pas}.stdin
  [ -e "$stdin" ] || stdin=/dev/null
  got=$("$CULEBRA" "$ENGINE" examples/languages/pl0/pl0_codegen.cul "$f" <"$stdin" 2>&1)
  want=$("$CULEBRA" "$ENGINE" examples/languages/pl0/pl0.cul "$f" <"$stdin" 2>&1)
  if [ "$got" = "$want" ]; then
    echo "OK   $(basename "$f")"
  else
    echo "DIFF $(basename "$f")"
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -20
    fail=1
  fi
done
exit $fail
