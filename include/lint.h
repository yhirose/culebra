#pragma once

// Static lint framework. Walks each module AST once before evaluation,
// modeling culebra's exact lexical scoping, and reports diagnostics.
// Error-severity findings abort before eval (sound: only failures the
// runtime is certain to raise); warnings are advisory. This is the shared
// home for static checks: rules over the `ScopeWalker` walk (let
// reassignment, break/continue/return placement, duplicate params,
// @packable field types) plus the self-contained shadow analyzer
// (`check_shadow`, moved here from the interp). The JIT calls `check_shadow`
// too (from analyze_program), so the rule has a single source.

#include <algorithm>
#include <format>
#include <map>
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
  int64_t line = 0;
  int64_t col = 0;
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
// against the engines' scope-opening sites: LEXICAL_SCOPE, FOR, WHILE
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
  // compiled backends' per-function loop scoping and every mainstream
  // language. (The interp
  // completes them through its flow slot, which likewise travels only to the
  // nearest enclosing loop, so this rule is the same shape in both backends.)
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

  // Validate + register an INIT_CLAUSE's bindings (while / if / match init clauses)
  // into the current top scope. Each binding must declare with
  // `let` / `mut`; a bare `x = 0` would reassign an outer variable or make an
  // immutable binding, so reject it pre-eval on every backend — the caller has
  // already pushed the enclosing scope the bindings live in.
  void walk_init_clause(const peg::Ast& init) {
    for (const auto& binding : init.nodes) {
      bool declared = binding->nodes.size() >= 2 &&
                      (binding->nodes[0]->token == "let" ||
                       binding->nodes[1]->token == "mut");
      if (!declared) {
        diags_.push_back(Diagnostic{
            "SyntaxError",
            "init binding must be declared with 'let' or 'mut'",
            static_cast<long>(binding->line),
            static_cast<long>(binding->column), Severity::Error});
      }
      walk(*binding);   // registers the declared name in the enclosing scope
    }
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

  // Reject `fn` / `self` as a parameter name. Both are language-core
  // identifiers the callee binds unconditionally — `fn` is the implicit
  // recursion handle, `self` the method receiver (see always_bound) — so a
  // same-named parameter shadows that binding. Rejecting pre-eval keeps the
  // reserved names reserved on every backend: the interpreter and JIT
  // previously accepted such a parameter and silently let it shadow (a real
  // divergence risk, and on the JIT the overwritten implicit slot leaked its
  // ref every call). Pattern params carry no simple name, so skipping them is
  // sound.
  void check_reserved_params(const peg::Ast& params) {
    for (const auto& p : params.nodes) {
      if (culebra::is_kw_only_sep(*p) || culebra::is_pattern_param(*p)) continue;
      auto loc = culebra::extract_param_name_loc(*p);
      if (loc.name == "fn" || loc.name == "self") {
        diags_.push_back(Diagnostic{
            "SyntaxError",
            std::format("'{}' is a reserved name and cannot be used as a "
                        "parameter", loc.name),
            static_cast<long>(loc.line), static_cast<long>(loc.column),
            Severity::Error});
      }
    }
  }

  // Reject a malformed parameter list — ordering rules that are certain to
  // fail. Faithfully mirrors the interpreter's parameter builder (the state
  // machine the engines run when binding parameters), matching its message and
  // position so every backend rejects the same shapes pre-eval. The JIT
  // historically checked only a subset (it silently accepted `**kw` not-last,
  // a duplicate `*`, and a bare trailing `*`); hoisting the full set here
  // closes those interp/JIT divergences. Stops at the first violation, as the
  // interp's throwing builder does.
  void check_param_wellformed(const peg::Ast& params) {
    bool seen_default = false, kw_only = false, seen_sep = false;
    bool seen_kwargs_rest = false, seen_args_rest = false;
    size_t kw_only_count = 0;
    size_t positional = 0;  // declared slot index (the synthetic name's)
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
      // A destructuring parameter cannot carry a default, so it is a
      // required one and falls under the same ordering rule — the binders
      // count required slots by position, and a required slot after a
      // defaulted one has no arity that can fill it.
      if (pv.default_value) {
        seen_default = true;
      } else if (seen_default && !kw_only) {
        return err(
            std::format("non-default parameter '{}' follows a default parameter",
                        pv.pattern ? culebra::destructure_param_name(positional)
                                   : pv.name),
            pv.name_line ? pv.name_line : node->line,
            pv.name_line ? pv.name_col : node->column);
      }
      if (pv.pattern) {
        positional++;
        continue;
      }
      positional++;
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
      check_reserved_params(*fv.params);
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
      // [(INIT_CLAUSE)?, condition, BLOCK]: the condition is evaluated before
      // each iteration in the enclosing scope; the body is a fresh child scope
      // per iteration (like FOR — see the engines' while lowering /
      // jit.h compile_while). The condition stays at the enclosing loop depth;
      // the body is inside the loop.
      if (node.nodes.size() < 2) { walk_children(node); return; }
      auto wv = culebra::view_while(node);
      // The init clause (`while mut i = 0; …`) binds loop-scoped variables in
      // an enclosing scope wrapping the condition, body, and nobreak. Each
      // binding must be a declaration (`let`/`mut`); a bare `x = 0` would
      // reassign an outer variable or create an immutable binding — reject it
      // pre-eval on every backend, mirroring the break/continue check above.
      bool pushed = false;
      if (wv.init) {
        scopes_.emplace_back();
        pushed = true;
        walk_init_clause(*wv.init);
      }
      walk(*wv.cond);
      {
        LoopDepthGuard g(loop_depth_, loop_depth_ + 1);
        scoped(*wv.body, [](Scope&) {});
      }
      // The nobreak block runs after the loop, so a break/continue inside it
      // belongs to an enclosing loop — walk it at the *outer* loop depth (no
      // guard), in its own scope. It still sees the init scope, so keep it
      // before the pop.
      if (wv.nobreak) scoped(*wv.nobreak, [](Scope&) {});
      if (pushed) scopes_.pop_back();
      return;
    }
    case "IF"_: {
      // [(INIT_CLAUSE)?, cond, block, cond, block, …, (else-block)?]. IF shares
      // the enclosing scope (no per-branch scope), so with no init clause the
      // default child walk is correct. An init clause (`if mut x = f(); …`)
      // scopes its bindings to the whole chain: push a scope, register them,
      // then walk the arms (from arm_off, skipping the INIT_CLAUSE child).
      auto iv = culebra::view_if(node);
      if (!iv.init) { walk_children(node); return; }
      scopes_.emplace_back();
      walk_init_clause(*iv.init);
      for (size_t i = iv.arm_off; i < node.nodes.size(); i++)
        walk(*node.nodes[i]);
      scopes_.pop_back();
      return;
    }
    case "FOR"_: {
      // [pattern, iterable, BLOCK, (NOBREAK_CLAUSE)?]: iterable in the
      // enclosing scope, loop var + body in a child scope inside the loop. A
      // nobreak block runs after the loop (enclosing loop depth, own scope);
      // the loop variable is not visible there.
      if (node.nodes.size() < 3) { walk_children(node); return; }
      auto fv = culebra::view_for(node);
      walk(*fv.iter);
      {
        LoopDepthGuard g(loop_depth_, loop_depth_ + 1);
        scoped(*fv.body, [&](Scope& s) { collect_idents(*fv.binding, s.muts); });
      }
      if (fv.nobreak) scoped(*fv.nobreak, [](Scope&) {});
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
      // [(INIT_CLAUSE)?, subject, ARMS]; each arm = [PATTERN, (GUARD)?, EXPR].
      // An init clause (`match mut x = f(); x { … }`) scopes its bindings to the
      // subject and every arm: push an enclosing scope, register them, then walk
      // the subject/arms inside it. With no init clause `scope_pushed` is false
      // and this behaves exactly as before.
      auto mv = culebra::view_match(node);
      bool scope_pushed = mv.init != nullptr;
      if (scope_pushed) {
        scopes_.emplace_back();
        walk_init_clause(*mv.init);
      }
      walk(*mv.subject);
      for (const auto& arm : mv.arms->nodes) {
        if (arm->nodes.empty()) continue;
        // A pattern is a constant position: reject interpolating `"..."`
        // patterns (the guard/body below may interpolate freely).
        check_pattern_const_strings(*arm->nodes[0], diags_);
        scopes_.emplace_back();
        collect_idents(*arm->nodes[0], scopes_.back().muts);
        for (size_t i = 1; i < arm->nodes.size(); i++) walk(*arm->nodes[i]);
        scopes_.pop_back();
      }
      if (scope_pushed) scopes_.pop_back();
      return;
    }
    case "MULTIFN_DECL"_: {
      // [DECORATOR*, head, PARAMETERS, (RETURN_TYPE)?, BLOCK]
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t d = 0; d < i; d++) walk(*node.nodes[d]);   // decorators: outer
      if (i + 1 >= node.nodes.size()) { walk_children(node); return; }
      check_dup_params(*node.nodes[i + 1]);
      check_reserved_params(*node.nodes[i + 1]);
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
        // Four names are derivable. Reported here for the same reason the
        // @packable field types are: pre-eval on every backend, so the
        // diagnostic does not depend on whether the declaration ran. The
        // decorator's own position is where the backends' safety-net throw
        // lands (positionless, stamped at the declaration).
        for (auto trait : culebra::view_derive(*node.nodes[d])) {
          if (culebra::find_derive_method(trait)) continue;
          diags_.push_back(Diagnostic{
              "SyntaxError", culebra::derive_unknown_trait_message(trait),
              static_cast<long>(node.nodes[d]->line),
              static_cast<long>(node.nodes[d]->column), Severity::Error});
        }
      }
      // `@packable` constrains typed fields to fixed scalar types. Validate
      // here so the error fires pre-eval on every backend at the field's
      // position (the layout calc in eval/compile is then only a safety net).
      auto class_name =
          culebra::parse_generic_head(node.nodes[i]->token).outer;
      // Method bodies and field initializers are function-boundary contexts.
      LoopDepthGuard g(loop_depth_, 0);
      std::vector<std::pair<std::string, std::string>> pk_fields;
      bool pk_ok = true;
      // Member-name rules in a class body. Both instance AND static methods
      // MAY overload — same name, distinct positional-param-type signatures
      // merge into one multidispatch dispatcher at eval/compile time.
      // Everything else stays an error (the later definition would be silently
      // dead): a duplicate field, a field/method name clash (instance or
      // static), or two methods (instance or static) with an identical
      // signature (unreachable / ambiguous dispatch). Static and instance
      // members live in different places (the class object vs instances), so
      // each tracks its own field set and method table.
      auto method_signature = [](const peg::Ast& params) {
        // Mirror the runtime's MultiMethod extraction: only regular
        // positional params (before any `*` kw-only separator, excluding
        // `**rest`) score; `*args` makes it variadic. Canonicalize types so
        // `Long|Float` and `Long | Float` compare equal.
        std::string sig;
        bool kw_region = false;
        for (const auto& pn : params.nodes) {
          auto pv = culebra::view_parameter(*pn);
          if (pv.is_kw_only_sep) { kw_region = true; continue; }
          if (pv.is_kwargs_rest || kw_region) continue;
          if (pv.is_args_rest) { sig += "*,"; continue; }
          sig += culebra::canonicalize_type_annotation(pv.type_annotation);
          sig += ',';
        }
        return sig;
      };
      auto dup_member = [&](const culebra::MethodView& mv) {
        diags_.push_back(Diagnostic{
            "SyntaxError",
            std::format("duplicate member '{}' in class `{}`", mv.name,
                        class_name),
            static_cast<long>(mv.name_line),
            static_cast<long>(mv.name_col), Severity::Error});
      };
      std::set<std::string, std::less<>> inst_fields, static_fields, new_sigs;
      std::map<std::string, std::set<std::string>, std::less<>> inst_methods,
          static_methods;
      auto dup_sig = [&](const culebra::MethodView& mv) {
        diags_.push_back(Diagnostic{
            "SyntaxError",
            std::format("duplicate method '{}' with identical signature "
                        "in class `{}`",
                        mv.name, class_name),
            static_cast<long>(mv.name_line),
            static_cast<long>(mv.name_col), Severity::Error});
      };
      // A member's name-uniqueness rule against its own scope's field set and
      // method table. A field must be unique and not shadow a method; a method
      // may overload (distinct signatures) but not shadow a field or repeat a
      // signature. Shared by the static (class object) and instance scopes so
      // the two stay a single source of truth.
      auto check_named = [&](std::set<std::string, std::less<>>& fields,
                             std::map<std::string, std::set<std::string>,
                                      std::less<>>& methods,
                             const culebra::MethodView& mv,
                             bool is_field_member) {
        if (is_field_member) {
          if (!fields.insert(std::string(mv.name)).second ||
              methods.count(mv.name)) {
            dup_member(mv);
          }
        } else if (fields.count(mv.name)) {
          dup_member(mv);
        } else if (!methods[std::string(mv.name)]
                        .insert(method_signature(*mv.params))
                        .second) {
          dup_sig(mv);
        }
      };
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto mv = culebra::view_method(*node.nodes[j]);
        bool is_field_member = mv.is_field || mv.is_typed_field;
        // A getter is read as a property, so it has no call site to take
        // arguments at — shared message with the evaluator-side safety net
        // (require_getter_no_params).
        if (mv.is_getter && culebra::getter_takes_params(mv)) {
          diags_.push_back(Diagnostic{
              "SyntaxError",
              culebra::getter_params_message(mv.name, class_name),
              static_cast<long>(mv.name_line),
              static_cast<long>(mv.name_col), Severity::Error});
        }
        // Static members live on the class object, instance members on
        // instances — each name space is checked independently. Constructors
        // (`new`) overload like methods — distinct signatures merge, an
        // identical signature is an unreachable-overload error. Operator/
        // dunder methods (`__add__`, `__eq__`, `__call__`, …) are ordinary
        // instance methods reached through the operator-lookup path, so they
        // overload on the same rule as any instance method (the retrieved
        // dispatcher scores on the operand type).
        if (mv.is_static) {
          check_named(static_fields, static_methods, mv, is_field_member);
        } else if (!is_field_member && mv.name == "new") {
          if (!new_sigs.insert(method_signature(*mv.params)).second) dup_sig(mv);
        } else {
          check_named(inst_fields, inst_methods, mv, is_field_member);
        }
        if (mv.is_field || mv.is_typed_field) {
          // An untyped instance field carries no type for the byte layout —
          // shared message with the evaluator-side safety net
          // (require_typed_packable_field).
          if (is_packable && mv.is_field && !mv.is_static) {
            diags_.push_back(Diagnostic{
                "SyntaxError",
                culebra::packable_untyped_field_message(mv.name, class_name),
                static_cast<long>(mv.name_line),
                static_cast<long>(mv.name_col), Severity::Error});
            pk_ok = false;
          }
          if (is_packable && mv.is_typed_field) {
            if (!culebra::is_packable_type(mv.type_annotation)) {
              diags_.push_back(Diagnostic{
                  "SyntaxError",
                  std::format(
                      "@packable class `{}`: field `{}` has non-packable type "
                      "`{}` (expected a fixed scalar — Float32/Float64/Int8/"
                      "Int16/Int32/Int64/Byte/Bool — or FixedArray<scalar, N> / "
                      "FixedString<N> / FixedSet<scalar, N> / "
                      "FixedMap<scalar, scalar, N> / Bytes<N> / an optional "
                      "scalar `T?` / a @packable enum or nested @packable class)",
                      class_name, mv.name, mv.type_annotation),
                  static_cast<long>(mv.name_line),
                  static_cast<long>(mv.name_col), Severity::Error});
              pk_ok = false;
            } else {
              pk_fields.emplace_back(std::string(mv.name),
                                     std::string(mv.type_annotation));
            }
          }
          if (mv.value) walk(*mv.value);
          continue;
        }
        check_dup_params(*mv.params);
        check_reserved_params(*mv.params);
        check_param_wellformed(*mv.params);
        FnDepthGuard fg(fn_depth_);
        scoped(**mv.body, [&](Scope& s) { collect_idents(*mv.params, s.muts); });
      }
      // Register the @packable class layout pre-eval so a later class that
      // nests this one (`inner: This`) validates its field type here.
      if (is_packable && pk_ok) {
        culebra::register_packable_layout(
            std::string(class_name),
            culebra::compute_packable_layout(std::string(class_name),
                                             pk_fields));
      }
      return;
    }
    case "ENUM_DECL"_: {
      // A `@packable` enum registers its tagged-union layout here, pre-eval,
      // so a later `@packable` class field of this enum type validates (the
      // registry is the single source the field check below consults). A
      // non-scalar payload is the @packable constraint surfacing at the
      // variant position.
      size_t i = culebra::first_non_decorator_index(node);
      auto enum_name =
          std::string(culebra::parse_generic_head(node.nodes[i]->token).outer);
      bool is_packable = false;
      for (size_t d = 0; d < i; d++) {
        walk(*node.nodes[d]);
        if (culebra::is_packable_decorator(*node.nodes[d])) is_packable = true;
        // `@derive` has nothing to generate for an enum; reported pre-eval
        // at the decorator, like the class form's unknown-trait check.
        if (!culebra::view_derive(*node.nodes[d]).empty()) {
          diags_.push_back(Diagnostic{
              "SyntaxError", culebra::derive_on_enum_message(enum_name),
              static_cast<long>(node.nodes[d]->line),
              static_cast<long>(node.nodes[d]->column), Severity::Error});
        }
      }
      if (is_packable) {
        std::vector<std::pair<std::string, std::vector<std::string>>> variants;
        bool ok = !(i + 1 >= node.nodes.size());
        for (size_t j = i + 1; j < node.nodes.size(); j++) {
          auto vv = culebra::view_variant(*node.nodes[j]);
          std::vector<std::string> ftypes;
          for (auto t : vv.field_types) {
            if (culebra::packable_type_info(t).size == 0) {
              diags_.push_back(Diagnostic{
                  "SyntaxError",
                  std::format("@packable enum `{}`: variant `{}` payload type "
                              "`{}` is not a fixed scalar",
                              enum_name, vv.name, t),
                  static_cast<long>(vv.name_line),
                  static_cast<long>(vv.name_col), Severity::Error});
              ok = false;
            }
            ftypes.emplace_back(t);
          }
          variants.emplace_back(std::string(vv.name), std::move(ftypes));
        }
        if (ok) {
          culebra::register_packable_enum(
              enum_name, culebra::compute_packable_enum_layout(variants));
        }
      }
      walk_children(node);
      return;
    }
    case "TRAIT_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t d = 0; d < i; d++) walk(*node.nodes[d]);
      LoopDepthGuard g(loop_depth_, 0);
      auto trait_name = culebra::parse_generic_head(
          culebra::parse_trait_head(node.nodes[i]->token).name).outer;
      std::set<std::string, std::less<>> method_names;
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto tv = culebra::view_trait_method(*node.nodes[j]);
        // A trait's contract and its default bodies are both keyed by
        // method name — a trait has no overload set to merge same-name
        // methods into (a class does). Two `m` entries would silently drop
        // one, so reject the declaration instead.
        if (!method_names.insert(std::string(tv.name)).second) {
          diags_.push_back(Diagnostic{
              "SyntaxError",
              std::format("duplicate method '{}' in trait `{}`", tv.name,
                          trait_name),
              static_cast<long>(tv.name_line),
              static_cast<long>(tv.name_col), Severity::Error});
        }
        check_dup_params(*tv.params);
        check_reserved_params(*tv.params);
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
      // the same function) is certain to fail. The JIT raises it at compile
      // time and the interp would otherwise leave the completion pending with
      // no loop to consume it; hoisting the check here gives all backends the
      // same SyntaxError + position before eval.
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
      // keyword-LHS check (it silently ran `if = 1`); hoisting all of them here
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
      } else if (av.lvalcnt == 1 &&
                 !culebra::is_assignable_name(*node.nodes[av.lvaloff])) {
        // Same test as the interp's check_assignable_name. A lone
        // non-identifier target (`1 = 2`) carries an empty token, which
        // eval_assign_var read as the variable name and declared, so the write
        // landed on an unnameable slot instead of erroring.
        syntax("left-hand side is invalid variable name.");
      } else if (av.lvalcnt > 1 &&
                 node.nodes[av.lvaloff + av.lvalcnt - 1]->original_tag ==
                     "ARGUMENTS"_) {
        // `f() = v`: the final postfix is a call, which has no storage. Both
        // backends fell through their lvalue switch's default and aborted
        // with a non-CulebraError (interp `logic_error`, JIT `runtime_error`).
        syntax("cannot assign to a function call result.");
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
    case "PLACE_ASSIGN"_: {
      // [target..., EXPRESSION]. A chain target's subexpressions are ordinary
      // reads; a plain-name target binds mutable-side, like the pattern names
      // above (conservative — never flag its later reassignment).
      walk(*node.nodes.back());
      culebra::for_each_place_target(
          node,
          [&](const peg::Ast& chain) {
            // Same certain-to-fail shape the ASSIGNMENT case rejects: a call
            // has no storage to write.
            if (chain.nodes.back()->original_tag == "ARGUMENTS"_) {
              diags_.push_back(Diagnostic{
                  "SyntaxError", "cannot assign to a function call result.",
                  static_cast<long>(node.line),
                  static_cast<long>(node.column), Severity::Error});
            }
            walk(chain);
          },
          [&](const peg::Ast& name) {
            scopes_.back().muts.insert(std::string(name.token));
          });
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
    // The optional init clause's bindings are picked up by the generic
    // recursion below (its ASSIGNMENT children register as locals).
    for (auto& arm : culebra::view_match(node).arms->nodes) {
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
    auto fv = culebra::view_for(node);
    auto& var = *fv.binding;
    // The loop variable may be a destructuring pattern (`for (i, x) in`).
    if (culebra::is_pattern_param(var)) {
      check_pattern(var, outer);
    } else {
      check(std::string(var.token), var.line, var.column, outer);
    }
    // FOR binding is block-scoped; deliberately not added to enclosing
    // locals so a same-named binding outside the loop doesn't see it.
    collect_locals(*fv.iter, locals, outer);
    collect_locals(*fv.body, locals, outer);
    // A nobreak block shares the enclosing function scope; collect its locals.
    if (fv.nobreak) collect_locals(*fv.nobreak, locals, outer);
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

  // PLACE_ASSIGN: a plain-name target may declare, so it is a local of this
  // scope; a chain target only reads.
  if (node.tag == "PLACE_ASSIGN"_) {
    culebra::for_each_place_target(
        node,
        [&](const peg::Ast& chain) { collect_locals(chain, locals, outer); },
        [&](const peg::Ast& name) { locals.insert(std::string(name.token)); });
    collect_locals(*node.nodes.back(), locals, outer);
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

  if (node.tag == "FOR"_) {
    // [pattern/var, iterable, BLOCK, (NOBREAK)?]. The loop variable is
    // block-scoped to the body and is captured by closures defined there, so a
    // nested function inside the body must not shadow it — but code after the
    // loop never sees it. Descend the body with the loop binding(s) folded into
    // a body-local copy of the enclosing locals; the iterable is in the
    // enclosing scope. A nobreak block runs after the loop with the loop var out
    // of scope, so descend it with the plain enclosing locals.
    if (node.nodes.size() >= 3) {
      auto fv = culebra::view_for(node);
      descend_into_nested(*fv.iter, my_locals, outer);
      NameSet body_locals = my_locals;
      auto& var = *fv.binding;
      if (culebra::is_pattern_param(var)) {
        culebra::for_each_pattern_binding(
            var, [&](std::string_view name, size_t, size_t) {
              if (!is_sink(name)) body_locals.insert(std::string(name));
            });
      } else if (var.is_token && !is_sink(var.token)) {
        body_locals.insert(std::string(var.token));
      }
      descend_into_nested(*fv.body, body_locals, outer);
      if (fv.nobreak) descend_into_nested(*fv.nobreak, my_locals, outer);
      return;
    }
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

// --- Static undefined-variable analyzer (sound subset, pyflakes-style) ---
//
// Flags a bare IDENTIFIER *read* of a name bound in no enclosing scope, no
// module top-level binding, and not a builtin/namespace — the subset that
// is certain to raise NameError at runtime. Soundness (no false positives)
// comes from over-approximating the bound set: every scope's bindings are
// collected first (via the shadow analyzer's `collect_locals` with an empty
// outer chain, so it never throws and treats every bare assignment as a
// binding) and only then are its reads checked. A forward reference to a
// later top-level / sibling binding therefore never false-positives, and a
// name bound anywhere reachable is treated as defined (so genuine
// use-before-def, which is execution-order dependent, is deliberately not
// flagged — it is left to the runtime). Non-variable identifier positions —
// `.member` / `?.member`, kwarg names, object-literal keys, parameter and
// pattern bindings, declaration names, type annotations — are excluded
// structurally. Returns diagnostics; the caller decides whether to surface
// them or abort.
namespace undefined {

using NameSet = std::set<std::string, std::less<>>;
using Chain = std::vector<const NameSet*>;

// Names the runtime binds implicitly (parser.h holds the rule; the capture
// analysis asks the same question).
inline bool always_bound(std::string_view n) {
  return culebra::is_always_bound_name(n);
}

inline bool resolves(std::string_view name, const Chain& chain,
                     const NameSet& globals) {
  if (is_sink(name) || always_bound(name)) return true;
  // Separate from `globals`: that set is built once and cached, the ambients
  // are per run (shared.h).
  if (globals.contains(name) || culebra::is_ambient_global(name)) return true;
  for (const auto* frame : chain)
    if (frame->contains(name)) return true;
  return false;
}

// Collect every name bound directly within one scope into `out` —
// over-approximating across nested blocks / control flow that share the
// runtime function scope, but stopping at nested function / lambda / defer
// boundaries (each opens its own scope, collected when it is walked). FOR
// loop variables are block-scoped to the body and bound there by `walk`, so
// they are not collected here. Over-collection only ever weakens detection,
// never causes a false positive — the soundness direction we need. (The
// shadow analyzer's collector is deliberately not reused: it reads a
// destructure pattern from the wrong child index, which is harmless for
// shadow over-approximation but would drop `let (a, b) = …` bindings here.)
inline void collect_scope(const peg::Ast& node, NameSet& out) {
  using namespace peg::udl;
  switch (node.tag) {
    case "FUNCTION"_:
    case "LAMBDA"_:
    case "DEFER"_:
      return;  // nested scope — its locals belong to it, not this frame
    case "ASSIGNMENT"_: {
      auto av = culebra::view_assignment(node);
      if (av.lvalcnt == 1 && node.nodes[av.lvaloff]->tag == "IDENTIFIER"_) {
        std::string_view name = node.nodes[av.lvaloff]->token;
        if (!is_sink(name)) out.insert(std::string(name));
      }
      collect_scope(*av.rhs, out);
      return;
    }
    case "DESTRUCTURE_ASSIGN"_: {
      // [LET, MUTABLE, pattern, EXPRESSION] — same fixed layout the interp /
      // JIT / shadow walkers read as nodes[2] / nodes[3].
      culebra::for_each_pattern_binding(
          *node.nodes[2], [&](std::string_view n, size_t, size_t) {
            if (!is_sink(n)) out.insert(std::string(n));
          });
      collect_scope(*node.nodes[3], out);
      return;
    }
    case "PLACE_ASSIGN"_: {
      // A plain-name target declares when nothing visible holds the name, so
      // it belongs to this scope; a chain target only reads.
      culebra::for_each_place_target(
          node, [&](const peg::Ast& chain) { collect_scope(chain, out); },
          [&](const peg::Ast& name) { out.insert(std::string(name.token)); });
      collect_scope(*node.nodes.back(), out);
      return;
    }
    case "FOR"_:
      // Loop var is block-scoped (bound by walk); collect iterable + body
      // (+ a nobreak block, which shares the enclosing function scope).
      if (node.nodes.size() >= 3) {
        auto fv = culebra::view_for(node);
        collect_scope(*fv.iter, out);
        collect_scope(*fv.body, out);
        if (fv.nobreak) collect_scope(*fv.nobreak, out);
      } else {
        for (const auto& c : node.nodes) collect_scope(*c, out);
      }
      return;
    case "TRY"_:
      // [tryBLOCK, catchID, catchBLOCK] — the catch var binds in the scope.
      if (node.nodes.size() >= 3) {
        collect_scope(*node.nodes[0], out);
        if (node.nodes[1]->is_token && !is_sink(node.nodes[1]->token))
          out.insert(std::string(node.nodes[1]->token));
        collect_scope(*node.nodes[2], out);
      } else {
        for (const auto& c : node.nodes) collect_scope(*c, out);
      }
      return;
    case "MATCH"_:
      // Arm patterns bind names visible in the arm; collect them all. An init
      // clause's bindings (`match mut x = f(); x { … }`) are scoped to the whole
      // match, so collect them too (over-approximation, harmless).
      if (node.nodes.size() >= 2) {
        auto mv = culebra::view_match(node);
        if (mv.init) collect_scope(*mv.init, out);
        collect_scope(*mv.subject, out);
        for (const auto& arm : mv.arms->nodes) {
          if (arm->nodes.empty()) continue;
          culebra::for_each_pattern_binding(
              *arm->nodes[0], [&](std::string_view n, size_t, size_t) {
                if (!is_sink(n)) out.insert(std::string(n));
              });
          for (size_t k = 1; k < arm->nodes.size(); k++)
            collect_scope(*arm->nodes[k], out);
        }
      } else {
        for (const auto& c : node.nodes) collect_scope(*c, out);
      }
      return;
    case "MULTIFN_DECL"_:
    case "CLASS_DECL"_:
    case "ENUM_DECL"_: {
      // Binds its declared name (generics stripped) in this scope; the body
      // is a separate scope, not descended into here.
      size_t i = culebra::first_non_decorator_index(node);
      auto name =
          std::string(culebra::parse_generic_head(node.nodes[i]->token).outer);
      if (!is_sink(name)) out.insert(name);
      return;
    }
    case "TRAIT_DECL"_:
      return;  // a trait is not a value binding (it lives in the registry)
    case "IMPORT_STMT"_:
      if (!node.nodes.empty() && node.nodes[0]->is_token)
        out.insert(std::string(node.nodes[0]->token));
      return;
    default:
      for (const auto& c : node.nodes) collect_scope(*c, out);
      return;
  }
}

// Build one scope frame: its parameter names plus every name bound in its
// body (via collect_scope).
inline void build_frame(const peg::Ast* params, const peg::Ast& body,
                        NameSet& frame) {
  if (params) {
    for (const auto& p : params->nodes) {
      if (culebra::is_kw_only_sep(*p)) continue;
      if (culebra::is_pattern_param(*p)) {
        culebra::for_each_pattern_binding(
            *p, [&](std::string_view nm, size_t, size_t) {
              if (!is_sink(nm)) frame.insert(std::string(nm));
            });
        continue;
      }
      auto loc = culebra::extract_param_name_loc(*p);
      if (!is_sink(loc.name)) frame.insert(std::string(loc.name));
    }
  }
  collect_scope(body, frame);
}

void walk(const peg::Ast& node, Chain& chain, const NameSet& globals,
          std::vector<Diagnostic>& diags);

// Analyze a function-like scope: collect its frame, then walk parameter
// default expressions (evaluated in the function's own scope, with earlier
// params bound) and the body for reads against the extended chain.
inline void analyze_fn(const peg::Ast* params, const peg::Ast& body,
                       Chain& chain, const NameSet& globals,
                       std::vector<Diagnostic>& diags) {
  NameSet frame;
  build_frame(params, body, frame);
  chain.push_back(&frame);
  if (params) {
    for (const auto& p : params->nodes)
      if (const auto* def = culebra::extract_default_expr(*p))
        walk(*def, chain, globals, diags);
  }
  walk(body, chain, globals, diags);
  chain.pop_back();
}

inline void walk(const peg::Ast& node, Chain& chain, const NameSet& globals,
                 std::vector<Diagnostic>& diags) {
  using namespace peg::udl;
  // A `.member` / `?.member` postfix carries the member name as its token
  // (tag IDENTIFIER, original_tag DOT/SAFE_DOT) — never a variable read.
  if (node.original_tag == "DOT"_ || node.original_tag == "SAFE_DOT"_) return;

  switch (node.tag) {
    case "FUNCTION"_:
    case "LAMBDA"_: {
      auto fv = node.tag == "FUNCTION"_ ? culebra::view_function(node)
                                        : culebra::view_lambda(node);
      analyze_fn(fv.params, *fv.body, chain, globals, diags);
      return;
    }
    case "MULTIFN_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      // Leading decorators (`@packable`, `@derive(Eq, ...)`, `@test`, user
      // decorators) are not plain variable reads — their callee names live in
      // a separate space and trait-name arguments aren't values — so the sound
      // subset skips the decorator subtrees entirely.
      if (i + 1 >= node.nodes.size()) return;
      analyze_fn(node.nodes[i + 1].get(), *node.nodes.back(), chain, globals,
                 diags);
      return;
    }
    case "CLASS_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      // Leading decorators (`@packable`, `@derive(Eq, ...)`, `@test`, user
      // decorators) are not plain variable reads — their callee names live in
      // a separate space and trait-name arguments aren't values — so the sound
      // subset skips the decorator subtrees entirely.
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto mv = culebra::view_method(*node.nodes[j]);
        if (mv.is_field || mv.is_typed_field) {
          if (mv.value) walk(*mv.value, chain, globals, diags);
          continue;
        }
        analyze_fn(mv.params, **mv.body, chain, globals, diags);
      }
      return;
    }
    case "TRAIT_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      // Leading decorators (`@packable`, `@derive(Eq, ...)`, `@test`, user
      // decorators) are not plain variable reads — their callee names live in
      // a separate space and trait-name arguments aren't values — so the sound
      // subset skips the decorator subtrees entirely.
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto tv = culebra::view_trait_method(*node.nodes[j]);
        if (tv.body) analyze_fn(tv.params, *tv.body, chain, globals, diags);
      }
      return;
    }
    case "ENUM_DECL"_:
      // Variant names + field type tokens only — nothing to read, and the
      // variant-name identifiers must NOT be treated as reads, so don't
      // descend (decorators aren't reads either).
      return;
    case "DEFER"_:
      // The deferred block is a nested 0-parameter scope.
      if (!node.nodes.empty())
        analyze_fn(nullptr, *node.nodes[0], chain, globals, diags);
      return;
    case "FOR"_: {
      // [binding, iterable, body, (NOBREAK)?]: the loop variable is
      // block-scoped to the body (the shadow collector deliberately omits it),
      // so bind it in a child frame for the body walk. The iterable is in the
      // enclosing scope. A nobreak block runs after the loop with the loop var
      // out of scope, so walk it in the enclosing chain.
      if (node.nodes.size() < 3) {
        for (const auto& c : node.nodes) walk(*c, chain, globals, diags);
        return;
      }
      auto fv = culebra::view_for(node);
      walk(*fv.iter, chain, globals, diags);
      NameSet loopvars;
      const auto& var = *fv.binding;
      if (culebra::is_pattern_param(var)) {
        culebra::for_each_pattern_binding(
            var, [&](std::string_view n, size_t, size_t) {
              if (!is_sink(n)) loopvars.insert(std::string(n));
            });
      } else if (var.is_token && !is_sink(var.token)) {
        loopvars.insert(std::string(var.token));
      }
      chain.push_back(&loopvars);
      walk(*fv.body, chain, globals, diags);
      chain.pop_back();
      if (fv.nobreak) walk(*fv.nobreak, chain, globals, diags);
      return;
    }
    case "TRY"_: {
      // [tryBLOCK, catchID, catchBLOCK]: the catch var was collected; skip it.
      if (node.nodes.size() < 3) {
        for (const auto& c : node.nodes) walk(*c, chain, globals, diags);
        return;
      }
      walk(*node.nodes[0], chain, globals, diags);
      walk(*node.nodes[2], chain, globals, diags);
      return;
    }
    case "MATCH"_: {
      // [(INIT_CLAUSE)?, subject, ARMS]; each arm = [PATTERN, (GUARD)?, body].
      // Pattern bindings were collected; skip the pattern, walk guard + body.
      // An init clause's bindings were collected too (their names resolve via
      // the enclosing locals); walk the clause so its RHS reads are checked.
      if (node.nodes.size() < 2) {
        for (const auto& c : node.nodes) walk(*c, chain, globals, diags);
        return;
      }
      auto mv = culebra::view_match(node);
      if (mv.init) walk(*mv.init, chain, globals, diags);
      walk(*mv.subject, chain, globals, diags);
      for (const auto& arm : mv.arms->nodes)
        for (size_t k = 1; k < arm->nodes.size(); k++)
          walk(*arm->nodes[k], chain, globals, diags);
      return;
    }
    case "ASSIGNMENT"_: {
      auto av = culebra::view_assignment(node);
      // A simple `x` target is a binding/write, not a read. A complex lvalue
      // (`o[k]` / `o.p`) reads its base object and any index expression, so
      // walk the chain (the DOT member is skipped by its own guard above).
      if (!(av.lvalcnt == 1 && node.nodes[av.lvaloff]->tag == "IDENTIFIER"_)) {
        for (int k = 0; k < av.lvalcnt; k++)
          walk(*node.nodes[av.lvaloff + k], chain, globals, diags);
      }
      walk(*av.rhs, chain, globals, diags);
      return;
    }
    case "DESTRUCTURE_ASSIGN"_:
      // [LET, MUTABLE, pattern, EXPRESSION]: pattern names were collected;
      // walk only the RHS.
      walk(*node.nodes.back(), chain, globals, diags);
      return;
    case "PLACE_ASSIGN"_:
      // Plain-name targets were collected by collect_scope (they may declare),
      // so only the chain targets' subexpressions and the RHS are reads.
      culebra::for_each_place_target(
          node,
          [&](const peg::Ast& c) { walk(c, chain, globals, diags); },
          [](const peg::Ast&) {});
      walk(*node.nodes.back(), chain, globals, diags);
      return;
    case "KWARG"_:
      // [name, value]: the kwarg name is not a read — walk only the value.
      if (node.nodes.size() >= 2) walk(*node.nodes[1], chain, globals, diags);
      return;
    case "OBJECT_PROPERTY"_: {
      // Shorthand `{x}` reads x (key doubles as value); `{k: v}` reads only v.
      auto pv = culebra::view_object_property(node);
      walk(pv.is_shorthand ? *pv.key : *pv.value, chain, globals, diags);
      return;
    }
    case "IMPORT_STMT"_:
      return;  // binds a name from a string path — no variable reads
    case "IDENTIFIER"_:
      if (node.is_token && !resolves(node.token, chain, globals)) {
        diags.push_back(Diagnostic{
            "NameError", std::format("undefined variable '{}'", node.token),
            static_cast<long>(node.line), static_cast<long>(node.column),
            Severity::Error});
      }
      return;
    default:
      for (const auto& c : node.nodes) walk(*c, chain, globals, diags);
      return;
  }
}

inline void analyze_module(const peg::Ast& ast, const NameSet& globals,
                           std::vector<Diagnostic>& diags) {
  NameSet top;
  collect_scope(ast, top);
  Chain chain{&top};
  walk(ast, chain, globals, diags);
}

}  // namespace undefined

