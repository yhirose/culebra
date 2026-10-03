#pragma once

// Front-end analysis over the AST, shared by every backend that compiles
// functions (docs/internals/vm.md §4): per-function capture sets (derived
// from resolve.h), own names, and EH/defer emission flags. Lifted out of jit.h so the
// bytecode compiler can consume the same passes as the JIT; deliberately
// LLVM-free.

#include <frontend/lint.h>
#include <frontend/parser.h>
#include <frontend/resolve.h>
#include <base/shared.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace culebra {

// Analysis result for a function (including the top-level main program).
struct FuncInfo {
  std::vector<std::string> free_vars;   // captured from outer
  // Free vars that only a method call `v.name(...)` names (a UFCS
  // candidate), never read as a variable. Where the compiler finds no binding
  // for one, the receiver's own method answers, so resolve_captures (vm.h)
  // feeds it a nil cell instead of refusing the closure.
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
inline bool is_field_init_slot_name(std::string_view name) {
  return name.starts_with("\x1f__finit_");
}

// How the compiler resolves a module (resolve.h): with the stdlib's global
// names, and in a session the names earlier inputs declared. The captures
// (FnAnalysis) and the agreement check (scope_check.h) are both held to it.
inline resolve::Options compiler_resolve_options(
    std::span<const std::string> session = {}) {
  return {.record_nodes = true, .globals = lint::builtin_names(),
          .session = session};
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
  // namespaces/...): what tells a stdlib namespace receiver (member
  // dispatch, never UFCS) apart. Injected as a plain predicate so the
  // analysis stays independent of the backend's extension machinery.
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

  // `session`: the names earlier inputs of a session declared, when
  // `session_top` (resolve::Options::session).
  FuncInfo analyze_program(const peg::Ast& programAst,
                           bool session_top = false,
                           std::span<const std::string> session = {}) {
    session_top_ = session_top;
    // Shadow analysis is single-sourced in lint.h (the same check the
    // interpreter runs).
    lint::check_shadow(programAst);
    res_ = resolve::resolve_module(programAst, {},
                                   compiler_resolve_options(session));
    fns_.clear();
    FuncInfo info;
    depth_ = 0;
    visit(programAst, info);
    scan_eh_defer(programAst, true, info);
    derive_captures(info);
    return info;
  }

 private:
  IsBuiltinVar is_builtin_var_;
  int defer_count_ = 0;  // DEFER nodes seen so far, across all scans
  // The program being analyzed is a REPL line: its top-level `let`s are
  // session cells a later line may rebind (analyze_program's flag).
  bool session_top_ = false;
  // The module being analyzed, resolved: what every name means.
  culebra::resolve::Resolution res_;
  // How many functions deep the walk is (0: the module's top level).
  int depth_ = 0;
  // This module's functions: each function scope res_ gives one, to the
  // FuncInfo its walk filled (std::map nodes stay put).
  std::unordered_map<size_t, FuncInfo*> fns_;
  // `let name = fn …` literals whose body reads `name` as itself: filled
  // by the assignment's walk, read when the literal's own walk starts.
  std::map<const peg::Ast*, std::string> literal_own_names_;

  // free_vars keeps first-seen order, so "add if absent" is a linear probe
  // rather than a set.
  static void add_free_var(FuncInfo& info, const std::string& name) {
    auto& fvs = info.free_vars;
    if (std::find(fvs.begin(), fvs.end(), name) == fvs.end())
      fvs.push_back(name);
  }

  // A nested function reading the receiver `self` captures this frame's:
  // cell-promote it, and keep propagating so an enclosing receiver reaches it
  // the same way. (A `self` some declaration names is a variable resolve.h
  // resolves, captured like any other.)
  static void propagate_self(const FuncInfo& nested, FuncInfo& info) {
    if (std::find(nested.free_vars.begin(), nested.free_vars.end(), "self") ==
        nested.free_vars.end())
      return;
    info.captured_locals.insert("self");
    add_free_var(info, "self");
  }

  // What a name node resolves to, kNone when nothing declares it.
  size_t symbol_of(const peg::Ast& name) const { return res_.symbol_of(name); }

  // A method call's receiver that is a stdlib namespace, which the call
  // reaches by member dispatch, never through UFCS.
  bool namespace_receiver(const peg::Ast& recv) const {
    using namespace peg::udl;
    if (recv.tag != "IDENTIFIER"_ || recv.original_tag == "DOT"_ ||
        !is_builtin_var_(std::string(recv.token)))
      return false;
    return symbol_of(recv) == culebra::resolve::kNone;
  }

  // `let name = fn …` names the literal after itself only when nothing can
  // rebind the name behind it: the variable is declared once.
  bool declared_once(const peg::Ast& target) const {
    size_t sym = symbol_of(target);
    return sym != culebra::resolve::kNone && res_.symbols[sym].declarations == 1;
  }

  void visit(const peg::Ast& node, FuncInfo& info) {
    using namespace peg::udl;

    if (node.tag == "FUNCTION"_ || node.tag == "LAMBDA"_ ||
        node.tag == "DEFER"_ || node.tag == "MULTIFN_DECL"_) {
      // A decorator's expression runs in the enclosing scope.
      if (node.tag == "MULTIFN_DECL"_) {
        for (auto& child : node.nodes) {
          if (child->tag != "DECORATOR"_) break;
          visit(*child, info);
        }
      }
      if (node.tag == "DEFER"_) {
        propagate_self(analyze_fn_common(&node, nullptr, *node.nodes[0]), info);
      } else if (node.tag == "MULTIFN_DECL"_) {
        propagate_self(analyze_multifn(node), info);
      } else {
        std::string_view own_name;
        if (auto it = literal_own_names_.find(&node);
            it != literal_own_names_.end())
          own_name = it->second;
        auto fv = culebra::view_function(node);
        propagate_self(analyze_fn_common(&node, fv.params, *fv.body, own_name),
                       info);
      }
      return;
    }

    if (node.tag == "ENUM_DECL"_) {
      // Only decorators (evaluated in the enclosing scope) can read anything;
      // variant names are not variables.
      for (auto& c : node.nodes) {
        if (c->tag != "DECORATOR"_) break;
        visit(*c, info);
      }
      return;
    }

    if (node.tag == "TRAIT_DECL"_) {
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t d = 0; d < i; d++) visit(*node.nodes[d], info);
      // node.nodes[i] is CLASS_HEAD; methods follow. A default body is a
      // receiver frame, so it captures no enclosing `self`.
      for (size_t j = i + 1; j < node.nodes.size(); j++) {
        auto tv = culebra::view_trait_method(*node.nodes[j]);
        if (tv.body) analyze_fn_common(node.nodes[j].get(), tv.params, *tv.body);
      }
      return;
    }

    if (node.tag == "CLASS_DECL"_) {
      size_t i = culebra::first_non_decorator_index(node);
      for (size_t d = 0; d < i; d++) visit(*node.nodes[d], info);
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
      if (all_compile_time && !(session_top_ && depth_ == 0))
        class_own = culebra::parse_generic_head(node.nodes[i]->token).outer;
      // Typed-field initializers execute per instance inside a synthetic
      // field-init function (invoked after the `new` body's parameter
      // binding, or by build_class_instance for a class with no `new`),
      // not at declaration time — analyze them as one nested function
      // whose FuncInfo is keyed by the CLASS_DECL node itself so
      // compile_class_decl can recover it; `self` is a builtin there.
      // Static-field values still evaluate at declaration time in the
      // enclosing scope.
      FuncInfo field_info;
      if (!class_own.empty()) {
        field_info.own_name = std::string(class_own);
        field_info.own_name_source = FuncInfo::OwnNameSource::Receiver;
      }
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
        if (mv.instance_field()) {
          has_instance_fields = true;
          if (mv.value) {
            fields_need_a_thunk = true;
            depth_++;
            visit(*mv.value, field_info);
            depth_--;
            scan_eh_defer(*mv.value, /*at_fn_top=*/true, field_info);
          }
          continue;
        }
        if (mv.is_field) {  // static field: declaration-time, enclosing scope
          if (mv.value) visit(*mv.value, info);
          continue;
        }
        if (!mv.is_static && mv.name == "new") new_method_asts.push_back(&method);
        analyze_fn_common(&method, mv.params, **mv.body, class_own,
                          FuncInfo::OwnNameSource::Receiver);
      }
      // The field-init frame is a receiver frame (build_class_instance /
      // the `new` body always invoke it with the instance as `self`), so
      // its lexical-fallback capture is dead weight.
      std::erase(field_info.free_vars, "self");
      record(&node, res_.initializer_scope, &node, std::move(field_info));
      // Each `new` body invokes the field-init closure right after its
      // parameter binding (interp parity: initializers run only once the
      // ctor args bound successfully). It reaches the closure through a
      // synthetic capture compile_class_decl binds in the class's scope,
      // which nothing outside resolves.
      if (has_instance_fields && fields_need_a_thunk) {
        auto slot_name = field_init_slot_name(node);
        for (auto* new_method_ast : new_method_asts)
          add_free_var(func_info[new_method_ast], slot_name);
      }
      return;
    }

