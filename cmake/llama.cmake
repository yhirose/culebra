# llama.cpp (ggml + llama, static) for the Llama namespace: where it is built
# and what a translation unit needs to see it. Included from CMakeLists.txt, at
# the option, before any target exists, when CULEBRA_ENABLE_LLAMA is ON. The
# build itself (ExternalProject) and the targets that link it are in
# cmake/llama_build.cmake, included once those targets are there.
#
# Like SDL3 and raylib, the statics depend only on their sources, their
# configuration and the target platform, never on culebra's build type, LTO or
# JIT settings, so they are built under a key made of those and published
# atomically (cmake/publish_dep.cmake): every build dir and worktree that wants
# the same bytes finds them there. Unlike them, llama.cpp is not a submodule: its
# tree is ~220 MB, which every worktree and every `submodules: recursive` CI job
# would pay for. The release archive (~38 MB, one download shared through the
# cache) is fetched against a pinned SHA-256 instead; cmake/llama_cpp.cmake holds
# the version and the hash.
include(${CMAKE_CURRENT_LIST_DIR}/llama_cpp.cmake)
_culebra_require_submodules(cpp-llamalib)

# GPU backend off macOS (macOS gets Metal, U5). ggml-vulkan needs the Vulkan
# headers and glslc to build; at run time the loader is opened, not linked
# (src/runtime/llama_vulkan_dynload.cc), so the binary still starts, and runs
# Llama on the CPU, where no Vulkan driver exists. Like CULEBRA_TENSOR_CUDA,
# AUTO turns it on where the tools are found; ON fails the configure without them.
set(CULEBRA_LLAMA_VULKAN AUTO CACHE STRING
    "Vulkan GPU backend for Llama off macOS: ON, OFF, or AUTO (Vulkan headers and glslc found)")
set_property(CACHE CULEBRA_LLAMA_VULKAN PROPERTY STRINGS AUTO ON OFF)

set(_llama_vulkan OFF)
# Tool locations go to llama.cpp's configure but not into the cache key below,
# which must not depend on where this machine keeps them.
set(_llama_tool_args "")
if(NOT APPLE AND NOT CULEBRA_LLAMA_VULKAN STREQUAL "OFF")
  # glslc by its path variable: the component form needs CMake 3.24.
  find_package(Vulkan)
  if(Vulkan_FOUND AND Vulkan_GLSLC_EXECUTABLE)
    set(_llama_vulkan ON)
    list(APPEND _llama_tool_args
      -DVulkan_INCLUDE_DIR=${Vulkan_INCLUDE_DIR}
      -DVulkan_LIBRARY=${Vulkan_LIBRARY}
      -DVulkan_GLSLC_EXECUTABLE=${Vulkan_GLSLC_EXECUTABLE})
  elseif(CULEBRA_LLAMA_VULKAN STREQUAL "ON")
    message(FATAL_ERROR
      "CULEBRA_LLAMA_VULKAN=ON but the Vulkan headers or glslc were not found. "
      "Install them (Ubuntu: libvulkan-dev glslc) or set VULKAN_SDK.")
  else()
    message(STATUS "Llama: Vulkan backend off (no Vulkan headers or glslc)")
  endif()
endif()

# The configuration is part of the key: a flag change builds into a new
# directory instead of being served stale bytes.
set(_llama_cmake_args
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_BUILD_TYPE=Release
  # The wrapper (vendor/cpp-llamalib) needs libllama's public API only. llama.cpp's
  # `common`, tools, server, app and tests are internal to its own programs.
  -DLLAMA_BUILD_COMMON=OFF -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_TESTS=OFF
  -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=OFF -DLLAMA_BUILD_APP=OFF
  # ggml's own thread pool, so the binary does not need libgomp.so at start.
  -DGGML_OPENMP=OFF
  # Portable code: not tuned to the machine that builds it. (On x86 that still
  # means AVX2; stdlib/llama.h checks the CPU for it before loading a model.)
  -DGGML_NATIVE=OFF
  -DGGML_CUDA=OFF -DGGML_METAL=OFF -DGGML_VULKAN=${_llama_vulkan} -DGGML_BLAS=OFF)
string(MD5 _llama_args_hash "${_llama_cmake_args}")
string(SUBSTRING "${_llama_args_hash}" 0 8 _llama_args_hash)
string(SUBSTRING "${CULEBRA_LLAMA_CPP_SHA256}" 0 12 _llama_src_hash)

_culebra_deps_cache_root(_llama_root)
set(_llama_key "${CMAKE_SYSTEM_NAME}" "${CMAKE_SYSTEM_PROCESSOR}")
if(CMAKE_OSX_ARCHITECTURES)
  list(APPEND _llama_key "${CMAKE_OSX_ARCHITECTURES}")
endif()
if(CMAKE_OSX_DEPLOYMENT_TARGET)
  list(APPEND _llama_key "macos${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
list(APPEND _llama_key "llama${CULEBRA_LLAMA_CPP_VERSION}" "${_llama_src_hash}"
                       "cfg${_llama_args_hash}")
string(REPLACE ";" "-" _llama_key "${_llama_key}")
set(_llama_install "${_llama_root}/${_llama_key}/llama")
message(STATUS "llama.cpp ${CULEBRA_LLAMA_CPP_VERSION} deps cache: ${_llama_root}/${_llama_key}")

# Link order matters for static archives: llama needs ggml, which needs the
# backends and the base, which the backends need in turn. _llama_aot_a is what
# `culebra build` links (and the driver embeds): the CPU backend only, whose one
# reference into the Vulkan backend src/runtime/llama_gpu_none.cc answers.
set(_llama_aot_a
  "${_llama_install}/lib/libllama.a"
  "${_llama_install}/lib/libggml.a"
  "${_llama_install}/lib/libggml-cpu.a"
  "${_llama_install}/lib/libggml-base.a")
set(_llama_vulkan_a "")
if(_llama_vulkan)
  set(_llama_vulkan_a "${_llama_install}/lib/libggml-vulkan.a")
endif()
set(_llama_a
  "${_llama_install}/lib/libllama.a"
  "${_llama_install}/lib/libggml.a"
  ${_llama_vulkan_a}
  "${_llama_install}/lib/libggml-cpu.a"
  "${_llama_install}/lib/libggml-base.a")
# For tools/checks/check_llama_vulkan_dynload.sh, which compares the forwarders
# with what the archive names ("" without the backend).
set(CULEBRA_LLAMA_VULKAN_ARCHIVE "${_llama_vulkan_a}" CACHE INTERNAL "")
set(_llama_system_libs "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND _llama_system_libs Threads::Threads ${CMAKE_DL_LIBS})
elseif(APPLE)
  # The CPU backend's matrix routines.
  list(APPEND _llama_system_libs "-framework Accelerate" "-framework Foundation")
endif()

# What `culebra build` appends to the link of a program that names Llama: the
# four statics by embedded name (@libllama.a@, see _rt_embed_extern in
# CMakeLists.txt) and the system libraries, never a path of this machine.
set(_llama_link "@libllama.a@ @libggml.a@ @libggml-cpu.a@ @libggml-base.a@")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  string(APPEND _llama_link " -lpthread -ldl -lm")
elseif(APPLE)
  string(APPEND _llama_link " -framework Accelerate -framework Foundation")
endif()

# Every translation unit that reaches stdlib/bindings.h compiles the Llama rows
# (stdlib/llama.h), so every one of them sees the headers.
add_compile_definitions(CULEBRA_LLAMA_ENABLED)
include_directories(SYSTEM "${_llama_install}/include"
                           "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-llamalib")
