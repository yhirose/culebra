#pragma once

// AOT bootstrap entry. The `culebra build` driver emits an IR `int
// main(int, char**)` that immediately calls into this function with
// the program's `__culebra_main` as `main_fn`. We do here exactly what
// `JIT::exec` does in-process (argv seeding, exception coalescing) so
// that AOT-built binaries and `culebra --jit` behave the same.
//
// This header is included from `src/runtime/culebra_rt.cc`, which is
// what forces the symbol to be emitted into `libculebra_rt.a`. Other
// TUs are free to include it too; the inline + COMDAT semantics keep
// the archive copy authoritative when linking AOT-emitted objects.

#ifdef CULEBRA_JIT_ENABLED

#include <jit.h>

#include <cstdio>
#include <exception>
#include <stdexcept>

extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE int culebra_aot_bootstrap(
    int argc, char** argv, void (*main_fn)()) {
  // Skip argv[0] (program name) so `Sys.argv` matches the
  // `culebra --jit script.cul -- a b c` convention where the holder
  // is populated with only the user-supplied args.
  auto& argv_holder = culebra::current_runtime().sys_argv;
  argv_holder.clear();
  if (argc > 1) {
    argv_holder.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      argv_holder.emplace_back(argv[i] ? argv[i] : "");
    }
  }

  try {
    main_fn();
  } catch (const CulebraException& e) {
    // Balance the retain performed in `culebra_runtime_throw`; format
    // matches `JIT::exec` so AOT and --jit produce identical stderr.
    _culebra_value_release_impl(e.tag, e.data);
    auto s = _culebra_uncaught_display(e.tag, e.data);
    try {
      culebra_runtime_defer_run_to(0);
    } catch (...) {
    }
    std::fprintf(stderr, "uncaught: %s\n", s.c_str());
    return 1;
  } catch (const culebra::CulebraError& e) {
    std::fprintf(stderr, "%s: %s\n", e.kind.c_str(), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}

#endif  // CULEBRA_JIT_ENABLED
