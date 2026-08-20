#!/usr/bin/env bash
# Differential test: generate the template-combinator corpus, run it under the
# tree interpreter and the JIT, and diff the outputs byte-for-byte. Any
# divergence is an interp/JIT behavioural asymmetry (the language's core
# correctness invariant: interp(P) == jit(P) for every program P).
#
# AOT is covered transitively — `just test` already asserts aot == jit, so
# interp == jit here plus aot == jit there gives interp == aot.
#
# The bytecode VM is a third lane, compared per record rather than
# byte-for-byte: its supported slice is still growing, and a construct outside
# it makes the case that reaches it raise VmError instead of answering (the
# whole module used to be rejected — see the poisoned chunk in vm.h). Those
# records are skips, and their count is a ratchet: it may fall, never rise.
#
# Usage: tools/difftest/run.sh [culebra-binary] [--update-vm-skips]
#        (binary defaults to ./build/culebra)
set -uo pipefail

CULEBRA="${1:-./build/culebra}"
UPDATE_VM_SKIPS=0
for a in "$@"; do [ "$a" = --update-vm-skips ] && UPDATE_VM_SKIPS=1; done
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${DIFFTEST_WORK:-build/difftest}"
mkdir -p "$WORK"

out_interp="$WORK/out_interp.txt"
out_jit="$WORK/out_jit.txt"
out_vm="$WORK/out_vm.txt"
# What marks a record as outside the slice rather than wrong: the VM named
# VmError and the interpreter did not. Not the `err=VmError|` prefix alone —
# a case may catch the rejection and report `e.kind` itself, which lands in an
# `ok=` record. Only the slice boundary raises VmError, and no other backend
# raises it at all, so its appearance on one side is exactly "this case is
# outside the slice".
VM_SKIP_MARK='VmError'
VM_SKIP_CEILING="$HERE/vm_skip_ceiling.txt"
# The JIT compiles each manifest as a single LLVM module, and compile time
# grows super-linearly in module size — one giant 5000-case module takes
# ~2min, where the same cases in chunks take a fraction. Chunking also gives
# the parallel runner below independent units of work. Each `_p` case is
# self-contained, so chunking never splits a case; both backends run the
# identical chunk files, so error-record line numbers stay chunk-local but
# consistent across backends (the diff is still exact). ~400 is the empirical
# sweet spot on an 8-core box (smaller modules compile faster and balance the
# pool better, but below ~256 per-process startup starts to dominate). Tunable.
CHUNK="${DIFFTEST_CHUNK:-400}"

# Generate cases (the static probe preamble is prepended per chunk below).
if ! "$CULEBRA" --tree "$HERE/gen.cul" > "$WORK/cases.cul"; then
  echo "difftest: FAIL — generator gen.cul did not run cleanly" >&2
  exit 1
fi
cases=$(grep -c '^_p(' "$WORK/cases.cul")

# Sanity floor: the generator must emit a substantial corpus. A near-empty
# count means gen.cul silently degraded (a swept dimension stopped emitting).
if [ "$cases" -lt 1000 ]; then
  echo "difftest: FAIL — only $cases cases generated (expected >= 1000)" >&2
  exit 1
fi

# Split into chunks. Each chunk record is one line:
#   <label> ::: ok=<Type>:<repr>      | err=<kind>|<message>|<line>|<col>
chunkdir="$WORK/chunks"
rm -rf "$chunkdir"; mkdir -p "$chunkdir"
split -l "$CHUNK" "$WORK/cases.cul" "$chunkdir/c."
# Capture the bare chunk list now, before per-chunk derivatives (.cul/.i/.j)
# land in the same dir — the concat below must glob only the split outputs.
chunks=( "$chunkdir"/c.* )

# Prepend the probe preamble to each chunk once (cheap, serial).
for cf in "${chunks[@]}"; do cat "$HERE/preamble.cul" "$HERE/canvas_fixtures.cul" "$cf" > "$cf.cul"; done

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
# through as a false "interp == jit ✓". Fail loudly instead. Concatenate in
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

