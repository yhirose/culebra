#!/bin/bash
# Prints the directory to pass as -DLLVM_DIR for the prebuilt static LLVM
# pinned in misc/prebuilt_llvm.env, fetching it first when this machine does
# not have it yet. Progress goes to stderr; stdout is the path alone, so
# `-DLLVM_DIR=$(misc/prebuilt_llvm.sh)` works.
#
# Why: Homebrew's llvm bottle compiles its static libraries at -O0, which
# makes the JIT's own optimization and code generation about 1.6x slower
# than with these (https://github.com/yhirose/culebra-llvm).
#
# The cache is shared by every worktree: CULEBRA_LLVM_CACHE names another
# directory. An unpacked release is final once its marker is written, so a
# second run, or a second worktree, only reads it.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
. "$here/prebuilt_llvm.env"

if [[ "$(uname -s)-$(uname -m)" != "Darwin-arm64" ]]; then
  echo "prebuilt llvm: only macOS arm64 has one so far" >&2
  exit 1
fi

root="${CULEBRA_LLVM_CACHE:-${HOME:?}/Library/Caches/culebra/llvm}"
dest="$root/$PREBUILT_LLVM_TAG"
marker="$dest/.sha256"

if [[ "$(cat "$marker" 2>/dev/null)" != "$PREBUILT_LLVM_SHA256" ]]; then
  mkdir -p "$root"
  # One fetcher at a time: worktrees start builds together.
  lock="$root/.lock-$PREBUILT_LLVM_TAG"
  until mkdir "$lock" 2>/dev/null; do sleep 1; done
  tmp=$(mktemp -d "$root/.fetch.XXXXXX")
  trap 'rmdir "$lock" 2>/dev/null || true; rm -rf "$tmp"' EXIT

  if [[ "$(cat "$marker" 2>/dev/null)" != "$PREBUILT_LLVM_SHA256" ]]; then
    url="https://github.com/yhirose/culebra-llvm/releases/download/$PREBUILT_LLVM_TAG/$PREBUILT_LLVM_ASSET"
    echo "prebuilt llvm: fetching $url" >&2
    curl -fL --retry 3 -sS -o "$tmp/asset" "$url"
    echo "$PREBUILT_LLVM_SHA256  $tmp/asset" | shasum -a 256 -c - >&2
    mkdir "$tmp/prefix"
    zstd -dc "$tmp/asset" | tar -x -C "$tmp/prefix"
    echo "$PREBUILT_LLVM_SHA256" > "$tmp/prefix/.sha256"
    rm -rf "$dest"
    mv "$tmp/prefix" "$dest"
  fi
fi

echo "$dest/lib/cmake/llvm"
