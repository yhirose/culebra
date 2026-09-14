// Unit test for static name resolution (include/frontend/resolve.h).
//
// The cases pin the scoping rules the compiler implements — each one was
// checked against what `culebra` does when the program runs. The corpus check
// then holds the resolver against the lint's undefined-variable analysis over
// every tracked .cul file: that analysis binds a name anywhere in its function,
// so a read it cannot bind must be one the resolver cannot bind either.
//
// Usage: resolve_test <source-dir>     (built and run by CTest)

#include <frontend/lint.h>
#include <frontend/resolve.h>

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace culebra::resolve;

int failures = 0;

void check(bool ok, const std::string& what) {
  if (ok) return;
  std::printf("FAIL: %s\n", what.c_str());
  failures++;
}

struct Parsed {
  std::string src;
  std::shared_ptr<peg::Ast> ast;
  Resolution res;
};

std::unique_ptr<Parsed> resolve_source(std::string src) {
  auto p = std::make_unique<Parsed>();
  p->src = std::move(src);
  std::vector<culebra::ParseFailure> failures;
  p->ast = culebra::parse("(test)", p->src, failures);
  if (!p->ast) {
    check(false, "parses: " + p->src);
    return nullptr;
  }
  p->res = resolve_module(*p->ast, p->src);
  return p;
}

// The byte offset of the `nth` (0-based) `needle` in `src`.
size_t at(const Parsed& p, std::string_view needle, int nth) {
  size_t pos = 0;
  for (int i = 0;; i++) {
    pos = p.src.find(needle, i == 0 ? 0 : pos + 1);
    if (pos == std::string::npos || i == nth) return pos;
  }
}

size_t sym(const Parsed& p, std::string_view needle, int nth) {
  size_t pos = at(p, needle, nth);
  if (pos == std::string::npos) return kNone;
  const Occurrence* o = p.res.occurrence_at(pos);
  return o && o->position == pos ? o->symbol : kNone;
}

const Occurrence* occ(const Parsed& p, std::string_view needle, int nth) {
  size_t pos = at(p, needle, nth);
  const Occurrence* o = p.res.occurrence_at(pos);
  return o && o->position == pos ? o : nullptr;
}

void same(const Parsed& p, std::string_view name, int a, int b,
          const char* what) {
  size_t sa = sym(p, name, a), sb = sym(p, name, b);
  check(sa != kNone && sa == sb,
        std::format("{}: `{}` #{} and #{} are one symbol", what, name, a, b));
}

void differ(const Parsed& p, std::string_view name, int a, int b,
            const char* what) {
  size_t sa = sym(p, name, a), sb = sym(p, name, b);
  check(sa != kNone && sb != kNone && sa != sb,
        std::format("{}: `{}` #{} and #{} are different symbols", what, name,
                    a, b));
}

void unbound(const Parsed& p, std::string_view name, int a, const char* what) {
  check(sym(p, name, a) == kNone,
        std::format("{}: `{}` #{} resolves to nothing", what, name, a));
}

void test_scopes() {
  if (auto p = resolve_source("fn main() {\n  { x = 1 }\n  print(x)\n}\n"))
    unbound(*p, "x", 1, "a bare assignment in a block stays in the block");
  if (auto p = resolve_source("fn main() {\n  if true { y = 1 }\n  print(y)\n}\n"))
    same(*p, "y", 0, 1, "an if arm shares its scope");
  if (auto p = resolve_source("for item in [1] { print(item) }\nprint(item)\n")) {
    same(*p, "item", 0, 1, "a loop variable is visible in the body");
    unbound(*p, "item", 2, "a loop variable ends with the loop");
  }
  if (auto p = resolve_source("while mut k = 0; k < 1 { k += 1 }\nprint(k)\n")) {
    same(*p, "k", 0, 2, "a while init binding is visible in the body");
    unbound(*p, "k", 3, "a while init binding ends with the loop");
  }
  if (auto p = resolve_source("let v = match 3 { n => n }\n"))
    same(*p, "n", 0, 1, "a match pattern binds in its arm");
  if (auto p = resolve_source("try { 1 } catch err { err }\nerr\n")) {
    same(*p, "err", 0, 1, "a catch variable binds in the catch body");
    unbound(*p, "err", 2, "a catch variable ends with the catch body");
  }
}

