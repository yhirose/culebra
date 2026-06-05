#pragma once

#include <generator_transform.h>
#include <module_loader.h>
#include <packable.h>
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
struct Environment;
inline void _call_drop_if_present(OrderedSymbolMap* m);
inline void _destroy_prop_map(OrderedSymbolMap* m);
// Used by ValueHash / ValueEq's Object case to invoke user-defined
// `hash()` / `eq()` methods (Hashable + Eq structural conformance).
// Defined further down once Value / Environment / FunctionValue are
// complete.
inline Value _invoke_method_no_args(const Value& receiver,
                                    std::string_view method_name);
inline bool _invoke_user_eq(const Value& a, const Value& b);

// Forward decls for method tables defined once FunctionValue and Value
// are complete. `string_builtins()` is the primitive String's method
// table. `iterator_builtins()` holds the lazy Iterator method chain
// (map / filter / take / ... / collect); any Object that has both
// `iter` and `next` properties picks these up via duck-typed fallback
// in eval_property.
inline std::unordered_map<std::string_view, Value>& string_builtins();
inline std::unordered_map<std::string_view, Value>& iterator_builtins();

// The static shadow analyzer now lives in lint.h (`lint::check_shadow`),
// the shared home for pre-eval static checks. The interpreter invokes it
// before evaluation; the JIT performs the equivalent check inline during
// compilation (jit.h `collect_fn_locals` / `visit_for_frees`).

// `_` is the non-binding sink in patterns and parameters.
inline bool is_sink_name(std::string_view s) { return s == "_"; }

// --- Multimethod dispatch (shared between interp and JIT) ---

// True for the reserved type-tag names value_dyn_type returns for
// non-Object values. Class instances surface their class name here,
// so a name that isn't on this list is always a class name.
inline bool is_primitive_type_label(std::string_view n) {
  return n == "Nil" || n == "Bool" || n == "Long" || n == "Float" ||
         n == "String" || n == "StringView" || n == "Array" ||
         n == "Function" || n == "Tensor" || n == "Tuple" || n == "Set";
}

// Specificity score for a (param_type, arg_type) pair. Higher = more
// specific. -1 means no match.
//
// Ordering: Any (0) < Object catch-all (1) < Union exact (2) <
// concrete exact (3) < Generic full match (4). A concrete `Long`
// param therefore wins over `Long | Float`; `Array<Long>` wins over
// bare `Array` when both happen to match.
//
// `Object` param is a catch-all for class instances ONLY (matches
// type_matches: primitives stay -1 so pick doesn't claim a match
// that the post-pick check_type would reject).
//
// Generic param (`Array<Long>`) matches an arg whose type label
// equals the outer name (`Array`) — element info isn't on the arg
// side in the MVP, so it tie-breaks only against bare `Array`.
inline int multifn_specificity(std::string_view param_type,
                                std::string_view arg_type) {
  // Tiers (×2 from the pre-trait era so trait can slot strictly
  // between Object and Union):
  //   0  Any
  //   2  Object catch-all (class instance via `Object` param)
  //   3  trait conformance
  //   4  Union exact (downgrade from any concrete alt inside)
  //   6  concrete exact / Generic outer-only / bare dict via Object param
  //   8  Generic full match (param has args and outer matches concrete)
  if (param_type.empty() || param_type == "Any") return 0;
  // Union branch must use the depth-aware top-level check — a bare
  // `find('|')` on `Array<Long | Float>` triggers the branch but
  // split_union_types yields one candidate, causing self-recursion.
  if (has_toplevel_pipe(param_type)) {
    int best = -1;
    for (auto cand : split_union_types(param_type)) {
      int s = multifn_specificity(cand, arg_type);
      if (s > best) best = s;
    }
    if (best < 0) return -1;
    // A concrete alt inside a Union scored 6; downgrade to 4 so a
    // bare concrete param outranks it. Lower-tier alts pass through.
    return best >= 6 ? 4 : best;
  }
  // Composite bound (`A + B`): all-of. The arg must satisfy every
  // part; score is the best part's tier (parts are traits → 3).
  if (has_toplevel_plus(param_type)) {
    int best = -1;
    for (auto part : split_intersection_types(param_type)) {
      int s = multifn_specificity(part, arg_type);
      if (s < 0) return -1;
      if (s > best) best = s;
    }
    return best;
  }
  // `T?` Optional sugar = `T | Nil`: score like a two-alt Union. A Nil
  // arg matches at the Union tier; a non-nil arg scores its base type,
  // downgraded (concrete-in-Union -> 4) so a bare `T` param outranks it.
  if (!param_type.empty() && param_type.back() == '?') {
    auto base = param_type.substr(0, param_type.size() - 1);
    if (arg_type == "Nil") return 4;
    int s = multifn_specificity(base, arg_type);
    if (s < 0) return -1;
    return s >= 6 ? 4 : s;
  }
  // Generic: outer-match against arg label (arg side carries no
  // type args today, so the args part only tie-breaks against bare
  // outer-only annotations).
  if (param_type.find('<') != std::string_view::npos) {
    auto outer = parse_generic_head(param_type).outer;
    int base = multifn_specificity(outer, arg_type);
    if (base < 0) return -1;
    return base == 6 ? 8 : base;
  }
  if (param_type == "Object") {
    if (arg_type == "Object") return 6;        // bare dict — exact
    if (is_primitive_type_label(arg_type)) return -1;
    return 2;                                  // class instance catch
  }
  if (param_type == arg_type) return 6;
  // A callable class instance satisfies `Function` (Option A: structural
  // callable). The arg side carries only a type label here, so score any
  // non-primitive (class instance / bare Object) at the Object-catch tier
  // and let the value-aware post-pick check_type confirm it actually has
  // `__call__` — a non-callable is rejected there, and an exact class
  // overload (6) still outranks this catch.
  if (param_type == "Function" && !is_primitive_type_label(arg_type)) {
    return 2;
  }
  // Trait conformance: a registered trait scores below concrete and
  // below Union exact, but above the bare-Object catch.
  if (auto* trait = lookup_trait(param_type)) {
    if (arg_type == "Object") {
      // Bare Object literal — `ObjectValue::builtins()` provides
      // default methods (`iter`, etc.) so the built-in table is the
      // authority on which traits the literal conforms to.
      return builtin_conforms_to_trait(arg_type, param_type) ? 3 : -1;
    }
    // Primitive arg: consult the hard-coded built-in conformance
    // table directly (no instance methods to walk).
    if (is_primitive_type_label(arg_type)) {
      return builtin_conforms_to_trait(arg_type, param_type) ? 3 : -1;
    }
    // Read-only path: `multifn_specificity` only reads the cache and
    // returns -1 on miss. Use `.find()` (not `operator[]`) so a miss
    // doesn't materialize an empty by_trait entry, and take a shared
    // lock so dispatchers don't serialize on this hot path.
    std::shared_lock lock(trait_mutex());
    auto& cache = trait_conformance_cache();
    auto outer = cache.find(std::string(arg_type));
    if (outer == cache.end()) return -1;
    auto it = outer->second.find(std::string(param_type));
    if (it != outer->second.end()) return it->second ? 3 : -1;
    return -1;
  }
  return -1;
}

// Pick the most specific matching entry. Returns:
//   idx >= 0  : matching entry index
//   -1        : no match
//   -2        : ambiguous (tie at the top)
// `params_of(entry)` must return a container of (regular) param-type
// strings; `is_variadic_of(entry)` reports whether the entry has a
// `*args` catch-all. A variadic entry matches any arity >= its regular
// count; the surplus positions score as Any (0), and on an otherwise
// exact tie a non-variadic (fixed-arity) entry wins.
template <class Entry, class ParamsOf, class IsVariadic>
inline int64_t multifn_pick(const std::vector<Entry>& methods,
                             const std::vector<std::string_view>& arg_types,
                             ParamsOf params_of, IsVariadic is_variadic_of) {
  std::vector<int> score(arg_types.size());
  std::vector<int> best_score(arg_types.size());
  size_t best_idx = 0;
  bool have_best = false;
  bool best_variadic = false;
  size_t best_regular = 0;
  bool ambiguous = false;
  for (size_t i = 0; i < methods.size(); i++) {
    const auto& params = params_of(methods[i]);
    bool variadic = is_variadic_of(methods[i]);
    if (variadic ? arg_types.size() < params.size()
                 : params.size() != arg_types.size()) {
      continue;
    }
    bool ok = true;
    for (size_t p = 0; p < arg_types.size(); p++) {
      // Positions beyond the regular params are absorbed by `*args` and
      // score as Any (0) so concrete fixed-arity matches outrank them.
      int s = p < params.size()
                  ? multifn_specificity(params[p], arg_types[p])
                  : 0;
      if (s < 0) { ok = false; break; }
      score[p] = s;
    }
    if (!ok) continue;
    if (!have_best) {
      best_idx = i;
      best_score = score;
      best_variadic = variadic;
      best_regular = params.size();
      have_best = true;
      continue;
    }
    bool better_any = false, worse_any = false;
    for (size_t p = 0; p < arg_types.size(); p++) {
      if (score[p] > best_score[p]) better_any = true;
      if (score[p] < best_score[p]) worse_any = true;
    }
    auto take = [&] {
      best_idx = i;
      best_score = score;
      best_variadic = variadic;
      best_regular = params.size();
      ambiguous = false;
    };
    if (better_any && !worse_any) {
      take();
    } else if (!better_any && !worse_any) {
      // Equal type-specificity. Break the tie by, in order: a fixed-arity
      // entry beats a variadic one (`*args` is the fallback); among two
      // variadic entries, the one with more regular params is more
      // specific. Anything still even is genuinely ambiguous.
      if (best_variadic && !variadic) {
        take();
      } else if (best_variadic && variadic) {
        if (params.size() > best_regular) take();
        else if (params.size() == best_regular) ambiguous = true;
      } else if (!best_variadic && !variadic) {
        ambiguous = true;
      }
    }
  }
  if (!have_best) return -1;
  if (ambiguous)  return -2;
  return static_cast<int64_t>(best_idx);
}

// Walk an AST subtree and throw SyntaxError if a CLASS_DECL appears
// in the current scope. Stops at FUNCTION / LAMBDA / LEXICAL_SCOPE
// boundaries because those re-open a fresh scope (class declarations
// inside a fn body or `{ ... }` block are allowed — only direct class
// body children are restricted).
//
// Called from eval_class_decl / compile_class_decl on each METHOD
// body and the constructor body to enforce "class declarations live
// at top level or inside fn / lambda / lexical scope, not directly
// inside another class".
inline void reject_class_decl_in_class_body(
    const peg::Ast& node, std::string_view outer_class) {
  using namespace peg::udl;
  if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_ ||
      node.tag == "LEXICAL_SCOPE"_) {
    return;
  }
  if (node.tag == "CLASS_DECL"_) {
    size_t k = culebra::first_non_decorator_index(node);
    auto inner_name = node.nodes[k]->token;
    throw CulebraError(
        "SyntaxError",
        std::format(
            "class `{}` cannot be declared inside class `{}` — declare "
            "classes at top level (or inside a fn / lambda / lexical "
            "scope), not directly in another class body",
            inner_name, outer_class),
        static_cast<long>(node.line),
        static_cast<long>(node.column));
  }
  for (const auto& c : node.nodes) {
    reject_class_decl_in_class_body(*c, outer_class);
  }
}

// Cycle collector for ObjectValue/ArrayValue (shared_ptr-based).
// Python-style mark-and-sweep; runs periodically and on program exit.
// Uses weak_ptr<void> so incomplete types (Symbol, Value) don't matter here.
// Process-wide singleton; NOT thread-safe (see THREAD SAFETY note at
// the top of the runtime section in jit.h).
//
// Tracked nodes are ArrayValue `values` vectors and closure-captured
// Environments (the two container kinds that form cycles RC can't break).
// Cycles routed entirely through Object property maps with no Array or
// Environment in between are not detected — microgpt's route Object → Array
// (`_children`) → Object and closures route Function → Environment, so both
// are broken correctly; direct Object→Object cycles leak (the language spec
// calls cycles a hazard and recommends `drop` for explicit teardown).
struct InterpGC {
  struct Entry {
    std::weak_ptr<void> weak;
    void* ptr;
    bool is_env;  // false = a ValVec (Array backing); true = an Environment
  };

  static InterpGC& instance() { return runtime_substate<InterpGC>(kSlotInterpGc); }

  ~InterpGC() { collect(); }

  // Live heap-object accounting for GC.stat() introspection. Counts the
  // OrderedSymbolMap backing each Object / Array / class-instance: bumped in
  // ObjectValue's ctor, dropped in _destroy_prop_map (the prop-map deleter),
  // so it's a per-logical-object count immune to Value handle copies.
  // `live_bytes` weights each by sizeof(OrderedSymbolMap) — a structural
  // approximation that excludes element buffers and allocator overhead.
  int64_t live_objects = 0;
  int64_t live_bytes = 0;

  template <typename T>
  void track_vec(std::shared_ptr<T> p) {
    push_entry(std::shared_ptr<void>(p, p.get()), /*is_env=*/false);
  }

  // Register a node (a type-erased aliasing shared_ptr) and trigger a
  // threshold collect. Shared by track_vec and track_env.
  void push_entry(std::shared_ptr<void> sp, bool is_env) {
    entries_.push_back({std::weak_ptr<void>(sp), sp.get(), is_env});
    bump();
  }

  // Register a captured Environment as a collectable cycle node (a closure's
  // def_env can form a cycle RC alone can't break). Deduped via e->gc_tracked.
  // Defined out-of-line below, where Environment is a complete type.
  void track_env(const std::shared_ptr<Environment>& e);

  void collect();

 private:
  std::vector<Entry> entries_;
  size_t alloc_counter_ = 0;
  static constexpr size_t GC_MIN_THRESHOLD = 10000;
  size_t threshold_ = GC_MIN_THRESHOLD;  // adaptive; see collect().
  bool running_ = false;

  // CULEBRA_GC_STRESS=1 collects on every tracked allocation, surfacing any
  // over-collection (a live node wrongly broken) immediately under the suite.
  static bool stress() {
    static const bool s = std::getenv("CULEBRA_GC_STRESS") != nullptr;
    return s;
  }

  void bump() {
    if (running_) return;
    if (stress()) {
      collect();
      return;
    }
    if (++alloc_counter_ >= threshold_) {
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
    std::string_view type_name;  // empty = no annotation; this is the
                                  // effective annotation runtime checks
                                  // use — may be rewritten (e.g. class
                                  // type-params → "Any").
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
    // Positional catch-all (`*args`): binds the overflow positional
    // arguments (beyond the regular params) as an Array. Mutually
    // exclusive with kwargs_rest on the same param.
    bool args_rest = false;
    // Destructuring parameter (`fn ({a, b})`): the pattern AST. The
    // param binds the arg to a synthetic name; the function body's entry
    // unpacks it via this pattern. nullptr for normal params.
    const peg::Ast* pattern = nullptr;
    // Declared annotation as written in source — preserved for
    // introspection (`fn.params[i].type`) when `type_name` has been
    // neutralized. Empty = falls back to `type_name` (= raw or
    // un-rewritten).
    std::string_view declared_type_name;

    // Convenience constructor for a synthetic `**rest` catch-all
    // parameter (used by the multifn dispatcher to forward unknown
    // kwargs into the picked method).
    static Parameter make_kwargs_rest(std::string_view name) {
      return {name, /*mut=*/false, /*type_name=*/{},
              /*default_expr=*/nullptr, /*default_value=*/{},
              /*kw_only=*/false, /*kwargs_rest=*/true};
    }

    // Convenience constructor for a `*args` positional catch-all.
    static Parameter make_args_rest(std::string_view name) {
      Parameter p{name, /*mut=*/false, /*type_name=*/{},
                  /*default_expr=*/nullptr, /*default_value=*/{},
                  /*kw_only=*/false, /*kwargs_rest=*/false};
      p.args_rest = true;
      return p;
    }
  };

  FunctionValue(
      const std::vector<Parameter>& params,
      const std::function<Value(const std::shared_ptr<Environment>& env)>& eval,
      std::string_view return_type = {},
      std::shared_ptr<Environment> def_env = {})
      : params(std::make_shared<std::vector<Parameter>>(params)),
        eval(eval),
        return_type(return_type),
        def_env(std::move(def_env)) {}

  std::shared_ptr<std::vector<Parameter>> params;
  std::function<Value(const std::shared_ptr<Environment>& env)> eval;
  std::string_view return_type;  // empty = no annotation
  // Definition environment — used only to evaluate parameter defaults
  // (for body execution, `eval` closes over this env itself).
  std::shared_ptr<Environment> def_env;
  // Evaluates a parameter's `default_expr` (an AST node) against a scope,
  // using the defining Interpreter. Set by `make_function_value` ONLY when
  // the function has an expr-default param (so the common no-default closure
  // stays a null std::function = no allocation). It is the single bridge that
  // lets the free-function callback binder (`bind_callback_params`, defined
  // before the Interpreter class) evaluate user defaults, so a HOF callback
  // binds defaults exactly like a direct call. Null for C++/stdlib functions
  // (they carry literal `default_value`s instead) and rebuilt by the isolate
  // layer since it runs through `make_function_value`.
  std::function<Value(const peg::Ast&, const std::shared_ptr<Environment>&)>
      eval_default_expr;
  // True when `eval` ALSO closes over `def_env` (the make_function_value
  // path), so this FunctionValue holds TWO shared_ptr refs to `def_env`
  // (the field + the eval capture). The cycle collector must subtract both
  // when computing gc_refs for `def_env`, else a closure→env→closure cycle
  // looks externally rooted and never gets reclaimed. Stdlib/C++ functions
  // leave this false (their `eval` captures no Environment; def_env is null).
  bool eval_captures_def_env = false;
  // Declared name for introspection (`fn.name`). Empty for anonymous
  // expressions (lambdas, `fn (x) { ... }`). Set by the caller after
  // construction so stdlib FunctionValues stay one-liners. Owned
  // std::string because the FunctionValue (e.g. inside a long-lived
  // closure registered with the test runner or REPL session) may
  // outlive the AST source backing the original token.
  std::string name;
  // Multifn dispatcher view-of-source: when set, `fn.params` and
  // `fn.return_type` look through to this snapshot of the first
  // registered method body. The dispatcher itself keeps a synthetic
  // `__KWARGS__` param list for the dispatch protocol — without this
  // redirect, introspection would expose that internal detail.
  std::shared_ptr<FunctionValue> introspection_target;
  // Multimethod accumulation table for a `fn name(...)` dispatcher: the
  // shared method vector its `eval` closure dispatches over. Carried on the
  // binding itself (not a global by-name registry) so a same-scope overload
  // appends to THIS table while a nested or redeclared `fn name` in another
  // scope or activation gets its own — matching lexical scoping and the JIT.
  // Opaque `shared_ptr<void>` to dodge the order cycle (MultiMethod is
  // defined later, inside Interpreter); eval_multifn_decl casts it back.
  std::shared_ptr<void> multimethod_table;
  // True for the wrapper produced by _wrap_method_with_this around a
  // built-in method (Array/String/Set/...). Built-in methods parse their
  // args positionally and never accept kwargs; the call site uses this to
  // raise a clean TypeError instead of an opaque ArityError. Matches JIT.
  bool is_builtin_method = false;
  // True when this wrapper is over a genuine builtin-table method
  // (value-type / dict / iterator) — the cases the JIT can match. Gates
  // the positional-arity check so interp and JIT stay symmetric;
  // trait-default and namespace wrappers (not in any builtin table) are
  // excluded. See [[project_jit_error_symmetry]].
  bool builtin_arity_checked = false;
  // True for the root stdlib functions (namespace methods like `Math.abs`
  // and bare globals like `type_of`) that declare a fixed positional
  // signature: invoke_user_function_with_args then rejects the wrong
  // positional count with the same count-based ArityError the JIT raises.
  // Set by mark_strict_arity_builtins after setup, so synthesized native
  // closures (enum/class constructors, which look identical — body ==
  // nullptr, fixed params) are NOT swept in. Variadic natives (min/max,
  // range) are skipped at marking time.
  bool strict_arity = false;
  // Defining AST for a user closure (fn / lambda / method), retained so the
  // closure can be REBUILT on another thread's heap by the isolate layer
  // (sendable.h): the eval std::function captures the *parent* Interpreter
  // and heap and cannot run as-is on a child thread, but the AST is
  // immutable + process-lifetime so a child Interpreter can re-create the
  // closure from these. `body == nullptr` marks a native/stdlib FunctionValue
  // (not rebuildable → not Sendable as a closure). Set by make_function_value.
  const peg::Ast* params_ast = nullptr;  // borrowed (process-lifetime AST)
  std::shared_ptr<peg::Ast> body;        // shared; control block is atomic
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
  // Own-field existence, excluding builtin methods. `has()` returns true
  // for builtin method names (`size`/`keys`/...) even when the instance
  // carries no such field, which is what property *reads* want (the
  // builtin is the fallback). Assignment-target checks must use this
  // instead, or `this.size = v` on a fresh field routes to `assign()`
  // (which expects an existing slot) instead of `initialize()`.
  bool has_own(std::string_view name) const;
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

  virtual std::unordered_map<std::string_view, Value>& builtins();

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
  std::unordered_map<std::string_view, Value>& builtins() override;

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
inline bool _array_eq(const Value& a, const Value& b);
inline bool _object_eq(const Value& a, const Value& b);

// Builtin Tensor type. The data buffer lives in a shared_ptr<TensorImpl>
// (see tensor.h); cycles are impossible because the buffer holds opaque
// bytes, so no InterpGC tracking is needed.
struct TensorValue : public ObjectValue {
  explicit TensorValue(TensorPtr i) : ObjectValue(), impl(std::move(i)) {}
  std::unordered_map<std::string_view, Value>& builtins() override;

  TensorPtr impl;
};

// Shared-ownership descriptor carried by every Value::StringView. The
// `source` shared_ptr keeps the owning std::string alive while any view
// referencing it survives; `view` is a borrowed window into `*source`.
// Multiple views can share the same source — chained substr / split /
// iter pay only one source-copy alloc total, then no further alloc for
// per-element views.
struct StringViewPayload {
  std::shared_ptr<const std::string> source;
  std::string_view view;
};

// Normalize a slice range against a sequence length: resolve negative
// indices from the end, apply the inclusive end (`..=`), then clamp both
// ends to [0, len] and force an empty window when start > end. Returns a
// half-open [start, end). Shared by interp + JIT slicing so the two
// backends stay symmetric.
inline std::pair<size_t, size_t> _slice_bounds(long lo, long hi,
                                               bool inclusive, size_t len) {
  long n = static_cast<long>(len);
  if (lo < 0) lo += n;
  if (hi < 0) hi += n;
  // `..=` includes the end. Skip the increment at LONG_MAX so a huge
  // endpoint (`xs[0..=<LONG_MAX>]`) doesn't signed-overflow; it clamps to
  // `n` below either way, so the result is unchanged for every other input.
  if (inclusive && hi != std::numeric_limits<long>::max()) hi += 1;
  if (lo < 0) lo = 0;
  if (lo > n) lo = n;
  if (hi < 0) hi = 0;
  if (hi > n) hi = n;
  if (hi < lo) hi = lo;
  return {static_cast<size_t>(lo), static_cast<size_t>(hi)};
}

struct Value {
  enum Type { Nil, Bool, Long, Float, String, Object, Array, Function, Tensor, Tuple, Set, StringView };

  Value() : type(Nil) {}
  Value(const Value& rhs) : type(rhs.type), v(rhs.v) {}
  // Move must std::move the std::any payload — without it, "moving" a
  // Value deep-copies the boxed String/Array/Function (heap alloc +
  // shared_ptr bumps) on every hot-path move (arg bind, return, swap).
  // Move must std::move the std::any payload — without it, "moving" a
  // Value deep-copies the boxed String/Array/Function (heap alloc +
  // shared_ptr bumps). A move that copies is a defect; measured ~13%
  // faster on a move-heavy build-array-of-strings loop.
  Value(Value&& rhs) noexcept : type(rhs.type), v(std::move(rhs.v)) {}

  Value& operator=(const Value& rhs) {
    if (this != &rhs) {
      type = rhs.type;
      v = rhs.v;
    }
    return *this;
  }

  Value& operator=(Value&& rhs) noexcept {
    type = rhs.type;
    v = std::move(rhs.v);
    return *this;
  }

  explicit Value(bool b) : type(Bool), v(b) {}
  explicit Value(long l) : type(Long), v(l) {}
  explicit Value(double d) : type(Float), v(d) {}
  explicit Value(std::string&& s) : type(String), v(std::move(s)) {}
  // StringView: borrowed bytes view with shared-ownership lifetime.
  // The `source` shared_ptr keeps the bytes alive — multiple views can
  // share the same source so chained substr / split / iter pay only one
  // source-copy alloc total. See [[project_string_model]].
  explicit Value(std::shared_ptr<const std::string> source,
                 std::string_view sv)
      : type(StringView),
        v(StringViewPayload{std::move(source), sv}) {}
  explicit Value(StringViewPayload p)
      : type(StringView), v(std::move(p)) {}
  explicit Value(ObjectValue&& o) : type(Object), v(std::move(o)) {}
  explicit Value(ArrayValue&& a) : type(Array), v(std::move(a)) {}
  explicit Value(TensorValue&& t) : type(Tensor), v(std::move(t)) {}
  explicit Value(FunctionValue&& f) : type(Function), v(f) {}
  explicit Value(TupleValue&& t) : type(Tuple), v(t) {}
  // Defined out-of-line so SetValue is complete here.
  explicit Value(SetValue&& s);

  bool is_numeric() const { return type == Long || type == Float; }
  bool is_stringlike() const { return type == String || type == StringView; }

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
      case StringView: return "StringView";
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
      case StringView:
        return std::string(get<StringViewPayload>().view);
      default:
        _throw_type_error("String");
    }
  }

