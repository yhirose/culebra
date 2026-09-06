#!/usr/bin/env bash
# Run one of examples/languages' front ends over its samples, and compare each
# against the oracle for that language. The three differ only in what to run
# and what to believe, so they are one script with three arms:
#
#   pl0           the tree-walking interpreter in pl0.cul beside it, which is
#                 an independent second implementation of the same language
#   mini-js       `node`. The samples are plain JavaScript, and fmt.js is the
#                 one formatting function both sides print through, so the
#                 comparison is the language rather than console.log's own
#                 rendering
#   mini-culebra  culebra. The samples are plain culebra.
set -u
cd "$(dirname "$0")/.."
CULEBRA=${CULEBRA:-./build-dev/culebra}
# Which engine to run the front end -- and its culebra-side oracle -- on.
ENGINE=${ENGINE:---vm}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}
LIB=examples/languages/mini-js/fmt.js
export CULEBRA ENGINE LIB

case "${1:-}" in
  pl0)
    GLOB='examples/languages/pl0/samples/*.pas'
    got() {
      # PL/0 reads from stdin, so a sample may bring its own
      local in=${1%.pas}.stdin
      [ -e "$in" ] || in=/dev/null
      "$CULEBRA" "$ENGINE" examples/languages/pl0/pl0_codegen.cul "$1" <"$in"
    }
    want() {
      local in=${1%.pas}.stdin
      [ -e "$in" ] || in=/dev/null
      "$CULEBRA" "$ENGINE" examples/languages/pl0/pl0.cul "$1" <"$in"
    }
    ;;
  mini-js)
    GLOB='examples/languages/mini-js/samples/*.js'
    got() {
      "$CULEBRA" "$ENGINE" examples/languages/mini-js/mini_js.cul \
        --lib "$LIB" "$1"
    }
    want() { node -e "$(cat "$LIB"; cat "$1")"; }
    ;;
  mini-culebra)
    GLOB='examples/languages/mini-culebra/samples/*.cul'
    got() {
      "$CULEBRA" "$ENGINE" \
        examples/languages/mini-culebra/mini_culebra.cul "$1"
    }
    want() { "$CULEBRA" "$ENGINE" "$1"; }
    ;;
  *)
    echo "usage: $0 pl0|mini-js|mini-culebra" >&2
    exit 2
    ;;
esac
export GLOB
export -f got want

one() {
  local f=$1 g w
  g=$(got "$f" 2>&1)
  w=$(want "$f" 2>&1)
  if [ "$g" = "$w" ]; then
    echo "OK   $(basename "$f")"
  else
    echo "DIFF $(basename "$f")"
    diff <(printf '%s\n' "$w") <(printf '%s\n' "$g") | head -20
    return 1
  fi
}
export -f one

# xargs answers 123 when any child failed; normalize it to a plain 1.
printf '%s\n' $GLOB | xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {} || exit 1