status=0
if diff -q "$out_interp" "$out_jit" >/dev/null; then
  echo "difftest: $cases cases, interp == jit ✓"
else
  # Report: count divergences per category (label prefix before the first '|')
  # and show the paired interp/jit lines.
  diverging=$(diff "$out_interp" "$out_jit" | grep -c '^<')
  echo "difftest: $cases cases, $diverging DIVERGENCES (interp vs jit)"
  echo
  echo "by category:"
  diff "$out_interp" "$out_jit" | grep '^<' | sed 's/^< //; s/|.*//' \
    | sort | uniq -c | sort -rn
  echo
  echo "divergences (< interp / > jit):"
  diff "$out_interp" "$out_jit" | grep -E '^[<>]'
  status=1
fi

# --- The VM lane ---
#
# Not a byte diff: a case the slice does not cover answers VmError, and its
# `print`/`inspect` output is missing with it, so the files do not align line
# by line. They do align RECORD by record — `_p` emits exactly one per case,
# in order, under every backend — so walk the records positionally and carry
# each one's trailing output with it. That needs no assumption that labels are
# unique. A NUL a case prints would end an awk record early; both files get the
# same escaping, so a difference in them still shows.
esc() { perl -pe 's/\0/\\0/g' "$1"; }
esc "$out_interp" > "$out_interp.esc"; esc "$out_vm" > "$out_vm.esc"
vm_report="$WORK/vm_report.txt"
: > "$vm_report"   # awk only truncates it when it has something to write
vm_skips=$(awk -v mark="$VM_SKIP_MARK" -v report="$vm_report" '
  function record(l) { return l ~ / ::: (ok=|err=)/ }
  FNR == 1 { file++; n = 0 }
  { if (record($0)) { n++; if (file == 1) { vr[n] = $0 } else { ir[n] = $0 } }
    else if (n) { if (file == 1) { vx[n] = vx[n] "\n" $0 }
                  else { ix[n] = ix[n] "\n" $0 } } }
  file == 1 { vn = n } file == 2 { in_ = n }
  END {
    if (vn != in_) {
      printf "record count differs: vm=%d interp=%d\n", vn, in_ > report
      print "-1"; exit
    }
    skips = 0
    for (i = 1; i <= vn; i++) {
      if ((index(vr[i] vx[i], mark)) && !index(ir[i] ix[i], mark)) {
        skips++; continue
      }
      if (vr[i] != ir[i] || vx[i] != ix[i])
        printf "< interp %s%s\n> vm     %s%s\n", ir[i], ix[i], vr[i], vx[i] \
          > report
    }
    print skips
  }' "$out_vm.esc" "$out_interp.esc")
vm_diverging=$(grep -c '^< interp ' "$vm_report" 2>/dev/null || true)
[ -n "$vm_diverging" ] || vm_diverging=0

if [ "$vm_skips" -lt 0 ] 2>/dev/null || [ "$vm_diverging" != 0 ]; then
  echo "difftest: $vm_diverging DIVERGENCES (interp vs --vm)"
  echo
  echo "by category:"
  grep '^< interp ' "$vm_report" | sed 's/^< interp //; s/|.*//' \
    | sort | uniq -c | sort -rn
  echo
  head -200 "$vm_report"
  status=1
else
  ceiling=$(cat "$VM_SKIP_CEILING" 2>/dev/null || echo 0)
  if [ "$UPDATE_VM_SKIPS" = 1 ]; then
    echo "$vm_skips" > "$VM_SKIP_CEILING"
    echo "difftest: --vm skip ceiling updated to $vm_skips"
  elif [ "$vm_skips" -gt "$ceiling" ]; then
    echo "difftest: FAIL — --vm skipped $vm_skips cases, ceiling is $ceiling" >&2
    echo "  The supported slice shrank, or the corpus grew: re-run with" >&2
    echo "  --update-vm-skips once the increase is understood." >&2
    status=1
  else
    echo "difftest: $((cases - vm_skips))/$cases cases on --vm," \
         "interp == vm ✓ (${vm_skips} outside the slice, ceiling $ceiling)"
  fi
fi
exit "$status"