// --- Static unused-local analyzer (advisory, Warning severity) ---
//
// The dual of the undefined check: undefined flags a read with no binding;
// unused flags a `let`/`mut` binding with no read. MVP scope = local bindings
// inside a function-like body. Parameters and module top-level bindings are
// deliberately NOT flagged — unused params are common and intentional
// (callback arity, interface conformance) and top-level names may be exported.
//
// Soundness (no false positives) comes from over-approximating *uses*: a
// binding counts as used if its name appears in any read position anywhere in
// the enclosing function body — including nested closures, which capture it.
// Over-counting reads only ever suppresses a warning (the safe direction);
// the only reads we must never miss are genuine ones, so compound-assignment
// targets (`x += 1` reads x) and shorthand `{x}` are counted explicitly.
namespace unused {

using NameSet = std::set<std::string, std::less<>>;

struct Decl {
  std::string name;
  int64_t line;
  int64_t col;
};

// A leading underscore marks a binding as intentionally unused (the sink `_`
// and the `_name` convention shared by Rust / Python / Go and already used in
// the culebra corpus for drop-on-discard / side-effect-only bindings). Such
// names are never flagged.
inline bool ignored_unused(std::string_view name) {
  return name.starts_with("_");
}

// The single lvalue IDENTIFIER node of a plain `let`/`mut` declaration (one
// simple target, not a compound assignment), or nullptr when `node` is not
// such a declaration. Centralizes the ASSIGNMENT grammar shape so the several
// passes that pick out declarations don't each re-encode it (and drift when
// the grammar changes). Callers apply `ignored_unused` to the returned name.
inline const peg::Ast* let_decl_target(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag != "ASSIGNMENT"_) return nullptr;
  auto av = culebra::view_assignment(node);
  if (!(av.is_let || av.is_mut) || av.compound || av.lvalcnt != 1)
    return nullptr;
  const auto& t = *node.nodes[av.lvaloff];
  return (t.tag == "IDENTIFIER"_ && t.is_token) ? &t : nullptr;
}

