# The full download's settings that differ from CMake's defaults, as an initial
# cache (`cmake -C cmake/release_full.cmake`). release.yml's three full builds
# load it, and so do the ci.yml jobs that hold each download's configuration
# (linux-canvas-window, macos-canvas-window, windows-aot-build through
# configure_windows_release.sh); check_release_covered_by_ci.sh holds them to it.
#
# Not the defaults themselves: Llama builds llama.cpp, minutes of compile and a
# download that `just dev` and the other lanes have no use for.

set(CULEBRA_ENABLE_LLAMA ON CACHE BOOL "")
# ON, not AUTO: a release whose machine lost the Vulkan tools fails its
# configure instead of shipping without the GPU. macOS ignores it (no Vulkan).
set(CULEBRA_LLAMA_VULKAN ON CACHE STRING "")
