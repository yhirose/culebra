#!/usr/bin/env bash
# Which commit build/culebra was built from, so that `just install` can tell it
# has nothing to build.
#
#   built_from.sh record    after a build: note HEAD, if the tree is clean
#   built_from.sh current   exit 0 when build/culebra is this clean tree's HEAD
#
# A build of a tree with changes notes nothing, and `just build` takes the note
# away before it compiles, so a note never stands for another binary.
set -euo pipefail
cd "$(dirname "$0")/.."

clean() { [ -z "$(git status --porcelain 2>/dev/null || echo unknown)" ]; }

case "${1:-}" in
  record)
    if clean; then git rev-parse HEAD >build/built-from; fi
    ;;
  current)
    [ -x build/culebra ] && clean &&
      [ "$(cat build/built-from 2>/dev/null)" = "$(git rev-parse HEAD)" ]
    ;;
  *)
    echo "usage: built_from.sh record|current" >&2
    exit 2
    ;;
esac
