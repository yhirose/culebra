#pragma once

#include <parser.h>
#include <shared.h>
#include <tensor.h>
#include <unicodelib.h>
#include <unicodelib_encodings.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <print>
#include <queue>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace culebra {

struct Value;
struct Symbol;
struct Environment;

// RAII drop helpers; defined at the bottom of this header where
// Environment and Value are complete. See the docblock at their
// definitions for semantics.
struct OrderedSymbolMap;
struct ValueHash;
struct ValueEq;
inline void _call_drop_if_present(OrderedSymbolMap* m);
inline void _destroy_prop_map(OrderedSymbolMap* m);

// Forward decls for method tables defined once FunctionValue and Value
// are complete. `string_builtins()` is the primitive String's method
// table. `iterator_builtins()` holds the lazy Iterator method chain
// (map / filter / take / ... / collect); any Object that has both
// `iter` and `next` properties picks these up via duck-typed fallback
// in eval_property.
inline std::map<std::string_view, Value>& string_builtins();
inline std::map<std::string_view, Value>& iterator_builtins();

// Raise the uniform shadow-prohibition error. Used by all sites that
// would introduce a binding shadowing a closure-captured variable:
// `let`/`mut` declarations, function parameters, and `match` pattern
// bindings — in both the interpreter and the JIT.
[[noreturn]] inline void throw_shadow_error(std::string_view name,
                                            size_t line, size_t column) {
  throw CulebraError(
      "ShadowError",
      std::format("cannot shadow outer variable '{}' (declared in an enclosing "
                  "function) at {}:{}.",
                  name, line, column),
      static_cast<long>(line), static_cast<long>(column));
}

// --- Static shadow analyzer ---
//
// Walks the AST once before eval, raising ShadowError at any binding
// site that would shadow a name from an enclosing function scope. This
// matches the JIT's compile-time check (jit.h `collect_fn_locals` /
// `visit_for_frees`) so both backends reject shadow violations
// uniformly — including dead code that never executes.
//
// `outer[0]` is the top-level scope: those names act as globals and
// may be shadowed freely. `outer[1..]` are enclosing function scopes
// whose names would be captured by the current function and so are
// off-limits for re-binding.

// `_` is the non-binding sink in patterns and parameters.
inline bool is_sink_name(std::string_view s) { return s == "_"; }

// --- Multimethod dispatch (shared between interp and JIT) ---

// Specificity score for a (param_type, arg_type) pair. Higher = more
// specific. -1 means no match.
inline int multifn_specificity(std::string_view param_type,
                                std::string_view arg_type) {
  if (param_type.empty() || param_type == "Any") return 0;
  if (param_type == "Object" && arg_type != "Object") return 1;
  if (param_type == arg_type) return 2;
  return -1;
}

// Pick the most specific matching entry. Returns:
//   idx >= 0  : matching entry index
//   -1        : no match
//   -2        : ambiguous (tie at the top)
// `params_of(entry)` must return a container of param-type strings.
template <class Entry, class ParamsOf>
inline int64_t multifn_pick(const std::vector<Entry>& methods,
                             const std::vector<std::string_view>& arg_types,
                             ParamsOf params_of) {
  std::vector<int> score(arg_types.size());
  std::vector<int> best_score(arg_types.size());
  size_t best_idx = 0;
  bool have_best = false;
  bool ambiguous = false;
  for (size_t i = 0; i < methods.size(); i++) {
    const auto& params = params_of(methods[i]);
    if (params.size() != arg_types.size()) continue;
    bool ok = true;
    for (size_t p = 0; p < arg_types.size(); p++) {
      int s = multifn_specificity(params[p], arg_types[p]);
      if (s < 0) { ok = false; break; }
      score[p] = s;
    }
    if (!ok) continue;
    if (!have_best) {
      best_idx = i;
      best_score = score;
      have_best = true;
      continue;
    }
    bool better_any = false, worse_any = false;
    for (size_t p = 0; p < arg_types.size(); p++) {
      if (score[p] > best_score[p]) better_any = true;
      if (score[p] < best_score[p]) worse_any = true;
    }
    if (better_any && !worse_any) {
      best_idx = i;
      best_score = score;
      ambiguous = false;
    } else if (!better_any && !worse_any) {
      ambiguous = true;
    }
  }
  if (!have_best) return -1;
  if (ambiguous)  return -2;
  return static_cast<int64_t>(best_idx);
}

namespace _shadow {

inline void check(std::string_view name, size_t line, size_t col,
                  const std::vector<const std::set<std::string, std::less<>>*>& outer) {
  if (is_sink_name(name)) return;
  for (size_t i = 1; i < outer.size(); i++) {
    if (outer[i]->contains(name)) {
      throw_shadow_error(name, line, col);
    }
  }
}

}  // namespace _shadow

// Invoke `f(name, line, column)` for each identifier that `pattern`
// would bind on a successful match. `_` (sink) is skipped.
template <typename F>
inline void for_each_pattern_binding(const peg::Ast& pattern, F&& f) {
  using namespace peg::udl;
  if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
    for (auto& sub : pattern.nodes) for_each_pattern_binding(*sub, f);
    return;
  }
  auto emit = [&](std::string_view name, size_t line, size_t col) {
    if (!is_sink_name(name)) f(name, line, col);
  };
  switch (pattern.tag) {
    case "IDENTIFIER"_:
      emit(pattern.token, pattern.line, pattern.column);
      return;
    case "TYPED_IDENT"_: {
      auto& id = *pattern.nodes[0];
      emit(id.token, id.line, id.column);
      return;
    }
    case "ARRAY_PATTERN"_:
    case "TUPLE_PATTERN"_:
      for (auto& e : pattern.nodes) for_each_pattern_binding(*e, f);
      return;
    case "REST_PATTERN"_: {
      auto& id = *pattern.nodes[0];
      emit(id.token, id.line, id.column);
      return;
    }
    case "OBJECT_PATTERN"_:
      for (auto& entry : pattern.nodes) {
        // `OBJECT_PAT_ENTRY` carries [IDENTIFIER, PATTERN]; recurse
        // into the sub-pattern. Bare IDENTIFIER is the shorthand form
        // — bind the key as the slot name.
        if (entry->tag == "OBJECT_PAT_ENTRY"_) {
          for_each_pattern_binding(*entry->nodes[1], f);
        } else {
          emit(entry->token, entry->line, entry->column);
        }
      }
      return;
    default:
      return;
  }
}

namespace _shadow {

inline void check_pattern(
    const peg::Ast& pattern,
    const std::vector<const std::set<std::string, std::less<>>*>& outer) {
  for_each_pattern_binding(
      pattern, [&](std::string_view name, size_t line, size_t col) {
        check(name, line, col, outer);
      });
}

void analyze_fn_body(const peg::Ast& params_ast, const peg::Ast& body_ast,
                     std::vector<const std::set<std::string, std::less<>>*>& outer);

// Phase 1: walk this function's body, collecting binding sites into
// `locals` and checking shadow violations. Stops at nested function
// boundaries (recursed into in phase 2).
inline void collect_locals(
    const peg::Ast& node, std::set<std::string, std::less<>>& locals,
    const std::vector<const std::set<std::string, std::less<>>*>& outer) {
  using namespace peg::udl;
  if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) return;

  if (node.tag == "MATCH"_) {
    // Match-arm bindings are arm-scoped at runtime, but we register
    // them in the enclosing function's locals so nested closures
    // inside an arm body can capture them. This over-approximates
    // (a closure in a *different* arm would see the previous arm's
    // names too) but only affects free-var resolution, never the
    // shadow check itself — `check` looks at outer[1..] only.
    for (auto& arm : node.nodes[1]->nodes) {
      check_pattern(*arm->nodes[0], outer);
      for_each_pattern_binding(
          *arm->nodes[0], [&](std::string_view name, size_t, size_t) {
            locals.insert(std::string(name));
          });
    }
    // fall through to walk arm bodies
  }

  if (node.tag == "TRY"_) {
    auto& id = *node.nodes[1];
    auto name = std::string(id.token);
    check(name, id.line, id.column, outer);
    if (!is_sink_name(name)) locals.insert(name);
    // fall through
  }

  if (node.tag == "FOR"_) {
    auto& id = *node.nodes[0];
    auto name = std::string(id.token);
    check(name, id.line, id.column, outer);
    // FOR binding is block-scoped; deliberately not added to enclosing
    // locals so a same-named binding outside the loop doesn't see it.
    collect_locals(*node.nodes[1], locals, outer);
    collect_locals(*node.nodes[2], locals, outer);
    return;
  }

  if (node.tag == "ASSIGNMENT"_) {
    auto lvalcnt = static_cast<int>(node.nodes.size()) - 4;
    auto op_tok = node.nodes[node.nodes.size() - 2]->token;
    bool compound = op_tok != "=";
    if (lvalcnt == 1 && !compound) {
      auto ident_node = node.nodes[2];
      if (ident_node->tag == "IDENTIFIER"_) {
        auto name = std::string(ident_node->token);
        bool is_let = (node.nodes[0]->token == "let");
        bool is_mut = (node.nodes[1]->token == "mut");
        if (is_let || is_mut) {
          check(name, ident_node->line, ident_node->column, outer);
          if (!is_sink_name(name)) locals.insert(name);
        } else {
          // Bare assignment is auto-local only when the name doesn't
          // already exist in any outer scope.
          bool in_outer = false;
          for (auto* s : outer) {
            if (s->contains(name)) {
              in_outer = true;
              break;
            }
          }
          if (!in_outer && !is_sink_name(name)) locals.insert(name);
        }
      }
    }
    collect_locals(*node.nodes.back(), locals, outer);
    return;
  }

  if (node.tag == "CLASS_DECL"_ || node.tag == "MULTIFN_DECL"_) {
    // Skip leading DECORATOR children (added grammar form
    // `@expr ... fn name() {...}` / `@expr ... class Name {...}`).
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
      collect_locals(*node.nodes[i], locals, outer);
      i++;
    }
    auto& id = *node.nodes[i];
    auto name = std::string(id.token);
    check(name, id.line, id.column, outer);
    if (!is_sink_name(name)) locals.insert(name);
    return;
  }

  // DESTRUCTURE_ASSIGN binds a pattern to a value.
  if (node.tag == "DESTRUCTURE_ASSIGN"_) {
    const auto& pattern = *node.nodes[1];
    check_pattern(pattern, outer);
    for_each_pattern_binding(
        pattern, [&](std::string_view name, size_t, size_t) {
          locals.insert(std::string(name));
        });
    collect_locals(*node.nodes[2], locals, outer);
    return;
  }

  for (auto& c : node.nodes) collect_locals(*c, locals, outer);
}

// Phase 2: descend into nested function bodies with the now-populated
// outer chain.
inline void descend_into_nested(
    const peg::Ast& node,
    const std::set<std::string, std::less<>>& my_locals,
    std::vector<const std::set<std::string, std::less<>>*>& outer) {
  using namespace peg::udl;

  if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) {
    outer.push_back(&my_locals);
    analyze_fn_body(*node.nodes[0], *node.nodes[1], outer);
    outer.pop_back();
    return;
  }

  if (node.tag == "MULTIFN_DECL"_) {
    // [DECORATOR*, IDENTIFIER, PARAMETERS, (RETURN_TYPE)?, BLOCK]
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
      // Decorators are evaluated in the outer scope, not the fn's
      // inner scope — descend with the current `outer`.
      descend_into_nested(*node.nodes[i], my_locals, outer);
      i++;
    }
    outer.push_back(&my_locals);
    analyze_fn_body(*node.nodes[i + 1], *node.nodes.back(), outer);
    outer.pop_back();
    return;
  }

  if (node.tag == "DEFER"_) {
    // [BLOCK] — same scope rules as a 0-param nested function.
    outer.push_back(&my_locals);
    std::set<std::string, std::less<>> defer_locals;
    collect_locals(*node.nodes[0], defer_locals, outer);
    descend_into_nested(*node.nodes[0], defer_locals, outer);
    outer.pop_back();
    return;
  }

  if (node.tag == "CLASS_DECL"_) {
    // [DECORATOR*, IDENTIFIER, METHOD ...] — each METHOD is
    // [IDENTIFIER, PARAMETERS, BLOCK].
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
      descend_into_nested(*node.nodes[i], my_locals, outer);
      i++;
    }
    for (size_t j = i + 1; j < node.nodes.size(); j++) {
      const auto& method = *node.nodes[j];
      outer.push_back(&my_locals);
      analyze_fn_body(*method.nodes[1], *method.nodes[2], outer);
      outer.pop_back();
    }
    return;
  }

  for (auto& c : node.nodes) descend_into_nested(*c, my_locals, outer);
}

inline void analyze_fn_body(
    const peg::Ast& params_ast, const peg::Ast& body_ast,
    std::vector<const std::set<std::string, std::less<>>*>& outer) {
  std::set<std::string, std::less<>> my_locals;
  for (auto& p : params_ast.nodes) {
    if (is_kw_only_sep(*p)) continue;
    auto [name_sv, line, col] = extract_param_name_loc(*p);
    auto name = std::string(name_sv);
    check(name, line, col, outer);
    if (!is_sink_name(name)) my_locals.insert(name);
  }
  collect_locals(body_ast, my_locals, outer);
  descend_into_nested(body_ast, my_locals, outer);
}

}  // namespace _shadow

// Top-level entry: run the static shadow check over an entire script
// AST before evaluation begins. Throws a `ShadowError` CulebraError on
// the first violation encountered. `outer` starts empty — top-level
// names enter the chain only when a nested function pushes them, so
// the first scope (`outer[0]` from a nested function's perspective)
// is the top-level / "globals" frame which `check` skips.
inline void check_shadow_static(const peg::Ast& ast) {
  std::set<std::string, std::less<>> top_locals;
  std::vector<const std::set<std::string, std::less<>>*> outer;
  _shadow::collect_locals(ast, top_locals, outer);
  _shadow::descend_into_nested(ast, top_locals, outer);
}

// Cycle collector for ObjectValue/ArrayValue (shared_ptr-based).
// Python-style mark-and-sweep; runs periodically and on program exit.
// Uses weak_ptr<void> so incomplete types (Symbol, Value) don't matter here.
// Process-wide singleton; NOT thread-safe (see THREAD SAFETY note at
// the top of the runtime section in jit.h).
//
// We only track ArrayValue's `values` vector: cycles that go entirely
// through Object property maps without an Array in between are not
// detected. This trade keeps the tracker entries down to one per
// Array (vs one per Object plus one per Array previously) and the
// per-collect walk visits roughly half the pointers. microgpt's
// cycles route Object → Array (`_children`) → Object, so they're
// still broken correctly. User code with direct Object→Object cycles
// will leak — the language spec already calls cycles a hazard and
// recommends weak refs (`drop` for explicit teardown).
struct InterpGC {
  struct Entry {
    std::weak_ptr<void> weak;
    void* ptr;
  };

  static InterpGC& instance() { return runtime_substate<InterpGC>(kSlotInterpGc); }

  ~InterpGC() { collect(); }

  template <typename T>
  void track_vec(std::shared_ptr<T> p) {
    entries_.push_back(
        {std::weak_ptr<void>(std::shared_ptr<void>(p, p.get())), p.get()});
    bump();
  }

  void collect();

 private:
  std::vector<Entry> entries_;
  size_t alloc_counter_ = 0;
  static constexpr size_t GC_MIN_THRESHOLD = 10000;
  size_t threshold_ = GC_MIN_THRESHOLD;  // adaptive; see collect().
  bool running_ = false;

  void bump() {
    if (!running_ && ++alloc_counter_ >= threshold_) {
      alloc_counter_ = 0;
      collect();
    }
  }
};

inline InterpGC& interp_gc() { return InterpGC::instance(); }

// Cycle detection during str() / out() to avoid infinite recursion on
// cyclic objects. RAII guard: inserts on construction, erases on destruction.
inline thread_local std::unordered_set<const void*> _str_visiting;

struct StrGuard {
  const void* key;
  bool already;
  explicit StrGuard(const void* k) : key(k) {
    already = !_str_visiting.insert(k).second;
  }
  ~StrGuard() {
    if (!already) _str_visiting.erase(key);
  }
};

struct FunctionValue {
  struct Parameter {
    std::string_view name;
    bool mut;
    std::string_view type_name;  // empty = no annotation
    // Default expression AST (nullptr = no default). Points into the
    // root AST, which outlives all FunctionValues for a given eval.
    const peg::Ast* default_expr = nullptr;
    // Literal default for C++-built FunctionValues (e.g. stdlib
    // entries). Used when `default_expr` is null and the slot is
    // unfilled. A null pointer means "no default" → ArityError.
    // shared_ptr (rather than `std::optional<Value>`) avoids the
    // incomplete-type cycle — Value is forward-declared at this point.
    // Use the `kw_default_*` helpers below to share canonical
    // instances of common values (false/true/0/"") so multiple
    // stdlib entries don't each allocate their own.
    std::shared_ptr<Value> default_value;
    bool kw_only = false;
    bool kwargs_rest = false;

    // Convenience constructor for a synthetic `**rest` catch-all
    // parameter (used by the multifn dispatcher to forward unknown
    // kwargs into the picked method).
    static Parameter make_kwargs_rest(std::string_view name) {
      return {name, /*mut=*/false, /*type_name=*/{},
              /*default_expr=*/nullptr, /*default_value=*/{},
              /*kw_only=*/false, /*kwargs_rest=*/true};
    }
  };

  FunctionValue(
      const std::vector<Parameter>& params,
      const std::function<Value(std::shared_ptr<Environment> env)>& eval,
      std::string_view return_type = {},
      std::shared_ptr<Environment> def_env = {})
      : params(std::make_shared<std::vector<Parameter>>(params)),
        eval(eval),
        return_type(return_type),
        def_env(std::move(def_env)) {}

  std::shared_ptr<std::vector<Parameter>> params;
  std::function<Value(std::shared_ptr<Environment> env)> eval;
  std::string_view return_type;  // empty = no annotation
  // Definition environment — used only to evaluate parameter defaults
  // (for body execution, `eval` closes over this env itself).
  std::shared_ptr<Environment> def_env;
};

struct ObjectValue {
  // Defined out-of-line after OrderedSymbolMap is complete.
  ObjectValue();

  // Synthetic ctor: skips default map allocation and GC tracking so that
  // _call_drop_if_present can build a `this` view over an existing map
  // without extra bookkeeping. Caller must assign `properties` itself.
  struct Synthetic {};
  explicit ObjectValue(Synthetic) {}

  bool has(std::string_view name) const;
  const Value& get(std::string_view name) const;
  void assign(std::string_view name, const Value& val);
  void initialize(std::string_view name, const Value& val, bool mut);

  // Non-String key overloads (Phase 7-B). `{1: "x", true: "y"}` etc.
  // String keys still flow through the std::string_view path above so
  // the existing fast paths (class method lookup, AST-token keys) are
  // unaffected.
  bool has(const Value& key) const;
  const Value& get(const Value& key) const;
  void assign(const Value& key, const Value& val);
  void initialize(const Value& key, const Value& val, bool mut);

  virtual std::map<std::string_view, Value>& builtins();

  std::shared_ptr<OrderedSymbolMap> properties;
  // Non-String key sidecar. Eager-allocated (empty) by the ctor so
  // `obj[k] = v` writes through a Value copy reach the same storage
  // as later reads — lazy alloc would mutate a per-copy shared_ptr
  // field and never propagate.
  std::shared_ptr<std::unordered_map<Value, Symbol, ValueHash, ValueEq>>
      non_string_props;
  // Unified key insertion order: every key (String and non-String) in
  // the order it was first set. str() walks this so mixed-key Objects
  // render with a true interleaved order. Eager-allocated for the same
  // copy-propagation reason. Class instances bypass `initialize()` and
  // leave this empty; str() falls back to the `properties` walk in that
  // case.
  std::shared_ptr<std::vector<Value>> key_order;
};

struct ArrayValue : public ObjectValue {
  ArrayValue() : values(std::make_shared<std::vector<Value>>()) {
    interp_gc().track_vec(values);
  }
  std::map<std::string_view, Value>& builtins() override;

  std::shared_ptr<std::vector<Value>> values;
};

// Immutable, fixed-arity sequence. Hashable when every element is.
struct TupleValue {
  std::shared_ptr<std::vector<Value>> elements;

  TupleValue() : elements(std::make_shared<std::vector<Value>>()) {}
  explicit TupleValue(std::vector<Value> v)
      : elements(std::make_shared<std::vector<Value>>(std::move(v))) {}
};

// Defined after ValueHash/ValueEq are complete (see below).
struct SetValue;
inline std::string _set_str(const Value& v);
inline bool _set_eq(const Value& a, const Value& b);

// Builtin Tensor type. The data buffer lives in a shared_ptr<TensorImpl>
// (see tensor.h); cycles are impossible because the buffer holds opaque
// bytes, so no InterpGC tracking is needed.
struct TensorValue : public ObjectValue {
  explicit TensorValue(TensorPtr i) : ObjectValue(), impl(std::move(i)) {}
  std::map<std::string_view, Value>& builtins() override;

  TensorPtr impl;
};

struct Value {
  enum Type { Nil, Bool, Long, Float, String, Object, Array, Function, Tensor, Tuple, Set };

  Value() : type(Nil) {}
  Value(const Value& rhs) : type(rhs.type), v(rhs.v) {}
  Value(Value&& rhs) : type(rhs.type), v(rhs.v) {}

  Value& operator=(const Value& rhs) {
    if (this != &rhs) {
      type = rhs.type;
      v = rhs.v;
    }
    return *this;
  }

  Value& operator=(Value&& rhs) {
    type = rhs.type;
    v = rhs.v;
    return *this;
  }

  explicit Value(bool b) : type(Bool), v(b) {}
  explicit Value(long l) : type(Long), v(l) {}
  explicit Value(double d) : type(Float), v(d) {}
  explicit Value(std::string&& s) : type(String), v(s) {}
  explicit Value(ObjectValue&& o) : type(Object), v(o) {}
  explicit Value(ArrayValue&& a) : type(Array), v(a) {}
  explicit Value(TensorValue&& t) : type(Tensor), v(t) {}
  explicit Value(FunctionValue&& f) : type(Function), v(f) {}
  explicit Value(TupleValue&& t) : type(Tuple), v(t) {}
  // Defined out-of-line so SetValue is complete here.
  explicit Value(SetValue&& s);

  bool is_numeric() const { return type == Long || type == Float; }

  // Stable name for the runtime type tag, used by the `to_X` accessors
  // below when constructing "type error: expected X, got Y" messages.
  // (The user-visible `type_of` builtin in stdlib_interp.h has its own
  // copy of this switch; both should stay in sync.)
  const char* type_name() const {
    switch (type) {
      case Nil:      return "Nil";
      case Bool:     return "Bool";
      case Long:     return "Long";
      case Float:    return "Float";
      case String:   return "String";
      case Array:    return "Array";
      case Object:   return "Object";
      case Function: return "Function";
      case Tensor:   return "Tensor";
      case Tuple:    return "Tuple";
      case Set:      return "Set";
    }
    return "?";
  }

  const TupleValue& to_tuple() const {
    if (type == Tuple) return get<TupleValue>();
    _throw_type_error("Tuple");
  }

