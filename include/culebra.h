#pragma once

#include "interpreter.h"
#include "debugger.h"
#include "repl.h"

#ifdef CULEBRA_JIT_ENABLED
#include "jit.h"

#ifndef NDEBUG
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string_view>

namespace culebra {

// Runs at program startup in debug builds: asserts that every method
// name defined in any interpreter builtin table has native JIT codegen.
// Drift (e.g. new method added to ArrayValue::builtins() but forgotten
// in JIT::known_builtin_methods()) otherwise silently falls through to
// user-dispatch at runtime and errors with a confusing message.
[[maybe_unused]] inline const bool _culebra_jit_method_drift_check = []() {
  std::set<std::string_view> interp;
  ObjectValue obj;
  ArrayValue arr;
  for (auto& [k, _] : obj.builtins()) interp.insert(k);
  for (auto& [k, _] : arr.builtins()) interp.insert(k);
  for (auto& [k, _] : string_builtins()) interp.insert(k);
  const auto& jit_set = JIT::known_builtin_methods();
  bool ok = true;
  for (auto& m : interp) {
    if (!jit_set.contains(m)) {
      std::fprintf(stderr,
                   "JIT method drift: interpreter has '%.*s', JIT does not.\n",
                   static_cast<int>(m.size()), m.data());
      ok = false;
    }
  }
  for (auto& m : jit_set) {
    if (!interp.contains(m)) {
      std::fprintf(stderr,
                   "JIT method drift: JIT has '%.*s', interpreter does not.\n",
                   static_cast<int>(m.size()), m.data());
      ok = false;
    }
  }
  if (!ok) std::abort();
  return true;
}();

}  // namespace culebra
#endif  // NDEBUG
#endif
