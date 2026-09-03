#pragma once

// Front-end analysis over the AST, shared by every backend that compiles
// functions (docs/internals/vm.md §4): per-function locals, free-variable /
// capture sets, and EH/defer emission flags. Lifted out of jit.h so the
// bytecode compiler can consume the same passes as the JIT; deliberately
// LLVM-free.

#include <frontend/lint.h>
#include <frontend/parser.h>
#include <base/shared.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace culebra {

// Analysis result for a function (including the top-level main program).
struct FuncInfo {
  std::vector<std::string> free_vars;   // captured from outer
  // Free vars noted only as UFCS method-name candidates, never read as a
  // variable. The enclosing locals set is flat per function, so a name
  // declared in a block that has already closed still lands here — for a
  // real read that's a compile error, but a candidate that isn't in reach
  // just means the receiver's builtin answers, so emit_closure_build binds
  // it to nil instead.
  std::set<std::string> optional_free_vars;
  std::set<std::string> captured_locals;  // my locals captured by nested
  // EH/defer emission flags (populated by scan_eh_defer):
  bool has_eh = false;        // contains TRY or any scope with defers
  bool has_fn_defer = false;  // contains DEFER directly in fn body
  bool has_any_defer = false; // contains DEFER at any depth (fn or a nested
                              // scope/arm). Drives whether the fn establishes
                              // a defer-stack mark so an early return/break/
                              // continue runs the still-pending defers — even
                              // when the only defers live in a lexical scope
                              // or a match block arm (interp runs these via
                              // its unwind catch-all).
  bool has_return = false;    // contains RETURN at any depth (stopping at
                              // nested fns). With has_any_defer, gates HOF
                              // callback inlining (is_inlinable_lambda).
  // True if the function body references the auto-bound `__ARGS__`
  // identifier (overflow args Array). When false, the prologue skips
  // building the Array entirely — saves a heap allocation per call
  // for the common no-varargs case.
  bool uses_args = false;
  // True if the body reads the `fn` recursion handle. Gates the
  // per-frame bound-handle cache slot (see compile_identifier's `fn`
  // path); frames that never mention `fn` pay nothing.
  bool uses_fn = false;
  // The body's own name: for an undecorated `fn name` (MULTIFN_DECL) the
  // declared name, for a `let name = fn …` literal the binding it
  // initializes. The body sees it as a prologue-bound local delivered by
  // the frame — the dispatch (culebra_runtime_multifn_self) for a multifn,
  // the running closure itself for a literal — rather than as a capture
  // of the declaring scope's binding cell. The capture would close a
  // refcount ring — cell → dispatcher/closure → body → cell — that only
  // the tracing backstop could reclaim; the prologue read borrows from
  // the caller instead, so no ring ever forms. Empty when the name is
  // unavailable this way (decorated decl: the binding is the decorator's
  // result; name shadowed by a parameter: the param wins; a `let` the
  // statement list declares again, or a REPL session's top-level `let`:
  // both may rebind, and a read through the cell is what sees the
  // rebinding). own_name_used gates the prologue bind so non-recursive
  // bodies pay nothing.
  std::string own_name;
  bool own_name_used = false;
  // Where the prologue reads it from: the running closure itself (a `let`
  // literal, fn_slot), the dispatch (a multifn, MfSelf), or the receiver
  // (a class member — the instance's class object, or the class object a
  // static runs on; ClsSelf).
  enum class OwnNameSource : uint8_t { Closure, Dispatch, Receiver };
  OwnNameSource own_name_source = OwnNameSource::Closure;
};

// Hidden scope-slot name binding a class's synthetic field-init closure
// for its `new` body to capture. Keyed by the CLASS_DECL node address so
// the analysis pass and compile_class_decl agree, two classes in one
// scope never collide, and user code can never name it (identifiers
// cannot contain \x1f).
inline std::string field_init_slot_name(const peg::Ast& class_decl) {
  return std::format("\x1f__finit_{:x}",
                     reinterpret_cast<uintptr_t>(&class_decl));
}

// One FnAnalysis instance per compilation. `func_info` and
// `scope_has_defer` are keyed by `peg::Ast*` and never cleared — they
// accumulate across the modules of a single compilation, which is safe
// because every module's AST stays live for the whole run, so their node
// addresses never collide. (A multi-module program calls analyze_program
// once per module on the same instance — see the build/run loops.) The
// instance must not be reused across *separate* compilations, where a
// freed AST's addresses could be recycled; callers enforce that by
// constructing a fresh instance per compilation.
struct FnAnalysis {
  // True for names the host backend supplies as builtins (fn/range/stdlib
  // namespaces/...); such names never resolve to user scopes, so they are
  // skipped as free-variable candidates. Injected as a plain predicate so
  // the analysis stays independent of the backend's extension machinery.
  using IsBuiltinVar = bool (*)(const std::string&);

  explicit FnAnalysis(IsBuiltinVar is_builtin_var)
      : is_builtin_var_(is_builtin_var) {}

  // Analysis results for each FUNCTION AST node (plus the main program).
  // Backends read these while compiling.
  std::map<const peg::Ast*, FuncInfo> func_info;

  // LEXICAL_SCOPE nodes that contain a DEFER within their own scope level
  // (not crossing nested LEXICAL_SCOPE / FUNCTION / DEFER). Scopes not in
  // this set skip emitting scope.mark / scope.cleanup landingpad.
  std::unordered_set<const peg::Ast*> scope_has_defer;

  // TRY nodes whose body subtree registers any defer on the enclosing
  // frame's stack, at ANY scope depth (still stopping at nested FUNCTION /
  // DEFER boundaries). A throw mid-region leaves those defers pending, so
  // the VM's region handler needs a mark exactly when the node is in here.
  std::unordered_set<const peg::Ast*> try_region_has_defer;

  // A flat `@value` class's own declaration AST, by its declared name, for
  // one this compile pass has already registered in the process-wide
  // `culebra::value_flat_layouts()` (vm.h's compile_class_decl fills both
  // together). Exists for exactly one shape: a class declared inside a
  // stdlib lazy-namespace module (`fn(){ @value class Vector2 {...};
  // Vector2 }()`, `include/stdlib/preamble.h`'s `_wrap_lazy_ns_module`) has
  // no `let`-bound name any ordinary scope's `lookup()` ever finds — every
  // reference resolves through the runtime namespace registry instead, so
  // `Binding::Known::value_class` (set only on a class's own declaration
  // binding, vm.h ~5613) never reaches it. This is the same fact under a
  // different key: name-addressable rather than binding-addressable, valid
  // for exactly this ONE compile pass (a fresh `FnAnalysis` per
  // `compile_module_impl`) — never process-wide, since a pointer into one
  // parse's AST is meaningless, or unsafe to read concurrently, against any
  // other. `postfix_value_class` (vm.h) is the only reader, falling back to
  // this only when `lookup()` finds no live local shadowing the name.
  std::map<std::string, const peg::Ast*, std::less<>> stdlib_value_classes;

