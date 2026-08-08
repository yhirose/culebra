#!/usr/bin/env bash
# Regression test for the parser's nesting-depth guard (include/parser.h
# get_parser's enter/leave hooks). Deep machine-written nesting must be a
# SyntaxError, not a C-stack SIGSEGV; realistic generated nesting must still
# parse. Nesting descends through whichever recursive rule family probes
# first (a `[` tower dives through the *pattern* rules before the array
# literal), so both bracket shapes are exercised.
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
for shape in "[ ]" "{ }"; do
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
