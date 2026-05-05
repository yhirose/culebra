#pragma once

#include <num_format.h>
#include <parser.h>
#include <random_state.h>
#include <unicodelib.h>
#include <unicodelib_encodings.h>
#include <well_known_props.h>

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
#include <vector>

namespace culebra {

struct Value;
struct Symbol;
struct Environment;

// RAII drop helpers; defined at the bottom of this header where
// Environment and Value are complete. See the docblock at their
// definitions for semantics.
inline void _call_drop_if_present(
    std::map<std::string_view, Symbol>* m);
inline void _destroy_prop_map(
    std::map<std::string_view, Symbol>* m);

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
  throw std::runtime_error(std::format(
      "cannot shadow outer variable '{}' (declared in an enclosing function) "
      "at {}:{}.",
      name, line, column));
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

  static InterpGC& instance() {
    static InterpGC g;
    return g;
  }

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
  ObjectValue() {
    auto* raw = new std::map<std::string_view, Symbol>();
    properties = std::shared_ptr<std::map<std::string_view, Symbol>>(
        raw, &_destroy_prop_map);
    // properties map is intentionally not GC-tracked — see the InterpGC
    // class header. Cycles are detected via the contained ArrayValues.
  }

  // Synthetic ctor: skips default map allocation and GC tracking so that
  // _call_drop_if_present can build a `this` view over an existing map
  // without extra bookkeeping. Caller must assign `properties` itself.
  struct Synthetic {};
  explicit ObjectValue(Synthetic) {}

  bool has(std::string_view name) const;
  const Value& get(std::string_view name) const;
  void assign(std::string_view name, const Value& val);
  void initialize(std::string_view name, const Value& val, bool mut);
  virtual std::map<std::string_view, Value>& builtins();

  std::shared_ptr<std::map<std::string_view, Symbol>> properties;
};

struct ArrayValue : public ObjectValue {
  ArrayValue() : values(std::make_shared<std::vector<Value>>()) {
    interp_gc().track_vec(values);
  }
  std::map<std::string_view, Value>& builtins() override;

  std::shared_ptr<std::vector<Value>> values;
};

struct Value {
  enum Type { Nil, Bool, Long, Float, String, Object, Array, Function };

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
  explicit Value(FunctionValue&& f) : type(Function), v(f) {}

  bool is_numeric() const { return type == Long || type == Float; }

  double to_double_coerce() const {
    switch (type) {
      case Long:
        return static_cast<double>(get<long>());
      case Float:
        return get<double>();
      default:
        throw std::runtime_error("type error.");
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
        throw std::runtime_error("type error.");
    }
  }

  long to_long() const {
    switch (type) {
      // case Bool: return get<bool>();
      case Long:
        return get<long>();
      default:
        throw std::runtime_error("type error.");
    }
  }

  std::string to_string() const {
    switch (type) {
      case String:
        return get<std::string>();
      default:
        throw std::runtime_error("type error.");
    }
  }

  FunctionValue to_function() const {
    switch (type) {
      case Function:
        return get<FunctionValue>();
      default:
        throw std::runtime_error("type error.");
    }
  }

  const ObjectValue& to_object() const {
    switch (type) {
      case Object:
        return get<ObjectValue>();
      case Array:
        return get<ArrayValue>();
      default:
        throw std::runtime_error("type error.");
    }
  }

  ObjectValue& to_object() {
    switch (type) {
      case Object:
        return get<ObjectValue>();
      case Array:
        return get<ArrayValue>();
      default:
        throw std::runtime_error("type error.");
    }
  }