  // Throws a "type error: expected X, got Y" runtime_error. Caller-side
  // messages have no trailing period — the eval() wrap appends one
  // along with " at L:C." so the format stays consistent.
  [[noreturn]] void _throw_type_error(const char* expected) const {
    throw CulebraError("TypeError", std::format(
        "type error: expected {}, got {}", expected, type_name()));
  }

  double to_double_coerce() const {
    switch (type) {
      case Long:
        return static_cast<double>(get<long>());
      case Float:
        return get<double>();
      default:
        _throw_type_error("Long or Float");
    }
  }

  template <typename T>
  T& get() {
    return std::any_cast<T&>(v);
  }

  template <typename T>
  const T& get() const {
    return std::any_cast<const T&>(v);
  }

  bool to_bool() const {
    switch (type) {
      case Bool:
        return get<bool>();
      case Long:
        return get<long>() != 0;
      case Float: {
        // Python semantics: 0.0 (and -0.0) are false, every other finite
        // value is true, NaN is true (matches Python's `bool(float('nan'))`).
        auto d = get<double>();
        return d != 0.0;
      }
      default:
        _throw_type_error("Bool, Long, or Float");
    }
  }

  long to_long() const {
    switch (type) {
      // case Bool: return get<bool>();
      case Long:
        return get<long>();
      default:
        _throw_type_error("Long");
    }
  }

  std::string to_string() const {
    switch (type) {
      case String:
        return get<std::string>();
      default:
        _throw_type_error("String");
    }
  }

  FunctionValue to_function() const {
    switch (type) {
      case Function:
        return get<FunctionValue>();
      default:
        _throw_type_error("Function");
    }
  }

  const ObjectValue& to_object() const {
    switch (type) {
      case Object:
        return get<ObjectValue>();
      case Array:
        return get<ArrayValue>();
      case Tensor:
        return get<TensorValue>();
      default:
        _throw_type_error("Object, Array, or Tensor");
    }
  }

  ObjectValue& to_object() {
    switch (type) {
      case Object:
        return get<ObjectValue>();
      case Array:
        return get<ArrayValue>();
      case Tensor:
        return get<TensorValue>();
      default:
        _throw_type_error("Object, Array, or Tensor");
    }
  }

  const ArrayValue& to_array() const {
    switch (type) {
      case Array:
        return get<ArrayValue>();
      default:
        _throw_type_error("Array");
    }
  }

  const TensorValue& to_tensor() const {
    switch (type) {
      case Tensor:
        return get<TensorValue>();
      default:
        _throw_type_error("Tensor");
    }
  }
  TensorValue& to_tensor() {
    switch (type) {
      case Tensor:
        return get<TensorValue>();
      default:
        _throw_type_error("Tensor");
    }
  }

  std::string str_object() const;

  std::string str_array() const {
    const auto& av = to_array();
    auto* key = av.values.get();
    StrGuard guard(key);
    if (guard.already) return "[...]";
    const auto& values = *av.values;
    std::string s = "[";
    for (auto i = 0u; i < values.size(); i++) {
      if (i != 0) {
        s += ", ";
      }
      s += values[i].str();
    }
    s += "]";
    return s;
  }

  std::string str() const {
    switch (type) {
      case Nil:
        return "nil";
      case Bool:
        return to_bool() ? "true" : "false";
      case Long:
        return std::to_string(to_long());
      case Float:
        return str_float();
      case String:
        return std::format("'{}'", to_string());
      case Object:
        return str_object();
      case Array:
        return str_array();
      case Tensor:
        return tensor_str(*get<TensorValue>().impl);
      case Function:
        return "[function]";
      case Tuple: {
        const auto& v = *get<TupleValue>().elements;
        std::string s = "(";
        for (size_t i = 0; i < v.size(); i++) {
          if (i) s += ", ";
          s += v[i].str();
        }
        if (v.size() == 1) s += ",";
        s += ")";
        return s;
      }
      case Set: return _set_str(*this);
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
  }

  std::string str_float() const {
    return format_float_shortest(get<double>());
  }

  // Unquoted variant: strings come through bare (for interpolation / to_string).
  std::string str_display() const {
    if (type == String) return get<std::string>();
    return str();
  }

  std::ostream& out(std::ostream& os) const {
    os << str();
    return os;
  }

  bool operator==(const Value& rhs) const {
    // Numeric cross-type equality: `1 == 1.0`, `0 == 0.0`. Matches
    // Python, and keeps comparisons meaningful after a Long↔Float
    // promotion elsewhere in an expression.
    if (type != rhs.type) {
      if (is_numeric() && rhs.is_numeric()) {
        return to_double_coerce() == rhs.to_double_coerce();
      }
      return false;
    }
    switch (type) {
      case Nil:
        return true;
      case Bool:
        return get<bool>() == rhs.get<bool>();
      case Long:
        return get<long>() == rhs.get<long>();
      case Float:
        return get<double>() == rhs.get<double>();
      case String:
        return get<std::string>() == rhs.get<std::string>();
      case Tuple: {
        const auto& a = *get<TupleValue>().elements;
        const auto& b = *rhs.get<TupleValue>().elements;
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++) {
          if (!(a[i] == b[i])) return false;
        }
        return true;
      }
      case Set: return _set_eq(*this, rhs);
      // TODO: Object and Array support
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
  }

  bool operator!=(const Value& rhs) const { return !operator==(rhs); }

  // Shared body for all four ordering operators. `cmp` is invoked on a
  // pair of doubles (numeric / Bool / String-lexicographic) or never
  // called for Nil (Nil always yields false — see spec §5).
  template <class Cmp>
  bool ord_compare(const Value& rhs, Cmp cmp) const {
    if (is_numeric() && rhs.is_numeric() && type != rhs.type) {
      return cmp(to_double_coerce(), rhs.to_double_coerce());
    }
    if (type != rhs.type) {
      throw CulebraError("TypeError", std::format(
          "type error: cannot compare {} and {}",
          type_name(), rhs.type_name()));
    }
    switch (type) {
      case Nil:
        return false;
      case Bool:
        return cmp(double(get<bool>()), double(rhs.get<bool>()));
      case Long:
        return cmp(double(get<long>()), double(rhs.get<long>()));
      case Float:
        return cmp(get<double>(), rhs.get<double>());
      case String: {
        auto c = get<std::string>().compare(rhs.get<std::string>());
        return cmp(double(c), 0.0);
      }
      // TODO: Object and Array support
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
  }

  bool operator<(const Value& rhs) const {
    return ord_compare(rhs, [](double a, double b) { return a < b; });
  }
  bool operator<=(const Value& rhs) const {
    return ord_compare(rhs, [](double a, double b) { return a <= b; });
  }
  bool operator>(const Value& rhs) const {
    return ord_compare(rhs, [](double a, double b) { return a > b; });
  }
  bool operator>=(const Value& rhs) const {
    return ord_compare(rhs, [](double a, double b) { return a >= b; });
  }

  Type type;
  std::any v;
};

struct Symbol {
  Value val;
  bool mut;
};

// Hash + equality for Value as a dictionary key. Numerically-equal
// Long / Float / Bool fall into the same bucket so `{1: "a", 1.0: "b"}`
// keeps only the second entry — matches Python's dict semantics.
// Unhashable inputs (Object / Array / Function / Tensor) throw.
struct ValueHash {
  size_t operator()(const Value& v) const {
    switch (v.type) {
      case Value::Nil:    return 0;
      case Value::Bool:   return std::hash<long>{}(v.get<bool>() ? 1 : 0);
      case Value::Long:   return std::hash<long>{}(v.get<long>());
      case Value::Float: {
        double d = v.get<double>();
        long as_long = static_cast<long>(d);
        if (std::isfinite(d) && static_cast<double>(as_long) == d) {
          return std::hash<long>{}(as_long);
        }
        return std::hash<double>{}(d);
      }
      case Value::String:
        return std::hash<std::string>{}(v.get<std::string>());
      case Value::Tuple: {
        size_t h = 0xa3b1c5d7e9f10000ULL;
        for (const auto& e : *v.get<TupleValue>().elements) {
          h ^= (*this)(e) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
      }
      default:
        throw CulebraError("TypeError", std::format(
            "unhashable type: '{}'", v.type_name()));
    }
  }
};

struct ValueEq {
  bool operator()(const Value& a, const Value& b) const { return a == b; }
};

// Unordered collection of unique hashable values.
struct SetValue {
  // shared_ptr so methods like .add()/.remove() on a copy reach the
  // same underlying storage (mirrors ArrayValue/ObjectValue semantics).
  std::shared_ptr<std::vector<Value>> members;  // insertion order
  std::shared_ptr<std::unordered_map<Value, size_t, ValueHash, ValueEq>> index;

  SetValue()
      : members(std::make_shared<std::vector<Value>>()),
        index(std::make_shared<
              std::unordered_map<Value, size_t, ValueHash, ValueEq>>()) {}

  // Insert `v` if not already present. Returns true on insert.
  bool add(const Value& v) {
    if (!index->emplace(v, members->size()).second) return false;
    members->push_back(v);
    return true;
  }
};

inline Value::Value(SetValue&& s) : type(Set), v(std::move(s)) {}

inline std::string _set_str(const Value& v) {
  const auto& xs = *v.get<SetValue>().members;
  std::string s = "{";
  for (size_t i = 0; i < xs.size(); i++) {
    if (i) s += ", ";
    s += xs[i].str();
  }
  s += "}";
  return s;
}

inline bool _set_eq(const Value& a, const Value& b) {
  const auto& ia = *a.get<SetValue>().index;
  const auto& ib = *b.get<SetValue>().index;
  if (ia.size() != ib.size()) return false;
  for (const auto& [k, _] : ia) {
    if (!ib.contains(k)) return false;
  }
  return true;
}

// Insertion-ordered string-to-Symbol map.
//
// `index_` owns the canonical key strings — unordered_map nodes are
// stable across rehashes, so the `string_view` aliases stored in
// `entries_` remain valid for the entry's lifetime. Erase is the only
// op that invalidates a node, and it removes the matching entry first.
// This lets `obj["x"] = v` insert runtime-allocated key strings
// safely (the Value-owned std::string passed at the call site would
// otherwise dangle once the call returns).
//
// `is_transparent` typedefs enable heterogeneous std::string_view
// lookups without forcing a string copy on every read.
//
// find/contains/insert are O(1) avg; erase is O(n) — entries shift
// down and indices recompute. Acceptable since Object.remove is rare.
struct OrderedSymbolMap {
  using Entry = std::pair<std::string_view, Symbol>;

  struct sv_hash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const noexcept {
      return std::hash<std::string_view>{}(sv);
    }
  };
  struct sv_equal {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
      return a == b;
    }
  };

  bool contains(std::string_view k) const { return index_.contains(k); }

  Symbol& at(std::string_view k) {
    return entries_[index_.find(k)->second].second;
  }
  const Symbol& at(std::string_view k) const {
    return entries_[index_.find(k)->second].second;
  }

  Symbol& operator[](std::string_view k) {
    auto it = index_.find(k);
    if (it != index_.end()) return entries_[it->second].second;
    auto [new_it, _] =
        index_.emplace(std::string(k), entries_.size());
    entries_.emplace_back(std::string_view(new_it->first), Symbol{});
    return entries_.back().second;
  }

  template <class S>
  std::pair<typename std::vector<Entry>::iterator, bool>
  emplace(std::string_view k, S&& s) {
    auto it = index_.find(k);
    if (it != index_.end()) {
      return {entries_.begin() + it->second, false};
    }
    auto [new_it, _] =
        index_.emplace(std::string(k), entries_.size());
    entries_.emplace_back(std::string_view(new_it->first),
                          std::forward<S>(s));
    return {entries_.begin() + (entries_.size() - 1), true};
  }

  template <class S>
  void insert_or_assign(std::string_view k, S&& s) {
    auto it = index_.find(k);
    if (it != index_.end()) {
      entries_[it->second].second = std::forward<S>(s);
    } else {
      auto [new_it, _] =
          index_.emplace(std::string(k), entries_.size());
      entries_.emplace_back(std::string_view(new_it->first),
                            std::forward<S>(s));
    }
  }

  // Compact erase: shifts later entries down and updates indices. O(n).
  size_t erase(std::string_view k) {
    auto it = index_.find(k);
    if (it == index_.end()) return 0;
    size_t idx = it->second;
    index_.erase(it);
    entries_.erase(entries_.begin() + idx);
    for (auto& [_, i] : index_) {
      if (i > idx) --i;
    }
    return 1;
  }

  size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

  auto find(std::string_view k) {
    auto it = index_.find(k);
    return it == index_.end() ? entries_.end()
                              : entries_.begin() + it->second;
  }
  auto find(std::string_view k) const {
    auto it = index_.find(k);
    return it == index_.end() ? entries_.cend()
                              : entries_.cbegin() + it->second;
  }

  auto begin() { return entries_.begin(); }
  auto end()   { return entries_.end(); }
  auto begin() const { return entries_.cbegin(); }
  auto end()   const { return entries_.cend(); }
  auto cbegin() const { return entries_.cbegin(); }
  auto cend()   const { return entries_.cend(); }

 private:
  std::vector<Entry> entries_;
  std::unordered_map<std::string, size_t, sv_hash, sv_equal> index_;
};

// Internal control-flow signal for `return expr`. Kept distinct from
// user-thrown Values so that a user `throw` propagates past function
// boundaries into the nearest `try/catch`, while `return` unwinds only
// the current function call.
struct ReturnValue {
  Value value;
};

// Internal control-flow signals for `break` / `continue`. Caught by the
// nearest enclosing loop; uncaught occurrences are a language error
// (checked at eval time via a stack-depth guard). Distinct types so a
// user `throw` cannot be mistaken for a loop signal.
struct BreakSignal {};
struct ContinueSignal {};

// Defined out-of-line so OrderedSymbolMap is complete here.
inline ObjectValue::ObjectValue() {
  auto* raw = new OrderedSymbolMap();
  properties = std::shared_ptr<OrderedSymbolMap>(raw, &_destroy_prop_map);
  // properties map is intentionally not GC-tracked — see the InterpGC
  // class header. Cycles are detected via the contained ArrayValues.
  // Sidecar maps are eager-allocated so `obj[k] = v` writes through a
  // Value copy reach the same storage as later reads. Lazy alloc would
  // mutate a per-copy shared_ptr field and never propagate.
  non_string_props = std::make_shared<
      std::unordered_map<Value, Symbol, ValueHash, ValueEq>>();
  key_order = std::make_shared<std::vector<Value>>();
}


inline std::ostream& operator<<(std::ostream& os, const Value& val) {
  return val.out(os);
}

// Canonical defaults for stdlib FunctionValue parameter lists. Each
// helper returns the SAME shared_ptr<Value> across calls, so multiple
// stdlib entries that take `lines = false` (etc.) share one allocation.
// Custom literal defaults still need a fresh `std::make_shared<Value>`
// at the call site.
inline const std::shared_ptr<Value>& kw_default_false() {
  static const auto v = std::make_shared<Value>(Value(false));
  return v;
}
inline const std::shared_ptr<Value>& kw_default_true() {
  static const auto v = std::make_shared<Value>(Value(true));
  return v;
}
inline const std::shared_ptr<Value>& kw_default_zero() {
  static const auto v = std::make_shared<Value>(Value((long)0));
  return v;
}
inline const std::shared_ptr<Value>& kw_default_one() {
  static const auto v = std::make_shared<Value>(Value((long)1));
  return v;
}
inline const std::shared_ptr<Value>& kw_default_nil() {
  static const auto v = std::make_shared<Value>(Value{});
  return v;
}

struct Environment {
  Environment(std::shared_ptr<Environment> parent = nullptr)
      : level(parent ? parent->level + 1 : 0) {}

  void append_outer(std::shared_ptr<Environment> outer) {
    if (this->outer) {
      this->outer->append_outer(outer);
    } else {
      this->outer = outer;
    }
  }

  bool has(std::string_view s) const {
    if (dictionary.find(s) != dictionary.end()) {
      return true;
    }
    return outer && outer->has(s);
  }

  // `_` is a non-binding sink: any binding form (`let _`, `for _ in`,
  // `fn(_, _, x)`, `match { _ => }`) drops the value instead of
  // introducing a name. Shadow checking happens statically via
  // `check_shadow_static` (see top of file) before evaluation begins.

  const Value& get(std::string_view s) const {
    if (auto it = dictionary.find(s); it != dictionary.end()) {
      return it->second.val;
    } else if (outer) {
      return outer->get(s);
    }
    throw CulebraError("NameError",
                       std::format("undefined variable '{}'...", s));
  }

  void assign(std::string_view s, Value val) {
    assert(has(s));
    if (auto it = dictionary.find(s); it != dictionary.end()) {
      auto& sym = it->second;
      if (!sym.mut) {
        throw CulebraError("ImmutableError",
                           std::format("cannot reassign '{}' (declared without 'mut')", s));
      }
      sym.val = std::move(val);
      return;
    }
    outer->assign(s, std::move(val));
    return;
  }

  void initialize(std::string_view s, Value val, bool mut) {
    if (is_sink_name(s)) return;
    if (auto it = dictionary.find(s); it != dictionary.end()) {
      it->second = Symbol{std::move(val), mut};
    } else {
      dictionary.emplace(std::string(s), Symbol{std::move(val), mut});
    }
  }

  size_t level;
  std::shared_ptr<Environment> outer;
  // Owned-string keys so REPL bindings survive after each input's
  // AST is destroyed. Heterogeneous lookup (`std::less<>`) lets
  // every callsite keep its `string_view` argument shape — only
  // new-entry insertion in `initialize` allocates.
  std::map<std::string, Symbol, std::less<>> dictionary;
  bool is_function_frame = false;
  // Deferred callables registered in this scope via `defer { ... }`.
  // Fired in LIFO order when the scope exits (normally or via throw).
  std::vector<std::function<void()>> deferred;
};

// Lexical scope chained to `outer`. Function frames don't use this —
// they construct `Environment(parent)` directly and look up captures
// via `def_env`, not via `outer`.
inline std::shared_ptr<Environment> make_scope(
    std::shared_ptr<Environment> outer) {
  auto env = std::make_shared<Environment>();
  env->append_outer(std::move(outer));
  return env;
}

typedef std::function<void(const peg::Ast& ast, Environment& env,
                           bool force_to_break)>
    Debugger;

inline bool ObjectValue::has(std::string_view name) const {
  if (!properties->contains(name)) {
    const auto& props = const_cast<ObjectValue*>(this)->builtins();
    return props.contains(name);
  }
  return true;
}

inline const Value& ObjectValue::get(std::string_view name) const {
  if (!properties->contains(name)) {
    const auto& props = const_cast<ObjectValue*>(this)->builtins();
    return props.at(name);
  }
  return properties->at(name).val;
}

// Validate the well-known-property contract (see shared.h)
// for a freshly-bound interpreter Value: must be a 0-arg Function.
inline void _check_drop_contract(std::string_view name, const Value& val) {
  if (!is_well_known_prop(name)) return;
  if (val.type != Value::Function) {
    throw_well_known_prop_contract_error(name);
  }
  const auto& fn = val.template get<FunctionValue>();
  if (!fn.params->empty()) {
    throw_well_known_prop_contract_error(name);
  }
}

inline void ObjectValue::assign(std::string_view name, const Value& val) {
  assert(has(name));
  auto& sym = properties->at(name);
  if (!sym.mut) {
    throw CulebraError("ImmutableError",
                       std::format("immutable property '{}'...", name));
  }
  _check_drop_contract(name, val);
  sym.val = val;
  return;
}

inline void ObjectValue::initialize(std::string_view name, const Value& val,
                                    bool mut) {
  _check_drop_contract(name, val);
  bool new_key = !properties->contains(name);
  (*properties)[name] = Symbol{val, mut};
  if (new_key) {
    key_order->push_back(Value(std::string(name)));
  }
}

// Value-keyed overloads. String keys are unified with the shape-based
// `obj.foo` path so `obj["foo"] = v` and `obj.foo = v` reach the same
// slot; every other hashable key (Long/Float/Bool/Nil/Tuple) goes to
// the sidecar.
inline bool ObjectValue::has(const Value& key) const {
  if (key.type == Value::String) {
    return properties->contains(
        std::string_view(key.template get<std::string>()));
  }
  if (non_string_props->empty()) return false;  // fast miss
  return non_string_props->contains(key);
}

inline const Value& ObjectValue::get(const Value& key) const {
  if (key.type == Value::String) {
    return properties->at(key.template get<std::string>()).val;
  }
  return non_string_props->at(key).val;
}

inline void ObjectValue::initialize(const Value& key, const Value& val,
                                    bool mut) {
  if (key.type == Value::String) {
    initialize(std::string_view(key.template get<std::string>()), val, mut);
    return;
  }
  auto [it, inserted] =
      non_string_props->try_emplace(key, Symbol{val, mut});
  if (inserted) {
    key_order->push_back(key);
  } else {
    it->second = Symbol{val, mut};
  }
}

inline void ObjectValue::assign(const Value& key, const Value& val) {
  if (key.type == Value::String) {
    assign(std::string_view(key.template get<std::string>()), val);
    return;
  }
  auto it = non_string_props->find(key);
  // Caller has already checked has(); this is defensive.
  if (it == non_string_props->end()) {
    throw CulebraError("KeyError", "key not present");
  }
  if (!it->second.mut) {
    throw CulebraError("ImmutableError",
                       "immutable entry on non-String key");
  }
  it->second.val = val;
}

// Build the structured-error Object surfaced to user `catch` blocks.
// Keys are string literals — safe for the string_view-keyed map.
inline Value make_error_object(std::string_view kind, std::string_view message,
                               long line, long col) {
  ObjectValue obj;
  obj.initialize("kind", Value(std::string(kind)), false);
  obj.initialize("message", Value(std::string(message)), false);
  obj.initialize("line", Value(line), false);
  obj.initialize("col", Value(col), false);
  return Value(std::move(obj));
}

// Runtime type check for optional annotations. "Any" matches everything.
// If `val` is a class-sugar instance, return a view of its class
// name (the synthetic `class:` String property the desugaring
// inserts). Returns nullopt for plain Objects and non-Objects. The
// view aliases the property's std::string and is valid as long as
// `val` is.
inline std::optional<std::string_view> class_tag(const Value& val) {
  if (val.type != Value::Object) return std::nullopt;
  const auto& obj = val.to_object();
  if (!obj.has("class")) return std::nullopt;
  const auto& cn = obj.get("class");
  if (cn.type != Value::String) return std::nullopt;
  return std::string_view(cn.template get<std::string>());
}

inline bool type_matches(const Value& val, std::string_view name) {
  if (name == "Any") return true;
  switch (val.type) {
    case Value::Nil:      return name == "Nil";
    case Value::Bool:     return name == "Bool";
    case Value::Long:     return name == "Long";
    case Value::Float:    return name == "Float";
    case Value::String:   return name == "String";
    case Value::Array:    return name == "Array";
    case Value::Object: {
      if (name == "Object") return true;
      auto tag = class_tag(val);
      return tag && *tag == name;
    }
    case Value::Function: return name == "Function";
    case Value::Tensor:   return name == "Tensor";
    case Value::Tuple:    return name == "Tuple";
    case Value::Set:      return name == "Set";
  }
  return false;
}

inline void check_type(const Value& val, std::string_view name,
                       std::string_view context, size_t line, size_t col) {
  if (name.empty()) return;
  if (type_matches(val, name)) return;
  throw CulebraError("TypeError", std::format(
      "type error: {} expects {} at {}:{}.", context, name, line, col),
      static_cast<long>(line), static_cast<long>(col));
}

inline std::string Value::str_object() const {
  const auto& obj = to_object();
  auto* key = obj.properties.get();
  StrGuard guard(key);
  if (guard.already) return "{...}";
  const auto& properties = *obj.properties;
  // If the object carries a String `class:` tag, hoist it as a
  // prefix so `{class: 'Matrix', rows: 2}` prints as
  // `Matrix {rows: 2}`. The `class` entry is then skipped in the
  // property list.
  std::string s;
  auto tag = class_tag(*this);
  if (tag) {
    s.assign(*tag);
    s += " ";
  }
  s += "{";
  bool first = true;
  // Walk `key_order` so String and non-String keys interleave by
  // insertion. The `class:` tag is hoisted to the prefix above and
  // skipped here.
  auto emit_entry = [&](const Value& key, const Symbol& sym) {
    if (!first) s += ", ";
    first = false;
    if (sym.mut) s += "mut ";
    if (key.type == Value::String) {
      s += key.template get<std::string>();
    } else {
      s += key.str();
    }
    s += ": ";
    s += sym.val.str();
  };
  // Walk `properties` first (covers class instances and other objects
  // built via direct `properties->emplace`, which bypass `initialize()`
  // and never push to key_order). Then append the non-String sidecar
  // entries from `key_order`. For Objects built entirely through
  // `initialize()`, key_order interleaves both — so we emit the
  // sidecar entries inline at their recorded positions instead.
  if (obj.key_order->empty() ||
      obj.key_order->size() < properties.size()) {
    // No key_order, or key_order is incomplete (class-instance hybrid):
    // fall back to property-map walk, then append sidecar entries.
    for (const auto& [name, sym] : properties) {
      if (tag && name == "class") continue;
      if (!first) s += ", ";
      first = false;
      if (sym.mut) s += "mut ";
      s += name;
      s += ": ";
      s += sym.val.str();
    }
    for (const auto& [key, sym] : *obj.non_string_props) {
      emit_entry(key, sym);
    }
  } else {
    for (const auto& key : *obj.key_order) {
      if (key.type == Value::String) {
        const auto& name = key.template get<std::string>();
        if (tag && name == "class") continue;
        emit_entry(key, properties.at(std::string_view(name)));
      } else {
        emit_entry(key, obj.non_string_props->at(key));
      }
    }
  }
  s += "}";
  return s;
}

inline std::pair<long, long> normalize_slice(long start, long end, long size) {
  if (start < 0) start += size;
  if (end < 0) end += size;
  if (start < 0) start = 0;
  if (start > size) start = size;
  if (end < start) end = start;
  if (end > size) end = size;
  return {start, end};
}

// Build an iterator-protocol step Object. Two shapes: {done: true}
// (end marker) and {done: false, value: v} (yielded value).
inline Value _iter_step_done() {
  ObjectValue step;
  step.initialize("done", Value(true), false);
  return Value(std::move(step));
}

inline Value _iter_step_value(Value v) {
  ObjectValue step;
  step.initialize("done", Value(false), false);
  step.initialize("value", std::move(v), false);
  return Value(std::move(step));
}

// Build an Iterator ObjectValue: `iter` returns self (so the iterator
// is also an Iterable), `next` is the caller-supplied advance function
// (captures its own state).
template <typename NextFn>
inline Value _make_iterator(NextFn&& next_impl) {
  ObjectValue iter_obj;
  iter_obj.initialize(
      "iter",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
        return callEnv->get("this");
      })),
      false);
  iter_obj.initialize(
      "next",
      Value(FunctionValue({}, std::forward<NextFn>(next_impl))),
      false);
  return Value(std::move(iter_obj));
}

