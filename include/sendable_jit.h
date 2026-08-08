#pragma once

// JIT side of the isolate value transfer (C2-c).
//
// The interpreter and the JIT share the same isolate architecture, matching
// every isolate-based runtime (V8 Workers, Ruby Ractor, Java, Go, Erlang):
// **immutable code is shared, the mutable heap is isolated, only data is
// copied.** The interp's "code" is the AST (shared, process-lifetime); the
// JIT's is the compiled `JitClosure::fn_ptr` (shared while the LLJIT is alive,
// which spans the whole JIT::run and therefore every isolate joined within it).
//
// So a JIT closure crosses a thread boundary as (fn_ptr + positionally-copied
// captures) — no AST, no names: the compiled body reads captures[i] by index,
// so the child rebuilds the cells in the same order. The child runs on its own
// JIT heap (fresh Runtime) and invokes the shared fn_ptr. Symmetric with the
// interp: both ship a shared code reference + copied data and run on their own
// heap. Values cross via the same neutral sendable::SendNode.

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "isolate.h"  // IsolateCore, sendable::SendNode, isolate_cap, g_live_isolates
#include "jit.h"      // JitValue and the runtime constructors

namespace culebra {

// --- JitValue <-> SendNode ---------------------------------------------------

struct JitSerCtx {
  std::map<const void*, int> closure_ids;  // JitClosure* -> ref id (recursion)
  std::map<const void*, int> proto_ids;    // class meta JitObject* -> ref id
  int next_id = 0;
  std::set<const void*> visiting;          // container cycle guard
};
struct JitDeCtx {
  std::map<int, JitValue> closures;        // ref id -> rebuilt closure
  std::map<int, JitObject*> protos;        // ref id -> rebuilt class meta
};

inline sendable::SendNode jit_serialize(JitValue v, JitSerCtx& ctx) {
  using K = sendable::SendNode::K;
  sendable::SendNode n;
  auto seq = [&](JitArray* a, K kind) -> sendable::SendNode {
    if (!ctx.visiting.insert(a).second)
      sendable::send_error("a cyclic value cannot be sent");
    culebra::ValueWalkFrame walk;
    n.kind = kind;
    n.elems.reserve(a->size);
    for (size_t i = 0; i < a->size; i++)
      n.elems.push_back(jit_serialize(a->items[i], ctx));
    ctx.visiting.erase(a);
    return n;
  };
  switch (v.tag) {
    case TAG_NIL:   n.kind = K::Nil; return n;
    case TAG_BOOL:  n.kind = K::Bool; n.b = v.data != 0; return n;
    case TAG_LONG:  n.kind = K::Long; n.i = v.data; return n;
    case TAG_FLOAT: n.kind = K::Float; n.d = _culebra_float_to_double(v.data); return n;
    case TAG_STRING:
    case TAG_STRINGVIEW:
      n.kind = K::Str;
      n.s = std::string(_culebra_str_view(v.tag, v.data));
      return n;
    case TAG_ARRAY:  return seq(reinterpret_cast<JitArray*>(v.data), K::Array);
    case TAG_TUPLE:  return seq(reinterpret_cast<JitArray*>(v.data), K::Tuple);
    case TAG_SET: {
      auto* s = reinterpret_cast<JitSet*>(v.data);
      if (!ctx.visiting.insert(s).second)
        sendable::send_error("a cyclic value cannot be sent");
      culebra::ValueWalkFrame walk;
      n.kind = K::Set;
      n.elems.reserve(s->members.size());
      for (const auto& m : s->members) n.elems.push_back(jit_serialize(m, ctx));
      ctx.visiting.erase(s);
      return n;
    }
    case TAG_OBJECT: {
      auto* o = reinterpret_cast<JitObject*>(v.data);
      // Channel endpoint — the Sendable exception: ship its id + role and bump
      // the in-flight ref (released after the node is consumed).
      if (o->find_slot("__channel_endpoint__") != static_cast<size_t>(-1)) {
        int64_t id = o->slots[o->find_slot("__channel_id__")].value.data;
        int role = static_cast<int>(
            o->slots[o->find_slot("__channel_role__")].value.data);
        if (!sendable::sharedval_freezing()) chan_bump(id, role, +1);
        n.kind = K::Channel;
        n.i = id;
        n.b = (role == 1);
        return n;
      }
      // SharedBuffer handle — the other shared-reference exception: ship its
      // id and bump the in-flight ref (released after the node is consumed).
      if (o->is_shared_buffer) {
        int64_t id = o->slots[o->find_slot("__sharedbuffer_id__")].value.data;
        if (!sendable::sharedval_freezing()) culebra::shared_buffer_bump(id, +1);
        n.kind = K::SharedBuffer;
        n.i = id;
        return n;
      }
      // Shared.new view: ship the frozen tree by (id, node) reference.
      // Bump for the in-flight window (released by the consumer's
      // release_inflight_channels; the receiver handle takes its own ref
      // at rebuild). Mirrors the interp's sharedval extract hook.
      if (o->is_shared_val) {
        size_t di = o->find_slot("_dropped");
        if (di != static_cast<size_t>(-1) && o->slots[di].value.data) {
          throw culebra::CulebraError("ClosedError",
                                      "Shared value has been dropped");
        }
        int64_t svid = o->slots[o->find_slot("__sharedval_id__")].value.data;
        if (!sendable::sharedval_freezing()) culebra::shared_val_bump(svid);
        n.kind = K::SharedVal;
        n.i = svid;
        n.ref_id = static_cast<int>(
            o->slots[o->find_slot("__sharedval_node__")].value.data);
        return n;
      }
      // Embed.dir handle: nothing to share — ship the directory name and let
      // the other side rebuild the handle over it (checked before the native
      // methods it carries would be refused below). Mirrors the interp.
      if (size_t ei = o->find_slot("__embed_dir__"), ni = o->find_slot("name");
          ei != static_cast<size_t>(-1) && ni != static_cast<size_t>(-1) &&
          o->slots[ni].value.tag == TAG_STRING) {
        JitValue nv = o->slots[ni].value;
        n.kind = K::EmbedDir;
        n.s = std::string(_culebra_str_view(nv.tag, nv.data));
        return n;
      }
      if (o->find_slot("__nonsendable__") != static_cast<size_t>(-1))
        sendable::send_error("a native handle is not Sendable");
      if (!ctx.visiting.insert(o).second)
        sendable::send_error("a cyclic value cannot be sent");
      culebra::ValueWalkFrame walk;
      n.kind = K::Object;
      if (o->shape) {
        for (size_t i = 0; i < o->shape->names.size(); i++) {
          sendable::SendNode key;
          key.kind = K::Str;
          key.s = o->shape->names[i];
          n.entries.emplace_back(std::move(key),
                                 jit_serialize(o->slots[i].value, ctx));
          n.entry_mut.push_back(o->slots[i].mut);
        }
      }
      // Class instance: ship the proto (the per-class meta holding the
      // method closures) as elems[0], so a method call works on the other
      // side — the interp gets the same effect from methods being own
      // props. Many instances share one meta, so it is memoized (same id
      // space as closures; the pointers never collide) and a repeat ships
      // as a backref. A meta whose methods reach a native closure (derived
      // eq/show thunks, a method capturing the class object and thus its
      // ctor) rejects exactly like the interp's body==nullptr check.
      if (o->proto) {
        // Carry the instance's own drop gate rather than re-deriving it
        // from the meta on the other side: a nested instance can relink
        // while its meta is still mid-fill (the memo registers before the
        // entries land), so probing the meta there would depend on the
        // class's member declaration order.
        n.b = o->has_drop;
        if (auto it = ctx.proto_ids.find(o->proto);
            it != ctx.proto_ids.end()) {
          sendable::SendNode ref;
          ref.kind = K::Object;
          ref.is_backref = true;
          ref.ref_id = it->second;
          n.elems.push_back(std::move(ref));
        } else {
          int proto_ref = ctx.next_id++;
          ctx.proto_ids.emplace(o->proto, proto_ref);
          n.elems.push_back(jit_serialize(
              {TAG_OBJECT, reinterpret_cast<int64_t>(o->proto)}, ctx));
          n.elems.back().ref_id = proto_ref;
        }
      }
      // Non-string keys: ship them after the string keys, matching the
      // interp serializer's order (string props, then non_string_props),
      // so the two backends produce identical SendNode entry lists (the
      // freeze indexer and the cross-isolate Object transfer both rely on
      // this). The key itself serializes as a value node.
      if (o->non_string_props) {
        for (const auto& [k, ent] : *o->non_string_props) {
          n.entries.emplace_back(jit_serialize(k, ctx),
                                 jit_serialize(ent.value, ctx));
          n.entry_mut.push_back(ent.mut);
        }
      }
      ctx.visiting.erase(o);
      return n;
    }
    case TAG_FUNC: {
      auto* c = reinterpret_cast<JitClosure*>(v.data);
      n.kind = K::Closure;
      if (auto it = ctx.closure_ids.find(c); it != ctx.closure_ids.end()) {
        n.is_backref = true;
        n.ref_id = it->second;
        return n;
      }
      // Closure chains nest through captures, not containers, so the
      // Function node counts a level too (mirrors the interp serializer).
      culebra::ValueWalkFrame walk;
      n.ref_id = ctx.next_id++;
      ctx.closure_ids.emplace(c, n.ref_id);  // before recursing (fib recursion)
      n.jit_fn = c->fn_ptr;
      n.i = static_cast<int64_t>(c->arity);
      // `fn name` is a dispatcher thunk: its overloads live in a thread_local
      // table, so ship each method body + its dispatch types explicitly.
      if (auto dit = _jit_multifn_dispatcher_names().find(c);
          dit != _jit_multifn_dispatcher_names().end()) {
        n.mf_name = dit->second;
        const auto& methods = _jit_multimethods()[n.mf_name];
        for (const auto& mth : methods) {
          n.mf_param_types.push_back(mth.param_types);
          n.mf_param_names.push_back(mth.param_names);
          n.mf_variadic.push_back(mth.variadic);
          n.mf_min_params.push_back(mth.min_params);
          n.elems.push_back(jit_serialize(
              JitValue{TAG_FUNC, reinterpret_cast<int64_t>(mth.body)}, ctx));
        }
        return n;
      }
      // A native (C++-bodied) closure is not Sendable: it can't be rebuilt
      // on another Runtime and its captures may hold raw same-heap pointers
      // (iterator wrappers, ns-method NsMethod*). Matches the interp's
      // body==nullptr rejection; without this the child dereferences
      // parent-heap state and hangs or crashes.
      if (_jit_is_native_fn(c->fn_ptr))
        sendable::send_error("a native/builtin function is not Sendable");
      // A `mut` capture is not Sendable: the value would silently diverge from
      // the parent's. Reject at the boundary, matching the interp (sendable.h).
      if (auto* nm = _jit_first_mut_capture(c->fn_ptr))
        sendable::send_error(
            "closure captures the mutable variable '" + *nm +
            "' (mutable captures are not Sendable — pass it as an "
            "argument instead)");
      n.elems.reserve(c->n_captures);  // positional captures
      for (size_t i = 0; i < c->n_captures; i++)
        n.elems.push_back(jit_serialize(c->captures[i]->value, ctx));
      return n;
    }
    case TAG_TENSOR:
      // Wording matches the interp serializer exactly (sendable.h).
      sendable::send_error("Tensor is not Sendable (share via SharedBuffer instead)");
  }
  sendable::send_error("value is not Sendable");
}

inline JitValue jit_float(double d) {
  int64_t bits;
  std::memcpy(&bits, &d, sizeof(double));
  return {TAG_FLOAT, bits};
}

inline JitValue _jit_make_channel_endpoint(int64_t id, int role);  // fwd
inline JitValue _jit_make_shared_buffer_handle(int64_t id, int64_t count);  // fwd
inline JitValue _jit_make_shared_val_view(int64_t id, int64_t node);       // fwd
inline JitValue _jit_make_embed_dir_handle(const std::string& name);      // fwd

inline JitValue jit_deserialize(const sendable::SendNode& n, JitDeCtx& ctx) {
  using K = sendable::SendNode::K;
  switch (n.kind) {
    case K::Nil:   return {TAG_NIL, 0};
    case K::Bool:  return {TAG_BOOL, n.b ? 1 : 0};
    case K::Long:  return {TAG_LONG, n.i};
    case K::Float: return jit_float(n.d);
    case K::Str:
      return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(n.s.c_str()))};
    case K::Array: {
      auto* a = culebra_runtime_array_new();
      for (const auto& e : n.elems) {
        JitValue ev = jit_deserialize(e, ctx);
        culebra_runtime_array_push(a, ev.tag, ev.data);
      }
      return {TAG_ARRAY, reinterpret_cast<int64_t>(a)};
    }
    case K::Tuple: {
      auto* a = culebra_runtime_tuple_new();
      for (const auto& e : n.elems) {
        JitValue ev = jit_deserialize(e, ctx);
        culebra_runtime_array_push(a, ev.tag, ev.data);
      }
      return {TAG_TUPLE, reinterpret_cast<int64_t>(a)};
    }
    case K::Set: {
      auto* s = culebra_runtime_set_new();
      for (const auto& e : n.elems) {
        JitValue ev = jit_deserialize(e, ctx);
        culebra_runtime_set_add(s, ev.tag, ev.data);
      }
      return {TAG_SET, reinterpret_cast<int64_t>(s)};
    }
    case K::Object: {
      // A backref to a class meta already rebuilt in this message. The
      // caller (the instance's proto link below) takes its own retain.
      if (n.is_backref)
        return {TAG_OBJECT,
                reinterpret_cast<int64_t>(ctx.protos.at(n.ref_id))};
      auto* o = culebra_runtime_object_new();
      // Register a shared class meta BEFORE filling entries so a backref
      // inside its own subtree (a method capturing another instance of
      // the same class) resolves — mirrors the closure memo above.
      if (n.ref_id >= 0) ctx.protos.emplace(n.ref_id, o);
      for (size_t k = 0; k < n.entries.size(); k++) {
        JitValue val = jit_deserialize(n.entries[k].second, ctx);
        bool mut = k < n.entry_mut.size() ? n.entry_mut[k] : true;
        if (n.entries[k].first.kind == sendable::SendNode::K::Str) {
          // Through the chokepoint, not set_or_append: mirrors the interp's
          // `initialize` (well-known contract check + owned `drop`
          // registration for a plain object's own slot).
          culebra_runtime_object_set(o, n.entries[k].first.s.c_str(), mut,
                                     val.tag, val.data, 0, 0,
                                     /*is_init=*/true);
        } else {
          // Non-string key: rebuild the key value and store via the
          // value-key path (consumes both +1s).
          JitValue key = jit_deserialize(n.entries[k].first, ctx);
          culebra_runtime_object_set_any(o, key.tag, key.data, mut, val.tag,
                                         val.data, 0, 0, /*is_init=*/true);
        }
      }
      // Class instance: relink the proto (elems[0] = the rebuilt class
      // meta), mirroring build_class_instance — the proto pointer holds
      // one retain (released by the JitObject destructor), and an
      // inherited `drop` is mirrored so the destructor gate and the
      // owned stack see it.
      if (!n.elems.empty()) {
        JitValue proto = jit_deserialize(n.elems[0], ctx);
        auto* meta = reinterpret_cast<JitObject*>(proto.data);
        o->proto = meta;
        meta->refcount++;
        // n.b carries the sender instance's has_drop gate — don't probe
        // the meta here, it may still be mid-fill for a nested instance.
        if (n.b) _jit_owned_bind_drop(o);
        // Drop the +1 jit_deserialize handed back when this was the
        // meta's first (full) rebuild; a backref arrives borrowed.
        if (!n.elems[0].is_backref)
          _culebra_value_release_impl(proto.tag, proto.data);
      }
      // A bare packed view (rare — usually the buffer crosses, not a view)
      // arrives as a generic Object; restore its O(1) discriminator, which is
      // a flag, not a slot. Buffers take the dedicated K::SharedBuffer path.
      if (o->find_slot("__packedview_id__") != static_cast<size_t>(-1))
        o->is_packed_view = true;
      return {TAG_OBJECT, reinterpret_cast<int64_t>(o)};
    }
    case K::Closure: {
      if (n.is_backref) return ctx.closures.at(n.ref_id);
      // Multifn dispatcher: rebuild the thunk closure and re-register its name
      // + overload methods in this thread's tables (recursion resolves because
      // the dispatcher is memoized before its method bodies are rebuilt).
      if (!n.mf_name.empty()) {
        auto* c = culebra_runtime_closure_new(n.jit_fn, 0,
                                              static_cast<size_t>(n.i));
        JitValue cv{TAG_FUNC, reinterpret_cast<int64_t>(c)};
        _jit_multifn_dispatcher_names()[c] = n.mf_name;
        ctx.closures.emplace(n.ref_id, cv);
        auto& methods = _jit_multimethods()[n.mf_name];
        for (size_t i = 0; i < n.elems.size(); i++) {
          JitValue body = jit_deserialize(n.elems[i], ctx);
          methods.push_back({n.mf_param_types[i],
                             n.mf_param_names[i],
                             reinterpret_cast<JitClosure*>(body.data),
                             n.mf_variadic[i],
                             n.mf_min_params[i]});
        }
        return cv;
      }
      auto* c = culebra_runtime_closure_new(n.jit_fn, n.elems.size(),
                                            static_cast<size_t>(n.i));
      JitValue cv{TAG_FUNC, reinterpret_cast<int64_t>(c)};
      ctx.closures.emplace(n.ref_id, cv);
      for (size_t i = 0; i < n.elems.size(); i++) {
        JitValue capv = jit_deserialize(n.elems[i], ctx);
        c->captures[i] = culebra_runtime_cell_new(capv.tag, capv.data);
      }
      return cv;
    }
    case K::Channel:
      chan_bump(n.i, n.b ? 1 : 0, +1);  // this rebuilt endpoint's own ref
      return _jit_make_channel_endpoint(n.i, n.b ? 1 : 0);
    case K::SharedBuffer: {
      culebra::shared_buffer_bump(n.i, +1);  // the rebuilt handle's own ref
      auto core = culebra::lookup_shared_buffer(n.i);
      return _jit_make_shared_buffer_handle(
          n.i, core ? static_cast<int64_t>(core->count) : 0);
    }
    case K::SharedVal: {
      // The receiver handle takes its own ref; the in-flight +1 from the
      // sender's extract is released by release_inflight_channels.
      culebra::shared_val_bump(n.i);
      return _jit_make_shared_val_view(n.i, n.ref_id);
    }
    case K::EmbedDir:
      return _jit_make_embed_dir_handle(n.s);
  }
  return {TAG_NIL, 0};
}

