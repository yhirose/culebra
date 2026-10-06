// Where a `yield` may be written.
//
// A `fn name(...) { ... }` whose body contains `yield` / `yield from` is a
// generator. The compiler takes it as it is written and each engine suspends
// its frame at a `yield` (vm.h Op::GenStart / Op::Yield, rt/gen.inc.h), so
// nothing here rewrites anything: this pass refuses the places a `yield`
// cannot stand, and what a generator body cannot hold.
//
// Those refusals date from when a generator was lowered to a state class, the
// way an effect body still is (effects_transform.h), and are kept with their
// wording until lifted. Lifting one is a language change of its own.

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

// First YIELD or YIELD_FROM belonging to this fn body, stopping at fn
// boundaries (see `is_fn_boundary`) — a nested generator's yields are its
// own. nullptr when absent.
inline const peg::Ast* find_yield_in_fn_body(const peg::Ast& node) {
  if (is_yield(node.tag)) return &node;
  if (is_fn_boundary(node.tag)) return nullptr;
  for (auto& c : node.nodes) {
    if (auto* y = find_yield_in_fn_body(*c)) return y;
  }
  return nullptr;
}

inline bool fn_body_has_yield(const peg::Ast& node) {
  return find_yield_in_fn_body(node) != nullptr;
}

// First YIELD or YIELD_FROM that belongs to no `fn name(...)` declaration,
// crossing fn boundaries: one in a class method, an object property's fn, a
// fn expression, a `defer` body or at top level. A named fn's own yields are
// the compiler's. The tag is enough: YIELD / YIELD_FROM are never collapsed
// away (ast_optimizer_keep_rules), and a parent collapsing onto one takes its
// tag.
inline const peg::Ast* find_orphan_yield(const peg::Ast& node,
                                         bool in_named_fn = false) {
  using namespace peg::udl;
  if (is_yield(node.tag)) return in_named_fn ? nullptr : &node;
  if (node.tag == "MULTIFN_DECL"_) in_named_fn = true;
  else if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_ ||
           node.tag == "METHOD"_ || node.tag == "DEFER"_)
    in_named_fn = false;
  for (auto& c : node.nodes) {
    if (auto* y = find_orphan_yield(*c, in_named_fn)) return y;
  }
  return nullptr;
}

// Locate the first YIELD reachable from inside a TRY's try-block / catch
// block, or from inside a DEFER's body. Returns nullptr if no such yield
// exists. Used to enforce the C# rule (CS1626): yield statements may not
// appear inside a try-catch or defer. `yield try {...}
// catch e {...}` is fine because the yield is OUTSIDE the try (only the
// try-expression's value flows through it) — the guard descends into a
// TRY's body BLOCKs but not into its position as an expression.
inline const peg::Ast* find_yield_inside_try_or_defer(const peg::Ast& body) {
  using namespace peg::udl;
  const peg::Ast* found = nullptr;
  std::function<void(const peg::Ast&, bool)> walk =
      [&](const peg::Ast& n, bool inside_guard) {
        if (found) return;
        if (is_fn_boundary(n.tag)) return;
        if (inside_guard && is_yield(n.tag)) {
          found = &n;
          return;
        }
        if (n.tag == "TRY"_) {
          // TRY children: try-BLOCK, catch-IDENT, catch-BLOCK
          if (n.nodes.size() > 0) walk(*n.nodes[0], true);
          if (n.nodes.size() > 2) walk(*n.nodes[2], true);
          return;
        }
        if (n.tag == "DEFER"_) {
          if (!n.nodes.empty()) walk(*n.nodes[0], true);
          return;
        }
        for (auto& c : n.nodes) walk(*c, inside_guard);
      };
  walk(body, false);
  return found;
}

// The rules one generator is held to. `fn` is a MULTIFN_DECL parsed from
// `src` whose body yields. (A self-contained `handle { … }` or an
// `effect fn` declared in the body is the effects pass's, which lowers it
// where it stands; a bare `perform` outside any handle is rejected there.)
inline void check_generator_fn(const peg::Ast& fn, const std::string& src) {
  using namespace peg::udl;
  size_t i = 0;
  while (i < fn.nodes.size() && fn.nodes[i]->tag == "DECORATOR"_) i++;
  if (i + 2 >= fn.nodes.size()) return;
  const auto& body = *fn.nodes.back();

  // C# rule (CS1626): a yield statement may not appear inside a try-catch
  // or defer block. Cleanup belongs in a top-level `defer { ... }`;
  // value-level recovery in the yielded expression
  // (`yield try { ... } catch e { ... }`).
  if (auto* bad = find_yield_inside_try_or_defer(body)) {
    throw CulebraError(
        "SyntaxError",
        "yield cannot appear inside a try-catch or defer block. Move "
        "the try to the yielded expression value (yield try { ... } "
        "catch e { ... }) or use a top-level `defer { ... }` for cleanup.",
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
  if (ast.tag == "MULTIFN_DECL"_ && fn_body_has_yield(*ast.nodes.back()))
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
        "yield can only appear inside a `fn name(...) { ... }` declaration "
        "body — a class method, an object property's function, or a fn "
        "expression cannot be a generator. Declare a named fn and call it "
        "instead.",
        static_cast<long>(y->line), static_cast<long>(y->column));
  }
}

}  // namespace culebra
