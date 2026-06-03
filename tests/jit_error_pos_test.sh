#!/usr/bin/env bash
# Regression test for compile-time error position symmetry between the
# interp and the JIT (include/jit.h compile() wrapper). The interp stamps
# the deepest eval()'s AST location onto any CulebraError thrown with
# line==0; the JIT's compile() wrapper mirrors this for compile-time
# throws, so a compile_* site that omits ast.line/column still yields the
# same `... at L:C.` as the interp. This guards against the recurring
# "JIT forgot the position" divergence. Usage: <path-to-culebra>
set -u
CULEBRA="${1:?usage: jit_error_pos_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# Each case is a program that fails at compile time. The interp and the
# JIT must print byte-identical diagnostics (kind + message + position).
check_same() {
  local name="$1" prog="$2"
  printf '%s\n' "$prog" > "$TMP/t.cul"
  local out_i out_j
  out_i=$("$CULEBRA" "$TMP/t.cul" 2>&1)
  out_j=$("$CULEBRA" --jit "$TMP/t.cul" 2>&1)
  if [[ "$out_i" != "$out_j" ]]; then
    echo "FAIL [$name]: interp/jit diverge"
    echo "  interp: $out_i"
    echo "  jit:    $out_j"
    fail=1
  elif [[ "$out_i" != *" at "*":"*"."* ]]; then
    echo "FAIL [$name]: no position in diagnostic: $out_i"
    fail=1
  fi
}

# compound-declare and `??=` throw in the JIT without an explicit position;
# the compile() wrapper must backfill the ASSIGNMENT node's line/col.
check_same "compound declare"   'let x += 1'
check_same "??= non-simple"     'o.a ??= 1'
# These pass an explicit (often more specific) position; symmetry must hold.
check_same "non-default param"  'fn f(a = 1, b) { a }'
check_same "*args not last"     'fn f(*xs, y) { y }'
# break/continue outside loop is hoisted to the shared lint pass, but the
# interp/jit must still agree on the SyntaxError + position.
check_same "break outside loop" 'break'
check_same "continue in fn"     'fn f() { continue }'

if [[ $fail -eq 0 ]]; then echo "jit_error_pos_test OK"; exit 0; fi
exit 1
