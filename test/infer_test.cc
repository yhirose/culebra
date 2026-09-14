// Unit test for editor type inference (include/cli/infer.h).
//
// The cases pin what inference establishes. The corpus pass measures it: of
// every method call in the tracked .cul files, how many receivers get a known
// type, and why the rest do not — which decides what inference should learn
// next. The measurement prints and never fails.
//
// Usage: infer_test <source-dir>     (built and run by CTest)

#include <cli/infer.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace culebra;
using infer::Kind;
using infer::MemberKind;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  std::printf("FAIL: %s\n", what.c_str());
  failures++;
}

// A catalog with one namespace and one global, standing in for the stdlib.
class FakeCatalog : public infer::Catalog {
 public:
  FakeCatalog() {
    math_.push_back({"abs", MemberKind::Function, "abs(x) -> Long | Float",
                     infer::parse_type("Long | Float")});
    math_.push_back({"pi", MemberKind::Constant, "pi: Float",
                     infer::Type::of(Kind::Float)});
    globals_.push_back({"Math", MemberKind::Namespace, "Math", {}});
    globals_.push_back({"type_of", MemberKind::Function, "type_of(v) -> String",
                        infer::Type::of(Kind::String)});
  }
  const std::vector<infer::Member>* namespace_members(
      std::string_view path) const override {
    return path == "Math" ? &math_ : nullptr;
  }
  const std::vector<infer::Member>& globals() const override { return globals_; }

 private:
  std::vector<infer::Member> math_, globals_;
};

const FakeCatalog kCatalog;

struct Analysed {
  std::string src;
  std::shared_ptr<peg::Ast> ast;
  resolve::Resolution res;
  std::unique_ptr<infer::Inference> inf;
};

std::unique_ptr<Analysed> analyse(std::string src) {
  auto a = std::make_unique<Analysed>();
  a->src = std::move(src);
  std::vector<ParseFailure> pf;
  a->ast = parse("(test)", a->src, pf);
  if (!a->ast) {
    check(false, "parses: " + a->src);
    return nullptr;
  }
  a->res = resolve::resolve_module(*a->ast, a->src);
  a->inf = std::make_unique<infer::Inference>(*a->ast, a->src, a->res, &kCatalog);
  return a;
}

// The type of the name written last in the source.
std::string type_of_last(const Analysed& a, std::string_view name) {
  size_t pos = a.src.rfind(name);
  const auto* o = a.res.occurrence_at(pos);
  if (!o || o->position != pos) return "<no symbol>";
  return a.inf->symbol_type(o->symbol).to_string();
}

bool has_member(const std::vector<infer::Member>& ms, std::string_view name,
                MemberKind kind) {
  return std::any_of(ms.begin(), ms.end(), [&](const infer::Member& m) {
    return m.name == name && m.kind == kind;
  });
}

std::vector<infer::Member> members_of_last(const Analysed& a,
                                           std::string_view name) {
  size_t pos = a.src.rfind(name);
  const auto* o = a.res.occurrence_at(pos);
  if (!o) return {};
  return a.inf->members(a.inf->symbol_type(o->symbol));
}

void eq(const std::string& got, const std::string& want, const char* what) {
  check(got == want, std::format("{}: got `{}`, want `{}`", what, got, want));
}

void test_values() {
  if (auto a = analyse("let s = 'a,b'\nlet parts = s.split(',')\nparts\n")) {
    eq(type_of_last(*a, "parts"), "Array<String>", "a built-in method's return type");
    check(has_member(members_of_last(*a, "parts"), "join", MemberKind::Method),
          "an array's members are the array methods");
  }
  if (auto a = analyse("let o = {count: 1, label: 'x'}\no\n")) {
    auto ms = members_of_last(*a, "o");
    check(has_member(ms, "count", MemberKind::Field) &&
              has_member(ms, "label", MemberKind::Field),
          "an object literal's keys are fields");
    check(has_member(ms, "keys", MemberKind::Method), "and the object methods");
  }
  if (auto a = analyse("mut v = 1\nv = 'a'\nv\n"))
    eq(type_of_last(*a, "v"), "Long | String", "assignments join");
  if (auto a = analyse("let x = nil ?? 'a'\nx\n"))
    eq(type_of_last(*a, "x"), "String", "?? drops the nil");
  if (auto a = analyse("for w in 'a b'.split(' ') { w }\n"))
    eq(type_of_last(*a, "w"), "String", "a loop variable is the element type");
  if (auto a = analyse("let n = 1 + 2.5\nn\n"))
    eq(type_of_last(*a, "n"), "Float", "mixed arithmetic is Float");
}