// --- Isolate handle registry (JIT handle methods are captureless → store the
//     IsolateCore behind an integer id, mirroring the channel registry) -------

inline std::mutex& jit_isolate_reg_mutex() { static std::mutex m; return m; }
inline std::unordered_map<long, std::shared_ptr<IsolateCore>>&
jit_isolate_reg() {
  static std::unordered_map<long, std::shared_ptr<IsolateCore>> r;
  return r;
}
inline std::atomic<long>& jit_isolate_next_id() {
  static std::atomic<long> n{1};
  return n;
}
inline std::shared_ptr<IsolateCore> jit_isolate_lookup(int64_t id) {
  std::lock_guard<std::mutex> lk(jit_isolate_reg_mutex());
  auto it = jit_isolate_reg().find(id);
  return it == jit_isolate_reg().end() ? nullptr : it->second;
}

// --- Child entry point: run the spawned closure on a fresh JIT heap ----------

inline void run_isolate_child_jit(std::shared_ptr<IsolateCore> core,
                                  sendable::SendNode sclosure,
                                  std::vector<sendable::SendNode> sargs,
                                  bool decrement_live) {
  // A fresh Runtime gives this work its own JIT heap (and fresh thread_local
  // multifn tables); the shared fn_ptr allocates on whichever Runtime is active.
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  rt.interrupt_flag = &core->interrupt;
  try {
    JitDeCtx dc;
    // The rebuilt closure is the child heap's only ref to whatever crossed the
    // boundary, so its captures drop when it does (the interp path drops them
    // via heap teardown). Held in JitOwnedVal because that has to happen on the
    // unwind path too: a body that throws past here with a captured `tx` would
    // otherwise leave the channel open forever and hang every receiver.
    JitOwnedVal fn(jit_deserialize(sclosure, dc));
    auto* cls = reinterpret_cast<JitClosure*>(fn.borrow().data);
    std::vector<JitOwnedVal> owned_args;
    owned_args.reserve(sargs.size());
    for (const auto& sa : sargs)
      owned_args.emplace_back(jit_deserialize(sa, dc));
    // Rebuilt endpoints hold their own refs now; drop the in-flight ones.
    release_inflight_channels(sclosure);
    for (const auto& sa : sargs) release_inflight_channels(sa);
    // Each arm consumes exactly what it passes: the invoke takes the args over,
    // and the callee frame releases the params on both its exit paths. Only the
    // variadic form needs them flattened into a buffer.
    JitValue raw;
    if (owned_args.empty()) raw = _culebra_invoke0(cls);
    else if (owned_args.size() == 1)
      raw = _culebra_invoke1(cls, owned_args[0].consume());
    else if (owned_args.size() == 2)
      raw = _culebra_invoke2(cls, owned_args[0].consume(),
                             owned_args[1].consume());
    else {
      std::vector<JitValue> args;
      args.reserve(owned_args.size());
      for (auto& a : owned_args) args.push_back(a.consume());
      raw = _jit_invoke(cls, {TAG_NO_SELF, 0},
                        static_cast<int64_t>(args.size()), args.data());
    }
    JitOwnedVal r(raw);
    JitSerCtx sc;
    sendable::SendNode out = jit_serialize(r.borrow(), sc);
    {
      std::lock_guard<std::mutex> lk(core->m);
      core->result = std::move(out);
      core->finished = true;
    }
  } catch (culebra::CulebraError& e) {
    std::lock_guard<std::mutex> lk(core->m);
    core->error = e;
    core->finished = true;
  } catch (CulebraException& e) {
    // A raw user `throw <value>` (not a structured error). Serialize the value
    // so it crosses the isolate boundary; _jit_isolate_extract re-raises it as a
    // CulebraException on the parent. Mirrors the interp child's `catch (Value&)`.
    JitValue tv;
    tv.tag = e.tag;
    tv.data = e.data;
    JitSerCtx sc;
    sendable::SendNode tn = jit_serialize(tv, sc);
    culebra_runtime_value_release(e.tag, e.data);  // balance the throw's retain
    std::lock_guard<std::mutex> lk(core->m);
    core->thrown = std::move(tn);
    core->finished = true;
  } catch (std::exception& e) {
    std::lock_guard<std::mutex> lk(core->m);
    core->error = culebra::CulebraError("RuntimeError", e.what());
    core->finished = true;
  }
  core->cv.notify_all();
  if (decrement_live)
    g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
}