  // Borrowed view over either flavor's bytes. Cheap for both:
  // String wraps its std::string; StringView returns the stored view.
  std::string_view to_string_view() const {
    switch (type) {
      case String:
        return get<std::string>();
      case StringView:
        return get<StringViewPayload>().view;
      default:
        _throw_type_error("String");
    }
  }

  // Shared source pointer for a string-flavor receiver.
  // - String: copy bytes once into a new shared_ptr<const string> so
  //   subsequent sub-views into the result are alloc-free.
  // - StringView: return the existing shared source — zero alloc.
  std::shared_ptr<const std::string> share_source() const {
    if (type == String) {
      return std::make_shared<const std::string>(get<std::string>());
    }
    if (type == StringView) {
      const auto& p = get<StringViewPayload>();
      return p.source ? p.source
                      : std::make_shared<const std::string>();
    }
    _throw_type_error("String");
  }

  // Convenience for split/slice/iter style methods: returns the shared
  // source and the visible byte window into it (the full source for
  // String, the existing view for StringView).
  std::pair<std::shared_ptr<const std::string>, std::string_view>
  share_source_and_view() const {
    auto src = share_source();
    std::string_view base = (type == String)
                                ? std::string_view(*src)
                                : get<StringViewPayload>().view;
    return {std::move(src), base};
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
      case StringView:
        return std::format("'{}'", to_string_view());
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
    if (type == StringView) return std::string(get<StringViewPayload>().view);
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
      // String-flavor cross-equality: String == StringView compares
      // bytes, so users can write `s.slice(0,3) == "abc"` without
      // worrying about which flavor came back.
      if ((type == String && rhs.type == StringView) ||
          (type == StringView && rhs.type == String)) {
        return to_string_view() == rhs.to_string_view();
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
      case StringView:
        return get<StringViewPayload>().view ==
               rhs.get<StringViewPayload>().view;
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
      // Structural (value) equality for collections — matches Tuple/Set
      // and Python/Ruby. The same-pointer short-circuit handles `a == a`
      // and breaks self-comparison without recursing. Arrays/Objects stay
      // *unhashable* (ValueHash still throws), so this doesn't make them
      // usable as keys. Spec §16.
      case Array:
        if (get<ArrayValue>().values.get() ==
            rhs.get<ArrayValue>().values.get())
          return true;
        return _array_eq(*this, rhs);
      case Object:
        if (get<ObjectValue>().properties.get() ==
            rhs.get<ObjectValue>().properties.get())
          return true;
        return _object_eq(*this, rhs);
      case Function:
        return get<FunctionValue>().params.get() ==
               rhs.get<FunctionValue>().params.get();
      case Tensor:
        return get<TensorValue>().impl.get() ==
               rhs.get<TensorValue>().impl.get();
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
      throw_compare_type_error(type_name(), rhs.type_name());
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
      case StringView: {
        auto c = get<StringViewPayload>().view.compare(
            rhs.get<StringViewPayload>().view);
        return cmp(double(c), 0.0);
      }
      // Collections (Array/Object/Tuple/Set) and Function/Tensor are not
      // ordered: == is structural (see value-equality), but <,<=,>,>= are a
      // clean TypeError — never an uncatchable internal abort. Same message
      // as the cross-type branch above; the eval-wrapper backfills location.
      default:
        throw_compare_type_error(type_name(), rhs.type_name());
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
// Unhashable inputs (Array / Function / Tensor, Object without `hash()`)
// throw. User class instances become hashable by defining `fn hash()`
// (returning Long) and `fn eq(other)` — the structural Hashable+Eq
// conformance pair.
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
        return std::hash<std::string_view>{}(
            std::string_view(v.get<std::string>()));
      case Value::StringView:
        return std::hash<std::string_view>{}(
            v.get<StringViewPayload>().view);
      case Value::Tuple: {
        size_t h = 0xa3b1c5d7e9f10000ULL;
        for (const auto& e : *v.get<TupleValue>().elements) {
          h ^= (*this)(e) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
      }
      case Value::Object: {
        const auto& obj = v.to_object();
        if (obj.has("hash")) {
          auto r = _invoke_method_no_args(v, "hash");
          if (r.type != Value::Long) {
            throw CulebraError("TypeError", "hash() must return Long");
          }
          return std::hash<long>{}(r.get<long>());
        }
        throw CulebraError("TypeError",
            "unhashable type: 'Object' (no hash() method)");
      }
      default:
        throw CulebraError("TypeError", std::format(
            "unhashable type: '{}'", v.type_name()));
    }
  }
};

// Equality for hash-keyed containers. Falls through to `Value::operator==`
// (numeric coercion + reference equality for heap types), but routes
// Object-vs-Object through a user-defined `eq(other)` method when both
// sides expose one — pairing with `ValueHash`'s `hash()` dispatch so a
// Hashable+Eq user class behaves correctly as an Object/Set/HashMap key.
// The Object-eq path is implemented in `_invoke_user_eq` (defined later
// once Environment / FunctionValue are complete).
struct ValueEq {
  bool operator()(const Value& a, const Value& b) const {
    if (a.type == Value::Object && b.type == Value::Object) {
      const auto& oa = a.to_object();
      const auto& ob = b.to_object();
      if (oa.has("eq") && ob.has("eq")) {
        return _invoke_user_eq(a, b);
      }
    }
    return a == b;
  }
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

// Element-wise array equality (recurses through Value::operator==).
inline bool _array_eq(const Value& a, const Value& b) {
  const auto& va = *a.get<ArrayValue>().values;
  const auto& vb = *b.get<ArrayValue>().values;
  if (va.size() != vb.size()) return false;
  for (size_t i = 0; i < va.size(); i++) {
    if (!(va[i] == vb[i])) return false;
  }
  return true;
}

// _object_eq is defined after OrderedSymbolMap is complete (below).

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
    ++mut_count_;
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
    ++mut_count_;
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
      ++mut_count_;
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
    ++mut_count_;
    return 1;
  }

  // Bumps on add/delete only; value overwrites do not (matches Python
  // dict semantics — `d[k] = v` mid-iter is allowed, `d[new] = v` and
  // `del d[k]` are not). Iterators snapshot this on init and compare
  // per step to fail-fast on structural mutation.
  size_t mut_count() const { return mut_count_; }

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
  size_t mut_count_ = 0;
};

// Structural object equality: same key set (String + non-String) with
// equal values, order-independent (like Python dict). Class-sugar
// instances compare their `class` tag and method values too — methods
// share a params shared_ptr across instances, so same-class instances
// match when their data fields do. Defined here so OrderedSymbolMap is
// complete.
inline bool _object_eq(const Value& a, const Value& b) {
  const auto& oa = a.to_object();
  const auto& ob = b.to_object();
  if (oa.properties->size() != ob.properties->size()) return false;
  for (const auto& [k, sym] : *oa.properties) {
    auto it = ob.properties->find(k);
    if (it == ob.properties->end() || !(sym.val == it->second.val)) {
      return false;
    }
  }
  size_t na = oa.non_string_props ? oa.non_string_props->size() : 0;
  size_t nb = ob.non_string_props ? ob.non_string_props->size() : 0;
  if (na != nb) return false;
  if (oa.non_string_props) {
    for (const auto& [k, sym] : *oa.non_string_props) {
      auto it = ob.non_string_props->find(k);
      if (it == ob.non_string_props->end() || !(sym.val == it->second.val)) {
        return false;
      }
    }
  }
  return true;
}

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
  // Live-object accounting (see InterpGC). Birth here, death in
  // _destroy_prop_map — both keyed on this OrderedSymbolMap, so a logical
  // Object/Array/class-instance is counted exactly once regardless of how
  // many Value handles copy the shared_ptr.
  auto& gc = interp_gc();
  gc.live_objects++;
  gc.live_bytes += static_cast<int64_t>(sizeof(OrderedSymbolMap));
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

struct Environment : std::enable_shared_from_this<Environment> {
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

  // Local-frame-only existence (does not walk `outer`). Used by
  // eval_multifn_decl to merge same-scope `fn name` overloads while a
  // nested or redeclared `fn name` shadows rather than mutates an outer one.
  bool has_own(std::string_view s) const {
    return dictionary.find(s) != dictionary.end();
  }

  // `_` is a non-binding sink: any binding form (`let _`, `for _ in`,
  // `fn(_, _, x)`, `match { _ => }`) drops the value instead of
  // introducing a name. Shadow checking happens statically via
  // `lint::check_shadow` (lint.h) before evaluation begins.

  const Value& get(std::string_view s) const {
    if (auto it = dictionary.find(s); it != dictionary.end()) {
      // Fast path: once all lazy modules are resolved, `lazy_pending`
      // is empty and the lookup is skipped entirely.
      if (!lazy_pending.empty()) {
        if (auto lit = lazy_pending.find(s); lit != lazy_pending.end()) {
          const_cast<Environment*>(this)->resolve_from_lazy(s, lit->second);
          it = dictionary.find(s);
        }
      }
      return it->second.val;
    } else if (outer) {
      return outer->get(s);
    }
    throw CulebraError("NameError",
                       std::format("undefined variable '{}'", s));
  }

  // Register `source` to be parsed + evaluated lazily on first
  // `get(name)`. The source must `let <name> = ...` so the eval pass
  // overwrites the placeholder binding in `dictionary`. Used for stdlib
  // modules that are expensive to parse but rarely touched in short
  // scripts — see [[project-startup-overhead]].
  void initialize_lazy(std::string_view name, std::string source) {
    initialize(name, Value{}, /*mut=*/false);
    if (is_sink_name(name)) return;
    lazy_pending.insert_or_assign(
        std::string(name),
        std::make_shared<std::string>(std::move(source)));
  }

  // Register multiple names sharing the same source. First `get(any_name)`
  // parses+evaluates the source once; the other names see their pending
  // entries cleared (the source's `let` statements bind every symbol in
  // one pass). Used for the matcher family — 10 globals from one source.
  void initialize_lazy_group(std::vector<std::string_view> names,
                              std::string source) {
    auto shared = std::make_shared<std::string>(std::move(source));
    for (auto name : names) {
      initialize(name, Value{}, /*mut=*/false);
      if (is_sink_name(name)) continue;
      lazy_pending.insert_or_assign(std::string(name), shared);
    }
  }

  // Defined below `interpret` since it parses + evaluates the source.
  void resolve_from_lazy(std::string_view name,
                         std::shared_ptr<std::string> source);

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
  // Set once this env has been registered with InterpGC as a collectable
  // node (it became some closure's def_env). Dedupes repeat track_env calls.
  bool gc_tracked = false;
  // Deferred callables registered in this scope via `defer { ... }`.
  // Fired in LIFO order when the scope exits (normally or via throw).
  std::vector<std::function<void()>> deferred;
  // Names not yet resolved, mapped to their source. The first `get` of
  // a key in this map triggers parse + eval and removes the entry. The
  // `get` fast path checks `empty()` first so once all modules are
  // resolved this costs nothing.
  std::map<std::string, std::shared_ptr<std::string>, std::less<>> lazy_pending;
  // Source buffers backing lazily-resolved stdlib modules. AST tokens
  // hold string_views into these; FunctionValue closures over methods
  // declared in the module survive long after `resolve_from_lazy`
  // returns, so the buffer must outlive any value bound through that
  // resolution. Anchored to the env that owns the lazy binding.
  std::vector<std::shared_ptr<std::string>> lazy_module_sources;
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

inline bool ObjectValue::has_own(std::string_view name) const {
  return properties->contains(name);
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
                       std::format("immutable property '{}'", name));
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
  // Union types (e.g. `Long | Float`) — any-of match. Recursively
  // check each candidate after trimming. Use depth-aware
  // has_toplevel_pipe: `Array<Long | Float>`'s `|` is nested and
  // must NOT enter this branch (split_union_types would return a
  // single candidate, causing infinite recursion).
  if (has_toplevel_pipe(name)) {
    for (auto candidate : split_union_types(name)) {
      if (type_matches(val, candidate)) return true;
    }
    return false;
  }
  // Composite bound (`A + B`) — all-of: conform to every part.
  if (has_toplevel_plus(name)) {
    for (auto part : split_intersection_types(name)) {
      if (!type_matches(val, part)) return false;
    }
    return true;
  }
  // `T?` Optional sugar = `T | Nil`: a trailing `?` accepts Nil, else
  // checks the base type. Unions split above, so this sees one name.
  if (!name.empty() && name.back() == '?') {
    if (val.type == Value::Nil) return true;
    return type_matches(val, name.substr(0, name.size() - 1));
  }
  // Generic outer-match: `Array<Long>` checks `Array` only (element
  // type is documentation in the MVP, see [[project_type_system]]).
  if (name.find('<') != std::string_view::npos) {
    name = parse_generic_head(name).outer;
  }
  // Built-in trait conformance: a primitive value (Long / String /
  // ...) can conform to one of the built-in traits (Stringer / Eq /
  // Comparable). Checked first so the per-case `return` paths below
  // don't reject before this matches.
  auto try_builtin_trait = [&](std::string_view label) {
    if (auto* t = lookup_trait(name)) {
      (void)t;
      return builtin_conforms_to_trait(label, name);
    }
    return false;
  };
  switch (val.type) {
    case Value::Nil:
      if (name == "Nil") return true;
      return try_builtin_trait("Nil");
    case Value::Bool:
      if (name == "Bool") return true;
      return try_builtin_trait("Bool");
    case Value::Long:
      if (name == "Long") return true;
      return try_builtin_trait("Long");
    case Value::Float:
      if (name == "Float") return true;
      return try_builtin_trait("Float");
    case Value::String:
      if (name == "String") return true;
      if (name == "StringLike") return true;
      return try_builtin_trait("String");
    case Value::StringView:
      if (name == "StringView") return true;
      if (name == "StringLike") return true;
      return try_builtin_trait("StringView");
    case Value::Array:
      if (name == "Array") return true;
      return try_builtin_trait("Array");
    case Value::Object: {
      if (name == "Object") return true;
      auto tag = class_tag(val);
      if (tag && *tag == name) return true;
      // A class instance that defines `__call__` is callable, so it
      // structurally satisfies the `Function` type — it can be bound to a
      // `Function`-annotated parameter and passed as a higher-order
      // callback, just like a closure (Option A: structural callable).
      // Limited to an own/proto `__call__` (a `__call__` inherited only
      // as a trait default isn't reachable from this free function);
      // direct `obj(x)` calls still dispatch trait-default `__call__`.
      if (name == "Function" && tag) {
        auto it = val.to_object().properties->find("__call__");
        if (it != val.to_object().properties->end() &&
            it->second.val.type == Value::Function) {
          return true;
        }
      }
      // Enum variant: an instance tagged with `__enum` also matches the
      // parent enum name (so `r: Result` accepts any `Result.*` variant).
      if (const auto& obj = val.to_object();
          obj.has("__enum")) {
        const auto& en = obj.get("__enum");
        if (en.type == Value::String && en.template get<std::string>() == name) {
          return true;
        }
      }
      // Structural trait conformance: if `name` is a registered trait
      // and this instance's class has the required methods, accept.
      // Cached per (class, trait) for O(1) repeat dispatch.
      if (auto* trait = lookup_trait(name)) {
        // Bare Object literal — `ObjectValue::builtins()` provides
        // defaults (e.g. `iter()` for Iterable) so the built-in table
        // is authoritative here.
        if (!tag) return builtin_conforms_to_trait("Object", name);
        std::string class_name(*tag);
        std::string trait_name(name);
        std::unique_lock lock(trait_mutex());
        auto& by_trait = trait_conformance_cache()[class_name];
        auto it = by_trait.find(trait_name);
        if (it != by_trait.end()) return it->second;
        // Walk instance methods → (name, arity) map for the check.
        std::unordered_map<std::string, size_t> class_methods;
        const auto& obj = val.to_object();
        for (auto& [k, sym] : *obj.properties) {
          if (sym.val.type != Value::Function) continue;
          const auto& fn = sym.val.template get<FunctionValue>();
          size_t arity = 0;
          for (const auto& p : *fn.params) {
            if (p.kw_only || p.kwargs_rest) continue;
            arity++;
          }
          class_methods.emplace(std::string(k), arity);
        }
        bool conforms = class_conforms_to_trait(class_methods, *trait);
        by_trait[trait_name] = conforms;
        return conforms;
      }
      return false;
    }
    case Value::Function:
      if (name == "Function") return true;
      return try_builtin_trait("Function");
    case Value::Tensor:
      if (name == "Tensor") return true;
      return try_builtin_trait("Tensor");
    case Value::Tuple:
      if (name == "Tuple") return true;
      return try_builtin_trait("Tuple");
    case Value::Set:
      if (name == "Set") return true;
      return try_builtin_trait("Set");
  }
  return false;
}

inline void check_type(const Value& val, std::string_view name,
                       std::string_view context, size_t line, size_t col) {
  if (name.empty()) return;
  if (type_matches(val, name)) return;
  throw CulebraError("TypeError", std::format(
      "type error: {} expects {}", context, name),
      static_cast<long>(line), static_cast<long>(col));
}

inline std::string Value::str_object() const {
  const auto& obj = to_object();
  auto* key = obj.properties.get();
  StrGuard guard(key);
  if (guard.already) return "{...}";
  const auto& properties = *obj.properties;
  // Range value: print in source form (`1..3`, `2..`, `..3`, `..`,
  // `1..=3`) rather than as a raw object.
  if (auto rt = class_tag(*this); rt && *rt == "Range") {
    const auto& s_v = obj.get("start");
    const auto& e_v = obj.get("end");
    std::string out;
    if (s_v.type != Value::Nil) out += s_v.str();
    out += obj.get("inclusive").to_bool() ? "..=" : "..";
    if (e_v.type != Value::Nil) out += e_v.str();
    return out;
  }
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

// Sentinels for an advance closure handed to `_make_iterator`. Returning
// `_iter_step_done()` ends the stream; `_iter_step_value(v)` yields v.
// (`std::optional<Value>` exposed as inline helpers so the dozens of
// existing advance closures keep their original return statements; only
// the wrapper changes shape under the Kotlin-style protocol.)
inline std::optional<Value> _iter_step_done() {
  return std::nullopt;
}

inline std::optional<Value> _iter_step_value(Value v) {
  return std::optional<Value>{std::move(v)};
}

// Build an Iterator ObjectValue conforming to the built-in
// `Iterator { has_next() -> Bool, next() -> Any }` + `Iterable.iter()`
// traits. The advance closure yields one value (or done) per call; we
// cache the lookahead so `has_next()` and `next()` stay idempotent in
// either order (Kotlin/Java contract) without requiring every advance
// closure to expose two callbacks.
template <typename AdvanceFn>
inline Value _make_iterator(AdvanceFn&& advance) {
  // - cached empty + ready=false → lookahead not yet pulled.
  // - cached empty + ready=true  → drained (advance returned nullopt).
  // - cached filled              → next value to surrender.
  struct LookaheadState {
    std::optional<Value> cached;
    bool ready = false;
    bool done = false;
    std::function<std::optional<Value>(std::shared_ptr<Environment>)> advance;
  };
  auto state = std::make_shared<LookaheadState>();
  state->advance = std::forward<AdvanceFn>(advance);

  auto ensure = [state](std::shared_ptr<Environment> callEnv) {
    if (state->ready) return;
    auto v = state->advance(callEnv);
    state->ready = true;
    if (!v) {
      state->done = true;
    } else {
      state->cached = std::move(*v);
    }
  };

  ObjectValue iter_obj;
  iter_obj.initialize(
      "iter",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
        return callEnv->get("this");
      })),
      false);
  iter_obj.initialize(
      "has_next",
      Value(FunctionValue({}, [state, ensure](
                                  std::shared_ptr<Environment> callEnv) {
        ensure(callEnv);
        return Value(!state->done);
      })),
      false);
  iter_obj.initialize(
      "next",
      Value(FunctionValue({}, [state, ensure](
                                  std::shared_ptr<Environment> callEnv) {
        ensure(callEnv);
        if (state->done) {
          throw CulebraError("StopIteration",
                             "next() called on a drained iterator");
        }
        auto v = std::move(*state->cached);
        state->cached.reset();
        state->ready = false;
        return v;
      })),
      false);
  return Value(std::move(iter_obj));
}