void test_functions() {
  if (auto a = analyse("fn make() {\n  {size: 1}\n}\nlet m = make()\nm\n")) {
    eq(type_of_last(*a, "m"), "Object", "a function returns its tail");
    check(has_member(members_of_last(*a, "m"), "size", MemberKind::Field),
          "and what it returns keeps its keys");
  }
  if (auto a = analyse("fn pick(x) {\n  return 'a' if x\n  1\n}\nlet p = pick(true)\np\n"))
    eq(type_of_last(*a, "p"), "Long | String", "a return joins the tail");
  if (auto a = analyse("fn f(xs: Array<Long>) { xs }\n"))
    eq(type_of_last(*a, "xs"), "Array<Long>", "a parameter's annotation");
  if (auto a = analyse("fn gen() { yield 1 }\nlet it = gen()\nit\n"))
    eq(type_of_last(*a, "it"), "Iterator<Long>", "a generator yields an iterator");
  if (auto a = analyse("fn label(v) { 'x' }\nlet q = 3\nlet r = q.label()\nr\n")) {
    eq(type_of_last(*a, "r"), "String", "a method call falls back to UFCS");
    auto candidates = a->inf->ufcs(a->src.rfind("r"));
    check(has_member(candidates, "label", MemberKind::Function),
          "UFCS candidates list the visible free functions");
  }
}

void test_callbacks() {
  if (auto a = analyse("let words = 'a b'.split(' ')\nlet loud = words.map(|word| word.upper())\nloud\n")) {
    eq(type_of_last(*a, "loud"), "Array<String>", "map gives an array of what the function returns");
    eq(type_of_last(*a, "word"), "String", "a callback's parameter is the receiver's element");
  }
  if (auto a = analyse("let total = [1, 2].reduce(0, |acc, n| acc + n)\ntotal\n"))
    eq(type_of_last(*a, "total"), "Long", "reduce gives the initial value's type");
  if (auto a = analyse("let hit = ['a'].find(|s| s.empty())\nhit\n"))
    eq(type_of_last(*a, "hit"), "String | Nil", "find gives an element or nil");
  if (auto a = analyse("let evens = 'a b'.split_iter(' ').filter(|w| w.empty())\nevens\n"))
    eq(type_of_last(*a, "evens"), "Iterator<String>", "filter keeps the receiver's element");
}

void test_call_sites() {
  if (auto a = analyse("fn shout(said) { said }\nshout('a')\nshout(b: 1) if false\n"))
    eq(type_of_last(*a, "said"), "String", "a parameter takes what its calls pass");
  if (auto a = analyse("fn tally(count) { count }\ntally(count: 2)\n"))
    eq(type_of_last(*a, "count"), "Long", "a keyword argument binds by name");
  if (auto a = analyse("fn twice(text) { text }\nlet r = 'a'.twice()\n"))
    eq(type_of_last(*a, "text"), "String", "a UFCS receiver is the first argument");
  if (auto a = analyse("fn grow(step = 1) { step }\n"))
    eq(type_of_last(*a, "step"), "Long", "a default is a value the parameter takes");
  if (auto a = analyse("fn ident(value) { value }\nlet alias = ident\nident(1)\n"))
    eq(type_of_last(*a, "value"), "", "a function passed as a value has callers out of view");
  if (auto a = analyse("fn size(xs) { xs }\n[1].size()\n"))
    eq(type_of_last(*a, "xs"), "", "a method the receiver owns is not a UFCS call");
}

void test_classes() {
  if (auto a = analyse("class Box {\n  new(w) { self.w = w }\n  area() { self.w * 2 }\n"
                       "  static unit() { Box(1) }\n}\nlet b = Box(3)\nb\n")) {
    eq(type_of_last(*a, "b"), "Box", "calling a class makes an instance");
    auto ms = members_of_last(*a, "b");
    check(has_member(ms, "area", MemberKind::Method), "an instance lists its methods");
    check(has_member(ms, "w", MemberKind::Field), "and the fields its methods set");
    check(!has_member(ms, "new", MemberKind::Constructor) &&
              !has_member(ms, "unit", MemberKind::Method),
          "but not the constructor or a static method");
  }
  if (auto a = analyse("class Box {\n  static unit() { Box() }\n}\nlet unit_box = Box.unit()\nlet made = Box.new()\nunit_box\nmade\nBox\n")) {
    eq(type_of_last(*a, "unit_box"), "Box", "a static method's return type");
    eq(type_of_last(*a, "made"), "Box", "Class.new() makes an instance");
    auto ms = members_of_last(*a, "Box");
    check(has_member(ms, "new", MemberKind::Constructor) &&
              has_member(ms, "unit", MemberKind::Method),
          "the class value lists new and its static methods");
  }
  if (auto a = analyse("class Box {\n  new() { self.size = 1 }\n  me() { self }\n}\nlet me_box = Box().me()\nme_box\n")) {
    eq(type_of_last(*a, "me_box"), "Box", "self is an instance of its class");
    check(has_member(members_of_last(*a, "me_box"), "size", MemberKind::Field),
          "and has the fields its methods set");
  }
}