// --- JIT isolate handle (join/poll/drop). JIT handle methods are captureless,
//     so the IsolateCore is reached through the registry by id. ---------------

inline JitValue _jit_isolate_extract(const std::shared_ptr<IsolateCore>& core) {
  if (!core->joined && core->thread.joinable()) {
    core->thread.join();
    core->joined = true;
  }
  if (core->error) throw *core->error;
  if (core->thrown) {
    // A raw user `throw <value>` from the child. Rebuild it on the parent heap
    // and re-raise in the JIT's throw form, so a compiled `catch` binds the
    // value (symmetric with the interp `throw chan_take(*core.thrown)`).
    JitDeCtx tdc;
    JitValue tv = jit_deserialize(*core->thrown, tdc);
    release_inflight_channels(*core->thrown);
    culebra_runtime_throw(tv.tag, tv.data);  // [[noreturn]]
  }
  JitDeCtx dc;
  return jit_deserialize(core->result, dc);  // on the parent heap
}

inline int64_t _jit_isolate_self_id(JitValue self) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t i = h->find_slot("_core_id");
  return i == static_cast<size_t>(-1) ? 0 : h->slots[i].value.data;
}

inline void _jit_isolate_join(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // Callee-consumes on any exit: _jit_isolate_extract re-raises the child's
  // failure, and the caller does not clean a native method's operands on
  // unwind, so a tail release would strand `self` there.
  JitMethodSelf _s{self};
  auto core = jit_isolate_lookup(_jit_isolate_self_id(self));
  JitValue ret{TAG_NIL, 0};
  if (core) {
    {
      std::unique_lock<std::mutex> lk(core->m);
      core->cv.wait(lk, [&] { return core->finished; });
    }
    ret = _jit_isolate_extract(core);
  }
  { *__ret = ret; return; }
}

inline void _jit_isolate_poll(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (extract re-raises)
  auto core = jit_isolate_lookup(_jit_isolate_self_id(self));
  JitValue ret{TAG_NIL, 0};
  if (core) {
    bool done;
    { std::lock_guard<std::mutex> lk(core->m); done = core->finished; }
    if (done) ret = _jit_isolate_extract(core);
  }
  { *__ret = ret; return; }
}

inline void _jit_isolate_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  int64_t id = _jit_isolate_self_id(self);
  auto core = jit_isolate_lookup(id);
  if (core && !core->joined && core->thread.joinable()) {
    mark_isolate_cancelled(*core);  // sets the JIT wake so a tight loop unwinds
    {
      std::unique_lock<std::mutex> lk(core->m);
      core->cv.wait(lk, [&] { return core->finished; });
    }
    core->thread.join();
    core->joined = true;
  }
  {
    std::lock_guard<std::mutex> lk(jit_isolate_reg_mutex());
    jit_isolate_reg().erase(id);
  }
  { *__ret = {TAG_NIL, 0}; return; }
}

// Cancel + join every JIT isolate still outstanding — a standalone
// Isolate.spawn handle (jit_isolate_reg) or a Channel.fan_in producer
// (merge_registry's MergeEntry.producers, isolate.h) — before JIT::exec
// (jit.h) tears down the LLJIT that backs every one's shared
// JitClosure::fn_ptr (see the header comment at the top of this file). A
// script normally reaps its own isolates via h.join()/h.drop(), which mark
// `joined` and make this a no-op walk; the gap is a top-level throw that
// unwinds straight past an unreached join(), leaving one still running when
// the LLJIT that owns its compiled body is about to be freed out from under
// it. Installed into isolate_teardown_join_hook (shared.h) below — jit.h
// can't call into isolate.h's registries directly (isolate.h reaches back to
// jit.h through stdlib_interp.h), so the hook meets both sides there.
inline void _jit_isolate_teardown_join_all() {
  std::vector<std::shared_ptr<IsolateCore>> live;
  {
    std::lock_guard<std::mutex> lk(jit_isolate_reg_mutex());
    for (auto& [_, core] : jit_isolate_reg())
      if (!core->joined) live.push_back(core);
  }
  collect_live_merge_producers(live);  // isolate.h; shared with the interp side
  cancel_and_join_isolates(std::move(live));
}
inline bool _install_jit_isolate_teardown_hook() {
  isolate_teardown_join_hook() = _jit_isolate_teardown_join_all;
  return true;
}
inline const bool _jit_isolate_teardown_hook_installed =
    _install_jit_isolate_teardown_hook();

// _jit_native_method (captureless native method closure) now lives in jit.h.

