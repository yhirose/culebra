// Generator (yield) AST transformation pass.
//
// A `fn` whose body contains `yield` / `yield from` is rewritten, at
// parse time, into an anonymous class implementing the Iterator
// protocol (iter / has_next / next / dispose), then re-parsed — the
// interp / JIT / AOT runtimes execute the synthesized class with no
// generator-specific support of their own.
//
//   fn name(...) { ... yield ... }
//     ->
//   fn name(...) {
//     class _Gen_<name>_<line>_<col> { new(...) {...} iter()/has_next()/
//                                      next()/dispose() }
//     _Gen_<name>_<line>_<col>.new(...)
//   }
//
// The body is lowered by a flat-dispatch CPS state machine (see
// `CpsBuilder`): each basic block becomes a state, control flow becomes
// `this._g_state = K; continue` jumps over one `while true` dispatch
// loop, and every local lives on the instance (all-locals-on-heap, so no
// liveness analysis). Two source-level pre-passes run first: the C# rule
// rejects yield inside try-catch/defer, and yielding for-in loops are
// desugared to `while it.has_next()` form. See
// [[project-generator-design]] for the design axes and the frozen spec.

#pragma once

#include "parser.h"

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <set>
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

// True if `node` carries at least one YIELD or YIELD_FROM anywhere
// inside, stopping at fn boundaries (see `is_fn_boundary`). Treating
// the two as equivalent here lets Stage 5's for-in desugar fire even
// when the only yield-shaped node inside is a `yield from`.
inline bool fn_body_has_yield(const peg::Ast& node) {
  using namespace peg::udl;
  if (node.tag == "YIELD"_ || node.tag == "YIELD_FROM"_) return true;
  if (is_fn_boundary(node.tag)) return false;
  for (auto& c : node.nodes) {
    if (fn_body_has_yield(*c)) return true;
  }
  return false;
}

// Collect every DEFER node in a fn body in source order, stopping at
// inner fn boundaries. Used by the dispatcher to verify that all defers
// live at the body's top level (where Stage 3 emits them into the
// generator's defer registry); defers buried inside loop/if bodies have
// no good translation today and surface as a SyntaxError.
inline std::vector<const peg::Ast*> collect_defers(const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (is_fn_boundary(n.tag)) return;
    if (n.tag == "DEFER"_) { out.push_back(&n); return; }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Locate the first YIELD reachable from inside a TRY's try-block / catch
// block, or from inside a DEFER's body. Returns nullptr if no such yield
// exists. Used by the dispatcher to enforce the C# rule (CS1626): yield
// statements may not appear inside a try-catch or defer. `yield try {...}
// catch e {...}` is fine because the yield is OUTSIDE the try (only the
// try-expression's value flows through it) — the guard descends into a
// TRY's body BLOCKs but not into its position as an expression.
inline const peg::Ast* find_yield_inside_try_or_defer(const peg::Ast& body) {
  using namespace peg::udl;
  const peg::Ast* found = nullptr;
  std::function<void(const peg::Ast&, bool)> walk =
      [&](const peg::Ast& n, bool inside_guard) {
        if (found) return;
        if (is_fn_boundary(n.tag)) return;
        if (inside_guard && (n.tag == "YIELD"_ || n.tag == "YIELD_FROM"_)) {
          found = &n;
          return;
        }
        if (n.tag == "TRY"_) {
          // TRY children: try-BLOCK, catch-IDENT, catch-BLOCK
          if (n.nodes.size() > 0) walk(*n.nodes[0], true);
          if (n.nodes.size() > 2) walk(*n.nodes[2], true);
          return;
        }
        if (n.tag == "DEFER"_) {
          if (!n.nodes.empty()) walk(*n.nodes[0], true);
          return;
        }
        for (auto& c : n.nodes) walk(*c, inside_guard);
      };
  walk(body, false);
  return found;
}

// Locate the first named function definition (`fn name(...) { ... }`, a
// MULTIFN_DECL) that appears as a statement inside a generator body — at the
// top level or nested in its control flow (if / for / while / block / try),
// but NOT inside a nested fn VALUE (an anonymous `fn (...) {...}` / `|x| ...`,
// which opens its own scope and binds fine). Returns nullptr if none. Used to
// reject the construct uniformly: the JIT's CPS lowering doesn't bind such a
// definition in the generator's state frame, so interp and JIT diverge.
inline const peg::Ast* find_nested_fndef(const peg::Ast& body) {
  using namespace peg::udl;
  const peg::Ast* found = nullptr;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    for (auto& c : n.nodes) {
      if (found) return;
      if (c->tag == "MULTIFN_DECL"_) {
        found = c.get();
        return;
      }
      // A nested fn VALUE keeps its own scope; its body is not part of the
      // generator's flattened frame, so leave it (and its inner defs) alone.
      if (c->tag == "FUNCTION"_ || c->tag == "LAMBDA"_) continue;
      walk(*c);
    }
  };
  walk(body);
  return found;
}

