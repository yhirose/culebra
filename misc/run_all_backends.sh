#!/usr/bin/env bash
# Run one .cul file through interp, JIT and AOT, and require the same output
# from all three.
#
# Usage: misc/run_all_backends.sh <culebra exe> <script.cul> <expected> <LABEL>
#
# The CI jobs that check backend symmetry on Windows otherwise each carried
# their own copy of this: the -e-safe capture (the msys2 shell runs with -e, so
# a failing command substitution ends the step before the output it captured
# can be printed), the timeout, and the per-backend pass/fail labelling.
set +e  # deliberately not -e; see above

exe=${1:?usage: run_all_backends.sh <exe> <script.cul> <expected> <LABEL>}
script=${2:?}
expected=${3:?}
label=${4:?}

fail=0
for mode in "" "--jit"; do
  echo "=== $exe $mode $script ==="
  out=$(timeout 60 "$exe" $mode "$script" 2>&1); rc=$?
  echo "out=[$out] rc=$rc"
  [ "$out" = "$expected" ] || { echo "${label}_${mode:---interp}_FAIL"; fail=1; }
done

echo "=== $exe build $script ==="
aot=./aot_$label.exe
if "$exe" build "$script" -o "$aot"; then
  out=$(timeout 60 "$aot" 2>&1); rc=$?
  echo "out=[$out] rc=$rc"
  [ "$out" = "$expected" ] && echo "${label}_AOT_PASS" \
    || { echo "${label}_AOT_RUN_FAIL"; fail=1; }
else
  echo "${label}_AOT_BUILD_FAIL rc=$?"
  fail=1
fi
exit $fail
