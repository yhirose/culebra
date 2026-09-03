#pragma once

// What the canonical signature tables say a built-in method's signature is,
// and whether a given call site binds against it. Read at compile time by
// vm::Compiler, which bakes the verdict into the chunk so both compiled lanes
// raise the same diagnostic. The data is canon_sigs.h — generated from the
// tree-walker's own parameter tables while both engines existed — so one copy
// of the binder's rules serves the whole front end; nothing here touches LLVM.

#include <canon_sigs.h>   // CanonSig / the per-receiver signature tables
#include <rt.h>           // the TAG_* receiver tags (rt_value.inc.h)
#include <parser.h>       // ArgScan, scan_arg_list
#include <shared.h>       // the arity / kwargs error message builders

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace culebra {

// Value-typed receiver tags paired with their builtin method table —
// the single source for both the arity check and the iterator-method
// receiver gate.
using BuiltinTable = CanonSigTable;
inline const std::vector<std::pair<int8_t, const BuiltinTable*>>&
builtin_value_tables() {
  static const std::vector<std::pair<int8_t, const BuiltinTable*>> tables{
      {TAG_ARRAY, &canon_array_sigs()},
      {TAG_STRING, &canon_string_sigs()},
      {TAG_STRINGVIEW, &canon_string_sigs()},
      {TAG_SET, &canon_set_sigs()},
      {TAG_TUPLE, &canon_tuple_sigs()},
      {TAG_TENSOR, &canon_tensor_sigs()},
  };
  return tables;
}

// The dict table — the builtins any Object receiver resolves
// (keys/has/size/...).
inline const BuiltinTable* dict_builtin_table() {
  return &canon_object_sigs();
}

// The declared signature of `method` in one builtin table, or null when the
// table has no entry of the name.
inline const CanonSig* builtin_method_sig(const BuiltinTable& tbl,
                                          const std::string& method) {
  auto it = tbl.find(method);
  return it == tbl.end() ? nullptr : it->second;
}

// What a builtin table does with `method` for a given argument shape: it
// does not have the name (Absent), it binds cleanly (Valid), or it raises
// (Error). The single reader of the canonical parameter lists, so no
// compiled lane can answer differently from it or from each other: the JIT
// turns Error into a tag-guarded throw, and the VM bakes the verdict per
// receiver tag — and treats Valid on a method it has no implementation for
// as out of its slice rather than guessing.
//
// A pure positional builtin uses the count-based ArityError; a kwarg-capable
// one (a defaulted / keyword-only / **rest param, e.g. sort_by's `reverse:`)
// mirrors the general binder, so adding a keyword-only param to any builtin
// stays symmetric with no per-method error code anywhere.
struct BuiltinVerdict {
  enum class Kind { Absent, Valid, Error };
  Kind kind = Kind::Absent;
  std::string err_kind, msg;
  bool at_call_root = false;  // else the ARGUMENTS node
};
inline BuiltinVerdict builtin_call_verdict(const BuiltinTable& tbl,
                                           const std::string& method,
                                           const ArgScan& scan,
                                           int64_t argc) {
  using K = BuiltinVerdict::Kind;
  auto err = [](const char* k, std::string m, bool root) {
    return BuiltinVerdict{K::Error, k, std::move(m), root};
  };
  const auto* sig = builtin_method_sig(tbl, method);
  if (!sig) return {};
  bool kwcap = sig->min_arity != sig->max_arity ||
               sig->first_kw_only_idx >= 0 || sig->kwargs_rest_idx >= 0;
  if (!kwcap) {
    if (sig->variadic || (argc >= sig->min_arity && argc <= sig->max_arity))
      return {K::Valid, {}, {}, false};
    return err("ArityError",
               builtin_arity_error_message(method, sig->min_arity,
                                           sig->max_arity, argc),
               false);
  }
  // Kwarg-capable: replicate the binder's order (too-many positional →
  // per-param missing / positional+keyword conflict → leftover unknown
  // keyword). Args are static here, so the whole check is compile-time.
  std::span<const CanonParam> params(sig->params,
                                     static_cast<size_t>(sig->n_params));
  if (!scan.splats.empty()) return {K::Valid, {}, {}, false};  // binds later
  auto n_pos = static_cast<int64_t>(scan.positional.size());
  bool has_rest = sig->kwargs_rest_idx >= 0;
  auto named = [&](std::string_view n) { return scan.kwarg(n) != nullptr; };
  if (!sig->variadic && n_pos > sig->max_arity)
    return err("TypeError", too_many_positionals_message(sig->max_arity, n_pos),
               true);
  for (size_t i = 0; i < params.size(); i++) {
    const auto& p = params[i];
    if (p.kwargs_rest || p.args_rest) continue;
    bool filled_pos = static_cast<long>(i) < n_pos && !p.kw_only;
    if (filled_pos && named(p.name))
      return err("TypeError", positional_kw_conflict_message(p.name), true);
    if (!filled_pos && !named(p.name) && !p.has_default)
      return err("ArityError", missing_required_arg_message(p.name), true);
  }
  if (!has_rest)
    for (const auto& [kn, _val] : scan.explicit_kwargs) {
      bool known = false;
      for (const auto& p : params)
        if (!p.kwargs_rest && p.name == kn) { known = true; break; }
      if (!known)
        return err("TypeError", unknown_kwarg_message(kn), true);
    }
  return {K::Valid, {}, {}, false};
}

// Whether every keyword this call supplies is one a builtin `method` can
// take — i.e. names a keyword-only parameter (`sort_by(f, reverse: true)`),
// the sole keyword shape built-ins accept (builtin_method_accepts_keyword).
// A `**` splat names nothing statically, so it never qualifies. The
// receiver's type is a run-time fact, so this over-approximates across the
// tables, exactly like emit_builtin_arity_check's own lookup; a receiver
// that turns out not to resolve the name took the UFCS arm before this.
inline bool builtin_method_keywords_bindable(const std::string& method,
                                             const peg::Ast& args_ast) {
  auto scan = scan_arg_list(args_ast);
  if (!scan.splats.empty()) return false;
  if (scan.explicit_kwargs.empty()) return true;
  // The signatures depend only on `method`; look each table up once.
  std::vector<std::span<const CanonParam>> params;
  auto note = [&](const CanonSig* s) {
    if (s)
      params.push_back(
          std::span(s->params, static_cast<size_t>(s->n_params)));
  };
  note(builtin_method_sig(*dict_builtin_table(), method));
  note(builtin_method_sig(canon_iterator_sigs(), method));
  for (auto& [tag, table] : builtin_value_tables())
    note(builtin_method_sig(*table, method));
  for (const auto& [kw, _val] : scan.explicit_kwargs) {
    bool ok = std::any_of(params.begin(), params.end(), [&](const auto& ps) {
      return builtin_method_accepts_keyword(ps, kw);
    });
    if (!ok) return false;
  }
  return true;
}

}  // namespace culebra
