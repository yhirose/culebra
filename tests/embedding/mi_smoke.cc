// Multiple-instance smoke test: two Runtimes (each with its own Embed
// session) on ONE host thread must stay fully independent — closure state,
// PRNG state, per-Runtime JIT hook sets. (The interp-era env/call surface
// this exercised retired with the tree-walker — Phase 4 B7-d; an Embed's
// per-instance session is what the two env shared_ptrs used to be.)

#include <cstdint>
#include <deque>
#include <iostream>
#include <string>

#include <culebra.h>
#include <stdlib_rt.h>
#include <vm_embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace mi_smoke_ns {

namespace {

const char* kScriptInit = R"(
let mut counter = 0
fn step() {
  counter = counter + 1
  counter
}
)";

// parse()'s AST holds string_view tokens into its source buffer, and
// callers keep using the returned AST beyond this call, so the buffer
// needs a stable, permanent home (JIT::run below; Embed::run_source owns
// its own copy).
std::shared_ptr<peg::Ast> parse_or_die(const char* code) {
  static std::deque<std::string> sources;
  sources.emplace_back(code);
  std::vector<std::string> msgs;
  auto ast = culebra::parse("<mi>", sources.back(), msgs);
  if (!ast) {
    for (auto& m : msgs) std::cerr << m;
    std::exit(1);
  }
  return ast;
}

}  // namespace

int run() {
  culebra::Runtime rt_a, rt_b;

  // One Embed per Runtime; constructed under that Runtime's scope so its
  // stdlib install and traits land there.
  std::unique_ptr<culebra::vm::Embed> embed_a, embed_b;

  // Initialize each Runtime independently.
  {
    culebra::RuntimeScope scope(rt_a);
    embed_a = std::make_unique<culebra::vm::Embed>();
    culebra::vm::Value v;
    std::vector<std::string> msgs;
    embed_a->run_source("<mi>", kScriptInit, v, msgs);
  }
  {
    culebra::RuntimeScope scope(rt_b);
    embed_b = std::make_unique<culebra::vm::Embed>();
    culebra::vm::Value v;
    std::vector<std::string> msgs;
    embed_b->run_source("<mi>", kScriptInit, v, msgs);
  }

  // Alternate calls into each Runtime's `step` function via the embedding
  // helper. Each Runtime should see only its own counter.
  bool ok = true;
  for (int i = 1; i <= 5; ++i) {
    {
      culebra::RuntimeScope scope(rt_a);
      auto v = embed_a->call("step");
      if (v.to_long() != i) {
        std::cerr << "A step " << i << " got " << v.to_long() << "\n";
        ok = false;
      }
    }
    {
      culebra::RuntimeScope scope(rt_b);
      auto v = embed_b->call("step");
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

  // Tear the Embeds down under their own Runtime's scope (their cells and
  // teardown hooks belong to it).
  {
    culebra::RuntimeScope scope(rt_a);
    embed_a.reset();
  }
  {
    culebra::RuntimeScope scope(rt_b);
    embed_b.reset();
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

  // A stdlib-less Runtime compiling plain `!=` -- no namespace, nothing an
  // extension would need to have installed. The built-in Eq trait's `neq`
  // default (`!self.eq(other)`, evaluated for every program regardless of
  // content) resolves its `.eq` call to whichever BMethSpec is registered
  // under that name; once Tensor registered one, every program's compiled
  // code carries a call into the Tensor runtime, gated at runtime by the
  // receiver's tag. Declaring that function only from Tensor's own
  // declare_runtime left a host that never installs stdlib with an
  // undeclared function and a null-deref in JIT::emit_call -- a crash, not
  // a script-level failure, in code that never mentions Tensor and has
  // every reason to expect at least a clean result from a stdlib-less JIT.
  // No namespace call here (Math/IO would need stdlib too and confuse what
  // failed); a bare comparison is the whole point.
  {
    culebra::Runtime jit_rt_c;
    culebra::RuntimeScope scope(jit_rt_c);
    // No install_jit_stdlib(): this Runtime has empty hooks, same as rt_b.
    auto ast = parse_or_die("1 != 2");
    try {
      culebra::JIT::run(ast);
    } catch (const std::exception& e) {
      std::cerr << "rt_c JIT (no stdlib, plain !=) unexpectedly threw: "
                << e.what() << "\n";
      ok = false;
    }
  }
#endif

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace mi_smoke_ns
