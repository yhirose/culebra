// culebra::define smoke test: register C++ callables as host functions
// with auto-deduced signatures, then invoke them from script and from
// C++ via culebra::call.

#include <cstring>
#include <iostream>
#include <string>

#include <culebra.h>
#include <stdlib_interp.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace define_smoke_ns {

namespace {

std::shared_ptr<peg::Ast> parse_or_die(const char* code) {
  std::vector<std::string> msgs;
  auto ast = culebra::parse("<dfn>", code, std::strlen(code), msgs);
  if (!ast) {
    for (auto& m : msgs) std::cerr << m;
    std::exit(1);
  }
  return ast;
}

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

}  // namespace

int run() {
  auto env = culebra::environment();
  bool ok = true;

  // long, long -> long
  culebra::define(env, "host_add",
                  [](long a, long b) -> long { return a + b; }, {"a", "b"});

  // const string& -> void (side effect captures into the host)
  std::string last_msg;
  culebra::define(env, "host_log",
                  [&last_msg](const std::string& m) { last_msg = m; }, {"m"});

  // double -> double
  culebra::define(env, "host_neg",
                  [](double x) -> double { return -x; }, {"x"});

  // Value passthrough
  culebra::define(env, "host_id",
                  [](culebra::Value v) -> culebra::Value { return v; }, {"v"});

  // (1) Call from C++ via culebra::call
  ok &= check(culebra::call(env, "host_add",
                            {culebra::Value(int64_t{3}), culebra::Value(int64_t{4})})
                  .to_long() == 7,
              "host_add via call");

  culebra::call(env, "host_log",
                {culebra::Value(std::string("hello"))});
  ok &= check(last_msg == "hello", "host_log captured msg");

  ok &= check(culebra::call(env, "host_neg", {culebra::Value(2.5)})
                  .get<double>() == -2.5,
              "host_neg");

  // (2) Call from script via interpret
  auto run = [&](const char* code, long expected) {
    auto ast = parse_or_die(code);
    culebra::Value v;
    std::vector<std::string> msgs;
    if (!culebra::interpret(ast, env, v, msgs, culebra::Debugger())) {
      for (auto& m : msgs) std::cerr << m;
      return false;
    }
    return v.to_long() == expected;
  };
  ok &= check(run("host_add(10, 20)", 30), "script: host_add(10, 20)");
  ok &= check(run("host_id(99)", 99), "script: host_id passthrough");

  // (3) Type annotation surfaces at call sites — passing a String
  // where Long is expected should raise.
  {
    auto ast = parse_or_die("host_add('x', 1)");
    culebra::Value v;
    std::vector<std::string> msgs;
    bool threw = !culebra::interpret(ast, env, v, msgs, culebra::Debugger());
    ok &= check(threw, "type annotation rejects wrong-typed arg");
  }

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace define_smoke_ns
