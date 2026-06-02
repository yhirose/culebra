#pragma once

#include "interpreter.h"
#include "debugger.h"
#include "module_loader.h"
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
//
// Must enumerate EVERY interp builtin method table — Object/Array/String
// plus Set/Tuple/iterator/Tensor — or the missing tables' methods show up
// as bogus "JIT has X, interpreter does not" drift. TensorValue::builtins()
// returns a static map and never touches `impl`, so a null-backed instance
// is enough to reach it.
[[maybe_unused]] inline const bool _culebra_jit_method_drift_check = []() {
  std::set<std::string_view> interp;
  ObjectValue obj;
  ArrayValue arr;
  for (auto& [k, _] : obj.builtins()) interp.insert(k);
  for (auto& [k, _] : arr.builtins()) interp.insert(k);
  for (auto& [k, _] : string_builtins()) interp.insert(k);
  for (auto& [k, _] : set_builtins()) interp.insert(k);
  for (auto& [k, _] : tuple_builtins()) interp.insert(k);
  for (auto& [k, _] : iterator_builtins()) interp.insert(k);
  for (auto& [k, _] : TensorValue(TensorPtr{}).builtins()) interp.insert(k);
  // A few interp builtin methods are intentionally ABSENT from
  // known_builtin_methods() so that `x.method(...)` on a receiver that lacks
  // that builtin still UFCS-resolves to a same-named global function — which
  // is how the interpreter behaves too:
  //   - `to_string`: String/StringView method, but `x.to_string()` routes
  //     through the global `to_string(x)` via UFCS.
  //   - `add`: Set has builtin codegen, but listing it would force
  //     `x.add(y)` on a non-Set receiver to the builtin path instead of
  //     UFCS-resolving to a user-defined global `add` (interp parity).
  // Excluding them keeps the parity check from flagging a non-drift.
  interp.erase("to_string");
  interp.erase("add");
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
