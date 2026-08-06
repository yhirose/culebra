// Smoke test: multiple host threads each parse + interpret a script
// independently. Verifies thread_local isolation of GC, defer stack,
// exception carriers, PRNG, and the PEG parser itself.

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <culebra.h>
#include <stdlib_interp.h>
#ifdef CULEBRA_JIT_ENABLED
#include <stdlib_jit.h>
#endif

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace mt_smoke_ns {

namespace {

// parse()'s AST holds string_view tokens into its source buffer, and
// run_interp/run_jit below use the AST well past the parse call — so these
// need a real, permanently-owned std::string, not a const char* (which
// would force a short-lived temporary at each culebra::parse call).
const std::string kScript = R"(
let mut acc = 0
for i in 0..1000 {
  try {
    if i % 7 == 0 { throw "skip" }
    acc = acc + i
  } catch e {
    acc = acc + 0
  }
}
acc
)";

long expected_sum() {
  long s = 0;
  for (int i = 0; i < 1000; ++i) if (i % 7 != 0) s += i;
  return s;
}

// The script throws "bad" if `acc` doesn't match the expected sum,
// so JIT::run will surface a runtime_error and we can detect it.
const std::string kCheckedScript = R"(
let mut acc = 0
for i in 0..1000 {
  try {
    if i % 7 == 0 { throw "skip" }
    acc = acc + i
  } catch e {
    acc = acc + 0
  }
}
if acc != 428429 { throw "bad" }
)";

std::atomic<int> failures{0};

void run_interp(int tid) {
  for (int rep = 0; rep < 50; ++rep) {
    std::vector<std::string> msgs;
    auto ast = culebra::parse("<mt>", kScript, msgs);
    if (!ast) {
      std::cerr << "parse tid=" << tid << " rep=" << rep << " failed\n";
      ++failures;
      continue;
    }
    auto env = culebra::environment();
    culebra::Value val;
    if (!culebra::interpret(ast, env, val, msgs, culebra::Debugger())) {
      std::cerr << "interp tid=" << tid << " rep=" << rep << " threw:";
      for (auto& m : msgs) std::cerr << " " << m;
      std::cerr << "\n";
      ++failures;
      continue;
    }
    if (val.to_long() != expected_sum()) {
      std::cerr << "interp tid=" << tid << " rep=" << rep
                << " got=" << val.to_long() << "\n";
      ++failures;
    }
  }
}

}  // namespace

#ifdef CULEBRA_JIT_ENABLED
void run_jit(int tid) {
  for (int rep = 0; rep < 10; ++rep) {
    std::vector<std::string> msgs;
    auto ast = culebra::parse("<mt-jit>", kCheckedScript, msgs);
    if (!ast) {
      std::cerr << "jit-parse tid=" << tid << " rep=" << rep << " failed\n";
      ++failures;
      continue;
    }
    try {
      culebra::JIT::run(ast);
    } catch (const std::exception& e) {
      std::cerr << "jit tid=" << tid << " rep=" << rep
                << " threw: " << e.what() << "\n";
      ++failures;
    }
  }
}
#endif

int run() {
#ifdef CULEBRA_JIT_ENABLED
  culebra::install_jit_stdlib();
#endif
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i) ts.emplace_back(run_interp, i);
#ifdef CULEBRA_JIT_ENABLED
  for (int i = 0; i < 4; ++i) ts.emplace_back(run_jit, i);
#endif
  for (auto& t : ts) t.join();
  std::cout << (failures == 0 ? "OK\n" : "FAIL\n");
  return failures == 0 ? 0 : 1;
}

}  // namespace mt_smoke_ns
