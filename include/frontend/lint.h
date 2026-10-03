#pragma once

// Static lint framework. Checks each module before evaluation and reports
// diagnostics. Error-severity findings abort before eval (sound: only
// failures the runtime is certain to raise); warnings are advisory. This is
// the shared home for static checks: rules over the `RuleWalker` walk (let
// reassignment, break/continue/return placement, duplicate params,
// @packable field types), and the undefined-name and shadow checks. What a
// name refers to is never decided here: the scope rules are resolve.h's,
// and every check that asks reads its resolution.

#include <algorithm>
#include <format>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "base/packable.h"
#include "frontend/parser.h"
#include "frontend/resolve.h"
#include "base/shared.h"

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

// Walks the AST once for the rules a node's place decides: where a
// break / continue / return may stand, what a parameter list and a class
// body may hold, and a `let` reassigned. Which variable a name is comes
// from the resolution (resolve.h), not from a scope model of its own.
class RuleWalker {
 public:
  RuleWalker(const resolve::Resolution& res, std::vector<Diagnostic>& diags)
      : res_(res), diags_(diags), passed_(res.symbols.size()) {}

  void run(const peg::Ast& ast) { walk(ast); }

 private:
  const resolve::Resolution& res_;
  std::vector<Diagnostic>& diags_;
  // Per variable, the declarations the walk has passed: a `let`, and one
  // that leaves it reassignable (a `mut`, a declaring pattern).
  enum : uint8_t { kLet = 1, kReassignable = 2 };
  std::vector<uint8_t> passed_;

  void pass(const peg::Ast& name, uint8_t what) {
    if (size_t sym = res_.symbol_of(name); sym != resolve::kNone)
      passed_[sym] |= what;
  }

  // `x = v` is certain to fail when the walk has passed a `let` of that
  // variable and nothing that leaves it reassignable. A binding the callee or
  // the construct makes (a parameter, a loop's, a catch's, a pattern's) is
  // left to the run.
  bool reassigns_let(const peg::Ast& target) const {
    using resolve::Form;
    size_t s = res_.symbol_of(target);
    if (s == resolve::kNone || passed_[s] != kLet) return false;
    const auto& sym = res_.symbols[s];
    return !sym.declared_as(Form::Parameter) && !sym.declared_as(Form::Loop) &&
           !sym.declared_as(Form::Catch) && !sym.declared_as(Form::Pattern);
  }
  // The loops a break/continue here could target, innermost last, each held
  // by its label (empty for an unlabelled loop). Its depth is what the
  // outside-a-loop check reads; its names are what a labelled `break outer`
  // resolves against.
  // Views of the AST tokens, which outlive the walk — the same shape
  // `escaping_loop_ctrl` keeps its open labels in.
  using LabelStack = std::vector<std::string_view>;
  LabelStack loop_labels_;

  // RAII entry into a loop body: the loop is a break/continue target for the
  // span of the walk.
  struct LoopScope {
    LabelStack& stack;
    LoopScope(LabelStack& s, const peg::Ast* label) : stack(s) {
      stack.push_back(label ? label->token : std::string_view{});
    }
    ~LoopScope() { stack.pop_back(); }
  };

  // RAII function boundary: a function / lambda / method / class / trait body
  // or a defer thunk, where no enclosing loop is reachable — break/continue
  // cannot cross a call, matching the compiled backend's per-chunk loop
  // scoping and every mainstream language.
  struct LoopBoundary {
    LabelStack& stack;
    LabelStack saved;
    explicit LoopBoundary(LabelStack& s) : stack(s), saved(std::move(s)) {
      stack.clear();
    }
    ~LoopBoundary() { stack = std::move(saved); }
  };

