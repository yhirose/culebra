// Smoke test: multiple host threads each parse + run a script
// independently. Verifies thread_local isolation of GC, defer stack,
// exception carriers, PRNG, the PEG parser, and the embedding session
// (vm::Embed swaps a thread_local session pointer — Phase 4 B7-d), and
// that the Shapes, which the threads do share, are interned as one set.

#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <culebra.h>
#include <stdlib/bindings.h>
#include <vm/embed.h>

// Unity-TU entry (smoke_suite.cc): the named namespace keeps
// this file's internals from colliding with the other smokes.
namespace mt_smoke_ns {

namespace {

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

void run_vm(int tid) {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  culebra::vm::Embed embed;
  for (int rep = 0; rep < 50; ++rep) {
    culebra::vm::Value val;
    std::vector<std::string> msgs;
    if (!embed.run_source("<mt>", kScript, val, msgs)) {
      std::cerr << "vm tid=" << tid << " rep=" << rep << " threw:";
      for (auto& m : msgs) std::cerr << " " << m;
      std::cerr << "\n";
      ++failures;
      continue;
    }
    if (val.to_long() != expected_sum()) {
      std::cerr << "vm tid=" << tid << " rep=" << rep
                << " got=" << val.to_long() << "\n";
      ++failures;
    }
  }
}

// Every thread that adds the same property to the same Shape gets the same
// Shape, while the others are adding more to it: a transition is read with
// no lock, so this is what a lost or doubled child would break.
constexpr int kShapeThreads = 8;
constexpr int kShapeKeys = 300;

void intern_shapes(const std::atomic<bool>* go,
                   std::vector<culebra::Shape*>* got) {
  while (!go->load()) std::this_thread::yield();
  auto& shapes = culebra::shape_registry();
  auto* parent = shapes.transition_add(shapes.root(), "mt_smoke_parent");
  for (int i = 0; i < kShapeKeys; ++i) {
    auto name = "mt_smoke_key" + std::to_string(i);
    got->push_back(shapes.transition_add(parent, name));
    got->push_back(
        shapes.transition_add(parent, name, culebra::FieldType::Long));
  }
}

void check_shapes() {
  std::vector<std::vector<culebra::Shape*>> got(kShapeThreads);
  std::atomic<bool> go{false};  // the threads start together
  std::vector<std::thread> ts;
  for (auto& mine : got) ts.emplace_back(intern_shapes, &go, &mine);
  go = true;
  for (auto& t : ts) t.join();
  for (auto& mine : got) {
    if (mine != got[0]) {
      std::cerr << "shapes: two threads interned different Shapes\n";
      ++failures;
    }
  }
  for (int i = 0; i < 2 * kShapeKeys; ++i) {
    auto* s = got[0][i];
    auto want = i % 2 ? culebra::FieldType::Long : culebra::FieldType::Any;
    if (s->names.back() != "mt_smoke_key" + std::to_string(i / 2) ||
        s->type_at(s->names.size() - 1) != want) {
      std::cerr << "shapes: transition " << i << " is another's\n";
      ++failures;
    }
  }
}

}  // namespace

#ifdef CULEBRA_JIT_ENABLED
void run_jit(int tid) {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  for (int rep = 0; rep < 10; ++rep) {
    std::vector<std::string> msgs;
    // Each thread parses its own copy: the parse normalizes newlines in the
    // buffer it is given, and the AST's tokens point into it, so a buffer
    // shared across threads would be written by all of them.
    std::string src = kCheckedScript;
    auto ast = culebra::parse("<mt-jit>", src, msgs);
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
  culebra::install_jit_stdlib();
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i) ts.emplace_back(run_vm, i);
#ifdef CULEBRA_JIT_ENABLED
  for (int i = 0; i < 4; ++i) ts.emplace_back(run_jit, i);
#endif
  for (auto& t : ts) t.join();
  check_shapes();
  std::cout << (failures == 0 ? "OK\n" : "FAIL\n");
  return failures == 0 ? 0 : 1;
}

}  // namespace mt_smoke_ns
