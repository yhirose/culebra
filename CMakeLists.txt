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

# Use ccache as the compiler launcher when available. Keeps a content-
# addressed cache of .o files in ~/.cache/ccache so that header touches
# that don't change content (e.g. branch switching) hit the cache and
# skip the compile. Auto-disabled if ccache isn't installed.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache: ${CCACHE_PROGRAM}")
endif()

# ThinLTO trims ~15 MB off the Release `culebra` driver and gives the
# JIT runtime a permanent +6.7% (cross-TU inlining of codegen). Cost
# is ~25 s extra link time per build. Disable with `-DCULEBRA_LTO=OFF`
# for fast local iterations.
#
# IMPORTANT: applied only to the `culebra` executable target, not to
# `culebra_rt` / `culebra_rt_no_tensor`. Those archives ship LTO
# bitcode that the end user's compiler links into AOT-built binaries —
# the user's compiler may be a different toolchain/version (GCC 13 vs
# 14, Clang vs GCC) and chokes on cross-version LTO bitcode. Object
# code only for archives keeps `culebra build` portable.
option(CULEBRA_LTO "Enable ThinLTO for the culebra driver (Release)" ON)
# Inner-loop dev switch: skip building+embedding the four AOT runtime
# archives. Each archive is a full recompile of culebra_rt.cc (which
# pulls in the interpreter + JIT headers), so embedding all four turns a
# single-TU header touch into a five-TU rebuild. With this ON, only
# main.cc recompiles + relinks; `culebra build` (AOT) then errors
# cleanly. `just dev` passes this; `just build`/`just test all` do not.
option(CULEBRA_DEV_NO_RT "Skip AOT runtime archives for fast inner-loop builds" OFF)
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_err)
if(CULEBRA_LTO AND _ipo_supported AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set(_culebra_enable_lto TRUE)
else()
  set(_culebra_enable_lto FALSE)
endif()

include_directories(./include)
include_directories(./vendor/cpp-peglib)
include_directories(./vendor/cpp-linenoise)
include_directories(./vendor/cpp-unicodelib)
include_directories(./vendor/cpp-httplib)

# Http stdlib namespace: cpp-httplib (vendored) + OpenSSL for TLS. OpenSSL is
# linked statically so the `culebra` driver and AOT-built binaries stay
# self-contained (cpp-httplib's OpenSSL path is identical to a future
# BoringSSL swap — that is a link-only change). The static .a libs come from
# Homebrew's openssl@3 on macOS; on Linux from the distro's OpenSSL dev pkg.
option(CULEBRA_ENABLE_HTTP "Enable builtin Http namespace (requires OpenSSL)" ON)
if(CULEBRA_ENABLE_HTTP)
  set(OPENSSL_USE_STATIC_LIBS TRUE)
  if(APPLE AND NOT OPENSSL_ROOT_DIR)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
      execute_process(
        COMMAND ${BREW_EXECUTABLE} --prefix openssl@3
        OUTPUT_VARIABLE BREW_OPENSSL_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE BREW_OPENSSL_RESULT)
      if(BREW_OPENSSL_RESULT EQUAL 0 AND BREW_OPENSSL_PREFIX)
        set(OPENSSL_ROOT_DIR "${BREW_OPENSSL_PREFIX}")
        message(STATUS "Homebrew OpenSSL detected at ${BREW_OPENSSL_PREFIX}")
      endif()
    endif()
  endif()
  find_package(OpenSSL REQUIRED)
  message(STATUS "Found OpenSSL ${OPENSSL_VERSION} (static)")
  # zlib enables gzip/deflate: cpp-httplib then auto-sends Accept-Encoding and
  # transparently decompresses responses (decompress_ defaults true). System
  # zlib (libz) is ubiquitous on macOS/Linux; linked dynamically.
  find_package(ZLIB REQUIRED)
  message(STATUS "Found ZLIB ${ZLIB_VERSION_STRING}")
  add_compile_definitions(CULEBRA_HTTP_ENABLED CPPHTTPLIB_OPENSSL_SUPPORT
                          CPPHTTPLIB_ZLIB_SUPPORT)
  # Make OpenSSL + zlib headers visible to every TU that includes httplib.h —
  # notably the culebra_rt archives, which compile httplib.h but don't link
  # these (the AOT consumer's link resolves the symbols via _ssl_link).
  include_directories(${OPENSSL_INCLUDE_DIR} ${ZLIB_INCLUDE_DIRS})

  # Link line reused by the driver, the smoke exes, and (as a string) the AOT
  # build step. Static OpenSSL drags in libdl/libpthread on Linux; on macOS
  # cpp-httplib reads the system trust store via the Security framework
  # (CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN auto-enables there).
  set(_ssl_libs OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB)
  if(APPLE)
    list(APPEND _ssl_libs "-framework CoreFoundation" "-framework Security")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _ssl_libs ${CMAKE_DL_LIBS})
    find_package(Threads REQUIRED)
    list(APPEND _ssl_libs Threads::Threads)
  endif()

  # AOT link string: resolve the OpenSSL static archives to absolute paths so
  # `culebra build` can append them when a program references Http. Mirrors
  # CULEBRA_BLAS_LINK for Tensor. zlib is linked dynamically (-lz).
  set(_ssl_link "${OPENSSL_SSL_LIBRARY} ${OPENSSL_CRYPTO_LIBRARY} -lz")
  if(APPLE)
    set(_ssl_link "${_ssl_link} -framework CoreFoundation -framework Security")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ssl_link "${_ssl_link} -ldl -lpthread")
  endif()