// Build an iterator that yields elements of a shared Value vector in
// index order. Used by Tuple's `for x in t` path and other places that
// snapshot keys/values into a vector before iterating.
inline Value _iter_over_vector(std::shared_ptr<std::vector<Value>> vec) {
  auto index = std::make_shared<size_t>(0);
  return _make_iterator([vec, index](std::shared_ptr<Environment>) {
    if (*index >= vec->size()) return _iter_step_done();
    auto v = (*vec)[*index];
    (*index)++;
    return _iter_step_value(std::move(v));
  });
}

// Decode one UTF-8 codepoint at `s + off` into `cp`, advancing `off` by
// the consumed byte count. Invalid / truncated sequences fall back to
// the raw byte value and advance by one — matching the permissive
// policy used by `String.iter` / `code_points` / `graphemes`.
inline void _decode_one_utf8(const std::string& s, size_t& off,
                             char32_t& cp) {
  size_t bytes;
  if (!unicode::utf8::decode_codepoint(s.data() + off, s.size() - off,
                                       bytes, cp)) {
    cp = static_cast<unsigned char>(s[off]);
    bytes = 1;
  }
  off += bytes;
}

// Build a call environment for invoking a well-known method (drop,
// iter, next) by reaching into an ObjectValue's raw FunctionValue.
// Populates the bindings that eval_function_call normally provides:
// `this`, `__LINE__`, `__COLUMN__`, plus the function-frame marker.
inline std::shared_ptr<Environment> _make_method_call_env(
    const Value& this_val, size_t line, size_t column) {
  auto env = std::make_shared<Environment>();
  env->is_function_frame = true;
  env->initialize("this", this_val, false);
  env->initialize("__LINE__", Value((long)line), false);
  env->initialize("__COLUMN__", Value((long)column), false);
  return env;
}

// Invoke a 0-parameter method stored on an iterator-shaped Object
// (receiver plays the iterator role). `receiver.next()` style call.
inline Value _invoke_method_no_args(const Value& receiver,
                                    std::string_view method_name) {
  const auto& fn = receiver.to_object().get(method_name);
  return fn.to_function().eval(_make_method_call_env(receiver, 0, 0));
}

// Display hook: if `v` is an Object carrying a `__str__` method, run
// it and return its String result. Returns nullopt otherwise so the
// caller falls back to the built-in formatter (`str_display`). The
// method is expected to return a String; anything else is a type
// error so a buggy `__str__` fails loudly.
inline std::optional<std::string> _try_str_special(const Value& v) {
  if (v.type != Value::Object) return std::nullopt;
  const auto& obj = v.to_object();
  if (!obj.has("__str__")) return std::nullopt;
  const auto& m = obj.get("__str__");
  if (m.type != Value::Function) return std::nullopt;
  auto r = _invoke_method_no_args(v, "__str__");
  if (r.type != Value::String) {
    throw CulebraError("TypeError", "__str__ must return a String");
  }
  return r.template get<std::string>();
}

// Like `v.str_display()` (unquoted strings) but honors `__str__` on
// Object — used by interpolation, `print`, and `to_string`.
inline std::string str_display_with_special(const Value& v) {
  if (auto r = _try_str_special(v)) return *r;
  return v.str_display();
}

// Like `v.str()` (quoted strings) but honors `__str__` on Object —
// used by `puts`. Objects with `__str__` return the custom form with
// no extra quoting regardless.
inline std::string str_quoted_with_special(const Value& v) {
  if (auto r = _try_str_special(v)) return *r;
  return v.str();
}

// Advance an iterator one step. Returns the yielded value, or
// `std::nullopt` when the iterator is done. Used by iterator
// methods to replace the repeated 4-line "call next(), check done,
// extract value" pattern with a single expression.
inline std::optional<Value> _iter_next_value(const Value& upstream) {
  auto step = _invoke_method_no_args(upstream, "next");
  const auto& s = step.to_object();
  if (s.get("done").to_bool()) return std::nullopt;
  return s.get("value");
}

// Invoke a user-supplied callable (mapper/predicate/reducer callback)
// on the given argument. Used by Iterator methods where the callback
// body runs repeatedly per step. No `this` is bound — callbacks are
// free-function calls — but `self` refers to the callback for
// recursion. Wrong arity falls through silently (0-param accepts),
// matching the existing Array higher-order conventions.
inline Value _invoke_callback(const Value& fn_val, const Value& a) {
  const auto& fn = fn_val.to_function();
  auto env = std::make_shared<Environment>();
  env->is_function_frame = true;
  env->initialize("self", fn_val, false);
  if (!fn.params->empty()) {
    const auto& p = (*fn.params)[0];
    env->initialize(p.name, a, p.mut);
  }
  env->initialize("__LINE__", Value(0L), false);
  env->initialize("__COLUMN__", Value(0L), false);
  try {
    return fn.eval(env);
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

inline Value _invoke_callback(const Value& fn_val, const Value& a,
                              const Value& b) {
  const auto& fn = fn_val.to_function();
  auto env = std::make_shared<Environment>();
  env->is_function_frame = true;
  env->initialize("self", fn_val, false);
  const auto& params = *fn.params;
  if (!params.empty()) {
    env->initialize(params[0].name, a, params[0].mut);
  }
  if (params.size() >= 2) {
    env->initialize(params[1].name, b, params[1].mut);
  }
  env->initialize("__LINE__", Value(0L), false);
  env->initialize("__COLUMN__", Value(0L), false);
  try {
    return fn.eval(env);
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

// Resolve the `iter` method on an iterable value (Object/Array/String)
// and call it to obtain an iterator Object. Throws `type error` with
// the given source location if the value is not iterable.
inline Value _get_iterator(const Value& iterable, size_t line, size_t col) {
  Value iter_fn;
  if (iterable.type == Value::String) {
    const auto& methods = string_builtins();
    auto it = methods.find("iter");
    if (it != methods.end()) iter_fn = it->second;
  } else if (iterable.type == Value::Object ||
             iterable.type == Value::Array) {
    if (iterable.to_object().has("iter")) {
      iter_fn = iterable.to_object().get("iter");
    }
  } else if (iterable.type == Value::Tuple) {
    return _iter_over_vector(iterable.get<TupleValue>().elements);
  } else if (iterable.type == Value::Set) {
    return _iter_over_vector(iterable.get<SetValue>().members);
  }
  if (iter_fn.type != Value::Function) {
    throw CulebraError("TypeError", std::format(
        "type error: target is not iterable at {}:{}.", line, col),
        static_cast<long>(line), static_cast<long>(col));
  }
  return iter_fn.to_function().eval(
      _make_method_call_env(iterable, line, col));
}

inline std::map<std::string_view, Value>& ObjectValue::builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"size"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& obj = callEnv->get("this").to_object();
         long n = static_cast<long>(obj.properties->size());
         if (obj.non_string_props) {
           n += static_cast<long>(obj.non_string_props->size());
         }
         return Value(n);
       }))},
      {"keys"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& obj = callEnv->get("this").to_object();
         ArrayValue arr;
         // Walk key_order when populated so String and non-String keys
         // come out interleaved in their actual insertion order. Fall
         // back to the property-map walk for objects built via direct
         // emplace (class instances).
         if (obj.key_order && !obj.key_order->empty()) {
           arr.values->reserve(obj.key_order->size());
           for (const auto& k : *obj.key_order) arr.values->push_back(k);
         } else {
           arr.values->reserve(obj.properties->size());
           for (const auto& [k, _] : *obj.properties) {
             arr.values->push_back(Value(std::string(k)));
           }
         }
         return Value(std::move(arr));
       }))},
      {"has"sv, Value(FunctionValue({{"key", false}},
                                    [](std::shared_ptr<Environment> callEnv) {
                                      const auto& obj = callEnv->get("this").to_object();
                                      const auto& key = callEnv->get("key");
                                      return Value(obj.has(key));
                                    }))},
      {"remove"sv,
       Value(FunctionValue({{"key", false}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& val = callEnv->get("this");
                             auto& obj = val.to_object();
                             const auto& key = callEnv->get("key");
                             // String keys live in `properties`;
                             // Long/Float/Bool/Nil/Tuple keys live in
                             // `non_string_props`. The String-key
                             // sidecar fallback below is unreachable
                             // after K but kept as belt-and-braces.
                             if (key.type == Value::String) {
                               const auto& k = key.template get<std::string>();
                               if (obj.properties->contains(k)) {
                                 obj.properties->erase(k);
                                 // Drop from key_order if tracked.
                                 if (obj.key_order) {
                                   auto& ko = *obj.key_order;
                                   ko.erase(std::remove_if(
                                       ko.begin(), ko.end(),
                                       [&](const Value& v) {
                                         return v.type == Value::String &&
                                                v.template get<std::string>() == k;
                                       }), ko.end());
                                 }
                                 return Value();
                               }
                             }
                             if (obj.non_string_props->erase(key) > 0) {
                               // Same: drop matching entry from key_order.
                               auto& ko = *obj.key_order;
                               ko.erase(std::remove_if(
                                   ko.begin(), ko.end(),
                                   [&](const Value& v) { return v == key; }),
                                   ko.end());
                             }
                             return Value();
                           }))},
      // Iterator protocol: yield keys in std::map order (ascending).
      // Snapshot keys up front — avoids invalidation if the map is
      // mutated mid-iteration, and keeps `next` O(1) per step.
      {"iter"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& val = callEnv->get("this");
         auto props = val.to_object().properties;  // shared_ptr copy
         auto keys = std::make_shared<std::vector<std::string>>();
         keys->reserve(props->size());
         for (const auto& [k, _] : *props) keys->emplace_back(k);
         auto index = std::make_shared<size_t>(0);
         return _make_iterator([keys, index](std::shared_ptr<Environment>) {
           if (*index >= keys->size()) return _iter_step_done();
           auto v = Value(std::string((*keys)[*index]));
           (*index)++;
           return _iter_step_value(std::move(v));
         });
       }))}};
  return props_;
}

// Higher-order Array helpers: invoke a 1-parameter callback `f` on `v`.
// Sets up a function-frame environment with `self` bound to the callback,
// and treats an early return (`return x` compiled as a thrown Value) as
// the callback's result.
inline Value invoke_unary_callback(std::shared_ptr<Environment> callEnv,
                                   const FunctionValue& f, const Value& v) {
  auto inner = std::make_shared<Environment>(callEnv);
  inner->is_function_frame = true;
  inner->initialize("self", callEnv->get("f"), false);
  if (!f.params->empty()) {
    const auto& p = (*f.params)[0];
    inner->initialize(p.name, v, p.mut);
  }
  try {
    return f.eval(inner);
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

// Default-valued params aren't resolved — pass all positional args.
// Extras bind to `__ARGS__` (matches the in-language calling convention).
inline Value call(std::shared_ptr<Environment> env, std::string_view name,
                  std::vector<Value> args) {
  if (!env->has(name)) {
    throw CulebraError("NameError",
        std::format("'{}' is not defined", name));
  }
  auto fn_val = env->get(name);
  if (fn_val.type != Value::Function) {
    throw CulebraError("TypeError",
        std::format("'{}' is not a function", name));
  }
  const auto& f = fn_val.get<FunctionValue>();
  const auto& params = *f.params;
  // Positional-only embedder API: kw-only and `**rest` params are
  // not bindable from C++ here, so they're seeded with empty defaults
  // (rest → empty Object, kw-only → default if any). Only the
  // regular-param count gates the arity check.
  size_t regulars = regular_param_count(params);
  if (args.size() < regulars) {
    throw CulebraError("ArityError",
        std::format("'{}' expects {} args, got {}",
                    name, regulars, args.size()));
  }

  auto callEnv = std::make_shared<Environment>(env);
  callEnv->is_function_frame = true;
  callEnv->initialize("self", fn_val, false);

  size_t pos = 0;
  for (size_t i = 0; i < params.size(); ++i) {
    if (params[i].kwargs_rest) {
      callEnv->initialize(params[i].name, Value(ObjectValue{}), false);
      continue;
    }
    if (params[i].kw_only) {
      if (params[i].default_value) {
        callEnv->initialize(params[i].name, *params[i].default_value,
                            params[i].mut);
      }
      continue;
    }
    callEnv->initialize(params[i].name, std::move(args[pos]), params[i].mut);
    pos++;
  }
  ArrayValue extras;
  for (size_t i = regulars; i < args.size(); ++i) {
    extras.values->push_back(std::move(args[i]));
  }
  callEnv->initialize("__ARGS__", Value(std::move(extras)), false);
  callEnv->initialize("__LINE__", Value(0L), false);
  callEnv->initialize("__COLUMN__", Value(0L), false);

  try {
    return f.eval(callEnv);
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

namespace _detail {

// Extract arg types and return type from a callable. Specialized for
// function pointers, std::function, and lambdas (via operator()).
template <class T>
struct fn_traits : fn_traits<decltype(&std::decay_t<T>::operator())> {};

template <class R, class... Args>
struct fn_traits<R(*)(Args...)> {
  using ret = R;
  using args = std::tuple<Args...>;
  static constexpr size_t arity = sizeof...(Args);
};

template <class C, class R, class... Args>
struct fn_traits<R(C::*)(Args...) const> {
  using ret = R;
  using args = std::tuple<Args...>;
  static constexpr size_t arity = sizeof...(Args);
};

template <class C, class R, class... Args>
struct fn_traits<R(C::*)(Args...)> {
  using ret = R;
  using args = std::tuple<Args...>;
  static constexpr size_t arity = sizeof...(Args);
};

template <class R, class... Args>
struct fn_traits<std::function<R(Args...)>> {
  using ret = R;
  using args = std::tuple<Args...>;
  static constexpr size_t arity = sizeof...(Args);
};

// Value → C++. Struct specialization so reference return types
// (`const std::string&`, `const Value&`) compose cleanly.
template <class T> struct ValueAs;

template <> struct ValueAs<long> {
  static long convert(const Value& v) { return v.to_long(); }
};
template <> struct ValueAs<int> {
  static int convert(const Value& v) { return static_cast<int>(v.to_long()); }
};
template <> struct ValueAs<double> {
  static double convert(const Value& v) { return v.to_double_coerce(); }
};
template <> struct ValueAs<float> {
  static float convert(const Value& v) { return static_cast<float>(v.to_double_coerce()); }
};
template <> struct ValueAs<bool> {
  static bool convert(const Value& v) { return v.to_bool(); }
};
template <> struct ValueAs<std::string> {
  static std::string convert(const Value& v) { return v.to_string(); }
};
template <> struct ValueAs<std::string_view> {
  static std::string_view convert(const Value& v) {
    return v.template get<std::string>();
  }
};
template <> struct ValueAs<const std::string&> {
  static const std::string& convert(const Value& v) {
    return v.template get<std::string>();
  }
};
template <> struct ValueAs<Value> {
  static Value convert(const Value& v) { return v; }
};
template <> struct ValueAs<const Value&> {
  static const Value& convert(const Value& v) { return v; }
};

// C++ → Value. `cpp_to_value(void)` is handled at the call site.
inline Value cpp_to_value(long v)        { return Value(v); }
inline Value cpp_to_value(int v)         { return Value(static_cast<long>(v)); }
inline Value cpp_to_value(double v)      { return Value(v); }
inline Value cpp_to_value(float v)       { return Value(static_cast<double>(v)); }
inline Value cpp_to_value(bool v)        { return Value(v); }
inline Value cpp_to_value(std::string v) { return Value(std::move(v)); }
inline Value cpp_to_value(std::string_view v) { return Value(std::string(v)); }
inline Value cpp_to_value(const char* v) { return Value(std::string(v)); }
inline Value cpp_to_value(Value v)       { return v; }

// Type annotation matching Culebra's "Bool"/"Long"/"Float"/"String"
// names. Empty for types that don't map cleanly — Param.type_name=""
// then means "no annotation" (any).
template <class T> constexpr std::string_view type_annotation_for() { return {}; }
template <> constexpr std::string_view type_annotation_for<long>()              { return "Long"; }
template <> constexpr std::string_view type_annotation_for<int>()               { return "Long"; }
template <> constexpr std::string_view type_annotation_for<double>()            { return "Float"; }
template <> constexpr std::string_view type_annotation_for<float>()             { return "Float"; }
template <> constexpr std::string_view type_annotation_for<bool>()              { return "Bool"; }
template <> constexpr std::string_view type_annotation_for<std::string>()       { return "String"; }
template <> constexpr std::string_view type_annotation_for<std::string_view>()  { return "String"; }
template <> constexpr std::string_view type_annotation_for<const std::string&>(){ return "String"; }

}  // namespace _detail

// Embedding helper: register a C++ callable as a host function under
// `name`. Argument and return types are deduced from `fn`'s signature
// and converted via the value_to_cpp / cpp_to_value tables. Use
// `param_names` to give the script meaningful names (otherwise they
// become `_arg0`, `_arg1`, ...).
//
//   culebra::define(env, "log", [](const std::string& msg) {
//     std::cout << msg << "\n";
//   }, {"msg"});
template <class Fn>
inline void define(std::shared_ptr<Environment> env, std::string_view name,
                   Fn&& fn, std::vector<std::string> param_names = {}) {
  using traits = _detail::fn_traits<std::decay_t<Fn>>;
  using ArgTuple = typename traits::args;
  using R = typename traits::ret;
  constexpr size_t arity = traits::arity;

  param_names.resize(arity);
  for (size_t i = 0; i < arity; ++i) {
    if (param_names[i].empty()) param_names[i] = "_arg" + std::to_string(i);
  }
  // Stable storage for the names. The closure captures `storage` so
  // the string_views in `params` stay valid for the FunctionValue's
  // lifetime.
  auto storage = std::make_shared<std::vector<std::string>>(std::move(param_names));

  auto type_at = []<size_t I>(std::integral_constant<size_t, I>) {
    return _detail::type_annotation_for<std::tuple_element_t<I, ArgTuple>>();
  };
  std::vector<FunctionValue::Parameter> params;
  params.reserve(arity);
  [&]<size_t... I>(std::index_sequence<I...>) {
    (params.push_back({std::string_view((*storage)[I]), false,
                       type_at(std::integral_constant<size_t, I>{})}), ...);
  }(std::make_index_sequence<arity>{});

  auto eval = [storage, fn = std::forward<Fn>(fn)](
                  std::shared_ptr<Environment> callEnv) -> Value {
    auto invoke = [&]<size_t... I>(std::index_sequence<I...>) -> Value {
      if constexpr (std::is_void_v<R>) {
        fn(_detail::ValueAs<std::tuple_element_t<I, ArgTuple>>::convert(
            callEnv->get(std::string_view((*storage)[I])))...);
        return Value();
      } else {
        return _detail::cpp_to_value(
            fn(_detail::ValueAs<std::tuple_element_t<I, ArgTuple>>::convert(
                callEnv->get(std::string_view((*storage)[I])))...));
      }
    };
    return invoke(std::make_index_sequence<arity>{});
  };
  env->initialize(name,
                  Value(FunctionValue(std::move(params), std::move(eval),
                                      _detail::type_annotation_for<R>())),
                  /*mut=*/false);
}

inline std::map<std::string_view, Value>& ArrayValue::builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"size"sv, Value(FunctionValue({},
                                     [](std::shared_ptr<Environment> callEnv) {
                                       const auto& val = callEnv->get("this");
                                       long n = val.to_array().values->size();
                                       return Value(n);
                                     }))},
      {"push"sv, Value(FunctionValue{{{"arg", false}},
                                     [](std::shared_ptr<Environment> callEnv) {
                                       const auto& val = callEnv->get("this");
                                       const auto& arg = callEnv->get("arg");
                                       val.to_array().values->push_back(arg);
                                       return Value();
                                     }})},
      {"pop"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& val = callEnv->get("this");
         auto& vs = *val.to_array().values;
         if (vs.empty()) return Value();
         auto last = vs.back();
         vs.pop_back();
         return last;
       }))},
      {"slice"sv,
       Value(FunctionValue(
           {{"start", false, "Long"sv}, {"end", false, "Long"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& val = callEnv->get("this");
             auto& vs = *val.to_array().values;
             auto [s, e] = normalize_slice(callEnv->get("start").to_long(),
                                           callEnv->get("end").to_long(),
                                           static_cast<long>(vs.size()));
             ArrayValue out;
             out.values->reserve(e - s);
             for (long i = s; i < e; i++) out.values->push_back(vs[i]);
             return Value(std::move(out));
           }))},
      {"join"sv,
       Value(FunctionValue({{"sep", false, "String"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& val = callEnv->get("this");
                             const auto& sep =
                                 callEnv->get("sep").to_string();
                             std::string out;
                             bool first = true;
                             for (const auto& v : *val.to_array().values) {
                               if (!first) out += sep;
                               first = false;
                               out += v.str_display();
                             }
                             return Value(std::move(out));
                           }))},
      {"index_of"sv,
       Value(FunctionValue({{"v", false}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& val = callEnv->get("this");
                             const auto& needle = callEnv->get("v");
                             const auto& vs = *val.to_array().values;
                             for (size_t i = 0; i < vs.size(); i++) {
                               if (vs[i] == needle)
                                 return Value(static_cast<long>(i));
                             }
                             return Value(static_cast<long>(-1));
                           }))},
      {"contains"sv,
       Value(FunctionValue({{"v", false}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& val = callEnv->get("this");
                             const auto& needle = callEnv->get("v");
                             for (const auto& v : *val.to_array().values) {
                               if (v == needle) return Value(true);
                             }
                             return Value(false);
                           }))},
      {"reverse"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& val = callEnv->get("this");
         auto& vs = *val.to_array().values;
         std::reverse(vs.begin(), vs.end());
         return Value();
       }))},
      {"map"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             ArrayValue out;
                             out.values->reserve(arr.values->size());
                             for (const auto& v : *arr.values) {
                               out.values->push_back(
                                   invoke_unary_callback(callEnv, f, v));
                             }
                             return Value(std::move(out));
                           }))},
      {"filter"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             ArrayValue out;
                             for (const auto& v : *arr.values) {
                               if (invoke_unary_callback(callEnv, f, v)
                                       .to_bool()) {
                                 out.values->push_back(v);
                               }
                             }
                             return Value(std::move(out));
                           }))},
      {"for_each"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             for (const auto& v : *arr.values) {
                               invoke_unary_callback(callEnv, f, v);
                             }
                             return Value();
                           }))},
      {"reduce"sv,
       Value(FunctionValue(
           {{"init", false}, {"f", false, "Function"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& arr = callEnv->get("this").to_array();
             const auto& f = callEnv->get("f").to_function();
             Value acc = callEnv->get("init");
             for (const auto& v : *arr.values) {
               auto inner = std::make_shared<Environment>(callEnv);
               inner->is_function_frame = true;
               inner->initialize("self", callEnv->get("f"), false);
               if (f.params->size() >= 1) {
                 const auto& p0 = (*f.params)[0];
                 inner->initialize(p0.name, acc, p0.mut);
               }
               if (f.params->size() >= 2) {
                 const auto& p1 = (*f.params)[1];
                 inner->initialize(p1.name, v, p1.mut);
               }
               try {
                 acc = f.eval(inner);
               } catch (const ReturnValue& r) {
                 acc = r.value;
               }
             }
             return acc;
           }))},
      {"find"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             for (const auto& v : *arr.values) {
                               if (invoke_unary_callback(callEnv, f, v)
                                       .to_bool()) return v;
                             }
                             return Value();
                           }))},
      {"any"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             for (const auto& v : *arr.values) {
                               if (invoke_unary_callback(callEnv, f, v)
                                       .to_bool()) return Value(true);
                             }
                             return Value(false);
                           }))},
      {"all"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             for (const auto& v : *arr.values) {
                               if (!invoke_unary_callback(callEnv, f, v)
                                        .to_bool()) return Value(false);
                             }
                             return Value(true);
                           }))},
      {"flat_map"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             ArrayValue out;
                             for (const auto& v : *arr.values) {
                               auto r = invoke_unary_callback(callEnv, f, v);
                               if (r.type != Value::Array) {
                                 throw CulebraError("TypeError",
                                     "type error: flat_map callback must "
                                     "return an Array.");
                               }
                               for (const auto& e : *r.to_array().values) {
                                 out.values->push_back(e);
                               }
                             }
                             return Value(std::move(out));
                           }))},
      {"sum"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& arr = callEnv->get("this").to_array();
         long acc = 0;
         for (const auto& v : *arr.values) acc += v.to_long();
         return Value(acc);
       }))},
      {"product"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& arr = callEnv->get("this").to_array();
         long acc = 1;
         for (const auto& v : *arr.values) acc *= v.to_long();
         return Value(acc);
       }))},
      {"min"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& arr = callEnv->get("this").to_array();
         if (arr.values->empty()) {
           throw CulebraError("ValueError",
                              "min of empty Array.");
         }
         long best = (*arr.values)[0].to_long();
         for (size_t i = 1; i < arr.values->size(); i++) {
           long x = (*arr.values)[i].to_long();
           if (x < best) best = x;
         }
         return Value(best);
       }))},
      {"max"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& arr = callEnv->get("this").to_array();
         if (arr.values->empty()) {
           throw CulebraError("ValueError",
                              "max of empty Array.");
         }
         long best = (*arr.values)[0].to_long();
         for (size_t i = 1; i < arr.values->size(); i++) {
           long x = (*arr.values)[i].to_long();
           if (x > best) best = x;
         }
         return Value(best);
       }))},
      {"sort_by"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             auto& arr = callEnv->get("this").to_array();
                             const auto& f = callEnv->get("f").to_function();
                             auto& vs = *arr.values;
                             std::vector<std::pair<Value, size_t>> keyed;
                             keyed.reserve(vs.size());
                             for (size_t i = 0; i < vs.size(); i++) {
                               keyed.emplace_back(
                                   invoke_unary_callback(callEnv, f, vs[i]), i);
                             }
                             std::stable_sort(
                                 keyed.begin(), keyed.end(),
                                 [](const auto& a, const auto& b) {
                                   return a.first < b.first;
                                 });
                             std::vector<Value> sorted;
                             sorted.reserve(vs.size());
                             for (auto& [k, i] : keyed) sorted.push_back(vs[i]);
                             vs = std::move(sorted);
                             return Value();
                           }))},
      // Iterator protocol: yield elements in index order.
      {"iter"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return _iter_over_vector(callEnv->get("this").to_array().values);
       }))}};
  return props_;
}

