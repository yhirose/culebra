#pragma once

// Static lint framework. Walks each module AST once before evaluation,
// modeling culebra's exact lexical scoping, and reports diagnostics.
// Error-severity findings abort before eval (sound: only failures the
// runtime is certain to raise); warnings are advisory. This is the shared
// home for static checks that today live ad hoc in the interp
// (check_shadow_static) and JIT (collect_fn_locals) — new rules register
// over one walk. First rule: reassignment of an immutable (`let`) binding,
// hoisting the runtime ImmutableError to before eval for all 3 backends.

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
// against interpreter.h's `make_scope` sites: LEXICAL_SCOPE, FOR, MATCH
// arms, TRY/catch bodies, DEFER, and function/lambda/method bodies open a
// child scope; IF / WHILE share the enclosing one.
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

  void walk(const peg::Ast& node);
};

inline void ScopeWalker::walk(const peg::Ast& node) {
  switch (node.tag) {
    case "FUNCTION"_:
    case "LAMBDA"_: {
      auto fv = node.tag == "FUNCTION"_ ? culebra::view_function(node)
                                        : culebra::view_lambda(node);
      check_dup_params(*fv.params);
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
      // [condition, BLOCK]: WHILE shares the enclosing variable scope (no
      // push), but its body is inside the loop. The condition is evaluated
      // before the loop is entered, so it stays at the enclosing loop depth
      // (matches the JIT pushing loop_stack_ only around the body).
      if (node.nodes.size() < 2) { walk_children(node); return; }
      walk(*node.nodes[0]);
      LoopDepthGuard g(loop_depth_, loop_depth_ + 1);
      walk(*node.nodes[1]);
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
                    "`{}` (expected a fixed scalar: Float32/Float64/Int8/"
                    "Int16/Int32/Int64/Byte/Bool)",
                    class_name, mv.name, mv.type_annotation),
                static_cast<long>(mv.name_line),
                static_cast<long>(mv.name_col), Severity::Error});
          }
          if (mv.value) walk(*mv.value);
          continue;
        }
        check_dup_params(*mv.params);
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
    case "ASSIGNMENT"_: {
      auto av = culebra::view_assignment(node);
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

}  // namespace culebra::lint
