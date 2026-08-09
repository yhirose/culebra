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
# 4000 rules deep are the guard — they finish in milliseconds now and would
# take 2^depth with a pattern rule whose alternatives share a consuming prefix
# (see TUPLE_PATTERN in grammar_def.h).
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

# --- too deep: clean SyntaxError, process must not crash -------------------
for shape in "[ ]" "{ }" "( )"; do
  set -- $shape
  deep "$1" "$2" 20000 > "$TMP/deep.cul"
  out=$("$CULEBRA" "$TMP/deep.cul" 2>&1)
  rc=$?
  # culebra exits 255 for a reported error; 128+N (but never 255) is a
  # signal death — the SIGSEGV this guard exists to prevent.
  if [ $rc -ge 128 ] && [ $rc -lt 255 ]; then
    echo "FAIL deep '$1': died with signal (rc=$rc), expected SyntaxError"
    fail=1
  elif ! printf '%s' "$out" | grep -q "SyntaxError: nesting too deep"; then
    echo "FAIL deep '$1': expected 'nesting too deep', got: ${out:0:120}"
    fail=1
  fi

  # `fmt` and `lint` parse the same file. The guard throws mid-parse, so each
  # has to catch it and report — an uncaught throw is terminate(), rc 134.
  for cmd in "fmt --check" "lint"; do
    out=$("$CULEBRA" $cmd "$TMP/deep.cul" 2>&1)
    rc=$?
    if [ $rc -ge 128 ] && [ $rc -lt 255 ]; then
      echo "FAIL $cmd deep '$1': died with signal (rc=$rc), expected a diagnostic"
      fail=1
    elif ! printf '%s' "$out" | grep -q "nesting too deep"; then
      echo "FAIL $cmd deep '$1': expected 'nesting too deep', got: ${out:0:120}"
      fail=1
    fi
  done
done

# --- realistic generated nesting still parses and runs ---------------------
deep "[" "]" 500 > "$TMP/ok.cul"
if ! out=$("$CULEBRA" "$TMP/ok.cul" 2>&1) || [ "$out" != "1" ]; then
  echo "FAIL 500-deep array: expected to run, got: ${out:0:120}"
  fail=1
fi
if ! out=$("$CULEBRA" --jit "$TMP/ok.cul" 2>&1) || [ "$out" != "1" ]; then
  echo "FAIL 500-deep array (jit): expected to run, got: ${out:0:120}"
  fail=1
fi

# --- nesting that parses must not backtrack exponentially ------------------
# `(` is the shape that costs: LET and MUTABLE are both optional, so
# EXPRESSION's first alternative (DESTRUCTURE_ASSIGN) reaches TUPLE_PATTERN
# for every `(`-initial expression. Both towers are far under the depth limit,
# so they are pure time checks — 200 parens took 2^200 steps before
# TUPLE_PATTERN was left-factored.
deep "(" ")" 200 > "$TMP/paren.cul"
if ! out=$("$CULEBRA" "$TMP/paren.cul" 2>&1) || [ "$out" != "1" ]; then
  echo "FAIL 200-deep parens: expected to run, got: ${out:0:120}"
  fail=1
fi

# The same tower through TUPLE_PATTERN itself: nested single-element tuples,
# pattern and value alike.
{
  printf 'let mut '
  printf '(%.0s' $(seq 1 100); printf 'a'; printf ',)%.0s' $(seq 1 100)
  printf ' = '
  printf '(%.0s' $(seq 1 100); printf '1'; printf ',)%.0s' $(seq 1 100)
  printf '\nIO.println(a)\n'
} > "$TMP/tuple.cul"
if ! out=$("$CULEBRA" "$TMP/tuple.cul" 2>&1) || [ "$out" != "1" ]; then
  echo "FAIL 100-deep tuple pattern: expected to run, got: ${out:0:120}"
  fail=1
fi

# --- flat width is not depth: long operator chains stay fine ---------------
{
  printf 'let x = '
  printf '1+%.0s' $(seq 1 50000)
  printf '1\nIO.println(x)\n'
} > "$TMP/wide.cul"
if ! out=$("$CULEBRA" "$TMP/wide.cul" 2>&1) || [ "$out" != "50001" ]; then
  echo "FAIL 50k-term chain: expected 50001, got: ${out:0:120}"
  fail=1
fi

if [ $fail -eq 0 ]; then
  echo "parse_depth_test OK"
else
  exit 1
fi
