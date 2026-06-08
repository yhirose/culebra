#pragma once

// Static lint framework. Walks each module AST once before evaluation,
// modeling culebra's exact lexical scoping, and reports diagnostics.
// Error-severity findings abort before eval (sound: only failures the
// runtime is certain to raise); warnings are advisory. This is the shared
// home for static checks: rules over the `ScopeWalker` walk (let
// reassignment, break/continue/return placement, duplicate params,
// @packable field types) plus the self-contained shadow analyzer
// (`check_shadow`, moved here from the interp). The JIT still performs
// the equivalent shadow check inline during compilation (collect_fn_locals).

#include <format>
#include <set>
#include <string>
#include <vector>

#include "packable.h"
#include "parser.h"
#include "shared.h"

namespace culebra::lint {

enum class Severity { Error, Warning };

struct Diagnostic {
  std::string kind;       // CulebraError-compatible (e.g. "ImmutableError")
  std::string message;
  long line = 0;
  long col = 0;
  Severity severity = Severity::Error;
};

namespace _detail {

using namespace peg::udl;

inline bool is_sink(std::string_view s) { return s == "_"; }

// True when a `"..."` literal carries an interpolation (`{expr}`), so it is
// not a compile-time constant. Object-literal keys and match patterns are
// constant positions: a constant `"..."` (or `'...'`) is allowed there, but
// an interpolating one is rejected (the runtime value isn't known statically).
inline bool interpolated_string_has_expr(const peg::Ast& node) {
  if (node.tag != "INTERPOLATED_STRING"_) return false;
  for (const auto& c : node.nodes)
    if (c->tag == "INTERP_EXPR"_) return true;
  return false;
}

// Recursively flag interpolating `"..."` literals used as patterns (a match
// arm's pattern subtree — leaves plus nested ctor/array/object/tuple
// sub-patterns). Guards and arm bodies are walked normally, not here.
inline void check_pattern_const_strings(const peg::Ast& pat,
                                        std::vector<Diagnostic>& diags) {
  if (pat.tag == "INTERPOLATED_STRING"_) {
    if (interpolated_string_has_expr(pat)) {
      diags.push_back(Diagnostic{
          "SyntaxError",
          "interpolated string is not a constant pattern (use '...' or a guard)",
          static_cast<long>(pat.line), static_cast<long>(pat.column),
          Severity::Error});
    }
    return;
  }
  for (const auto& c : pat.nodes) check_pattern_const_strings(*c, diags);
}

// Over-approximate binding capture: insert every IDENTIFIER token under
// `node` into `out`. Used for params / patterns / catch vars, where
// over-binding only ever weakens detection (a missed reassignment), never
// produces a false positive — the soundness direction we want.
inline void collect_idents(const peg::Ast& node,
                           std::set<std::string, std::less<>>& out) {
  if (node.tag == "IDENTIFIER"_ && node.is_token) {
    if (!is_sink(node.token)) out.insert(std::string(node.token));
    return;
  }
  for (const auto& c : node.nodes) collect_idents(*c, out);
}

// One lexical scope, mirroring a runtime Environment: names bound
// immutably (`let`) vs mutably (`mut` / params / loop & catch vars).
struct Scope {
  std::set<std::string, std::less<>> lets;
  std::set<std::string, std::less<>> muts;
};

// Walks the AST modeling culebra's exact scope boundaries, verified
// against interpreter.h's `make_scope` sites: LEXICAL_SCOPE, FOR, WHILE
// body, MATCH arms, TRY/catch bodies, DEFER, and function/lambda/method
// bodies open a child scope; IF shares the enclosing one.
class ScopeWalker {
 public:
  explicit ScopeWalker(std::vector<Diagnostic>& diags) : diags_(diags) {}

  void run(const peg::Ast& ast) {
    scopes_.emplace_back();   // top-level / module scope
    walk(ast);
    scopes_.pop_back();
  }

 private:
  std::vector<Scope> scopes_;
  std::vector<Diagnostic>& diags_;
  int loop_depth_ = 0;

