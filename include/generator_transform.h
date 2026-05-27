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
// dispatch over phases.
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

// --- Stage 2 pattern: pre-loop init + while { pre-yield, yield, post-yield }
// (single while, single yield, yield must be inside the while). Returns
// the structured pieces or nullopt if the body doesn't match.

struct Stage2Pattern {
  std::vector<const peg::Ast*> pre_loop;
  const peg::Ast* loop_cond;
  std::vector<const peg::Ast*> pre_yield;
  const peg::Ast* yield_expr;
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

inline std::optional<Stage2Pattern> match_stage2_pattern(
    const peg::Ast& body, const std::vector<const peg::Ast*>& yields) {
  using namespace peg::udl;
  if (yields.size() != 1) return std::nullopt;
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
  // The yield must be inside the loop body, not in pre-loop stmts.
  // Cheapest check: the yield's position is within the while body's range.
  const peg::Ast* loop_body = while_node->nodes[1].get();
  if (yields[0]->position < loop_body->position ||
      yields[0]->position >= loop_body->position + loop_body->length) {
    return std::nullopt;
  }
  Stage2Pattern p;
  p.loop_cond = while_node->nodes[0].get();
  for (size_t i = 0; i < while_idx; i++) p.pre_loop.push_back(stmts[i]);
  // Reject anything after the while too (Stage 2 keeps the loop terminal).
  if (while_idx + 1 != stmts.size()) return std::nullopt;
  auto inner = body_stmts(*loop_body);
  size_t yidx = inner.size();
  for (size_t i = 0; i < inner.size(); i++) {
    if (unwrap_stmt(inner[i])->tag == "YIELD"_) {
      yidx = i;
      p.yield_expr = unwrap_stmt(inner[i])->nodes[0].get();
      break;
    }
  }
  if (yidx == inner.size()) return std::nullopt;
  for (size_t i = 0; i < yidx; i++) p.pre_yield.push_back(inner[i]);
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

// Stage 2: while-loop with single yield, all-locals-on-heap state
// machine. `has_next()` carries the work — it runs pre-loop init on
// the first call, post-yield code on subsequent calls, then the loop
// cond + pre-yield code; if the iteration produces a value, it
// stashes it in a lookahead slot and returns true; if the cond fails
// it marks drained and returns false. `next()` consumes the stash.
// This mirrors Phase 2's lookahead-cache pattern and keeps has_next
// idempotent + accurate (calling has_next twice returns the same
// result without advancing the generator). See
// [[project-generator-design]] §Stage 2 設計詳細.
inline std::shared_ptr<peg::Ast> transform_one_generator_fn_stage2(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len,
    const peg::Ast& name_ast,
    const peg::Ast& params_ast,
    const Stage2Pattern& p) {
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
  auto yield_src = rewrite_locals_to_this(
      ast_source_slice(*p.yield_expr, src, src_len), rewrite_set);

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
      "      if {5} {{\n"
      "{6}\n"
      "        this._g_la = {7}\n"
      "        this._g_has_la = true\n"
      "        return true\n"
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
      "  {0}.new({8})\n"
      "}}\n",
      gen_name, ctor_params, ctor_inits,
      post_yield_src, pre_loop_src,
      cond_src, pre_yield_src, yield_src, ctor_call_args));
  swap_body_from_wrapper(ast, synthesized);
  return ast;
}

// Dispatcher: pattern-match the body and route to the right stage.
// Stage 2 gets priority because its body would parse as a 1-yield
// case that Stage 1 mishandles (eager eval of yield exprs that
// reference loop locals would NameError at the factory call site).
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
  if (auto p = match_stage2_pattern(*body_slot, yields)) {
    return transform_one_generator_fn_stage2(ast, src, src_len, name_ast,
                                             params_ast, *p);
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
