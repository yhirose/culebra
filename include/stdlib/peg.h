#pragma once

// Value-neutral PEG core for the `PEG` namespace (the `_PEG` native rows in
// stdlib_rt.h). Mirrors regex.h / http.h: no Value / JitValue here — the
// binding layer turns the flat node table below into its own Objects.
//
// culebra's own front end already runs on cpp-peglib (parser.h), so handing a
// parser to programs adds no dependency. What it does add is a grammar that is
// user data, so both guards the front end applies to its own grammar apply
// here: the bound on rules in progress (machine-written nesting otherwise
// overflows the C stack inside peglib — an uncatchable SIGSEGV instead of an
// error) and the value-nesting bound the rest of the stdlib applies to trees
// it hands back (json.h).
//
// Unlike regex.h, this header carries no linkage split: it is always compiled
// into the core archive, whether or not a program ever names `PEG` (see
// docs/deployment.md §1 for the size trade-off and why Regex/Tensor can't
// make the same call). The namespace-group mechanism (stdlib_rt.h's dispatch
// rows) does dead-strip the actual parsing entry points --
// culebra::pegparser::compile() itself is absent from a binary that never
// names PEG, verified with `nm -C --defined-only` on a `--keep-symbols` build
// (plain `nm` on a stripped binary proves nothing either way). What survives
// regardless is inert, not reachable code: peglib's own `Ope` class hierarchy
// leaves vtables and typeinfo behind that `--gc-sections` keeps no matter
// what (shared with culebra's own front end, parser.h, which needs a working
// peg::parser regardless of this header), plus a handful of
// `std::function`-wrapped local lambdas in this file and stdlib_rt.h (the
// grammar's error logger, the JIT's action-map builder) whose
// `_Function_handler<...>::_M_manager` template instantiation outlives its
// only caller -- a known comdat/--gc-sections gap where a template
// instantiation can survive after everything that would have called it is
// pruned.
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

#include <base/shared.h>  // CulebraError, the nesting-bound wording

#include <peglib.h>