// Every variable READ in a subtree, recursing through nested closures (they
// capture outer locals). Excludes the positions that are writes or non-reads:
// a plain `x = …` target (a write), a destructure pattern, a kwarg name, and a
// non-shorthand object key. A compound `x += …` target IS a read.
// A `.member` / `?.member` postfix name is deliberately COUNTED as a read:
// under UFCS, `receiver.name(args)` calls the free function `name`, so a name
// appearing only as `.name` may be the sole use of a binding. Over-counting
// reads only ever suppresses a warning (the sound direction for "unused"), so
// treating every `.name` as a potential use keeps the analysis free of false
// positives at the cost of occasionally missing a genuinely dead binding whose
// name coincides with a method call. Declaration heads and parameter names are
// likewise left in (harmless — it only suppresses warnings).
inline void collect_reads(const peg::Ast& node, NameSet& reads) {
  using namespace peg::udl;
  switch (node.tag) {
    case "IDENTIFIER"_:
      if (node.is_token && !is_sink(node.token))
        reads.insert(std::string(node.token));
      return;
    case "ASSIGNMENT"_: {
      auto av = culebra::view_assignment(node);
      bool simple_ident = av.lvalcnt == 1 &&
                          node.nodes[av.lvaloff]->tag == "IDENTIFIER"_;
      if (simple_ident) {
        // A plain write is not a read; a compound assignment reads first.
        if (av.compound) {
          const auto& t = *node.nodes[av.lvaloff];
          if (t.is_token && !is_sink(t.token))
            reads.insert(std::string(t.token));
        }
      } else {
        for (int k = 0; k < av.lvalcnt; k++)
          collect_reads(*node.nodes[av.lvaloff + k], reads);
      }
      collect_reads(*av.rhs, reads);
      return;
    }
    case "DESTRUCTURE_ASSIGN"_:
      collect_reads(*node.nodes.back(), reads);  // pattern = write; walk RHS
      return;
    case "PLACE_ASSIGN"_:
      // Plain-name target = write; a chain target reads its receiver and index.
      culebra::for_each_place_target(
          node, [&](const peg::Ast& c) { collect_reads(c, reads); },
          [](const peg::Ast&) {});
      collect_reads(*node.nodes.back(), reads);
      return;
    case "KWARG"_:
      if (node.nodes.size() >= 2) collect_reads(*node.nodes[1], reads);
      return;
    case "OBJECT_PROPERTY"_: {
      auto pv = culebra::view_object_property(node);
      collect_reads(pv.is_shorthand ? *pv.key : *pv.value, reads);
      return;
    }
    case "IMPORT_STMT"_:
      return;  // binds a name; its path is a string literal — no reads. An
               // `export { a, b }` is left to the default recursion so its
               // identifiers DO count as reads (an exported name is used).
    default:
      for (const auto& c : node.nodes) collect_reads(*c, reads);
  }
}

