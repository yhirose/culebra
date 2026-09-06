#!/usr/bin/env bash
# Run every mini-culebra sample through the front end and through culebra
# itself, and report the ones that disagree. The samples are plain culebra,
# so the real implementation says what each has to print.
set -u
cd "$(dirname "$0")/.."
CULEBRA=${CULEBRA:-./build-dev/culebra}
# Which engine to run the front end -- and its culebra-side oracle -- on.
ENGINE=${ENGINE:---vm}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}
export CULEBRA ENGINE

one() {
  local f=$1
  local got want
  got=$("$CULEBRA" "$ENGINE" \
        examples/languages/mini-culebra/mini_culebra.cul "$f" 2>&1)
  want=$("$CULEBRA" "$ENGINE" "$f" 2>&1)
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
printf '%s\n' examples/languages/mini-culebra/samples/*.cul |
  xargs -P "$JOBS" -I{} bash -c 'one "$@"' _ {}
[ $? -eq 0 ] || exit 1
