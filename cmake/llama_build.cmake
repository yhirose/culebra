# Building llama.cpp into the deps cache, and linking it into what uses it.
# Included from CMakeLists.txt when CULEBRA_ENABLE_LLAMA is ON, once the driver
# and the embedding tests exist (the paths and flags are cmake/llama.cmake's).
include(ExternalProject)

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

# The in-process lanes run Llama straight from the driver. The embedding tests
# reach the same rows through bindings.h, so they link the statics as well.
# Everything that compiles those rows waits for the headers llama.cpp installs.
foreach(_t culebra smoke_suite)
  target_link_libraries(${_t} ${_llama_a} ${_llama_system_libs})
  if(TARGET llama_ext)
    add_dependencies(${_t} llama_ext)
  endif()
endforeach()

# No model: the static llama.cpp links, its backends come up, and a model that
# cannot be loaded is the wrapper's ModelLoadError.
add_executable(llama_smoke test/llama_smoke.cc)
target_compile_features(llama_smoke PRIVATE cxx_std_23)
target_link_libraries(llama_smoke ${_llama_a} ${_llama_system_libs})
add_test(NAME llama_smoke COMMAND llama_smoke)

# The model handle of stdlib/llama.h. The part that needs a model runs when
# CULEBRA_TEST_MODEL names a GGUF file.
add_executable(llama_test test/llama_test.cc)
target_compile_features(llama_test PRIVATE cxx_std_23)
target_link_libraries(llama_test ${_llama_a} ${_llama_system_libs})
add_test(NAME llama_test COMMAND llama_test)

foreach(_t llama_smoke llama_test)
  if(TARGET llama_ext)
    add_dependencies(${_t} llama_ext)
  endif()
endforeach()

# The AOT half: the strong archive compiles against llama.cpp's headers, and the
# statics are packed into the driver (_rt_embed_extern) once they are published.
foreach(_t culebra_rt_llama culebra_rt_packed)
  if(TARGET ${_t} AND TARGET llama_ext)
    add_dependencies(${_t} llama_ext)
  endif()
endforeach()