// The `let`/`mut` simple-identifier bindings declared directly in one
// function scope (descending through blocks / loops / if / try / match that
// share the scope, but stopping at nested function-like boundaries — each is
// its own scope, checked when it is walked). Compound assignments cannot
// declare (lint rejects `let x += 1` earlier), so a declaration is always a
// plain `let`/`mut`.
inline void collect_local_decls(const peg::Ast& node, std::vector<Decl>& out) {
  using namespace peg::udl;
  switch (node.tag) {
    case "FUNCTION"_:
    case "LAMBDA"_:
    case "DEFER"_:
    case "MULTIFN_DECL"_:
    case "CLASS_DECL"_:
    case "TRAIT_DECL"_:
    case "ENUM_DECL"_:
    // An `effect fn` body and each handler clause body lower to their own
    // functions, so their locals belong to those scopes — not the enclosing
    // one. (A HANDLE's own handled block, node[0], is NOT listed: it runs
    // inline in the enclosing scope, so we descend into it like any block.)
    case "EFFECT_FN_DECL"_:
    case "HANDLE_CLAUSE"_:
    case "RETURN_CLAUSE"_:
      return;  // separate scope
    case "ASSIGNMENT"_: {
      if (const auto* t = let_decl_target(node); t && !ignored_unused(t->token))
        out.push_back({std::string(t->token), static_cast<long>(t->line),
                       static_cast<long>(t->column)});
      collect_local_decls(*culebra::view_assignment(node).rhs, out);
      return;
    }
    default:
      for (const auto& c : node.nodes) collect_local_decls(*c, out);
  }
}

