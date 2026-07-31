// Multi-instance smoke test: two Runtimes coexist on the same thread,
// holding independent state (RNG seed, exception carriers, GC).
// Verifies the RuntimeScope + isolated-state guarantee.

#include <cstring>
#include <iostream>

#include <culebra.h>
#include <stdlib_interp.h>
#ifdef CULEBRA_JIT_ENABLED
#include <stdlib_jit.h>
#endif

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace mi_smoke_ns {

namespace {

// Script A maintains its own counter; Script B does too. Each is seeded
// independently. Without isolation, B's writes would leak into A.
// Script defines a stateful function the host can call repeatedly via
// culebra::call. Each Runtime keeps its own closed-over state.
const char* kScriptInit = R"(
let mut counter = 0
fn step() {
  counter = counter + 1
  counter
}
)";

std::shared_ptr<peg::Ast> parse_or_die(const char* code) {
  std::vector<std::string> msgs;
  auto ast = culebra::parse("<mi>", code, std::strlen(code), msgs);
  if (!ast) {
    for (auto& m : msgs) std::cerr << m;
    std::exit(1);
  }
  return ast;
}

}  // namespace

int run() {
  culebra::Runtime rt_a, rt_b;

  std::shared_ptr<culebra::Environment> env_a, env_b;

  // Initialize each Runtime independently.
  {
    culebra::RuntimeScope scope(rt_a);
    env_a = culebra::environment();
    culebra::Value v;
    std::vector<std::string> msgs;
    culebra::interpret(parse_or_die(kScriptInit), env_a, v, msgs,
                       culebra::Debugger());
  }
  {
    culebra::RuntimeScope scope(rt_b);
    env_b = culebra::environment();
    culebra::Value v;
    std::vector<std::string> msgs;
    culebra::interpret(parse_or_die(kScriptInit), env_b, v, msgs,
                       culebra::Debugger());
  }

  // Alternate calls into each Runtime's `step` function via the
  // embedding helper. Each Runtime should see only its own counter.
  bool ok = true;
  for (int i = 1; i <= 5; ++i) {
    {
      culebra::RuntimeScope scope(rt_a);
      auto v = culebra::call(env_a, "step", {});
      if (v.to_long() != i) {
        std::cerr << "A step " << i << " got " << v.to_long() << "\n";
        ok = false;
      }
    }
    {
      culebra::RuntimeScope scope(rt_b);
      auto v = culebra::call(env_b, "step", {});
      if (v.to_long() != i) {
        std::cerr << "B step " << i << " got " << v.to_long() << "\n";
        ok = false;
      }
    }
  }

  // Verify per-Runtime state (PRNG): seed A and B differently, then
  // alternate draws. Identical seeds + algorithms would leak if the
  // state were shared.
  {
    culebra::RuntimeScope scope(rt_a);
    culebra::random_engine().seed(42);
  }
  {
    culebra::RuntimeScope scope(rt_b);
    culebra::random_engine().seed(42);
  }
  // A draws first
  uint64_t a_first;
  {
    culebra::RuntimeScope scope(rt_a);
    a_first = culebra::random_engine()();
  }
  // B draws (untouched by A's draw)
  uint64_t b_first;
  {
    culebra::RuntimeScope scope(rt_b);
    b_first = culebra::random_engine()();
  }
  // Both seeded identically, both untouched by each other -> equal
  if (a_first != b_first) {
    std::cerr << "RNG isolation failed: a=" << a_first
              << " b=" << b_first << "\n";
    ok = false;
  }
  // Now A draws again; should be a different value (state advanced)
  uint64_t a_second;
  {
    culebra::RuntimeScope scope(rt_a);
    a_second = culebra::random_engine()();
  }
  if (a_second == a_first) {
    std::cerr << "RNG didn't advance for A\n";
    ok = false;
  }
  // B's state should still be at the post-first-draw point
  uint64_t b_second;
  {
    culebra::RuntimeScope scope(rt_b);
    b_second = culebra::random_engine()();
  }
  if (a_second != b_second) {
    std::cerr << "B's state contaminated by A: a2=" << a_second
              << " b2=" << b_second << "\n";
    ok = false;
  }

#ifdef CULEBRA_JIT_ENABLED
  // Verify per-Runtime JIT hooks: hooks installed in rt_a stay
  // independent of those in rt_b. rt_a runs a script that uses
  // Math.sqrt (provided by stdlib); rt_b without stdlib install can't
  // compile the same call.
  {
    culebra::Runtime jit_rt_a, jit_rt_b;
    {
      culebra::RuntimeScope scope(jit_rt_a);
      culebra::install_jit_stdlib();
      auto ast = parse_or_die("IO.print(Math.sqrt(16.0).to_string())");
      try {
        culebra::JIT::run(ast);
      } catch (const std::exception& e) {
        std::cerr << "rt_a JIT (with stdlib) unexpectedly threw: "
                  << e.what() << "\n";
        ok = false;
      }
    }
    {
      culebra::RuntimeScope scope(jit_rt_b);
      // No install_jit_stdlib here — rt_b has empty hooks.
      auto ast = parse_or_die("Math.sqrt(16.0)");
      bool threw = false;
      try {
        culebra::JIT::run(ast);
      } catch (...) {
        threw = true;
      }
      if (!threw) {
        std::cerr << "rt_b JIT (no stdlib) should have failed\n";
        ok = false;
      }
    }
  }
#endif

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace mi_smoke_ns