// Shared body for .sum / .mean / .max — variadic Long arg selects
// axis (lazy Tensor result) vs no arg (eager Float result).
template <Op op>
inline Value _make_tensor_reduction_method() {
  return Value(FunctionValue(
      {},
      [](std::shared_ptr<Environment> callEnv) {
        const auto& self = callEnv->get("this").to_tensor().impl;
        if (callEnv->has("__ARGS__")) {
          const auto& args = *callEnv->get("__ARGS__").to_array().values;
          if (!args.empty()) {
            if (args[0].type != Value::Long) {
              throw_type_error_at(callEnv->get("__LINE__").to_long(),
                                  callEnv->get("__COLUMN__").to_long());
            }
            return Value(TensorValue(
                tensor_reduce_axis(op, self, args[0].to_long())));
          }
        }
        return Value(tensor_reduce_all<op>(self));
      }));
}

// Builtin methods on Tensor values. M1 added .shape(); M2 adds .pow().
// Linear-algebra / reduction methods land in M3–M4.
inline std::map<std::string_view, Value>& TensorValue::builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"shape"sv, Value(FunctionValue(
                       {},
                       [](std::shared_ptr<Environment> callEnv) {
                         const auto& impl =
                             *callEnv->get("this").to_tensor().impl;
                         ArrayValue out;
                         out.values->reserve(impl.shape.dims.size());
                         for (auto d : impl.shape.dims) {
                           out.values->push_back(Value(static_cast<long>(d)));
                         }
                         return Value(std::move(out));
                       },
                       "Array"sv))},
      {"pow"sv, Value(FunctionValue(
                     {{"exp", false}},
                     [](std::shared_ptr<Environment> callEnv) {
                       const auto& self =
                           callEnv->get("this").to_tensor().impl;
                       const auto& exp = callEnv->get("exp");
                       TensorPtr b;
                       if (exp.type == Value::Tensor) {
                         b = exp.to_tensor().impl;
                       } else if (exp.is_numeric()) {
                         b = tensor_scalar(exp.to_double_coerce(),
                                           self->dtype);
                       } else {
                         throw_type_error_at(
                             callEnv->get("__LINE__").to_long(),
                             callEnv->get("__COLUMN__").to_long());
                       }
                       return Value(TensorValue(
                           tensor_binop(Op::Pow, self, std::move(b))));
                     },
                     "Tensor"sv))},
      {"transpose"sv, Value(FunctionValue(
                          {},
                          [](std::shared_ptr<Environment> callEnv) {
                            const auto& self =
                                callEnv->get("this").to_tensor().impl;
                            return Value(TensorValue(tensor_transpose(self)));
                          },
                          "Tensor"sv))},
      {"clone"sv, Value(FunctionValue(
                      {},
                      [](std::shared_ptr<Environment> callEnv) {
                        const auto& self =
                            callEnv->get("this").to_tensor().impl;
                        return Value(TensorValue(tensor_clone(self)));
                      },
                      "Tensor"sv))},
      {"slice"sv, Value(FunctionValue(
                       {{"start", false, "Long"sv},
                        {"end", false, "Long"sv}},
                       [](std::shared_ptr<Environment> callEnv) {
                         const auto& self =
                             callEnv->get("this").to_tensor().impl;
                         auto start = callEnv->get("start").to_long();
                         auto end = callEnv->get("end").to_long();
                         return Value(TensorValue(
                             tensor_slice(self, start, end)));
                       },
                       "Tensor"sv))},
      {"reshape"sv, Value(FunctionValue(
                         {{"dims", false, "Array"sv}},
                         [](std::shared_ptr<Environment> callEnv) {
                           const auto& self =
                               callEnv->get("this").to_tensor().impl;
                           const auto& dims_v =
                               *callEnv->get("dims").to_array().values;
                           std::vector<int64_t> new_dims;
                           new_dims.reserve(dims_v.size());
                           for (const auto& d : dims_v) {
                             if (d.type != Value::Long) {
                               throw_type_error_at(
                                   callEnv->get("__LINE__").to_long(),
                                   callEnv->get("__COLUMN__").to_long());
                             }
                             new_dims.push_back(d.to_long());
                           }
                           return Value(TensorValue(tensor_reshape(
                               self, TensorShape(std::move(new_dims)))));
                         },
                         "Tensor"sv))},
      // Reductions: zero-arg form returns a Float scalar (eager); the
      // axis form (1 Long arg) returns a lazy Tensor with that axis
      // dropped. argmax has no axis-less form.
      {"sum"sv, _make_tensor_reduction_method<Op::Sum>()},
      {"mean"sv, _make_tensor_reduction_method<Op::Mean>()},
      {"max"sv, _make_tensor_reduction_method<Op::Max>()},
      {"argmax"sv, Value(FunctionValue(
                        {{"axis", false, "Long"sv}},
                        [](std::shared_ptr<Environment> callEnv) {
                          const auto& self =
                              callEnv->get("this").to_tensor().impl;
                          auto axis = callEnv->get("axis").to_long();
                          return Value(TensorValue(tensor_reduce_axis(
                              Op::Argmax, self, axis)));
                        },
                        "Tensor"sv))},
      {"dot"sv, Value(FunctionValue(
                     {{"other", false, "Tensor"sv}},
                     [](std::shared_ptr<Environment> callEnv) {
                       const auto& self =
                           callEnv->get("this").to_tensor().impl;
                       const auto& other =
                           callEnv->get("other").to_tensor().impl;
                       return Value(TensorValue(tensor_dot(self, other)));
                     },
                     "Tensor"sv))},
      {"linear_sigmoid"sv, Value(FunctionValue(
           {{"x", false, "Tensor"sv}, {"b", false, "Tensor"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& W = callEnv->get("this").to_tensor().impl;
             const auto& x = callEnv->get("x").to_tensor().impl;
             const auto& b = callEnv->get("b").to_tensor().impl;
             return Value(TensorValue(tensor_linear_sigmoid(W, x, b)));
           },
           "Tensor"sv))},
      // .to_array(): exit point from the Tensor world. Forces eval
      // and returns a Culebra Array of Float (1D) or Array of Array
      // (2D). Higher ranks are out of scope for Phase 1.
      {"to_array"sv, Value(FunctionValue(
                          {},
                          [](std::shared_ptr<Environment> callEnv) {
                            const auto& self =
                                callEnv->get("this").to_tensor().impl;
                            tensor_eval_node(*self);
                            auto read_at = [&](int64_t flat_idx) {
                              return self->dtype == Dtype::F32
                                  ? static_cast<double>(
                                        self->data_as<float>()[flat_idx])
                                  : self->data_as<double>()[flat_idx];
                            };
                            const auto& dims = self->shape.dims;
                            if (dims.size() == 1) {
                              ArrayValue out;
                              out.values->reserve(dims[0]);
                              for (int64_t i = 0; i < dims[0]; i++) {
                                out.values->push_back(
                                    Value(read_at(i * self->strides[0])));
                              }
                              return Value(std::move(out));
                            }
                            if (dims.size() == 2) {
                              ArrayValue out;
                              out.values->reserve(dims[0]);
                              for (int64_t i = 0; i < dims[0]; i++) {
                                ArrayValue row;
                                row.values->reserve(dims[1]);
                                for (int64_t j = 0; j < dims[1]; j++) {
                                  row.values->push_back(Value(read_at(
                                      i * self->strides[0] +
                                      j * self->strides[1])));
                                }
                                out.values->push_back(Value(std::move(row)));
                              }
                              return Value(std::move(out));
                            }
                            throw CulebraError("ValueError",
                                "to_array: rank > 2 not supported.");
                          },
                          "Array"sv))},
  };
  return props_;
}

// Method lookup table for primitive String values. Not part of any Object.
inline std::map<std::string_view, Value>& string_builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"size"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return Value(static_cast<long>(
             callEnv->get("this").to_string().size()));
       }))},
      {"upper"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto s = callEnv->get("this").to_string();
         for (auto& c : s) {
           c = static_cast<char>(
               std::toupper(static_cast<unsigned char>(c)));
         }
         return Value(std::move(s));
       }))},
      {"lower"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto s = callEnv->get("this").to_string();
         for (auto& c : s) {
           c = static_cast<char>(
               std::tolower(static_cast<unsigned char>(c)));
         }
         return Value(std::move(s));
       }))},
      {"trim"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& s = callEnv->get("this").to_string();
         return Value(std::string(trim_ascii(s)));
       }))},
      {"split"sv,
       Value(FunctionValue(
           {{"sep", false, "String"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& s = callEnv->get("this").to_string();
             const auto& sep = callEnv->get("sep").to_string();
             ArrayValue out;
             if (sep.empty()) {
               out.values->push_back(Value(std::string(s)));
             } else {
               size_t pos = 0;
               while (true) {
                 auto p = s.find(sep, pos);
                 if (p == std::string::npos) {
                   out.values->push_back(Value(s.substr(pos)));
                   break;
                 }
                 out.values->push_back(Value(s.substr(pos, p - pos)));
                 pos = p + sep.size();
               }
             }
             return Value(std::move(out));
           }))},
      {"contains"sv,
       Value(FunctionValue({{"sub", false, "String"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& s = callEnv->get("this").to_string();
                             const auto& sub =
                                 callEnv->get("sub").to_string();
                             return Value(s.find(sub) != std::string::npos);
                           }))},
      {"starts_with"sv,
       Value(FunctionValue(
           {{"prefix", false, "String"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& s = callEnv->get("this").to_string();
             const auto& prefix = callEnv->get("prefix").to_string();
             return Value(s.size() >= prefix.size() &&
                          s.compare(0, prefix.size(), prefix) == 0);
           }))},
      {"ends_with"sv,
       Value(FunctionValue(
           {{"suffix", false, "String"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& s = callEnv->get("this").to_string();
             const auto& suf = callEnv->get("suffix").to_string();
             return Value(s.size() >= suf.size() &&
                          s.compare(s.size() - suf.size(), suf.size(), suf) ==
                              0);
           }))},
      {"slice"sv,
       Value(FunctionValue(
           {{"start", false, "Long"sv}, {"end", false, "Long"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& s = callEnv->get("this").to_string();
             auto [ss, ee] = normalize_slice(callEnv->get("start").to_long(),
                                             callEnv->get("end").to_long(),
                                             static_cast<long>(s.size()));
             return Value(s.substr(ss, ee - ss));
           }))},
      // Iterator protocol: lazy UTF-8 walk yielding one-scalar Strings.
      // Culebra does not currently validate UTF-8 at String construction
      // (e.g. IO.read returns raw bytes as String), so an invalid or
      // truncated lead byte must not stall the iterator; in that case
      // we emit the offending byte as a one-byte substring and advance.
      {"iter"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         // Share the UTF-8 bytes through a shared_ptr<const string> so
         // each call to next() only indexes into the shared buffer.
         auto buf = std::make_shared<const std::string>(
             callEnv->get("this").to_string());
         auto offset = std::make_shared<size_t>(0);
         return _make_iterator([buf, offset](std::shared_ptr<Environment>) {
           if (*offset >= buf->size()) return _iter_step_done();
           size_t len = peg::codepoint_length(buf->data() + *offset,
                                              buf->size() - *offset);
           if (len == 0) len = 1;  // invalid/truncated: emit raw byte
           auto scalar = buf->substr(*offset, len);
           *offset += len;
           return _iter_step_value(Value(std::move(scalar)));
         });
       }))},
      // Lazy UTF-8 walk yielding Unicode scalar values as Long. For
      // numeric / range / classification work where the per-scalar
      // String allocation of `iter`/`for-in` is wasteful. Invalid or
      // truncated bytes yield the raw byte value (0–255) and advance
      // by one, mirroring `iter`'s permissive policy.
      {"code_points"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto buf = std::make_shared<const std::string>(
             callEnv->get("this").to_string());
         auto offset = std::make_shared<size_t>(0);
         return _make_iterator([buf, offset](std::shared_ptr<Environment>) {
           if (*offset >= buf->size()) return _iter_step_done();
           char32_t cp;
           _decode_one_utf8(*buf, *offset, cp);
           return _iter_step_value(Value(static_cast<long>(cp)));
         });
       }))},
      // Lazy walk yielding Extended Grapheme Cluster boundaries (UAX
      // #29) as Strings — one user-perceived character per step (e.g.
      // `'👨‍👩‍👧'.graphemes()` yields one element).
      //
      // Streaming decode: grapheme_length requires UTF-32 context, but
      // we only decode enough UTF-8 into `u32` to make the next cluster
      // boundary observable. `take(n)` on a multi-MB string therefore
      // touches only the prefix it actually consumes — mirroring how
      // `code_points` walks UTF-8 incrementally. Per-iterator state is
      // independent, so two iterators over the same String don't
      // interfere.
      {"graphemes"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto buf = std::make_shared<const std::string>(
             callEnv->get("this").to_string());
         auto u32 = std::make_shared<std::u32string>();
         auto byte_off = std::make_shared<size_t>(0);
         auto cp_off = std::make_shared<size_t>(0);
         return _make_iterator(
             [buf, u32, byte_off, cp_off](std::shared_ptr<Environment>) {
           // Chunk size when growing u32 to confirm a cluster boundary.
           // 1 is legal but pays the grapheme_length cost per codepoint;
           // 16 amortizes it across typical cluster lengths.
           constexpr size_t kExtendChunk = 16;
           // Discarding the already-walked prefix keeps memory bounded
           // for long-lived iterators on large strings — without it a
           // full walk of a 10 MB ASCII input would hold a 40 MB u32.
           // UAX #29 only needs bounded lookback (regional-indicator
           // pairs, at most 2 codepoints), so trimming at step boundary
           // is safe.
           constexpr size_t kTrimThreshold = 4096;

           auto extend_to = [&](size_t target) {
             while (u32->size() < target && *byte_off < buf->size()) {
               char32_t cp;
               _decode_one_utf8(*buf, *byte_off, cp);
               u32->push_back(cp);
             }
           };
           extend_to(*cp_off + 1);
           if (*cp_off >= u32->size()) return _iter_step_done();
           // Grow the buffer until `grapheme_length` returns strictly
           // less than the available size (a boundary is confirmed to
           // lie inside the buffer) or until the UTF-8 input is
           // exhausted — without lookahead, a `len == avail` return
           // could still be truncated by a continuation codepoint.
           for (;;) {
             size_t avail = u32->size() - *cp_off;
             size_t len = unicode::grapheme_length(
                 u32->data() + *cp_off, avail);
             if (len == 0) len = 1;  // grapheme_length contract says >=1 on avail>=1; belt-and-braces
             if (len < avail || *byte_off >= buf->size()) {
               std::string out;
               unicode::utf8::encode(u32->data() + *cp_off, len, out);
               *cp_off += len;
               if (*cp_off >= kTrimThreshold) {
                 u32->erase(0, *cp_off);
                 *cp_off = 0;
               }
               return _iter_step_value(Value(std::move(out)));
             }
             extend_to(u32->size() + kExtendChunk);
           }
         });
       }))}};
  return props_;
}

