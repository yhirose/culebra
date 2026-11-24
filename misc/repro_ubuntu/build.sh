#!/usr/bin/env bash
# Build inside this directory's Dockerfile image, the way ci.yml's Linux
# jobs do. Not part of the build; a reproduction aid.
#
# The image decides the window: built with `--build-arg WINDOW=ON` it carries
# SDL3's build dependencies and CULEBRA_CANVAS_WINDOW_DEFAULT=ON, and this
# builds the Canvas window and Scene as a release does. On an Apple Silicon
# Mac the image and the build are native arm64.
#
# The driver alone by default (it pulls in the runtime archives it embeds);
# TARGET=all adds smoke_suite and the C++ test executables, which CI builds
# too. Both logs stay in build-linux/, outliving a `docker run --rm`.
set -euo pipefail
cd /src
mkdir -p build-linux

# CULEBRA_ENABLE_JIT defaults to OFF (CMakeLists.txt:7), and a driver built
# without it reads `--jit` as a file name rather than refusing it — so a run
# that meant to exercise the JIT lane quietly exercises the executor twice.
# The window is passed, not left to its default: build-linux/ outlives the
# container, and option() never rewrites a cached value, so a tree first
# configured by the other image would keep that image's answer.
window=${CULEBRA_CANVAS_WINDOW_DEFAULT:-OFF}
cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Release \
      -DCULEBRA_ENABLE_JIT=ON \
      -DCULEBRA_ENABLE_CANVAS_WINDOW="$window" \
      -DCULEBRA_ENABLE_SCENE="$window" > build-linux/configure.log 2>&1 || {
  tail -25 build-linux/configure.log
  exit 1
}
echo CONFIGURED

# The jobs the machine's memory holds (misc/jobs_within.sh), counting each at
# a runtime translation unit's peak rather than `just build`'s 2 GB average:
# the runtime archives are each a culebra_rt.cc compile of ~4.4 GB (measured
# on arm64), they come due together, and Docker Desktop's VM defaults to
# ~8 GB, where four at once get cc1plus OOM-killed ("fatal error: Killed
# signal terminated program").
jobs=${CULEBRA_BUILD_JOBS:-$(misc/jobs_within.sh 4500000000)}
cmake --build build-linux -j"$jobs" --target "${TARGET:-culebra}" \
      > build-linux/build.log 2>&1 || {
  grep -m 15 -E "error:" build-linux/build.log || tail -15 build-linux/build.log
  exit 1
}
echo BUILT
ls -la build-linux/culebra

# What a machine needs to run it, for one this binary is carried to: the
# newest symbol versions it asks of glibc and libstdc++.
for lib in GLIBC GLIBCXX; do
  echo "needs $lib >= $(objdump -T build-linux/culebra |
    grep -o "${lib}_[0-9.]*" | sed "s/^${lib}_//" | sort -uV | tail -1)"
done

if [ "$window" = ON ]; then
  # The window's own checks (each script says what turns it off unnoticed).
  misc/aot_axes/verify_canvas_window.sh build-linux
  misc/aot_axes/verify_scene_axis.sh build-linux
  # SDL3 leaves Wayland out, without failing, when its pkg-config or
  # wayland-scanner is missing (vendor/SDL/cmake/sdlchecks.cmake, CheckWayland).
  # The symbols are read once: `nm | grep -q` under pipefail fails on the
  # SIGPIPE grep's early exit sends nm.
  syms=$(nm build-linux/culebra)
  for backend in X11 Wayland; do
    grep -q " ${backend}_bootstrap$" <<< "$syms" ||
      { echo "ERROR: SDL3 built without its $backend backend" >&2; exit 1; }
  done
  echo "OK: SDL3 has its X11 and Wayland backends"
fi
