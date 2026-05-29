// Generator (yield) AST transformation pass.
//
// Stage 0 (yield 構文 + 1 yield 直線 body): re-parse-based source
// synthesis. A `fn name(...) { yield expr }` is rewritten to:
//
//   fn name(...) {
//     class _Gen_<name>_<line>_<col> {
//       new(_y) { this.phase = 0; this._y = _y }
//       iter() { this }
//       has_next() { this.phase != 1 }
//       next() {
//         if this.phase == 0 {
//           this.phase = 1
//           return this._y
//         }
//         return nil
//       }
//     }
//     _Gen_<name>_<line>_<col>.new(<original yield expr>)
//   }
//
// Stage 0 cheats by evaluating the yield expression eagerly and
// passing it to the synthesized class's constructor — fine for the
// single-yield case, replaced in Stage 1 by a real state machine
// dispatch over phases. Stage 2 added while-loop bodies with a direct
// yield; Stage 3 generalizes it so the yield can live inside an
// IF/ELSE/MATCH branch of the loop body.
//
// See [[project-generator-design]] for the 5 core design axes
// (anonymous class / pure transformation / Phase 2 protocol /
// source-location propagation / all-locals-on-heap).

#pragma once

#include "parser.h"

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// Storage for synthesized generator source fragments. peg::Ast holds
// `string_view`s into the parsed source, so anything the transform
// re-parses needs process-lifetime backing. See
// [[feedback-ast-source-lifetime]] — same fix the lazy-module path
// uses.
inline std::vector<std::shared_ptr<std::string>>&
generator_transform_sources() {
  static std::vector<std::shared_ptr<std::string>> sources;
  return sources;
}

// A YIELD found inside one of these tag types belongs to the *inner*
// function, not the enclosing one — the walkers below stop here so a
// nested generator's yields aren't reattributed upward.
inline bool is_fn_boundary(unsigned int tag) {
  using namespace peg::udl;
  return tag == "FUNCTION"_ || tag == "LAMBDA"_ || tag == "MULTIFN_DECL"_;
}

// True if `node` carries at least one YIELD or YIELD_FROM anywhere
// inside, stopping at fn boundaries (see `is_fn_boundary`). Treating
// the two as equivalent here lets Stage 5's for-in desugar fire even
// when the only yield-shaped node inside is a `yield from`.
inline bool fn_body_has_yield(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "YIELD"_ || node.tag == "YIELD_FROM"_) return true;
  if (is_fn_boundary(node.tag)) return false;
  for (auto& c : node.nodes) {
    if (fn_body_has_yield(*c)) return true;
  }
  return false;
}

// True if `node` has at least one YIELD_FROM. The dispatcher uses this
// to route bodies with delegation through the Stage 6b phase machine
// instead of the Stage 3 verbatim-while codegen.
inline bool body_has_yield_from(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "YIELD_FROM"_) return true;
  if (is_fn_boundary(node.tag)) return false;
  for (auto& c : node.nodes) {
    if (body_has_yield_from(*c)) return true;
  }
  return false;
}