// Slice the source range an AST node spans. peg::Ast carries
// `position` + `length` in bytes; safe so long as `src` is the same
// buffer that produced `node`.
inline std::string_view ast_source_slice(const peg::Ast& node,
                                          const char* src, size_t src_len) {
  if (node.position + node.length > src_len) return {};
  return std::string_view(src + node.position, node.length);
}

// --- Source-text helpers -------------------------------------------------

// Drop the enclosing `{` / `}` from a BLOCK source slice. A BLOCK that's
// been parsed will always be wrapped in braces, but the check stays
// defensive in case the slice is already brace-stripped or empty.
inline std::string_view strip_block_braces(std::string_view s) {
  if (s.size() >= 2 && s.front() == '{' && s.back() == '}') {
    return s.substr(1, s.size() - 2);
  }
  return s;
}

// Rewrite every standalone `<name>` in `src` to `this.<name>` for each
// name in `names`, then strip `let `/`mut ` prefixes that now sit in
// front of `this.X` (assignment, no longer declaration).
//
// The match requires the identifier NOT be preceded by `.` — so a
// member access like `arr.size()` is left alone even when a local/param
// is named `size`. Without this, `\bsize\b` rewrote the `.size()` method
// name to `arr.this.size()`, producing malformed source (a parser crash
// for any generator whose binding collides with a builtin method like
// size / push / keys). The same `.`-exclusion also prevents re-rewriting
// the `this.` prefixes this pass just inserted. The `..` range operator is
// exempted from that exclusion: `0..n`'s `n` is an operand, not a member,
// so a range bound naming a local/param (`for i in 0..n`, desugared to
// `(0..n).iter()`) must still be rewritten to `0..this.n`. String-literal
// and comment false positives remain out of scope (regex, not a real lexer).
inline std::string rewrite_locals_to_this(std::string_view src,
                                          const std::set<std::string>& names) {
  // The two declaration-strip patterns are name-independent — hoist
  // them to file scope so each Stage 2 transform doesn't recompile
  // them. `std::regex` construction is the famously expensive part.
  static const std::regex strip_let_this(R"(\blet\s+this\.)");
  static const std::regex strip_mut_this(R"(\bmut\s+this\.)");
  std::string out(src);
  std::vector<std::string> sorted(names.begin(), names.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.size() > b.size(); });
  for (const auto& name : sorted) {
    // Group 1 captures the boundary (start-of-string, the `..` range
    // operator, or any byte that is neither `.` nor an identifier char) so
    // it can be restored ahead of the inserted `this.`. `..` is listed
    // before the single-char class so a range bound is matched as an operand
    // rather than mistaken for a member access on its second dot.
    std::regex pat("(^|\\.\\.|[^.A-Za-z0-9_])" + name + "\\b");
    out = std::regex_replace(out, pat, "$1this." + name);
  }
  out = std::regex_replace(out, strip_let_this, "this.");
  out = std::regex_replace(out, strip_mut_this, "this.");
  return out;
}