    if (node.tag == "CALL"_) {
      // The builder `_lazy_ns_register("Ns", fn(){...})` registers is a
      // self-contained module rebuilt per Runtime, so it must stay
      // captureless (resolve.h resolves it as a root of its own).
      if (const peg::Ast* builder = lazy_ns_builder(node)) {
        auto fv = culebra::view_function(*builder);
        int saved = std::exchange(depth_, 0);
        analyze_fn_common(builder, fv.params, *fv.body);
        depth_ = saved;
        return;
      }
      // `v.name(...)` may reach the body's own name through UFCS, unless the
      // receiver is a stdlib namespace (member dispatch).
      for (size_t i = 1; i + 1 < node.nodes.size(); i++) {
        const auto& c = *node.nodes[i];
        if ((c.original_tag == "DOT"_ || c.original_tag == "SAFE_DOT"_) &&
            node.nodes[i + 1]->original_tag == "ARGUMENTS"_ &&
            c.token == info.own_name && !namespace_receiver(*node.nodes[i - 1]))
          info.own_name_used = true;
      }
    }

    // A member name (`.x`, `?.x`), an object key, a keyword argument's label
    // and a pattern entry's key are not variables.
    if (node.original_tag == "DOT"_ || node.original_tag == "SAFE_DOT"_) return;
    if (node.tag == "OBJECT_PROPERTY"_) {
      // A computed key is an expression like any other (resolve.h's walk).
      auto pv = culebra::view_object_property(node);
      if (!pv.is_shorthand && pv.key->tag != "IDENTIFIER"_) visit(*pv.key, info);
      visit(*pv.value, info);
      return;
    }
    if (node.tag == "KWARG"_ || node.tag == "OBJECT_PAT_ENTRY"_) {
      for (size_t i = 1; i < node.nodes.size(); i++) visit(*node.nodes[i], info);
      return;
    }