  // RAII override of loop_depth_ for the span of a body walk: a loop body
  // bumps it (+1), a function / lambda / method / class / trait body resets
  // it to 0 — break/continue cannot cross a function boundary, matching the
  // JIT's per-function loop_stack_ and every mainstream language. (The interp
  // instead lets a BreakSignal propagate dynamically through a call, the
  // divergence this rule removes.)
  struct LoopDepthGuard {
    int& slot;
    int saved;
    LoopDepthGuard(int& s, int v) : slot(s), saved(s) { slot = v; }
    ~LoopDepthGuard() { slot = saved; }
  };

  // Nesting depth inside function-like bodies (function / lambda / method /
  // defer closure). `return` is valid only when this is > 0; a top-level
  // `return` is a SyntaxError (docs §return; a defer's own `return` exits the
  // defer closure, so a defer body counts as a boundary).
  int fn_depth_ = 0;
  struct FnDepthGuard {
    int& slot;
    explicit FnDepthGuard(int& s) : slot(s) { ++slot; }
    ~FnDepthGuard() { --slot; }
  };

  // Innermost-first lookup: 'l' = let (immutable), 'm' = mutable, 0 = unknown.
  char classify(std::string_view name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
      if (it->muts.contains(name)) return 'm';
      if (it->lets.contains(name)) return 'l';
    }
    return 0;
  }

  // Walk `body` in a fresh child scope; `bind` populates names that belong
  // to the new scope (params, loop var, catch var) before the body.
  template <class Bind>
  void scoped(const peg::Ast& body, Bind&& bind) {
    scopes_.emplace_back();
    bind(scopes_.back());
    walk(body);
    scopes_.pop_back();
  }

  void walk_children(const peg::Ast& node) {
    for (const auto& c : node.nodes) walk(*c);
  }

  // Reject a duplicate parameter name in one parameter list. The earlier
  // binding is unreachable (calls bind last-wins), so it is always a mistake
  // — every mainstream language rejects it. `_` (sink) may repeat. Pattern
  // params (`fn ({a, b})`) are skipped (sound: only certain duplicates among
  // plain/rest params are flagged).
  void check_dup_params(const peg::Ast& params) {
    std::set<std::string, std::less<>> seen;
    for (const auto& p : params.nodes) {
      if (culebra::is_kw_only_sep(*p) || culebra::is_pattern_param(*p)) continue;
      auto loc = culebra::extract_param_name_loc(*p);
      if (is_sink(loc.name)) continue;
      if (!seen.insert(std::string(loc.name)).second) {
        diags_.push_back(Diagnostic{
            "SyntaxError",
            std::format("duplicate parameter '{}'", loc.name),
            static_cast<long>(loc.line), static_cast<long>(loc.column),
            Severity::Error});
      }
    }
  }

  // Reject a malformed parameter list — ordering rules that are certain to
  // fail. Faithfully mirrors the interpreter's parameter builder (the state
  // machine in interpreter.h `build_parameters`), matching its message and
  // position so every backend rejects the same shapes pre-eval. The JIT
  // historically checked only a subset (it silently accepted `**kw` not-last,
  // a duplicate `*`, and a bare trailing `*`); hoisting the full set here
  // closes those interp/JIT divergences. Stops at the first violation, as the
  // interp's throwing builder does.
  void check_param_wellformed(const peg::Ast& params) {
    bool seen_default = false, kw_only = false, seen_sep = false;
    bool seen_kwargs_rest = false, seen_args_rest = false;
    size_t kw_only_count = 0;
    auto err = [&](std::string msg, size_t line, size_t col) {
      diags_.push_back(Diagnostic{"SyntaxError", std::move(msg),
                                  static_cast<long>(line),
                                  static_cast<long>(col), Severity::Error});
    };
    for (const auto& node : params.nodes) {
      auto pv = culebra::view_parameter(*node);
      if (seen_kwargs_rest) {
        return err("'**' catch-all must be the last parameter", node->line,
                   node->column);
      }
      if (seen_args_rest) {
        return err("'*args' must be the last parameter", node->line,
                   node->column);
      }
      if (pv.is_kw_only_sep) {
        if (seen_sep) {
          return err("duplicate '*' keyword-only separator", node->line,
                     node->column);
        }
        seen_sep = true;
        kw_only = true;
        seen_default = false;  // keyword-only params may default freely
        continue;
      }
      if (pv.is_args_rest) {
        if (seen_sep) {
          return err("'*args' cannot follow a '*' separator", node->line,
                     node->column);
        }
        seen_args_rest = true;
        continue;
      }
      if (pv.is_kwargs_rest) {
        seen_kwargs_rest = true;
        continue;
      }
      if (pv.pattern) continue;
      if (pv.default_value) {
        seen_default = true;
      } else if (seen_default && !kw_only) {
        return err(
            std::format("non-default parameter '{}' follows a default parameter",
                        pv.name),
            pv.name_line, pv.name_col);
      }
      if (kw_only) kw_only_count++;
    }
    if (seen_sep && kw_only_count == 0 && !seen_kwargs_rest) {
      err("named arguments must follow '*' separator", params.line,
          params.column);
    }
  }