// Collect every name introduced by a `let`/`mut` decl in a fn body
// (single-lvalue form only; multi-lvalue chains skipped). Stops at
// nested fn boundaries.
inline std::set<std::string> collect_local_names(const peg::Ast& body) {
  using namespace peg::udl;
  std::set<std::string> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    // A nested `handle` opens its own computation scope; its locals belong to
    // that inner computation, not this one (they must not be rewritten to this
    // instance's `this.`). Generators carry no HANDLE, so this is a no-op there.
    if (is_fn_boundary(n.tag) || n.tag == "HANDLE"_) return;
    if (n.tag == "ASSIGNMENT"_) {
      auto av = view_assignment(n);
      if ((av.is_let || av.is_mut) && av.lvalcnt == 1) {
        const auto& ident = *n.nodes[av.lvaloff];
        if (ident.tag == "IDENTIFIER"_) out.insert(std::string(ident.token));
      }
    }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Unwrap a STATEMENT wrapper down to the concrete tag (CLASS_DECL,
// WHILE, ASSIGNMENT, ...). AstOptimizer collapses most STATEMENTs
// already, but the wrapper survives for the choice-tagged variants.
inline const peg::Ast* unwrap_stmt(const peg::Ast* s) {
  using namespace peg::udl;
  if (s && s->tag == "STATEMENT"_ && !s->nodes.empty()) {
    return s->nodes[0].get();
  }
  return s;
}

// View the children of a fn body (BLOCK / STATEMENTS / single stmt) as
// a flat vector of statement-shaped nodes.
inline std::vector<const peg::Ast*> body_stmts(const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  if (body.tag == "STATEMENTS"_) {
    for (auto& c : body.nodes) out.push_back(c.get());
  } else {
    out.push_back(&body);
  }
  return out;
}

// --- Stage 5: for-in desugar to while + iterator -------------------------

// Outermost FOR nodes (within a single fn) that contain at least one YIELD
// somewhere in their subtree. Stops at fn boundaries and does NOT descend
// into yielding FORs — those are rewritten as a unit, so any inner FOR
// nested within them is left alone for a follow-up pass. The walker only
// surfaces FORs whose yields belong to *this* generator.
inline std::vector<const peg::Ast*> collect_outermost_yielding_fors(
    const peg::Ast& body) {
  using namespace peg::udl;
  std::vector<const peg::Ast*> out;
  std::function<void(const peg::Ast&)> walk = [&](const peg::Ast& n) {
    if (is_fn_boundary(n.tag)) return;
    if (n.tag == "FOR"_ && fn_body_has_yield(n)) {
      out.push_back(&n);
      return;
    }
    for (auto& c : n.nodes) walk(*c);
  };
  walk(body);
  return out;
}

// Source-level rewrite: each outermost yielding `for x in expr BODY` becomes
// `let _g_it_<pos> = (expr).iter()
//  while _g_it_<pos>.has_next() {
//    let x = _g_it_<pos>.next()
//    BODY_inner
//  }`
// Returns the rewritten body source, or `nullopt` when there were no
// yielding FORs to rewrite (so callers can gate the desugar pipeline on a
// single walk). Iterator variable names use the FOR's source position to
// stay unique. The result has fresh source positions once re-parsed;
// callers must re-parse before walking the AST again.
inline std::optional<std::string> rewrite_yielding_fors_to_while(
    const peg::Ast& body, const char* src, size_t src_len) {
  auto fors = collect_outermost_yielding_fors(body);
  if (fors.empty()) return std::nullopt;
  std::string out(ast_source_slice(body, src, src_len));
  size_t base = body.position;
  for (auto it = fors.rbegin(); it != fors.rend(); ++it) {
    auto* f = *it;
    if (f->nodes.size() < 3) continue;
    const auto& var_node = *f->nodes[0];
    const auto& expr_node = *f->nodes[1];
    const auto& blk_node = *f->nodes[2];
    auto expr_sv = ast_source_slice(expr_node, src, src_len);
    auto blk_inner = strip_block_braces(
        ast_source_slice(blk_node, src, src_len));
    auto iter_var = std::format("_g_it_{}", f->position);
    auto replacement = std::format(
        "let {0} = ({1}).iter()\n"
        "while {0}.has_next() {{\n"
        "  let {2} = {0}.next()\n"
        "  {3}\n"
        "}}",
        iter_var, std::string(expr_sv),
        std::string(var_node.token), std::string(blk_inner));
    out.replace(f->position - base, f->length, replacement);
  }
  return out;
}

// --- Transformation entry points -----------------------------------------

// Re-parse a `fn __gen_wrapper__(...) { ... }` source fragment and return
// its MULTIFN_DECL node, after registering the source for lifetime. peg::Ast
// holds string_views into the source, so the synthesized buffer must stay
// alive until the AST is discarded — generator_transform_sources() owns it.
// Parse a synthesized buffer after registering it for process lifetime (the
// resulting AST's string_views point into it). The single place that pairs the
// lifetime store with `parse` — used by both the generator wrapper parse and
// the effects transform's body re-parse, so the "register before parse" rule
// lives in one spot.
inline std::shared_ptr<peg::Ast> parse_registered_source(
    const char* label, std::shared_ptr<std::string> synthesized) {
  generator_transform_sources().push_back(synthesized);
  std::vector<std::string> msgs;
  return parse(label, synthesized->data(), synthesized->size(), msgs);
}

inline std::shared_ptr<peg::Ast> parse_wrapper_fn(
    std::shared_ptr<std::string> synthesized) {
  using namespace peg::udl;
  auto sub_ast = parse_registered_source("<generator-transform>", synthesized);
  if (!sub_ast) return nullptr;
  std::shared_ptr<peg::Ast> wrapper_fn;
  std::function<void(std::shared_ptr<peg::Ast>)> walk =
      [&](std::shared_ptr<peg::Ast> n) {
        if (wrapper_fn) return;
        if (n->tag == "MULTIFN_DECL"_) { wrapper_fn = n; return; }
        for (auto& c : n->nodes) walk(c);
      };
  walk(sub_ast);
  return wrapper_fn;
}

// Replace `ast->nodes.back()` (the original body BLOCK) with the BLOCK
// from a freshly-parsed `fn __gen_wrapper__() { ... }` source fragment.
// Shared by Stage 1 and Stage 2 — both end with "now swap the body".
inline bool swap_body_from_wrapper(std::shared_ptr<peg::Ast> ast,
                                   std::shared_ptr<std::string> synthesized) {
  auto wrapper_fn = parse_wrapper_fn(synthesized);
  if (!wrapper_fn) return false;
  ast->nodes.back() = wrapper_fn->nodes.back();
  return true;
}

// Replace `ast`'s PARAMETERS (at `params_idx`) and body with those from a
// freshly-parsed `fn __gen_wrapper__(<params>) { <body> }` source. Keeps the
// original name (and decorators) intact, while giving downstream stages an
// AST whose param + body positions point into the synthesized source — the
// shape Stage 3 needs after the Stage 5 for-in desugar rewrites the body.
inline bool swap_body_with_wrapper_params(
    std::shared_ptr<peg::Ast> ast,
    std::shared_ptr<std::string> synthesized,
    size_t params_idx) {
  auto wrapper_fn = parse_wrapper_fn(synthesized);
  if (!wrapper_fn || wrapper_fn->nodes.size() < 3) return false;
  if (ast->nodes.size() <= params_idx + 1) return false;
  ast->nodes[params_idx + 1] = wrapper_fn->nodes[1];
  ast->nodes.back() = wrapper_fn->nodes.back();
  return true;
}

// Wire the generated class's `new(_p0, _p1, ...)` slot list and append
// matching `this.<param> = _pN` initializers, then `this.<local> = nil`
// for every body-declared local not shadowed by a param. Shared between
// Stage 3 and Stage 6b — both stages seed `ctor_inits` with their own
// state-field prefix (`_g_drained` / `_g_phase` / etc.) before calling
// this to append the per-instance bindings.
// Positional parameter names of a PARAMETERS node, skipping the kw-only
// separator and any `**kwargs` rest. Shared by the generator and effects
// transforms — both feed the names into ctor slot emission, so the skip rules
// must stay in lockstep.
inline std::vector<std::string_view> collect_positional_param_names(
    const peg::Ast& params_ast) {
  std::vector<std::string_view> names;
  for (const auto& pn : params_ast.nodes) {
    if (is_kw_only_sep(*pn) || is_kwargs_rest(*pn)) continue;
    names.push_back(view_parameter(*pn).name);
  }
  return names;
}

inline void emit_ctor_param_and_local_inits(
    const std::vector<std::string_view>& param_names,
    const std::set<std::string>& locals,
    std::string& ctor_params,
    std::string& ctor_call_args,
    std::string& ctor_inits) {
  std::set<std::string> param_set;
  for (size_t j = 0; j < param_names.size(); j++) {
    auto pn = std::string(param_names[j]);
    auto slot = std::format("_p{}", j);
    if (j > 0) {
      ctor_params += ", ";
      ctor_call_args += ", ";
    }
    ctor_params += slot;
    ctor_call_args += pn;
    ctor_inits += std::format("      this.{} = {}\n", pn, slot);
    param_set.insert(std::move(pn));
  }
  for (const auto& l : locals) {
    if (!param_set.contains(l)) {
      ctor_inits += std::format("      this.{} = nil\n", l);
    }
  }
}

// --- CPS engine: flat-dispatch state machine -----------------------------
//
// Linearizes a generator body into a flat list of states. Each state is a
// culebra statement sequence ending in a transition: a yield (set
// lookahead, advance state, `return true`), a delegate hand-off
// (`yield from`), or a `this._g_state = K; continue` jump. has_next() is
// one `while true` dispatch loop over the states. Because culebra's yield
// is a STATEMENT (never an expression) and locals live on the heap
// (this.X), the linearizer works statement-by-statement with no
// expression splitting and no liveness analysis; "flat" dispatch (every
// basic block is a state, edges set a counter) sidesteps the relooper
// problem of reconstructing structured loops. See
// [[project-generator-design]] §CPS.
//
// Handles plain stmts / yield / yield from / if-elseif-else / while
// (incl. nested) / break / continue / return / defer. for-in is
// desugared to while by an upstream pre-pass. A yielding `match` arm is
// grammatically impossible (yield is a statement, match arms are
// expressions), so match never carries a yield to lower.

// True if `node` contains a break/continue that targets an *enclosing*
// loop (i.e. not nested inside a while/for within `node`), or a return
// anywhere. Such statements can't be emitted verbatim into the dispatch
// loop — break/continue/return must become state jumps. A self-contained
// inner loop with its own internal break stays verbatim-safe (the break
// binds to that real inner loop). Stops at fn boundaries.
inline bool has_escaping_loop_ctrl(const peg::Ast& node, int loop_depth = 0) {
  using namespace peg::udl;
  if (is_fn_boundary(node.tag)) return false;
  if (node.tag == "RETURN"_) return true;
  if ((node.tag == "BREAK"_ || node.tag == "CONTINUE"_) && loop_depth == 0) {
    return true;
  }
  int inner = (node.tag == "WHILE"_ || node.tag == "FOR"_) ? loop_depth + 1
                                                           : loop_depth;
  for (auto& c : node.nodes) {
    if (has_escaping_loop_ctrl(*c, inner)) return true;
  }
  return false;
}

struct CpsBuilder {
  const char* src;
  size_t src_len;
  const std::set<std::string>& rewrite_set;
  std::vector<std::string> states;
  int terminal = -1;  // state that sets drained + returns false
  // (header, exit) of each enclosing CPS-managed loop, innermost last.
  std::vector<std::pair<int, int>> loop_stack;
  // `defer { B }` bodies, in source order. Each is registered (a
  // `_g_defer_K` flag set true) when its state is reached and run at
  // dispose in reverse (LIFO).
  std::vector<std::string> defer_bodies;
  bool failed = false;

  int fresh() {
    states.emplace_back();
    return static_cast<int>(states.size()) - 1;
  }
  std::string rw(const peg::Ast& n) {
    return rewrite_locals_to_this(ast_source_slice(n, src, src_len),
                                  rewrite_set);
  }
  // A fresh state that just jumps to `target` (break/continue/return).
  int jump_state(int target) {
    int e = fresh();
    states[e] = std::format("      this._g_state = {}\n      continue\n",
                            target);
    return e;
  }

  // True if this statement needs structural compilation: it contains a
  // yield anywhere, a break/continue/return escaping to an enclosing loop
  // / the generator, or a defer (which must register into the dispose
  // registry, not fire as a has_next-scope defer). Everything else is
  // verbatim-safe.
  static bool needs_split(const peg::Ast& s) {
    return fn_body_has_yield(s) || has_escaping_loop_ctrl(s) ||
           !collect_defers(s).empty();
  }

  // Linearize `stmts`, returning the entry state. `cont` is the state to
  // jump to after the sequence completes. Maximal runs of yield-free
  // statements collapse into a single state.
  int compile_seq(const std::vector<const peg::Ast*>& stmts, int cont) {
    int k = cont;
    std::string pending;
    auto flush = [&]() {
      if (pending.empty()) return;
      int s = fresh();
      states[s] = pending +
                  std::format("      this._g_state = {}\n      continue\n", k);
      k = s;
      pending.clear();
    };
    for (size_t idx = stmts.size(); idx-- > 0;) {
      const peg::Ast* s = stmts[idx];
      if (!needs_split(*s)) {
        pending = "      " + rw(*s) + "\n" + pending;
      } else {
        flush();
        k = compile_stmt(s, k);
        if (failed) return -1;
      }
    }
    flush();
    return k;
  }

  int compile_stmt(const peg::Ast* s, int cont) {
    using namespace peg::udl;
    auto* u = unwrap_stmt(s);
    if (u->tag == "YIELD"_ && !u->nodes.empty()) {
      int e = fresh();
      states[e] = std::format(
          "      this._g_la = ({})\n"
          "      this._g_has_la = true\n"
          "      this._g_state = {}\n"
          "      return true\n",
          rw(*u->nodes[0]), cont);
      return e;
    }
    if (u->tag == "YIELD_FROM"_ && !u->nodes.empty()) {
      int e = fresh();
      states[e] = std::format(
          "      this._g_delegate = ({}).iter()\n"
          "      this._g_state = {}\n"
          "      continue\n",
          rw(*u->nodes[0]), cont);
      return e;
    }
    if (u->tag == "BREAK"_) {
      if (loop_stack.empty()) { failed = true; return -1; }
      return jump_state(loop_stack.back().second);
    }
    if (u->tag == "CONTINUE"_) {
      if (loop_stack.empty()) { failed = true; return -1; }
      return jump_state(loop_stack.back().first);
    }
    if (u->tag == "RETURN"_) {
      // Generators ignore a return value; `return` just ends iteration.
      return jump_state(terminal);
    }
    if (u->tag == "DEFER"_ && !u->nodes.empty()) {
      // Register the defer body when reached; dispose runs it (LIFO).
      int k = static_cast<int>(defer_bodies.size());
      defer_bodies.push_back(rewrite_locals_to_this(
          strip_block_braces(ast_source_slice(*u->nodes[0], src, src_len)),
          rewrite_set));
      int e = fresh();
      states[e] = std::format(
          "      this._g_defer_{} = true\n      this._g_state = {}\n"
          "      continue\n",
          k, cont);
      return e;
    }
    if (u->tag == "IF"_) return compile_if(u, cont);
    if (u->tag == "WHILE"_ && u->nodes.size() >= 2) {
      int h = fresh();
      loop_stack.push_back({h, cont});
      int body_entry = compile_seq(body_stmts(*u->nodes[1]), h);
      loop_stack.pop_back();
      if (failed) return -1;
      states[h] = std::format(
          "      if {} {{ this._g_state = {} }} else {{ this._g_state = {} }}\n"
          "      continue\n",
          rw(*u->nodes[0]), body_entry, cont);
      return h;
    }
    if (u->tag == "LEXICAL_SCOPE"_ || u->tag == "STATEMENTS"_) {
      return compile_seq(body_stmts(*u), cont);
    }
    failed = true;  // anything unexpected
    return -1;
  }

  // if / else-if / else chain. IF nodes are [cond, block, cond, block, ...,
  // elseblock?]; a trailing odd node is the bare `else` block.
  int compile_if(const peg::Ast* ifnode, int cont) {
    const auto& nodes = ifnode->nodes;
    size_t n = nodes.size();
    int else_entry;
    size_t pairs;
    if (n % 2 == 1) {
      else_entry = compile_seq(body_stmts(*nodes[n - 1]), cont);
      if (failed) return -1;
      pairs = (n - 1) / 2;
    } else {
      else_entry = cont;
      pairs = n / 2;
    }
    int chain = else_entry;
    for (size_t p = pairs; p-- > 0;) {
      int block_entry = compile_seq(body_stmts(*nodes[2 * p + 1]), cont);
      if (failed) return -1;
      int s = fresh();
      states[s] = std::format(
          "      if {} {{ this._g_state = {} }} else {{ this._g_state = {} }}\n"
          "      continue\n",
          rw(*nodes[2 * p]), block_entry, chain);
      chain = s;
    }
    return chain;
  }
};

// CPS transform entry. Returns the transformed ast on success, or the
// original ast unchanged when the body uses a construct outside the
// engine's scope (caller then reports / falls back).
inline std::shared_ptr<peg::Ast> transform_one_generator_fn_cps(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len,
    const peg::Ast& name_ast, const peg::Ast& params_ast) {
  using namespace peg::udl;

  auto gen_name = std::format("_Gen_{}_{}_{}",
                              std::string(name_ast.token),
                              name_ast.line, name_ast.column);

  auto param_names = collect_positional_param_names(params_ast);
  auto locals = collect_local_names(*ast->nodes.back());
  std::set<std::string> rewrite_set = locals;
  for (auto& pn : param_names) rewrite_set.insert(std::string(pn));

  CpsBuilder b{src, src_len, rewrite_set, {}};
  b.terminal = b.fresh();
  b.states[b.terminal] =
      "      this._g_drained = true\n      return false\n";
  int entry = b.compile_seq(body_stmts(*ast->nodes.back()), b.terminal);
  if (b.failed || entry < 0) return ast;  // unsupported shape

  std::string ctor_params;
  std::string ctor_call_args;
  std::string ctor_inits = std::format(
      "      this._g_drained = false\n"
      "      this._g_has_la = false\n"
      "      this._g_la = nil\n"
      "      this._g_disposed = false\n"
      "      this._g_delegate = nil\n"
      "      this._g_state = {}\n",
      entry);
  for (size_t k = 0; k < b.defer_bodies.size(); k++) {
    ctor_inits += std::format("      this._g_defer_{} = false\n", k);
  }
  emit_ctor_param_and_local_inits(param_names, locals,
                                  ctor_params, ctor_call_args, ctor_inits);

  std::string dispatch;
  for (size_t id = 0; id < b.states.size(); id++) {
    dispatch += std::format(
        "      if this._g_state == {} {{\n{}      }}\n", id, b.states[id]);
  }

  // Registered defers run LIFO at dispose, each gated on its reach flag.
  // `defer_bodies` is already in reverse source order (compile_seq walks
  // statements back-to-front), so iterating it forward IS the LIFO order.
  std::string defer_runs;
  for (size_t k = 0; k < b.defer_bodies.size(); k++) {
    defer_runs += std::format(
        "      if this._g_defer_{} {{ {} }}\n", k, b.defer_bodies[k]);
  }

  auto synthesized = std::make_shared<std::string>(std::format(
      "fn __gen_wrapper__() {{\n"
      "  class {0} {{\n"
      "    new({1}) {{\n{2}    }}\n"
      "    iter() {{ this }}\n"
      "    has_next() {{\n"
      "      while true {{\n"
      "        if this._g_drained {{ return false }}\n"
      "        if this._g_has_la {{ return true }}\n"
      "        if this._g_delegate != nil {{\n"
      "          if this._g_delegate.has_next() {{\n"
      "            this._g_la = this._g_delegate.next()\n"
      "            this._g_has_la = true\n"
      "            return true\n"
      "          }}\n"
      "          if this._g_delegate.has('dispose') {{ this._g_delegate.dispose() }}\n"
      "          this._g_delegate = nil\n"
      "        }}\n"
      "{3}"
      "      }}\n"
      "    }}\n"
      "    next() {{\n"
      "      if !this._g_has_la {{ this.has_next() }}\n"
      "      let _v = this._g_la\n"
      "      this._g_la = nil\n"
      "      this._g_has_la = false\n"
      "      return _v\n"
      "    }}\n"
      "    dispose() {{\n"
      "      if this._g_disposed {{ return nil }}\n"
      "      this._g_disposed = true\n"
      "      this._g_drained = true\n"
      "      if this._g_delegate != nil {{\n"
      "        if this._g_delegate.has('dispose') {{ this._g_delegate.dispose() }}\n"
      "        this._g_delegate = nil\n"
      "      }}\n"
      "{5}"
      "    }}\n"
      "  }}\n"
      "  {0}.new({4})\n"
      "}}\n",
      gen_name, ctor_params, ctor_inits, dispatch, ctor_call_args,
      defer_runs));
  swap_body_from_wrapper(ast, synthesized);
  return ast;
}

// Dispatcher: lower a yield-carrying fn to the flat-dispatch CPS state
// machine. Two source-level pre-passes run first: the C# rule rejects
// yield inside try-catch/defer, and the for-in desugar rewrites every
// yielding `for x in e` to `while it.has_next()` form (to a fixpoint so
// nested for-ins are covered) since the CPS engine works over while.
// Everything else — straight-line yields, while/if branching, multiple
// yields per iteration, post-loop tails, break/continue/return, defer,
// yield from — is handled by the one engine.
inline std::shared_ptr<peg::Ast> transform_one_generator_fn(
    std::shared_ptr<peg::Ast> ast, const char* src, size_t src_len) {
  using namespace peg::udl;
  size_t i = 0;
  while (i < ast->nodes.size() && ast->nodes[i]->tag == "DECORATOR"_) i++;
  if (i + 2 >= ast->nodes.size()) return ast;
  if (!fn_body_has_yield(*ast->nodes.back())) return ast;

  // C# rule (CS1626): a yield statement may not appear inside a try-catch
  // or defer block. yield-spanning try is permanently out of scope
  // ([[project-generator-design]] §仕様凍結項目). Cleanup belongs in a
  // top-level `defer { ... }`; value-level recovery in the yielded
  // expression (`yield try { ... } catch e { ... }`).
  if (auto* bad = find_yield_inside_try_or_defer(*ast->nodes.back())) {
    throw CulebraError(
        "SyntaxError",
        "yield cannot appear inside a try-catch or defer block. Move "
        "the try to the yielded expression value (yield try { ... } "
        "catch e { ... }) or use a top-level `defer { ... }` for cleanup.",
        static_cast<long>(bad->line),
        static_cast<long>(bad->column));
  }

  // A named function definition inside a generator body has no good CPS
  // lowering — the JIT doesn't bind it in the generator's state frame, so it
  // raised NameError while the interp ran it, an interp/JIT divergence. Reject
  // it uniformly (like the yield-in-try rule). Anonymous fn / lambda VALUES
  // (`let f = |x| ...` / `let f = fn (x) { ... }`) work and are unaffected.
  if (auto* fd = find_nested_fndef(*ast->nodes.back())) {
    throw CulebraError(
        "SyntaxError",
        "a named function definition cannot appear inside a generator body "
        "(a function that uses yield). Bind a lambda instead (let f = |x| ... "
        "/ let f = fn (x) { ... }) or define the function outside the "
        "generator.",
        static_cast<long>(fd->line),
        static_cast<long>(fd->column));
  }

  // Desugar yielding for-in loops to while + iterator, re-parsing each
  // time, until none remain (nested for-ins surface as outermost on the
  // next pass). After the swap, params/body positions point into the
  // synthesized source, so `src`/`src_len` track it.
  while (auto rewritten = rewrite_yielding_fors_to_while(
             *ast->nodes.back(), src, src_len)) {
    auto params_sv = ast_source_slice(*ast->nodes[i + 1], src, src_len);
    auto body_inner = strip_block_braces(*rewritten);
    auto desugared = std::make_shared<std::string>(std::format(
        "fn __gen_wrapper__{} {{\n{}\n}}\n",
        std::string(params_sv), std::string(body_inner)));
    if (!swap_body_with_wrapper_params(ast, desugared, i)) return ast;
    src = desugared->data();
    src_len = desugared->size();
    if (!fn_body_has_yield(*ast->nodes.back())) return ast;
  }

  const auto& name_ast = *ast->nodes[i];
  const auto& params_ast = *ast->nodes[i + 1];
  auto orig_body = ast->nodes.back();
  auto out = transform_one_generator_fn_cps(ast, src, src_len, name_ast,
                                            params_ast);
  if (out->nodes.back().get() != orig_body.get()) return out;

  // CPS left the body untouched — it hit a construct it can't lower
  // (e.g. a yielding `match` arm, which the grammar already forbids, or a
  // break/continue outside any loop).
  throw CulebraError(
      "SyntaxError",
      "unsupported control flow in a generator body (yield reachable "
      "through a construct the generator transform can't lower).",
      static_cast<long>(name_ast.line),
      static_cast<long>(name_ast.column));
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

// Parse + the generator transformation pass. The public entry
// `parse_with_transforms` (effects_transform.h) chains the effects pass
// after this one; callers route through the public entry.
inline std::shared_ptr<peg::Ast> parse_with_generator_transforms(
    const std::string& path, const char* expr, size_t len,
    std::vector<std::string>& msgs) {
  auto ast = parse(path, expr, len, msgs);
  if (!ast) return ast;
  return transform_generators_in(ast, expr, len);
}

}  // namespace culebra
