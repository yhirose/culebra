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
# `culebra_rt` / the feature objects. Those archives ship LTO
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
# cpp-tensorlib (header-only): the Tensor execution engine (lazy graph,
# fusion, Accelerate/Metal backends). Vendored as a git submodule under
# vendor/ so a fresh clone builds with `--recursive`. Override
# CULEBRA_TENSORLIB_DIR to point at a sibling checkout for co-development.
# Presence is checked in the CULEBRA_ENABLE_TENSOR block below (the option
# is defined further down).
set(CULEBRA_TENSORLIB_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-tensorlib/include"
    CACHE PATH "cpp-tensorlib include directory")
include_directories(${CULEBRA_TENSORLIB_DIR})
include_directories(./vendor/cpp-peglib)
include_directories(./vendor/cpp-linenoise)
include_directories(./vendor/cpp-unicodelib)
include_directories(./vendor/cpp-regexlib)
include_directories(./vendor/cpp-httplib)
include_directories(./vendor/sqlite)

# SQLite amalgamation (vendor/sqlite/sqlite3.c, public domain) for the builtin
# SQLite namespace. Compiled once into an OBJECT library and reused by both the
# `culebra` driver (in-process interp/JIT run SQLite directly) and the gated
# culebra_rt_sqlite feature archive (AOT). Like cpp-httplib's OpenSSL choke,
# the heavy dependency is partitioned: a non-SQLite AOT binary force-loads
# neither this object nor the strong wrappers (see sqlite.h / aot_uses_sqlite).
# Warnings are silenced (-w) since the amalgamation is third-party generated
# code; -ffunction-sections lets the AOT link dead-strip unused SQLite code.
add_library(culebra_sqlite3 OBJECT ./vendor/sqlite/sqlite3.c)
target_compile_options(culebra_sqlite3 PRIVATE -w -ffunction-sections -fdata-sections)
target_compile_definitions(culebra_sqlite3 PRIVATE
  SQLITE_THREADSAFE=1
  SQLITE_DQS=0
  SQLITE_OMIT_LOAD_EXTENSION
  SQLITE_DEFAULT_FOREIGN_KEYS=1
  SQLITE_OMIT_DEPRECATED
  SQLITE_OMIT_SHARED_CACHE
  SQLITE_LIKE_DOESNT_MATCH_BLOBS
  SQLITE_DEFAULT_MEMSTATUS=0
  SQLITE_ENABLE_FTS5
  SQLITE_ENABLE_RTREE)

# zlib (libz): the gzip choke for the Compress namespace (gzip/gunzip) and, when
# Http is enabled, cpp-httplib's transparent response gzip. Found unconditionally
# so Compress works in builds without Http. System libz is ubiquitous on
# macOS/Linux and linked dynamically (macOS ships no static libz.a); a non-
# Compress, non-Http AOT binary still omits it via the weak gzip/gunzip choke in
# the core archive (see compress.h / aot_uses_compress).
# On Windows there is no system zlib, so prefer the static libz.a — a dynamic
# zlib1.dll would make the otherwise self-contained .exe fail to start on a
# plain Windows box that lacks it.
if(WIN32)
  set(ZLIB_USE_STATIC_LIBS ON)
endif()
find_package(ZLIB REQUIRED)
message(STATUS "Found ZLIB ${ZLIB_VERSION_STRING}")
include_directories(${ZLIB_INCLUDE_DIRS})
# AOT link fragment appended by `culebra build` only when the program references
# Compress (or Http) — resolved to the system libz (-lz, dynamic).
set(_zlib_link "-lz")

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
  # cpp-httplib then auto-sends Accept-Encoding and transparently decompresses
  # gzip responses (decompress_ defaults true). zlib is found unconditionally
  # above (also used by the Compress namespace).
  add_compile_definitions(CULEBRA_HTTP_ENABLED CPPHTTPLIB_OPENSSL_SUPPORT
                          CPPHTTPLIB_ZLIB_SUPPORT)
  # Make OpenSSL headers visible to every TU that includes httplib.h — notably
  # the culebra_rt archives, which compile httplib.h but don't link it (the AOT
  # consumer's link resolves the symbols via _ssl_link). zlib headers are added
  # unconditionally above.
  include_directories(${OPENSSL_INCLUDE_DIR})

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

