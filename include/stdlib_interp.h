#pragma once

// Interpreter-side implementation of the Culebra standard library:
// global functions and I/O. Built-in type methods live in interpreter.h
// alongside their types (language-level; see docs/language.md §17).
//
// Fragment header: included from within `namespace culebra { ... }` in
// interpreter.h. Do not wrap in a namespace.

#include <cctype>
#include <fstream>
#include <iostream>
#include <string>

inline void setup_built_in_functions(Environment& env) {
  using namespace std::literals;

  env.initialize("puts",
                 Value(FunctionValue({{"arg", true}},
                                     [](std::shared_ptr<Environment> env) {
                                       std::cout << env->get("arg").str()
                                                 << std::endl;
                                       return Value();
                                     })),
                 false);

  env.initialize("print",
                 Value(FunctionValue({{"arg", true}},
                                     [](std::shared_ptr<Environment> env) {
                                       std::cout << env->get("arg").str_display();
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

  env.initialize(
      "abs",
      Value(FunctionValue({{"x", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto x = env->get("x").to_long();
                            return Value(x < 0 ? -x : x);
                          },
                          "Long"sv)),
      false);

  env.initialize(
      "min",
      Value(FunctionValue({{"a", false, "Long"sv}, {"b", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto a = env->get("a").to_long();
                            auto b = env->get("b").to_long();
                            return Value(a < b ? a : b);
                          },
                          "Long"sv)),
      false);

  env.initialize(
      "max",
      Value(FunctionValue({{"a", false, "Long"sv}, {"b", false, "Long"sv}},
                          [](std::shared_ptr<Environment> env) {
                            auto a = env->get("a").to_long();
                            auto b = env->get("b").to_long();
                            return Value(a > b ? a : b);
                          },
                          "Long"sv)),
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

  env.initialize(
      "input",
      Value(FunctionValue({}, [](std::shared_ptr<Environment>) {
                            std::string line;
                            if (!std::getline(std::cin, line)) {
                              return Value(std::string(""));
                            }
                            return Value(std::move(line));
                          },
                          "String"sv)),
      false);

  env.initialize(
      "read_file",
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

  env.initialize(
      "write_file",
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
}
