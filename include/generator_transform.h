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

// True if `node` carries at least one YIELD anywhere inside, stopping
// at fn boundaries (see `is_fn_boundary`).
inline bool fn_body_has_yield(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "YIELD"_) return true;
  if (is_fn_boundary(node.tag)) return false;
  for (auto& c : node.nodes) {
    if (fn_body_has_yield(*c)) return true;
  }
  return false;
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

// --- Transformation entry points -----------------------------------------

// Replace `ast->nodes.back()` (the original body BLOCK) with the BLOCK
// from a freshly-parsed `fn __gen_wrapper__() { ... }` source fragment.
// Shared by Stage 1 and Stage 2 — both end with "now swap the body".
inline bool swap_body_from_wrapper(std::shared_ptr<peg::Ast> ast,
                                   std::shared_ptr<std::string> synthesized) {
  using namespace peg::udl;
  generator_transform_sources().push_back(synthesized);
  std::vector<std::string> msgs;
  auto sub_ast = parse("<generator-transform>", synthesized->data(),
                       synthesized->size(), msgs);
  if (!sub_ast) return false;
  std::shared_ptr<peg::Ast> wrapper_fn;
  std::function<void(std::shared_ptr<peg::Ast>)> walk =
      [&](std::shared_ptr<peg::Ast> n) {
        if (wrapper_fn) return;
        if (n->tag == "MULTIFN_DECL"_) { wrapper_fn = n; return; }
        for (auto& c : n->nodes) walk(c);
      };
  walk(sub_ast);
  if (!wrapper_fn) return false;
  ast->nodes.back() = wrapper_fn->nodes.back();
  return true;
}

// Stage 1: yield expressions are eager-evaluated in the factory and
// passed through positional ctor args `_y0, _y1, ...`. Works for any
// body whose yields depend only on params/literals — see
// [[project-generator-design]] §Stage 1.
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
      "      this._g_inited = false\n";
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

  // Rewrite each body fragment to use `this.X` for params + locals.
  auto pre_loop_src = rewrite_locals_to_this(
      ast_source_range(p.pre_loop, src, src_len), rewrite_set);
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
      "  }}\n"
      "  {0}.new({9})\n"
      "}}\n",
      gen_name, ctor_params, ctor_inits,
      post_yield_src, pre_loop_src,
      cond_src, pre_yield_src, yield_stmt_src, post_yield_src,
      ctor_call_args));
  swap_body_from_wrapper(ast, synthesized);
  return ast;
}

// Dispatcher: pattern-match the body and route to the right stage.
// Stage 3 gets priority — its pattern is a superset of Stage 2's (which
// has been folded in) and handles any while-bound generator whose
// yields all live in one statement. Falls back to Stage 1's eager-eval
// path for straight-line bodies whose yield expressions don't reference
// loop-mutated locals.
inline std::shared_ptr<peg::Ast> transform_one_generator_fn(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  using namespace peg::udl;
  size_t i = 0;
  while (i < ast->nodes.size() && ast->nodes[i]->tag == "DECORATOR"_) i++;
  if (i + 2 >= ast->nodes.size()) return ast;
  const auto& name_ast = *ast->nodes[i];
  const auto& params_ast = *ast->nodes[i + 1];
  auto& body_slot = ast->nodes.back();
  auto yields = collect_yields(*body_slot);
  if (yields.empty()) return ast;
  if (auto p = match_stage3_pattern(*body_slot, yields)) {
    return transform_one_generator_fn_stage3(ast, src, src_len, name_ast,
                                             params_ast, *p, yields);
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