void test_catalog_and_scope() {
  if (auto a = analyse("let absolute = Math.abs(-1)\nlet circle = Math.pi\nlet kind = type_of(absolute)\ncircle\nkind\n")) {
    eq(type_of_last(*a, "absolute"), "Long | Float", "a namespace member's return type");
    eq(type_of_last(*a, "circle"), "Float", "a namespace constant");
    eq(type_of_last(*a, "kind"), "String", "a global function's return type");
  }
  if (auto a = analyse("let a = 1\nfn f(p) {\n  let b = 2\n  here\n  let c = 3\n}\nlet z = 1\n")) {
    auto names = a->inf->visible(a->src.find("here"));
    auto has = [&](std::string_view n) {
      return std::any_of(names.begin(), names.end(),
                         [&](const infer::Member& m) { return m.name == n; });
    };
    check(has("a") && has("f") && has("p") && has("b") && has("z"),
          "visible names: earlier in the function, anywhere around it");
    check(!has("c"), "a later declaration in the same function is not visible");
  }
}

// ---- corpus measurement -----------------------------------------------------

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Tally {
  size_t calls = 0, known = 0;
  size_t unknown_parameter = 0, unknown_global = 0, unknown_self = 0,
         unknown_chain = 0, unknown_variable = 0, unknown_index = 0,
         unknown_other = 0;
};

void measure(const peg::Ast& n, const Analysed& a, Tally& t) {
  using namespace peg::udl;
  if (n.tag == "CALL"_) {
    for (size_t i = 1; i + 1 < n.nodes.size(); i++) {
      const auto& c = *n.nodes[i];
      if (c.tag != "IDENTIFIER"_ || c.original_tag != "DOT"_) continue;
      if (n.nodes[i + 1]->original_tag != "ARGUMENTS"_) continue;
      t.calls++;
      infer::Type r = a.inf->chain_type(n, i);
      if (!r.unknown()) {
        t.known++;
        continue;
      }
      const auto& head = *n.nodes[0];
      // Known up to an earlier link of the chain, unknown from there on.
      if (i > 1) {
        bool earlier_known = false;
        for (size_t j = 1; j < i && !earlier_known; j++)
          earlier_known = !a.inf->chain_type(n, j).unknown();
        if (earlier_known || !a.inf->expr_type(head).unknown()) {
          t.unknown_chain++;
          continue;
        }
        const auto& prev = *n.nodes[i - 1];
        if (prev.original_tag == "INDEX"_ || prev.tag == "INDEX"_) {
          t.unknown_index++;
          continue;
        }
      }
      if (i == 1 && head.tag == "IDENTIFIER"_) {
        if (head.token == "self") {
          t.unknown_self++;
          continue;
        }
        const auto* o = a.res.occurrence_at(
            static_cast<size_t>(head.token.data() - a.src.data()));
        if (!o) {
          t.unknown_global++;
          continue;
        }
        if (a.res.symbols[o->symbol].kind == resolve::SymbolKind::Parameter) {
          t.unknown_parameter++;
          continue;
        }
        t.unknown_variable++;
        continue;
      }
      t.unknown_other++;
    }
  }
  for (const auto& c : n.nodes) measure(*c, a, t);
}

void measure_corpus(const std::filesystem::path& root) {
  Tally t;
  size_t files = 0;
  for (const char* dir : {"tests", "examples"}) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root / dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file() || it->path().extension() != ".cul") continue;
      auto a = std::make_unique<Analysed>();
      a->src = read_file(it->path());
      std::vector<ParseFailure> pf;
      a->ast = parse(it->path().string(), a->src, pf);
      if (!a->ast) continue;
      a->res = resolve::resolve_module(*a->ast, a->src);
      // No catalog: a stdlib namespace's call counts as unknown here.
      a->inf = std::make_unique<infer::Inference>(*a->ast, a->src, a->res);
      files++;
      measure(*a->ast, *a, t);
    }
  }
  auto pct = [&](size_t n) { return t.calls ? 100.0 * n / t.calls : 0.0; };
  std::printf("infer_test: corpus %zu files, %zu method calls\n", files, t.calls);
  std::printf("  receiver type known          %6zu  %5.1f%%\n", t.known, pct(t.known));
  std::printf("  unknown: a parameter         %6zu  %5.1f%%\n", t.unknown_parameter,
              pct(t.unknown_parameter));
  std::printf("  unknown: self                %6zu  %5.1f%%\n", t.unknown_self,
              pct(t.unknown_self));
  std::printf("  unknown: a global/namespace  %6zu  %5.1f%%\n", t.unknown_global,
              pct(t.unknown_global));
  std::printf("  unknown: later in a chain    %6zu  %5.1f%%\n", t.unknown_chain,
              pct(t.unknown_chain));
  std::printf("  unknown: a variable          %6zu  %5.1f%%\n", t.unknown_variable,
              pct(t.unknown_variable));
  std::printf("  unknown: after an index      %6zu  %5.1f%%\n", t.unknown_index,
              pct(t.unknown_index));
  std::printf("  unknown: other               %6zu  %5.1f%%\n", t.unknown_other,
              pct(t.unknown_other));
  check(files > 400, std::format("the corpus was read ({} files)", files));
}

}  // namespace

int main(int argc, char** argv) {
  test_values();
  test_functions();
  test_classes();
  test_callbacks();
  test_call_sites();
  test_catalog_and_scope();
  if (argc >= 2) measure_corpus(argv[1]);

  if (failures) {
    std::printf("infer_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("infer_test OK\n");
  return 0;
}