// Method lookup table for primitive Set values. Mirrors string_builtins;
// SetValue is not an ObjectValue so dispatch goes through a dedicated
// path in eval_property.
inline std::map<std::string_view, Value>& set_builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"size"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& self = callEnv->get("this").get<SetValue>();
         return Value(static_cast<long>(self.members->size()));
       }))},
      {"contains"sv,
       Value(FunctionValue(
           {{"x", false}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& self = callEnv->get("this").get<SetValue>();
             const auto& x = callEnv->get("x");
             return Value(self.index->contains(x));
           },
           "Bool"sv))},
      {"union"sv,
       Value(FunctionValue(
           {{"other", false, "Set"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& a = callEnv->get("this").get<SetValue>();
             const auto& b = callEnv->get("other").get<SetValue>();
             SetValue out;
             for (const auto& v : *a.members) out.add(v);
             for (const auto& v : *b.members) out.add(v);
             return Value(std::move(out));
           },
           "Set"sv))},
      {"intersect"sv,
       Value(FunctionValue(
           {{"other", false, "Set"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& a = callEnv->get("this").get<SetValue>();
             const auto& b = callEnv->get("other").get<SetValue>();
             SetValue out;
             for (const auto& v : *a.members) {
               if (b.index->contains(v)) out.add(v);
             }
             return Value(std::move(out));
           },
           "Set"sv))},
      {"diff"sv,
       Value(FunctionValue(
           {{"other", false, "Set"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& a = callEnv->get("this").get<SetValue>();
             const auto& b = callEnv->get("other").get<SetValue>();
             SetValue out;
             for (const auto& v : *a.members) {
               if (!b.index->contains(v)) out.add(v);
             }
             return Value(std::move(out));
           },
           "Set"sv))},
      {"sym_diff"sv,
       Value(FunctionValue(
           {{"other", false, "Set"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& a = callEnv->get("this").get<SetValue>();
             const auto& b = callEnv->get("other").get<SetValue>();
             SetValue out;
             for (const auto& v : *a.members) {
               if (!b.index->contains(v)) out.add(v);
             }
             for (const auto& v : *b.members) {
               if (!a.index->contains(v)) out.add(v);
             }
             return Value(std::move(out));
           },
           "Set"sv))},
      {"subset"sv,
       Value(FunctionValue(
           {{"other", false, "Set"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& a = callEnv->get("this").get<SetValue>();
             const auto& b = callEnv->get("other").get<SetValue>();
             for (const auto& v : *a.members) {
               if (!b.index->contains(v)) return Value(false);
             }
             return Value(true);
           },
           "Bool"sv))},
      {"superset"sv,
       Value(FunctionValue(
           {{"other", false, "Set"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& a = callEnv->get("this").get<SetValue>();
             const auto& b = callEnv->get("other").get<SetValue>();
             for (const auto& v : *b.members) {
               if (!a.index->contains(v)) return Value(false);
             }
             return Value(true);
           },
           "Bool"sv))},
      {"add"sv,
       Value(FunctionValue(
           {{"x", false}},
           [](std::shared_ptr<Environment> callEnv) {
             auto& self = const_cast<SetValue&>(
                 callEnv->get("this").get<SetValue>());
             return Value(self.add(callEnv->get("x")));
           },
           "Bool"sv))},
      {"remove"sv,
       Value(FunctionValue(
           {{"x", false}},
           [](std::shared_ptr<Environment> callEnv) {
             auto& self = const_cast<SetValue&>(
                 callEnv->get("this").get<SetValue>());
             auto it = self.index->find(callEnv->get("x"));
             if (it == self.index->end()) return Value(false);
             size_t idx = it->second;
             self.index->erase(it);
             self.members->erase(self.members->begin() + idx);
             // Shift indices that came after the removed slot.
             for (auto& [_, i] : *self.index) {
               if (i > idx) --i;
             }
             return Value(true);
           },
           "Bool"sv))},
      {"to_array"sv,
       Value(FunctionValue(
           {},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& self = callEnv->get("this").get<SetValue>();
             ArrayValue out;
             out.values->reserve(self.members->size());
             for (const auto& v : *self.members) out.values->push_back(v);
             return Value(std::move(out));
           },
           "Array"sv))},
      {"iter"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return _iter_over_vector(
             callEnv->get("this").get<SetValue>().members);
       }))},
  };
  return props_;
}

// Method lookup table for primitive Tuple values. Mirrors set_builtins;
// TupleValue is not an ObjectValue so dispatch goes through the same
// dedicated path in eval_property.
inline std::map<std::string_view, Value>& tuple_builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"size"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& self = callEnv->get("this").get<TupleValue>();
         return Value(static_cast<long>(self.elements->size()));
       }))},
      {"contains"sv,
       Value(FunctionValue(
           {{"x", false}},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& self = callEnv->get("this").get<TupleValue>();
             const auto& x = callEnv->get("x");
             for (const auto& e : *self.elements) {
               if (e == x) return Value(true);
             }
             return Value(false);
           },
           "Bool"sv))},
      {"to_array"sv,
       Value(FunctionValue(
           {},
           [](std::shared_ptr<Environment> callEnv) {
             const auto& self = callEnv->get("this").get<TupleValue>();
             ArrayValue out;
             out.values->reserve(self.elements->size());
             for (const auto& e : *self.elements) out.values->push_back(e);
             return Value(std::move(out));
           },
           "Array"sv))},
      {"iter"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return _iter_over_vector(
             callEnv->get("this").get<TupleValue>().elements);
       }))}};
  return props_;
}

// Iterator method table. Any Object that has both `iter` and `next`
// properties picks these up as methods via the duck-typed fallback in
// eval_property. The lazy non-terminal methods (map/filter/take/...)
// return a new iterator wrapping the receiver; the terminal methods
// (collect/for_each/reduce/...) consume the iterator and return a
// concrete value.
inline std::map<std::string_view, Value>& iterator_builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      // --- Non-terminal: return a new lazy Iterator ----------------

      {"map"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto f = callEnv->get("f");
         return _make_iterator(
             [upstream, f](std::shared_ptr<Environment>) {
               auto v = _iter_next_value(upstream);
               if (!v) return _iter_step_done();
               return _iter_step_value(_invoke_callback(f, *v));
             });
       }))},

      {"filter"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = callEnv->get("p");
         return _make_iterator(
             [upstream, p](std::shared_ptr<Environment>) {
               for (;;) {
                 auto v = _iter_next_value(upstream);
                 if (!v) return _iter_step_done();
                 if (_invoke_callback(p, *v).to_bool()) {
                   return _iter_step_value(std::move(*v));
                 }
               }
             });
       }))},

      {"take"sv,
       Value(FunctionValue({{"n", false, "Long"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto limit = callEnv->get("n").to_long();
         auto count = std::make_shared<long>(0);
         return _make_iterator(
             [upstream, limit, count](std::shared_ptr<Environment>) {
               if (*count >= limit) return _iter_step_done();
               auto v = _iter_next_value(upstream);
               if (!v) return _iter_step_done();
               (*count)++;
               return _iter_step_value(std::move(*v));
             });
       }))},

      {"skip"sv,
       Value(FunctionValue({{"n", false, "Long"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto limit = callEnv->get("n").to_long();
         auto skipped = std::make_shared<bool>(false);
         return _make_iterator(
             [upstream, limit, skipped](std::shared_ptr<Environment>) {
               if (!*skipped) {
                 *skipped = true;
                 for (long i = 0; i < limit; i++) {
                   if (!_iter_next_value(upstream)) return _iter_step_done();
                 }
               }
               auto v = _iter_next_value(upstream);
               if (!v) return _iter_step_done();
               return _iter_step_value(std::move(*v));
             });
       }))},

      {"take_while"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = callEnv->get("p");
         auto exhausted = std::make_shared<bool>(false);
         return _make_iterator(
             [upstream, p, exhausted](std::shared_ptr<Environment>) {
               if (*exhausted) return _iter_step_done();
               auto v = _iter_next_value(upstream);
               if (!v || !_invoke_callback(p, *v).to_bool()) {
                 *exhausted = true;
                 return _iter_step_done();
               }
               return _iter_step_value(std::move(*v));
             });
       }))},

      // f(x) must return an iterable; inner iterator is consumed
      // before advancing the upstream.
      {"flat_map"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto f = callEnv->get("f");
         // nullopt-like sentinel: inner iterator value (or Nil when
         // none is active).
         auto inner = std::make_shared<Value>();
         auto line = callEnv->get("__LINE__").to_long();
         auto col = callEnv->get("__COLUMN__").to_long();
         return _make_iterator(
             [upstream, f, inner, line, col](std::shared_ptr<Environment>) {
               for (;;) {
                 if (inner->type == Value::Nil) {
                   auto outer = _iter_next_value(upstream);
                   if (!outer) return _iter_step_done();
                   *inner = _get_iterator(_invoke_callback(f, *outer),
                                          line, col);
                 }
                 auto v = _iter_next_value(*inner);
                 if (!v) { *inner = Value(); continue; }
                 return _iter_step_value(std::move(*v));
               }
             });
       }))},

      {"chain"sv,
       Value(FunctionValue({{"other", false}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto first = callEnv->get("this");
         auto line = callEnv->get("__LINE__").to_long();
         auto col = callEnv->get("__COLUMN__").to_long();
         // Resolve other's iterator up front; also accept anything
         // iterable for `other` (duck-typed).
         auto second = _get_iterator(callEnv->get("other"), line, col);
         auto on_first = std::make_shared<bool>(true);
         return _make_iterator(
             [first, second, on_first](std::shared_ptr<Environment>) {
               if (*on_first) {
                 auto v = _iter_next_value(first);
                 if (v) return _iter_step_value(std::move(*v));
                 *on_first = false;
               }
               auto v = _iter_next_value(second);
               if (!v) return _iter_step_done();
               return _iter_step_value(std::move(*v));
             });
       }))},

      // Pairs elements from both iterators as {first, second} Objects;
      // stops at the shorter side.
      {"zip"sv,
       Value(FunctionValue({{"other", false}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto a = callEnv->get("this");
         auto line = callEnv->get("__LINE__").to_long();
         auto col = callEnv->get("__COLUMN__").to_long();
         auto b = _get_iterator(callEnv->get("other"), line, col);
         return _make_iterator([a, b](std::shared_ptr<Environment>) {
           auto va = _iter_next_value(a);
           if (!va) return _iter_step_done();
           auto vb = _iter_next_value(b);
           if (!vb) return _iter_step_done();
           ObjectValue pair;
           pair.initialize("first", std::move(*va), false);
           pair.initialize("second", std::move(*vb), false);
           return _iter_step_value(Value(std::move(pair)));
         });
       }))},

      // Yields {index, value} Objects with index starting at 0.
      {"enumerate"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto index = std::make_shared<long>(0);
         return _make_iterator(
             [upstream, index](std::shared_ptr<Environment>) {
               auto v = _iter_next_value(upstream);
               if (!v) return _iter_step_done();
               ObjectValue pair;
               pair.initialize("index", Value(*index), false);
               pair.initialize("value", std::move(*v), false);
               (*index)++;
               return _iter_step_value(Value(std::move(pair)));
             });
       }))},

      // --- Terminal: consume the iterator and return a value -------

      {"collect"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         ArrayValue out;
         while (auto v = _iter_next_value(upstream)) {
           out.values->push_back(std::move(*v));
         }
         return Value(std::move(out));
       }))},

      {"for_each"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto f = callEnv->get("f");
         while (auto v = _iter_next_value(upstream)) {
           _invoke_callback(f, *v);
         }
         return Value();
       }))},

      {"reduce"sv,
       Value(FunctionValue({{"init", false}, {"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto acc = callEnv->get("init");
         auto f = callEnv->get("f");
         while (auto v = _iter_next_value(upstream)) {
           acc = _invoke_callback(f, acc, *v);
         }
         return acc;
       }))},

      {"find"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = callEnv->get("p");
         while (auto v = _iter_next_value(upstream)) {
           if (_invoke_callback(p, *v).to_bool()) return *v;
         }
         return Value();
       }))},

      {"any"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = callEnv->get("p");
         while (auto v = _iter_next_value(upstream)) {
           if (_invoke_callback(p, *v).to_bool()) return Value(true);
         }
         return Value(false);
       }))},

      {"all"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = callEnv->get("p");
         while (auto v = _iter_next_value(upstream)) {
           if (!_invoke_callback(p, *v).to_bool()) return Value(false);
         }
         return Value(true);
       }))},

      {"count"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         long n = 0;
         while (_iter_next_value(upstream)) n++;
         return Value(n);
       }))},

      {"sum"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         long acc = 0;
         while (auto v = _iter_next_value(upstream)) acc += v->to_long();
         return Value(acc);
       }))},

      {"product"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         long acc = 1;
         while (auto v = _iter_next_value(upstream)) acc *= v->to_long();
         return Value(acc);
       }))},

      {"min"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto first = _iter_next_value(upstream);
         if (!first) {
           throw CulebraError("ValueError",
                              "min of empty Iterator.");
         }
         long best = first->to_long();
         while (auto v = _iter_next_value(upstream)) {
           long x = v->to_long();
           if (x < best) best = x;
         }
         return Value(best);
       }))},

      {"max"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto first = _iter_next_value(upstream);
         if (!first) {
           throw CulebraError("ValueError",
                              "max of empty Iterator.");
         }
         long best = first->to_long();
         while (auto v = _iter_next_value(upstream)) {
           long x = v->to_long();
           if (x > best) best = x;
         }
         return Value(best);
       }))},
  };
  return props_;
}

// Shared arg parser for the integer range/iota factories. 1 arg → end
// (start defaults to 0), 2+ args → (start, end). Missing args leave the
// pair at (0, 0), which yields an empty range/iota.
inline std::pair<long, long> _parse_range_args(
    std::shared_ptr<Environment> callEnv) {
  long start = 0, end = 0;
  const auto& extras = *callEnv->get("__ARGS__").to_array().values;
  if (extras.size() == 1) {
    end = extras[0].to_long();
  } else if (extras.size() >= 2) {
    start = extras[0].to_long();
    end = extras[1].to_long();
  }
  return {start, end};
}

// Language-core globals available in every Environment that opts in via
// setup_core_globals (called by `culebra::environment()` for the CLI
// and by any embedder that wants the same defaults). `range` / `iota`
// are first-class iterator/array factories — Python/Rust/Kotlin etc.
// expose them as builtins, not under a Math namespace.
inline void setup_core_globals(Environment& env) {
  // iota(n) / iota(start, end): materialize a new Array of consecutive
  // integers. Eager; for lazy iteration use range.
  env.initialize(
      "iota",
      Value(FunctionValue(
          {}, [](std::shared_ptr<Environment> callEnv) {
            auto [start, end] = _parse_range_args(callEnv);
            ArrayValue out;
            if (end > start) out.values->reserve(end - start);
            for (long i = start; i < end; i++) {
              out.values->push_back(Value(i));
            }
            return Value(std::move(out));
          })),
      false);

  // range(n) / range(start, end) / range(..., step: N): lazy integer
  // iterator (constant additional memory). Step is keyword-only and
  // must be non-zero. Yields successive integers via the iterator
  // protocol (see language.md §17.5).
  env.initialize(
      "range",
      Value(FunctionValue(
          {{"step", false, {}, nullptr, kw_default_one(), true}},
          [](std::shared_ptr<Environment> callEnv) {
            auto [start, end] = _parse_range_args(callEnv);
            auto step = callEnv->get("step").to_long();
            if (step == 0) {
              auto line = callEnv->get("__LINE__").to_long();
              auto col = callEnv->get("__COLUMN__").to_long();
              throw CulebraError("ValueError",
                  "range() step must not be zero", line, col);
            }
            auto current = std::make_shared<long>(start);
            return _make_iterator(
                [current, end, step](std::shared_ptr<Environment>) {
                  bool done = step > 0 ? *current >= end : *current <= end;
                  if (done) return _iter_step_done();
                  auto v = Value(*current);
                  *current += step;
                  return _iter_step_value(std::move(v));
                });
          })),
      false);
}

// Held by shared_ptr so FunctionValue / multifn dispatcher / class
// constructor / defer lambdas can keep the Interpreter alive past the
// stack-scope of `culebra::interpret`. Embedders that build a
// FunctionValue inside `interpret(ast, env, ...)` and then invoke it
// later through `culebra::call(env, name, args)` rely on this — the
// captured `Interpreter*` would otherwise dangle.
struct Interpreter : std::enable_shared_from_this<Interpreter> {
  Interpreter(Debugger debugger = nullptr) : debugger_(debugger) {}

  struct MultiMethod {
    std::vector<std::string> param_types;
    Value body;
  };
  // Method tables for top-level `fn name(...)` decls. The dispatcher
  // registered in env captures a shared_ptr to one of these vectors,
  // so subsequent decls just push_back here. Owned here (not in env)
  // because env's keys are std::string_view into AST tokens, and the
  // synthesized table key has to outlive any such view.
  std::map<std::string, std::shared_ptr<std::vector<MultiMethod>>>
      multimethods_;

  Value eval(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    try {
      return _eval_dispatch(ast, env);
    } catch (const CulebraError& e) {
      // Attach the AST location of the deepest eval() that threw — only
      // once, by checking whether the message already carries " at ".
      // Re-throw as CulebraError so kind/line/col reach eval_try intact.
      std::string_view msg = e.what();
      if (msg.find(" at ") != std::string_view::npos) throw;
      throw CulebraError(
          e.kind, std::format("{} at {}:{}.", msg, ast.line, ast.column),
          static_cast<long>(ast.line), static_cast<long>(ast.column));
    } catch (const std::runtime_error& e) {
      // Legacy path for any C++ throw site not yet converted.
      std::string_view msg = e.what();
      if (msg.find(" at ") != std::string_view::npos) throw;
      throw std::runtime_error(
          std::format("{} at {}:{}.", msg, ast.line, ast.column));
    }
  }

  Value _eval_dispatch(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;

    if (debugger_) {
      if (ast.original_tag == "STATEMENT"_) {
        auto force_to_break = ast.tag == "DEBUGGER"_;
        debugger_(ast, *env, force_to_break);
      }
    }

    switch (ast.tag) {
      case "STATEMENTS"_:
        return eval_statements(ast, env);
      case "WHILE"_:
        return eval_while(ast, env);
      case "FOR"_:
        return eval_for(ast, env);
      case "IF"_:
        return eval_if(ast, env);
      case "MATCH"_:
        return eval_match(ast, env);
      case "FUNCTION"_:
        return eval_function(ast, env);
      case "LAMBDA"_:
        return eval_lambda(ast, env);
      case "CALL"_:
        return eval_call(ast, env);
      case "LEXICAL_SCOPE"_:
        return eval_lexical_scope(ast, env);
      case "ASSIGNMENT"_:
        return eval_assignment(ast, env);
      case "LOGICAL_OR"_:
        return eval_logical_or(ast, env);
      case "NIL_COALESCE"_:
        return eval_nil_coalesce(ast, env);
      case "LOGICAL_AND"_:
        return eval_logical_and(ast, env);
      case "CONDITION"_:
        return eval_condition(ast, env);
      case "UNARY_PLUS"_:
        return eval_unary_plus(ast, env);
      case "UNARY_MINUS"_:
        return eval_unary_minus(ast, env);
      case "UNARY_NOT"_:
        return eval_unary_not(ast, env);
      case "ADDITIVE"_:
      case "MULTIPLICATIVE"_:
        return eval_bin_expression(ast, env);
      case "RANGE"_:
        return eval_range(ast, env);
      case "DESTRUCTURE_ASSIGN"_:
        return eval_destructure_assign(ast, env);
      case "POWER"_:
        return eval_power(ast, env);
      case "IDENTIFIER"_:
        return eval_identifier(ast, env);
      case "OBJECT"_:
        return eval_object(ast, env);
      case "ARRAY"_:
        return eval_array(ast, env);
      case "TUPLE"_: {
        std::vector<Value> elems;
        elems.reserve(ast.nodes.size());
        for (const auto& n : ast.nodes) elems.push_back(eval(*n, env));
        return Value(TupleValue(std::move(elems)));
      }
      case "SET"_: {
        SetValue s;
        for (const auto& n : ast.nodes) s.add(eval(*n, env));
        return Value(std::move(s));
      }
      case "NIL"_:
        return eval_nil(ast, env);
      case "BOOLEAN"_:
        return eval_bool(ast, env);
      case "NUMBER"_:
        return eval_number(ast, env);
      case "FLOAT"_:
        return eval_float(ast, env);
      case "INTERPOLATED_STRING"_:
        return eval_interpolated_string(ast, env);
      case "DEBUGGER"_:
        return Value();
      case "RETURN"_:
        eval_return(ast, env);
        std::unreachable();
      case "THROW"_:
        eval_throw(ast, env);
        std::unreachable();
      case "BREAK"_:
        throw BreakSignal{};
      case "CONTINUE"_:
        throw ContinueSignal{};
      case "TRY"_:
        return eval_try(ast, env);
      case "DEFER"_:
        eval_defer(ast, env);
        return Value();
      case "CLASS_DECL"_:
        return eval_class_decl(ast, env);
      case "MULTIFN_DECL"_:
        return eval_multifn_decl(ast, env);
    }

    if (ast.is_token) {
      // STRING (single-quote) is raw; INTERPOLATED_CONTENT decodes \X.
      if (ast.tag == "INTERPOLATED_CONTENT"_) {
        return Value(decode_interpolated_content(ast.token));
      }
      return Value(std::string(ast.token));
    }

    std::unreachable();
    throw std::logic_error("invalid Ast type");
  }

 private:
  Value eval_statements(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    if (ast.is_token) {
      return eval(ast, env);
    } else if (ast.nodes.empty()) {
      return Value();
    }
    auto it = ast.nodes.begin();
    while (it != ast.nodes.end() - 1) {
      eval(**it, env);
      ++it;
    }
    return eval(**it, env);
  }

  Value eval_while(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    for (;;) {
      auto cond = eval(*ast.nodes[0], env);
      if (!cond.to_bool()) {
        break;
      }
      try {
        eval(*ast.nodes[1], env);
      } catch (const BreakSignal&) {
        return Value();
      } catch (const ContinueSignal&) {
        // fall through to next iteration
      }
    }
    return Value();
  }

  // for IDENT in EXPR BLOCK
  // Calls EXPR.iter().next() repeatedly; binds value to IDENT in a
  // fresh scope per iteration. Honors break/continue and defer.
  Value eval_for(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;
    const auto& id = *ast.nodes[0];
    const auto& iter_expr = *ast.nodes[1];
    const auto& body = *ast.nodes[2];
    const auto& var_name = id.token;

    auto iterable = eval(iter_expr, env);
    auto iter_val = _get_iterator(iterable, iter_expr.line, iter_expr.column);
    auto iter_proto_error = [&](std::string_view what) -> CulebraError {
      return CulebraError(
          "TypeError",
          std::format("type error: {} at {}:{}.", what, iter_expr.line,
                      iter_expr.column),
          static_cast<long>(iter_expr.line),
          static_cast<long>(iter_expr.column));
    };
    if (iter_val.type != Value::Object) {
      throw iter_proto_error("iter() did not return an Object");
    }

    for (;;) {
      // step = iterator.next()
      const auto& iter_obj = iter_val.to_object();
      if (!iter_obj.has("next")) {
        throw iter_proto_error("iterator has no next()");
      }
      auto next_fn = iter_obj.get("next");
      if (next_fn.type != Value::Function) {
        throw iter_proto_error("iterator.next is not a Function");
      }
      auto step = next_fn.to_function().eval(_make_method_call_env(
          iter_val, iter_expr.line, iter_expr.column));
      if (step.type != Value::Object) {
        throw iter_proto_error("next() did not return an Object");
      }
      const auto& step_obj = step.to_object();
      if (step_obj.has("done") && step_obj.get("done").to_bool()) {
        break;
      }
      Value loop_val = step_obj.has("value") ? step_obj.get("value") : Value();

      auto scopeEnv = make_scope(env);
      scopeEnv->initialize(var_name, loop_val, false);
      try {
        eval(body, scopeEnv);
        run_deferred(scopeEnv);
      } catch (const BreakSignal&) {
        run_deferred(scopeEnv);
        return Value();
      } catch (const ContinueSignal&) {
        run_deferred(scopeEnv);
        // fall through
      } catch (...) {
        run_deferred(scopeEnv);
        throw;
      }
    }
    return Value();
  }