// Collect every DEFER node in a fn body in source order, stopping at
// inner fn boundaries. Used by the dispatcher to verify that all defers
// live at the body's top level (where Stage 3 emits them into the
// generator's defer registry); defers buried inside loop/if bodies have
// no good translation today and surface as a SyntaxError.
inline std::vector<const peg::Ast*> collect_defers(const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (is_fn_boundary(n.tag)) return;
    if (n.tag == "DEFER"_) { out.push_back(&n); return; }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Locate the first YIELD reachable from inside a TRY's try-block / catch
// block, or from inside a DEFER's body. Returns nullptr if no such yield
// exists. Used by the dispatcher to enforce the C# rule (CS1626): yield
// statements may not appear inside a try-catch or defer. `yield try {...}
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
        if (inside_guard && (n.tag == "YIELD"_ || n.tag == "YIELD_FROM"_)) {
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

// Collect every YIELD inside a fn body in source order, stopping at
// inner fn boundaries (same rule as `fn_body_has_yield`).
inline std::vector<const peg::Ast*> collect_yields(const peg::Ast& node) {
  std::vector<const peg::Ast*> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    using namespace peg::udl;
    if (n.tag == "YIELD"_) { out.push_back(&n); return; }
    if (is_fn_boundary(n.tag)) return;
    for (auto& c : n.nodes) walk(*c);
  };
  walk(node);
  return out;
}

// Slice the source range an AST node spans. peg::Ast carries
// `position` + `length` in bytes; safe so long as `src` is the same
// buffer that produced `node`.
inline std::string_view ast_source_slice(const peg::Ast& node,
                                          const char* src, size_t src_len) {
  if (node.position + node.length > src_len) return {};
  return std::string_view(src + node.position, node.length);
}

// --- Source-text helpers -------------------------------------------------

// Concatenated source range covering a contiguous slice of AST nodes
// (whitespace + comments between them included). Empty list → "".
inline std::string ast_source_range(const std::vector<const peg::Ast*>& stmts,
                                     const char* src, size_t src_len) {
  if (stmts.empty()) return "";
  auto first = stmts.front()->position;
  auto last_end = stmts.back()->position + stmts.back()->length;
  if (last_end > src_len) return "";
  return std::string(src + first, last_end - first);
}

// Drop the enclosing `{` / `}` from a BLOCK source slice. A BLOCK that's
// been parsed will always be wrapped in braces, but the check stays
// defensive in case the slice is already brace-stripped or empty.
inline std::string_view strip_block_braces(std::string_view s) {
  if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// Rewrite every `\b<name>\b` in `src` to `this.<name>` for each name in
// `names`, then strip `let `/`mut ` prefixes that now sit in front of
// `this.X` (assignment, no longer declaration). Stage 2 spike uses a
// regex word-boundary pass — adequate for counter/fib bodies; the
// edge cases (string literals / comments containing local names,
// for-in loop bindings) are out of Stage 2 scope and surface as
// transformation bugs to revisit if hit.
inline std::string rewrite_locals_to_this(std::string_view src,
                                          const std::set<std::string>& names) {
  // The two declaration-strip patterns are name-independent — hoist
  // them to file scope so each Stage 2 transform doesn't recompile
  // them. `std::regex` construction is the famously expensive part.
  static const std::regex strip_let_this(R"(\blet\s+this\.)");
  static const std::regex strip_mut_this(R"(\bmut\s+this\.)");
  std::string out(src);
  std::vector<std::string> sorted(names.begin(), names.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  for (const auto& name : sorted) {
    std::regex pat("\\b" + name + "\\b");
    out = std::regex_replace(out, pat, "this." + name);
  }
  out = std::regex_replace(out, strip_let_this, "this.");
  out = std::regex_replace(out, strip_mut_this, "this.");
  return out;
}

// Collect every name introduced by a `let`/`mut` decl in a fn body
// (single-lvalue form only; multi-lvalue chains skipped). Stops at
// nested fn boundaries.
inline std::set<std::string> collect_local_names(const peg::Ast& body) {
  using namespace peg::udl;
  std::set<std::string> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (is_fn_boundary(n.tag)) return;
    if (n.tag == "ASSIGNMENT"_) {
      auto av = view_assignment(n);
      if ((av.is_let || av.is_mut) && av.lvalcnt == 1) {
        const auto& ident = *n.nodes[av.lvaloff];
        if (ident.tag == "IDENTIFIER"_) out.insert(std::string(ident.token));
      }
    }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// --- Stage 3 pattern: pre-loop init + while { pre-yield, yield-bearing stmt,
// post-yield }. `yield_stmt` is one loop-body statement that may BE a yield
// (Stage 2 case) or an IF/MATCH whose branches contain the yields. All yields
// in the loop must live inside that one statement.

struct Stage3Pattern {
  std::vector<const peg::Ast*> pre_loop;
  const peg::Ast* loop_cond;
  std::vector<const peg::Ast*> pre_yield;
  const peg::Ast* yield_stmt;
  std::vector<const peg::Ast*> post_yield;
};

// Unwrap a STATEMENT wrapper down to the concrete tag (CLASS_DECL,
// WHILE, ASSIGNMENT, ...). AstOptimizer collapses most STATEMENTs
// already, but the wrapper survives for the choice-tagged variants.
inline const peg::Ast* unwrap_stmt(const peg::Ast* s) {
  using namespace peg::udl;
  if (s && s->tag == "STATEMENT"_ && !s->nodes.empty()) {
    return s->nodes[0].get();
  }
  return s;
}

// View the children of a fn body (BLOCK / STATEMENTS / single stmt) as
// a flat vector of statement-shaped nodes.
inline std::vector<const peg::Ast*> body_stmts(const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  if (body.tag == "STATEMENTS"_) {
    for (auto& c : body.nodes) out.push_back(c.get());
  } else {
    out.push_back(&body);
  }
  return out;
}

inline std::optional<Stage3Pattern> match_stage3_pattern(
    const peg::Ast& body, const std::vector<const peg::Ast*>& yields) {
  using namespace peg::udl;
  if (yields.empty()) return std::nullopt;
  auto stmts = body_stmts(body);
  // Locate the WHILE among top-level body stmts.
  size_t while_idx = stmts.size();
  const peg::Ast* while_node = nullptr;
  for (size_t i = 0; i < stmts.size(); i++) {
    auto* s = unwrap_stmt(stmts[i]);
    if (s->tag == "WHILE"_) {
      if (while_node) return std::nullopt;  // multiple loops
      while_node = s;
      while_idx = i;
    }
  }
  if (!while_node || while_node->nodes.size() < 2) return std::nullopt;
  // Reject anything after the while (Stage 3 keeps the loop terminal).
  if (while_idx + 1 != stmts.size()) return std::nullopt;
  // Every yield must be inside the loop body, not in pre-loop stmts.
  const peg::Ast* loop_body = while_node->nodes[1].get();
  size_t lb_lo = loop_body->position;
  size_t lb_hi = loop_body->position + loop_body->length;
  for (auto* y : yields) {
    if (y->position < lb_lo || y->position + y->length > lb_hi) {
      return std::nullopt;
    }
  }
  // All yields must live inside one loop-body statement (which may itself BE
  // a yield, or an IF/MATCH containing them). yields are in source order, so
  // the stmt containing yields[0] is the only possible candidate — anything
  // straddling into a sibling stmt fails the pattern.
  auto inner = body_stmts(*loop_body);
  size_t yidx = inner.size();
  for (size_t i = 0; i < inner.size(); i++) {
    auto* s = inner[i];
    size_t s_lo = s->position;
    size_t s_hi = s->position + s->length;
    if (yields[0]->position < s_lo ||
        yields[0]->position + yields[0]->length > s_hi) continue;
    for (auto* y : yields) {
      if (y->position < s_lo || y->position + y->length > s_hi) {
        return std::nullopt;
      }
    }
    yidx = i;
    break;
  }
  if (yidx == inner.size()) return std::nullopt;
  Stage3Pattern p;
  p.loop_cond = while_node->nodes[0].get();
  for (size_t i = 0; i < while_idx; i++) p.pre_loop.push_back(stmts[i]);
  for (size_t i = 0; i < yidx; i++) p.pre_yield.push_back(inner[i]);
  p.yield_stmt = inner[yidx];
  for (size_t i = yidx + 1; i < inner.size(); i++) {
    p.post_yield.push_back(inner[i]);
  }
  return p;
}

// --- Stage 5: for-in desugar to while + iterator -------------------------

// Outermost FOR nodes (within a single fn) that contain at least one YIELD
// somewhere in their subtree. Stops at fn boundaries and does NOT descend
// into yielding FORs — those are rewritten as a unit, so any inner FOR
// nested within them is left alone for a follow-up pass. The walker only
// surfaces FORs whose yields belong to *this* generator.
inline std::vector<const peg::Ast*> collect_outermost_yielding_fors(
    const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (is_fn_boundary(n.tag)) return;
    if (n.tag == "FOR"_ && fn_body_has_yield(n)) {
      out.push_back(&n);
      return;
    }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Source-level rewrite: each outermost yielding `for x in expr BODY` becomes
// `let _g_it_<pos> = (expr).iter()
//  while _g_it_<pos>.has_next() {
//    let x = _g_it_<pos>.next()
//    BODY_inner
//  }`
// Returns the rewritten body source, or `nullopt` when there were no
// yielding FORs to rewrite (so callers can gate the desugar pipeline on a
// single walk). Iterator variable names use the FOR's source position to
// stay unique. The result has fresh source positions once re-parsed;
// callers must re-parse before walking the AST again.
inline std::optional<std::string> rewrite_yielding_fors_to_while(
    const peg::Ast& body, const char* src, size_t src_len) {
  auto fors = collect_outermost_yielding_fors(body);
  if (fors.empty()) return std::nullopt;
  std::string out(ast_source_slice(body, src, src_len));
  size_t base = body.position;
  for (auto it = fors.rbegin(); it != fors.rend(); ++it) {
    auto* f = *it;
    if (f->nodes.size() < 3) continue;
    const auto& var_node = *f->nodes[0];
    const auto& expr_node = *f->nodes[1];
    const auto& blk_node = *f->nodes[2];
    auto expr_sv = ast_source_slice(expr_node, src, src_len);
    auto blk_inner = strip_block_braces(
        ast_source_slice(blk_node, src, src_len));
    auto iter_var = std::format("_g_it_{}", f->position);
    auto replacement = std::format(
        "let {0} = ({1}).iter()\n"
        "while {0}.has_next() {{\n"
        "  let {2} = {0}.next()\n"
        "  {3}\n"
        "}}",
        iter_var, std::string(expr_sv),
        std::string(var_node.token), std::string(blk_inner));
    out.replace(f->position - base, f->length, replacement);
  }
  return out;
}

// --- Transformation entry points -----------------------------------------

// Re-parse a `fn __gen_wrapper__(...) { ... }` source fragment and return
// its MULTIFN_DECL node, after registering the source for lifetime. peg::Ast
// holds string_views into the source, so the synthesized buffer must stay
// alive until the AST is discarded — generator_transform_sources() owns it.
inline std::shared_ptr<peg::Ast> parse_wrapper_fn(
    std::shared_ptr<std::string> synthesized) {
  using namespace peg::udl;
  generator_transform_sources().push_back(synthesized);
  std::vector<std::string> msgs;
  auto sub_ast = parse("<generator-transform>", synthesized->data(),
                       synthesized->size(), msgs);
  if (!sub_ast) return nullptr;
  std::shared_ptr<peg::Ast> wrapper_fn;
  std::function<void(std::shared_ptr<peg::Ast>)> walk =
      [&](std::shared_ptr<peg::Ast> n) {
        if (wrapper_fn) return;
        if (n->tag == "MULTIFN_DECL"_) { wrapper_fn = n; return; }
        for (auto& c : n->nodes) walk(c);
      };
  walk(sub_ast);
  return wrapper_fn;
}

// Replace `ast->nodes.back()` (the original body BLOCK) with the BLOCK
// from a freshly-parsed `fn __gen_wrapper__() { ... }` source fragment.
// Shared by Stage 1 and Stage 2 — both end with "now swap the body".
inline bool swap_body_from_wrapper(std::shared_ptr<peg::Ast> ast,
                                   std::shared_ptr<std::string> synthesized) {
  auto wrapper_fn = parse_wrapper_fn(synthesized);
  if (!wrapper_fn) return false;
  ast->nodes.back() = wrapper_fn->nodes.back();
  return true;
}

// Replace `ast`'s PARAMETERS (at `params_idx`) and body with those from a
// freshly-parsed `fn __gen_wrapper__(<params>) { <body> }` source. Keeps the
// original name (and decorators) intact, while giving downstream stages an
// AST whose param + body positions point into the synthesized source — the
// shape Stage 3 needs after the Stage 5 for-in desugar rewrites the body.
inline bool swap_body_with_wrapper_params(
    std::shared_ptr<peg::Ast> ast,
    std::shared_ptr<std::string> synthesized,
    size_t params_idx) {
  auto wrapper_fn = parse_wrapper_fn(synthesized);
  if (!wrapper_fn || wrapper_fn->nodes.size() < 3) return false;
  if (ast->nodes.size() <= params_idx + 1) return false;
  ast->nodes[params_idx + 1] = wrapper_fn->nodes[1];
  ast->nodes.back() = wrapper_fn->nodes.back();
  return true;
}

// Stage 1: yield expressions are eager-evaluated in the factory and
// passed through positional ctor args `_y0, _y1, ...`. Works for any
// body whose yields depend only on params/literals — see
// [[project-generator-design]] §Stage 1. The class carries a no-op
// `dispose()` so for-in's unconditional cleanup call lands on a real
// method (the dispatcher already rejects Stage 1 bodies that try to
// use `defer`, so there's nothing to actually run here).
inline std::shared_ptr<peg::Ast> transform_one_generator_fn_stage1(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len,
    const peg::Ast& name_ast,
    const std::vector<const peg::Ast*>& yields) {
  auto gen_name = std::format("_Gen_{}_{}_{}",
                              std::string(name_ast.token),
                              name_ast.line, name_ast.column);

  std::string ctor_params;
  std::string ctor_body_inits;
  std::string ctor_call_args;
  std::string arms;
  for (size_t j = 0; j < yields.size(); j++) {
    auto yexpr = ast_source_slice(*yields[j]->nodes[0], src, src_len);
    if (yexpr.empty()) return ast;
    auto slot = std::format("_y{}", j);
    if (j > 0) {
      ctor_params += ", ";
      ctor_call_args += ", ";
    }
    ctor_params += slot;
    ctor_call_args += std::string(yexpr);
    ctor_body_inits += std::format("; this.{0} = {0}", slot);
    arms += std::format(
        "      if this.phase == {0} {{\n"
        "        this.phase = {1}\n"
        "        return this.{2}\n"
        "      }}\n",
        j, j + 1, slot);
  }

  auto synthesized = std::make_shared<std::string>(std::format(
      "fn __gen_wrapper__() {{\n"
      "  class {0} {{\n"
      "    new({1}) {{ this.phase = 0{2} }}\n"
      "    iter() {{ this }}\n"
      "    has_next() {{ this.phase != {3} }}\n"
      "    next() {{\n"
      "{4}"
      "      return nil\n"
      "    }}\n"
      "    dispose() {{}}\n"
      "  }}\n"
      "  {0}.new({5})\n"
      "}}\n",
      gen_name, ctor_params, ctor_body_inits,
      yields.size(), arms, ctor_call_args));
  swap_body_from_wrapper(ast, synthesized);
  return ast;
}

// Substitute every YIELD inside `stmt_src` (a source slice starting at
// absolute `base_pos`) with the lookahead-set-and-return inline. Yields
// are processed back-to-front so earlier positions stay valid as later
// replacements shift the tail. `yields` must be in source order and
// every yield must lie inside the stmt range (caller guarantees).
// YIELD carries `{ no_ast_opt }` so its position+length survives AST
// collapse exactly — the replacement maps `yield expr` 1:1 with no
// outer-block padding needed.
inline std::string substitute_yields_in_stmt(
    std::string_view stmt_src, size_t base_pos,
    const std::vector<const peg::Ast*>& yields,
    const char* src, size_t src_len) {
  std::string out(stmt_src);
  for (auto it = yields.rbegin(); it != yields.rend(); ++it) {
    auto* y = *it;
    if (y->position < base_pos ||
        y->position + y->length > base_pos + stmt_src.size()) {
      continue;
    }
    size_t local_pos = y->position - base_pos;
    auto yexpr_sv = ast_source_slice(*y->nodes[0], src, src_len);
    auto replacement = std::format(
        "this._g_la = ({}); this._g_has_la = true; return true",
        std::string(yexpr_sv));
    out.replace(local_pos, y->length, replacement);
  }
  return out;
}

// Wire the generated class's `new(_p0, _p1, ...)` slot list and append
// matching `this.<param> = _pN` initializers, then `this.<local> = nil`
// for every body-declared local not shadowed by a param. Shared between
// Stage 3 and Stage 6b — both stages seed `ctor_inits` with their own
// state-field prefix (`_g_drained` / `_g_phase` / etc.) before calling
// this to append the per-instance bindings.
inline void emit_ctor_param_and_local_inits(
    const std::vector<std::string_view>& param_names,
    const std::set<std::string>& locals,
    std::string& ctor_params,
    std::string& ctor_call_args,
    std::string& ctor_inits) {
  std::set<std::string> param_set;
  for (size_t j = 0; j < param_names.size(); j++) {
    auto pn = std::string(param_names[j]);
    auto slot = std::format("_p{}", j);
    if (j > 0) {
      ctor_params += ", ";
      ctor_call_args += ", ";
    }
    ctor_params += slot;
    ctor_call_args += pn;
    ctor_inits += std::format("      this.{} = {}\n", pn, slot);
    param_set.insert(std::move(pn));
  }
  for (const auto& l : locals) {
    if (!param_set.contains(l)) {
      ctor_inits += std::format("      this.{} = nil\n", l);
    }
  }
}

// Split pre-loop into (a) the non-defer statements (kept inline) and
// (b) the LIFO-ordered defer bodies (lifted into the generator's
// `dispose()` method directly, inlined rather than wrapped in
// closures). Inlining sidesteps a JIT limitation around `this` capture
// in closures created from method bodies, and the closure indirection
// wasn't doing anything Stage 6a's minimum scope needed anyway —
// defers at body top level are unconditional, so reach + run are
// equivalent. The dispose code is gated on `_g_inited` so a generator
// disposed before any iteration doesn't fire defers whose locals
// haven't been initialized yet.
inline void split_pre_loop_stmts_and_defers(
    const std::vector<const peg::Ast*>& stmts,
    const char* src, size_t src_len,
    const std::set<std::string>& rewrite_set,
    std::string& out_stmts,
    std::string& out_dispose_body) {
  using namespace peg::udl;
  std::vector<std::string> defer_bodies;
  for (auto* stmt : stmts) {
    auto* unwrapped = unwrap_stmt(stmt);
    if (unwrapped->tag == "DEFER"_ && !unwrapped->nodes.empty()) {
      auto block_sv = ast_source_slice(*unwrapped->nodes[0], src, src_len);
      auto inner_sv = strip_block_braces(block_sv);
      defer_bodies.push_back(rewrite_locals_to_this(inner_sv, rewrite_set));
    } else {
      auto stmt_sv = ast_source_slice(*stmt, src, src_len);
      out_stmts += rewrite_locals_to_this(stmt_sv, rewrite_set);
      out_stmts += "\n";
    }
  }
  // LIFO: last-registered defer runs first.
  for (auto it = defer_bodies.rbegin(); it != defer_bodies.rend(); ++it) {
    out_dispose_body += "        ";
    out_dispose_body += *it;
    out_dispose_body += "\n";
  }
}

// Stage 3: while-loop body whose yields all live inside one statement
// (the yield-bearing stmt — may be a direct YIELD, an IF/ELSE, or a
// MATCH). The loop is preserved verbatim in `has_next()` so iterations
// that don't yield (e.g. filter_even's odd numbers) skip naturally. The
// yield-bearing stmt's source is rewritten so each `yield expr` becomes
// `this._g_la = (expr); this._g_has_la = true; return true` — control
// re-enters `has_next()` on the next call and the top-of-method
// post-yield block advances state past the returned yield. This
// supersedes Stage 2 (single direct yield) — a Stage 2-shape body
// matches Stage 3 with `yield_stmt = YIELD` and produces equivalent
// behavior. See [[project-generator-design]] §Stage 3 設計詳細.
inline std::shared_ptr<peg::Ast> transform_one_generator_fn_stage3(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len,
    const peg::Ast& name_ast,
    const peg::Ast& params_ast,
    const Stage3Pattern& p,
    const std::vector<const peg::Ast*>& yields) {
  auto gen_name = std::format("_Gen_{}_{}_{}",
                              std::string(name_ast.token),
                              name_ast.line, name_ast.column);

  std::vector<std::string_view> param_names;
  for (const auto& pn : params_ast.nodes) {
    if (is_kw_only_sep(*pn) || is_kwargs_rest(*pn)) continue;
    param_names.push_back(view_parameter(*pn).name);
  }
  auto locals = collect_local_names(*ast->nodes.back());
  std::set<std::string> rewrite_set = locals;
  for (auto& pn : param_names) rewrite_set.insert(std::string(pn));

  // Ctor takes `_p0, _p1, ...` (off the user namespace to avoid
  // culebra's cross-fn shadow check) and stores them under the
  // original parameter names.
  std::string ctor_params;
  std::string ctor_call_args;
  std::string ctor_inits =
      "      this._g_drained = false\n"
      "      this._g_has_la = false\n"
      "      this._g_la = nil\n"
      "      this._g_inited = false\n"
      "      this._g_disposed = false\n";
  emit_ctor_param_and_local_inits(param_names, locals,
                                  ctor_params, ctor_call_args, ctor_inits);

  // Rewrite each body fragment to use `this.X` for params + locals.
  // pre_loop walks statements one by one so DEFER nodes can be lifted
  // into the dispose method directly (inlined, not wrapped in closures
  // — JIT can't capture `this` through a method-local closure today).
  std::string pre_loop_src;
  std::string dispose_body_src;
  split_pre_loop_stmts_and_defers(
      p.pre_loop, src, src_len, rewrite_set,
      pre_loop_src, dispose_body_src);
  auto pre_yield_src = rewrite_locals_to_this(
      ast_source_range(p.pre_yield, src, src_len), rewrite_set);
  auto post_yield_src = rewrite_locals_to_this(
      ast_source_range(p.post_yield, src, src_len), rewrite_set);
  auto cond_src = rewrite_locals_to_this(
      ast_source_slice(*p.loop_cond, src, src_len), rewrite_set);

  // Yield-bearing stmt: substitute each yield first (positions valid),
  // then rewrite locals to `this.X`.
  auto yield_stmt_sv = ast_source_slice(*p.yield_stmt, src, src_len);
  auto yield_stmt_substituted = substitute_yields_in_stmt(
      yield_stmt_sv, p.yield_stmt->position, yields, src, src_len);
  auto yield_stmt_src = rewrite_locals_to_this(yield_stmt_substituted,
                                                rewrite_set);

  auto synthesized = std::make_shared<std::string>(std::format(
      "fn __gen_wrapper__() {{\n"
      "  class {0} {{\n"
      "    new({1}) {{\n{2}    }}\n"
      "    iter() {{ this }}\n"
      "    has_next() {{\n"
      "      if this._g_drained {{ return false }}\n"
      "      if this._g_has_la {{ return true }}\n"
      "      if this._g_inited {{\n"
      "{3}\n"
      "      }} else {{\n"
      "{4}\n"
      "        this._g_inited = true\n"
      "      }}\n"
      "      while {5} {{\n"
      "{6}\n"
      "        {7}\n"
      "{8}\n"
      "      }}\n"
      "      this._g_drained = true\n"
      "      return false\n"
      "    }}\n"
      "    next() {{\n"
      "      if !this._g_has_la {{ this.has_next() }}\n"
      "      let _v = this._g_la\n"
      "      this._g_la = nil\n"
      "      this._g_has_la = false\n"
      "      return _v\n"
      "    }}\n"
      "    dispose() {{\n"
      "      if this._g_disposed {{ return nil }}\n"
      "      this._g_disposed = true\n"
      "      this._g_drained = true\n"
      "      if this._g_inited {{\n"
      "{10}"
      "      }}\n"
      "    }}\n"
      "  }}\n"
      "  {0}.new({9})\n"
      "}}\n",
      gen_name, ctor_params, ctor_inits,
      post_yield_src, pre_loop_src,
      cond_src, pre_yield_src, yield_stmt_src, post_yield_src,
      ctor_call_args, dispose_body_src));
  swap_body_from_wrapper(ast, synthesized);
  return ast;
}

// --- Stage 6b: yield from via phase machine ------------------------------

// WHILE body acceptable by Stage 6b: zero or more leading statements
// (typically `let x = _g_it.next()` after Stage 5 desugar) followed by
// a single YIELD or YIELD_FROM as the final stmt. Bodies with multiple
// yields, branches, or yield-from nested deeper than top level are
// rejected — Stage 6b falls back to the dispatcher's error path so the
// user gets a clean SyntaxError instead of silent miscompilation.
struct While6bBody {
  std::vector<const peg::Ast*> pre_stmts;
  const peg::Ast* yield_node;  // YIELD or YIELD_FROM
  bool is_yield_from;
};

inline std::optional<While6bBody> analyze_while_body_for_stage6b(
    const peg::Ast& while_node) {
  using namespace peg::udl;
  if (while_node.nodes.size() < 2) return std::nullopt;
  const auto& body = *while_node.nodes[1];
  auto stmts = body_stmts(body);
  While6bBody result;
  for (size_t i = 0; i < stmts.size(); i++) {
    auto* u = unwrap_stmt(stmts[i]);
    if (u->tag == "YIELD"_ || u->tag == "YIELD_FROM"_) {
      if (i + 1 != stmts.size()) return std::nullopt;
      result.yield_node = u;
      result.is_yield_from = (u->tag == "YIELD_FROM"_);
      return result;
    }
    // Reject nested yields inside non-terminal stmts.
    if (fn_body_has_yield(*stmts[i])) return std::nullopt;
    result.pre_stmts.push_back(stmts[i]);
  }
  return std::nullopt;
}

// Emit the body of a single phase block of has_next(), with locals
// rewritten via `rewrite_locals_to_this`. Each phase ends with either
// `return true` (yielded a value), `continue` (state advanced, restart
// the dispatch loop), or `return false` (drained).
inline std::string emit_stage6b_phase_block(
    int next_phase, const peg::Ast& stmt,
    const char* src, size_t src_len,
    const std::set<std::string>& rewrite_set) {
  using namespace peg::udl;
  auto* u = unwrap_stmt(&stmt);
  if (u->tag == "YIELD"_ && !u->nodes.empty()) {
    auto expr = rewrite_locals_to_this(
        ast_source_slice(*u->nodes[0], src, src_len), rewrite_set);
    return std::format(
        "        this._g_la = ({0})\n"
        "        this._g_has_la = true\n"
        "        this._g_phase = {1}\n"
        "        return true\n",
        expr, next_phase);
  }
  if (u->tag == "YIELD_FROM"_ && !u->nodes.empty()) {
    auto expr = rewrite_locals_to_this(
        ast_source_slice(*u->nodes[0], src, src_len), rewrite_set);
    return std::format(
        "        this._g_delegate = ({0}).iter()\n"
        "        this._g_phase = {1}\n"
        "        continue\n",
        expr, next_phase);
  }
  if (u->tag == "WHILE"_) {
    auto wb = analyze_while_body_for_stage6b(*u);
    if (!wb) return {};
    auto cond = rewrite_locals_to_this(
        ast_source_slice(*u->nodes[0], src, src_len), rewrite_set);
    std::string pre;
    for (auto* s : wb->pre_stmts) {
      pre += "          ";
      pre += rewrite_locals_to_this(
          ast_source_slice(*s, src, src_len), rewrite_set);
      pre += "\n";
    }
    auto yexpr = rewrite_locals_to_this(
        ast_source_slice(*wb->yield_node->nodes[0], src, src_len), rewrite_set);
    std::string yield_part;
    if (wb->is_yield_from) {
      yield_part = std::format(
          "          this._g_delegate = ({0}).iter()\n"
          "          continue\n",
          yexpr);
    } else {
      yield_part = std::format(
          "          this._g_la = ({0})\n"
          "          this._g_has_la = true\n"
          "          return true\n",
          yexpr);
    }
    return std::format(
        "        if {0} {{\n"
        "{1}"
        "{2}"
        "        }} else {{\n"
        "          this._g_phase = {3}\n"
        "          continue\n"
        "        }}\n",
        cond, pre, yield_part, next_phase);
  }
  // Plain statement: execute verbatim (with locals rewritten) and advance.
  auto stmt_rw = rewrite_locals_to_this(
      ast_source_slice(stmt, src, src_len), rewrite_set);
  return std::format(
      "        {0}\n"
      "        this._g_phase = {1}\n"
      "        continue\n",
      stmt_rw, next_phase);
}

inline std::shared_ptr<peg::Ast> transform_one_generator_fn_stage6b(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len,
    const peg::Ast& name_ast,
    const peg::Ast& params_ast) {
  using namespace peg::udl;
  auto gen_name = std::format("_Gen_{}_{}_{}",
                              std::string(name_ast.token),
                              name_ast.line, name_ast.column);

  std::vector<std::string_view> param_names;
  for (const auto& pn : params_ast.nodes) {
    if (is_kw_only_sep(*pn) || is_kwargs_rest(*pn)) continue;
    param_names.push_back(view_parameter(*pn).name);
  }
  auto locals = collect_local_names(*ast->nodes.back());
  std::set<std::string> rewrite_set = locals;
  for (auto& pn : param_names) rewrite_set.insert(std::string(pn));

  std::string ctor_params;
  std::string ctor_call_args;
  std::string ctor_inits =
      "      this._g_drained = false\n"
      "      this._g_has_la = false\n"
      "      this._g_la = nil\n"
      "      this._g_disposed = false\n"
      "      this._g_delegate = nil\n"
      "      this._g_phase = 0\n";
  emit_ctor_param_and_local_inits(param_names, locals,
                                  ctor_params, ctor_call_args, ctor_inits);

  // Walk top-level body statements and build phase blocks. Last phase
  // is the synthetic "drained" terminator so each real phase can hand
  // off to its successor with a simple ++index.
  auto top_stmts = body_stmts(*ast->nodes.back());
  std::string phases_src;
  int phase_idx = 0;
  for (auto* stmt : top_stmts) {
    auto block = emit_stage6b_phase_block(phase_idx + 1,
                                          *stmt, src, src_len, rewrite_set);
    if (block.empty()) return ast;  // unsupported shape — caller will report
    phases_src += std::format("      if this._g_phase == {} {{\n{}      }}\n",
                              phase_idx, block);
    phase_idx++;
  }
  // Terminator phase.
  phases_src += std::format(
      "      if this._g_phase == {} {{\n"
      "        this._g_drained = true\n"
      "        return false\n"
      "      }}\n",
      phase_idx);

  auto synthesized = std::make_shared<std::string>(std::format(
      "fn __gen_wrapper__() {{\n"
      "  class {0} {{\n"
      "    new({1}) {{\n{2}    }}\n"
      "    iter() {{ this }}\n"
      "    has_next() {{\n"
      "      while true {{\n"
      "        if this._g_drained {{ return false }}\n"
      "        if this._g_has_la {{ return true }}\n"
      "        if this._g_delegate != nil {{\n"
      "          if this._g_delegate.has_next() {{\n"
      "            this._g_la = this._g_delegate.next()\n"
      "            this._g_has_la = true\n"
      "            return true\n"
      "          }}\n"
      "          if this._g_delegate.has('dispose') {{ this._g_delegate.dispose() }}\n"
      "          this._g_delegate = nil\n"
      "        }}\n"
      "{3}"
      "      }}\n"
      "    }}\n"
      "    next() {{\n"
      "      if !this._g_has_la {{ this.has_next() }}\n"
      "      let _v = this._g_la\n"
      "      this._g_la = nil\n"
      "      this._g_has_la = false\n"
      "      return _v\n"
      "    }}\n"
      "    dispose() {{\n"
      "      if this._g_disposed {{ return nil }}\n"
      "      this._g_disposed = true\n"
      "      this._g_drained = true\n"
      "      if this._g_delegate != nil {{\n"
      "        if this._g_delegate.has('dispose') {{ this._g_delegate.dispose() }}\n"
      "        this._g_delegate = nil\n"
      "      }}\n"
      "    }}\n"
      "  }}\n"
      "  {0}.new({4})\n"
      "}}\n",
      gen_name, ctor_params, ctor_inits, phases_src, ctor_call_args));
  swap_body_from_wrapper(ast, synthesized);
  return ast;
}

// Dispatcher: pattern-match the body and route to the right stage.
// Stage 5 runs first as a source-level pre-pass that rewrites yielding
// `for-in` loops into `while + iterator` form, then Stage 3 handles the
// result. Stage 6b takes any body containing `yield from` and emits a
// phase machine instead. Stage 3 (verbatim while-preserve) is the
// primary backend for plain yield generators; Stage 1's eager-eval path
// catches straight-line bodies whose yield expressions don't reference
// loop-mutated locals.
inline std::shared_ptr<peg::Ast> transform_one_generator_fn(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  using namespace peg::udl;
  size_t i = 0;
  while (i < ast->nodes.size() && ast->nodes[i]->tag == "DECORATOR"_) i++;
  if (i + 2 >= ast->nodes.size()) return ast;
  auto yields = collect_yields(*ast->nodes.back());
  bool has_yf = body_has_yield_from(*ast->nodes.back());
  if (yields.empty() && !has_yf) return ast;

  // C# rule (CS1626): a yield statement may not appear inside a try-catch
  // or defer block. The state-machine cost of supporting yield-spanning
  // try blocks is permanently out of scope for culebra ([[generator-design]]
  // §仕様凍結項目). Cleanup belongs in `defer { ... }` at body top level;
  // value-level error recovery belongs in the yielded expression
  // (`yield try { ... } catch e { ... }`).
  if (auto* bad = find_yield_inside_try_or_defer(*ast->nodes.back())) {
    throw CulebraError(
        "SyntaxError",
        "yield cannot appear inside a try-catch or defer block. Move "
        "the try to the yielded expression value (yield try { ... } "
        "catch e { ... }) or use a top-level `defer { ... }` for cleanup.",
        static_cast<long>(bad->line),
        static_cast<long>(bad->column));
  }

  // Stage 5: rewrite yielding for-in loops to while + iterator at source
  // level, then re-parse so Stage 3 picks them up.
  if (auto rewritten = rewrite_yielding_fors_to_while(
          *ast->nodes.back(), src, src_len)) {
    auto params_sv = ast_source_slice(*ast->nodes[i + 1], src, src_len);
    auto body_inner = strip_block_braces(*rewritten);
    auto desugared = std::make_shared<std::string>(std::format(
        "fn __gen_wrapper__{} {{\n{}\n}}\n",
        std::string(params_sv), std::string(body_inner)));
    if (!swap_body_with_wrapper_params(ast, desugared, i)) return ast;
    src = desugared->data();
    src_len = desugared->size();
    yields = collect_yields(*ast->nodes.back());
    has_yf = body_has_yield_from(*ast->nodes.back());
    if (yields.empty() && !has_yf) return ast;
  }

  // Stage 6a defer scope: only top-level `defer { ... }` in the body is
  // supported (lifted into the generator's defer registry at dispose
  // time). Defers buried in loop / if / match bodies have no clean
  // translation today — surface as a SyntaxError instead of silently
  // letting the existing scope-local defer mechanism mis-fire on yield.
  auto all_defers = collect_defers(*ast->nodes.back());
  const auto& name_ast = *ast->nodes[i];
  const auto& params_ast = *ast->nodes[i + 1];

  // Stage 6b: any body containing `yield from` goes through the phase
  // machine codegen instead of Stage 3's verbatim while-preserve. The
  // transformer leaves the body untouched when it can't pattern-match
  // the shape; surface that as a SyntaxError so the user gets a clear
  // failure mode rather than silent fallthrough to Stage 1.
  if (has_yf) {
    auto orig_body = ast->nodes.back();
    auto out = transform_one_generator_fn_stage6b(ast, src, src_len,
                                                   name_ast, params_ast);
    if (out->nodes.back().get() == orig_body.get()) {
      throw CulebraError(
          "SyntaxError",
          "yield from is only supported in the Stage 6b minimum scope: "
          "linear sequence of yield / yield from at body top level, "
          "optionally with a single trailing for-in / while whose body "
          "is a single yield or yield from. Nested / branching "
          "delegation is not yet supported.",
          static_cast<long>(name_ast.line),
          static_cast<long>(name_ast.column));
    }
    return out;
  }

  if (auto p = match_stage3_pattern(*ast->nodes.back(), yields)) {
    if (!all_defers.empty()) {
      std::set<const peg::Ast*> pre_loop_defers;
      for (auto* stmt : p->pre_loop) {
        auto* unwrapped = unwrap_stmt(stmt);
        if (unwrapped->tag == "DEFER"_) pre_loop_defers.insert(unwrapped);
      }
      for (auto* d : all_defers) {
        if (!pre_loop_defers.contains(d)) {
          throw CulebraError(
              "SyntaxError",
              "defer in generator must be at the body's top level "
              "(before the loop). Defers nested inside loop / if / match "
              "bodies are not supported.",
              static_cast<long>(d->line),
              static_cast<long>(d->column));
        }
      }
    }
    return transform_one_generator_fn_stage3(ast, src, src_len, name_ast,
                                             params_ast, *p, yields);
  }
  if (!all_defers.empty()) {
    auto* d = all_defers.front();
    throw CulebraError(
        "SyntaxError",
        "defer in generator requires a loop body (while or for-in). "
        "Straight-line generators have no drain point to attach the "
        "cleanup to.",
        static_cast<long>(d->line),
        static_cast<long>(d->column));
  }
  return transform_one_generator_fn_stage1(ast, src, src_len, name_ast, yields);
}

// Walk the AST, transforming every yield-carrying MULTIFN_DECL. The
// walk visits every node — yield-free modules pay one whole-tree
// pointer pass (dwarfed by the PEG parse already run).
inline std::shared_ptr<peg::Ast> transform_generators_in(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  using namespace peg::udl;
  for (auto& child : ast->nodes) {
    child = transform_generators_in(child, src, src_len);
  }
  if (ast->tag == "MULTIFN_DECL"_) {
    auto& body = ast->nodes.back();
    if (fn_body_has_yield(*body)) {
      return transform_one_generator_fn(ast, src, src_len);
    }
  }
  return ast;
}

// Public parse entry: identical to `parse()` plus the generator
// transformation pass. Every caller that wants `yield` support
// (interp module load, JIT/AOT, REPL, lazy module loader) routes
// through here.
inline std::shared_ptr<peg::Ast> parse_with_transforms(
    const std::string& path, const char* expr, size_t len,
    std::vector<std::string>& msgs) {
  auto ast = parse(path, expr, len, msgs);
  if (!ast) return ast;
  return transform_generators_in(ast, expr, len);
}

}  // namespace culebra