    if (node.tag == "IDENTIFIER"_) {
      // `__ARGS__` is auto-bound by the function prologue; flag use so
      // the prologue can skip the Array allocation when nothing reads it.
      if (node.token == "__ARGS__") info.uses_args = true;
      // Reading the `fn` handle allocates the bound-handle cache slot.
      if (node.token == "fn") info.uses_fn = true;
      // Any mention of the body's own name turns on its prologue bind.
      if (node.token == info.own_name) info.own_name_used = true;
      // `self` is always capturable: every frame defines a self slot (the
      // receiver, or the lexical fallback), and a frame with no lexical self
      // feeds the capture a NO_SELF cell (emit_closure_build's fallback) so
      // the read guard still raises the interp's NameError.
      if (node.token == "self" && symbol_of(node) == culebra::resolve::kNone)
        add_free_var(info, "self");
      return;
    }

    if (node.tag == "ASSIGNMENT"_) {
      auto av = culebra::view_assignment(node);
      if (av.lvalcnt == 1) {
        // A simple target that is not a declaration is a read too: a bare
        // write to the body's own name (an immutable binding) has to be bound
        // for the write to be refused. A bare `self = v` is refused by the
        // frame's own receiver, which it does not capture.
        const auto& ident = *node.nodes[av.lvaloff];
        bool receiver_write = ident.token == "self" && !av.compound;
        if (ident.tag == "IDENTIFIER"_ && !av.is_mut &&
            (!av.is_let || av.compound) && !receiver_write)
          visit(ident, info);
      } else {
        // Complex lvalue: primary + postfixes. TYPE_ANNOTATION and
        // ASSIGN_OP sit between the last lvalue and rhs, so stopping at
        // `lvaloff + lvalcnt` naturally skips them.
        for (int i = 0; i < av.lvalcnt; i++)
          visit(*node.nodes[av.lvaloff + i], info);
      }
      // `let name = fn …`: the literal reads `name` as itself (see
      // FuncInfo::own_name) wherever nothing can rebind the name behind it,
      // and not at a REPL line's top level, where a later line may.
      if (av.is_let && !av.is_mut && !av.compound && av.lvalcnt == 1 &&
          !(session_top_ && depth_ == 0)) {
        if (const auto* target = culebra::assign_name_target(node, av))
          if (!is_sink_name(target->token) && declared_once(*target))
            if (const auto* lit = fn_literal_of(*av.rhs))
              literal_own_names_[lit] = std::string(target->token);
      }
      visit(*av.rhs, info);
      return;
    }

