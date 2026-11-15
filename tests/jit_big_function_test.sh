#!/usr/bin/env bash
# The JIT compiles a function past kMaxOptimizedBlocks (include/jit/jit.h)
# unoptimised: `optnone` + `noinline`, so neither the IR pipeline nor the
# backend spends superlinear time on it. A program that only computes the same
# answer either way cannot see the cut go, so this reads the IR.
# Usage: <path-to-culebra>
set -u
CULEBRA="${1:?usage: jit_big_function_test.sh <culebra-binary>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
LIMIT=4096

emit() { CULEBRA_JIT_CACHE=off "$CULEBRA" --jit "$@" --emit-llvm "$TMP/p.cul" 2>/dev/null; }

# The functions the lowering hands the pipeline with more than LIMIT blocks
# (-O0 prints the lowered IR as it is).
big_names() {
  emit -O0 | awk -v min="$LIMIT" '
    /^define/ { n = $0; sub(/\(.*/, "", n); sub(/.*@/, "", n); b = 0; f = 1; next }
    f && /^}/ { if (b > min) print n; f = 0; next }
    f && /^[A-Za-z0-9_.$"-]+:/ { b++ }'
}

# The attributes the optimised module gives function `name`.
attrs_of() {
  awk -v name="$1" '
    $0 ~ "^define .*@" name "\\(" { split($0, w, "#"); g = w[2]; sub(/[^0-9].*/, "", g) }
    /^attributes #/ { a = $2; sub(/#/, "", a); attrs[a] = $0 }
    END { print attrs[g] }' "$TMP/opt.ll"
}

# tests/test_jit_big_function.cul holds one function past the cut.
cp "$HERE/test_jit_big_function.cul" "$TMP/p.cul"
names=$(big_names)
emit -O2 > "$TMP/opt.ll"
if [[ -z "$names" ]]; then
  echo "FAIL: no function past $LIMIT blocks in test_jit_big_function.cul"
  fail=1
fi
for n in $names; do
  a=$(attrs_of "$n")
  if [[ "$a" != *optnone* || "$a" != *noinline* ]]; then
    echo "FAIL: $n (past $LIMIT blocks) is not optnone + noinline: $a"
    fail=1
  fi
done

# A small program keeps the whole pipeline.
printf 'fn f(x) { x > 1 ? x * 2 : x + 1 }\nprintln(f(3))\n' > "$TMP/p.cul"
if emit -O2 | grep -q optnone; then
  echo "FAIL: a small program was compiled optnone"
  fail=1
fi

[[ $fail -eq 0 ]] && echo "jit_big_function_test: OK"
exit $fail
