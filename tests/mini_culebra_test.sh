#!/usr/bin/env bash
# examples/languages/mini-culebra/mini_culebra.cul against the real culebra binary:
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
MINI="$ROOT/examples/languages/mini-culebra/mini_culebra.cul"
SAMPLES="$ROOT/examples/languages/mini-culebra/samples"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0

# All samples go through one mini invocation per engine, so the compiler's
# own parse (and, under --jit, its compile) is paid once per lane instead
# of once per sample -- per-sample invocation blew CI's ctest budget. The
# batch driver prints a marker line before each program; the oracle side
# is the per-sample culebra runs joined by the same markers.
lane() {
  local flag="$1"
  : >"$TMP/want$flag"
  local path
  for path in "$SAMPLES"/*.cul; do
    {
      echo "-----8<----- $(basename "$path")"
      "$CULEBRA" "$flag" "$path" 2>&1
    } >>"$TMP/want$flag"
  done
  "$CULEBRA" "$flag" "$MINI" "$SAMPLES"/*.cul >"$TMP/got$flag" 2>&1
}
lane --vm &
lane --jit &
wait
for flag in --vm --jit; do
  if ! diff -u "$TMP/want$flag" "$TMP/got$flag" >"$TMP/diff$flag"; then
    echo "FAIL [$flag]: mini_culebra differs from culebra itself" >&2
    cat "$TMP/diff$flag" >&2
    fail=1
  else
    echo "ok   [$flag $(ls "$SAMPLES"/*.cul | wc -l | tr -d ' ') samples]"
  fi
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
