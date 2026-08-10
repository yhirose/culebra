#!/usr/bin/env bash
# Diff the VM lanes against the interpreter on every case file.
# Usage: [STRESS=1] compare.sh <culebra-binary> [lane-flag]
# With no lane flag, both VM lanes (--vm, --vm-llvm) are asserted in one
# run — the three-lane agreement the docs claim. STRESS=1 runs the VM lanes
# under CULEBRA_GC_STRESS=1 (allocations forced to collect); the interp
# reference run stays unstressed either way.
set -u
BIN="${1:-./build-dev/culebra}"
BIN="$(realpath "$BIN" 2>/dev/null || echo "$BIN")"
if [ ! -x "$BIN" ]; then
  echo "compare.sh: binary not found or not executable: $BIN" >&2
  exit 1
fi
if [ $# -ge 2 ]; then LANES=("$2"); else LANES=(--vm --vm-llvm); fi
STRESS="${STRESS:-}"
tag="${STRESS:+ (GC_STRESS)}"
cd "$(dirname "$0")"
fail=0
for f in *.cul; do
  a="$("$BIN" "$f" 2>&1)"; a_rc=$?
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
