#pragma once

// Sendable: the value-transfer layer for isolates ([[project-concurrency-c2]]).
//
// An isolate runs on its own OS thread with its own GC heap (thread_local
// Runtime + InterpGC, see shared.h). Script Values therefore CANNOT be moved
// or referenced across the boundary — the two heaps must never share a
// pointer, or one heap's GC would free the other's objects. So a value
// crossing the boundary is SERIALIZED into a neutral, heap-free representation
// (SendNode) on the sender thread and RECONSTRUCTED on the receiver thread.
//
// This is ONE generic recursive walk — no per-type / per-namespace special
// casing beyond the two unavoidable ones: (a) un-sendable values (native
// handles / Tensor / functions with no rebuildable body) throw SendError at
// the walk; (b) closures are split into their immutable AST (shareable,
// process-lifetime) plus their captured free-variable VALUES (serialized
// recursively, with memoization so a recursive function like `fib` — which
// references itself — terminates). Data cycles (an Array/Object containing
// itself) are rejected with SendError for the MVP.
//
// Closures are rebuilt on the receiver via Interpreter::rebuild_closure, which
// re-creates the FunctionValue against the receiver's heap from the retained
// FunctionValue::params_ast/body. A FunctionValue with `body == nullptr` is a
// native/stdlib function and is not Sendable.

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "stdlib_interp.h"  // Value, Environment, Interpreter, environment()

namespace culebra::sendable {

// ---------------------------------------------------------------------------
// Neutral representation. Owns no GC-heap pointer. The only cross-heap-safe
// shared_ptr it may hold is shared_ptr<peg::Ast> (process-lifetime, atomic
// control block).
// ---------------------------------------------------------------------------
struct SendNode {
  enum class K { Nil, Bool, Long, Float, Str, Array, Object, Set, Tuple,
                 Closure, Channel, SharedBuffer, SharedVal };
  K kind = K::Nil;

  bool b = false;  // Bool; also Channel role (false = tx, true = rx)
  long i = 0;      // Long; also Channel id (into the process-wide registry)
  double d = 0.0;
  std::string s;   // Str

  std::vector<SendNode> elems;  // Array / Set / Tuple

  // Object: ordered (key, value) pairs + parallel mutability flags. Keys are
  // themselves SendNodes (String keys → K::Str; non-String keys serialize as
  // values). A nested by-value Entry struct can't be defined inside the still-
  // incomplete SendNode, but a vector of std::pair<SendNode,SendNode> is fine.
  std::vector<std::pair<SendNode, SendNode>> entries;
  std::vector<bool> entry_mut;

  // Closure — interp form: defining AST + named free-var captures.
  const peg::Ast* params_ast = nullptr;            // borrowed, process-lifetime
  std::shared_ptr<peg::Ast> body;                  // shared, thread-safe
  std::string return_type;
  std::vector<std::pair<std::string, SendNode>> captures;  // free var → value
  // Closure — JIT form: shared native code pointer (process-lifetime while the
  // LLJIT is alive) + positional captures in `elems` + arity in `i`. The two
  // forms never mix: an interp program ships interp closures, a JIT program
  // ships JIT closures. `jit_fn != nullptr` selects the JIT form.
  void* jit_fn = nullptr;
  // Closure — JIT multifn dispatcher (`fn name`): the dispatcher thunk shares
  // fn_ptr, but its overload methods live in a thread_local table, so they are
  // shipped explicitly (method bodies in `elems`, types/variadic parallel) and
  // re-registered on the child. mf_name non-empty selects this form.
  std::string mf_name;
  std::vector<std::vector<std::string>> mf_param_types;
  std::vector<std::vector<std::string>> mf_param_names;
  std::vector<bool> mf_variadic;
  std::vector<size_t> mf_min_params;
  int ref_id = -1;          // closure identity (for back-references)
  bool is_backref = false;  // true ⇒ this node is just a ref to ref_id
};

// ---------------------------------------------------------------------------
// Builtin / stdlib name set. Captured free variables that resolve to one of
// these are NOT shipped — the receiver builds its own stdlib via
// environment({}), so `print` / `Math` / `Isolate` etc. resolve locally. Only
// USER bindings (e.g. a top-level `fn fib`) are transferred. Computed once.
// ---------------------------------------------------------------------------
// The base environment an isolate runs in: the standard stdlib plus the
// inspect/print/println globals the CLI aliases (so a closure that prints
// transfers cleanly — those names resolve to the child's own copies and
// aren't shipped). builtin_names() is derived from this same env, keeping
// the "skip" set and the child's environment exactly in sync.
inline std::shared_ptr<Environment> isolate_base_env() {
  auto env = culebra::environment({});
  if (env->has("IO")) {
    const auto& io = env->get("IO").to_object();
    env->initialize("inspect", io.get("inspect"), false);
    env->initialize("print", io.get("print"), false);
    env->initialize("println", io.get("println"), false);
  }
  return env;
}

inline const std::set<std::string, std::less<>>& builtin_names() {
  static const std::set<std::string, std::less<>> names = [] {
    std::set<std::string, std::less<>> s;
    auto e = isolate_base_env();
    for (const auto& [k, sym] : e->dictionary) s.insert(k);
    return s;
  }();
  return names;
}

// ---------------------------------------------------------------------------
// Free-variable analysis for a closure body. Models culebra's lexical scoping
// (mirrors lint.h's ScopeWalker and jit.h's visit_for_frees): an IDENTIFIER
// referenced but not bound within the closure is "free" and (if it resolves in
// the closure's def_env and is not a builtin) becomes a capture. Sound for
// legal programs, which always declare before use. Property names (`x.foo`),
// object-literal keys, and kwarg names are NOT references and are skipped.
// ---------------------------------------------------------------------------
namespace _detail {
using namespace peg::udl;

inline void collect_binding_idents(const peg::Ast& node,
                                   std::set<std::string, std::less<>>& out) {
  if (node.tag == "IDENTIFIER"_ && node.is_token) {
    if (node.token != "_") out.insert(std::string(node.token));
    return;
  }
  for (const auto& c : node.nodes) collect_binding_idents(*c, out);
}

class FreeVarWalker {
 public:
  std::set<std::string, std::less<>> frees;

