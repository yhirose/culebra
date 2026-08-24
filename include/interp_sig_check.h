#pragma once

// The interp-vs-canonical-signature 1:1 startup check (see the full story
// on the function below). Include from a TU that holds BOTH stacks (main.cc,
// culebra_rt.cc), AFTER stdlib_interp.h and stdlib_jit.h; the static init at
// the bottom installs the check into stdlib_jit.h's _canon_sig_check_hook.
// Deleted with the tree-walker in B7-f.

#ifndef NDEBUG
namespace culebra {
// TEMPORARY (Phase 4 B7-a → deleted with the tree-walker in B7-f): while both
// engines exist, verify at first slow-path resolve that the generated
// canonical signature table — and the runtime lookup over it (_canon_sig's
// keying, the wrap-row synthesis) — answers exactly what the interp's own
// parameter lists say, field for field. `just check-canon-sigs` already pins
// the generated FILE to the interp source; this check pins the LOOKUP: a
// keying bug (wrong ns/sub split, a wrap row missing its params) would pass
// the file gate and still be caught here. Also keeps the old
// namespace-coverage drift check (now also enforced at generation time).
inline void _check_canon_sigs_impl() {
  static const bool checked = []() {
    static const std::shared_ptr<culebra::Environment> env_holder =
        culebra::environment();
    const auto& env = *env_holder;
    auto fail = [](const std::string& what) {
      std::fprintf(stderr, "culebra: canonical-signature drift — %s\n",
                   what.c_str());
      std::abort();
    };
    // The interp-side resolution the deleted _canon_fn performed. A copy of
    // the same logic the generator uses, so it cannot answer differently
    // from generation — the value of this check is pinning the RUNTIME
    // LOOKUP (the registry keying, the wrap-row synthesis), which the
    // `--check` file gate cannot see.
    auto interp_fn =
        [&](const NsMethod& m) -> const culebra::FunctionValue* {
      const culebra::Value* fnv = nullptr;
      if (m.ns == nullptr || m.ns[0] == '\0') {
        auto it = env.dictionary.find(std::string(m.name));
        if (it != env.dictionary.end()) {
          fnv = &it->second.val;
        } else {
          auto io_it = env.dictionary.find("IO");
          if (io_it != env.dictionary.end() &&
              io_it->second.val.type == culebra::Value::Object) {
            const auto& io_obj = io_it->second.val.to_object();
            if (io_obj.has(m.name)) fnv = &io_obj.get(m.name);
          }
        }
      } else {
        auto it = env.dictionary.find(std::string(m.ns));
        if (it == env.dictionary.end() ||
            it->second.val.type != culebra::Value::Object) return nullptr;
        const auto& ns_obj = it->second.val.to_object();
        if (m.sub != nullptr && m.sub[0] != '\0') {
          if (!ns_obj.has(m.sub)) return nullptr;
          const auto& sub_v = ns_obj.get(m.sub);
          if (sub_v.type != culebra::Value::Object) return nullptr;
          const auto& sub_obj = sub_v.to_object();
          if (!sub_obj.has(m.name)) return nullptr;
          fnv = &sub_obj.get(m.name);
        } else {
          if (!ns_obj.has(m.name)) return nullptr;
          fnv = &ns_obj.get(m.name);
        }
      }
      if (fnv == nullptr || fnv->type != culebra::Value::Function)
        return nullptr;
      return &fnv->get<culebra::FunctionValue>();
    };
    auto compare = [&](const std::string& label, const CanonSig* s,
                       const culebra::FunctionValue* fn) {
      if ((s == nullptr) != (fn == nullptr)) {
        fail(label + ": table " + (s ? "resolves" : "misses") +
             " but interp " + (fn ? "resolves" : "misses"));
      }
      if (!s) return;
      const auto& ps = *fn->params;
      if (static_cast<int>(ps.size()) != s->n_params)
        fail(label + ": param count " + std::to_string(s->n_params) + " vs " +
             std::to_string(ps.size()));
      for (int i = 0; i < s->n_params; i++) {
        const auto& a = s->params[i];
        const auto& b = ps[static_cast<size_t>(i)];
        bool b_has_default =
            b.default_expr != nullptr || b.default_value != nullptr;
        if (a.name != b.name || a.type != b.type_name ||
            a.kw_only != b.kw_only || a.kwargs_rest != b.kwargs_rest ||
            a.args_rest != b.args_rest || a.mut != b.mut ||
            a.has_default != b_has_default) {
          fail(label + ": param " + std::to_string(i) + " ('" +
               std::string(b.name) + "') differs");
        }
        if (a.has_default) {
          const auto& v = *b.default_value;
          bool ok = false;
          switch (a.default_kind) {
            case CanonDefault::Nil: ok = v.type == culebra::Value::Nil; break;
            case CanonDefault::Bool:
              ok = v.type == culebra::Value::Bool &&
                   (v.get<bool>() ? 1 : 0) == a.default_bits;
              break;
            case CanonDefault::Long:
              ok = v.type == culebra::Value::Long &&
                   v.get<int64_t>() == a.default_bits;
              break;
            case CanonDefault::Float:
              ok = v.type == culebra::Value::Float &&
                   std::bit_cast<int64_t>(v.get<double>()) == a.default_bits;
              break;
            case CanonDefault::Str:
              ok = v.type == culebra::Value::String &&
                   v.to_string_view() == a.default_str;
              break;
            default: break;
          }
          if (!ok)
            fail(label + ": param '" + std::string(b.name) +
                 "' default value differs");
        }
      }
      if (s->return_type != std::string_view(fn->return_type))
        fail(label + ": return type differs");
      auto bnd = culebra::builtin_arity_bounds(ps);
      if (bnd.min != s->min_arity || bnd.max != s->max_arity ||
          bnd.variadic != s->variadic)
        fail(label + ": arity bounds differ");
    };
    auto check_row = [&](const NsMethod& m) {
      std::string label = std::string(m.ns ? m.ns : "");
      if (m.sub && m.sub[0]) label += std::string(".") + m.sub;
      if (!label.empty()) label += ".";
      label += m.name;
      compare(label, _canon_sig(&m), interp_fn(m));
    };
    for (const auto& m : kNsMethods) check_row(m);
    for (const auto& m : kBuiltinFns) check_row(m);
    for (const auto& m : _wrapped_ns_methods()) check_row(m);
    // The value-type built-in tables, both directions.
    auto check_table = [&](const auto& interp_tbl,
                           const culebra::CanonSigTable& canon_tbl,
                           const char* tname) {
      size_t fn_entries = 0;
      for (const auto& [name, mapped] : interp_tbl) {
        const culebra::Value& fnv = [&]() -> const culebra::Value& {
          if constexpr (std::is_same_v<std::decay_t<decltype(mapped)>,
                                       culebra::IterBuiltin>) {
            return mapped.fn;
          } else {
            return mapped;
          }
        }();
        if (fnv.type != culebra::Value::Function) continue;
        fn_entries++;
        auto it = canon_tbl.find(name);
        std::string label = std::string(tname) + "." + std::string(name);
        if (it == canon_tbl.end()) fail(label + ": missing from canon table");
        compare(label, it->second, &fnv.get<culebra::FunctionValue>());
      }
      if (fn_entries != canon_tbl.size())
        fail(std::string(tname) + ": canon table has extra entries");
    };
    check_table(culebra::ObjectValue().builtins(), culebra::canon_object_sigs(),
                "Object");
    check_table(culebra::ArrayValue().builtins(), culebra::canon_array_sigs(),
                "Array");
    check_table(culebra::string_builtins(), culebra::canon_string_sigs(),
                "String");
    check_table(culebra::set_builtins(), culebra::canon_set_sigs(), "Set");
    check_table(culebra::tuple_builtins(), culebra::canon_tuple_sigs(),
                "Tuple");
    check_table(culebra::TensorValue(culebra::TensorPtr{}).builtins(),
                culebra::canon_tensor_sigs(), "Tensor");
    check_table(culebra::iterator_builtins(), culebra::canon_iterator_sigs(),
                "Iterator");
    // Namespace coverage (kept from the original drift check; also enforced
    // by the generator at generation time).
    for (const auto& [key, sym] : env.dictionary) {
      if (sym.val.type != culebra::Value::Object) continue;
      if (!_is_known_ns(key))
        fail("interp binds namespace '" + std::string(key) +
             "' but stdlib_jit.h::kNsMethods does not cover it");
    }
    return true;
  }();
  (void)checked;
}

inline const bool _canon_sig_check_installed = [] {
  _canon_sig_check_hook() = &_check_canon_sigs_impl;
  return true;
}();

}  // namespace culebra
#endif