namespace culebra::pegparser {

// The value-nesting bound (language.md), applied to the tree we hand back the
// same way json.h applies it to a parsed document.
inline constexpr int64_t kPEGTreeDepthLimit = kCulebraRecursionLimit;

// Rule matches in progress, not tree levels — the unit parser.h bounds for
// culebra's own grammar, at the same limit and for the same reason. peglib
// also fails a parse whose tree is deeper than this (a left-recursive chain),
// before optimize_ast walks it recursively; a shallower tree past the bound
// above is _flatten's ValueError.
inline constexpr int64_t kPEGParseDepthLimit = 4000;

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
// (no JitValue here, so it names no JIT type) — the binding layer's own
// `std::any` payload is a small copyable wrapper around a retained JitValue,
// opaque here. `Sv&`
// rather than `const Sv&`: `sv` is a fresh, single-use local `_peg_reduce`
// builds and hands to exactly one action call, so the binding layer moves
// each value out of `sv.values` instead of duplicating it — nothing else
// ever reads `sv` again. peglib's own reduce hook already threads a mutable
// `SemanticValues&` the same way, for the same reason.
using RuleAction = std::function<std::any(Sv&)>;
using ActionMap = std::unordered_map<std::string, RuleAction>;

// A loaded grammar, owned by the per-thread compile cache and handed out
// shared so a caller's handle outlives a cache eviction. `err` is where the
// parser's logger leaves the last diagnostic: a parser is only ever reached
// from the thread that cached it, so one slot per grammar is enough.
struct Compiled {
  ::peg::parser parser;
  std::string err;
  size_t err_line = 0, err_col = 0;
  // The subject's name, for the duration of one parse() call only (set at its
  // top, read when it reports a failure). Not a property of the grammar, so
  // it isn't threaded through compile()'s Options -- cpp-peglib's own
  // parse_n() takes a path per call for the same reason: one parser, many
  // files.
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
  // What compile() built this from, and a second parser built from the same:
  // a tree-mode parse a registered action starts goes there, since the rules
  // here carry the reduce the outer parse installed -- swapping them back
  // would destroy the action that is running.
  std::string grammar;
  Options opt;
  std::shared_ptr<Compiled> tree_twin;
};
using Handle = std::shared_ptr<Compiled>;

[[noreturn]] inline void _fail(const std::string& msg) {
  // No position: these are offsets into the grammar or the subject, not into
  // the culebra source, so they go in the text and the binding layer stamps
  // the call site (regex.h does the same).
  throw CulebraError("PEGError", msg, 0, 0);
}

// A caller who named the subject (PEG.parse's `path`) gets a message that
// reads like any other compiler diagnostic; one who didn't gets the
// engine-prefixed form regex.h's own errors use.
inline std::string _fmt_err(std::string_view path, size_t ln, size_t col,
                            std::string_view msg) {
  return path.empty() ? culebra::format("PEG: {}:{}: {}", ln, col, msg)
                      : culebra::format("{}:{}:{}: {}", path, ln, col, msg);
}

// A failed parse of the subject, as a PEGError.
[[noreturn]] inline void _fail_parse(const Compiled& c) {
  _fail(_fmt_err(c.path, c.err_line, c.err_col,
                 c.err.empty() ? "syntax error" : c.err));
}

// The error peglib's set_max_depth bound reports, as the logger rewords it.
inline bool _is_depth_error(const std::string& msg) {
  return msg == nesting_too_deep_message(kPEGParseDepthLimit);
}

// Load (or cache-hit) `grammar`. Throws CulebraError("PEGError") for a
// malformed grammar, with the position inside the grammar text in the message.
inline Handle _build(std::string_view grammar, const Options& opt) {
  auto h = std::make_shared<Compiled>();
  h->grammar.assign(grammar);
  h->opt = opt;
  // Raw `this`: the parser is a member, so the callback cannot outlive it.
  auto* c = h.get();
  h->parser.set_logger([c](size_t ln, size_t col, const std::string& msg) {
    auto m = reword_parse_depth_error(msg, kPEGParseDepthLimit);
    // The depth bound ends the parse, so it outranks an error recovered
    // (`%recover`) before it.
    if (c->err.empty() || _is_depth_error(m)) {
      c->err = std::move(m);
      c->err_line = ln;
      c->err_col = col;
    }
  });
  if (!h->parser.load_grammar(grammar, opt.start)) {
    _fail(culebra::format("PEG: grammar:{}:{}: {}", c->err_line, c->err_col,
                          c->err.empty() ? "invalid grammar" : c->err));
  }
  h->parser.enable_ast();
  if (opt.packrat) h->parser.enable_packrat_parsing();
  // Reported through the logger above, as a failed parse.
  h->parser.set_max_depth(kPEGParseDepthLimit);
  return h;
}

inline Handle compile(std::string_view grammar,
                      const Options& opt) {
  static thread_local std::unordered_map<std::string, Handle> cache;
  std::string key;
  key.reserve(grammar.size() + opt.start.size() + 2);
  key += opt.packrat ? '1' : '0';
  key += opt.start;
  key += '\n';  // no rule name contains one, so the split is unambiguous
  key += grammar;
  if (auto it = cache.find(key); it != cache.end()) return it->second;
  auto h = _build(grammar, opt);
  if (cache.size() > 64) cache.clear();  // bound growth (a grammar is big)
  cache.emplace(std::move(key), h);
  return h;
}

// The parser a tree-mode parse of `c` runs on: `c` itself, or its twin while
// a parse_with_actions on `c` is in flight.
inline Compiled& _tree_mode(Compiled& c) {
  if (c.action_stack.empty()) return c;
  if (!c.tree_twin) c.tree_twin = _build(c.grammar, c.opt);
  return *c.tree_twin;
}

inline void _flatten(const ::peg::Ast& a, Tree& t, int64_t depth) {
  if (depth >= kPEGTreeDepthLimit) {
    throw CulebraError(
        "ValueError",
        culebra::format("PEG.parse: {}",
                        nesting_too_deep_message(kPEGTreeDepthLimit)),
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

// A registered action parsing again (this grammar applied to itself, or just
// to a substring it extracted) reuses this Compiled's err/err_line/err_col/
// path. Each parse starts them fresh and puts back what it found on every
// exit, a thrown one included, so a nested call's bookkeeping does not leak
// into the resuming outer one.
struct _ParseState {
  Compiled& c;
  std::string err;
  size_t line, col;
  std::string path;
  _ParseState(Compiled& compiled, std::string_view subject)
      : c(compiled), err(std::move(compiled.err)), line(compiled.err_line),
        col(compiled.err_col), path(std::move(compiled.path)) {
    c.err.clear();
    c.err_line = c.err_col = 0;
    c.path.assign(subject);
  }
  ~_ParseState() {
    c.err = std::move(err);
    c.err_line = line;
    c.err_col = col;
    c.path = std::move(path);
  }
};

// Parse `text`. Throws CulebraError("PEGError") on a syntax error, with the
// position inside `text` in the message -- prefixed by `path` when the caller
// named the subject (PEG.parse(..., path: "prog.pas")), the way cpp-peglib's
// own parse_n() takes a path per call: one parser, many files.
inline Tree parse(Compiled& c, std::string_view text,
                  bool optimize, std::string_view path) {
  std::shared_ptr<::peg::Ast> ast;
  {
    Compiled& t = _tree_mode(c);
    _ParseState state(t, path);
    if (!t.parser.parse(text, ast)) _fail_parse(t);
    if (optimize) ast = t.parser.optimize_ast(ast);
  }
  Tree t;
  t.nodes.reserve(64);
  _flatten(*ast, t, 0);
  t.owner = ast;
  return t;
}

inline bool test(Compiled& c, std::string_view text) {
  Compiled& t = _tree_mode(c);
  _ParseState state(t, {});
  if (t.parser.parse(text)) return true;
  // Too deep a subject is an error, as in parse(), not a mismatch.
  if (_is_depth_error(t.err)) _fail_parse(t);
  return false;
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
// payload wrapper (JitAny, stdlib_rt.h) off the hook for a retain it would
// otherwise need on every level a value passes through on its way up.
inline std::any _peg_unbox(std::any& boxed) {
  if (!boxed.has_value()) return std::any();
  return std::move(std::any_cast<_AnyBox&>(boxed).v);
}

// `rule_name` identifies which rule this reduction is for -- NOT
// `vs.name()`. For an ordinary rule those agree, but a `{ precedence }`
// rule's own PrecedenceClimbing operator steals the installed action and
// re-invokes it once per binary step with a synthesized SemanticValues whose
// `name()` is stale (the last Reference it matched, e.g. "ATOM") or empty,
// never the rule that owns it (measured: for `EXPRESSION <- ATOM (OPERATOR
// ATOM)* { precedence ... }`, vs.name() during those calls is "ATOM" then
// ""). Passing the name down from the closure that installed this action
// -- captured once per rule at install time, below -- is what still routes
// correctly for a precedence rule's registered action.
inline std::any _peg_reduce(Compiled& c, const std::string& rule_name,
                            ::peg::SemanticValues& vs) {
  const auto& frame = c.action_stack.back();
  if (auto it = frame.actions->find(rule_name); it != frame.actions->end()) {
    Sv sv;
    sv.name = rule_name;
    // token(), not sv() -- for a `< ... >` token rule referenced inside a
    // sequence (e.g. `(OPERATOR ATOM)*`), sv() reaches past the boundary
    // into the whitespace %whitespace skips before the next element
    // (measured: OPERATOR's sv() for "+ 2" is "+ ", two bytes). token()
    // is the accessor peglib's own token_to_string()/token_to_number() use
    // for exactly this reason; sv() and token() agree for a branch rule
    // (no `< ... >` of its own), so this is a no-op there.
    sv.token = vs.token();
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
      for (const auto& [name, def] : c.parser.get_grammar()) {
        std::string rule_name = name;  // one copy per rule, captured below
        c.parser[name.c_str()].action =
            [&compiled, rule_name](::peg::SemanticValues& vs) {
              return _peg_reduce(compiled, rule_name, vs);
            };
      }
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
// default) is the whole result. Throws CulebraError("PEGError") on a syntax
// error or an `actions` key naming no rule in the grammar, with `path`
// honored the same way plain parse() honors it. Whatever exception a
// registered action itself throws (a culebra throw, a native TypeError, …)
// propagates unchanged -- cpp-peglib installs no catch around a rule
// reduction, so nothing here needs to either.
inline std::any parse_with_actions(Compiled& c,
                                   std::string_view text,
                                   std::string_view path,
                                   const ActionMap& actions) {
  for (const auto& [name, fn] : actions) {
    if (!c.parser.get_grammar().count(name)) {
      _fail(culebra::format("PEG: no such rule '{}'", name));
    }
  }
  _ParseState state(c, path);
  _AnyBox result;
  {
    ActionScope scope(c, actions, text.data());
    if (!c.parser.parse_n(text.data(), text.size(), result)) _fail_parse(c);
  }
  return result.v;
}

}  // namespace culebra::pegparser