  void walk(const peg::Ast& node);
};

inline void ScopeWalker::walk(const peg::Ast& node) {
  switch (node.tag) {
    case "FUNCTION"_:
    case "LAMBDA"_: {
      auto fv = node.tag == "FUNCTION"_ ? culebra::view_function(node)
                                        : culebra::view_lambda(node);
      check_dup_params(*fv.params);
      check_param_wellformed(*fv.params);
      LoopDepthGuard g(loop_depth_, 0);
      FnDepthGuard fg(fn_depth_);
      scoped(*fv.body, [&](Scope& s) { collect_idents(*fv.params, s.muts); });
      return;
    }
    case "LEXICAL_SCOPE"_:   // LEXICAL_SCOPE <- BLOCK: a child variable scope
      scopes_.emplace_back();  // that shares the enclosing loop / control flow.
      walk_children(node);     // Walk the statements, not the node (recursion).
      scopes_.pop_back();
      return;
    case "DEFER"_: {
      // DEFER <- [BLOCK]: the body is a deferred thunk — a separate closure
      // in the JIT — so it is a function boundary for break/continue. A
      // `defer { break }` cannot reach the enclosing loop (the JIT segfaults
      // on it today, the interp silently propagates), so reset loop depth.
      // It is also a function boundary for `return` (which exits the defer
      // closure, docs §defer).
      scopes_.emplace_back();
      LoopDepthGuard g(loop_depth_, 0);
      FnDepthGuard fg(fn_depth_);
      walk_children(node);
      scopes_.pop_back();
      return;
    }
    case "WHILE"_: {
      // [condition, BLOCK]: the condition is evaluated before each iteration
      // in the enclosing scope; the body is a fresh child scope per iteration
      // (like FOR — see interpreter.h eval_while / jit.h compile_while). The
      // condition stays at the enclosing loop depth; the body is inside the
      // loop.
      if (node.nodes.size() < 2) { walk_children(node); return; }
      walk(*node.nodes[0]);
      LoopDepthGuard g(loop_depth_, loop_depth_ + 1);
      scoped(*node.nodes[1], [](Scope&) {});
      return;
    }
    case "FOR"_: {
      // [pattern, iterable, BLOCK]: iterable in the enclosing scope, loop
      // var + body in a child scope that is inside the loop.
      if (node.nodes.size() < 3) { walk_children(node); return; }
      walk(*node.nodes[1]);
      LoopDepthGuard g(loop_depth_, loop_depth_ + 1);
      scoped(*node.nodes[2],
             [&](Scope& s) { collect_idents(*node.nodes[0], s.muts); });
      return;
    }
    case "TRY"_: {
      // [tryBLOCK, catchID, catchBLOCK]
      if (node.nodes.size() < 3) { walk_children(node); return; }
      scoped(*node.nodes[0], [](Scope&) {});
      scoped(*node.nodes[2],
             [&](Scope& s) { collect_idents(*node.nodes[1], s.muts); });
      return;
    }
    case "MATCH"_: {
      // [subject, ARMS]; each arm = [PATTERN, (GUARD)?, EXPRESSION].
      if (node.nodes.size() < 2) { walk_children(node); return; }
      walk(*node.nodes[0]);
      for (const auto& arm : node.nodes[1]->nodes) {
        if (arm->nodes.empty()) continue;
        // A pattern is a constant position: reject interpolating `"..."`
        // patterns (the guard/body below may interpolate freely).
        check_pattern_const_strings(*arm->nodes[0], diags_);
        scopes_.emplace_back();
        collect_idents(*arm->nodes[0], scopes_.back().muts);
        for (size_t i = 1; i < arm->nodes.size(); i++) walk(*arm->nodes[i]);
        scopes_.pop_back();
      }
      return;
    }
    case "MULTIFN_DECL"_: {
      // [DECORATOR*, head, PARAMETERS, (RETURN_TYPE)?, BLOCK]
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t d = 0; d < i; d++) walk(*node.nodes[d]);   // decorators: outer
      if (i + 1 >= node.nodes.size()) { walk_children(node); return; }
      check_dup_params(*node.nodes[i + 1]);
      check_param_wellformed(*node.nodes[i + 1]);
      LoopDepthGuard g(loop_depth_, 0);
      FnDepthGuard fg(fn_depth_);
      scoped(*node.nodes.back(),
             [&](Scope& s) { collect_idents(*node.nodes[i + 1], s.muts); });
      return;
    }
    case "CLASS_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      bool is_packable = false;
      for (size_t d = 0; d < i; d++) {
        walk(*node.nodes[d]);
        if (culebra::is_packable_decorator(*node.nodes[d])) is_packable = true;
      }
      // `@packable` constrains typed fields to fixed scalar types. Validate
      // here so the error fires pre-eval on every backend at the field's
      // position (the layout calc in eval/compile is then only a safety net).
      auto class_name =
          culebra::parse_generic_head(node.nodes[i]->token).outer;
      // Method bodies and field initializers are function-boundary contexts.
      LoopDepthGuard g(loop_depth_, 0);
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto mv = culebra::view_method(*node.nodes[j]);
        if (mv.is_field || mv.is_typed_field) {
          if (is_packable && mv.is_typed_field &&
              !culebra::is_packable_type(mv.type_annotation)) {
            diags_.push_back(Diagnostic{
                "SyntaxError",
                std::format(
                    "@packable class `{}`: field `{}` has non-packable type "
                    "`{}` (expected a fixed scalar — Float32/Float64/Int8/"
                    "Int16/Int32/Int64/Byte/Bool — or FixedArray<scalar, N>)",
                    class_name, mv.name, mv.type_annotation),
                static_cast<long>(mv.name_line),
                static_cast<long>(mv.name_col), Severity::Error});
          }
          if (mv.value) walk(*mv.value);
          continue;
        }
        check_dup_params(*mv.params);
        check_param_wellformed(*mv.params);
        FnDepthGuard fg(fn_depth_);
        scoped(**mv.body, [&](Scope& s) { collect_idents(*mv.params, s.muts); });
      }
      return;
    }
    case "TRAIT_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t d = 0; d < i; d++) walk(*node.nodes[d]);
      LoopDepthGuard g(loop_depth_, 0);
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto tv = culebra::view_trait_method(*node.nodes[j]);
        check_dup_params(*tv.params);
        check_param_wellformed(*tv.params);
        if (!tv.body) continue;   // signature-only method: nothing to walk
        FnDepthGuard fg(fn_depth_);
        scoped(*tv.body, [&](Scope& s) { collect_idents(*tv.params, s.muts); });
      }
      return;
    }
    case "BREAK"_:
    case "CONTINUE"_:
      // Sound static check: a break/continue with no enclosing loop (within
      // the same function) is certain to fail. The JIT already raises this
      // at compile time; the interp throws an uncaught Break/ContinueSignal
      // and aborts the process. Hoisting it here gives all backends the same
      // SyntaxError + position before eval.
      if (loop_depth_ == 0) {
        diags_.push_back(Diagnostic{
            "SyntaxError",
            node.tag == "BREAK"_ ? "break outside loop" : "continue outside loop",
            static_cast<long>(node.line), static_cast<long>(node.column),
            Severity::Error});
      }
      return;
    case "RETURN"_:
      // `return` is valid only inside a function body (docs §return). A
      // top-level `return` is silently swallowed by the interp's module
      // ReturnValue catch today; the module interface is `export`, so the
      // returned value goes nowhere — hoist it to a SyntaxError like the
      // break/continue control-flow checks. Walk the return value (if any)
      // for nested diagnostics regardless.
      if (fn_depth_ == 0) {
        diags_.push_back(Diagnostic{
            "SyntaxError", "return outside function",
            static_cast<long>(node.line), static_cast<long>(node.column),
            Severity::Error});
      }
      walk_children(node);
      return;
    case "OBJECT"_: {
      // Object-literal keys are a constant position: a literal `"..."` key is
      // its constant text, but an interpolating one has no static key. Flag it
      // pre-eval on every backend (the eval/compile path would otherwise build
      // a dynamic key silently). Spread elements carry no key.
      for (const auto& prop : node.nodes) {
        if (prop->tag != "OBJECT_PROPERTY"_) continue;
        auto pv = culebra::view_object_property(*prop);
        if (interpolated_string_has_expr(*pv.key)) {
          diags_.push_back(Diagnostic{
              "SyntaxError",
              "interpolated string is not a constant object key (use '...' "
              "or assign with o[key] = value)",
              static_cast<long>(pv.key->line),
              static_cast<long>(pv.key->column), Severity::Error});
        }
      }
      walk_children(node);
      return;
    }
    case "ASSIGNMENT"_: {
      auto av = culebra::view_assignment(node);
      // Well-formedness — certain-to-fail shapes the interp's eval_assignment
      // throws before any write. The JIT mirrored the first two but lacked the
      // keyword-LHS check (it silently ran `if = 1`); hoisting all three here
      // makes every backend reject them pre-eval at the ASSIGNMENT node, which
      // is the position the interp ends up with. First match only, as the
      // interp's throwing path does.
      auto syntax = [&](const char* msg) {
        diags_.push_back(Diagnostic{"SyntaxError", msg,
                                    static_cast<long>(node.line),
                                    static_cast<long>(node.column),
                                    Severity::Error});
      };
      if (av.compound && (av.is_let || av.is_mut)) {
        syntax("compound assignment cannot declare a new variable.");
      } else if (av.compound && av.op_base == "??" && av.lvalcnt != 1) {
        syntax("`??=` is only supported on a simple variable target.");
      } else if (av.lvalcnt == 1 && node.nodes[av.lvaloff]->is_token &&
                 culebra::is_keyword(node.nodes[av.lvaloff]->token)) {
        syntax("left-hand side is invalid variable name.");
      }
      walk(*av.rhs);
      if (av.lvalcnt == 1) {
        const auto& lval = *node.nodes[av.lvaloff];
        if (lval.tag == "IDENTIFIER"_ && lval.is_token) {
          std::string_view name = lval.token;
          // `let mut x` carries both flags and is mutable, so check
          // is_mut first; only a bare `let` (no mut) is immutable.
          if (av.is_mut) {
            if (!is_sink(name)) scopes_.back().muts.insert(std::string(name));
          } else if (av.is_let) {
            if (!is_sink(name)) scopes_.back().lets.insert(std::string(name));
          } else if (!av.compound && classify(name) == 'l') {
            diags_.push_back(Diagnostic{
                "ImmutableError",
                std::format("cannot reassign '{}' (declared without 'mut')",
                            name),
                static_cast<long>(lval.line),
                static_cast<long>(lval.column), Severity::Error});
          }
        }
      } else {
        // Complex lvalue (index / property chain): walk its expression
        // parts; immutability of fields/elements is enforced at runtime.
        for (int k = 0; k < av.lvalcnt; k++) walk(*node.nodes[av.lvaloff + k]);
      }
      return;
    }
    case "DESTRUCTURE_ASSIGN"_: {
      // [LET, MUTABLE, pattern, EXPRESSION]. Bind pattern names mutable-side
      // (conservative — never flag a destructured-name reassignment), then
      // walk the RHS.
      walk(*node.nodes.back());
      if (node.nodes.size() >= 3)
        collect_idents(*node.nodes[2], scopes_.back().muts);
      return;
    }
    default:
      walk_children(node);
      return;
  }
}