  const ArrayValue& to_array() const {
    switch (type) {
      case Array:
        return get<ArrayValue>();
      default:
        throw std::runtime_error("type error.");
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
      case Function:
        return "[function]";
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
    if (type != rhs.type) throw std::runtime_error("type error.");
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

inline std::ostream& operator<<(std::ostream& os, const Value& val) {
  return val.out(os);
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
    if (dictionary.contains(s)) {
      return true;
    }
    return outer && outer->has(s);
  }

  // `_` is a non-binding sink: any binding form (`let _`, `for _ in`,
  // `fn(_, _, x)`, `match { _ => }`) drops the value instead of
  // introducing a name. Centralizing the check in Environment means
  // every bind site (initialize / would_shadow_capture /
  // def_scope_captures) inherits the rule for free.
  static bool is_sink(std::string_view s) { return s == "_"; }

  // Used for shadow-prohibition: declaring a new `let`/`mut` binding
  // that shadows a closure-captured variable (from an enclosing function)
  // is an error. Block-scope shadowing within the same function is
  // allowed, as is shadowing a global (builtin or top-level binding).
  bool would_shadow_capture(std::string_view s) const {
    if (is_sink(s)) return false;
    // Walk through same-function block envs until we reach the current
    // function's frame, then skip past it. Anything found beyond that
    // (excluding the global root) is a closure-captured binding.
    const Environment* e = this;
    while (e && !e->is_function_frame) {
      e = e->outer.get();
    }
    if (!e) return false;  // top-level execution — no enclosing function
    e = e->outer.get();
    while (e && e->outer) {
      if (e->dictionary.contains(s)) return true;
      e = e->outer.get();
    }
    return false;
  }

  // Shadow-check helper for function-literal parameters: at definition
  // time, determine whether `name` would end up shadowing a captured
  // variable once the function is called. Walks this env (the
  // definition scope) and its outer chain, stopping before the global
  // root.
  bool def_scope_captures(std::string_view s) const {
    if (is_sink(s)) return false;
    const Environment* e = this;
    while (e && e->outer) {
      if (e->dictionary.contains(s)) return true;
      e = e->outer.get();
    }
    return false;
  }

  const Value& get(std::string_view s) const {
    if (dictionary.contains(s)) {
      return dictionary.at(s).val;
    } else if (outer) {
      return outer->get(s);
    }
    std::string msg = "undefined variable '";
    msg += s;
    msg += "'...";
    throw std::runtime_error(msg);
  }

  void assign(std::string_view s, Value val) {
    assert(has(s));
    if (dictionary.contains(s)) {
      auto& sym = dictionary[s];
      if (!sym.mut) {
        std::string msg = "immutable variable '";
        msg += s;
        msg += "'...";
        throw std::runtime_error(msg);
      }
      sym.val = std::move(val);
      return;
    }
    outer->assign(s, std::move(val));
    return;
  }

  void initialize(std::string_view s, Value val, bool mut) {
    if (is_sink(s)) return;
    dictionary[s] = Symbol{std::move(val), mut};
  }

  size_t level;
  std::shared_ptr<Environment> outer;
  std::map<std::string_view, Symbol> dictionary;
  bool is_function_frame = false;
  // Deferred callables registered in this scope via `defer { ... }`.
  // Fired in LIFO order when the scope exits (normally or via throw).
  std::vector<std::function<void()>> deferred;
};

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

// Validate the well-known-property contract (see well_known_props.h)
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
    std::string msg = "immutable property '";
    msg += name;
    msg += "'...";
    throw std::runtime_error(msg);
  }
  _check_drop_contract(name, val);
  sym.val = val;
  return;
}

inline void ObjectValue::initialize(std::string_view name, const Value& val,
                                    bool mut) {
  _check_drop_contract(name, val);
  (*properties)[name] = Symbol{val, mut};
}

// Runtime type check for optional annotations. "Any" matches everything.
inline bool type_matches(const Value& val, std::string_view name) {
  if (name == "Any") return true;
  switch (val.type) {
    case Value::Nil:      return name == "Nil";
    case Value::Bool:     return name == "Bool";
    case Value::Long:     return name == "Long";
    case Value::Float:    return name == "Float";
    case Value::String:   return name == "String";
    case Value::Array:    return name == "Array";
    case Value::Object:   return name == "Object";
    case Value::Function: return name == "Function";
  }
  return false;
}

inline void check_type(const Value& val, std::string_view name,
                       std::string_view context, size_t line, size_t col) {
  if (name.empty()) return;
  if (type_matches(val, name)) return;
  throw std::runtime_error(std::format(
      "type error: {} expects {} at {}:{}.", context, name, line, col));
}

