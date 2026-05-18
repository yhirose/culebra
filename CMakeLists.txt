cmake_minimum_required(VERSION 3.20)
project(culebra)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CULEBRA_ENABLE_JIT "Enable LLVM JIT compilation" OFF)

# Minimum LLVM version. ORC JIT APIs used by jit.h have been stable
# since LLVM 17; routinely tested against current (22.x). Override
# with -DCULEBRA_MIN_LLVM_VERSION=<n> if needed.
set(CULEBRA_MIN_LLVM_VERSION 17 CACHE STRING
    "Minimum required LLVM major version for the JIT build")

if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
endif()

include_directories(./include)
include_directories(./vendor/cpp-peglib)
include_directories(./vendor/cpp-linenoise)
include_directories(./vendor/cpp-unicodelib)

if(CULEBRA_ENABLE_JIT)
  # Homebrew installs LLVM keg-only so find_package can't see it without
  # a hint. When the user didn't pass CMAKE_PREFIX_PATH themselves,
  # derive it from `brew --prefix llvm`.
  if(APPLE AND NOT CMAKE_PREFIX_PATH)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
      execute_process(
        COMMAND ${BREW_EXECUTABLE} --prefix llvm
        OUTPUT_VARIABLE BREW_LLVM_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE BREW_LLVM_RESULT)
      if(BREW_LLVM_RESULT EQUAL 0 AND BREW_LLVM_PREFIX)
        list(APPEND CMAKE_PREFIX_PATH "${BREW_LLVM_PREFIX}")
        message(STATUS "Homebrew LLVM detected at ${BREW_LLVM_PREFIX}")
      endif()
    endif()
  endif()

  find_package(LLVM CONFIG)
  if(NOT LLVM_FOUND)
    if(APPLE)
      set(_hint "brew install llvm")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(_hint "apt install llvm-${CULEBRA_MIN_LLVM_VERSION}-dev   (Debian/Ubuntu)\n         dnf install llvm-devel                (Fedora/RHEL)")
    elseif(WIN32)
      set(_hint "winget install LLVM.LLVM   or   choco install llvm")
    else()
      set(_hint "install LLVM ${CULEBRA_MIN_LLVM_VERSION}+ via your platform's package manager")
    endif()
    message(FATAL_ERROR
      "LLVM ${CULEBRA_MIN_LLVM_VERSION}+ not found. Install it with:\n"
      "         ${_hint}\n"
      "then re-run cmake (or `just build`). To build without JIT, "
      "pass -DCULEBRA_ENABLE_JIT=OFF.")
  endif()
  if(LLVM_PACKAGE_VERSION VERSION_LESS ${CULEBRA_MIN_LLVM_VERSION})
    message(FATAL_ERROR
      "Found LLVM ${LLVM_PACKAGE_VERSION} but Culebra requires "
      "LLVM ${CULEBRA_MIN_LLVM_VERSION}+. Upgrade via your package "
      "manager, or override the floor with "
      "-DCULEBRA_MIN_LLVM_VERSION=<n>.")
  endif()
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")

  include_directories(${LLVM_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})

  llvm_map_components_to_libnames(llvm_libs
    core support orcjit native passes)

  add_compile_definitions(CULEBRA_JIT_ENABLED)
endif()

add_executable(culebra ./src/main.cc)

if(CULEBRA_ENABLE_JIT)
  target_link_libraries(culebra ${llvm_libs})
  # ORC JIT looks up culebra_runtime_* symbols in the host process at
  # codegen time. On macOS those symbols are visible by default; on
  # ELF/Linux they are stripped from the dynamic symbol table unless
  # we link with -rdynamic. ENABLE_EXPORTS adds the right flag per
  # platform (no-op on macOS).
  set_target_properties(culebra PROPERTIES ENABLE_EXPORTS ON)

  # Static archive of the runtime helpers used by AOT-built CLI
  # binaries. `culebra build foo.cul -o foo` emits an object that
  # references `culebra_runtime_*` symbols; the link step pulls those
  # definitions out of this archive (no LLVM dependency required for
  # the produced binary).
  add_library(culebra_rt STATIC ./src/runtime/culebra_rt.cc)
  target_compile_features(culebra_rt PRIVATE cxx_std_23)
  # The runtime archive is intended to be linked into binaries that
  # do NOT depend on LLVM. Suppress the abi-breaking-check witness
  # symbol (`llvm::DisableABIBreakingChecks`) that every LLVM-header
  # TU would otherwise pull in.
  target_compile_definitions(culebra_rt PRIVATE
    LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
    CULEBRA_RT_DEFINE_RUNTIME=1)
  # One section per function/data so the AOT link can `-dead_strip`
  # the ~200 `culebra_runtime_*` helpers a typical program doesn't
  # touch. Without this every runtime helper is in the same .text /
  # .data section and the linker can't drop them.
  target_compile_options(culebra_rt PRIVATE
    -ffunction-sections -fdata-sections)
  # The `culebra build` driver needs to know where to find the
  # archive at link time. Override at runtime with $CULEBRA_RT_LIB.
  target_compile_definitions(culebra PRIVATE
    CULEBRA_RT_LIBPATH="$<TARGET_FILE:culebra_rt>")
endif()

# Tensor primitive: BLAS-backed kernels (sgemm, vDSP, etc.) land in
# Phase 1 / M2-M4. M1 ships the type without a BLAS dependency, but
# we link Accelerate (macOS) / BLAS (Linux) up front so subsequent
# milestones don't re-touch the build system.
option(CULEBRA_ENABLE_TENSOR "Enable builtin Tensor type" ON)

if(CULEBRA_ENABLE_TENSOR)
  add_compile_definitions(CULEBRA_TENSOR_ENABLED)
  if(APPLE)
    set(_blas_link "-framework Accelerate")
    target_link_libraries(culebra "-framework Accelerate")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(BLAS REQUIRED)
    string(REPLACE ";" " " _blas_link "${BLAS_LIBRARIES}")
    target_link_libraries(culebra ${BLAS_LIBRARIES})
  else()
    set(_blas_link "")
  endif()
  # The `culebra build` driver appends this to the link command only
  # when the program references Tensor (see `aot_scan_namespaces`).
  # Programs that never touch Tensor avoid the BLAS dependency.
  if(CULEBRA_ENABLE_JIT)
    target_compile_definitions(culebra PRIVATE
      CULEBRA_BLAS_LINK="${_blas_link}")
  endif()
elseif(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_BLAS_LINK="")
endif()

# Embedding smoke tests. mt_smoke exercises thread_local isolation
# (parser, GC, exception state); mi_smoke exercises RuntimeScope and
# per-Runtime hooks. Build them alongside `culebra` so they stay in
# step with the headers.
enable_testing()

function(culebra_add_smoke_exe name)
  add_executable(${name} tests/embedding/${name}.cc)
  target_compile_features(${name} PRIVATE cxx_std_23)
  if(CULEBRA_ENABLE_TENSOR)
    if(APPLE)
      target_link_libraries(${name} "-framework Accelerate")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_libraries(${name} ${BLAS_LIBRARIES})
    endif()
  endif()
  if(CULEBRA_ENABLE_JIT)
    target_link_libraries(${name} ${llvm_libs})
    set_target_properties(${name} PROPERTIES ENABLE_EXPORTS ON)
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

culebra_add_smoke_exe(mt_smoke)
culebra_add_smoke_exe(mi_smoke)
culebra_add_smoke_exe(define_smoke)
