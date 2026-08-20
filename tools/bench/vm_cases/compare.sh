#!/usr/bin/env bash
# Diff the bytecode VM's two consumers against the interpreter on every case
# file: its executor (--vm) and its LLVM lowering (--jit).
# Usage: [STRESS=1] compare.sh <culebra-binary> [lane-flag]
# With no lane flag, both are asserted in one run — the three-lane agreement
# the docs claim. STRESS=1 runs the compiled lanes under CULEBRA_GC_STRESS=1
# (allocations forced to collect); the interp reference run stays unstressed
# either way.
set -u
BIN="${1:-./build-dev/culebra}"
BIN="$(realpath "$BIN" 2>/dev/null || echo "$BIN")"
if [ ! -x "$BIN" ]; then
  echo "compare.sh: binary not found or not executable: $BIN" >&2
  exit 1
fi
if [ $# -ge 2 ]; then LANES=("$2"); else LANES=(--vm --jit); fi
STRESS="${STRESS:-}"
tag="${STRESS:+ (GC_STRESS)}"
cd "$(dirname "$0")"
fail=0
for f in *.cul; do
  a="$("$BIN" --tree "$f" 2>&1)"; a_rc=$?
  # A case that fails to PARSE agrees on every lane — they all print the same
  # SyntaxError — so it passes while testing nothing. Two files raise one on
  # purpose (a getter with parameters, a duplicate trait method); both are
  # named error_*, which is the exemption.
  # An UNCAUGHT diagnostic starts its own line ("SyntaxError: … at L:C."); a
  # case that catches one and prints it (`err=SyntaxError|…`) is testing the
  # error, not failing to parse.
  if printf '%s\n' "$a" | grep -q '^SyntaxError: '; then
    case "$f" in
      error_*) ;;
      *) echo "FAIL $f does not parse (a case that cannot run is not a test)"
         printf '%s\n' "$a" | head -2 | sed 's/^/     /'
         fail=1; continue;;
    esac
  fi
  for lane in "${LANES[@]}"; do
    # The runtime checks CULEBRA_GC_STRESS for presence, not value, so only
    # set it when stressing.
    if [ -n "$STRESS" ]; then
      b="$(CULEBRA_GC_STRESS=1 "$BIN" "$lane" "$f" 2>&1)"; b_rc=$?
    else
      b="$("$BIN" "$lane" "$f" 2>&1)"; b_rc=$?
    fi
    if [[ "$a" == "$b" && "$a_rc" == "$b_rc" ]]; then
      echo "OK   $f $lane$tag"
    else
      echo "FAIL $f (interp rc=$a_rc, $lane$tag rc=$b_rc)"
      diff <(printf '%s\n' "$a") <(printf '%s\n' "$b") | sed 's/^/     /'
      fail=1
    fi
  done
done
exit $fail
