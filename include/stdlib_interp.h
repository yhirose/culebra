#pragma once

// Interpreter-side implementation of the Culebra standard library.
//
// Core built-ins bound on every environment: assert, range, to_long,
// to_string, type_of (see docs/language.md §18). Everything else is
// grouped under a namespace ObjectValue: Math (abs/min/max), IO
// (puts/print/input/read/write), Sys (argv). The CLI (src/main.cc)
// additionally aliases IO.puts / IO.print as globals for scripting
// ergonomics; embedders get a clean environment by default.
//
// Fragment header: included from within `namespace culebra { ... }` in
// interpreter.h. Do not wrap in a namespace.

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

inline Value make_math_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize(
      "abs",
      Value(FunctionValue({{"x", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto x = env->get("x").to_long();
                            return Value(x < 0 ? -x : x);
                          },
                          "Long"sv)),
      false);

  ns.initialize(
      "min",
      Value(FunctionValue({{"a", false, "Long"sv}, {"b", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto a = env->get("a").to_long();
                            auto b = env->get("b").to_long();
                            return Value(a < b ? a : b);
                          },
                          "Long"sv)),
      false);

  ns.initialize(
      "max",
      Value(FunctionValue({{"a", false, "Long"sv}, {"b", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto a = env->get("a").to_long();
                            auto b = env->get("b").to_long();
                            return Value(a > b ? a : b);
                          },
                          "Long"sv)),
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

  return Value(std::move(ns));
}

inline Value make_io_namespace() {
  using namespace std::literals;
  ObjectValue ns;

  ns.initialize("puts",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      std::cout << env->get("arg").str()
                                                << std::endl;
                                      return Value();
                                    })),
                false);

  ns.initialize("print",
                Value(FunctionValue({{"arg", true}},
                                    [](std::shared_ptr<Environment> env) {
                                      std::cout << env->get("arg").str_display();
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

  // range(n) -> [0..n), range(start, end) -> [start..end). Uses __ARGS__.
  env.initialize(
      "range",
      Value(FunctionValue(
          {}, [](std::shared_ptr<Environment> callEnv) {
            long start = 0, end = 0;
            if (callEnv->has("__ARGS__")) {
              const auto& extras =
                  *callEnv->get("__ARGS__").to_array().values;
              if (extras.size() == 1) {
                end = extras[0].to_long();
              } else if (extras.size() >= 2) {
                start = extras[0].to_long();
                end = extras[1].to_long();
              }
            }
            ArrayValue out;
            if (end > start) out.values->reserve(end - start);
            for (long i = start; i < end; i++) {
              out.values->push_back(Value(i));
            }
            return Value(std::move(out));
          })),
      false);

  env.initialize(
      "to_long",
      Value(FunctionValue({{"s", false, "String"sv}},
                          [](std::shared_ptr<Environment> env) {
                            const auto& s = env->get("s").to_string();
                            size_t i = 0, j = s.size();
                            while (i < j && std::isspace(
                                                static_cast<unsigned char>(s[i])))
                              i++;
                            while (j > i && std::isspace(
                                                static_cast<unsigned char>(s[j - 1])))
                              j--;
                            auto t = s.substr(i, j - i);
                            if (t.empty()) {
                              auto line = env->get("__LINE__").to_long();
                              auto col = env->get("__COLUMN__").to_long();
                              throw std::runtime_error(std::format(
                                  "type error at {}:{}.", line, col));
                            }
                            try {
                              size_t idx = 0;
                              long v = std::stol(t, &idx, 10);
                              if (idx != t.size()) throw std::invalid_argument("");
                              return Value(v);
                            } catch (...) {
                              auto line = env->get("__LINE__").to_long();
                              auto col = env->get("__COLUMN__").to_long();
                              throw std::runtime_error(std::format(
                                  "type error at {}:{}.", line, col));
                            }
                          },
                          "Long"sv)),
      false);

  env.initialize("to_string",
                 Value(FunctionValue({{"v", false}},
                                     [](std::shared_ptr<Environment> env) {
                                       return Value(
                                           env->get("v").str_display());
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
  env.initialize("Sys", make_sys_namespace(argv), false);
}
