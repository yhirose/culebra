#!/usr/bin/env bash
# Regression test for how the driver takes its input and for --ast (src/main.cc
# read_file / run_scripts). None of it is reachable from a .cul sweep: the cases
# are about what the process does with a non-regular path, an unwritable file,
# or a flag documented to stop before running.
# Usage: cli_input_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: cli_input_test.sh <culebra-binary>}"
TMP=$(mktemp -d)
trap 'chmod -R u+w "$TMP" 2>/dev/null; rm -rf "$TMP"' EXIT
fail=0

# A script arriving through a pipe has no size to seek to. Sizing it up front
# yielded 0 and ran an empty program, exiting 0 with no output at all.
out=$("$CULEBRA" <(printf 'IO.println("piped")\n') 2>&1); rc=$?
[[ $rc -eq 0 && "$out" == "piped" ]] || { echo "FAIL pipe: rc=$rc out=$out"; fail=1; }

# A directory opens like a file, so it used to reach the reader and blow up
# there (a huge seek size, then a throw out of the stream).
out=$("$CULEBRA" "$TMP" 2>&1); rc=$?
[[ $rc -ne 0 ]] || { echo "FAIL dir: expected failure, got rc=0"; fail=1; }
[[ "$out" == *"can't open"* ]] || { echo "FAIL dir: unexpected message: $out"; fail=1; }

# --ast prints the parsed program *instead of running it* (docs/language.md,
# and the --help text) — the dump used to be followed by a real run.
printf 'IO.println("RAN")\n' > "$TMP/ast.cul"
out=$("$CULEBRA" --ast "$TMP/ast.cul" 2>&1); rc=$?
[[ $rc -eq 0 ]] || { echo "FAIL --ast: rc=$rc"; fail=1; }
[[ -n "$out" ]] || { echo "FAIL --ast: printed nothing"; fail=1; }
grep -qx 'RAN' <<<"$out" && { echo "FAIL --ast: the program ran"; fail=1; }

# One program has one parsed AST: --jit must not change the dump (it splices
# the stdlib preamble in for its own run, which is not what was parsed).
if "$CULEBRA" --help 2>&1 | grep -q -- '--jit'; then
  jit_out=$("$CULEBRA" --ast --jit "$TMP/ast.cul" 2>&1)
  [[ "$jit_out" == "$out" ]] || { echo "FAIL --ast: --jit dumps a different AST"; fail=1; }
fi

# An in-place rewrite that can't be written must say so and exit non-zero,
# rather than reporting the file as formatted/fixed.
printf 'let  x=1\n' > "$TMP/ro.cul"
chmod 444 "$TMP/ro.cul"
out=$("$CULEBRA" fmt -i "$TMP/ro.cul" 2>&1); rc=$?
[[ $rc -eq 2 && "$out" == *"can't write"* ]] || {
  echo "FAIL fmt -i readonly: rc=$rc out=$out"; fail=1; }
grep -qx 'let  x=1' "$TMP/ro.cul" || { echo "FAIL fmt -i readonly: file changed"; fail=1; }

printf "import Math from 'std/math'\nIO.println(1)\n" > "$TMP/ro_lint.cul"
chmod 444 "$TMP/ro_lint.cul"
out=$("$CULEBRA" lint --fix "$TMP/ro_lint.cul" 2>&1); rc=$?
[[ $rc -eq 2 && "$out" == *"can't write"* ]] || {
  echo "FAIL lint --fix readonly: rc=$rc out=$out"; fail=1; }
[[ "$out" != *"fixed 1 unused import"* ]] || {
  echo "FAIL lint --fix readonly: reported a fix it could not write"; fail=1; }

if [[ $fail -eq 0 ]]; then echo "cli_input_test OK"; exit 0; fi
echo "cli_input_test FAILED"; exit 1