// Flag this function body's local `let`/`mut` bindings that are never read in
// the body (reads include nested closures, which capture them). Appends
// Warning diagnostics. Parameters are deliberately NOT checked: an unused
// parameter is overwhelmingly intentional in culebra — a multidispatch clause
// or trait/method signature fixes the arity, a higher-order callback (`fn(req)`
// route handler, `|i| 4.0`) ignores an argument it must still declare, and a
// type annotation exists to steer dispatch rather than to be read. Measured
// over the whole corpus, every such warning was a true-but-unwanted positive,
// so the check can't meet the linter's zero-false-positive bar.
inline void check_scope_body(const peg::Ast& body,
                             std::vector<Diagnostic>& diags) {
  std::vector<Decl> decls;
  collect_local_decls(body, decls);
  if (decls.empty()) return;
  NameSet reads;
  collect_reads(body, reads);
  for (const auto& d : decls) {
    if (!reads.contains(d.name)) {
      diags.push_back(Diagnostic{
          "UnusedVariable", std::format("unused variable '{}'", d.name),
          d.line, d.col, Severity::Warning});
    }
  }
}

// Walk the whole AST, checking each function-like body's local bindings. The
// module top level is intentionally not checked (its names may be exported).
inline void analyze_walk(const peg::Ast& node, std::vector<Diagnostic>& diags) {
  using namespace peg::udl;
  switch (node.tag) {
    case "FUNCTION"_:
    case "LAMBDA"_: {
      auto fv = node.tag == "FUNCTION"_ ? culebra::view_function(node)
                                        : culebra::view_lambda(node);
      check_scope_body(*fv.body, diags);
      break;
    }
    case "MULTIFN_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      if (i + 1 < node.nodes.size())
        check_scope_body(*node.nodes.back(), diags);
      break;
    }
    case "CLASS_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto mv = culebra::view_method(*node.nodes[j]);
        if (!mv.is_field && !mv.is_typed_field && mv.body)
          check_scope_body(**mv.body, diags);
      }
      break;
    }
    case "TRAIT_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto tv = culebra::view_trait_method(*node.nodes[j]);
        if (tv.body) check_scope_body(*tv.body, diags);
      }
      break;
    }
    case "DEFER"_:
      if (!node.nodes.empty()) check_scope_body(*node.nodes[0], diags);
      break;
    case "EFFECT_FN_DECL"_:
      // The body is the trailing BLOCK, absent for a signature-only operation.
      if (culebra::effect_fn_has_body(node))
        check_scope_body(*node.nodes.back(), diags);
      break;
    case "HANDLE"_: {
      // `handle BLOCK (with (RETURN_CLAUSE / HANDLE_CLAUSE))+` — node[0] is the
      // handled block (inline, checked as part of the enclosing scope); each
      // clause body is its own scope. HANDLE_CLAUSE is `IDENTIFIER PARAMETERS
      // BLOCK`, RETURN_CLAUSE is `PARAMETERS BLOCK`; the body is the last node.
      for (size_t j = 1; j < node.nodes.size(); j++) {
        const auto& clause = *node.nodes[j];
        if (!clause.nodes.empty())
          check_scope_body(*clause.nodes.back(), diags);
      }
      break;
    }
    default:
      break;
  }
  for (const auto& c : node.nodes) analyze_walk(*c, diags);
}

