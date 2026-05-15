#pragma once

// Interpreter-side implementation of the Culebra standard library.
//
// Core built-ins bound on every environment: assert, to_long,
// to_string, type_of (see docs/language.md §18). Everything else is
// grouped under a namespace ObjectValue: Math (abs/min/max/pow/sign/
// clamp/iota), IO (puts/print/input/read/write), Sys (argv/exit/env).
// The CLI (src/main.cc) additionally aliases IO.puts / IO.print as
// globals for scripting ergonomics; embedders get a clean environment
// by default.
//
// Independent header. Include from main.cc (or any embedder) after
// interpreter.h to wire stdlib into a fresh Environment via
// `culebra::environment(argv)` or `culebra::setup_built_in_functions(env)`.

#include <interpreter.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <chrono>
#include <random>
#include <string>
#include <vector>

namespace culebra {

inline Value make_math_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize(
      "abs",
      Value(FunctionValue({{"x", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& x = env->get("x");
                            if (x.type == Value::Long) {
                              auto v = x.get<long>();
                              return Value(v < 0 ? -v : v);
                            }
                            if (x.type == Value::Float) {
                              return Value(std::fabs(x.get<double>()));
                            }
                            auto line = env->get("__LINE__").to_long();
                            auto col = env->get("__COLUMN__").to_long();
                            throw_type_error_at(line, col);
                          })),
      false);

  // Returns Long if every arg was Long, else Float. Requires ≥2 args.
  auto numeric_reduce = [](std::shared_ptr<Environment> env,
                           auto better) {
    long line = env->get("__LINE__").to_long();
    long col = env->get("__COLUMN__").to_long();
    if (!env->has("__ARGS__")) throw_type_error_at(line, col);
    const auto& extras = *env->get("__ARGS__").to_array().values;
    if (extras.empty()) throw_type_error_at(line, col);
    bool any_float = false;
    for (const auto& v : extras) {
      if (!v.is_numeric()) throw_type_error_at(line, col);
      if (v.type == Value::Float) any_float = true;
    }
    if (any_float) {
      double acc = extras[0].to_double_coerce();
      for (size_t i = 1; i < extras.size(); i++) {
        double x = extras[i].to_double_coerce();
        if (better(x, acc)) acc = x;
      }
      return Value(acc);
    }
    long acc = extras[0].get<long>();
    for (size_t i = 1; i < extras.size(); i++) {
      long x = extras[i].get<long>();
      if (better(static_cast<double>(x), static_cast<double>(acc))) acc = x;
    }
    return Value(acc);
  };

  ns.initialize(
      "min",
      Value(FunctionValue({}, [numeric_reduce](std::shared_ptr<Environment> env) {
        return numeric_reduce(env, [](double a, double b) { return a < b; });
      })),
      false);

  ns.initialize(
      "max",
      Value(FunctionValue({}, [numeric_reduce](std::shared_ptr<Environment> env) {
        return numeric_reduce(env, [](double a, double b) { return a > b; });
      })),
      false);

  // Integer power: 0^0 == 1; negative exponent throws.
  ns.initialize(
      "pow",
      Value(FunctionValue({{"base", false, "Long"sv}, {"exp", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto base = env->get("base").to_long();
                            auto exp = env->get("exp").to_long();
                            if (exp < 0) {
                              auto line = env->get("__LINE__").to_long();
                              auto col = env->get("__COLUMN__").to_long();
                              throw_type_error_at(line, col);
                            }
                            return Value(ipow_nonneg(base, exp));
                          },
                          "Long"sv)),
      false);

  ns.initialize(
      "sign",
      Value(FunctionValue({{"x", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto x = env->get("x").to_long();
                            return Value(x > 0 ? 1L : (x < 0 ? -1L : 0L));
                          },
                          "Long"sv)),
      false);

  ns.initialize(
      "clamp",
      Value(FunctionValue({{"x", false, "Long"sv},
                           {"lo", false, "Long"sv},
                           {"hi", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto x = env->get("x").to_long();
                            auto lo = env->get("lo").to_long();
                            auto hi = env->get("hi").to_long();
                            if (x < lo) return Value(lo);
                            if (x > hi) return Value(hi);
                            return Value(x);
                          },
                          "Long"sv)),
      false);

  auto float_to_float = [](auto fn) {
    return Value(FunctionValue(
        {{"x", false}},
        [fn](std::shared_ptr<Environment> env) {
          const auto& v = env->get("x");
          if (!v.is_numeric()) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            throw_type_error_at(line, col);
          }
          return Value(fn(v.to_double_coerce()));
        },
        "Float"sv));
  };
  auto float_to_long = [](auto fn) {
    return Value(FunctionValue(
        {{"x", false}},
        [fn](std::shared_ptr<Environment> env) {
          const auto& v = env->get("x");
          if (v.type == Value::Long) return v;
          if (v.type != Value::Float) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            throw_type_error_at(line, col);
          }
          return Value(static_cast<long>(fn(v.get<double>())));
        },
        "Long"sv));
  };
  ns.initialize("log",   float_to_float([](double x) { return std::log(x); }), false);
  ns.initialize("exp",   float_to_float([](double x) { return std::exp(x); }), false);
  ns.initialize("sqrt",  float_to_float([](double x) { return std::sqrt(x); }), false);
  ns.initialize("floor", float_to_long ([](double x) { return std::floor(x); }), false);
  ns.initialize("ceil",  float_to_long ([](double x) { return std::ceil(x); }), false);
  // std::rint honors the current IEEE 754 rounding mode, which defaults to
  // round-half-to-even (banker's rounding, matching Python's built-in round()).
  ns.initialize("round", float_to_long ([](double x) { return std::rint(x); }), false);

  ns.initialize("pi",  Value(M_PI), false);
  ns.initialize("e",   Value(M_E), false);
  ns.initialize("inf", Value(std::numeric_limits<double>::infinity()), false);
  ns.initialize("nan", Value(std::numeric_limits<double>::quiet_NaN()), false);

  return Value(std::move(ns));
}

inline Value make_io_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize("puts",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      std::cout
                                          << str_quoted_with_special(
                                                 env->get("arg"))
                                          << std::endl;
                                      return Value();
                                    })),
                false);

  ns.initialize("print",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      std::cout << str_display_with_special(
                                          env->get("arg"));
                                      return Value();
                                    })),
                false);

  ns.initialize(
      "input",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment>) {
                            std::string line;
                            if (!std::getline(std::cin, line)) {
                              return Value(std::string(""));
                            }
                            return Value(std::move(line));
                          },
                          "String"sv)),
      false);

  ns.initialize(
      "read",
      Value(FunctionValue({{"path", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& p = env->get("path").to_string();
                            std::ifstream ifs(p, std::ios::binary);
                            if (!ifs) {
                              auto line = env->get("__LINE__").to_long();
                              auto col = env->get("__COLUMN__").to_long();
                              throw CulebraError("IOError",
                                  std::format("IO.read: cannot open '{}' at {}:{}.",
                                              p, line, col),
                                  line, col);
                            }
                            std::string s(
                                (std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
                            return Value(std::move(s));
                          },
                          "String"sv)),
      false);

  ns.initialize(
      "write",
      Value(FunctionValue(
          {{"path", false, "String"sv}, {"content", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& p = env->get("path").to_string();
            const auto& c = env->get("content").to_string();
            std::ofstream ofs(p, std::ios::binary);
            if (!ofs) {
              auto line = env->get("__LINE__").to_long();
              auto col = env->get("__COLUMN__").to_long();
              throw CulebraError("IOError",
                  std::format("IO.write: cannot open '{}' at {}:{}.",
                              p, line, col),
                  line, col);
            }
            ofs.write(c.data(), c.size());
            return Value();
          })),
      false);

  ns.initialize(
      "exists",
      Value(FunctionValue({{"path", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& p = env->get("path").to_string();
                            std::error_code ec;
                            return Value(std::filesystem::exists(p, ec));
                          },
                          "Bool"sv)),
      false);

  return Value(std::move(ns));
}

inline Value make_random_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  auto get_num = [](std::shared_ptr<Environment>& env, const char* name) {
    const auto& v = env->get(name);
    if (!v.is_numeric()) {
      auto line = env->get("__LINE__").to_long();
      auto col = env->get("__COLUMN__").to_long();
      throw_type_error_at(line, col);
    }
    return v.to_double_coerce();
  };

  ns.initialize(
      "seed",
      Value(FunctionValue({{"n", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto n = env->get("n").to_long();
                            random_engine().seed(
                                static_cast<uint64_t>(n));
                            return Value();
                          })),
      false);

  ns.initialize(
      "int",
      Value(FunctionValue(
          {{"lo", false, "Long"sv}, {"hi", false, "Long"sv}},
          [](std::shared_ptr<Environment> env) {
            auto lo = env->get("lo").to_long();
            auto hi = env->get("hi").to_long();
            if (hi <= lo) {
              auto line = env->get("__LINE__").to_long();
              auto col = env->get("__COLUMN__").to_long();
              throw_type_error_at(line, col);
            }
            std::uniform_int_distribution<long> d(lo, hi - 1);
            return Value(d(random_engine()));
          },
          "Long"sv)),
      false);

  ns.initialize(
      "uniform",
      Value(FunctionValue(
          {{"lo", false}, {"hi", false}},
          [get_num](std::shared_ptr<Environment> env) {
            auto lo = get_num(env, "lo");
            auto hi = get_num(env, "hi");
            std::uniform_real_distribution<double> d(lo, hi);
            return Value(d(random_engine()));
          },
          "Float"sv)),
      false);

  ns.initialize(
      "gauss",
      Value(FunctionValue(
          {{"mu", false}, {"sigma", false}},
          [get_num](std::shared_ptr<Environment> env) {
            auto mu = get_num(env, "mu");
            auto sigma = get_num(env, "sigma");
            std::normal_distribution<double> d(mu, sigma);
            return Value(d(random_engine()));
          },
          "Float"sv)),
      false);

  ns.initialize(
      "shuffle",
      Value(FunctionValue(
          {{"a", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            auto& arr = *env->get("a").to_array().values;
            std::shuffle(arr.begin(), arr.end(), random_engine());
            return Value();
          })),
      false);

  ns.initialize(
      "weighted_choice",
      Value(FunctionValue(
          {{"pop", false, "Array"sv}, {"weights", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& pop = *env->get("pop").to_array().values;
            const auto& weights = *env->get("weights").to_array().values;
            if (pop.empty() || pop.size() != weights.size()) {
              auto line = env->get("__LINE__").to_long();
              auto col = env->get("__COLUMN__").to_long();
              throw_type_error_at(line, col);
            }
            // Reused between calls to avoid a heap allocation per draw
            // (the microgpt inference path calls this per token).
            thread_local std::vector<double> scratch;
            scratch.clear();
            scratch.reserve(weights.size());
            for (const auto& v : weights) {
              if (!v.is_numeric()) {
                auto line = env->get("__LINE__").to_long();
                auto col = env->get("__COLUMN__").to_long();
                throw_type_error_at(line, col);
              }
              scratch.push_back(v.to_double_coerce());
            }
            std::discrete_distribution<size_t> d(scratch.begin(),
                                                 scratch.end());
            return pop[d(random_engine())];
          })),
      false);

  return Value(std::move(ns));
}

// Tensor.zeros(3, 4) and Tensor.zeros([3, 4]) produce the same shape.
// Reads positional Long entries from `args[offset..]`, or unwraps a
// single Array argument if that is what's there.
inline TensorShape parse_tensor_shape(const std::vector<Value>& args,
                                      size_t offset, size_t line, size_t col) {
  std::vector<int64_t> dims;
  auto push_long = [&](const Value& v) {
    if (v.type != Value::Long) throw_type_error_at(line, col);
    dims.push_back(v.to_long());
  };
  if (args.size() - offset == 1 && args[offset].type == Value::Array) {
    for (const auto& v : *args[offset].to_array().values) push_long(v);
  } else {
    for (size_t i = offset; i < args.size(); i++) push_long(args[i]);
  }
  // TensorShape ctor rejects negative dims.
  return TensorShape(std::move(dims));
}

// If args[0] is a "f32"/"f64" tag, consume it; otherwise default F32.
// Returns {dtype, offset of first shape arg}.
inline std::pair<Dtype, size_t> parse_tensor_dtype_prefix(
    const std::vector<Value>& args, size_t line, size_t col) {
  if (args.empty() || args[0].type != Value::String) return {Dtype::F32, 0};
  auto d = parse_dtype(args[0].get<std::string>());
  if (!d) throw_type_error_at(line, col);
  return {*d, 1};
}

template <typename T>
inline void _tensor_fill_1d(T* out, const std::vector<Value>& vs, size_t line,
                            size_t col) {
  for (size_t i = 0; i < vs.size(); i++) {
    if (!vs[i].is_numeric()) throw_type_error_at(line, col);
    out[i] = static_cast<T>(vs[i].to_double_coerce());
  }
}

template <typename T>
inline void _tensor_fill_2d(T* out, const std::vector<Value>& rows_v,
                            size_t cols, size_t line, size_t col) {
  for (size_t i = 0; i < rows_v.size(); i++) {
    if (rows_v[i].type != Value::Array) throw_type_error_at(line, col);
    const auto& row = *rows_v[i].to_array().values;
    if (row.size() != cols) throw_type_error_at(line, col);
    for (size_t j = 0; j < cols; j++) {
      if (!row[j].is_numeric()) throw_type_error_at(line, col);
      out[i * cols + j] = static_cast<T>(row[j].to_double_coerce());
    }
  }
}

// Detects 1D Array<Float|Long> or 2D Array of equal-length numeric
// rows. Higher-rank "from" is deferred to M2+.
inline TensorPtr tensor_from_array(const ArrayValue& a, Dtype dt, size_t line,
                                   size_t col) {
  const auto& vs = *a.values;
  if (vs.empty()) return tensor_zeros(TensorShape({0}), dt);

  if (vs[0].is_numeric()) {
    auto t = std::make_shared<TensorImpl>(
        TensorShape({static_cast<int64_t>(vs.size())}), dt);
    if (dt == Dtype::F32) _tensor_fill_1d(t->data_as<float>(), vs, line, col);
    else                  _tensor_fill_1d(t->data_as<double>(), vs, line, col);
    return t;
  }

  if (vs[0].type != Value::Array) throw_type_error_at(line, col);
  size_t cols = vs[0].to_array().values->size();
  size_t rows = vs.size();
  auto t = std::make_shared<TensorImpl>(
      TensorShape({static_cast<int64_t>(rows), static_cast<int64_t>(cols)}),
      dt);
  if (dt == Dtype::F32)
    _tensor_fill_2d(t->data_as<float>(), vs, cols, line, col);
  else
    _tensor_fill_2d(t->data_as<double>(), vs, cols, line, col);
  return t;
}

inline Value make_tensor_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  // Variadic ctor: declared with no formal params so every positional
  // arg lands in __ARGS__. Layout is [optional dtype string, shape
  // varargs OR single Array of Long].
  auto make_ctor = [](TensorPtr (*kernel)(TensorShape, Dtype)) {
    return Value(FunctionValue(
        {},
        [kernel](std::shared_ptr<Environment> env) {
          auto line = env->get("__LINE__").to_long();
          auto col = env->get("__COLUMN__").to_long();
          if (!env->has("__ARGS__")) throw_type_error_at(line, col);
          const auto& args = *env->get("__ARGS__").to_array().values;
          auto [dt, offset] = parse_tensor_dtype_prefix(args, line, col);
          auto shape = parse_tensor_shape(args, offset, line, col);
          return Value(TensorValue(kernel(std::move(shape), dt)));
        },
        "Tensor"sv));
  };

  ns.initialize("zeros", make_ctor(&tensor_zeros), false);
  ns.initialize("ones", make_ctor(&tensor_ones), false);
  ns.initialize("randn", make_ctor(&tensor_randn), false);

  // Activations: namespace functions (not Tensor methods), since
  // `relu / sigmoid / softmax` are common class-method names — having
  // them as methods would shadow user definitions. The namespace form
  // makes the call site explicitly Tensor-flavored.
  auto make_unary_ns = [](Op op) {
    return Value(FunctionValue(
        {{"t", false, "Tensor"sv}},
        [op](std::shared_ptr<Environment> env) {
          const auto& t = env->get("t").to_tensor().impl;
          return Value(TensorValue(tensor_unary(op, t)));
        },
        "Tensor"sv));
  };
  ns.initialize("sigmoid", make_unary_ns(Op::Sigmoid), false);
  ns.initialize("relu", make_unary_ns(Op::Relu), false);
  ns.initialize("softmax", make_unary_ns(Op::Softmax), false);

  ns.initialize(
      "eval",
      Value(FunctionValue(
          {},
          [](std::shared_ptr<Environment> env) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            if (!env->has("__ARGS__")) return Value();
            const auto& args = *env->get("__ARGS__").to_array().values;
            for (const auto& v : args) {
              if (v.type != Value::Tensor) throw_type_error_at(line, col);
              tensor_eval_node(*v.to_tensor().impl);
            }
            return Value();
          })),
      false);

  ns.initialize(
      "from_csv",
      Value(FunctionValue(
          {{"path", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            const auto& path = env->get("path").to_string();
            return Value(TensorValue(tensor_from_csv(path, Dtype::F32)));
          },
          "Tensor"sv)),
      false);

  ns.initialize(
      "from",
      Value(FunctionValue(
          {{"a", false, "Array"sv}},
          [](std::shared_ptr<Environment> env) {
            auto line = env->get("__LINE__").to_long();
            auto col = env->get("__COLUMN__").to_long();
            const auto& a = env->get("a").to_array();
            // No dtype arg in M1: always F32. M2+ may take a trailing
            // string tag once mixed-dtype binops are sorted.
            auto t = tensor_from_array(a, Dtype::F32, line, col);
            return Value(TensorValue(std::move(t)));
          },
          "Tensor"sv)),
      false);

  return Value(std::move(ns));
}

inline Value make_sys_namespace(const std::vector<std::string>& argv) {
  using namespace std::literals;
  ObjectValue ns;

  ArrayValue arr;
  arr.values->reserve(argv.size());
  for (const auto& s : argv) {
    arr.values->push_back(Value(std::string(s)));
  }
  ns.initialize("argv", Value(std::move(arr)), false);

  ns.initialize(
      "exit",
      Value(FunctionValue({{"code", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto code = env->get("code").to_long();
                            std::exit(static_cast<int>(code));
                            return Value();  // unreachable
                          })),
      false);

  ns.initialize(
      "env",
      Value(FunctionValue({{"name", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& name = env->get("name").to_string();
                            const char* v = std::getenv(name.c_str());
                            return Value(std::string(v ? v : ""));
                          },
                          "String"sv)),
      false);

  // Monotonic seconds since first call. Anchor at process startup so
  // time differences across calls are measured against a stable origin.
  ns.initialize(
      "time",
      Value(FunctionValue({},
                          [](std::shared_ptr<Environment>) {
                            using clock = std::chrono::steady_clock;
                            static const auto t0 = clock::now();
                            auto now = clock::now();
                            return Value(std::chrono::duration<double>(
                                             now - t0)
                                             .count());
                          },
                          "Float"sv)),
      false);

  return Value(std::move(ns));
}

// --- JSON stringify/parse ---

inline std::string json_stringify(const Value& v) {
  switch (v.type) {
    case Value::Nil:    return "null";
    case Value::Bool:   return v.get<bool>() ? "true" : "false";
    case Value::Long:   return std::to_string(v.get<long>());
    case Value::Float: {
      double d = v.get<double>();
      if (!std::isfinite(d)) {
        throw CulebraError("ValueError",
                           "JSON.stringify: non-finite Float");
      }
      return format_float_shortest(d);
    }
    case Value::String: return culebra::json_escape(v.get<std::string>());
    case Value::Array: {
      std::string s = "[";
      const auto& xs = *v.to_array().values;
      for (size_t i = 0; i < xs.size(); i++) {
        if (i) s += ",";
        s += json_stringify(xs[i]);
      }
      return s + "]";
    }
    case Value::Object: {
      const auto& obj = v.to_object();
      if (obj.non_string_order && !obj.non_string_order->empty()) {
        throw CulebraError("TypeError",
            "JSON.stringify: Object has non-String keys");
      }
      std::string s = "{";
      bool first = true;
      for (const auto& [k, sym] : *obj.properties) {
        if (!first) s += ",";
        first = false;
        s += culebra::json_escape(k) + ":" + json_stringify(sym.val);
      }
      return s + "}";
    }
    default:
      throw CulebraError("TypeError", std::format(
          "JSON.stringify: cannot serialize {}", v.type_name()));
  }
}

// Minimal recursive-descent JSON parser.
struct _JsonParser {
  const char* p;
  const char* end;
  void skip_ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
  [[noreturn]] void fail(const char* msg) {
    throw CulebraError("ValueError",
                       std::format("JSON.parse: {}", msg));
  }
  Value parse_value() {
    skip_ws();
    if (p >= end) fail("unexpected end");
    char c = *p;
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') return parse_string();
    if (c == 't' || c == 'f') return parse_bool();
    if (c == 'n') return parse_null();
    return parse_number();
  }
  Value parse_object() {
    ++p; skip_ws();
    ObjectValue obj;
    if (p < end && *p == '}') { ++p; return Value(std::move(obj)); }
    while (p < end) {
      skip_ws();
      auto key = parse_string_raw();
      skip_ws();
      if (p >= end || *p != ':') fail("expected ':'");
      ++p;
      auto val = parse_value();
      // Heap-stable storage for the key string_view.
      static thread_local std::deque<std::string> pool;
      pool.push_back(std::move(key));
      obj.initialize(std::string_view(pool.back()), val, false);
      skip_ws();
      if (p < end && *p == ',') { ++p; continue; }
      if (p < end && *p == '}') { ++p; return Value(std::move(obj)); }
      fail("expected ',' or '}'");
    }
    fail("unterminated object");
  }
  Value parse_array() {
    ++p; skip_ws();
    ArrayValue arr;
    if (p < end && *p == ']') { ++p; return Value(std::move(arr)); }
    while (p < end) {
      arr.values->push_back(parse_value());
      skip_ws();
      if (p < end && *p == ',') { ++p; continue; }
      if (p < end && *p == ']') { ++p; return Value(std::move(arr)); }
      fail("expected ',' or ']'");
    }
    fail("unterminated array");
  }
  Value parse_string() { return Value(parse_string_raw()); }
  std::string parse_string_raw() {
    if (*p != '"') fail("expected string");
    ++p;
    std::string out;
    while (p < end && *p != '"') {
      if (*p == '\\' && p + 1 < end) {
        ++p;
        switch (*p) {
          case '"':  out += '"'; break;
          case '\\': out += '\\'; break;
          case '/':  out += '/'; break;
          case 'n':  out += '\n'; break;
          case 'r':  out += '\r'; break;
          case 't':  out += '\t'; break;
          case 'b':  out += '\b'; break;
          case 'f':  out += '\f'; break;
          default:   fail("bad escape");
        }
        ++p;
      } else {
        out += *p++;
      }
    }
    if (p >= end) fail("unterminated string");
    ++p;
    return out;
  }
  Value parse_bool() {
    if (end - p >= 4 && std::string_view(p, 4) == "true")  { p += 4; return Value(true); }
    if (end - p >= 5 && std::string_view(p, 5) == "false") { p += 5; return Value(false); }
    fail("bad bool");
  }
  Value parse_null() {
    if (end - p >= 4 && std::string_view(p, 4) == "null") { p += 4; return Value(); }
    fail("bad null");
  }
  Value parse_number() {
    const char* start = p;
    if (*p == '-') ++p;
    bool is_float = false;
    while (p < end && (*p >= '0' && *p <= '9')) ++p;
    if (p < end && *p == '.') {
      is_float = true; ++p;
      while (p < end && (*p >= '0' && *p <= '9')) ++p;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
      is_float = true; ++p;
      if (p < end && (*p == '+' || *p == '-')) ++p;
      while (p < end && (*p >= '0' && *p <= '9')) ++p;
    }
    std::string buf(start, p);
    if (is_float) return Value(std::stod(buf));
    return Value(static_cast<long>(std::stoll(buf)));
  }
};

inline Value json_parse(std::string_view s) {
  _JsonParser jp{s.data(), s.data() + s.size()};
  auto v = jp.parse_value();
  jp.skip_ws();
  if (jp.p != jp.end) {
    throw CulebraError("ValueError",
                       "JSON.parse: trailing characters");
  }
  return v;
}

inline Value make_json_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "stringify",
      Value(FunctionValue({{"v", false}},
          [](std::shared_ptr<Environment> env) {
            return Value(json_stringify(env->get("v")));
          }, "String"sv)),
      false);
  ns.initialize(
      "parse",
      Value(FunctionValue({{"s", false, "String"sv}},
          [](std::shared_ptr<Environment> env) {
            return json_parse(env->get("s").to_string());
          })),
      false);
  return Value(std::move(ns));
}

inline void setup_built_in_functions(
    Environment& env, const std::vector<std::string>& argv = {}) {
  using namespace std::literals;

  env.initialize(
      "assert",
      Value(FunctionValue({{"arg", true}},
                          [](std::shared_ptr<Environment> env) {
                            auto cond = env->get("arg").to_bool();
                            if (!cond) {
                              auto line = env->get("__LINE__").to_long();
                              auto column = env->get("__COLUMN__").to_long();
                              throw CulebraError("AssertionError",
                                  std::format("assert failed at {}:{}.", line, column),
                                  line, column);
                            }
                            return Value();
                          })),
      false);

  env.initialize(
      "to_long",
      Value(FunctionValue({{"v", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& v = env->get("v");
                            auto line = env->get("__LINE__").to_long();
                            auto col = env->get("__COLUMN__").to_long();
                            if (v.type == Value::Long) return v;
                            if (v.type == Value::Float) {
                              // Truncate toward zero (matches Python's int()).
                              return Value(static_cast<long>(v.get<double>()));
                            }
                            if (v.type != Value::String) throw_type_error_at(line, col);
                            return Value(parse_long_strict(v.to_string(), line, col));
                          },
                          "Long"sv)),
      false);

  env.initialize(
      "to_float",
      Value(FunctionValue({{"v", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& v = env->get("v");
                            auto line = env->get("__LINE__").to_long();
                            auto col = env->get("__COLUMN__").to_long();
                            if (v.type == Value::Float) return v;
                            if (v.type == Value::Long) {
                              return Value(static_cast<double>(v.get<long>()));
                            }
                            if (v.type != Value::String) throw_type_error_at(line, col);
                            return Value(parse_double_strict(v.to_string(), line, col));
                          },
                          "Float"sv)),
      false);

  env.initialize("to_string",
                 Value(FunctionValue({{"v", false}},
                                     [](std::shared_ptr<Environment> env) {
                                       return Value(str_display_with_special(
                                           env->get("v")));
                                     },
                                     "String"sv)),
                 false);

  env.initialize(
      "type_of",
      Value(FunctionValue({{"v", false}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& v = env->get("v");
                            const char* n = "Nil";
                            switch (v.type) {
                              case Value::Nil:      n = "Nil"; break;
                              case Value::Bool:     n = "Bool"; break;
                              case Value::Long:     n = "Long"; break;
                              case Value::Float:    n = "Float"; break;
                              case Value::String:   n = "String"; break;
                              case Value::Array:    n = "Array"; break;
                              case Value::Object:   n = "Object"; break;
                              case Value::Function: n = "Function"; break;
                              case Value::Tensor:   n = "Tensor"; break;
                              case Value::Tuple:    n = "Tuple"; break;
                              case Value::Set:      n = "Set"; break;
                            }
                            return Value(std::string(n));
                          },
                          "String"sv)),
      false);

  env.initialize("Math", make_math_namespace(), false);
  env.initialize("IO", make_io_namespace(), false);
  env.initialize("Random", make_random_namespace(), false);
  env.initialize("Sys", make_sys_namespace(argv), false);
  env.initialize("Tensor", make_tensor_namespace(), false);
  env.initialize("JSON", make_json_namespace(), false);
}

inline std::shared_ptr<Environment> environment(
    const std::vector<std::string>& argv = {}) {
  auto env = std::make_shared<Environment>();
  setup_core_globals(*env);
  setup_built_in_functions(*env, argv);
  return env;
}

}  // namespace culebra