# --- Builtin Graphics namespace (raylib + SDL): config ---------------------
# Heavy, GUI/platform-bound, so OFF by default (normal/headless builds + CI stay
# lean). Computed before the runtime/embed block so the feature archive + the
# AOT link string can be registered there. Both SDL2 and raylib (SDL backend)
# are built statically from vendored submodules further below; here we only
# resolve paths + flags. Static SDL2 = fully self-contained binaries (no external
# dylib path baked in) and keeps gamepad rumble across platforms — vs GLFW, whose
# macOS gamepad subsystem is broken and which has no haptics API at all.
option(CULEBRA_ENABLE_GRAPHICS "Enable builtin Graphics namespace (raylib + SDL)" OFF)
set(_graphics_link "")
if(CULEBRA_ENABLE_GRAPHICS)
  set(_sdl_install "${CMAKE_CURRENT_BINARY_DIR}/sdl_ext")
  set(_sdl_a "${_sdl_install}/lib/libSDL2.a")
  set(_raylib_install "${CMAKE_CURRENT_BINARY_DIR}/raylib_ext")
  set(_raylib_a "${_raylib_install}/lib/libraylib.a")
  # AOT link fragment (CULEBRA_GRAPHICS_LINK): the built raylib + SDL2 statics +
  # GUI/audio system frameworks. `culebra build` appends it only when the program
  # uses Graphics. Every path is in-tree or a system framework — no external dylib.
  set(_graphics_link "'${_raylib_a}' '${_sdl_a}'")
  if(APPLE)
    # Frameworks SDL2 needs (window/input/audio/haptics) + raylib's GL path.
    # Single-sourced so the AOT link fragment here and the in-process link of the
    # driver (further below) can't drift apart.
    set(_graphics_frameworks
      OpenGL Cocoa IOKit CoreVideo CoreAudio AudioToolbox CoreFoundation
      GameController ForceFeedback CoreHaptics Carbon Metal)
    foreach(_fw IN LISTS _graphics_frameworks)
      string(APPEND _graphics_link " -framework ${_fw}")
    endforeach()
  endif()
  message(STATUS "Graphics: enabled (vendored static SDL2 + raylib)")
endif()

# --- Builtin Webview namespace (native WebView desktop): config ------------
# ON by default: unlike Graphics there is no static library to build — webview
# is a single vendored header (vendor/webview/webview.h) plus the OS's native
# WebView framework, so it adds ~300 KB to the driver and nothing to non-Webview
# AOT binaries (usage-gated force-load, mirroring the Graphics axis). On
# platforms where the toolkit headers are missing (Linux without gtk4 +
# webkitgtk-6.0, untested Windows) it auto-disables — the binding TU wouldn't
# compile there.
option(CULEBRA_ENABLE_WEBVIEW "Enable builtin Webview namespace (native WebView)" ON)
set(_webview_link "")
if(CULEBRA_ENABLE_WEBVIEW)
  # AOT link fragment (CULEBRA_WEBVIEW_LINK): the OS WebView framework(s).
  # `culebra build` appends it only when the program uses Webview. Single-sourced
  # with the driver's in-process link (further below) so they can't drift.
  if(APPLE)
    set(_webview_link "-framework WebKit -ldl")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
      pkg_check_modules(WEBKITGTK gtk4 webkitgtk-6.0)
    endif()
    if(WEBKITGTK_FOUND)
      string(REPLACE ";" " " _webview_link "${WEBKITGTK_LDFLAGS}")
      string(APPEND _webview_link " -ldl")
    else()
      message(STATUS "Webview: disabled (gtk4 + webkitgtk-6.0 not found via pkg-config)")
      set(CULEBRA_ENABLE_WEBVIEW OFF)
    endif()
  else()
    message(STATUS "Webview: disabled (unsupported platform for the native WebView backend)")
    set(CULEBRA_ENABLE_WEBVIEW OFF)
  endif()
endif()
if(CULEBRA_ENABLE_WEBVIEW)
  message(STATUS "Webview: enabled (native WebView, vendored webview.h)")
endif()