inline void analyze_module(const peg::Ast& ast,
                           std::vector<Diagnostic>& diags) {
  analyze_walk(ast, diags);
}

}  // namespace unused

// Unused top-level bindings: an `import`ed name or a top-level `let`/`mut`
// that the module never reads and never re-exports. Function / class / enum /
// trait declarations are intentionally not candidates — they are a module's
// export surface, plausibly used by importers this single-file check can't
// see. `let`/`mut` and imports are the private working set, so a truly unread
// one is dead. Soundness matches the unused-local check: reads are
// over-approximated module-wide (a name appearing anywhere as a read, even in
// a nested closure or `export { … }`, suppresses the warning), so the only
// flagged names are genuinely never referenced.
namespace toplevel {

using unused::ignored_unused;

struct Cand {
  std::string name;
  int64_t line;
  int64_t col;
  bool is_import;
};

// Candidates come only from the module's direct top-level statements (a `let`
// buried in a top-level `if`/`for` block is left to the runtime — keeping the
// first version to straight top-level statements avoids the scope subtleties
// of when a block shares the module frame).
inline void collect_candidates(const peg::Ast& root, std::vector<Cand>& out) {
  using namespace peg::udl;
  auto consider = [&](const peg::Ast& s) {
    if (s.tag == "IMPORT_STMT"_) {
      if (!s.nodes.empty() && s.nodes[0]->is_token &&
          !ignored_unused(s.nodes[0]->token)) {
        const auto& id = *s.nodes[0];
        out.push_back({std::string(id.token), static_cast<long>(id.line),
                       static_cast<long>(id.column), true});
      }
      return;
    }
    if (const auto* t = unused::let_decl_target(s);
        t && !ignored_unused(t->token))
      out.push_back({std::string(t->token), static_cast<long>(t->line),
                     static_cast<long>(t->column), false});
  };
  if (root.tag == "STATEMENTS"_)
    for (const auto& s : root.nodes) consider(*s);
  else
    consider(root);  // a single-statement program collapses past STATEMENTS
}

inline void analyze_module(const peg::Ast& root,
                           std::vector<Diagnostic>& diags) {
  std::vector<Cand> cands;
  collect_candidates(root, cands);
  if (cands.empty()) return;
  unused::NameSet reads;
  unused::collect_reads(root, reads);  // module-wide; exports count as reads
  for (const auto& c : cands) {
    if (reads.contains(c.name)) continue;
    std::string msg = c.is_import
                          ? std::format("unused import '{}'", c.name)
                          : std::format("unused top-level binding '{}'", c.name);
    diags.push_back(Diagnostic{c.is_import ? "UnusedImport" : "UnusedBinding",
                               std::move(msg), c.line, c.col,
                               Severity::Warning});
  }
}

}  // namespace toplevel

// Unreachable code: a statement that can never execute because a straight-line
// control-flow terminator (`return` / `throw` / `break` / `continue`)
// precedes it in the same statement block. Only bare terminators that are
// themselves direct statements count — a `return` nested inside an `if` does
// not make the rest of the enclosing block dead, so control-flow-aware
// analysis (both branches of an if/else terminating, etc.) is deliberately
// left to a later version.
namespace unreachable {

inline bool is_terminator(const peg::Ast& s) {
  using namespace peg::udl;
  return s.tag == "RETURN"_ || s.tag == "THROW"_ || s.tag == "BREAK"_ ||
         s.tag == "CONTINUE"_;
}

inline void analyze_walk(const peg::Ast& node, std::vector<Diagnostic>& diags) {
  using namespace peg::udl;
  if (node.tag == "STATEMENTS"_) {
    for (size_t i = 0; i + 1 < node.nodes.size(); i++) {
      if (is_terminator(*node.nodes[i])) {
        // The whole tail is dead, but one marker per block is enough — point
        // at the first statement that can never run.
        const auto& dead = *node.nodes[i + 1];
        diags.push_back(Diagnostic{"UnreachableCode", "unreachable code",
                                   static_cast<long>(dead.line),
                                   static_cast<long>(dead.column),
                                   Severity::Warning});
        break;
      }
    }
  }
  for (const auto& c : node.nodes) analyze_walk(*c, diags);
}

inline void analyze_module(const peg::Ast& ast,
                           std::vector<Diagnostic>& diags) {
  analyze_walk(ast, diags);
}

}  // namespace unreachable