// --- Static shadow analyzer ---
//
// Walks the AST once before eval, raising ShadowError at any binding
// site that would shadow a name from an enclosing function scope. This
// matches the JIT's compile-time check (jit.h `collect_fn_locals` /
// `visit_for_frees`) so both backends reject shadow violations
// uniformly — including dead code that never executes.
//
// `outer[0]` is the top-level scope: those names act as globals and
// may be shadowed freely. `outer[1..]` are enclosing function scopes
// whose names would be captured by the current function and so are
// off-limits for re-binding. The two-phase structure (collect a
// function's full local set, *then* descend into its nested functions)
// is load-bearing: a nested function shadows an outer binding even when
// that binding appears textually after the nested function.
//
// Throws directly via `throw_shadow_error` on the first violation —
// byte-identical to the interp's former `check_shadow_static`, which
// this replaces. (Kept as a self-contained walk rather than folded into
// the ScopeWalker rules above, whose single-pass block-granular model
// can't reproduce the collect-then-descend ordering without diverging.)
namespace shadow {

using NameSet = std::set<std::string, std::less<>>;
using OuterChain = std::vector<const NameSet*>;

inline void check(std::string_view name, size_t line, size_t col,
                  const OuterChain& outer) {
  if (is_sink(name)) return;
  for (size_t i = 1; i < outer.size(); i++) {
    if (outer[i]->contains(name)) {
      culebra::throw_shadow_error(name, line, col);
    }
  }
}

inline void check_pattern(const peg::Ast& pattern, const OuterChain& outer) {
  culebra::for_each_pattern_binding(
      pattern, [&](std::string_view name, size_t line, size_t col) {
        check(name, line, col, outer);
      });
}

void analyze_fn_body(const peg::Ast& params_ast, const peg::Ast& body_ast,
                     OuterChain& outer);

// Phase 1: walk this function's body, collecting binding sites into
// `locals` and checking shadow violations. Stops at nested function
// boundaries (recursed into in phase 2).
inline void collect_locals(const peg::Ast& node, NameSet& locals,
                           const OuterChain& outer) {
  using namespace peg::udl;
  if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) return;

