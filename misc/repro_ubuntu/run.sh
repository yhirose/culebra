#!/usr/bin/env bash
# Reproduce (and then re-check) the Linux-only CI failures inside the
# container image. Not part of the build; a reproduction aid.
set -u
cd /src
BIN=build-linux/culebra

run() {   # run <label> <expected rc> <args...>
  local label=$1 want=$2
  shift 2
  "$@" > /tmp/out.txt 2>&1
  local rc=$?
  if [ "$rc" -eq "$want" ]; then
    echo "OK   $label (rc=$rc)"
  else
    echo "FAIL $label (rc=$rc, wanted $want)"
    tail -5 /tmp/out.txt
  fi
}

run "kwargs --vm  GC_STRESS" 0 env CULEBRA_GC_STRESS=1 "$BIN" tools/bench/vm_cases/kwargs.cul
run "kwargs --jit GC_STRESS" 0 env CULEBRA_GC_STRESS=1 "$BIN" --jit tools/bench/vm_cases/kwargs.cul
run "test_class --vm"        0 "$BIN" tests/test_class.cul
run "test_class --jit"       0 "$BIN" --jit tests/test_class.cul
run "test_typed_fields --vm"  0 "$BIN" tests/test_typed_fields.cul
run "test_typed_fields --jit" 0 "$BIN" --jit tests/test_typed_fields.cul
run "test_param_tag --vm"     0 "$BIN" tests/test_param_tag.cul
run "test_param_tag --jit"    0 "$BIN" --jit tests/test_param_tag.cul
