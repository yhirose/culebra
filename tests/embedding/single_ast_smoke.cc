// Single-input embedder entries must behave like the module path.
// `vm::Embed::run_source` and `culebra::JIT::run(ast)` bypass the loader,
// and both have to see the built-in trait preamble — otherwise `Comparable`
// is an undefined name and its default methods (lt/le/gt/ge, Eq's neq) are
// missing on the embedder API only. (The interp-era `culebra::interpret`
// lane this exercised retired with the tree-walker — Phase 4 B7-d.)

#include <deque>
#include <iostream>
#include <string>

#include <culebra.h>
#include <stdlib/bindings.h>
#include <vm/embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace single_ast_smoke_ns {

namespace {

// Exercises only preamble-provided machinery: the six Comparable defaults on
// top of a hand-written `cmp`, Eq's default `neq`, and `Comparable` as a
// parameter type. Every check throws on failure, so both lanes report the
// same way — Embed via `msgs`, JIT via std::runtime_error.
constexpr const char* kScript = R"(
class Pri {
  new(v) {
    self.v = v
  }
  cmp(other) {
    self.v - other.v
  }
  eq(other) {
    self.v == other.v
  }
}
let lo = Pri.new(1)
let hi = Pri.new(5)
if !lo.lt(hi) { throw 'lt' }
if !lo.le(lo) { throw 'le' }
if !hi.gt(lo) { throw 'gt' }
if !hi.ge(hi) { throw 'ge' }
if !lo.neq(hi) { throw 'neq' }
fn only_comparable(x: Comparable) -> Long {
  x.cmp(x)
}
if only_comparable(lo) != 0 { throw 'trait param' }
)";

// parse()'s AST holds string_view tokens into its source buffer, and the AST
// outlives this call, so the buffer needs a stable home (see repl.h's
// retained_sources_; Embed::run_source owns its copy instead).
std::shared_ptr<peg::Ast> parse_or_die(const char* code) {
  static std::deque<std::string> sources;
  sources.emplace_back(code);
  std::vector<std::string> msgs;
  auto ast = culebra::parse("<single-ast>", sources.back(), msgs);
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
  bool ok = true;

  {
    culebra::Runtime rt;
    culebra::RuntimeScope scope(rt);
    culebra::vm::Embed embed;
    culebra::vm::Value v;
    std::vector<std::string> msgs;
    bool ran = embed.run_source("<single-ast>", kScript, v, msgs);
    if (!ran) {
      for (auto& m : msgs) std::cerr << m << "\n";
    }
    ok &= check(ran, "Embed::run_source sees the built-in traits");
  }

#ifdef CULEBRA_JIT_ENABLED
  {
    culebra::Runtime rt;
    culebra::RuntimeScope scope(rt);
    culebra::install_jit_stdlib();
    bool ran = true;
    try {
      culebra::JIT::run(parse_or_die(kScript));
    } catch (const std::exception& e) {
      std::cerr << e.what() << "\n";
      ran = false;
    }
    ok &= check(ran, "JIT::run(ast) sees the built-in traits");
  }
#endif

  std::cout << (ok ? "OK\n" : "FAIL\n");
  return ok ? 0 : 1;
}

}  // namespace single_ast_smoke_ns