// A range value (`a..b`). Represented as a tagged Object carrying its
// bounds so it is a first-class storable/passable value; an absent
// endpoint (open-ended range) is stored as Nil. Slicing reads the bounds
// directly; for-in builds a bounded iterator (see _get_iterator).
inline Value _make_range(std::optional<long> start, std::optional<long> end,
                         bool inclusive) {
  ObjectValue r;
  r.initialize("class", Value(std::string("Range")), false);
  r.initialize("start", start ? Value(*start) : Value(), false);
  r.initialize("end", end ? Value(*end) : Value(), false);
  r.initialize("inclusive", Value(inclusive), false);
  return Value(std::move(r));
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
// the consumed byte count. Invalid / truncated sequences yield U+FFFD
// (the Unicode replacement character) and advance by one byte — the
// industry-standard behavior (Go's `for range`, Swift, JS TextDecoder).
// The source bytes are never dropped (silent skip), so the original
// String still round-trips via byte index / slice; only the decoded
// codepoint value is the replacement. Shared by `String.iter` /
// `code_points` / `graphemes`.
inline void _decode_one_utf8(std::string_view s, size_t& off,
                             char32_t& cp) {
  size_t bytes;
  if (!unicode::utf8::decode_codepoint(s.data() + off, s.size() - off,
                                       bytes, cp)) {
    cp = 0xFFFD;
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
  // Method bodies use `return X` (generator-synthesized next() does,
  // and any user iterator with an early return). `FunctionValue::eval`
  // raises ReturnValue rather than returning the value, so catch it
  // here — without this, the surrounding for-in would unwind silently.
  try {
    return fn.to_function().eval(_make_method_call_env(receiver, 0, 0));
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

// Pair to `ValueEq` (forward-declared earlier): invoke `a.eq(b)` for
// user-defined Hashable+Eq classes acting as Object/Set/HashMap keys.
// Caller guarantees both sides are Object with an `eq` property.
inline bool _invoke_user_eq(const Value& a, const Value& b) {
  const auto& fn = a.to_object().get("eq").to_function();
  auto env = _make_method_call_env(a, 0, 0);
  if (!fn.params->empty()) {
    env->initialize((*fn.params)[0].name, b, false);
  }
  return fn.eval(env).to_bool();
}

// --- @derive support (project_type_system.md §D) ----------------------
//
// `@derive(Eq, Hash, Show, Comparable)` is a compiler-recognized class
// directive (not a user function). The generated methods are reflective:
// they walk the instance's own data fields at call time, so a single
// implementation serves every class. Eq -> eq(other), Hash -> hash(),
// Show -> to_s(), Comparable -> cmp(other). The trait defaults (neq,
// lt/le/gt/ge) fall out of structural conformance once eq / cmp exist.

// Visit each own data field (name, value) of a class instance in
// insertion order, skipping the `class`/`__enum` tags and any method
// slots. Value is passed by const-ref — no per-field copy (derived
// methods run at script-call time, so this is a hot path).
template <class F>
inline void _for_each_derived_field(const Value& v, F&& fn) {
  if (v.type != Value::Object) return;
  for (const auto& [name, sym] : *v.to_object().properties) {
    if (name == "class" || name == "__enum") continue;
    if (sym.val.type == Value::Function) continue;
    fn(name, sym.val);
  }
}

// Inject the methods requested by `@derive(...)` into `method_template`,
// honoring "user definitions win" (a derived method is skipped when the
// class already declares one of that name). `derive_method_for` validates
// the trait name (shared with the JIT).
inline void inject_derived_methods(
    std::vector<std::pair<std::string_view, Value>>& method_template,
    std::string_view class_name,
    const std::vector<std::string_view>& derive_traits) {
  auto has_method = [&](std::string_view n) {
    for (auto& [name, _] : method_template)
      if (name == n) return true;
    return false;
  };
  auto add = [&](std::string_view name, std::vector<FunctionValue::Parameter> ps,
                 std::function<Value(std::shared_ptr<Environment>)> body) {
    if (has_method(name)) return;
    Value fn(FunctionValue(ps, std::move(body)));
    fn.get<FunctionValue>().name = std::string(name);
    method_template.push_back({name, std::move(fn)});
  };
  for (auto trait : derive_traits) {
    auto dm = culebra::derive_method_for(trait);
    switch (dm.kind) {
      case 0:  // Eq -> eq(other)
        add(dm.name, {{"other", false}},
            [](const std::shared_ptr<Environment>& env) -> Value {
              const auto& self_v = env->get("this");
              const auto& other = env->get("other");
              if (other.type != Value::Object) return Value(false);
              if (class_tag(self_v) != class_tag(other)) return Value(false);
              const auto& ob = other.to_object();
              bool eq = true;
              _for_each_derived_field(
                  self_v, [&](std::string_view name, const Value& av) {
                    if (!eq) return;
                    auto it = ob.properties->find(name);
                    if (it == ob.properties->end() ||
                        !ValueEq{}(av, it->second.val))
                      eq = false;
                  });
              return Value(eq);
            });
        break;
      case 1:  // Hash -> hash()
        add(dm.name, {},
            [class_name](const std::shared_ptr<Environment>& env) -> Value {
              const auto& self_v = env->get("this");
              size_t h = std::hash<std::string_view>{}(class_name);
              _for_each_derived_field(
                  self_v, [&](std::string_view, const Value& av) {
                    h = h * 31 + ValueHash{}(av);
                  });
              return Value(static_cast<long>(h));
            });
        break;
      case 2:  // Show -> to_s()
        add(dm.name, {},
            [class_name](const std::shared_ptr<Environment>& env) -> Value {
              const auto& self_v = env->get("this");
              std::string s(class_name);
              s += "(";
              bool first = true;
              _for_each_derived_field(
                  self_v, [&](std::string_view, const Value& av) {
                    if (!first) s += ", ";
                    first = false;
                    s += av.str();
                  });
              s += ")";
              return Value(std::move(s));
            });
        break;
      case 3:  // Comparable -> cmp(other)
        add(dm.name, {{"other", false}},
            [](const std::shared_ptr<Environment>& env) -> Value {
              const auto& self_v = env->get("this");
              const auto& other = env->get("other");
              if (other.type != Value::Object) return Value(0L);
              const auto& ob = other.to_object();
              long result = 0;
              bool done = false;
              _for_each_derived_field(
                  self_v, [&](std::string_view name, const Value& av) {
                    if (done) return;
                    auto it = ob.properties->find(name);
                    if (it == ob.properties->end()) {
                      done = true;
                      return;
                    }
                    const Value& bv = it->second.val;
                    if (av == bv) return;
                    result = (av < bv) ? -1 : 1;
                    done = true;
                  });
              return Value(result);
            });
        break;
    }
  }
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
  if (r.type != Value::String && r.type != Value::StringView) {
    throw CulebraError("TypeError", "__str__ must return a String");
  }
  return std::string(r.to_string_view());
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
// `std::nullopt` when the iterator is drained. Built on the Kotlin-
// style `Iterator { has_next() -> Bool, next() -> Any }` contract so
// each upstream advance is one `has_next()` + (when true) `next()`.
inline std::optional<Value> _iter_next_value(const Value& upstream) {
  if (!_invoke_method_no_args(upstream, "has_next").to_bool()) {
    return std::nullopt;
  }
  return _invoke_method_no_args(upstream, "next");
}

// Bind positional callback args into a function frame (defined below, after
// bind_overflow_args). Forward-declared so the iterator callback invokers can
// share the one binder that handles *args / defaults / __ARGS__ builtins.
inline void bind_callback_params(Environment& frame, const FunctionValue& f,
                                 std::initializer_list<Value> args);

// Invoke a user-supplied callable (mapper/predicate/reducer callback)
// on the given argument. Used by Iterator methods where the callback
// body runs repeatedly per step. No `this` is bound — callbacks are
// free-function calls — but `self` refers to the callback for
// recursion. Arity is validated once at the iterator HOF entry
// (check_callback_arity); the binder handles *args / defaults uniformly.
inline Value _invoke_callback(const Value& fn_val) {
  const auto& fn = fn_val.to_function();
  auto env = std::make_shared<Environment>();
  env->is_function_frame = true;
  env->initialize("self", fn_val, false);
  bind_callback_params(*env, fn, {});
  env->initialize("__LINE__", Value(0L), false);
  env->initialize("__COLUMN__", Value(0L), false);
  try {
    return fn.eval(env);
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

inline Value _invoke_callback(const Value& fn_val, const Value& a) {
  const auto& fn = fn_val.to_function();
  auto env = std::make_shared<Environment>();
  env->is_function_frame = true;
  env->initialize("self", fn_val, false);
  bind_callback_params(*env, fn, {a});
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
  bind_callback_params(*env, fn, {a, b});
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
  // A range value iterates start..end (an open start or end has no defined
  // iteration bound, so an unbounded range is not iterable).
  if (auto ct = class_tag(iterable); ct && *ct == "Range") {
    const auto& obj = iterable.to_object();
    const auto& s = obj.get("start");
    const auto& e = obj.get("end");
    if (s.type == Value::Nil || e.type == Value::Nil) {
      throw CulebraError("TypeError", "cannot iterate an unbounded range",
          static_cast<long>(line), static_cast<long>(col));
    }
    long end = e.to_long() + (obj.get("inclusive").to_bool() ? 1 : 0);
    auto current = std::make_shared<long>(s.to_long());
    return _make_iterator([current, end](std::shared_ptr<Environment>) {
      if (*current >= end) return _iter_step_done();
      auto v = Value(*current);
      (*current)++;
      return _iter_step_value(std::move(v));
    });
  }
  if (iterable.type == Value::String ||
      iterable.type == Value::StringView) {
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
    // Match the JIT's for-in check: list the allowed iterable types and
    // the actual type, single-sourced through the shared helper.
    throw_type_mismatch("Array, Tuple, Set, Object, or String",
                        iterable.type_name(),
                        static_cast<long>(line), static_cast<long>(col));
  }
  return iter_fn.to_function().eval(
      _make_method_call_env(iterable, line, col));
}

inline std::unordered_map<std::string_view, Value>& ObjectValue::builtins() {
  using namespace std::literals;
  static std::unordered_map<std::string_view, Value> props_ = {
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
      // Iterator protocol: yield keys in insertion order. Keys are
      // snapshotted up front so `next` stays O(1) per step. Structural
      // mutation of the underlying Object (add/delete) during iteration
      // raises — matches Python dict semantics. Value updates on
      // existing keys are allowed and don't bump the counter.
      {"iter"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         struct State {
           std::shared_ptr<OrderedSymbolMap> props;
           std::vector<Value> keys;
           size_t index = 0;
           size_t version = 0;
         };
         const auto& obj = callEnv->get("this").to_object();
         auto st = std::make_shared<State>();
         st->props = obj.properties;
         st->version = st->props->mut_count();
         // Snapshot all keys (String + non-String) in insertion order via
         // the same `key_order` walk `keys()` uses, so for-in matches
         // keys() / size(). Fall back to the property-map walk for objects
         // built by direct emplace (class instances, no key_order).
         if (obj.key_order && !obj.key_order->empty()) {
           st->keys.reserve(obj.key_order->size());
           for (const auto& k : *obj.key_order) st->keys.push_back(k);
         } else {
           st->keys.reserve(st->props->size());
           for (const auto& [k, _] : *st->props) {
             st->keys.emplace_back(std::string(k));
           }
         }
         return _make_iterator([st](std::shared_ptr<Environment>) {
           if (st->props->mut_count() != st->version) {
             throw CulebraError(
                 "RuntimeError",
                 "Object changed size during iteration");
           }
           if (st->index >= st->keys.size()) return _iter_step_done();
           auto v = st->keys[st->index];
           st->index++;
           return _iter_step_value(std::move(v));
         });
       }))}};
  return props_;
}

// Higher-order Array helpers: invoke a 1-parameter callback `f` on `v`.
// Bind the positional overflow (args beyond the regular params) to the
// implicit `__ARGS__` and, if declared, to the named `*args` parameter —
// both alias the same Array. Shared by the embedder `call` path and the
// main call binder.
inline void bind_overflow_args(
    const std::vector<FunctionValue::Parameter>& params,
    Environment& callEnv, const Value& extras_val) {
  callEnv.initialize("__ARGS__", extras_val, false);
  for (const auto& p : params) {
    if (p.args_rest) {
      callEnv.initialize(p.name, extras_val, false);
      break;
    }
  }
}

// Resolve parameter `p`'s default the way the full call binder does, so the
// two binders share one implementation and cannot drift. A user `default_expr`
// (AST) is evaluated against the function's `def_env` plus the earlier params
// already bound in `earlier`; a C++ `default_value` is used directly. Returns
// nullopt when the param has no default (a missing required argument — the
// caller decides whether that is an error). `eval_fn` is the AST evaluator:
// the interpreter's `eval` member at a normal call site, or `f.eval_default_expr`
// (which closes over the interpreter) from the free-function callback binder.
// Invariant: any param with a `default_expr` was built by make_function_value,
// which sets `eval_default_expr`, so `eval_fn` is non-null whenever it is used.
template <class EvalFn>
inline std::optional<Value> resolve_param_default(const FunctionValue& f,
                                                  size_t p,
                                                  const Environment& earlier,
                                                  EvalFn&& eval_fn) {
  const auto& param = (*f.params)[p];
  if (param.default_expr) {
    auto defEnv = std::make_shared<Environment>(f.def_env);
    defEnv->outer = f.def_env;
    for (size_t j = 0; j < p; j++) {
      const auto& pj = (*f.params)[j];
      if (earlier.has(pj.name))
        defEnv->initialize(pj.name, earlier.get(pj.name), false);
    }
    return eval_fn(*param.default_expr, defEnv);
  }
  if (param.default_value) return *param.default_value;
  return std::nullopt;
}

// Bind `args` positionally into `frame` for callback `f`, routing overflow
// into a `*args` catch-all and honoring defaults (literal AND expression) the
// same way the full call binder does — via the shared `resolve_param_default`.
// Every higher-order callback invocation goes through here so a variadic
// (`fn (*xs)`) or defaulted callback binds exactly like a direct call. The
// caller sets up `self` and seeds __LINE__/__COLUMN__; this only binds the
// parameters. `args` is an initializer_list so the common 1-2 arg call sites
// avoid a heap-backed vector.
inline void bind_callback_params(Environment& frame, const FunctionValue& f,
                                 std::initializer_list<Value> args) {
  const auto& params = *f.params;
  size_t regulars = regular_param_count(params);
  size_t pos = 0;
  for (size_t i = 0; i < params.size(); i++) {
    const auto& p = params[i];
    if (p.kwargs_rest) {
      frame.initialize(p.name, Value(ObjectValue{}), false);
      continue;
    }
    if (p.args_rest) continue;  // bound from the overflow Array below
    if (p.kw_only) {
      // A callback receives positional args only, so a kw-only param always
      // takes its default (expression or literal), same as the full binder.
      if (auto dv = resolve_param_default(f, i, frame, f.eval_default_expr))
        frame.initialize(p.name, *dv, p.mut);
      continue;
    }
    if (pos < args.size()) {
      frame.initialize(p.name, args.begin()[pos], p.mut);
      pos++;
    } else if (auto dv = resolve_param_default(f, i, frame, f.eval_default_expr)) {
      frame.initialize(p.name, *dv, p.mut);
    }
  }
  // Only build the overflow Array (and bind `__ARGS__` / `*args`) when there
  // is surplus or a `*args` catch-all to fill. A plain fixed-arity callback —
  // the common case — skips the per-call ArrayValue allocation and its GC
  // registration entirely; every `__ARGS__`-reading builtin either declares
  // `*args` or has zero regular params, so it still falls inside this guard.
  if (regulars < args.size() || has_args_rest(params)) {
    ArrayValue extras;
    for (size_t i = regulars; i < args.size(); ++i) {
      extras.values->push_back(args.begin()[i]);
    }
    bind_overflow_args(params, frame, Value(std::move(extras)));
  }
}

// Reject a higher-order callback that can't be invoked with exactly
// `expected` positional args, mirroring the JIT's `_culebra_expect_callback`
// so both backends accept/reject the same callbacks (see
// `callback_arity_accepts`). Thrown once per HOF call (before the loop) so an
// empty receiver still errors symmetrically. Position is left unset for the
// eval wrapper to backfill from the call site, matching the JIT's call-site
// line/col.
inline void check_callback_arity(const FunctionValue& f, long expected,
                                 std::string_view method) {
  auto b = builtin_arity_bounds(*f.params);
  long cb_max = b.variadic ? -1 : b.max;
  if (!callback_arity_accepts(b.min, cb_max, expected)) {
    throw CulebraError("TypeError",
        std::format("type error: {} expects a {}-parameter function",
                    method, expected));
  }
}

// Resolve a higher-order builtin's callback argument to a FunctionValue.
// A plain function is returned as-is; a callable class instance (own/proto
// `__call__`) is wrapped so the builtin invokes it like any function, with
// `this` bound to the instance (Option A: structural callable). type_matches
// already let the instance bind to the `f: Function` parameter; this is
// where the per-call dispatch happens. A genuine non-callable falls through
// to to_function(), which raises the standard "expected Function" error.
inline FunctionValue as_callback(const Value& cb) {
  if (cb.type == Value::Object && class_tag(cb)) {
    auto it = cb.to_object().properties->find("__call__");
    if (it != cb.to_object().properties->end() &&
        it->second.val.type == Value::Function) {
      const auto& pf = it->second.val.get<FunctionValue>();
      FunctionValue bound(*pf.params, [pf, cb](std::shared_ptr<Environment> ce) {
        ce->initialize("this", cb, false);
        return pf.eval(ce);
      });
      bound.name = pf.name;
      return bound;
    }
  }
  return cb.to_function();
}

// Sets up a function-frame environment with `self` bound to the callback,
// and treats an early return (`return x` compiled as a thrown Value) as
// the callback's result.
inline Value invoke_unary_callback(std::shared_ptr<Environment> callEnv,
                                   const FunctionValue& f, const Value& v) {
  auto inner = std::make_shared<Environment>(callEnv);
  inner->is_function_frame = true;
  inner->initialize("self", callEnv->get("f"), false);
  // Bind the element through the shared callback binder so a `*args` /
  // defaulted / __ARGS__-reading (range/iota) callback binds exactly like a
  // direct call. Arity is validated once at the HOF entry (check_callback_arity).
  bind_callback_params(*inner, f, {v});
  // A function frame doesn't inherit the caller's __LINE__/__COLUMN__, so a
  // builtin callback that reads them (`to_long`/`to_float`) would NameError
  // when handed to a HOF (`map(to_float)`). Seed 0 like _invoke_callback.
  inner->initialize("__LINE__", Value(0L), false);
  inner->initialize("__COLUMN__", Value(0L), false);
  try {
    return f.eval(inner);
  } catch (const ReturnValue& r) {
    return r.value;
  }
}

// Default-valued params aren't resolved — pass all positional args.
// Extras bind to `__ARGS__` (matches the in-language calling convention).
inline Value call(const std::shared_ptr<Environment>& env, std::string_view name,
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
    if (params[i].args_rest) continue;  // bound from extras below
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
  bind_overflow_args(params, *callEnv, Value(std::move(extras)));
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
inline void define(const std::shared_ptr<Environment>& env, std::string_view name,
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

inline std::unordered_map<std::string_view, Value>& ArrayValue::builtins() {
  using namespace std::literals;
  static std::unordered_map<std::string_view, Value> props_ = {
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "map");
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "filter");
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "for_each");
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
             auto f = as_callback(callEnv->get("f"));
             check_callback_arity(f, 2, "reduce");
             Value acc = callEnv->get("init");
             for (const auto& v : *arr.values) {
               auto inner = std::make_shared<Environment>(callEnv);
               inner->is_function_frame = true;
               inner->initialize("self", callEnv->get("f"), false);
               // Bind (acc, elem) through the shared binder so a `*args` /
               // defaulted reducer binds like a direct call.
               bind_callback_params(*inner, f, {acc, v});
               // See invoke_unary_callback: a builtin reducer reading
               // __LINE__/__COLUMN__ would NameError without these.
               inner->initialize("__LINE__", Value(0L), false);
               inner->initialize("__COLUMN__", Value(0L), false);
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "find");
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "any");
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "all");
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "flat_map");
                             ArrayValue out;
                             for (const auto& v : *arr.values) {
                               auto r = invoke_unary_callback(callEnv, f, v);
                               if (r.type != Value::Array) {
                                 throw CulebraError("TypeError",
                                     "type error: flat_map callback must "
                                     "return an Array");
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
                             auto f = as_callback(callEnv->get("f"));
                             check_callback_arity(f, 1, "sort_by");
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
  // Optional axis: `t.sum()` reduces all (Float), `t.sum(axis)` reduces
  // along an axis (Tensor). Declared as a `*args` catch-all so the arity
  // is honest (variadic, not the fixed-0 that a `{}` list would imply) —
  // the positional-arity check keys off this. The body still reads the
  // overflow via __ARGS__.
  return Value(FunctionValue(
      {FunctionValue::Parameter::make_args_rest("axis")},
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
inline std::unordered_map<std::string_view, Value>& TensorValue::builtins() {
  using namespace std::literals;
  static std::unordered_map<std::string_view, Value> props_ = {
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
      // Activations as instance methods: `t.relu()` / `.sigmoid()` /
      // `.softmax()`. A user class may still define its own `relu` etc.;
      // method lookup gives the class method priority (the JIT does the
      // same via compile_user_method_over_builtin), so these never shadow
      // a user definition.
      {"relu"sv, Value(FunctionValue(
                     {},
                     [](std::shared_ptr<Environment> callEnv) {
                       const auto& self =
                           callEnv->get("this").to_tensor().impl;
                       return Value(TensorValue(tensor_unary(Op::Relu, self)));
                     },
                     "Tensor"sv))},
      {"sigmoid"sv, Value(FunctionValue(
                        {},
                        [](std::shared_ptr<Environment> callEnv) {
                          const auto& self =
                              callEnv->get("this").to_tensor().impl;
                          return Value(
                              TensorValue(tensor_unary(Op::Sigmoid, self)));
                        },
                        "Tensor"sv))},
      {"softmax"sv, Value(FunctionValue(
                        {},
                        [](std::shared_ptr<Environment> callEnv) {
                          const auto& self =
                              callEnv->get("this").to_tensor().impl;
                          return Value(
                              TensorValue(tensor_unary(Op::Softmax, self)));
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
inline std::unordered_map<std::string_view, Value>& string_builtins() {
  using namespace std::literals;
  static std::unordered_map<std::string_view, Value> props_ = {
      {"size"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return Value(static_cast<long>(
             callEnv->get("this").to_string_view().size()));
       }))},
      // `.view()` → StringView sharing the receiver's bytes.
      {"view"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto [src, sv] = callEnv->get("this").share_source_and_view();
         return Value(std::move(src), sv);
       }))},
      // `.to_string()` on StringView → owning String (alloc + copy).
      // No-op when called on a String (returns a new String of the
      // same bytes). Primary way to escape the parameter-only
      // lifetime: store as String, not StringView.
      {"to_string"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return Value(std::string(callEnv->get("this").to_string_view()));
       }))},
      {"upper"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return Value(culebra::ascii_upper(
             callEnv->get("this").to_string()));
       }))},
      {"lower"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         return Value(culebra::ascii_lower(
             callEnv->get("this").to_string()));
       }))},
      {"trim"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& s = callEnv->get("this").to_string();
         return Value(std::string(trim_ascii(s)));
       }))},
      // Array<StringView>. Returning Array (eager) matches
      // Python/JS/Swift/Ruby/Go; for early exit use `split_iter`.
      {"split"sv,
       Value(FunctionValue(
           {{"sep", false, "StringLike"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             auto [src, base] = callEnv->get("this").share_source_and_view();
             auto sep = std::string(callEnv->get("sep").to_string_view());
             ArrayValue out;
             if (sep.empty()) {
               out.values->push_back(Value(src, base));
             } else {
               size_t pos = 0;
               while (true) {
                 auto p = base.find(sep, pos);
                 if (p == std::string_view::npos) {
                   out.values->push_back(Value(src, base.substr(pos)));
                   break;
                 }
                 out.values->push_back(
                     Value(src, base.substr(pos, p - pos)));
                 pos = p + sep.size();
               }
             }
             return Value(std::move(out));
           }))},
      // Lazy variant of split. `.take(n).collect()` short-circuits.
      {"split_iter"sv,
       Value(FunctionValue(
           {{"sep", false, "StringLike"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             struct State {
               std::string sep;
               size_t pos = 0;
               bool done = false;
             };
             auto [src, base] = callEnv->get("this").share_source_and_view();
             auto st = std::make_shared<State>();
             st->sep = std::string(callEnv->get("sep").to_string_view());
             return _make_iterator(
                 [src, base, st](std::shared_ptr<Environment>) {
                   if (st->done) return _iter_step_done();
                   if (st->sep.empty()) {
                     st->done = true;
                     return _iter_step_value(Value(src, base));
                   }
                   auto p = base.find(st->sep, st->pos);
                   if (p == std::string_view::npos) {
                     st->done = true;
                     auto sv = base.substr(st->pos);
                     return _iter_step_value(Value(src, sv));
                   }
                   auto sv = base.substr(st->pos, p - st->pos);
                   st->pos = p + st->sep.size();
                   return _iter_step_value(Value(src, sv));
                 });
           }))},
      {"contains"sv,
       Value(FunctionValue({{"sub", false, "StringLike"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             auto s = callEnv->get("this").to_string_view();
                             auto sub =
                                 callEnv->get("sub").to_string_view();
                             return Value(s.find(sub) != std::string_view::npos);
                           }))},
      {"starts_with"sv,
       Value(FunctionValue(
           {{"prefix", false, "StringLike"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             auto s = callEnv->get("this").to_string_view();
             auto prefix = callEnv->get("prefix").to_string_view();
             return Value(s.size() >= prefix.size() &&
                          s.compare(0, prefix.size(), prefix) == 0);
           }))},
      {"ends_with"sv,
       Value(FunctionValue(
           {{"suffix", false, "StringLike"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             auto s = callEnv->get("this").to_string_view();
             auto suf = callEnv->get("suffix").to_string_view();
             return Value(s.size() >= suf.size() &&
                          s.compare(s.size() - suf.size(), suf.size(), suf) ==
                              0);
           }))},
      // Sub-view sharing the receiver's bytes. One source-copy alloc
      // for a String receiver; chained slices reuse the source.
      {"slice"sv,
       Value(FunctionValue(
           {{"start", false, "Long"sv}, {"end", false, "Long"sv}},
           [](std::shared_ptr<Environment> callEnv) {
             auto [src, base] = callEnv->get("this").share_source_and_view();
             auto [ss, ee] = normalize_slice(callEnv->get("start").to_long(),
                                             callEnv->get("end").to_long(),
                                             static_cast<long>(base.size()));
             return Value(std::move(src), base.substr(ss, ee - ss));
           }))},
      // Lazy UTF-8 walk yielding one-scalar StringViews. Invalid /
      // truncated lead bytes yield as 1-byte views and advance.
      {"iter"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto [src, base] = callEnv->get("this").share_source_and_view();
         auto offset = std::make_shared<size_t>(0);
         return _make_iterator(
             [src, base, offset](std::shared_ptr<Environment>) {
               if (*offset >= base.size()) return _iter_step_done();
               size_t len = peg::codepoint_length(base.data() + *offset,
                                                  base.size() - *offset);
               if (len == 0) len = 1;
               auto sv = base.substr(*offset, len);
               *offset += len;
               return _iter_step_value(Value(src, sv));
             });
       }))},
      // Lazy UTF-8 walk yielding Unicode scalar values as Long. For
      // numeric / range / classification work where the per-scalar
      // String allocation of `iter`/`for-in` is wasteful. Invalid or
      // truncated bytes yield the raw byte value (0–255) and advance
      // by one, mirroring `iter`'s permissive policy.
      {"code_points"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         auto [src, base] = callEnv->get("this").share_source_and_view();
         auto offset = std::make_shared<size_t>(0);
         return _make_iterator(
             [src, base, offset](std::shared_ptr<Environment>) {
               if (*offset >= base.size()) return _iter_step_done();
               char32_t cp;
               _decode_one_utf8(base, *offset, cp);
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
         // Single shared_ptr keeps refcount inc/dec to one pair per
         // step instead of three (was: u32, byte_off, cp_off separately).
         struct State {
           std::u32string u32;
           size_t byte_off = 0;
           size_t cp_off = 0;
         };
         auto [src, base] = callEnv->get("this").share_source_and_view();
         auto st = std::make_shared<State>();
         return _make_iterator(
             [src, base, st](std::shared_ptr<Environment>) {
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
             while (st->u32.size() < target && st->byte_off < base.size()) {
               char32_t cp;
               _decode_one_utf8(base, st->byte_off, cp);
               st->u32.push_back(cp);
             }
           };
           extend_to(st->cp_off + 1);
           if (st->cp_off >= st->u32.size()) return _iter_step_done();
           // Grow the buffer until `grapheme_length` returns strictly
           // less than the available size (a boundary is confirmed to
           // lie inside the buffer) or until the UTF-8 input is
           // exhausted — without lookahead, a `len == avail` return
           // could still be truncated by a continuation codepoint.
           for (;;) {
             size_t avail = st->u32.size() - st->cp_off;
             size_t len = unicode::grapheme_length(
                 st->u32.data() + st->cp_off, avail);
             if (len == 0) len = 1;  // grapheme_length contract says >=1 on avail>=1; belt-and-braces
             if (len < avail || st->byte_off >= base.size()) {
               std::string out;
               unicode::utf8::encode(st->u32.data() + st->cp_off, len, out);
               st->cp_off += len;
               if (st->cp_off >= kTrimThreshold) {
                 st->u32.erase(0, st->cp_off);
                 st->cp_off = 0;
               }
               // Wrap the per-cluster bytes as a StringView. Truly
               // zero-copy graphemes (no per-cluster alloc) needs
               // byte-offset tracking in the source UTF-8.
               auto cluster_src = std::make_shared<const std::string>(std::move(out));
               auto sv = std::string_view(*cluster_src);
               return _iter_step_value(Value(std::move(cluster_src), sv));
             }
             extend_to(st->u32.size() + kExtendChunk);
           }
         });
       }))}};
  return props_;
}

