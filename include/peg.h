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

#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <shared.h>  // CulebraError, nesting_too_deep_message

#if !defined(CULEBRA_RT_PEG_WEAK)
#include <peglib.h>
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

// A rule's reduction, handed to the caller's action when one is registered
// for `name` (semantic actions: the binding layer interprets a subject
// directly, without ever materializing a Tree). `values[i]` is whatever
// child i's own action produced (default: `values.empty() ? {} :
// values.front()`, matching cpp-peglib's own reduce default). `token` is
// this rule's own matched text, unlike a Node's — cpp-peglib gives every
// rule its full span (`SemanticValues::sv()`), branch or leaf, so it needs
// no `is_token` flag the way a Tree Node does.
struct Sv {
  std::string_view name;
  std::string_view token;
  size_t line = 1, column = 1, position = 0, length = 0, choice = 0;
  std::vector<std::any> values;
};

// The binding layer's action for one rule, and the map `parse_with_actions`
// dispatches through. `std::any` in and out: this header stays value-neutral
// (no JitValue here, so the AOT weak archive references no peg:: symbol and
// no JIT type either) — the binding layer's own `std::any` payload is a
// small copyable wrapper around a retained JitValue, opaque here. `Sv&`
// rather than `const Sv&`: `sv` is a fresh, single-use local `_peg_reduce`
// builds and hands to exactly one action call, so the binding layer moves
// each value out of `sv.values` instead of duplicating it — nothing else
// ever reads `sv` again. peglib's own reduce hook already threads a mutable
// `SemanticValues&` the same way, for the same reason.
using RuleAction = std::function<std::any(Sv&)>;
using ActionMap = std::unordered_map<std::string, RuleAction>;

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
CULEBRA_RT_PEG_LINKAGE Tree parse(Compiled&, std::string_view, bool,
                                  std::string_view) {
  _not_linked();
}
CULEBRA_RT_PEG_LINKAGE bool test(Compiled&, std::string_view) {
  _not_linked();
}
CULEBRA_RT_PEG_LINKAGE std::any parse_with_actions(Compiled&, std::string_view,
                                                   std::string_view,
                                                   const ActionMap&) {
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
  // The subject's name, for the duration of one parse() call only (set at its
  // top, read by the enter lambda below through the `c` it captures). Not a
  // property of the grammar, so it isn't threaded through compile()'s
  // Options -- cpp-peglib's own parse_n() takes a path per call for the same
  // reason: one parser, many files.
  std::string path;
  // One reentrant parse_with_actions() call's frame: which actions map is
  // active and where its subject text starts (SemanticValues::sv() is a view
  // into it, but carries no offset of its own -- only line/column -- so this
  // is how Sv::position gets computed).
  struct ActionFrame {
    const ActionMap* actions;
    const char* text_base;
  };
  // The stack, rather than a single frame, is what makes an action reachable
  // from *within* another action on the same Compiled (this grammar parsing
  // itself recursively, on a possibly different subject) resolve against its
  // own actions and its own text -- pushed/popped by every nested call, read
  // from the back by the universal reduce below. Only the 0<->1 transition
  // touches the grammar's rules (see ActionScope); the stack itself always
  // reflects exactly whichever call is innermost right now.
  std::vector<ActionFrame> action_stack;
};
using Handle = std::shared_ptr<Compiled>;

inline thread_local int64_t _peg_parse_depth = 0;

[[noreturn]] inline void _fail(const std::string& msg) {
  // No position: these are offsets into the grammar or the subject, not into
  // the culebra source, so they go in the text and the binding layer stamps
  // the call site (regex.h does the same).
  throw CulebraError("PegError", msg, 0, 0);
}

