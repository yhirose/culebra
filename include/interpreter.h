#pragma once

#include <parser.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <map>
#include <print>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace culebra {

struct Value;
struct Symbol;
struct Environment;

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
struct InterpGC {
  enum Kind : int { MAP = 0, VEC = 1 };

  struct Entry {
    std::weak_ptr<void> weak;
    void* ptr;
    Kind kind;
  };

  static InterpGC& instance() {
    static InterpGC g;
    return g;
  }

  ~InterpGC() { collect(); }

  template <typename T>
  void track_map(std::shared_ptr<T> p) {
    entries_.push_back({std::weak_ptr<void>(std::shared_ptr<void>(p, p.get())),
                        p.get(), MAP});
    bump();
  }
  template <typename T>
  void track_vec(std::shared_ptr<T> p) {
    entries_.push_back({std::weak_ptr<void>(std::shared_ptr<void>(p, p.get())),
                        p.get(), VEC});
    bump();
  }

  void collect();

 private:
  std::vector<Entry> entries_;
  size_t alloc_counter_ = 0;
  size_t threshold_ = 10000;
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
  };

  FunctionValue(
      const std::vector<Parameter>& params,
      const std::function<Value(std::shared_ptr<Environment> env)>& eval,
      std::string_view return_type = {})
      : params(std::make_shared<std::vector<Parameter>>(params)),
        eval(eval),
        return_type(return_type) {}

  std::shared_ptr<std::vector<Parameter>> params;
  std::function<Value(std::shared_ptr<Environment> env)> eval;
  std::string_view return_type;  // empty = no annotation
};

struct ObjectValue {
  ObjectValue()
      : properties(std::make_shared<std::map<std::string_view, Symbol>>()) {
    interp_gc().track_map(properties);
  }
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
  enum Type { Nil, Bool, Long, String, Object, Array, Function };

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
  explicit Value(std::string&& s) : type(String), v(s) {}
  explicit Value(ObjectValue&& o) : type(Object), v(o) {}
  explicit Value(ArrayValue&& a) : type(Array), v(a) {}
  explicit Value(FunctionValue&& f) : type(Function), v(f) {}

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
    if (type != rhs.type) return false;
    switch (type) {
      case Nil:
        return true;
      case Bool:
        return get<bool>() == rhs.get<bool>();
      case Long:
        return get<long>() == rhs.get<long>();
      case String:
        return get<std::string>() == rhs.get<std::string>();
      // TODO: Object and Array support
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
  }

  bool operator!=(const Value& rhs) const { return !operator==(rhs); }

