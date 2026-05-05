#pragma once

#include <parser.h>

#include <algorithm>
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

// Cycle collector for ObjectValue/ArrayValue (shared_ptr-based).
// Python-style mark-and-sweep; runs periodically and on program exit.
// Uses weak_ptr<void> so incomplete types (Symbol, Value) don't matter here.
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

inline std::map<std::string_view, Value>& ObjectValue::builtins() {
  using namespace std::literals;
  static std::map<std::string_view, Value> props_ = {
      {"size"sv,
       Value(FunctionValue({}, [](std::shared_ptr<Environment> callEnv) {
         const auto& val = callEnv->get("this");
         long n = val.to_object().properties->size();
         return Value(n);
       }))}};
  return props_;
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
                                     }})}};
  return props_;
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

inline void setup_built_in_functions(Environment& env) {
  env.initialize("puts",
                 Value(FunctionValue({{"arg", true}},
                                     [](std::shared_ptr<Environment> env) {
                                       std::cout << env->get("arg").str()
                                                 << std::endl;
                                       return Value();
                                     })),
                 false);

  env.initialize(
      "assert",
      Value(FunctionValue({{"arg", true}},
                          [](std::shared_ptr<Environment> env) {
                            auto cond = env->get("arg").to_bool();
                            if (!cond) {
                              auto line = env->get("__LINE__").to_long();
                              auto column = env->get("__COLUMN__").to_long();
                              throw std::runtime_error(
                                  std::format("assert failed at {}:{}.", line, column));
                            }
                            return Value();
                          })),
      false);
}

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

  Value eval_function(const peg::Ast& ast, std::shared_ptr<Environment> env) {
    std::vector<FunctionValue::Parameter> params;
    for (auto node : ast.nodes[0]->nodes) {
      auto mut = node->nodes[0]->token == "mut";
      const auto& name = node->nodes[1]->token;
      auto type_name = extract_type_annotation(*node, 2);
      params.push_back({name, mut, type_name});
    }

    size_t body_idx = 1;
    auto return_type = extract_return_type(ast, body_idx);
    auto body = ast.nodes[body_idx];

    return Value(FunctionValue(
        params,
        [=, this](std::shared_ptr<Environment> callEnv) {
          callEnv->append_outer(env);
          return eval(*body, callEnv);
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
      callEnv->initialize("__LINE__", Value((long)ast.line), false);
      callEnv->initialize("__COLUMN__", Value((long)ast.column), false);
      Value result;
      try {
        result = f.eval(callEnv);
      } catch (const Value& e) {
        result = e;
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
    const auto& obj = val.to_object();
    auto name = ast.token;
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
    for (auto node : ast.nodes) {
      eval(*node, scopeEnv);
    }
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
        "nil"sv,    "true"sv,  "false"sv, "mut"sv,  "debugger"sv,
        "return"sv, "while"sv, "if"sv,    "else"sv, "fn"sv};
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
      if (!let && env->has(ident)) {
        env->assign(ident, rval);
      } else if (is_keyword(ident)) {
        throw std::runtime_error("left-hand side is invalid variable name.");
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
      throw Value();
    } else {
      throw eval(*ast.nodes[0], env);
    }
  }

  Debugger debugger_;
};

inline bool interpret(const std::shared_ptr<peg::Ast>& ast,
                      std::shared_ptr<Environment> env, Value& val,
                      std::vector<std::string>& msgs,
                      Debugger debugger = nullptr) {
  try {
    val = Interpreter(debugger).eval(*ast, env);
    return true;
  } catch (const Value& e) {
    val = e;
    return true;
  } catch (std::runtime_error& e) {
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