inline JitValue _jit_build_isolate_handle(int64_t id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_core_id", JitValue{TAG_LONG, id}, true);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  auto m = [&](const char* name,
               void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    h->set_or_append(
        name,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  m("join", _jit_isolate_join);
  m("poll", _jit_isolate_poll);
  m("drop", _jit_isolate_drop);
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Runtime entry for Isolate.spawn(fn, *args). First slice: validates cross-
// thread execution of a shared fn_ptr. Always threaded (the parallelism-cap
// fallback lands with the full port).
inline JitValue culebra_jit_isolate_spawn(int8_t fn_tag, int64_t fn_data,
                                          int64_t n_args, JitValue* args,
                                          int64_t line, int64_t col) {
  if (fn_tag != TAG_FUNC) {
    throw culebra::CulebraError(
        "TypeError", "Isolate.spawn: first argument must be a function", line,
        col);
  }
  JitSerCtx sc;
  sendable::SendNode sclo;
  std::vector<sendable::SendNode> sargs;
  // Prefix the operation + carry the spawn-site location, mirroring the interp
  // (isolate.h). Keeps the SendError message byte-identical across backends.
  try {
    sclo = jit_serialize({fn_tag, fn_data}, sc);
    sargs.reserve(n_args);
    for (int64_t i = 0; i < n_args; i++)
      sargs.push_back(jit_serialize(args[i], sc));
  } catch (culebra::CulebraError& e) {
    if (e.kind == "SendError" && e.line == 0) {
      throw culebra::CulebraError(
          "SendError", std::string("Isolate.spawn: ") + e.what(), line, col);
    }
    throw;
  }

  auto core = std::make_shared<IsolateCore>();
  int64_t id = jit_isolate_next_id().fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(jit_isolate_reg_mutex());
    jit_isolate_reg()[id] = core;
  }
  // A channel-carrying closure (or arg) may block on a peer isolate, so it MUST
  // run concurrently — never the synchronous over-cap fallback below, which
  // joins immediately and would deadlock (e.g. a streaming producer fills a
  // bounded channel before any consumer starts). Mirrors the interp spawn and
  // fan_in(items, fn). CPU-bound closures still honor the cap.
  bool must_thread = node_carries_channel(sclo);
  for (const auto& a : sargs)
    if (!must_thread && node_carries_channel(a)) must_thread = true;
  bool under_cap =
      g_live_isolates().fetch_add(1, std::memory_order_relaxed) < isolate_cap();
  bool threaded = must_thread || under_cap;
  if (!threaded) g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
  core->thread = culebra::SizedThread([core, sclo = std::move(sclo),
                              sargs = std::move(sargs), threaded]() mutable {
    run_isolate_child_jit(core, sclo, sargs, /*decrement_live=*/threaded);
  });
  if (!threaded) {
    // Synchronous fallback over the cap: still a fresh thread (the JIT multifn
    // tables are thread_local, so an inline run on the parent thread would
    // pollute the parent's overloads) but joined immediately, so concurrency
    // stays bounded.
    core->thread.join();
    core->joined = true;
  }
  return _jit_build_isolate_handle(id);
}

// ===========================================================================
// JIT channel endpoints. The ChannelCore + registry + auto-close are the
// backend-neutral machinery in isolate.h; here is the JitObject endpoint and
// the JitValue serialize/deserialize boundary. Endpoint methods are captureless
// and reach the core by the id stored on the endpoint.
// ===========================================================================

inline int64_t _jit_self_long(JitValue self, const char* key) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t i = h->find_slot(key);
  return i == static_cast<size_t>(-1) ? 0 : h->slots[i].value.data;
}

inline void _jit_chan_send(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n,
                               JitValue* args) {
  JitValue self{self_tag, self_data};
  // Callee-consumes: release self + the sent value on any exit. jit_serialize
  // throws for a non-Sendable value, and the caller does not clean a native
  // method's operands on unwind, so the guards must (else self + arg strand).
  JitMethodSelf _s{self};
  JitMethodArgs _a{n, args};
  int64_t id = _jit_self_long(self, "__channel_id__");
  if (n >= 1) {
    JitSerCtx sc;
    // The thunk ABI carries no line/col; backfill the serializer's
    // positionless errors (SendError, nesting ValueError) from the call
    // site the codegen published, like _jit_ns_method_dispatch does.
    _jit_at_call_site([&] {
      channel_send_node(id, jit_serialize(args[0], sc));  // Sendable check here
    });
  }
  { *__ret = {TAG_NIL, 0}; return; }
}

inline void _jit_chan_recv(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // Callee-consumes on any exit: chan_pop_blocking raises Interrupted on a
  // cancelled isolate, which a tail release would strand `self` on.
  JitMethodSelf _s{self};
  int64_t id = _jit_self_long(self, "__channel_id__");
  auto node = chan_pop_blocking(id);
  JitValue ret{TAG_NIL, 0};
  if (node) {
    JitDeCtx dc;
    ret = jit_deserialize(*node, dc);
    release_inflight_channels(*node);
  }
  { *__ret = ret; return; }
}

// True if the handle was already dropped; otherwise marks it dropped. Shared
// by the channel and SharedBuffer drop handlers so their refcount moves once
// across explicit `drop()` + GC teardown.
inline bool _jit_handle_drop_consumed(JitObject* h) {
  size_t di = h->find_slot("_dropped");
  if (di != static_cast<size_t>(-1) && h->slots[di].value.tag == TAG_BOOL &&
      h->slots[di].value.data) {
    return true;
  }
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 1}, true);
  return false;
}

inline void _jit_chan_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  if (!_jit_handle_drop_consumed(reinterpret_cast<JitObject*>(self.data)))
    chan_drop(_jit_self_long(self, "__channel_id__"),
              static_cast<int>(_jit_self_long(self, "__channel_role__")));
  { *__ret = {TAG_NIL, 0}; return; }
}

inline void _jit_chan_clone(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  int64_t id = _jit_self_long(self, "__channel_id__");
  int role = static_cast<int>(_jit_self_long(self, "__channel_role__"));
  chan_bump(id, role, +1);
  { *__ret = _jit_make_channel_endpoint(id, role); return; }
}

