// Unity translation unit for the embedding smoke tests. Each smoke
// includes the full runtime header stack, and that stack's codegen
// (the `used`-pinned runtime helpers) costs minutes per TU on the
// 3-core macOS CI runner
// — five separate executables made the smokes ~28 of the ~33 CI build
// minutes whenever a core header changed. One TU emits it once.
//
// Isolation is unchanged: ctest invokes this binary once per smoke
// (argv[1] selects it), so each test still runs in its own process —
// signal handlers, thread_locals, and GC state never bleed across
// smokes. Each included file keeps its internals in a distinct named
// namespace so the unity TU has no symbol collisions.

#include "define_smoke.cc"
#include "closure_flags_smoke.cc"
#include "mi_smoke.cc"
#include "module_scope_smoke.cc"
#include "mt_smoke.cc"
#include "signal_smoke.cc"
#include "single_ast_smoke.cc"
#include "tensor_device_smoke.cc"
#include "utf8_invalid_smoke.cc"

#include <cstdio>
#include <cstring>

int main(int argc, char** argv) {
  if (argc == 2) {
    if (std::strcmp(argv[1], "define") == 0) return define_smoke_ns::run();
    if (std::strcmp(argv[1], "closure_flags") == 0)
      return closure_flags_smoke_ns::run();
    if (std::strcmp(argv[1], "mi") == 0) return mi_smoke_ns::run();
    if (std::strcmp(argv[1], "module_scope") == 0)
      return module_scope_smoke_ns::run();
    if (std::strcmp(argv[1], "mt") == 0) return mt_smoke_ns::run();
    if (std::strcmp(argv[1], "signal") == 0) return signal_smoke_ns::run();
    if (std::strcmp(argv[1], "single_ast") == 0)
      return single_ast_smoke_ns::run();
    if (std::strcmp(argv[1], "tensor_device") == 0)
      return tensor_device_smoke_ns::run(false);
    if (std::strcmp(argv[1], "tensor_device_pin") == 0)
      return tensor_device_smoke_ns::run(true);
    if (std::strcmp(argv[1], "utf8_invalid") == 0)
      return utf8_invalid_smoke_ns::run();
  }
  std::fprintf(stderr,
               "usage: smoke_suite <define|mi|module_scope|mt|signal|"
               "single_ast|tensor_device|tensor_device_pin|utf8_invalid>\n");
  return 2;
}
