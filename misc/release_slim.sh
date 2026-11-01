#!/usr/bin/env bash
# Build, check and package the slim download (CULEBRA_SLIM) for this platform:
# what release.yml ships and ci.yml checks, from one place.
#
# Usage: misc/release_slim.sh <os> <arch> [tag]
#   <os> <arch>  macos arm64 | linux x64 | windows x64
#   [tag]        the release tag; omitted, a dry run of this checkout
#
# Prints the files it wrote, one per line, as package_release.sh does; the
# build and the checks report on stderr.
set -euo pipefail
cd "$(dirname "$0")/.."

os=${1:?usage: release_slim.sh <os> <arch> [tag]}
arch=${2:?usage: release_slim.sh <os> <arch> [tag]}
tag=${3:--}

{
  case "$os" in
    windows)
      # The Windows download's own configuration, made slim.
      misc/windows_toolchain/configure_windows_release.sh build-slim -DCULEBRA_SLIM=ON
      cmake --build build-slim --target culebra -j
      bin=build-slim/culebra.exe
      misc/windows_toolchain/verify_standalone_exe.sh "$bin"
      ;;
    macos | linux)
      cmake -S . -B build-slim -DCMAKE_BUILD_TYPE=Release -DCULEBRA_SLIM=ON
      cmake --build build-slim --target culebra \
        -j"${CULEBRA_BUILD_JOBS:-$(misc/jobs_within.sh 2000000000)}"
      bin=build-slim/culebra
      if [ "$os" = macos ]; then misc/verify_standalone_macho.sh "$bin"; fi
      ;;
    *)
      echo "release_slim: unknown os '$os'" >&2
      exit 2
      ;;
  esac
  misc/test_vm_only.sh "$bin"
} >&2

misc/package_release.sh "$bin" "$tag" "$os" "$arch" slim
