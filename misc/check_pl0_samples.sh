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
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}
export CULEBRA ENGINE

one() {
  local f=$1
  local stdin=${f%.pas}.stdin
  [ -e "$stdin" ] || stdin=/dev/null
  local got want
  got=$("$CULEBRA" "$ENGINE" examples/languages/pl0/pl0_codegen.cul "$f" \
        <"$stdin" 2>&1)
  want=$("$CULEBRA" "$ENGINE" examples/languages/pl0/pl0.cul "$f" \
         <"$stdin" 2>&1)
  if [ "$got" = "$want" ]; then
    echo "OK   $(basename "$f")"
  else
    echo "DIFF $(basename "$f")"
    diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -20
    return 1
  fi
}
export -f one

# xargs answers 123 when any child failed; normalize it to a plain 1.
printf '%s\n' examples/languages/pl0/samples/*.pas |
  xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
[ $? -eq 0 ] || exit 1
