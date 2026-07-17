// Algebraic-effects AST transformation pass.
//
// `effect fn` / `perform` / `handle … with` are rewritten, at parse time,
// into plain culebra source (synthesized classes + calls into the `__Eff`
// runtime preamble), then re-parsed — the interp / JIT / AOT backends run
// the lowered code with no effects-specific support of their own, so the
// three backends stay symmetric for free (see [[project-algebraic-effects]],
// [[feedback-check-jit-interp-symmetry]]).
//
// Design (dynamic scope, one-shot resume):
//   * An `effect fn f(...) { BODY }` (with body) lowers to a normal fn that
//     RETURNS a *computation object* — a flat-dispatch state machine whose
//     `_step(rv)` runs the body until the next suspension point, then hands
//     control back to the driver. A signature-only `effect fn op(...)`
//     declares an operation and lowers to a throwing stub.
//   * A suspension point is a statement-level `perform op(args)` (SUSPEND) or
//     a statement-level call to another effect fn (DELEGATE). The body may use
//     arbitrary control flow — if / while / for (desugared to while) with
//     break / continue / return — lowered by a flat-dispatch CPS builder (the
//     same shape the generator transform uses; see `build_dispatch`). An
//     expression-nested `perform` is first hoisted to statement level by an
//     A-normalization pre-pass (`anf_program`), so the CPS layer only ever
//     sees statement-level suspensions. A `perform` in a control-flow
//     condition / iterable, or a nested `handle` that captures an enclosing
//     binding, is rejected symmetrically.
//   * `handle { BODY } with op(params, resume) { H }` lowers to
//     `__Eff.handle(<BODY as a computation>, "op", <handler adapter>)`. The
//     driver (`__Eff.drive`, in src/preambles/effects.cul) walks the
//     dynamically-scoped handler stack; `resume` is the one-shot continuation
//     (an ordinary RC value, so leak-safety is inherited from the generator
//     machinery it mirrors — [[project-rc-gc-correct-model]]). Handles may
//     nest; each computation's `_step` resume parameter is uniquely named so an
//     inner computation doesn't shadow an enclosing one.
//
// Reuses the generator transform's source-slice / local-rewrite helpers
// (generator_transform.h): everything here is the same "splice verbatim
// source between suspension points, rebuild only the seams" recipe.

#pragma once

