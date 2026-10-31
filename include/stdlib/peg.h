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
// culebra::pegparser::load() itself is absent from a binary that never
// names PEG, verified with `nm -C --defined-only` on a `--keep-symbols` build
// (plain `nm` on a stripped binary proves nothing either way), and so is every
// peglib symbol with it (check_aot_feature_axes.sh). What used to survive —
// peglib's vtables and the std::function managers of this file's lambdas —
// was held by GCC's SLP constant pool, which the core archive compiles
// without (CMakeLists.txt).
//
// The namespace is `pegparser`, not `peg`: a `culebra::peg` would shadow
// peglib's own `peg::` at every `peg::Ast` spelled inside namespace culebra
// (lint.h, vm.h, test_engine.h, …).

#include <algorithm>
#include <any>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <base/shared.h>  // CulebraError, the nesting-bound wording
#include <frontend/syntax_error.h>  // format_syntax_error()

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

// One thing peglib says of a grammar or of a subject, as its logger is told.
struct Diag {
  size_t line = 0, col = 0;
  std::string msg, rule;
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


// What a rule's callback is told about the match it runs for: cpp-peglib's
// SemanticValues, less what a caller of this header cannot hold. `sv` is the
// whole match and `tokens` the rule's own `< >` boundaries. `values[i]` is
// what child i's action produced; a tree-mode parse leaves it empty, since
// the values there are peglib's AST nodes.
struct Sv {
  std::string_view name;
  std::string_view sv;
  std::vector<std::string_view> tokens;
  size_t line = 1, column = 1, choice = 0, choice_count = 0;
  std::vector<std::any> values;
};

// What a predicate answers: cpp-peglib's bool with its two out parameters.
struct Verdict {
  bool ok = true;
  std::string msg;  // why not, reported where the rule started
  std::any data;    // the rule's action receives it as `predicate_data`
};

// A parse's callbacks, by rule name -- cpp-peglib's `parser["RULE"]` members,
// held per parse rather than on the grammar (which the compile cache shares).
// `std::any` in and out: this header stays value-neutral, and the binding
// layer's payload is a small copyable wrapper around a retained JitValue.
// `Sv&` rather than `const Sv&`: it is a fresh local handed to one call, so
// the binding layer moves each value out of it.
using RuleAction =
    std::function<std::any(Sv&, const std::any& predicate_data)>;
using RulePredicate = std::function<Verdict(Sv&)>;
// `pos` is the byte offset in the subject the rule was entered at, and
// `matchlen` peglib's own: size_t(-1) for a rule that did not match.
using RuleEnter = std::function<void(size_t pos)>;
using RuleLeave =
    std::function<void(size_t pos, size_t matchlen, const std::any& value)>;
using Logger = std::function<void(size_t line, size_t col,
                                  const std::string& msg,
                                  const std::string& rule)>;

template <typename F> using RuleMap = std::unordered_map<std::string, F>;

struct Hooks {
  // Set: every rule reduces through `actions` (one with none passes its first
  // value up, peglib's own default) and the parse yields the start rule's
  // value. Clear: the parse builds a tree and `actions` is not read.
  bool reduces = false;
  RuleMap<RuleAction> actions;
  RuleMap<RulePredicate> predicates;
  RuleMap<RuleEnter> enter;
  RuleMap<RuleLeave> leave;
  Logger log;  // every error of a subject, as peglib reports it
  // cpp-peglib's enable_packrat_parsing(), off until asked for as it is
  // there. Without memoization, alternatives that share a prefix backtrack
  // exponentially: measured on cpp-peglib's own README calculator, 10 levels
  // of nesting is 0.04 ms memoized and 2.0 s not. The cost is a table
  // proportional to input x rules (~1 KB per input byte).
  bool packrat = false;
};

// One peglib parser of a grammar. Its rules carry the callbacks of the parse
// running on it, so it runs one parse at a time: a callback that parses the
// same grammar again takes another engine (Compiled::engines).
struct Engine {
  ::peg::parser parser;
  bool busy = false;

