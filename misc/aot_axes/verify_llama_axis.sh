#!/usr/bin/env bash
# Assert a build really has the Llama namespace in it, and its GPU backend:
# Metal on macOS, Vulkan elsewhere (its loader opened at run time, not linked).
#
# Usage: misc/aot_axes/verify_llama_axis.sh <build dir>
#
# The sibling of verify_scene_axis.sh: a lane that lost
# cmake/release_full.cmake still builds green, without Llama.
set -eu

build=${1:?usage: verify_llama_axis.sh <build dir>}
[ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "

if [ ! -f "$build/libculebra_rt_llama.a" ]; then
  echo "${prefix}built without Llama (cmake/release_full.cmake, and not DEV_NO_RT)" >&2
  grep -i llama "$build/CMakeCache.txt" >&2 || true
  exit 1
fi
gpu=vulkan
[ "$(uname -s)" = Darwin ] && gpu=metal
if [ ! -f "$build/libculebra_llama_gpu_$gpu.a" ]; then
  echo "${prefix}Llama was built without its $gpu backend" >&2
  grep -i "$gpu" "$build/CMakeCache.txt" >&2 || true
  exit 1
fi
if [ "$gpu" = vulkan ]; then
  bash "$(dirname "$0")/../../tools/checks/check_llama_vulkan_dynload.sh" "$build"
fi
echo "OK: Llama axis built"
