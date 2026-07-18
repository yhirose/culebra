#!/usr/bin/env bash
# Leak-fuzz gate: reuse the difftest template-combinator corpus (each case is a
# re-runnable thunk) as an RC-leak oracle. Run every case N times under
# CULEBRA_GC_NEVER=1 (conservative backstop OFF, so an RC leak is NOT masked)
# and measure GC.stat().live_objects growth. A case whose JIT growth exceeds
# its interpreter growth (interp is leak-free by shared_ptr) by the threshold
# is a JIT-specific RC leak.
#
# This gate is a *regression* gate, not a zero-leak gate: the JIT still has a
# known set of carve-out leaks (tools/difftest/leak_baseline.txt). The gate
# fails only on a leak that is NOT in the baseline (a new regression). A
# baseline leak that stops leaking is reported so the baseline can shrink as
# the structural ownership work closes each class. Goal: drive the baseline to
# empty, then retire the backstop.
#
# Usage:
#   tools/difftest/leak.sh [culebra-binary]          # gate (fail on new leak)
#   tools/difftest/leak.sh [culebra-binary] --update-baseline
set -uo pipefail

# Default to the no-LTO dev build: the audit/growth oracles under-report on
# an LTO binary (stale-stack aliasing), so a stale ./build/ default has caused
# false-green runs twice. Pass build-gate/culebra explicitly from the gate.
CULEBRA="${1:-./build-dev/culebra}"
[ -x "$CULEBRA" ] || { echo "error: $CULEBRA not found/executable — pass a no-LTO culebra binary" >&2; exit 1; }
[ "${2:-}" = --update-baseline ] && UPDATE=1 || UPDATE=0
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${LEAKFUZZ_WORK:-build/leakfuzz}"
BASELINE="$HERE/leak_baseline.txt"
# JIT-specific growth (over 40 thunk() calls, see leak_preamble.cul) at or above
# this is a leak. Real leaks measure as clean multiples of 40 (>=40); non-leaks
# are 0. 20 sits in the empty gap with margin, robust to any warmup jitter.
THRESH="${LEAKFUZZ_THRESH:-20}"
mkdir -p "$WORK"

# Generate cases, then prepend the leak-measuring preamble (not difftest's).
if ! "$CULEBRA" "$HERE/gen.cul" > "$WORK/cases.cul"; then
  echo "leakfuzz: FAIL — generator gen.cul did not run cleanly" >&2; exit 1
fi
cases=$(grep -c '^_p(' "$WORK/cases.cul")
if [ "$cases" -lt 1000 ]; then
  echo "leakfuzz: FAIL — only $cases cases generated (expected >= 1000)" >&2; exit 1
fi

# Chunk for JIT compile time + parallelism (identical rationale to run.sh).
CHUNK="${LEAKFUZZ_CHUNK:-400}"
chunkdir="$WORK/chunks"; rm -rf "$chunkdir"; mkdir -p "$chunkdir"
split -l "$CHUNK" "$WORK/cases.cul" "$chunkdir/c."
chunks=( "$chunkdir"/c.* )
for cf in "${chunks[@]}"; do cat "$HERE/leak_preamble.cul" "$cf" > "$cf.cul"; done

JOBS="${LEAKFUZZ_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
run_one() {
  local cf="$1" backend="$2"
  if [ "$backend" = i ]; then CULEBRA_GC_NEVER=1 "$CULEBRA"       "$cf.cul" > "$cf.i" 2>&1
  else                        CULEBRA_GC_NEVER=1 "$CULEBRA" --jit "$cf.cul" > "$cf.j" 2>&1; fi
}
export -f run_one; export CULEBRA
for cf in "${chunks[@]}"; do printf '%s\ti\n%s\tj\n' "$cf" "$cf"; done \
  | xargs -P "$JOBS" -L1 bash -c 'run_one "$1" "$2"' _

: > "$WORK/out_interp.txt"; : > "$WORK/out_jit.txt"
fail=0
for cf in "${chunks[@]}"; do
  cc=$(grep -c '^_p(' "$cf")
  ci=$(grep -c '' "$cf.i"); cj=$(grep -c '' "$cf.j")
  if [ "$ci" -lt "$cc" ] || [ "$cj" -lt "$cc" ]; then
    echo "leakfuzz: FAIL — chunk $(basename "$cf") did not run to completion" >&2
    echo "  chunk_cases=$cc interp_lines=$ci jit_lines=$cj" >&2
    tail -3 "$cf.j" >&2; fail=1
  fi
  cat "$cf.i" >> "$WORK/out_interp.txt"; cat "$cf.j" >> "$WORK/out_jit.txt"
done
[ "$fail" = 0 ] || exit 1

# JIT-specific leaks: labels where (jit_growth - max(0, interp_growth)) >= THRESH.
# interp is the leak-free baseline, so this subtracts legitimately-accumulating
# cases (globals) and pure reference cycles (which leak equally on both). Clamp
# the interp reference at 0: a *negative* interp growth means interp rc_objects
# settled during the window (a warmup transient not fully absorbed, or churn from
# neighbouring cases), and subtracting it would manufacture a phantom JIT leak
# out of a JIT growth that is actually 0.
awk -F' ::: growth=' -v TH="$THRESH" '
  FNR==NR { if ($2 != "") ig[$1] = $2 + 0; next }
  $2 != "" { ref = ($1 in ig) ? ig[$1] : 0; if (ref < 0) ref = 0; d = ($2 + 0) - ref; if (d >= TH) print $1 }
' "$WORK/out_interp.txt" "$WORK/out_jit.txt" | LC_ALL=C sort -u > "$WORK/current_leaks.txt"
now=$(grep -c '' "$WORK/current_leaks.txt")

if [ "$UPDATE" = 1 ]; then
  cp "$WORK/current_leaks.txt" "$BASELINE"
  echo "leakfuzz: baseline updated — $now known JIT-specific leaks recorded"
  exit 0
fi

[ -f "$BASELINE" ] || { echo "leakfuzz: FAIL — no baseline ($BASELINE). Run --update-baseline." >&2; exit 1; }
LC_ALL=C sort -u "$BASELINE" > "$WORK/baseline_sorted.txt"
base=$(grep -c '' "$WORK/baseline_sorted.txt")
new_leaks=$(LC_ALL=C comm -23 "$WORK/current_leaks.txt" "$WORK/baseline_sorted.txt")
fixed=$(LC_ALL=C comm -13 "$WORK/current_leaks.txt" "$WORK/baseline_sorted.txt")

echo "leakfuzz: $cases cases, $now JIT-specific leaks (baseline $base)"
if [ -n "$fixed" ]; then
  echo
  echo "leakfuzz: $(printf '%s\n' "$fixed" | grep -c '') baseline leak(s) NO LONGER leak — shrink the baseline:"
  printf '  fixed: %s\n' "$fixed"
  echo "  → run: tools/difftest/leak.sh <bin> --update-baseline"
fi
if [ -n "$new_leaks" ]; then
  echo
  echo "leakfuzz: FAIL — $(printf '%s\n' "$new_leaks" | grep -c '') NEW JIT RC leak(s) not in baseline:" >&2
  printf '  NEW LEAK: %s\n' "$new_leaks" >&2
  exit 1
fi
echo "leakfuzz: no new leaks ✓"
exit 0
