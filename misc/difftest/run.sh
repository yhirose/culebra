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

manifest="$WORK/manifest.cul"
out_interp="$WORK/out_interp.txt"
out_jit="$WORK/out_jit.txt"

# Generate cases, then prepend the static probe preamble to form the manifest.
"$CULEBRA" "$HERE/gen.cul" > "$WORK/cases.cul"
cat "$HERE/preamble.cul" "$WORK/cases.cul" > "$manifest"
cases=$(grep -c '^_p(' "$WORK/cases.cul")

# Run both backends to completion, then diff. Each manifest line is one case:
#   <label> ::: ok=<Type>:<repr>      | err=<kind>|<message>|<line>|<col>
"$CULEBRA"       "$manifest" > "$out_interp" 2>&1
"$CULEBRA" --jit "$manifest" > "$out_jit"    2>&1

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