  Value eval_if(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    const auto& nodes = ast.nodes;

    for (auto i = 0u; i < nodes.size(); i += 2) {
      if (i + 1 == nodes.size()) {
        return eval(*nodes[i], env);
      } else {
        auto cond = eval(*nodes[i], env);
        if (cond.to_bool()) {
          return eval(*nodes[i + 1], env);
        }
      }
    }

    return Value();
  }

  // Bind a pattern variable into `env`, first checking that it does not
  // shadow a variable captured from an enclosing function. `ident_node`
  // supplies both the name (via its token) and the diagnostic location.
  // `mut` controls the mutability of the binding — match arms always bind
  // as mut; destructure-let honors the user-declared `mut` qualifier.
  void bind_pattern_name(std::shared_ptr<Environment>& env,
                         const peg::Ast& ident_node, Value val,
                         bool mut = true) {
    auto name = ident_node.token;
    env->initialize(name, std::move(val), mut);
  }

  // Try to match a pattern against `val`. On success, bind any introduced
  // variables into `env` and return true.
  bool try_pattern(const peg::Ast& pattern, const Value& val,
                   std::shared_ptr<Environment> env, bool mut = true) {
    using namespace peg::udl;

    // PATTERN with multiple children is an OR pattern.
    if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
      for (const auto& sub : pattern.nodes) {
        if (try_pattern(*sub, val, env, mut)) return true;
      }
      return false;
    }