// Idiom warnings: forms that run, mean exactly what the shorter spelling
// means, and have no reading under which leaving them is right. That last
// clause is the admission test — the same zero-false-positive bar the
// unused-parameter check failed above.
//
// Deliberately absent: an `if`/`else` a ternary *could* express (two long arms
// read better as a block) and a manual index `enumerate()` *could* replace (the
// index may be wanted for something else, or the walk may run backwards). Both
// are real advice and both have exceptions, so they belong in the prose
// (`docs/quick-guide.md` §3) rather than in a check that gates CI.
namespace idiom {

inline bool is_name(const peg::Ast& n, std::string_view name) {
  return n.name == "IDENTIFIER" && n.token == name;
}

// `x = x + 1` says `x += 1` the long way. Only a lone binary operand matches:
// `x = x - a + b` is *not* `x -= a + b`, and rather than re-derive precedence
// here the check simply declines to look at longer chains.
inline void check_self_assign(const peg::Ast& n, std::vector<Diagnostic>& diags) {
  if (n.name != "ASSIGNMENT") return;
  auto av = culebra::view_assignment(n);
  if (av.compound || av.lvalcnt != 1 || !av.type_annotation.empty()) return;
  const peg::Ast& target = *n.nodes[av.lvaloff];
  if (target.name != "IDENTIFIER") return;
  const peg::Ast& rhs = *av.rhs;
  if (rhs.name != "ADDITIVE" && rhs.name != "MULTIPLICATIVE") return;
  if (rhs.nodes.size() != 3) return;  // exactly `lhs op rhs`
  if (!is_name(*rhs.nodes[0], target.token)) return;
  std::string_view op = rhs.nodes[1]->token;
  diags.push_back(Diagnostic{
      "RedundantSelfAssign",
      std::format("'{0} = {0} {1} …' can be '{0} {1}= …'", target.token, op),
      static_cast<long>(n.line), static_cast<long>(n.column), Severity::Warning});
}

// A no-argument `.size()` at the end of a call chain.
inline bool is_size_call(const peg::Ast& n) {
  if (n.name != "CALL" || n.nodes.size() < 3) return false;
  const peg::Ast& dot = *n.nodes[n.nodes.size() - 2];
  const peg::Ast& args = *n.nodes[n.nodes.size() - 1];
  return dot.original_name == "DOT" && dot.token == "size" &&
         args.name == "ARG_LIST" && args.nodes.empty();
}

inline bool is_zero(const peg::Ast& n) {
  return n.name == "NUMBER" && n.token == "0";
}

// `xs.size() == 0` / `> 0` / `!= 0` — `empty()` answers the same question, and
// says which question it is.
inline void check_size_zero(const peg::Ast& n, std::vector<Diagnostic>& diags) {
  if (n.name != "CONDITION" || n.nodes.size() != 3) return;
  std::string_view op = n.nodes[1]->token;
  const peg::Ast& lhs = *n.nodes[0];
  const peg::Ast& rhs = *n.nodes[2];
  const bool size_left = is_size_call(lhs) && is_zero(rhs);
  const bool size_right = is_size_call(rhs) && is_zero(lhs);
  if (!size_left && !size_right) return;
  // `< 0` is never true and `>= 0` always is; those are a different bug, and
  // the rewrite suggested here would not be equivalent to either.
  const bool eq = op == "==";
  const bool ne = op == "!=";
  const bool gt = size_left ? op == ">" : op == "<";
  if (!eq && !ne && !gt) return;
  diags.push_back(Diagnostic{
      "SizeZeroComparison",
      eq ? "comparing .size() to 0 — use .empty()"
         : "comparing .size() to 0 — use !….empty()",
      static_cast<long>(n.line), static_cast<long>(n.column), Severity::Warning});
}

// `range(0, n)` is the range `range(n)` already describes.
inline void check_range_zero(const peg::Ast& n, std::vector<Diagnostic>& diags) {
  if (n.name != "CALL" || n.nodes.size() < 2) return;
  if (!is_name(*n.nodes[0], "range")) return;
  const peg::Ast& args = *n.nodes[1];
  if (args.name != "ARG_LIST" || args.nodes.size() < 2) return;
  if (!is_zero(*args.nodes[0])) return;
  diags.push_back(Diagnostic{
      "RangeZeroStart", "range(0, n) is range(n)",
      static_cast<long>(n.line), static_cast<long>(n.column), Severity::Warning});
}

// A fourth check — a callback written `fn (x) { expr }` that a lambda would
// say more briefly — was written and withdrawn. Over tests/ it fired 154
// times, and the ones worth acting on (`.filter(fn (x) { x > 1 })`) could not
// be told from the ones that were not: a body that is a single expression but
// spans several lines reads worse as `|x| …`, a zero-parameter callback would
// have to open with `||` (the logical-or token), and `fn () { throw … }` is a
// statement the optimizer merely collapses to look like an expression. Each
// exclusion is a judgement, which is the signature of a check that cannot meet
// the bar. The advice keeps its place in `docs/quick-guide.md` §3 instead.

inline void analyze_walk(const peg::Ast& node, std::vector<Diagnostic>& diags) {
  check_self_assign(node, diags);
  check_size_zero(node, diags);
  check_range_zero(node, diags);
  for (const auto& c : node.nodes) analyze_walk(*c, diags);
}

inline void analyze_module(const peg::Ast& ast, std::vector<Diagnostic>& diags) {
  analyze_walk(ast, diags);
}

}  // namespace idiom

// Non-exhaustive enum match: `match` returns `nil` on no arm matching
// (docs/language.md), so a missing variant fails silently instead of
// raising. Unlike Object shapes (unbounded key combinations, see
// docs/language.md's exhaustiveness rationale), an enum's variant set is
// closed by its declaration and every arm's pattern names its target
// directly — no type inference needed, purely a syntactic tally.
namespace enum_exhaustiveness {

// One file's enum declarations, gathered before any match is checked so a
// match can reference an enum declared later in the file (or never fully
// declared — those enums just never appear in `by_variant`).
struct Registry {
  // enum name -> its declared variant names.
  std::map<std::string, std::set<std::string, std::less<>>, std::less<>> variants;
  // variant name -> owning enum name, or "" when 2+ enums in this file
  // declare a variant with that name (unqualified references are then
  // ambiguous; see resolve() below).
  std::map<std::string, std::string, std::less<>> owner;
};

inline void collect_enums(const peg::Ast& node, Registry& reg) {
  using namespace peg::udl;
  if (node.tag == "ENUM_DECL"_) {
    size_t i = culebra::first_non_decorator_index(node);
    if (i < node.nodes.size()) {
      auto enum_name =
          std::string(culebra::parse_generic_head(node.nodes[i]->token).outer);
      auto& vs = reg.variants[enum_name];
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto vname = std::string(culebra::view_variant(*node.nodes[j]).name);
        vs.insert(vname);
        auto [it, first] = reg.owner.try_emplace(vname, enum_name);
        if (!first && it->second != enum_name) it->second.clear();
      }
    }
  }
  for (const auto& c : node.nodes) collect_enums(*c, reg);
}

// One `match`'s enum-coverage tally, built by folding every ungaurded arm's
// pattern(s) through `consider`.
struct MatchTally {
  const Registry& reg;
  std::string target;        // the one enum this match's patterns name
  bool catch_all = false;    // an arm matches unconditionally
  bool ambiguous = false;    // an unqualified name >1 enum in this file shares,
                             // or patterns naming more than one enum
  std::set<std::string, std::less<>> covered;

  // `name` is a variant, optionally `Enum.Variant`-qualified. A qualified
  // name trusts its own prefix over the file-wide registry when that prefix
  // really is a declared enum owning that variant — `Shape.Circle` means
  // Shape's Circle even if some unrelated enum also has a Circle variant
  // (this is also the runtime's own reading: try_pattern's CTOR_PATTERN case
  // discards the qualifier and matches by variant name alone).
  void resolve(std::string_view name) {
    auto dot = name.rfind('.');
    std::string variant(dot == std::string_view::npos ? name : name.substr(dot + 1));
    std::string owner;
    if (dot != std::string_view::npos) {
      std::string hint(name.substr(0, dot));
      auto it = reg.variants.find(hint);
      if (it != reg.variants.end() && it->second.count(variant)) owner = hint;
    }
    if (owner.empty()) {
      auto it = reg.owner.find(variant);
      if (it == reg.owner.end()) return;  // not a known variant at all
      if (it->second.empty()) { ambiguous = true; return; }
      owner = it->second;
    }
    if (!target.empty() && target != owner) { ambiguous = true; return; }
    target = owner;
    covered.insert(variant);
  }

  // One PRIMARY_PATTERN (already unwrapped from any top-level `|` OR).
  void consider(const peg::Ast& p) {
    using namespace peg::udl;
    if (p.tag == "WILDCARD"_ || p.tag == "IDENTIFIER"_) {
      catch_all = true;
      return;
    }
    if (p.tag == "CTOR_PATTERN"_ && !p.nodes.empty()) {
      resolve(p.nodes[0]->token);
      return;
    }
    if (p.tag == "TYPED_IDENT"_ && p.nodes.size() > 1) {
      std::string_view type_name = p.nodes[1]->token;
      // `x: Shape` names the enum itself, not one variant — matches every
      // instance of it, same as a bare catch-all.
      auto head = std::string(culebra::parse_generic_head(type_name).outer);
      if (reg.variants.count(head)) {
        if (!target.empty() && target != head) { ambiguous = true; return; }
        target = head;
        catch_all = true;
        return;
      }
      resolve(type_name);  // `x: Origin` / `x: Ok` — a nullary or named variant
      return;
    }
    // NIL/BOOLEAN/NUMBER/STRING/ARRAY_PATTERN/OBJECT_PATTERN/TUPLE_PATTERN —
    // not enum-related, neither covers a variant nor stands in for a catch-all.
  }
};

