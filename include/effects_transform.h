// Algebraic-effects (thin slice) AST transformation pass.
//
// `effect fn` / `perform` / `handle … with` are rewritten, at parse time,
// into plain culebra source (synthesized classes + calls into the `__Eff`
// runtime preamble), then re-parsed — the interp / JIT / AOT backends run
// the lowered code with no effects-specific support of their own, so the
// three backends stay symmetric for free (see [[project-algebraic-effects]],
// [[feedback-check-jit-interp-symmetry]]).
//
// Design (thin slice, dynamic scope, one-shot resume):
//   * An `effect fn f(...) { BODY }` (with body) lowers to a normal fn that
//     RETURNS a *computation object* — a flat-dispatch state machine whose
//     `_step(rv)` runs the body until the next suspension point, then hands
//     control back to the driver. A signature-only `effect fn op(...)`
//     declares an operation and lowers to a throwing stub.
//   * A suspension point is a statement-level `perform op(args)` (SUSPEND) or
//     a statement-level call to another effect fn (DELEGATE). The thin slice
//     supports straight-line bodies only; a suspension reachable through an
//     `if`/`while`/`for` or nested inside a larger expression is rejected
//     (A-normalization + control-flow lowering are the next cycle).
//   * `handle { BODY } with op(params, resume) { H }` lowers to
//     `__Eff.handle(<BODY as a computation>, "op", <handler adapter>)`. The
//     driver (`__Eff.drive`, in src/preambles/effects.cul) walks the
//     dynamically-scoped handler stack; `resume` is the one-shot continuation
//     (an ordinary RC value, so leak-safety is inherited from the generator
//     machinery it mirrors — [[project-rc-gc-correct-model]]).
//
// Reuses the generator transform's source-slice / local-rewrite helpers
// (generator_transform.h): everything here is the same "splice verbatim
// source between suspension points, rebuild only the seams" recipe.

#pragma once

#include "generator_transform.h"
#include "parser.h"

#include <format>
#include <functional>
#include <memory>
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

// Classify one body statement. Returns nullopt for a verbatim (non-suspending)
// statement; the caller emits it literally. Throws SyntaxError when a
// suspension hides somewhere the thin slice can't lower (nested in a larger
// expression / inside control flow), keeping interp and JIT honestly in step.
struct EffStmtClass {
  enum Kind { Verbatim, Suspend, Return } kind;
  EffSuspension susp;         // when kind == Suspend
  const peg::Ast* expr = nullptr;  // when kind == Return: the returned expr (may be null)
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