// Method lookup table for primitive Set values. Mirrors string_builtins;
// SetValue is not an ObjectValue so dispatch goes through a dedicated
// path in eval_property.
inline std::unordered_map<std::string_view, Value>& set_builtins() {
  using namespace std::literals;
  static std::unordered_map<std::string_view, Value> props_ = {
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
inline std::unordered_map<std::string_view, Value>& tuple_builtins() {
  using namespace std::literals;
  static std::unordered_map<std::string_view, Value> props_ = {
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
inline std::unordered_map<std::string_view, Value>& iterator_builtins() {
  using namespace std::literals;
  static std::unordered_map<std::string_view, Value> props_ = {
      // --- Non-terminal: return a new lazy Iterator ----------------

      {"map"sv,
       Value(FunctionValue({{"f", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto f = Value(as_callback(callEnv->get("f")));
         check_callback_arity(f.to_function(), 1, "map");
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
         auto p = Value(as_callback(callEnv->get("p")));
         check_callback_arity(p.to_function(), 1, "filter");
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
         auto p = Value(as_callback(callEnv->get("p")));
         check_callback_arity(p.to_function(), 1, "take_while");
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
         auto f = Value(as_callback(callEnv->get("f")));
         check_callback_arity(f.to_function(), 1, "flat_map");
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
         auto f = Value(as_callback(callEnv->get("f")));
         check_callback_arity(f.to_function(), 1, "for_each");
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
         auto f = Value(as_callback(callEnv->get("f")));
         check_callback_arity(f.to_function(), 2, "reduce");
         while (auto v = _iter_next_value(upstream)) {
           acc = _invoke_callback(f, acc, *v);
         }
         return acc;
       }))},

      {"find"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = Value(as_callback(callEnv->get("p")));
         check_callback_arity(p.to_function(), 1, "find");
         while (auto v = _iter_next_value(upstream)) {
           if (_invoke_callback(p, *v).to_bool()) return *v;
         }
         return Value();
       }))},

      {"any"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = Value(as_callback(callEnv->get("p")));
         check_callback_arity(p.to_function(), 1, "any");
         while (auto v = _iter_next_value(upstream)) {
           if (_invoke_callback(p, *v).to_bool()) return Value(true);
         }
         return Value(false);
       }))},

      {"all"sv,
       Value(FunctionValue({{"p", false, "Function"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
         auto upstream = callEnv->get("this");
         auto p = Value(as_callback(callEnv->get("p")));
         check_callback_arity(p.to_function(), 1, "all");
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
    std::shared_ptr<Environment> callEnv, const char* name) {
  const auto& extras = *callEnv->get("__ARGS__").to_array().values;
  // range/iota take 1 (end) or 2 (start, end) positional arguments — too
  // few or too many is an error, not a silently-truncated/empty range
  // (matches the JIT and avoids swallowing `range(a, b, c)` typos).
  if (extras.size() < 1 || extras.size() > 2) {
    throw CulebraError(
        "ArityError",
        builtin_arity_error_message(name, 1, 2,
                                    static_cast<long>(extras.size())),
        callEnv->get("__LINE__").to_long(),
        callEnv->get("__COLUMN__").to_long());
  }
  if (extras.size() == 1) return {0, extras[0].to_long()};
  return {extras[0].to_long(), extras[1].to_long()};
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
          // Modeled as `iota(*args)` — the 1-or-2 positional args are read
          // from __ARGS__ by _parse_range_args. Declaring `*args` (rather
          // than an empty list) makes the arity variadic so iota stays usable
          // as a higher-order callback (`map(iota)`), matching the JIT's
          // JIT_VARIADIC_ARITY treatment and the NsParamMeta args_rest model.
          {FunctionValue::Parameter::make_args_rest("args")},
          [](std::shared_ptr<Environment> callEnv) {
            auto [start, end] = _parse_range_args(callEnv, "iota");
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
          // Modeled as `range(*args, step=1)`: the 1-or-2 positional args are
          // read from __ARGS__, `step` is keyword-only. The `*args` makes the
          // arity variadic so range stays usable as a higher-order callback
          // (`map(range)`), matching the JIT's JIT_VARIADIC_ARITY treatment.
          {FunctionValue::Parameter::make_args_rest("args"),
           {"step", false, {}, nullptr, kw_default_one(), true}},
          [](std::shared_ptr<Environment> callEnv) {
            auto [start, end] = _parse_range_args(callEnv, "range");
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

  // Borrowed cooperative-cancellation flag (an isolate's IsolateCore::interrupt,
  // set by isolate.h on the child's interpreter). Cached here so the per-
  // statement poll in _eval_dispatch is a single member load + null test
  // instead of a thread_local current_runtime() round-trip — null and free on
  // the main thread. The channel blocking waits use current_runtime() directly
  // (they're not on the hot path).
  std::atomic<bool>* interrupt_flag_ = nullptr;

  // --- Public entry points for the isolate / sendable layer ---
  // Rebuild a user closure on THIS interpreter's heap from its retained
  // defining AST (FunctionValue::params_ast/body) and a freshly-populated
  // capture environment. Used by sendable::deserialize to reconstruct a
  // closure shipped across a thread boundary. Forwards to make_function_value.
  Value rebuild_closure(const peg::Ast& params_ast,
                        std::shared_ptr<peg::Ast> body,
                        std::string_view return_type,
                        const std::shared_ptr<Environment>& env) {
    return make_function_value(params_ast, std::move(body), return_type, env);
  }
  // Invoke a (rebuilt) closure with positional args, binding params and
  // catching `return`. Used by the isolate child to run the spawned body.
  Value call_closure(const Value& fn, const std::shared_ptr<Environment>& env,
                     std::vector<Value> args) {
    CallArgs ca;
    ca.positional = std::move(args);
    // invoke_user_function_with_args indexes positional_locs in lock-step with
    // positional (for per-arg error locations); keep them the same length.
    ca.positional_locs.assign(ca.positional.size(), {0, 0});
    return invoke_user_function_with_args(fn, env, std::move(ca), 0, 0);
  }

  // The multi-module driver lives outside the class for symmetry with
  // `interpret(...)` but needs to drive per-module defer flushes and
  // export extraction.
  friend bool interpret_modules(const std::vector<LoadedModule>& modules,
                                const std::shared_ptr<Environment>& env,
                                Value& val,
                                std::vector<std::string>& msgs,
                                Debugger debugger);

  struct MultiMethod {
    std::vector<std::string> param_types;
    Value body;
    // `*args` catch-all: matches any arity >= param_types.size(); the
    // surplus args are absorbed (scored as Any) so a fixed-arity entry
    // always wins a tie.
    bool variadic = false;
  };

  // Cached module values keyed by absolute on-disk path. The driver
  // walks the dependency graph (ModuleLoader), evaluates each module
  // in topological order, and stores its `export { ... }` Object here.
  // `IMPORT_STMT` then binds the cached entry in the importing scope.
  std::unordered_map<std::string, Value> module_cache_;

  // Owning storage for Generic class type-param neutralized strings.
  // Parameter::type_name is `string_view`; when class-method
  // neutralization rewrites `Array<T>` → `Array<Any>` we need
  // long-lived storage backing the view. Lives as long as the
  // Interpreter so all derived FunctionValues stay safe to introspect.
  std::vector<std::shared_ptr<std::string>> generic_type_storage_;

  // Default-impl method bodies declared on traits, owned by this
  // Interpreter so dispatch can call them with the right closure.
  // Outer map: trait name → method name → FunctionValue (the default).
  std::unordered_map<std::string,
                     std::unordered_map<std::string, Value>>
      trait_default_impls_;
  // Absolute paths of the modules currently being evaluated. The top
  // entry is the active module; `eval_import_stmt` uses its directory
  // to resolve relative paths in `import name from "./..."`.
  std::vector<std::filesystem::path> module_stack_;

  Value eval(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    try {
      return _eval_dispatch(ast, env);
    } catch (CulebraError& e) {
      // Stamp the deepest eval()'s AST location onto the CulebraError
      // line/col fields (only when they are still zero). Keep the
      // message untouched so `e.message` is identical across interp
      // and JIT; main.cc prints line/col separately from the fields.
      if (e.line == 0 && e.col == 0) {
        e.line = static_cast<long>(ast.line);
        e.col = static_cast<long>(ast.column);
      }
      throw;
    } catch (const std::runtime_error& e) {
      // Legacy path for any C++ throw site not yet converted to
      // CulebraError. Preserve location stamping in the message since
      // there's no structured field to populate.
      std::string_view msg = e.what();
      if (msg.find(" at ") != std::string_view::npos) throw;
      throw std::runtime_error(
          std::format("{} at {}:{}.", msg, ast.line, ast.column));
    }
  }

  Value _eval_dispatch(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;

    if (ast.original_tag == "STATEMENT"_) {
      // Cooperative cancellation: an isolate being dropped/cancelled unwinds
      // here at the next statement boundary. Null (main thread) → no-op.
      if (interrupt_flag_ &&
          interrupt_flag_->load(std::memory_order_relaxed)) {
        throw CulebraError("Interrupted", "isolate cancelled");
      }
      if (debugger_) {
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
      case "UNARY_BNOT"_:
        return eval_unary_bnot(ast, env);
      case "BIT_OR"_:
      case "BIT_XOR"_:
      case "BIT_AND"_:
      case "SHIFT"_:
        return eval_bitwise(ast, env);
      case "ADDITIVE"_:
      case "MULTIPLICATIVE"_:
        return eval_bin_expression(ast, env);
      case "RANGE"_:
        return eval_range(ast, env);
      case "RANGE_OPERATOR"_:
        // Bare `..` (full range) — the AST optimizer collapses the
        // operator-only RANGE to a lone RANGE_OPERATOR node.
        return _make_range(std::nullopt, std::nullopt, ast.token == "..=");
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
      case "TRIPLE_STRING"_:
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
      case "ENUM_DECL"_:
        return eval_enum_decl(ast, env);
      case "TRAIT_DECL"_:
        return eval_trait_decl(ast, env);
      case "MULTIFN_DECL"_:
        return eval_multifn_decl(ast, env);
      case "IMPORT_STMT"_:
        return eval_import_stmt(ast, env);
      case "EXPORT_STMT"_:
        // No-op at eval time. `run_program` extracts the export object
        // from the module's AST after the body finishes, so the binding
        // dance lives in one place.
        return Value();
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
  Value eval_statements(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
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

  Value eval_while(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
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
  Value eval_for(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    const auto& var = *ast.nodes[0];
    const auto& iter_expr = *ast.nodes[1];
    const auto& body = *ast.nodes[2];
    bool var_is_ident = var.tag == "IDENTIFIER"_;

    auto iterable = eval(iter_expr, env);
    auto iter_val = _get_iterator(iterable, iter_expr.line, iter_expr.column);
    auto iter_proto_error = [&](std::string_view what) -> CulebraError {
      return CulebraError(
          "TypeError",
          std::format("type error: {}", what),
          static_cast<long>(iter_expr.line),
          static_cast<long>(iter_expr.column));
    };
    if (iter_val.type != Value::Object) {
      throw iter_proto_error("iter() did not return an Object");
    }
    const auto& iter_obj = iter_val.to_object();
    if (!iter_obj.has("has_next") || !iter_obj.has("next")) {
      throw iter_proto_error("iterator missing has_next()/next()");
    }

    // Iterator dispose protocol: call iter.dispose() on every exit path
    // (drain / break / exception). Default trait impl is a no-op, so
    // built-in iterators pay just a method lookup; generators override
    // to run registered defers. See [[generator-design]] §dispose.
    auto dispose_iter = [&]() {
      if (iter_val.type != Value::Object) return;
      const auto& iter_obj = iter_val.to_object();
      if (!iter_obj.has("dispose")) return;
      _invoke_method_no_args(iter_val, "dispose");
    };

    // Drive via the Kotlin-style protocol: `has_next()` gates each
    // `next()`. `_iter_next_value` already encapsulates that pair.
    for (;;) {
      auto v = _iter_next_value(iter_val);
      if (!v) { dispose_iter(); break; }

      auto scopeEnv = make_scope(env);
      if (var_is_ident) {
        scopeEnv->initialize(var.token, std::move(*v), false);
      } else if (!try_pattern(var, *v, scopeEnv, /*mut=*/false)) {
        dispose_iter();
        throw_destructure_mismatch_at(static_cast<long>(var.line),
                                      static_cast<long>(var.column));
      }
      try {
        eval(body, scopeEnv);
        run_deferred(scopeEnv);
      } catch (const BreakSignal&) {
        run_deferred(scopeEnv);
        dispose_iter();
        return Value();
      } catch (const ContinueSignal&) {
        run_deferred(scopeEnv);
        // fall through (loop continues; dispose only at final exit)
      } catch (...) {
        run_deferred(scopeEnv);
        try { dispose_iter(); } catch (...) {}  // preserve in-flight throw
        throw;
      }
    }
    return Value();
  }

  Value eval_if(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
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
  // When false (a `let`-less destructure / parallel assignment), a leaf
  // binds like scalar `x = v`: assign to the existing variable, else
  // declare. When true (match arms, `let`/`mut` destructure), always
  // declare a fresh binding. Set for the duration of one pattern tree by
  // eval_destructure_assign; defaults to declare for every other caller.
  bool pattern_declare_ = true;

  void bind_pattern_name(const std::shared_ptr<Environment>& env,
                         const peg::Ast& ident_node, Value val,
                         bool mut = true) {
    auto name = ident_node.token;
    if (!pattern_declare_ && env->has(name)) {
      env->assign(name, std::move(val));
    } else {
      env->initialize(name, std::move(val), mut);
    }
  }

  // Try to match a pattern against `val`. On success, bind any introduced
  // variables into `env` and return true.
  bool try_pattern(const peg::Ast& pattern, const Value& val,
                   const std::shared_ptr<Environment>& env, bool mut = true) {
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
               val.get<long>() == parse_integer_literal(pattern.token);
      case "FLOAT"_:
        return val.type == Value::Float &&
               val.get<double>() == pattern.token_to_number<double>();
      case "STRING"_:
        return (val.type == Value::String || val.type == Value::StringView) &&
               val.to_string_view() == pattern.token;
      case "INTERPOLATED_CONTENT"_:
        return (val.type == Value::String || val.type == Value::StringView) &&
               val.to_string_view() ==
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
      case "CTOR_PATTERN"_: {
        // nodes[0] = CTOR_PATH (`Ok` / `Result.Ok`); match the variant by
        // its name (the part after any `.`), then destructure positional
        // payload fields `_0.._n` against nodes[1..].
        auto path = pattern.nodes[0]->token;
        auto dot = path.rfind('.');
        auto variant =
            dot == std::string_view::npos ? path : path.substr(dot + 1);
        if (!type_matches(val, variant) || val.type != Value::Object) {
          return false;
        }
        const auto& obj = val.to_object();
        for (size_t i = 1; i < pattern.nodes.size(); i++) {
          auto fname = culebra::positional_field_name(i - 1);
          if (!obj.has(fname)) return false;
          if (!try_pattern(*pattern.nodes[i], obj.get(fname), env, mut)) {
            return false;
          }
        }
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

  Value eval_match(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
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
      const peg::Ast& params_ast, const std::shared_ptr<Environment>& env) {
    std::vector<FunctionValue::Parameter> params;
    bool seen_default = false;
    bool kw_only = false;
    bool seen_sep = false;
    bool seen_rest = false;
    bool seen_args_rest = false;
    size_t kw_only_count = 0;
    for (auto node : params_ast.nodes) {
      auto pv = culebra::view_parameter(*node);
      if (seen_rest) {
        throw CulebraError("SyntaxError",
            "'**' catch-all must be the last parameter",
            static_cast<long>(node->line),
            static_cast<long>(node->column));
      }
      if (seen_args_rest) {
        throw CulebraError("SyntaxError",
            "'*args' must be the last parameter",
            static_cast<long>(node->line),
            static_cast<long>(node->column));
      }
      if (pv.is_kw_only_sep) {
        if (seen_sep || seen_args_rest) {
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
      if (pv.is_args_rest) {
        if (seen_sep) {
          throw CulebraError("SyntaxError",
              "'*args' cannot follow a '*' separator",
              static_cast<long>(node->line),
              static_cast<long>(node->column));
        }
        params.push_back(FunctionValue::Parameter::make_args_rest(pv.name));
        seen_args_rest = true;
        continue;
      }
      if (pv.is_kwargs_rest) {
        params.push_back({pv.name, false, {}, nullptr, {}, false, true});
        seen_rest = true;
        continue;
      }
      if (pv.pattern) {
        // Destructuring param: bind a synthetic positional slot; the
        // body entry unpacks it (see make_function_value).
        FunctionValue::Parameter pp{destructure_param_name(params.size()),
                                    false, {}, nullptr, {}, kw_only, false};
        pp.pattern = pv.pattern;
        params.push_back(pp);
        continue;
      }
      if (pv.default_value) {
        seen_default = true;
      } else if (seen_default && !kw_only) {
        throw CulebraError("SyntaxError", std::format(
            "non-default parameter '{}' follows a default parameter",
            std::string(pv.name)),
            static_cast<long>(pv.name_line),
            static_cast<long>(pv.name_col));
      }
      params.push_back({pv.name, pv.is_mut, pv.type_annotation,
                        pv.default_value, {}, kw_only});
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
                            const std::shared_ptr<Environment>& env) {
    auto params = parse_parameters(params_ast, env);
    // Destructuring params (`fn ({a, b})`) bind a synthetic slot; unpack
    // them at the body's entry.
    std::vector<std::pair<std::string_view, const peg::Ast*>> destructures;
    for (auto& p : params)
      if (p.pattern) destructures.push_back({p.name, p.pattern});
    auto self = shared_from_this();
    FunctionValue fv(
        params,
        [self = std::move(self), body, env,
         destructures](std::shared_ptr<Environment> callEnv) {
          callEnv->append_outer(env);
          for (auto& [name, pat] : destructures) {
            if (!self->try_pattern(*pat, callEnv->get(name), callEnv,
                                   /*mut=*/false)) {
              throw_destructure_mismatch_at(
                  static_cast<long>(pat->line), static_cast<long>(pat->column));
            }
          }
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
        env);
    // The eval closure above captures `env`, so this FunctionValue holds two
    // refs to `def_env`. Flag it for the cycle collector and register the
    // definition environment as a collectable node — a self-referential
    // closure forms a cycle through it that RC alone can't break.
    fv.eval_captures_def_env = true;
    // If any parameter has an expression default, give the callback binder a
    // bridge to evaluate it (the binder is a free function declared before the
    // Interpreter, so it can't call `eval` directly). Only set it when needed
    // so the common no-default closure stays a null std::function (no alloc).
    for (const auto& p : params) {
      if (p.default_expr) {
        fv.eval_default_expr =
            [self = shared_from_this()](
                const peg::Ast& expr,
                const std::shared_ptr<Environment>& scope) {
              return self->eval(expr, scope);
            };
        break;
      }
    }
    // Retain the defining AST so the isolate layer can rebuild this closure
    // on another thread (see FunctionValue::params_ast/body).
    fv.params_ast = &params_ast;
    fv.body = body;
    if (env) interp_gc().track_env(env);
    return Value(std::move(fv));
  }

  Value eval_function(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    auto fv = culebra::view_function(ast);
    return make_function_value(*fv.params, fv.body, fv.return_type, env);
  };

  // LAMBDA: [LAMBDA_PARAMS, BODY]. BODY may be a BLOCK or a bare
  // expression — both are handled by eval()'s tag dispatch.
  Value eval_lambda(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    auto fv = culebra::view_lambda(ast);
    return make_function_value(*fv.params, fv.body, fv.return_type, env);
  }

  // Dynamic type-name view for dispatch. For Object instances with a
  // `class:` String tag this aliases the stored class name; the view
  // is valid as long as `v` is.
  static std::string_view value_dyn_type(const Value& v) {
    switch (v.type) {
      case Value::Nil:        return "Nil";
      case Value::Bool:       return "Bool";
      case Value::Long:       return "Long";
      case Value::Float:      return "Float";
      case Value::String:     return "String";
      case Value::StringView: return "StringView";
      case Value::Array:      return "Array";
      case Value::Function:   return "Function";
      case Value::Tensor:     return "Tensor";
      case Value::Tuple:      return "Tuple";
      case Value::Set:        return "Set";
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
    // Pre-populate trait conformance cache so multifn_specificity can
    // resolve `fn show(x: Stringer)` style traits without holding the
    // instance.
    for (const auto& trait_name : snapshot_trait_names()) {
      for (const auto& arg : args) {
        (void)type_matches(arg, trait_name);
      }
    }
    auto r = multifn_pick(methods, arg_types,
                          [](const MultiMethod& m) -> const std::vector<std::string>& {
                            return m.param_types;
                          },
                          [](const MultiMethod& m) { return m.variadic; });
    if (r == -1) return {PickResult::NoMatch, 0};
    if (r == -2) return {PickResult::Ambiguous, 0};
    return {PickResult::Match, static_cast<size_t>(r)};
  }

  // `fn name(params) body` — first decl registers a dispatcher under
  // `name`; subsequent decls append (or overwrite a same-signature
  // entry).
  // Build a module's export Object by collecting every name listed in
  // each `EXPORT_STMT` from the module's env. Empty when the module
  // has no export statement — the resulting Object is the value
  // `import name from "..."` binds in the caller.
  Value extract_export(const peg::Ast& module_ast,
                       const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    ObjectValue obj;
    const peg::Ast* stmts =
        module_ast.tag == "STATEMENTS"_ ||
                module_ast.original_tag == "STATEMENTS"_
            ? &module_ast
            : (module_ast.nodes.empty() ? nullptr
                                         : module_ast.nodes[0].get());
    if (!stmts) return Value(std::move(obj));
    for (const auto& s : stmts->nodes) {
      if (s->tag != "EXPORT_STMT"_) continue;
      for (const auto& id : s->nodes) {
        auto name = std::string(id->token);
        if (!env->has(name)) {
          throw CulebraError(
              "NameError",
              std::format("export '{}' is not defined in module", name),
              static_cast<long>(id->line),
              static_cast<long>(id->column));
        }
        obj.properties->emplace(name, Symbol{env->get(name), false});
      }
    }
    return Value(std::move(obj));
  }

  // IMPORT_STMT: [IDENTIFIER, STRING]. The relative path is resolved
  // against the active module's directory (`module_stack_.back()`); the
  // resulting absolute path keys into `module_cache_`, which the driver
  // populated when it evaluated the dependency before this module.
  Value eval_import_stmt(const peg::Ast& ast,
                         const std::shared_ptr<Environment>& env) {
    auto name = std::string(ast.nodes[0]->token);
    auto rel = std::string(ast.nodes[1]->token);
    if (module_stack_.empty()) {
      throw CulebraError(
          "ImportError",
          "`import` is not supported in this context (REPL or direct "
          "eval); run via `culebra script.cul`");
    }
    auto canon = resolve_module_path(rel, module_stack_.back().parent_path());
    auto it = module_cache_.find(canon.string());
    if (it == module_cache_.end()) {
      throw CulebraError(
          "ImportError",
          std::format("module '{}' was not loaded — `import` statements "
                      "must be reachable from the entry point's "
                      "dependency graph", rel));
    }
    env->initialize(name, it->second, false);
    return Value();
  }

  // Lower a single type annotation slot in place to its runtime-check
  // form (see shared.h::lower_type_params). Rewritten strings are owned
  // by generic_type_storage_ so their string_views outlive the call.
  void neutralize_type_slot(
      std::string_view& slot,
      const std::vector<std::string_view>& type_params) {
    if (type_params.empty() || slot.empty()) return;
    auto rewritten = culebra::lower_type_params(slot, type_params);
    if (rewritten == slot) return;
    if (rewritten == "Any") { slot = "Any"; return; }
    auto storage = std::make_shared<std::string>(std::move(rewritten));
    generic_type_storage_.push_back(storage);
    slot = *storage;
  }

  // Lower all param + return annotations on a function signature,
  // snapshotting the original annotation into declared_type_name first
  // so introspection still surfaces `T` / `Array<T>`. Shared by class
  // methods and free (multifn) functions carrying Generic type-params.
  void neutralize_fn_type_params(
      FunctionValue& fn,
      const std::vector<std::string_view>& type_params) {
    if (type_params.empty()) return;
    for (auto& p : *fn.params) {
      p.declared_type_name = p.type_name;
      neutralize_type_slot(p.type_name, type_params);
    }
    neutralize_type_slot(fn.return_type, type_params);
  }

  Value eval_multifn_decl(const peg::Ast& ast,
                          const std::shared_ptr<Environment>& env) {
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

    // CLASS_HEAD may carry Generic params (`min<T: Comparable>`); the
    // bound name is the outer only. Mirrors eval_class_decl. The
    // type-params themselves drive neutralization below so a bare `T`
    // param lowers to "Any" (unbounded) or its bound trait (bounded).
    auto mf_head = parse_generic_head(ast.nodes[i]->token);
    auto name_view = mf_head.outer;
    auto name_owned = std::string(name_view);
    std::vector<std::string_view> type_params;
    if (!mf_head.args.empty()) type_params = split_generic_args(mf_head.args);

    // Children: CLASS_HEAD, PARAMETERS, [RETURN_TYPE,] BLOCK
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
    fn_val.get<FunctionValue>().name = std::string(name_view);
    neutralize_fn_type_params(fn_val.get<FunctionValue>(), type_params);

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
      if (p.args_rest) { method.variadic = true; continue; }
      if (p.kw_only || p.kwargs_rest) continue;
      // Canonicalize so `Long|Float` and `Long | Float` dedup to one entry.
      method.param_types.emplace_back(canonicalize_type_annotation(p.type_name));
    }

    // Accumulate onto an existing dispatcher only when one is bound in THIS
    // scope frame (has_own, not the lexical chain). That keeps same-scope
    // overloads merging into one dispatcher while a nested or redeclared
    // `fn name` in a different scope — or the same decl re-run in a fresh
    // activation — gets its own table, shadowing any outer binding. (A
    // global by-name registry conflated all of these: it skipped the env
    // bind on every decl after the first, so a re-entered `fn name` was
    // never bound in its new scope, and re-entrancy corrupted the table.)
    std::shared_ptr<std::vector<MultiMethod>> methods_ptr;
    if (env->has_own(name_owned)) {
      const auto& existing = env->get(name_owned);
      if (existing.type == Value::Function &&
          existing.get<FunctionValue>().multimethod_table) {
        methods_ptr = std::static_pointer_cast<std::vector<MultiMethod>>(
            existing.get<FunctionValue>().multimethod_table);
      }
    }

    if (methods_ptr) {
      auto& methods = *methods_ptr;
      for (size_t i = 0; i < methods.size(); i++) {
        if (methods[i].param_types == method.param_types) {
          methods[i] = std::move(method);
          // Refresh the dispatcher's introspection_target whenever the
          // FIRST method is redefined — the dispatcher exposes the
          // first method's signature via `fn.params` / `fn.return_type`,
          // and stale snapshots would diverge from the JIT path which
          // re-reads methods.front() at every introspect call.
          if (i == 0) {
            auto& disp = const_cast<Value&>(env->get(name_owned));
            disp.get<FunctionValue>().introspection_target =
                std::make_shared<FunctionValue>(
                    methods[0].body.get<FunctionValue>());
          }
          return Value();
        }
      }
      methods.push_back(std::move(method));
      return Value();
    }

    // Fresh binding: first `fn name` in this scope, or one shadowing an
    // outer decl. Build a new method table + dispatcher and bind it here.
    auto methods = std::make_shared<std::vector<MultiMethod>>();
    methods->push_back(std::move(method));

    // Synthetic `**__KWARGS__` catch-all on the dispatcher so kwargs
    // pass through to method dispatch (Julia-flavored kwsorter:
    // multimethod picks on positional types, then the picked
    // method's signature absorbs kwargs / splats).
    auto self = shared_from_this();
    auto first_method_snapshot =
        std::make_shared<FunctionValue>(fn_val.get<FunctionValue>());
    auto dispatcher = Value(FunctionValue(
        {FunctionValue::Parameter::make_kwargs_rest("__KWARGS__")},
        [self = std::move(self), methods, name_owned](std::shared_ptr<Environment> callEnv) {
          auto line = callEnv->get("__LINE__").to_long();
          auto col = callEnv->get("__COLUMN__").to_long();
          const auto& args = *callEnv->get("__ARGS__").to_array().values;
          auto pick = self->pick_method(*methods, args);
          if (pick.status == PickResult::NoMatch) {
            throw CulebraError("DispatchError", std::format(
                "no matching method for `{}`", name_owned), line, col);
          }
          if (pick.status == PickResult::Ambiguous) {
            throw CulebraError("DispatchError", std::format(
                "ambiguous dispatch for `{}`", name_owned), line, col);
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
    // Surface the first method on the dispatcher for introspection.
    // The dispatcher keeps its synthetic `__KWARGS__` params for the
    // multifn protocol; eval_property reads `introspection_target`
    // instead of `params` so callers see the user-facing signature.
    dispatcher.get<FunctionValue>().name = std::string(name_view);
    dispatcher.get<FunctionValue>().introspection_target = first_method_snapshot;
    dispatcher.get<FunctionValue>().multimethod_table =
        std::static_pointer_cast<void>(methods);
    env->initialize(name_view, dispatcher, false);
    return Value();
  }

  // `class Name { new(...) {...}  m(...) {...} }` desugars to
  //   Name = { new: fn(...) { mut this = { class: 'Name', m: fn(...){...} };
  //                           <new body>; this } }
  // `new` is optional; absent form accepts zero args and returns a
  // bare instance with only the class tag and methods. Methods close
  // over the defining scope but `this` is bound fresh per method call
  // via the existing method-dispatch protocol.
  // Zero value for a declared field type when no default is supplied
  // (`x: Float32` -> 0.0). Numeric types get their numeric zero, Bool
  // false, String "", and any reference type nil — the Go/C#/Java
  // "zero value" rule. Mirrored in the JIT backend.
  Value zero_value_for_type(std::string_view type) {
    if (type == "Float32" || type == "Float64" || type == "Float")
      return Value(0.0);
    if (type == "Bool") return Value(false);
    if (type == "String") return Value(std::string());
    if (type == "Long" || type == "Byte" || type.starts_with("Int"))
      return Value(static_cast<long>(0));
    return Value();  // reference types default to nil
  }

  // --- @packable SharedBuffer: raw bytes <-> Value bridge ---------------
  // Decode field `f` from a record's raw bytes into a primitive Value.
  // Integers widen to Long, Float32/64 to Float, Bool to Bool. memcpy
  // keeps it alignment-safe and avoids strict-aliasing UB.
  static Value packable_read_field(const uint8_t* base,
                                   const culebra::PackableField& f) {
    const uint8_t* p = base + f.offset;
    if (f.type == "Float32") {
      float v; std::memcpy(&v, p, 4);
      return Value(static_cast<double>(v));
    }
    if (f.type == "Float64" || f.type == "Float") {
      double v; std::memcpy(&v, p, 8);
      return Value(v);
    }
    if (f.type == "Int8") {
      int8_t v; std::memcpy(&v, p, 1);
      return Value(static_cast<long>(v));
    }
    if (f.type == "Int16") {
      int16_t v; std::memcpy(&v, p, 2);
      return Value(static_cast<long>(v));
    }
    if (f.type == "Int32") {
      int32_t v; std::memcpy(&v, p, 4);
      return Value(static_cast<long>(v));
    }
    if (f.type == "Int64" || f.type == "Long") {
      int64_t v; std::memcpy(&v, p, 8);
      return Value(static_cast<long>(v));
    }
    if (f.type == "Byte") {
      uint8_t v; std::memcpy(&v, p, 1);
      return Value(static_cast<long>(v));
    }
    if (f.type == "Bool") {
      uint8_t v; std::memcpy(&v, p, 1);
      return Value(v != 0);
    }
    return Value();
  }

  // Encode `val` into field `f`'s raw bytes (truncating wide integers to
  // the field width, like a C store). Numeric coercion mirrors the
  // language's implicit Long<->Float rules.
  static void packable_write_field(uint8_t* base,
                                   const culebra::PackableField& f,
                                   const Value& val) {
    uint8_t* p = base + f.offset;
    if (f.type == "Float32") {
      float v = static_cast<float>(val.to_double_coerce());
      std::memcpy(p, &v, 4); return;
    }
    if (f.type == "Float64" || f.type == "Float") {
      double v = val.to_double_coerce();
      std::memcpy(p, &v, 8); return;
    }
    if (f.type == "Int8") {
      int8_t v = static_cast<int8_t>(val.to_long());
      std::memcpy(p, &v, 1); return;
    }
    if (f.type == "Int16") {
      int16_t v = static_cast<int16_t>(val.to_long());
      std::memcpy(p, &v, 2); return;
    }
    if (f.type == "Int32") {
      int32_t v = static_cast<int32_t>(val.to_long());
      std::memcpy(p, &v, 4); return;
    }
    if (f.type == "Int64" || f.type == "Long") {
      int64_t v = static_cast<int64_t>(val.to_long());
      std::memcpy(p, &v, 8); return;
    }
    if (f.type == "Byte") {
      uint8_t v = static_cast<uint8_t>(val.to_long());
      std::memcpy(p, &v, 1); return;
    }
    if (f.type == "Bool") {
      uint8_t v = val.to_bool() ? 1 : 0;
      std::memcpy(p, &v, 1); return;
    }
  }

  // A packed view is a lightweight ObjectValue handle carrying the
  // buffer id + element index — `buf[i]`. Field reads/writes hit the
  // shared backing bytes directly (zero copy): two views of the same
  // element observe each other's writes.
  static bool is_packed_view(const Value& v) {
    return v.type == Value::Object &&
           v.to_object().has("__packedview_id__");
  }
  static bool is_shared_buffer(const Value& v) {
    return v.type == Value::Object &&
           v.to_object().has("__sharedbuffer_id__");
  }

  // Resolve a packed view to (core, record base pointer). Throws if the
  // buffer was somehow freed (no public free yet, but keeps the lookup
  // honest).
  // --- FixedArray<T,N> view: a fixed-capacity inline collection laid out as
  // `[len:i32][T × N]` inside a @packable record. `record.field` yields this
  // view; its methods + `[i]` + for-in read/write the backing bytes in place
  // (zero copy, shared across isolates with the buffer). -------------------
  struct FaView {
    std::shared_ptr<culebra::SharedBufferCore> core;
    long off;      // absolute byte offset of the field (the i32 len lives here)
    long cap;      // capacity N
    long dataoff;  // byte offset of T[0] within the field (after len)
    long esize;    // sizeof(T)
    std::string etype;
  };
  static FaView fa_resolve(const Value& view) {
    const auto& o = view.to_object();
    auto core = culebra::lookup_shared_buffer(o.get("__fa_id__").to_long());
    if (!core) throw CulebraError("ValueError", "SharedBuffer has been dropped");
    return {core, o.get("__fa_off__").to_long(), o.get("__fa_cap__").to_long(),
            o.get("__fa_dataoff__").to_long(), o.get("__fa_esize__").to_long(),
            o.get("__fa_etype__").get<std::string>()};
  }
  static long fa_len(const FaView& v) {
    int32_t n; std::memcpy(&n, v.core->data + v.off, 4); return n;
  }
  static void fa_set_len(const FaView& v, long n) {
    int32_t x = static_cast<int32_t>(n);
    std::memcpy(v.core->data + v.off, &x, 4);
  }
  static culebra::PackableField fa_elem_field(const FaView& v) {
    culebra::PackableField f;
    f.type = v.etype;
    f.offset = 0;
    return f;
  }
  static uint8_t* fa_elem_ptr(const FaView& v, long i) {
    return v.core->data + v.off + v.dataoff + i * v.esize;
  }
  static bool is_fixed_array_view(const Value& v) {
    return v.type == Value::Object && v.to_object().has("__fa_id__");
  }
  static Value fa_get(const Value& view, long i) {
    auto v = fa_resolve(view);
    long n = fa_len(v);
    if (i < 0) i += n;
    if (i < 0 || i >= n) throw CulebraError("IndexError", "index out of range");
    return packable_read_field(fa_elem_ptr(v, i), fa_elem_field(v));
  }
  static void fa_set(const Value& view, long i, const Value& val) {
    auto v = fa_resolve(view);
    long n = fa_len(v);
    if (i < 0) i += n;
    if (i < 0 || i >= n) throw CulebraError("IndexError", "index out of range");
    packable_write_field(fa_elem_ptr(v, i), fa_elem_field(v), val);
  }
  static Value make_fixed_array_view(long id, long abs_off,
                                     const culebra::PackableField& f) {
    using namespace std::literals;
    ObjectValue h;
    h.initialize("__fa_id__", Value(id), false);
    h.initialize("__fa_off__", Value(abs_off), false);
    h.initialize("__fa_cap__", Value(static_cast<long>(f.capacity)), false);
    h.initialize("__fa_dataoff__", Value(static_cast<long>(f.data_offset)), false);
    h.initialize("__fa_esize__", Value(static_cast<long>(f.elem_size)), false);
    h.initialize("__fa_etype__", Value(std::string(f.elem_type)), false);
    h.initialize("size", Value(FunctionValue({},
        [](std::shared_ptr<Environment> e) {
          return Value(fa_len(fa_resolve(e->get("this"))));
        }, "Long"sv)), false);
    h.initialize("capacity", Value(FunctionValue({},
        [](std::shared_ptr<Environment> e) {
          return Value(fa_resolve(e->get("this")).cap);
        }, "Long"sv)), false);
    h.initialize("push", Value(FunctionValue({{"v", false, ""sv}},
        [](std::shared_ptr<Environment> e) -> Value {
          auto v = fa_resolve(e->get("this"));
          long n = fa_len(v);
          if (n >= v.cap)
            throw CulebraError("IndexError",
                std::format("FixedArray is full (capacity {})", v.cap));
          packable_write_field(fa_elem_ptr(v, n), fa_elem_field(v), e->get("v"));
          fa_set_len(v, n + 1);
          return Value();
        }, "Nil"sv)), false);
    h.initialize("get", Value(FunctionValue({{"i", false, ""sv}},
        [](std::shared_ptr<Environment> e) {
          return fa_get(e->get("this"), e->get("i").to_long());
        })), false);
    h.initialize("set", Value(FunctionValue({{"i", false, ""sv}, {"v", false, ""sv}},
        [](std::shared_ptr<Environment> e) -> Value {
          fa_set(e->get("this"), e->get("i").to_long(), e->get("v"));
          return Value();
        }, "Nil"sv)), false);
    h.initialize("iter", Value(FunctionValue({},
        [](std::shared_ptr<Environment> e) {
          Value self = e->get("this");
          auto idx = std::make_shared<long>(0);
          return _make_iterator(
              [self, idx](std::shared_ptr<Environment>) -> std::optional<Value> {
                auto v = fa_resolve(self);
                if (*idx >= fa_len(v)) return _iter_step_done();
                Value e2 = fa_get(self, *idx);
                (*idx)++;
                return _iter_step_value(std::move(e2));
              });
        })), false);
    return Value(std::move(h));
  }

  std::pair<std::shared_ptr<culebra::SharedBufferCore>, uint8_t*>
  packed_view_record(const Value& view) {
    const auto& o = view.to_object();
    long id = o.get("__packedview_id__").to_long();
    long idx = o.get("__packedview_index__").to_long();
    auto core = culebra::lookup_shared_buffer(id);
    if (!core) {
      throw CulebraError("ValueError",
                         "packed view references a freed SharedBuffer");
    }
    uint8_t* base = core->data +
                    static_cast<size_t>(idx) * core->layout.stride;
    return {core, base};
  }

  Value packed_view_get(const Value& view, std::string_view name) {
    auto [core, base] = packed_view_record(view);
    const auto* f = core->layout.find(name);
    if (!f) {
      throw CulebraError(
          "AttributeError",
          std::format("@packable {} has no field `{}`", core->class_name,
                      name));
    }
    if (f->is_fixed_array) {
      const auto& o = view.to_object();
      long abs_off = o.get("__packedview_index__").to_long() *
                         static_cast<long>(core->layout.stride) +
                     static_cast<long>(f->offset);
      return make_fixed_array_view(o.get("__packedview_id__").to_long(),
                                   abs_off, *f);
    }
    return packable_read_field(base, *f);
  }

  void packed_view_set(const Value& view, std::string_view name,
                       const Value& val, size_t line, size_t col) {
    auto [core, base] = packed_view_record(view);
    const auto* f = core->layout.find(name);
    if (!f) {
      throw CulebraError(
          "AttributeError",
          std::format("@packable {} has no field `{}`", core->class_name,
                      name),
          static_cast<long>(line), static_cast<long>(col));
    }
    if (f->is_fixed_array) {
      throw CulebraError(
          "TypeError",
          std::format("cannot assign to FixedArray field `{}`; mutate it via "
                      ".push(...) / [i] = ...", name),
          static_cast<long>(line), static_cast<long>(col));
    }
    packable_write_field(base, *f, val);
  }

  Value eval_class_decl(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;

    // Optional leading DECORATOR children. Apply them to the final
    // class Object before binding. `@derive(...)` is special-cased: it
    // names traits whose methods we inject below rather than a callable.
    size_t k = 0;
    std::vector<const peg::Ast*> decorators;
    std::vector<std::string_view> derive_traits;
    bool is_packable = false;
    while (k < ast.nodes.size() && ast.nodes[k]->tag == "DECORATOR"_) {
      auto traits = culebra::view_derive(*ast.nodes[k]);
      if (!traits.empty()) {
        derive_traits.insert(derive_traits.end(), traits.begin(), traits.end());
      } else if (culebra::is_packable_decorator(*ast.nodes[k])) {
        is_packable = true;
      } else {
        decorators.push_back(ast.nodes[k].get());
      }
      k++;
    }

    // Method names and class name are kept as `string_view` into the
    // source AST (stable for the program's lifetime). This matches how
    // Environment and ObjectValue store keys, and avoids dangling
    // `string_view`s caused by moving std::strings inside captured
    // lambdas.
    //
    // CLASS_HEAD token may include Generic type parameters
    // (`Box<T>`, `Pair<K, V>`). The runtime drops them — class is
    // bound under `Box`, class_tag stores `Box`. Type parameters are
    // documentation in the MVP; param/return annotations naming a
    // type-param (e.g. `v: T`) are rewritten to "Any" below so they
    // pass type_check at invocation.
    auto class_head = parse_generic_head(ast.nodes[k]->token);
    auto class_name = class_head.outer;
    std::vector<std::string_view> type_params;
    if (!class_head.args.empty()) {
      type_params = split_generic_args(class_head.args);
    }

    // Reject CLASS_DECL directly inside another class body — declares
    // must live at top level (or inside a fn / lambda / lexical scope
    // within a method body, which are scope boundaries).
    for (size_t i = k + 1; i < ast.nodes.size(); i++) {
      auto mv = culebra::view_method(*ast.nodes[i]);
      const peg::Ast* body_node =
          (mv.is_typed_field || mv.is_field) ? mv.value : mv.body->get();
      if (body_node) reject_class_decl_in_class_body(*body_node, class_name);
    }
    const peg::Ast* new_ast = nullptr;
    std::vector<std::pair<std::string_view, Value>> method_template;
    std::vector<std::pair<std::string_view, Value>> static_template;
    std::vector<std::pair<std::string_view, Value>> static_field_template;
    // Typed instance fields (`x: Float32 = 0.0`). Declaration order is
    // the field order (the @packable layout reads it). Default value, or
    // the type's zero value when omitted.
    std::vector<std::pair<std::string_view, Value>> field_template;
    // Declared (name, type) pairs in field order — the @packable layout
    // reads this to compute byte offsets.
    std::vector<std::pair<std::string, std::string>> packable_fields;
    for (size_t i = k + 1; i < ast.nodes.size(); i++) {
      const auto& m = *ast.nodes[i];
      auto mv = culebra::view_method(m);
      if (mv.is_typed_field) {
        Value init = mv.value ? eval(*mv.value, env)
                              : zero_value_for_type(mv.type_annotation);
        field_template.push_back({mv.name, std::move(init)});
        packable_fields.push_back(
            {std::string(mv.name), std::string(mv.type_annotation)});
        continue;
      }
      if (mv.is_field) {
        culebra::require_static_field(mv, class_name);
        Value val = eval(*mv.value, env);
        static_field_template.push_back({mv.name, std::move(val)});
        continue;
      }
      if (!mv.is_static && mv.name == "new") {
        new_ast = &m;
        continue;
      }
      auto fn_val = make_function_value(*mv.params, *mv.body, {}, env);
      fn_val.get<FunctionValue>().name = std::string(mv.name);
      neutralize_fn_type_params(fn_val.get<FunctionValue>(), type_params);
      if (mv.is_static) {
        static_template.push_back({mv.name, std::move(fn_val)});
      } else {
        method_template.push_back({mv.name, std::move(fn_val)});
      }
    }

    // Inject @derive methods (after user methods so user definitions
    // win) before the instance template is frozen into build_instance.
    if (!derive_traits.empty()) {
      inject_derived_methods(method_template, class_name, derive_traits);
    }

    auto build_instance = [class_name, method_template, field_template]() {
      ObjectValue instance;
      instance.properties->emplace(
          "class", Symbol{Value(std::string(class_name)), false});
      for (const auto& [name, val] : method_template) {
        instance.properties->emplace(name, Symbol{val, false});
      }
      // Typed fields are mutable instance state (like `this.x = ...`).
      for (const auto& [name, val] : field_template) {
        instance.properties->emplace(name, Symbol{val, true});
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
      auto ctor_view = culebra::view_method(*new_ast);
      auto ctor_params = parse_parameters(*ctor_view.params, env);
      // Neutralize class type-params on the constructor signature.
      // Save the declared annotation first for introspection symmetry
      // with method params.
      for (auto& p : ctor_params) {
        p.declared_type_name = p.type_name;
        neutralize_type_slot(p.type_name, type_params);
      }
      auto body = *ctor_view.body;
      auto self = shared_from_this();
      constructor = Value(FunctionValue(
          ctor_params,
          [self = std::move(self), body, env, build_instance, promote_all_mut](
              std::shared_ptr<Environment> callEnv) {
            callEnv->append_outer(env);
            // `this` is immutable inside the constructor body — match
            // Java / Crystal / Ruby and the JIT backend. Attempts to
            // write `this = newObj` raise ImmutableError instead of
            // silently swapping the returned instance, and the
            // constructor's return value is always the originally
            // allocated object (so `this.x = ...` is the supported way
            // to populate fields). See project_constructor_semantics.md.
            auto inst = Value(build_instance());
            callEnv->initialize("this", inst, false);
            try {
              self->eval(*body, callEnv);
              self->run_deferred(callEnv);
            } catch (const ReturnValue&) {
              // Explicit `return` inside `new` is fine — we still hand
              // back the allocated instance; the returned value is
              // discarded.
              self->run_deferred(callEnv);
            } catch (...) {
              self->run_deferred(callEnv);
              throw;
            }
            promote_all_mut(inst);
            return inst;
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

    // @packable: compute the fixed C-ABI layout (throws on a non-fixed
    // field type) and register it under the class name. A hidden marker
    // property lets `SharedBuffer.new(n, Cls)` recover the class name from
    // the class value to look the layout up.
    if (is_packable) {
      auto layout =
          culebra::compute_packable_layout(class_name, packable_fields);
      culebra::register_packable_layout(std::string(class_name),
                                        std::move(layout));
    }

    ObjectValue class_obj;
    class_obj.properties->emplace("new", Symbol{constructor, false});
    if (is_packable) {
      class_obj.properties->emplace(
          "__packable__",
          Symbol{Value(std::string(class_name)), false});
    }
    for (auto& [name, fn_val] : static_template) {
      class_obj.properties->emplace(name, Symbol{std::move(fn_val), false});
    }
    for (auto& [name, val] : static_field_template) {
      class_obj.properties->emplace(name, Symbol{std::move(val), false});
    }
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

  // `trait Name { fn req(); fn def() { ... } }` — register a structural
  // contract. Required methods (no body) must be present on conforming
  // classes; default-impl methods (with body) are owned by the trait
  // and become callable on conforming instances even without a class
  // definition. No new keyword in the value environment — traits live
  // in `culebra::trait_registry()` keyed by name.
  Value eval_trait_decl(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    size_t k = 0;
    k = culebra::first_non_decorator_index(ast);
    // TRAIT_HEAD: trait name (+ optional Generic params) and optional
    // supertrait list (`trait Ord: Eq`). Supertrait methods are flattened
    // into this trait by register_trait.
    auto th = culebra::parse_trait_head(ast.nodes[k]->token);
    auto trait_name = std::string(parse_generic_head(th.name).outer);

    TraitDef def;
    def.name = trait_name;
    for (auto super : th.supertraits) def.supertraits.emplace_back(super);

    // Per-method default body storage (interp-side). Owned by the trait
    // — every conforming instance shares one FunctionValue per default.
    auto& defaults = trait_default_impls_[trait_name];
    defaults.clear();

    for (size_t i = k + 1; i < ast.nodes.size(); i++) {
      auto tv = culebra::view_trait_method(*ast.nodes[i]);
      auto params = parse_parameters(*tv.params, env);
      auto arity = culebra::regular_param_count(params);
      def.methods.push_back({std::string(tv.name), arity,
                             static_cast<bool>(tv.body)});

      if (tv.body) {
        auto fn_val =
            make_function_value(*tv.params, tv.body, tv.return_type, env);
        fn_val.get<FunctionValue>().name = std::string(tv.name);
        defaults.emplace(std::string(tv.name), std::move(fn_val));
      }
    }
    register_trait(std::move(def));
    return Value();
  }

  // `enum Name<T> { Ok(T), Err(E), None }` → a namespace object bound
  // under `Name`. Each variant becomes a constructor (payload variants,
  // `Name.Ok(v)`) or a singleton instance (nullary, `Name.None`). A
  // variant instance is tagged with `class` = variant name and `__enum`
  // = enum name, with positional payload fields `_0.._n`. type_matches
  // then accepts both `: Ok` (the variant) and `: Name` (the enum).
  Value eval_enum_decl(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    size_t k = 0;
    std::vector<const peg::Ast*> decorators;
    while (k < ast.nodes.size() && ast.nodes[k]->tag == "DECORATOR"_) {
      decorators.push_back(ast.nodes[k].get());
      k++;
    }
    // CLASS_HEAD: enum name (Generic params are stripped — documentation).
    auto enum_name = std::string(parse_generic_head(ast.nodes[k]->token).outer);

    auto make_variant_instance =
        [](const std::string& variant, const std::string& en) {
          // Fields are mutable, matching class instances (promote_all_mut)
          // and the JIT build_variant path — keeps the two backends in
          // lockstep ([[feedback-check-jit-interp-symmetry]]).
          ObjectValue inst;
          inst.properties->emplace(
              "class", Symbol{Value(std::string(variant)), true});
          inst.properties->emplace(
              "__enum", Symbol{Value(std::string(en)), true});
          return inst;
        };

    ObjectValue enum_obj;
    for (size_t i = k + 1; i < ast.nodes.size(); i++) {
      auto vv = culebra::view_variant(*ast.nodes[i]);
      std::string variant(vv.name);
      if (vv.arity == 0) {
        // Nullary variant: a singleton instance value.
        enum_obj.properties->emplace(
            variant,
            Symbol{Value(make_variant_instance(variant, enum_name)), false});
        continue;
      }
      // Payload variant: a constructor with positional params `_0.._n`.
      std::vector<FunctionValue::Parameter> params;
      for (size_t f = 0; f < vv.arity; f++) {
        FunctionValue::Parameter p;
        p.name = culebra::positional_field_name(f);
        params.push_back(p);
      }
      size_t arity = vv.arity;
      auto ctor = Value(FunctionValue(
          std::move(params),
          [variant, enum_name, arity, make_variant_instance](
              std::shared_ptr<Environment> callEnv) {
            auto inst = make_variant_instance(variant, enum_name);
            for (size_t f = 0; f < arity; f++) {
              auto fname = culebra::positional_field_name(f);
              inst.properties->emplace(
                  fname, Symbol{callEnv->get(fname), true});
            }
            return Value(std::move(inst));
          },
          {}, env));
      enum_obj.properties->emplace(variant, Symbol{std::move(ctor), false});
    }

    Value enum_val(std::move(enum_obj));
    for (auto it = decorators.rbegin(); it != decorators.rend(); ++it) {
      const auto& dec_expr = *(*it)->nodes[0];
      Value deco_val = eval(dec_expr, env);
      CallArgs args;
      args.positional.push_back(std::move(enum_val));
      args.positional_locs.emplace_back(dec_expr.line, dec_expr.column);
      enum_val = invoke_user_function_with_args(
          deco_val, env, std::move(args), dec_expr.line, dec_expr.column);
    }
    env->initialize(enum_name, std::move(enum_val), false);
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
                       const std::shared_ptr<Environment>& env,
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
      const Value& fn_val, const std::shared_ptr<Environment>& env,
      CallArgs args, size_t call_line, size_t call_column) {
    const auto& f = fn_val.to_function();
    const auto& params = *f.params;

    // Native built-in functions (namespace methods / bare globals, body ==
    // nullptr) that declare a fixed positional signature reject the wrong
    // positional count with a count-based ArityError, matching the JIT's
    // _ns_adapt path so `Math.abs(1, 2)` / `let f = Math.abs; f()` agree.
    // Value-type *methods* (is_builtin_method) are arity-checked at the call
    // site with their own message; variadic / zero-declared-param natives
    // (min/max, range, Tensor ctors — empty params reading __ARGS__, or
    // *args) carry no positional cap and are skipped. The flag is set only on
    // the root stdlib functions (see mark_strict_arity_builtins), so
    // synthesized native closures (enum/class constructors) are unaffected.
    if (f.strict_arity) {
      auto b = builtin_arity_bounds(params);
      // A pure positional builtin (all params required, no defaults / kw-only
      // / **rest) carries no kwarg metadata on the JIT side, where such a
      // closure rejects keyword/`**` arguments outright. Rich ns-methods
      // (Proc.run, FS.remove, JSON.stringify, ...) declare defaulted/kw-only
      // params and accept keywords, so only the bare positional builtins
      // (Math.abs, type_of, ...) reject here.
      bool kwarg_capable = b.min != b.max ||
                           first_kw_only_index(params).has_value();
      for (const auto& p : params) {
        if (p.kwargs_rest) kwarg_capable = true;
      }
      if (!kwarg_capable && (!args.kwargs.empty() || !args.splats.empty())) {
        throw CulebraError("TypeError",
                           "function does not accept keyword arguments",
                           static_cast<long>(call_line),
                           static_cast<long>(call_column));
      }
      if (!b.variadic) {
        long got = static_cast<long>(args.positional.size());
        if (got < b.min || got > b.max) {
          throw CulebraError("ArityError",
                             ns_fn_arity_error_message(b.max, got),
                             static_cast<long>(call_line),
                             static_cast<long>(call_column));
        }
      }
    }

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
    // kw-only slot when too many positionals were given. A `*args`
    // catch-all removes the cap entirely — overflow flows into it.
    auto cap = first_kw_only_index(params);
    throw_if_too_many_positionals(
        (cap && !has_args_rest(params)) ? static_cast<long>(*cap) : -1,
        static_cast<long>(args.positional.size()),
        static_cast<long>(call_line), static_cast<long>(call_column));

    auto callEnv = std::make_shared<Environment>(env);
    callEnv->is_function_frame = true;
    callEnv->initialize("self", fn_val, false);

    // C. Walk formal params in declaration order; consume positional
    //    first, then merged map, then default. The `**rest` catch-all
    //    is handled in step D.
    for (size_t p = 0; p < params.size(); p++) {
      if (params[p].kwargs_rest || params[p].args_rest) continue;
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
      } else if (auto dv = resolve_param_default(
                     f, p, *callEnv,
                     [this](const peg::Ast& e,
                            const std::shared_ptr<Environment>& s) {
                       return eval(e, s);
                     })) {
        // Default (expression evaluated against def_env + earlier params, or
        // a literal) — shared with the callback binder via resolve_param_default.
        v = std::move(*dv);
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

    // E. Overflow positional bind to __ARGS__ (and to a named `*args`
    //    param if declared). Extras start at the regular-param count —
    //    anything past the last positional-bindable slot flows into
    //    __ARGS__, regardless of whether the fn also declares kw-only
    //    params or a `**rest` catch-all.
    size_t extras_start = regular_param_count(params);
    ArrayValue extras;
    for (size_t idx = extras_start;
         idx < args.positional.size(); idx++) {
      extras.values->push_back(std::move(args.positional[idx]));
    }
    bind_overflow_args(params, *callEnv, Value(std::move(extras)));

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
                             const std::shared_ptr<Environment>& env, size_t total,
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
                           const std::shared_ptr<Environment>& env, const Value& val,
                           size_t call_line, size_t call_column) {
    using namespace peg::udl;
    // Built-in methods (Array/String/Set/... wrappers) parse positionally
    // and never accept kwargs — raise a clean TypeError rather than letting
    // the unconsumed kwargs surface as an opaque ArityError. Matches JIT.
    if (val.type == Value::Function) {
      const auto& fn = val.get<FunctionValue>();
      if (fn.is_builtin_method) {
        for (const auto& child : ast.nodes) {
          if (child->tag == "KWARG"_ || child->tag == "KWARG_SPLAT"_) {
            throw CulebraError("TypeError",
                std::format("built-in method '{}' does not accept keyword "
                            "arguments", fn.name.empty() ? "<builtin>" : fn.name),
                ast.line, ast.column);
          }
        }
        // Positional arity: a fixed-shape built-in on a value-typed
        // receiver rejects too few / too many args with a count-based
        // ArityError (the JIT raises the same — see jit.h). No kwargs
        // reach here, so the node count is the positional count.
        if (fn.builtin_arity_checked) {
          auto b = builtin_arity_bounds(*fn.params);
          long argc = static_cast<long>(ast.nodes.size());
          if (argc < b.min || (!b.variadic && argc > b.max)) {
            throw CulebraError("ArityError",
                builtin_arity_error_message(fn.name, b.min, b.max, argc),
                ast.line, ast.column);
          }
        }
      }
    }
    CallArgs args;
    split_call_args(ast, env, args);
    // Callable class instance: `obj(args)` dispatches to the instance's
    // `__call__` method, the twin of `__index__`. resolve_class_method
    // gates on class_tag (a plain dict holding a "__call__" key stays
    // non-callable) and honors trait-default `__call__`. The full CallArgs
    // (positional + keyword + **splat) flow into __call__, so a keyword
    // call `obj(x: 1)` binds against __call__'s parameters like any method.
    if (auto bound = resolve_class_method(val, "__call__")) {
      return invoke_user_function_with_args(*bound, env, std::move(args),
                                            call_line, call_column);
    }
    return invoke_user_function_with_args(val, env, std::move(args),
                                          call_line, call_column);
  }

  // UFCS (D / Nim style): call `fn_val` as if `receiver` were its first
  // positional argument, with the remaining arguments taken from the
  // AST's `ARGUMENTS` node. Deliberately does NOT bind `this` (UFCS is
  // a free-function call, not a method).
  Value eval_ufcs_call(const peg::Ast& args_ast,
                       const std::shared_ptr<Environment>& env,
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
                             const std::shared_ptr<Environment>& env,
                             const Value& val) {
    auto key = eval(ast, env);
    // `seq[r]` where r is a range value slices instead of a point lookup:
    // Array -> shallow copy, String/StringView -> byte-unit zero-copy view,
    // Tuple -> new tuple. Works for literal (`xs[1..3]`) and stored ranges.
    if (auto ct = class_tag(key); ct && *ct == "Range") {
      return eval_slice(val, key);
    }
    // FixedArray view `arr[i]`: read element i from the inline bytes.
    if (is_fixed_array_view(val)) {
      return fa_get(val, key.to_long());
    }
    // SharedBuffer[i]: hand back a packed view over element `i` (zero
    // copy — the view points back at the shared backing bytes).
    if (val.type == Value::Object &&
        val.to_object().has("__sharedbuffer_id__")) {
      const auto& buf = val.to_object();
      long id = buf.get("__sharedbuffer_id__").to_long();
      auto core = culebra::lookup_shared_buffer(id);
      if (!core) throw CulebraError("ValueError", "SharedBuffer has been dropped");
      long n = static_cast<long>(core->count);
      long idx = key.to_long();
      if (idx < 0) idx += n;
      if (idx < 0 || idx >= n) {
        throw CulebraError("IndexError", "index out of range");
      }
      ObjectValue view;
      view.initialize("__packedview_id__", Value(id), false);
      view.initialize("__packedview_index__", Value(idx), false);
      return Value(std::move(view));
    }
    // Object[k]: look up the Value-keyed sidecar. A class instance may
    // define `__index__(key)` to handle subscripts the sidecar misses
    // (e.g. an integer index into a wrapped collection).
    if (val.type == Value::Object) {
      const auto& obj = val.to_object();
      if (obj.has(key)) return obj.get(key);
      // Subscript overloading is a class-instance feature; a plain dict
      // miss is a KeyError even if it happens to hold an `__index__` value.
      if (class_tag(val)) {
        if (auto r = try_special(val, &key, "__index__", env)) return *r;
      }
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
      throw CulebraError("IndexError", "index out of range");
    }
    const auto& arr = val.to_array();
    auto idx = key.to_long();
    if (idx < 0) {
      idx = arr.values->size() + idx;
    }
    if (0 <= idx && idx < static_cast<long>(arr.values->size())) {
      return arr.values->at(idx);
    } else {
      throw CulebraError("IndexError", "index out of range");
    }
    return val;
  }

  // Slice `val` by a range value. An open start defaults to 0, an open end
  // to the sequence length; bounds are normalized + clamped by
  // _slice_bounds. String/StringView yield a byte-unit zero-copy view;
  // Array a shallow copy; Tuple a new tuple.
  Value eval_slice(const Value& val, const Value& range) {
    const auto& robj = range.to_object();
    const auto& sv = robj.get("start");
    const auto& ev = robj.get("end");
    bool open_end = ev.type == Value::Nil;
    bool inclusive = !open_end && robj.get("inclusive").to_bool();
    long lo = sv.type == Value::Nil ? 0 : sv.to_long();
    auto bounds = [&](size_t len) {
      long hi = open_end ? static_cast<long>(len) : ev.to_long();
      return _slice_bounds(lo, hi, inclusive, len);
    };
    if (val.is_stringlike()) {
      auto [src, base] = val.share_source_and_view();
      auto [s, e] = bounds(base.size());
      return Value(src, base.substr(s, e - s));
    }
    if (val.type == Value::Tuple) {
      const auto& elems = *val.get<TupleValue>().elements;
      auto [s, e] = bounds(elems.size());
      return Value(TupleValue(
          std::vector<Value>(elems.begin() + s, elems.begin() + e)));
    }
    const auto& arr = val.to_array();
    auto [s, e] = bounds(arr.values->size());
    ArrayValue out;
    out.values->assign(arr.values->begin() + s, arr.values->begin() + e);
    return Value(std::move(out));
  }

  // Wrap a method value (looked up from a builtin table) into a
  // Function that will bind `this` to `val` when invoked. Matches how
  // eval_property used to inline this for Object/Array methods.
  // `is_builtin` marks wrappers around primitive method tables
  // (Array/String/Set/Tuple/iterator builtins) that parse positionally
  // and reject kwargs. Namespace/user-object methods (own Function
  // properties) pass false so their kwargs/defaults still work.
  static Value _wrap_method_with_this(const Value& prop, const Value& val,
                                      bool is_builtin = true,
                                      std::string_view method_name = {}) {
    const auto& pf = prop.to_function();
    Value wrapped = Value(
        FunctionValue(*pf.params, [=](std::shared_ptr<Environment> callEnv) {
          callEnv->initialize("this", val, false);
          return pf.eval(callEnv);
        }));
    // Carry introspection metadata (name / return_type /
    // introspection_target) through the method-binding wrapper so
    // `obj.method.name` and friends report the underlying method
    // info instead of an empty anonymous view. Built-in table entries
    // carry no name on the FunctionValue itself, so adopt the looked-up
    // method name (also what the arity-error message reports).
    auto& wf = wrapped.get<FunctionValue>();
    wf.name = method_name.empty() ? pf.name : method_name;
    wf.return_type = pf.return_type;
    wf.introspection_target = pf.introspection_target;
    wf.is_builtin_method = is_builtin;
    // Arity-check genuine builtin-table methods only. The table dispatch
    // sites pass `method_name` (for the error message); trait-default and
    // other non-table wrappers don't, so they stay unchecked — keeping
    // interp symmetric with the JIT, which only knows the builtin tables
    // (value-type + dict + iterator), not trait defaults.
    wf.builtin_arity_checked = is_builtin && !method_name.empty();
    // Namespace methods (Math.abs, ...) are reached through this wrapper, so
    // carry the strict-arity flag through it or `Math.abs(1, 2)` would slip
    // past the positional check that `type_of(1, 2)` (unwrapped) hits.
    wf.strict_arity = pf.strict_arity;
    return wrapped;
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

  Value eval_property(const peg::Ast& ast, const std::shared_ptr<Environment>& env,
                      const Value& val) {
    auto name = ast.token;

    // @packable SharedBuffer handles: a packed view's `.field` reads the
    // backing bytes; a buffer's `.size`/`.count`/`.len` reports its length.
    if (val.type == Value::Object) {
      const auto& o = val.to_object();
      if (o.has("__packedview_id__")) return packed_view_get(val, name);
      if (o.has("__sharedbuffer_id__") &&
          (name == "size" || name == "count" || name == "len")) {
        return o.get("__sharedbuffer_count__");
      }
    }

    // String and StringView share the same method table; the
    // dispatch site bridges receiver type with to_string_view().
    if (val.type == Value::String || val.type == Value::StringView) {
      const auto& methods = string_builtins();
      auto it = methods.find(name);
      if (it == methods.end()) return Value();
      return _wrap_method_with_this(it->second, val, true, name);
    }

    // Set / Tuple are not ObjectValues either; same dedicated-table dispatch.
    if (val.type == Value::Set) {
      const auto& methods = set_builtins();
      auto it = methods.find(name);
      if (it == methods.end()) return Value();
      return _wrap_method_with_this(it->second, val, true, name);
    }
    if (val.type == Value::Tuple) {
      const auto& methods = tuple_builtins();
      auto it = methods.find(name);
      if (it == methods.end()) return Value();
      return _wrap_method_with_this(it->second, val, true, name);
    }

    // Function introspection: `fn.name`, `fn.params`, `fn.return_type`.
    // The `params` form is a fresh Array<Object>; mutating it does not
    // affect the function. Anonymous functions return "" for `.name`.
    // Multifn dispatchers read params/return_type from their
    // introspection_target snapshot so the dispatch-only `__KWARGS__`
    // synthetic param doesn't leak to user code.
    if (val.type == Value::Function) {
      const auto& fn = val.get<FunctionValue>();
      const auto& source = fn.introspection_target ? *fn.introspection_target : fn;
      if (name == "name") return Value(std::string(fn.name));
      if (name == "return_type") {
        return Value(canonicalize_type_annotation(source.return_type));
      }
      if (name == "params") {
        ArrayValue arr;
        for (const auto& p : *source.params) {
          // `*args` is a synthetic overflow slot, not a positional
          // parameter — omit it (the JIT meta omits it too, so both
          // backends report the same param list).
          if (p.args_rest) continue;
          ObjectValue o;
          o.initialize("name", Value(std::string(p.name)), false);
          o.initialize("mut", Value(p.mut), false);
          // Prefer the declared annotation (preserves `T` / `Array<T>`
          // on Generic class methods); fall back to type_name when
          // declared is empty (non-class, no neutralization).
          auto src = p.declared_type_name.empty() ? p.type_name
                                                  : p.declared_type_name;
          o.initialize("type", Value(canonicalize_type_annotation(src)),
                       false);
          bool has_default =
              p.default_expr != nullptr || p.default_value != nullptr;
          o.initialize("has_default", Value(has_default), false);
          o.initialize("kw_only", Value(p.kw_only), false);
          o.initialize("kwargs_rest", Value(p.kwargs_rest), false);
          arr.values->push_back(Value(std::move(o)));
        }
        return Value(std::move(arr));
      }
      return Value();
    }

    const auto& obj = val.to_object();
    if (obj.has(name)) {
      const auto& prop = obj.get(name);
      if (prop.type == Value::Function) {
        // Own Function properties (user methods, namespace methods like
        // Proc.run) accept kwargs; only builtins()-table methods don't.
        return _wrap_method_with_this(prop, val, !obj.has_own(name), name);
      }
      return prop;
    }

    // Trait default-method fallback: if this instance's class doesn't
    // define `name`, look for a registered trait that (a) has a default
    // for `name` and (b) the instance structurally conforms to.
    if (class_tag(val)) {
      for (const auto& [trait_name, default_methods] : trait_default_impls_) {
        auto it = default_methods.find(std::string(name));
        if (it == default_methods.end()) continue;
        if (type_matches(val, trait_name)) {
          return _wrap_method_with_this(it->second, val);
        }
      }
    }

    // Auto-synthesized `parameters()` on class instances: walks the
    // instance's fields and returns a flat Array of class-instance
    // Values. A user-defined `parameters` method takes precedence
    // (handled by the obj.has(name) branch above).
    if (name == "parameters" && class_tag(val)) {
      return _synthesize_parameters(val);
    }

    // Duck-typed iterator protocol fallback: any Object/Array exposing the
    // iterator interface (`next` plus `has_next`, or `iter`) gains the lazy
    // method set (map/filter/take/.../collect — see iterator_builtins()),
    // which drives the receiver via has_next()/next(). This is what lets a
    // user `iter()` result (`{has_next, next}`) chain combinators, not just
    // built-in iterators. Eager Array methods take priority above.
    if (obj.has("next") && (obj.has("has_next") || obj.has("iter"))) {
      const auto& methods = iterator_builtins();
      auto it = methods.find(name);
      if (it != methods.end()) {
        return _wrap_method_with_this(it->second, val, true, name);
      }
    }
    return Value();
  }

  // Decide whether `val` has a method/property named `name` (for
  // distinguishing "real method call" from "UFCS fallback candidate").
  // Returning false does NOT imply `val` is a non-object — primitives
  // like Long simply have no methods.
  bool receiver_has_property(const Value& val, std::string_view name) {
    if (val.type == Value::String || val.type == Value::StringView) {
      return string_builtins().count(name) > 0;
    }
    if (val.type == Value::Set) {
      return set_builtins().count(name) > 0;
    }
    if (val.type == Value::Tuple) {
      return tuple_builtins().count(name) > 0;
    }
    if (val.type == Value::Function) {
      return name == "name" || name == "params" || name == "return_type";
    }
    if (val.type == Value::Object || val.type == Value::Array) {
      if (val.to_object().has(name)) return true;
      // Synthesized `parameters()` is available on every class instance
      // (see eval_property).
      if (name == "parameters" && class_tag(val)) return true;
      // Trait default-method visibility: if a registered trait owns a
      // default named `name` and this instance conforms, the property
      // exists — block UFCS from hijacking via a same-named global fn.
      if (class_tag(val)) {
        for (const auto& [trait_name, default_methods] : trait_default_impls_) {
          if (default_methods.find(std::string(name)) ==
              default_methods.end()) continue;
          if (type_matches(val, trait_name)) return true;
        }
      }
    }
    return false;
  }

  Value eval_call(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;

    Value val = eval(*ast.nodes[0], env);

    for (auto i = 1u; i < ast.nodes.size(); i++) {
      const auto& postfix = *ast.nodes[i];

      switch (postfix.original_tag) {
        case "ARGUMENTS"_: {
          // Attribute call errors to the callee, not the arg list, so a
          // DispatchError on `f(1)` points at `f` — matching the JIT.
          auto [cl, cc] = call_callee_position(ast);
          val = eval_function_call(postfix, env, val, cl, cc);
          break;
        }
        case "INDEX"_:
          val = eval_array_reference(postfix, env, val);
          break;
        case "SAFE_DOT"_:
          // `a?.b` / `a?.m()` — optional chaining: a nil receiver
          // short-circuits the entire remaining postfix chain to nil.
          if (val.type == Value::Nil) return Value();
          [[fallthrough]];
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
        case "SAFE_INDEX"_:
          // `a?[k]` — optional index: nil receiver short-circuits to nil.
          if (val.type == Value::Nil) return Value();
          val = eval_array_reference(postfix, env, val);
          break;
        case "NONNULL"_:
          // `expr!!` — non-null assertion: nil raises NilError, any
          // other value passes through unchanged.
          if (val.type == Value::Nil) {
            throw CulebraError("NilError", "`!!` applied to nil",
                               postfix.line, postfix.column);
          }
          break;
        default:
          throw std::logic_error("invalid internal condition.");
      }
    }

    return val;
  }

  Value eval_lexical_scope(const peg::Ast& ast,
                           const std::shared_ptr<Environment>& env) {
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

  Value eval_logical_or(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
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
                         const std::shared_ptr<Environment>& env) {
    Value val;
    for (auto node : ast.nodes) {
      val = eval(*node, env);
      if (!val.to_bool()) {
        return val;
      }
    }
    return val;
  }

  // `a OPE b` or a comparison chain `a < b < c …` = `(a<b) && (b<c) …`.
  // Each middle operand is evaluated once (the rhs becomes the next lhs);
  // short-circuits to false at the first failing link (Python semantics).
  Value eval_condition(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    auto prev = eval(*ast.nodes[0], env);
    for (size_t i = 1; i + 1 < ast.nodes.size(); i += 2) {
      auto rhs = eval(*ast.nodes[i + 1], env);
      auto r = compare_values(prev, rhs, ast.nodes[i]->token, env);
      if (!r.to_bool()) return Value(false);
      prev = std::move(rhs);
    }
    return Value(true);
  }

  // Six-way comparison on pre-evaluated operands. Factored out of
  // eval_condition so the dispatch (Object `__eq__`/`__lt__` + auto
  // reflection for `==`/`!=`) is callable without re-evaluating sides.
  Value compare_values(const Value& lhs, const Value& rhs,
                       std::string_view ope,
                       const std::shared_ptr<Environment>& env) {
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

    if (ope == "==") return Value(lhs == rhs);
    if (ope == "!=") return Value(lhs != rhs);
    if (ope == "<=") return Value(lhs <= rhs);
    if (ope == "<") return Value(lhs < rhs);
    if (ope == ">=") return Value(lhs >= rhs);
    if (ope == ">") return Value(lhs > rhs);
    throw std::logic_error("invalid internal condition.");
  }

  Value eval_unary_plus(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    return eval(*ast.nodes[1], env);
  }

  Value eval_unary_minus(const peg::Ast& ast,
                         const std::shared_ptr<Environment>& env) {
    auto v = eval(*ast.nodes[1], env);
    if (auto r = try_special_unary(v, "__neg__", env)) return *r;
    if (v.type == Value::Float) return Value(-v.get<double>());
    if (v.type != Value::Long)
      culebra::throw_type_mismatch("Long or Float", v.type_name());
    return Value(v.get<long>() * -1);
  }

  Value eval_unary_not(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    return Value(!eval(*ast.nodes[1], env).to_bool());
  }

  // `~x` — bitwise complement. Integer-only (Long); Float / others are a
  // type error, matching the binary bit operators.
  Value eval_unary_bnot(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    auto v = eval(*ast.nodes[1], env);
    if (v.type != Value::Long) {
      throw CulebraError("TypeError", std::format(
          "type error: '~' requires Long, got {}", v.type_name()));
    }
    return Value(~v.get<long>());
  }

  // Bitwise / shift chains (`^`, `&`, `<<`, `>>`). Left-associative over
  // [operand, OP, operand, ...]; operands must be Long. The operator is
  // read from the captured OP token (handles the 2-char `<<` / `>>`).
  Value eval_bitwise(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    auto require_long = [&](const Value& v) -> long {
      if (v.type != Value::Long) {
        throw CulebraError("TypeError", std::format(
            "type error: bitwise operator requires Long, got {}",
            v.type_name()));
      }
      return v.get<long>();
    };
    long acc = require_long(eval(*ast.nodes[0], env));
    for (size_t i = 1; i < ast.nodes.size(); i += 2) {
      auto op = ast.nodes[i]->token;
      long rhs = require_long(eval(*ast.nodes[i + 1], env));
      if (op == "|") acc |= rhs;
      else if (op == "^") acc ^= rhs;
      else if (op == "&") acc &= rhs;
      // Shift count is taken modulo the operand width (low 6 bits), the
      // hardware/Java/C# rule. This keeps a count >= 64 (or negative)
      // well-defined — C++ would be UB — so interp and JIT agree (the JIT
      // masks the same way before its `shl`/`ashr`). `1 << 64 == 1`.
      else if (op == "<<") acc <<= (rhs & 63);
      else if (op == ">>") acc >>= (rhs & 63);
      else throw std::logic_error("invalid bitwise operator");
    }
    return Value(acc);
  }

  // Arithmetic with Long↔Float promotion. Both operands Long → Long
  // result (integer arithmetic, truncated division/modulo — matching
  // C semantics, unchanged from before Float was introduced). Either
  // operand Float → Float result via double promotion.
  // `a ?? b ?? c` returns the first non-nil operand, short-circuiting
  // on evaluation. All-nil chains return nil.
  Value eval_nil_coalesce(const peg::Ast& ast,
                          const std::shared_ptr<Environment>& env) {
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
  // A RANGE node carries `[start?] OP [end?]` — either endpoint may be
  // omitted (open-ended). The operator child is at index 0 when there is
  // no start, else index 1; an end follows it when present. (The bare `..`
  // form collapses to a lone RANGE_OPERATOR node, handled in eval().)
  Value eval_range(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    size_t op_idx = (ast.nodes[0]->tag == "RANGE_OPERATOR"_) ? 0 : 1;
    bool has_start = op_idx == 1;
    bool has_end = op_idx + 1 < ast.nodes.size();
    std::optional<long> start, end;
    if (has_start) start = eval(*ast.nodes[0], env).to_long();
    if (has_end) end = eval(*ast.nodes[op_idx + 1], env).to_long();
    return _make_range(start, end, ast.nodes[op_idx]->token == "..=");
  }

  // Resolve a method by name on a class instance, honoring both the
  // instance's own Function properties and inherited trait defaults
  // (mirrors eval_property's method lookup, which is where `__call__`
  // can live when it's a trait default). Returns the `this`-bound
  // method, or nullopt for a non-instance / missing method. Gated on
  // class_tag so a plain dict holding a "__call__" key stays an
  // ordinary value, matching the subscript-overload gate.
  std::optional<Value> resolve_class_method(const Value& val,
                                             std::string_view name) {
    if (val.type != Value::Object || !class_tag(val)) return std::nullopt;
    const auto& obj = val.to_object();
    auto it = obj.properties->find(name);
    if (it != obj.properties->end() &&
        it->second.val.type == Value::Function) {
      return _wrap_method_with_this(it->second.val, val);
    }
    for (const auto& [trait_name, default_methods] : trait_default_impls_) {
      auto dit = default_methods.find(std::string(name));
      if (dit == default_methods.end()) continue;
      if (type_matches(val, trait_name)) {
        return _wrap_method_with_this(dit->second, val);
      }
    }
    return std::nullopt;
  }

  // Shared special-method dispatch core. Arity 0 (unary) or 1 (binary);
  // the optional `rhs` is consumed only when arity == 1. Returns nullopt
  // if `receiver` isn't an Object carrying a callable special method of
  // this name.
  std::optional<Value> try_special(const Value& receiver, const Value* rhs,
                                  std::string_view special,
                                  const std::shared_ptr<Environment>& env) {
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
                                        const std::shared_ptr<Environment>& env) {
    return try_special(lhs, &rhs, special, env);
  }

  std::optional<Value> try_special_unary(const Value& operand,
                                        std::string_view special,
                                        const std::shared_ptr<Environment>& env) {
    return try_special(operand, nullptr, special, env);
  }

  // Two-argument special-method dispatch (e.g. `__setindex__(key, value)`).
  // Returns nullopt if `receiver` isn't an Object carrying a callable
  // method of this name.
  std::optional<Value> try_special2(const Value& receiver, const Value& a1,
                                    const Value& a2, std::string_view special,
                                    const std::shared_ptr<Environment>& env) {
    if (receiver.type != Value::Object) return std::nullopt;
    const auto& obj = receiver.to_object();
    auto it = obj.properties->find(special);
    if (it == obj.properties->end()) return std::nullopt;
    const auto& m = it->second.val;
    if (m.type != Value::Function) return std::nullopt;
    auto bound = _wrap_method_with_this(m, receiver);
    const Value* args[2] = {&a1, &a2};
    return invoke_user_function(
        bound, env, 2, [&](size_t i) { return *args[i]; },
        [&](size_t) -> std::pair<size_t, size_t> { return {0, 0}; }, 0, 0);
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
                         const std::shared_ptr<Environment>& env) {
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
    // String concatenation: `+` on String / StringView yields a new
    // owned String. Mixed types (e.g. String + Long) fall through to the
    // type-error branch below — use interpolation `"{x}"` for those.
    if (ope == '+' && lhs.is_stringlike() && rhs.is_stringlike()) {
      auto a = lhs.to_string_view();
      auto b = rhs.to_string_view();
      std::string out;
      out.reserve(a.size() + b.size());
      out.append(a).append(b);
      return Value(std::move(out));
    }
    // `@` has no numeric meaning, so skip the numeric path entirely;
    // reaching this point means the LHS didn't supply `__matmul__`.
    if (ope == '@' || !lhs.is_numeric() || !rhs.is_numeric()) {
      culebra::throw_arith_type_error(std::string_view(&ope, 1),
                                     lhs.type_name(), rhs.type_name());
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
                            const std::shared_ptr<Environment>& env) {
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
                      const std::shared_ptr<Environment>& env) {
    if (auto r = try_special_binop(base, exp, "__pow__", env)) return *r;
    if (!base.is_numeric() || !exp.is_numeric()) {
      throw_arith_type_error("**", base.type_name(), exp.type_name());
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

  Value eval_power(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    auto base = eval(*ast.nodes[0], env);
    auto exp = eval(*ast.nodes[2], env);
    return compute_power(base, exp, env);
  }

  // `**` has its own integer fast path (compute_power); the other ops
  // share the eval_bin_op_step dispatch. `op` is the operator without
  // the trailing `=`.
  Value apply_compound_op(const Value& lhs, const Value& rhs,
                          std::string_view op,
                          const std::shared_ptr<Environment>& env) {
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

  // DESTRUCTURE_ASSIGN children: [LET, MUTABLE, PATTERN, EXPRESSION].
  // `let`/`let mut` declares; a bare `(a, b) = …` (LET empty) reassigns
  // existing variables (parallel / swap assignment). Evaluates RHS once,
  // then reuses `try_pattern`. Mismatch is a runtime error (unlike match).
  Value eval_destructure_assign(const peg::Ast& ast,
                                const std::shared_ptr<Environment>& env) {
    bool declares = ast.nodes[0]->token == "let" || ast.nodes[1]->token == "mut";
    auto mut = ast.nodes[1]->token == "mut";
    const auto& pattern = *ast.nodes[2];
    auto rval = eval(*ast.nodes[3], env);
    auto saved = pattern_declare_;
    pattern_declare_ = declares;
    bool ok;
    try {
      ok = try_pattern(pattern, rval, env, mut);
    } catch (...) {
      pattern_declare_ = saved;
      throw;
    }
    pattern_declare_ = saved;
    if (!ok) {
      // Helper carries line:col so the structured error has the same
      // location the JIT path attaches — see shared.h.
      throw_destructure_mismatch_at(static_cast<long>(ast.line),
                                    static_cast<long>(ast.column));
    }
    return rval;
  }

  Value eval_assignment(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    // ASSIGNMENT layout (see parser.h `view_assignment`):
    //   [LET, MUTABLE, lval-chain..., (TYPE_ANNOTATION)?, ASSIGN_OP, EXPRESSION]
    auto av = culebra::view_assignment(ast);
    auto lvaloff = av.lvaloff;
    auto lvalcnt = av.lvalcnt;
    auto let = av.is_let;
    auto mut = av.is_mut;
    bool compound = av.compound;

    if (compound && (let || mut)) {
      throw CulebraError("SyntaxError",
          "compound assignment cannot declare a new variable.",
          static_cast<long>(ast.line), static_cast<long>(ast.column));
    }

    auto base_op = av.op_base;
    // `??=` (nil-coalescing assign) short-circuits: the RHS is evaluated
    // and assigned only when the current lvalue reads as nil (or is
    // missing). Defer the RHS so a non-nil lvalue skips its side effects.
    bool nil_coalesce = compound && base_op == "??";
    // `??=` is MVP-limited to a simple variable target (the short-circuit
    // write on indexed / property lvalues is a JIT-parity follow-up).
    if (nil_coalesce && lvalcnt != 1) {
      throw CulebraError("SyntaxError",
          "`??=` is only supported on a simple variable target.",
          static_cast<long>(ast.line), static_cast<long>(ast.column));
    }

    auto eval_rhs = [&]() {
      auto v = eval(*av.rhs, env);
      if (!av.type_annotation.empty()) {
        check_type(v, av.type_annotation, "assignment", ast.line, ast.column);
      }
      return v;
    };

    Value rval;
    if (!nil_coalesce) rval = eval_rhs();

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
        if (nil_coalesce) {
          if (cur.type != Value::Nil) return cur;  // short-circuit
          auto v = eval_rhs();
          env->assign(ident, v);
          return v;
        }
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
          case "ARGUMENTS"_: {
            // Attribute the call error to the chain head (the lvalue's base
            // expression), matching the rvalue path's nodes[0] and the JIT.
            // The immediately-preceding postfix would point at an inner arg
            // list for chained calls like `f()(x).y = z`.
            const auto& head = *ast.nodes[lvaloff];
            lval = eval_function_call(postfix, env, lval,
                                      head.line, head.column);
            break;
          }
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
          // FixedArray view `arr[i] = v` (and compound `arr[i] += v`): write
          // element i into the inline bytes.
          if (is_fixed_array_view(lval)) {
            long i = eval(postfix, env).to_long();
            if (compound) {
              Value cur = fa_get(lval, i);
              Value nv = apply_compound_op(cur, rval, base_op, env);
              fa_set(lval, i, nv);
              return nv;
            }
            fa_set(lval, i, rval);
            return rval;
          }
          // Whole-element assignment `buf[i] = ...` isn't supported — a
          // packed record has no Value form; write fields individually.
          if (is_shared_buffer(lval)) {
            throw CulebraError(
                "TypeError",
                "cannot assign to a SharedBuffer element directly; "
                "set fields via buf[i].field = value",
                static_cast<long>(postfix.line),
                static_cast<long>(postfix.column));
          }
          // `obj[k] = v` on an Object. String keys flow into the same
          // slot as `obj.foo`; other hashable keys live in the sidecar.
          // Existing slots honor their `mut` flag (matches the DOT path
          // above and the JIT's `object_set` behavior).
          if (lval.type == Value::Object) {
            auto key = eval(postfix, env);
            auto& obj = lval.to_object();
            if (compound) {
              if (obj.has(key)) {
                auto cur = obj.get(key);
                auto new_val = apply_compound_op(cur, rval, base_op, env);
                obj.assign(key, new_val);
                return new_val;
              }
              // `g[k] op= v` on a class instance: read via __index__ and
              // write back via __setindex__ (matches the JIT, which lowers
              // it to a plain get + set).
              if (class_tag(lval)) {
                if (auto cur = try_special(lval, &key, "__index__", env)) {
                  auto new_val = apply_compound_op(*cur, rval, base_op, env);
                  if (try_special2(lval, key, new_val, "__setindex__", env)) {
                    return new_val;
                  }
                }
              }
              throw CulebraError("KeyError",
                  "compound assignment on missing key.");
            }
            // A class instance may define `__setindex__(key, value)` to
            // handle assignment to subscripts the sidecar misses.
            if (obj.has(key)) {
              obj.assign(key, rval);
            } else if (class_tag(lval) &&
                       try_special2(lval, key, rval, "__setindex__", env)) {
              // handled by the user method
            } else {
              obj.initialize(key, rval, mut);
            }
            return rval;
          }
          const auto& arr = lval.to_array();
          auto idx = eval(postfix, env).to_long();
          if (idx < 0 || idx >= static_cast<long>(arr.values->size())) {
            throw CulebraError("IndexError", "index out of range");
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
          // `buf[i].field = v` / `view.field = v`: write straight into the
          // shared backing bytes (zero copy).
          if (is_packed_view(lval)) {
            auto name = postfix.token;
            if (compound) {
              Value cur = packed_view_get(lval, name);
              Value new_val = apply_compound_op(cur, rval, base_op, env);
              packed_view_set(lval, name, new_val, postfix.line,
                              postfix.column);
              return new_val;
            }
            packed_view_set(lval, name, rval, postfix.line, postfix.column);
            return rval;
          }
          auto& obj = lval.to_object();
          auto name = postfix.token;
          if (compound) {
            if (!obj.has_own(name)) {
              // Shared helper so the JIT path (see jit.h
              // `compile_assignment` DOT-compound branch) raises
              // the same AttributeError with location attached.
              // has_own (not has) so a builtin-named miss still errors
              // rather than compounding against a builtin method value.
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
          // has_own: a fresh field whose name shadows a builtin method
          // (`size`/`keys`/...) must initialize a new slot, not route to
          // assign() which expects an existing property.
          if (obj.has_own(name)) {
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

  Value eval_identifier(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    return env->get(ast.token);
  };

  Value eval_object(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    ObjectValue obj;
    for (auto i = 0u; i < ast.nodes.size(); i++) {
      // `{...obj}` spread: merge another Object's entries (later keys win).
      if (ast.nodes[i]->tag == "SPREAD_ELEM"_) {
        auto v = eval(*ast.nodes[i]->nodes[0], env);
        if (v.type != Value::Object) {
          throw CulebraError("TypeError",
              std::format("cannot spread {} into an object (Object only)",
                          v.type_name()),
              static_cast<long>(ast.nodes[i]->line),
              static_cast<long>(ast.nodes[i]->column));
        }
        const auto& src = v.to_object();
        // Merged keys are mutable so a later explicit `key: v` can
        // override them, and so the JIT path (object_set, which enforces
        // immutability on overwrite) stays in lockstep.
        for (const auto& [k, sym] : *src.properties) {
          obj.initialize(k, sym.val, true);
        }
        continue;
      }
      auto pv = culebra::view_object_property(*ast.nodes[i]);
      // Non-IDENTIFIER literal keys: store under Value-keyed sidecar.
      if (pv.key->tag != "IDENTIFIER"_) {
        auto key = eval(*pv.key, env);
        auto val = eval(*pv.value, env);
        obj.initialize(key, std::move(val), pv.is_mut);
        continue;
      }
      // Shorthand resolves the identifier in current scope; long form
      // evaluates the EXPRESSION node — `view.value` points to the
      // right child in either case.
      Value val = pv.is_shorthand ? env->get(pv.key->token)
                                  : eval(*pv.value, env);
      obj.initialize(pv.key->token, val, pv.is_mut);
    }
    return Value(std::move(obj));
  }

  // Append an iterable's elements to `out` for `[...x]` array spread.
  // MVP sources: Array / Tuple / Set (the common ones); other values
  // raise a TypeError.
  void spread_into_array(const Value& v, ArrayValue& out, long line,
                         long col) {
    if (v.type == Value::Array) {
      for (auto& e : *v.to_array().values) out.values->push_back(e);
    } else if (v.type == Value::Tuple) {
      for (auto& e : *v.get<TupleValue>().elements) out.values->push_back(e);
    } else if (v.type == Value::Set) {
      for (auto& e : *v.get<SetValue>().members) out.values->push_back(e);
    } else {
      throw CulebraError("TypeError",
          std::format("cannot spread {} into an array (Array/Tuple/Set only)",
                      v.type_name()), line, col);
    }
  }

  Value eval_array(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
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
    size_t idx = 0;
    for (auto& expr : nodes) {
      if (expr->tag == "SPREAD_ELEM"_) {
        auto v = eval(*expr->nodes[0], env);
        spread_into_array(v, arr, static_cast<long>(expr->line),
                          static_cast<long>(expr->column));
        idx = arr.values->size();
      } else {
        auto val = eval(*expr, env);
        if (idx < arr.values->size()) {
          arr.values->at(idx) = std::move(val);
        } else {
          arr.values->push_back(std::move(val));
        }
        idx++;
      }
    }

    return Value(std::move(arr));
  }

  Value eval_nil(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    return Value();
  };

  Value eval_bool(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    return Value(ast.token == "true");
  };

  Value eval_number(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    return Value(parse_integer_literal(ast.token));
  };

  Value eval_float(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    return Value(ast.token_to_number<double>());
  }

  // Render a value under a `{x:spec}` format spec. Numeric values honor
  // the spec's type char (`.2f` formats a Long as Float, `x` an int in
  // hex); everything else formats its display string (width / align).
  std::string apply_format_spec(const Value& v, std::string_view spec,
                                long line, long col) {
    if (spec.empty()) return str_display_with_special(v);
    if (v.type == Value::Long) {
      if (format_spec_wants_float(spec))
        return format_value_double(static_cast<double>(v.get<long>()), spec,
                                   line, col);
      return format_value_long(v.get<long>(), spec, line, col);
    }
    if (v.type == Value::Float) {
      if (format_spec_wants_int(spec))
        return format_value_long(static_cast<long>(v.get<double>()), spec,
                                 line, col);
      return format_value_double(v.get<double>(), spec, line, col);
    }
    return format_value_string(str_display_with_special(v), spec, line, col);
  }

  Value eval_interpolated_string(const peg::Ast& ast,
                                 const std::shared_ptr<Environment>& env) {
    using namespace peg::udl;
    std::string s;
    for (auto node : ast.nodes) {
      if (node->tag == "INTERPOLATED_CONTENT"_ ||
          node->tag == "TRIPLE_CONTENT"_) {
        s += decode_interpolated_content(node->token);
      } else if (node->tag == "INTERP_EXPR"_) {
        // `{expr}` → [EXPRESSION]; `{expr:spec}` → [EXPRESSION, FORMAT_SPEC].
        const auto& val = eval(*node->nodes[0], env);
        if (node->nodes.size() > 1) {
          s += apply_format_spec(val, node->nodes[1]->token,
                                 static_cast<long>(node->line),
                                 static_cast<long>(node->column));
        } else {
          s += str_display_with_special(val);
        }
      } else {
        // Defensive: a bare expression child (shouldn't occur now that
        // INTERP_EXPR is kept, but keeps old ASTs working).
        const auto& val = eval(*node, env);
        s += str_display_with_special(val);
      }
    }
    return Value(std::move(s));
  };

  void eval_return(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    if (ast.nodes.empty()) {
      throw ReturnValue{Value()};
    } else {
      throw ReturnValue{eval(*ast.nodes[0], env)};
    }
  }

  void eval_throw(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
    throw eval(*ast.nodes[0], env);
  }

  // Run deferred callables registered in `env` in LIFO order. If one
  // throws, it propagates; remaining defers for that scope are abandoned
  // (matches Swift; Go would run all, but we keep it simple).
  void run_deferred(const std::shared_ptr<Environment>& env) {
    while (!env->deferred.empty()) {
      auto fn = std::move(env->deferred.back());
      env->deferred.pop_back();
      fn();
    }
  }

  // TRY = [BLOCK, IDENTIFIER, BLOCK]  (try-body, catch-binding, catch-body)
  Value eval_try(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
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

  void eval_defer(const peg::Ast& ast, const std::shared_ptr<Environment>& env) {
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

// Drives a multi-module program: evaluates dependencies in
// topological order into fresh per-module scopes, caches each
// module's export Object, and finally evaluates the entry module
// against `env` so its top-level bindings stay visible to the
// caller. `modules.back()` is the entry. Error handling matches
// `interpret` for the single-AST path.
inline bool interpret_modules(const std::vector<LoadedModule>& orig_modules,
                              const std::shared_ptr<Environment>& env,
                              Value& val,
                              std::vector<std::string>& msgs,
                              Debugger debugger = nullptr) {
  if (orig_modules.empty()) return true;
  // Prepend a synthetic preamble module that registers the built-in
  // traits (Stringer / Eq / Comparable). The AST is cached process-
  // wide; default-method closures are constructed per-Interpreter
  // (they capture env), matching how user-declared traits behave.
  std::vector<LoadedModule> modules;
  modules.reserve(orig_modules.size() + 1);
  if (auto pre_ast = parse_builtin_traits_preamble()) {
    LoadedModule preamble;
    preamble.abs_path = "<builtin>";
    preamble.source = std::make_shared<std::string>(
        std::string(culebra::builtin_traits_preamble()));
    preamble.ast = pre_ast;
    modules.push_back(std::move(preamble));
  }
  for (const auto& m : orig_modules) modules.push_back(m);

  auto interp = std::make_shared<Interpreter>(debugger);
  auto flush_top_defers = [&] {
    while (!env->deferred.empty()) {
      auto fn = std::move(env->deferred.back());
      env->deferred.pop_back();
      try { fn(); } catch (...) {}
    }
  };
  try {
    for (const auto& m : modules) lint::check_shadow(*m.ast);

    for (size_t i = 0; i + 1 < modules.size(); ++i) {
      const auto& m = modules[i];
      interp->module_stack_.push_back(m.abs_path);
      // make_scope sets `outer`; bare Environment(parent) only sets `level`.
      auto mod_env = make_scope(env);
      try {
        interp->eval(*m.ast, mod_env);
        interp->run_deferred(mod_env);
      } catch (...) {
        interp->run_deferred(mod_env);
        interp->module_stack_.pop_back();
        throw;
      }
      interp->module_cache_[m.abs_path.string()] =
          interp->extract_export(*m.ast, mod_env);
      interp->module_stack_.pop_back();
    }

    // Entry runs against the caller-supplied env so REPL / embedding
    // sessions can read its top-level bindings afterwards.
    const auto& entry = modules.back();
    interp->module_stack_.push_back(entry.abs_path);
    val = interp->eval(*entry.ast, env);
    interp->module_stack_.pop_back();
    flush_top_defers();
    return true;
  } catch (const ReturnValue& r) {
    val = r.value;
    flush_top_defers();
    return true;
  } catch (const Value& e) {
    flush_top_defers();
    msgs.push_back(std::format("uncaught: {}", e.str_display()));
  } catch (const CulebraError& e) {
    flush_top_defers();
    if (e.line > 0 || e.col > 0) {
      msgs.push_back(std::format("{}: {} at {}:{}.",
                                  e.kind, e.what(), e.line, e.col));
    } else {
      msgs.push_back(std::format("{}: {}", e.kind, e.what()));
    }
  } catch (const std::runtime_error& e) {
    flush_top_defers();
    msgs.push_back(std::format("RuntimeError: {}", e.what()));
  }
  return false;
}

inline bool interpret(const std::shared_ptr<peg::Ast>& ast,
                      const std::shared_ptr<Environment>& env, Value& val,
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
    lint::check_shadow(*ast);
    // Held by shared_ptr so FunctionValues created during eval (which
    // capture `shared_from_this()`) can keep the Interpreter alive
    // past this call's stack scope — see comment on `Interpreter`.
    auto interp = std::make_shared<Interpreter>(debugger);
    // Built-in trait preamble: run once before user code so REPL /
    // single-AST callers (which bypass interpret_modules) also see
    // Stringer / Eq / Comparable. trait_registry is process-wide but
    // each Interpreter needs its own default-method closures (they
    // capture env), so we eval the preamble per Interpreter.
    if (auto pre = parse_builtin_traits_preamble()) {
      try { interp->eval(*pre, env); } catch (...) {}
    }
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
    // Mirror main.cc's CulebraError formatter: append the source
    // location from the structured fields when present so both
    // backends produce identical uncaught-error output.
    if (e.line > 0 || e.col > 0) {
      msgs.push_back(std::format("{}: {} at {}:{}.",
                                  e.kind, e.what(), e.line, e.col));
    } else {
      msgs.push_back(std::format("{}: {}", e.kind, e.what()));
    }
  } catch (const std::runtime_error& e) {
    flush_top_defers();
    msgs.push_back(std::format("RuntimeError: {}", e.what()));
  }

  return false;
}

// Resolve a lazy-registered module: parse + interpret the source against
// this env so the placeholder binding is replaced by the real value. The
// pending entry is removed before evaluation to short-circuit re-entry
// if the source references the same name. The source is anchored in
// `lazy_module_sources` so AST tokens (string_views into it) stay valid
// for the lifetime of the env — class methods bound during eval may be
// invoked long after this call returns.
inline void Environment::resolve_from_lazy(
    std::string_view name, std::shared_ptr<std::string> source) {
  // Group lazy bind: a prior eval through a sibling name may have already
  // bound this symbol via the same source. Detect by non-Nil dictionary
  // entry + skip parse to avoid redundant work.
  if (auto it = dictionary.find(name); it != dictionary.end()) {
    if (it->second.val.type != Value::Nil) {
      lazy_pending.erase(std::string(name));
      return;
    }
  }
  lazy_pending.erase(std::string(name));
  lazy_module_sources.push_back(source);
  std::vector<std::string> parse_msgs;
  auto vpath = std::format("<lazy-{}>", name);
  auto ast = parse_with_transforms(vpath, source->data(), source->size(),
                                   parse_msgs);
  if (!ast) {
    std::fprintf(stderr, "culebra: lazy module '%s' failed to parse\n",
                 std::string(name).c_str());
    for (auto& m : parse_msgs) std::fprintf(stderr, "  %s", m.c_str());
    std::abort();
  }
  Value dummy;
  std::vector<std::string> eval_msgs;
  if (!interpret(ast, shared_from_this(), dummy, eval_msgs)) {
    std::fprintf(stderr, "culebra: lazy module '%s' failed to eval\n",
                 std::string(name).c_str());
    for (auto& m : eval_msgs) std::fprintf(stderr, "  %s", m.c_str());
    std::abort();
  }
}

// --- Cycle collector implementation ---
// (Defined here so Value is complete.)

inline void InterpGC::track_env(const std::shared_ptr<Environment>& e) {
  if (!e || e->gc_tracked) return;
  e->gc_tracked = true;
  push_entry(std::shared_ptr<void>(e, e.get()), /*is_env=*/true);
}

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
    bool is_env;
    std::shared_ptr<void> sp;
  };
  std::vector<Live> live;
  live.reserve(entries_.size());
  for (auto& e : entries_) {
    if (auto sp = e.weak.lock())
      live.push_back({e.ptr, e.is_env, std::move(sp)});
  }

  // Per-node state: gc_refs = use_count - 1 (our local copy in `live`), plus
  // the node kind so the BFS can dispatch walk_node without a second map.
  struct Node {
    long refs;
    bool is_env;
  };
  std::unordered_map<void*, Node> gc_refs;
  gc_refs.reserve(live.size());
  for (auto& l : live) gc_refs[l.ptr] = {l.sp.use_count() - 1, l.is_env};

  // Walk a node's edges to OTHER tracked nodes (ValVecs + Environments),
  // invoking emit(child_ptr, multiplicity). A ValVec yields its element
  // Values; an Environment yields its bindings' Values plus its `outer`.
  // For each Value: Array -> its tracked ValVec; Function -> its def_env
  // (multiplicity 2 when `eval` also captures it — see FunctionValue); and
  // Object/Tuple/Set are descended ONE level (untracked containers) to
  // surface the tracked nodes they hold. The one-level cap matches the old
  // walk and bounds recursion through untracked-container cycles. Missing an
  // edge only under-counts gc_refs (a bounded leak); only OVER-counting could
  // free a live node, which is why the Function multiplicity must be exact.
  auto walk_node = [&](void* ptr, bool is_env, auto&& emit) {
    auto walk_value = [&emit](const Value& val, bool descend, auto&& wv) -> void {
      switch (val.type) {
        case Value::Array:
          emit(val.template get<ArrayValue>().values.get(), 1L);
          break;
        case Value::Function: {
          auto& fv = val.template get<FunctionValue>();
          if (fv.def_env)
            emit(fv.def_env.get(), fv.eval_captures_def_env ? 2L : 1L);
          break;
        }
        case Value::Object:
          if (descend)
            for (auto& [k, sym] : *val.template get<ObjectValue>().properties)
              wv(sym.val, false, wv);
          break;
        case Value::Tuple:
          if (descend)
            for (auto& e : *val.template get<TupleValue>().elements)
              wv(e, false, wv);
          break;
        case Value::Set:
          if (descend)
            for (auto& e : *val.template get<SetValue>().members)
              wv(e, false, wv);
          break;
        default:
          break;
      }
    };
    if (is_env) {
      auto* e = static_cast<Environment*>(ptr);
      for (auto& [k, sym] : e->dictionary) walk_value(sym.val, true, walk_value);
      if (e->outer) emit(e->outer.get(), 1L);
    } else {
      for (auto& val : *static_cast<ValVec*>(ptr))
        walk_value(val, true, walk_value);
    }
  };

  // Subtract internal references.
  for (auto& l : live) {
    walk_node(l.ptr, l.is_env, [&](void* p, long mult) {
      auto it = gc_refs.find(p);
      if (it != gc_refs.end()) it->second.refs -= mult;
    });
  }

  // Mark external roots and BFS-propagate reachability.
  std::unordered_set<void*> reachable;
  std::queue<void*> q;
  for (auto& [ptr, n] : gc_refs) {
    if (n.refs > 0) {
      reachable.insert(ptr);
      q.push(ptr);
    }
  }
  auto mark = [&](void* c, long) {
    if (reachable.insert(c).second && gc_refs.contains(c)) q.push(c);
  };
  while (!q.empty()) {
    void* p = q.front();
    q.pop();
    walk_node(p, gc_refs.find(p)->second.is_env, mark);
  }

  // Break cycles by clearing unreachable nodes. The shared_ptr cascade does
  // the rest, including each freed PropMap's custom deleter (which fires
  // `drop`). Clearing an Environment drops its bindings + outer chain.
  for (auto& l : live) {
    if (reachable.contains(l.ptr)) continue;
    if (l.is_env) {
      auto* e = static_cast<Environment*>(l.ptr);
      e->dictionary.clear();
      e->outer.reset();
    } else {
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
  // Live-object accounting: matches the bump in ObjectValue's ctor.
  auto& gc = interp_gc();
  gc.live_objects--;
  gc.live_bytes -= static_cast<int64_t>(sizeof(OrderedSymbolMap));
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
