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

# Like check_same but without the position requirement — for diagnostics that
# carry no `at L:C.` (e.g. an uncaught top-level `throw`).
check_eq() {
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

# UFCS calls to the unary global builtins (to_string/hash/type_of/to_long/
# to_float). The JIT used to fall through to a property-get TypeError on a
# wrong-arity call (and lacked hash/to_float entirely); now it raises the
# same ArityError as the interp. `to_string` is special — a String/StringView
# value-method (0-arg) vs the arity-1 global elsewhere — so cover both.
check_same "to_string extra (Long)"   '(1).to_string(9)'
check_same "to_string extra (String)" '"x".to_string(9)'
check_same "to_string extra (Array)"  '[1].to_string(9)'
check_same "hash extra (Long)"        '(1).hash(9)'
check_same "hash extra (String)"      '"x".hash(9)'
check_same "type_of extra"            '(5).type_of(9)'
check_same "to_long extra"            '"5".to_long(9)'
check_same "to_float extra"           '(5).to_float(9)'

# A bad UFCS conversion attributes its type/parse error to the call's arg
# list (matching interp's eval_ufcs_call, which invokes with args_ast's
# position) — not the receiver. Both kind+message and position must agree.
check_same "to_long bad type"         'true.to_long()'
check_same "to_long bad type (Array)" '[1].to_long()'
check_same "to_float bad type"        'true.to_float()'
check_same "to_long bad string"       '"abc".to_long()'

# An uncaught top-level `throw` prints `uncaught: <value>`. A thrown string
# prints raw (it's the message), matching the interp's str_display — the JIT
# once quoted it (`uncaught: 'boom'`). Non-strings already agreed.
check_eq "uncaught throw string"      'throw "boom"'
check_eq "uncaught throw int"         'throw 42'
check_eq "uncaught throw object"      'throw {code: 1, msg: "x"}'
check_eq "uncaught throw array"       'throw [1, "a"]'

# A non-Function passed to an iterator/array higher-order method reports
# "parameter '<name>' expects Function" at the ARGUMENT's position, with the
# method's actual param name (map/for_each/... → 'f', filter/find/any/all/
# take_while → 'p'). The JIT once hardcoded 'f' and pointed at 1:1.
check_same "non-fn to eager map"      '[1, 2, 3].map(99)'
check_same "non-fn to eager filter"   '[1, 2, 3].filter(42)'
check_same "non-fn to lazy map"       '[1, 2, 3].iter().map({a: 1}).collect()'
check_same "non-fn to lazy filter"    '[1, 2, 3].iter().filter(42).collect()'
check_same "non-fn to lazy find"      '[1, 2, 3].iter().find(42).collect()'
check_same "non-fn to lazy take_while" '[1, 2, 3].iter().take_while(42).collect()'
check_same "non-fn to sort_by"        '[1, 2, 3].sort_by(42)'
check_same "non-fn to reduce"         '[1, 2, 3].reduce(0, 42)'

# RUNTIME errors from value-neutral lib helpers (tensor.h) are thrown without
# a position. The interp backfills it at its eval boundary; the JIT now mirrors
# this by publishing the op position (emit_set_op_pos) before the fallible call
# and backfilling at the runtime exception boundaries. Without it these printed
# location-less on the JIT/AOT while the interp carried `at L:C`.
check_same "tensor reshape mismatch" 'Tensor.zeros([6]).reshape([4])'
check_same "tensor reshape neg dim"  'Tensor.zeros([6]).reshape([-2])'
check_same "tensor sum bad axis"     'Tensor.zeros([2, 3]).sum(9)'
check_same "tensor slice oob"        'Tensor.zeros([3]).slice(5, 9)'
check_same "tensor reshape nested"   'let x = 1 + Tensor.zeros([6]).reshape([4]).sum()'

if [[ $fail -eq 0 ]]; then echo "jit_error_pos_test OK"; exit 0; fi
exit 1
