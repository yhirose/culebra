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
#
# UCRT64's clang, not its gcc: same libstdc++, libgcc and C runtime (so the
# gcc-built LLVM statics link unchanged), but PE codegen that `culebra build`
# can dead-strip. GCC emits each function's .pdata$fn/.xdata$fn as a plain
# COMDAT that GNU ld never garbage-collects, and puts switch jump tables in one
# shared .rdata; with the runtime archive built that way an AOT hello kept all
# 2,710 archive functions and weighed 6.1 MB. clang ties both to the function
# (associative COMDATs) and lld — which src/main.cc links with — drops them
# with it.
#
# The driver links with lld as well, and not by choice: GNU ld refuses clang's
# objects against the gcc-built libstdc++.a ("duplicate section
# `.rdata$_ZTSSt9exception' has different size" — the typeinfo COMDATs are
# padded differently), the same refusal that met v0.1.0's LTO link. lld takes
# the first definition of an Any-selection COMDAT and moves on.
set -eu

build=${1:?usage: configure_windows_release.sh <build dir> [extra cmake args…]}
shift

cmake -S . -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -static -static-libgcc -static-libstdc++" \
  -DCULEBRA_ENABLE_JIT=ON \
  -DCULEBRA_ENABLE_HTTP=ON \
  -DCULEBRA_ENABLE_WEBVIEW=ON \
  -DCULEBRA_DEV_NO_RT=OFF \
  -DCULEBRA_LTO=OFF \
  "$@"
