#pragma once

// The compiled-lane registry rows a wrap.h class declaration produces:
// WrappedNsRow (an NsMethod-shaped row per ctor/static, with the full
// declared signature) and the class-name list. Split out of wrap.h so
// stdlib_jit.h can read the registries without wrap.h's interp half
// (Phase 4 B7-b); wrap.h includes this and pushes into both.

#include <rt.h>  // JitValue (the adapter signature)

#include <string>
#include <vector>

namespace culebra {

// One NsMethod-shaped row per ctor/static of a wrapped class.
// stdlib_jit.h materializes NsMethod rows from these (lazily, after
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
  // stdlib_jit.h are unreachable for wrap rows — delete, don't rediscover.
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

}  // namespace culebra