// rx for-in iterator: has_next pulls one value (blocking, ends on close), next
// hands over the cached lookahead. State lives in the iterator's own slots.
inline void _jit_chan_iter_self(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                    JitValue*) {
  JitValue self{self_tag, self_data};
  culebra_runtime_value_retain(self.tag, self.data);
  { *__ret = self; return; }
}
inline void _jit_chan_iter_has_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                        JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (blocking pull can raise)
  auto* it = reinterpret_cast<JitObject*>(self.data);
  int64_t st = _jit_self_long(self, "_st");
  JitValue ret;
  if (st == 1) {
    ret = {TAG_BOOL, 1};
  } else if (st == 2) {
    ret = {TAG_BOOL, 0};
  } else {
    auto node = chan_pop_blocking(_jit_self_long(self, "_cid"));
    if (node) {
      JitDeCtx dc;
      JitValue v = jit_deserialize(*node, dc);
      release_inflight_channels(*node);
      it->set_or_append("_lv", v, true);  // slot owns the +1
      it->set_or_append("_st", JitValue{TAG_LONG, 1}, true);
      ret = {TAG_BOOL, 1};
    } else {
      it->set_or_append("_st", JitValue{TAG_LONG, 2}, true);
      ret = {TAG_BOOL, 0};
    }
  }
  { *__ret = ret; return; }
}
inline void _jit_chan_iter_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                    JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  JitValue ret{TAG_NIL, 0};
  if (_jit_self_long(self, "_st") == 1) {
    ret = it->slots[it->find_slot("_lv")].value;  // transfer slot's +1 to caller
    it->set_or_append("_lv", JitValue{TAG_NIL, 0}, true);  // overwrite, no release
    it->set_or_append("_st", JitValue{TAG_LONG, 0}, true);
  }
  { *__ret = ret; return; }
}
inline void _jit_chan_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  int64_t id = _jit_self_long(self, "__channel_id__");
  auto* it = culebra_runtime_object_new();
  it->set_or_append("_cid", JitValue{TAG_LONG, id}, true);
  it->set_or_append("_st", JitValue{TAG_LONG, 0}, true);
  it->set_or_append("_lv", JitValue{TAG_NIL, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    it->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("iter", _jit_chan_iter_self);
  meth("has_next", _jit_chan_iter_has_next);
  meth("next", _jit_chan_iter_next);
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}

inline JitValue _jit_make_channel_endpoint(int64_t id, int role) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__channel_endpoint__", JitValue{TAG_BOOL, 1}, false);
  h->set_or_append("__channel_id__", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__channel_role__", JitValue{TAG_LONG, role}, false);
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    h->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("drop", _jit_chan_drop);
  meth("clone", _jit_chan_clone);
  if (role == 0) {
    meth("send", _jit_chan_send);
  } else {
    meth("recv", _jit_chan_recv);
    meth("iter", _jit_chan_iter);
  }
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Idempotent SharedBuffer handle drop (explicit `buf.drop()` + GC teardown).
inline void _jit_shared_buffer_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                        JitValue*) {
  JitValue self{self_tag, self_data};
  if (!_jit_handle_drop_consumed(reinterpret_cast<JitObject*>(self.data)))
    culebra::shared_buffer_drop(_jit_self_long(self, "__sharedbuffer_id__"));
  { *__ret = {TAG_NIL, 0}; return; }
}

// `flush()` on a file-backed buffer (msync). Only attached to file handles.
inline void _jit_shared_buffer_flush(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                         JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  culebra::shared_buffer_flush(_jit_self_long(self, "__sharedbuffer_id__"));
  { *__ret = {TAG_NIL, 0}; return; }
}

// `with_lock(fn)`: run `fn()` holding the buffer's lock, return its value. The
// lock guard + JitMethodSelf + JitMethodArgs release the lock, `self`, and the
// callback on any exit (mirrors the interp make_shared_buffer_handle with_lock).
inline void _jit_shared_buffer_with_lock(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data,
                                             int64_t n_args, JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  // Callee-consumes the callback arg on any exit. _culebra_invoke0 only borrows
  // it, so without this the +1 the caller handed over strands (the callback's
  // result is a separate +1, returned before this guard runs).
  JitMethodArgs _a{n_args, args};
  if (n_args < 1 || args[0].tag != TAG_FUNC) {
    throw culebra::CulebraError("TypeError",
        "SharedBuffer.with_lock expects a function");
  }
  auto core = culebra::lookup_shared_buffer(
      _jit_self_long(self, "__sharedbuffer_id__"));
  if (!core) {
    throw culebra::CulebraError("ValueError", "SharedBuffer has been dropped");
  }
  culebra::SharedBufferLockGuard guard(std::move(core));
  { *__ret = _culebra_invoke0(reinterpret_cast<JitClosure*>(args[0].data)); return; }
}


// ===========================================================================
// Shared.new views (concurrency C4). The frozen SendNode tree lives in
// culebra::shared_val_registry(); a view is a plain JitObject carrying
// (id, node-index) markers + the O(1) `is_shared_val` flag + the reader
// methods. Data reads are intercepted in the object get/index runtime
// helpers (jit.h), mirroring the interp's eval_property /
// eval_array_reference hooks.
// ===========================================================================

struct JitResolvedNode {
  std::shared_ptr<culebra::SharedValCore> core;
  const sendable::SendNode* node;
  int64_t id;
  size_t idx;  // node table index (== obj_index key for an Object node)
};

inline JitResolvedNode
_jit_shared_val_node_of(JitObject* view, int64_t line = 0, int64_t col = 0) {
  // Position: the get/index interceptions pass the access position; the
  // handle methods leave 0 and fall back to the published call site
  // (the handle-method ClosedError convention).
  if (line == 0) {
    line = _jit_call_site_line;
    col = _jit_call_site_col;
  }
  size_t di = view->find_slot("_dropped");
  if (di != static_cast<size_t>(-1) && view->slots[di].value.data) {
    throw culebra::CulebraError("ClosedError",
                                "Shared value has been dropped", line, col);
  }
  int64_t id = view->slots[view->find_slot("__sharedval_id__")].value.data;
  auto core = culebra::lookup_shared_val(id);
  if (!core) {
    throw culebra::CulebraError("ClosedError",
                                "Shared value has been dropped", line, col);
  }
  int64_t node = view->slots[view->find_slot("__sharedval_node__")].value.data;
  if (node < 0 || node >= static_cast<int64_t>(core->nodes.size())) {
    throw culebra::CulebraError("ValueError", "corrupt Shared view",
                                line, col);
  }
  const auto* n = core->nodes[static_cast<size_t>(node)];
  return {std::move(core), n, id, static_cast<size_t>(node)};
}

// One node, one JitValue (+1): a leaf materializes (strings are heap
// strings, leak-bounded by design), a container mints a sub-view (+1 on
// the registry, released by that view's drop).
inline JitValue _jit_shared_val_read(int64_t id,
                                     const culebra::SharedValCore& core,
                                     const sendable::SendNode& n) {
  using K = sendable::SendNode::K;
  switch (n.kind) {
    case K::Nil:   return {TAG_NIL, 0};
    case K::Bool:  return {TAG_BOOL, n.b ? 1 : 0};
    case K::Long:  return {TAG_LONG, n.i};
    case K::Float: return {TAG_FLOAT, _culebra_double_to_bits(n.d)};
    case K::Str:
      return {TAG_STRING, reinterpret_cast<int64_t>(_culebra_heap_str(n.s))};
    case K::Array:
    case K::Object:
    case K::Set:
    case K::Tuple: {
      culebra::shared_val_bump(id);
      return _jit_make_shared_val_view(
          id, static_cast<int64_t>(core.ids.at(&n)));
    }
    default:
      throw culebra::CulebraError("ValueError", "corrupt Shared view");
  }
}

// Memoize a container child read on the PARENT view so the +0-borrowed
// contract the get/index interceptions return under is honored WITHOUT a leak.
// `_jit_shared_val_read` mints a FRESH +1 sub-view for a container node;
// returning that through the borrowed-read channel orphaned the +1 (only the
// tracing backstop reclaimed it — the sharedval carve-out leak). Instead cache
// the sub-view on the parent view, keyed by the child's frozen-node index, so
// the PARENT owns the +1 and every read of the same child returns the same
// borrowed handle. The parent's release cascades: its non_string_props sidecar
// (unused by a view otherwise — views are immutable, no user non-String keys)
// releases the cached sub-views on the object release path, each dropping its
// registry ref. Primitives/strings have no sub-view, so they pass straight
// through. Views are per-isolate JitObjects (only the core id is shared), so
// this mutation needs no lock. "Leak-free" here means bounded retention for the
// parent view's lifetime (a distinct child read pins its sub-view until the
// view drops), not eager reclaim of a transiently-unreferenced sub-view — the
// necessary cost of honoring the +0-borrowed return contract. Retention is
// bounded by the frozen tree's own container-node count per live view.
inline JitValue _jit_shared_val_child(JitObject* view, int64_t id,
                                      const culebra::SharedValCore& core,
                                      const sendable::SendNode& child) {
  using K = sendable::SendNode::K;
  switch (child.kind) {
    case K::Array:
    case K::Object:
    case K::Set:
    case K::Tuple:
      break;  // container: memoize below
    default:
      return _jit_shared_val_read(id, core, child);  // primitive/string leaf
  }
  JitValue key{TAG_LONG, static_cast<int64_t>(core.ids.at(&child))};
  if (view->non_string_props) {
    auto it = view->non_string_props->find(key);
    if (it != view->non_string_props->end())
      return it->second.value;  // cache hit: borrowed (+0), parent owns the +1
  }
  JitValue sub = _jit_shared_val_read(id, core, child);  // fresh +1
  if (!view->non_string_props) {
    view->non_string_props = new JitObject::AnyKeyMap();
    view->key_order = new std::vector<JitValue>();
  }
  // Transfer the mint's +1 into the cache (no extra retain). key_order carries
  // the key so the GC child enumeration (which walks key_order ∩
  // non_string_props) sees the cached sub-view as an owned edge; the object
  // release path frees the stored value, and the Long key is non-refcounted.
  (*view->non_string_props)[key] = JitObjectEntry{sub, false};
  view->key_order->push_back(key);
  return sub;  // borrowed (+0)
}

// `view.name` data read on an own-slot miss — Object-node field, nil on
// miss (mirroring a plain Object). Declared in jit.h for the get_ic hook.
CULEBRA_RT_INLINE JitValue _jit_shared_val_prop(JitObject* view,
                                                const char* name,
                                                int64_t line, int64_t col) {
  auto [core, n, id, node_id] = _jit_shared_val_node_of(view, line, col);
  using K = sendable::SendNode::K;
  if (n->kind != K::Object) return {TAG_NIL, 0};
  auto idx_it = core->obj_index.find(node_id);
  if (idx_it != core->obj_index.end()) {
    auto e = idx_it->second.find(std::string_view(name));
    if (e != idx_it->second.end()) {
      return _jit_shared_val_child(view, id, *core,
                                   n->entries[e->second].second);
    }
  }
  return {TAG_NIL, 0};
}

inline bool _jit_shared_val_key_eq(const sendable::SendNode& k, int8_t tag,
                                   int64_t data) {
  using K = sendable::SendNode::K;
  switch (k.kind) {
    case K::Str:
      return (tag == TAG_STRING || tag == TAG_STRINGVIEW) &&
             _culebra_str_view(tag, data) == k.s;
    case K::Long: return tag == TAG_LONG && data == k.i;
    case K::Bool: return tag == TAG_BOOL && (data != 0) == k.b;
    case K::Float:
      return tag == TAG_FLOAT && _culebra_float_to_double(data) == k.d;
    default: return false;
  }
}

// `view[key]` — Object key lookup (KeyError on miss) or Array/Tuple
// positional read (IndexError). Key is BORROWED here (the caller
// consumes it per the object_get_any contract).
CULEBRA_RT_INLINE JitValue _jit_shared_val_index(JitObject* view,
                                                 int8_t key_tag,
                                                 int64_t key_data,
                                                 int64_t line, int64_t col) {
  auto [core, n, id, node_id] = _jit_shared_val_node_of(view, line, col);
  using K = sendable::SendNode::K;
  switch (n->kind) {
    case K::Object: {
      if (key_tag == TAG_STRING || key_tag == TAG_STRINGVIEW) {
        auto idx_it = core->obj_index.find(node_id);
        if (idx_it != core->obj_index.end()) {
          auto e = idx_it->second.find(_culebra_str_view(key_tag, key_data));
          if (e != idx_it->second.end()) {
            return _jit_shared_val_child(view, id, *core,
                                         n->entries[e->second].second);
          }
        }
        throw culebra::CulebraError("KeyError", "key not present", line, col);
      }
      for (const auto& [k, v] : n->entries) {
        if (_jit_shared_val_key_eq(k, key_tag, key_data)) {
          return _jit_shared_val_child(view, id, *core, v);
        }
      }
      throw culebra::CulebraError("KeyError", "key not present", line, col);
    }
    case K::Array:
    case K::Tuple: {
      int64_t len = static_cast<int64_t>(n->elems.size());
      if (key_tag != TAG_LONG) {
        // The interp's key.to_long(): no Float truncation, canonical
        // "expected Long, got <T>".
        culebra_runtime_type_error_typed(line, col, "Long", key_tag);
      }
      int64_t idx = key_data;
      if (idx < 0) idx += len;
      if (idx < 0 || idx >= len) {
        throw culebra::CulebraError("IndexError", "index out of range",
                                    line, col);
      }
      return _jit_shared_val_child(view, id, *core,
                                   n->elems[static_cast<size_t>(idx)]);
    }
    default:
      throw culebra::CulebraError("TypeError",
                                  "this Shared value is not indexable",
                                  line, col);
  }
}

// --- reader methods (handle ABI: self arrives +1, released here) ---
// Throw for an Object-only reader (has/keys/values) on a non-Object node.
inline void _jit_sv_require_object(const sendable::SendNode* n,
                                   const char* m) {
  if (n->kind != sendable::SendNode::K::Object) {
    throw culebra::CulebraError("TypeError",
        std::string(m) + "() requires an Object Shared view");
  }
}



inline void _jit_sv_size(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  // Callee-consumes on any exit: _jit_shared_val_node_of raises ClosedError
  // on a dropped view, which a tail release would strand `self` on.
  JitMethodSelf _s{self};
  auto [core, n, id, idx] = _jit_shared_val_node_of(
      reinterpret_cast<JitObject*>(self.data));
  using K = sendable::SendNode::K;
  int64_t sz = n->kind == K::Object
                   ? static_cast<int64_t>(n->entries.size())
                   : static_cast<int64_t>(n->elems.size());
  { *__ret = {TAG_LONG, sz}; return; }
}

inline void _jit_sv_has(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t n_args,
                            JitValue* args) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  JitMethodArgs _a{n_args, args};  // consumed on any exit, throws included
  auto [core, n, id, idx] = _jit_shared_val_node_of(
      reinterpret_cast<JitObject*>(self.data));
  using K = sendable::SendNode::K;
  _jit_sv_require_object(n, "has");
  if (n_args < 1) {
    throw culebra::CulebraError("ArityError",
                                culebra::missing_required_arg_message("key"));
  }
  for (const auto& [k, v] : n->entries) {
    if (_jit_shared_val_key_eq(k, args[0].tag, args[0].data)) {
      { *__ret = {TAG_BOOL, 1}; return; }
    }
  }
  { *__ret = {TAG_BOOL, 0}; return; }
}