#include "generator_transform.h"
#include "parser.h"

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace culebra {

// Return-tag protocol shared with the `__Eff` driver (effects.cul):
//   0 = DONE     — `this._eff_val` holds the computation's result
//   1 = SUSPEND  — `this._eff_op` / `this._eff_args` describe a perform
//   2 = DELEGATE — `this._eff_delegate` is a sub-computation to drive
inline constexpr int EFF_DONE = 0;
inline constexpr int EFF_SUSPEND = 1;
inline constexpr int EFF_DELEGATE = 2;

// --- EFFECT_FN_DECL shape ------------------------------------------------
// Grammar: effect _ fn _ CLASS_HEAD _ PARAMETERS (_ RETURN_TYPE)? (_ BLOCK)?
// Children: [CLASS_HEAD, PARAMETERS, (RETURN_TYPE)?, (BODY)?]. The AstOptimizer
// collapses a single-statement BLOCK onto its statement, so the body may
// surface as STATEMENTS or a bare expression, not literally a BLOCK. RETURN_TYPE
// is keep-listed and keeps its tag, so a body is present iff there is a child
// past PARAMETERS whose tag is not RETURN_TYPE.
inline bool effect_fn_has_body(const peg::Ast& decl) {
  using namespace peg::udl;
  return decl.nodes.size() > 2 && decl.nodes.back()->tag != "RETURN_TYPE"_;
}

// The declared name — CLASS_HEAD's token, trimmed of any generic head
// (`<...>`) the thin slice never emits but the grammar admits.
inline std::string effect_fn_name(const peg::Ast& decl) {
  auto tok = std::string(decl.nodes[0]->token);
  auto lt = tok.find('<');
  if (lt != std::string::npos) tok = tok.substr(0, lt);
  // trim trailing whitespace a generic head may have left
  while (!tok.empty() && (tok.back() == ' ' || tok.back() == '\t')) tok.pop_back();
  return tok;
}

// Names of every effect fn WITH a body reachable in the program (the
// "colored" functions whose direct calls are suspension points). Operations
// (signature-only) are invoked via `perform`, never called directly, so they
// are excluded.
inline std::set<std::string> collect_effect_fn_names(const peg::Ast& root) {
  using namespace peg::udl;
  std::set<std::string> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (n.tag == "EFFECT_FN_DECL"_ && effect_fn_has_body(n)) {
      out.insert(effect_fn_name(n));
    }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(root);
  return out;
}

// A statement-level suspension point discovered by the splitter.
struct EffSuspension {
  enum Kind { Perform, Delegate } kind;
  std::string target;      // local bound to the result (empty = discarded)
  bool binds = false;      // true when the statement was `let target = …`
  std::string op;          // Perform: operation name
  std::string args_array;  // Perform: `[a, b, …]` source (rewritten)
  std::string call_src;    // Delegate: the effect-fn call source (rewritten)
};

// Classification of a leaf body statement: a statement-level suspension
// (`perform` / effect-fn call, possibly bound by `let`) or a plain Verbatim
// statement. Reached only after A-normalization, so a suspension still hidden
// in an expression is rejected — keeping interp and JIT honestly in step.
struct EffStmtClass {
  enum Kind { Verbatim, Suspend } kind;
  EffSuspension susp;  // when kind == Suspend
};

class EffectsLowerer {
 public:
  EffectsLowerer(const char* src, size_t src_len,
                 const std::set<std::string>& effect_fns)
      : src_(src), src_len_(src_len), effect_fns_(effect_fns) {}

  // --- entry: rebuild the tree, lowering effect constructs -------------
  std::shared_ptr<peg::Ast> transform(std::shared_ptr<peg::Ast> ast) {
    using namespace peg::udl;
    if (ast->tag == "EFFECT_FN_DECL"_) return lower_effect_fn_decl(ast);
    if (ast->tag == "HANDLE"_) return lower_handle(ast);
    if (ast->tag == "PERFORM"_) {
      // Reached through the generic walk => not consumed by an effect fn /
      // handle body, i.e. a `perform` in ordinary (non-effect) code.
      throw CulebraError(
          "SyntaxError",
          "`perform` may only appear inside an `effect fn` body or a "
          "`handle` block.",
          static_cast<long>(ast->line), static_cast<long>(ast->column));
    }
    for (auto& child : ast->nodes) child = transform(child);
    return ast;
  }

 private:
  const char* src_;
  size_t src_len_;
  const std::set<std::string>& effect_fns_;

  std::string_view slice(const peg::Ast& n) const {
    return ast_source_slice(n, src_, src_len_);
  }

  // Detect `f(args)` where f is a known effect fn (a Delegate suspension).
  // Requires the plain call shape [IDENTIFIER, ARGUMENTS]; a method chain or
  // indexed callee is left verbatim (and, if it hides a perform, rejected
  // elsewhere).
  bool is_effect_call(const peg::Ast& n) const {
    using namespace peg::udl;
    // A plain `name(args)` call is a CALL with exactly two children: the
    // callee IDENTIFIER and its argument list (ARGUMENTS collapses to ARG_LIST
    // after optimization). A method chain / indexed callee has more children.
    if (n.tag != "CALL"_ || n.nodes.size() != 2) return false;
    if (n.nodes[0]->tag != "IDENTIFIER"_) return false;
    return effect_fns_.count(std::string(n.nodes[0]->token)) > 0;
  }

  // True if a perform/effect-call is reachable inside `n` (used to reject
  // suspensions the thin slice can't lower — e.g. buried in an if/while body
  // or a compound expression). Stops at fn boundaries and at HANDLE (a nested
  // handle re-establishes its own effect scope).
  bool has_suspension(const peg::Ast& n) const {
    using namespace peg::udl;
    if (is_fn_boundary(n.tag) || n.tag == "HANDLE"_) return false;
    if (n.tag == "PERFORM"_) return true;
    if (is_effect_call(n)) return true;
    for (auto& c : n.nodes)
      if (has_suspension(*c)) return true;
    return false;
  }

  // Build the `[a, b, …]` array source for a perform's arguments, rewriting
  // locals to `this.` per `rewrite`. Rejects kwargs / `**` splat (thin slice).
  std::string perform_args_array(const peg::Ast& arguments,
                                 const std::set<std::string>& rewrite) const {
    using namespace peg::udl;
    std::string out = "[";
    for (size_t i = 0; i < arguments.nodes.size(); i++) {
      const auto& item = *arguments.nodes[i];
      if (item.tag == "KWARG"_ || item.tag == "KWARG_SPLAT"_) {
        throw CulebraError(
            "SyntaxError",
            "keyword / splat arguments are not supported in `perform` yet.",
            static_cast<long>(item.line), static_cast<long>(item.column));
      }
      if (i > 0) out += ", ";
      out += rewrite_locals_to_this(slice(item), rewrite);
    }
    out += "]";
    return out;
  }

  // Classify a body statement. `rewrite` is the set of names rewritten to
  // `this.` (params + body locals).
  EffStmtClass classify(const peg::Ast* stmt,
                        const std::set<std::string>& rewrite) const {
    using namespace peg::udl;
    const auto* s = unwrap_stmt(stmt);

    // `let x = perform …` / `let x = effectfn(…)`
    if (s->tag == "ASSIGNMENT"_) {
      auto av = view_assignment(*s);
      const peg::Ast* rhs = av.rhs;
      bool bind = av.is_let && av.lvalcnt == 1 && !av.compound;
      std::string target;
      if (bind) {
        const auto& lval = *s->nodes[av.lvaloff];
        if (lval.tag == "IDENTIFIER"_) target = std::string(lval.token);
        else bind = false;
      }
      if (rhs->tag == "PERFORM"_) {
        if (!bind) {
          throw CulebraError(
              "SyntaxError",
              "`perform` can only bind to a fresh `let x = perform …` in this "
              "slice (no compound / destructuring / re-assignment targets).",
              static_cast<long>(s->line), static_cast<long>(s->column));
        }
        return EffStmtClass{EffStmtClass::Suspend,
                            make_perform(*rhs, target, /*binds=*/true, rewrite)};
      }
      if (is_effect_call(*rhs)) {
        if (!bind) {
          throw CulebraError(
              "SyntaxError",
              "an effect-fn call can only bind to a fresh `let x = f(…)` in "
              "this slice.",
              static_cast<long>(s->line), static_cast<long>(s->column));
        }
        return EffStmtClass{EffStmtClass::Suspend,
                            make_delegate(*rhs, target, /*binds=*/true, rewrite)};
      }
      // Plain assignment: verbatim, but must not hide a suspension.
      if (has_suspension(*s)) reject_hidden(*s);
      return EffStmtClass{EffStmtClass::Verbatim, {}};
    }

    // bare `perform …`
    if (s->tag == "PERFORM"_) {
      return EffStmtClass{EffStmtClass::Suspend,
                          make_perform(*s, "", /*binds=*/false, rewrite)};
    }
    // bare `effectfn(…)`
    if (is_effect_call(*s)) {
      return EffStmtClass{EffStmtClass::Suspend,
                          make_delegate(*s, "", /*binds=*/false, rewrite)};
    }

    // Any other statement is verbatim — as long as no suspension hides in it.
    if (has_suspension(*s)) reject_hidden(*s);
    return EffStmtClass{EffStmtClass::Verbatim, {}};
  }

  void reject_hidden(const peg::Ast& s) const {
    throw CulebraError(
        "SyntaxError",
        "a `perform` / effect-fn call is only supported at statement level "
        "here — bind it first (`let x = perform …`).",
        static_cast<long>(s.line), static_cast<long>(s.column));
  }

  EffSuspension make_perform(const peg::Ast& perform, std::string target,
                             bool binds,
                             const std::set<std::string>& rewrite) const {
    EffSuspension su;
    su.kind = EffSuspension::Perform;
    su.target = std::move(target);
    su.binds = binds;
    su.op = std::string(perform.nodes[0]->token);
    su.args_array = perform_args_array(*perform.nodes[1], rewrite);
    return su;
  }

  EffSuspension make_delegate(const peg::Ast& call, std::string target,
                              bool binds,
                              const std::set<std::string>& rewrite) const {
    EffSuspension su;
    su.kind = EffSuspension::Delegate;
    su.target = std::move(target);
    su.binds = binds;
    su.call_src = rewrite_locals_to_this(slice(call), rewrite);
    return su;
  }

  // --- A-normalization (expression-nested perform) ---------------------
  //
  // The straight-line splitter (`build_step`) only lowers a suspension that
  // sits at statement level (`let x = perform …` / a bare `perform …`). A
  // `perform` buried in a larger expression (`let x = f(perform a()) + 1`)
  // is first hoisted, here, into a sequence of statement-level bindings —
  // ANF (administrative-normal form) — so the splitter then handles it with
  // no changes. This is a source pre-pass over the (already re-parsed) body:
  // every `perform` / effect-fn call in an expression position is bound to a
  // fresh `_anf_N` temp, in left-to-right evaluation order, and operands to
  // the left of a suspension are frozen into temps so their evaluation order
  // (and side effects) are preserved across the suspension.
  //
  // Scope of this slice (matching the thin-slice's "reject, don't guess"
  // stance): only *unconditionally-evaluated* positions are hoisted —
  // arithmetic / comparison / bitwise chains, unary operators, positional
  // call arguments, subscripts, and array elements. A `perform` in a
  // conditionally-evaluated position (`&&` / `||` / `??` right arm, a
  // ternary arm) or behind a non-trivial callee (a method chain whose
  // receiver would need freezing) is rejected with a symmetric SyntaxError —
  // generated-`if` guards and receiver freezing are the next sub-cycle.
  // Because this is a parse-time rewrite, interp / JIT / AOT stay in step
  // for free, rejections included ([[feedback-check-jit-interp-symmetry]]).

  static bool is_operator_node(const peg::Ast& n) {
    // Operator leaves in the precedence chain are named `*_OPERATOR`
    // (ADDITIVE_OPERATOR, MULTIPLICATIVE_OPERATOR, CONDITION_OPERATOR, …);
    // everything else at a chain node's children is an operand.
    static const std::string suffix = "_OPERATOR";
    const std::string& s = n.name;
    return s.size() >= suffix.size() &&
           std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
  }

  // A re-evaluable leaf: a literal or a bare identifier. Freezing one before a
  // suspension is unnecessary (a literal has no side effect; an identifier
  // resolves to a stable `this.<local>` that a handler cannot mutate).
  static bool is_atom(const peg::Ast& n) {
    using namespace peg::udl;
    switch (n.tag) {
      case "NUMBER"_:
      case "FLOAT"_:
      case "STRING"_:
      case "RAW_STRING"_:
      case "BOOLEAN"_:
      case "NIL"_:
      case "IDENTIFIER"_:
        return true;
      default:
        return false;
    }
  }

  // Infix chains whose operands are *all* evaluated left-to-right (no
  // short-circuit), so any operand is a safe hoist site.
  static bool is_eager_chain(unsigned int tag) {
    using namespace peg::udl;
    return tag == "ADDITIVE"_ || tag == "MULTIPLICATIVE"_ || tag == "POWER"_ ||
           tag == "CONDITION"_ || tag == "BIT_OR"_ || tag == "BIT_XOR"_ ||
           tag == "BIT_AND"_ || tag == "SHIFT"_ || tag == "RANGE"_;
  }

  static bool is_unary(unsigned int tag) {
    using namespace peg::udl;
    return tag == "UNARY_PLUS"_ || tag == "UNARY_MINUS"_ ||
           tag == "UNARY_NOT"_ || tag == "UNARY_BNOT"_;
  }

  std::string fresh_temp(int& ctr) const {
    return std::format("_anf_{}", ctr++);
  }

  // Strip the enclosing `[ … ]` from a (pos/len-polluted) subscript slice.
  static std::string strip_index_brackets(std::string_view s) {
    size_t lb = s.find('[');
    size_t rb = s.rfind(']');
    if (lb != std::string_view::npos && rb != std::string_view::npos &&
        rb > lb) {
      return std::string(s.substr(lb + 1, rb - lb - 1));
    }
    return std::string(s);
  }

  // ANF an expression given as source text (used to sidestep pos/len
  // pollution): re-parse it, on its own clean buffer, as the RHS of a throwaway
  // `let`, then ANF that clean subtree — hoists and the temp counter thread
  // through the caller's. `context` only supplies a position for a parse-error
  // rejection.
  std::string anf_reparsed(const std::string& expr_src,
                           const peg::Ast& context, int& ctr,
                           std::vector<std::string>& hoists) const {
    using namespace peg::udl;
    auto buf = std::make_shared<std::string>("let __anf_e = " + expr_src + "\n");
    auto prog = parse_registered_source("<eff-anf-expr>", buf);
    const peg::Ast* expr = nullptr;
    if (prog) {
      std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
        if (expr) return;
        if (n.tag == "ASSIGNMENT"_) {
          expr = view_assignment(n).rhs;
          return;
        }
        for (auto& c : n.nodes) walk(*c);
      };
      walk(*prog);
    }
    if (!expr) reject_unsupported_expr(context);
    EffectsLowerer sub(buf->data(), buf->size(), effect_fns_);
    return sub.anf(*expr, ctr, hoists);
  }

  [[noreturn]] void reject_conditional(const peg::Ast& n) const {
    throw CulebraError(
        "SyntaxError",
        "a `perform` in a short-circuit (`&&` / `||` / `??`) or ternary "
        "(`? :`) operand is not supported yet — bind it first "
        "(`let x = perform …`).",
        static_cast<long>(n.line), static_cast<long>(n.column));
  }
  [[noreturn]] void reject_complex_callee(const peg::Ast& n) const {
    throw CulebraError(
        "SyntaxError",
        "a `perform` behind a method chain / computed callee is not supported "
        "yet — bind the receiver or the argument first "
        "(`let r = obj.foo(); let a = perform …`).",
        static_cast<long>(n.line), static_cast<long>(n.column));
  }
  [[noreturn]] void reject_unsupported_expr(const peg::Ast& n) const {
    throw CulebraError(
        "SyntaxError",
        "a `perform` in this expression position is not supported yet — bind "
        "it first (`let x = perform …`).",
        static_cast<long>(n.line), static_cast<long>(n.column));
  }
  [[noreturn]] void reject_control_expr(const peg::Ast& n) const {
    throw CulebraError(
        "SyntaxError",
        "a `perform` in a control-flow condition / iterable is not supported "
        "yet — bind it first (`let c = perform …; while c { … }`).",
        static_cast<long>(n.line), static_cast<long>(n.column));
  }

  // Replace, in `node`'s source, each of `operands`' spans with the aligned
  // residual text, keeping every byte in between (operators, delimiters,
  // parentheses) verbatim. Operands are siblings in source order, so editing
  // right-to-left keeps earlier offsets valid.
  std::string splice_operands(const peg::Ast& node,
                              const std::vector<const peg::Ast*>& operands,
                              const std::vector<std::string>& residuals) const {
    std::string out(slice(node));
    size_t base = node.position;
    for (size_t i = operands.size(); i-- > 0;) {
      size_t off = operands[i]->position - base;
      out.replace(off, operands[i]->length, residuals[i]);
    }
    return out;
  }

  // Normalize an ordered operand list. Every operand left of the rightmost
  // suspension is fully evaluated now — hoisted (if it suspends) then frozen
  // into a temp (unless it is an atom) — so left-to-right order survives the
  // suspension. The rightmost suspending operand is hoisted but its residual
  // may stay in place; operands to its right are emitted verbatim (nothing
  // after them suspends).
  std::vector<std::string> anf_operands(
      const std::vector<const peg::Ast*>& operands, int& ctr,
      std::vector<std::string>& hoists) const {
    int k = -1;
    for (int i = 0; i < static_cast<int>(operands.size()); i++) {
      if (has_suspension(*operands[i])) k = i;
    }
    std::vector<std::string> out(operands.size());
    for (int i = 0; i < static_cast<int>(operands.size()); i++) {
      const peg::Ast& op = *operands[i];
      if (i < k) {
        std::string r = has_suspension(op) ? anf(op, ctr, hoists)
                                           : std::string(slice(op));
        if (is_atom(op) && !has_suspension(op)) {
          out[i] = std::move(r);
        } else {
          std::string t = fresh_temp(ctr);
          hoists.push_back(std::format("let {} = ({})", t, r));
          out[i] = std::move(t);
        }
      } else if (i == k) {
        out[i] = anf(op, ctr, hoists);
      } else {
        out[i] = std::string(slice(op));
      }
    }
    return out;
  }

  // Positional-argument nodes of a call/perform ARGUMENTS list. Rejects the
  // whole call if it carries a kwarg / `**` splat while also suspending (the
  // thin slice can't reorder those around a hoist).
  std::vector<const peg::Ast*> call_operands(const peg::Ast& args) const {
    using namespace peg::udl;
    std::vector<const peg::Ast*> ops;
    for (auto& c : args.nodes) {
      if (c->tag == "KWARG"_ || c->tag == "KWARG_SPLAT"_) {
        throw CulebraError(
            "SyntaxError",
            "keyword / splat arguments are not supported alongside a nested "
            "`perform` yet.",
            static_cast<long>(c->line), static_cast<long>(c->column));
      }
      ops.push_back(c.get());
    }
    return ops;
  }

  // Expression-mode ANF: append hoist statements to `hoists`, return the
  // suspension-free residual source for `node`. Any perform / effect-fn call,
  // including the outermost, becomes a fresh temp.
  std::string anf(const peg::Ast& node, int& ctr,
                  std::vector<std::string>& hoists) const {
    using namespace peg::udl;
    if (!has_suspension(node)) return std::string(slice(node));

    if (node.tag == "PERFORM"_ || is_effect_call(node)) {
      // A suspending call in expression position: normalize its args in place
      // (`anf_keep_call`), then hoist the whole call into a fresh temp.
      std::string call = anf_keep_call(node, ctr, hoists);
      std::string t = fresh_temp(ctr);
      hoists.push_back(std::format("let {} = {}", t, call));
      return t;
    }
    if (is_eager_chain(node.tag)) {
      std::vector<const peg::Ast*> ops;
      for (auto& c : node.nodes)
        if (!is_operator_node(*c)) ops.push_back(c.get());
      auto res = anf_operands(ops, ctr, hoists);
      return splice_operands(node, ops, res);
    }
    if (is_unary(node.tag)) {
      std::vector<const peg::Ast*> ops{node.nodes.back().get()};
      auto res = anf_operands(ops, ctr, hoists);
      return splice_operands(node, ops, res);
    }
    if (node.tag == "CALL"_) {
      // Only a plain `name(args)` or `name[index]` is lowerable: the base is a
      // bare identifier (an atom, no freeze). Anything else (a method chain, a
      // call/index on a call) would need receiver freezing — deferred.
      if (node.nodes.size() != 2 || node.nodes[0]->tag != "IDENTIFIER"_) {
        reject_complex_callee(node);
      }
      // The postfix's original_tag names its kind — INDEX/DOT collapse onto
      // their inner expression, so `tag` alone can't tell a subscript from a
      // member (same mechanism the interpreter dispatches on).
      const peg::Ast& post = *node.nodes[1];
      if (post.original_tag == "ARGUMENTS"_) {
        auto ops = call_operands(post);  // plain call: hoist positional args
        auto res = anf_operands(ops, ctr, hoists);
        return splice_operands(node, ops, res);
      }
      if (post.original_tag == "INDEX"_) {
        // `[ EXPR ]` collapses onto EXPR but leaves its span covering the
        // brackets (the AstOptimizer single-child pos/len trap — see
        // [[peglib-ast-optimizer]]), so slicing/splicing it directly would
        // eat the brackets. Re-parse the bracket-trimmed index as a
        // standalone expression for clean positions, ANF it, and rebuild
        // `base[<residual>]`.
        std::string idx_res = anf_reparsed(strip_index_brackets(slice(post)),
                                           node, ctr, hoists);
        return std::string(slice(*node.nodes[0])) + "[" + idx_res + "]";
      }
      reject_complex_callee(node);  // member (DOT) / computed callee
    }
    if (node.tag == "ARRAY"_) {
      const peg::Ast& seq = *node.nodes[0];
      std::vector<const peg::Ast*> ops;
      for (auto& c : seq.nodes) {
        if (c->tag == "SPREAD_ELEM"_ && has_suspension(*c)) {
          reject_unsupported_expr(*c);
        }
        ops.push_back(c.get());
      }
      auto res = anf_operands(ops, ctr, hoists);
      return splice_operands(node, ops, res);
    }
    if (node.tag == "LOGICAL_AND"_ || node.tag == "LOGICAL_OR"_ ||
        node.tag == "NIL_COALESCE"_ || node.tag == "CONDITIONAL"_) {
      reject_conditional(node);
    }
    reject_unsupported_expr(node);
  }

  // Statement-mode ANF: like `anf`, but a `perform` / effect-fn call already
  // at statement level (a bare call, or a `let x = perform …` RHS) is KEPT
  // there — only its nested arguments are hoisted — so `build_step` still sees
  // the clean statement-level shape it lowers directly.
  std::string anf_keep_call(const peg::Ast& call, int& ctr,
                            std::vector<std::string>& hoists) const {
    auto ops = call_operands(*call.nodes[1]);
    auto res = anf_operands(ops, ctr, hoists);
    return splice_operands(call, ops, res);
  }

  // True if a statement-level perform/effect-call carries a suspension inside
  // its arguments (so its args need hoisting even though the call stays put).
  bool args_have_suspension(const peg::Ast& call) const {
    return call.nodes.size() > 1 && has_suspension(*call.nodes[1]);
  }

  // Normalize one body statement. Returns nullopt when nothing needed hoisting
  // (the caller emits the statement verbatim); otherwise the returned source is
  // the hoist lines followed by the residual statement.
  std::optional<std::string> anf_statement(const peg::Ast* stmt,
                                           int& ctr) const {
    using namespace peg::udl;
    const peg::Ast* s = unwrap_stmt(stmt);
    std::vector<std::string> hoists;
    std::string residual_stmt;

    // Control-flow statement carrying a suspension: recurse ANF into each
    // block body (hoists stay inside the block so a loop re-evaluates them),
    // leaving the driving condition / iterable untouched (a `perform` there is
    // rejected — its evaluation order across iterations has no hoist).
    if ((s->tag == "IF"_ || s->tag == "WHILE"_ || s->tag == "FOR"_) &&
        has_suspension(*s)) {
      return anf_control_flow(s, ctr);
    }

    if (s->tag == "RETURN"_) {
      if (s->nodes.empty() || !has_suspension(*s->nodes[0])) return std::nullopt;
      std::string r = anf(*s->nodes[0], ctr, hoists);
      residual_stmt = splice_operands(*s, {s->nodes[0].get()}, {r});
    } else if (s->tag == "ASSIGNMENT"_) {
      auto av = view_assignment(*s);
      const peg::Ast* rhs = av.rhs;
      if (!has_suspension(*rhs)) return std::nullopt;
      std::string r;
      if (rhs->tag == "PERFORM"_ || is_effect_call(*rhs)) {
        if (!args_have_suspension(*rhs)) return std::nullopt;  // already clean
        r = anf_keep_call(*rhs, ctr, hoists);
      } else {
        r = anf(*rhs, ctr, hoists);
      }
      residual_stmt = splice_operands(*s, {rhs}, {r});
    } else {
      // bare expression statement
      if (!has_suspension(*s)) return std::nullopt;
      if (s->tag == "PERFORM"_ || is_effect_call(*s)) {
        if (!args_have_suspension(*s)) return std::nullopt;
        residual_stmt = anf_keep_call(*s, ctr, hoists);
      } else {
        residual_stmt = anf(*s, ctr, hoists);
      }
    }

    std::string out;
    for (auto& h : hoists) out += h + "\n";
    out += residual_stmt;
    return out;
  }

  // ANF a control-flow statement's block bodies in place. `ctr` threads through
  // so hoist temps stay unique across nesting levels (a per-block reset would
  // alias `_anf_0` across blocks, and every temp becomes a shared `this.` field).
  std::optional<std::string> anf_control_flow(const peg::Ast* s,
                                              int& ctr) const {
    using namespace peg::udl;
    std::vector<const peg::Ast*> blocks;
    if (s->tag == "WHILE"_) {
      if (s->nodes.size() < 2) reject_unsupported_expr(*s);
      if (has_suspension(*s->nodes[0])) reject_control_expr(*s->nodes[0]);
      blocks.push_back(s->nodes[1].get());
    } else if (s->tag == "FOR"_) {
      if (s->nodes.size() < 3) reject_unsupported_expr(*s);
      if (has_suspension(*s->nodes[1])) reject_control_expr(*s->nodes[1]);
      blocks.push_back(s->nodes[2].get());
    } else {  // IF: [cond, block, cond, block, …, (else-block)]
      size_t n = s->nodes.size();
      size_t pairs = n / 2;
      for (size_t p = 0; p < pairs; p++) {
        if (has_suspension(*s->nodes[2 * p]))
          reject_control_expr(*s->nodes[2 * p]);
        blocks.push_back(s->nodes[2 * p + 1].get());
      }
      if (n % 2 == 1) blocks.push_back(s->nodes[n - 1].get());
    }
    std::vector<std::string> new_blocks;
    for (auto* b : blocks) new_blocks.push_back(anf_block(*b, ctr));
    return splice_operands(*s, blocks, new_blocks);
  }

  // ANF a `{ … }` block: re-parse its inner (for clean positions), ANF the
  // statements, and re-brace. Returns the block verbatim when nothing hoisted.
  std::string anf_block(const peg::Ast& block, int& ctr) const {
    auto inner = std::string(strip_block_braces(slice(block)));
    auto buf = std::make_shared<std::string>(inner + "\n");
    auto prog = parse_registered_source("<eff-anf-block>", buf);
    if (!prog) {
      throw CulebraError("InternalError",
                         "effects ANF could not re-parse a block body", 0, 0);
    }
    EffectsLowerer sub(buf->data(), buf->size(), effect_fns_);
    auto norm = sub.anf_program(*prog, ctr);
    return "{\n" + (norm ? *norm : inner) + "\n}";
  }

  // Whole-body ANF pass. Returns the rewritten body source when any statement
  // needed hoisting, else nullopt (so the proven straight-line path runs
  // unchanged for bodies with no expression-nested performs). `ctr` threads
  // the fresh-temp counter through nested block recursion.
  std::optional<std::string> anf_program(const peg::Ast& program,
                                         int& ctr) const {
    auto stmts = body_stmts(program);
    bool changed = false;
    std::string out;
    for (auto* st : stmts) {
      if (auto expanded = anf_statement(st, ctr)) {
        changed = true;
        out += *expanded;
      } else {
        out += std::string(slice(*st));
      }
      if (out.empty() || out.back() != '\n') out += "\n";
    }
    return changed ? std::optional<std::string>(std::move(out)) : std::nullopt;
  }

  // --- CPS: flat-dispatch state machine over control flow ----------------
  //
  // The body lowers to a flat list of states — the same shape and reasoning as
  // the generator's `CpsBuilder` ([[project-generator-design]] §CPS): each
  // basic block is a state, control flow becomes `this._eff_state = K; continue`
  // jumps over one `while true` dispatch loop, and every local lives on the
  // instance (all-locals-on-heap, so no liveness analysis). The transition
  // primitives differ from the generator's: a `perform` returns SUSPEND (and
  // on resume binds `_rv` to its target), an effect-fn call returns DELEGATE,
  // and the body's tail expression / a `return` assign `this._eff_val`. A
  // statement-level suspension is all the CPS layer sees; expression-nested
  // performs are hoisted to statement level by the ANF pre-pass first, and
  // `for`-in is desugared to `while` upstream. `_rv` is the per-class resume
  // parameter name (unique so a nested computation doesn't shadow an enclosing
  // one — culebra forbids shadowing an enclosing-fn variable).
  struct CpsState {
    std::vector<std::string> states;
    std::vector<std::pair<int, int>> loop_stack;  // (header, exit) innermost last
    int terminal = -1;                            // state that returns EFF_DONE
    std::string rv;                               // resume parameter name
    bool failed = false;
    int fresh() {
      states.emplace_back();
      return static_cast<int>(states.size()) - 1;
    }
  };

  std::string cps_rw(const peg::Ast& n,
                     const std::set<std::string>& rw) const {
    return rewrite_locals_to_this(slice(n), rw);
  }
  bool cps_needs_split(const peg::Ast& s) const {
    return has_suspension(s) || has_escaping_loop_ctrl(s);
  }

  int cps_jump(CpsState& st, int target) const {
    int e = st.fresh();
    st.states[e] =
        std::format("      this._eff_state = {}\n      continue\n", target);
    return e;
  }

  // A suspension state (+ resume state when it binds a result or is in tail
  // position). On resume the driver re-enters `_step` at the resume state,
  // where `_rv` holds the resumed value.
  int cps_emit_suspension(CpsState& st, const EffSuspension& su, int cont,
                          bool tail) const {
    int after = cont;
    if (su.binds || tail) {
      int resume = st.fresh();
      std::string b;
      if (su.binds)
        b += std::format("      this.{} = {}\n", su.target, st.rv);
      if (tail) {
        std::string v = su.binds ? ("this." + su.target) : st.rv;
        b += std::format("      this._eff_val = {}\n", v);
      }
      b += std::format("      this._eff_state = {}\n      continue\n", cont);
      st.states[resume] = b;
      after = resume;
    }
    int susp = st.fresh();
    if (su.kind == EffSuspension::Perform) {
      st.states[susp] = std::format(
          "      this._eff_op = \"{}\"\n      this._eff_args = {}\n"
          "      this._eff_state = {}\n      return {}\n",
          su.op, su.args_array, after, EFF_SUSPEND);
    } else {
      st.states[susp] = std::format(
          "      this._eff_delegate = ({})\n      this._eff_state = {}\n"
          "      return {}\n",
          su.call_src, after, EFF_DELEGATE);
    }
    return susp;
  }

  int cps_return(CpsState& st, const peg::Ast* u,
                 const std::set<std::string>& rw) const {
    const peg::Ast* e = u->nodes.empty() ? nullptr : u->nodes[0].get();
    if (e && has_suspension(*e)) reject_hidden(*u);  // ANF hoists; defensive
    int s = st.fresh();
    std::string v = e ? cps_rw(*e, rw) : "nil";
    st.states[s] = std::format(
        "      this._eff_val = ({})\n      this._eff_state = {}\n      continue\n",
        v, st.terminal);
    return s;
  }

  // A verbatim statement in tail (value) position: a bare expression IS the
  // value; an assignment runs, then its single-ident target is read back as the
  // value (matching culebra's block-value semantics — `let x = e` / `x = e`
  // evaluate to `e`); anything else leaves `_eff_val` nil.
  int cps_tail_value(CpsState& st, const peg::Ast* u,
                     const std::set<std::string>& rw) const {
    using namespace peg::udl;
    int s = st.fresh();
    std::string body;
    if (u->tag == "ASSIGNMENT"_) {
      body = "      " + cps_rw(*u, rw) + "\n";
      auto av = view_assignment(*u);
      if (av.lvalcnt == 1) {
        const auto& lval = *u->nodes[av.lvaloff];
        if (lval.tag == "IDENTIFIER"_)
          body += std::format("      this._eff_val = this.{}\n",
                              std::string(lval.token));
      }
    } else {
      body = std::format("      this._eff_val = ({})\n", cps_rw(*u, rw));
    }
    body += std::format("      this._eff_state = {}\n      continue\n", st.terminal);
    st.states[s] = body;
    return s;
  }

  // Compile a control-flow block's statements. A single-statement block
  // collapses onto its lone statement, whose span then covers the enclosing
  // braces (the AstOptimizer pos/len trap — [[peglib-ast-optimizer]]); re-parse
  // the brace-stripped inner for clean positions. A sub-lowerer bound to the
  // fresh buffer feeds states into the shared `st`. Multi-statement blocks
  // re-parse too — uniform and cheap.
  int cps_block_seq(CpsState& st, const peg::Ast& block, int cont, bool tail,
                    const std::set<std::string>& rw) const {
    auto sv = slice(block);
    if (sv.size() >= 2 && sv.front() == '{') {
      auto inner = std::string(strip_block_braces(sv));
      auto buf = std::make_shared<std::string>(inner + "\n");
      auto prog = parse_registered_source("<eff-block>", buf);
      if (!prog) { st.failed = true; return -1; }
      EffectsLowerer sub(buf->data(), buf->size(), effect_fns_);
      return sub.cps_seq(st, body_stmts(*prog), cont, tail, rw);
    }
    return cps_seq(st, body_stmts(block), cont, tail, rw);
  }

  int cps_while(CpsState& st, const peg::Ast* w, int cont,
                const std::set<std::string>& rw) const {
    if (w->nodes.size() < 2) { st.failed = true; return -1; }
    if (has_suspension(*w->nodes[0])) reject_control_expr(*w->nodes[0]);
    int h = st.fresh();
    st.loop_stack.push_back({h, cont});
    int body_entry = cps_block_seq(st, *w->nodes[1], h, /*tail=*/false, rw);
    st.loop_stack.pop_back();
    if (st.failed) return -1;
    st.states[h] = std::format(
        "      if {} {{ this._eff_state = {} }} else {{ this._eff_state = {} }}\n"
        "      continue\n",
        cps_rw(*w->nodes[0], rw), body_entry, cont);
    return h;
  }

  // if / else-if / else. IF nodes are [cond, block, …, elseblock?]. When `tail`,
  // each arm is compiled in value position; a missing else leaves the value nil.
  int cps_if(CpsState& st, const peg::Ast* ifn, int cont, bool tail,
             const std::set<std::string>& rw) const {
    const auto& nodes = ifn->nodes;
    size_t n = nodes.size();
    int else_entry;
    size_t pairs;
    if (n % 2 == 1) {
      else_entry = cps_block_seq(st, *nodes[n - 1], cont, tail, rw);
      if (st.failed) return -1;
      pairs = (n - 1) / 2;
    } else {
      else_entry = cont;
      pairs = n / 2;
    }
    int chain = else_entry;
    for (size_t p = pairs; p-- > 0;) {
      if (has_suspension(*nodes[2 * p])) reject_control_expr(*nodes[2 * p]);
      int block_entry = cps_block_seq(st, *nodes[2 * p + 1], cont, tail, rw);
      if (st.failed) return -1;
      int s = st.fresh();
      st.states[s] = std::format(
          "      if {} {{ this._eff_state = {} }} else {{ this._eff_state = {} }}\n"
          "      continue\n",
          cps_rw(*nodes[2 * p], rw), block_entry, chain);
      chain = s;
    }
    return chain;
  }

  int cps_stmt(CpsState& st, const peg::Ast* s, int cont, bool tail,
               const std::set<std::string>& rw) const {
    using namespace peg::udl;
    auto* u = unwrap_stmt(s);
    if (u->tag == "IF"_) return cps_if(st, u, cont, tail, rw);
    if (u->tag == "WHILE"_) return cps_while(st, u, cont, rw);
    if (u->tag == "RETURN"_) return cps_return(st, u, rw);
    if (u->tag == "BREAK"_) {
      if (st.loop_stack.empty()) { st.failed = true; return -1; }
      return cps_jump(st, st.loop_stack.back().second);
    }
    if (u->tag == "CONTINUE"_) {
      if (st.loop_stack.empty()) { st.failed = true; return -1; }
      return cps_jump(st, st.loop_stack.back().first);
    }
    if (u->tag == "LEXICAL_SCOPE"_ || u->tag == "STATEMENTS"_) {
      return cps_block_seq(st, *u, cont, tail, rw);
    }
    // Leaf statement: a statement-level suspension (post-ANF) or a rejected
    // hidden one.
    EffStmtClass c = classify(u, rw);
    if (c.kind == EffStmtClass::Suspend)
      return cps_emit_suspension(st, c.susp, cont, tail);
    st.failed = true;  // FOR (must be desugared) / anything unexpected
    return -1;
  }

  // The tail statement of a value-producing sequence. Loops carry no value; an
  // `if` recurses per arm; a suspension's resumed value becomes the value; a
  // plain statement is compiled by `cps_tail_value`.
  int cps_tail_stmt(CpsState& st, const peg::Ast* s, int cont,
                    const std::set<std::string>& rw) const {
    using namespace peg::udl;
    auto* u = unwrap_stmt(s);
    if (u->tag == "IF"_) return cps_if(st, u, cont, /*tail=*/true, rw);
    if (u->tag == "WHILE"_ || u->tag == "RETURN"_ || u->tag == "BREAK"_ ||
        u->tag == "CONTINUE"_)
      return cps_stmt(st, s, cont, /*tail=*/false, rw);
    if (u->tag == "FOR"_) {
      // A suspending `for` is desugared to `while` upstream; a non-suspending
      // one just runs verbatim. Either way a loop yields no value (nil, as in
      // culebra), so tail position adds nothing.
      if (cps_needs_split(*u)) return cps_stmt(st, s, cont, /*tail=*/false, rw);
      int e = st.fresh();
      st.states[e] = "      " + cps_rw(*u, rw) +
                     std::format("\n      this._eff_state = {}\n      continue\n",
                                 cont);
      return e;
    }
    if (u->tag == "LEXICAL_SCOPE"_ || u->tag == "STATEMENTS"_)
      return cps_block_seq(st, *u, cont, /*tail=*/true, rw);
    if (has_suspension(*u)) {
      EffStmtClass c = classify(u, rw);
      if (c.kind == EffStmtClass::Suspend)
        return cps_emit_suspension(st, c.susp, cont, /*tail=*/true);
      reject_hidden(*u);
    }
    return cps_tail_value(st, u, rw);
  }

  // Linearize a sequence with no value semantics (interior of a body / loop):
  // maximal runs of split-free statements collapse into one state.
  int cps_interior(CpsState& st, const std::vector<const peg::Ast*>& stmts,
                   int cont, const std::set<std::string>& rw) const {
    int k = cont;
    std::string pending;
    auto flush = [&]() {
      if (pending.empty()) return;
      int s = st.fresh();
      st.states[s] = pending +
                     std::format("      this._eff_state = {}\n      continue\n", k);
      k = s;
      pending.clear();
    };
    for (size_t idx = stmts.size(); idx-- > 0;) {
      const peg::Ast* s = stmts[idx];
      if (!cps_needs_split(*s)) {
        pending = "      " + cps_rw(*s, rw) + "\n" + pending;
      } else {
        flush();
        k = cps_stmt(st, s, k, /*tail=*/false, rw);
        if (st.failed) return -1;
      }
    }
    flush();
    return k;
  }

  // Compile a statement sequence; when `tail`, its final statement is in value
  // position (assigns `this._eff_val`).
  int cps_seq(CpsState& st, const std::vector<const peg::Ast*>& stmts, int cont,
              bool tail, const std::set<std::string>& rw) const {
    if (tail && !stmts.empty()) {
      std::vector<const peg::Ast*> rest(stmts.begin(), stmts.end() - 1);
      int tail_entry = cps_tail_stmt(st, stmts.back(), cont, rw);
      if (st.failed) return -1;
      return cps_interior(st, rest, tail_entry, rw);
    }
    return cps_interior(st, stmts, cont, rw);
  }

  // Lower a body into the `_step` dispatch source and its entry state index.
  // `rewrite` = params + body locals (moved to `this.` so they persist across
  // states). Throws SyntaxError for a construct the CPS engine can't lower.
  std::pair<std::string, int> build_dispatch(
      const peg::Ast& body, const std::set<std::string>& rewrite,
      const std::string& rv_name) const {
    CpsState st;
    st.rv = rv_name;
    st.terminal = st.fresh();
    st.states[st.terminal] = std::format("      return {}\n", EFF_DONE);
    int entry = cps_seq(st, body_stmts(body), st.terminal, /*tail=*/true,
                        rewrite);
    if (st.failed || entry < 0) {
      throw CulebraError(
          "SyntaxError",
          "unsupported control flow in an effect body (a `perform` reachable "
          "through a construct the effects transform can't lower).",
          static_cast<long>(body.line), static_cast<long>(body.column));
    }
    std::string dispatch = "      while true {\n";
    for (size_t i = 0; i < st.states.size(); i++) {
      dispatch += std::format("        if this._eff_state == {} {{\n{}        }}\n",
                              i, st.states[i]);
    }
    dispatch += "      }\n";
    return {dispatch, entry};
  }

  // Re-parse a body's inner source as a standalone program so its statement
  // nodes are cleanly optimized with correct source positions. A single-
  // statement BLOCK collapses onto its lone statement whose pos/len then span
  // the enclosing braces (the AstOptimizer pos/len trap — see
  // [[peglib-ast-optimizer]]); parsing the brace-stripped inner sidesteps it
  // uniformly for single- and multi-statement bodies alike.
  struct ReparsedBody {
    std::shared_ptr<peg::Ast> program;
    std::shared_ptr<std::string> source;  // owns the buffer nodes point into
  };
  ReparsedBody reparse_body(const peg::Ast& body_node) const {
    auto inner = std::string(strip_block_braces(slice(body_node)));
    auto src = std::make_shared<std::string>(std::move(inner));
    auto prog = parse_registered_source("<eff-body>", src);
    return {prog, src};
  }

  // --- for-in desugar (to `while` + iterator) ----------------------------
  // The CPS engine works over `while`, so a `for x in e { … }` that carries a
  // suspension is rewritten to `let _it = (e).iter(); while _it.has_next() {
  // let x = _it.next(); … }` — the same source pre-pass the generator uses,
  // keyed on `has_suspension` instead of `has_yield`. Only a single loop
  // variable is supported; a destructuring `for k, v in …` with a suspension is
  // rejected. Stops at fn / HANDLE boundaries so a nested handle's own for-ins
  // aren't reattributed.
  std::vector<const peg::Ast*> collect_outermost_suspending_fors(
      const peg::Ast& body) const {
    using namespace peg::udl;
    std::vector<const peg::Ast*> out;
    std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
      if (is_fn_boundary(n.tag) || n.tag == "HANDLE"_) return;
      if (n.tag == "FOR"_ && has_suspension(n)) { out.push_back(&n); return; }
      for (auto& c : n.nodes) walk(*c);
    };
    walk(body);
    return out;
  }
  std::optional<std::string> rewrite_suspending_fors(
      const peg::Ast& body) const {
    using namespace peg::udl;
    auto fors = collect_outermost_suspending_fors(body);
    if (fors.empty()) return std::nullopt;
    std::string out(slice(body));
    size_t base = body.position;
    for (auto it = fors.rbegin(); it != fors.rend(); ++it) {
      auto* f = *it;
      if (f->nodes.size() < 3) continue;
      const auto& var_node = *f->nodes[0];
      const auto& expr_node = *f->nodes[1];
      const auto& blk_node = *f->nodes[2];
      if (var_node.tag != "IDENTIFIER"_) {
        throw CulebraError(
            "SyntaxError",
            "a destructuring `for k, v in …` with a `perform` inside is not "
            "supported yet — iterate a single value.",
            static_cast<long>(f->line), static_cast<long>(f->column));
      }
      if (has_suspension(expr_node)) reject_control_expr(expr_node);
      auto iter_var = std::format("_eff_it_{}", f->position);
      auto replacement = std::format(
          "let {0} = ({1}).iter()\n"
          "while {0}.has_next() {{\n"
          "  let {2} = {0}.next()\n"
          "  {3}\n"
          "}}",
          iter_var, std::string(slice(expr_node)),
          std::string(var_node.token),
          std::string(strip_block_braces(slice(blk_node))));
      out.replace(f->position - base, f->length, replacement);
    }
    return out;
  }

  // Build the whole computation class source (ctor + _step) for a body node.
  // `param_names` are the enclosing fn's params (empty for a handle body);
  // `rv_name` is the per-class resume parameter (unique so a nested computation
  // doesn't shadow an enclosing one). Two source pre-passes run first over the
  // re-parsed body: `for`-in desugar (to a fixpoint for nested loops), then
  // A-normalization (hoisting expression-nested performs to statement level),
  // so the CPS builder only ever sees statement-level suspensions over
  // if/while. Each pre-pass re-parses into a clean buffer whose slices resolve.
  std::string build_computation_class(
      const std::string& class_name, const peg::Ast& body_node,
      const std::vector<std::string_view>& param_names,
      const std::string& rv_name) const {
    auto rb = reparse_body(body_node);
    if (!rb.program) {
      throw CulebraError("InternalError",
                         "effects transform could not re-parse a body", 0, 0);
    }
    std::shared_ptr<peg::Ast> prog = rb.program;
    std::shared_ptr<std::string> src = rb.source;

    // for-in desugar to a fixpoint (a nested for-in surfaces as outermost on
    // the next pass).
    while (true) {
      EffectsLowerer fl(src->data(), src->size(), effect_fns_);
      auto desugared = fl.rewrite_suspending_fors(*prog);
      if (!desugared) break;
      src = std::make_shared<std::string>(std::move(*desugared));
      prog = parse_registered_source("<eff-for>", src);
      if (!prog) {
        throw CulebraError(
            "InternalError", "effects for-in desugar produced unparseable "
            "source", 0, 0);
      }
    }

    // A-normalization.
    EffectsLowerer anf_pass(src->data(), src->size(), effect_fns_);
    int ctr = 0;
    if (auto normalized = anf_pass.anf_program(*prog, ctr)) {
      auto src2 = std::make_shared<std::string>(std::move(*normalized));
      auto prog2 = parse_registered_source("<eff-anf>", src2);
      if (!prog2) {
        throw CulebraError(
            "InternalError",
            "effects A-normalization produced unparseable source", 0, 0);
      }
      EffectsLowerer sub(src2->data(), src2->size(), effect_fns_);
      return sub.build_class_from_program(class_name, *prog2, param_names,
                                          rv_name);
    }
    EffectsLowerer sub(src->data(), src->size(), effect_fns_);
    return sub.build_class_from_program(class_name, *prog, param_names, rv_name);
  }

  // A nested `handle` that references an enclosing computation's binding is
  // rejected: the inner handler / body lowers to a closure whose `this` is NOT
  // the enclosing instance, so a rewritten `this.<local>` reads nil (the interp
  // happens to capture `this` lexically, the JIT does not — an asymmetry). Test
  // by the same matcher the rewrite uses, so detection tracks it exactly.
  void reject_handle_capture(const peg::Ast& program,
                             const std::set<std::string>& names) const {
    using namespace peg::udl;
    if (names.empty()) return;
    std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
      if (n.tag == "HANDLE"_) {
        auto src = slice(n);
        if (rewrite_locals_to_this(src, names) != std::string(src)) {
          throw CulebraError(
              "SyntaxError",
              "a nested `handle` that references an enclosing effect binding is "
              "not supported yet — pass the value in explicitly.",
              static_cast<long>(n.line), static_cast<long>(n.column));
        }
        return;  // the inner handle's own captures are checked when it lowers
      }
      for (auto& c : n.nodes) walk(*c);
    };
    for (auto& c : program.nodes) walk(*c);
  }

  std::string build_class_from_program(
      const std::string& class_name, const peg::Ast& program,
      const std::vector<std::string_view>& param_names,
      const std::string& rv_name) const {
    auto locals = collect_local_names(program);
    std::set<std::string> rewrite = locals;
    for (auto& p : param_names) rewrite.insert(std::string(p));
    reject_handle_capture(program, rewrite);

    auto [step, entry] = build_dispatch(program, rewrite, rv_name);

    std::string ctor_params, ctor_call_args;
    std::string ctor_inits = std::format(
        "      this._eff_state = {}\n"
        "      this._eff_val = nil\n"
        "      this._eff_op = nil\n"
        "      this._eff_args = nil\n"
        "      this._eff_delegate = nil\n",
        entry);
    emit_ctor_param_and_local_inits(param_names, locals, ctor_params,
                                    ctor_call_args, ctor_inits);

    return std::format(
        "  class {0} {{\n"
        "    new({1}) {{\n{2}    }}\n"
        "    _step({3}) {{\n{4}    }}\n"
        "  }}\n",
        class_name, ctor_params, ctor_inits, rv_name, step);
  }

  // `effect fn f(params) { BODY }` -> `fn f(params) { class …; ….new(args) }`
  // A signature-only op -> a throwing stub (it must be invoked via perform).
  std::shared_ptr<peg::Ast> lower_effect_fn_decl(std::shared_ptr<peg::Ast> ast) {
    using namespace peg::udl;
    std::string name = effect_fn_name(*ast);

    if (!effect_fn_has_body(*ast)) {
      // Operation declaration: a stub that rejects a direct call.
      auto params_sv = slice(*ast->nodes[1]);
      auto synth = std::make_shared<std::string>(std::format(
          "fn {0}{1} {{ throw {{ kind: \"EffectError\", message: \"effect "
          "operation '{0}' must be invoked via `perform`\" }} }}\n",
          name, std::string(params_sv)));
      return reparse_decl(synth);
    }

    const auto& params_ast = *ast->nodes[1];
    const auto& body = *ast->nodes.back();
    auto param_names = collect_positional_param_names(params_ast);
    auto class_name = std::format("_EffComp_{}_{}_{}", name, ast->line,
                                  ast->column);
    auto rv_name = std::format("_rv_{}_{}", ast->line, ast->column);
    std::string cls =
        build_computation_class(class_name, body, param_names, rv_name);

    std::string call_args;
    for (size_t j = 0; j < param_names.size(); j++) {
      if (j > 0) call_args += ", ";
      call_args += std::string(param_names[j]);
    }
    auto params_sv = slice(params_ast);
    auto synth = std::make_shared<std::string>(std::format(
        "fn {0}{1} {{\n{2}  {3}.new({4})\n}}\n",
        name, std::string(params_sv), cls, class_name, call_args));
    return reparse_decl(synth);
  }

  // `handle { BODY } with op1(params) { H1 } … with return(v) { R }` ->
  //   __Eff.handle(
  //     (fn() { class _EffBody…; _EffBody….new() })(),
  //     { op1: fn(_eh_args, _eh_resume) { <bind op-args>; let <resume> =
  //             _eh_resume; H1 }, op2: … },
  //     fn(_eh_val) { let v = _eh_val; R })   // the return-clause map, or nil
  // Each op `with` clause becomes one entry of the handler frame; `_find` (in
  // effects.cul) looks an operation up in the dynamically-scoped frame stack.
  // The optional `with return` clause maps the normal-completion value.
  std::shared_ptr<peg::Ast> lower_handle(std::shared_ptr<peg::Ast> ast) {
    using namespace peg::udl;
    const auto& body = *ast->nodes[0];

    auto class_name = std::format("_EffBody_{}_{}", ast->line, ast->column);
    auto rv_name = std::format("_rv_{}_{}", ast->line, ast->column);
    std::string cls = build_computation_class(class_name, body, {}, rv_name);

    // Build the handler frame `{ op: adapter, … }` over every op `with` clause,
    // plus an optional `with return(v) { … }` that maps the normal-completion
    // value. An adapter binds the operation's args from _eh_args[i], then names
    // the last parameter as the one-shot resume continuation.
    std::string frame = "{";
    std::string return_fn = "nil";  // the return-clause adapter, or nil
    std::set<std::string> seen;
    bool first_op = true;
    for (size_t c = 1; c < ast->nodes.size(); c++) {
      const auto& clause = *ast->nodes[c];

      if (clause.tag == "RETURN_CLAUSE"_) {  // [PARAMETERS, BLOCK]
        if (return_fn != "nil") {
          throw CulebraError(
              "SyntaxError",
              "a `handle` may have only one `return` clause.",
              static_cast<long>(clause.line), static_cast<long>(clause.column));
        }
        const auto& params = *clause.nodes[0];
        if (params.nodes.size() != 1) {
          throw CulebraError(
              "SyntaxError",
              "a `return` clause takes exactly one parameter "
              "(`with return(value) { … }`).",
              static_cast<long>(clause.line), static_cast<long>(clause.column));
        }
        auto vn = view_parameter(*params.nodes[0]).name;
        auto ret_src = strip_block_braces(slice(*clause.nodes[1]));
        return_fn = std::format(
            "fn(_eh_val) {{\n      let {} = _eh_val\n      {}\n    }}",
            std::string(vn), std::string(ret_src));
        continue;
      }

      // [IDENTIFIER, PARAMETERS, BLOCK]
      std::string op = std::string(clause.nodes[0]->token);
      if (!seen.insert(op).second) {
        throw CulebraError(
            "SyntaxError",
            std::format("duplicate handler clause for effect '{}' in one "
                        "`handle`.", op),
            static_cast<long>(clause.line), static_cast<long>(clause.column));
      }
      const auto& params = *clause.nodes[1];
      const auto& handler_body = *clause.nodes[2];
      if (params.nodes.empty()) {
        throw CulebraError(
            "SyntaxError",
            "a handler clause needs a `resume` parameter (`with op(resume) "
            "{ … }`).",
            static_cast<long>(clause.line), static_cast<long>(clause.column));
      }
      std::string binds;
      size_t nparams = params.nodes.size();
      for (size_t i = 0; i + 1 < nparams; i++) {
        auto pn = view_parameter(*params.nodes[i]).name;
        binds += std::format("      let {} = _eh_args[{}]\n", std::string(pn), i);
      }
      auto resume_name = view_parameter(*params.nodes[nparams - 1]).name;
      binds += std::format("      let {} = _eh_resume\n", std::string(resume_name));
      auto handler_src = strip_block_braces(slice(handler_body));
      if (!first_op) frame += ",";
      first_op = false;
      frame += std::format(
          "\n    {0}: fn(_eh_args, _eh_resume) {{\n{1}      {2}\n    }}",
          op, binds, std::string(handler_src));
    }
    frame += "}";

    auto synth = std::make_shared<std::string>(std::format(
        "fn __eff_handle_wrapper__() {{\n"
        "  __Eff.handle(\n"
        "    (fn() {{\n{0}    {1}.new()\n    }})(),\n"
        "    {2},\n"
        "    {3})\n"
        "}}\n",
        cls, class_name, frame, return_fn));
    return reparse_expr(synth);
  }

  // Re-parse a synthesized `fn name(...) { … }` and return its MULTIFN_DECL,
  // then recursively lower any effect constructs the fragment still carries
  // (composition of nested handles / effect fns). The synthesized source is
  // registered for lifetime via the generator transform's source store.
  std::shared_ptr<peg::Ast> reparse_decl(std::shared_ptr<std::string> synth) {
    auto fn = parse_wrapper_fn(synth);
    if (!fn) {
      throw CulebraError("InternalError",
                         "effects transform produced unparseable source", 0, 0);
    }
    EffectsLowerer sub(synth->data(), synth->size(), effect_fns_);
    return sub.transform(fn);
  }

  // Re-parse a synthesized `fn __wrapper__() { <expr> }` and return the single
  // expression node in its body (to splice in place of a HANDLE), recursively
  // lowering nested effect constructs.
  std::shared_ptr<peg::Ast> reparse_expr(std::shared_ptr<std::string> synth) {
    using namespace peg::udl;
    auto fn = parse_wrapper_fn(synth);
    if (!fn) {
      throw CulebraError("InternalError",
                         "effects transform produced unparseable source", 0, 0);
    }
    auto body = fn->nodes.back();
    std::shared_ptr<peg::Ast> expr;
    if (body->tag == "STATEMENTS"_ && !body->nodes.empty()) {
      expr = body->nodes[0];
    } else {
      expr = body;
    }
    EffectsLowerer sub(synth->data(), synth->size(), effect_fns_);
    return sub.transform(expr);
  }
};

// Walk the AST, lowering every effect construct. Yield-free / effect-free
// modules pay one whole-tree pointer pass.
inline std::shared_ptr<peg::Ast> transform_effects_in(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  auto effect_fns = collect_effect_fn_names(*ast);
  EffectsLowerer lowerer(src, src_len, effect_fns);
  return lowerer.transform(ast);
}

// Public parse entry: `parse()` plus the generator and effects
// transformation passes. Every caller that wants `yield` / effects support
// (interp module load, JIT/AOT, REPL, lazy module loader) routes through
// here.
inline std::shared_ptr<peg::Ast> parse_with_transforms(
    const std::string& path, const char* expr, size_t len,
    std::vector<std::string>& msgs) {
  auto ast = parse_with_generator_transforms(path, expr, len, msgs);
  if (!ast) return ast;
  return transform_effects_in(ast, expr, len);
}

}  // namespace culebra
