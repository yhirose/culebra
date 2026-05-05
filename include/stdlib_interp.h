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
// Fragment header: included from within `namespace culebra { ... }` in
// interpreter.h. Do not wrap in a namespace.

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>

// Unified arg extraction for Math.iota / Math.range (and any future
// "integer range" factory): 1 arg → end (start defaults to 0), 2+ args →
// (start, end). Missing args leave the output pair at (0, 0), yielding
// an empty range.
inline std::pair<long, long> _parse_iota_range_args(
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

  // Fold a non-empty set of numeric arguments (`a`, `b`, and any
  // overflow in `__ARGS__`) with a picker. Returns Long if every arg
  // was Long, else Float. At least 2 args are required.
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
                              throw std::runtime_error(std::format(
                                  "type error at {}:{}.", line, col));
                            }
                            long r = 1;
                            while (exp > 0) {
                              if (exp & 1) r *= base;
                              base *= base;
                              exp >>= 1;
                            }
                            return Value(r);
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

  // Math.iota(n) / Math.iota(start, end): materialize a new Array of
  // consecutive integers. Name follows APL / C++ std::iota / SRFI-1.
  // For iteration without materializing the Array, use Math.range
  // (lazy iterator counterpart).
  ns.initialize(
      "iota",
      Value(FunctionValue(
          {}, [](std::shared_ptr<Environment> callEnv) {
            auto [start, end] = _parse_iota_range_args(callEnv);
            ArrayValue out;
            if (end > start) out.values->reserve(end - start);
            for (long i = start; i < end; i++) {
              out.values->push_back(Value(i));
            }
            return Value(std::move(out));
          })),
      false);

  // Float-domain functions. Each one accepts Long or Float input and
  // produces Float (log/exp/sqrt) or Long (floor/ceil/round).
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

  // Constants.
  ns.initialize("pi",  Value(M_PI), false);
  ns.initialize("e",   Value(M_E), false);
  ns.initialize("inf", Value(std::numeric_limits<double>::infinity()), false);
  ns.initialize("nan", Value(std::numeric_limits<double>::quiet_NaN()), false);

  // Math.range(n) / Math.range(start, end): lazy integer iterator. The
  // counterpart to Math.iota — yields successive integers via the
  // iterator protocol (see language.md §17.5) without allocating the
  // full Array up front. Use in for-in loops to iterate in constant
  // additional memory.
  ns.initialize(
      "range",
      Value(FunctionValue(
          {}, [](std::shared_ptr<Environment> callEnv) {
            auto [start, end] = _parse_iota_range_args(callEnv);
            auto current = std::make_shared<long>(start);
            return _make_iterator(
                [current, end](std::shared_ptr<Environment>) {
                  if (*current >= end) return _iter_step_done();
                  auto v = Value(*current);
                  (*current)++;
                  return _iter_step_value(std::move(v));
                });
          })),
      false);

  return Value(std::move(ns));
}

inline Value make_io_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize("puts",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      std::cout
                                          << str_quoted_with_dunder(
                                                 env->get("arg"))
                                          << std::endl;
                                      return Value();
                                    })),
                false);

  ns.initialize("print",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      std::cout << str_display_with_dunder(
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
                              throw std::runtime_error(std::format(
                                  "type error at {}:{}.", line, col));
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
              throw std::runtime_error(
                  std::format("type error at {}:{}.", line, col));
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

  // Small helper: read a numeric argument or raise type error.
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
                              throw std::runtime_error(
                                  std::format("assert failed at {}:{}.", line, column));
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
                                       return Value(str_display_with_dunder(
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
                            }
                            return Value(std::string(n));
                          },
                          "String"sv)),
      false);

  env.initialize("Math", make_math_namespace(), false);
  env.initialize("IO", make_io_namespace(), false);
  env.initialize("Random", make_random_namespace(), false);
  env.initialize("Sys", make_sys_namespace(argv), false);
}
