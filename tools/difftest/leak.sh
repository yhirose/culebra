#!/usr/bin/env bash
# Leak-fuzz gate: reuse the difftest template-combinator corpus (each case is a
# re-runnable thunk) as an RC-leak oracle. Run every case N times under
# CULEBRA_GC_NEVER=1 (conservative backstop and cycle collector OFF, so an RC
# leak is NOT masked) on both compiled lanes and measure GC.stat().rc_objects
# growth per lane. Any case whose growth reaches the threshold on a lane is a
# leak on that lane.
#
# The judgment is a per-lane ABSOLUTE, not a diff against a leak-free second
# implementation: the tree-walker used to be that baseline, and B7-c measured
# what the subtraction was hiding — a shared-runtime leak grows both compiled
# lanes AND grew the interp by the same amount (the effects cases), so the
# diff read 0 and the gate was blind to it; and a lane-specific leak on the
# lane the gate didn't run (the executor) was invisible outright. Absolute
# growth sees all three classes. What the subtraction legitimately removed —
# corpus cases that accumulate on purpose — measured ZERO cases on the
# compiled lanes (accumulating templates push unboxed Longs, which
# rc_objects does not count), so nothing replaces it.
#
# This gate is a *regression* gate, not a zero-leak gate: the baseline below
# records the known leaks per lane. It fails only on a leak that is NOT in
# the baseline (a new regression). A baseline entry that stops leaking is
# reported so the file can shrink. The true-cycle section is the exception —
# a reference cycle leaks under GC_NEVER by definition (the cycle collector
# is its collector, and it is off here), so those entries are permanent;
# the goal is to drive the *non-cycle* section to empty.
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
# Growth (over 40 thunk() calls, see leak_preamble.cul) at or above this is a
# leak. Real leaks measure as clean multiples of 40 (>=40); non-leaks are 0.
# 20 sits in the empty gap with margin, robust to any warmup jitter.
THRESH="${LEAKFUZZ_THRESH:-20}"
mkdir -p "$WORK"

# Generate cases, then prepend the leak-measuring preamble (not difftest's).
if ! "$CULEBRA" --vm "$HERE/gen.cul" > "$WORK/cases.cul"; then
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
for cf in "${chunks[@]}"; do cat "$HERE/leak_preamble.cul" "$HERE/canvas_fixtures.cul" "$cf" > "$cf.cul"; done

JOBS="${LEAKFUZZ_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
run_one() {
  local cf="$1" backend="$2"
  if [ "$backend" = v ]; then CULEBRA_GC_NEVER=1 "$CULEBRA" --vm "$cf.cul" > "$cf.v" 2>&1
  else                        CULEBRA_GC_NEVER=1 "$CULEBRA" --jit "$cf.cul" > "$cf.j" 2>&1; fi
}
export -f run_one; export CULEBRA
for cf in "${chunks[@]}"; do printf '%s\tv\n%s\tj\n' "$cf" "$cf"; done \
  | xargs -P "$JOBS" -L1 bash -c 'run_one "$1" "$2"' _

: > "$WORK/out_vm.txt"; : > "$WORK/out_jit.txt"
fail=0
for cf in "${chunks[@]}"; do
  cc=$(grep -c '^_p(' "$cf")
  cv=$(grep -c '' "$cf.v"); cj=$(grep -c '' "$cf.j")
  if [ "$cv" -lt "$cc" ] || [ "$cj" -lt "$cc" ]; then
    echo "leakfuzz: FAIL — chunk $(basename "$cf") did not run to completion" >&2
    echo "  chunk_cases=$cc vm_lines=$cv jit_lines=$cj" >&2
    tail -3 "$cf.j" >&2; fail=1
  fi
  cat "$cf.v" >> "$WORK/out_vm.txt"; cat "$cf.j" >> "$WORK/out_jit.txt"
done
[ "$fail" = 0 ] || exit 1

# Per-lane absolute: every label whose growth reaches the threshold, keyed
# "<lane><TAB><label>" (labels contain '|', so TAB is the separator; the
# lane key is what lets a fixed report fire when only one lane heals).
{
  awk -F' ::: growth=' -v TH="$THRESH" \
      '$2 != "" && $2 + 0 >= TH { print "vm\t" $1 }' "$WORK/out_vm.txt"
  awk -F' ::: growth=' -v TH="$THRESH" \
      '$2 != "" && $2 + 0 >= TH { print "jit\t" $1 }' "$WORK/out_jit.txt"
} | LC_ALL=C sort -u > "$WORK/current_leaks.txt"
now=$(grep -c '' "$WORK/current_leaks.txt")

if [ "$UPDATE" = 1 ]; then
  # Surgical update: keep the file's own order (the section notes and the
  # entries filed under them), drop entries that no longer leak, and append
  # genuinely-new ones at the end for manual filing into a section — so the
  # curated true-cycle / RC-leak split survives its own tooling.
  if [ -f "$BASELINE" ]; then
    awk -v cur="$WORK/current_leaks.txt" '
      BEGIN { while ((getline l < cur) > 0) keep[l] = 1 }
      /^[[:space:]]*(#|$)/ { print; next }
      ($0 in keep) { print }' "$BASELINE" > "$BASELINE.tmp"
    grep -v '^[[:space:]]*#' "$BASELINE" | grep -v '^[[:space:]]*$' \
      | LC_ALL=C sort -u > "$WORK/old_entries.txt"
    new=$(LC_ALL=C comm -23 "$WORK/current_leaks.txt" "$WORK/old_entries.txt")
    if [ -n "$new" ]; then
      { echo "#"
        echo "# --- new since the last update: file each into a section ---"
        printf '%s\n' "$new"; } >> "$BASELINE.tmp"
    fi
    mv "$BASELINE.tmp" "$BASELINE"
  else
    cp "$WORK/current_leaks.txt" "$BASELINE"
  fi
  echo "leakfuzz: baseline updated — $now known leak lane-entries recorded"
  exit 0
fi

[ -f "$BASELINE" ] || { echo "leakfuzz: FAIL — no baseline ($BASELINE). Run --update-baseline." >&2; exit 1; }
grep -v '^[[:space:]]*#' "$BASELINE" | grep -v '^[[:space:]]*$' \
  | LC_ALL=C sort -u > "$WORK/baseline_sorted.txt"
base=$(grep -c '' "$WORK/baseline_sorted.txt")
new_leaks=$(LC_ALL=C comm -23 "$WORK/current_leaks.txt" "$WORK/baseline_sorted.txt")
fixed=$(LC_ALL=C comm -13 "$WORK/current_leaks.txt" "$WORK/baseline_sorted.txt")

echo "leakfuzz: $cases cases, $now leak lane-entries (baseline $base)"
if [ -n "$fixed" ]; then
  echo
  echo "leakfuzz: $(printf '%s\n' "$fixed" | grep -c '') baseline entr(ies) NO LONGER leak — shrink the baseline:"
  printf '  fixed: %s\n' "$fixed"
  echo "  → run: tools/difftest/leak.sh <bin> --update-baseline"
fi
if [ -n "$new_leaks" ]; then
  echo
  echo "leakfuzz: FAIL — $(printf '%s\n' "$new_leaks" | grep -c '') NEW RC leak lane-entr(ies) not in baseline:" >&2
  printf '  NEW LEAK: %s\n' "$new_leaks" >&2
  exit 1
fi
echo "leakfuzz: no new leaks ✓"
exit 0
