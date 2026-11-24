#!/usr/bin/env bash
# Build the driver inside this directory's Dockerfile image, with ci.yml's
# Linux toolchain (TARGET=all for every target CI builds), and copy it to
# build-linux/culebra. The image's WINDOW decides the window and Scene. Not
# part of the build; a reproduction aid.
set -euo pipefail
cd /src
# Not under /src: on Docker Desktop's bind mount make misreads build files
# CMake has just rewritten ("No rule to make target .../depend", a different
# target each run). Mount a volume here, one per checkout, to keep it.
build=/build
mkdir -p "$build"

# Passed, not left to option(): a cached option keeps the image that first
# configured the build dir.
window=${CULEBRA_CANVAS_WINDOW_DEFAULT:-OFF}
# CULEBRA_ENABLE_JIT defaults to OFF, and a driver without it reads `--jit`
# as a file name.
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release \
      -DCULEBRA_ENABLE_JIT=ON \
      -DCULEBRA_ENABLE_CANVAS_WINDOW="$window" \
      -DCULEBRA_ENABLE_SCENE="$window" > "$build/configure.log" 2>&1 || {
  tail -25 "$build/configure.log"
  exit 1
}
echo CONFIGURED

# Jobs at a runtime TU's peak (~4.4 GB on arm64): four at `just build`'s
# 2 GB each got OOM-killed in Docker Desktop's default 8 GB VM.
jobs=${CULEBRA_BUILD_JOBS:-$(misc/jobs_within.sh 4500000000)}
cmake --build "$build" -j"$jobs" --target "${TARGET:-culebra}" \
      > "$build/build.log" 2>&1 || {
  grep -m 15 -E "error:" "$build/build.log" || tail -15 "$build/build.log"
  exit 1
}
mkdir -p build-linux
cp "$build/culebra" build-linux/culebra
echo BUILT
ls -la build-linux/culebra

# What a machine this binary is carried to must have.
needed=$(objdump -p build-linux/culebra | awk '/NEEDED/ { print $2 }')
echo "needs" $needed
for lib in GLIBC GLIBCXX; do
  v=$(objdump -T build-linux/culebra | grep -o "${lib}_[0-9.]*" |
      sed "s/^${lib}_//" | sort -uV | tail -1 || true)
  echo "needs $lib >= ${v:-(none)}"
done

if [ "$window" = ON ]; then
  misc/aot_axes/verify_canvas_window.sh "$build"
  misc/aot_axes/verify_scene_axis.sh "$build"
  # SDL3 reaches these through dlopen; linking one would stop a headless start.
  if grep -iE 'libX11|libGL|libEGL|libwayland|libSDL|libasound|libpulse' <<< "$needed"; then
    echo "ERROR: the driver links a display or audio library" >&2
    exit 1
  fi
  # SDL3 drops Wayland silently when its pkg-config or wayland-scanner is
  # missing. nm is read once: `nm | grep -q` fails under pipefail.
  syms=$(nm build-linux/culebra)
  for backend in X11 Wayland; do
    grep -q " ${backend}_bootstrap$" <<< "$syms" ||
      { echo "ERROR: SDL3 built without its $backend backend" >&2; exit 1; }
  done
  echo "OK: SDL3 has its X11 and Wayland backends"
fi
