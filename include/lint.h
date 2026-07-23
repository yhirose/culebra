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

  // Reject `self` / `this` as a parameter name. Both are language-core
  // identifiers the callee binds unconditionally — `self` is the implicit
  // recursion handle, `this` the method receiver (see always_bound) — so a
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
      if (loc.name == "self" || loc.name == "this") {
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
      // per iteration (like FOR — see interpreter.h eval_while /
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
        for (const auto& binding : wv.init->nodes) {
          bool declared = binding->nodes.size() >= 2 &&
                          (binding->nodes[0]->token == "let" ||
                           binding->nodes[1]->token == "mut");
          if (!declared) {
            diags_.push_back(Diagnostic{
                "SyntaxError",
                "while-init binding must be declared with 'let' or 'mut'",
                static_cast<long>(binding->line),
                static_cast<long>(binding->column), Severity::Error});
          }
          walk(*binding);   // registers the declared name in the init scope
        }
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
      // Member-name rules in a class body. Instance methods MAY overload —
      // same name, distinct positional-param-type signatures merge into one
      // method-multidispatch dispatcher at eval/compile time. Everything else
      // stays an error (the later definition would be silently dead): a
      // duplicate field, a field/method name clash, any duplicate static
      // member (statics don't overload yet), or two instance methods with an
      // identical signature (unreachable / ambiguous dispatch). Static and
      // instance members live in different places (the class object vs
      // instances), so each tracks its own set.
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
      std::set<std::string, std::less<>> seen_static, inst_fields, seen_special;
      std::map<std::string, std::set<std::string>, std::less<>> inst_methods;
      // Constructors and operator/dunder methods (`__call__`, `__add__`, …)
      // dispatch through dedicated paths, not the method dispatcher, so they
      // don't overload yet — any duplicate is an error.
      auto is_dunder = [](std::string_view n) {
        return n.size() >= 4 && n.substr(0, 2) == "__" &&
               n.substr(n.size() - 2) == "__";
      };
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto mv = culebra::view_method(*node.nodes[j]);
        bool is_field_member = mv.is_field || mv.is_typed_field;
        if (mv.is_static) {
          if (!seen_static.insert(std::string(mv.name)).second) dup_member(mv);
        } else if (is_field_member) {
          if (!inst_fields.insert(std::string(mv.name)).second ||
              inst_methods.count(mv.name)) {
            dup_member(mv);
          }
        } else if (mv.name == "new" || is_dunder(mv.name)) {
          if (!seen_special.insert(std::string(mv.name)).second) dup_member(mv);
        } else {  // instance method — overloads allowed, identical sig rejected
          if (inst_fields.count(mv.name)) {
            dup_member(mv);
          } else if (!inst_methods[std::string(mv.name)]
                          .insert(method_signature(*mv.params))
                          .second) {
            diags_.push_back(Diagnostic{
                "SyntaxError",
                std::format("duplicate method '{}' with identical signature "
                            "in class `{}`",
                            mv.name, class_name),
                static_cast<long>(mv.name_line),
                static_cast<long>(mv.name_col), Severity::Error});
          }
        }
        if (mv.is_field || mv.is_typed_field) {
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
      bool is_packable = false;
      for (size_t d = 0; d < i; d++) {
        walk(*node.nodes[d]);
        if (culebra::is_packable_decorator(*node.nodes[d])) is_packable = true;
      }
      if (is_packable) {
        auto enum_name =
            std::string(culebra::parse_generic_head(node.nodes[i]->token).outer);
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
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto tv = culebra::view_trait_method(*node.nodes[j]);
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

  if (node.tag == "FOR"_) {
    // [pattern/var, iterable, BLOCK]. The loop variable is block-scoped to the
    // body and is captured by closures defined there, so a nested function
    // inside the body must not shadow it — but code after the loop never sees
    // it. Descend the body with the loop binding(s) folded into a body-local
    // copy of the enclosing locals; the iterable is in the enclosing scope.
    if (node.nodes.size() >= 3) {
      descend_into_nested(*node.nodes[1], my_locals, outer);
      NameSet body_locals = my_locals;
      auto& var = *node.nodes[0];
      if (culebra::is_pattern_param(var)) {
        culebra::for_each_pattern_binding(
            var, [&](std::string_view name, size_t, size_t) {
              if (!is_sink(name)) body_locals.insert(std::string(name));
            });
      } else if (var.is_token && !is_sink(var.token)) {
        body_locals.insert(std::string(var.token));
      }
      descend_into_nested(*node.nodes[2], body_locals, outer);
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

// Names the runtime binds implicitly, never via a user declaration:
// `this` (method receiver) / `self` (current function, for recursion), and
// the reserved dunder names it injects per scope — `__ARGS__` / `__KWARGS__`
// (the positional / keyword argument collections), `__LINE__` / `__COLUMN__`
// (the current source position), `__cls__` (a static method's class), and
// any future `__x__`. Treating the whole `__x__` space as bound is sound:
// these are reserved internals, never plain variables, and a blanket rule
// can't drift as new injected dunders are added.
inline bool always_bound(std::string_view n) {
  if (n == "this" || n == "self") return true;
  return n.size() > 4 && n.starts_with("__") && n.ends_with("__");
}

inline bool resolves(std::string_view name, const Chain& chain,
                     const NameSet& globals) {
  if (is_sink(name) || always_bound(name)) return true;
  if (globals.contains(name)) return true;
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
    case "FOR"_:
      // Loop var is block-scoped (bound by walk); collect iterable + body.
      if (node.nodes.size() >= 3) {
        collect_scope(*node.nodes[1], out);
        collect_scope(*node.nodes[2], out);
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
      // Arm patterns bind names visible in the arm; collect them all.
      if (node.nodes.size() >= 2) {
        collect_scope(*node.nodes[0], out);
        for (const auto& arm : node.nodes[1]->nodes) {
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
      // [binding, iterable, body]: the loop variable is block-scoped to the
      // body (the shadow collector deliberately omits it), so bind it in a
      // child frame for the body walk. The iterable is in the enclosing scope.
      if (node.nodes.size() < 3) {
        for (const auto& c : node.nodes) walk(*c, chain, globals, diags);
        return;
      }
      walk(*node.nodes[1], chain, globals, diags);
      NameSet loopvars;
      const auto& var = *node.nodes[0];
      if (culebra::is_pattern_param(var)) {
        culebra::for_each_pattern_binding(
            var, [&](std::string_view n, size_t, size_t) {
              if (!is_sink(n)) loopvars.insert(std::string(n));
            });
      } else if (var.is_token && !is_sink(var.token)) {
        loopvars.insert(std::string(var.token));
      }
      chain.push_back(&loopvars);
      walk(*node.nodes[2], chain, globals, diags);
      chain.pop_back();
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
      // [subject, ARMS]; each arm = [PATTERN, (GUARD)?, body]. Pattern
      // bindings were collected; skip the pattern, walk guard + body.
      if (node.nodes.size() < 2) {
        for (const auto& c : node.nodes) walk(*c, chain, globals, diags);
        return;
      }
      walk(*node.nodes[0], chain, globals, diags);
      for (const auto& arm : node.nodes[1]->nodes)
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
  long line;
  long col;
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
  long line;
  long col;
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
  std::sort(diags.begin(), diags.end(), [](const Diagnostic& a,
                                           const Diagnostic& b) {
    return a.line != b.line ? a.line < b.line : a.col < b.col;
  });
  return diags;
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
    diags.push_back(Diagnostic{e.kind, e.what(), e.line, e.col, Severity::Error});
  }
}

}  // namespace culebra::lint
