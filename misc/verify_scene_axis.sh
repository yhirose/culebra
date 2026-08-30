#!/usr/bin/env bash
# Assert a build really has the Scene namespace in it.
#
# Usage: misc/verify_scene_axis.sh <build dir>
#
# The sibling of verify_webview_axis.sh and verify_canvas_window.sh, and needed
# for the same reason: a lane that forgot the option, or a deps-cache miss,
# builds green and every later check passes on a binary with no `Scene` name in
# it. The feature archive beside the driver is the honest answer — a build that
# skips the AOT archives (DEV_NO_RT) has no Scene archive to find either way.
set -eu

build=${1:?usage: verify_scene_axis.sh <build dir>}

if [ ! -f "$build/libculebra_rt_scene.a" ]; then
  [ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "
  echo "${prefix}built without Scene (-DCULEBRA_ENABLE_SCENE=ON, and not DEV_NO_RT)" >&2
  grep -i scene "$build/CMakeCache.txt" >&2 || true
  exit 1
fi
echo "OK: Scene axis built"
