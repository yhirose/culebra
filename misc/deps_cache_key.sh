#!/usr/bin/env bash
# The actions/cache key for the vendored SDL3 + raylib tree.
#
# Usage: misc/deps_cache_key.sh <prefix>     # e.g. windows-deps
#
# CMake keys its own deps directory on these two submodule revisions
# (_culebra_dep_rev in CMakeLists.txt), so anything coarser here would hit on a
# vendor bump, restore a tree CMake then ignores, and — because an exact hit
# skips the save — leave every later run rebuilding. Single-sourced because the
# CI job that publishes the tree and the release job that restores it must
# produce byte-identical keys, and nothing would fail if they drifted: the
# release would just quietly go back to a cold ~4 min build.
set -eu

prefix=${1:?usage: deps_cache_key.sh <prefix>}

echo "$prefix-$(git -C vendor/SDL rev-parse --short=12 HEAD)-$(git -C vendor/raylib rev-parse --short=12 HEAD)"
