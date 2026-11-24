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

# The tags naming the pinned commit. Which tags a checkout holds depends on how
# it was made: a shallow clone (CI's) leaves out a tag on a commit behind the
# branch head, so when the checkout holds none, the remote says.
head=$(git -C "$lib" rev-parse HEAD)
tags=$(git -C "$lib" tag --points-at HEAD)
if [[ -z "$tags" ]]; then
  if ! remote=$(git -C "$lib" ls-remote --tags origin 2>/dev/null); then
    echo "llama-versions FAIL: $lib holds no tag for ${head:0:7}, and its remote could not be asked" >&2
    exit 1
  fi
  # an annotated tag names its commit on the peeled line, refs/tags/<name>^{}
  tags=$(awk -v h="$head" '$1 == h { n = $2; sub(/^refs\/tags\//, "", n); sub(/\^\{\}$/, "", n); print n }' <<< "$remote" | sort -u)
fi
# The wrapper's tag is the release it was tested against, with a fourth number
# for a revision of the wrapper alone (v0.8.0.1 is still built on v0.8.0).
tag=""
for t in $tags; do
  if [[ "$t" == "$version"* && ${t#"$version"} =~ ^(\.[0-9]+)?$ ]]; then
    tag=$t
    break
  fi
done
if [[ -z "$tag" ]]; then
  echo "llama-versions FAIL: $lib is at ${head:0:7} (tags: $(echo ${tags:-none})), not at a release tag for llama.cpp $version" >&2
  echo "  It is pinned to the tag named for the llama.cpp release: $version" >&2
  echo "  (or $version.N for a revision of the wrapper alone)." >&2
  exit 1
fi

# The tag claims the llama.cpp release it was tested against; the wrapper's own
# script reads the release its build file fetches.
fetched=$("$lib/scripts/llama-cpp-version.sh" get || true)
if [[ "$fetched" != "$version" ]]; then
  echo "llama-versions FAIL: $lib/CMakeLists.txt fetches llama.cpp ${fetched:-nothing}, not $version" >&2
  exit 1
fi

echo "llama-versions OK ($version: cmake/llama_cpp.cmake, $lib $tag, hash ${hash:0:12})"