  // A label that shadows an enclosing loop's would make `break outer` mean
  // two loops in the same nest; culebra rejects the ambiguity rather than
  // resolving it by rule (the bytecode compiler rejects it too).
  void check_dup_loop_label(const peg::Ast* label) {
    if (!label) return;
    if (std::find(loop_labels_.begin(), loop_labels_.end(), label->token) ==
        loop_labels_.end())
      return;
    diags_.push_back(
        Diagnostic{"SyntaxError", culebra::duplicate_loop_label_msg(label->token),
                   static_cast<long>(label->line),
                   static_cast<long>(label->column), Severity::Error});
  }

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

  void walk_children(const peg::Ast& node) {
    for (const auto& c : node.nodes) walk(*c);
  }

  // An INIT_CLAUSE's bindings (while / if / match init clauses). Each must
  // declare with `let` / `mut`; a bare `x = 0` would reassign an outer
  // variable or make an immutable binding, so reject it pre-eval on every
  // backend.
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
      walk(*binding);
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
  // `ctor`: this is a class's `new`, the one list a field parameter
  // (`.x`, docs/language.md §10) may appear in.
  void check_param_wellformed(const peg::Ast& params, bool ctor = false) {
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
      if (pv.is_field && !ctor) {
        return err(std::format("field parameter '.{}' is only allowed in a "
                               "class's `new`", pv.name),
                   pv.name_line, pv.name_col);
      }
      if (pv.is_optional && (pv.default_value || !pv.type_annotation.empty())) {
        return err(std::format("field parameter '.{}?' takes its type and "
                               "default from the field's declaration", pv.name),
                   pv.name_line, pv.name_col);
      }
      // A destructuring parameter cannot carry a default, so it is a
      // required one and falls under the same ordering rule — the binders
      // count required slots by position, and a required slot after a
      // defaulted one has no arity that can fill it. `.x?` is optional the
      // same way a defaulted one is.
      if (pv.default_value || pv.is_optional) {
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

inline void RuleWalker::walk(const peg::Ast& node) {
  switch (node.tag) {
    case "FUNCTION"_:
    case "LAMBDA"_: {
      auto fv = node.tag == "FUNCTION"_ ? culebra::view_function(node)
                                        : culebra::view_lambda(node);
      check_dup_params(*fv.params);
      check_reserved_params(*fv.params);
      check_param_wellformed(*fv.params);
      LoopBoundary g(loop_labels_);
      FnDepthGuard fg(fn_depth_);
      walk(*fv.body);
      return;
    }
    case "DEFER"_: {
      // DEFER <- [BLOCK]: the body is a deferred thunk — a separate closure
      // in the JIT — so it is a function boundary for break/continue. A
      // `defer { break }` cannot reach the enclosing loop (the JIT segfaults
      // on it today, the interp silently propagates), so reset loop depth.
      // It is also a function boundary for `return` (which exits the defer
      // closure, docs §defer).
      LoopBoundary g(loop_labels_);
      FnDepthGuard fg(fn_depth_);
      walk_children(node);
      return;
    }
    case "WHILE"_: {
      // [(INIT_CLAUSE)?, condition, BLOCK]: the condition stays at the
      // enclosing loop depth; the body is inside the loop.
      if (node.nodes.size() < 2) { walk_children(node); return; }
      auto wv = culebra::view_while(node);
      if (wv.init) walk_init_clause(*wv.init);
      walk(*wv.cond);
      {
        check_dup_loop_label(wv.label);
        LoopScope g(loop_labels_, wv.label);
        walk(*wv.body);
      }
      // The nobreak block runs after the loop, so a break/continue inside it
      // belongs to an enclosing loop — walk it at the *outer* loop depth (no
      // guard).
      if (wv.nobreak) walk(*wv.nobreak);
      return;
    }
    case "IF"_: {
      // [(INIT_CLAUSE)?, cond, block, cond, block, …, (else-block)?]: an
      // init clause's bindings are held to its rule, then the arms (from
      // arm_off, past the INIT_CLAUSE child).
      auto iv = culebra::view_if(node);
      if (!iv.init) { walk_children(node); return; }
      walk_init_clause(*iv.init);
      for (size_t i = iv.arm_off; i < node.nodes.size(); i++)
        walk(*node.nodes[i]);
      return;
    }
    case "FOR"_: {
      // [pattern, iterable, BLOCK, (NOBREAK_CLAUSE)?]: the iterable is
      // outside the loop, the body inside it. A nobreak block runs after the
      // loop (enclosing loop depth).
      if (node.nodes.size() < 3) { walk_children(node); return; }
      auto fv = culebra::view_for(node);
      walk(*fv.iter);
      {
        check_dup_loop_label(fv.label);
        LoopScope g(loop_labels_, fv.label);
        walk(*fv.body);
      }
      if (fv.nobreak) walk(*fv.nobreak);
      return;
    }
    case "MATCH"_: {
      // [(INIT_CLAUSE)?, subject, ARMS]; each arm = [PATTERN, (GUARD)?, EXPR].
      auto mv = culebra::view_match(node);
      if (mv.init) walk_init_clause(*mv.init);
      walk(*mv.subject);
      for (const auto& arm : mv.arms->nodes) {
        if (arm->nodes.empty()) continue;
        // A pattern is a constant position: reject interpolating `"..."`
        // patterns (the guard/body below may interpolate freely).
        check_pattern_const_strings(*arm->nodes[0], diags_);
        for (size_t i = 1; i < arm->nodes.size(); i++) walk(*arm->nodes[i]);
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
      LoopBoundary g(loop_labels_);
      FnDepthGuard fg(fn_depth_);
      walk(*node.nodes.back());
      return;
    }
    case "CLASS_DECL"_: {
      size_t i = culebra::first_non_decorator_index(node);
      bool is_packable = false;
      bool is_value = false;
      for (size_t d = 0; d < i; d++) {
        walk(*node.nodes[d]);
        if (culebra::is_packable_decorator(*node.nodes[d])) is_packable = true;
        if (culebra::is_value_decorator(*node.nodes[d])) is_value = true;
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
      // The instance fields in declaration order (the body's and the ones
      // the `new` field parameters declare), gathered up front: a `new`
      // may sit above the fields its parameters name.
      auto inst_field_list = culebra::collect_instance_fields(node, i + 1);
      auto declared_fields = culebra::declared_instance_fields(inst_field_list);
      // Method bodies and field initializers are function-boundary contexts.
      LoopBoundary g(loop_labels_);
      std::vector<std::pair<std::string, std::string>> pk_fields;
      bool pk_ok = true;
      // `@value` fixes the semantics rather than a byte layout, but its
      // declaration rules read the same members, so they are checked in the
      // same pass — pre-eval on every backend, at the offending position.
      bool val_ok = true;
      if (is_value && is_packable) {
        diags_.push_back(Diagnostic{
            "SyntaxError", culebra::value_packable_message(class_name),
            static_cast<long>(node.nodes[i]->line),
            static_cast<long>(node.nodes[i]->column), Severity::Error});
        val_ok = false;
      }
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
        if (culebra::static_field_lacks_value(mv)) {
          diags_.push_back(Diagnostic{
              "SyntaxError",
              culebra::static_field_value_message(mv.name, class_name),
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
          culebra::check_field_params(
              *mv.params, declared_fields, class_name,
              [&](std::string msg, size_t line, size_t col) {
                diags_.push_back(Diagnostic{"SyntaxError", std::move(msg),
                                            static_cast<long>(line),
                                            static_cast<long>(col),
                                            Severity::Error});
              });
        } else {
          check_named(inst_fields, inst_methods, mv, is_field_member);
        }
        // The three per-member clauses of the `@value` contract, in the order
        // require_value_member (the compile-side safety net) applies them and
        // over the same members: a `static` member is the class object's,
        // outside the instance protocol and its field set. No `drop` — a
        // destructor tells one instance from another, which is the identity
        // the contract removes.
        const bool value_member = is_value && !mv.is_static;
        if (value_member && mv.name == "drop") {
          diags_.push_back(Diagnostic{
              "SyntaxError", culebra::value_drop_message(class_name),
              static_cast<long>(mv.name_line),
              static_cast<long>(mv.name_col), Severity::Error});
          val_ok = false;
        }
        if (mv.is_field || mv.is_typed_field) {
          // The field's own checks ran over the field list above; only its
          // initializer is left to walk.
          if (mv.value) walk(*mv.value);
          continue;
        }
        check_dup_params(*mv.params);
        check_reserved_params(*mv.params);
        check_param_wellformed(*mv.params,
                               /*ctor=*/mv.name == "new" && !mv.is_static);
        FnDepthGuard fg(fn_depth_);
        walk(**mv.body);
      }
      // The per-field clauses, over the instance fields in declaration order
      // — the body's and the ones the `new` field parameters declare, the
      // list the compiler lays the class out from.
      for (const auto* f : inst_field_list) {
        auto mv = culebra::view_method(*f);
        // A field a `new` parameter declares joins the instance namespace
        // under the same uniqueness rule as a body declaration; the body's
        // own fields already passed through check_named in the loop above.
        if (culebra::is_field_param(*f))
          check_named(inst_fields, inst_methods, mv, /*is_field_member=*/true);
        {
          // A value's field set is fixed at the declaration, and each field
          // holds a scalar or another value.
          if (is_value && mv.is_typed_field &&
              !culebra::is_value_field_type(mv.type_annotation)) {
            diags_.push_back(Diagnostic{
                "SyntaxError",
                culebra::value_field_type_message(class_name, mv.name,
                                                  mv.type_annotation),
                static_cast<long>(mv.name_line),
                static_cast<long>(mv.name_col), Severity::Error});
            val_ok = false;
          }
          if (is_value && mv.is_field) {
            diags_.push_back(Diagnostic{
                "SyntaxError",
                culebra::value_untyped_field_message(mv.name, class_name),
                static_cast<long>(mv.name_line),
                static_cast<long>(mv.name_col), Severity::Error});
            val_ok = false;
          }
          // An untyped instance field carries no type for the byte layout —
          // shared message with the evaluator-side safety net
          // (require_typed_packable_field).
          if (is_packable && mv.is_field) {
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
                  culebra::packable_type_error(class_name, mv.name,
                                               mv.type_annotation),
                  static_cast<long>(mv.name_line),
                  static_cast<long>(mv.name_col), Severity::Error});
              pk_ok = false;
            } else {
              pk_fields.emplace_back(std::string(mv.name),
                                     std::string(mv.type_annotation));
            }
          }
        }
      }
      // The clause that needs the whole field set: a member writing
      // `self.<undeclared>` would give one instance a field its siblings
      // lack. Same scan the compiler runs, after its own member loop.
      if (is_value) {
        std::set<std::string, std::less<>> value_declared;
        for (const auto& [name, _] : declared_fields) value_declared.insert(name);
        auto writes = culebra::find_value_self_writes_in_members(
            node, i + 1, value_declared);
        for (const auto& w : writes) {
          diags_.push_back(Diagnostic{
              "SyntaxError",
              culebra::value_undeclared_self_write_message(class_name,
                                                           w.field),
              static_cast<long>(w.line), static_cast<long>(w.col),
              Severity::Error});
          val_ok = false;
        }
      }
      // Register the class as a value pre-eval, for the same reason the
      // @packable layout registers below: a later `@value` class declaring a
      // field of this one validates its type against the registry.
      if (is_value && val_ok)
        culebra::register_value_class(std::string(class_name));
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
        // `@value` has no declared fields to hold an enum to; reported at the
        // decorator, like the unknown-trait check above.
        if (culebra::is_value_decorator(*node.nodes[d])) {
          diags_.push_back(Diagnostic{
              "SyntaxError", culebra::value_on_enum_message(enum_name),
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
      LoopBoundary g(loop_labels_);
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
        walk(*tv.body);
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
      if (loop_labels_.empty()) {
        diags_.push_back(Diagnostic{
            "SyntaxError",
            node.tag == "BREAK"_ ? "break outside loop" : "continue outside loop",
            static_cast<long>(node.line), static_cast<long>(node.column),
            Severity::Error});
        return;
      }
      // A label names one of the enclosing loops; one that names none is the
      // same certain failure as no loop at all, so it is reported here too.
      if (auto label = culebra::break_label_of(node); !label.empty() &&
          std::find(loop_labels_.begin(), loop_labels_.end(), label) ==
              loop_labels_.end()) {
        diags_.push_back(
            Diagnostic{"SyntaxError", culebra::no_such_loop_label_msg(label),
                       static_cast<long>(node.nodes[0]->line),
                       static_cast<long>(node.nodes[0]->column),
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
          // `let mut x` carries both flags and is mutable, so check
          // is_mut first; only a bare `let` (no mut) is immutable.
          if (av.is_mut) {
            pass(lval, kReassignable);
          } else if (av.is_let) {
            pass(lval, kLet);
          } else if (!av.compound && reassigns_let(lval)) {
            diags_.push_back(Diagnostic{
                "ImmutableError",
                std::format("cannot reassign '{}' (declared without 'mut')",
                            lval.token),
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
      // [LET, MUTABLE, pattern, EXPRESSION]. What a pattern declares stays
      // reassignable here (conservative — never flag a destructured name's
      // reassignment).
      auto dv = culebra::view_destructure(node);
      walk(*dv.rhs);
      if (dv.declares)
        culebra::for_each_pattern_leaf(
            *dv.pattern,
            [&](const peg::Ast& id, bool) { pass(id, kReassignable); });
      return;
    }
    case "PLACE_ASSIGN"_: {
      // [target..., EXPRESSION]. A chain target's subexpressions are ordinary
      // reads.
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
          [](const peg::Ast&) {});
      return;
    }
    default:
      walk_children(node);
      return;
  }
}

// --- The scope checks, read off the resolution (resolve.h) ---

// The resolution lists a function's names after the body around it; the
// diagnostics from `first` on are put in source order, so the first one
// reported is the first in the source.
inline void sort_by_position(std::vector<Diagnostic>& diags, size_t first) {
  std::stable_sort(diags.begin() + static_cast<std::ptrdiff_t>(first),
                   diags.end(), [](const Diagnostic& a, const Diagnostic& b) {
                     return a.line != b.line ? a.line < b.line : a.col < b.col;
                   });
}

// A read no declaration visible there declares and no global answers: the
// name of a variable whose scope has closed, one read above its declaration,
// one nothing declares. It is certain to raise NameError when it runs, so it
// is reported before anything does. A name some visible declaration does
// declare is the run's to decide: the declaration may not have run (a
// closure called early, a skipped `c && (let q = 1)`).
// Excluded by construction (resolve.h never looks them up): `.member`,
// kwarg labels, object keys, `_`, `self`, the always-bound names.
inline void undefined_reads(const resolve::Resolution& res,
                            const std::set<std::string, std::less<>>& globals,
                            std::vector<Diagnostic>& diags) {
  size_t first = diags.size();
  for (const peg::Ast* n : res.unbound_reads) {
    // Separate from `globals`: that set is built once and cached, the
    // ambients are per run (shared.h).
    if (globals.contains(n->token) || culebra::is_ambient_global(n->token))
      continue;
    diags.push_back(Diagnostic{
        "NameError", std::format("undefined variable '{}'", n->token),
        static_cast<long>(n->line), static_cast<long>(n->column),
        Severity::Error});
  }
  sort_by_position(diags, first);
}

// A declaration in a function of a name that is, where the function is
// written, a variable of an enclosing function: the one the function would
// capture, which a bare `x = v` in it writes (so a bare write declares only
// where no such variable is). The module's own names are globals and may be
// shadowed freely; a name whose scope closed before the function, or one a
// sibling scope holds, is not visible to it and so is not shadowed.
inline void shadows(const resolve::Resolution& res,
                    std::vector<Diagnostic>& diags) {
  size_t first = diags.size();
  for (const auto& d : res.declarations) {
    const auto& sym = res.symbols[d.symbol];
    size_t around = res.scopes[res.function_of(sym.scope)].parent;
    if (around == resolve::kNone) continue;  // the module's own
    size_t outer = res.lookup(around, sym.name);
    if (outer == resolve::kNone || res.frame_of(outer) == 0) continue;
    diags.push_back(Diagnostic{
        "ShadowError", culebra::shadow_error_msg(sym.name),
        static_cast<long>(d.node->line), static_cast<long>(d.node->column),
        Severity::Error});
  }
  sort_by_position(diags, first);
}

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
  // Shape's Circle even if some unrelated enum also has a Circle variant.
  // The compiler reads it the same way: compile_ctor_pattern_test tests the
  // subject's `__enum` against the qualifier.
  void resolve(std::string_view name) {
    auto q = culebra::parse_qualified_variant(name);
    std::string variant(q.variant);
    std::string owner;
    if (!q.enum_name.empty()) {
      std::string hint(q.enum_name);
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

// The builtin names, or null before the hook is installed.
inline const std::set<std::string, std::less<>>* builtin_names() {
  return builtin_names_hook ? builtin_names_hook() : nullptr;
}

// How a module is resolved (resolve.h) for the compiler and for the checks
// here: with the stdlib's global names, and in a session the names earlier
// inputs declared.
inline resolve::Options resolve_options(
    std::span<const std::string> session = {}) {
  return {.record_nodes = true, .globals = builtin_names(), .session = session};
}

// The Error-severity static analyses of the load, shared by the enforce path
// (`check_module`, run on every load) and the report path (`collect_module`,
// the `culebra lint` CLI). These are the checks the runtime is certain to
// raise: malformed control flow / declarations (RuleWalker) and the reads
// nothing declares. `res` is `ast` resolved with resolve_options(). Appends
// to `diags`.
inline void run_error_checks(const peg::Ast& ast, const resolve::Resolution& res,
                             std::vector<Diagnostic>& diags) {
  _detail::RuleWalker walker(res, diags);
  walker.run(ast);
  // Run only when the builtin-name provider is installed: without it every
  // stdlib name would read as undefined.
  if (const auto* globals = builtin_names())
    _detail::undefined_reads(res, *globals, diags);
}

// Run the load-stage static lint checks over one module AST before evaluation.
// Throws the first Error-severity diagnostic as a CulebraError (same shape
// the runtime would raise), so the module loader surfaces it uniformly to
// every backend. Advisory warnings (e.g. unused locals) are NOT run here —
// they would be discarded, and the load path stays free of their cost; the
// `culebra lint` CLI runs the full set via `collect_module`.
inline void check_module(const peg::Ast& ast) {
  auto res = resolve::resolve_module(ast, {}, resolve_options());
  std::vector<Diagnostic> diags;
  run_error_checks(ast, res, diags);
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
inline std::vector<Diagnostic> collect_module(const peg::Ast& lowered,
                                              const peg::Ast& authored) {
  std::vector<Diagnostic> diags;
  auto res = resolve::resolve_module(lowered, {}, resolve_options());
  run_error_checks(lowered, res, diags);
  // Shadow is an error-severity static check the run path applies before eval;
  // like the other error checks it runs on the lowered AST (positions map back
  // to the authored source, same as when running the file).
  _detail::shadows(res, diags);
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

// The shadow check over a resolved module, before it is compiled: throws a
// `ShadowError` CulebraError for the first violation. The compiler calls it
// (FnAnalysis::analyze_program) on the resolution it compiles from, so every
// lane rejects the same programs, dead code included.
inline void check_shadow(const resolve::Resolution& res) {
  std::vector<Diagnostic> diags;
  _detail::shadows(res, diags);
  if (!diags.empty())
    throw culebra::CulebraError(diags[0].kind, diags[0].message, diags[0].line,
                                diags[0].col);
}

}  // namespace culebra::lint
