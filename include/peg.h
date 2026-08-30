#pragma once

// Value-neutral PEG core for the `Peg` namespace (the `_Peg` native rows in
// stdlib_jit.h). Mirrors regex.h / http.h: no Value / JitValue here — the
// binding layer turns the flat node table below into its own Objects.
//
// culebra's own front end already runs on cpp-peglib (parser.h), so handing a
// parser to programs adds no dependency. What it does add is a grammar that is
// user data, so both guards the front end applies to its own grammar apply
// here: the per-rule recursion counter (machine-written nesting otherwise
// overflows the C stack inside peglib — an uncatchable SIGSEGV instead of an
// error) and the value-nesting bound the rest of the stdlib applies to trees
// it hands back (json.h).
//
// Linkage partitioning, the same choke regex.h applies: the AOT core archive
// must not name a peglib symbol. The namespace-group mechanism alone does not
// suffice here — an unused namespace's *functions* are collected, but peglib's
// Ope class hierarchy leaves vtables and typeinfo behind, which `--gc-sections`
// keeps (measured: 71 peglib symbols in a `print('hello')` binary before this
// was split out, 0 after).
//   - core archive  (CULEBRA_RT_PEG_WEAK):   weak throwing stubs; peglib.h is
//     never even included, so the archive references no peg:: symbol.
//   - peg archive   (CULEBRA_RT_PEG_STRONG): strong real bodies, force-loaded
//     only when the AST scan reports Peg use.
//   - header-only / in-process JIT (neither): the normal inline body.
//
// The namespace is `pegparser`, not `peg`: a `culebra::peg` would shadow
// peglib's own `peg::` at every `peg::Ast` spelled inside namespace culebra
// (lint.h, vm.h, test_engine.h, …).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <shared.h>  // CulebraError, nesting_too_deep_message

#if !defined(CULEBRA_RT_PEG_WEAK)
#include <peglib.h>
#include <unordered_map>
#endif

#if defined(CULEBRA_RT_PEG_STRONG)
#define CULEBRA_RT_PEG_LINKAGE
#elif defined(CULEBRA_RT_PEG_WEAK)
#define CULEBRA_RT_PEG_LINKAGE __attribute__((weak))
#else
#define CULEBRA_RT_PEG_LINKAGE inline
#endif

