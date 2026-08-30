cmake_minimum_required(VERSION 3.20)
project(culebra)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CULEBRA_ENABLE_JIT "Enable LLVM JIT compilation" OFF)

# Minimum LLVM version. jit.h uses ORC APIs that landed in LLVM 20;
# routinely tested against current (22.x). Override with
# -DCULEBRA_MIN_LLVM_VERSION=<n> if needed.
set(CULEBRA_MIN_LLVM_VERSION 20 CACHE STRING
    "Minimum required LLVM major version for the JIT build")

if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")
endif()

# Two clang diagnostics fire ~50 times across the JIT runtime headers without
# ever pointing at a defect, which buries the warnings that do mean something.
# Both spellings are clang-only (and -Wvla-cxx-extension is clang 18+), so ask
# the compiler rather than assuming. playground/build.sh passes the same pair.
#
#   return-type-c-linkage  Helpers sit inside the big `extern "C"` block for
#       its name mangling while returning std::optional / std::string. The
#       warning is about calling them from C, which nothing does — JIT'd code
#       only ever calls the C-ABI subset.
#   vla-cxx-extension  vm.h sizes the register window from the chunk on
#       purpose (see run_frame); kMaxSlots bounds it.
include(CheckCXXCompilerFlag)
foreach(_w -Wno-return-type-c-linkage -Wno-vla-cxx-extension)
  string(MAKE_C_IDENTIFIER "HAS${_w}" _have)
  check_cxx_compiler_flag("${_w}" ${_have})
  if(${_have})
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${_w}>")
  endif()
endforeach()

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