  if (node.tag == "MATCH"_) {
    // Match-arm bindings are arm-scoped at runtime, but we register
    // them in the enclosing function's locals so nested closures
    // inside an arm body can capture them. This over-approximates
    // (a closure in a *different* arm would see the previous arm's
    // names too) but only affects free-var resolution, never the
    // shadow check itself — `check` looks at outer[1..] only.
    for (auto& arm : node.nodes[1]->nodes) {
      check_pattern(*arm->nodes[0], outer);
      culebra::for_each_pattern_binding(
          *arm->nodes[0], [&](std::string_view name, size_t, size_t) {
            locals.insert(std::string(name));
          });
    }
    // fall through to walk arm bodies
  }

  if (node.tag == "TRY"_) {
    auto& id = *node.nodes[1];
    auto name = std::string(id.token);
    check(name, id.line, id.column, outer);
    if (!is_sink(name)) locals.insert(name);
    // fall through
  }

  if (node.tag == "FOR"_) {
    auto& var = *node.nodes[0];
    // The loop variable may be a destructuring pattern (`for (i, x) in`).
    if (culebra::is_pattern_param(var)) {
      check_pattern(var, outer);
    } else {
      check(std::string(var.token), var.line, var.column, outer);
    }
    // FOR binding is block-scoped; deliberately not added to enclosing
    // locals so a same-named binding outside the loop doesn't see it.
    collect_locals(*node.nodes[1], locals, outer);
    collect_locals(*node.nodes[2], locals, outer);
    return;
  }