  FuncInfo analyze_program(const peg::Ast& programAst,
                           bool session_top = false) {
    session_top_ = session_top;
    // Shadow analysis is single-sourced in lint.h (the same check the
    // interpreter runs). collect_fn_locals/visit_for_frees below only collect
    // locals + free variables for codegen; they no longer check shadowing.
    lint::check_shadow(programAst);
    std::vector<const std::set<std::string>*> outer;
    std::set<std::string> my_locals;
    DeclKinds kinds;
    collect_fn_locals(programAst, my_locals, outer, kinds);

    FuncInfo info;
    DeclaredScope declared(*this, my_locals, kinds);
    visit_for_frees(programAst, my_locals, outer, info);
    scan_eh_defer(programAst, true, info);
    return info;
  }

 private:
  IsBuiltinVar is_builtin_var_;
  int defer_count_ = 0;  // DEFER nodes seen so far, across all scans

  // How a local gets its binding, collected alongside the locals set.
  // `from_assign` names are bound by the statement that assigns them, so
  // before that statement the name still means whatever it meant outside
  // (the interp resolves through the environment chain at the moment the
  // read runs). `scope_wide` names — parameters, loop / catch / match
  // bindings, `fn` / `class` / `enum` / `import` declarations — are in
  // scope for the whole body, exactly as they are today.
  struct DeclKinds {
    std::set<std::string> from_assign;
    std::set<std::string> scope_wide;
    // Declared more than once in the function's flat locals (a second
    // `let`, a block-scoped shadow, a parameter's name): a literal bound
    // to such a name keeps reading the cell (FuncInfo::own_name).
    std::set<std::string> redeclared;
  };

  // Locals of the function being walked that are already bound at the
  // current point. Seeded with everything but the not-yet-assigned names;
  // visit_for_frees adds each as it passes its declaration.
  std::set<std::string> declared_;
  // The function being walked's DeclKinds::redeclared.
  std::set<std::string> redeclared_;
  // The program being analyzed is a REPL line: its top-level `let`s are
  // session cells a later line may rebind (analyze_program's flag).
  bool session_top_ = false;
  // `let name = fn …` literals whose body reads `name` as itself: filled
  // by the assignment's walk, read when the literal's own walk starts.
  std::map<const peg::Ast*, std::string> literal_own_names_;

  // Save/restore `declared_` across a nested function's walk.
  struct DeclaredScope {
    FnAnalysis& fa;
    std::set<std::string> saved, saved_redeclared;
    DeclaredScope(FnAnalysis& fa, const std::set<std::string>& my_locals,
                  const DeclKinds& kinds)
        : fa(fa),
          saved(std::move(fa.declared_)),
          saved_redeclared(std::move(fa.redeclared_)) {
      fa.declared_.clear();
      fa.redeclared_ = kinds.redeclared;
      for (const auto& name : my_locals) {
        if (kinds.from_assign.contains(name) &&
            !kinds.scope_wide.contains(name)) {
          continue;  // bound by its own assignment, walked below
        }
        fa.declared_.insert(name);
      }
    }
    ~DeclaredScope() {
      fa.declared_ = std::move(saved);
      fa.redeclared_ = std::move(saved_redeclared);
    }
  };

  // A block's own declarations die with it: after `{ let x = 1 }` the
  // name means again whatever it meant outside the block. The locals set
  // is flat per function, so this is where block granularity lives.
  // Sites match lint.h's ScopeWalker (LEXICAL_SCOPE, loop bodies, match
  // arms, try/catch bodies); `if` deliberately shares its enclosing one.
  struct DeclaredBlock {
    FnAnalysis& fa;
    std::set<std::string> saved;
    explicit DeclaredBlock(FnAnalysis& fa) : fa(fa), saved(fa.declared_) {}
    ~DeclaredBlock() { fa.declared_ = std::move(saved); }
  };

  // Collect names introduced by `let x = ...` or by bare `x = ...` where x is
  // not in any outer scope (auto-local). Does not descend into nested
  // functions.
  // Does an enclosing function already hold this name? A bare (`let`-less)
  // write to such a name reassigns it rather than declaring a local.
  static bool visible_in_outer(
      std::string_view name,
      const std::vector<const std::set<std::string>*>& outer) {
    for (auto* s : outer)
      if (s->contains(std::string(name))) return true;
    return false;
  }

  // free_vars keeps first-seen order, so "add if absent" is a linear probe
  // rather than a set. Returns whether the name was new.
  static bool add_free_var(FuncInfo& info, const std::string& name) {
    auto& fvs = info.free_vars;
    if (std::find(fvs.begin(), fvs.end(), name) != fvs.end()) return false;
    fvs.push_back(name);
    return true;
  }

  void collect_fn_locals(
      const peg::Ast& node, std::set<std::string>& locals,
      const std::vector<const std::set<std::string>*>& outer,
      DeclKinds& kinds) const {
    using namespace peg::udl;
    if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) return;
    // Record which bucket a freshly collected local belongs to; see
    // DeclKinds. Called next to every `locals.insert` below.
    auto note = [&](std::string_view name, bool from_assign) {
      (from_assign ? kinds.from_assign : kinds.scope_wide)
          .insert(std::string(name));
    };

    if (node.tag == "MATCH"_) {
      // MATCH = [(INIT_CLAUSE)?, subject, MATCH_ARMS]; MATCH_ARM =
      // [PATTERN, (GUARD)?, EXPR]. Register pattern-bound names as locals of the
      // enclosing function so that nested closures capturing them are handled
      // correctly by the free-variable analysis (the bindings are then promoted
      // to cells via `captured_locals`). Same mechanism as TRY's catch binding
      // below. The optional init clause's own bindings are picked up by the
      // generic recursive walk that follows (like a plain `let`).
      for (auto& arm : culebra::view_match(node).arms->nodes) {
        for_each_pattern_binding(
            *arm->nodes[0],
            [&](std::string_view name, size_t, size_t) {
              locals.insert(std::string(name));
              note(name, /*from_assign=*/false);
            });
      }
      // fall through to normal recursive walk
    }

    if (node.tag == "DESTRUCTURE_ASSIGN"_) {
      // [LET, MUTABLE, PATTERN, EXPRESSION]. The pattern's bound names are
      // locals of the enclosing function (like a plain `let`), so nested
      // closures can capture them and the bindings get promoted to cells.
      // Same mechanism as MATCH above; without this they look like globals and
      // a capturing closure raises NameError. A `let`-less destructure
      // reassigns whatever is visible, exactly as bare `x = v` does, so a name
      // an enclosing scope already holds is a free variable, not a local.
      if (node.nodes.size() >= 3) {
        bool is_declare = node.nodes[0]->token == "let" ||
                          node.nodes[1]->token == "mut";
        for_each_pattern_binding(
            *node.nodes[2],
            [&](std::string_view name, size_t, size_t) {
              if (is_declare || !visible_in_outer(name, outer)) {
                locals.insert(std::string(name));
                note(name, /*from_assign=*/true);
              }
            });
      }
      // fall through to walk the RHS
    }

    if (node.tag == "PLACE_ASSIGN"_) {
      // A plain-name target declares when nothing visible holds the name, so
      // it is a local of the enclosing function for the same reason as the
      // destructure patterns above — and a free variable when an enclosing
      // scope does hold it. Chain targets bind nothing and are walked as
      // ordinary expressions below.
      culebra::for_each_place_target(
          node, [](const peg::Ast&) {},
          [&](const peg::Ast& name) {
            if (!visible_in_outer(name.token, outer)) {
              locals.insert(std::string(name.token));
              note(name.token, /*from_assign=*/true);
            }
          });
      // fall through to walk the targets' subexpressions and the RHS
    }