  void run(const peg::Ast& params, const peg::Ast& body) {
    scopes_.emplace_back();
    collect_binding_idents(params, scopes_.back());
    walk(body);
    scopes_.pop_back();
  }

 private:
  std::vector<std::set<std::string, std::less<>>> scopes_;

  bool bound(std::string_view n) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it)
      if (it->contains(n)) return true;
    return false;
  }
  void bind(std::string_view n) {
    if (n != "_") scopes_.back().insert(std::string(n));
  }
  void walk_children(const peg::Ast& node) {
    for (const auto& c : node.nodes) walk(*c);
  }
  template <class Bind>
  void scoped(const peg::Ast& body, Bind&& bind_fn) {
    scopes_.emplace_back();
    bind_fn(scopes_.back());
    walk(body);
    scopes_.pop_back();
  }

  void walk(const peg::Ast& node) {
    // `x.foo` collapses to an IDENTIFIER tag but original_tag is DOT — it's a
    // property name, never a variable reference.
    if (node.original_tag == "DOT"_ || node.original_tag == "SAFE_DOT"_) return;

    switch (node.tag) {
      case "IDENTIFIER"_:
        if (node.is_token && !bound(node.token))
          frees.insert(std::string(node.token));
        return;
      case "FUNCTION"_:
      case "LAMBDA"_: {
        auto fv = node.tag == "FUNCTION"_ ? culebra::view_function(node)
                                          : culebra::view_lambda(node);
        scoped(*fv.body,
               [&](auto& s) { collect_binding_idents(*fv.params, s); });
        return;
      }
      case "LEXICAL_SCOPE"_:
      case "DEFER"_:
        scopes_.emplace_back();
        walk_children(node);
        scopes_.pop_back();
        return;
      case "FOR"_: {
        if (node.nodes.size() < 3) { walk_children(node); return; }
        auto fv = culebra::view_for(node);
        walk(*fv.iter);  // iterable: enclosing scope
        scoped(*fv.body,
               [&](auto& s) { collect_binding_idents(*fv.binding, s); });
        // A `nobreak { … }` runs after the loop in the enclosing scope; the
        // loop variable is not visible there, so walk it without the binding.
        if (fv.nobreak) walk(*fv.nobreak);
        return;
      }
      case "TRY"_:
        if (node.nodes.size() < 3) { walk_children(node); return; }
        scoped(*node.nodes[0], [](auto&) {});
        scoped(*node.nodes[2],
               [&](auto& s) { collect_binding_idents(*node.nodes[1], s); });
        return;
      case "MATCH"_: {
        if (node.nodes.size() < 2) { walk_children(node); return; }
        // An init clause (`match mut x = f(); x { … }`) scopes its bindings to
        // the subject and every arm: push an enclosing scope and register them.
        auto mv = culebra::view_match(node);
        bool scope_pushed = mv.init != nullptr;
        if (scope_pushed) {
          scopes_.emplace_back();
          for (const auto& binding : mv.init->nodes) walk(*binding);
        }
        walk(*mv.subject);
        for (const auto& arm : mv.arms->nodes) {
          if (arm->nodes.empty()) continue;
          scopes_.emplace_back();
          collect_binding_idents(*arm->nodes[0], scopes_.back());
          for (size_t k = 1; k < arm->nodes.size(); k++) walk(*arm->nodes[k]);
          scopes_.pop_back();
        }
        if (scope_pushed) scopes_.pop_back();
        return;
      }
      case "OBJECT_PROPERTY"_: {
        // Walk only the value; the key is a property name, not a reference.
        auto pv = culebra::view_object_property(node);
        if (pv.value) walk(*pv.value);
        return;
      }
      case "ASSIGNMENT"_: {
        auto av = culebra::view_assignment(node);
        walk(*av.rhs);
        if (av.lvalcnt == 1) {
          const auto& lval = *node.nodes[av.lvaloff];
          if (lval.tag == "IDENTIFIER"_ && lval.is_token) {
            if (av.is_let || av.is_mut) {
              bind(lval.token);  // declaration: new local
            } else {
              // bare / compound assignment to an existing name: a reference.
              if (!bound(lval.token))
                frees.insert(std::string(lval.token));
            }
          }
        } else {
          for (int k = 0; k < av.lvalcnt; k++) walk(*node.nodes[av.lvaloff + k]);
        }
        return;
      }
      case "DESTRUCTURE_ASSIGN"_:
        walk(*node.nodes.back());
        if (node.nodes.size() >= 3)
          collect_binding_idents(*node.nodes[2], scopes_.back());
        return;
      case "PLACE_ASSIGN"_:
        // A chain target reads its receiver (so it can capture a free
        // variable); a plain-name target binds, like a destructured name.
        walk(*node.nodes.back());
        culebra::for_each_place_target(
            node, [&](const peg::Ast& chain) { walk(chain); },
            [&](const peg::Ast& name) { bind(name.token); });
        return;
      default:
        walk_children(node);
        return;
    }
  }
};

