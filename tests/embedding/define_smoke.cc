// vm::Embed::define smoke test: register C++ callables as host functions
// with auto-deduced signatures, then invoke them from script and from
// C++ via Embed::call. (The interp-era culebra::define/call surface this
// exercised retired with the tree-walker — Phase 4 B7-d.)

#include <iostream>
#include <string>

#include <culebra.h>
#include <vm/embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace define_smoke_ns {

namespace {

bool check(bool cond, const char* what) {
  if (!cond) std::cerr << "FAIL: " << what << "\n";
  return cond;
}

}  // namespace

int run() {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;
  bool ok = true;

  // long, long -> long
  embed.define("host_add",
               [](int64_t a, int64_t b) -> int64_t { return a + b; },
               {"a", "b"});

  // const string -> void (side effect captures into the host)
  std::string last_msg;
  embed.define("host_log", [&last_msg](std::string m) { last_msg = m; },
               {"m"});

  // double -> double
  embed.define("host_neg", [](double x) -> double { return -x; }, {"x"});

  // Value passthrough
  embed.define("host_id",
               [](culebra::vm::Value v) -> culebra::vm::Value { return v; },
               {"v"});

  // (1) Call from C++ via Embed::call
  {
    std::vector<culebra::vm::Value> args;
    args.emplace_back(int64_t{3});
    args.emplace_back(int64_t{4});
    ok &= check(embed.call("host_add", std::move(args)).to_long() == 7,
                "host_add via call");
  }
  {
    std::vector<culebra::vm::Value> args;
    args.emplace_back(std::string_view("hello"));
    embed.call("host_log", std::move(args));
    ok &= check(last_msg == "hello", "host_log captured msg");
  }
  {
    std::vector<culebra::vm::Value> args;
    args.emplace_back(2.5);
    ok &= check(embed.call("host_neg", std::move(args)).to_double() == -2.5,
                "host_neg");
  }

  // (2) Call from script via run_source
  auto run = [&](const char* code, long expected) {
    culebra::vm::Value v;
    std::vector<std::string> msgs;
    if (!embed.run_source("<dfn>", code, v, msgs)) {
      for (auto& m : msgs) std::cerr << m << "\n";
      return false;
    }
    return v.to_long() == expected;
  };
  ok &= check(run("host_add(10, 20)", 30), "script: host_add(10, 20)");
  ok &= check(run("host_id(99)", 99), "script: host_id passthrough");

  // (3) The deduced parameter type surfaces at call sites — passing a
  // String where Long is expected raises a catchable TypeError.
  {
    culebra::vm::Value v;
    std::vector<std::string> msgs;
    bool ran = embed.run_source(
        "<dfn>", "try { host_add('x', 1) } catch e { e.kind }", v, msgs);
    ok &= check(ran && v.to_string() == "TypeError",
                "wrong-typed arg raises TypeError");
  }

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace define_smoke_ns