void test_order() {
  if (auto p = resolve_source(
          "x = 1\nfn f() {\n  print(x)\n  let x = 2\n  print(x)\n}\n")) {
    same(*p, "x", 0, 1, "a read before a local declaration is the outer one");
    same(*p, "x", 2, 3, "a read after it is the local one");
    differ(*p, "x", 0, 2, "the local shadows the outer");
  }
  if (auto p = resolve_source("x = 1\nfn f() {\n  let x = x\n}\n")) {
    same(*p, "x", 0, 2, "a declaration's right-hand side reads the outer name");
    differ(*p, "x", 0, 1, "and declares a new one");
  }
  if (auto p = resolve_source("let a = 1\nlet a = 2\nprint(a)\n")) {
    same(*p, "a", 0, 1, "a repeated let in one scope is one variable");
    same(*p, "a", 1, 2, "read after the repeat");
  }
  if (auto p = resolve_source(
          "fn main() {\n  let f = fn () { late }\n  let late = 1\n}\n"))
    same(*p, "late", 0, 1, "a closure sees a later declaration");
  if (auto p = resolve_source(
          "fn main() {\n  fn inner() { w = 9 }\n  w = 1\n}\n")) {
    same(*p, "w", 0, 1, "a nested bare write reaches the outer variable");
    const Occurrence* o = occ(*p, "w", 0);
    check(o && o->role == Role::Write, "the nested bare write is a write");
  }
  if (auto p = resolve_source("class C {\n  m() { later }\n}\nlater = 4\n"))
    same(*p, "later", 0, 1, "a method sees a later top-level name");
  if (auto p = resolve_source("fn f(a, b = a) { b }\n"))
    same(*p, "a", 0, 1, "a default sees an earlier parameter");
  if (auto p = resolve_source("(pa, pb) = (1, 2)\nprint(pa)\n"))
    same(*p, "pa", 0, 1, "a place assignment declares a bare name");
}

void test_declarations() {
  if (auto p = resolve_source(
          "fn area(s: Long) { 1 }\nfn area(s: String) { 2 }\narea(1)\n")) {
    same(*p, "area", 0, 1, "overloads are one symbol");
    same(*p, "area", 1, 2, "a call reaches the overloads");
    size_t s = sym(*p, "area", 0);
    size_t decls = 0;
    if (s != kNone)
      for (size_t i : p->res.symbols[s].occurrences)
        decls += p->res.occurrences[i].role == Role::Declaration;
    check(decls == 2, "each overload is a declaration");
  }
  if (auto p = resolve_source("fn shared() { 1 }\nexport { shared }\n")) {
    size_t s = sym(*p, "shared", 0);
    check(s != kNone && p->res.symbols[s].exported, "export marks the symbol");
  }
  if (auto p = resolve_source(
          "effect fn ask()\nhandle { perform ask() } with ask(resume) { resume(1) }\n")) {
    same(*p, "ask", 0, 1, "perform names the effect operation");
    same(*p, "ask", 0, 2, "a handler clause names the effect operation");
    same(*p, "resume", 0, 1, "a clause parameter binds in its body");
  }
  if (auto p = resolve_source("import M from './m.cul'\nM.helper(1)\n")) {
    same(*p, "M", 0, 1, "an import is a name");
    check(p->res.imports.size() == 1 && p->res.imports[0].path == "./m.cul",
          "the import's path is recorded");
    check(p->res.module_members.size() == 1 &&
              p->res.module_members[0].member == "helper",
          "Alias.member on an import is recorded");
  }
}

void test_spellings() {
  if (auto p = resolve_source("let x = 1\nlet o = {x}\n")) {
    same(*p, "x", 0, 1, "an object shorthand reads the name");
    const Occurrence* o = occ(*p, "x", 1);
    check(o && o->spelling == Spelling::ObjectShorthand,
          "an object shorthand is spelled as one");
  }
  if (auto p = resolve_source("let {q} = {q: 1}\nprint(q)\n")) {
    same(*p, "q", 0, 2, "a pattern shorthand declares the name");
    const Occurrence* o = occ(*p, "q", 0);
    check(o && o->spelling == Spelling::PatternShorthand,
          "a pattern shorthand is spelled as one");
    check(occ(*p, "q", 1) == nullptr, "an object key is not a name");
  }
  if (auto p = resolve_source("fn f(count) { count }\nf(count: 3)\n")) {
    same(*p, "count", 0, 2, "a keyword label names the parameter");
    const Occurrence* o = occ(*p, "count", 2);
    check(o && o->spelling == Spelling::KeywordLabel,
          "a keyword label is spelled as one");
  }
  if (auto p = resolve_source("let o = {}\no.member\n"))
    check(occ(*p, "member", 0) == nullptr, "a member is not a name");
  if (auto p = resolve_source("fn size(v) { 0 }\n[1].size()\n"))
    check(p->res.called_as_method("size"), "a method call's name is recorded");
}