    switch (pattern.tag) {
      case "WILDCARD"_:
        return true;
      case "NIL"_:
        return val.type == Value::Nil;
      case "BOOLEAN"_:
        return val.type == Value::Bool &&
               val.get<bool>() == (pattern.token == "true");
      case "NUMBER"_:
        return val.type == Value::Long &&
               val.get<long>() == pattern.token_to_number<long>();
      case "FLOAT"_:
        return val.type == Value::Float &&
               val.get<double>() == pattern.token_to_number<double>();
      case "STRING"_:
        return val.type == Value::String &&
               val.get<std::string>() == std::string(pattern.token);
      case "INTERPOLATED_CONTENT"_:
        return val.type == Value::String &&
               val.get<std::string>() ==
                   decode_interpolated_content(pattern.token);
      case "IDENTIFIER"_: {
        bind_pattern_name(env, pattern, val, mut);
        return true;
      }
      case "TYPED_IDENT"_: {
        auto type_name = pattern.nodes[1]->token;
        if (!type_matches(val, type_name)) return false;
        bind_pattern_name(env, *pattern.nodes[0], val, mut);
        return true;
      }
      case "ARRAY_PATTERN"_: {
        if (val.type != Value::Array) return false;
        const auto& items = *val.to_array().values;
        const auto& elems = pattern.nodes;

        // Find optional rest position
        int rest_idx = -1;
        for (size_t i = 0; i < elems.size(); i++) {
          if (elems[i]->tag == "REST_PATTERN"_) {
            if (rest_idx >= 0) return false;  // at most one rest
            rest_idx = static_cast<int>(i);
          }
        }

        if (rest_idx < 0) {
          if (items.size() != elems.size()) return false;
          for (size_t i = 0; i < elems.size(); i++) {
            if (!try_pattern(*elems[i], items[i], env, mut)) return false;
          }
          return true;
        }

        // With rest: require at least fixed element count
        auto fixed = elems.size() - 1;
        if (items.size() < fixed) return false;
        // Match pre-rest fixed elements
        for (int i = 0; i < rest_idx; i++) {
          if (!try_pattern(*elems[i], items[i], env, mut)) return false;
        }
        // Collect rest into new Array
        auto rest_len = items.size() - fixed;
        ArrayValue rest;
        rest.values->reserve(rest_len);
        for (size_t j = 0; j < rest_len; j++) {
          rest.values->push_back(items[rest_idx + j]);
        }
        bind_pattern_name(env, *elems[rest_idx]->nodes[0],
                          Value(std::move(rest)), mut);
        // Match post-rest fixed elements
        for (size_t i = rest_idx + 1; i < elems.size(); i++) {
          auto src_idx = items.size() - (elems.size() - i);
          if (!try_pattern(*elems[i], items[src_idx], env, mut)) return false;
        }
        return true;
      }
      case "OBJECT_PATTERN"_: {
        if (val.type != Value::Object) return false;
        const auto& obj = val.to_object();
        for (const auto& entry : pattern.nodes) {
          // Two shapes per entry:
          //   - shorthand: bare IDENTIFIER, hoisted by the optimizer →
          //     bind that name to obj[name]
          //   - full: OBJECT_PAT_ENTRY with [IDENTIFIER, PATTERN] →
          //     bind/match PATTERN against obj[key]
          std::string_view key;
          const peg::Ast* sub_pattern = nullptr;
          if (entry->tag == "OBJECT_PAT_ENTRY"_) {
            key = entry->nodes[0]->token;
            sub_pattern = entry->nodes[1].get();
          } else {
            key = entry->token;
          }
          if (!obj.has(key)) return false;
          if (sub_pattern) {
            if (!try_pattern(*sub_pattern, obj.get(key), env, mut)) {
              return false;
            }
          } else {
            bind_pattern_name(env, *entry, obj.get(key), mut);
          }
        }
        return true;
      }
      case "TUPLE_PATTERN"_: {
        if (val.type != Value::Tuple) return false;
        const auto& items = *val.get<TupleValue>().elements;
        const auto& elems = pattern.nodes;
        if (items.size() != elems.size()) return false;
        for (size_t i = 0; i < elems.size(); i++) {
          if (!try_pattern(*elems[i], items[i], env, mut)) return false;
        }
        return true;
      }
    }
    return false;
  }

  Value eval_match(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;
    auto subject = eval(*ast.nodes[0], env);
    const auto& arms = ast.nodes[1]->nodes;  // MATCH_ARMS
    for (const auto& arm : arms) {
      // arm.nodes: PATTERN (GUARD)? EXPRESSION
      const auto& pattern = *arm->nodes[0];
      size_t next = 1;
      auto armEnv = make_scope(env);
      if (!try_pattern(pattern, subject, armEnv)) continue;

      if (next < arm->nodes.size() && arm->nodes[next]->tag == "GUARD"_) {
        auto guard_val = eval(*arm->nodes[next]->nodes[0], armEnv);
        next++;
        if (!guard_val.to_bool()) continue;
      }
      return eval(*arm->nodes[next], armEnv);
    }
    return Value();  // no arm matched → nil
  }

  // Parse a PARAMETERS AST node into FunctionValue::Parameter objects,
  // enforcing shadow + trailing-default rules. Shared by FUNCTION,
  // class METHODs, and class constructors.
  std::vector<FunctionValue::Parameter> parse_parameters(
      const peg::Ast& params_ast, std::shared_ptr<Environment> env) {
    std::vector<FunctionValue::Parameter> params;
    bool seen_default = false;
    bool kw_only = false;
    bool seen_sep = false;
    bool seen_rest = false;
    size_t kw_only_count = 0;
    for (auto node : params_ast.nodes) {
      if (seen_rest) {
        throw CulebraError("SyntaxError",
            "'**' catch-all must be the last parameter",
            static_cast<long>(node->line),
            static_cast<long>(node->column));
      }
      if (is_kw_only_sep(*node)) {
        if (seen_sep) {
          throw CulebraError("SyntaxError",
              "duplicate '*' keyword-only separator",
              static_cast<long>(node->line),
              static_cast<long>(node->column));
        }
        seen_sep = true;
        kw_only = true;
        // Keyword-only params can have any default pattern, so reset
        // the "non-default follows default" check at the separator.
        seen_default = false;
        continue;
      }
      if (is_kwargs_rest(*node)) {
        params.push_back({node->token, false, {}, nullptr, {}, false, true});
        seen_rest = true;
        continue;
      }
      auto mut = node->nodes[0]->token == "mut";
      auto& id = *node->nodes[1];
      const auto& name = id.token;
      auto type_name = extract_type_annotation(*node, 2);
      auto* default_expr = extract_default_expr(*node);
      if (default_expr) {
        seen_default = true;
      } else if (seen_default && !kw_only) {
        throw CulebraError("SyntaxError", std::format(
            "non-default parameter '{}' follows a default parameter at {}:{}.",
            std::string(name), id.line, id.column),
            static_cast<long>(id.line), static_cast<long>(id.column));
      }
      params.push_back({name, mut, type_name, default_expr, {}, kw_only});
      if (kw_only) kw_only_count++;
    }
    if (seen_sep && kw_only_count == 0 && !seen_rest) {
      throw CulebraError("SyntaxError",
          "named arguments must follow '*' separator",
          static_cast<long>(params_ast.line),
          static_cast<long>(params_ast.column));
    }
    return params;
  }

  // Build a FunctionValue from explicit PARAMETERS / BLOCK AST nodes.
  // Shared by eval_function (FUNCTION ast) and eval_class_decl (METHOD
  // ast), which have different wrapper shapes around the same core.
  Value make_function_value(const peg::Ast& params_ast,
                            std::shared_ptr<peg::Ast> body,
                            std::string_view return_type,
                            std::shared_ptr<Environment> env) {
    auto params = parse_parameters(params_ast, env);
    auto self = shared_from_this();
    return Value(FunctionValue(
        params,
        [self = std::move(self), body, env](std::shared_ptr<Environment> callEnv) {
          callEnv->append_outer(env);
          try {
            auto r = self->eval(*body, callEnv);
            self->run_deferred(callEnv);
            return r;
          } catch (...) {
            self->run_deferred(callEnv);
            throw;
          }
        },
        return_type,
        env));
  }

  Value eval_function(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    size_t body_idx = 1;
    auto return_type = extract_return_type(ast, body_idx);
    return make_function_value(*ast.nodes[0], ast.nodes[body_idx],
                               return_type, env);
  };

  // LAMBDA: [LAMBDA_PARAMS, BODY]. BODY may be a BLOCK or a bare
  // expression — both are handled by eval()'s tag dispatch.
  Value eval_lambda(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return make_function_value(*ast.nodes[0], ast.nodes[1], {}, env);
  }

  // Dynamic type-name view for dispatch. For Object instances with a
  // `class:` String tag this aliases the stored class name; the view
  // is valid as long as `v` is.
  static std::string_view value_dyn_type(const Value& v) {
    switch (v.type) {
      case Value::Nil:      return "Nil";
      case Value::Bool:     return "Bool";
      case Value::Long:     return "Long";
      case Value::Float:    return "Float";
      case Value::String:   return "String";
      case Value::Array:    return "Array";
      case Value::Function: return "Function";
      case Value::Tensor:   return "Tensor";
      case Value::Object:
        if (auto tag = class_tag(v)) return *tag;
        return "Object";
    }
    return "Object";
  }

  struct PickResult {
    enum Status { Match, NoMatch, Ambiguous } status;
    size_t idx;  // valid only when status == Match
  };

  // Adapter around the shared `culebra::multifn_pick` template — maps
  // its int64_t return into the interp-side PickResult enum.
  PickResult pick_method(const std::vector<MultiMethod>& methods,
                         const std::vector<Value>& args) {
    std::vector<std::string_view> arg_types(args.size());
    for (size_t p = 0; p < args.size(); p++) {
      arg_types[p] = value_dyn_type(args[p]);
    }
    auto r = multifn_pick(methods, arg_types,
                          [](const MultiMethod& m) -> const std::vector<std::string>& {
                            return m.param_types;
                          });
    if (r == -1) return {PickResult::NoMatch, 0};
    if (r == -2) return {PickResult::Ambiguous, 0};
    return {PickResult::Match, static_cast<size_t>(r)};
  }

  // `fn name(params) body` — first decl registers a dispatcher under
  // `name`; subsequent decls append (or overwrite a same-signature
  // entry).
  Value eval_multifn_decl(const peg::Ast& ast,
                          std::shared_ptr<Environment> env) {
    using namespace peg::udl;

    // Optional leading DECORATOR children. Decorator inside MULTIFN_DECL
    // is mutually exclusive with multimethod dispatch in this phase —
    // a decorated `fn name(...)` is bound directly under `name` and
    // does not participate in same-name overload accumulation.
    size_t i = 0;
    std::vector<const peg::Ast*> decorators;
    while (i < ast.nodes.size() && ast.nodes[i]->tag == "DECORATOR"_) {
      decorators.push_back(ast.nodes[i].get());
      i++;
    }

    auto name_view = ast.nodes[i]->token;
    auto name_owned = std::string(name_view);

    // Children: IDENTIFIER, PARAMETERS, [RETURN_TYPE,] BLOCK
    size_t params_idx = i + 1;
    size_t body_idx = i + 2;
    std::string_view return_type;
    if (body_idx < ast.nodes.size() &&
        ast.nodes[body_idx]->tag == "RETURN_TYPE"_) {
      return_type = ast.nodes[body_idx]->token;
      body_idx++;
    }
    auto fn_val = make_function_value(*ast.nodes[params_idx],
                                       ast.nodes[body_idx],
                                       return_type, env);

    if (!decorators.empty()) {
      // Apply decorators bottom-up: the closest to the `fn` keyword
      // (last in source order) wraps the raw value first, the
      // topmost decorator wraps the outermost.
      for (auto it = decorators.rbegin(); it != decorators.rend(); ++it) {
        const auto& dec_expr = *(*it)->nodes[0];
        Value deco_val = eval(dec_expr, env);
        CallArgs args;
        args.positional.push_back(std::move(fn_val));
        args.positional_locs.emplace_back(dec_expr.line, dec_expr.column);
        fn_val = invoke_user_function_with_args(
            deco_val, env, std::move(args), dec_expr.line, dec_expr.column);
      }
      env->initialize(name_view, std::move(fn_val), false);
      return Value();
    }

    MultiMethod method;
    method.body = fn_val;
    // Only regular positionally-bindable params participate in
    // multifn type dispatch — kw-only and `**rest` are sorted into
    // the picked method via the kwsorter side of the dispatcher.
    for (const auto& p : *fn_val.to_function().params) {
      if (p.kw_only || p.kwargs_rest) continue;
      method.param_types.emplace_back(p.type_name);
    }

    auto it = multimethods_.find(name_owned);
    if (it == multimethods_.end()) {
      auto methods = std::make_shared<std::vector<MultiMethod>>();
      methods->push_back(std::move(method));
      multimethods_[name_owned] = methods;

      // Synthetic `**__KWARGS__` catch-all on the dispatcher so kwargs
      // pass through to method dispatch (Julia-flavored kwsorter:
      // multimethod picks on positional types, then the picked
      // method's signature absorbs kwargs / splats).
      auto self = shared_from_this();
      auto dispatcher = Value(FunctionValue(
          {FunctionValue::Parameter::make_kwargs_rest("__KWARGS__")},
          [self = std::move(self), methods, name_owned](std::shared_ptr<Environment> callEnv) {
            auto line = callEnv->get("__LINE__").to_long();
            auto col = callEnv->get("__COLUMN__").to_long();
            const auto& args = *callEnv->get("__ARGS__").to_array().values;
            auto pick = self->pick_method(*methods, args);
            if (pick.status == PickResult::NoMatch) {
              throw CulebraError("DispatchError", std::format(
                  "no matching method for `{}` at {}:{}.", name_owned, line,
                  col), line, col);
            }
            if (pick.status == PickResult::Ambiguous) {
              throw CulebraError("DispatchError", std::format(
                  "ambiguous dispatch for `{}` at {}:{}.", name_owned, line,
                  col), line, col);
            }
            CallArgs call_args;
            call_args.positional = args;
            call_args.positional_locs.assign(
                args.size(),
                {static_cast<size_t>(line), static_cast<size_t>(col)});
            call_args.splats.push_back(callEnv->get("__KWARGS__"));
            return self->invoke_user_function_with_args(
                (*methods)[pick.idx].body, callEnv, std::move(call_args),
                static_cast<size_t>(line), static_cast<size_t>(col));
          }));
      env->initialize(name_view, dispatcher, false);
      return Value();
    }

    auto& methods = *it->second;
    for (auto& existing : methods) {
      if (existing.param_types == method.param_types) {
        existing = std::move(method);
        return Value();
      }
    }
    methods.push_back(std::move(method));
    return Value();
  }

  // `class Name { new(...) {...}  m(...) {...} }` desugars to
  //   Name = { new: fn(...) { mut this = { class: 'Name', m: fn(...){...} };
  //                           <new body>; this } }
  // `new` is optional; absent form accepts zero args and returns a
  // bare instance with only the class tag and methods. Methods close
  // over the defining scope but `this` is bound fresh per method call
  // via the existing method-dispatch protocol.
  Value eval_class_decl(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;

    // Optional leading DECORATOR children. Apply them to the final
    // class Object before binding.
    size_t k = 0;
    std::vector<const peg::Ast*> decorators;
    while (k < ast.nodes.size() && ast.nodes[k]->tag == "DECORATOR"_) {
      decorators.push_back(ast.nodes[k].get());
      k++;
    }

    // Method names and class name are kept as `string_view` into the
    // source AST (stable for the program's lifetime). This matches how
    // Environment and ObjectValue store keys, and avoids dangling
    // `string_view`s caused by moving std::strings inside captured
    // lambdas.
    auto class_name = ast.nodes[k]->token;

    const peg::Ast* new_ast = nullptr;
    std::vector<std::pair<std::string_view, Value>> method_template;
    for (size_t i = k + 1; i < ast.nodes.size(); i++) {
      const auto& m = *ast.nodes[i];
      auto name_view = m.nodes[0]->token;
      if (name_view == "new") {
        new_ast = &m;
      } else {
        auto fn_val =
            make_function_value(*m.nodes[1], m.nodes[2], {}, env);
        method_template.push_back({name_view, std::move(fn_val)});
      }
    }

    auto build_instance = [class_name, method_template]() {
      ObjectValue instance;
      instance.properties->emplace(
          "class", Symbol{Value(std::string(class_name)), false});
      for (const auto& [name, val] : method_template) {
        instance.properties->emplace(name, Symbol{val, false});
      }
      return instance;
    };

    // Class-sugar convention: instance fields set via `this.x = y` are
    // mutable regardless of the `let`/`mut` prefix used at assignment
    // time. This matches Python / Ruby / JS class semantics — methods
    // routinely mutate `this` — and spares the user from peppering
    // constructors with `mut this.x = ...`.
    auto promote_all_mut = [](const Value& inst) {
      if (inst.type != Value::Object) return;
      for (auto& [_, sym] : *inst.to_object().properties) {
        sym.mut = true;
      }
    };

    Value constructor;
    if (new_ast) {
      auto ctor_params = parse_parameters(*new_ast->nodes[1], env);
      auto body = new_ast->nodes[2];
      auto self = shared_from_this();
      constructor = Value(FunctionValue(
          ctor_params,
          [self = std::move(self), body, env, build_instance, promote_all_mut](
              std::shared_ptr<Environment> callEnv) {
            callEnv->append_outer(env);
            callEnv->initialize("this", Value(build_instance()), true);
            try {
              self->eval(*body, callEnv);
              self->run_deferred(callEnv);
            } catch (const ReturnValue&) {
              // Explicit `return` inside `new` is fine — we still hand
              // back `this`; the returned value is discarded.
              self->run_deferred(callEnv);
            } catch (...) {
              self->run_deferred(callEnv);
              throw;
            }
            auto this_val = callEnv->get("this");
            promote_all_mut(this_val);
            return this_val;
          },
          {},
          env));
    } else {
      constructor = Value(FunctionValue(
          {},
          [=](std::shared_ptr<Environment>) {
            auto inst = Value(build_instance());
            promote_all_mut(inst);
            return inst;
          },
          {},
          env));
    }

    ObjectValue class_obj;
    class_obj.properties->emplace("new", Symbol{constructor, false});
    Value class_val(std::move(class_obj));

    // Apply decorators bottom-up to the class value before binding.
    for (auto it = decorators.rbegin(); it != decorators.rend(); ++it) {
      const auto& dec_expr = *(*it)->nodes[0];
      Value deco_val = eval(dec_expr, env);
      CallArgs args;
      args.positional.push_back(std::move(class_val));
      args.positional_locs.emplace_back(dec_expr.line, dec_expr.column);
      class_val = invoke_user_function_with_args(
          deco_val, env, std::move(args), dec_expr.line, dec_expr.column);
    }

    env->initialize(class_name, std::move(class_val), false);
    return Value();
  }

  // Shared backbone for invoking a user function. `arg_value(i)` /
  // `arg_loc(i)` abstract where each of the `total` effective arguments
  // comes from — lazily evaluating AST args for a direct call, or
  // returning a pre-evaluated receiver at index 0 for a UFCS call.
  // `call_line`/`call_column` point at the call site for error messages.
  // Bundle of call arguments split into the three forms culebra
  // recognizes at a call site. Built by `split_call_args` from an
  // ARGUMENTS / ARG_LIST AST.
  struct CallArgs {
    std::vector<Value> positional;
    std::vector<std::pair<size_t, size_t>> positional_locs;
    std::vector<std::pair<std::string_view, Value>> kwargs;
    std::vector<std::pair<size_t, size_t>> kwarg_locs;
    std::vector<Value> splats;
  };

  // Walks an ARGUMENTS node (post-optimization, so it's the ARG_LIST
  // body). Splits children into positional / kwarg / splat buckets,
  // enforcing "positional must precede any kwarg/splat" at the AST
  // scan (yields a real line/col for the SyntaxError).
  void split_call_args(const peg::Ast& args_ast,
                       std::shared_ptr<Environment> env,
                       CallArgs& out) {
    using namespace peg::udl;
    bool saw_named = false;
    for (auto& child : args_ast.nodes) {
      // ARG_ITEM is not in the AstOptimizer keep-list, so an item
      // collapses to its inner KWARG / KWARG_SPLAT / expression node
      // (its `original_tag` stays "ARG_ITEM" but `tag` reveals the
      // concrete form).
      switch (child->tag) {
        case "KWARG_SPLAT"_: {
          saw_named = true;
          out.splats.push_back(eval(*child->nodes[0], env));
          break;
        }
        case "KWARG"_: {
          saw_named = true;
          out.kwargs.emplace_back(child->nodes[0]->token,
                                  eval(*child->nodes[1], env));
          out.kwarg_locs.emplace_back(child->line, child->column);
          break;
        }
        default: {
          if (saw_named) {
            throw CulebraError("SyntaxError",
                "positional argument follows keyword argument",
                child->line, child->column);
          }
          out.positional.push_back(eval(*child, env));
          out.positional_locs.emplace_back(child->line, child->column);
          break;
        }
      }
    }
  }

  // Resolves the three argument buckets against `fn_val`'s formal
  // parameters and populates a fresh function frame.
  Value invoke_user_function_with_args(
      const Value& fn_val, std::shared_ptr<Environment> env,
      CallArgs args, size_t call_line, size_t call_column) {
    const auto& f = fn_val.to_function();
    const auto& params = *f.params;

    // A. Merge splats into a single name→value map (later splat wins).
    std::unordered_map<std::string_view, Value> merged;
    for (const auto& sv : args.splats) {
      if (sv.type != Value::Object) {
        throw CulebraError("TypeError", std::format(
            "**: splat operand must be Object, got {}", sv.type_name()),
            call_line, call_column);
      }
      const auto& obj = sv.to_object();
      if (obj.non_string_props && !obj.non_string_props->empty()) {
        throw CulebraError("TypeError",
            "**: splat key must be String",
            call_line, call_column);
      }
      for (const auto& [k, sym] : *obj.properties) {
        merged[k] = sym.val;
      }
    }

    // B. Layer explicit kwargs on top; duplicates within explicit are
    //    rejected. Explicit always overrides a splat's contribution.
    std::unordered_set<std::string_view> seen_explicit;
    for (size_t i = 0; i < args.kwargs.size(); i++) {
      const auto& [name, val] = args.kwargs[i];
      if (!seen_explicit.insert(name).second) {
        auto [ln, col] = args.kwarg_locs[i];
        throw CulebraError("TypeError",
            std::format("duplicate keyword argument '{}'", name),
            static_cast<long>(ln), static_cast<long>(col));
      }
      merged[name] = val;
    }

    // A mid-list `*` separator caps positional count; check before
    // the per-param walk so this fires before "missing required" on a
    // kw-only slot when too many positionals were given.
    auto cap = first_kw_only_index(params);
    throw_if_too_many_positionals(
        cap ? static_cast<long>(*cap) : -1,
        static_cast<long>(args.positional.size()),
        static_cast<long>(call_line), static_cast<long>(call_column));

    auto callEnv = std::make_shared<Environment>(env);
    callEnv->is_function_frame = true;
    callEnv->initialize("self", fn_val, false);

    // C. Walk formal params in declaration order; consume positional
    //    first, then merged map, then default. The `**rest` catch-all
    //    is handled in step D.
    for (size_t p = 0; p < params.size(); p++) {
      if (params[p].kwargs_rest) continue;
      Value v;
      size_t ln = call_line, col = call_column;
      if (p < args.positional.size() && !params[p].kw_only) {
        v = std::move(args.positional[p]);
        std::tie(ln, col) = args.positional_locs[p];
        if (merged.contains(params[p].name)) {
          throw CulebraError("TypeError", std::format(
              "got argument '{}' both positionally and as a keyword",
              params[p].name), call_line, call_column);
        }
      } else if (auto it = merged.find(params[p].name); it != merged.end()) {
        v = std::move(it->second);
        merged.erase(it);
      } else if (params[p].default_expr) {
        // Default needs access to the FUNCTION's definition env (for any
        // closure-captured names) plus any earlier params. Build a fresh
        // env over def_env and copy already-bound earlier params in.
        // (Environment's ctor only inits `level`; set `outer` explicitly.)
        auto defEnv = std::make_shared<Environment>(f.def_env);
        defEnv->outer = f.def_env;
        for (size_t j = 0; j < p; j++) {
          defEnv->initialize(params[j].name, callEnv->get(params[j].name),
                             false);
        }
        v = eval(*params[p].default_expr, defEnv);
      } else if (params[p].default_value) {
        v = *params[p].default_value;
      } else {  // No default — required and missing.
        throw CulebraError("ArityError", std::format(
            "missing required argument '{}'", params[p].name),
            call_line, call_column);
      }
      check_type(v, params[p].type_name,
                 std::format("parameter '{}'", params[p].name), ln, col);
      callEnv->initialize(params[p].name, v, params[p].mut);
    }

    // D. Leftover kwargs flow into a `**rest` catch-all if declared,
    //    otherwise they are unknown to this function. The rest is
    //    always bound (even with empty merged) so the callee always
    //    sees the rest variable — matches the JIT prologue contract.
    auto rest_it = std::find_if(params.begin(), params.end(),
        [](auto& p) { return p.kwargs_rest; });
    if (rest_it != params.end()) {
      ObjectValue rest;
      for (auto& [k, v] : merged) {
        rest.initialize(k, std::move(v), false);
      }
      callEnv->initialize(rest_it->name, Value(std::move(rest)), false);
      merged.clear();
    } else if (!merged.empty()) {
      throw CulebraError("TypeError", std::format(
          "unknown keyword argument '{}'", merged.begin()->first),
          call_line, call_column);
    }

    // E. Overflow positional bind to __ARGS__. Extras start at the
    //    regular-param count — anything past the last positional-
    //    bindable slot flows into __ARGS__, regardless of whether the
    //    fn also declares kw-only params or a `**rest` catch-all.
    size_t extras_start = regular_param_count(params);
    ArrayValue extras;
    for (size_t idx = extras_start;
         idx < args.positional.size(); idx++) {
      extras.values->push_back(std::move(args.positional[idx]));
    }
    callEnv->initialize("__ARGS__", Value(std::move(extras)), false);

    callEnv->initialize("__LINE__", Value((long)call_line), false);
    callEnv->initialize("__COLUMN__", Value((long)call_column), false);

    Value result;
    try {
      result = f.eval(callEnv);
    } catch (const ReturnValue& r) {
      result = r.value;
    }
    check_type(result, f.return_type, "return value", call_line, call_column);
    return result;
  }

  // Legacy positional-only callback entry. Kept for callers that build
  // arguments programmatically (iterator protocol, helper invocations).
  template <typename ArgVal, typename ArgLoc>
  Value invoke_user_function(const Value& fn_val,
                             std::shared_ptr<Environment> env, size_t total,
                             ArgVal arg_value, ArgLoc arg_loc,
                             size_t call_line, size_t call_column) {
    CallArgs args;
    args.positional.reserve(total);
    args.positional_locs.reserve(total);
    for (size_t i = 0; i < total; i++) {
      args.positional.push_back(arg_value(i));
      auto [ln, col] = arg_loc(i);
      args.positional_locs.emplace_back(ln, col);
    }
    return invoke_user_function_with_args(fn_val, env, std::move(args),
                                          call_line, call_column);
  }

  Value eval_function_call(const peg::Ast& ast,
                           std::shared_ptr<Environment> env, const Value& val) {
    CallArgs args;
    split_call_args(ast, env, args);
    return invoke_user_function_with_args(val, env, std::move(args),
                                          ast.line, ast.column);
  }

  // UFCS (D / Nim style): call `fn_val` as if `receiver` were its first
  // positional argument, with the remaining arguments taken from the
  // AST's `ARGUMENTS` node. Deliberately does NOT bind `this` (UFCS is
  // a free-function call, not a method).
  Value eval_ufcs_call(const peg::Ast& args_ast,
                       std::shared_ptr<Environment> env,
                       const Value& fn_val, const Value& receiver,
                       size_t dot_line, size_t dot_column) {
    CallArgs args;
    args.positional.push_back(receiver);
    args.positional_locs.emplace_back(dot_line, dot_column);
    split_call_args(args_ast, env, args);
    return invoke_user_function_with_args(fn_val, env, std::move(args),
                                          args_ast.line, args_ast.column);
  }

  Value eval_array_reference(const peg::Ast& ast,
                             std::shared_ptr<Environment> env,
                             const Value& val) {
    auto key = eval(ast, env);
    // Object[k]: look up the Value-keyed sidecar.
    if (val.type == Value::Object) {
      const auto& obj = val.to_object();
      if (obj.has(key)) return obj.get(key);
      throw CulebraError("KeyError", "key not present");
    }
    // Tuple[i]: same Long-indexed access as Array, but read-only.
    if (val.type == Value::Tuple) {
      const auto& elems = *val.get<TupleValue>().elements;
      auto idx = key.to_long();
      if (idx < 0) idx = static_cast<long>(elems.size()) + idx;
      if (0 <= idx && idx < static_cast<long>(elems.size())) {
        return elems[idx];
      }
      throw CulebraError("IndexError", "index out of range.");
    }
    const auto& arr = val.to_array();
    auto idx = key.to_long();
    if (idx < 0) {
      idx = arr.values->size() + idx;
    }
    if (0 <= idx && idx < static_cast<long>(arr.values->size())) {
      return arr.values->at(idx);
    } else {
      throw CulebraError("IndexError", "index out of range.");
    }
    return val;
  }

  // Wrap a method value (looked up from a builtin table) into a
  // Function that will bind `this` to `val` when invoked. Matches how
  // eval_property used to inline this for Object/Array methods.
  static Value _wrap_method_with_this(const Value& prop, const Value& val) {
    const auto& pf = prop.to_function();
    return Value(
        FunctionValue(*pf.params, [=](std::shared_ptr<Environment> callEnv) {
          callEnv->initialize("this", val, false);
          return pf.eval(callEnv);
        }));
  }

  // Recursively flatten `val` into class-instance leaves: Arrays are
  // descended element-wise, plain Object dicts (no `class:` tag) are
  // descended into their property values, and class instances are
  // collected as leaves. Scalars are skipped. Property keys starting
  // with '_' are skipped (private/cache fields, like microgpt's
  // `_visited` flag). Used to auto-synthesize `parameters()` for
  // class instances; see eval_property below.
  static void _walk_collect_params(const Value& val,
                                   std::vector<Value>& out) {
    if (val.type == Value::Array) {
      for (const auto& elem : *val.to_array().values) {
        _walk_collect_params(elem, out);
      }
    } else if (val.type == Value::Object) {
      if (class_tag(val)) {
        out.push_back(val);
        return;
      }
      for (const auto& [key, sym] : *val.to_object().properties) {
        if (key == "class") continue;
        if (!key.empty() && key[0] == '_') continue;
        _walk_collect_params(sym.val, out);
      }
    }
  }

  // Synthesize a parameter-less `parameters()` method on a class
  // instance. Walks the instance's own properties and returns a flat
  // Array of every class-instance Value found inside (the instance
  // itself is not included). User-defined `parameters()` takes
  // precedence — handled by the existing user-property branch in
  // eval_property.
  static Value _synthesize_parameters(const Value& val) {
    return Value(
        FunctionValue({}, [val](std::shared_ptr<Environment>) -> Value {
          ArrayValue result;
          if (val.type == Value::Object) {
            for (const auto& [key, sym] : *val.to_object().properties) {
              if (key == "class") continue;
              if (!key.empty() && key[0] == '_') continue;
              _walk_collect_params(sym.val, *result.values);
            }
          }
          return Value(std::move(result));
        }));
  }

  Value eval_property(const peg::Ast& ast, std::shared_ptr<Environment> env,
                      const Value& val) {
    auto name = ast.token;

    // String is a primitive; its methods live in a separate table.
    if (val.type == Value::String) {
      const auto& methods = string_builtins();
      auto it = methods.find(name);
      if (it == methods.end()) return Value();
      return _wrap_method_with_this(it->second, val);
    }

    // Set / Tuple are not ObjectValues either; same dedicated-table dispatch.
    if (val.type == Value::Set) {
      const auto& methods = set_builtins();
      auto it = methods.find(name);
      if (it == methods.end()) return Value();
      return _wrap_method_with_this(it->second, val);
    }
    if (val.type == Value::Tuple) {
      const auto& methods = tuple_builtins();
      auto it = methods.find(name);
      if (it == methods.end()) return Value();
      return _wrap_method_with_this(it->second, val);
    }

    const auto& obj = val.to_object();
    if (obj.has(name)) {
      const auto& prop = obj.get(name);
      if (prop.type == Value::Function) {
        return _wrap_method_with_this(prop, val);
      }
      return prop;
    }

    // Auto-synthesized `parameters()` on class instances: walks the
    // instance's fields and returns a flat Array of class-instance
    // Values. A user-defined `parameters` method takes precedence
    // (handled by the obj.has(name) branch above).
    if (name == "parameters" && class_tag(val)) {
      return _synthesize_parameters(val);
    }

    // Duck-typed iterator protocol fallback: any Object/Array that has
    // both `iter` and `next` methods is treated as an Iterator, and
    // gains the lazy method set (map/filter/take/.../collect — see
    // iterator_builtins()). Eager Array methods take priority above.
    if (obj.has("iter") && obj.has("next")) {
      const auto& methods = iterator_builtins();
      auto it = methods.find(name);
      if (it != methods.end()) {
        return _wrap_method_with_this(it->second, val);
      }
    }
    return Value();
  }

  // Decide whether `val` has a method/property named `name` (for
  // distinguishing "real method call" from "UFCS fallback candidate").
  // Returning false does NOT imply `val` is a non-object — primitives
  // like Long simply have no methods.
  bool receiver_has_property(const Value& val, std::string_view name) {
    if (val.type == Value::String) {
      return string_builtins().count(name) > 0;
    }
    if (val.type == Value::Set) {
      return set_builtins().count(name) > 0;
    }
    if (val.type == Value::Tuple) {
      return tuple_builtins().count(name) > 0;
    }
    if (val.type == Value::Object || val.type == Value::Array) {
      if (val.to_object().has(name)) return true;
      // Synthesized `parameters()` is available on every class instance
      // (see eval_property).
      if (name == "parameters" && class_tag(val)) return true;
    }
    return false;
  }

  Value eval_call(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;

    Value val = eval(*ast.nodes[0], env);

    for (auto i = 1u; i < ast.nodes.size(); i++) {
      const auto& postfix = *ast.nodes[i];

      switch (postfix.original_tag) {
        case "ARGUMENTS"_:
          val = eval_function_call(postfix, env, val);
          break;
        case "INDEX"_:
          val = eval_array_reference(postfix, env, val);
          break;
        case "DOT"_: {
          // UFCS: when DOT is immediately followed by ARGUMENTS and the
          // receiver has no matching property, look up the name as a
          // free function in the surrounding environment and call it
          // with `val` as the first argument. Existing methods always
          // take priority (Option 1 from the UFCS design discussion).
          auto name = postfix.token;
          bool has_prop = receiver_has_property(val, name);
          bool next_is_args =
              (i + 1 < ast.nodes.size()) &&
              (ast.nodes[i + 1]->original_tag == "ARGUMENTS"_);
          if (!has_prop && next_is_args && env->has(name)) {
            auto fn_val = env->get(name);
            if (fn_val.type == Value::Function) {
              val = eval_ufcs_call(*ast.nodes[i + 1], env, fn_val, val,
                                   postfix.line, postfix.column);
              i++;  // consume the ARGUMENTS postfix
              break;
            }
          }
          val = eval_property(postfix, env, val);
          break;
        }
        default:
          throw std::logic_error("invalid internal condition.");
      }
    }

    return val;
  }

  Value eval_lexical_scope(const peg::Ast& ast,
                           std::shared_ptr<Environment> env) {
    auto scopeEnv = make_scope(env);
    try {
      for (auto node : ast.nodes) {
        eval(*node, scopeEnv);
      }
    } catch (...) {
      run_deferred(scopeEnv);
      throw;
    }
    run_deferred(scopeEnv);
    return Value();
  }

  Value eval_logical_or(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    assert(ast.nodes.size() >
           1);  // if the size is 1, thes node will be hoisted.
    Value val;
    for (auto node : ast.nodes) {
      val = eval(*node, env);
      if (val.to_bool()) {
        return val;
      }
    }
    return val;
  }

  Value eval_logical_and(const peg::Ast& ast,
                         std::shared_ptr<Environment> env) {
    Value val;
    for (auto node : ast.nodes) {
      val = eval(*node, env);
      if (!val.to_bool()) {
        return val;
      }
    }
    return val;
  }

  Value eval_condition(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    auto lhs = eval(*ast.nodes[0], env);
    auto ope = eval(*ast.nodes[1], env).to_string();
    auto rhs = eval(*ast.nodes[2], env);

    // Special-method dispatch for Object LHS. A class that defines just
    // `__eq__` and `__lt__` gets the full six-way suite: `__le__` is
    // derived as `lt || eq`, and the three "greater" ops are negations
    // of the corresponding "less-or-equal" / "less" forms.
    auto bool_val = [&](const Value& v) {
      return v.type == Value::Bool ? v.template get<bool>() : v.to_bool();
    };
    auto le_as_bool = [&]() -> std::optional<bool> {
      if (auto le = try_special_binop(lhs, rhs, "__le__", env))
        return bool_val(*le);
      auto lt = try_special_binop(lhs, rhs, "__lt__", env);
      auto eq = try_special_binop(lhs, rhs, "__eq__", env);
      if (!lt && !eq) return std::nullopt;
      return (lt && bool_val(*lt)) || (eq && bool_val(*eq));
    };
    // Auto-reflection: `==` is commutative, so if the LHS doesn't
    // carry `__eq__` but the RHS does, reach over and use it. Ordering
    // operators (`<`, `<=`) are not commutative and don't reflect.
    auto try_eq = [&]() -> std::optional<Value> {
      if (auto r = try_special_binop(lhs, rhs, "__eq__", env)) return r;
      if (auto r = try_special_binop(rhs, lhs, "__eq__", env)) return r;
      return std::nullopt;
    };
    if (lhs.type == Value::Object || rhs.type == Value::Object) {
      if (ope == "==") {
        if (auto r = try_eq()) return Value(bool_val(*r));
      } else if (ope == "!=") {
        if (auto r = try_eq()) return Value(!bool_val(*r));
      }
    }
    if (lhs.type == Value::Object) {
      if (ope == "<") {
        if (auto r = try_special_binop(lhs, rhs, "__lt__", env))
          return Value(bool_val(*r));
      } else if (ope == "<=") {
        if (auto r = le_as_bool()) return Value(*r);
      } else if (ope == ">") {
        if (auto r = le_as_bool()) return Value(!*r);
      } else if (ope == ">=") {
        if (auto r = try_special_binop(lhs, rhs, "__lt__", env))
          return Value(!bool_val(*r));
      }
    }

    if (ope == "==") {
      return Value(lhs == rhs);
    } else if (ope == "!=") {
      return Value(lhs != rhs);
    } else if (ope == "<=") {
      return Value(lhs <= rhs);
    } else if (ope == "<") {
      return Value(lhs < rhs);
    } else if (ope == ">=") {
      return Value(lhs >= rhs);
    } else if (ope == ">") {
      return Value(lhs > rhs);
    } else {
      throw std::logic_error("invalid internal condition.");
    }
  }

  Value eval_unary_plus(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return eval(*ast.nodes[1], env);
  }

  Value eval_unary_minus(const peg::Ast& ast,
                         std::shared_ptr<Environment> env) {
    auto v = eval(*ast.nodes[1], env);
    if (auto r = try_special_unary(v, "__neg__", env)) return *r;
    if (v.type == Value::Float) return Value(-v.get<double>());
    return Value(v.to_long() * -1);
  }

  Value eval_unary_not(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return Value(!eval(*ast.nodes[1], env).to_bool());
  }

  // Arithmetic with Long↔Float promotion. Both operands Long → Long
  // result (integer arithmetic, truncated division/modulo — matching
  // C semantics, unchanged from before Float was introduced). Either
  // operand Float → Float result via double promotion.
  // `a ?? b ?? c` returns the first non-nil operand, short-circuiting
  // on evaluation. All-nil chains return nil.
  Value eval_nil_coalesce(const peg::Ast& ast,
                          std::shared_ptr<Environment> env) {
    auto val = eval(*ast.nodes[0], env);
    for (size_t i = 1; i < ast.nodes.size(); i++) {
      if (val.type != Value::Nil) return val;
      val = eval(*ast.nodes[i], env);
    }
    return val;
  }

  // `a..b` (exclusive) and `a..=b` (inclusive) yield a lazy integer
  // iterator equivalent to Math.range — for-in compatible, no up-front
  // allocation. `to_long` throws `type error` on Float or non-numeric
  // operands (matching the JIT's `value_to_long` guard); range literals
  // are stricter than Math.range on purpose.
  Value eval_range(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    auto start = eval(*ast.nodes[0], env).to_long();
    auto end = eval(*ast.nodes[2], env).to_long();
    if (ast.nodes[1]->token == "..=") end++;
    auto current = std::make_shared<long>(start);
    return _make_iterator(
        [current, end](std::shared_ptr<Environment>) {
          if (*current >= end) return _iter_step_done();
          auto v = Value(*current);
          (*current)++;
          return _iter_step_value(std::move(v));
        });
  }

  // Shared special-method dispatch core. Arity 0 (unary) or 1 (binary);
  // the optional `rhs` is consumed only when arity == 1. Returns nullopt
  // if `receiver` isn't an Object carrying a callable special method of
  // this name.
  std::optional<Value> try_special(const Value& receiver, const Value* rhs,
                                  std::string_view special,
                                  std::shared_ptr<Environment> env) {
    if (receiver.type != Value::Object) return std::nullopt;
    const auto& obj = receiver.to_object();
    auto it = obj.properties->find(special);
    if (it == obj.properties->end()) return std::nullopt;
    const auto& m = it->second.val;
    if (m.type != Value::Function) return std::nullopt;
    auto bound = _wrap_method_with_this(m, receiver);
    size_t arity = rhs ? 1 : 0;
    return invoke_user_function(
        bound, env, arity,
        [&](size_t) { return rhs ? *rhs : Value(); },
        [&](size_t) -> std::pair<size_t, size_t> { return {0, 0}; },
        0, 0);
  }

  std::optional<Value> try_special_binop(const Value& lhs, const Value& rhs,
                                        std::string_view special,
                                        std::shared_ptr<Environment> env) {
    return try_special(lhs, &rhs, special, env);
  }

  std::optional<Value> try_special_unary(const Value& operand,
                                        std::string_view special,
                                        std::shared_ptr<Environment> env) {
    return try_special(operand, nullptr, special, env);
  }

  static const char* arith_op_to_special(char ope) {
    switch (ope) {
      case '+': return "__add__";
      case '-': return "__sub__";
      case '*': return "__mul__";
      case '/': return "__div__";
      case '%': return "__mod__";
      case '@': return "__matmul__";
    }
    return nullptr;
  }

  // `**` is only valid for in-place; regular binop uses `compute_power`.
  static std::optional<Op> op_to_tensor_op(std::string_view op) {
    if (op == "+") return Op::Add;
    if (op == "-") return Op::Sub;
    if (op == "*") return Op::Mul;
    if (op == "/") return Op::Div;
    if (op == "**") return Op::Pow;
    return std::nullopt;
  }

  // Returns nullptr when v is neither Tensor nor numeric — callers
  // decide whether to throw or fall back.
  static TensorPtr lift_to_tensor(const Value& v, Dtype dt) {
    if (v.type == Value::Tensor) return v.to_tensor().impl;
    if (v.is_numeric()) return tensor_scalar(v.to_double_coerce(), dt);
    return nullptr;
  }

  // Auto-reflection only fires for operators where `rhs.__op__(lhs)`
  // still computes the mathematically-correct `lhs op rhs` — i.e.,
  // commutative arithmetic. Non-commutative ops (`-`, `/`, `%`, `@`,
  // `**`) require the LHS to carry the special method. Reflection is
  // only attempted after the LHS-side special-method lookup fails.
  static bool op_reflects(char ope) {
    return ope == '+' || ope == '*';
  }

  Value eval_bin_op_step(const Value& lhs, const Value& rhs, char ope,
                         std::shared_ptr<Environment> env) {
    if (lhs.type == Value::Tensor || rhs.type == Value::Tensor) {
      auto tensor_op = op_to_tensor_op(std::string_view(&ope, 1));
      if (!tensor_op) throw CulebraError("TypeError", "type error.");
      Dtype dt = (lhs.type == Value::Tensor) ? lhs.to_tensor().impl->dtype
                                             : rhs.to_tensor().impl->dtype;
      auto l = lift_to_tensor(lhs, dt);
      auto r = lift_to_tensor(rhs, dt);
      if (!l || !r) throw CulebraError("TypeError", "type error.");
      return Value(TensorValue(tensor_binop(*tensor_op, l, r)));
    }
    // Container operations on Set / Tuple use method form
    // (`.union`, `.intersect`, `.diff`, `.concat`) — `+ - | & ^` on
    // these container types throws below via the type-error branch.
    if (auto* special = arith_op_to_special(ope)) {
      if (auto r = try_special_binop(lhs, rhs, special, env)) return *r;
      if (op_reflects(ope)) {
        if (auto r = try_special_binop(rhs, lhs, special, env)) return *r;
      }
    }
    // `@` has no numeric meaning, so skip the numeric path entirely;
    // reaching this point means the LHS didn't supply `__matmul__`.
    if (ope == '@' || !lhs.is_numeric() || !rhs.is_numeric()) {
      throw CulebraError("TypeError", std::format(
          "type error: cannot apply '{}' to {} and {}",
          ope, lhs.type_name(), rhs.type_name()));
    }
    // Integer fast path: both Long.
    if (lhs.type == Value::Long && rhs.type == Value::Long) {
      return arith_op(lhs.get<long>(), rhs.get<long>(), ope);
    }
    // Mixed or both-Float path: promote to double.
    return arith_op(lhs.to_double_coerce(), rhs.to_double_coerce(), ope);
  }

  // Float divide-by-zero raises (matches Python), so the b == 0
  // check fires for both Long and Float.
  template <class T>
  static Value arith_op(T a, T b, char ope) {
    switch (ope) {
      case '+': return Value(a + b);
      case '-': return Value(a - b);
      case '*': return Value(a * b);
      case '/':
        if (b == T{0}) throw CulebraError("ZeroDivisionError", "divide by 0 error");
        return Value(a / b);
      case '%':
        if (b == T{0}) throw CulebraError("ZeroDivisionError", "divide by 0 error");
        if constexpr (std::is_floating_point_v<T>) return Value(std::fmod(a, b));
        else return Value(a % b);
    }
    throw std::logic_error("invalid arithmetic operator");
  }

  Value eval_bin_expression(const peg::Ast& ast,
                            std::shared_ptr<Environment> env) {
    auto ret = eval(*ast.nodes[0], env);
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto rhs = eval(*ast.nodes[i + 1], env);
      auto ope = eval(*ast.nodes[i], env).to_string()[0];
      ret = eval_bin_op_step(ret, rhs, ope, env);
    }
    return ret;
  }

  // `**` is right-associative (grammar makes POWER recurse on the RHS),
  // so we evaluate lhs and rhs then combine. Python semantics:
  //   Long ** non-negative Long  → Long
  //   Long ** negative Long      → Float
  //   any Float operand          → Float
  //   0 ** 0 = 1 (matches IEEE 754 and Python).
  Value compute_power(const Value& base, const Value& exp,
                      std::shared_ptr<Environment> env) {
    if (auto r = try_special_binop(base, exp, "__pow__", env)) return *r;
    if (!base.is_numeric() || !exp.is_numeric()) {
      throw CulebraError("TypeError", std::format(
          "type error: '**' requires numeric operands, got {} and {}",
          base.type_name(), exp.type_name()));
    }
    if (base.type == Value::Long && exp.type == Value::Long) {
      auto a = base.get<long>();
      auto b = exp.get<long>();
      if (b >= 0) return Value(ipow_nonneg(a, b));
      // Negative integer exponent: promote to float so `2 ** -1 == 0.5`.
      if (a == 0) throw CulebraError("ZeroDivisionError", "divide by 0 error");
      return Value(std::pow(static_cast<double>(a), static_cast<double>(b)));
    }
    auto x = base.to_double_coerce();
    auto y = exp.to_double_coerce();
    return Value(std::pow(x, y));
  }

  Value eval_power(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    auto base = eval(*ast.nodes[0], env);
    auto exp = eval(*ast.nodes[2], env);
    return compute_power(base, exp, env);
  }

  // `**` has its own integer fast path (compute_power); the other ops
  // share the eval_bin_op_step dispatch. `op` is the operator without
  // the trailing `=`.
  Value apply_compound_op(const Value& lhs, const Value& rhs,
                          std::string_view op,
                          std::shared_ptr<Environment> env) {
    if (op == "**") return compute_power(lhs, rhs, env);
    return eval_bin_op_step(lhs, rhs, op[0], env);
  }

  // In-place fast path for `t OP= rhs` when `t` is a Tensor — skips
  // the per-step buffer allocation that the lazy `t = t OP rhs` would
  // do. Returns false if `t` is not a Tensor or the in-place
  // preconditions fail (caller then falls back to apply_compound_op).
  bool try_tensor_inplace(Value& lhs, std::string_view op, const Value& rhs) {
    if (lhs.type != Value::Tensor) return false;
    auto tensor_op = op_to_tensor_op(op);
    if (!tensor_op) return false;
    auto& dst = *lhs.to_tensor().impl;
    auto rhs_tensor = lift_to_tensor(rhs, dst.dtype);
    if (!rhs_tensor) return false;
    return tensor_inplace_binop(dst, *tensor_op, std::move(rhs_tensor));
  }

  bool is_keyword(std::string_view ident) const {
    using namespace std::literals;
    static std::set<std::string_view> keywords = {
        "nil"sv,    "true"sv,     "false"sv,  "mut"sv,      "debugger"sv,
        "return"sv, "while"sv,    "for"sv,    "in"sv,       "if"sv,
        "else"sv,   "fn"sv,       "match"sv,  "throw"sv,    "try"sv,
        "catch"sv,  "break"sv,    "continue"sv, "defer"sv};
    return keywords.contains(ident);
  }

  // DESTRUCTURE_ASSIGN ast children: [MUTABLE, (OBJECT_PATTERN|ARRAY_PATTERN), EXPRESSION]
  // `let` is suppressed; `let mut` yields MUTABLE.token == "mut".
  // Evaluates RHS once, then reuses `try_pattern` for binding. Pattern
  // mismatch is a runtime error (unlike match, where it's just "no").
  Value eval_destructure_assign(const peg::Ast& ast,
                                std::shared_ptr<Environment> env) {
    auto mut = ast.nodes[0]->token == "mut";
    const auto& pattern = *ast.nodes[1];
    auto rval = eval(*ast.nodes[2], env);
    if (!try_pattern(pattern, rval, env, mut)) {
      // Helper carries line:col so the structured error has the same
      // location the JIT path attaches — see shared.h.
      throw_destructure_mismatch_at(static_cast<long>(ast.line),
                                    static_cast<long>(ast.column));
    }
    return rval;
  }

  Value eval_assignment(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;
    auto lvaloff = 2;
    // ASSIGNMENT layout:
    //   [LET, MUTABLE, lval-chain..., (TYPE_ANNOTATION)?, ASSIGN_OP, EXPRESSION]
    // — the same shape is assumed by the JIT (collect_fn_locals,
    // visit_for_frees, compile_assignment) and by extract_type_annotation
    // (which reads the slot before ASSIGN_OP).
    auto lvalcnt = ast.nodes.size() - 4;

    auto type_name =
        extract_type_annotation(ast, ast.nodes.size() - 3);
    if (!type_name.empty()) lvalcnt--;

    auto let = ast.nodes[0]->token == "let";
    auto mut = ast.nodes[1]->token == "mut";
    auto op_tok = ast.nodes[ast.nodes.size() - 2]->token;
    bool compound = op_tok != "=";

    if (compound && (let || mut)) {
      throw CulebraError("SyntaxError",
          "compound assignment cannot declare a new variable.");
    }

    auto rval = eval(*ast.nodes.back(), env);

    if (!type_name.empty()) {
      check_type(rval, type_name, "assignment", ast.line, ast.column);
    }

    // For compound ops the binary operand is everything before the '='.
    auto base_op = compound
        ? op_tok.substr(0, op_tok.size() - 1)
        : std::string_view{};

    if (lvalcnt == 1) {
      const auto& ident = ast.nodes[lvaloff]->token;
      if (is_keyword(ident)) {
        throw CulebraError("SyntaxError",
                           "left-hand side is invalid variable name.");
      }
      if (compound) {
        if (!env->has(ident)) {
          throw CulebraError("NameError", std::format(
              "compound assignment on undefined name '{}'", ident));
        }
        auto cur = env->get(ident);
        // In-place fast path for Tensor LHS — see try_tensor_inplace.
        if (try_tensor_inplace(cur, base_op, rval)) {
          return cur;
        }
        auto new_val = apply_compound_op(cur, rval, base_op, env);
        env->assign(ident, new_val);
        return new_val;
      }
      auto declare = let || mut;
      if (declare) {
        env->initialize(ident, rval, mut);
      } else if (env->has(ident)) {
        env->assign(ident, rval);
      } else {
        env->initialize(ident, rval, mut);
      }
      return rval;
    } else {
      Value lval = eval(*ast.nodes[lvaloff], env);

      auto end = lvaloff + lvalcnt - 1;
      for (auto i = lvaloff + 1; i < end; i++) {
        const auto& postfix = *ast.nodes[i];

        switch (postfix.original_tag) {
          case "ARGUMENTS"_:
            lval = eval_function_call(postfix, env, lval);
            break;
          case "INDEX"_:
            lval = eval_array_reference(postfix, env, lval);
            break;
          case "DOT"_:
            lval = eval_property(postfix, env, lval);
            break;
          default:
            throw std::logic_error("invalid internal condition.");
        }
      }

      const auto& postfix = *ast.nodes[end];

      switch (postfix.original_tag) {
        case "INDEX"_: {
          // `obj[k] = v` on an Object. String keys flow into the same
          // slot as `obj.foo`; other hashable keys live in the sidecar.
          // Existing slots honor their `mut` flag (matches the DOT path
          // above and the JIT's `object_set` behavior).
          if (lval.type == Value::Object) {
            auto key = eval(postfix, env);
            auto& obj = lval.to_object();
            if (compound) {
              if (!obj.has(key)) {
                throw CulebraError("KeyError",
                    "compound assignment on missing key.");
              }
              auto cur = obj.get(key);
              auto new_val = apply_compound_op(cur, rval, base_op, env);
              obj.assign(key, new_val);
              return new_val;
            }
            if (obj.has(key)) {
              obj.assign(key, rval);
            } else {
              obj.initialize(key, rval, mut);
            }
            return rval;
          }
          const auto& arr = lval.to_array();
          auto idx = eval(postfix, env).to_long();
          if (idx < 0 || idx >= static_cast<long>(arr.values->size())) {
            throw CulebraError("IndexError", "index out of range.");
          }
          if (compound) {
            auto cur = arr.values->at(idx);
            if (try_tensor_inplace(cur, base_op, rval)) {
              return cur;
            }
            auto new_val = apply_compound_op(cur, rval, base_op, env);
            arr.values->at(idx) = new_val;
            return new_val;
          }
          arr.values->at(idx) = rval;
          return rval;
        }
        case "DOT"_: {
          auto& obj = lval.to_object();
          auto name = postfix.token;
          if (compound) {
            if (!obj.has(name)) {
              // Shared helper so the JIT path (see jit.h
              // `compile_assignment` DOT-compound branch) raises
              // the same AttributeError with location attached.
              throw_compound_missing_property_at(
                  static_cast<long>(postfix.line),
                  static_cast<long>(postfix.column));
            }
            auto cur = obj.get(name);
            if (try_tensor_inplace(cur, base_op, rval)) {
              return cur;
            }
            auto new_val = apply_compound_op(cur, rval, base_op, env);
            obj.assign(name, new_val);
            return new_val;
          }
          if (obj.has(name)) {
            obj.assign(name, rval);
          } else {
            obj.initialize(name, rval, mut);
          }
          return rval;
        }
        default:
          throw std::logic_error("invalid internal condition.");
      }
    }
  };

  Value eval_identifier(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return env->get(ast.token);
  };

  Value eval_object(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;
    ObjectValue obj;
    for (auto i = 0u; i < ast.nodes.size(); i++) {
      const auto& prop = *ast.nodes[i];
      auto mut = prop.nodes[0]->token == "mut";
      const auto& key_node = *prop.nodes[1];

      // Non-IDENTIFIER literal keys: store under Value-keyed sidecar.
      if (key_node.tag != "IDENTIFIER"_) {
        auto key = eval(key_node, env);
        auto val = eval(*prop.nodes[2], env);
        obj.initialize(key, std::move(val), mut);
        continue;
      }

      auto name = key_node.token;
      Value val;
      if (prop.nodes.size() < 3) {
        // Shorthand: name resolves in current scope.
        val = env->get(name);
      } else {
        val = eval(*prop.nodes[2], env);
      }
      obj.initialize(name, val, mut);
    }
    return Value(std::move(obj));
  }

  Value eval_array(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    ArrayValue arr;

    if (ast.nodes.size() >= 2) {
      auto count = eval(*ast.nodes[1], env).to_long();
      if (ast.nodes.size() == 3) {
        auto val = eval(*ast.nodes[2], env);
        arr.values->resize(count, std::move(val));
      } else {
        arr.values->resize(count);
      }
    }

    const auto& nodes = ast.nodes[0]->nodes;
    for (auto i = 0u; i < nodes.size(); i++) {
      auto expr = nodes[i];
      auto val = eval(*expr, env);
      if (i < arr.values->size()) {
        arr.values->at(i) = std::move(val);
      } else {
        arr.values->push_back(std::move(val));
      }
    }

    return Value(std::move(arr));
  }

  Value eval_nil(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return Value();
  };

  Value eval_bool(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return Value(ast.token == "true");
  };

  Value eval_number(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return Value(ast.token_to_number<long>());
  };

  Value eval_float(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return Value(ast.token_to_number<double>());
  }

  Value eval_interpolated_string(const peg::Ast& ast,
                                 std::shared_ptr<Environment> env) {
    using namespace peg::udl;
    std::string s;
    for (auto node : ast.nodes) {
      if (node->tag == "INTERPOLATED_CONTENT"_) {
        s += decode_interpolated_content(node->token);
      } else {
        const auto& val = eval(*node, env);
        s += str_display_with_special(val);
      }
    }
    return Value(std::move(s));
  };

  void eval_return(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    if (ast.nodes.empty()) {
      throw ReturnValue{Value()};
    } else {
      throw ReturnValue{eval(*ast.nodes[0], env)};
    }
  }

  void eval_throw(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    throw eval(*ast.nodes[0], env);
  }

  // Run deferred callables registered in `env` in LIFO order. If one
  // throws, it propagates; remaining defers for that scope are abandoned
  // (matches Swift; Go would run all, but we keep it simple).
  void run_deferred(std::shared_ptr<Environment> env) {
    while (!env->deferred.empty()) {
      auto fn = std::move(env->deferred.back());
      env->deferred.pop_back();
      fn();
    }
  }

  // TRY = [BLOCK, IDENTIFIER, BLOCK]  (try-body, catch-binding, catch-body)
  Value eval_try(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    auto tryEnv = make_scope(env);
    Value tryResult;
    bool threw = false;
    Value thrown;
    try {
      tryResult = eval(*ast.nodes[0], tryEnv);
    } catch (const Value& e) {
      threw = true;
      thrown = e;
    } catch (const CulebraError& e) {
      threw = true;
      thrown = make_error_object(e.kind, e.what(), e.line, e.col);
    } catch (const std::runtime_error& e) {
      // Unconverted internal error site — surface as a generic
      // RuntimeError so user `catch` blocks can still introspect it.
      threw = true;
      thrown = make_error_object("RuntimeError", e.what(), 0, 0);
    } catch (...) {
      run_deferred(tryEnv);
      throw;
    }
    run_deferred(tryEnv);
    if (!threw) return tryResult;

    auto catchEnv = make_scope(env);
    // The catch binding introduces a new name: apply the same shadow
    // check as parameters and pattern bindings.
    bind_pattern_name(catchEnv, *ast.nodes[1], thrown);
    Value catchResult;
    try {
      catchResult = eval(*ast.nodes[2], catchEnv);
    } catch (...) {
      run_deferred(catchEnv);
      throw;
    }
    run_deferred(catchEnv);
    return catchResult;
  }

  void eval_defer(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    // Capture the body AST (shared_ptr keeps subtree alive) and `env`
    // weakly so the scope can be destroyed without a cycle.
    auto body = ast.nodes[0];
    std::weak_ptr<Environment> wenv = env;
    auto self = shared_from_this();
    env->deferred.push_back([self = std::move(self), body, wenv]() {
      auto e = wenv.lock();
      if (!e) return;
      auto scopeEnv = make_scope(e);
      // A `return` inside a defer body exits only the defer closure,
      // not the enclosing function. This matches the JIT's semantics
      // (the defer body compiles to its own LLVM function whose `ret`
      // stays local).
      try {
        self->eval(*body, scopeEnv);
      } catch (const ReturnValue&) {}
    });
  }

  Debugger debugger_;
};