inline std::string Value::str_object() const {
  const auto& obj = to_object();
  auto* key = obj.properties.get();
  StrGuard guard(key);
  if (guard.already) return "{...}";
  const auto& properties = *obj.properties;
  // If the object carries a String `class:` tag (class-sugar instance
  // or any user-built Object that uses the same convention), hoist the
  // tag to a prefix so `{class: 'Matrix', rows: 2}` prints as
  // `Matrix {rows: 2}` — cleaner at a glance and emphasises the nominal
  // type. The `class` entry itself is skipped in the property list to
  // avoid repeating information.
  std::string s;
  auto it = properties.find("class");
  bool has_class_tag = it != properties.end() &&
                       it->second.val.type == String;
  if (has_class_tag) {
    s = it->second.val.template get<std::string>();
    s += " ";
  }
  s += "{";
  bool first = true;
  for (const auto& [name, sym] : properties) {
    if (has_class_tag && name == "class") continue;
    if (!first) {
      s += ", ";
    }
    first = false;
    if (sym.mut) {
      s += "mut ";
    }
    s += name;
    s += ": ";
    s += sym.val.str();
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
inline std::optional<std::string> _try_str_dunder(const Value& v) {
  if (v.type != Value::Object) return std::nullopt;
  const auto& obj = v.to_object();
  if (!obj.has("__str__")) return std::nullopt;
  const auto& m = obj.get("__str__");
  if (m.type != Value::Function) return std::nullopt;
  auto r = _invoke_method_no_args(v, "__str__");
  if (r.type != Value::String) {
    throw std::runtime_error("__str__ must return a String");
  }
  return r.template get<std::string>();
}

// Like `v.str_display()` (unquoted strings) but honors `__str__` on
// Object — used by interpolation, `print`, and `to_string`.
inline std::string str_display_with_dunder(const Value& v) {
  if (auto r = _try_str_dunder(v)) return *r;
  return v.str_display();
}

// Like `v.str()` (quoted strings) but honors `__str__` on Object —
// used by `puts`. Objects with `__str__` return the custom form with
// no extra quoting regardless.
inline std::string str_quoted_with_dunder(const Value& v) {
  if (auto r = _try_str_dunder(v)) return *r;
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
  }
  if (iter_fn.type != Value::Function) {
    throw std::runtime_error(std::format(
        "type error: target is not iterable at {}:{}.", line, col));
  }
  return iter_fn.to_function().eval(
      _make_method_call_env(iterable, line, col));
}

inline std::map<std::string_view, Value>& ObjectValue::builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"size"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& val = callEnv->get("this");
         long n = val.to_object().properties->size();
         return Value(n);
       }))},
      {"keys"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& val = callEnv->get("this");
         const auto& props = *val.to_object().properties;
         ArrayValue arr;
         arr.values->reserve(props.size());
         for (const auto& [k, _] : props) {
           arr.values->push_back(Value(std::string(k)));
         }
         return Value(std::move(arr));
       }))},
      {"has"sv, Value(FunctionValue({{"key", false, "String"sv}},
                                    [](std::shared_ptr<Environment> callEnv) {
                                      const auto& val = callEnv->get("this");
                                      const auto& k =
                                          callEnv->get("key").to_string();
                                      return Value(
                                          val.to_object().properties->contains(
                                              k));
                                    }))},
      {"remove"sv,
       Value(FunctionValue({{"key", false, "String"sv}},
                           [](std::shared_ptr<Environment> callEnv) {
                             const auto& val = callEnv->get("this");
                             const auto& k = callEnv->get("key").to_string();
                             val.to_object().properties->erase(k);
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
                                 throw std::runtime_error(
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
           throw std::runtime_error("type error: min of empty Array.");
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
           throw std::runtime_error("type error: max of empty Array.");
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
         const auto& val = callEnv->get("this");
         auto values = val.to_array().values;  // shared_ptr copy
         auto index = std::make_shared<size_t>(0);
         return _make_iterator([values, index](std::shared_ptr<Environment>) {
           if (*index >= values->size()) return _iter_step_done();
           auto v = (*values)[*index];
           (*index)++;
           return _iter_step_value(std::move(v));
         });
       }))}};
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
         auto s = callEnv->get("this").to_string();
         size_t start = 0;
         while (start < s.size() &&
                std::isspace(static_cast<unsigned char>(s[start]))) {
           start++;
         }
         size_t end = s.size();
         while (end > start &&
                std::isspace(static_cast<unsigned char>(s[end - 1]))) {
           end--;
         }
         return Value(s.substr(start, end - start));
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
           throw std::runtime_error("type error: min of empty Iterator.");
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
           throw std::runtime_error("type error: max of empty Iterator.");
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

