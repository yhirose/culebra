#pragma once

// The compiled-lane registry rows a wrap.h class declaration produces:
// WrappedNsRow (an NsMethod-shaped row per ctor/static, with the full
// declared signature) and the class-name list. Split out of wrap.h so
// stdlib_rt.h can read the registries without wrap.h's interp half
// (Phase 4 B7-b); wrap.h includes this and pushes into both.

#include <rt/rt.h>  // JitValue (the adapter signature)

#include <string>
#include <unordered_map>
#include <vector>

namespace culebra {

// One NsMethod-shaped row per ctor/static of a wrapped class.
// stdlib_rt.h materializes NsMethod rows from these (lazily, after
// static-init froze the registry, so the c_str pointers are stable) and
// merges them into every table consumer. The class name rides in `sub`:
// `Ns.Class.method` is a nested namespace, slow-path only — the same
// shape as Encoding.html.
struct WrappedNsRow {
  std::string ns;
  std::string name;
  std::string sub;
  std::string arg0_type;  // empty = none
  std::string arg0_name;
  int8_t arity;
  JitValue (*adapter)(JitValue*, int64_t);
  // The full declared signature, so the compiled lanes can rebuild the
  // canonical spec (NsParamMeta / introspection) without the interp
  // environment: wrap methods are always all-required positionals whose
  // names/types come straight from the declaration (typed_params emits the
  // same), and the return annotation is return_annotation<R>(). Storage is
  // stable once static-init froze the registry, like the c_str fields above.
  // B7-f note: these subsume arg0_type/arg0_name (== param_types[0] /
  // param_names[0]), and since every wrap row now synthesizes a full
  // CanonSig, the "canonical lookup failed → arg0 only" fallbacks in
  // stdlib_rt.h are unreachable for wrap rows — delete, don't rediscover.
  std::vector<std::string> param_names;
  std::vector<std::string> param_types;
  std::string return_type;
};
inline std::vector<WrappedNsRow>& wrapped_ns_rows() {
  static std::vector<WrappedNsRow> v;
  return v;
}
// Every wrapped class's (ns, name), whether or not it declared any
// ctor/static rows: the engines bind an (empty) class sub-object for a
// rowless class too. Pushed by ~ClassBinder (wrap.h); the AOT gate reads
// the ns column to decide whether a program links the wrapped library.
struct WrappedClassName {
  std::string ns;
  std::string name;
};
inline std::vector<WrappedClassName>& wrapped_class_names() {
  static std::vector<WrappedClassName> v;
  return v;
}

// The wrapped classes that implement an interface culebra accepts by handle
// (search::ISplitter is the one today), by the per-T state-fn id their
// handles carry (foreign.h): a binding has a handle and no T, and this is how
// it reaches the instance as the interface. The accessor is wrap.h's
// jit_handle_self<T>, so a dropped handle raises the same ClosedError there
// as a method call would. Filled by register_implementer (wrap.h).
template <class I>
using WrappedImplementerOf = const I* (*)(JitValue handle);
template <class I>
inline std::unordered_map<int64_t, WrappedImplementerOf<I>>&
wrapped_implementers() {
  static std::unordered_map<int64_t, WrappedImplementerOf<I>> v;
  return v;
}
// The accessor for `v`, or null when it is not a handle of a wrapped class
// implementing I (a missing `_state_fn` slot reads as -1, which no class has).
template <class I>
inline WrappedImplementerOf<I> wrapped_implementer(JitValue v) {
  if (v.tag != TAG_OBJECT) return nullptr;
  auto& registry = wrapped_implementers<I>();
  auto it = registry.find(
      _jit_handle_long(reinterpret_cast<JitObject*>(v.data), "_state_fn"));
  return it == registry.end() ? nullptr : it->second;
}

}  // namespace culebra