inline bool interpret(const std::shared_ptr<peg::Ast>& ast,
                      std::shared_ptr<Environment> env, Value& val,
                      std::vector<std::string>& msgs,
                      Debugger debugger = nullptr) {
  auto flush_top_defers = [&] {
    // Best-effort: swallow exceptions thrown by top-level defers so
    // that all registered defers get a chance to run.
    while (!env->deferred.empty()) {
      auto fn = std::move(env->deferred.back());
      env->deferred.pop_back();
      try { fn(); } catch (...) {}
    }
  };
  try {
    check_shadow_static(*ast);
    // Held by shared_ptr so FunctionValues created during eval (which
    // capture `shared_from_this()`) can keep the Interpreter alive
    // past this call's stack scope — see comment on `Interpreter`.
    auto interp = std::make_shared<Interpreter>(debugger);
    val = interp->eval(*ast, env);
    flush_top_defers();
    return true;
  } catch (const ReturnValue& r) {
    // bare `return` at top level is unusual but harmless — use its value
    val = r.value;
    flush_top_defers();
    return true;
  } catch (const Value& e) {
    // uncaught user `throw` propagated to the top level
    flush_top_defers();
    msgs.push_back(std::format("uncaught: {}", e.str_display()));
  } catch (const CulebraError& e) {
    flush_top_defers();
    msgs.push_back(std::format("{}: {}", e.kind, e.what()));
  } catch (const std::runtime_error& e) {
    flush_top_defers();
    msgs.push_back(std::format("RuntimeError: {}", e.what()));
  }

  return false;
}

// --- Cycle collector implementation ---
// (Defined here so Value is complete.)

inline void InterpGC::collect() {
  if (running_) return;
  running_ = true;
  using ValVec = std::vector<Value>;

  // Prune expired entries.
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [](auto& e) { return e.weak.expired(); }),
                 entries_.end());

  // Lock live shared_ptrs.
  struct Live {
    void* ptr;
    std::shared_ptr<void> sp;
  };
  std::vector<Live> live;
  live.reserve(entries_.size());
  for (auto& e : entries_) {
    if (auto sp = e.weak.lock()) live.push_back({e.ptr, std::move(sp)});
  }

  // gc_refs: use_count - 1 (our local copy in `live`).
  std::unordered_map<void*, long> gc_refs;
  gc_refs.reserve(live.size());
  for (auto& l : live) gc_refs[l.ptr] = l.sp.use_count() - 1;

  // For each Value held in a tracked ValVec, find the ValVec(s)
  // reachable in one step and invoke `fn(child_ptr)`. Object property
  // maps are untracked but we descend through them once to surface the
  // tracked Arrays they contain. `auto&& fn` keeps the visitor
  // monomorphic per call site, so the compiler inlines instead of
  // emitting a thunk.
  auto walk_array_children = [](ValVec* v, auto&& fn) {
    for (auto& val : *v) {
      if (val.type == Value::Array) {
        fn(val.template get<ArrayValue>().values.get());
      } else if (val.type == Value::Object) {
        for (auto& [_, sym] : *val.template get<ObjectValue>().properties) {
          if (sym.val.type == Value::Array) {
            fn(sym.val.template get<ArrayValue>().values.get());
          }
        }
      }
    }
  };

  // Subtract internal references.
  for (auto& l : live) {
    walk_array_children(static_cast<ValVec*>(l.ptr), [&](void* p) {
      auto it = gc_refs.find(p);
      if (it != gc_refs.end()) --it->second;
    });
  }

  // Mark external roots and BFS-propagate reachability.
  std::unordered_set<void*> reachable;
  std::queue<void*> q;
  for (auto& [ptr, r] : gc_refs) {
    if (r > 0) {
      reachable.insert(ptr);
      q.push(ptr);
    }
  }
  auto mark = [&](void* c) {
    if (reachable.insert(c).second && gc_refs.contains(c)) q.push(c);
  };
  while (!q.empty()) {
    auto* v = static_cast<ValVec*>(q.front());
    q.pop();
    walk_array_children(v, mark);
  }

  // Break cycles by clearing unreachable ValVecs. shared_ptr cascade
  // does the rest, including invoking each freed PropMap's custom
  // deleter (which fires `drop` on its way out).
  for (auto& l : live) {
    if (!reachable.contains(l.ptr)) {
      static_cast<ValVec*>(l.ptr)->clear();
    }
  }

  // Re-arm the next-collect threshold to twice the surviving live set
  // (floored at GC_MIN_THRESHOLD). Without this, a fixed threshold makes
  // collect fire O(N) times per step on workloads that retain a large
  // live set, turning total GC work into O(N^2).
  threshold_ = std::max(GC_MIN_THRESHOLD, reachable.size() * 2);

  running_ = false;
}

// RAII drop: invoked by the PropMap shared_ptr custom deleter (and by
// InterpGC for cycle members). Looks up a `drop` Function property and
// calls it with `this` bound to a non-owning view of the same map.
// The map is always left untouched by this helper itself — the caller
// (deleter or cycle-collector) performs the actual clear/delete.
//
// Re-entry: an empty or drop-less map short-circuits immediately, so
// the temporary ObjectValue constructed below does not recurse.
//
// Exceptions in drop are logged to stderr and swallowed to preserve
// the rest of the cleanup cascade (matching Python / Swift).
//
// Resurrection warning: storing `this` somewhere outside drop leaves
// a dangling reference once the caller completes the teardown. Do
// not resurrect `this` in drop bodies.
inline void _destroy_prop_map(OrderedSymbolMap* m) {
  if (!m) return;
  _call_drop_if_present(m);
  delete m;
}

inline void _call_drop_if_present(OrderedSymbolMap* m) {
  if (!m) return;
  auto it = m->find("drop");
  if (it == m->end()) return;
  if (it->second.val.type != Value::Function) return;

  const auto& fn = it->second.val.template get<FunctionValue>();
  if (!fn.params->empty()) return;

  ObjectValue this_view(ObjectValue::Synthetic{});
  this_view.properties =
      std::shared_ptr<OrderedSymbolMap>(m, [](OrderedSymbolMap*) {});

  try {
    fn.eval(_make_method_call_env(Value(std::move(this_view)), 0, 0));
  } catch (const std::exception& e) {
    std::cerr << "drop: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "drop: unknown error" << std::endl;
  }
}

}  // namespace culebra
