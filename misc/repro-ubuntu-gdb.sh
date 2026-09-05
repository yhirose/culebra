#!/usr/bin/env bash
# Stack trace for the kwargs GC_STRESS segfault. Not part of the build.
set -u
cd /src
CULEBRA_GC_STRESS=1 gdb -batch \
  -ex 'run tools/bench/vm_cases/kwargs.cul' \
  -ex 'bt 30' \
  -ex 'info registers rip rsp' \
  --args build-linux/culebra tools/bench/vm_cases/kwargs.cul 2>&1 | tail -50