  bool operator<=(const Value& rhs) const {
    if (type != rhs.type) throw std::runtime_error("type error.");
    switch (type) {
      case Nil:
        return false;
      case Bool:
        return get<bool>() <= rhs.get<bool>();
      case Long:
        return get<long>() <= rhs.get<long>();
      case String:
        return get<std::string>() <= rhs.get<std::string>();
      // TODO: Object and Array support
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
  }

  bool operator<(const Value& rhs) const {
    if (type != rhs.type) throw std::runtime_error("type error.");
    switch (type) {
      case Nil:
        return false;
      case Bool:
        return get<bool>() < rhs.get<bool>();
      case Long:
        return get<long>() < rhs.get<long>();
      case String:
        return get<std::string>() < rhs.get<std::string>();
      // TODO: Object and Array support
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
  }

  bool operator>=(const Value& rhs) const {
    if (type != rhs.type) throw std::runtime_error("type error.");
    switch (type) {
      case Nil:
        return false;
      case Bool:
        return get<bool>() >= rhs.get<bool>();
      case Long:
        return get<long>() >= rhs.get<long>();
      case String:
        return get<std::string>() >= rhs.get<std::string>();
      // TODO: Object and Array support
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
  }

  bool operator>(const Value& rhs) const {
    if (type != rhs.type) throw std::runtime_error("type error.");
    switch (type) {
      case Nil:
        return false;
      case Bool:
        return get<bool>() > rhs.get<bool>();
      case Long:
        return get<long>() > rhs.get<long>();
      case String:
        return get<std::string>() > rhs.get<std::string>();
      // TODO: Object and Array support
      default:
        throw std::logic_error("invalid internal condition.");
    }
    std::unreachable();
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

  // Used for shadow-prohibition: declaring a new `let`/`mut` binding
  // that shadows a closure-captured variable (from an enclosing function)
  // is an error. Block-scope shadowing within the same function is
  // allowed, as is shadowing a global (builtin or top-level binding).
  bool would_shadow_capture(std::string_view s) const {
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

inline void ObjectValue::assign(std::string_view name, const Value& val) {
  assert(has(name));
  auto& sym = properties->at(name);
  if (!sym.mut) {
    std::string msg = "immutable property '";
    msg += name;
    msg += "'...";
    throw std::runtime_error(msg);
  }
  sym.val = val;
  return;
}

inline void ObjectValue::initialize(std::string_view name, const Value& val,
                                    bool mut) {
  (*properties)[name] = Symbol{val, mut};
}

// Runtime type check for optional annotations. "Any" matches everything.
inline bool type_matches(const Value& val, std::string_view name) {
  if (name == "Any") return true;
  switch (val.type) {
    case Value::Nil:      return name == "Nil";
    case Value::Bool:     return name == "Bool";
    case Value::Long:     return name == "Long";
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
  std::string s = "{";
  bool first = true;
  for (const auto& [name, sym] : properties) {
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
           }))}};
  return props_;
}

#include <stdlib_interp.h>

inline std::shared_ptr<Environment> environment() {
  auto env = std::make_shared<Environment>();
  setup_built_in_functions(*env);
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
      case "IF"_:
        return eval_if(ast, env);
      case "MATCH"_:
        return eval_match(ast, env);
      case "FUNCTION"_:
        return eval_function(ast, env);
      case "CALL"_:
        return eval_call(ast, env);
      case "LEXICAL_SCOPE"_:
        return eval_lexical_scope(ast, env);
      case "ASSIGNMENT"_:
        return eval_assignment(ast, env);
      case "LOGICAL_OR"_:
        return eval_logical_or(ast, env);
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
      case "TRY"_:
        return eval_try(ast, env);
      case "DEFER"_:
        eval_defer(ast, env);
        return Value();
    }

    if (ast.is_token) {
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
      eval(*ast.nodes[1], env);
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
  void bind_pattern_name(std::shared_ptr<Environment>& env,
                         const peg::Ast& ident_node, Value val) {
    auto name = ident_node.token;
    if (env->would_shadow_capture(name)) {
      throw_shadow_error(name, ident_node.line, ident_node.column);
    }
    env->initialize(name, std::move(val), true);
  }

  // Try to match a pattern against `val`. On success, bind any introduced
  // variables into `env` and return true.
  bool try_pattern(const peg::Ast& pattern, const Value& val,
                   std::shared_ptr<Environment> env) {
    using namespace peg::udl;

    // PATTERN with multiple children is an OR pattern.
    if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
      for (const auto& sub : pattern.nodes) {
        if (try_pattern(*sub, val, env)) return true;
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
      case "STRING"_:
      case "INTERPOLATED_CONTENT"_:
        return val.type == Value::String &&
               val.get<std::string>() == std::string(pattern.token);
      case "IDENTIFIER"_: {
        bind_pattern_name(env, pattern, val);
        return true;
      }
      case "TYPED_IDENT"_: {
        auto type_name = pattern.nodes[1]->token;
        if (!type_matches(val, type_name)) return false;
        bind_pattern_name(env, *pattern.nodes[0], val);
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
            if (!try_pattern(*elems[i], items[i], env)) return false;
          }
          return true;
        }

        // With rest: require at least fixed element count
        auto fixed = elems.size() - 1;
        if (items.size() < fixed) return false;
        // Match pre-rest fixed elements
        for (int i = 0; i < rest_idx; i++) {
          if (!try_pattern(*elems[i], items[i], env)) return false;
        }
        // Collect rest into new Array
        auto rest_len = items.size() - fixed;
        ArrayValue rest;
        rest.values->reserve(rest_len);
        for (size_t j = 0; j < rest_len; j++) {
          rest.values->push_back(items[rest_idx + j]);
        }
        bind_pattern_name(env, *elems[rest_idx]->nodes[0],
                          Value(std::move(rest)));
        // Match post-rest fixed elements
        for (size_t i = rest_idx + 1; i < elems.size(); i++) {
          auto src_idx = items.size() - (elems.size() - i);
          if (!try_pattern(*elems[i], items[src_idx], env)) return false;
        }
        return true;
      }
      case "OBJECT_PATTERN"_: {
        if (val.type != Value::Object) return false;
        const auto& obj = val.to_object();
        for (const auto& key_node : pattern.nodes) {
          auto key = key_node->token;
          if (!obj.has(key)) return false;
          bind_pattern_name(env, *key_node, obj.get(key));
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

  Value eval_function(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    std::vector<FunctionValue::Parameter> params;
    for (auto node : ast.nodes[0]->nodes) {
      auto mut = node->nodes[0]->token == "mut";
      auto& id = *node->nodes[1];
      const auto& name = id.token;
      auto type_name = extract_type_annotation(*node, 2);
      if (env->def_scope_captures(name)) {
        throw_shadow_error(name, id.line, id.column);
      }
      params.push_back({name, mut, type_name});
    }

    size_t body_idx = 1;
    auto return_type = extract_return_type(ast, body_idx);
    auto body = ast.nodes[body_idx];

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
        return_type));
  };

  Value eval_function_call(const peg::Ast& ast,
                           std::shared_ptr<Environment> env, const Value& val) {
    const auto& f = val.to_function();
    const auto& params = *f.params;
    const auto& args = ast.nodes;

    if (params.size() <= args.size()) {
      auto callEnv = std::make_shared<Environment>(env);
      callEnv->is_function_frame = true;
      callEnv->initialize("self", val, false);
      for (auto iprm = 0u; iprm < params.size(); iprm++) {
        auto param = params[iprm];
        auto arg = args[iprm];
        auto val = eval(*arg, env);
        check_type(val, param.type_name,
                   std::format("parameter '{}'", param.name),
                   arg->line, arg->column);
        callEnv->initialize(param.name, val, param.mut);
      }
      // Bind extra positional args to __ARGS__ for variadic built-ins.
      if (args.size() > params.size()) {
        ArrayValue extras;
        for (auto i = params.size(); i < args.size(); i++) {
          extras.values->push_back(eval(*args[i], env));
        }
        callEnv->initialize("__ARGS__", Value(std::move(extras)), false);
      }
      callEnv->initialize("__LINE__", Value((long)ast.line), false);
      callEnv->initialize("__COLUMN__", Value((long)ast.column), false);
      Value result;
      try {
        result = f.eval(callEnv);
      } catch (const ReturnValue& r) {
        result = r.value;
      }
      check_type(result, f.return_type, "return value", ast.line, ast.column);
      return result;
    }

    std::string msg = "arguments error...";
    throw std::runtime_error(msg);
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

  Value eval_property(const peg::Ast& ast, std::shared_ptr<Environment> env,
                      const Value& val) {
    auto name = ast.token;

    // String is a primitive; its methods live in a separate table.
    if (val.type == Value::String) {
      const auto& methods = string_builtins();
      auto it = methods.find(name);
      if (it == methods.end()) return Value();
      const auto& pf = it->second.to_function();
      return Value(
          FunctionValue(*pf.params, [=](std::shared_ptr<Environment> callEnv) {
            callEnv->initialize("this", val, false);
            return pf.eval(callEnv);
          }));
    }

    const auto& obj = val.to_object();
    if (!obj.has(name)) {
      return Value();
    }
    const auto& prop = obj.get(name);
    if (prop.type == Value::Function) {
      const auto& pf = prop.to_function();
      return Value(
          FunctionValue(*pf.params, [=](std::shared_ptr<Environment> callEnv) {
            callEnv->initialize("this", val, false);
            return pf.eval(callEnv);
          }));
    }
    return prop;
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
        case "DOT"_:
          val = eval_property(postfix, env, val);
          break;
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
    return Value(eval(*ast.nodes[1], env).to_long() * -1);
  }

  Value eval_unary_not(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    return Value(!eval(*ast.nodes[1], env).to_bool());
  }

  Value eval_bin_expression(const peg::Ast& ast,
                            std::shared_ptr<Environment> env) {
    auto ret = eval(*ast.nodes[0], env).to_long();
    for (auto i = 1u; i < ast.nodes.size(); i += 2) {
      auto val = eval(*ast.nodes[i + 1], env).to_long();
      auto ope = eval(*ast.nodes[i], env).to_string()[0];
      switch (ope) {
        case '+':
          ret += val;
          break;
        case '-':
          ret -= val;
          break;
        case '*':
          ret *= val;
          break;
        case '%':
          ret %= val;
          break;
        case '/':
          if (val == 0) {
            throw std::runtime_error("divide by 0 error");
          }
          ret /= val;
          break;
      }
    }
    return Value(ret);
  }

  bool is_keyword(std::string_view ident) const {
    using namespace std::literals;
    static std::set<std::string_view> keywords = {
        "nil"sv,    "true"sv,  "false"sv, "mut"sv,    "debugger"sv,
        "return"sv, "while"sv, "if"sv,    "else"sv,   "fn"sv,
        "match"sv,  "throw"sv, "try"sv,   "catch"sv,  "defer"sv};
    return keywords.contains(ident);
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
      auto val = eval(*prop.nodes[2], env);
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

  Value eval_interpolated_string(const peg::Ast& ast,
                                 std::shared_ptr<Environment> env) {
    std::string s;
    for (auto node : ast.nodes) {
      const auto& val = eval(*node, env);
      if (val.type == Value::String) {
        s += val.to_string();
      } else {
        s += val.str();
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
  using PropMap = std::map<std::string_view, Symbol>;
  using ValVec = std::vector<Value>;

  // Prune expired, lock live, and track via typed shared_ptrs.
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                [](auto& e) { return e.weak.expired(); }),
                 entries_.end());

  struct Live {
    void* ptr;
    Kind kind;
    std::shared_ptr<void> sp;  // void-typed for type erasure
  };
  std::vector<Live> live;
  live.reserve(entries_.size());
  for (auto& e : entries_) {
    if (auto sp = e.weak.lock()) {
      live.push_back({e.ptr, e.kind, std::move(sp)});
    }
  }

  // gc_refs: use_count - 1 (our local), then subtract internal incoming refs
  std::unordered_map<void*, long> gc_refs;
  gc_refs.reserve(live.size());
  for (auto& l : live) gc_refs[l.ptr] = l.sp.use_count() - 1;

  auto dec_if_tracked = [&](void* p) {
    auto it = gc_refs.find(p);
    if (it != gc_refs.end()) --it->second;
  };
  auto decrement_for_value = [&](const Value& val) {
    if (val.type == Value::Object) {
      dec_if_tracked(val.template get<ObjectValue>().properties.get());
    } else if (val.type == Value::Array) {
      const auto& av = val.template get<ArrayValue>();
      dec_if_tracked(av.properties.get());
      dec_if_tracked(av.values.get());
    }
  };

  auto each_child_value = [&](void* ptr, Kind k, auto&& fn) {
    if (k == MAP) {
      auto* m = static_cast<PropMap*>(ptr);
      for (auto& [_, sym] : *m) fn(sym.val);
    } else {
      auto* v = static_cast<ValVec*>(ptr);
      for (auto& val : *v) fn(val);
    }
  };

  // Subtract internal references
  for (auto& l : live) {
    each_child_value(l.ptr, l.kind, decrement_for_value);
  }

  // Mark external roots and propagate reachability
  std::unordered_set<void*> reachable;
  std::queue<void*> q;
  for (auto& [ptr, r] : gc_refs) {
    if (r > 0) {
      reachable.insert(ptr);
      q.push(ptr);
    }
  }

  std::unordered_map<void*, Kind> kind_by_ptr;
  kind_by_ptr.reserve(live.size());
  for (auto& l : live) kind_by_ptr[l.ptr] = l.kind;

  while (!q.empty()) {
    auto* p = q.front();
    q.pop();
    auto kit = kind_by_ptr.find(p);
    if (kit == kind_by_ptr.end()) continue;
    each_child_value(p, kit->second, [&](const Value& val) {
      auto mark = [&](void* c) {
        auto [_, inserted] = reachable.insert(c);
        if (inserted && gc_refs.contains(c)) q.push(c);
      };
      if (val.type == Value::Object) {
        mark(val.template get<ObjectValue>().properties.get());
      } else if (val.type == Value::Array) {
        const auto& av = val.template get<ArrayValue>();
        mark(av.properties.get());
        mark(av.values.get());
      }
    });
  }

  // Break cycles: clear unreachable containers. shared_ptr cascade handles
  // the rest.
  for (auto& l : live) {
    if (!reachable.contains(l.ptr)) {
      if (l.kind == MAP)
        static_cast<PropMap*>(l.ptr)->clear();
      else
        static_cast<ValVec*>(l.ptr)->clear();
    }
  }

  running_ = false;
}

}  // namespace culebra