    if (node.tag == "TRY"_) {
      // TRY = [body_block, catch_ident, catch_body]. The catch binding
      // introduces a new local in the enclosing function; register it
      // so nested closures that capture it see it and get a cell.
      // `try ... catch _ { ... }` is the sink form (drop the value).
      auto& id = *node.nodes[1];
      auto name = std::string(id.token);
      if (!is_sink_name(name)) {
        locals.insert(name);
        note(name, /*from_assign=*/false);
      }
      // fall through to walk the bodies
    }

    if (node.tag == "FOR"_) {
      // FOR = [IDENT(var), EXPRESSION(iterable), BLOCK(body), (NOBREAK)?]. The
      // loop binding is BLOCK-SCOPED (visible only within the body), so we
      // deliberately don't add it to the enclosing function's flat
      // `locals` set — otherwise functions defined OUTSIDE the for
      // body in the same enclosing function would wrongly see the
      // binding in their `outer`. Subtree-local visibility for closures
      // inside the body is re-established in visit_for_frees' FOR handler.
      auto fv = culebra::view_for(node);
      collect_fn_locals(*fv.iter, locals, outer, kinds);
      collect_fn_locals(*fv.body, locals, outer, kinds);
      // A `nobreak { … }` block can define closures too; walk it so their
      // captured locals are registered (compile_for emits it).
      if (fv.nobreak) collect_fn_locals(*fv.nobreak, locals, outer, kinds);
      return;
    }

    if (node.tag == "ASSIGNMENT"_) {
      auto av = culebra::view_assignment(node);
      if (!av.compound) {
        if (const auto* ident_node = culebra::assign_name_target(node, av)) {
          auto name = std::string(ident_node->token);
          bool is_declare = av.is_let || av.is_mut;

          if (is_declare) {
            if (!is_sink_name(name)) {
              if (!locals.insert(name).second) kinds.redeclared.insert(name);
              note(name, /*from_assign=*/true);
            }
          } else if (!visible_in_outer(name, outer) && !is_sink_name(name) &&
                     !is_builtin_var_(name)) {
            // A bare assignment declares only where nothing already answers
            // the name: not in an enclosing scope, and not as a stdlib global
            // (writing one is a reassignment that throws, so the frame gains
            // no binding — a nested read still means the global).
            locals.insert(name);
            note(name, /*from_assign=*/true);
          }
        }
      }
      collect_fn_locals(*node.nodes.back(), locals, outer, kinds);
      return;
    }

