// Cooperative-interrupt (Ctrl+C) mechanism, in-process and deterministic.
//
// `tests/signal_test.sh` drives the binary and sends a real SIGINT, but that
// needs POSIX signals + process control + timing, so it can't run on a native
// Windows CI and is mildly timing-sensitive. This test exercises the portable
// half — the cooperative throw itself — by setting the process SIGINT flag
// directly (no signal, no child process, no sleeps) and checking that the
// VM executor and the JIT throw a catchable `Interrupted`, that the flag is
// one-shot (consumed on the throw, so a `catch` can resume), and that an
// isolate-style sticky cancel keeps its own message and is not consumed. Runs
// on any platform that builds culebra.

#include <atomic>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <culebra.h>
#include <stdlib_jit.h>
#include <vm_embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace signal_smoke_ns {

namespace {

int failures = 0;
void expect(bool cond, const char* what) {
  if (!cond) {
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
  }
}

// parse()'s AST holds string_view tokens into its source buffer, and
// callers here keep using the returned AST beyond this call, so the buffer
// needs a stable, permanent home. A deque never relocates existing elements
// on growth (unlike vector, which could move a short string's SSO storage),
// matching repl.h's retained_sources_.
std::shared_ptr<peg::Ast> parse_or_die(const char* code) {
  static std::deque<std::string> sources;
  sources.emplace_back(code);
  std::vector<std::string> msgs;
  auto ast = culebra::parse("<sig>", sources.back(), msgs);
  if (!ast) {
    for (auto& m : msgs) std::cerr << m;
    std::exit(2);
  }
  return ast;
}

// Loops bounded high enough that, if the safepoint/poll never fires (a broken
// mechanism), the program finishes WITHOUT throwing — so the test fails cleanly
// rather than hanging. With the flag pre-set the very first check throws, so
// the bound is never actually reached on the passing path.
const char* kLoop = "mut i = 0\nwhile i < 100000000 { i = i + 1 }\ni";
const char* kCatchLoop =
    "mut i = 0\n"
    "try { while i < 100000000 { i = i + 1 } } catch e { }\n"
    "0";

}  // namespace

int run() {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::install_jit_stdlib();

  // --- 1. throw_if_interrupted: throws "interrupted" and consumes (one-shot).
  culebra::request_interrupt();
  {
    bool threw = false;
    try {
      culebra::throw_if_interrupted();
    } catch (const culebra::CulebraError& e) {
      threw = true;
      expect(e.kind == "Interrupted", "throw_if_interrupted kind");
      expect(std::string(e.what()) == "interrupted",
             "throw_if_interrupted message");
      expect(e.line == 0 && e.col == 0, "Interrupted has no source position");
    }
    expect(threw, "throw_if_interrupted threw when flag set");
  }
  expect(!culebra::culebra_g_sigint.load(), "flag consumed (one-shot)");
  culebra::throw_if_interrupted();  // no-op when clear — must not throw

  // --- 2. isolate-style sticky cancel: different message, NOT consumed.
  {
    std::atomic<bool> iso{true};
    culebra::current_runtime().interrupt_flag = &iso;
    bool threw = false;
    try {
      culebra::throw_if_interrupted();
    } catch (const culebra::CulebraError& e) {
      threw = true;
      expect(e.kind == "Interrupted", "isolate cancel kind");
      expect(std::string(e.what()) == "isolate cancelled",
             "isolate cancel message");
    }
    expect(threw, "isolate sticky flag throws");
    expect(iso.load(), "isolate cancel is sticky (not consumed)");
    culebra::current_runtime().interrupt_flag = nullptr;
  }

  // --- 3. VM executor: a running program throws Interrupted, flag consumed.
  {
    culebra::vm::Embed embed;
    culebra::request_interrupt();
    culebra::vm::Value v;
    std::vector<std::string> msgs;
    bool ran = embed.run_source("<sig>", kLoop, v, msgs);
    expect(!ran, "vm program interrupted");
    expect(!msgs.empty() &&
               msgs.back().find("Interrupted") != std::string::npos,
           "vm interrupted kind");
    expect(!culebra::culebra_g_sigint.load(), "vm consumed flag");
  }

  // --- 3b. VM catch-continue: the interrupt is caught in-script and the
  // program resumes — the run succeeds.
  {
    culebra::vm::Embed embed;
    culebra::request_interrupt();
    culebra::vm::Value v;
    std::vector<std::string> msgs;
    bool ran = embed.run_source("<sig>", kCatchLoop, v, msgs);
    expect(ran, "vm catch-continue: interrupt caught, not propagated");
    expect(!culebra::culebra_g_sigint.load(),
           "vm catch-continue consumed flag");
  }

#ifdef CULEBRA_JIT_ENABLED
  // --- 4. JIT: the loop safepoint throws Interrupted, flag consumed.
  {
    auto ast = parse_or_die(kLoop);
    culebra::request_interrupt();
    bool threw = false;
    try {
      culebra::JIT::run(ast);
    } catch (const culebra::CulebraError& e) {
      threw = (e.kind == "Interrupted");
      expect(e.kind == "Interrupted", "jit interrupted kind");
    } catch (const std::exception& e) {
      expect(false, "jit threw a non-CulebraError");
    }
    expect(threw, "jit loop safepoint interrupted");
    expect(!culebra::culebra_g_sigint.load(), "jit consumed flag");
  }

  // --- 5. JIT catch-continue: the interrupt fires inside the try (at the loop
  // safepoint), is caught, and the program resumes — JIT::run does NOT throw.
  {
    auto ast = parse_or_die(kCatchLoop);
    culebra::request_interrupt();
    bool propagated = false;
    try {
      culebra::JIT::run(ast);
    } catch (...) {
      propagated = true;
    }
    expect(!propagated, "jit catch-continue: interrupt caught, not propagated");
    expect(!culebra::culebra_g_sigint.load(),
           "jit catch-continue consumed flag");
  }
#endif

  if (failures == 0) std::cout << "signal_smoke OK\n";
  return failures ? 1 : 0;
}

}  // namespace signal_smoke_ns