inline std::set<std::string, std::less<>> free_vars(const peg::Ast& params,
                                                    const peg::Ast& body) {
  FreeVarWalker w;
  w.run(params, body);
  return std::move(w.frees);
}

// Locate a name's Symbol (value + mutability) across the def_env chain, so a
// closure capture can be checked for mutability without going through get()
// (which would resolve lazy stdlib modules). Returns nullptr if unbound.
inline const Symbol* find_symbol(const Environment* e, std::string_view name) {
  for (; e; e = e->outer.get()) {
    auto it = e->dictionary.find(name);
    if (it != e->dictionary.end()) return &it->second;
  }
  return nullptr;
}

}  // namespace _detail

// ---------------------------------------------------------------------------
// Serialize context: closure identity memo + data-cycle guard.
// ---------------------------------------------------------------------------
struct SerCtx {
  std::map<std::pair<const void*, const void*>, int> closure_ids;
  int next_id = 0;
  std::set<const void*> visiting;  // backing-store ptrs on the current path
};

[[noreturn]] inline void send_error(const std::string& what) {
  throw culebra::CulebraError("SendError", what);
}

// True while Shared.new is freezing a value (serialize-as-freeze). The
// extract hooks consult it to SKIP the in-flight refcount bump: a freeze
// is synchronous (no cross-thread in-flight window to protect) and Shared
// rejects every handle anyway, so the bump would only ever need undoing
// — and undoing it on the serialize-throws path (a cycle past an already
// extracted handle) is exactly the leak this avoids. Set by a RAII guard
// in make_shared_namespace / _ns_shared_new.
inline bool& sharedval_freezing() {
  static thread_local bool f = false;
  return f;
}
struct FreezeGuard {
  bool prev;
  FreezeGuard() : prev(sharedval_freezing()) { sharedval_freezing() = true; }
  ~FreezeGuard() { sharedval_freezing() = prev; }
};

