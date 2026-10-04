// Unit test for static name resolution (include/frontend/resolve.h).
//
// The cases pin the scoping rules the compiler implements — each one was
// checked against what `culebra` does when the program runs — and the three
// checks the load-time lint reads off a resolution (include/frontend/lint.h).
//
// Usage: resolve_test     (built and run by CTest)

#include <frontend/lint.h>
#include <frontend/resolve.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
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

std::unique_ptr<Parsed> resolve_source(std::string src, Options opts = {}) {
  auto p = std::make_unique<Parsed>();
  p->src = std::move(src);
  std::vector<culebra::ParseFailure> failures;
  p->ast = culebra::parse("(test)", p->src, failures);
  if (!p->ast) {
    check(false, "parses: " + p->src);
    return nullptr;
  }
  p->res = resolve_module(*p->ast, p->src, opts);
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
    unbound(*p, "y", 1, "a bare assignment in an if arm stays in the arm");
  if (auto p = resolve_source("if c { let a = 1; a } else { let a = 2; a }\n")) {
    same(*p, "a", 0, 1, "an arm's declaration is visible in the arm");
    differ(*p, "a", 0, 2, "each arm of an if is a scope of its own");
  }
  if (auto p = resolve_source(
          "cond { c => { let b = 1 }, _ => { let b = 2 } }\nc ? (zz = 1) : 0\nzz\n")) {
    differ(*p, "b", 0, 1, "each arm of a cond is a scope of its own");
    unbound(*p, "zz", 1, "a ternary arm is a scope of its own");
  }
  if (auto p = resolve_source(
          "if c { 0 } else if (let m = 1) > 0 { m }\nm\n"
          "cond { c => 0, (let kk = 1) > 0 => kk }\nkk\n")) {
    same(*p, "m", 0, 1, "a later test's declaration is visible in its arm");
    same(*p, "m", 0, 2, "a later test declares around the if");
    same(*p, "kk", 0, 2, "a cond test declares around the cond");
  }
  if (auto p = resolve_source("if let k = 1; k > 0 { k }\nprint(k)\n")) {
    same(*p, "k", 0, 2, "an if init binding is visible in the arms");
    unbound(*p, "k", 3, "an if init binding ends with the if");
  }
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
  if (auto p = resolve_source(
          "let lp = 1\nfn f(x = lp, y = || lp, z = || (|| lp)(), lp = 5) { lp }\n")) {
    same(*p, "lp", 0, 1, "a default reads the outer name of a later parameter");
    same(*p, "lp", 0, 2, "so does a closure in a default");
    same(*p, "lp", 0, 3, "and a closure nested in one");
    same(*p, "lp", 4, 5, "the body reads the parameter");
  }
  if (auto p = resolve_source(
          "let dv = 1\nfn f(x = (let dv = 2) + (|| dv)()) { dv }\n")) {
    same(*p, "dv", 1, 2, "a closure in a default reads its declaration");
    same(*p, "dv", 0, 3, "the body does not: the declaration is the default's");
  }
  if (auto p = resolve_source("fn f() {\n  let self = 1\n  self\n}\n"))
    same(*p, "self", 0, 1, "a `let self` is a variable");
  if (auto p = resolve_source("fn f() {\n  self.v\n}\n"))
    unbound(*p, "self", 0, "the receiver is no variable");
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
    check(p->res.called_as_method(sym(*p, "size", 0)),
          "a method call may reach the function of its name");
  if (auto p = resolve_source("fn f() {\n  let size = 1\n}\n[1].size()\n"))
    check(!p->res.called_as_method(sym(*p, "size", 0)),
          "a method call reaches only the name visible where it is written");
  if (auto p = resolve_source(
          "fn f() {\n  let size = fn (v) { 0 }\n  [1].size()\n}\n"))
    check(p->res.called_as_method(sym(*p, "size", 0)),
          "a method call in a function reaches the function's name");
  if (auto p = resolve_source("let k = 1\nlet o = {(k, 1): 2, k: 3}\n")) {
    same(*p, "k", 0, 1, "a computed object key reads the name");
    check(occ(*p, "k", 2) == nullptr, "a key spelled as a name is not a name");
  }
}

// A stdlib global, as the compiler's Options::globals knows it.
const std::set<std::string, std::less<>> kGlobals{"println", "range", "Math"};