    // `return e`
    if (s->tag == "RETURN"_) {
      const peg::Ast* e = s->nodes.empty() ? nullptr : s->nodes[0].get();
      if (e && has_suspension(*e)) {
        throw CulebraError(
            "SyntaxError",
            "a `perform` inside a `return` expression is not supported yet "
            "(only statement-level `perform`).",
            static_cast<long>(s->line), static_cast<long>(s->column));
      }
      return EffStmtClass{EffStmtClass::Return, {}, e};
    }

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
                            make_perform(*rhs, target, /*binds=*/true, rewrite),
                            nullptr};
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
                            make_delegate(*rhs, target, /*binds=*/true, rewrite),
                            nullptr};
      }
      // Plain assignment: verbatim, but must not hide a suspension.
      if (has_suspension(*s)) reject_hidden(*s);
      return EffStmtClass{EffStmtClass::Verbatim, {}, nullptr};
    }

    // bare `perform …`
    if (s->tag == "PERFORM"_) {
      return EffStmtClass{EffStmtClass::Suspend,
                          make_perform(*s, "", /*binds=*/false, rewrite),
                          nullptr};
    }
    // bare `effectfn(…)`
    if (is_effect_call(*s)) {
      return EffStmtClass{EffStmtClass::Suspend,
                          make_delegate(*s, "", /*binds=*/false, rewrite),
                          nullptr};
    }

    // Any other statement is verbatim — as long as no suspension hides in it.
    if (has_suspension(*s)) reject_hidden(*s);
    return EffStmtClass{EffStmtClass::Verbatim, {}, nullptr};
  }

  void reject_hidden(const peg::Ast& s) const {
    throw CulebraError(
        "SyntaxError",
        "a `perform` / effect-fn call is only supported at statement level in "
        "this slice (not nested in an expression or control-flow body).",
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

  // Lower a straight-line body (BLOCK) into the `_step(_rv)` dispatch source
  // plus the `this._<field> = nil` seeds a ctor needs beyond params/locals.
  // `rewrite` = params + body locals (moved to `this.` so they persist across
  // states). Returns the full `_step` method body (the `while true { … }`).
  std::string build_step(const peg::Ast& body,
                         const std::set<std::string>& rewrite) const {
    auto stmts = body_stmts(body);
    std::vector<std::string> states;  // states[i] = source inside `if _state==i`
    std::string cur;                  // statements accumulating into the current state
    // States are emitted in order. A suspension names its successor as
    // `states.size()+1` (the state that opens right after the pending flush);
    // the terminal DONE state is appended last, once all statements are
    // consumed.

    auto flush_state = [&](const std::string& transition) {
      states.push_back(cur + transition);
      cur.clear();
    };

    for (size_t i = 0; i < stmts.size(); i++) {
      bool last = (i + 1 == stmts.size());
      EffStmtClass c = classify(stmts[i], rewrite);

      if (c.kind == EffStmtClass::Verbatim) {
        if (last) {
          // The trailing expression is the computation's value.
          cur += std::format("      this._eff_val = ({})\n",
                             rewrite_locals_to_this(slice(*stmts[i]), rewrite));
        } else {
          cur += "      " +
                 rewrite_locals_to_this(slice(*stmts[i]), rewrite) + "\n";
        }
        continue;
      }

      if (c.kind == EffStmtClass::Return) {
        std::string val = c.expr
            ? rewrite_locals_to_this(slice(*c.expr), rewrite)
            : "nil";
        cur += std::format("      this._eff_val = ({})\n", val);
        // remaining statements are dead; stop here.
        break;
      }

      // Suspend: emit the transition, open the next state, bind the result.
      int next = static_cast<int>(states.size()) + 1;
      const EffSuspension& su = c.susp;
      if (su.kind == EffSuspension::Perform) {
        flush_state(std::format(
            "      this._eff_op = \"{}\"\n"
            "      this._eff_args = {}\n"
            "      this._state = {}\n"
            "      return {}\n",
            su.op, su.args_array, next, EFF_SUSPEND));
      } else {
        flush_state(std::format(
            "      this._eff_delegate = ({})\n"
            "      this._state = {}\n"
            "      return {}\n",
            su.call_src, next, EFF_DELEGATE));
      }
      // Next state opens by binding the resumed value.
      if (su.binds) {
        cur += std::format("      this.{} = _rv\n", su.target);
      }
      if (last) {
        // A trailing bare suspension's result is the computation's value.
        if (su.binds) {
          cur += std::format("      this._eff_val = this.{}\n", su.target);
        } else {
          cur += "      this._eff_val = _rv\n";
        }
      }
    }

    // Close the final state: DONE. `_state` already equals this state's index
    // on entry (a predecessor set it), so no self-assignment is needed.
    states.push_back(cur + std::format("      return {}\n", EFF_DONE));

    std::string dispatch = "      while true {\n";
    for (size_t i = 0; i < states.size(); i++) {
      dispatch += std::format("        if this._state == {} {{\n{}        }}\n",
                              i, states[i]);
    }
    dispatch += "      }\n";
    return dispatch;
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

  // Build the whole computation class source (ctor + _step) for a body node.
  // `param_names` are the enclosing fn's params (empty for a handle body). The
  // body is re-parsed into a clean buffer; a sub-lowerer bound to that buffer
  // does the actual construction so all source slices resolve correctly.
  std::string build_computation_class(
      const std::string& class_name, const peg::Ast& body_node,
      const std::vector<std::string_view>& param_names) const {
    auto rb = reparse_body(body_node);
    if (!rb.program) {
      throw CulebraError("InternalError",
                         "effects transform could not re-parse a body", 0, 0);
    }
    EffectsLowerer sub(rb.source->data(), rb.source->size(), effect_fns_);
    return sub.build_class_from_program(class_name, *rb.program, param_names);
  }

  std::string build_class_from_program(
      const std::string& class_name, const peg::Ast& program,
      const std::vector<std::string_view>& param_names) const {
    auto locals = collect_local_names(program);
    std::set<std::string> rewrite = locals;
    for (auto& p : param_names) rewrite.insert(std::string(p));

    std::string ctor_params, ctor_call_args;
    std::string ctor_inits =
        "      this._state = 0\n"
        "      this._eff_val = nil\n"
        "      this._eff_op = nil\n"
        "      this._eff_args = nil\n"
        "      this._eff_delegate = nil\n";
    emit_ctor_param_and_local_inits(param_names, locals, ctor_params,
                                    ctor_call_args, ctor_inits);

    std::string step = build_step(program, rewrite);
    return std::format(
        "  class {0} {{\n"
        "    new({1}) {{\n{2}    }}\n"
        "    _step(_rv) {{\n{3}    }}\n"
        "  }}\n",
        class_name, ctor_params, ctor_inits, step);
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
    std::string cls = build_computation_class(class_name, body, param_names);

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

  // `handle { BODY } with op(params) { H }` ->
  //   __Eff.handle(
  //     (fn() { class _EffBody…; _EffBody….new() })(),
  //     "op",
  //     fn(_eh_args, _eh_resume) { <bind op-args>; let <resume> = _eh_resume; H })
  std::shared_ptr<peg::Ast> lower_handle(std::shared_ptr<peg::Ast> ast) {
    using namespace peg::udl;
    const auto& body = *ast->nodes[0];
    std::string op = std::string(ast->nodes[1]->token);
    const auto& params = *ast->nodes[2];
    const auto& handler_body = *ast->nodes[3];

    auto class_name = std::format("_EffBody_{}_{}", ast->line, ast->column);
    std::string cls = build_computation_class(class_name, body, {});

    // Handler adapter: leading params bind op args from _eh_args[i]; the last
    // param is the resume continuation.
    if (params.nodes.empty()) {
      throw CulebraError(
          "SyntaxError",
          "a handler clause needs a `resume` parameter (`with op(resume) "
          "{ … }`).",
          static_cast<long>(ast->line), static_cast<long>(ast->column));
    }
    std::string binds;
    size_t nparams = params.nodes.size();
    for (size_t i = 0; i + 1 < nparams; i++) {
      auto pn = view_parameter(*params.nodes[i]).name;
      binds += std::format("    let {} = _eh_args[{}]\n", std::string(pn), i);
    }
    auto resume_name = view_parameter(*params.nodes[nparams - 1]).name;
    binds += std::format("    let {} = _eh_resume\n", std::string(resume_name));
    auto handler_src =
        strip_block_braces(slice(handler_body));

    auto synth = std::make_shared<std::string>(std::format(
        "fn __eff_handle_wrapper__() {{\n"
        "  __Eff.handle(\n"
        "    (fn() {{\n{0}    {1}.new()\n    }})(),\n"
        "    \"{2}\",\n"
        "    fn(_eh_args, _eh_resume) {{\n{3}    {4}\n    }})\n"
        "}}\n",
        cls, class_name, op, binds, std::string(handler_src)));
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
