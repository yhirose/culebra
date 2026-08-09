#!/usr/bin/env bash
# Regression test for the parser's behaviour on pathological nesting.
#
# Depth (include/parser.h get_parser's enter/leave hooks): deep
# machine-written nesting must be a SyntaxError, not a C-stack SIGSEGV;
# realistic generated nesting must still parse. Nesting descends through
# whichever recursive rule family probes first (a `[` tower dives through the
# *pattern* rules before the array literal), so all three bracket shapes are
# exercised.
#
# Time: nesting that parses must not backtrack exponentially. The towers below
# stay under 4000 rules deep, so they are pure time checks — they finish in
# milliseconds and would take 2^depth if peglib stopped memoizing the rules
# that alternatives sharing a consuming prefix re-parse (see TUPLE_PATTERN in
# grammar_def.h for the shape that depends on it).
# Usage: parse_depth_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: parse_depth_test.sh <culebra-binary>}"
TMP="${TMPDIR:-/tmp}/culebra_parse_depth_test.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
fail=0

deep() {  # deep <open> <close> <count> — `let x = <<<1>>>` nested N deep
  local n="$3" o="" c=""
  o=$(printf "%${n}s" | tr ' ' "$1")
  c=$(printf "%${n}s" | tr ' ' "$2")
  printf 'let x = %s1%s\nIO.println(1)\n' "$o" "$c"
}

run_ok() {  # run_ok <label> <expected-stdout> <culebra-args...>
  local label="$1" expected="$2" out
  shift 2
  if ! out=$("$CULEBRA" "$@" 2>&1) || [ "$out" != "$expected" ]; then
    echo "FAIL $label: expected '$expected', got: ${out:0:120}"
    fail=1
  fi
}

# --- too deep: clean SyntaxError, process must not crash -------------------
# `run`, `fmt` and `lint` all parse the file; parser.h converts the guard's
# throw into the ordinary parse-error channel, so each reports and exits with
# its usual error status. rc in 128..254 is a signal death — the SIGSEGV this
# guard exists to prevent — and 134 is what an uncaught throw would be.
for shape in "[ ]" "{ }" "( )"; do
  set -- $shape
  deep "$1" "$2" 20000 > "$TMP/deep.cul"
  for cmd in "" "fmt --check" "lint"; do
    out=$("$CULEBRA" $cmd "$TMP/deep.cul" 2>&1)
    rc=$?
    if [ $rc -ge 128 ] && [ $rc -lt 255 ]; then
      echo "FAIL ${cmd:-run} deep '$1': died with signal (rc=$rc), expected a diagnostic"
      fail=1
    elif ! printf '%s' "$out" | grep -q "nesting too deep"; then
      echo "FAIL ${cmd:-run} deep '$1': expected 'nesting too deep', got: ${out:0:120}"
      fail=1
    fi
  done
done

# --- realistic generated nesting still parses and runs ---------------------
deep "[" "]" 500 > "$TMP/ok.cul"
run_ok "500-deep array" 1 "$TMP/ok.cul"
run_ok "500-deep array (jit)" 1 --jit "$TMP/ok.cul"

# --- nesting that parses must not backtrack exponentially ------------------
# `(` is the shape that costs: LET and MUTABLE are both optional, so
# EXPRESSION's first alternative (DESTRUCTURE_ASSIGN) reaches TUPLE_PATTERN
# for every `(`-initial expression, and TUPLE_PATTERN's two alternatives
# re-parse PATTERN at the same position. 200 parens took 2^200 steps until
# peglib's packrat filter started memoizing across a shared prefix.
deep "(" ")" 200 > "$TMP/paren.cul"
run_ok "200-deep parens" 1 "$TMP/paren.cul"

# The same tower through TUPLE_PATTERN itself: nested single-element tuples,
# pattern and value alike.
{
  printf 'let mut '
  printf '(%.0s' $(seq 1 100); printf 'a'; printf ',)%.0s' $(seq 1 100)
  printf ' = '
  printf '(%.0s' $(seq 1 100); printf '1'; printf ',)%.0s' $(seq 1 100)
  printf '\nIO.println(a)\n'
} > "$TMP/tuple.cul"
run_ok "100-deep tuple pattern" 1 "$TMP/tuple.cul"

# --- flat width is not depth: long operator chains stay fine ---------------
{
  printf 'let x = '
  printf '1+%.0s' $(seq 1 50000)
  printf '1\nIO.println(x)\n'
} > "$TMP/wide.cul"
run_ok "50k-term chain" 50001 "$TMP/wide.cul"

if [ $fail -eq 0 ]; then
  echo "parse_depth_test OK"
else
  exit 1
fi