void test_globals() {
  if (auto p = resolve_source("println = 1\nprintln(2)\n",
                              {.globals = &kGlobals})) {
    unbound(*p, "println", 0, "a bare write to a global is refused, not declared");
    unbound(*p, "println", 1, "and a read after it is the global");
  }
  if (auto p = resolve_source("println = 1\nprintln(2)\n"))
    same(*p, "println", 0, 1, "with no globals known, the bare write declares");
  if (auto p = resolve_source("[range] = [5]\n", {.globals = &kGlobals}))
    unbound(*p, "range", 0, "so does a bare destructure");
  if (auto p = resolve_source("let println = 0\nprintln = 1\n",
                              {.globals = &kGlobals}))
    same(*p, "println", 0, 1, "a declared name of a global's spelling is written");
  // A stdlib function value's method call reaches a free function by UFCS;
  // the globals do not tell it from a namespace's member call.
  if (auto p = resolve_source("fn twice(f, x) { f(f(x)) }\nprintln.twice(1)\n",
                              {.globals = &kGlobals}))
    check(p->res.called_as_method(sym(*p, "twice", 0)),
          "a method call on a global may reach a free function");
  if (auto p = resolve_source("let x = 1\n_lazy_ns_register('N', fn () { x })\n"))
    unbound(*p, "x", 1, "a lazy namespace builder sees nothing around it");
}

void test_session() {
  std::vector<std::string> earlier{"c"};
  if (auto p = resolve_source("fn bump() { c = c + 1 }\nlet c = 5\n",
                              {.session = earlier})) {
    size_t c = p->res.lookup(0, "c");
    check(c != kNone && p->res.symbols[c].declared_at == nullptr,
          "an earlier input's name is the module scope's");
    check(sym(*p, "c", 0) == c && sym(*p, "c", 1) == c,
          "a nested write and read reach the earlier input's variable");
    check(sym(*p, "c", 2) == c, "a top-level `let` declares that same variable");
  }
  if (auto p = resolve_source("fn bump() { c = 1 }\n")) {
    size_t c = sym(*p, "c", 0);
    check(c != kNone && p->res.symbols[c].scope != 0,
          "out of a session the nested write declares a local");
  }
}

void test_class_values() {
  if (auto p = resolve_source(
          "fn main() {\n  class S {\n    static s = v\n  }\n  let v = 1\n}\n"))
    unbound(*p, "v", 0, "a static value runs where the class is declared");
  if (auto p = resolve_source("fn main() {\n  let v = 0\n  class S {\n"
                              "    static s = (let v = 1) + v\n  }\n  v\n}\n")) {
    differ(*p, "v", 0, 1, "a static value's declaration is its own");
    same(*p, "v", 1, 2, "read within the value");
    same(*p, "v", 0, 3, "and ends with it");
    size_t v0 = sym(*p, "v", 0), v1 = sym(*p, "v", 1);
    check(v0 != kNone && v1 != kNone &&
              p->res.frame_of(v0) == p->res.frame_of(v1),
          "a static value is in the declaring function");
  }
  // The parse folds a one-statement body into the statement, the class here.
  if (auto p = resolve_source(
          "fn main() {\n  class S {\n    static s = (let v = 1) + (|| v)()\n  }\n}\n"))
    same(*p, "v", 0, 1, "a closure in a static value reads its declaration");
  if (auto p = resolve_source(
          "fn main() {\n  class K {\n    f = later\n  }\n  let later = 1\n}\n"))
    same(*p, "later", 0, 1, "an initializer runs per instance, after the body");
  if (auto p = resolve_source("class K {\n  f = (let u = 7) + u\n  g = u\n}\n")) {
    same(*p, "u", 0, 1, "an initializer reads its own declaration");
    unbound(*p, "u", 2, "which ends with the initializer");
  }
  if (auto p = resolve_source(
          "class K {\n  f = (let u = 7) + 0\n  g: Long = (let w = 1) + 0\n}\n")) {
    size_t u = sym(*p, "u", 0), w = sym(*p, "w", 0);
    check(u != kNone && w != kNone && p->res.frame_of(u) != 0 &&
              p->res.frame_of(u) == p->res.frame_of(w),
          "a class's initializers share one function scope");
  }
}