endif()

# cpp-embedlib provides the cpp_embedlib_add() macro used below to bake
# the runtime archives into the `culebra` driver, so `culebra build` can
# operate as a single self-contained binary (no sibling .a files needed).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-embedlib/cmake")
include(cpp-embedlib)

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

  # Native target keeps the JIT lean. WebAssembly is bundled so the
  # docs-site WASM playground (see project-docs-site memory) can be
  # produced via `culebra build --target=wasm32-…` from the same binary.
  # AllTargets* extend codegen + asm tables for any other LLVM target —
  # opt-in because they add ~31 MB to the `culebra` driver. AOT-built
  # executables never carry LLVM regardless.
  option(CULEBRA_LLVM_ALL_TARGETS
    "Link all LLVM backends for cross-compile (adds ~31 MB)" OFF)
  if(CULEBRA_LLVM_ALL_TARGETS)
    set(_extra_target_libs
      AllTargetsAsmParsers AllTargetsCodeGens AllTargetsDescs
      AllTargetsInfos)
    add_compile_definitions(CULEBRA_LLVM_ALL_TARGETS)
  else()
    set(_extra_target_libs
      WebAssemblyAsmParser WebAssemblyCodeGen WebAssemblyDesc
      WebAssemblyInfo)
  endif()
  llvm_map_components_to_libnames(llvm_libs
    core support orcjit native passes ${_extra_target_libs})

  add_compile_definitions(CULEBRA_JIT_ENABLED)
endif()