  if (node.tag == "ASSIGNMENT"_) {
    auto av = culebra::view_assignment(node);
    if (av.lvalcnt == 1 && !av.compound) {
      auto ident_node = node.nodes[av.lvaloff];
      if (ident_node->tag == "IDENTIFIER"_) {
        auto name = std::string(ident_node->token);
        if (av.is_let || av.is_mut) {
          check(name, ident_node->line, ident_node->column, outer);
          if (!is_sink(name)) locals.insert(name);
        } else {
          // Bare assignment is auto-local only when the name doesn't
          // already exist in any outer scope.
          bool in_outer = false;
          for (auto* s : outer) {
            if (s->contains(name)) {
              in_outer = true;
              break;
            }
          }
          if (!in_outer && !is_sink(name)) locals.insert(name);
        }
      }
    }
    collect_locals(*node.nodes.back(), locals, outer);
    return;
  }

  if (node.tag == "TRAIT_DECL"_) {
    // A trait binds no name in the value env (traits live in the trait
    // registry, not as a value), so nothing enters `locals`. Crucially we
    // do NOT fall through to the generic recursion: descending would
    // hoist every trait method's params and body-lets into the enclosing
    // function's local set, producing a false shadow report between
    // sibling methods. Method bodies are analyzed on their own by
    // descend_into_nested. Matches the JIT's collect_fn_locals.
    return;
  }