void test_decorators() {
  if (auto p = resolve_source(
          "let wrap = fn (f) { f }\n@wrap\nfn g() { 1 }\n@wrap\nclass C {}\n")) {
    same(*p, "wrap", 0, 1, "a function's decorator reads its callee");
    same(*p, "wrap", 0, 2, "a class's decorator reads its callee");
  }
  if (auto p = resolve_source(
          "fn mk(arg) { |f| f }\nfn main() {\n  let width = 2\n  @mk(width)\n  fn h() { 1 }\n}\n")) {
    same(*p, "mk", 0, 1, "a decorator call's callee is read");
    same(*p, "width", 0, 1, "its arguments are read in the declaring scope");
  }
  if (auto p = resolve_source("let value = 1\n@value\nclass V { x = 0 }\n"))
    check(occ(*p, "value", 1) == nullptr, "a compiler directive reads nothing");
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

// The name nodes spelled `name`, in tree order.
void collect_names(const peg::Ast& n, std::string_view name,
                   std::vector<const peg::Ast*>& out) {
  if (n.is_token && n.token == name) out.push_back(&n);
  for (const auto& c : n.nodes) collect_names(*c, name, out);
}

void test_node_records() {
  const std::string src =
      "let x = 1\n{ let x = 2; x }\nx\nfn f(k) { k }\nf(k: x)\nr = re'a+'\n";
  if (auto p = resolve_source(src))
    check(p->res.uses.empty() && p->res.declarations.empty() &&
              p->res.unbound_reads.empty() && p->res.body_scope.empty(),
          "nodes are recorded only on request");
  auto p = resolve_source(src, {.record_nodes = true});
  if (!p) return;
  const Resolution& res = p->res;
  auto symbol_of = [&](const peg::Ast* n) {
    auto it = res.uses.find(n);
    return it == res.uses.end() ? kNone - 1 : it->second.symbol;
  };

  std::vector<const peg::Ast*> xs;
  collect_names(*p->ast, "x", xs);
  check(xs.size() == 5, "five x nodes");
  if (xs.size() == 5) {
    size_t outer = symbol_of(xs[0]), inner = symbol_of(xs[1]);
    check(outer < res.symbols.size() && inner < res.symbols.size() &&
              outer != inner,
          "each declaration node records its own symbol");
    check(outer < res.symbols.size() && res.symbols[outer].declared_at == xs[0],
          "a symbol records the node that declares it");
    check(symbol_of(xs[2]) == inner, "a read in the block records the inner x");
    check(symbol_of(xs[3]) == outer && symbol_of(xs[4]) == outer,
          "reads after the block record the outer x");
    check(res.uses.at(xs[2]).scope != res.uses.at(xs[3]).scope,
          "a node records the scope it is written in");
  }

  // `f(k: x)`: the label names a parameter, not a variable read here.
  std::vector<const peg::Ast*> ks;
  collect_names(*p->ast, "k", ks);
  check(ks.size() == 3 && !res.uses.contains(ks.back()),
        "a keyword label is not recorded");

  // `fn f(k) { k }`: the body maps to a function scope bound to `f`.
  bool found = false;
  for (const auto& [body, scope] : res.body_scope) {
    const Scope& s = res.scopes[scope];
    found |= s.function && s.owner != kNone &&
             res.symbols[s.owner].name == "f";
  }
  check(found, "a function body maps to its scope and the symbol it binds");

  // `re'a+'` desugars to a call through a name the source never spells.
  bool synthesized = false;
  for (const auto& [node, use] : res.uses)
    synthesized |= name_offset(*node, node->token, p->src) == kNone &&
                   use.symbol == kNone;
  check(synthesized, "a synthesized name node is recorded");
}

// How each declaration is written, and the names nothing declares.
void test_declaration_forms() {
  const std::string src =
      "let a = 1\n"
      "mut b = 2\n"
      "let mut c = 3\n"
      "d = 4\n"
      "let (e, f) = (5, 6)\n"
      "mut [g] = [7]\n"
      "(h, a) = (8, 9)\n"
      "fn k(p, (q, r)) { for i in [p] { i } }\n"
      "try { 0 } catch err { err }\n"
      "match 1 { m => m }\n"
      "class C {}\n"
      "enum E { V }\n"
      "import M from 'm'\n"
      "a = 2\n"
      "b = 3\n"
      "let a = 4\n"
      "nowhere\n"
      "nothing += 1\n";
  auto p = resolve_source(src, {.record_nodes = true});
  if (!p) return;
  const Resolution& res = p->res;
  // Each name is one variable here, so its symbol's forms are the name's.
  std::map<std::string, size_t> symbols;
  for (const auto& d : res.declarations)
    symbols[std::string(d.node->token)] = d.symbol;
  auto is = [&](const char* name, std::vector<Form> want) {
    uint16_t mask = 0;
    for (Form f : want) mask |= static_cast<uint16_t>(1u << static_cast<int>(f));
    check(symbols.contains(name) && res.symbols[symbols[name]].forms == mask,
          std::format("the form(s) `{}` is declared in", name));
  };
  is("a", {Form::Let});
  is("b", {Form::Mut});
  is("c", {Form::Mut});
  is("d", {Form::Bare});
  is("e", {Form::Let});
  is("f", {Form::Let});
  is("g", {Form::Mut});
  is("h", {Form::Bare});
  is("k", {Form::Function});
  is("p", {Form::Parameter});
  is("q", {Form::Parameter});
  is("r", {Form::Parameter});
  is("i", {Form::Loop});
  is("err", {Form::Catch});
  is("m", {Form::Pattern});
  is("C", {Form::Class});
  is("E", {Form::Enum});
  is("M", {Form::Import});

  size_t a = res.lookup(0, "a"), b = res.lookup(0, "b");
  check(a != kNone && res.symbols[a].declared_as(Form::Let) &&
            !res.symbols[a].declared_as(Form::Mut) &&
            !res.symbols[a].declared_as(Form::Bare),
        "a symbol records every form that declares it, and no other");
  check(b != kNone && res.symbols[b].declared_as(Form::Mut),
        "a mutable symbol records `mut`");

  // `(h, a) = …` and `a = 2` write the `a` already there; the two `let`s
  // declare it.
  size_t a_declarations = 0;
  for (const auto& d : res.declarations) a_declarations += d.symbol == a;
  check(a != kNone && res.symbols[a].declarations == 2 && a_declarations == 2,
        "a bare write to a declared name declares nothing");

  // Declarations come in walk order: the module's, then the function's.
  std::vector<std::string> order;
  for (const auto& d : res.declarations) order.emplace_back(d.node->token);
  auto pos = [&](const char* n) {
    return std::find(order.begin(), order.end(), n) - order.begin();
  };
  check(pos("M") < pos("p") && pos("p") < pos("q") && pos("r") < pos("i"),
        "a function's declarations follow those of the body around it");

  check(res.unbound_reads.size() == 1 &&
            res.unbound_reads[0]->token == "nowhere",
        "a read nothing declares is listed; a write is not");
}


// ---- which constructs are scopes -------------------------------------------

// A `let v` written in each construct is on the function's own level or in
// a scope of the construct's. A scope that is no function's is recorded
// under the block that opens it (Resolution::block_scope), which is how a
// lowering finds the variables of a block it compiles.
void test_scope_levels() {
  struct Case {
    const char* what;
    const char* construct;
    bool own_level;
  };
  const std::vector<Case> cases = {
      {"a statement", "let v = 1", true},
      {"an if test", "if (let v = 1) > 0 { 0 }", true},
      {"an else-if test", "if false { 0 } else if (let v = 1) > 0 { 0 }", true},
      {"a cond test", "cond { (let v = 1) > 0 => 0, _ => 0 }", true},
      {"a ternary test", "(let v = 1) > 0 ? 1 : 0", true},
      {"an if arm", "if true { let v = 1 }", false},
      {"an if arm of several statements", "if true { 0; let v = 1 }", false},
      {"an else-if arm", "if false { 0 } else if true { let v = 1 }", false},
      {"an else arm", "if false { 0 } else { let v = 1 }", false},
      {"a cond arm", "cond { true => { let v = 1 }, _ => 0 }", false},
      {"a cond default arm", "cond { false => 0, _ => { let v = 1 } }", false},
      {"a ternary arm", "true ? (let v = 1) : 0", false},
      {"a ternary else arm", "false ? 0 : (let v = 1)", false},
      {"an arm of an if with an init clause",
       "if let c = 1; c > 0 { let v = 1 }", false},
      {"an operand", "true && (let v = 1)", true},
      {"a block", "{ let v = 1 }", false},
      {"a for body", "for i in [1] { let v = 1 }", false},
      {"a nobreak block", "for i in [] { 0 } nobreak { let v = 1 }", false},
      {"a while body", "while false { let v = 1 }", false},
      {"a match arm", "match 1 { _ => { let v = 1 } }", false},
      {"a try body", "try { let v = 1 } catch e { 0 }", false},
      {"a catch body", "try { 0 } catch e { let v = 1 }", false},
      {"a defer", "defer { let v = 1 }", false},
      {"a function literal", "g = fn () { let v = 1 }", false},
      {"a lambda", "g = || (let v = 1)", false},
      {"a named function", "fn g() { let v = 1 }", false},
      {"a default", "fn g(x = (let v = 1)) { x }", false},
      {"a method", "class K { m() { let v = 1 } }", false},
      {"an initializer", "class K { f = (let v = 1) }", false},
      {"a static value", "class K { static s = (let v = 1) }", false},
      {"a trait method", "trait T { m() { let v = 1 } }", false},
      {"a handled block", "handle { let v = 1 } with ask(resume) { 0 }", false},
      {"a handler clause", "handle { 0 } with ask(resume) { let v = 1 }", false},
      {"an if's init clause", "if let v = 1; v > 0 { 0 }", false},
      {"a while's init clause", "while let v = 1; v > 1 { 0 }", false},
      {"a match's init clause", "match let v = 1; v { _ => 0 }", false},
  };
  auto probe = [&](const Case& c, bool& own_level, bool& recorded) {
    auto p = resolve_source(
        std::format("effect fn ask()\nfn f() {{\n  0\n  {}\n  0\n}}\n", c.construct),
        {.record_nodes = true});
    if (!p) return false;
    const Resolution& res = p->res;
    const peg::Ast* body = nullptr;
    size_t scope = kNone;
    for (const auto& [b, s] : res.body_scope)
      if (res.scopes[s].owner != kNone &&
          res.symbols[res.scopes[s].owner].name == "f") {
        body = b;
        scope = s;
      }
    const Declaration* v = nullptr;
    for (const auto& d : res.declarations)
      if (d.node->token == "v") v = &d;
    if (!body || !v) {
      check(false, std::format("scope levels [{}]: no `v` in `f`", c.what));
      return false;
    }
    size_t held = res.symbols[v->symbol].scope;
    own_level = held == scope;
    recorded = res.scopes[held].function;
    for (const auto& [block, s] : res.block_scope) recorded |= s == held;
    return true;
  };
  for (const auto& c : cases) {
    bool own_level = false, recorded = false;
    if (!probe(c, own_level, recorded)) continue;
    check(own_level == c.own_level,
          std::format("scope levels [{}]: `v` is {} the function's own level",
                      c.what, own_level ? "on" : "off"));
    check(recorded,
          std::format("scope levels [{}]: the scope of `v` is no function's "
                      "and no recorded block's",
                      c.what));
  }
}

// ---- a body on its own -------------------------------------------------------

void test_body() {
  std::string src =
      "let a = p\n"         // the body's own `a`, from a parameter
      "{ let a = 2 }\n"     // a block's `a`
      "for i in [1] { let t = i }\n"
      "if let c = 1; c > 0 { let u = 1 } else { let u = 2 }\n"
      "out = a\n"           // a write to a name visible around the function
      "fresh = a\n";        // a name nothing around declares: the body's own
  std::vector<culebra::ParseFailure> failures;
  auto ast = culebra::parse("(test)", src, failures);
  if (!ast) return check(false, "body: parses");
  std::vector<std::string> around{"out", "a"};
  std::vector<std::string_view> params{"p"};
  auto res = resolve_body(*ast, params, {.record_nodes = true, .session = around});

  auto it = res.body_scope.find(ast.get());
  if (it == res.body_scope.end()) return check(false, "body: its scope is recorded");
  size_t body = it->second;
  check(res.scopes[body].function && res.scopes[body].parent == 0,
        "body: a function scope under the one around it");
  auto in = [&](size_t scope, std::string_view name) {
    auto f = res.scopes[scope].names.find(name);
    return f == res.scopes[scope].names.end() ? kNone : f->second;
  };
  size_t p = in(body, "p");
  check(p != kNone && res.symbols[p].declared_as(Form::Parameter),
        "body: a parameter known by name is the body's");
  size_t a = in(body, "a");
  check(a != kNone && a != in(0, "a"),
        "body: its `let` of a name visible around it is its own variable");
  check(in(body, "out") == kNone && res.symbols[in(0, "out")].declarations == 0,
        "body: a bare write to a name around it declares nothing");
  check(in(body, "fresh") != kNone,
        "body: a bare write to a name nothing declares is the body's");

  // Every block that is a scope is recorded under its node, and holds what
  // is declared on its level.
  std::map<std::string, size_t, std::less<>> holder;  // name -> block scope
  for (const auto& [node, scope] : res.block_scope)
    for (const auto& [name, symbol] : res.scopes[scope].names)
      holder[name] = scope;
  check(holder.contains("t") && holder["t"] == holder["i"],
        "body: a loop's variable and its body's are one recorded scope");
  check(holder.contains("c") && holder.contains("u") &&
            res.scopes[holder["u"]].parent == holder["c"],
        "body: an init clause's scope is recorded, around the arms'");
  size_t blocks_a = 0;
  for (const auto& [node, scope] : res.block_scope)
    blocks_a += res.scopes[scope].names.contains("a");
  check(blocks_a == 1, "body: the block's `a` is in the block's scope");
  for (const auto& [node, scope] : res.block_scope)
    check(res.function_of(scope) == body,
          "body: a block's scope is in the body's frame");
}

// ---- the load-time lint ------------------------------------------------------

// The `kind` errors the lint reports for `src`, as `name@line`, in the order
// it reports them.
std::vector<std::string> lint_errors(std::string src, std::string_view kind) {
  static const std::set<std::string, std::less<>> globals{"inspect"};
  auto p = resolve_source(std::move(src),
                          {.record_nodes = true, .globals = &globals});
  if (!p) return {};
  std::vector<culebra::lint::Diagnostic> diags;
  culebra::lint::_detail::LetRule(p->res, diags).run(*p->ast);
  culebra::lint::_detail::undefined_reads(p->res, globals, diags);
  culebra::lint::_detail::shadows(p->res, diags);
  std::vector<std::string> out;
  for (const auto& d : diags) {
    if (d.kind != kind) continue;
    auto open = d.message.find('\'');
    auto close = d.message.find('\'', open + 1);
    out.push_back(std::format("{}@{}",
                              d.message.substr(open + 1, close - open - 1),
                              d.line));
  }
  return out;
}

struct LintCase {
  const char* what;
  const char* src;
  std::vector<std::string> want;
};

void check_lint(std::string_view kind, const std::vector<LintCase>& cases) {
  for (const auto& c : cases) {
    auto got = lint_errors(c.src, kind);
    if (got == c.want) continue;
    std::string list;
    for (const auto& g : got) list += g + " ";
    check(false, std::format("{} [{}]: got {}", kind, c.what,
                             list.empty() ? "nothing" : list));
  }
}

// A read is undefined when no declaration is visible where it is written.
void test_lint_undefined() {
  check_lint("NameError", {
      {"nothing declares it", "zzz\n", {"zzz@1"}},
      {"in a function nothing calls", "fn f() {\n  zzz\n}\n", {"zzz@2"}},
      {"a global", "inspect(1)\n", {}},
      {"after its block", "{ let a = 1 }\na\n", {"a@2"}},
      {"a loop variable after the loop", "for i in [1] { }\ni\n", {"i@2"}},
      {"a loop body's name after the loop", "for i in [1] { let t = i }\nt\n",
       {"t@2"}},
      {"a catch variable after the catch",
       "fn f() {\n  try { throw 1 } catch e { 0 }\n  e\n}\n", {"e@3"}},
      {"a pattern's binding after the arm",
       "fn f(v) {\n  match v { q => 0 }\n  q\n}\n", {"q@3"}},
      {"an if arm's name after the if", "if true { x = 1 }\nx\n", {"x@2"}},
      {"an if arm's name in the other arm",
       "fn f(c) {\n  if c { let x = 1 } else { x }\n}\n", {"x@2"}},
      {"a cond arm's name after the cond",
       "cond { true => { x = 1 }, _ => 0 }\nx\n", {"x@2"}},
      {"a ternary arm's name after it", "true ? (x = 1) : 0\nx\n", {"x@2"}},
      {"above its declaration", "a\nlet a = 1\n", {"a@1"}},
      {"its own right-hand side", "let b = b\n", {"b@1"}},
      {"a decorator nothing declares", "@nope\nfn f() { 1 }\n", {"nope@1"}},
      // An operation meets its handler by name when it is performed.
      {"an operation nothing declares",
       "handle { perform op(zzz) } with op(x, k) { k(x) }\n", {"zzz@1"}},
      {"a handler clause's own names",
       "effect fn op(x)\nhandle { perform op(1) } with op(x, k) { k(x) }\nk\n",
       {"k@3"}},
      {"reported in source order", "fn f() {\n  late\n}\nearly\n",
       {"late@2", "early@4"}},
      // A declaration is visible but may not have run: the run's to decide.
      {"a closure called early", "let g = fn () { x }\ng()\nlet x = 1\n", {}},
      {"a skipped declaration", "true && (let q = 5)\nq\n", {}},
      {"a later test's name after the if",
       "if false { 0 } else if (let m = 1) > 0 { 0 }\nm\n", {}},
      {"a function declared below", "fn a() { b() }\nfn b() { 1 }\n", {}},
      // Not reads of a variable.
      {"a compound write", "y += 1\n", {}},
      {"the receiver and the sink", "class C { f() { self } }\nlet _ = 1\n", {}},
      {"a member, a label, a key",
       "let o = {k: 1}\nfn f(a) { a }\nf(a: o.k)\n", {}},
  });
}

// A declaration shadows when the name is, where its function is written, a
// variable of an enclosing function.
void test_lint_shadow() {
  check_lint("ShadowError", {
      {"a let", "fn o() {\n  let a = 1\n  fn i() {\n    let a = 2\n  }\n}\n",
       {"a@4"}},
      {"a parameter", "fn o() {\n  let a = 1\n  fn i(a) { a }\n}\n", {"a@3"}},
      {"a pattern's leaf",
       "fn o() {\n  let a = 1\n  fn i() {\n    let (a, b) = (1, 2)\n  }\n}\n",
       {"a@4"}},
      {"a match binding",
       "fn o() {\n  let a = 1\n  fn i(v) {\n    match v { a => a }\n  }\n}\n",
       {"a@4"}},
      {"a loop variable, a catch variable",
       "fn o() {\n  let a = 1\n  fn i() {\n    for a in [1] { }\n"
       "    try { 0 } catch a { 0 }\n  }\n}\n",
       {"a@4", "a@5"}},
      {"a function's and a class's name",
       "fn o() {\n  let a = 1\n  let b = 2\n  fn i() {\n    fn a() { 0 }\n"
       "    class b {}\n  }\n}\n",
       {"a@5", "b@6"}},
      {"in a default", "fn o() {\n  let a = 1\n  fn i(x = (let a = 2)) { x }\n}\n",
       {"a@3"}},
      {"in an initializer",
       "fn o() {\n  let a = 1\n  class K {\n    f = (let a = 2)\n  }\n}\n",
       {"a@4"}},
      {"in a defer", "fn f() {\n  let r = 1\n  defer { let r = 2 }\n}\n",
       {"r@3"}},
      {"a name declared below the function",
       "fn o() {\n  fn i() { let a = 2 }\n  let a = 1\n}\n", {"a@2"}},
      {"a loop variable, inside the loop",
       "fn f() {\n  for i in [1] {\n    fn g(i) { i }\n  }\n}\n", {"i@3"}},
      {"two functions deep",
       "fn o() {\n  let a = 1\n  fn m() {\n    fn i() { let a = 2 }\n  }\n}\n",
       {"a@4"}},
      {"reported in source order",
       "fn o() {\n  let x = 1\n  fn b() {\n    fn d() { let x = 2 }\n  }\n"
       "  fn c() { let x = 3 }\n}\n",
       {"x@4", "x@6"}},
      // Not visible where the function is written, so not captured.
      {"a closed block's name",
       "fn o() {\n  { let a = 1 }\n  fn i() { let a = 2 }\n}\n", {}},
      {"a loop variable, after the loop",
       "fn f() {\n  for i in [1] { }\n  fn g() { let i = 2 }\n}\n", {}},
      {"another arm's name",
       "fn o(v) {\n  match v {\n    1 => { let a = 1 },\n"
       "    _ => { fn i() { let a = 2 } },\n  }\n}\n",
       {}},
      {"another if arm's name",
       "fn o(c) {\n  if c { let a = 1 } else { fn i() { let a = 2 } }\n}\n",
       {}},
      {"a sibling function's name",
       "fn a() { let x = 1 }\nfn b() { let x = 2 }\n", {}},
      // The module's names are globals; a block in a function is its own.
      {"a top-level name", "let x = 1\nfn f() { let x = 2 }\n", {}},
      {"a top-level block's name", "{\n  let x = 1\n  fn f() { let x = 2 }\n}\n",
       {}},
      {"a block in the same function", "fn f() {\n  let a = 0\n  { let a = 1 }\n}\n",
       {}},
      // A bare write is the outer variable's.
      {"a bare write", "fn o() {\n  mut a = 1\n  fn i() { a = 2 }\n}\n", {}},
  });
}

// `x = v` after a `let x` of that variable.
// A state class a lowering synthesized holds, in its methods, a body written
// in the function around the class: what it declares there was held to the
// shadow rule as written, and is no declaration of the method's own.
void test_lint_lowered_class() {
  auto shadows_in = [](const char* cls) {
    std::string src = std::format(
        "fn g(z) {{\n  class {} {{\n    step() {{ let z = 9 }}\n  }}\n}}\n", cls);
    std::vector<culebra::ParseFailure> failures;
    auto ast = culebra::parse("<gen#0>", src, failures);
    if (!ast) return size_t{99};
    auto res = resolve_module(*ast, src, {.record_nodes = true});
    std::vector<culebra::lint::Diagnostic> diags;
    culebra::lint::_detail::shadows(res, diags);
    return diags.size();
  };
  check(shadows_in("K") == 1,
        "ShadowError [a method over the enclosing function's parameter]");
  check(shadows_in("_Gen_g_1_1") == 0,
        "ShadowError [a lowered state class's method holds the body's own]");
}

void test_lint_let() {
  check_lint("ImmutableError", {
      {"a let", "let a = 1\na = 2\n", {"a@2"}},
      {"from a closure", "let a = 1\nf = fn () {\n  a = 2\n}\n", {"a@3"}},
      {"a mut after the write", "let a = 1\na = 2\nmut a = 3\n", {"a@2"}},
      {"a name a default reads", "let n = 1\nfn f(a = n) {\n  n = 2\n}\n",
       {"n@3"}},
      {"under a bare pattern", "let a = 1\nfn g(p) {\n  [a, b] = p\n  a = 5\n}\n",
       {"a@4"}},
      {"a mut before the write", "let a = 1\nmut a = 2\na = 3\n", {}},
      {"a let mut", "let mut a = 1\na = 2\n", {}},
      {"a declaring pattern before the write",
       "let a = 1\nlet (a, b) = (2, 3)\na = 4\n", {}},
      {"a write above the let", "let poke = fn () { fixed = 1 }\nlet fixed = 0\n",
       {}},
      {"a block's let, then the outer name", "{ let a = 1 }\na = 2\n", {}},
      {"an if arm's let, then the outer name", "if true { let a = 1 }\na = 2\n",
       {}},
      {"in an if arm", "if true {\n  let a = 1\n  a = 2\n}\n", {"a@3"}},
      {"a let over a parameter", "f = fn (a) {\n  let a = 1\n  a = 2\n}\n", {}},
      {"a bare declaration", "a = 1\na = 2\n", {}},
      {"a compound write", "let a = 1\na += 1\n", {}},
  });
}

}  // namespace

int main() {
  test_body();
  test_scopes();
  test_order();
  test_declarations();
  test_spellings();
  test_globals();
  test_session();
  test_class_values();
  test_decorators();
  test_outline();
  test_node_records();
  test_declaration_forms();
  test_scope_levels();
  test_lint_lowered_class();
  test_lint_undefined();
  test_lint_shadow();
  test_lint_let();

  if (failures) {
    std::printf("resolve_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("resolve_test OK\n");
  return 0;
}
