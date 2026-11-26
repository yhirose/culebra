#!/bin/bash
# The prebuilt static LLVM for this machine, pinned in misc/prebuilt_llvm.env,
# fetched first when this machine does not have it yet.
#
#   misc/prebuilt_llvm.sh          the directory to pass as -DLLVM_DIR; fails
#                                  when this platform has no prebuilt
#   misc/prebuilt_llvm.sh --flag   `-DLLVM_DIR=<that directory>`, or nothing at
#                                  all when this platform has none (the
#                                  justfile's one question about platforms)
#
# Progress goes to stderr; stdout is the answer alone.
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

flag=0
[[ "${1:-}" == --flag ]] && flag=1

os=$(uname -s | tr '[:upper:]' '[:lower:]')
case "$os" in mingw*|msys*|cygwin*) os=windows ;; esac
key="${os}_$(uname -m | tr '[:upper:]' '[:lower:]')"

tag_var="PREBUILT_LLVM_${key}_TAG"
asset_var="PREBUILT_LLVM_${key}_ASSET"
sha_var="PREBUILT_LLVM_${key}_SHA256"
if [[ -z "${!tag_var:-}" ]]; then
  if (( flag )); then exit 0; fi
  echo "prebuilt llvm: none for $key yet" >&2
  exit 1
fi
tag=${!tag_var}
asset=${!asset_var:?$asset_var is missing in prebuilt_llvm.env}
sha=${!sha_var:?$sha_var is missing in prebuilt_llvm.env}

case "$os" in
  darwin) default_root="${HOME:?}/Library/Caches/culebra/llvm" ;;
  windows) default_root="${LOCALAPPDATA:-${HOME:?}/AppData/Local}/culebra/llvm" ;;
  *) default_root="${XDG_CACHE_HOME:-${HOME:?}/.cache}/culebra/llvm" ;;
esac
root="${CULEBRA_LLVM_CACHE:-$default_root}"
dest="$root/$tag-$key"
marker="$dest/.sha256"

if [[ "$(cat "$marker" 2>/dev/null)" != "$sha" ]]; then
  mkdir -p "$root"
  # One fetcher at a time: worktrees start builds together.
  lock="$root/.lock-$tag-$key"
  until mkdir "$lock" 2>/dev/null; do sleep 1; done
  tmp=$(mktemp -d "$root/.fetch.XXXXXX")
  trap 'rmdir "$lock" 2>/dev/null || true; rm -rf "$tmp"' EXIT

  if [[ "$(cat "$marker" 2>/dev/null)" != "$sha" ]]; then
    url="https://github.com/yhirose/culebra-llvm/releases/download/$tag/$asset"
    echo "prebuilt llvm: fetching $url" >&2
    curl -fL --retry 3 -sS -o "$tmp/asset" "$url"
    if command -v sha256sum >/dev/null; then
      echo "$sha  $tmp/asset" | sha256sum -c - >&2
    else
      echo "$sha  $tmp/asset" | shasum -a 256 -c - >&2
    fi
    mkdir "$tmp/prefix"
    zstd -dc "$tmp/asset" | tar -x -C "$tmp/prefix"
    echo "$sha" > "$tmp/prefix/.sha256"
    rm -rf "$dest"
    mv "$tmp/prefix" "$dest"
  fi
fi

dir="$dest/lib/cmake/llvm"
if (( flag )); then echo "-DLLVM_DIR=$dir"; else echo "$dir"; fi