add_executable(culebra ./src/main.cc)
if(_culebra_enable_lto)
  set_target_properties(culebra PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Out-of-tree binding declarations (`culebra wrap` drives these): each
# TU is a wrap.h declaration whose static initializer registers a
# wrapped C++ class. The TUs compile into the main binary (the linker
# always loads explicit objects, so the registrars run), and — in the
# JIT/AOT configuration below — into a `culebra_rt_wrap` archive that is
# embedded and force-loaded into every `culebra build` output, so AOT
# binaries register the same classes.
set(CULEBRA_WRAP_SOURCES "" CACHE STRING
  "Semicolon-separated wrap.h declaration TUs to compile in")
set(CULEBRA_WRAP_LINK "" CACHE STRING
  "Extra link flags for the wrapped libraries (e.g. -lmylib)")
if(CULEBRA_WRAP_SOURCES)
  target_sources(culebra PRIVATE ${CULEBRA_WRAP_SOURCES})
  if(CULEBRA_WRAP_LINK)
    separate_arguments(_wrap_link_args NATIVE_COMMAND "${CULEBRA_WRAP_LINK}")
    target_link_options(culebra PRIVATE ${_wrap_link_args})
  endif()
endif()

if(CULEBRA_ENABLE_JIT)
  target_link_libraries(culebra ${llvm_libs})
  # ORC JIT looks up culebra_runtime_* symbols in the host process at
  # codegen time. On macOS those symbols are visible by default; on
  # ELF/Linux they are stripped from the dynamic symbol table unless
  # we link with -rdynamic. ENABLE_EXPORTS adds the right flag per
  # platform (no-op on macOS).
  set_target_properties(culebra PROPERTIES ENABLE_EXPORTS ON)

  # `culebra wrap` rebuilds this tree with CULEBRA_WRAP_SOURCES; the
  # baked source path is the default when CULEBRA_HOME is unset.
  target_compile_definitions(culebra PRIVATE
    CULEBRA_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

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
  # Tensor is no longer an archive *variant*. The cblas-bearing body
  # lives in a single choke (culebra::tensor_eval_node); CULEBRA_RT_-
  # TENSOR_EVAL_WEAK compiles it as a weak throwing stub here, so the
  # core archive references no cblas symbol even though the full Tensor
  # namespace + culebra_runtime_tensor_* helpers stay real. The strong
  # body is force-loaded from culebra_rt_tensor (below) only when the
  # program uses Tensor — collapsing the tensor axis from 2 variants to
  # this one stub flag (see DESIGN_linear_rt.md).
  target_compile_definitions(culebra_rt PRIVATE
    LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
    CULEBRA_RT_DEFINE_RUNTIME=1
    CULEBRA_RT_TENSOR_EVAL_WEAK=1
    CULEBRA_RT_HTTP_REQUEST_WEAK=1
    CULEBRA_RT_COMPRESS_WEAK=1
    CULEBRA_RT_SQLITE_WEAK=1)
  # One section per function/data so the AOT link can `-dead_strip`
  # the ~200 `culebra_runtime_*` helpers a typical program doesn't
  # touch. Without this every runtime helper is in the same .text /
  # .data section and the linker can't drop them.
  target_compile_options(culebra_rt PRIVATE
    -ffunction-sections -fdata-sections)

  # Tensor feature object: the strong real tensor_eval_node (the lone
  # cblas anchor). Force-loaded into the AOT binary only when the AST
  # scan reports Tensor use (see materialize + -force_load in main.cc).
  add_library(culebra_rt_tensor STATIC ./src/runtime/culebra_rt_tensor.cc)
  target_compile_features(culebra_rt_tensor PRIVATE cxx_std_23)
  target_compile_options(culebra_rt_tensor PRIVATE
    -ffunction-sections -fdata-sections)

  # Http feature object: the strong real http_request (the lone OpenSSL/
  # zlib anchor — the only code that instantiates httplib::Client).
  # Force-loaded only when the AST scan reports Http use. Like the tensor
  # object, this folds the http axis into the core archive's weak stub,
  # so there is no longer an http archive *variant*: core + tensor + http
  # = 3 objects (N+1), regardless of how many heavy deps are added later
  # (was 2^N = 4 for these two).
  add_library(culebra_rt_http STATIC ./src/runtime/culebra_rt_http.cc)
  target_compile_features(culebra_rt_http PRIVATE cxx_std_23)
  target_compile_options(culebra_rt_http PRIVATE
    -ffunction-sections -fdata-sections)

  # Compress feature object: the strong real culebra::compress::gzip/gunzip
  # (the lone zlib anchor — the only code that calls deflate/inflate).
  # Force-loaded only when the AST scan reports Compress use, mirroring the
  # tensor/http objects, so a non-Compress binary references no libz symbol.
  add_library(culebra_rt_compress STATIC ./src/runtime/culebra_rt_compress.cc)
  target_compile_features(culebra_rt_compress PRIVATE cxx_std_23)
  target_compile_options(culebra_rt_compress PRIVATE
    -ffunction-sections -fdata-sections)

  # SQLite feature object: the strong real culebra::sqlite::* wrappers (the lone
  # sqlite3 anchor) plus the amalgamation object itself, so force-loading this
  # one archive into an AOT binary is fully self-contained. Force-loaded only
  # when the AST scan reports SQLite use, mirroring the tensor/http/compress
  # objects, so a non-SQLite binary references no sqlite3 symbol.
  add_library(culebra_rt_sqlite STATIC ./src/runtime/culebra_rt_sqlite.cc
    $<TARGET_OBJECTS:culebra_sqlite3>)
  target_compile_features(culebra_rt_sqlite PRIVATE cxx_std_23)
  target_compile_definitions(culebra_rt_sqlite PRIVATE CULEBRA_RT_SQLITE_STRONG=1)
  target_compile_options(culebra_rt_sqlite PRIVATE
    -ffunction-sections -fdata-sections)

  # Embed the runtime archives into the `culebra` driver via
  # cpp-embedlib. `culebra build` materializes the core archive (+ the
  # tensor / http feature objects when needed) to
  # ~/.cache/culebra/<fingerprint>/ on first use, so the driver is a
  # single self-contained binary. `--rt-lib=<path>` overrides the core
  # archive for cross-compile.
  #
  # cpp-embedlib computes relative paths at configure time, so passing
  # $<TARGET_FILE:...> generator expressions doesn't work. Reconstruct
  # the archive paths from CMake's static library naming convention
  # instead — these resolve to libculebra_rt*.a in the build dir, where
  # CMake writes STATIC libraries by default.
  set(_rt_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_tensor_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_tensor${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_http_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_http${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_compress_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_compress${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_sqlite_archive
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_sqlite${CMAKE_STATIC_LIBRARY_SUFFIX}")
  set(_rt_embed_files
    "${_rt_archive}"
    "${_rt_tensor_archive}"
    "${_rt_http_archive}"
    "${_rt_compress_archive}"
    "${_rt_sqlite_archive}")
  set(_rt_embed_deps culebra_rt culebra_rt_tensor culebra_rt_http
    culebra_rt_compress culebra_rt_sqlite)
  if(CULEBRA_WRAP_SOURCES)
    # Wrap declarations for AOT outputs. The TUs hold static registrars
    # that nothing references by name, so `culebra build` force-loads
    # this archive whenever it is present among the embedded assets.
    add_library(culebra_rt_wrap STATIC ${CULEBRA_WRAP_SOURCES})
    target_compile_features(culebra_rt_wrap PRIVATE cxx_std_23)
    target_compile_definitions(culebra_rt_wrap PRIVATE
      LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1)
    target_compile_options(culebra_rt_wrap PRIVATE
      -ffunction-sections -fdata-sections)
    list(APPEND _rt_embed_files
      "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_wrap${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND _rt_embed_deps culebra_rt_wrap)
    # The wrapped library's own link flags must reach AOT outputs too;
    # `culebra build` appends this verbatim (CULEBRA_BLAS_LINK pattern).
    target_compile_definitions(culebra PRIVATE
      CULEBRA_WRAP_LINK_FLAGS="${CULEBRA_WRAP_LINK}")
  else()
    target_compile_definitions(culebra PRIVATE CULEBRA_WRAP_LINK_FLAGS="")
  endif()
  # Graphics feature archive: the wrap registration (its static registrar pulls
  # in raylib). Force-loaded into an AOT binary only when the AST scan reports
  # Graphics use (raylib/SDL come from CULEBRA_GRAPHICS_LINK below) — the same
  # usage-gating as sqlite, but with no weak choke since Graphics simply isn't in
  # the base archive. Compiling needs only raylib's headers (vendored in-tree).
  if(CULEBRA_ENABLE_GRAPHICS)
    add_library(culebra_rt_graphics STATIC ./src/runtime/culebra_rt_graphics.cc)
    target_compile_features(culebra_rt_graphics PRIVATE cxx_std_23)
    target_include_directories(culebra_rt_graphics PRIVATE ./vendor/raylib/src)
    target_compile_definitions(culebra_rt_graphics PRIVATE CULEBRA_ENABLE_GRAPHICS=1)
    target_compile_options(culebra_rt_graphics PRIVATE -ffunction-sections -fdata-sections)
    list(APPEND _rt_embed_files
      "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_graphics${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND _rt_embed_deps culebra_rt_graphics)
  endif()
  # Webview feature archive: same usage-gating as Graphics (force-loaded into an
  # AOT binary only when the AST scan reports Webview use; the OS WebView
  # framework comes from CULEBRA_WEBVIEW_LINK). Compiling needs only the vendored
  # single header.
  if(CULEBRA_ENABLE_WEBVIEW)
    add_library(culebra_rt_webview STATIC ./src/runtime/culebra_rt_webview.cc)
    target_compile_features(culebra_rt_webview PRIVATE cxx_std_23)
    target_include_directories(culebra_rt_webview PRIVATE ./vendor/webview)
    target_compile_definitions(culebra_rt_webview PRIVATE CULEBRA_ENABLE_WEBVIEW=1)
    target_compile_options(culebra_rt_webview PRIVATE -ffunction-sections -fdata-sections)
    list(APPEND _rt_embed_files
      "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_webview${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND _rt_embed_deps culebra_rt_webview)
  endif()
  cpp_embedlib_add(culebra_rt_assets
    FILES ${_rt_embed_files}
    BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}"
    NAMESPACE CulebraRT)
  # cpp-embedlib's per-file DEPENDS is a file-level dependency. Force
  # the asset library to wait for the real archive targets too, so the
  # .a files actually exist when the embed step runs.
  add_dependencies(culebra_rt_assets ${_rt_embed_deps})
  target_link_libraries(culebra culebra_rt_assets)
  endif()
endif()

# Tensor primitive: since M4 the execution engine is cpp-tensorlib
# (lazy graph + fusion + Accelerate/Metal backends; own CPU/CUDA kernels
# on other platforms land in M5/M6). Header-only — the only link deps
# are macOS OS frameworks, which the static-link policy allows.
option(CULEBRA_ENABLE_TENSOR "Enable builtin Tensor type" ON)

if(CULEBRA_ENABLE_TENSOR)
  if(NOT EXISTS "${CULEBRA_TENSORLIB_DIR}/tensorlib.h")
    message(FATAL_ERROR
      "cpp-tensorlib headers not found at ${CULEBRA_TENSORLIB_DIR}. "
      "Run `git submodule update --init vendor/cpp-tensorlib`, or pass "
      "-DCULEBRA_TENSORLIB_DIR=<path> to a checkout.")
  endif()
  add_compile_definitions(CULEBRA_TENSOR_ENABLED)
  if(APPLE)
    set(_blas_link
        "-framework Accelerate -framework Metal -framework Foundation")
    target_link_libraries(culebra
        "-framework Accelerate" "-framework Metal" "-framework Foundation")
  else()
    set(_blas_link "")
  endif()
  # The `culebra build` driver appends this to the link command only
  # when the program references Tensor (see `aot_scan_namespaces`).
  # Programs that never touch Tensor avoid the framework dependency.
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

# zlib for the Compress namespace: always linked into the `culebra` driver (the
# in-process interp/JIT run gzip/gunzip directly). `culebra build` appends the
# AOT link fragment only when the program references Compress (see
# aot_uses_compress) — non-Compress AOT binaries omit libz.
target_link_libraries(culebra ZLIB::ZLIB)
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_ZLIB_LINK="${_zlib_link}")
endif()

# SQLite amalgamation linked into the `culebra` driver so the in-process interp
# and JIT run SQLite directly (the embedded culebra_rt_sqlite archive serves AOT
# outputs only). The amalgamation is self-contained on macOS; on Linux it pulls
# pthread/dl/m. `culebra build` appends CULEBRA_SQLITE_LINK only when the program
# references SQLite (see aot_uses_sqlite) — a non-SQLite AOT binary omits it.
target_link_libraries(culebra culebra_sqlite3)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  target_link_libraries(culebra Threads::Threads ${CMAKE_DL_LIBS})
  set(_sqlite_link "-lpthread -ldl -lm")
else()
  set(_sqlite_link "")
endif()

# Windows (mingw-w64 for Phase 1): link winpthread for std::thread (the posix
# thread model libstdc++ uses) and Winsock for cpp-httplib. http.h is always
# compiled into the driver, so ws2_32 is linked unconditionally; harmless when
# the Http namespace is disabled. (TLS via OpenSSL is a separate opt-in — see the
# CULEBRA_ENABLE_HTTP block; Phase 1 Windows builds run without it.)
if(WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(culebra Threads::Threads ws2_32)
  # std::print/println (C++23) resolves the Windows console at runtime via
  # __open_terminal / __write_to_terminal, which libstdc++ ships in the separate
  # experimental library — not pulled in by -static-libstdc++. Link it explicitly
  # (before libstdc++, which the g++ driver appends last) so std::print links.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_libraries(culebra stdc++exp)
  endif()
endif()
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_SQLITE_LINK="${_sqlite_link}")
endif()

# `culebra build` appends the raylib + SDL2 statics + frameworks only when the
# program references Graphics (see aot_scan). Defined unconditionally (gated on
# JIT) like the peer link fragments above so the fast inner-loop build
# (CULEBRA_DEV_NO_RT) — which skips the AOT runtime archives — still compiles the
# driver that references this macro. `_graphics_link` is "" when Graphics is off.
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_GRAPHICS_LINK="${_graphics_link}")
endif()

# `culebra build` appends the OS WebView framework only when the program
# references Webview (see aot_scan). Gated on JIT like the peers so the
# CULEBRA_DEV_NO_RT inner-loop build still compiles the driver. "" when off.
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_WEBVIEW_LINK="${_webview_link}")
endif()

# --- Graphics: build SDL2 + raylib + link into the driver -----------------
# Config (option / static lib paths / link flags) was resolved before the driver
# target above. Here we build SDL2 (static) and raylib (SDL backend, static) from
# vendored submodules, then compile the Graphics binding straight into the driver
# so interp + JIT run Graphics in-process. AOT outputs instead force-load the
# feature archive + CULEBRA_GRAPHICS_LINK (gated on usage). raylib finds the
# vendored SDL via CMAKE_PREFIX_PATH; both statics + frameworks are supplied at
# the final link (a static archive records no link deps of its own).
if(CULEBRA_ENABLE_GRAPHICS)
  include(ExternalProject)
  ExternalProject_Add(sdl_ext
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL"
    PREFIX "${CMAKE_CURRENT_BINARY_DIR}/sdl_ext_build"
    CMAKE_ARGS
      -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST=OFF
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_INSTALL_PREFIX=${_sdl_install}
      -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
      -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
      -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
    BUILD_BYPRODUCTS "${_sdl_a}")

  ExternalProject_Add(raylib_ext
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/raylib"
    PREFIX "${CMAKE_CURRENT_BINARY_DIR}/raylib_ext_build"
    DEPENDS sdl_ext
    CMAKE_ARGS
      -DPLATFORM=SDL -DBUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_PREFIX_PATH=${_sdl_install}
      -DCMAKE_INSTALL_PREFIX=${_raylib_install}
      -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
      -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
      -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
    BUILD_BYPRODUCTS "${_raylib_a}")

  target_sources(culebra PRIVATE ./src/runtime/culebra_rt_graphics.cc)
  target_include_directories(culebra PRIVATE ./vendor/raylib/src)
  target_compile_definitions(culebra PRIVATE CULEBRA_ENABLE_GRAPHICS=1)
  add_dependencies(culebra raylib_ext)
  target_link_libraries(culebra "${_raylib_a}" "${_sdl_a}")
  if(APPLE)
    foreach(_fw IN LISTS _graphics_frameworks)   # same single source as _graphics_link
      target_link_libraries(culebra "-framework ${_fw}")
    endforeach()
  endif()
endif()

# --- Webview: compile the binding into the driver -------------------------
# No static library to build (webview is a vendored header + the OS's native
# WebView framework). Compile the binding straight into the driver so interp +
# JIT run Webview in-process; AOT outputs force-load the feature archive +
# CULEBRA_WEBVIEW_LINK (gated on usage). The header stays plain-C++ compilable
# (it drives the ObjC runtime via objc_msgSend on macOS, so no .mm needed).
if(CULEBRA_ENABLE_WEBVIEW)
  target_sources(culebra PRIVATE ./src/runtime/culebra_rt_webview.cc)
  target_include_directories(culebra PRIVATE ./vendor/webview)
  target_compile_definitions(culebra PRIVATE CULEBRA_ENABLE_WEBVIEW=1)
  if(APPLE)
    target_link_libraries(culebra "-framework WebKit" ${CMAKE_DL_LIBS})
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(WEBKITGTK_FOUND)
      target_include_directories(culebra PRIVATE ${WEBKITGTK_INCLUDE_DIRS})
      target_link_libraries(culebra ${WEBKITGTK_LDFLAGS} ${CMAKE_DL_LIBS})
    endif()
  endif()
endif()

# Embedding smoke tests. mt_smoke exercises thread_local isolation
# (parser, GC, exception state); mi_smoke exercises RuntimeScope and
# per-Runtime hooks. Build them alongside `culebra` so they stay in
# step with the headers.
enable_testing()

# One UNITY executable for all five smokes (tests/embedding/smoke_suite.cc
# #includes each smoke .cc). Every smoke TU pulls in the interpreter +
# JIT header stack whose codegen costs minutes on the 3-core macOS CI
# runner; as five separate executables the smokes were ~28 of the ~33 CI
# build minutes whenever a core header changed — the unity TU emits that
# once. ctest still runs each smoke as its own PROCESS (argv selects it),
# so signal handlers / thread_locals / GC state stay isolated per test.
add_executable(smoke_suite tests/embedding/smoke_suite.cc)
target_compile_features(smoke_suite PRIVATE cxx_std_23)
if(CULEBRA_ENABLE_TENSOR AND APPLE)
  # cpp-tensorlib backends (in-process embedding uses tensors directly)
  target_link_libraries(smoke_suite
      "-framework Accelerate" "-framework Metal" "-framework Foundation")
endif()
if(CULEBRA_ENABLE_JIT)
  target_link_libraries(smoke_suite ${llvm_libs})
  # The in-process JIT (mt/mi/signal) resolves the `used`-pinned runtime
  # helpers from the host binary via ORC — keep them exported.
  set_target_properties(smoke_suite PROPERTIES ENABLE_EXPORTS ON)
endif()
if(CULEBRA_ENABLE_HTTP)
  target_link_libraries(smoke_suite ${_ssl_libs})
endif()
# The embedding TUs include stdlib_interp.h -> compress.h inline, which calls
# zlib directly, so the suite needs libz.
target_link_libraries(smoke_suite ZLIB::ZLIB)
# stdlib_interp.h -> sqlite.h inline wrappers call sqlite3_* directly.
target_link_libraries(smoke_suite culebra_sqlite3)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(smoke_suite Threads::Threads ${CMAKE_DL_LIBS})
endif()
foreach(smoke mt mi define signal utf8_invalid)
  add_test(NAME ${smoke}_smoke COMMAND smoke_suite ${smoke})
endforeach()

# Headless DAP adapter test — drives `culebra dap` over a pipe with a scripted
# debug session. Standalone (no culebra linkage); spawns the binary at runtime.
add_executable(dap_test tests/dap_test.cc)
target_compile_features(dap_test PRIVATE cxx_std_23)
add_test(NAME dap_test COMMAND dap_test $<TARGET_FILE:culebra>)

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

# sqlite.h core test: drives the value-neutral cursor API (open / prepare /
# bind / step / column / type-mapping / errors) against an in-memory database,
# fully deterministic and offline. Links the amalgamation directly (the test TU
# includes sqlite.h with inline bodies).
add_executable(sqlite_test test/sqlite_test.cc)
target_compile_features(sqlite_test PRIVATE cxx_std_23)
target_link_libraries(sqlite_test culebra_sqlite3)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(sqlite_test Threads::Threads ${CMAKE_DL_LIBS})
endif()
add_test(NAME sqlite_test COMMAND sqlite_test)

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

# Source formatter (include/formatter.h): golden-fixture output, the comment
# guard (commented files are left untouched in Phase 0), and a corpus sweep
# asserting the re-parse safety net never refuses and formatting is idempotent.
add_test(NAME fmt_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/fmt_test.sh
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

# IO.stdin() reader handle — .lines() / .read() / .read(n) over a real pipe,
# across interp / JIT / AOT (a .cul sweep test can't feed stdin or use
# Sys.executable as an interpreter under AOT).
add_test(NAME io_stdin_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/io_stdin_test.sh
                 $<TARGET_FILE:culebra>)

# Log records on stderr — content (levels, fields, filtering, child binding)
# across interp / JIT / AOT (logs never reach stdout, so the .cul sweep can't
# verify them).
add_test(NAME log_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/log_test.sh
                 $<TARGET_FILE:culebra>)
