#pragma once

// The runtime layer the compiled lanes share: the value model (JitValue,
// JitCell, JitObject, ...), the extern "C" helpers compiled code calls, and
// the small front-end contract the bytecode compiler reads back
// (ExtensionHooks / is_builtin_var / fn_introspection_name / ForKind).
//
// Nothing here touches LLVM, so it builds in every configuration. That is the
// point: the bytecode VM (vm.h) compiles and runs on this layer alone, which
// is what makes it the engine of a build without the JIT (docs/internals/vm.md
// §9). `CULEBRA_JIT_ENABLED` reads as "LLVM is linked": it guards jit.h,
// vm_lowering.h, the one member of stdlib_jit.h that declares this layer's
// helpers on a module, and the AOT bootstrap — which needs no LLVM itself but
// only exists where `culebra build` can emit an object to link it into.
//
// The `.inc.h` headers in the second block are not standalone: they are this
// file's body, split for size when this layer was carved out of jit.h's first
// ~10,600 lines. They rely on the block above them and MUST be included in
// exactly this order — `rt_runtime.inc.h` opens an `extern "C"` block that
// stays open across rt_fixed / rt_dispatch and closes in rt_iter. Include
// rt.h; never one of them on its own.

#include <fn_analysis.h>
#include <rt_gc.h>
#include <rt_slab.h>
#include <packable.h>
#include <parser.h>
#include <rt_format.h>
#include <rt_shared_tls.h>  // CULEBRA_RT_CORE_OWNED (one owner per thread_local)
#include <runtime/rt_macros.h>
#include <shared.h>
#include <stdout_capture.h>  // program_out() — where a program's output goes
#include <tensor.h>
#include <unicode_str.h>
#include <unicodelib.h>
#include <unicodelib_encodings.h>

#include <atomic>
#include <cassert>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rt_value.inc.h>
#include <rt_owned.inc.h>
#include <rt_string.inc.h>
#include <rt_runtime.inc.h>
#include <rt_fixed.inc.h>
#include <rt_dispatch.inc.h>
#include <rt_iter.inc.h>
#include <rt_mem.inc.h>

namespace culebra {

// Defined in jit.h when LLVM is linked. Only ExtensionHooks::declare_runtime
// names it, and only a JIT build ever fills that field in.
struct JIT;

// Extension hooks. Built-in functions and namespaces (Math, IO, Random,
// Sys, ...) live outside the language core; an embedder installs them
// by passing a populated ExtensionHooks struct via install_extension().
// Either field may be nullptr — coalesced as a no-op at the call site,
// so the core compiles to a "no extensions" engine when nothing is
// registered. What a call to an extension global or namespace *emits*
// is decided by the bytecode compiler, so no hook here takes an AST.
struct ExtensionHooks {
  // Declare runtime function signatures on the LLVM module so emitted
  // calls into the extension link cleanly. Null in a build without the JIT:
  // there is no module to declare them on, and the VM executor calls the
  // same helpers directly.
  void (*declare_runtime)(JIT&);
  // True if `name` is provided by the extension (inspect/Math/IO/...).
  // Free-variable analysis uses this to skip names that don't live in
  // user bindings.
  bool (*is_builtin_var)(const std::string& name);
};

// The process-wide fallback every Runtime without its own hooks resolves to.
inline ExtensionHooks& default_extension_hooks() {
  static ExtensionHooks hooks{};
  return hooks;
}

// Inside a RuntimeScope: install into that Runtime (overrides the
// default, for sandboxing). Outside any scope: install into the
// process-wide default that all Runtimes fall back to.
inline void install_extension(const ExtensionHooks& hooks) {
  if (culebra::_culebra_rt.current) {
    culebra::runtime_substate<ExtensionHooks>(culebra::kSlotJitHooks) = hooks;
  } else {
    default_extension_hooks() = hooks;
  }
}

inline const ExtensionHooks& current_hooks() {
  auto& rt = culebra::current_runtime();
  if (auto* p =
          static_cast<ExtensionHooks*>(rt.substate[culebra::kSlotJitHooks])) {
    return *p;
  }
  return default_extension_hooks();
}

// `fn` is the implicit recursion handle (the enclosing function's own
// value), bound in every function frame. `self` is NOT here: it is a
// lexically capturable binding (FnAnalysis::note_free_var special-cases
// it) so a nested closure inherits the enclosing frame's receiver,
// interp-style. `range`/`iota`/`grid` are core globals (see
// `try_compile_core_global`); everything else (inspect/Math/IO/...) is
// supplied by the registered extension.
inline bool is_builtin_var(const std::string& name) {
  if (name == "fn") return true;
  if (name == "range" || name == "iota" || name == "grid") return true;
  auto& h = current_hooks();
  return h.is_builtin_var && h.is_builtin_var(name);
}

// The three function-introspection properties (`g.name` / `g.params` /
// `g.return_type`) — the only names a Function receiver owns
// (interp's receiver_has_property Function arm). Their property reads
// return a +1-owned view (the JIT's emit_property_get fn_mode retains).
inline bool fn_introspection_name(std::string_view name) {
  return name == "name" || name == "params" || name == "return_type";
}

// Cursor kinds the unified for-in head switches on: the compiler picks one
// per container, both engines dispatch on it.
enum ForKind : int8_t { FOR_ARRAY = 0, FOR_PROTO = 1, FOR_STRING = 2 };

}  // namespace culebra
