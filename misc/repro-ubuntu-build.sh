#!/usr/bin/env bash
# Build inside misc/repro-ubuntu.dockerfile's image, the way ci.yml's Linux
# jobs do. Not part of the build; a reproduction aid.
set -euo pipefail
cd /src

# CULEBRA_ENABLE_JIT defaults to OFF (CMakeLists.txt:7), and a driver built
# without it reads `--jit` as a file name rather than refusing it — so a run
# that meant to exercise the JIT lane quietly exercises the executor twice.
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
      -DCULEBRA_ENABLE_JIT=ON \
      -DCULEBRA_CANVAS_WINDOW_DEFAULT=OFF > /tmp/cfg.log 2>&1 || {
  tail -25 /tmp/cfg.log
  exit 1
}
echo CONFIGURED

# Four, not the host's core count: the JIT translation units are
# LLVM-header-heavy and Docker Desktop's VM defaults to ~8 GB, where -j20
# gets cc1plus OOM-killed ("fatal error: Killed signal terminated program").
cmake --build build-linux -j"${JOBS:-4}" > /tmp/build.log 2>&1 || {
  grep -E "error:" /tmp/build.log | head -15
  exit 1
}
echo BUILT
ls -la build-linux/culebra