  // What the rules carry, kept from one parse to the next: a grammar is
  // mostly parsed the same way every time.
  bool reduces = false;
  bool packrat = false;
  std::vector<std::string> predicates, enters, leaves;

  // The parse in flight. `err` is the first error it reported, or the depth
  // bound, which ends the parse and so outranks one recovered before it.
  const Hooks* hooks = nullptr;
  const char* base = nullptr;
  Diag err;
};

// A loaded grammar, owned by the per-thread compile cache and handed out
// shared so a caller's handle outlives a cache eviction. An engine is only
// ever reached from the thread that cached it.
struct Compiled {
  std::string grammar;
  std::string start;  // start rule; empty = the grammar's first definition
  std::vector<std::unique_ptr<Engine>> engines;  // load() builds the first
  // The node names of the rules marked `no_ast_opt`: what cpp-peglib's
  // optimize_ast() keeps, for a caller that optimizes a tree it was handed.
  std::vector<std::string> no_ast_opt;
  // What peglib said of a grammar it did load (a rule nothing refers to).
  std::vector<Diag> warnings;
};
using Handle = std::shared_ptr<Compiled>;

[[noreturn]] inline void _fail(const std::string& msg) {
  // No position: these are offsets into the grammar or the subject, not into
  // the culebra source, so they go in the text and the binding layer stamps
  // the call site (regex.h does the same).
  throw CulebraError("PEGError", msg, 0, 0);
}

// The error peglib's set_max_depth bound reports, as _build()'s `record` rewords it.
inline bool _is_depth_error(const std::string& msg) {
  return msg == nesting_too_deep_message(kPEGParseDepthLimit);
}

// Load `grammar`. A malformed one is a null engine. Either way `diags` is
// what peglib reported for it: the errors of one that did not load, the
// warnings of one that did (positions are inside the grammar text).
inline std::unique_ptr<Engine> _build(std::string_view grammar,
                                      std::string_view start,
                                      std::vector<Diag>& diags) {
  auto engine = std::make_unique<Engine>();
  // Raw pointer: the parser is a member, so the callback cannot outlive it.
  auto* e = engine.get();
  // The grammar's own failures (load_grammar) come through the logger, a
  // subject's through the reporter: the same words, with a control character
  // the grammar expects spelled as its escape.
  e->parser.set_logger([&diags](size_t ln, size_t col, const std::string& msg,
                                const std::string& rule) {
    diags.push_back(Diag{
        ln, col, reword_parse_depth_error(msg, kPEGParseDepthLimit), rule});
  });
  if (!e->parser.load_grammar(grammar, start)) {
    if (diags.empty()) diags.push_back(Diag{0, 0, "invalid grammar", ""});
    return nullptr;
  }
  e->parser.set_logger(peg::Log{});
  auto record = [e](size_t ln, size_t col, const std::string& msg,
                    const std::string& rule) {
    auto m = reword_parse_depth_error(msg, kPEGParseDepthLimit);
    if (e->hooks && e->hooks->log) e->hooks->log(ln, col, m, rule);
    if (e->err.msg.empty() || _is_depth_error(m)) {
      e->err = Diag{ln, col, std::move(m), rule};
    }
  };
  e->parser.set_error_reporter([record](const peg::ErrorReport& r) {
    record(r.line, r.col, culebra::format_syntax_error(r, nullptr), r.label);
  });
  e->parser.enable_ast();
  // Reported like any failure of a parse, through the reporter above.
  e->parser.set_max_depth(kPEGParseDepthLimit);
  return engine;
}

// cpp-peglib's load_grammar(): the loaded grammar, or null for a malformed
// one. `diags` is what its logger is told, the same on every load: loaded
// grammars are kept per thread, so loading one again costs a lookup.
inline Handle load(std::string_view grammar, std::string_view start,
                   std::vector<Diag>& diags) {
  static thread_local std::unordered_map<std::string, Handle> cache;
  std::string key;
  key.reserve(grammar.size() + start.size() + 1);
  key += start;
  key += '\n';  // no rule name contains one, so the split is unambiguous
  key += grammar;
  if (auto it = cache.find(key); it != cache.end()) {
    diags = it->second->warnings;
    return it->second;
  }
  auto engine = _build(grammar, start, diags);
  if (!engine) return nullptr;
  auto h = std::make_shared<Compiled>();
  h->grammar.assign(grammar);
  h->start.assign(start);
  h->warnings = diags;
  for (const auto& [name, rule] : engine->parser.get_grammar()) {
    if (rule.no_ast_opt) h->no_ast_opt.push_back(rule.node_name());
  }
  std::ranges::sort(h->no_ast_opt);
  h->engines.push_back(std::move(engine));
  if (cache.size() > 64) cache.clear();  // bound growth (a grammar is big)
  cache.emplace(std::move(key), h);
  return h;
}

// The same, for a caller with no logger to tell: a malformed grammar throws
// CulebraError("PEGError") with its first error.
inline Handle compile(std::string_view grammar, std::string_view start) {
  std::vector<Diag> diags;
  if (auto h = load(grammar, start, diags)) return h;
  _fail(culebra::format("PEG: grammar:{}:{}: {}", diags.front().line,
                        diags.front().col, diags.front().msg));
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

// A reducing parse boxes every rule's value in this one type. peglib's
// `parse_n<T>` reads the start rule's value with `std::any_cast<T>`, and a
// rule with no children and no action reduces to a contentless `std::any()`
// that no `any_cast` reads back: boxed, the outer type is always the same and
// its own `v` may be empty.
struct _AnyBox {
  std::any v;
};

// Moves the value out of a child's box: an action is the last reader of its
// `vs`, so each slot is read once. That is what keeps the binding layer's
// payload (JitAny, bindings.h) from needing a retain at every level a value
// passes through on its way up.
inline std::any _unbox(std::any& boxed) {
  if (!boxed.has_value()) return std::any();
  return std::move(std::any_cast<_AnyBox&>(boxed).v);
}

// Copies it instead: a predicate and a `leave` handler read values that the
// rule's action, or its parent's, reads after them.
inline std::any _peek(const std::any& boxed) {
  if (!boxed.has_value()) return std::any();
  return std::any_cast<const _AnyBox&>(boxed).v;
}

// `rule` is the name the callback was installed under, not `vs.name()`: a
// `{ precedence }` rule's PrecedenceClimbing operator re-invokes the rule's
// action once per binary step with a synthesized SemanticValues whose name is
// the last rule it matched, or empty.
inline Sv _sv(const std::string& rule, const ::peg::SemanticValues& vs) {
  Sv sv;
  sv.name = rule;
  sv.sv = vs.sv();
  sv.tokens = vs.tokens;
  std::tie(sv.line, sv.column) = vs.line_info();
  sv.choice = vs.choice();
  sv.choice_count = vs.choice_count();
  return sv;
}

// Every rule's action in a reducing parse: the caller's when it gave one,
// else peglib's own default (`vs.empty() ? any() : vs.front()`).
inline std::any _reduce(Engine& e, const std::string& rule,
                        ::peg::SemanticValues& vs,
                        const std::any& predicate_data) {
  auto it = e.hooks->actions.find(rule);
  if (it == e.hooks->actions.end()) {
    return std::any(_AnyBox{vs.empty() ? std::any() : _unbox(vs.front())});
  }
  Sv sv = _sv(rule, vs);
  sv.values.reserve(vs.size());
  for (size_t i = 0; i < vs.size(); i++) sv.values.push_back(_unbox(vs[i]));
  return std::any(_AnyBox{it->second(sv, predicate_data)});
}

// Switches the rules between reducing through _reduce and building a tree.
inline void _set_reduces(Engine& e, bool reduces) {
  if (e.reduces == reduces) return;
  for (const auto& [name, def] : e.parser.get_grammar()) {
    auto& action = e.parser[name.c_str()].action;
    if (reduces) {
      action = [&e, rule = name](::peg::SemanticValues& vs, std::any&,
                                 const std::any& predicate_data) {
        return _reduce(e, rule, vs, predicate_data);
      };
    } else {
      action = ::peg::Action();
    }
  }
  // enable_ast() gives its action to every rule that has none.
  if (!reduces) e.parser.enable_ast();
  e.reduces = reduces;
}

// Switches memoization, which peglib keeps as a flag on the start rule and
// reads at each parse. It has no call to clear it, so every rule is cleared.
inline void _set_packrat(Engine& e, bool packrat) {
  if (e.packrat == packrat) return;
  if (packrat) {
    e.parser.enable_packrat_parsing();
  } else {
    for (const auto& [name, def] : e.parser.get_grammar()) {
      e.parser[name.c_str()].enablePackratParsing = false;
    }
  }
  e.packrat = packrat;
}

// Makes the rules that carry one kind of callback the ones `wanted` names.
// What is installed looks its function up in the parse in flight, so a rule
// carrying one from the parse before needs nothing done.
template <typename F, typename Install, typename Remove>
inline void _carry(std::vector<std::string>& carried, const RuleMap<F>& wanted,
                   Install install, Remove remove) {
  auto same = carried.size() == wanted.size() &&
              std::ranges::all_of(carried, [&](const std::string& rule) {
                return wanted.contains(rule);
              });
  if (same) return;
  for (const auto& rule : carried) {
    if (!wanted.contains(rule)) remove(rule);
  }
  for (const auto& [rule, fn] : wanted) {
    if (std::ranges::find(carried, rule) == carried.end()) install(rule);
  }
  carried.clear();
  for (const auto& [rule, fn] : wanted) carried.push_back(rule);
}

inline void _install(Engine& e, const Hooks& hooks) {
  _set_reduces(e, hooks.reduces);
  _set_packrat(e, hooks.packrat);
  auto def = [&e](const std::string& rule) -> ::peg::Definition& {
    return e.parser[rule.c_str()];
  };
  _carry(
      e.predicates, hooks.predicates,
      [&](const std::string& rule) {
        def(rule).predicate = [&e, rule](const ::peg::SemanticValues& vs,
                                         const std::any&, std::string& msg,
                                         std::any& predicate_data) {
          Sv sv = _sv(rule, vs);
          if (e.reduces) {
            sv.values.reserve(vs.size());
            for (size_t i = 0; i < vs.size(); i++) {
              sv.values.push_back(_peek(vs[i]));
            }
          }
          Verdict v = e.hooks->predicates.at(rule)(sv);
          msg = std::move(v.msg);
          predicate_data = std::move(v.data);
          return v.ok;
        };
      },
      [&](const std::string& rule) { def(rule).predicate = ::peg::Predicate(); });
  _carry(
      e.enters, hooks.enter,
      [&](const std::string& rule) {
        def(rule).enter = [&e, rule](const ::peg::Context&, const char* s,
                                     size_t, std::any&) {
          e.hooks->enter.at(rule)(static_cast<size_t>(s - e.base));
        };
      },
      [&](const std::string& rule) { def(rule).enter = nullptr; });
  _carry(
      e.leaves, hooks.leave,
      [&](const std::string& rule) {
        def(rule).leave = [&e, rule](const ::peg::Context&, const char* s,
                                     size_t, size_t matchlen, std::any& value,
                                     std::any&) {
          e.hooks->leave.at(rule)(static_cast<size_t>(s - e.base), matchlen,
                                  e.reduces ? _peek(value) : std::any());
        };
      },
      [&](const std::string& rule) { def(rule).leave = nullptr; });
}

// An engine no parse is running on, one whose rules already reduce the way
// this parse does if there is one.
inline Engine& _idle_engine(Compiled& c, bool reduces) {
  Engine* other = nullptr;
  for (auto& e : c.engines) {
    if (e->busy) continue;
    if (e->reduces == reduces) return *e;
    other = e.get();
  }
  if (other) return *other;
  std::vector<Diag> none;  // the grammar loaded once, so it loads again
  c.engines.push_back(_build(c.grammar, c.start, none));
  return *c.engines.back();
}

// One parse's hold on an engine, given up on every exit, a thrown one
// included.
struct _Run {
  Engine& e;
  _Run(Compiled& c, const Hooks& hooks, const char* base)
      : e(_idle_engine(c, hooks.reduces)) {
    _install(e, hooks);
    e.busy = true;
    e.hooks = &hooks;
    e.base = base;
    e.err = {};
  }
  ~_Run() {
    e.busy = false;
    e.hooks = nullptr;
  }
};

// What a parse came to. `tree` is a tree-mode parse's value and `value` a
// reducing one's; `err` says why a subject did not parse, and where.
struct Parsed {
  bool ok = false;
  Tree tree;
  std::any value;
  Diag err;
};

// A subject that did not parse, as a PEGError. A caller who named the subject
// (`path`) gets a message that reads like any other compiler diagnostic; one
// who didn't gets the engine-prefixed form regex.h's own errors use.
[[noreturn]] inline void fail(const Parsed& r, std::string_view path) {
  std::string_view msg = r.err.msg;
  if (msg.empty()) msg = "syntax error";
  _fail(path.empty()
            ? culebra::format("PEG: {}:{}: {}", r.err.line, r.err.col, msg)
            : culebra::format("{}:{}:{}: {}", path, r.err.line, r.err.col,
                              msg));
}

// Parse `text` with `hooks`. A subject that does not parse is `ok` false, not
// a throw: fail() makes it one. What does throw is a hook naming no rule in
// the grammar (PEGError), a tree past the value-nesting bound (ValueError),
// and whatever a callback throws, unchanged -- cpp-peglib installs no catch
// around a rule, so nothing here does either. `want_value` false runs the
// same parse and builds nothing to hand back. `optimize` is cpp-peglib's
// optimize_ast() applied to a tree, with that `opt_mode`; unset leaves the
// tree as enable_ast() builds it.
inline Parsed run(Compiled& c, std::string_view text, const Hooks& hooks,
                  std::optional<bool> optimize, bool want_value) {
  const auto& rules = c.engines.front()->parser.get_grammar();
  auto check = [&](const auto& map) {
    for (const auto& [rule, fn] : map) {
      if (!rules.contains(rule)) {
        _fail(culebra::format("PEG: no such rule '{}'", rule));
      }
    }
  };
  check(hooks.actions);
  check(hooks.predicates);
  check(hooks.enter);
  check(hooks.leave);

  Parsed out;
  std::shared_ptr<::peg::Ast> ast;
  {
    _Run run(c, hooks, text.data());
    auto& parser = run.e.parser;
    if (!want_value) {
      out.ok = parser.parse(text);
    } else if (hooks.reduces) {
      _AnyBox result;
      out.ok = parser.parse_n(text.data(), text.size(), result);
      out.value = std::move(result.v);
    } else {
      out.ok = parser.parse(text, ast);
      if (out.ok && optimize) ast = parser.optimize_ast(ast, *optimize);
    }
    if (!out.ok) out.err = std::move(run.e.err);
  }
  if (out.ok && ast) {
    out.tree.nodes.reserve(64);
    _flatten(*ast, out.tree, 0);
    out.tree.owner = ast;
  }
  return out;
}

}  // namespace culebra::pegparser