#include <stdlib_interp.h>

inline std::shared_ptr<Environment> environment(
    const std::vector<std::string>& argv = {}) {
  auto env = std::make_shared<Environment>();
  setup_built_in_functions(*env, argv);
  return env;
}

struct Interpreter {
  Interpreter(Debugger debugger = nullptr) : debugger_(debugger) {}

  Value eval(const peg::Ast& ast, std::shared_ptr<Environment> env) {
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

    // Shadow rule: a for-binding introduces a new name, so it must not
    // shadow a closure-captured variable in an enclosing function.
    if (env->def_scope_captures(var_name)) {
      throw_shadow_error(var_name, id.line, id.column);
    }

    auto iterable = eval(iter_expr, env);
    auto iter_val = _get_iterator(iterable, iter_expr.line, iter_expr.column);
    if (iter_val.type != Value::Object) {
      throw std::runtime_error(std::format(
          "type error: iter() did not return an Object at {}:{}.",
          iter_expr.line, iter_expr.column));
    }

    for (;;) {
      // step = iterator.next()
      const auto& iter_obj = iter_val.to_object();
      if (!iter_obj.has("next")) {
        throw std::runtime_error(std::format(
            "type error: iterator has no next() at {}:{}.",
            iter_expr.line, iter_expr.column));
      }
      auto next_fn = iter_obj.get("next");
      if (next_fn.type != Value::Function) {
        throw std::runtime_error(std::format(
            "type error: iterator.next is not a Function at {}:{}.",
            iter_expr.line, iter_expr.column));
      }
      auto step = next_fn.to_function().eval(_make_method_call_env(
          iter_val, iter_expr.line, iter_expr.column));
      if (step.type != Value::Object) {
        throw std::runtime_error(std::format(
            "type error: next() did not return an Object at {}:{}.",
            iter_expr.line, iter_expr.column));
      }
      const auto& step_obj = step.to_object();
      if (step_obj.has("done") && step_obj.get("done").to_bool()) {
        break;
      }
      Value loop_val = step_obj.has("value") ? step_obj.get("value") : Value();

      auto scopeEnv = std::make_shared<Environment>();
      scopeEnv->append_outer(env);
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
    if (env->would_shadow_capture(name)) {
      throw_shadow_error(name, ident_node.line, ident_node.column);
    }
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
        for (const auto& key_node : pattern.nodes) {
          auto key = key_node->token;
          if (!obj.has(key)) return false;
          bind_pattern_name(env, *key_node, obj.get(key), mut);
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
      auto armEnv = std::make_shared<Environment>();
      armEnv->append_outer(env);
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
    for (auto node : params_ast.nodes) {
      auto mut = node->nodes[0]->token == "mut";
      auto& id = *node->nodes[1];
      const auto& name = id.token;
      auto type_name = extract_type_annotation(*node, 2);
      auto* default_expr = extract_default_expr(*node);
      if (default_expr) {
        seen_default = true;
      } else if (seen_default) {
        throw std::runtime_error(std::format(
            "non-default parameter '{}' follows a default parameter at {}:{}.",
            std::string(name), id.line, id.column));
      }
      if (env->def_scope_captures(name)) {
        throw_shadow_error(name, id.line, id.column);
      }
      params.push_back({name, mut, type_name, default_expr});
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
    return Value(FunctionValue(
        params,
        [=, this](std::shared_ptr<Environment> callEnv) {
          callEnv->append_outer(env);
          try {
            auto r = eval(*body, callEnv);
            run_deferred(callEnv);
            return r;
          } catch (...) {
            run_deferred(callEnv);
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

  // `class Name { new(...) {...}  m(...) {...} }` desugars to
  //   Name = { new: fn(...) { mut this = { class: 'Name', m: fn(...){...} };
  //                           <new body>; this } }
  // `new` is optional; absent form accepts zero args and returns a
  // bare instance with only the class tag and methods. Methods close
  // over the defining scope but `this` is bound fresh per method call
  // via the existing method-dispatch protocol.
  Value eval_class_decl(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    // Method names and class name are kept as `string_view` into the
    // source AST (stable for the program's lifetime). This matches how
    // Environment and ObjectValue store keys, and avoids dangling
    // `string_view`s caused by moving std::strings inside captured
    // lambdas.
    auto class_name = ast.nodes[0]->token;

    const peg::Ast* new_ast = nullptr;
    std::vector<std::pair<std::string_view, Value>> method_template;
    for (size_t i = 1; i < ast.nodes.size(); i++) {
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
      constructor = Value(FunctionValue(
          ctor_params,
          [=, this](std::shared_ptr<Environment> callEnv) {
            callEnv->append_outer(env);
            callEnv->initialize("this", Value(build_instance()), true);
            try {
              eval(*body, callEnv);
              run_deferred(callEnv);
            } catch (const ReturnValue&) {
              // Explicit `return` inside `new` is fine — we still hand
              // back `this`; the returned value is discarded.
              run_deferred(callEnv);
            } catch (...) {
              run_deferred(callEnv);
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
    env->initialize(class_name, Value(std::move(class_obj)), false);
    return Value();
  }

  // Shared backbone for invoking a user function. `arg_value(i)` /
  // `arg_loc(i)` abstract where each of the `total` effective arguments
  // comes from — lazily evaluating AST args for a direct call, or
  // returning a pre-evaluated receiver at index 0 for a UFCS call.
  // `call_line`/`call_column` point at the call site for error messages.
  template <typename ArgVal, typename ArgLoc>
  Value invoke_user_function(const Value& fn_val,
                             std::shared_ptr<Environment> env, size_t total,
                             ArgVal arg_value, ArgLoc arg_loc,
                             size_t call_line, size_t call_column) {
    const auto& f = fn_val.to_function();
    const auto& params = *f.params;

    // A parameter with a default is optional; all others are required.
    size_t required = 0;
    for (const auto& p : params) {
      if (!p.default_expr) required++;
    }
    if (total < required) {
      throw std::runtime_error("arguments error...");
    }

    auto callEnv = std::make_shared<Environment>(env);
    callEnv->is_function_frame = true;
    callEnv->initialize("self", fn_val, false);

    for (size_t p = 0; p < params.size(); p++) {
      Value v;
      size_t ln = call_line, col = call_column;
      if (p < total) {
        v = arg_value(p);
        std::tie(ln, col) = arg_loc(p);
      } else {
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
      }
      check_type(v, params[p].type_name,
                 std::format("parameter '{}'", params[p].name), ln, col);
      callEnv->initialize(params[p].name, v, params[p].mut);
    }

    // Overflow args (incl. variadic built-ins' sink) bind to __ARGS__.
    // Always bind — the Array is empty when no overflow — so a body that
    // checks `__ARGS__.size()` works whether or not the caller passes
    // extras. This matches the JIT's unified calling convention.
    ArrayValue extras;
    for (size_t idx = params.size(); idx < total; idx++) {
      extras.values->push_back(arg_value(idx));
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

  Value eval_function_call(const peg::Ast& ast,
                           std::shared_ptr<Environment> env, const Value& val) {
    const auto& args = ast.nodes;
    return invoke_user_function(
        val, env, args.size(),
        [&](size_t i) { return eval(*args[i], env); },
        [&](size_t i) -> std::pair<size_t, size_t> {
          return {args[i]->line, args[i]->column};
        },
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
    const auto& args = args_ast.nodes;
    return invoke_user_function(
        fn_val, env, 1 + args.size(),
        [&](size_t i) {
          return i == 0 ? receiver : eval(*args[i - 1], env);
        },
        [&](size_t i) -> std::pair<size_t, size_t> {
          return i == 0 ? std::pair{dot_line, dot_column}
                        : std::pair{args[i - 1]->line, args[i - 1]->column};
        },
        args_ast.line, args_ast.column);
  }

  Value eval_array_reference(const peg::Ast& ast,
                             std::shared_ptr<Environment> env,
                             const Value& val) {
    const auto& arr = val.to_array();
    auto idx = eval(ast, env).to_long();
    if (idx < 0) {
      idx = arr.values->size() + idx;
    }
    if (0 <= idx && idx < static_cast<long>(arr.values->size())) {
      return arr.values->at(idx);
    } else {
      throw std::logic_error("index out of range.");
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

    const auto& obj = val.to_object();
    if (obj.has(name)) {
      const auto& prop = obj.get(name);
      if (prop.type == Value::Function) {
        return _wrap_method_with_this(prop, val);
      }
      return prop;
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
    if (val.type == Value::Object || val.type == Value::Array) {
      return val.to_object().has(name);
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
    auto scopeEnv = std::make_shared<Environment>();
    scopeEnv->append_outer(env);
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

    // Dunder dispatch for Object LHS. A class that defines just
    // `__eq__` and `__lt__` gets the full six-way suite: `__le__` is
    // derived as `lt || eq`, and the three "greater" ops are negations
    // of the corresponding "less-or-equal" / "less" forms.
    auto bool_val = [&](const Value& v) {
      return v.type == Value::Bool ? v.template get<bool>() : v.to_bool();
    };
    auto le_as_bool = [&]() -> std::optional<bool> {
      if (auto le = try_dunder_binop(lhs, rhs, "__le__", env))
        return bool_val(*le);
      auto lt = try_dunder_binop(lhs, rhs, "__lt__", env);
      auto eq = try_dunder_binop(lhs, rhs, "__eq__", env);
      if (!lt && !eq) return std::nullopt;
      return (lt && bool_val(*lt)) || (eq && bool_val(*eq));
    };
    // Auto-reflection: `==` is commutative, so if the LHS doesn't
    // carry `__eq__` but the RHS does, reach over and use it. Ordering
    // operators (`<`, `<=`) are not commutative and don't reflect.
    auto try_eq = [&]() -> std::optional<Value> {
      if (auto r = try_dunder_binop(lhs, rhs, "__eq__", env)) return r;
      if (auto r = try_dunder_binop(rhs, lhs, "__eq__", env)) return r;
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
        if (auto r = try_dunder_binop(lhs, rhs, "__lt__", env))
          return Value(bool_val(*r));
      } else if (ope == "<=") {
        if (auto r = le_as_bool()) return Value(*r);
      } else if (ope == ">") {
        if (auto r = le_as_bool()) return Value(!*r);
      } else if (ope == ">=") {
        if (auto r = try_dunder_binop(lhs, rhs, "__lt__", env))
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
    if (auto r = try_dunder_unary(v, "__neg__", env)) return *r;
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

  // Shared dunder-dispatch core. Arity 0 (unary) or 1 (binary); the
  // optional `rhs` is consumed only when arity == 1. Returns nullopt if
  // `receiver` isn't an Object carrying a callable dunder of this name.
  std::optional<Value> try_dunder(const Value& receiver, const Value* rhs,
                                  std::string_view dunder,
                                  std::shared_ptr<Environment> env) {
    if (receiver.type != Value::Object) return std::nullopt;
    const auto& obj = receiver.to_object();
    auto it = obj.properties->find(dunder);
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

  std::optional<Value> try_dunder_binop(const Value& lhs, const Value& rhs,
                                        std::string_view dunder,
                                        std::shared_ptr<Environment> env) {
    return try_dunder(lhs, &rhs, dunder, env);
  }

  std::optional<Value> try_dunder_unary(const Value& operand,
                                        std::string_view dunder,
                                        std::shared_ptr<Environment> env) {
    return try_dunder(operand, nullptr, dunder, env);
  }

  static const char* arith_op_to_dunder(char ope) {
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

  // Auto-reflection only fires for operators where `rhs.__op__(lhs)`
  // still computes the mathematically-correct `lhs op rhs` — i.e.,
  // commutative arithmetic. Non-commutative ops (`-`, `/`, `%`, `@`,
  // `**`) require the LHS to carry the dunder. Reflection is only
  // attempted after the LHS-side dunder lookup fails.
  static bool op_reflects(char ope) {
    return ope == '+' || ope == '*';
  }

  Value eval_bin_op_step(const Value& lhs, const Value& rhs, char ope,
                         std::shared_ptr<Environment> env) {
    if (auto* dunder = arith_op_to_dunder(ope)) {
      if (auto r = try_dunder_binop(lhs, rhs, dunder, env)) return *r;
      if (op_reflects(ope)) {
        if (auto r = try_dunder_binop(rhs, lhs, dunder, env)) return *r;
      }
    }
    // `@` has no numeric meaning, so skip the numeric path entirely;
    // reaching this point means the LHS didn't supply `__matmul__`.
    if (ope == '@' || !lhs.is_numeric() || !rhs.is_numeric()) {
      throw std::runtime_error("type error.");
    }
    // Integer fast path: both Long.
    if (lhs.type == Value::Long && rhs.type == Value::Long) {
      auto a = lhs.get<long>();
      auto b = rhs.get<long>();
      switch (ope) {
        case '+': return Value(a + b);
        case '-': return Value(a - b);
        case '*': return Value(a * b);
        case '/':
          if (b == 0) throw std::runtime_error("divide by 0 error");
          return Value(a / b);
        case '%':
          if (b == 0) throw std::runtime_error("divide by 0 error");
          return Value(a % b);
      }
      throw std::logic_error("invalid arithmetic operator");
    }
    // Mixed or both-Float path: promote to double.
    auto a = lhs.to_double_coerce();
    auto b = rhs.to_double_coerce();
    switch (ope) {
      case '+': return Value(a + b);
      case '-': return Value(a - b);
      case '*': return Value(a * b);
      case '/':
        // Follow Python: float divide-by-zero also raises.
        if (b == 0.0) throw std::runtime_error("divide by 0 error");
        return Value(a / b);
      case '%':
        if (b == 0.0) throw std::runtime_error("divide by 0 error");
        return Value(std::fmod(a, b));
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
  Value eval_power(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    auto base = eval(*ast.nodes[0], env);
    auto exp = eval(*ast.nodes[2], env);
    if (auto r = try_dunder_binop(base, exp, "__pow__", env)) return *r;
    if (!base.is_numeric() || !exp.is_numeric()) {
      throw std::runtime_error("type error.");
    }
    if (base.type == Value::Long && exp.type == Value::Long) {
      auto a = base.get<long>();
      auto b = exp.get<long>();
      if (b >= 0) {
        // Integer exponentiation by squaring; wraps on overflow to
        // match the rest of Long arithmetic (no bignum).
        long result = 1, acc = a;
        long e = b;
        while (e > 0) {
          if (e & 1) result *= acc;
          e >>= 1;
          if (e > 0) acc *= acc;
        }
        return Value(result);
      }
      // Negative integer exponent: promote to float so `2 ** -1 == 0.5`.
      if (a == 0) throw std::runtime_error("divide by 0 error");
      return Value(std::pow(static_cast<double>(a), static_cast<double>(b)));
    }
    auto x = base.to_double_coerce();
    auto y = exp.to_double_coerce();
    return Value(std::pow(x, y));
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
      throw std::runtime_error("destructure pattern did not match value");
    }
    return rval;
  }

  Value eval_assignment(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    using namespace peg::udl;
    auto lvaloff = 2;
    auto lvalcnt = ast.nodes.size() - 3;

    // Optional TYPE_ANNOTATION appears just before the trailing EXPRESSION.
    auto type_name =
        extract_type_annotation(ast, ast.nodes.size() - 2);
    if (!type_name.empty()) lvalcnt--;

    auto let = ast.nodes[0]->token == "let";
    auto mut = ast.nodes[1]->token == "mut";
    auto rval = eval(*ast.nodes.back(), env);

    if (!type_name.empty()) {
      check_type(rval, type_name, "assignment", ast.line, ast.column);
    }

    if (lvalcnt == 1) {
      const auto& ident = ast.nodes[lvaloff]->token;
      if (is_keyword(ident)) {
        throw std::runtime_error("left-hand side is invalid variable name.");
      }
      auto declare = let || mut;
      if (declare) {
        if (env->would_shadow_capture(ident)) {
          auto& ident_node = *ast.nodes[lvaloff];
          throw_shadow_error(ident, ident_node.line, ident_node.column);
        }
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
          const auto& arr = lval.to_array();
          auto idx = eval(postfix, env).to_long();
          if (0 <= idx && idx < static_cast<long>(arr.values->size())) {
            arr.values->at(idx) = rval;
          } else {
            throw std::logic_error("index out of range.");
          }
          return rval;
        }
        case "DOT"_: {
          auto& obj = lval.to_object();
          auto name = postfix.token;
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
    ObjectValue obj;
    for (auto i = 0u; i < ast.nodes.size(); i++) {
      const auto& prop = *ast.nodes[i];
      auto mut = prop.nodes[0]->token == "mut";
      const auto& name = prop.nodes[1]->token;
      // Shorthand: name resolves in current scope (error path mirrors
      // bare identifier).
      Value val;
      if (prop.nodes.size() < 3) {
        val = env->get(name);
      } else {
        val = eval(*prop.nodes[2], env);
      }
      _check_drop_contract(name, val);
      obj.properties->emplace(name, Symbol{std::move(val), mut});
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
        s += str_display_with_dunder(val);
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
    auto tryEnv = std::make_shared<Environment>();
    tryEnv->append_outer(env);
    Value tryResult;
    bool threw = false;
    Value thrown;
    try {
      tryResult = eval(*ast.nodes[0], tryEnv);
    } catch (const Value& e) {
      threw = true;
      thrown = e;
    } catch (...) {
      run_deferred(tryEnv);
      throw;
    }
    run_deferred(tryEnv);
    if (!threw) return tryResult;

    auto catchEnv = std::make_shared<Environment>();
    catchEnv->append_outer(env);
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
    env->deferred.push_back([this, body, wenv]() {
      auto e = wenv.lock();
      if (!e) return;
      auto scopeEnv = std::make_shared<Environment>();
      scopeEnv->append_outer(e);
      // A `return` inside a defer body exits only the defer closure,
      // not the enclosing function. This matches the JIT's semantics
      // (the defer body compiles to its own LLVM function whose `ret`
      // stays local).
      try {
        eval(*body, scopeEnv);
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
    val = Interpreter(debugger).eval(*ast, env);
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
  } catch (std::runtime_error& e) {
    flush_top_defers();
    msgs.push_back(e.what());
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
inline void _destroy_prop_map(
    std::map<std::string_view, Symbol>* m) {
  if (!m) return;
  _call_drop_if_present(m);
  delete m;
}

inline void _call_drop_if_present(
    std::map<std::string_view, Symbol>* m) {
  if (!m) return;
  auto it = m->find("drop");
  if (it == m->end()) return;
  if (it->second.val.type != Value::Function) return;

  const auto& fn = it->second.val.template get<FunctionValue>();
  if (!fn.params->empty()) return;

  ObjectValue this_view(ObjectValue::Synthetic{});
  this_view.properties =
      std::shared_ptr<std::map<std::string_view, Symbol>>(
          m, [](std::map<std::string_view, Symbol>*) {});

  try {
    fn.eval(_make_method_call_env(Value(std::move(this_view)), 0, 0));
  } catch (const std::exception& e) {
    std::cerr << "drop: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "drop: unknown error" << std::endl;
  }
}

}  // namespace culebra