inline void check_match(const peg::Ast& node, const Registry& reg,
                        std::vector<Diagnostic>& diags) {
  using namespace peg::udl;
  if (reg.variants.empty()) return;  // no enum in this file to check against
  auto mv = culebra::view_match(node);
  MatchTally tally{reg};
  for (const auto& arm : mv.arms->nodes) {
    if (arm->nodes.empty()) continue;
    // A guarded arm may reject at runtime (`Circle(r) if r > 0 => …`), so it
    // cannot be trusted to fully dispose of what it names — conservatively,
    // a guarded arm contributes nothing (design note: "保守的には数えない").
    if (arm->nodes.size() > 1 && arm->nodes[1]->tag == "GUARD"_) continue;
    const auto& pattern = *arm->nodes[0];
    if (pattern.tag == "PATTERN"_ && !pattern.nodes.empty()) {
      for (const auto& sub : pattern.nodes) tally.consider(*sub);
    } else {
      tally.consider(pattern);
    }
  }
  if (tally.ambiguous || tally.catch_all || tally.target.empty()) return;
  const auto& all = reg.variants.at(tally.target);
  std::vector<std::string> missing;
  for (const auto& v : all)
    if (!tally.covered.count(v)) missing.push_back(v);
  if (missing.empty()) return;
  std::string list;
  for (size_t i = 0; i < missing.size(); i++) {
    if (i) list += ", ";
    list += missing[i];
  }
  diags.push_back(Diagnostic{
      "NonExhaustiveMatch",
      std::format("match on enum '{}' doesn't handle: {} (falls through to nil)",
                 tally.target, list),
      static_cast<long>(node.line), static_cast<long>(node.column),
      Severity::Warning});
}

inline void analyze_walk(const peg::Ast& node, const Registry& reg,
                         std::vector<Diagnostic>& diags) {
  using namespace peg::udl;
  if (node.tag == "MATCH"_) check_match(node, reg, diags);
  for (const auto& c : node.nodes) analyze_walk(*c, reg, diags);
}

inline void analyze_module(const peg::Ast& ast, std::vector<Diagnostic>& diags) {
  Registry reg;
  collect_enums(ast, reg);
  analyze_walk(ast, reg, diags);
}

}  // namespace enum_exhaustiveness

// Which source lines an autofix may delete outright. `culebra lint --fix`
// removes an unused `import` by dropping its line, so the line has to belong
// to that import alone: the grammar lets `;` join statements, and a dead
// import sharing a line with a live statement would be deleted along with it.
namespace autofix {

struct LineUse {
  int imports = 0;    // import statements starting on this line
  int foreign = 0;    // tokens on this line that belong to something else
  bool spills = false;  // an import here whose tokens run onto other lines
};

inline void note_spill(const peg::Ast& n, int64_t start, bool& spills) {
  if (n.nodes.empty()) {
    if (static_cast<long>(n.line) != start) spills = true;
    return;
  }
  for (const auto& c : n.nodes) note_spill(*c, start, spills);
}

inline void scan(const peg::Ast& n, std::map<long, LineUse>& out) {
  using namespace peg::udl;
  auto line = static_cast<long>(n.line);
  if (n.tag == "IMPORT_STMT"_) {
    auto& use = out[line];
    use.imports++;
    note_spill(n, line, use.spills);
    return;  // an import's own tokens aren't foreign to its line
  }
  if (n.nodes.empty()) {
    out[line].foreign++;
    return;
  }
  for (const auto& c : n.nodes) scan(*c, out);
}

}  // namespace autofix

}  // namespace _detail

// Cross-layer provider for the builtin / global names the undefined-var
// check treats as always defined. lint.h sits below the stdlib layer that
// owns the global environment, so that layer installs this hook (see
// `install_undefined_var_lint`); until it does, the undefined-var check is
// skipped — embedders that never build a stdlib environment simply don't
// get it. A captureless lambda / free function converts to this pointer.
inline const std::set<std::string, std::less<>>* (*builtin_names_hook)() =
    nullptr;

// The Error-severity static analyses, shared by the enforce path
// (`check_module`, run on every load) and the report path (`collect_module`,
// the `culebra lint` CLI). These are the checks the runtime is certain to
// raise: malformed control flow / declarations (ScopeWalker) and the sound
// undefined-variable subset. Appends to `diags`.
inline void run_error_checks(const peg::Ast& ast,
                             std::vector<Diagnostic>& diags) {
  _detail::ScopeWalker walker(diags);
  walker.run(ast);
  // Undefined-variable check (the sound subset that is certain to raise
  // NameError) — run only when the builtin-name provider is installed.
  if (builtin_names_hook) {
    if (const auto* globals = builtin_names_hook())
      _detail::undefined::analyze_module(ast, *globals, diags);
  }
}

// Run the load-stage static lint checks over one module AST before evaluation.
// Throws the first Error-severity diagnostic as a CulebraError (same shape
// the runtime would raise), so the module loader surfaces it uniformly to
// every backend. Advisory warnings (e.g. unused locals) are NOT run here —
// they would be discarded, and the load path stays free of their cost; the
// `culebra lint` CLI runs the full set via `collect_module`.
inline void check_module(const peg::Ast& ast) {
  std::vector<Diagnostic> diags;
  run_error_checks(ast, diags);
  for (const auto& d : diags) {
    if (d.severity == Severity::Error) {
      throw culebra::CulebraError(d.kind, d.message, d.line, d.col);
    }
  }
}

// Report mode for the `culebra lint` CLI / future LSP diagnostics: run every
// static analysis (the Error-severity load checks plus advisory Warnings like
// unused locals) and RETURN all diagnostics without throwing, so the caller
// can print them all. Diagnostics are sorted by source position.
//
// The two analyses need different views of the program, so the caller passes
// both:
//   `lowered`  — what the backends actually run: generators and effects
//                lowered to classes plus runtime calls, i.e. the output of
//                `parse_with_transforms`, which is also what the load-stage
//                `check_module` sees. Error checks must use this form to stay
//                sound — on the raw parse, `effect fn` and `handle … with`
//                introduce bindings no analyzer knows about, so every
//                operation, clause parameter and `resume` reads as undefined.
//   `authored` — the raw parse of the source the user typed. Advisory
//                warnings must use this form, because a synthesized binding
//                is not the user's to act on (an abort clause deliberately
//                never reads its `resume`; a clause may ignore an operation
//                argument) and synthesized positions collapse onto the
//                enclosing construct rather than pointing at editable source.
// The warning analyzers treat `effect fn` bodies and handler clause bodies as
// their own scopes (they lower to functions), so an unused local written in
// one is reported at its authored position, same as in any function.
// Report-mode wrapper for the shadow check (defined below): fills `diags`
// instead of throwing.
inline void collect_shadow(const peg::Ast& ast, std::vector<Diagnostic>& diags);

inline std::vector<Diagnostic> collect_module(const peg::Ast& lowered,
                                              const peg::Ast& authored) {
  std::vector<Diagnostic> diags;
  run_error_checks(lowered, diags);
  // Shadow is an error-severity static check the run path applies before eval;
  // like the other error checks it runs on the lowered AST (positions map back
  // to the authored source, same as when running the file).
  collect_shadow(lowered, diags);
  _detail::unused::analyze_module(authored, diags);
  _detail::toplevel::analyze_module(authored, diags);
  _detail::unreachable::analyze_module(authored, diags);
  _detail::idiom::analyze_module(authored, diags);
  _detail::enum_exhaustiveness::analyze_module(authored, diags);
  std::sort(diags.begin(), diags.end(), [](const Diagnostic& a,
                                           const Diagnostic& b) {
    return a.line != b.line ? a.line < b.line : a.col < b.col;
  });
  return diags;
}

// The lines `culebra lint --fix` may delete to remove an unused import: ones
// holding exactly one import statement, all of its tokens, and nothing else.
// An import that shares its line with another statement (`import A from 'a';
// f()`) is reported but left for the author — a line-wise delete would take
// the neighbour with it, and no re-parse check can notice, since dropping a
// statement leaves the rest of the file parsing and linting just as cleanly.
inline std::set<long> removable_import_lines(const peg::Ast& root) {
  std::map<long, _detail::autofix::LineUse> uses;
  _detail::autofix::scan(root, uses);
  std::set<long> out;
  for (const auto& [line, use] : uses)
    if (use.imports == 1 && use.foreign == 0 && !use.spills) out.insert(line);
  return out;
}

// Static shadow check over an entire script AST before evaluation begins.
// Throws a `ShadowError` CulebraError on the first violation. `outer`
// starts empty — top-level names enter the chain only when a nested
// function pushes them, so the first scope (`outer[0]` from a nested
// function's perspective) is the top-level / "globals" frame which
// `shadow::check` skips. Invoked by both backends: the interpreter before
// eval and the JIT from analyze_program, so the rule has one source.
inline void check_shadow(const peg::Ast& ast) {
  _detail::shadow::NameSet top_locals;
  _detail::shadow::OuterChain outer;
  _detail::shadow::collect_locals(ast, top_locals, outer);
  _detail::shadow::descend_into_nested(ast, top_locals, outer);
}

// `culebra lint` reports the same static problems the run path aborts on, so it
// must surface shadow errors too. `check_shadow` throws on the first violation
// — which is exactly what running the file surfaces (it aborts before eval on
// the first) — so catch it and record one diagnostic. The walker is not
// structured to enumerate every violation, and reporting more than the run path
// ever would is not the goal.
inline void collect_shadow(const peg::Ast& ast, std::vector<Diagnostic>& diags) {
  try {
    check_shadow(ast);
  } catch (const culebra::CulebraError& e) {
    // interrupt: a static walk over an AST, running no culebra code.
    diags.push_back(Diagnostic{e.kind, e.what(), e.line, e.col, Severity::Error});
  }
}

}  // namespace culebra::lint