// Deep materialization of one node into the local heap — shared by
// keys()/copy()/iteration keys. Data-only by construction (freeze
// rejected closures/handles), so a plain DeCtx round-trip is exact.
inline JitValue _jit_sv_materialize(const sendable::SendNode& n) {
  JitDeCtx dc;
  return jit_deserialize(n, dc);
}

inline void _jit_sv_keys(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto [core, n, id, idx] = _jit_shared_val_node_of(
      reinterpret_cast<JitObject*>(self.data));
  using K = sendable::SendNode::K;
  _jit_sv_require_object(n, "keys");
  auto* arr = culebra_runtime_array_new();
  for (const auto& [k, v] : n->entries) {
    JitValue kv = _jit_sv_materialize(k);
    culebra_runtime_array_push(arr, kv.tag, kv.data);
    _culebra_value_release_impl(kv.tag, kv.data);
  }
  { *__ret = {TAG_ARRAY, reinterpret_cast<int64_t>(arr)}; return; }
}

inline void _jit_sv_values(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                               JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* view = reinterpret_cast<JitObject*>(self.data);
  auto [core, n, id, idx] = _jit_shared_val_node_of(view);
  using K = sendable::SendNode::K;
  _jit_sv_require_object(n, "values");
  auto* arr = culebra_runtime_array_new();
  for (const auto& [k, v] : n->entries) {
    JitValue vv = _jit_shared_val_read(id, *core, v);
    culebra_runtime_array_push(arr, vv.tag, vv.data);
    _culebra_value_release_impl(vv.tag, vv.data);
  }
  { *__ret = {TAG_ARRAY, reinterpret_cast<int64_t>(arr)}; return; }
}

inline void _jit_sv_copy(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto [core, n, id, idx] = _jit_shared_val_node_of(
      reinterpret_cast<JitObject*>(self.data));
  { *__ret = _jit_sv_materialize(*n); return; }
}

inline void _jit_sv_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  if (!_jit_handle_drop_consumed(reinterpret_cast<JitObject*>(self.data)))
    culebra::shared_val_drop(_jit_self_long(self, "__sharedval_id__"));
  { *__ret = {TAG_NIL, 0}; return; }
}

// for-in protocol: the JIT's object iter dispatch prefers a user `iter`
// own slot, so binding one here covers `for ... in view` with no core
// changes. Object nodes yield (key, value) pairs, Array/Tuple/Set nodes
// yield elements — mirroring the local collection protocols. The tree is
// immutable, so a plain cursor is a correct snapshot.
inline void _jit_sv_iter_has_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                      JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (node_of raises ClosedError)
  auto [core, n, id, nidx] = _jit_shared_val_node_of(
      reinterpret_cast<JitObject*>(self.data));
  (void)nidx;
  using K = sendable::SendNode::K;
  int64_t total = n->kind == K::Object ? static_cast<long>(n->entries.size())
                                    : static_cast<long>(n->elems.size());
  int64_t idx = _jit_self_long(self, "_idx");
  { *__ret = {TAG_BOOL, idx < total ? 1 : 0}; return; }
}

inline void _jit_sv_iter_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                  JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  auto* it = reinterpret_cast<JitObject*>(self.data);
  auto [core, n, id, nidx] = _jit_shared_val_node_of(it);
  (void)nidx;
  using K = sendable::SendNode::K;
  int64_t total = n->kind == K::Object ? static_cast<long>(n->entries.size())
                                    : static_cast<long>(n->elems.size());
  int64_t idx = _jit_self_long(self, "_idx");
  if (idx >= total) { *__ret = {TAG_NIL, 0}; return; }
  it->set_or_append("_idx", JitValue{TAG_LONG, idx + 1}, true);
  if (n->kind == K::Object) {
    const auto& [k, v] = n->entries[static_cast<size_t>(idx)];
    JitValue kv = _jit_sv_materialize(k);
    JitValue vv = _jit_shared_val_read(id, *core, v);
    auto* pair = culebra_runtime_tuple_new();
    culebra_runtime_tuple_push(pair, kv.tag, kv.data);
    culebra_runtime_tuple_push(pair, vv.tag, vv.data);
    _culebra_value_release_impl(kv.tag, kv.data);
    _culebra_value_release_impl(vv.tag, vv.data);
    { *__ret = {TAG_TUPLE, reinterpret_cast<int64_t>(pair)}; return; }
  }
  { *__ret = _jit_shared_val_read(id, *core,
                              n->elems[static_cast<size_t>(idx)]); return; }
}

inline void _jit_sv_iter_self(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                  JitValue*) {
  JitValue self{self_tag, self_data};
  culebra_runtime_value_retain(self.tag, self.data);
  { *__ret = self; return; }
}

