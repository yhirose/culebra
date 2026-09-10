#!/usr/bin/env bash
# Run one .cul file through interp, JIT, the VM executor and AOT, and require
# the same output from every lane that runs it (the compiled lanes + AOT;
# the tree-walker lane retired in B7-c). The `--vm` lane rejects
# out-of-slice constructs at compile time ("--vm: unsupported: ..."); that
# reports the lane as SKIP (visible, still green), so each test's VM lane
# lights up on its own as the slice grows. Any other VM mismatch — including a
# runtime VmError — fails like the rest.
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

# What a lane saw, as an annotation as well as on stdout. A run's log needs
# repository admin to read while its annotations do not, so on the platforms
# with no local oracle — Windows above all — this is what carries a failure
# off the runner. `%0A` is how a workflow command spells a newline.
note_failure() {
  local what=$1 rc=$2 detail=$3
  echo "$what"
  [ -n "${GITHUB_ACTIONS:-}" ] || return 0
  detail=$(printf '%s' "$detail" | sed 's/%/%25/g' | sed ':a;N;$!ba;s/\n/%0A/g')
  printf '::error::%s rc=%s: %s\n' "$what" "$rc" "$(printf '%.2000s' "$detail")"
}

fail=0
for mode in "--jit" "--vm"; do
  echo "=== $exe $mode $script ==="
  out=$(timeout 60 "$exe" $mode "$script" 2>&1); rc=$?
  echo "out=[$out] rc=$rc"
  [ "$out" = "$expected" ] && continue
  case $mode in
    --vm*)
      # Out-of-slice rejects share one contract ("--vm: unsupported: ...",
      # Compiler::reject + main.cc's multi-module case); match that prefix,
      # not the VmError kind, so a runtime VmError still FAILs loudly.
      if [ "$rc" -ne 0 ] && [[ "$out" == *'--vm: unsupported:'* ]]; then
        echo "${label}_${mode}_SKIP"
        continue
      fi ;;
  esac
  note_failure "${label}_${mode}_FAIL" "$rc" "$out"; fail=1
done

echo "=== $exe build $script ==="
aot=./aot_$label.exe
build_out=$("$exe" build "$script" -o "$aot" 2>&1); build_rc=$?
printf '%s\n' "$build_out"
if [ "$build_rc" -eq 0 ]; then
  # An AOT binary that will not start prints nothing, so the file itself and
  # what it imports are the report — without them "empty output" cannot be
  # told from "the assertions failed".
  ls -l "$aot"
  imports=""
  if [ "${OS:-}" = "Windows_NT" ] && command -v objdump > /dev/null 2>&1; then
    imports=$(objdump -p "$aot" 2>/dev/null | sed -n 's/.*DLL Name:[[:space:]]*//p' \
      | sort -u | tr '\n' ' ')
    echo "imports=[$imports]"
  fi
  out=$(timeout 60 "$aot" 2>&1); rc=$?
  echo "out=[$out] rc=$rc"
  if [ "$out" = "$expected" ]; then
    echo "${label}_AOT_PASS"
  else
    detail="$out${imports:+ | imports: $imports}"
    # A binary that produced nothing has not said whether it ran. Ask the two
    # questions its silence leaves open: how big it is (a link that emitted a
    # stub answers here), and what Windows itself makes of it — cmd.exe
    # reports the process's own exit status, where the msys layer reports its
    # spawn's, and 127 from one with 0 from the other is a launch that never
    # happened rather than a program that failed.
    if [ "${OS:-}" = "Windows_NT" ]; then
      native=$(cmd //c "$(basename "$aot")" 2>&1); native_rc=$?
      echo "native_out=[$native] native_rc=$native_rc"
      detail="$detail | size: $(wc -c < "$aot" 2>/dev/null) | native rc=$native_rc out=[$native]"
    fi
    note_failure "${label}_AOT_RUN_FAIL" "$rc" "$detail"
    fail=1
  fi
else
  note_failure "${label}_AOT_BUILD_FAIL" "$build_rc" "$build_out"
  fail=1
fi
exit $fail
