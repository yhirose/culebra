#!/usr/bin/env bash
# Run every mini-js sample through the front end and through `node`, and
# report the ones that disagree. The samples are the front end's oracle:
# each is plain JavaScript, so `node` says what it has to print.
set -u
cd "$(dirname "$0")/.."
CULEBRA=${CULEBRA:-./build-dev/culebra}
# Which engine to run the front end on.
ENGINE=${ENGINE:---vm}
LIB=${LIB:-examples/languages/mini-js/fmt.js}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}
export CULEBRA ENGINE LIB

one() {
  local f=$1
  local got want
  got=$("$CULEBRA" "$ENGINE" examples/languages/mini-js/mini_js.cul \
        --lib "$LIB" "$f" 2>&1)
  want=$(node -e "$(cat "$LIB"; cat "$f")" 2>&1)
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
printf '%s\n' examples/languages/mini-js/samples/*.js |
  xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
[ $? -eq 0 ] || exit 1
