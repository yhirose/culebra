#!/usr/bin/env bash
# Assert a release build really has the Canvas desktop window in it.
#
# Usage: misc/aot_axes/verify_canvas_window.sh <build dir>
#
# The window backend is the thing a download most easily loses without failing:
# an inherited CULEBRA_CANVAS_WINDOW_DEFAULT, or a deps-cache miss, turns the
# option off and the build succeeds anyway — shipping a binary that claims
# window support and cannot open one. CMake never rewrites the entry after the
# option(), so the cache is the honest answer.
set -eu

build=${1:?usage: verify_canvas_window.sh <build dir>}

if ! grep -qx 'CULEBRA_ENABLE_CANVAS_WINDOW:BOOL=ON' "$build/CMakeCache.txt"; then
  # `::error::` where a workflow is reading, so the failure lands on the job
  # summary rather than only in the log. The annotation is the only thing the
  # CI jobs used to inline a copy of this check for.
  [ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
  echo "${prefix}built without the Canvas desktop window" >&2
  grep -i canvas "$build/CMakeCache.txt" >&2 || true
  exit 1
fi
echo "OK: Canvas games open a real window"