// Channel endpoints are the one Sendable native handle (a shared, GC-external
// channel, referenced — not copied). Since isolates are threads in one process,
// an endpoint is identified by a process-wide registry id + role (0 = tx,
// 1 = rx). isolate.h registers these hooks so the channel-agnostic walk can
// ship an endpoint by id. `extract` reads id+role and bumps the endpoint
// refcount (so the channel can't auto-close in the in-flight window before the
// receiver rebuilds it); `rebuild` materializes a fresh endpoint over the same
// id (no extra bump — extract already accounted for it).
struct ChannelRef {
  long id = 0;
  int role = 0;
};
inline std::function<ChannelRef(const Value&)>& channel_extract_hook() {
  static std::function<ChannelRef(const Value&)> f;
  return f;
}
inline std::function<Value(const ChannelRef&)>& channel_rebuild_hook() {
  static std::function<Value(const ChannelRef&)> f;
  return f;
}

// SharedBuffer is the other shared-reference Sendable exception (a @packable
// buffer crosses by its registry id, giving the child a zero-copy view of the
// same bytes). `extract` reads the id and bumps the buffer refcount (in-flight
// protection so a temporary buffer can't be reclaimed before the child rebuilds
// it); `rebuild` materializes a fresh handle over the same id (bumping its own
// ref, released by that handle's drop). Same shape as the channel hooks.
inline std::function<long(const Value&)>& sharedbuffer_extract_hook() {
  static std::function<long(const Value&)> f;
  return f;
}
inline std::function<Value(long)>& sharedbuffer_rebuild_hook() {
  static std::function<Value(long)> f;
  return f;
}

// Shared.new views are the third shared-reference exception: the frozen
// tree crosses by registry id + node index (a sub-view is just a deeper
// node). Same extract-bumps / rebuild-transfers contract as above.
struct SharedValRef {
  long id = 0;
  long node = 0;
};
inline std::function<SharedValRef(const Value&)>& sharedval_extract_hook() {
  static std::function<SharedValRef(const Value&)> f;
  return f;
}
inline std::function<Value(const SharedValRef&)>& sharedval_rebuild_hook() {
  static std::function<Value(const SharedValRef&)> f;
  return f;
}

