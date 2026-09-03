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

#include <rt/rt.h>

#include <cstdio>
#include <exception>
#include <stdexcept>

extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE int culebra_aot_bootstrap(
    int argc, char** argv, void (*main_fn)()) {
  // Skip argv[0] (program name) so `Sys.argv` matches the
  // `culebra --jit script.cul -- a b c` convention where the holder
  // is populated with only the user-supplied args.
  auto& argv_holder = culebra::sys_argv();
  argv_holder.clear();
  if (argc > 1) {
    argv_holder.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) {
      argv_holder.emplace_back(argv[i] ? argv[i] : "");
    }
  }

  // UTF-8 console output and cooperative Ctrl+C, same as the CLI: the program's
  // loop safepoints and (if it runs the interpreter) statement poll observe the
  // flag.
  culebra::install_console_utf8();
  culebra::install_sigint_handler();

  // Close any watch the program left open, on every exit path — the AOT twin
  // of the script lanes' ScriptTeardownGuard. A
  // top-level `let w = FS.watch(...)` never runs its drop (docs/language.md),
  // so without this its OS notification thread would live into process
  // teardown, and the three backends would differ in when the watch stops.
  struct ScriptTeardownGuard {
    ~ScriptTeardownGuard() { culebra::fswatch::fs_watch_close_all(); }
  } script_teardown_guard;

  try {
    main_fn();
  } catch (const CulebraException& e) {
    // Format first, then consume the carrier's reference (the payload's
    // final +1). The shared formatter is what keeps this identical to
    // `JIT::exec` and the VM's boundary.
    auto s = format_uncaught_throw(e);
    _culebra_value_release_impl(e.tag, e.data);
    try {
      culebra_runtime_defer_run_to(0);
    } catch (...) {
    }
    std::fprintf(stderr, "%s\n", s.c_str());
    return 1;
  } catch (const culebra::Interrupted&) {
    // Uncaught Ctrl+C / cancel: drain top-level defers, then the clean
    // message + conventional 128+SIGINT.
    try {
      culebra_runtime_defer_run_to(0);
    } catch (...) {
    }
    std::fprintf(stderr, "interrupted\n");
    return 130;
  } catch (culebra::CulebraError& e) {
    // Drain top-level defers the uncaught error skipped (mirrors JIT::exec).
    try {
      culebra_runtime_defer_run_to(0);
    } catch (...) {
    }
    // Backfill a positionless runtime error from the published op position
    // (JIT::exec does the same before main.cc formats it), then print
    // `kind: msg at L:C.` so AOT matches `culebra --jit` / the interpreter.
    _jit_backfill_op_pos(e);
    if (e.line > 0 || e.col > 0) {
      std::fprintf(stderr, "%s: %s at %lld:%lld.\n", e.kind.c_str(), e.what(),
                   static_cast<long long>(e.line),
                   static_cast<long long>(e.col));
    } else {
      std::fprintf(stderr, "%s: %s\n", e.kind.c_str(), e.what());
    }
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}

#endif  // CULEBRA_JIT_ENABLED