  if (node.tag == "CLASS_DECL"_ || node.tag == "MULTIFN_DECL"_ ||
      node.tag == "ENUM_DECL"_) {
    // Skip leading DECORATOR children (added grammar form
    // `@expr ... fn name() {...}` / `@expr ... class Name {...}`).
    // ENUM_DECL binds its enum name the same way (CLASS_HEAD node).
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
      collect_locals(*node.nodes[i], locals, outer);
      i++;
    }
    auto& id = *node.nodes[i];
    // Both CLASS_DECL and MULTIFN_DECL now use CLASS_HEAD, which may
    // carry Generic params (`Box<T>`, `min<T: Bound>`); strip via
    // parse_generic_head so the binding lives under the outer name.
    auto name = std::string(culebra::parse_generic_head(id.token).outer);
    check(name, id.line, id.column, outer);
    if (!is_sink(name)) locals.insert(name);
    return;
  }

  // IMPORT_STMT introduces a local binding for the imported namespace.
  if (node.tag == "IMPORT_STMT"_) {
    auto& id = *node.nodes[0];
    auto name = std::string(id.token);
    check(name, id.line, id.column, outer);
    if (!is_sink(name)) locals.insert(name);
    return;
  }

  // DESTRUCTURE_ASSIGN binds a pattern to a value.
  if (node.tag == "DESTRUCTURE_ASSIGN"_) {
    const auto& pattern = *node.nodes[1];
    check_pattern(pattern, outer);
    culebra::for_each_pattern_binding(
        pattern, [&](std::string_view name, size_t, size_t) {
          locals.insert(std::string(name));
        });
    collect_locals(*node.nodes[2], locals, outer);
    return;
  }

  for (auto& c : node.nodes) collect_locals(*c, locals, outer);
}

