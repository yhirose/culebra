# llama.cpp (ggml + llama, static), built once into the shared deps cache, and
# the Llama smoke test that proves the build links. Included from
# CMakeLists.txt when CULEBRA_ENABLE_LLAMA is ON; nothing here touches the
# `culebra` driver yet, so a build with the option on behaves as one with it off.
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
include(ExternalProject)
include(${CMAKE_CURRENT_LIST_DIR}/llama_cpp.cmake)
_culebra_require_submodules(cpp-llamalib)

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
  # Portable code: not tuned to the machine that builds it.
  -DGGML_NATIVE=OFF
  -DGGML_CUDA=OFF -DGGML_METAL=OFF -DGGML_VULKAN=OFF -DGGML_BLAS=OFF)
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

# Link order matters for static archives: llama needs ggml, which needs the CPU
# backend and the base, which the backend needs in turn.
set(_llama_a
  "${_llama_install}/lib/libllama.a"
  "${_llama_install}/lib/libggml.a"
  "${_llama_install}/lib/libggml-cpu.a"
  "${_llama_install}/lib/libggml-base.a")
set(_llama_system_libs "")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND _llama_system_libs Threads::Threads ${CMAKE_DL_LIBS})
elseif(APPLE)
  # The CPU backend's matrix routines.
  list(APPEND _llama_system_libs "-framework Accelerate" "-framework Foundation")
endif()

if(NOT EXISTS "${_llama_install}/lib/libllama.a")
  set(_llama_stage "${CMAKE_CURRENT_BINARY_DIR}/llama_stage")
  ExternalProject_Add(llama_ext
    URL "https://github.com/ggml-org/llama.cpp/archive/refs/tags/${CULEBRA_LLAMA_CPP_VERSION}.tar.gz"
    URL_HASH "SHA256=${CULEBRA_LLAMA_CPP_SHA256}"
    DOWNLOAD_DIR "${_llama_root}/downloads"
    DOWNLOAD_NAME "llama.cpp-${CULEBRA_LLAMA_CPP_VERSION}.tar.gz"
    PREFIX "${CMAKE_CURRENT_BINARY_DIR}/llama_ext_build"
    CMAKE_ARGS
      ${_llama_cmake_args}
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DCMAKE_INSTALL_PREFIX=${_llama_stage}
      -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
      -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
      -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
      -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
    BUILD_BYPRODUCTS ${_llama_a})
  ExternalProject_Add_Step(llama_ext publish
    COMMAND ${CMAKE_COMMAND} -DSTAGE=${_llama_stage} -DDEST=${_llama_install}
            -DPROBE=${_llama_install}/lib/libllama.a
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/publish_dep.cmake"
    COMMENT "Publishing llama.cpp to ${_llama_install}"
    DEPENDEES install)
endif()

# No model: the static llama.cpp links, its backends come up, and a model that
# cannot be loaded is the wrapper's ModelLoadError.
add_executable(llama_smoke test/llama_smoke.cc)
target_compile_features(llama_smoke PRIVATE cxx_std_23)
target_include_directories(llama_smoke SYSTEM PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-llamalib" "${_llama_install}/include")
target_link_libraries(llama_smoke ${_llama_a} ${_llama_system_libs})
if(TARGET llama_ext)
  add_dependencies(llama_smoke llama_ext)
endif()
add_test(NAME llama_smoke COMMAND llama_smoke)
