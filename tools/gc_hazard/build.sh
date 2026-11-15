#!/usr/bin/env bash
# Build gc_hazard into build-gc-hazard/ against the clang libraries of the
# LLVM that $LLVM_CONFIG (or Homebrew's llvm) names. Prints the binary's path.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/gc_hazard.cc"
mkdir -p "$(dirname "$0")/../../build-gc-hazard"
cd "$(dirname "$0")/../../build-gc-hazard"
if [[ -z "${LLVM_CONFIG:-}" ]] && command -v brew >/dev/null; then
  LLVM_CONFIG="$(brew --prefix llvm)/bin/llvm-config"
fi
install="install the clang libraries (macOS: brew install llvm; Ubuntu:"\
" libclang-<N>-dev libclang-cpp<N>-dev)"
if [[ -z "${LLVM_CONFIG:-}" || ! -x "$LLVM_CONFIG" ]]; then
  echo "gc_hazard: no llvm-config; set LLVM_CONFIG, or $install" >&2
  exit 1
fi
bindir=$("$LLVM_CONFIG" --bindir)
libdir=$("$LLVM_CONFIG" --libdir)
inc=$("$LLVM_CONFIG" --includedir)
if [[ ! -f "$inc/clang/Tooling/Tooling.h" ]]; then
  echo "gc_hazard: $inc has no clang headers; $install" >&2
  exit 1
fi
CXX="$bindir/clang++"
[[ -x "$CXX" ]] || CXX=c++
out=gc_hazard
# Rebuild only when the source, this script or the LLVM changed.
stamp="$LLVM_CONFIG $("$LLVM_CONFIG" --version) $(cksum < "$src") $(cksum < "$here/build.sh")"
if [[ -x $out && -f $out.stamp && "$(cat $out.stamp)" == "$stamp" ]]; then
  echo "$PWD/$out"
  exit 0
fi
resource=$("$bindir/clang" -print-resource-dir 2>/dev/null || true)
[[ -n "$resource" ]] || resource="$libdir/clang/$("$LLVM_CONFIG" --version | cut -d. -f1)"
"$CXX" -std=c++17 -O1 -fno-rtti $("$LLVM_CONFIG" --cxxflags) \
  -DGC_HAZARD_RESOURCE_DIR="\"$resource\"" "$src" -o $out \
  $("$LLVM_CONFIG" --ldflags) -lclang-cpp \
  $("$LLVM_CONFIG" --link-shared --libs) -Wl,-rpath,"$libdir" >&2
echo "$stamp" > $out.stamp
echo "$PWD/$out"