void test_outline() {
  auto p = resolve_source(
      "import M from './m.cul'\nfn f() { 1 }\nclass C {\n  new() { 1 }\n"
      "  area() { 1 }\n  w: Long = 0\n}\nenum E { A, B }\nlet v = 1\nv = 2\n");
  if (!p) return;
  auto items = outline(*p->ast, p->src, p->res);
  std::vector<std::string> names;
  for (const auto& it : items) names.push_back(it.name);
  check(names == std::vector<std::string>{"M", "f", "C", "E", "v"},
        "the outline lists the top-level declarations once each");
  if (items.size() == 5) {
    const auto& c = items[2].children;
    check(c.size() == 3 && c[0].kind == OutlineKind::Constructor &&
              c[1].kind == OutlineKind::Method && c[2].kind == OutlineKind::Field,
          "a class lists its constructor, method and field");
    check(items[3].children.size() == 2, "an enum lists its variants");
  }
}

// ---- corpus -----------------------------------------------------------------

std::string read_file(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// (line, byte column, name) of a byte offset.
std::tuple<long, long, std::string> locate(const std::string& src, size_t off,
                                           const std::string& name) {
  long line = 1;
  size_t start = 0;
  for (size_t i = 0; i < off && i < src.size(); i++)
    if (src[i] == '\n') {
      line++;
      start = i + 1;
    }
  return {line, static_cast<long>(off - start + 1), name};
}

void test_corpus(const std::filesystem::path& root) {
  size_t files = 0, reads = 0;
  for (const char* dir : {"tests", "examples"}) {
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(root / dir, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (!it->is_regular_file() || it->path().extension() != ".cul") continue;
      std::string src = read_file(it->path());
      // The lint analyses the lowered program; on the source as written an
      // effect operation is a name only the resolver knows.
      if (src.find("effect fn") != std::string::npos) continue;
      std::vector<culebra::ParseFailure> pf;
      auto ast = culebra::parse(it->path().string(), src, pf);
      if (!ast) continue;
      files++;
      auto res = resolve_module(*ast, src);

      std::set<std::tuple<long, long, std::string>> resolver;
      for (const auto& u : res.unresolved)
        resolver.insert(locate(src, u.position, u.name));
      std::vector<culebra::lint::Diagnostic> diags;
      culebra::lint::_detail::undefined::analyze_module(*ast, {}, diags);
      std::vector<size_t> line_starts{0};
      for (size_t i = 0; i < src.size(); i++)
        if (src[i] == '\n') line_starts.push_back(i + 1);
      for (const auto& d : diags) {
        std::string name = d.message.substr(d.message.find('\'') + 1);
        name.pop_back();
        // A read the parse synthesized (a regex literal's `Regex.compile`) has
        // no text in the source, so there is nothing an editor could point at.
        size_t off = d.line >= 1 && static_cast<size_t>(d.line) <= line_starts.size()
                         ? line_starts[d.line - 1] + static_cast<size_t>(d.col - 1)
                         : std::string::npos;
        if (off == std::string::npos || src.compare(off, name.size(), name) != 0)
          continue;
        reads++;
        if (!resolver.contains({d.line, d.col, name}))
          check(false, std::format("{}:{}:{}: the lint cannot bind `{}` but the "
                                   "resolver does",
                                   it->path().string(), d.line, d.col, name));
      }
    }
  }
  check(files > 400, std::format("the corpus was read ({} files)", files));
  std::printf("resolve_test: corpus %zu files, %zu unbound reads agree\n", files,
              reads);
}

}  // namespace

int main(int argc, char** argv) {
  test_scopes();
  test_order();
  test_declarations();
  test_spellings();
  test_outline();
  if (argc >= 2) test_corpus(argv[1]);

  if (failures) {
    std::printf("resolve_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("resolve_test OK\n");
  return 0;
}