inline SendNode serialize(const Value& v, SerCtx& ctx) {
  SendNode n;
  // Array / Set / Tuple share the same shape: cycle-guard the backing store,
  // then walk its elements. (Their backing stores are all
  // shared_ptr<vector<Value>>.)
  auto seq = [&](const std::shared_ptr<std::vector<Value>>& store,
                 SendNode::K kind) -> SendNode {
    const void* bp = store.get();
    if (!ctx.visiting.insert(bp).second)
      send_error("a cyclic value cannot be sent");
    n.kind = kind;
    n.elems.reserve(store->size());
    for (const auto& e : *store) n.elems.push_back(serialize(e, ctx));
    ctx.visiting.erase(bp);
    return n;
  };
  switch (v.type) {
    case Value::Nil:    n.kind = SendNode::K::Nil; return n;
    case Value::Bool:   n.kind = SendNode::K::Bool; n.b = v.to_bool(); return n;
    case Value::Long:   n.kind = SendNode::K::Long; n.i = v.to_long(); return n;
    case Value::Float:  n.kind = SendNode::K::Float; n.d = v.get<double>(); return n;
    case Value::String:
    case Value::StringView:
      n.kind = SendNode::K::Str;
      n.s = std::string(v.to_string_view());
      return n;
    case Value::Tensor:
      send_error("Tensor is not Sendable (share via SharedBuffer instead)");
    case Value::Array:  return seq(v.to_array().values, SendNode::K::Array);
    case Value::Set:    return seq(v.get<SetValue>().members, SendNode::K::Set);
    case Value::Tuple:  return seq(v.get<TupleValue>().elements, SendNode::K::Tuple);
    case Value::Object: {
      const auto& obj = v.to_object();
      // Channel endpoint: the Sendable exception — ship the shared core by
      // reference (must be checked before the __nonsendable__ guard, since an
      // endpoint also carries native methods).
      if (obj.has("__channel_endpoint__")) {
        ChannelRef ref = channel_extract_hook()(v);
        n.kind = SendNode::K::Channel;
        n.i = ref.id;
        n.b = (ref.role == 1);
        return n;
      }
      // SharedBuffer handle: ship the buffer by id (shared, not copied).
      // Like the channel endpoint, checked before the __nonsendable__ guard
      // since the handle also carries a native `drop` method.
      if (obj.has("__sharedbuffer_id__")) {
        n.kind = SendNode::K::SharedBuffer;
        n.i = sharedbuffer_extract_hook()(v);
        return n;
      }
      // Shared.new view: ship the frozen tree by (id, node) reference.
      if (obj.is_shared_val) {
        auto ref = sharedval_extract_hook()(v);
        n.kind = SendNode::K::SharedVal;
        n.i = ref.id;
        n.ref_id = static_cast<int>(ref.node);
        return n;
      }
      if (obj.has("__nonsendable__"))
        send_error("a native handle is not Sendable");
      const void* bp = obj.properties.get();
      if (!ctx.visiting.insert(bp).second)
        send_error("a cyclic value cannot be sent");
      n.kind = SendNode::K::Object;
      size_t nprops = obj.properties->size() +
                      (obj.non_string_props ? obj.non_string_props->size() : 0);
      n.entries.reserve(nprops);
      n.entry_mut.reserve(nprops);
      for (const auto& [k, sym] : *obj.properties) {
        SendNode key;
        key.kind = SendNode::K::Str;
        key.s = std::string(k);
        n.entries.emplace_back(std::move(key), serialize(sym.val, ctx));
        n.entry_mut.push_back(sym.mut);
      }
      if (obj.non_string_props) {
        for (const auto& [k, sym] : *obj.non_string_props) {
          n.entries.emplace_back(serialize(k, ctx), serialize(sym.val, ctx));
          n.entry_mut.push_back(sym.mut);
        }
      }
      ctx.visiting.erase(bp);
      return n;
    }
    case Value::Function: {
      const FunctionValue& f = v.get<FunctionValue>();
      // A `fn name(...)` declaration is a multifn dispatcher with a synthetic
      // body. Ship every overload's body + dispatch signature so the child
      // rebuilds the full multimethod (mirrors the JIT, sendable_jit.h).
      if (f.multimethod_for_each_overload) {
        n.kind = SendNode::K::Closure;
        // Identity = the shared method table (stable across value copies, and
        // distinct per dispatcher so recursive back-refs resolve correctly).
        auto key = std::make_pair(
            static_cast<const void*>(f.multimethod_table.get()),
            static_cast<const void*>(nullptr));
        if (auto it = ctx.closure_ids.find(key); it != ctx.closure_ids.end()) {
          n.is_backref = true;
          n.ref_id = it->second;
          return n;
        }
        n.ref_id = ctx.next_id++;
        ctx.closure_ids.emplace(key, n.ref_id);  // before recursing (recursion)
        n.mf_name = f.name.empty() ? std::string("__multifn__") : f.name;
        f.multimethod_for_each_overload(
            [&](const Value& body, const std::vector<std::string>& ptypes,
                const std::vector<std::string>& pnames, bool variadic,
                size_t min_params) {
              n.mf_param_types.push_back(ptypes);
              n.mf_param_names.push_back(pnames);
              n.mf_variadic.push_back(variadic);
              n.mf_min_params.push_back(min_params);
              n.elems.push_back(serialize(body, ctx));
            });
        return n;
      }
      const FunctionValue* impl = &f;
      if (!impl->body || !impl->params_ast)
        send_error("a native/builtin function is not Sendable");

      n.kind = SendNode::K::Closure;
      // Identity = the method body + its def_env (stable across value copies,
      // and distinct per dispatcher so recursion back-refs resolve correctly).
      auto key = std::make_pair(static_cast<const void*>(impl->body.get()),
                                static_cast<const void*>(impl->def_env.get()));
      if (auto it = ctx.closure_ids.find(key); it != ctx.closure_ids.end()) {
        n.is_backref = true;
        n.ref_id = it->second;
        return n;
      }
      n.ref_id = ctx.next_id++;
      ctx.closure_ids.emplace(key, n.ref_id);  // before recursing (fib cycle)
      n.params_ast = impl->params_ast;
      n.body = impl->body;
      n.return_type = std::string(impl->return_type);
      auto frees = _detail::free_vars(*impl->params_ast, *impl->body);
      for (const auto& name : frees) {
        if (builtin_names().contains(name)) continue;  // child's own
        const Symbol* sym = _detail::find_symbol(impl->def_env.get(), name);
        if (!sym) continue;  // declared inside the body / rebound on the child
        // A `mut` capture is not Sendable: the value would silently diverge
        // from the parent's. Reject at the boundary (pass it as an argument, or
        // share read-only data via Shared.new — a later cycle).
        if (sym->mut)
          send_error("closure captures the mutable variable '" +
                     std::string(name) +
                     "' (mutable captures are not Sendable — pass it as an "
                     "argument instead)");
        n.captures.emplace_back(name, serialize(sym->val, ctx));
      }
      return n;
    }
  }
  send_error("value is not Sendable");
}

