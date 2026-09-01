#!/usr/bin/env bash
# Prove a Windows build carries its own linker.
#
# Usage: misc/windows_toolchain/verify_inprocess_lld.sh <culebra.exe> [--size]
#
# lld is found by library at configure time, so an LLVM install without the lld
# archives configures and builds perfectly well and produces a binary that
# silently falls back to needing clang++ on PATH. Shipping that alongside a
# toolchain kit would leave the kit useless and the download unable to say why —
# the kit holds libraries only.
#
# Single-sourced across the CI job and the release build for the same reason
# verify_standalone_exe.sh is: a check spelled inline in one workflow is
# invisible to tools/checks/check_release_covered_by_ci.sh, so the release gate could
# drift weaker than the CI one by omission. That is the failure v0.1.0 and
# v0.3.0 both shipped.
#
# --size also prints the driver's byte count. Carrying lld forces every LLVM
# backend in (its COFF driver references each one), which every Windows download
# pays for whether or not it ever runs `culebra build`. That trade is only
# defensible against a number, so CI prints the number.
set -eu

exe=${1:?usage: verify_inprocess_lld.sh <exe> [--size]}
want_size=${2:-}

# `toolchain status` exits non-zero when no kit is installed, which is the
# normal state on a build machine — so its text is read, not its exit code, and
# neither `set -e` nor pipefail gets to decide the outcome.
out=$("$exe" toolchain status || true)
printf '%s\n' "$out"

case "$out" in
  *"lld, carried in this binary"*)
    echo "OK: this build can link without a toolchain on the machine" ;;
  *)
    echo "ERROR: this culebra.exe would need an external clang++ — a" >&2
    echo "  toolchain kit cannot help it (were the lld libraries missing" >&2
    echo "  at configure time?)" >&2
    exit 1 ;;
esac

if [ "$want_size" = "--size" ]; then
  printf 'driver: %s bytes\n' "$(stat -c %s "$exe")"
fi