namespace culebra::pegparser {

// The value-nesting bound (language.md), applied to the tree we hand back the
// same way json.h applies it to a parsed document.
inline constexpr int64_t kPegTreeDepthLimit = kCulebraRecursionLimit;

// Rule entries, not tree levels — the unit parser.h counts for culebra's own
// grammar, at the same limit and for the same reason.
inline constexpr int64_t kPegParseDepthLimit = 4000;

// What identifies a loaded grammar — and so what the compile cache keys on.
// AST optimization is not here: it is applied to a finished tree, per parse.
struct Options {
  std::string start;    // start rule; empty = the grammar's first definition
  // On by default, and the default is load-bearing: without memoization,
  // alternatives that share a prefix backtrack exponentially. Measured on
  // cpp-peglib's own README calculator — 10 levels of nesting is 0.04 ms
  // memoized and 2.0 s not. The cost is a table proportional to input x
  // rules (~1 KB per input byte), which is why it can still be turned off.
  bool packrat = true;
};

// One AST node, flattened. A whole parse is two vectors, and `Tree` holds the
// nodes in prefix order — so the binding layer builds its objects by walking
// the table backwards, every child finished before its parent, with no
// recursion of its own.
struct Node {
  std::string_view name;   // rule name, or the grammar's `ast_name` override
  std::string_view token;  // a token node's matched text; empty otherwise
  size_t line = 1, column = 1;
  size_t position = 0, length = 0;
  size_t choice = 0;  // which alternative of a prioritized choice matched
  bool is_token = false;
  // [child_begin, child_begin + child_count) into Tree::children. Prefix order
  // does not keep a node's children adjacent, so they are addressed through
  // that index list rather than as a range of nodes.
  size_t child_begin = 0;
  size_t child_count = 0;
};

struct Tree {
  std::vector<Node> nodes;         // nodes[0] is the root
  std::vector<uint32_t> children;  // child index lists, addressed by Node
  // Keeps the peglib AST alive: Node::name views into it (Node::token views
  // into the subject, which the caller owns). Opaque, so a caller of this
  // header never names a peglib type.
  std::shared_ptr<void> owner;
};

#if defined(CULEBRA_RT_PEG_WEAK)

// Named, never defined, by the stubs below — the weak archive holds no peglib
// type at all.
struct Compiled;
using Handle = std::shared_ptr<Compiled>;

[[noreturn]] inline void _not_linked() {
  throw CulebraError("InternalError",
                     "Peg runtime entered in a no-Peg binary", 0, 0);
}
CULEBRA_RT_PEG_LINKAGE Handle compile(std::string_view, const Options&) {
  _not_linked();
}
CULEBRA_RT_PEG_LINKAGE Tree parse(Compiled&, std::string_view, bool) {
  _not_linked();
}
CULEBRA_RT_PEG_LINKAGE bool test(Compiled&, std::string_view) {
  _not_linked();
}

#else  // the real bodies

// A loaded grammar, owned by the per-thread compile cache and handed out
// shared so a caller's handle outlives a cache eviction. `err` is where the
// parser's logger leaves the last diagnostic: a parser is only ever reached
// from the thread that cached it, so one slot per grammar is enough.
struct Compiled {
  ::peg::parser parser;
  std::string err;
  size_t err_line = 0, err_col = 0;
};
using Handle = std::shared_ptr<Compiled>;

inline thread_local int64_t _peg_parse_depth = 0;

[[noreturn]] inline void _fail(const std::string& msg) {
  // No position: these are offsets into the grammar or the subject, not into
  // the culebra source, so they go in the text and the binding layer stamps
  // the call site (regex.h does the same).
  throw CulebraError("PegError", msg, 0, 0);
}

// Load (or cache-hit) `grammar`. Throws CulebraError("PegError") for a
// malformed grammar, with the position inside the grammar text in the message.
CULEBRA_RT_PEG_LINKAGE Handle compile(std::string_view grammar,
                                      const Options& opt) {
  static thread_local std::unordered_map<std::string, Handle> cache;
  std::string key;
  key.reserve(grammar.size() + opt.start.size() + 2);
  key += opt.packrat ? '1' : '0';
  key += opt.start;
  key += '\n';  // no rule name contains one, so the split is unambiguous
  key += grammar;
  if (auto it = cache.find(key); it != cache.end()) return it->second;

  auto h = std::make_shared<Compiled>();
  // Raw `this`: the parser is a member, so the callback cannot outlive it.
  auto* c = h.get();
  h->parser.set_logger([c](size_t ln, size_t col, const std::string& msg) {
    if (c->err.empty()) {
      c->err = msg;
      c->err_line = ln;
      c->err_col = col;
    }
  });
  if (!h->parser.load_grammar(grammar, opt.start)) {
    _fail(culebra::format("Peg: grammar:{}:{}: {}", c->err_line, c->err_col,
                          c->err.empty() ? "invalid grammar" : c->err));
  }
  h->parser.enable_ast();
  if (opt.packrat) h->parser.enable_packrat_parsing();

  // Every named rule counts, for the reason parser.h gives: deep nesting
  // descends through whichever recursive rule family matches first, so
  // hooking a hand-picked subset is a losing game. peglib runs `leave` from a
  // scope_exit, so backtracking keeps the count balanced.
  auto enter = [](const ::peg::Context& ctx, const char* s, size_t, std::any&) {
    if (++_peg_parse_depth > kPegParseDepthLimit) {
      auto [ln, col] = ctx.line_info(s);
      _fail(culebra::format("Peg: {}:{}: {}", ln, col,
                            nesting_too_deep_message(kPegParseDepthLimit)));
    }
  };
  auto leave = [](const ::peg::Context&, const char*, size_t, size_t, std::any&,
                  std::any&) { --_peg_parse_depth; };
  for (const auto& [rule_name, def] : h->parser.get_grammar()) {
    h->parser[rule_name.c_str()].enter = enter;
    h->parser[rule_name.c_str()].leave = leave;
  }

  if (cache.size() > 64) cache.clear();  // bound growth (a grammar is big)
  cache.emplace(std::move(key), h);
  return h;
}

inline void _flatten(const ::peg::Ast& a, Tree& t, int64_t depth) {
  if (depth >= kPegTreeDepthLimit) {
    throw CulebraError(
        "ValueError",
        culebra::format("Peg.parse: {}",
                        nesting_too_deep_message(kPegTreeDepthLimit)),
        0, 0);
  }
  size_t self = t.nodes.size();
  t.nodes.push_back(Node{a.name, a.is_token ? a.token : std::string_view{},
                         a.line, a.column, a.position, a.length, a.choice,
                         a.is_token, 0, 0});
  std::vector<uint32_t> kids;
  kids.reserve(a.nodes.size());
  for (const auto& child : a.nodes) {
    kids.push_back(static_cast<uint32_t>(t.nodes.size()));
    _flatten(*child, t, depth + 1);
  }
  t.nodes[self].child_begin = t.children.size();
  t.nodes[self].child_count = kids.size();
  t.children.insert(t.children.end(), kids.begin(), kids.end());
}

// Parse `text`. Throws CulebraError("PegError") on a syntax error, with the
// position inside `text` in the message.
CULEBRA_RT_PEG_LINKAGE Tree parse(Compiled& c, std::string_view text,
                                  bool optimize) {
  c.err.clear();
  c.err_line = c.err_col = 0;
  _peg_parse_depth = 0;
  std::shared_ptr<::peg::Ast> ast;
  if (!c.parser.parse(text, ast)) {
    _fail(culebra::format("Peg: {}:{}: {}", c.err_line, c.err_col,
                          c.err.empty() ? "syntax error" : c.err));
  }
  if (optimize) ast = c.parser.optimize_ast(ast);
  Tree t;
  t.nodes.reserve(64);
  _flatten(*ast, t, 0);
  t.owner = ast;
  return t;
}

CULEBRA_RT_PEG_LINKAGE bool test(Compiled& c, std::string_view text) {
  c.err.clear();
  c.err_line = c.err_col = 0;
  _peg_parse_depth = 0;
  return c.parser.parse(text);
}

#endif  // CULEBRA_RT_PEG_WEAK

}  // namespace culebra::pegparser