    for (auto& c : node.nodes) visit(*c, info);
  }

  // The function literal an expression IS — the AST optimizer has already
  // folded the EXPRESSION wrapper onto it, so this is a tag test, not a
  // descent: `[fn …]` is an Array holding one, not one.
  static const peg::Ast* fn_literal_of(const peg::Ast& expr) {
    using namespace peg::udl;
    return (expr.tag == "FUNCTION"_ || expr.tag == "LAMBDA"_) ? &expr
                                                               : nullptr;
  }

  // Each name a parameter binds. A destructuring param (`fn ({a, b})`)
  // binds the pattern's names, not a single identifier —
  // extract_param_name_loc would index a non-existent IDENTIFIER child
  // (flaky OOB read).
  template <class F>
  static void for_each_param_name(const peg::Ast& p, F&& f) {
    if (culebra::is_pattern_param(p)) {
      for_each_pattern_binding(
          p, [&](std::string_view nm, size_t, size_t) { f(nm); });
      return;
    }
    f(culebra::extract_param_name_loc(p).name);
  }

  // Every function's captures, from what resolve.h says each name means: a
  // name a function reads (or writes) whose variable another function
  // declares is a free variable of each function from the reader out to the
  // declaring one, which captures it. A body's own name is the one the
  // prologue binds (FuncInfo::own_name), so it stops there. A method call
  // `v.name(...)` makes `name` an optional free variable (a UFCS candidate),
  // unless the receiver is a stdlib namespace. In a session, a name no
  // statement of this input declares — an earlier input's, or one nothing
  // declares — is the session's: a free variable out to the top, captured by
  // none, short of a function that declares the name itself. The receiver
  // `self` and the field-init slot stay as the walk (visit) found them.
  void derive_captures(FuncInfo& top) {
    namespace rs = culebra::resolve;
    const rs::Resolution& r = res_;
    fns_[0] = &top;
    std::unordered_map<size_t, size_t> own_symbol;
    for (const auto& [f, info] : fns_)
      if (f != 0 && !info->own_name.empty())
        own_symbol[f] = r.lookup(r.scopes[f].parent, info->own_name);

    // In a session, the names each function declares in any of its scopes.
    std::set<std::pair<size_t, std::string_view>> declares;
    if (session_top_)
      for (size_t sym = 0; sym < r.symbols.size(); sym++)
        if (r.symbols[sym].declarations > 0)
          declares.emplace(r.frame_of(sym), r.symbols[sym].name);

    // Names are views of the AST's tokens, which outlive this.
    struct Found {
      std::unordered_map<std::string_view, size_t> free;  // first position
      std::set<std::string_view> optional, read, captured;
    };
    std::unordered_map<size_t, Found> found;
    auto reach = [&](size_t sym, std::string_view name, size_t scope,
                     size_t position, bool optional) {
      size_t own = sym;  // what the name means, the session's included
      if (session_top_ && sym != rs::kNone && !r.symbols[sym].declared_at)
        sym = rs::kNone;
      size_t home = sym == rs::kNone ? rs::kNone : r.frame_of(sym);
      size_t from = r.function_of(scope);
      if (sym == rs::kNone && (!session_top_ || from == 0)) return;
      for (size_t f = from; f != rs::kNone && f != home;
           f = r.function_of(r.scopes[f].parent)) {
        // A session name stops at a function declaring the name in a scope of
        // its own: bound across the function, it would turn that scope's own
        // write into a reassignment. The reader binds the session's cell
        // where it builds its closure (vm.h's resolve_captures).
        if (sym == rs::kNone && declares.contains({f, name})) return;
        if (auto o = own_symbol.find(f);
            own != rs::kNone && o != own_symbol.end() && o->second == own) {
          if (f != from) found[f].captured.insert(name);
          return;
        }
        auto& here = found[f];
        auto [it, fresh] = here.free.try_emplace(name, position);
        if (!fresh) it->second = std::min(it->second, position);
        (optional ? here.optional : here.read).insert(name);
        if (sym != rs::kNone && r.function_of(r.scopes[f].parent) == home)
          found[home].captured.insert(name);
      }
    };
    for (const auto& [node, use] : r.uses)
      reach(use.symbol, node->token, use.scope, node->position, false);
    for (const auto& m : r.method_calls)
      if (m.symbol != rs::kNone && !namespace_receiver(*m.receiver))
        reach(m.symbol, m.node->token, m.scope, m.node->position, true);

    for (auto& [f, info] : fns_) {
      auto& here = found[f];
      std::vector<std::string> free;
      for (const auto& n : info->free_vars)
        if (n == "self" || is_field_init_slot_name(n)) free.push_back(n);
      std::vector<std::pair<size_t, std::string_view>> by_position;
      for (const auto& [n, pos] : here.free) by_position.emplace_back(pos, n);
      std::sort(by_position.begin(), by_position.end());
      for (const auto& [pos, n] : by_position)
        if (std::find(free.begin(), free.end(), n) == free.end())
          free.emplace_back(n);
      info->free_vars = std::move(free);
      info->optional_free_vars.clear();
      for (const auto& n : here.optional)
        if (!here.read.contains(n)) info->optional_free_vars.emplace(n);
      bool self = info->captured_locals.contains("self");
      info->captured_locals.clear();
      for (const auto& n : here.captured) info->captured_locals.emplace(n);
      if (self) info->captured_locals.insert("self");
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

  // The shared analysis of a function-like body: a FUNCTION / LAMBDA, a
  // METHOD, a trait's default body, a `fn name`, a `defer`, a lazy builder.
  // `info_key` is the node func_info keys it by, which compile_* recovers it
  // from.
  const FuncInfo& analyze_fn_common(
      const peg::Ast* info_key, const peg::Ast* params_ast,
      const peg::Ast& body_ast, std::string_view own_name = {},
      FuncInfo::OwnNameSource own_name_source =
          FuncInfo::OwnNameSource::Closure) {
    using namespace peg::udl;
    FuncInfo info;
    // The body's own name is the prologue's (FuncInfo::own_name), unless a
    // same-named parameter shadows it.
    bool shadowed = false;
    if (params_ast)
      for (auto& p : params_ast->nodes)
        if (!culebra::is_kw_only_sep(*p))
          for_each_param_name(*p, [&](std::string_view nm) {
            if (nm == own_name) shadowed = true;
          });
    if (!own_name.empty() && !culebra::is_sink_name(own_name) && !shadowed) {
      info.own_name = std::string(own_name);
      info.own_name_source = own_name_source;
    }
    depth_++;
    if (params_ast)
      for (auto& p : params_ast->nodes)
        if (!culebra::is_kw_only_sep(*p) && !culebra::is_kwargs_rest(*p))
          if (const auto* def = extract_default_expr(*p)) visit(*def, info);
    visit(body_ast, info);
    depth_--;
    scan_eh_defer(body_ast, true, info);
    // Receiver frames (methods, trait defaults) always arrive with a
    // dispatched receiver — every invoke path passes one, and a detached
    // read binds it into the wrapper (culebra_runtime_bind_method_value).
    // Their lexical-fallback capture of an enclosing `self` is therefore
    // dead weight: drop it. captured_locals keeps "self" so a nested
    // closure inside the method still cell-captures the receiver slot.
    if (info_key->tag == "METHOD"_ || info_key->tag == "TRAIT_METHOD"_)
      std::erase(info.free_vars, "self");
    return record(info_key, res_.body_scope, &body_ast, std::move(info));
  }

  // Keep `info` under `key`, and remember it as the function scope `scopes`
  // maps `scope_key` to (derive_captures fills its captures).
  const FuncInfo& record(
      const peg::Ast* key,
      const std::unordered_map<const peg::Ast*, size_t>& scopes,
      const peg::Ast* scope_key, FuncInfo info) {
    FuncInfo& slot = func_info[key] = std::move(info);
    if (auto it = scopes.find(scope_key); it != scopes.end())
      fns_[it->second] = &slot;
    return slot;
  }

  // MULTIFN_DECL ast: [DECORATOR*, IDENTIFIER, PARAMETERS, [RETURN_TYPE,]
  // BLOCK]. An undecorated body gets its own name as the prologue-bound
  // self-handle (FuncInfo::own_name). A decorated one keeps the plain
  // capture: its binding is the decorator's result, which only the
  // declaring scope's cell knows.
  const FuncInfo& analyze_multifn(const peg::Ast& multifnAst) {
    size_t name_idx = culebra::first_non_decorator_index(multifnAst);
    auto self_name =
        name_idx == 0
            ? culebra::parse_generic_head(multifnAst.nodes[0]->token).outer
            : std::string_view{};
    return analyze_fn_common(&multifnAst, multifnAst.nodes[name_idx + 1].get(),
                             *multifnAst.nodes.back(), self_name,
                             FuncInfo::OwnNameSource::Dispatch);
  }
};

}  // namespace culebra