inline void _jit_sv_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (node_of raises ClosedError)
  auto* view = reinterpret_cast<JitObject*>(self.data);
  // Validate first (a dropped handle must not mint a live iterator) and
  // reuse the resolved id/node index.
  auto [core, n, id, idx] = _jit_shared_val_node_of(view);
  int64_t node = static_cast<int64_t>(idx);
  // The iterator is a PLAIN object (NOT a view): it carries the id/node
  // for has_next/next's node accessor plus a cursor, but `is_shared_val`
  // stays false so introspection (.keys()/.size()) sees an ordinary
  // iterator object rather than routing into the frozen tree — the
  // interp's iterator is likewise a plain `{has_next, next}` object. Its
  // registry ref (bumped here) is released by its own drop on scope exit
  // (has_drop), keeping the tree alive for the iteration like the interp
  // captures the shared_ptr.
  culebra::shared_val_bump(id);
  auto* it = culebra_runtime_object_new();
  it->set_or_append("__sharedval_id__", JitValue{TAG_LONG, id}, false);
  it->set_or_append("__sharedval_node__", JitValue{TAG_LONG, node}, false);
  it->set_or_append("_idx", JitValue{TAG_LONG, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    it->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("iter", _jit_sv_iter_self);
  meth("has_next", _jit_sv_iter_has_next);
  meth("next", _jit_sv_iter_next);
  meth("drop", _jit_sv_drop);
  it->has_drop = true;
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}

inline JitValue _jit_make_shared_val_view(int64_t id, int64_t node) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__sharedval_id__", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__sharedval_node__", JitValue{TAG_LONG, node}, false);
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    h->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("size", _jit_sv_size);
  meth("has", _jit_sv_has);
  meth("keys", _jit_sv_keys);
  meth("values", _jit_sv_values);
  meth("copy", _jit_sv_copy);
  meth("iter", _jit_sv_iter);
  meth("drop", _jit_sv_drop);
  h->is_shared_val = true;
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Build a JIT SharedBuffer handle: markers + O(1) discriminator flag + a
// GC-driven `drop` that releases the buffer's ref. Mirrors the channel
// endpoint and the interp make_shared_buffer_handle.
inline JitValue _jit_make_shared_buffer_handle(int64_t id, int64_t count) {
  auto* h = culebra_runtime_object_new();
  h->is_shared_buffer = true;
  h->set_or_append("__sharedbuffer_id__", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__sharedbuffer_count__", JitValue{TAG_LONG, count}, false);
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 0}, true);
  h->set_or_append(
      "drop",
      JitValue{TAG_FUNC,
               reinterpret_cast<int64_t>(_jit_native_method(_jit_shared_buffer_drop))},
      false);
  h->set_or_append(
      "with_lock",
      JitValue{TAG_FUNC, reinterpret_cast<int64_t>(
                             _jit_native_method(_jit_shared_buffer_with_lock))},
      false);
  // File-backed buffers additionally expose `flush()` (msync) — mirrors the
  // interp make_shared_buffer_handle.
  auto core = culebra::lookup_shared_buffer(id);
  if (core && core->storage == culebra::SharedBufferCore::Storage::File) {
    h->set_or_append(
        "flush",
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(
                               _jit_native_method(_jit_shared_buffer_flush))},
        false);
  }
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// JIT merged rx (Channel.fan_in). Same JitObject endpoint shape as a channel rx,
// but reaches several sources via chan_select_recv (the backend-neutral core).
// The iterator's iter_self/next slots are generic, so only has_next differs.
inline void _jit_merged_recv(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (blocking select can raise)
  int64_t mid = _jit_self_long(self, "__merged_id__");
  auto node = chan_select_recv(mid);
  JitValue ret{TAG_NIL, 0};
  if (node) {
    JitDeCtx dc;
    ret = jit_deserialize(*node, dc);
    release_inflight_channels(*node);
  }
  { *__ret = ret; return; }
}
inline void _jit_merged_drop(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t di = h->find_slot("_dropped");
  if (di != static_cast<size_t>(-1) && h->slots[di].value.tag == TAG_BOOL &&
      h->slots[di].value.data) {
    { *__ret = {TAG_NIL, 0}; return; }
  }
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 1}, true);
  chan_merge_drop(_jit_self_long(self, "__merged_id__"));  // idempotent anyway
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_merged_join(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (join re-raises a failure)
  int64_t mid = _jit_self_long(self, "__merged_id__");
  // Join the producers (fan_in(items, fn)) and surface the first error. Extract
  // through the JIT path so a structured error re-raises as CulebraError and a
  // raw `throw <value>` re-raises as a CulebraException — both caught by a
  // compiled `catch`. (chan_merge_join is the interp twin, using Value throws.)
  std::optional<culebra::CulebraError> first_err;
  bool have_thrown = false;
  int8_t tt = 0;
  int64_t td = 0;
  for (auto& core : chan_merge_producers(mid)) {
    try {
      _jit_isolate_extract(core);  // joins; re-raises the producer's failure
    } catch (culebra::CulebraError& e) {
      if (!first_err && !have_thrown) first_err = e;
    } catch (CulebraException& e) {
      // We are the matching catch, so we owe the throw's balancing release
      // (a compiled `catch` would do it). Keep the first value's origin ref for
      // the re-raise below; drop any later ones fully.
      culebra_runtime_value_release(e.tag, e.data);
      if (!first_err && !have_thrown) {
        have_thrown = true;
        tt = e.tag;
        td = e.data;
      } else {
        culebra_runtime_value_release(e.tag, e.data);
      }
    }
  }
  if (have_thrown) culebra_runtime_throw(tt, td);
  if (first_err) throw *first_err;
  { *__ret = {TAG_NIL, 0}; return; }
}
inline void _jit_merged_iter_has_next(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t,
                                          JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};  // consumed on any exit (blocking select can raise)
  auto* it = reinterpret_cast<JitObject*>(self.data);
  int64_t st = _jit_self_long(self, "_st");
  JitValue ret;
  if (st == 1) {
    ret = {TAG_BOOL, 1};
  } else if (st == 2) {
    ret = {TAG_BOOL, 0};
  } else {
    auto node = chan_select_recv(_jit_self_long(self, "_mid"));
    if (node) {
      JitDeCtx dc;
      JitValue v = jit_deserialize(*node, dc);
      release_inflight_channels(*node);
      it->set_or_append("_lv", v, true);
      it->set_or_append("_st", JitValue{TAG_LONG, 1}, true);
      ret = {TAG_BOOL, 1};
    } else {
      it->set_or_append("_st", JitValue{TAG_LONG, 2}, true);
      ret = {TAG_BOOL, 0};
    }
  }
  { *__ret = ret; return; }
}
inline void _jit_merged_iter(JitValue* __ret, JitClosure*, int8_t self_tag, int64_t self_data, int64_t, JitValue*) {
  JitValue self{self_tag, self_data};
  JitMethodSelf _s{self};
  int64_t mid = _jit_self_long(self, "__merged_id__");
  auto* it = culebra_runtime_object_new();
  it->set_or_append("_mid", JitValue{TAG_LONG, mid}, true);
  it->set_or_append("_st", JitValue{TAG_LONG, 0}, true);
  it->set_or_append("_lv", JitValue{TAG_NIL, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    it->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("iter", _jit_chan_iter_self);          // reuse: returns self
  meth("has_next", _jit_merged_iter_has_next);
  meth("next", _jit_chan_iter_next);          // reuse: hands over _lv
  { *__ret = {TAG_OBJECT, reinterpret_cast<int64_t>(it)}; return; }
}
inline JitValue _jit_make_merged_rx_endpoint(int64_t mid) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__merged_rx__", JitValue{TAG_BOOL, 1}, false);
  h->set_or_append("__merged_id__", JitValue{TAG_LONG, mid}, false);
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 0}, true);
  auto meth = [&](const char* nm,
                  void (*f)(JitValue*, JitClosure*, int8_t, int64_t, int64_t, JitValue*)) {
    h->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("recv", _jit_merged_recv);
  meth("iter", _jit_merged_iter);
  meth("drop", _jit_merged_drop);
  meth("join", _jit_merged_join);
  h->has_drop = true;
  return {TAG_OBJECT, reinterpret_cast<int64_t>(h)};
}

// Channel.fan_in(a, fn = nil) -> merged rx. With one arg, merge the given
// receivers. With (items, fn), spawn one producer per item (fn(item, tx)) and
// merge their streams — JIT mirror of make_channel_namespace's fan_in.
inline JitValue culebra_jit_channel_fan_in(int64_t n, JitValue* args,
                                           int64_t line, int64_t col) {
  if (n < 1 || args[0].tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        "Channel.fan_in: first argument must be an Array", line, col);
  }
  auto* arr = reinterpret_cast<JitArray*>(args[0].data);

  if (n < 2 || args[1].tag != TAG_FUNC) {
    // fan_in([rx, ...]): merge existing receivers.
    std::vector<int64_t> ids;
    ids.reserve(arr->size);
    for (size_t i = 0; i < arr->size; i++) {
      JitValue s = arr->items[i];
      bool ok = s.tag == TAG_OBJECT;
      JitObject* o = ok ? reinterpret_cast<JitObject*>(s.data) : nullptr;
      if (!ok ||
          o->find_slot("__channel_endpoint__") == static_cast<size_t>(-1) ||
          o->slots[o->find_slot("__channel_role__")].value.data != 1) {
        throw culebra::CulebraError("TypeError",
            "Channel.fan_in: every source must be a receiver (rx)", line, col);
      }
      int64_t id = o->slots[o->find_slot("__channel_id__")].value.data;
      chan_bump(id, /*role=*/1, +1);
      ids.push_back(id);
    }
    return _jit_make_merged_rx_endpoint(chan_merge_register(std::move(ids)));
  }

  // fan_in(items, fn): spawn one producer per item, merge. Producers run
  // threaded (a streaming producer blocks on send, so the inline-over-cap
  // fallback would deadlock). fn(item, tx) sends to its own channel; the
  // parent's tx is never created, the producer's auto-drops on exit.
  JitSerCtx fsc;
  sendable::SendNode sfn;
  try {
    sfn = jit_serialize(args[1], fsc);  // Sendable check (once)
  } catch (culebra::CulebraError& e) {
    if (e.kind == "SendError" && e.line == 0)
      throw culebra::CulebraError(
          "SendError", std::string("Channel.fan_in: ") + e.what(), line, col);
    throw;
  }
  std::vector<int64_t> ids;
  std::vector<std::shared_ptr<IsolateCore>> producers;
  for (size_t i = 0; i < arr->size; i++) {
    auto chcore = std::make_shared<ChannelCore>(1);
    long cid = channel_next_id().fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard<std::mutex> lk(channel_registry_mutex());
      channel_registry()[cid] = chcore;
    }
    chcore->tx_count = 0;
    chcore->rx_count = 1;
    JitSerCtx asc;
    std::vector<sendable::SendNode> sargs;
    try {
      sargs.push_back(jit_serialize(arr->items[i], asc));
    } catch (culebra::CulebraError& e) {
      if (e.kind == "SendError" && e.line == 0)
        throw culebra::CulebraError(
            "SendError", std::string("Channel.fan_in: ") + e.what(), line, col);
      throw;
    }
    sendable::SendNode txnode;
    txnode.kind = sendable::SendNode::K::Channel;
    txnode.i = cid;
    txnode.b = false;  // tx
    chan_bump(cid, /*role=*/0, +1);  // in-flight (released by the child)
    sargs.push_back(std::move(txnode));
    auto pcore = std::make_shared<IsolateCore>();
    g_live_isolates().fetch_add(1, std::memory_order_relaxed);
    pcore->thread = culebra::SizedThread(
        [pcore, sfn, sargs = std::move(sargs)]() mutable {
          run_isolate_child_jit(pcore, sfn, sargs, /*decrement_live=*/true);
        });
    producers.push_back(std::move(pcore));
    ids.push_back(cid);
  }
  return _jit_make_merged_rx_endpoint(
      chan_merge_register(std::move(ids), std::move(producers)));
}

// ===========================================================================
// JIT Parallel.map / Parallel.each — reuses the backend-neutral ParallelState
// (SendNode-based) and fail-fast machinery from isolate.h; only the per-element
// work (deserialize → invoke fn_ptr → serialize) is JIT-specific.
// ===========================================================================

// A settled Result Object {ok, value, error} on the JIT heap; set_or_append
// takes ownership of `value`, so the caller hands its +1 over and releases the
// returned Object (which frees the fields). Mirrors interp _settled_ok/_err.
inline JitValue _jit_settled_result(bool ok, JitValue value, const char* err) {
  auto* o = culebra_runtime_object_new();
  o->set_or_append("ok", JitValue{TAG_BOOL, ok ? 1 : 0}, false);
  o->set_or_append("value", value, false);
  o->set_or_append("error",
                   ok ? JitValue{TAG_NIL, 0}
                      : JitValue{TAG_STRING,
                                 reinterpret_cast<int64_t>(_culebra_heap_str(err))},
                   false);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(o)};
}

