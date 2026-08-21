#!/usr/bin/env bash
# Differential test: generate the template-combinator corpus, run it under
# every engine and diff the outputs byte-for-byte.
#
# The primary comparison is `--vm` against `--jit` — the executor and the LLVM
# lowering, the two consumers that outlive Phase 4. Both are handed the same
# bytecode by the same compiler, so this holds the backends to each other and
# says nothing about the compiler between them. That is what the third lane is
# for: while the tree-walker exists it is an independently written second
# implementation, and `--tree == --vm` is the check that answers for the
# compiler itself. When it goes, so does that check (docs/internals/vm.md §7).
#
# AOT is covered transitively — `just test` already asserts aot == jit.
#
# All three lanes are compared byte-for-byte. They were not always: the VM's
# supported slice used to be partial, and a case outside it answered VmError
# where the others answered, so that lane was walked record by record with a
# ceiling on the skips. The slice closed in Phase 2 and the ceiling reached 0,
# which makes a VmError here an ordinary divergence.
#
# Usage: tools/difftest/run.sh [culebra-binary]
#        (binary defaults to ./build/culebra)
set -uo pipefail

CULEBRA="${1:-./build/culebra}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$HERE/corpus.sh"
WORK="${DIFFTEST_WORK:-build/difftest}"

out_interp="$WORK/out_interp.txt"
out_jit="$WORK/out_jit.txt"
out_vm="$WORK/out_vm.txt"

# Build the corpus. Each chunk record is one line:
#   <label> ::: ok=<Type>:<repr>      | err=<kind>|<message>|<line>|<col>
cases=$(corpus_generate "$CULEBRA" "$WORK") || exit 1
corpus_chunk "$WORK" "${DIFFTEST_CHUNK:-400}"
chunks=( "${CORPUS_CHUNKS[@]}" )

# Run every (chunk × backend) as an independent parallel job — this is the bulk
# of the wall-clock (each chunk's JIT module compile dominates), and the chunks
# are fully independent, so it scales near-linearly with cores. Each job writes
# its own output file, so completion order is irrelevant; diff alignment is
# restored by concatenating in chunk order afterwards. Per-chunk error-record
# line numbers depend only on the (preamble + chunk) text, identical for both
# backends, so the byte diff stays exact regardless of CHUNK or scheduling.
JOBS="${DIFFTEST_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
# Streams are captured apart and then concatenated, never merged live: a
# third-party library that writes to stderr (ALSA, when the canvas fixtures
# touch audio on a machine with no sound card) interleaves at whatever byte
# the two buffers happen to meet, which splices its text mid-word into a
# record and makes two lanes differ over nothing. Appending stderr after
# stdout is the same rearrangement on every lane, so the byte diff stays
# exact and the error text is still compared.
run_one() {
  local cf="$1" backend="$2" flag="--tree"
  case "$backend" in
    j) flag="--jit" ;;
    v) flag="--vm" ;;
  esac
  "$CULEBRA" $flag "$cf.cul" > "$cf.$backend" 2> "$cf.$backend.err"
  cat "$cf.$backend.err" >> "$cf.$backend"
  rm -f "$cf.$backend.err"
}
export -f run_one; export CULEBRA
for cf in "${chunks[@]}"; do
  printf '%s\ti\n%s\tj\n%s\tv\n' "$cf" "$cf" "$cf"
done | xargs -P "$JOBS" -L1 bash -c 'run_one "$1" "$2"' _

# Completion guard (the invariant that makes this test non-vacuous): every
# `_p` call prints exactly one record line, and `inspect`/`print` cases only ADD
# lines, so a complete chunk yields at least `cc` lines under each backend.
# Fewer means it never ran to completion — a parse error or crash made both
# backends emit the same short error, which the byte diff would otherwise wave
# through as a lane agreeing with itself. Fail loudly instead. Concatenate in
# chunk order so the byte diff aligns case by case.
: > "$out_interp"; : > "$out_jit"; : > "$out_vm"
fail=0
for cf in "${chunks[@]}"; do
  cc=$(grep -c '^_p(' "$cf")
  ci=$(grep -c '' "$cf.i"); cj=$(grep -c '' "$cf.j"); cv=$(grep -c '' "$cf.v")
  if [ "$ci" -lt "$cc" ] || [ "$cj" -lt "$cc" ] || [ "$cv" -lt "$cc" ]; then
    echo "difftest: FAIL — chunk $(basename "$cf") did not run to completion" >&2
    echo "  chunk_cases=$cc  interp_lines=$ci  jit_lines=$cj  vm_lines=$cv" \
         "(expected >= cases)" >&2
    echo "  --- interp tail ---" >&2; tail -3 "$cf.i" >&2
    echo "  --- jit tail ---"    >&2; tail -3 "$cf.j" >&2
    echo "  --- vm tail ---"     >&2; tail -3 "$cf.v" >&2
    fail=1
  fi
  cat "$cf.i" >> "$out_interp"; cat "$cf.j" >> "$out_jit"
  cat "$cf.v" >> "$out_vm"
done
[ "$fail" = 0 ] || exit 1

# Compare one pair of lanes. On a difference, count the divergences per
# category (the label prefix before the first '|') and show the paired lines.
compare_lanes() {
  local left="$1" right="$2" lf="$3" rf="$4" d
  if diff -q "$lf" "$rf" > /dev/null; then
    echo "difftest: $cases cases, $left == $right ✓"
    return 0
  fi
  # --text: a case is free to print a NUL, and without it diff answers
  # "binary files differ" and the report below has nothing to show.
  d=$(diff --text "$lf" "$rf" | grep -E '^[<>]')
  echo "difftest: $cases cases, $(grep -c '^<' <<< "$d") DIVERGENCES" \
       "($left vs $right)"
  echo
  echo "by category:"
  grep '^<' <<< "$d" | sed 's/^< //; s/|.*//' | sort | uniq -c | sort -rn
  echo
  echo "divergences (< $left / > $right):"
  printf '%s\n' "$d"
  return 1
}

status=0
compare_lanes --vm --jit "$out_vm" "$out_jit" || status=1
compare_lanes --tree --vm "$out_interp" "$out_vm" || status=1

exit "$status"
