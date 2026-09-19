#!/usr/bin/env bash
# `culebra test` must report a test that calls Sys.exit as a failure and go on
# to the rest. The process ending there hid every later verdict — and, at
# exit(0), reported the run green. Not reachable from a .cul sweep: the case
# is about what the runner process does.
# Usage: unit_runner_test.sh <path-to-culebra>
set -u
CULEBRA="${1:?usage: unit_runner_test.sh <culebra-binary>}"
[[ "$CULEBRA" = /* ]] || CULEBRA="$PWD/$CULEBRA"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

printf 'test("exits", fn () {\n  Sys.exit(0)\n})\n' > "$TMP/test_a.cul"
printf 'test("passes", fn () {\n  assert_eq(1, 1)\n})\n' > "$TMP/test_b.cul"
out=$(cd "$TMP" && "$CULEBRA" test . 2>&1); rc=$?
[[ $rc -eq 1 ]] || { echo "FAIL rc: expected 1, got $rc"; echo "$out"; fail=1; }
[[ "$out" == *"ExitError"* ]] || { echo "FAIL: no ExitError reported"; echo "$out"; fail=1; }
[[ "$out" == *"1 passed, 1 failed"* ]] || { echo "FAIL summary: $out"; fail=1; }

exit $fail