// Phase 2: descend into nested function bodies with the now-populated
// outer chain.
inline void descend_into_nested(const peg::Ast& node, const NameSet& my_locals,
                                OuterChain& outer) {
  using namespace peg::udl;

  if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) {
    // FUNCTION's RETURN_TYPE (if present) is metadata only — the
    // shadow walker only needs params + body, which `view_function`
    // resolves uniformly for both forms.
    auto fv = node.tag == "FUNCTION"_ ? culebra::view_function(node)
                                      : culebra::view_lambda(node);
    outer.push_back(&my_locals);
    analyze_fn_body(*fv.params, *fv.body, outer);
    outer.pop_back();
    return;
  }

  if (node.tag == "MULTIFN_DECL"_) {
    // [DECORATOR*, IDENTIFIER, PARAMETERS, (RETURN_TYPE)?, BLOCK]
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
      // Decorators are evaluated in the outer scope, not the fn's
      // inner scope — descend with the current `outer`.
      descend_into_nested(*node.nodes[i], my_locals, outer);
      i++;
    }
    outer.push_back(&my_locals);
    analyze_fn_body(*node.nodes[i + 1], *node.nodes.back(), outer);
    outer.pop_back();
    return;
  }

  if (node.tag == "DEFER"_) {
    // [BLOCK] — same scope rules as a 0-param nested function.
    outer.push_back(&my_locals);
    NameSet defer_locals;
    collect_locals(*node.nodes[0], defer_locals, outer);
    descend_into_nested(*node.nodes[0], defer_locals, outer);
    outer.pop_back();
    return;
  }

  if (node.tag == "CLASS_DECL"_) {
    // [DECORATOR*, CLASS_HEAD, METHOD ...]. Each METHOD is viewed
    // through `view_method` so size 3 (static field) and size 4
    // (method) share the same handling.
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
      descend_into_nested(*node.nodes[i], my_locals, outer);
      i++;
    }
    for (size_t j = i + 1; j < node.nodes.size(); j++) {
      auto mv = culebra::view_method(*node.nodes[j]);
      if (mv.is_field || mv.is_typed_field) {
        if (mv.value) descend_into_nested(*mv.value, my_locals, outer);
        continue;
      }
      outer.push_back(&my_locals);
      analyze_fn_body(*mv.params, **mv.body, outer);
      outer.pop_back();
    }
    return;
  }

  if (node.tag == "TRAIT_DECL"_) {
    // [DECORATOR*, CLASS_HEAD, TRAIT_METHOD ...] — each TRAIT_METHOD is
    // [IDENTIFIER, PARAMETERS, RETURN_TYPE?, TRAIT_BODY?]. Default-body
    // methods are nested fns whose body must pass the shadow check;
    // signature-only methods have no body to analyze.
    size_t i = 0;
    while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
      descend_into_nested(*node.nodes[i], my_locals, outer);
      i++;
    }
    for (size_t j = i + 1; j < node.nodes.size(); j++) {
      auto tv = culebra::view_trait_method(*node.nodes[j]);
      if (!tv.body) continue;
      outer.push_back(&my_locals);
      analyze_fn_body(*tv.params, *tv.body, outer);
      outer.pop_back();
    }
    return;
  }

  for (auto& c : node.nodes) descend_into_nested(*c, my_locals, outer);
}

inline void analyze_fn_body(const peg::Ast& params_ast,
                            const peg::Ast& body_ast, OuterChain& outer) {
  NameSet my_locals;
  for (auto& p : params_ast.nodes) {
    if (culebra::is_kw_only_sep(*p)) continue;
    if (culebra::is_pattern_param(*p)) {
      // Destructuring param (`fn ({a, b})`) — register every name the
      // pattern binds (not a single param node).
      check_pattern(*p, outer);
      culebra::for_each_pattern_binding(
          *p, [&](std::string_view nm, size_t, size_t) {
            if (!is_sink(nm)) my_locals.insert(std::string(nm));
          });
      continue;
    }
    auto [name_sv, line, col] = culebra::extract_param_name_loc(*p);
    auto name = std::string(name_sv);
    check(name, line, col, outer);
    if (!is_sink(name)) my_locals.insert(name);
  }
  collect_locals(body_ast, my_locals, outer);
  descend_into_nested(body_ast, my_locals, outer);
}

}  // namespace shadow

}  // namespace _detail

// Run all static lint checks over one module AST before evaluation.
// Throws the first Error-severity diagnostic as a CulebraError (same shape
// the runtime would raise), so the module loader surfaces it uniformly to
// every backend. Warnings are advisory (collected, not thrown).
inline void check_module(const peg::Ast& ast) {
  std::vector<Diagnostic> diags;
  _detail::ScopeWalker walker(diags);
  walker.run(ast);
  for (const auto& d : diags) {
    if (d.severity == Severity::Error) {
      throw culebra::CulebraError(d.kind, d.message, d.line, d.col);
    }
  }
}

// Static shadow check over an entire script AST before evaluation begins.
// Throws a `ShadowError` CulebraError on the first violation. `outer`
// starts empty — top-level names enter the chain only when a nested
// function pushes them, so the first scope (`outer[0]` from a nested
// function's perspective) is the top-level / "globals" frame which
// `shadow::check` skips. Invoked by the interpreter; the JIT performs the
// equivalent check inline during compilation (collect_fn_locals).
inline void check_shadow(const peg::Ast& ast) {
  _detail::shadow::NameSet top_locals;
  _detail::shadow::OuterChain outer;
  _detail::shadow::collect_locals(ast, top_locals, outer);
  _detail::shadow::descend_into_nested(ast, top_locals, outer);
}

}  // namespace culebra::lint