# LTO gives the JIT runtime a permanent +6.7% (cross-TU inlining of codegen);
# what it saves on size is a rounding error beside the dead-strip. It costs a
# relink of the whole driver — disable with `-DCULEBRA_LTO=OFF` for fast local
# iterations.
#
# IMPORTANT: applied only to the `culebra` executable target, not to
# `culebra_rt` / the feature objects. Those archives ship LTO
# bitcode that the end user's compiler links into AOT-built binaries —
# the user's compiler may be a different toolchain/version (GCC 13 vs
# 14, Clang vs GCC) and chokes on cross-version LTO bitcode. Object
# code only for archives keeps `culebra build` portable.
option(CULEBRA_LTO "Enable LTO for the culebra driver (Release)" ON)
# Inner-loop dev switch: skip building+embedding the four AOT runtime
# archives. Each archive is a full recompile of culebra_rt.cc (which
# pulls in the interpreter + JIT headers), so embedding all four turns a
# single-TU header touch into a five-TU rebuild. With this ON, only
# main.cc recompiles + relinks; `culebra build` (AOT) then errors
# cleanly. `just dev` passes this; `just build`/`just test all` do not.
option(CULEBRA_DEV_NO_RT "Skip AOT runtime archives for fast inner-loop builds" OFF)
# The other inner-loop dev switch: two thirds of the -O3 compile of main.cc is
# the optimizer, and none of it shows in test wall-clock (the JIT's codegen is
# in prebuilt LLVM). -O0 is faster still but slows the interpreter 2.4x.
# A build with this ON is not a build to benchmark with.
option(CULEBRA_DEV_O1 "Compile at -O1 for fast inner-loop rebuilds" OFF)
if(CULEBRA_DEV_O1)
  string(REGEX REPLACE "-O[0-9gsz]+" "-O1"
         CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
  string(REGEX REPLACE "-O[0-9gsz]+" "-O1"
         CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
endif()
# Coverage build for the Phase 4 question in docs/internals/vm.md §7: which
# of the surface the two compiled engines share — the bytecode compiler, the
# shared C++ runtime fragments, the shared emitter — is reached only by the
# generated corpus. That surface is header-only and lands in the driver TU, so
# the flags go on the `culebra` target alone and vendor targets stay clean.
# -O0 leaves every `inline` helper a function of its own, which is the
# granularity the ratchet is keyed on (-fno-inline is belt: nothing here is
# always_inline, and GCC does not inline at -O0); -fprofile-update=atomic keeps
# the isolates from losing counts to a non-atomic increment.
option(CULEBRA_COVERAGE "Instrument the driver for gcov (docs/internals/vm.md)" OFF)

# `just asan` is the one entry point that sanitizes, and it passes the flags
# through CMAKE_CXX_FLAGS. Decided once here because the driver's link options
# key off it in two places.
if(CMAKE_CXX_FLAGS MATCHES "sanitize")
  set(_culebra_sanitized TRUE)
else()
  set(_culebra_sanitized FALSE)
endif()
# Sanitized and coverage builds want the same two things a shipped driver does
# not: no whole-program transform, and every symbol kept. Deriving it once
# means a third instrumented mode adds one name here rather than three guards.
if(_culebra_sanitized OR CULEBRA_COVERAGE)
  set(_culebra_diagnostic_build TRUE)
else()
  set(_culebra_diagnostic_build FALSE)
endif()
include(CheckIPOSupported)
check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_err)
if(CULEBRA_LTO AND NOT _culebra_diagnostic_build AND _ipo_supported
   AND CMAKE_BUILD_TYPE STREQUAL "Release")
  set(_culebra_enable_lto TRUE)
else()
  set(_culebra_enable_lto FALSE)
endif()

include_directories(./include)
# cpp-tensorlib (header-only): the Tensor execution engine (lazy graph,
# fusion, Accelerate/Metal backends). Vendored as a git submodule under
# vendor/ so a fresh clone builds with `--recursive`. Override
# CULEBRA_TENSORLIB_DIR to point at a sibling checkout for co-development.
# Presence is checked in the Tensor block below.
set(CULEBRA_TENSORLIB_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-tensorlib/include"
    CACHE PATH "cpp-tensorlib include directory")
include_directories(${CULEBRA_TENSORLIB_DIR})
include_directories(./vendor/cpp-peglib)
include_directories(./vendor/cpp-linenoise)
include_directories(./vendor/cpp-unicodelib)
include_directories(./vendor/cpp-regexlib)
include_directories(./vendor/cpp-httplib)
# stb_image (vendor/stb/stb_image.h, public domain) — the PNG decoder behind
# Canvas.Sprite.from_png. Header-only with its own inflate, so it adds no link
# dependency; see include/image.h for how it is compiled in. stb_truetype
# (vendor/stb/stb_truetype.h, same licence) is the TTF/OTF rasterizer behind
# Canvas.Font, same shape — header-only, no link dependency; see
# include/font_ttf.h.
include_directories(./vendor/stb)
# Builtin SQLite namespace. OFF drops the amalgamation entirely (no sqlite3
# object, no link, no `SQLite` name in either backend) — the WASM playground
# build uses this to shed ~1.2 MB. Mirrors the CULEBRA_ENABLE_HTTP gate.
option(CULEBRA_ENABLE_SQLITE "Enable builtin SQLite namespace" ON)
if(CULEBRA_ENABLE_SQLITE)
add_compile_definitions(CULEBRA_SQLITE_ENABLED)
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
endif()

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
  # Pin the Windows API baseline on the command line so it is set before ANY
  # header. mingw's <windows.h> locks _WIN32_WINNT to its (lower) default on the
  # first windows-header inclusion in a TU, and that can happen (via winpthread
  # etc.) before os_compat.h's #ifndef floor runs — leaving version-gated APIs
  # like GetCurrentThreadStackLimits (conservative-GC stack_base, needs 0x0602)
  # undeclared. Windows 10 (0x0A00) is the realistic floor.
  add_compile_definitions(_WIN32_WINNT=0x0A00)
  # 16MB PE stack reserve: mingw's 2MB default holds ~400 interpreter eval
  # frames, while the recursion limit (shared.h kCulebraRecursionLimit) is
  # calibrated for an 8MB stack. Every thread inherits the reserve, so this
  # also covers isolates/workers (SizedThread's Windows arm relies on it).
  # `culebra build` passes the same flag to AOT links (src/main.cc).
  add_link_options(-Wl,--stack,16777216)
endif()
if(APPLE)
  # The same 16MB, for the one thread SizedThread cannot size: macOS fixes the
  # main thread's stack at exec time from the LC_MAIN header, and its 8MB
  # default is under what the interpreter needs. A 1000-frame sweep
  # (kCulebraRecursionLimit) measures 6.6MB built with gcc-14 -O3+LTO and
  # overruns 8MB built with clang, which is why the macOS CI lane crashed in
  # tests/test_recursion_limit.cul while every Linux lane passed. Only the main
  # thread reads this; spawned threads carry SizedThread's own size, and AOT
  # binaries need neither (compiled frames are ~350B — the same sweep runs in
  # under 1MB).
  add_link_options(-Wl,-stack_size,0x1000000)
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
  # culebra_rt_http (the Http feature archive), which compiles httplib.h but
  # doesn't link it (the AOT consumer's link resolves the symbols via
  # _ssl_link). The core archive (culebra_rt) no longer includes httplib.h at
  # all — see http.h's CULEBRA_RT_HTTP_REQUEST_WEAK include guard — so it needs
  # neither these headers nor _ssl_link. zlib headers are added unconditionally
  # above.
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
  elseif(WIN32)
    # Static OpenSSL takes its entropy and its trust store from the OS:
    # BCryptGenRandom (bcrypt), the system root store (crypt32), and sockets
    # (ws2_32). MSVC picks these up from OpenSSL's own #pragma comment(lib);
    # mingw has to be told.
    list(APPEND _ssl_libs crypt32 bcrypt ws2_32)
  endif()

  # AOT link string: the OpenSSL statics by embedded name, not by the path they
  # sit at here — `culebra build` appends this on the *user's* machine, which
  # has no such path (see _rt_embed_extern). Mirrors CULEBRA_BLAS_LINK for
  # Tensor. zlib is linked dynamically (-lz).
  cmake_path(GET OPENSSL_SSL_LIBRARY FILENAME _ssl_a_name)
  cmake_path(GET OPENSSL_CRYPTO_LIBRARY FILENAME _crypto_a_name)
  set(_ssl_link "@${_ssl_a_name}@ @${_crypto_a_name}@ -lz")
  if(APPLE)
    set(_ssl_link "${_ssl_link} -framework CoreFoundation -framework Security")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_ssl_link "${_ssl_link} -ldl -lpthread")
  elseif(WIN32)
    set(_ssl_link "${_ssl_link} -lcrypt32 -lbcrypt -lws2_32")
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
  # LLVM_DEFINITIONS is a single space-joined string on some toolchains (msys2
  # mingw LLVM: "-D_GLIBCXX_USE_CXX11_ABI=1 -D_FILE_OFFSET_BITS=64 ..."). Passed
  # straight to add_definitions() it becomes ONE -D, quoting the trailing flags
  # into the first macro's value, so `#if _GLIBCXX_USE_CXX11_ABI` then sees
  # "1 -D..." and every compile fails ("token '=' is not valid in preprocessor
  # expressions"). Split into separate arguments first (a no-op on toolchains
  # where it is already a clean list).
  separate_arguments(_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")
  add_definitions(${_llvm_defs})

  # --- lld, linked into the driver (Windows) --------------------------------
  # `culebra build` links through an external C++ driver everywhere else, and
  # that is fine where one exists: macOS has the CLT shim, Linux ships gcc in
  # every base image worth the name. Windows ships no toolchain at all, so the
  # link step is the one thing a downloaded culebra.exe cannot do — and what it
  # is missing is a linker, not a compiler, because the object is emitted
  # in-process already.
  #
  # Carrying lld answers that without carrying a toolchain. What remains on the
  # user's side is the mingw half of a link (CRT objects, libstdc++, import
  # libraries), which misc/pack_windows_toolchain.sh packs and `culebra
  # toolchain install` fetches — 8 MB compressed.
  #
  # Sharing this binary's LLVM is what makes that cheap: linking lld here costs
  # +23 MB on the download, more than the 4 MB of lld archives, because lld's
  # COFF driver references every LLVM backend. The alternatives were measured
  # rather than assumed — docs/deployment.md §"Why Windows needs no compiler".
  #
  # Found by library and header rather than through LLDConfig.cmake: MSYS2
  # ships the archives and lld/Common/Driver.h without always shipping the
  # CMake package, and the archives are what this needs. Absence is not fatal —
  # a build without them keeps the external-driver path, which is what a local
  # MSYS2 developer has anyway.
  #
  # Resolved here, before the target list below, because linking lld decides
  # that list: its COFF driver calls InitializeAllTargets*() for LTO inputs, so
  # every backend has to be present or the driver does not link.
  set(CULEBRA_LLD_LIBS "")
  if(WIN32)
    find_library(CULEBRA_LLD_MINGW  NAMES lldMinGW  HINTS ${LLVM_LIBRARY_DIRS})
    find_library(CULEBRA_LLD_COFF   NAMES lldCOFF   HINTS ${LLVM_LIBRARY_DIRS})
    find_library(CULEBRA_LLD_COMMON NAMES lldCommon HINTS ${LLVM_LIBRARY_DIRS})
    find_path(CULEBRA_LLD_INCLUDE NAMES lld/Common/Driver.h
              HINTS ${LLVM_INCLUDE_DIRS})
    if(CULEBRA_LLD_MINGW AND CULEBRA_LLD_COFF AND CULEBRA_LLD_COMMON
       AND CULEBRA_LLD_INCLUDE)
      # MinGW before COFF before Common: the mingw driver translates GNU-style
      # arguments and calls the COFF one, and both reach Common.
      set(CULEBRA_LLD_LIBS "${CULEBRA_LLD_MINGW}" "${CULEBRA_LLD_COFF}"
                           "${CULEBRA_LLD_COMMON}")
      add_compile_definitions(CULEBRA_INPROCESS_LLD)
      include_directories(${CULEBRA_LLD_INCLUDE})
      message(STATUS "lld: linked into the driver (AOT needs no external linker)")
    else()
      message(STATUS "lld: not found as a library — `culebra build` will need "
                     "an external clang++ on PATH")
    endif()
  endif()

  # Native target keeps the JIT lean. WebAssembly is bundled so the
  # docs-site WASM playground (see project-docs-site memory) can be
  # produced via `culebra build --target=wasm32-…` from the same binary.
  # AllTargets* extend codegen + asm tables for any other LLVM target —
  # opt-in because they add ~31 MB to the `culebra` driver. AOT-built
  # executables never carry LLVM regardless.
  option(CULEBRA_LLVM_ALL_TARGETS
    "Link all LLVM backends for cross-compile (adds ~31 MB)" OFF)
  # Not a choice where lld is linked in: lld's COFF driver initializes every
  # backend so it can accept LTO bitcode, and those calls are unconditional, so
  # the backends have to be there for the driver to link at all. Forced rather
  # than made a requirement of the caller, because a configure that quietly
  # dropped in-process lld would produce a Windows binary that cannot link
  # without MSYS2 — the exact failure this feature exists to remove.
  if(CULEBRA_LLD_LIBS AND NOT CULEBRA_LLVM_ALL_TARGETS)
    set(CULEBRA_LLVM_ALL_TARGETS ON)
    message(STATUS "LLVM backends: all (lld's COFF driver references each one)")
  endif()
  if(CULEBRA_LLVM_ALL_TARGETS)
    set(_extra_target_libs
      AllTargetsAsmParsers AllTargetsCodeGens AllTargetsDescs
      AllTargetsInfos)
    add_compile_definitions(CULEBRA_LLVM_ALL_TARGETS)
  else()
    set(_extra_target_libs
      WebAssemblyAsmParser WebAssemblyCodeGen WebAssemblyDesc
      WebAssemblyInfo)
    if(NOT "WebAssembly" IN_LIST LLVM_TARGETS_TO_BUILD)
      message(FATAL_ERROR
        "This LLVM build was not configured with the WebAssembly target "
        "(LLVM_TARGETS_TO_BUILD lacks it), but jit.h always registers it "
        "for `culebra build --target=wasm32-…`. Either install/build an "
        "LLVM with WebAssembly support, or pass "
        "-DCULEBRA_LLVM_ALL_TARGETS=ON to link every backend instead.")
    endif()
  endif()
  # What lld's COFF driver reaches that the JIT never does. libdriver is the
  # `/lib` archive mode it links in whether or not anyone asks for it, and
  # dtlto is distributed ThinLTO, which LLVM 22 keeps in its own library rather
  # than in LLVMLTO. Getting this list wrong is loud — the driver fails to link
  # with the symbols named — so there is no silent-fallback case to guard, and
  # `grep <mangled name> $(llvm-config --libdir)/*.a` is how to answer the next
  # one.
  set(_lld_components "")
  if(CULEBRA_LLD_LIBS)
    set(_lld_components
      lto dtlto option object binaryformat targetparser libdriver
      windowsdriver windowsmanifest debuginfopdb debuginfocodeview)
  endif()
  llvm_map_components_to_libnames(llvm_libs
    core support orcjit native passes ${_extra_target_libs} ${_lld_components})
  if(CULEBRA_LLD_LIBS)
    message(STATUS "lld's extra LLVM components: ${_lld_components}")
  endif()

  # Some distros (e.g. Arch Linux) package only the combined libLLVM.so
  # and omit per-component static archives — or ship a handful of stale
  # leftover ones `ld` can't even parse (see culebra issue #13). Detect
  # that by checking whether the core component's static archive is
  # actually installed; if not, fall back to the single shared library
  # that LLVMConfig.cmake exposes as the imported target `LLVM`.
  option(CULEBRA_LLVM_SHARED
    "Force linking against libLLVM.so instead of per-component static \
libs (auto-detected when the static libs are missing)" OFF)
  find_library(CULEBRA_LLVM_CORE_ARCHIVE NAMES LLVMCore
    PATHS ${LLVM_LIBRARY_DIRS} NO_DEFAULT_PATH)
  if(CULEBRA_LLVM_SHARED OR NOT CULEBRA_LLVM_CORE_ARCHIVE)
    if(NOT TARGET LLVM)
      message(FATAL_ERROR
        "Per-component LLVM static libraries were not found under "
        "${LLVM_LIBRARY_DIRS}, and no combined libLLVM shared library "
        "is available either (no `LLVM` imported target). This LLVM "
        "install looks incomplete — reinstall LLVM ${CULEBRA_MIN_LLVM_VERSION}+ "
        "or point CMAKE_PREFIX_PATH at a complete one.")
    endif()
    message(STATUS "Linking against the shared libLLVM (no per-component static libs found)")
    set(llvm_libs LLVM)
  endif()

  # LLVMSupport's exported interface names zstd::libzstd_shared, so even a
  # -static link imports libzstd.dll — which a downloaded .exe won't find on a
  # plain Windows box. The same package ships the static archive; point the
  # imported target's import library at it. Windows-only: elsewhere a shared
  # zstd is a normal system library.
  if(WIN32 AND TARGET zstd::libzstd_shared)
    find_library(CULEBRA_ZSTD_STATIC NAMES libzstd.a zstd_static)
    if(CULEBRA_ZSTD_STATIC)
      # Per-configuration properties too: a plain IMPORTED_IMPLIB is ignored
      # whenever an IMPORTED_IMPLIB_<CONFIG> exists, and zstd's config package
      # sets those.
      set(_zstd_props IMPORTED_IMPLIB "${CULEBRA_ZSTD_STATIC}"
                      IMPORTED_LOCATION "${CULEBRA_ZSTD_STATIC}")
      get_target_property(_zstd_cfgs zstd::libzstd_shared IMPORTED_CONFIGURATIONS)
      foreach(_c IN LISTS _zstd_cfgs)
        list(APPEND _zstd_props IMPORTED_IMPLIB_${_c} "${CULEBRA_ZSTD_STATIC}"
                                IMPORTED_LOCATION_${_c} "${CULEBRA_ZSTD_STATIC}")
      endforeach()
      set_target_properties(zstd::libzstd_shared PROPERTIES ${_zstd_props})
      message(STATUS "zstd: linking ${CULEBRA_ZSTD_STATIC} statically (LLVM dep)")
    endif()
  endif()

  # The same shape one library further out, and only where lld is linked in:
  # lld's COFF driver embeds manifests through LLVMWindowsManifest, which is
  # built against libxml2, and LLVM's config finds the import library — so a
  # -static culebra.exe ends up importing libxml2-16.dll, which is exactly the
  # DLL a downloaded binary does not have. misc/verify_standalone_exe.sh is
  # what caught it.
  #
  # A static libxml2 stops hiding its own dependencies behind a DLL, so iconv
  # and lzma have to be named here; they are what libxml2's .pc lists as
  # private.
  #
  # --whole-archive, and not as a precaution: LLVMWindowsManifest.a was built
  # against the shared libxml2, so it references __imp_xmlFoo. lld's MinGW mode
  # will alias that to xmlFoo — but only once xmlFoo is defined, and pulling an
  # archive member happens by matching the undefined name, which __imp_xmlFoo
  # never does. So the members have to come in unconditionally for the aliasing
  # to have anything to alias.
  set(CULEBRA_LIBXML2_DEPS "")
  if(CULEBRA_LLD_LIBS AND TARGET LibXml2::LibXml2)
    find_library(CULEBRA_LIBXML2_STATIC NAMES libxml2.a)
    find_library(CULEBRA_ICONV_STATIC NAMES libiconv.a)
    find_library(CULEBRA_LZMA_STATIC NAMES liblzma.a)
    if(CULEBRA_LIBXML2_STATIC)
      set(_xml_props IMPORTED_IMPLIB "${CULEBRA_LIBXML2_STATIC}"
                     IMPORTED_LOCATION "${CULEBRA_LIBXML2_STATIC}")
      get_target_property(_xml_cfgs LibXml2::LibXml2 IMPORTED_CONFIGURATIONS)
      foreach(_c IN LISTS _xml_cfgs)
        list(APPEND _xml_props IMPORTED_IMPLIB_${_c} "${CULEBRA_LIBXML2_STATIC}"
                               IMPORTED_LOCATION_${_c} "${CULEBRA_LIBXML2_STATIC}")
      endforeach()
      set_target_properties(LibXml2::LibXml2 PROPERTIES ${_xml_props})
      list(APPEND CULEBRA_LIBXML2_DEPS
           "-Wl,--whole-archive" "${CULEBRA_LIBXML2_STATIC}"
           "-Wl,--no-whole-archive")
      foreach(_d CULEBRA_ICONV_STATIC CULEBRA_LZMA_STATIC)
        if(${_d})
          list(APPEND CULEBRA_LIBXML2_DEPS "${${_d}}")
        endif()
      endforeach()
      # xmlInitRandom seeds itself from BCryptGenRandom. culebra.exe happens to
      # reach bcrypt by another route, so its own link never says so — named
      # here because this list is libxml2's requirements, not the ones this
      # binary would otherwise notice.
      list(APPEND CULEBRA_LIBXML2_DEPS "-lbcrypt")
      message(STATUS "libxml2: linking ${CULEBRA_LIBXML2_STATIC} statically (lld dep)")
    else()
      message(WARNING
        "no static libxml2 beside the shared one — culebra.exe will import "
        "libxml2's DLL, which a downloaded binary will not find")
    endif()
  endif()

  add_compile_definitions(CULEBRA_JIT_ENABLED)
endif()

# The commit a vendored submodule is checked out at, or "" when that can't be
# determined (source tarball, uninitialized submodule). Keys the shared
# dependency cache below. The .git check matters: `git -C` inside an empty
# submodule dir would answer with the *superproject's* HEAD, which changes on
# every culebra commit.
function(_culebra_dep_rev out dir)
  set(rev "")
  find_program(GIT_EXE git)
  if(GIT_EXE AND EXISTS "${dir}/.git")
    execute_process(COMMAND ${GIT_EXE} -C "${dir}" rev-parse --short=12 HEAD
                    OUTPUT_VARIABLE rev OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET)
  endif()
  set(${out} "${rev}" PARENT_SCOPE)
endfunction()

# Publish, in `tgt`'s dynamic symbol table, the names ORC JIT resolves out of the
# host process at codegen time. Every executable that hosts the in-process JIT
# needs this and needs it the same way, so both call here rather than each
# reaching for a platform flag.
#
# macOS: visible by default, ENABLE_EXPORTS is the portable way to say so.
#
# Windows: ENABLE_EXPORTS is a no-op for a MinGW .exe, so GetProcAddress over
# this image (how ORC's process-symbol resolver works there) finds nothing.
# --export-all-symbols is the analog of -rdynamic, but exporting *everything*
# overflows the 65535-entry PE export table once LLVM's ~80k static symbols are
# counted ("export ordinal too large"). GNU ld could be narrowed with
# --exclude-libs ALL; lld, which the release build links with (see
# misc/configure_windows_release.sh), has no such option. So the target's own
# objects are read with nm just before the link and the names the whitelist
# below matches written to a .def (cmake/gen_pe_exports.cmake) — the same list,
# read from the same file, in a form both linkers take. The C++ EH entry points the
# JIT'd landingpads reference live in libstdc++/libgcc, outside that list; they
# are supplied to the JIT as absolute symbols (JIT::define_windows_eh_symbols).
#
# ELF: -rdynamic would work and is actively harmful. A symbol in the
# executable's dynamic table takes precedence over the same name in every shared
# object, including ones the OS dlopens into us later, and -rdynamic published
# all ~67k symbols the link pulled in — LLVM's among them. Mesa's DRI drivers
# link their own LLVM (Gallium), so loading one bound its 81 LLVM imports to
# culebra's LLVM 22 instead of the libLLVM it was built against and the first GL
# context a Canvas window created segfaulted inside Mesa. OpenSSL, zlib and
# cpp-peglib are the same hazard waiting for a library that wants those names.
# Publish a whitelist instead — see cmake/exported_symbols.txt.
function(culebra_export_jit_symbols tgt)
  if(WIN32)
    find_program(CULEBRA_NM NAMES nm llvm-nm REQUIRED)
    set(_def "${CMAKE_CURRENT_BINARY_DIR}/${tgt}_exports.def")
    add_custom_command(TARGET ${tgt} PRE_LINK
      COMMAND ${CMAKE_COMMAND} -DNM=${CULEBRA_NM} -DOUT=${_def}
              -DLIST=${CMAKE_CURRENT_SOURCE_DIR}/cmake/exported_symbols.txt
              "-DOBJECTS=$<TARGET_OBJECTS:${tgt}>"
              -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_pe_exports.cmake
      COMMENT "Listing ${tgt}'s exported symbols for its PE export table"
      VERBATIM)
    target_link_options(${tgt} PRIVATE "${_def}")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(${tgt} PRIVATE
      "-Wl,--dynamic-list=${CMAKE_CURRENT_SOURCE_DIR}/cmake/exported_symbols.txt")
  else()
    set_target_properties(${tgt} PROPERTIES ENABLE_EXPORTS ON)
  endif()
endfunction()

# --- Builtin Scene namespace (raylib + SDL): config ---------------------
# A 3D namespace nothing else needs, so OFF by default (normal builds + CI stay
# lean). Computed before the runtime/embed block so the feature archive + the
# AOT link string can be registered there. Both SDL3 and raylib (SDL backend)
# are built statically from vendored submodules further below; here we only
# resolve paths + flags. Static SDL3 = fully self-contained binaries (no external
# dylib path baked in) and keeps gamepad rumble across platforms — vs GLFW, whose
# macOS gamepad subsystem is broken and which has no haptics API at all.
option(CULEBRA_ENABLE_SCENE "Enable builtin Scene namespace (raylib + SDL)" OFF)
# The native Canvas desktop window (2D framebuffer) rides on the same vendored
# raylib + SDL3 statics as the 3D Scene namespace. ON wherever a window is known
# to work — macOS, Linux and Windows — so a plain `just build` runs a Canvas
# game in a window with no knob to remember. It costs nothing at runtime: the
# framebuffer core is identical and CULEBRA_CANVAS_HEADLESS turns the window off
# per run, no rebuild. Nor does it cost anything to a displayless machine: SDL3
# dlopens X11/GL/audio (opengl32.dll on Windows) on first use, so a window build
# adds no load-time dependency on them and still starts on a headless server —
# unlike the Webview namespace, which is why that one stays off here.
# A build that wants raylib gone entirely passes
# -DCULEBRA_ENABLE_CANVAS_WINDOW=OFF; CULEBRA_CANVAS_WINDOW_DEFAULT in the
# environment flips the default for every configure in a job, which is how CI
# skips the vendored SDL3 + raylib build it has no display for.
#
# On Linux that default is conditional on the toolchain, the way Webview's is
# below: SDL3's configure fails the whole build when a header it probes for is
# absent, rather than dropping that backend the way it drops a missing runtime
# library, so defaulting ON unconditionally would break `just build` on any
# machine without SDL3's dev packages. Smoke-test a few of the ones it hard-errors
# on (vendor/SDL/docs/README-linux.md lists the full set) and stay off when they
# are missing. This is a check that the documented set was installed, not a
# replication of SDL3's own probing — -DCULEBRA_ENABLE_CANVAS_WINDOW=ON still
# forces the issue.
#
# Windows needs no such check: mingw-w64 already ships the headers SDL3's Win32
# backends want, and the libraries below are part of every Windows install.
set(_canvas_window_default OFF)
if(APPLE OR WIN32)
  set(_canvas_window_default ON)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_canvas_window_default ON)
  foreach(_hdr X11/Xlib.h X11/extensions/scrnsaver.h X11/extensions/XTest.h
               xkbcommon/xkbcommon.h alsa/asoundlib.h)
    unset(_canvas_hdr CACHE)
    find_path(_canvas_hdr NAMES ${_hdr})
    if(NOT _canvas_hdr)
      message(STATUS "Canvas window: off (SDL3 build dependency missing: ${_hdr}"
                     " — see vendor/SDL/docs/README-linux.md)")
      set(_canvas_window_default OFF)
      break()
    endif()
  endforeach()
  unset(_canvas_hdr CACHE)
