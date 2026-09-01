// culebra's implementation of cpp-vmlib's host-runtime contract
// (vendor/cpp-vmlib/include/coreir/rt.h) -- the six functions any
// CodeGen.Module in this process shares. coreir_rt_default.cc (cpp-vmlib's
// own stdio implementation, for its standalone CLI) is not linked here; these
// definitions are the only ones in the binary.
//
// Deliberately does NOT include stdlib_jit.h: that header carries every
// stdlib namespace's native bindings (Http, Net, ...) and their thread_local
// state, and compiling it into a feature archive re-defines symbols the core
// archive already owns (tools/checks/check_rt_archive_tls.sh catches exactly this).
// culebra_runtime_println/_input live only there, so this file reaches
// program_out()/read_stdin_line_interruptible directly instead -- the same
// primitives those two functions are themselves built on.
//
// coreir_rt_fail throws culebra::CulebraError rather than exiting. Since the
// exception phase it is fatal-only: a trap or Throw a Core-IR TryCatch
// guards never reaches it, and an unguarded one arrives with the executor's
// frames already popped and released by its own unwinder -- throwing a C++
// exception from here is exactly the documented contract (cpp-vmlib's
// tests/throw_safety.cc pins the leak-free unwind).

#include <cerrno>
#include <cstdlib>
#include <string>

#include "coreir/rt.h"

#include "shared.h"
#include "stdout_capture.h"

extern "C" {

void coreir_rt_out(int64_t v) { culebra::program_out() << v << std::endl; }

void coreir_rt_out_str(const char* bytes, int64_t len) {
  culebra::program_out().write(bytes, len) << std::endl;
}

void coreir_rt_out_raw(const char* bytes, int64_t len) {
  culebra::program_out().write(bytes, len) << std::flush;
}

int64_t coreir_rt_in(int64_t line, int64_t col) {
  std::string s;
  if (!culebra::read_stdin_line_interruptible(s)) {
    throw culebra::CulebraError("IrError", "invalid input", line, col);
  }

  errno = 0;
  char* end = nullptr;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  if (errno != 0 || end != s.c_str() + s.size() || s.empty()) {
    throw culebra::CulebraError("IrError", "invalid input", line, col);
  }
  return static_cast<int64_t>(v);
}

[[noreturn]] void coreir_rt_fail(const char* msg, int64_t line, int64_t col) {
  throw culebra::CulebraError("IrError", msg, line, col);
}

void coreir_rt_poll(void) {
  // Mirrors the JIT/AOT loop backedge's own convention (jit_runtime.h's
  // culebra_runtime_safepoint): a relaxed load first, the real check -- which
  // also handles an isolate's own sticky cancel -- only when it might matter.
  if (culebra::culebra_g_wake.load(std::memory_order_relaxed)) {
    culebra::throw_if_interrupted();
  }
}

}  // extern "C"