add_executable(culebra ./src/main.cc)
if(_culebra_enable_lto)
  set_target_properties(culebra PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if(CULEBRA_ENABLE_JIT)
  target_link_libraries(culebra ${llvm_libs})
  # ORC JIT looks up culebra_runtime_* symbols in the host process at
  # codegen time. On macOS those symbols are visible by default; on
  # ELF/Linux they are stripped from the dynamic symbol table unless
  # we link with -rdynamic. ENABLE_EXPORTS adds the right flag per
  # platform (no-op on macOS).
  set_target_properties(culebra PROPERTIES ENABLE_EXPORTS ON)

  if(CULEBRA_DEV_NO_RT)
  # Fast inner-loop build: don't compile or embed the runtime archives.
  # Provide a stub culebra_rt_assets.h so main.cc still links; an empty
  # CulebraRT::FS makes `culebra build` (AOT) fail with a clear
  # "embedded runtime archive ... not found" instead of crashing.
  set(_dev_rt_stub_dir "${CMAKE_CURRENT_BINARY_DIR}/dev_rt_stub")
  file(WRITE "${_dev_rt_stub_dir}/culebra_rt_assets.h"
"// Auto-generated dev stub (CULEBRA_DEV_NO_RT). No AOT runtime embedded.
#pragma once
#include <cpp-embedlib/cpp-embedlib.h>
#include <span>
namespace CulebraRT {
inline constexpr std::span<const ::cpp_embedlib::detail::StorageEntry> _entries{};
inline const auto FS = ::cpp_embedlib::EmbeddedFS{_entries};
} // namespace CulebraRT
")
  target_include_directories(culebra PRIVATE "${_dev_rt_stub_dir}"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-embedlib/include")
  else()
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

  # Tensor-free variant. `culebra build` picks this when AST scan
  # reports no Tensor references — `culebra_runtime_tensor_binop` and
  # the tensor cases in value-release / value-to-str fall through to
  # nullptr / no-op stubs, breaking the static reachability chain to
  # cblas / tensor.h kernels so Accelerate / BLAS can be dropped too.
  add_library(culebra_rt_no_tensor STATIC ./src/runtime/culebra_rt.cc)
  target_compile_features(culebra_rt_no_tensor PRIVATE cxx_std_23)
  target_compile_definitions(culebra_rt_no_tensor PRIVATE
    LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
    CULEBRA_RT_DEFINE_RUNTIME=1
    CULEBRA_RT_NO_TENSOR=1)
  target_compile_options(culebra_rt_no_tensor PRIVATE
    -ffunction-sections -fdata-sections)

  # Http-free variants. `culebra build` picks one of these when the AST
  # scan reports no Http references — CULEBRA_RT_NO_HTTP drops the http
  # namespace (and its httplib.h include) entirely, so the static
  # reachability chain to OpenSSL / zlib is broken and they can be left
  # out of the AOT link. Crossed with the tensor axis, that's 2x2 = 4
  # archives total.
  add_library(culebra_rt_no_http STATIC ./src/runtime/culebra_rt.cc)
  target_compile_features(culebra_rt_no_http PRIVATE cxx_std_23)
  target_compile_definitions(culebra_rt_no_http PRIVATE
    LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
    CULEBRA_RT_DEFINE_RUNTIME=1
    CULEBRA_RT_NO_HTTP=1)
  target_compile_options(culebra_rt_no_http PRIVATE
    -ffunction-sections -fdata-sections)

  add_library(culebra_rt_no_tensor_no_http STATIC ./src/runtime/culebra_rt.cc)
  target_compile_features(culebra_rt_no_tensor_no_http PRIVATE cxx_std_23)
  target_compile_definitions(culebra_rt_no_tensor_no_http PRIVATE
    LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
    CULEBRA_RT_DEFINE_RUNTIME=1
    CULEBRA_RT_NO_TENSOR=1
    CULEBRA_RT_NO_HTTP=1)
  target_compile_options(culebra_rt_no_tensor_no_http PRIVATE
    -ffunction-sections -fdata-sections)

  # Embed all four runtime archives into the `culebra` driver via
  # cpp-embedlib. `culebra build` materializes them to
  # ~/.cache/culebra/<fingerprint>/ on first use, so the driver is a
  # single self-contained binary. `--rt-lib=<path>` overrides the
  # archive for cross-compile (env override removed — the flag is
  # explicit and covers every case).
  #
  # cpp-embedlib computes relative paths at configure time, so passing
  # $<TARGET_FILE:...> generator expressions doesn't work. Reconstruct
  # the archive paths from CMake's static library naming convention
  # instead — these resolve to libculebra_rt.a / libculebra_rt_no_tensor.a
  # in the build dir, which is where CMake writes STATIC libraries by
  # default.
  set(_rt_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_no_tensor_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_no_tensor${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_no_http_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_no_http${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_no_tensor_no_http_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_no_tensor_no_http${CMAKE_STATIC_LIBRARY_SUFFIX}")
  cpp_embedlib_add(culebra_rt_assets
    FILES
      "${_rt_archive}"
      "${_rt_no_tensor_archive}"
      "${_rt_no_http_archive}"
      "${_rt_no_tensor_no_http_archive}"
    BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}"
    NAMESPACE CulebraRT)
  # cpp-embedlib's per-file DEPENDS is a file-level dependency. Force
  # the asset library to wait for the real archive targets too, so the
  # .a files actually exist when the embed step runs.
  add_dependencies(culebra_rt_assets culebra_rt culebra_rt_no_tensor
    culebra_rt_no_http culebra_rt_no_tensor_no_http)
  target_link_libraries(culebra culebra_rt_assets)
  endif()
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

if(CULEBRA_ENABLE_HTTP)
  target_link_libraries(culebra ${_ssl_libs})
  # `culebra build` appends this to the link command only when the program
  # references Http (see aot_scan_namespaces). Programs that never touch Http
  # avoid the OpenSSL dependency, same as Tensor/BLAS.
  if(CULEBRA_ENABLE_JIT)
    target_compile_definitions(culebra PRIVATE CULEBRA_SSL_LINK="${_ssl_link}")
  endif()
elseif(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_SSL_LINK="")
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
  if(CULEBRA_ENABLE_HTTP)
    target_link_libraries(${name} ${_ssl_libs})
  endif()
  add_test(NAME ${name} COMMAND ${name})
endfunction()

culebra_add_smoke_exe(mt_smoke)
culebra_add_smoke_exe(mi_smoke)
culebra_add_smoke_exe(define_smoke)
culebra_add_smoke_exe(signal_smoke)
culebra_add_smoke_exe(utf8_invalid_smoke)

# regexlib is a standalone header-only library (destined for cpp-regexlib); its
# test depends only on regexlib.h + cpp-unicodelib, so it needs no JIT/LLVM
# link. Built alongside `culebra` to stay in step with the header.
add_executable(regexlib_test test/regexlib_test.cc)
target_compile_features(regexlib_test PRIVATE cxx_std_17)
add_test(NAME regexlib_test COMMAND regexlib_test)

# http.h core test: in-process loopback cpp-httplib server exercises the
# value-neutral client core (GET/POST/DELETE/404 + transport-error + URL
# validation). Needs OpenSSL (httplib.h pulls it in under HTTP builds) but the
# server here is plain HTTP, so the test runs offline.
if(CULEBRA_ENABLE_HTTP)
  add_executable(http_test test/http_test.cc)
  target_compile_features(http_test PRIVATE cxx_std_20)
  target_link_libraries(http_test ${_ssl_libs})
  add_test(NAME http_test COMMAND http_test)
endif()

# Deterministic regression fuzzer: gates on oracle-free robustness invariants
# (no crash/hang, self-consistent match spans, find_all/search agreement) and
# prints an informational std::regex differential. Run under ASan/UBSan in CI.
add_executable(regexlib_fuzz test/regexlib_fuzz.cc)
target_compile_features(regexlib_fuzz PRIVATE cxx_std_17)
add_test(NAME regexlib_fuzz COMMAND regexlib_fuzz)

# Portable semantic-regression corpus: cases adjudicated once against Python's
# `re` and baked into test/regexlib_corpus_data.h (no runtime oracle needed).
add_executable(regexlib_corpus test/regexlib_corpus.cc)
target_compile_features(regexlib_corpus PRIVATE cxx_std_17)
add_test(NAME regexlib_corpus COMMAND regexlib_corpus)

# GC unit tests — the standalone collector (include/jit_gc.h) validated in
# isolation (pointer-validation, conservative root scanning, mark-sweep), no
# JIT/LLVM link, independent of the in-tree jit.h wiring.
foreach(gc_test jit_gc_phase0 jit_gc_collect)
  add_executable(${gc_test} tests/${gc_test}.cc)
  target_compile_features(${gc_test} PRIVATE cxx_std_20)
  target_include_directories(${gc_test} PRIVATE include)
  add_test(NAME ${gc_test} COMMAND ${gc_test})
endforeach()

# Lint regression (include/lint.h): the static let-reassignment check must
# abort before eval, and the sound-negative cases (let mut, separate
# scopes, block-locals) must be accepted. Driven E2E through the built
# `culebra` binary, which all backends share via the ModuleLoader.
add_test(NAME lint_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/lint_test.sh
                 $<TARGET_FILE:culebra>)

# Compile-time error position symmetry (include/jit.h compile() wrapper):
# the JIT must backfill the compiling node's line/col onto position-less
# compile-time throws, matching the interp's eval() stamping byte-for-byte.
add_test(NAME jit_error_pos_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/jit_error_pos_test.sh
                 $<TARGET_FILE:culebra>)

# Cooperative Ctrl+C (SIGINT): a tight loop is interruptible, the throw is a
# catchable `Interrupted` (defer runs, catch resumes), uncaught exits 130 —
# identical across interp / JIT / AOT. Drives the binary and sends a signal.
add_test(NAME signal_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/signal_test.sh
                 $<TARGET_FILE:culebra>)
