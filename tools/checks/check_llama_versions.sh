#!/usr/bin/env bash
# The Llama namespace is built against one llama.cpp release, and the wrapper
# that drives it is tagged with the release it was tested against.
#
# cmake/llama_cpp.cmake names the release (and the hash of its source archive);
# vendor/cpp-llamalib is checked out at a tag, and its own CMakeLists.txt names
# the llama.cpp it fetches. The three have to be the same release, or the
# wrapper is compiled against a llama.h it was never run with. Moving on means
# changing the version and hash in cmake/llama_cpp.cmake and the submodule in one
# commit; this check says which of them is behind.
set -euo pipefail
cd "$(dirname "$0")/../.."

cfg=cmake/llama_cpp.cmake
lib=vendor/cpp-llamalib

version=$(sed -n 's/^set(CULEBRA_LLAMA_CPP_VERSION \(v[0-9][0-9.]*\))$/\1/p' "$cfg")
hash=$(tr -d '[:space:]' < "$cfg" | sed -n 's/.*CULEBRA_LLAMA_CPP_SHA256\([0-9a-f]\{64\}\))$/\1/p')

# A check that reads nothing checks nothing.
if [[ -z "$version" ]]; then
  echo "llama-versions FAIL: no CULEBRA_LLAMA_CPP_VERSION (vN.N.N) in $cfg" >&2
  exit 1
fi
if [[ -z "$hash" ]]; then
  echo "llama-versions FAIL: no 64 hex digit CULEBRA_LLAMA_CPP_SHA256 in $cfg" >&2
  exit 1
fi
if [[ ! -e "$lib/.git" ]]; then
  echo "llama-versions FAIL: $lib is not checked out (git submodule update --init $lib)" >&2
  exit 1
fi

tag=$(git -C "$lib" describe --tags --exact-match 2>/dev/null || true)
if [[ -z "$tag" ]]; then
  at=$(git -C "$lib" describe --tags --always 2>/dev/null || echo unknown)
  echo "llama-versions FAIL: $lib is at $at, not at a release tag" >&2
  echo "  It is pinned to the tag named for the llama.cpp release: $version." >&2
  exit 1
fi
if [[ "$tag" != "$version" ]]; then
  echo "llama-versions FAIL: $lib is at $tag, but $cfg builds llama.cpp $version" >&2
  exit 1
fi

# The tag claims the llama.cpp release it was tested against; its own build
# file is where it fetches that release from.
fetched=$(sed -n '0,/GIT_TAG/{s/^[[:space:]]*GIT_TAG[[:space:]]\{1,\}\([^[:space:]]*\).*/\1/p}' "$lib/CMakeLists.txt")
if [[ "$fetched" != "$version" ]]; then
  echo "llama-versions FAIL: $lib/CMakeLists.txt fetches llama.cpp ${fetched:-nothing}, not $version" >&2
  exit 1
fi

echo "llama-versions OK ($version: cmake/llama_cpp.cmake, $lib $tag, hash ${hash:0:12})"