// JIT counterparts of parallel_record_success / _element_error (the interp ones
// build Value; these build JitValue). The result is owned and released here.
inline void jit_parallel_record_success(ParallelState& st, size_t i,
                                        JitValue raw) {
  JitOwnedVal r(raw);  // released on every path, a throwing serialize included
  switch (st.mode) {
    case culebra::PMode::Map: {
      JitSerCtx sc;
      st.results[i] = jit_serialize(r.borrow(), sc);
      break;
    }
    case culebra::PMode::MapSettled: {
      JitOwnedVal o(_jit_settled_result(true, r.consume(), nullptr));  // owns r
      JitSerCtx sc;
      st.results[i] = jit_serialize(o.borrow(), sc);
      break;
    }
    case culebra::PMode::Each:
      break;
    case culebra::PMode::Race:
      if (!st.race_won.exchange(true)) {
        std::lock_guard<std::mutex> lk(st.err_m);
        JitSerCtx sc;
        st.race_result = jit_serialize(r.borrow(), sc);
        st.interrupt.store(true, std::memory_order_relaxed);
      }
      break;
  }
}

inline void jit_parallel_record_element_error(ParallelState& st, size_t i,
                                              culebra::CulebraError e) {
  switch (st.mode) {
    case culebra::PMode::MapSettled: {
      JitValue o = _jit_settled_result(false, JitValue{TAG_NIL, 0}, e.what());
      JitSerCtx sc;
      st.results[i] = jit_serialize(o, sc);
      culebra_runtime_value_release(o.tag, o.data);
      break;
    }
    case culebra::PMode::Race: {
      std::lock_guard<std::mutex> lk(st.err_m);
      if (!st.failed) { st.failed = true; st.err_index = i; st.err = std::move(e); }
      break;
    }
    default:
      parallel_record_error(st, i, std::move(e));
  }
}

inline void jit_parallel_worker(std::shared_ptr<ParallelState> st) {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  rt.interrupt_flag = &st->interrupt;
  try {
    JitDeCtx dc;
    // Owned for the same reason as the isolate child's closure: the captures
    // (a `tx` above all) must drop on every exit path, throw included.
    JitOwnedVal fn(jit_deserialize(st->fn, dc));
    auto* cls = reinterpret_cast<JitClosure*>(fn.borrow().data);
    while (!st->interrupt.load(std::memory_order_relaxed)) {
      size_t i = st->next.fetch_add(1, std::memory_order_relaxed);
      if (i >= st->items.size()) break;
      try {
        JitDeCtx idc;
        JitOwnedVal item(jit_deserialize(st->items[i], idc));
        JitValue r = _culebra_invoke1(cls, item.consume());  // invoke consumes
        jit_parallel_record_success(*st, i, r);
      } catch (culebra::CulebraError& e) {
        jit_parallel_record_element_error(*st, i, e);
      } catch (std::exception& e) {
        jit_parallel_record_element_error(*st, i,
            culebra::CulebraError("RuntimeError", e.what()));
      }
      st->done.fetch_add(1, std::memory_order_relaxed);  // for on_progress
    }
  } catch (culebra::CulebraError& e) {
    parallel_record_error(*st, 0, e);
  } catch (std::exception& e) {
    parallel_record_error(*st, 0,
        culebra::CulebraError("RuntimeError", e.what()));
  }
}

// Parent-thread coordinator: polls `done` and calls the JIT closure
// on_progress(done, total) on the parent heap. Mirrors the interp version; the
// JIT callback is a JitClosure invoked directly via _culebra_invoke2.
inline void jit_parallel_progress_coordinator(
    std::shared_ptr<ParallelState> st, JitClosure* cb,
    std::exception_ptr& cb_err) {
  const size_t total = st->items.size();
  size_t reported = 0;
  while (reported < total && !st->interrupt.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    size_t d = std::min(st->done.load(std::memory_order_relaxed), total);
    if (d == reported) continue;
    reported = d;
    try {
      JitValue rv = _culebra_invoke2(
          cb, JitValue{TAG_LONG, static_cast<int64_t>(reported)},
          JitValue{TAG_LONG, static_cast<int64_t>(total)});
      culebra_runtime_value_release(rv.tag, rv.data);
    } catch (...) {
      cb_err = std::current_exception();
      st->interrupt.store(true, std::memory_order_relaxed);
      return;
    }
  }
}

inline JitValue jit_parallel_run(JitValue items_v, JitValue fn_v, int64_t limit,
                                 culebra::PMode mode, int64_t line, int64_t col,
                                 JitValue on_progress = JitValue{TAG_NIL, 0}) {
  const char* who = culebra::parallel_mode_name(mode);
  if (items_v.tag != TAG_ARRAY) {
    throw culebra::CulebraError("TypeError",
        std::format("Parallel.{}: first argument must be an Array", who),
        line, col);
  }
  if (fn_v.tag != TAG_FUNC) {
    throw culebra::CulebraError("TypeError",
        std::format("Parallel.{}: second argument must be a function", who),
        line, col);
  }
  auto* arr = reinterpret_cast<JitArray*>(items_v.data);
  auto st = std::make_shared<ParallelState>();
  st->mode = mode;
  { JitSerCtx sc; st->fn = jit_serialize(fn_v, sc); }
  st->items.reserve(arr->size);
  for (size_t i = 0; i < arr->size; i++) {
    JitSerCtx sc;
    st->items.push_back(jit_serialize(arr->items[i], sc));
  }
  if (arr->size == 0) {
    release_inflight_channels(st->fn);
    if (mode == culebra::PMode::Race) {
      throw culebra::CulebraError("ParallelError",
          "Parallel.race: empty array has no result", line, col);
    }
    return st->collects() ? JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(
                                culebra_runtime_array_new())}
                          : JitValue{TAG_NIL, 0};
  }
  if (st->collects()) st->results.resize(arr->size);

  if (limit < 1) limit = isolate_cap();
  size_t target = std::min(static_cast<size_t>(limit),
                           static_cast<size_t>(arr->size));
  // Workers run on threads only (the JIT multifn tables are thread_local, so an
  // inline run on the parent thread would pollute its overloads). Each thread
  // drains the shared queue; one thread still finishes the whole job.
  std::vector<culebra::SizedThread> threads;
  for (size_t w = 0; w < target; w++) {
    if (g_live_isolates().fetch_add(1, std::memory_order_relaxed) <
        isolate_cap()) {
      threads.emplace_back([st] {
        jit_parallel_worker(st);
        g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
      });
    } else {
      g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
      break;
    }
  }
  std::exception_ptr cb_err;
  if (threads.empty()) {
    // Cap exhausted: run the whole job on one thread (no live progress).
    culebra::SizedThread t([st] { jit_parallel_worker(st); });
    t.join();
  } else {
    if (on_progress.tag == TAG_FUNC) {
      jit_parallel_progress_coordinator(
          st, reinterpret_cast<JitClosure*>(on_progress.data), cb_err);
    }
    for (auto& t : threads) t.join();
  }

  release_inflight_channels(st->fn);
  for (const auto& it : st->items) release_inflight_channels(it);
  if (cb_err) std::rethrow_exception(cb_err);

  if (mode == culebra::PMode::Race) {
    if (st->race_won.load()) {
      JitDeCtx dc;
      JitValue v = jit_deserialize(st->race_result, dc);
      release_inflight_channels(st->race_result);
      return v;
    }
    throw culebra::CulebraError("ParallelError",
        std::format("Parallel.race: all {} elements failed: {}",
                    static_cast<size_t>(arr->size),
                    st->err ? st->err->what() : "unknown"),
        line, col);
  }
  if (st->failed) {
    for (const auto& rn : st->results) release_inflight_channels(rn);
    throw culebra::CulebraError("ParallelError",
        std::format("Parallel.{}: element[{}] failed: {}", who, st->err_index,
                    st->err->what()),
        line, col);
  }
  if (!st->collects()) return {TAG_NIL, 0};
  auto* out = culebra_runtime_array_new();
  for (const auto& rn : st->results) {
    JitDeCtx dc;
    JitValue v = jit_deserialize(rn, dc);
    release_inflight_channels(rn);
    culebra_runtime_array_push(out, v.tag, v.data);
  }
  return {TAG_ARRAY, reinterpret_cast<int64_t>(out)};
}

// Channel.new(cap = 1) -> (tx, rx).
inline JitValue culebra_jit_channel_new(int64_t n, JitValue* args,
                                        int64_t line, int64_t col) {
  int64_t cap = (n >= 1 && args[0].tag == TAG_LONG) ? args[0].data : 1;
  if (cap < 0) {
    throw culebra::CulebraError("ValueError",
        "Channel.new: capacity must be >= 0", line, col);
  }
  auto core = std::make_shared<ChannelCore>(static_cast<size_t>(cap));
  int64_t id = channel_next_id().fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(channel_registry_mutex());
    channel_registry()[id] = core;
  }
  core->tx_count = 1;
  core->rx_count = 1;
  auto* tup = culebra_runtime_tuple_new();
  JitValue tx = _jit_make_channel_endpoint(id, 0);
  JitValue rx = _jit_make_channel_endpoint(id, 1);
  culebra_runtime_array_push(tup, tx.tag, tx.data);  // adopts the +1
  culebra_runtime_array_push(tup, rx.tag, rx.data);
  return {TAG_TUPLE, reinterpret_cast<int64_t>(tup)};
}

}  // namespace culebra
