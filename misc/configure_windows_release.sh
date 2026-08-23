#!/usr/bin/env bash
# Configure the Windows download's build.
#
# Usage: misc/configure_windows_release.sh <build dir> [extra cmake args…]
#
# Single-sourced because release.yml and ci.yml both build this configuration
# and a disagreement between them is invisible until a tag is pushed — which is
# how v0.1.0 died. release.yml alone had CULEBRA_LTO=ON, every ci.yml Windows
# job had it OFF, so the combination had never been linked: UCRT64's gcc cannot
# LTO-link its own prebuilt libstdc++.a, and ld refused the release the first
# time anyone asked for it. The flag now has one home, and
# tools/check_release_covered_by_ci.sh sees this script in both workflows.
#
# The Canvas window is deliberately NOT spelled here. CMake defaults it ON, and
# both callers assert that with misc/verify_canvas_window.sh — passing the
# option would make that assertion unfalsifiable.
#
# mingw-w64/UCRT64, not MSVC: the Value type is used through std::vector<Value>
# while incomplete, which libstdc++ accepts and the MSVC STL rejects. Static
# libgcc/libstdc++ so the download needs no runtime DLLs beside it.
set -eu

build=${1:?usage: configure_windows_release.sh <build dir> [extra cmake args…]}
shift

cmake -S . -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++" \
  -DCULEBRA_ENABLE_JIT=ON \
  -DCULEBRA_ENABLE_HTTP=ON \
  -DCULEBRA_ENABLE_WEBVIEW=ON \
  -DCULEBRA_DEV_NO_RT=OFF \
  -DCULEBRA_LTO=OFF \
  "$@"