endif()
if(DEFINED ENV{CULEBRA_CANVAS_WINDOW_DEFAULT})
  set(_canvas_window_default "$ENV{CULEBRA_CANVAS_WINDOW_DEFAULT}")
endif()
option(CULEBRA_ENABLE_CANVAS_WINDOW
  "Enable a native desktop window for the Canvas namespace (raylib + SDL)"
  ${_canvas_window_default})
set(_scene_link "")
set(_canvas_link "")

# Resolve the shared static-lib paths + frameworks whenever either raylib-backed
# namespace is on, so the ExternalProject build + the driver link below are
# single-sourced across both knobs.
if(CULEBRA_ENABLE_SCENE OR CULEBRA_ENABLE_CANVAS_WINDOW)
  # Where the two statics live. They depend only on their own sources and the
  # target platform — never on culebra's build type, LTO or JIT settings — so
  # building them under each build dir made every fresh build/, build-dev/,
  # build-gate/ (and every new worktree) pay ~3.5 min for the same bytes.
  # Install them once into a cache keyed on the submodule commits + platform
  # instead: a vendor bump lands under a new key, so a stale artifact can never
  # be picked up, and the build below skips ExternalProject entirely on a hit.
  # ~10 MB per key; old keys are inert (delete with `rm -rf ~/.cache/culebra/deps`).
  # The root is a cache entry seeded from $CULEBRA_DEPS_CACHE, so it stays put for
  # a build dir once configured (CI, or a sandbox that can't write $HOME, sets the
  # variable; -DCULEBRA_DEPS_CACHE= moves an existing one).
  # Without git (a source tarball) there is no key, so we keep the old per-build
  # dir location rather than risk sharing across unknown sources.
  _culebra_dep_rev(_sdl_rev "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL")
  _culebra_dep_rev(_raylib_rev "${CMAKE_CURRENT_SOURCE_DIR}/vendor/raylib")
  if(_sdl_rev AND _raylib_rev)
    if(DEFINED ENV{CULEBRA_DEPS_CACHE})
      set(_deps_root_default "$ENV{CULEBRA_DEPS_CACHE}")
    else()
      set(_deps_root_default "$ENV{HOME}/.cache/culebra/deps")
    endif()
    set(CULEBRA_DEPS_CACHE "${_deps_root_default}" CACHE PATH
      "Where the vendored SDL3 + raylib statics are built and shared from")
    set(_deps_root "${CULEBRA_DEPS_CACHE}")
    set(_deps_key "${CMAKE_SYSTEM_NAME}" "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_OSX_ARCHITECTURES)
      list(APPEND _deps_key "${CMAKE_OSX_ARCHITECTURES}")
    endif()
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      list(APPEND _deps_key "macos${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    list(APPEND _deps_key "sdl${_sdl_rev}" "raylib${_raylib_rev}")
    string(REPLACE ";" "-" _deps_key "${_deps_key}")
    set(_sdl_install "${_deps_root}/${_deps_key}/sdl")
    set(_raylib_install "${_deps_root}/${_deps_key}/raylib")
    message(STATUS "SDL3 + raylib deps cache: ${_deps_root}/${_deps_key}")
  else()
    set(_sdl_install "${CMAKE_CURRENT_BINARY_DIR}/sdl_ext")
    set(_raylib_install "${CMAKE_CURRENT_BINARY_DIR}/raylib_ext")
    message(STATUS "SDL3 + raylib deps: this build dir (no submodule revision)")
  endif()
  set(_sdl_a "${_sdl_install}/lib/libSDL3.a")
  set(_raylib_a "${_raylib_install}/lib/libraylib.a")
  if(APPLE)
    # Frameworks SDL3 needs (window/input/audio/haptics) + raylib's GL path,
    # mirroring the installed sdl3.pc Libs.private. Single-sourced so the AOT
    # link fragment and the in-process link of the driver (further below) can't
    # drift apart.
    set(_scene_frameworks
      OpenGL Cocoa IOKit CoreVideo CoreMedia CoreAudio AudioToolbox
      CoreFoundation AVFoundation Foundation GameController ForceFeedback
      Carbon Metal QuartzCore)
    # SDL3 links these weakly so binaries still load on older macOS.
    set(_scene_weak_frameworks CoreHaptics UniformTypeIdentifiers)
  elseif(WIN32)
    # What the frameworks above are on macOS. Mirrors the Win32 list SDL3's own
    # build records (vendor/SDL/CMakeLists.txt, "Libraries for Win32 native and
    # MinGW"), plus dinput8 from its joystick block — mingw-w64 ships dinput.h,
    # so that backend compiles in. No opengl32: with PLATFORM=SDL raylib reaches
    # GL through SDL_GL_GetProcAddress, and SDL loads opengl32.dll at runtime,
    # so nothing here imports it at link time.
    set(_scene_system_libs
      kernel32 user32 gdi32 winmm imm32 ole32 oleaut32 version uuid advapi32
      setupapi shell32 dinput8)
  endif()
  # The AOT link fragment (raylib + SDL3 statics + frameworks) `culebra build`
  # appends on usage. Built once here — Scene and Canvas share it, so they can't
  # drift apart — then assigned to the two per-feature vars (kept separate
  # because they ride different usage axes: uses_scene vs uses_canvas).
  # The two statics are named, not pathed: the deps cache they live in belongs
  # to the machine that built this driver, and `culebra build` runs on the
  # user's. src/main.cc resolves the name to its embedded copy and quotes it
  # for the platform's shell (see _rt_embed_extern).
  cmake_path(GET _raylib_a FILENAME _raylib_a_name)
  cmake_path(GET _sdl_a FILENAME _sdl_a_name)
  set(_raylib_aot_link "@${_raylib_a_name}@ @${_sdl_a_name}@")
  if(APPLE)
    foreach(_fw IN LISTS _scene_frameworks)
      string(APPEND _raylib_aot_link " -framework ${_fw}")
    endforeach()
    foreach(_fw IN LISTS _scene_weak_frameworks)
      string(APPEND _raylib_aot_link " -weak_framework ${_fw}")
    endforeach()
  elseif(WIN32)
    foreach(_lib IN LISTS _scene_system_libs)
      string(APPEND _raylib_aot_link " -l${_lib}")
    endforeach()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # What the frameworks above are on macOS: the system libraries a static
    # SDL3 records in its sdl3.pc but a static archive cannot carry itself. The
    # driver's own link of these statics takes them from Threads::Threads +
    # CMAKE_DL_LIBS further below; the AOT fragment has to spell them, or the
    # two link paths this block exists to single-source drift. Omitting them
    # only linked at all because glibc >= 2.34 folded libpthread and libdl into
    # libc — an older target, or a cross-compile, fails on dlopen/pthread_*.
    string(APPEND _raylib_aot_link " -lpthread -ldl -lm")
  endif()
endif()

if(CULEBRA_ENABLE_SCENE)
  set(_scene_link "${_raylib_aot_link}")
  # Scene's binding reaches wrap.h, so its archive can carry a `culebra_runtime_*`
  # helper the compiler declined to inline, against the core archive's strong
  # one — the leftover Webview's Windows fragment absorbs above, for the same
  # reason (PE has no weak external). Appended after the split from
  # _raylib_aot_link, not into it: a Canvas link that never force-loads the
  # Scene archive keeps strict duplicate detection.
  # tools/check_rt_archive_tls.sh fails if the count grows past a leftover or two.
  if(WIN32)
    string(APPEND _scene_link " -Wl,--allow-multiple-definition")
  endif()
  message(STATUS "Scene: enabled (vendored static SDL3 + raylib)")
endif()

if(CULEBRA_ENABLE_CANVAS_WINDOW)
  set(_canvas_link "${_raylib_aot_link}")
  message(STATUS "Canvas window: enabled (vendored static SDL3 + raylib)")
endif()

# --- Builtin Webview namespace (native WebView desktop): config ------------
# ON by default: unlike Scene there is no static library to build — webview
# is a single vendored header (vendor/webview/webview.h) plus the OS's native
# WebView framework, so it adds ~300 KB to the driver and nothing to non-Webview
# AOT binaries (usage-gated force-load, mirroring the Scene axis). On
# platforms where the engine's headers are missing (Linux without gtk4 +
# webkitgtk-6.0, Windows without WebView2.h) it auto-disables — the binding TU
# wouldn't compile there.
option(CULEBRA_ENABLE_WEBVIEW "Enable builtin Webview namespace (native WebView)" ON)
set(_webview_link "")
set(_webview_sources ./src/runtime/culebra_rt_webview.cc)
if(CULEBRA_ENABLE_WEBVIEW)
  # Resolve the platform's engine once into four siblings the later blocks
  # consume unconditionally, the way Http does with _ssl_libs/_ssl_link:
  # _webview_includes (header dirs), _webview_libs (driver + archive link),
  # _webview_link (the CULEBRA_WEBVIEW_LINK string `culebra build` appends only
  # when the program uses Webview) and _webview_sources (the binding TU, plus
  # whatever else a platform needs to reach its engine). One platform chain, not
  # one per consumer.
  if(APPLE)
    set(_webview_libs "-framework WebKit" ${CMAKE_DL_LIBS})
    set(_webview_link "-framework WebKit -ldl")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
      pkg_check_modules(WEBKITGTK gtk4 webkitgtk-6.0)
    endif()
    if(WEBKITGTK_FOUND)
      # Headers, and deliberately nothing else: the engine is dlopen'd when a
      # window is created (src/runtime/webview_gtk_dynload.cc, which defines
      # every GTK/WebKit symbol the binding reaches). Linking gtk4/webkitgtk is
      # what put libgtk-4.so.1 and libwebkitgtk-6.0.so.4 in DT_NEEDED and stopped
      # the binary from starting where a desktop stack is absent — the reason
      # the released Linux asset used to be built with Webview off. So the
      # pkg-config Libs go unused here; only its include dirs are.
      #
      # Two link-line workarounds went with them: the -Wl,--export-dynamic that
      # webkitgtk inherits from gmodule (it would have republished all ~68k
      # symbols past culebra_export_jit_symbols()'s whitelist, LLVM's among
      # them, which is what segfaults the Mesa/Gallium LLVM WebKitGTK dlopens),
      # and the -lz that answered ld diagnosing the core archive's dead gzip
      # references once libz became reachable as an indirect DSO of libwebkitgtk.
      # Neither DSO reaches the link now, so neither workaround has anything to
      # act on. misc/probe_webview_aot_link.sh is what proves the second.
      set(_webview_includes ${WEBKITGTK_INCLUDE_DIRS})
      set(_webview_libs ${CMAKE_DL_LIBS})
      set(_webview_link "-ldl")
      list(APPEND _webview_sources ./src/runtime/webview_gtk_dynload.cc)
      # Keeps the forwarders out of the executable's dynamic symbol table, where
      # they would interpose on GTK's and WebKit's own internal calls. It covers
      # the GTK/GLib half; WEBKIT_API pins the rest to default visibility and the
      # --dynamic-list whitelist is what holds those. See the TU's header.
      set_source_files_properties(./src/runtime/webview_gtk_dynload.cc
                                  PROPERTIES COMPILE_OPTIONS -fvisibility=hidden)
    else()
      message(STATUS "Webview: disabled (gtk4 + webkitgtk-6.0 not found via pkg-config)")
      set(CULEBRA_ENABLE_WEBVIEW OFF)
    endif()
  elseif(WIN32)
    # Only the SDK header is needed at build time — webview.h's builtin loader
    # finds the Edge WebView2 runtime through the registry, so there is no
    # import library and WebView2Loader.dll is optional. MSYS2 ships the header
    # as mingw-w64-<env>-webview2-loader. The libs are the ones webview.h asks
    # MSVC for via #pragma comment(lib). Unset around find_path like the Canvas
    # probe: a cached hit is never re-validated, so an uninstalled SDK would
    # keep configuring and then fail the compile.
    unset(_webview2_hdr CACHE)
    find_path(_webview2_hdr NAMES WebView2.h)
    if(_webview2_hdr)
      set(_webview_includes ${_webview2_hdr})
      set(_webview_libs ole32 shell32 shlwapi user32 version advapi32)
      list(TRANSFORM _webview_libs PREPEND "-l" OUTPUT_VARIABLE _webview_lflags)
      list(JOIN _webview_lflags " " _webview_link)
      # The binding reaches wrap.h, so its TU can emit a `culebra_runtime_*`
      # helper the core archive defines strongly, and PE has no weak external
      # to fold the two with. CULEBRA_RT_FEATURE_ARCHIVE (rt_macros.h) stops
      # the wholesale emission but not the last un-inlined one — that is the
      # compiler's business. It takes the first definition and drops the
      # diagnostic; COMDAT folding is what handles the ordinary inline bodies.
      # Scoped to this axis, so a link that does not use Webview keeps strict
      # duplicate detection, and tools/check_rt_archive_tls.sh fails if the
      # count grows past a leftover or two.
      string(APPEND _webview_link " -Wl,--allow-multiple-definition")
    else()
      message(STATUS "Webview: disabled (WebView2.h not found — install webview2-loader)")
      set(CULEBRA_ENABLE_WEBVIEW OFF)
    endif()
    unset(_webview2_hdr CACHE)
  else()
    message(STATUS "Webview: disabled (unsupported platform for the native WebView backend)")
    set(CULEBRA_ENABLE_WEBVIEW OFF)
  endif()
endif()
if(CULEBRA_ENABLE_WEBVIEW)
  message(STATUS "Webview: enabled (native WebView, vendored webview.h)")
endif()

# The reference docs travel inside the binary so `culebra docs` answers from
# the same build the user is running — a URL or a checked-out tree can drift
# from it, and the release archive carries no docs/ directory at all. Generated
# at build time rather than committed: docs change every cycle, and a committed
# 1.3 MB blob would land in every diff and need a staleness gate of its own.
set(_docs_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/docs_embedded.cc")
file(GLOB _docs_sources "${CMAKE_CURRENT_SOURCE_DIR}/docs/*.md")
add_custom_command(
  OUTPUT ${_docs_gen}
  COMMAND ${CMAKE_COMMAND}
          -DDOCS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/docs
          -DOUT=${_docs_gen}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_docs.cmake
  DEPENDS ${_docs_sources} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_docs.cmake
  COMMENT "Embedding docs/*.md"
  VERBATIM)

# The VSCode/Vim/Zed editor-integration payload files travel inside the binary
# too, so `culebra init` can lay them down with no source checkout — same
# rationale as the docs embedding just above, and not committed for the same
# reason (the payload changes with grammar/keyword sync and would need its
# own staleness gate if committed).
set(_editor_gen "${CMAKE_CURRENT_BINARY_DIR}/gen/editor_assets_embedded.cc")
file(GLOB _editor_sources
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/vscode/*.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/vscode/*.js"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/vscode/syntaxes/*.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/vim/*.vim"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/zed/Cargo.toml"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/zed/src/*.rs"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/zed/debug_adapter_schemas/*.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/zed/languages/culebra/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/zed/debug.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/misc/zed/extension.toml.template")
add_custom_command(
  OUTPUT ${_editor_gen}
  COMMAND ${CMAKE_COMMAND}
          -DMISC_DIR=${CMAKE_CURRENT_SOURCE_DIR}/misc
          -DVERSION_HEADER=${CMAKE_CURRENT_SOURCE_DIR}/include/culebra.h
          -DOUT=${_editor_gen}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_editor_assets.cmake
  DEPENDS ${_editor_sources} ${CMAKE_CURRENT_SOURCE_DIR}/include/culebra.h
          ${CMAKE_CURRENT_SOURCE_DIR}/cmake/gen_editor_assets.cmake
  COMMENT "Embedding misc/{vscode,vim,zed} editor assets"
  VERBATIM)

add_executable(culebra ./src/main.cc ./src/source_dir.cc ./src/docs_cmd.cc
                       ./src/init_cmd.cc ./src/toolchain_cmd.cc
                       ${_docs_gen} ${_editor_gen})
if(_culebra_enable_lto)
  set_target_properties(culebra PROPERTIES INTERPROCEDURAL_OPTIMIZATION TRUE)
  # Before 3.24, CMake hands GCC a bare `-flto`, which runs all 128 LTRANS jobs
  # on one core: 103 s of link against 16 s parallel, for byte-identical output
  # (it schedules the partitions, it does not change them) at an unchanged peak
  # RSS. CMake does this itself from 3.24 (MR !7400), so this block goes when the
  # floor at the top of the file reaches it; `auto` is GCC's spelling, and
  # clang's ThinLTO already threads its link.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_VERSION VERSION_LESS 3.24)
    target_link_options(culebra PRIVATE -flto=auto)
  endif()
endif()

if(CULEBRA_COVERAGE)
  target_compile_options(culebra PRIVATE -O0 -fno-inline --coverage
                                         -fprofile-update=atomic)
  target_link_options(culebra PRIVATE --coverage)
endif()

# Shed what a shipped driver never uses. Both halves are what the AOT link has
# always done to its outputs (src/main.cc: strip_syms, dead_strip); the driver
# was the one binary still keeping it all. Sanitizer builds opt out of both —
# the locals and the unreached code are what make their reports readable, and
# a coverage build has a reason of its own: --gc-sections is free to drop the
# counter section of a function nothing calls, which is the population being
# measured.
if(NOT _culebra_diagnostic_build)
  # Local symbols: __LINKEDIT 16.8 -> 4.6 MB, binary 112.7 -> 101.0 MB on macOS
  # arm64 (-Wl,-x since f90a3cc). Not on Windows, whose PE export table is
  # tuned separately below.
  if(NOT WIN32)
    target_link_options(culebra PRIVATE -Wl,-x)
  endif()
  # Then the code nothing reaches: 121.3 -> 108.5 MB on Linux x86-64. Under LTO
  # that is the linker's doing alone, over inputs already split by whoever built
  # them — the LLVM statics orcjit and the native target barely call, and the
  # sqlite object above. GCC drops -ffunction-sections at compile in an LTO
  # build (nothing but bitcode to place); it earns its keep in the LTO=OFF
  # configs, where it is worth 13.7 MB for ~6% larger objects. The JIT's helpers
  # stay alive as GC roots via the export lists below.
  target_compile_options(culebra PRIVATE -ffunction-sections -fdata-sections)
  if(APPLE)
    target_link_options(culebra PRIVATE -Wl,-dead_strip)
  else()
    target_link_options(culebra PRIVATE -Wl,--gc-sections)
  endif()
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
  # wrap.h reaches interpreter.h and jit.h, so these TUs would define five of
  # main.cc's thread_locals a second time in the same image — the link error
  # PE has and ELF does not. The archive copy of the same sources gets this
  # from the feature loop below; the driver copy needs saying (rt_shared_tls.h).
  # This flag only, never CULEBRA_RT_FEATURE_ARCHIVE: that one belongs to the
  # archive target, and here it would strip the JIT's helpers out of the driver.
  set_source_files_properties(${CULEBRA_WRAP_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS CULEBRA_RT_FEATURE_BUILD=1)
  if(CULEBRA_WRAP_LINK)
    separate_arguments(_wrap_link_args NATIVE_COMMAND "${CULEBRA_WRAP_LINK}")
    target_link_options(culebra PRIVATE ${_wrap_link_args})
  endif()
endif()

if(CULEBRA_ENABLE_JIT)
  # lld ahead of ${llvm_libs}: its archives reach into LLVM, not the other way
  # round, and a static link resolves left to right.
  target_link_libraries(culebra ${CULEBRA_LLD_LIBS} ${llvm_libs}
                                ${CULEBRA_LIBXML2_DEPS})
  culebra_export_jit_symbols(culebra)

  # `culebra wrap` rebuilds this tree with CULEBRA_WRAP_SOURCES; the
  # baked source path is the default when CULEBRA_HOME is unset. Scoped to
  # source_dir.cc, not the target — see include/source_dir.h.
  set_source_files_properties(./src/source_dir.cc PROPERTIES
    COMPILE_DEFINITIONS "CULEBRA_SOURCE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}\"")

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
  # this one stub flag.
  target_compile_definitions(culebra_rt PRIVATE
    LLVM_DISABLE_ABI_BREAKING_CHECKS_ENFORCING=1
    CULEBRA_RT_DEFINE_RUNTIME=1
    CULEBRA_RT_TENSOR_EVAL_WEAK=1
    CULEBRA_RT_HTTP_REQUEST_WEAK=1
    CULEBRA_RT_COMPRESS_WEAK=1
    CULEBRA_RT_SQLITE_WEAK=1
    CULEBRA_RT_REGEX_WEAK=1
    CULEBRA_RT_PEG_WEAK=1)
  # In a window build the Canvas backend is the raylib one: the base archive
  # carries weak headless present/input stubs (no raylib), overridden by the
  # strong bodies force-loaded from culebra_rt_canvas on Canvas use. In a
  # default build these flags are absent and the base archive's Canvas ops are
  # the real headless inline bodies, as before.
  if(CULEBRA_ENABLE_CANVAS_WINDOW)
    target_compile_definitions(culebra_rt PRIVATE
      CULEBRA_CANVAS_WINDOW=1 CULEBRA_RT_CANVAS_WEAK=1)
  endif()
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
  # objects, so a non-SQLite binary references no sqlite3 symbol. Gated on
  # CULEBRA_ENABLE_SQLITE (added to the embed list further down, like Scene).
  if(CULEBRA_ENABLE_SQLITE)
  add_library(culebra_rt_sqlite STATIC ./src/runtime/culebra_rt_sqlite.cc
    $<TARGET_OBJECTS:culebra_sqlite3>)
  target_compile_features(culebra_rt_sqlite PRIVATE cxx_std_23)
  target_compile_definitions(culebra_rt_sqlite PRIVATE CULEBRA_RT_SQLITE_STRONG=1)
  target_compile_options(culebra_rt_sqlite PRIVATE
    -ffunction-sections -fdata-sections)
  endif()

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
  set(_rt_embed_files
    "${_rt_archive}"
    "${_rt_tensor_archive}"
    "${_rt_http_archive}"
    "${_rt_compress_archive}")
  set(_rt_embed_deps culebra_rt culebra_rt_tensor culebra_rt_http
    culebra_rt_compress)
  # Regex feature object: the same weak choke, applied to a self-hosted
  # subsystem (regex.h) that links nothing external but costs every binary
  # its code (and, via __builtin_cpu_supports, a start-up CPUID constructor)
  # otherwise. No link flags; embedded like the three above.
  add_library(culebra_rt_regex STATIC ./src/runtime/culebra_rt_regex.cc)
  target_compile_features(culebra_rt_regex PRIVATE cxx_std_23)
  target_compile_options(culebra_rt_regex PRIVATE
    -ffunction-sections -fdata-sections)
  list(APPEND _rt_embed_files
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_regex${CMAKE_STATIC_LIBRARY_SUFFIX}")
  list(APPEND _rt_embed_deps culebra_rt_regex)
  # Peg feature object: the same weak choke over cpp-peglib. The namespace
  # group does not carry this one on its own — peglib's Ope hierarchy leaves
  # vtables and typeinfo that --gc-sections keeps (see peg.h). No link flags.
  add_library(culebra_rt_peg STATIC ./src/runtime/culebra_rt_peg.cc)
  target_compile_features(culebra_rt_peg PRIVATE cxx_std_23)
  target_compile_options(culebra_rt_peg PRIVATE
    -ffunction-sections -fdata-sections)
  list(APPEND _rt_embed_files
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_peg${CMAKE_STATIC_LIBRARY_SUFFIX}")
  list(APPEND _rt_embed_deps culebra_rt_peg)
  # The __Foreign wrap fixture: a static registrar, so the link can never drop
  # it from the core archive (see src/runtime/culebra_rt_foreign.cc). Its own
  # archive, force-loaded only when the program names __Foreign.
  add_library(culebra_rt_foreign STATIC ./src/runtime/culebra_rt_foreign.cc)
  target_compile_features(culebra_rt_foreign PRIVATE cxx_std_23)
  target_compile_options(culebra_rt_foreign PRIVATE
    -ffunction-sections -fdata-sections)
  list(APPEND _rt_embed_files
    "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_foreign${CMAKE_STATIC_LIBRARY_SUFFIX}")
  list(APPEND _rt_embed_deps culebra_rt_foreign)
  # SQLite feature archive (gated), appended like Scene/Webview below.
  if(CULEBRA_ENABLE_SQLITE)
    list(APPEND _rt_embed_files
      "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_sqlite${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND _rt_embed_deps culebra_rt_sqlite)
  endif()
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
  # Scene feature archive: the wrap registration (its static registrar pulls
  # in raylib). Force-loaded into an AOT binary only when the AST scan reports
  # Scene use (raylib/SDL come from CULEBRA_SCENE_LINK below) — the same
  # usage-gating as sqlite, but with no weak choke since Scene simply isn't in
  # the base archive. Compiling needs only raylib's headers (vendored in-tree).
  if(CULEBRA_ENABLE_SCENE)
    add_library(culebra_rt_scene STATIC ./src/runtime/culebra_rt_scene.cc)
    target_compile_features(culebra_rt_scene PRIVATE cxx_std_23)
    target_include_directories(culebra_rt_scene PRIVATE ./vendor/raylib/src)
    target_compile_definitions(culebra_rt_scene PRIVATE CULEBRA_ENABLE_SCENE=1)
    target_compile_options(culebra_rt_scene PRIVATE -ffunction-sections -fdata-sections)
    list(APPEND _rt_embed_files
      "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_scene${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND _rt_embed_deps culebra_rt_scene)
  endif()
  # Canvas window feature archive: the strong raylib present/input bodies
  # (culebra_rt_canvas.cc) that override the base archive's weak headless stubs.
  # Force-loaded into an AOT binary only when the AST scan reports Canvas use
  # (raylib/SDL come from CULEBRA_CANVAS_LINK below) — the same weak choke as
  # sqlite. Compiling needs only raylib's headers (vendored in-tree).
  if(CULEBRA_ENABLE_CANVAS_WINDOW)
    add_library(culebra_rt_canvas STATIC ./src/runtime/culebra_rt_canvas.cc)
    target_compile_features(culebra_rt_canvas PRIVATE cxx_std_23)
    target_include_directories(culebra_rt_canvas PRIVATE ./vendor/raylib/src
                                                         ./vendor/SDL/include)
    target_compile_definitions(culebra_rt_canvas PRIVATE CULEBRA_CANVAS_WINDOW=1)
    target_compile_options(culebra_rt_canvas PRIVATE -ffunction-sections -fdata-sections)
    list(APPEND _rt_embed_files
      "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_canvas${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND _rt_embed_deps culebra_rt_canvas)
  endif()
  # Webview feature archive: same usage-gating as Scene (force-loaded into an
  # AOT binary only when the AST scan reports Webview use; the OS WebView
  # framework comes from CULEBRA_WEBVIEW_LINK). The vendored single header is
  # all macOS needs (it drives the ObjC runtime directly); GTK and WebView2 add
  # their own headers, which _webview_includes carries (empty on macOS).
  if(CULEBRA_ENABLE_WEBVIEW)
    add_library(culebra_rt_webview STATIC ${_webview_sources})
    target_compile_features(culebra_rt_webview PRIVATE cxx_std_23)
    target_include_directories(culebra_rt_webview SYSTEM PRIVATE ./vendor/webview
                                                          ${_webview_includes})
    target_compile_definitions(culebra_rt_webview PRIVATE CULEBRA_ENABLE_WEBVIEW=1)
    target_compile_options(culebra_rt_webview PRIVATE -ffunction-sections -fdata-sections)
    list(APPEND _rt_embed_files
      "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_STATIC_LIBRARY_PREFIX}culebra_rt_webview${CMAKE_STATIC_LIBRARY_SUFFIX}")
    list(APPEND _rt_embed_deps culebra_rt_webview)
  endif()
  # The external statics an AOT link needs whole: OpenSSL for Http, the vendored
  # SDL3 + raylib for Scene / the Canvas window. Unlike the archives above these
  # are not ours, but they ride here for the same reason — the link runs on the
  # user's machine, and the paths they sit at (a distro dir, a Homebrew prefix,
  # an MSYS2 tree, this build's deps cache) exist only on the machine that built
  # this driver. The link fragments name them (@libssl.a@); src/main.cc
  # materializes the name. They keep their own filenames, so nothing here has to
  # agree with the fragment beyond that name. Kept out of _rt_embed_files: those
  # pair a file with the target that builds it, and these are plain inputs (SDL3
  # + raylib are ordered against their ExternalProjects further below, where
  # those targets exist).
  set(_rt_embed_extern "")
  if(CULEBRA_ENABLE_HTTP)
    list(APPEND _rt_embed_extern
      "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}")
  endif()
  if(CULEBRA_ENABLE_SCENE OR CULEBRA_ENABLE_CANVAS_WINDOW)
    list(APPEND _rt_embed_extern "${_raylib_a}" "${_sdl_a}")
  endif()
  # Every feature archive is force-loaded next to the core archive, so the two
  # must not both define the same symbol: mingw's ld rejects the duplicate and
  # the Windows AOT link fails. Two flags say so, because the two halves do not
  # have the same scope. CULEBRA_RT_FEATURE_BUILD flips the CULEBRA_RT_CORE_OWNED
  # declarations (rt_shared_tls.h) to `extern`, and belongs on the binding
  # sources too — the driver's copies of them borrow main.cc's thread_locals the
  # same way. CULEBRA_RT_FEATURE_ARCHIVE stops the runtime helpers being
  # force-emitted (rt_macros.h) and belongs ONLY here, on the archive targets:
  # the driver is what the in-process JIT resolves those helpers from, so a
  # source-file property carrying this to the driver's copy would strip them out
  # of the JIT's own image. Ratchets: tools/check_rt_archive_tls.sh for what the
  # archives may define, tools/check_jit_host_symbols.sh for what the driver
  # must still define.
  foreach(_rt_feature culebra_rt_tensor culebra_rt_http culebra_rt_compress
                      culebra_rt_regex culebra_rt_peg culebra_rt_foreign
                      culebra_rt_sqlite culebra_rt_scene culebra_rt_canvas
                      culebra_rt_webview culebra_rt_wrap)
    if(TARGET ${_rt_feature})
      target_compile_definitions(${_rt_feature} PRIVATE
        CULEBRA_RT_FEATURE_BUILD=1 CULEBRA_RT_FEATURE_ARCHIVE=1)
    endif()
  endforeach()
  # Embed the archives deflated: 33.8 -> 6.9 MB of .rodata, a quarter of the
  # driver, for one inflate per archive on the first `culebra build` of a given
  # fingerprint (materialize_archive caches the expanded .a). The compressed
  # copies live in their own directory under the archives' own names, so the
  # embedded entry is still "libculebra_rt.a" and the driver needs no filename
  # convention to agree with; the real archives stay put for --rt-lib.
  add_executable(compress_asset ./tools/compress_asset.cc)
  target_compile_features(compress_asset PRIVATE cxx_std_23)
  target_link_libraries(compress_asset ZLIB::ZLIB)
  # Uses std::println, so it needs the same libstdc++exp the driver links for
  # __open_terminal / __write_to_terminal (see the culebra target's WIN32
  # block). GCC 14/15 objects happened to link without it; GCC 16's emit the
  # references outright — which CI's mingw lane only showed once its ccache
  # stopped serving objects compiled by the older compiler.
  if(WIN32 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(compress_asset stdc++exp)
  endif()
  set(_rt_packed_dir "${CMAKE_CURRENT_BINARY_DIR}/rt-packed")
  file(MAKE_DIRECTORY "${_rt_packed_dir}")
  set(_rt_embed_packed "")
  foreach(_rt_a _rt_dep IN ZIP_LISTS _rt_embed_files _rt_embed_deps)
    cmake_path(GET _rt_a FILENAME _rt_a_name)
    add_custom_command(
      OUTPUT "${_rt_packed_dir}/${_rt_a_name}"
      COMMAND compress_asset "${_rt_a}" "${_rt_packed_dir}/${_rt_a_name}"
      DEPENDS "${_rt_a}" ${_rt_dep} compress_asset
      COMMENT "Compressing ${_rt_a_name}"
      VERBATIM)
    list(APPEND _rt_embed_packed "${_rt_packed_dir}/${_rt_a_name}")
  endforeach()
  foreach(_extern IN LISTS _rt_embed_extern)
    cmake_path(GET _extern FILENAME _extern_name)
    # Depend on the file only once something has produced it. On a cold build
    # SDL3 and raylib are still an ExternalProject away, and a BUILD_BYPRODUCT
    # is a rule to Ninja but not to Make — naming one there fails the build with
    # "no rule to make target". Ordering against those builds is a target-level
    # dependency instead (add_dependencies further below, where they exist).
    # The stamp carries the source path so a vendor bump — which moves the
    # archive to a new deps-cache key without touching this output — reruns the
    # compression instead of leaving the previous version embedded.
    set(_extern_dep "")
    if(EXISTS "${_extern}")
      set(_extern_dep "${_extern}")
    endif()
    file(GENERATE OUTPUT "${_rt_packed_dir}/${_extern_name}.src"
         CONTENT "${_extern}\n")
    add_custom_command(
      OUTPUT "${_rt_packed_dir}/${_extern_name}"
      COMMAND compress_asset "${_extern}" "${_rt_packed_dir}/${_extern_name}"
      DEPENDS ${_extern_dep} "${_rt_packed_dir}/${_extern_name}.src" compress_asset
      COMMENT "Compressing ${_extern_name}"
      VERBATIM)
    list(APPEND _rt_embed_packed "${_rt_packed_dir}/${_extern_name}")
  endforeach()
  add_custom_target(culebra_rt_packed DEPENDS ${_rt_embed_packed})

  cpp_embedlib_add(culebra_rt_assets
    FILES ${_rt_embed_packed}
    BASE_DIR "${_rt_packed_dir}"
    NAMESPACE CulebraRT)
  # cpp-embedlib's per-file DEPENDS is a file-level dependency. Force
  # the asset library to wait for the compress step (and through it the real
  # archive targets), so the packed files exist when the embed step runs.
  add_dependencies(culebra_rt_assets culebra_rt_packed)
  target_link_libraries(culebra culebra_rt_assets)
  endif()
endif()

# Tensor primitive: since M4 the execution engine is cpp-tensorlib
# (lazy graph + fusion; Accelerate/Metal on macOS, own CPU kernels plus the
# CUDA backend below elsewhere). Header-only — the only link deps are macOS
# OS frameworks and libdl, which the static-link policy allows. Not optional:
# `Tensor` is one of the twelve types the language documents, interpreter.h
# builds TensorValue into the value core, and a build flag that changed the
# type set would be a second dialect for all three backends to stay symmetric
# across. What stays switchable is the GPU backend below.
if(NOT EXISTS "${CULEBRA_TENSORLIB_DIR}/tensorlib.h")
  message(FATAL_ERROR
    "cpp-tensorlib headers not found at ${CULEBRA_TENSORLIB_DIR}. "
    "Run `git submodule update --init vendor/cpp-tensorlib`, or pass "
    "-DCULEBRA_TENSORLIB_DIR=<path> to a checkout.")
endif()

# GPU backend off macOS (macOS gets Metal from the frameworks below, with no
# build-time dependency). nvcc is needed only to compile the kernels to PTX;
# the driver is dlopen'd at run time, so a CUDA-enabled binary still runs —
# on the CPU backend — where no GPU or driver exists. AUTO therefore costs
# nothing on a machine without the toolkit and needs no flag on one with it.
set(CULEBRA_TENSOR_CUDA AUTO CACHE STRING
    "CUDA GPU backend for Tensor off macOS: ON, OFF, or AUTO (nvcc found)")
set_property(CACHE CULEBRA_TENSOR_CUDA PROPERTY STRINGS AUTO ON OFF)

set(_tensor_cuda OFF)
if(NOT APPLE AND NOT CULEBRA_TENSOR_CUDA STREQUAL "OFF")
  find_program(CULEBRA_NVCC NAMES nvcc
    HINTS ENV CUDA_PATH ENV CUDA_HOME /usr/local/cuda
    PATH_SUFFIXES bin)
  if(CULEBRA_NVCC)
    set(_tensor_cuda ON)
  elseif(CULEBRA_TENSOR_CUDA STREQUAL "ON")
    message(FATAL_ERROR
      "CULEBRA_TENSOR_CUDA=ON but nvcc was not found. Add it to PATH or "
      "set CUDA_PATH.")
  else()
    message(STATUS
      "Tensor: CUDA backend off (no nvcc) — Tensor.use_gpu() falls back to CPU")
  endif()
endif()

if(_tensor_cuda)
  # compute_80 PTX JITs onto any sm_80+ device, so one blob covers recent
  # GPUs. bin2c turns it into the byte array cuda.h #includes (off-Apple
  # compilers predate C23 #embed).
  if(NOT CULEBRA_TENSOR_CUDA_PTX_ARCH)
    set(CULEBRA_TENSOR_CUDA_PTX_ARCH sm_80)
  endif()
  set(_tl_dir "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-tensorlib")
  set(_tl_ptx "${CMAKE_CURRENT_BINARY_DIR}/tensorlib_cuda.ptx")
  set(_tl_ptx_inc "${CMAKE_CURRENT_BINARY_DIR}/tensorlib_cuda_ptx.inc")
  set(_nvcc_args -arch=${CULEBRA_TENSOR_CUDA_PTX_ARCH} -ptx)
  if(WIN32)
    # nvcc's host-compiler version gate hard-errors on a Visual Studio newer
    # than its supported window. PTX is device code — the host compiler only
    # preprocesses — so skipping the gate is safe.
    list(APPEND _nvcc_args -allow-unsupported-compiler)
  endif()
  add_custom_command(OUTPUT ${_tl_ptx}
    COMMAND ${CULEBRA_NVCC} ${_nvcc_args}
            ${_tl_dir}/kernels/tensorlib_cuda.cu -o ${_tl_ptx}
    DEPENDS ${_tl_dir}/kernels/tensorlib_cuda.cu
    COMMENT "nvcc: tensorlib_cuda.cu -> PTX (${CULEBRA_TENSOR_CUDA_PTX_ARCH})"
    VERBATIM)
  add_custom_command(OUTPUT ${_tl_ptx_inc}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_tl_ptx} -DOUTPUT=${_tl_ptx_inc}
            -P ${_tl_dir}/cmake/bin2c.cmake
    DEPENDS ${_tl_ptx}
    COMMENT "bin2c: PTX -> tensorlib_cuda_ptx.inc"
    VERBATIM)
  add_custom_target(culebra_tensor_cuda_ptx DEPENDS ${_tl_ptx_inc})

  # Applied to every target that instantiates a tensorlib backend body. The
  # driver (interp/JIT) and culebra_rt_tensor (AOT) must agree, or the same
  # program would pick a different device per backend. The core archive is
  # deliberately excluded: its weak chokes never odr-use tl, so it stays free
  # of both the backend and the embedded PTX.
  function(culebra_enable_tensor_cuda tgt)
    add_dependencies(${tgt} culebra_tensor_cuda_ptx)
    target_include_directories(${tgt} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_definitions(${tgt} PRIVATE TENSORLIB_CUDA)
  endfunction()
  culebra_enable_tensor_cuda(culebra)
  if(TARGET culebra_rt_tensor)
    culebra_enable_tensor_cuda(culebra_rt_tensor)
  endif()
endif()

if(APPLE)
  set(_blas_link
      "-framework Accelerate -framework Metal -framework Foundation")
  target_link_libraries(culebra
      "-framework Accelerate" "-framework Metal" "-framework Foundation")
elseif(_tensor_cuda AND CMAKE_DL_LIBS)
  # Nothing links against CUDA itself — libdl is only for the driver probe.
  # (Windows loads nvcuda.dll through kernel32, so CMAKE_DL_LIBS is empty.)
  set(_blas_link "-ldl")
  target_link_libraries(culebra ${CMAKE_DL_LIBS})
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
# The driver needs pthread on Linux for isolate (std::thread) regardless of
# SQLite/HTTP — link it unconditionally here so no single feature owns it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_package(Threads REQUIRED)
  target_link_libraries(culebra Threads::Threads ${CMAKE_DL_LIBS})
endif()
# FS.watch's macOS backend is FSEvents, which lives in CoreServices. FS is a
# core namespace (fswatcher.h reaches every build through stdlib_interp.h), so
# this is unconditional rather than a kFeatureAxes row — every target that
# links the interpreter stack needs it, and `culebra build` appends the same
# framework to every Mach-O link (src/main.cc, core_frameworks).
if(APPLE)
  set(_core_frameworks "-framework CoreServices")
else()
  set(_core_frameworks "")
endif()
target_link_libraries(culebra ${_core_frameworks})
if(CULEBRA_ENABLE_SQLITE)
  target_link_libraries(culebra culebra_sqlite3)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_sqlite_link "-lpthread -ldl -lm")
  else()
    set(_sqlite_link "")
  endif()
else()
  set(_sqlite_link "")
endif()

# Windows (mingw-w64 for Phase 1): link winpthread for std::thread (the posix
# thread model libstdc++ uses) and Winsock for cpp-httplib. http.h is always
# compiled into the driver, so ws2_32 is linked unconditionally; harmless when
# the Http namespace is disabled. (TLS via OpenSSL is a separate opt-in — see
# the CULEBRA_ENABLE_HTTP block, which the CI and release Windows builds set.)
if(WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(culebra Threads::Threads ws2_32)
  # std::print/println (C++23) resolves the Windows console at runtime via
  # __open_terminal / __write_to_terminal, which libstdc++ ships in the separate
  # experimental library — not pulled in by -static-libstdc++. Link it explicitly
  # (before libstdc++, which the g++ driver appends last) so std::print links.
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_link_libraries(culebra stdc++exp)
  endif()
endif()
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_SQLITE_LINK="${_sqlite_link}")
endif()

# `culebra build` appends the raylib + SDL3 statics + frameworks only when the
# program references Scene (see aot_scan). Defined unconditionally (gated on
# JIT) like the peer link fragments above so the fast inner-loop build
# (CULEBRA_DEV_NO_RT) — which skips the AOT runtime archives — still compiles the
# driver that references this macro. `_scene_link` is "" when Scene is off.
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_SCENE_LINK="${_scene_link}")
endif()

# `culebra build` appends the raylib + SDL3 statics + frameworks only when the
# program references Canvas in a window build (see aot_scan). Defined
# unconditionally (gated on JIT) like the peers. "" when the window backend is
# off, so a default build's AOT link is unchanged.
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_CANVAS_LINK="${_canvas_link}")
endif()

# `culebra build` appends the OS WebView framework only when the program
# references Webview (see aot_scan). Gated on JIT like the peers so the
# CULEBRA_DEV_NO_RT inner-loop build still compiles the driver. "" when off.
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_WEBVIEW_LINK="${_webview_link}")
endif()

# The __Foreign fixture archive reaches wrap.h, so its TU can emit a
# `culebra_runtime_*` helper the core archive defines strongly — the same
# un-inlined leftover Webview's Windows fragment absorbs above, and for the
# same reason (PE has no weak external). Scoped to this axis, which only the
# foreign-object tests trigger, so ordinary links keep strict duplicate
# detection; tools/check_rt_archive_tls.sh fails if the count grows.
set(_foreign_link "")
if(WIN32)
  set(_foreign_link "-Wl,--allow-multiple-definition")
endif()
if(CULEBRA_ENABLE_JIT)
  target_compile_definitions(culebra PRIVATE CULEBRA_FOREIGN_LINK="${_foreign_link}")
endif()

# Every fragment above is spliced into a link command that runs on the *user's*
# machine, so none of them may name a path from this one: a downloaded binary
# would splice a directory only the release runner ever had, and the link would
# fail on a missing file. What an AOT link needs whole is embedded instead and
# named @<filename>@ (_rt_embed_extern, expanded in src/main.cc). Checked here,
# once, for all of them — the release is where this goes wrong, and a tagged
# build is the one place nobody can re-run before it ships. CULEBRA_WRAP_LINK is
# not on the list: those flags come from whoever ran `culebra wrap`, who is also
# the one running the build.
foreach(_frag _blas_link _ssl_link _zlib_link _sqlite_link _scene_link
              _canvas_link _webview_link _foreign_link)
  string(REPLACE " " ";" _frag_tokens "${${_frag}}")
  foreach(_tok IN LISTS _frag_tokens)
    if(_tok MATCHES "^[/\\]" OR _tok MATCHES "^[A-Za-z]:[/\\]")
      message(FATAL_ERROR
        "${_frag} names a build-machine path (${_tok}). Embed the file "
        "(_rt_embed_extern) and name it @<filename>@ in the fragment instead.")
    endif()
  endforeach()
endforeach()

# --- Scene / Canvas window: build SDL3 + raylib + link into the driver ------
# Config (options / static lib paths / link flags) was resolved before the
# driver target above. Here we build SDL3 (static) and raylib (SDL backend,
# static) once from the vendored submodules — shared by the 3D Scene namespace
# and the 2D Canvas window — then compile whichever bindings are enabled straight
# into the driver so interp + JIT run them in-process. AOT outputs instead
# force-load the feature archive + link fragment (gated on usage). raylib finds
# the vendored SDL via CMAKE_PREFIX_PATH; both statics + frameworks are supplied
# at the final link (a static archive records no link deps of its own).
if(CULEBRA_ENABLE_SCENE OR CULEBRA_ENABLE_CANVAS_WINDOW)
  include(ExternalProject)
  # Each dependency is built only when its static isn't already at the cache
  # path resolved above — a hit means some earlier build dir, possibly in
  # another worktree, published these exact sources for this platform. Builds
  # install into a staging dir and publish_dep.cmake renames that into place, so
  # a cache entry only ever appears complete: a concurrent cold build elsewhere
  # sees either nothing yet or a finished tree, never a half-written archive.
  set(_deps_stage "${CMAKE_CURRENT_BINARY_DIR}/deps_stage")
  # A sub-build picks its own compiler off PATH unless told; pass this build's
  # down so the statics are made by the toolchain that will link them.
  set(_deps_toolchain -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER})
  if(NOT EXISTS "${_sdl_a}")
    ExternalProject_Add(sdl_ext
      SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SDL"
      PREFIX "${CMAKE_CURRENT_BINARY_DIR}/sdl_ext_build"
      CMAKE_ARGS
        -DSDL_STATIC=ON -DSDL_SHARED=OFF
        -DSDL_TEST_LIBRARY=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
        -DSDL_INSTALL_CPACK=OFF
        -DCMAKE_BUILD_TYPE=Release
        ${_deps_toolchain}
        -DCMAKE_INSTALL_PREFIX=${_deps_stage}/sdl
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
      BUILD_BYPRODUCTS "${_sdl_a}")
    ExternalProject_Add_Step(sdl_ext publish
      COMMAND ${CMAKE_COMMAND} -DSTAGE=${_deps_stage}/sdl -DDEST=${_sdl_install}
              -DPROBE=${_sdl_a}
              -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/publish_dep.cmake"
      COMMENT "Publishing SDL3 to ${_sdl_install}"
      DEPENDEES install)
  endif()

  if(NOT EXISTS "${_raylib_a}")
    set(_raylib_dep "")
    if(TARGET sdl_ext)
      set(_raylib_dep DEPENDS sdl_ext)
    endif()
    # rcore.c's Windows branch calls isalpha() without including <ctype.h>, and
    # raylib builds itself with -Werror=implicit-function-declaration. Supply
    # the declaration rather than switching that diagnostic off.
    set(_raylib_c_flags "")
    if(WIN32)
      set(_raylib_c_flags "-DCMAKE_C_FLAGS=-include ctype.h")
    endif()
    ExternalProject_Add(raylib_ext
      SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/raylib"
      PREFIX "${CMAKE_CURRENT_BINARY_DIR}/raylib_ext_build"
      ${_raylib_dep}
      CMAKE_ARGS
        -DPLATFORM=SDL -DBUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
        -DCMAKE_BUILD_TYPE=Release
        # SDL3_DIR pins raylib's find_package(SDL3) to the vendored install. A
        # system/homebrew SDL3 would otherwise be a candidate, and linking one
        # SDL's headers against another's static archive fails at the final link.
        -DSDL3_DIR=${_sdl_install}/lib/cmake/SDL3
        -DCMAKE_PREFIX_PATH=${_sdl_install}
        ${_deps_toolchain}
        ${_raylib_c_flags}
        -DCMAKE_INSTALL_PREFIX=${_deps_stage}/raylib
        -DCMAKE_OSX_ARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}
        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
        -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}
        -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}
      BUILD_BYPRODUCTS "${_raylib_a}")
    ExternalProject_Add_Step(raylib_ext publish
      COMMAND ${CMAKE_COMMAND} -DSTAGE=${_deps_stage}/raylib -DDEST=${_raylib_install}
              -DPROBE=${_raylib_a}
              -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/publish_dep.cmake"
      COMMENT "Publishing raylib to ${_raylib_install}"
      DEPENDEES install)
  endif()

  # Shared raylib headers + link (built once, used by both bindings). The SDL
  # headers come from the submodule's own include/ (self-contained, no build
  # step) so the Canvas binding can reach past raylib's API to SDL — see
  # stop_text_input() in culebra_rt_canvas.cc.
  target_include_directories(culebra PRIVATE ./vendor/raylib/src ./vendor/SDL/include)
  foreach(_dep sdl_ext raylib_ext)
    if(TARGET ${_dep})
      add_dependencies(culebra ${_dep})
      # The two statics are embedded as well as linked (_rt_embed_extern), and
      # the compress step reads them off the cache path an ExternalProject
      # publishes late. Order the packing behind the build that produces them.
      if(TARGET culebra_rt_packed)
        add_dependencies(culebra_rt_packed ${_dep})
      endif()
    endif()
  endforeach()
  target_link_libraries(culebra "${_raylib_a}" "${_sdl_a}")
  if(APPLE)
    foreach(_fw IN LISTS _scene_frameworks)   # same single source as _scene_link
      target_link_libraries(culebra "-framework ${_fw}")
    endforeach()
    foreach(_fw IN LISTS _scene_weak_frameworks)
      target_link_libraries(culebra "-weak_framework ${_fw}")
    endforeach()
  elseif(WIN32)
    target_link_libraries(culebra ${_scene_system_libs})  # same single source
  endif()
  # Per-namespace bindings compiled into the driver.
  if(CULEBRA_ENABLE_SCENE)
    target_sources(culebra PRIVATE ./src/runtime/culebra_rt_scene.cc)
    target_compile_definitions(culebra PRIVATE CULEBRA_ENABLE_SCENE=1)
  endif()
  if(CULEBRA_ENABLE_CANVAS_WINDOW)
    target_sources(culebra PRIVATE ./src/runtime/culebra_rt_canvas.cc)
    target_compile_definitions(culebra PRIVATE CULEBRA_CANVAS_WINDOW=1)
  endif()
endif()

# --- Webview: compile the binding into the driver -------------------------
# No static library to build (webview is a vendored header + the OS's native
# WebView framework). Compile the binding straight into the driver so interp +
# JIT run Webview in-process; AOT outputs force-load the feature archive +
# CULEBRA_WEBVIEW_LINK (gated on usage). The header stays plain-C++ compilable
# (it drives the ObjC runtime via objc_msgSend on macOS, so no .mm needed).
if(CULEBRA_ENABLE_WEBVIEW)
  target_sources(culebra PRIVATE ${_webview_sources})
  target_include_directories(culebra SYSTEM PRIVATE ./vendor/webview ${_webview_includes})
  target_compile_definitions(culebra PRIVATE CULEBRA_ENABLE_WEBVIEW=1)
  target_link_libraries(culebra ${_webview_libs})
endif()

# main.cc owns the CULEBRA_RT_CORE_OWNED thread_locals; a second driver TU
# defining them too is a link error on PE/COFF, which has no weak external
# (ELF and Mach-O fold the duplicate TLS init symbols, so only Windows sees
# it). These three reach them through wrap.h and borrow instead.
#
# The flag is not a general licence to include the world: it moves the
# CULEBRA_RT_CORE_OWNED variables and nothing else — a TU that reaches culebra.h
# picks up ~18 more thread_locals from the net/http/sqlite registries that no
# flag can move. Such a TU has to stop reaching those headers.
#
# A source-file property reaches every target that names the file, and these
# three are named twice: once here and once by the culebra_rt_* archive built
# from the same source. That is why the helpers' force-emission is NOT on this
# flag (rt_macros.h) — suppressing `used` is right for the archive and wrong for
# the driver, which is the image the in-process JIT resolves those helpers from.
# check_jit_host_symbols.sh is what fails when that distinction is lost.
set_source_files_properties(
  ./src/runtime/culebra_rt_webview.cc
  ./src/runtime/culebra_rt_scene.cc
  ./src/runtime/culebra_rt_canvas.cc
  PROPERTIES COMPILE_DEFINITIONS CULEBRA_RT_FEATURE_BUILD=1)

# Embedding smoke tests. mt_smoke exercises thread_local isolation
# (parser, GC, exception state); mi_smoke exercises RuntimeScope and
# per-Runtime hooks. Build them alongside `culebra` so they stay in
# step with the headers.
enable_testing()

# One UNITY executable for every smoke (tests/embedding/smoke_suite.cc
# #includes each smoke .cc). Every smoke TU pulls in the interpreter +
# JIT header stack whose codegen costs minutes on the 3-core macOS CI
# runner; as separate executables the smokes were ~28 of the ~33 CI
# build minutes whenever a core header changed — the unity TU emits that
# once. ctest still runs each smoke as its own PROCESS (argv selects it),
# so signal handlers / thread_locals / GC state stay isolated per test.
add_executable(smoke_suite tests/embedding/smoke_suite.cc)
target_compile_features(smoke_suite PRIVATE cxx_std_23)
if(APPLE)
  # cpp-tensorlib backends (in-process embedding uses tensors directly)
  target_link_libraries(smoke_suite
      "-framework Accelerate" "-framework Metal" "-framework Foundation")
endif()
if(_tensor_cuda)
  # Same backend as the driver, so an embedded interpreter reports the same
  # Tensor.gpu_available() the CLI does.
  culebra_enable_tensor_cuda(smoke_suite)
  target_link_libraries(smoke_suite ${CMAKE_DL_LIBS})
endif()
if(CULEBRA_ENABLE_JIT)
  target_link_libraries(smoke_suite ${llvm_libs})
  # The in-process JIT (mt/mi/signal) resolves the `used`-pinned runtime helpers
  # from the host binary via ORC — same export contract as the driver, same
  # mechanism. Not the same link: the driver is dead-stripped above and this
  # test binary is not, so the smokes prove the export path, not the driver's
  # exact link.
  culebra_export_jit_symbols(smoke_suite)
endif()
if(CULEBRA_ENABLE_HTTP)
  target_link_libraries(smoke_suite ${_ssl_libs})
endif()
# The embedding TUs include vm_embed.h -> stdlib_jit.h -> compress.h inline, which calls
# zlib directly, so the suite needs libz.
target_link_libraries(smoke_suite ZLIB::ZLIB)
# ... and the same stack reaches fswatcher.h, which calls FSEvents on macOS.
target_link_libraries(smoke_suite ${_core_frameworks})
# ... and sqlite.h inline wrappers, which call sqlite3_* directly.
if(CULEBRA_ENABLE_SQLITE)
  target_link_libraries(smoke_suite culebra_sqlite3)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(smoke_suite Threads::Threads ${CMAKE_DL_LIBS})
endif()
foreach(smoke mt mi module_scope define signal single_ast utf8_invalid)
  add_test(NAME ${smoke}_smoke COMMAND smoke_suite ${smoke})
endforeach()
# Two entries, not one: the device default installs from a function-local
# static, so each ordering needs its own process.
foreach(smoke tensor_device tensor_device_pin)
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
if(CULEBRA_ENABLE_SQLITE)
  add_executable(sqlite_test test/sqlite_test.cc)
  target_compile_features(sqlite_test PRIVATE cxx_std_23)
  target_link_libraries(sqlite_test culebra_sqlite3)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(sqlite_test Threads::Threads ${CMAKE_DL_LIBS})
  endif()
  add_test(NAME sqlite_test COMMAND sqlite_test)
endif()

# net.h core test: loopback TCP/UDP on ephemeral ports drives the value-neutral
# socket core (connect/accept, the buffered stream reader, timeouts, UDP,
# resolve, error paths). Single-threaded, offline, no interpreter/JIT linkage.
add_executable(net_test test/net_test.cc)
target_compile_features(net_test PRIVATE cxx_std_23)
target_include_directories(net_test PRIVATE include)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(net_test Threads::Threads)
endif()
if(WIN32)
  target_link_libraries(net_test ws2_32)
endif()
add_test(NAME net_test COMMAND net_test)

# fswatcher.h core test: drives create/modify/delete inside a temp directory and
# pulls them back through fs_watch_next. Offline, no interpreter/JIT linkage.
# Backed by FSEvents (macOS), inotify (Linux) or ReadDirectoryChangesW
# (Windows); a genuinely backend-less platform (e.g. wasm) still builds this
# and the test itself asserts the "not supported" error path.
add_executable(fswatcher_test test/fswatcher_test.cc)
target_compile_features(fswatcher_test PRIVATE cxx_std_23)
target_include_directories(fswatcher_test PRIVATE include
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/cpp-unicodelib")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(fswatcher_test Threads::Threads)
endif()
target_link_libraries(fswatcher_test ${_core_frameworks})
add_test(NAME fswatcher_test COMMAND fswatcher_test)

# Message formatter (include/rt_format.h) against std::format, which is what it
# has to render: the messages it builds are compared across the three lanes
# and against the previous release. Header-only, no interpreter/JIT linkage.
add_executable(format_test test/format_test.cc)
target_compile_features(format_test PRIVATE cxx_std_23)
target_include_directories(format_test PRIVATE include)
add_test(NAME format_test COMMAND format_test)

# The parse funnel's newline normalization (include/parser.h): a CRLF file
# parses to the same tree as the LF one, and a bare \r is rejected. The
# end-to-end half runs on Windows CI, whose checkout has CRLF endings.
add_executable(source_newlines_test test/source_newlines_test.cc)
target_compile_features(source_newlines_test PRIVATE cxx_std_23)
target_include_directories(source_newlines_test PRIVATE include
                           vendor/cpp-peglib)
add_test(NAME source_newlines_test COMMAND source_newlines_test)

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

# Embedded reference docs (src/docs_cmd.cc): the exit-code contract callers
# script against, and the guard that keeps a broad pattern from dumping the
# whole reference set.
add_test(NAME docs_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/docs_test.sh
                 $<TARGET_FILE:culebra>)

# Source formatter (include/formatter.h): golden-fixture output, the comment
# guard (commented files are left untouched in Phase 0), and a corpus sweep
# asserting the re-parse safety net never refuses and formatting is idempotent.
add_test(NAME fmt_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/fmt_test.sh
                 $<TARGET_FILE:culebra>)

# Parser behaviour on pathological nesting: 20k-deep brackets must be a
# SyntaxError rather than a recursive-descent stack overflow, while 500-deep
# generated literals and 50k-term flat chains keep parsing — and nesting that
# does parse must not backtrack exponentially (200-deep parens).
add_test(NAME parse_depth_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/parse_depth_test.sh
                 $<TARGET_FILE:culebra>)

# `culebra init` (src/init_cmd.cc): the AI-instructions marker block is
# idempotent and preserves existing file content, the Vim step only writes
# where a config dir already exists, and a missing editor is a skip, not a
# failure. Every case is HOME/cwd-isolated into a tmpdir.
add_test(NAME init_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/init_test.sh
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
# Also the runner lanes (`culebra test`, `test --doc`), where one press has to
# end the run rather than fail the item and carry on to the next.
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

# Args.parse's process contract — help on stdout + exit 0, `error: <message>`
# on stderr + exit 2, both raw. test_args.cul only reaches try_parse, which
# raises instead of exiting, so none of that is observable in-process.
add_test(NAME args_cli_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/args_cli_test.sh
                 $<TARGET_FILE:culebra>)

# That arguments reach the program at all, and reach an isolate with it. The
# .cul sweeps run every file with none, so an empty Sys.argv is all they can
# see. The AOT half needs the runtime archive, so justfile's AOT lane runs the
# same script again with --aot.
add_test(NAME sys_argv_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/sys_argv_test.sh
                 $<TARGET_FILE:culebra>)

# --- Baked stdlib preamble -----------------------------------------------------
# The stdlib modules written in Culebra (src/preambles/*.cul) are compiled to
# native objects at build time and carried by the driver (for `--jit`) and by
# libculebra_rt.a (for `culebra build`), so neither lane lowers them at
# start-up: one entry per module, `culebra_preamble_<Name>`, registers its
# builder (stdlib_preamble.h). culebra_preamble_cc is the driver's own
# compiler and lowering in a tool of their own — it inherits the driver's
# definitions, options and libraries so the two cannot disagree about what
# a module is — and the list below is checked against the C++ list at build.
# The inner-loop build (CULEBRA_DEV_NO_RT) skips all of it and splices the
# source as before.
if(CULEBRA_ENABLE_JIT AND NOT CULEBRA_DEV_NO_RT)
  # The driver's per-namespace binding TUs (src/runtime/*.cc: the window
  # backend, the webview loader, ...) hold the strong bodies the runtime
  # headers reference; the tool links the same set.
  get_target_property(_v culebra SOURCES)
  list(FILTER _v INCLUDE REGEX "src/runtime/")
  add_executable(culebra_preamble_cc ./src/preamble_cc.cc ./src/source_dir.cc
                 ${_v})
  target_compile_features(culebra_preamble_cc PRIVATE cxx_std_23)
  # The tool's own speed is irrelevant — the objects it writes are optimized
  # by the LLVM pipeline it drives, not by how it was compiled — and its TU
  # pulls the whole lowering, so -O3 costs a minute and a half per rebuild.
  target_compile_options(culebra_preamble_cc PRIVATE -O1)
  foreach(_prop COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES)
    get_target_property(_v culebra ${_prop})
    if(_v)
      set_property(TARGET culebra_preamble_cc APPEND PROPERTY ${_prop} ${_v})
    endif()
  endforeach()
  get_target_property(_v culebra LINK_LIBRARIES)
  list(REMOVE_ITEM _v culebra_rt_assets)
  target_link_libraries(culebra_preamble_cc ${_v})
  # The driver's generated headers (the CUDA PTX include, ...) are ours too.
  get_target_property(_v culebra MANUALLY_ADDED_DEPENDENCIES)
  if(_v)
    add_dependencies(culebra_preamble_cc ${_v})
  endif()

  set(_baked_names Time Term Canvas Args Regex Peg Log Path Vector2 Vector3
                   Deque PriorityQueue __Eff __Matchers __StringFns)
  if(CULEBRA_ENABLE_WEBVIEW)
    list(APPEND _baked_names Desktop)
  endif()
  set(_baked_dir "${CMAKE_CURRENT_BINARY_DIR}/baked-preambles")
  file(MAKE_DIRECTORY "${_baked_dir}")
  set(_baked_objs "")
  foreach(_n IN LISTS _baked_names)
    set(_o "${_baked_dir}/preamble_${_n}${CMAKE_CXX_OUTPUT_EXTENSION}")
    add_custom_command(
      OUTPUT "${_o}"
      COMMAND culebra_preamble_cc ${_n} -o "${_o}"
      DEPENDS culebra_preamble_cc
              "${CMAKE_CURRENT_SOURCE_DIR}/include/stdlib_preambles.gen.h"
      COMMENT "Baking stdlib preamble ${_n}"
      VERBATIM)
    list(APPEND _baked_objs "${_o}")
  endforeach()
  # (add_custom_command already marks its outputs GENERATED.)
  set_source_files_properties(${_baked_objs} PROPERTIES EXTERNAL_OBJECT TRUE)
  # The driver's table: name -> entry, over exactly the objects above.
  set(_baked_tu "${_baked_dir}/baked_preambles.gen.cc")
  set(_baked_src "// Generated by CMakeLists.txt (baked stdlib preamble) — do not edit.\n#include <stdlib_preamble.h>\n\n")
  foreach(_n IN LISTS _baked_names)
    string(APPEND _baked_src "extern \"C\" void culebra_preamble_${_n}();\n")
  endforeach()
  string(APPEND _baked_src "\nnamespace culebra {\nstatic const BakedPreamble kBakedPreambles[] = {\n")
  foreach(_n IN LISTS _baked_names)
    string(APPEND _baked_src "    {\"${_n}\", culebra_preamble_${_n}},\n")
  endforeach()
  string(APPEND _baked_src "};\nstd::span<const BakedPreamble> baked_preambles() { return kBakedPreambles; }\n}  // namespace culebra\n")
  # CONFIGURE, not WRITE: WRITE touches the file on every configure, and the
  # driver is relinked (under LTO) for a table that did not change.
  file(CONFIGURE OUTPUT "${_baked_tu}" CONTENT "${_baked_src}" @ONLY)
  target_sources(culebra PRIVATE ${_baked_objs} "${_baked_tu}")
  target_compile_definitions(culebra PRIVATE CULEBRA_HAS_BAKED_PREAMBLES=1)
  # ...and archive members for `culebra build`, pulled by reference.
  target_sources(culebra_rt PRIVATE ${_baked_objs})
  add_custom_target(culebra_preamble_list_check
    COMMAND culebra_preamble_cc --check-list ${_baked_names}
    DEPENDS culebra_preamble_cc
    COMMENT "Checking the baked stdlib preamble list"
    VERBATIM)
  add_dependencies(culebra culebra_preamble_list_check)
endif()

# How the driver takes its input (a piped script, a directory), what --ast does
# instead of running, and that a rewrite it can't write says so rather than
# reporting success. All process-level, so no .cul sweep reaches it.
add_test(NAME cli_input_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_input_test.sh
                 $<TARGET_FILE:culebra>)

# What the REPL echoes (nil results are not), that a declaration's state
# survives to the next input, and that an error doesn't end the session.
# Reaching any of it needs a process with a piped stdin.
add_test(NAME repl_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/repl_test.sh
                 $<TARGET_FILE:culebra>)
