#!/usr/bin/env bash
# Package one release build into the archive GitHub Releases serves.
#
# Usage: misc/package_release.sh <binary> <tag> <os> <arch>
#   <binary>  path to the built culebra (or culebra.exe)
#   <tag>     the release tag, e.g. v0.1.0
#   <os>      macos | linux | windows
#   <arch>    arm64 | x64
#
# Single-sources the naming convention and the checksum across the release
# workflow's per-platform jobs, which otherwise cannot share steps (each needs
# a different shell and toolchain). Prints the files it wrote, one per line, so
# the caller can hand them straight to `gh release upload`.
#
# Windows gets .zip (what Explorer opens without a detour), everything else
# .tar.gz. Both hold a single top-level directory so extracting never scatters
# files into the user's cwd.
set -euo pipefail

if [ $# -ne 4 ]; then
  echo "usage: package_release.sh <binary> <tag> <os> <arch>" >&2
  exit 2
fi

binary=$1 tag=$2 os=$3 arch=$4

[ -x "$binary" ] || { echo "package_release: not executable: $binary" >&2; exit 1; }

# The binary self-reports the version it was compiled with (include/culebra.h);
# the tag is what the release claims. A mismatch means the version bump missed
# the header, which would otherwise ship silently — fail here instead.
want=${tag#v}
got=$("$binary" --version | awk '{print $2}')
if [ "$want" != "$got" ]; then
  echo "package_release: tag says $want but the binary reports $got" >&2
  echo "  (did the release commit update CULEBRA_VERSION in include/culebra.h?)" >&2
  exit 1
fi

name="culebra-$tag-$os-$arch"
staging="$(mktemp -d)/$name"
mkdir -p "$staging"
cp "$binary" "$staging/"
cp LICENSE "$staging/"

out=$PWD
if [ "$os" = windows ]; then
  archive="$name.zip"
  (cd "$(dirname "$staging")" && zip -qr "$out/$archive" "$name")
else
  archive="$name.tar.gz"
  tar -czf "$archive" -C "$(dirname "$staging")" "$name"
fi

# Relative name in, relative name out, so the digest line names just the
# archive — `sha256sum -c` then works from wherever the user downloaded it.
if command -v sha256sum >/dev/null; then
  sha256sum "$archive" >"$archive.sha256"
else
  shasum -a 256 "$archive" >"$archive.sha256"
fi

echo "$archive"
echo "$archive.sha256"