// A caller who named the subject (Peg.parse's `path`) gets a message that
// reads like any other compiler diagnostic; one who didn't gets the
// engine-prefixed form regex.h's own errors use.
inline std::string _fmt_err(std::string_view path, size_t ln, size_t col,
                            std::string_view msg) {
  return path.empty() ? culebra::format("Peg: {}:{}: {}", ln, col, msg)
                      : culebra::format("{}:{}:{}: {}", path, ln, col, msg);
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
  auto enter = [c](const ::peg::Context& ctx, const char* s, size_t, std::any&) {
    if (++_peg_parse_depth > kPegParseDepthLimit) {
      auto [ln, col] = ctx.line_info(s);
      _fail(_fmt_err(c->path, ln, col,
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
// position inside `text` in the message -- prefixed by `path` when the caller
// named the subject (Peg.parse(..., path: "prog.pas")), the way cpp-peglib's
// own parse_n() takes a path per call: one parser, many files.
CULEBRA_RT_PEG_LINKAGE Tree parse(Compiled& c, std::string_view text,
                                  bool optimize, std::string_view path) {
  c.err.clear();
  c.err_line = c.err_col = 0;
  c.path.assign(path);
  _peg_parse_depth = 0;
  std::shared_ptr<::peg::Ast> ast;
  if (!c.parser.parse(text, ast)) {
    _fail(_fmt_err(c.path, c.err_line, c.err_col,
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
  c.path.clear();
  _peg_parse_depth = 0;
  return c.parser.parse(text);
}

// The rule this reduction belongs to, whatever action fires: default when
// `actions` has no entry for it, matching cpp-peglib's own reduce default
// (`vs.empty() ? any() : vs.front()`) with `{}` standing in for `any()` --
// this header's `std::any` payload has no "empty" the binding layer can read
// back, so nil (the binding layer's own empty payload) is what an unset leaf
// rule with no children resolves to.
// peglib's own `parser::parse_n<T>` extracts the start rule's reduction via
// `std::any_cast<T>`, which needs T to be the literal type every reduction
// actually holds -- and that can't be the binding layer's own payload type,
// since a leaf rule with no children and no action reduces to a bare,
// contentless `std::any()`, which no `any_cast<T>` (for any real T) can ever
// read back. Boxing every reduction in this uniform, always-the-same-type
// wrapper is what lets `parse_n<_AnyBox>` read the root's value regardless of
// what it holds (see parse_with_actions's own retrieval): `_AnyBox` is always
// the outer type, and its own `v` may itself be empty.
struct _AnyBox {
  std::any v;
};

// Moves the wrapped value out of `boxed` (a child's own `_AnyBox`-in-any
// reduction) rather than copying it -- `vs` is a fresh SemanticValues no one
// reads again after this reduce call, so every one of its slots is read at
// most once. That, not just style, is what keeps the binding layer's own
// payload wrapper (JitAny, stdlib_jit.h) off the hook for a retain it would
// otherwise need on every level a value passes through on its way up.
inline std::any _peg_unbox(std::any& boxed) {
  if (!boxed.has_value()) return std::any();
  return std::move(std::any_cast<_AnyBox&>(boxed).v);
}

inline std::any _peg_reduce(Compiled& c, ::peg::SemanticValues& vs) {
  const auto& frame = c.action_stack.back();
  if (auto it = frame.actions->find(vs.name()); it != frame.actions->end()) {
    Sv sv;
    sv.name = vs.name();
    sv.token = vs.sv();
    std::tie(sv.line, sv.column) = vs.line_info();
    sv.position = static_cast<size_t>(sv.token.data() - frame.text_base);
    sv.length = sv.token.size();
    sv.choice = vs.choice();
    sv.values.reserve(vs.size());
    for (size_t i = 0; i < vs.size(); i++) sv.values.push_back(_peg_unbox(vs[i]));
    return std::any(_AnyBox{it->second(sv)});
  }
  return std::any(_AnyBox{vs.empty() ? std::any() : _peg_unbox(vs.front())});
}

// Swaps every rule's action for `actions`'s duration (installing the reduce
// above once, on the 0->1 transition of the stack this Compiled already
// tracks -- see its member comment for why a stack and not a flag), and
// restores compile()'s tree-mode actions on the way back out, on every exit
// path including a thrown one.
struct ActionScope {
  Compiled& c;
  ActionScope(Compiled& compiled, const ActionMap& actions, const char* text_base)
      : c(compiled) {
    c.action_stack.push_back({&actions, text_base});
    if (c.action_stack.size() == 1) {
      for (const auto& [name, def] : c.parser.get_grammar())
        c.parser[name.c_str()].action =
            [&compiled](::peg::SemanticValues& vs) {
              return _peg_reduce(compiled, vs);
            };
    }
  }
  ~ActionScope() {
    c.action_stack.pop_back();
    if (c.action_stack.empty()) {
      // Action has no copy constructor (only copy assignment), so there is
      // no saved value to reinstate here -- clearing every rule back to "no
      // action" and re-running enable_ast() reaches the identical state
      // compile() left them in, since enable_ast() only touches actionless
      // rules.
      for (const auto& [name, def] : c.parser.get_grammar())
        c.parser[name.c_str()].action = ::peg::Action();
      c.parser.enable_ast();
    }
  }
};

// Parse `text` against `actions` (rule name -> a culebra closure wrapped as
// `std::any(const Sv&)`), interpreting it directly rather than building a
// Tree: the root rule's own reduction (its registered action, or the
// default) is the whole result. Throws CulebraError("PegError") on a syntax
// error or an `actions` key naming no rule in the grammar, with `path`
// honored the same way plain parse() honors it. Whatever exception a
// registered action itself throws (a culebra throw, a native TypeError, …)
// propagates unchanged -- cpp-peglib installs no catch around a rule
// reduction, so nothing here needs to either.
CULEBRA_RT_PEG_LINKAGE std::any parse_with_actions(Compiled& c,
                                                   std::string_view text,
                                                   std::string_view path,
                                                   const ActionMap& actions) {
  for (const auto& [name, fn] : actions) {
    if (!c.parser.get_grammar().count(name)) {
      _fail(culebra::format("Peg: no such rule '{}'", name));
    }
  }
  // A registered action recursively parsing the same Compiled (this grammar
  // applied to itself, or just to a substring it extracted) reuses this same
  // err/err_line/err_col/path state, and the depth guard's counter is a
  // single thread_local shared by every parse. Restoring all of it to what
  // THIS call found on entry, on every exit including a thrown one, is what
  // keeps a reentrant call's own bookkeeping from leaking into -- or, for the
  // depth counter, silently discounting -- the resuming outer call's.
  struct Restore {
    Compiled& c;
    std::string err;
    size_t line, col;
    std::string path;
    int64_t depth;
    ~Restore() {
      c.err = std::move(err);
      c.err_line = line;
      c.err_col = col;
      c.path = std::move(path);
      _peg_parse_depth = depth;
    }
  } restore{c, c.err, c.err_line, c.err_col, c.path, _peg_parse_depth};

  c.err.clear();
  c.err_line = c.err_col = 0;
  c.path.assign(path);
  _peg_parse_depth = 0;
  _AnyBox result;
  {
    ActionScope scope(c, actions, text.data());
    if (!c.parser.parse_n(text.data(), text.size(), result)) {
      _fail(_fmt_err(c.path, c.err_line, c.err_col,
                    c.err.empty() ? "syntax error" : c.err));
    }
  }
  return result.v;
}

#endif  // CULEBRA_RT_PEG_WEAK

}  // namespace culebra::pegparser
