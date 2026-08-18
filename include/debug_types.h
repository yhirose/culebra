#pragma once

// The records a source-level debugger shows: one call-stack frame and one
// visible binding. Shared by the engine seam (debug_engine.h) and the VM's
// session (vm_debug.h) so neither side repackages the other's fields.

#include <cstdint>
#include <string>

namespace culebra {

struct DebugFrame {
  std::string name;
  std::string path;
  int64_t line;
};

struct DebugVar {
  std::string name;
  std::string value;
  std::string type;
};

}  // namespace culebra
