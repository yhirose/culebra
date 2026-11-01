#!/usr/bin/env bash
# Assert a build really has the Llama namespace in it, and off macOS its Vulkan
# backend.
#
# Usage: misc/aot_axes/verify_llama_axis.sh <build dir>
#
# The sibling of verify_scene_axis.sh, for the same reason: a lane that lost
# cmake/release_full.cmake builds green without Llama, and one without the
# Vulkan tools would build it CPU-only if the option were AUTO. The archives
# beside the driver are the answer: the feature archive, and the GPU half the
# driver links (culebra_llama_gpu_vulkan exists only when ggml-vulkan was built).
set -eu

build=${1:?usage: verify_llama_axis.sh <build dir>}
[ -n "${GITHUB_ACTIONS:-}" ] && prefix="::error::" || prefix="ERROR: "

if [ ! -f "$build/libculebra_rt_llama.a" ]; then
  echo "${prefix}built without Llama (cmake/release_full.cmake, and not DEV_NO_RT)" >&2
  grep -i llama "$build/CMakeCache.txt" >&2 || true
  exit 1
fi
if [ "$(uname -s)" != Darwin ] && [ ! -f "$build/libculebra_llama_gpu_vulkan.a" ]; then
  echo "${prefix}Llama was built without its Vulkan backend" >&2
  grep -i vulkan "$build/CMakeCache.txt" >&2 || true
  exit 1
fi
echo "OK: Llama axis built"
