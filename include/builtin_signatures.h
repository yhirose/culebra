#pragma once

// What the interp's own parameter tables say a built-in method's signature is,
// and whether a given call site binds against it. Read at compile time by
// vm::Compiler, which bakes the verdict into the chunk so both compiled lanes
// raise the same diagnostic; nothing here touches LLVM, so it sits beside the
// runtime layer rather than inside the JIT, and one copy of the interp binder's
// rules serves the whole front end.

#include <interpreter.h>  // the builtin tables, FunctionValue, IterBuiltin
#include <rt.h>           // the TAG_* receiver tags (jit_value.h)
#include <parser.h>       // ArgScan, scan_arg_list, builtin_arity_bounds
#include <shared.h>       // the arity / kwargs error message builders

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace culebra {

// Value-typed receiver tags paired with their builtin method table —
// the single source for both the arity check and the iterator-method
// receiver gate. `builtins()` returns a function-local static, so
// throwaway instances suffice and the table pointers outlive them;
// built once.
using BuiltinTable = std::unordered_map<std::string_view, Value>;
inline const std::vector<std::pair<int8_t, const BuiltinTable*>>&
builtin_value_tables() {
  static const std::vector<std::pair<int8_t, const BuiltinTable*>> tables =
      [] {
        ArrayValue arr;
        TensorValue ten{nullptr};
        return std::vector<std::pair<int8_t, const BuiltinTable*>>{
            {TAG_ARRAY, &arr.builtins()},
            {TAG_STRING, &string_builtins()},
            {TAG_STRINGVIEW, &string_builtins()},
            {TAG_SET, &set_builtins()},
            {TAG_TUPLE, &tuple_builtins()},
            {TAG_TENSOR, &ten.builtins()},
        };
      }();
  return tables;
}

// The dict table — the builtins any Object receiver resolves
// (keys/has/size/...). Same throwaway-instance reasoning as above.
inline const BuiltinTable* dict_builtin_table() {
  static const BuiltinTable* table = [] {
    ObjectValue o;
    return &o.builtins();
  }();
  return table;
}

// The declared parameters of `method` in one builtin table, or null when
// that table has no Function-valued entry of the name. Generic over the
// mapped type: the value tables map to Value, iterator_builtins() to
// IterBuiltin{fn, kind}.
template <class Table>
inline const std::vector<FunctionValue::Parameter>* builtin_method_params(
    const Table& tbl, const std::string& method) {
  auto it = tbl.find(method);
  if (it == tbl.end()) return nullptr;
  const Value& fnv = [&]() -> const Value& {
    if constexpr (std::is_same_v<std::decay_t<decltype(it->second)>,
                                 IterBuiltin>) {
      return it->second.fn;
    } else {
      return it->second;
    }
  }();
  if (fnv.type != Value::Function) return nullptr;
  return fnv.get<FunctionValue>().params.get();
}

// What a builtin table does with `method` for a given argument shape: it
// does not have the name (Absent), it binds cleanly (Valid), or it raises
// (Error). The single reader of the interp's own parameter lists, so no
// compiled lane can answer differently from it or from each other: the JIT
// turns Error into a tag-guarded throw, and the VM bakes the verdict per
// receiver tag — and treats Valid on a method it has no implementation for
// as out of its slice rather than guessing.
//
// A pure positional builtin uses the count-based ArityError; a kwarg-capable
// one (a defaulted / keyword-only / **rest param, e.g. sort_by's `reverse:`)
// mirrors interp's general binder, so adding a keyword-only param to any
// builtin stays symmetric with no per-method error code anywhere. Generic
// over the mapped type: the value tables map to Value, iterator_builtins()
// to IterBuiltin{fn, kind}.
struct BuiltinVerdict {
  enum class Kind { Absent, Valid, Error };
  Kind kind = Kind::Absent;
  std::string err_kind, msg;
  bool at_call_root = false;  // else the ARGUMENTS node
};
template <class Table>
inline BuiltinVerdict builtin_call_verdict(const Table& tbl,
                                           const std::string& method,
                                           const ArgScan& scan,
                                           int64_t argc) {
  using K = BuiltinVerdict::Kind;
  auto err = [](const char* k, std::string m, bool root) {
    return BuiltinVerdict{K::Error, k, std::move(m), root};
  };
  const auto* pp = builtin_method_params(tbl, method);
  if (!pp) return {};
  const auto& params = *pp;
  auto b = builtin_arity_bounds(params);
  bool kwcap = b.min != b.max;
  for (const auto& p : params)
    if (p.kw_only || p.kwargs_rest) kwcap = true;
  if (!kwcap) {
    if (b.variadic || (argc >= b.min && argc <= b.max))
      return {K::Valid, {}, {}, false};
    return err("ArityError",
               builtin_arity_error_message(method, b.min, b.max, argc),
               false);
  }
  // Kwarg-capable: replicate interp's bind order (too-many positional →
  // per-param missing / positional+keyword conflict → leftover unknown
  // keyword). Args are static here, so the whole check is compile-time.
  if (!scan.splats.empty()) return {K::Valid, {}, {}, false};  // binds later
  auto n_pos = static_cast<int64_t>(scan.positional.size());
  bool has_rest = false;
  for (const auto& p : params) if (p.kwargs_rest) has_rest = true;
  auto named = [&](std::string_view n) { return scan.kwarg(n) != nullptr; };
  if (!b.variadic && n_pos > b.max)
    return err("TypeError", too_many_positionals_message(b.max, n_pos), true);
  for (size_t i = 0; i < params.size(); i++) {
    const auto& p = params[i];
    if (p.kwargs_rest || p.args_rest) continue;
    bool filled_pos = static_cast<long>(i) < n_pos && !p.kw_only;
    if (filled_pos && named(p.name))
      return err("TypeError", positional_kw_conflict_message(p.name), true);
    bool has_def = p.default_expr != nullptr || p.default_value != nullptr;
    if (!filled_pos && !named(p.name) && !has_def)
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
  // The parameter lists depend only on `method`; look each table up once.
  std::vector<const std::vector<FunctionValue::Parameter>*> params;
  auto note = [&](const std::vector<FunctionValue::Parameter>* p) {
    if (p) params.push_back(p);
  };
  note(builtin_method_params(*dict_builtin_table(), method));
  note(builtin_method_params(iterator_builtins(), method));
  for (auto& [tag, table] : builtin_value_tables())
    note(builtin_method_params(*table, method));
  for (const auto& [kw, _val] : scan.explicit_kwargs) {
    bool ok = std::any_of(params.begin(), params.end(), [&](const auto* p) {
      return builtin_method_accepts_keyword(*p, kw);
    });
    if (!ok) return false;
  }
  return true;
}

}  // namespace culebra