// ---------------------------------------------------------------------------
// Deserialize context: back-reference table for recursive closures.
// ---------------------------------------------------------------------------
struct DeCtx {
  std::map<int, Value> closures;
};

inline Value deserialize(const SendNode& n, Interpreter& interp,
                         const std::shared_ptr<Environment>& base, DeCtx& ctx);

inline bool contains_closure(const SendNode& n) {
  if (n.kind == SendNode::K::Closure) return true;
  for (const auto& e : n.elems)
    if (contains_closure(e)) return true;
  for (const auto& e : n.entries)
    if (contains_closure(e.first) || contains_closure(e.second)) return true;
  for (const auto& c : n.captures)
    if (contains_closure(c.second)) return true;
  return false;
}

inline Value deserialize(const SendNode& n, Interpreter& interp,
                         const std::shared_ptr<Environment>& base, DeCtx& ctx) {
  using K = SendNode::K;
  switch (n.kind) {
    case K::Nil:   return Value();
    case K::Bool:  return Value(n.b);
    case K::Long:  return Value(n.i);
    case K::Float: return Value(n.d);
    case K::Str:   return Value(std::string(n.s));
    case K::Channel:
      return channel_rebuild_hook()(ChannelRef{n.i, n.b ? 1 : 0});
    case K::SharedBuffer:
      return sharedbuffer_rebuild_hook()(n.i);
    case K::SharedVal:
      return sharedval_rebuild_hook()(SharedValRef{n.i, n.ref_id});
    case K::Array: {
      ArrayValue av;
      av.values->reserve(n.elems.size());
      for (const auto& e : n.elems)
        av.values->push_back(deserialize(e, interp, base, ctx));
      return Value(std::move(av));
    }
    case K::Set: {
      SetValue sv;
      for (const auto& e : n.elems) sv.add(deserialize(e, interp, base, ctx));
      return Value(std::move(sv));
    }
    case K::Tuple: {
      std::vector<Value> els;
      els.reserve(n.elems.size());
      for (const auto& e : n.elems)
        els.push_back(deserialize(e, interp, base, ctx));
      return Value(TupleValue(std::move(els)));
    }
    case K::Object: {
      ObjectValue obj;
      for (size_t k = 0; k < n.entries.size(); k++) {
        const SendNode& key = n.entries[k].first;
        Value val = deserialize(n.entries[k].second, interp, base, ctx);
        bool mut = k < n.entry_mut.size() ? n.entry_mut[k] : true;
        if (key.kind == K::Str) {
          obj.initialize(key.s, val, mut);
        } else {
          obj.initialize(deserialize(key, interp, base, ctx), val, mut);
        }
      }
      return Value(std::move(obj));
    }
    case K::Closure: {
      if (n.is_backref) {
        auto it = ctx.closures.find(n.ref_id);
        if (it == ctx.closures.end())
          throw culebra::CulebraError("SendError",
                                      "dangling closure back-reference");
        return it->second;
      }
      if (!n.mf_name.empty()) {
        // `fn name` dispatcher: build the shell and register it *before*
        // deserializing the method bodies, so an overload that recurses back
        // into the dispatcher resolves to it; then append each overload.
        std::shared_ptr<void> table;
        Value disp = interp.make_multifn_shell(n.mf_name, table);
        ctx.closures.emplace(n.ref_id, disp);
        for (size_t i = 0; i < n.elems.size(); i++) {
          Value body = deserialize(n.elems[i], interp, base, ctx);
          interp.multifn_add_method(table, std::move(body), n.mf_param_types[i],
                                    n.mf_param_names[i], n.mf_variadic[i],
                                    n.mf_min_params[i], disp);
        }
        return disp;
      }
      // Build the closure first (so a recursive self-capture resolves to it),
      // then populate the capture environment.
      auto fresh = std::make_shared<Environment>();
      if (base) fresh->append_outer(base);
      Value fn = interp.rebuild_closure(*n.params_ast, n.body,
                                        std::string_view(n.return_type), fresh);
      ctx.closures.emplace(n.ref_id, fn);
      for (const auto& [name, cnode] : n.captures) {
        Value cv = deserialize(cnode, interp, base, ctx);
        fresh->initialize(name, cv, /*mut=*/false);
      }
      return fn;
    }
  }
  return Value();
}

}  // namespace culebra::sendable
