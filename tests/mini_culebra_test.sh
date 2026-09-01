#!/usr/bin/env bash
# examples/mini-culebra/mini_culebra.cul against the real culebra binary:
# every samples/*.cul is written in the subset both implement, so the real
# implementation is the compiler's oracle. Each sample must produce
# identical stdout from both, under the executor and --jit.
#
# No AOT leg: pl0_codegen_test.sh already proves the CodeGen archive links
# and runs under AOT, and this test adds no new runtime surface to that
# axis. No error-diff leg either: a failing program is *supposed* to
# differ (mini reports IrError where culebra reports ZeroDivisionError and
# so on), so only the binder's own diagnostics are pinned, directly.
#
# Usage: mini_culebra_test.sh <path-to-culebra>
set -u

CULEBRA="${1:?usage: mini_culebra_test.sh <culebra>}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MINI="$ROOT/examples/mini-culebra/mini_culebra.cul"
SAMPLES="$ROOT/examples/mini-culebra/samples"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

# run <desc> <engine-flag> <sample>
run() {
  local desc="$1" flag="$2" sample="$3"
  local want got
  want="$("$CULEBRA" "$flag" "$SAMPLES/$sample" 2>&1)"
  got="$("$CULEBRA" "$flag" "$MINI" "$SAMPLES/$sample" 2>&1)"
  if [ "$got" != "$want" ]; then
    echo "FAIL [$desc]: mini_culebra differs from culebra itself" >&2
    echo "--- culebra ---" >&2; echo "$want" >&2
    echo "--- mini_culebra ---" >&2; echo "$got" >&2
    fail=1
  else
    echo "ok   [$desc]"
  fi
}

for sample in arith.cul fact.cul fib.cul gcd.cul counter.cul hof.cul \
              mutual.cul nested.cul adder.cul; do
  run "vm  $sample"  "--vm"  "$sample"
  run "jit $sample"  "--jit" "$sample"
done

# --- Diagnostics: the binder's own rejections, checked directly (the
# restrictions are deliberate -- see mini_culebra.cul's header). ---
check_err() {
  local desc="$1" src="$2" want_substr="$3"
  local f="$TMP/$desc.cul"
  printf '%s' "$src" >"$f"
  local got
  got="$("$CULEBRA" --vm "$MINI" "$f" 2>&1)"
  case "$got" in
    *"$want_substr"*) echo "ok   [err $desc]" ;;
    *)
      echo "FAIL [err $desc]: want substring [$want_substr] got [$got]" >&2
      fail=1
      ;;
  esac
}

check_err "undefvar" 'println(x)
' "undefined variable 'x'"

# A closure cannot see its own binding -- the self-reference would be the
# cell->closure->cell cycle reference counting cannot free.
check_err "selfref" 'let f = fn () { f() }
println(f())
' "undefined name 'f'"

check_err "fnvalue" 'fn g() { 1 }
let h = g
println(h)
' "can only be called"

check_err "letassign" 'let x = 1
x = 2
println(x)
' "cannot assign to immutable 'x'"

# The runaway named-fn recursion fails at the VM's own frame bound instead
# of overflowing the host stack.
check_err "runaway" 'fn spin(n) { spin(n + 1) }
println(spin(0))
' "recursion limit exceeded"

if [ "$fail" -ne 0 ]; then
  echo "mini_culebra_test FAIL" >&2
  exit 1
fi
