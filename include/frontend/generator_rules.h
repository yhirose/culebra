// Where a `yield` may be written.
//
// A function or method whose body contains `yield` / `yield from` is a
// generator. The compiler takes it as it is written and each engine suspends
// its frame at a `yield` (vm.h Op::GenStart / Op::Yield, rt/gen.inc.h), so
// nothing here rewrites anything: this pass refuses the two places a `yield`
// cannot stand, a `defer` and whatever is not the body of a function or a
// method. Both rules are the language's, not an implementation's.

#pragma once

#include "frontend/fragments.h"
#include "frontend/parser.h"

#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace culebra {

// These tag types open a fresh fn-body scope: anything found inside (a
// yield, a local, a class decl) belongs to the *inner* function, not the
// enclosing one — walkers stop here so it isn't reattributed upward.
inline bool is_fn_boundary(unsigned int tag) {
  using namespace peg::udl;
  return tag == "FUNCTION"_ || tag == "LAMBDA"_ || tag == "MULTIFN_DECL"_;
}

// A `yield` or a `yield from`: both suspend, so every walk counts them alike.
inline bool is_yield(unsigned int tag) {
  using namespace peg::udl;
  return tag == "YIELD"_ || tag == "YIELD_FROM"_;
}

// A function that a `yield` in its body makes a generator: a `fn name`, a
// `fn (...) { ... }` expression, a class method, a trait method's default
// body. (A `|...|` lambda is a function too, but its body is an expression.)
inline bool can_be_generator(unsigned int tag) {
  using namespace peg::udl;
  return tag == "MULTIFN_DECL"_ || tag == "FUNCTION"_ || tag == "METHOD"_ ||
         tag == "TRAIT_METHOD"_;
}

// A node that owns the yields written inside it.
inline bool opens_yield_scope(unsigned int tag) {
  using namespace peg::udl;
  return can_be_generator(tag) || tag == "LAMBDA"_;
}

// First YIELD or YIELD_FROM belonging to this fn body, stopping at the
// functions inside it (see `opens_yield_scope`) — a nested generator's yields
// are its own. nullptr when absent.
inline const peg::Ast* find_yield_in_fn_body(const peg::Ast& node) {
  if (is_yield(node.tag)) return &node;
  if (opens_yield_scope(node.tag)) return nullptr;
  for (auto& c : node.nodes) {
    if (auto* y = find_yield_in_fn_body(*c)) return y;
  }
  return nullptr;
}

inline bool fn_body_has_yield(const peg::Ast& node) {
  return find_yield_in_fn_body(node) != nullptr;
}

// First YIELD or YIELD_FROM that belongs to no generator, crossing function
// boundaries. A function's own yields are the compiler's, whatever kind of
// function it is: a `fn name`, a `fn (...) { ... }` expression (an object
// property's among them), a class method, a trait's default method. What is
// left: the top level of a file, a `|...|` lambda (its body is an expression),
// a `defer` body, a constructor `new` (it hands back the instance) and
// `drop` (the runtime calls it, and its result goes nowhere). The tag is
// enough: YIELD / YIELD_FROM are never collapsed away
// (ast_optimizer_keep_rules), and a parent collapsing onto one takes its tag.
inline const peg::Ast* find_orphan_yield(const peg::Ast& node,
                                         bool owned = false) {
  using namespace peg::udl;
  if (is_yield(node.tag)) return owned ? nullptr : &node;
  if (node.tag == "MULTIFN_DECL"_ || node.tag == "FUNCTION"_ ||
      node.tag == "TRAIT_METHOD"_) {
    owned = true;
  } else if (node.tag == "METHOD"_) {
    auto name = view_method(node).name;
    owned = name != "new" && name != "drop";
  } else if (node.tag == "CLASS_DECL"_ || node.tag == "TRAIT_DECL"_ ||
             node.tag == "LAMBDA"_ || node.tag == "DEFER"_) {
    owned = false;
  }
  for (auto& c : node.nodes) {
    if (auto* y = find_orphan_yield(*c, owned)) return y;
  }
  return nullptr;
}

// Locate the first YIELD reachable from inside a DEFER's body, or nullptr. A
// defer runs as its scope is left, which includes the close of a generator
// suspended in that scope, so a body that suspended again could not be
// closed. A `try` / `catch` body may yield: it is ordinary code of the frame.
inline const peg::Ast* find_yield_inside_defer(const peg::Ast& body) {
  using namespace peg::udl;
  const peg::Ast* found = nullptr;
  std::function<void(const peg::Ast&, bool)> walk =
      [&](const peg::Ast& n, bool inside_defer) {
        if (found) return;
        if (opens_yield_scope(n.tag)) return;
        if (inside_defer && is_yield(n.tag)) {
          found = &n;
          return;
        }
        if (n.tag == "DEFER"_) inside_defer = true;
        for (auto& c : n.nodes) walk(*c, inside_defer);
      };
  walk(body, false);
  return found;
}

// The rules one generator is held to. `fn` is a function of any kind parsed
// from `src`, its body last, whose body yields. (A self-contained
// `handle { … }` or an `effect fn` declared in the body is the effects pass's,
// which lowers it where it stands; a bare `perform` outside any handle is
// rejected there.)
inline void check_generator_fn(const peg::Ast& fn, const std::string& src) {
  const auto& body = *fn.nodes.back();

  // A yield statement may not appear inside a defer block (the C# rule,
  // CS1626, without its try-catch half). Cleanup belongs in a defer, and a
  // value that needs one yields before the scope ends.
  if (auto* bad = find_yield_inside_defer(body)) {
    throw CulebraError(
        "SyntaxError",
        "yield cannot appear inside a defer block: a defer runs as its scope "
        "is left, including when the generator is closed there, so it cannot "
        "suspend. Yield before the scope ends.",
        source_pos(*bad, src).line, source_pos(*bad, src).col);
  }
}

// Hold every generator in the tree to those rules. `handle` and `effect fn`
// are left to the effects pass, which runs this over what it emits for them.
// A generator's own body is walked too: a function value in it may declare
// another generator.
inline void check_generators_in(const peg::Ast& ast, const std::string& src) {
  using namespace peg::udl;
  if (ast.tag == "HANDLE"_ || ast.tag == "EFFECT_FN_DECL"_) return;
  if (can_be_generator(ast.tag) && !ast.nodes.empty() &&
      fn_body_has_yield(*ast.nodes.back()))
    check_generator_fn(ast, src);
  for (const auto& c : ast.nodes) check_generators_in(*c, src);
}

// Reject the yields no generator owns. Runs from `apply_transforms` once the
// effects pass is done, so that pass gets to diagnose its own bodies first,
// and the yields it re-parses into fragments are covered too.
inline void reject_orphan_yield(const peg::Ast& ast) {
  if (auto* y = find_orphan_yield(ast)) {
    throw CulebraError(
        "SyntaxError",
        "yield can only appear inside the body of a function or a method "
        "(not `new` or `drop`): the top level of a file, a `|...|` lambda, "
        "a constructor and `drop` cannot be generators. Move the yield into "
        "a named fn and call it instead.",
        static_cast<long>(y->line), static_cast<long>(y->column));
  }
}

}  // namespace culebra
