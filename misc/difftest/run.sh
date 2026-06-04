#!/usr/bin/env bash
# Differential test: generate the template-combinator corpus, run it under the
# tree interpreter and the JIT, and diff the outputs byte-for-byte. Any
# divergence is an interp/JIT behavioural asymmetry (the language's core
# correctness invariant: interp(P) == jit(P) for every program P).
#
# AOT is covered transitively — `just test` already asserts aot == jit, so
# interp == jit here plus aot == jit there gives interp == aot.
#
# Usage: misc/difftest/run.sh [culebra-binary]   (default: ./build/culebra)
set -uo pipefail

CULEBRA="${1:-./build/culebra}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${DIFFTEST_WORK:-build/difftest}"
mkdir -p "$WORK"

out_interp="$WORK/out_interp.txt"
out_jit="$WORK/out_jit.txt"
# The JIT compiles each manifest as a single LLVM module, and compile time
# grows super-linearly in module size — one giant 5000-case module takes
# ~2min, where the same cases in chunks take a fraction. Each `_p` case is
# self-contained, so chunking never splits a case; both backends run the
# identical chunk files, so error-record line numbers stay chunk-local but
# consistent across backends (the diff is still exact). Tunable for probing.
CHUNK="${DIFFTEST_CHUNK:-800}"

# Generate cases (the static probe preamble is prepended per chunk below).
if ! "$CULEBRA" "$HERE/gen.cul" > "$WORK/cases.cul"; then
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

# Split into chunks and run each under both backends, appending to the two
# accumulated outputs in chunk order so the byte diff still aligns case by
# case. Each chunk record is one line:
#   <label> ::: ok=<Type>:<repr>      | err=<kind>|<message>|<line>|<col>
chunkdir="$WORK/chunks"
rm -rf "$chunkdir"; mkdir -p "$chunkdir"
split -l "$CHUNK" "$WORK/cases.cul" "$chunkdir/c."
: > "$out_interp"; : > "$out_jit"
for cf in "$chunkdir"/c.*; do
  m="$cf.cul"
  cat "$HERE/preamble.cul" "$cf" > "$m"
  cc=$(grep -c '^_p(' "$cf")
  "$CULEBRA"       "$m" > "$cf.i" 2>&1
  "$CULEBRA" --jit "$m" > "$cf.j" 2>&1
  # Completion guard (the invariant that makes this test non-vacuous): every
  # `_p` call prints exactly one record line, and `puts`/`print` cases only ADD
  # lines, so a complete chunk yields at least `cc` lines under each backend.
  # Fewer means it never ran to completion — a parse error or crash made both
  # backends emit the same short error, which the byte diff would otherwise wave
  # through as a false "interp == jit ✓". Fail loudly instead.
  ci=$(grep -c '' "$cf.i"); cj=$(grep -c '' "$cf.j")
  if [ "$ci" -lt "$cc" ] || [ "$cj" -lt "$cc" ]; then
    echo "difftest: FAIL — chunk $(basename "$cf") did not run to completion" >&2
    echo "  chunk_cases=$cc  interp_lines=$ci  jit_lines=$cj (expected >= cases)" >&2
    echo "  --- interp tail ---" >&2; tail -3 "$cf.i" >&2
    echo "  --- jit tail ---"    >&2; tail -3 "$cf.j" >&2
    exit 1
  fi
  cat "$cf.i" >> "$out_interp"; cat "$cf.j" >> "$out_jit"
done

if diff -q "$out_interp" "$out_jit" >/dev/null; then
  echo "difftest: $cases cases, interp == jit ✓"
  exit 0
fi

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
exit 1