    if (node.tag == "CLASS_DECL"_ || node.tag == "ENUM_DECL"_ ||
        node.tag == "MULTIFN_DECL"_) {
      // `class/enum/fn Name ...` binds `Name` in the enclosing scope. The
      // bodies are analyzed separately (visit_for_frees), enum variants are
      // namespaced (`Name.Ok`), not bound bare, and leading DECORATOR
      // children precede the head. Generic params are stripped so
      // `class Pair<K, V>` binds under `Pair`.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        collect_fn_locals(*node.nodes[i], locals, outer, kinds);
        i++;
      }
      auto name =
          std::string(culebra::parse_generic_head(node.nodes[i]->token).outer);
      locals.insert(name);
      note(name, /*from_assign=*/false);
      return;
    }

    if (node.tag == "TRAIT_DECL"_) {
      // trait declarations don't bind a name in the value env (they
      // live in culebra::trait_registry()). Default-method bodies are
      // analyzed in visit_for_frees, not here.
      return;
    }

    if (node.tag == "IMPORT_STMT"_) {
      // `import name from "path"` binds `name` in the enclosing scope.
      auto& id = *node.nodes[0];
      auto name = std::string(id.token);
      locals.insert(name);
      note(name, /*from_assign=*/false);
      return;
    }

    for (auto& c : node.nodes) {
      collect_fn_locals(*c, locals, outer, kinds);
    }
  }

  // If `node` is a `_lazy_ns_register("Ns", fn(){...})` intrinsic call,
  // return its builder FUNCTION/LAMBDA argument (descending single-child
  // wrappers), else nullptr. Mirrors the detection in the stdlib intrinsic.
  const peg::Ast* lazy_ns_builder_arg(const peg::Ast& node) const {
    using namespace peg::udl;
    if (node.tag != "CALL"_ || node.nodes.size() != 2) return nullptr;
    const auto& callee = *node.nodes[0];
    if (callee.tag != "IDENTIFIER"_ ||
        callee.token != "_lazy_ns_register")
      return nullptr;
    const auto& args = *node.nodes[1];
    if (args.original_tag != "ARGUMENTS"_ || args.nodes.size() != 2)
      return nullptr;
    const peg::Ast* builder = args.nodes[1].get();
    while (builder->nodes.size() == 1 && builder->tag != "FUNCTION"_ &&
           builder->tag != "LAMBDA"_)
      builder = builder->nodes[0].get();
    return (builder->tag == "FUNCTION"_ || builder->tag == "LAMBDA"_)
               ? builder
               : nullptr;
  }

  // The function literal an expression IS — the AST optimizer has already
  // folded the EXPRESSION wrapper onto it, so this is a tag test, not a
  // descent: `[fn …]` is an Array holding one, not one.
  static const peg::Ast* fn_literal_of(const peg::Ast& expr) {
    using namespace peg::udl;
    return (expr.tag == "FUNCTION"_ || expr.tag == "LAMBDA"_) ? &expr
                                                               : nullptr;
  }

  // Record `name` as a free variable of the function under analysis when it
  // resolves to an enclosing lexical scope. Shared by the IDENTIFIER read path
  // and the UFCS method-name path.
  //
  // A real binding in an enclosing scope is captured even when `name` also
  // matches a builtin: lexical scope wins, matching the interp (`let Math = 5;
  // fn(){ Math }` sees 5, not the Math namespace; likewise a stdlib module's
  // own helper/class — e.g. Regex's internal `Regex` class — captured by its
  // methods). Only a name with no enclosing binding falls through to builtin /
  // namespace resolution at its use site (compile_identifier → namespace_get).
  // lookup_var resolves captures/slots before builtins, so capture and
  // resolution stay consistent.
  //
  // `optional` marks a UFCS method-name candidate (see optional_free_vars); a
  // genuine read of the same name clears the mark whichever order they appear.
  void note_free_var(const std::string& name,
                     const std::set<std::string>& my_locals,
                     std::vector<const std::set<std::string>*>& outer,
                     FuncInfo& info, bool optional = false) {
    // Any mention of a multifn body's own name — a direct read or a UFCS
    // candidate — turns on the prologue self-handle bind. Checked before
    // the locals cut: the name IS a local of the body (seeded by
    // analyze_fn_common), which is exactly what keeps it out of free_vars.
    if (name == info.own_name) info.own_name_used = true;
    // A local only means "this frame's binding" from the statement that
    // binds it onward. Before that the declaration has not run, so the
    // name still resolves outward — the interp walks the environment
    // chain at the moment of the read, and an enclosing binding (or a
    // global) answers. Fall through to the outer scan so the compiled
    // backends capture the same thing the interp would find.
    if (my_locals.contains(name) && declared_.contains(name)) return;
    // `self` is always capturable: every frame defines a self slot (the
    // receiver, or the lexical fallback), so the outer-scope scan below
    // would never see it in a locals set. Register it unconditionally;
    // the enclosing frame cell-promotes its slot via the merge loop's
    // matching special case, and a frame with no lexical self feeds the
    // capture a NO_SELF cell (emit_closure_build's fallback) so the read
    // guard still raises the interp's NameError.
    if (name == "self") {
      add_free_var(info, name);
      return;
    }
    for (auto* scope : outer) {
      if (scope->contains(name)) {
        if (add_free_var(info, name) && optional)
          info.optional_free_vars.insert(name);
        if (!optional) info.optional_free_vars.erase(name);
        return;
      }
    }
    // In a session unit a name nothing in scope binds is the session's, and
    // the enclosing frame can hand its cell over — so capture it rather than
    // leave the body to look the name up, which it would do on whatever
    // thread runs it (an isolate's has neither the session nor the Runtime
    // the cell was minted in). Not a name the runtime binds itself, and not a
    // UFCS candidate, which is not a read.
    if (session_top_ && !outer.empty() && !optional &&
        !is_always_bound_name(name)) {
      add_free_var(info, name);
      info.optional_free_vars.erase(name);
      return;
    }
    // else: builtin/global (resolved at the use site) or unresolved (runtime
    // NameError) — not a free variable either way.
  }

  void visit_for_frees(const peg::Ast& node,
                       const std::set<std::string>& my_locals,
                       std::vector<const std::set<std::string>*>& outer,
                       FuncInfo& info) {
    using namespace peg::udl;

    if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_ ||
        node.tag == "DEFER"_ || node.tag == "MULTIFN_DECL"_) {
      // Analyze nested function / defer / multimethod body; its
      // locals/frees don't leak into the enclosing scope, but the
      // enclosing scope owns any captured vars (cells) that it
      // references. FUNCTION and LAMBDA share analyze_function (same
      // AST shape: [params, body]; LAMBDA just lacks the optional
      // RETURN_TYPE slot). MULTIFN_DECL has [name, params, body] —
      // analyze_multifn picks params/body off nodes[1] and the last
      // child.
      // MULTIFN_DECL may carry leading DECORATOR children whose
      // expressions live in the enclosing scope (not the fn's inner
      // scope) — visit them directly so any free vars they reference
      // surface to `info`.
      if (node.tag == "MULTIFN_DECL"_) {
        for (auto& child : node.nodes) {
          if (child->tag != "DECORATOR"_) break;
          visit_for_frees(*child, my_locals, outer, info);
        }
      }
      outer.push_back(&my_locals);
      FuncInfo nested_info;
      if (node.tag == "DEFER"_) {
        nested_info = analyze_defer(node, outer);
      } else if (node.tag == "MULTIFN_DECL"_) {
        nested_info = analyze_multifn(node, outer);
      } else {
        std::string_view own_name;
        if (auto it = literal_own_names_.find(&node);
            it != literal_own_names_.end())
          own_name = it->second;
        nested_info = analyze_function(node, outer, own_name);
      }
      outer.pop_back();
      for (const auto& fv : nested_info.free_vars) {
        if (my_locals.contains(fv)) {
          info.captured_locals.insert(fv);
        } else if (fv == "self") {
          // A nested closure captures THIS frame's self slot (dynamic
          // receiver, or our own lexical fallback) — cell-promote it AND
          // keep propagating so our fallback is wired the same way.
          // (An explicit `let self` shadow lands in my_locals and takes
          // the plain-local arm above instead.)
          info.captured_locals.insert(fv);
          add_free_var(info, fv);
        } else {
          // A candidate stays optional as it travels outward: no frame on
          // the way holds a binding for it either.
          if (add_free_var(info, fv) &&
              nested_info.optional_free_vars.contains(fv)) {
            info.optional_free_vars.insert(fv);
          }
          if (!nested_info.optional_free_vars.contains(fv)) {
            info.optional_free_vars.erase(fv);
          }
        }
      }
      return;
    }

    if (node.tag == "ENUM_DECL"_) {
      // Enum variants have no fn bodies — only decorators (evaluated in
      // the enclosing scope) can reference free vars. Explicitly handle
      // it so the generic tail recurse doesn't scan VARIANT identifiers
      // (e.g. `Ok`) as variable references.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        visit_for_frees(*node.nodes[i], my_locals, outer, info);
        i++;
      }
      return;
    }

    if (node.tag == "TRAIT_DECL"_) {
      // Default-impl bodies are nested functions captured from the
      // enclosing scope. Signature-only methods skip analysis.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        visit_for_frees(*node.nodes[i], my_locals, outer, info);
        i++;
      }
      // node.nodes[i] is CLASS_HEAD; methods follow.
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        // A signature-only method analyzes to {} (analyze_trait_method finds
        // no TRAIT_BODY), so its propagate loop is a no-op.
        outer.push_back(&my_locals);
        auto method_info = analyze_trait_method(*node.nodes[j], outer);
        outer.pop_back();
        for (const auto& fv : method_info.free_vars) {
          if (my_locals.contains(fv)) {
            info.captured_locals.insert(fv);
          } else {
            add_free_var(info, fv);
          }
        }
      }
      return;
    }

    if (node.tag == "CLASS_DECL"_) {
      // Each METHOD is a nested function (params + body) that captures
      // from the enclosing scope. Propagate their free_vars exactly
      // like the FUNCTION branch above. Leading DECORATOR children
      // live in the enclosing scope.
      size_t i = 0;
      while (i < node.nodes.size() && node.nodes[i]->tag == "DECORATOR"_) {
        visit_for_frees(*node.nodes[i], my_locals, outer, info);
        i++;
      }
      // A class whose value the declarator loop never touches names
      // itself through its receiver (FuncInfo::own_name) rather than
      // capturing the declaring scope's cell — the ring
      // cls -> ctor -> meta -> method -> cell -> cls. That is every
      // undecorated class, and it is ALSO `@value`/`@packable`/`@derive`,
      // since those are markers the compiler reads itself
      // (`is_compile_time_decorator`) rather than callables
      // `apply_decorators` ever hands the class object to — the same
      // eligibility `compile_class_decl`'s constructor-chunk grant already
      // uses (vm.h). A REAL decorator's class keeps the capture: the
      // binding is what THAT decorator returns, which may not be the class
      // at all. So does a REPL session's top-level class, which a later
      // line may redeclare into the same session cell — and whose binding
      // never drops, so the ring costs nothing there.
      bool all_compile_time = std::all_of(
          node.nodes.begin(), node.nodes.begin() + i,
          [](const auto& d) { return culebra::is_compile_time_decorator(*d); });
      std::string_view class_own;
      if (all_compile_time && !(session_top_ && outer.empty()))
        class_own = culebra::parse_generic_head(node.nodes[i]->token).outer;
      // Typed-field initializers execute per instance inside a synthetic
      // field-init function (invoked after the `new` body's parameter
      // binding, or by build_class_instance for a class with no `new`),
      // not at declaration time — analyze them as one nested function
      // whose FuncInfo is keyed by the CLASS_DECL node itself so
      // compile_class_decl can recover it. It has no locals; `self` is a
      // builtin. Static-field values still evaluate at declaration time
      // in the enclosing scope.
      FuncInfo field_info;
      std::set<std::string> field_locals;
      // The class name is already in declared_ (a scope-wide local of the
      // enclosing frame), so seeding it here is what makes a field
      // initializer's read the frame's own — analyze_fn_common's seed.
      if (!class_own.empty() &&
          field_locals.insert(std::string(class_own)).second) {
        field_info.own_name = std::string(class_own);
        field_info.own_name_source = FuncInfo::OwnNameSource::Receiver;
      }
      auto propagate = [&](const FuncInfo& nested) {
        for (const auto& fv : nested.free_vars) {
          if (my_locals.contains(fv)) {
            info.captured_locals.insert(fv);
          } else {
            add_free_var(info, fv);
          }
        }
      };
      bool has_instance_fields = false;
      // An initializer expression is the only reason a `new` body reaches
      // the field-init closure at all; a plain `x: Float` is stored by the
      // body's own prologue. vm.h's compile_class_decl asks this same
      // question under this same name — keep the two answers together.
      bool fields_need_a_thunk = false;
      // Every `new` overload's body invokes the field-init closure, so each
      // needs the synthetic capture — not just the last one seen.
      std::vector<const peg::Ast*> new_method_asts;
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        const auto& method = *node.nodes[j];
        auto mv = culebra::view_method(method);
        if (mv.is_typed_field || (mv.is_field && !mv.is_static)) {
          has_instance_fields = true;
          if (mv.value) {
            fields_need_a_thunk = true;
            outer.push_back(&my_locals);
            visit_for_frees(*mv.value, field_locals, outer, field_info);
            scan_eh_defer(*mv.value, /*at_fn_top=*/true, field_info);
            outer.pop_back();
          }
          continue;
        }
        if (mv.is_field) {  // static field: declaration-time, enclosing scope
          if (mv.value) visit_for_frees(*mv.value, my_locals, outer, info);
          continue;
        }
        if (!mv.is_static && mv.name == "new") new_method_asts.push_back(&method);
        outer.push_back(&my_locals);
        auto method_info = analyze_method(method, outer, class_own);
        outer.pop_back();
        propagate(method_info);
      }
      // The field-init frame is a receiver frame (build_class_instance /
      // the `new` body always invoke it with the instance as `self`), so
      // its lexical-fallback capture is dead weight — drop it like
      // analyze_fn_common does for METHOD bodies. Must happen before
      // propagate: the enclosing frame would otherwise carry a free
      // `self` without the cell promotion the capture needs.
      std::erase(field_info.free_vars, "self");
      propagate(field_info);
      func_info[&node] = std::move(field_info);
      // Each `new` body invokes the field-init closure right after its
      // parameter binding (interp parity: initializers run only once the
      // ctor args bound successfully). It reaches the closure through a
      // synthetic capture compile_class_decl binds in the class's scope;
      // added AFTER propagate so the hidden name never leaks into the
      // enclosing function's free list (nothing outside resolves it).
      if (has_instance_fields && fields_need_a_thunk) {
        auto slot_name = field_init_slot_name(node);
        for (auto* new_method_ast : new_method_asts)
          add_free_var(func_info[new_method_ast], slot_name);
      }
      return;
    }

    // CALL = primary + postfix chain. A `DOT(name)` (or `SAFE_DOT`)
    // immediately followed by ARGUMENTS is a method call that may resolve
    // via UFCS to a free function `name(receiver, ...)`. If `name` is an
    // outer-scope variable, it must be captured so the nested-fn UFCS
    // lookup (lookup_var at the call site) can find it — otherwise
    // `(5).dbl()` inside a closure would miss the captured `dbl` that
    // `dbl(5)` resolves fine. Builtin method names count too: whether
    // `remove` is the builtin or a UFCS candidate is a property of the
    // receiver, decided at runtime, so the name has to be in reach either
    // way. Bare property access (DOT without a following ARGUMENTS) never
    // uses UFCS, so it's left alone.
    if (node.tag == "CALL"_) {
      // Scope barrier for a lazy-ns builder. The stdlib splices
      // `_lazy_ns_register("Ns", fn(){...})` at entry-module top level; the
      // builder fn is a self-contained module rebuilt per-Runtime via its
      // fn_ptr, so it MUST stay captureless. A UFCS-candidate method name in
      // its body (`comp._step(rv)`) would otherwise be noted as a free var
      // and capture a same-named entry-module user global, breaking the
      // captureless invariant. Analyze the builder with no outer scope so
      // such names resolve as globals (never captures); skip the rest of the
      // CALL so it isn't re-analyzed under the enclosing scope.
      if (const peg::Ast* builder = lazy_ns_builder_arg(node)) {
        std::vector<const std::set<std::string>*> barrier;  // no outer scope
        analyze_function(*builder, barrier);
        return;
      }
      for (size_t i = 0; i < node.nodes.size(); i++) {
        const auto& child = *node.nodes[i];
        bool is_method = (child.original_tag == "DOT"_ ||
                          child.original_tag == "SAFE_DOT"_) &&
                         i + 1 < node.nodes.size() &&
                         node.nodes[i + 1]->original_tag == "ARGUMENTS"_;
        if (is_method) {
          auto mname = std::string(child.token);
          // A call whose receiver is a builtin namespace identifier
          // (`Http.server()`, `Time.sleep()`) is member dispatch, never
          // UFCS — don't capture the member name. Capturing it would pull
          // an outer var of the same name into the closure; if that var is
          // the one being assigned this very closure (e.g.
          // `server = Isolate.spawn(fn{ Http.server() })`) its cell isn't
          // in scope yet and closure-build fails. A local shadowing the
          // namespace name is a real receiver, so require it out of scope.
          // is_method means this DOT is a postfix, so the grammar
          // (CALL <- PRIMARY postfix*) guarantees a receiver at i - 1.
          const auto& recv = *node.nodes[i - 1];
          auto recv_name = std::string(recv.token);
          bool ns_receiver =
              recv.tag == "IDENTIFIER"_ && recv.original_tag != "DOT"_ &&
              is_builtin_var_(recv_name) && !my_locals.contains(recv_name) &&
              std::none_of(outer.begin(), outer.end(), [&](auto* s) {
                return s->contains(recv_name);
              });
          if (!ns_receiver) {
            note_free_var(mname, my_locals, outer, info, /*optional=*/true);
          }
          continue;  // skip the DOT recursion (it would early-return)
        }
        visit_for_frees(child, my_locals, outer, info);
      }
      return;
    }

    // DOT[IDENTIFIER] is a property name, not a variable reference.
    // The AST optimizer collapses the single-child rule so node.tag
    // reads as IDENTIFIER; use original_tag and check before the
    // IDENTIFIER handler below.
    if (node.original_tag == "DOT"_) {
      return;
    }

    if (node.tag == "IDENTIFIER"_) {
      auto name = std::string(node.token);
      // `__ARGS__` is auto-bound by the function prologue; flag use so
      // the prologue can skip the Array allocation when nothing reads it.
      if (name == "__ARGS__") info.uses_args = true;
      // Reading the `fn` handle allocates the bound-handle cache slot.
      if (name == "fn") info.uses_fn = true;
      note_free_var(name, my_locals, outer, info);
      return;
    }

    if (node.tag == "FOR"_) {
      // FOR = [IDENT(var), EXPRESSION(iterable), BLOCK(body), (NOBREAK)?]. The
      // binding is block-scoped to the body — make it visible only
      // while walking the body (by extending `my_locals` for that
      // subtree) so nested closures inside the body can capture it
      // while closures outside the body don't see it. Also register
      // the binding in `info.captured_locals` if any nested closure
      // references it — that flag is what triggers cell promotion when the
      // loop body is emitted.
      auto fv = culebra::view_for(node);
      visit_for_frees(*fv.iter, my_locals, outer, info);
      auto extended = my_locals;
      // A destructuring loop binding (`for (k, v) in …`) binds every leaf of
      // the pattern, not one identifier — collect them all, as MATCH and
      // DESTRUCTURE_ASSIGN do, or a closure in the body sees them as free
      // variables and raises NameError.
      std::vector<std::string> names;
      culebra::for_each_pattern_binding(
          *fv.binding, [&](std::string_view nm, size_t, size_t) {
            names.emplace_back(nm);
            extended.insert(std::string(nm));
          });
      {
        DeclaredBlock body_scope(*this);
        visit_for_frees(*fv.body, extended, outer, info);
      }
      // A `nobreak { … }` runs after the loop with the loop variable OUT of
      // scope, so walk it in `my_locals` (not `extended`) — a closure there
      // must not resolve the loop binding.
      if (fv.nobreak) {
        DeclaredBlock nobreak_scope(*this);
        visit_for_frees(*fv.nobreak, my_locals, outer, info);
      }
      // If the body walk pulled a name into the enclosing function's
      // free-vars (because a nested closure referenced it), we instead
      // mark it captured here and drop it from the free list — the
      // enclosing function owns it.
      for (const auto& name : names) {
        auto it = std::find(info.free_vars.begin(), info.free_vars.end(),
                            name);
        if (it != info.free_vars.end()) {
          info.captured_locals.insert(name);
          info.free_vars.erase(it);
        }
      }
      return;
    }

    if (node.tag == "OBJECT_PROPERTY"_) {
      // `view.value` collapses long form (EXPRESSION) and shorthand
      // (IDENTIFIER read-from-scope) so the walker doesn't branch.
      auto pv = culebra::view_object_property(node);
      visit_for_frees(*pv.value, my_locals, outer, info);
      return;
    }

    // The other two places an IDENTIFIER names something that is not a
    // variable of this scope (OBJECT_PROPERTY above is the third): a keyword
    // argument's label names the callee's parameter, and a pattern entry's
    // key names the property being matched. Walking them as reads made a
    // closure claim the name as free, which is harmless where something
    // binds it and a rejected program where an inner statement list declares
    // it further down. Everything to the right of the label is a read.
    if (node.tag == "KWARG"_ || node.tag == "OBJECT_PAT_ENTRY"_) {
      for (size_t i = 1; i < node.nodes.size(); i++) {
        visit_for_frees(*node.nodes[i], my_locals, outer, info);
      }
      return;
    }

    if (node.tag == "ASSIGNMENT"_) {
      auto av = culebra::view_assignment(node);
      if (av.lvalcnt == 1) {
        // Simple target: `x = expr` / `let x = expr` / `x += expr`.
        // For compound (`x += expr`), x must already exist — visit it as
        // an identifier so the closure-capture analyzer sees the read.
        auto ident_node = node.nodes[av.lvaloff];
        if (ident_node->tag == "IDENTIFIER"_) {
          auto name = std::string(ident_node->token);
          // A builtin name is not skipped here: note_free_var only records a
          // name an enclosing scope actually declares, so `println = 9` inside
          // a closure captures an enclosing `mut println` — and stays a
          // non-capture when nothing shadows the global.
          if ((!av.is_let || av.compound) && !my_locals.contains(name)) {
            visit_for_frees(*ident_node, my_locals, outer, info);
          }
          // A bare write to the body's own name is a write to that
          // (immutable) binding — it has to be bound for the write to be
          // refused, rather than declare a body-local of the same name.
          if (!av.is_let && !av.is_mut && name == info.own_name)
            info.own_name_used = true;
        }
      } else {
        // Complex lvalue: primary + postfixes. TYPE_ANNOTATION and
        // ASSIGN_OP sit between the last lvalue and rhs, so stopping at
        // `lvaloff + lvalcnt` naturally skips them.
        for (int i = 0; i < av.lvalcnt; i++) {
          visit_for_frees(*node.nodes[av.lvaloff + i], my_locals, outer, info);
        }
      }
      // `let name = fn …`: the literal reads `name` as itself (see
      // FuncInfo::own_name) wherever nothing can rebind the name behind
      // it — an immutable binding the list declares once, outside a REPL
      // session's top level.
      if (av.is_let && !av.is_mut && !av.compound && av.lvalcnt == 1 &&
          !(session_top_ && outer.empty())) {
        if (const auto* target = culebra::assign_name_target(node, av)) {
          auto name = std::string(target->token);
          if (!is_sink_name(name) && !redeclared_.contains(name))
            if (const auto* lit = fn_literal_of(*av.rhs))
              literal_own_names_[lit] = std::move(name);
        }
      }
      visit_for_frees(*av.rhs, my_locals, outer, info);
      // The binding exists from here on: a later read of the name is this
      // frame's local, not the enclosing one it resolved to above.
      if (!av.compound && av.lvalcnt == 1) {
        if (const auto* target = culebra::assign_name_target(node, av))
          declared_.insert(std::string(target->token));
      }
      return;
    }

    if (node.tag == "DESTRUCTURE_ASSIGN"_ && node.nodes.size() >= 4) {
      // A destructure binds the leaves this frame declares; those are not
      // reads. The declaring form binds every leaf, the `let`-less form the
      // ones collect_fn_locals made locals (bare `[a, b] = v` declares where
      // bare `a = v` would) and reads the rest, which reassign further out.
      // The right-hand side is walked first either way, so `[a, b] = [a, 1]`
      // still reads the `a` that was in scope before the statement.
      bool declares =
          node.nodes[0]->token == "let" || node.nodes[1]->token == "mut";
      visit_for_frees(*node.nodes[3], my_locals, outer, info);
      for_each_pattern_binding(
          *node.nodes[2], [&](std::string_view nm, size_t, size_t) {
            std::string name(nm);
            if (declares || my_locals.contains(name)) {
              declared_.insert(std::move(name));
              return;
            }
            note_free_var(name, my_locals, outer, info);
          });
      return;
    }

    if (node.tag == "LEXICAL_SCOPE"_) {
      DeclaredBlock block(*this);
      for (auto& c : node.nodes) visit_for_frees(*c, my_locals, outer, info);
      return;
    }

    if (node.tag == "WHILE"_) {
      // The init clause binds for the whole loop (its scope wraps the
      // body); the body and the nobreak tail are each their own.
      auto wv = culebra::view_while(node);
      DeclaredBlock loop(*this);
      if (wv.init) visit_for_frees(*wv.init, my_locals, outer, info);
      visit_for_frees(*wv.cond, my_locals, outer, info);
      {
        DeclaredBlock body(*this);
        visit_for_frees(*wv.body, my_locals, outer, info);
      }
      if (wv.nobreak) {
        DeclaredBlock tail(*this);
        visit_for_frees(*wv.nobreak, my_locals, outer, info);
      }
      return;
    }

    if (node.tag == "TRY"_) {
      // [body BLOCK, catch IDENTIFIER, catch BLOCK] — two sibling scopes;
      // the catch binding itself is scope-wide (collect_fn_locals).
      for (auto& c : node.nodes) {
        DeclaredBlock block(*this);
        visit_for_frees(*c, my_locals, outer, info);
      }
      return;
    }

    if (node.tag == "MATCH_ARM"_) {
      DeclaredBlock arm(*this);
      for (auto& c : node.nodes) visit_for_frees(*c, my_locals, outer, info);
      return;
    }

    for (auto& c : node.nodes) {
      visit_for_frees(*c, my_locals, outer, info);
    }
  }

  // Walk this function's body populating `info.has_eh`,
  // `info.has_fn_defer`, and `scope_has_defer`. Returns true iff the
  // subtree contains a DEFER at the caller's own scope level — used so
  // an enclosing LEXICAL_SCOPE can mark itself without rescanning.
  // Recursion stops at nested FUNCTION / DEFER boundaries (those own
  // their own defers, analyzed separately). `at_fn_top` is true only
  // while still at the function's top level (not inside any `{}`).
  bool scan_eh_defer(const peg::Ast& node, bool at_fn_top, FuncInfo& info) {
    using namespace peg::udl;
    if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_) return false;
    // Flag only; recurse below — the returned expression may contain TRY etc.
    if (node.tag == "RETURN"_) info.has_return = true;
    if (node.tag == "DEFER"_) {
      info.has_any_defer = true;
      ++defer_count_;  // try_region_has_defer's any-depth witness
      if (at_fn_top) {
        info.has_fn_defer = true;
        // Function-level defers run on throw via a cleanup landingpad
        // emitted in compile_fn_common; the IR requires a personality
        // function, gated by `has_eh`.
        info.has_eh = true;
      }
      return true;
    }
    if (node.tag == "TRY"_) {
      info.has_eh = true;
      // TRY = [body, catch_ident, catch_body]. Both blocks are their own
      // scopes (interp's tryEnv / catchEnv each run_deferred at exit):
      // absorb their defers so they fire when the block closes, not at
      // function exit. The counter snapshot sees defers the body's nested
      // scopes absorbed (the return value deliberately does not).
      int defers_before_body = defer_count_;
      if (scan_eh_defer(*node.nodes[0], /*at_fn_top=*/false, info)) {
        scope_has_defer.insert(node.nodes[0].get());
      }
      if (defer_count_ > defers_before_body) try_region_has_defer.insert(&node);
      if (scan_eh_defer(*node.nodes[2], /*at_fn_top=*/false, info)) {
        scope_has_defer.insert(node.nodes[2].get());
      }
      return false;  // the try/catch blocks absorb their own defers
    }
    if (node.tag == "WHILE"_) {
      // The loop safepoint (emit_safepoint) can throw Interrupted on Ctrl+C, so
      // the enclosing function needs a personality to unwind through.
      info.has_eh = true;
      // nodes: [(INIT_CLAUSE)?, condition EXPRESSION, BLOCK]. Like FOR, the
      // body is a per-iteration scope: a defer in it fires each iteration, not
      // at function exit. Scan any init clause + the condition at the enclosing
      // level; absorb the body's defers into the body node (mark it) so they
      // don't bubble up to has_fn_defer. Matches compile_while + eval_while.
      auto wv = culebra::view_while(node);
      if (wv.init) scan_eh_defer(*wv.init, at_fn_top, info);
      scan_eh_defer(*wv.cond, at_fn_top, info);
      if (scan_eh_defer(*wv.body, /*at_fn_top=*/false, info)) {
        scope_has_defer.insert(wv.body);
      }
      // The nobreak block is its own scope (like the body): absorb its defers
      // so they fire at its exit, not at function exit.
      if (wv.nobreak &&
          scan_eh_defer(*wv.nobreak, /*at_fn_top=*/false, info)) {
        scope_has_defer.insert(wv.nobreak);
      }
      return false;  // the body / nobreak absorb their own defers
    }
    if (node.tag == "FOR"_) {
      // for-in over the iterator protocol (Object iter() path) emits an
      // exception landingpad in compile_for_protocol_loop so iter.dispose()
      // fires even when the body throws. The landingpad requires the
      // enclosing function to carry a personality, so flag it here even
      // though no try/defer is present.
      info.has_eh = true;
      // nodes: [pattern, iterable, BLOCK, (NOBREAK_CLAUSE)?]. The body is a
      // per-iteration scope: a defer in it fires each iteration (docs: "defer
      // in a loop body fires on every iteration"), not at function exit. Scan
      // the body / nobreak with at_fn_top=false and absorb their defers (mark
      // the node) so they don't bubble up to has_fn_defer; pattern/iterable
      // stay at the enclosing level.
      auto fv = culebra::view_for(node);
      scan_eh_defer(*fv.binding, at_fn_top, info);
      scan_eh_defer(*fv.iter, at_fn_top, info);
      if (scan_eh_defer(*fv.body, /*at_fn_top=*/false, info)) {
        scope_has_defer.insert(fv.body);
      }
      if (fv.nobreak &&
          scan_eh_defer(*fv.nobreak, /*at_fn_top=*/false, info)) {
        scope_has_defer.insert(fv.nobreak);
      }
      return false;  // the body / nobreak absorb their own defers
    }
    if (node.tag == "LEXICAL_SCOPE"_) {
      bool inner = false;
      for (auto& c : node.nodes) inner |= scan_eh_defer(*c, false, info);
      if (inner) {
        scope_has_defer.insert(&node);
        info.has_eh = true;
      }
      return false;  // this scope absorbs its own defers
    }
    if (node.tag == "MATCH"_) {
      // nodes: [(INIT_CLAUSE)?, subject EXPRESSION, MATCH_ARMS]. A block arm's
      // body is its own scope (like a lexical block): a defer in it fires at arm
      // exit, not function exit (matching interp's eval_match, which runs the arm
      // scope's deferred on exit). Scan any init clause + subject + each arm's
      // pattern/guard at the enclosing level; absorb each arm body's defers into
      // the body node.
      auto mv = culebra::view_match(node);
      if (mv.init) scan_eh_defer(*mv.init, at_fn_top, info);
      scan_eh_defer(*mv.subject, at_fn_top, info);
      for (auto& arm : mv.arms->nodes) {
        for (size_t i = 0; i + 1 < arm->nodes.size(); i++)
          scan_eh_defer(*arm->nodes[i], at_fn_top, info);
        auto& body = *arm->nodes.back();
        if (scan_eh_defer(body, /*at_fn_top=*/false, info)) {
          scope_has_defer.insert(&body);
          info.has_eh = true;
        }
      }
      return false;  // arm bodies absorb their own defers
    }
    bool any = false;
    for (auto& c : node.nodes) any |= scan_eh_defer(*c, at_fn_top, info);
    return any;
  }

  // Body of the shared analysis for user-defined callable AST nodes
  // (FUNCTION or METHOD). `params_ast` / `body_ast` are supplied
  // explicitly because METHOD puts its IDENTIFIER at index 0, pushing
  // the params / body down by one slot. `info_key` is the AST pointer
  // used to key `func_info` — callers point at the outer FUNCTION /
  // METHOD node so `compile_*` can recover the analysis result later.
  FuncInfo analyze_fn_common(
      const peg::Ast* info_key,
      const peg::Ast& params_ast,
      const peg::Ast& body_ast,
      std::vector<const std::set<std::string>*>& outer,
      std::string_view own_name = {},
      FuncInfo::OwnNameSource own_name_source =
          FuncInfo::OwnNameSource::Closure) {
    std::set<std::string> my_locals;
    DeclKinds kinds;
    for (auto& p : params_ast.nodes) {
      if (culebra::is_kw_only_sep(*p)) continue;
      // A destructuring param (`fn ({a, b})`) binds the pattern's names,
      // not a single identifier — extract_param_name_loc would index a
      // non-existent IDENTIFIER child (flaky OOB read). Mirror the interp
      // analyzer and collect the pattern's bindings.
      if (culebra::is_pattern_param(*p)) {
        for_each_pattern_binding(
            *p, [&](std::string_view nm, size_t, size_t) {
              my_locals.insert(std::string(nm));
              kinds.scope_wide.insert(std::string(nm));
            });
        continue;
      }
      auto [name_sv, line, col] = culebra::extract_param_name_loc(*p);
      auto name = std::string(name_sv);
      my_locals.insert(name);
      kinds.scope_wide.insert(name);
    }

    FuncInfo info;
    // The body's own name becomes a body-level local (the prologue binds
    // it — see FuncInfo::own_name), unless a same-named parameter shadows
    // it. Seeded before collect so a nested fn's reference lands in
    // captured_locals like any local's would.
    if (!own_name.empty() && !culebra::is_sink_name(own_name) &&
        my_locals.insert(std::string(own_name)).second) {
      info.own_name = std::string(own_name);
      info.own_name_source = own_name_source;
      kinds.scope_wide.insert(info.own_name);
    }
    collect_fn_locals(body_ast, my_locals, outer, kinds);

    DeclaredScope declared(*this, my_locals, kinds);
    for (auto& p : params_ast.nodes) {
      if (culebra::is_kw_only_sep(*p) || culebra::is_kwargs_rest(*p)) continue;
      if (auto* def = extract_default_expr(*p)) {
        visit_for_frees(*def, my_locals, outer, info);
      }
    }
    visit_for_frees(body_ast, my_locals, outer, info);
    scan_eh_defer(body_ast, true, info);

    // Receiver frames (methods, trait defaults) always arrive with a
    // dispatched receiver — every invoke path passes one, and a detached
    // read binds it into the wrapper (culebra_runtime_bind_method_value).
    // Their lexical-fallback capture of an enclosing `self` is therefore
    // dead weight: drop it. captured_locals keeps "self" so a nested
    // closure inside the method still cell-captures the receiver slot.
    {
      using namespace peg::udl;
      if (info_key->tag == "METHOD"_ || info_key->tag == "TRAIT_METHOD"_) {
        std::erase(info.free_vars, "self");
      }
    }

    func_info[info_key] = info;
    return info;
  }

  // Shared by FUNCTION ([PARAMETERS, (RETURN_TYPE)?, BLOCK]) and LAMBDA
  // ([LAMBDA_PARAMS, BODY]) — both have params at index 0, but a declared
  // return type shifts the body, so find it through view_function.
  FuncInfo analyze_function(
      const peg::Ast& fnAst,
      std::vector<const std::set<std::string>*>& outer,
      std::string_view own_name = {}) {
    auto fv = culebra::view_function(fnAst);
    return analyze_fn_common(&fnAst, *fv.params, *fv.body, outer, own_name);
  }

  // METHOD ast: [IDENTIFIER, PARAMETERS, BLOCK]. Analyzed just like a
  // nested FUNCTION — the implicit `self` arrives as the dispatched
  // receiver (analyze_fn_common drops the lexical-fallback capture).
  // `class_own` is the class's name for a member that reads it through its
  // receiver (FuncInfo::own_name), empty when the class is decorated.
  FuncInfo analyze_method(
      const peg::Ast& methodAst,
      std::vector<const std::set<std::string>*>& outer,
      std::string_view class_own = {}) {
    auto mv = culebra::view_method(methodAst);
    return analyze_fn_common(&methodAst, *mv.params, **mv.body, outer,
                             class_own, FuncInfo::OwnNameSource::Receiver);
  }

  // TRAIT_METHOD: only default-body methods need analysis (sig-only
  // methods carry no body to walk). `view_trait_method` finds the
  // optional TRAIT_BODY regardless of where it lands relative to the
  // optional RETURN_TYPE sibling.
  FuncInfo analyze_trait_method(
      const peg::Ast& traitMethodAst,
      std::vector<const std::set<std::string>*>& outer) {
    auto tv = culebra::view_trait_method(traitMethodAst);
    if (!tv.body) return {};
    return analyze_fn_common(&traitMethodAst, *tv.params, *tv.body, outer);
  }

  // MULTIFN_DECL ast: [IDENTIFIER, PARAMETERS, [RETURN_TYPE,] BLOCK].
  // Analyzed like a nested FUNCTION — body is the last child.
  FuncInfo analyze_multifn(
      const peg::Ast& multifnAst,
      std::vector<const std::set<std::string>*>& outer) {
    using namespace peg::udl;
    // Skip leading DECORATOR children — params live right after the
    // IDENTIFIER (which is itself right after the decorators).
    size_t name_idx = 0;
    while (name_idx < multifnAst.nodes.size() &&
           multifnAst.nodes[name_idx]->tag == "DECORATOR"_) {
      name_idx++;
    }
    auto paramsIdx = name_idx + 1;
    auto bodyIdx = multifnAst.nodes.size() - 1;
    // An undecorated body gets its own name as the prologue-bound
    // self-handle (FuncInfo::own_name). A decorated one keeps the plain
    // capture: its binding is the decorator's result, which only the
    // declaring scope's cell knows.
    auto self_name =
        name_idx == 0
            ? culebra::parse_generic_head(multifnAst.nodes[0]->token).outer
            : std::string_view{};
    return analyze_fn_common(&multifnAst, *multifnAst.nodes[paramsIdx],
                             *multifnAst.nodes[bodyIdx], outer, self_name,
                             FuncInfo::OwnNameSource::Dispatch);
  }

  // `defer { BODY }` behaves like a 0-parameter nested function that
  // closes over the enclosing scope. Shadow checks are unneeded (no
  // params, no let/mut at the defer line itself).
  FuncInfo analyze_defer(
      const peg::Ast& deferAst,
      std::vector<const std::set<std::string>*>& outer) {
    std::set<std::string> my_locals;
    DeclKinds kinds;
    collect_fn_locals(*deferAst.nodes[0], my_locals, outer, kinds);

    FuncInfo info;
    DeclaredScope declared(*this, my_locals, kinds);
    visit_for_frees(*deferAst.nodes[0], my_locals, outer, info);
    scan_eh_defer(*deferAst.nodes[0], true, info);

    func_info[&deferAst] = info;
    return info;
  }
};

}  // namespace culebra
