// Unit test for the fragment ledger's caller-owned form
// (include/frontend/generator_transform.h, effects_transform.h).
//
// A lowering re-parses the source it synthesizes, and the AST views those
// buffers. By default the process owns them for good; the parse_with_transforms
// overload that takes a FragmentLedger hands them to the caller instead, so a
// long-lived analyser (an editor re-linting a buffer on every keystroke) frees
// them with the ASTs rather than growing without bound.
//
// Built and run by CTest (see CMakeLists.txt).

#include <frontend/effects_transform.h>

#include <cstdio>
#include <format>
#include <memory>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  std::printf("FAIL: %s\n", what.c_str());
  failures++;
}

// Both lowerings: a generator and an effect handler each synthesize fragments.
constexpr const char* kSource = R"(fn counter() {
  yield 1
  yield 2
}
effect fn ask()
let x = handle {
  let n = perform ask()
  n + 1
} with ask(resume) {
  resume(10)
}
)";

size_t process_fragments() {
  return culebra::fragment_sources_snapshot().size();
}

// The tree's shape by rule name alone: synthesized names carry counters that
// differ between two lowerings of one source, the shape does not.
std::string shape(const peg::Ast& n) {
  std::string s = n.name;
  for (const auto& c : n.nodes) s += "(" + shape(*c) + ")";
  return s;
}

void test_owned_lowering_leaves_the_process_ledger_alone() {
  size_t before = process_fragments();
  culebra::FragmentLedger fragments;
  std::string src = kSource;
  std::vector<std::string> msgs;
  auto ast = culebra::parse_with_transforms("(owned)", src, msgs, fragments);
  check(ast != nullptr, "the owned lowering parses");
  check(!fragments.sources.empty(), "the caller's ledger holds the fragments");
  check(process_fragments() == before,
        std::format("the process ledger did not grow ({} -> {})", before,
                    process_fragments()));
}

void test_fragments_are_freed_with_their_ledger() {
  std::vector<std::weak_ptr<std::string>> watched;
  {
    culebra::FragmentLedger fragments;
    std::string src = kSource;
    std::vector<std::string> msgs;
    auto ast = culebra::parse_with_transforms("(freed)", src, msgs, fragments);
    check(ast != nullptr, "the lowering to free parses");
    for (const auto& f : fragments.sources) watched.push_back(f);
  }
  check(!watched.empty(), "there were fragments to watch");
  size_t alive = 0;
  for (const auto& w : watched) alive += !w.expired();
  check(alive == 0, std::format("{} fragment(s) outlived their ledger", alive));
}

void test_owned_lowering_builds_the_same_tree() {
  std::string a = kSource, b = kSource;
  std::vector<std::string> ma, mb;
  culebra::FragmentLedger fragments;
  auto shared = culebra::parse_with_transforms("(shared)", a, ma);
  auto owned = culebra::parse_with_transforms("(owned)", b, mb, fragments);
  check(shared && owned, "both lowerings parse");
  if (shared && owned)
    check(shape(*shared) == shape(*owned),
          "the owned lowering builds the same tree");
}

void test_repeated_analysis_does_not_grow_the_process() {
  size_t before = process_fragments();
  for (int i = 0; i < 50; i++) {
    culebra::FragmentLedger fragments;
    std::string src = kSource;
    std::vector<std::string> msgs;
    auto ast = culebra::parse_with_transforms("(again)", src, msgs, fragments);
    if (!ast) {
      check(false, std::format("iteration {} parses", i));
      return;
    }
  }
  check(process_fragments() == before,
        "fifty owned lowerings leave the process ledger as it was");
}

}  // namespace

int main() {
  test_owned_lowering_leaves_the_process_ledger_alone();
  test_fragments_are_freed_with_their_ledger();
  test_owned_lowering_builds_the_same_tree();
  test_repeated_analysis_does_not_grow_the_process();

  if (failures) {
    std::printf("fragment_ledger_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("fragment_ledger_test OK\n");
  return 0;
}
