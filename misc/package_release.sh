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
#
# The archive filename carries no version, which is what lets README link to
#   .../releases/latest/download/culebra-<os>-<arch>.tar.gz
# and have it keep working across releases. The directory inside does carry it,
# so an extracted copy still says which release it came from — and the binary
# itself answers definitively with `culebra --version`.
set -euo pipefail

if [ $# -ne 4 ]; then
  echo "usage: package_release.sh <binary> <tag> <os> <arch>" >&2
  exit 2
fi

binary=$1 tag=$2 os=$3 arch=$4

[ -x "$binary" ] || { echo "package_release: not executable: $binary" >&2; exit 1; }

dir="culebra-$tag-$os-$arch"   # inside the archive: says which release
stem="culebra-$os-$arch"       # the archive itself: stable across releases
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT  # the staged copy is ~100 MB; don't leak it locally
staging="$tmp/$dir"
staged="$staging/$(basename "$binary")"
mkdir -p "$staging"
cp "$binary" "$staged"
cp LICENSE "$staging/"

# Strip the staged copy, not the build tree's, which the gate and local
# debugging still want: -Wl,-x dropped the locals at link, and the global
# .symtab left over is ~9 MB no downloader can use (it also takes 2.5 MB off
# the tarball). Everything below then checks the staged copy, since these are
# no longer the bytes CI tested.
if command -v strip >/dev/null; then
  strip "$staged"
fi

# The binary self-reports the version it was compiled with (include/culebra.h);
# the tag is what the release claims. A mismatch means the version bump missed
# the header, which would otherwise ship silently — fail here instead.
want=${tag#v}
got=$("$staged" --version | awk '{print $2}')
if [ "$want" != "$got" ]; then
  echo "package_release: tag says $want but the binary reports $got" >&2
  echo "  (did the release commit update CULEBRA_VERSION in include/culebra.h?)" >&2
  exit 1
fi

# Both backends: the interpreter needs no symbols, but the JIT resolves its
# helpers by name, so a strip that reached too far shows up here. Each run is
# ~10 ms. `|| said=` keeps a crash reportable — under `set -e` a failing
# substitution would otherwise end the script before the message.
printf 'print(6 * 7)\n' >"$tmp/smoke.cul"
for lane in interp jit; do
  [ "$lane" = jit ] && flag=--jit || flag=
  said=$("$staged" $flag "$tmp/smoke.cul") || said="exited $?"
  if [ "$said" != "42" ]; then
    echo "package_release: the stripped binary failed the $lane smoke" >&2
    echo "  (expected 42, got '$said')" >&2
    exit 1
  fi
done

out=$PWD
if [ "$os" = windows ]; then
  archive="$stem.zip"
  (cd "$tmp" && zip -qr "$out/$archive" "$dir")
else
  archive="$stem.tar.gz"
  tar -czf "$archive" -C "$tmp" "$dir"
fi

# Relative name in, relative name out, so the digest line names just the
# archive — `sha256sum -c` then works from wherever the user downloaded it.
#
# Write the line rather than take the tool's: coreutils under MSYS2 reads in
# binary mode and marks it ` *name`, while macOS shasum writes `  name`, so
# SHA256SUMS came out of v0.1.0 with the Windows row spelled differently from
# the other two. Only the digest is taken from the tool, so the bytes hashed
# are still whatever it reads natively — passing --text to even out the marker
# would instead change the read mode, and on a platform where that means CRLF
# translation it would hash something other than the archive.
if command -v sha256sum >/dev/null; then
  sum() { sha256sum "$@"; }
else
  sum() { shasum -a 256 "$@"; }
fi
printf '%s  %s\n' "$(sum "$archive" | cut -d' ' -f1)" "$archive" >"$archive.sha256"

# Prove the line we just wrote verifies here, on the platform that produced it.
# The check is the point: it is what says the one spelling is readable
# everywhere, instead of leaving it to be discovered by whoever downloads.
sum -c "$archive.sha256" >/dev/null || {
  echo "package_release: the .sha256 we wrote does not verify" >&2
  exit 1
}

echo "$archive"
echo "$archive.sha256"
