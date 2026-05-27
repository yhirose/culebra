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

#include <format>
#include <functional>
#include <memory>
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

// Stage 1 transformation: rewrite a MULTIFN_DECL with N yields in its
// (straight-line) body into a state-machine dispatch over phases
// 0..N. Each `yield expr_i` becomes a `phase == i` arm that bumps
// phase and returns the stored value; `has_next()` reports `phase != N`.
//
// Stage 1 limits (relaxed by later stages):
//   - body is straight-line (no while/for/if/match/try around yield)
//   - yield expressions depend only on params / literals — they're
//     evaluated EAGERLY in the factory and the values passed through
//     the ctor as positional args `_y0, _y1, ...`. Stage 2 introduces
//     all-locals-on-heap + lazy per-arm evaluation when locals or
//     loops enter the picture.
//
// The class lives inside the outer fn so it can be looked up at the
// factory call site; we keep the synthesized identifiers (`_y0`, ...)
// off the user's namespace so culebra's cross-fn shadow check doesn't
// flag them against any outer-scope name.
inline std::shared_ptr<peg::Ast> transform_one_generator_fn(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  using namespace peg::udl;
  // MULTIFN_DECL: [DECORATOR*, IDENTIFIER, PARAMETERS, (RETURN_TYPE)?, BLOCK]
  size_t i = 0;
  while (i < ast->nodes.size() && ast->nodes[i]->tag == "DECORATOR"_) i++;
  if (i + 2 >= ast->nodes.size()) return ast;
  const auto& name_ast = *ast->nodes[i];
  auto& body_slot = ast->nodes.back();
  auto yields = collect_yields(*body_slot);
  if (yields.empty()) return ast;

  auto gen_name = std::format("_Gen_{}_{}_{}",
                              std::string(name_ast.token),
                              name_ast.line, name_ast.column);

  // Build ctor signature `(_y0, _y1, ...)`, init body `this._y0 = _y0; ...`,
  // factory call args `expr0, expr1, ...`, and per-phase dispatch arms.
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
  generator_transform_sources().push_back(synthesized);
  std::vector<std::string> msgs;
  auto sub_ast = parse("<generator-transform>", synthesized->data(),
                       synthesized->size(), msgs);
  if (!sub_ast) return ast;
  std::shared_ptr<peg::Ast> wrapper_fn;
  std::function<void(std::shared_ptr<peg::Ast>)> walk =
      [&](std::shared_ptr<peg::Ast> n) {
        if (wrapper_fn) return;
        if (n->tag == "MULTIFN_DECL"_) { wrapper_fn = n; return; }
        for (auto& c : n->nodes) walk(c);
      };
  walk(sub_ast);
  if (!wrapper_fn) return ast;
  body_slot = wrapper_fn->nodes.back();
  return ast;
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
