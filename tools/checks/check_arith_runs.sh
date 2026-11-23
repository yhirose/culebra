#!/usr/bin/env bash
# Operators on values the compiler cannot type fold to bare doubles: the
# lowering emits a stretch of them twice, once behind a guard that every
# input holds a Float (Lowering::arith_runs, docs/internals/vm.md §7), and
# inside that copy one operator's double feeds the next with no test between.
#
# The control is the same arithmetic with a call cutting the stretch, so no
# product feeds a sum directly; a control that folded on its own would mean
# the gate measures nothing. -O0 runs no pipeline to fold a copy, so it
# makes none.
set -euo pipefail
cd "$(dirname "$0")/../.."

# The binary under test, as check_float_carry.sh takes it.
BIN="${1:-build-dev/culebra}"
TMP="$(mktemp -d "${TMPDIR:-/tmp}/culebra-arithruns.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/run.cul" <<'CUL'
class P {
  new(@a, @b, @c) {}
}
fn hot(p) {
  let (a, b, c) = (p.a, p.b, p.c)
  a * b + c
}
println(hot(P(1.5, 2.0, 0.25)))
CUL

cat > "$TMP/cut.cul" <<'CUL'
class P {
  new(@a, @b, @c) {}
}
fn id(x) {
  x
}
fn hot(p) {
  let (a, b, c) = (p.a, p.b, p.c)
  id(a * b) + c
}
println(hot(P(1.5, 2.0, 0.25)))
CUL

# An fadd whose operand is an fmul's result as it stands: nothing tested and
# merged it on the way.
chained() {
  CULEBRA_JIT_CACHE=off "$BIN" --jit --emit-llvm "$1" 2>/dev/null |
    grep -cE '= fadd double (%fmul[0-9]*,|.*, %fmul[0-9]*$)' || true
}

run=$(chained "$TMP/run.cul")
cut=$(chained "$TMP/cut.cul")
left=$(CULEBRA_JIT_CACHE=off "$BIN" --jit -O0 --emit-llvm "$TMP/run.cul" 2>/dev/null |
  grep -c 'arith\.' || true)

if [ "$cut" -ne 0 ]; then
  echo "arith-runs FAIL: the control, whose operators a call keeps apart, still" >&2
  echo "  chains a product into a sum ($cut), so this gate measures nothing." >&2
  echo "  Rewrite the control so it needs the guarded copy to fold." >&2
  exit 1
fi

if [ "$run" -eq 0 ]; then
  echo "arith-runs FAIL: Float operands read from fields leave every operator" >&2
  echo "  tested and merged; the guarded copy (Lowering::arith_runs) is gone" >&2
  echo "  or the pipeline no longer folds it." >&2
  exit 1
fi

if [ "$left" -ne 0 ]; then
  echo "arith-runs FAIL: -O0 carries $left line(s) of a guarded copy, which" >&2
  echo "  nothing there folds; lower_program(optimized=false) must make none." >&2
  exit 1
fi

echo "arith-runs OK (chained $run, control $cut, -O0 left 0)"
