#pragma once

// JIT side of the isolate value transfer ([[project-concurrency-c2]], C2-c).
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
  int next_id = 0;
  std::set<const void*> visiting;          // container cycle guard
};
struct JitDeCtx {
  std::map<int, JitValue> closures;        // ref id -> rebuilt closure
};

inline sendable::SendNode jit_serialize(JitValue v, JitSerCtx& ctx) {
  using K = sendable::SendNode::K;
  sendable::SendNode n;
  auto seq = [&](JitArray* a, K kind) -> sendable::SendNode {
    if (!ctx.visiting.insert(a).second)
      sendable::send_error("a cyclic value cannot be sent");
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
        long id = o->slots[o->find_slot("__channel_id__")].value.data;
        int role = static_cast<int>(
            o->slots[o->find_slot("__channel_role__")].value.data);
        chan_bump(id, role, +1);
        n.kind = K::Channel;
        n.i = id;
        n.b = (role == 1);
        return n;
      }
      if (o->find_slot("__nonsendable__") != static_cast<size_t>(-1))
        sendable::send_error("a native handle is not Sendable");
      if (!ctx.visiting.insert(o).second)
        sendable::send_error("a cyclic value cannot be sent");
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
      // Non-string keys are rare here; defer (most data objects are string-keyed).
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
      n.ref_id = ctx.next_id++;
      ctx.closure_ids.emplace(c, n.ref_id);  // before recursing (fib recursion)
      n.jit_fn = c->fn_ptr;
      n.i = static_cast<long>(c->arity);
      // `fn name` is a dispatcher thunk: its overloads live in a thread_local
      // table, so ship each method body + its dispatch types explicitly.
      if (auto dit = _jit_multifn_dispatcher_names().find(c);
          dit != _jit_multifn_dispatcher_names().end()) {
        n.mf_name = dit->second;
        const auto& methods = _jit_multimethods()[n.mf_name];
        for (const auto& mth : methods) {
          n.mf_param_types.push_back(mth.param_types);
          n.mf_variadic.push_back(mth.variadic);
          n.elems.push_back(jit_serialize(
              JitValue{TAG_FUNC, reinterpret_cast<int64_t>(mth.body)}, ctx));
        }
        return n;
      }
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
      sendable::send_error("Tensor is not Sendable");
  }
  sendable::send_error("value is not Sendable");
}

inline JitValue jit_float(double d) {
  int64_t bits;
  std::memcpy(&bits, &d, sizeof(double));
  return {TAG_FLOAT, bits};
}

inline JitValue _jit_make_channel_endpoint(long id, int role);  // fwd

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
      auto* o = culebra_runtime_object_new();
      for (size_t k = 0; k < n.entries.size(); k++) {
        JitValue val = jit_deserialize(n.entries[k].second, ctx);
        bool mut = k < n.entry_mut.size() ? n.entry_mut[k] : true;
        o->set_or_append(n.entries[k].first.s, val, mut);
      }
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
                             reinterpret_cast<JitClosure*>(body.data),
                             n.mf_variadic[i]});
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
inline std::shared_ptr<IsolateCore> jit_isolate_lookup(long id) {
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
    JitValue fn = jit_deserialize(sclosure, dc);  // rebuild closure on child heap
    auto* cls = reinterpret_cast<JitClosure*>(fn.data);
    std::vector<JitValue> args;
    args.reserve(sargs.size());
    for (const auto& sa : sargs) args.push_back(jit_deserialize(sa, dc));
    // Rebuilt endpoints hold their own refs now; drop the in-flight ones.
    release_inflight_channels(sclosure);
    for (const auto& sa : sargs) release_inflight_channels(sa);
    JitValue r;
    if (args.empty()) r = _culebra_invoke0(cls);
    else if (args.size() == 1) r = _culebra_invoke1(cls, args[0]);
    else if (args.size() == 2) r = _culebra_invoke2(cls, args[0], args[1]);
    else r = reinterpret_cast<JitFn>(cls->fn_ptr)(cls, {0, 0},
                                                  static_cast<int64_t>(args.size()),
                                                  args.data());
    JitSerCtx sc;
    sendable::SendNode out = jit_serialize(r, sc);
    {
      std::lock_guard<std::mutex> lk(core->m);
      core->result = std::move(out);
      core->finished = true;
    }
    culebra_runtime_value_release(r.tag, r.data);
    // Release the closure so its captures drop on the child thread (the interp
    // path drops them via heap teardown). Without this a captured channel
    // endpoint never drops, so a producer that relies on auto-close — rather
    // than an explicit tx.drop() — leaves the channel open and receivers hang.
    culebra_runtime_value_release(fn.tag, fn.data);
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

inline long _jit_isolate_self_id(JitValue self) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t i = h->find_slot("_core_id");
  return i == static_cast<size_t>(-1) ? 0 : h->slots[i].value.data;
}

inline JitValue _jit_isolate_join(JitClosure*, JitValue self, int64_t, JitValue*) {
  auto core = jit_isolate_lookup(_jit_isolate_self_id(self));
  JitValue ret{TAG_NIL, 0};
  if (core) {
    {
      std::unique_lock<std::mutex> lk(core->m);
      core->cv.wait(lk, [&] { return core->finished; });
    }
    ret = _jit_isolate_extract(core);
  }
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}

inline JitValue _jit_isolate_poll(JitClosure*, JitValue self, int64_t, JitValue*) {
  auto core = jit_isolate_lookup(_jit_isolate_self_id(self));
  JitValue ret{TAG_NIL, 0};
  if (core) {
    bool done;
    { std::lock_guard<std::mutex> lk(core->m); done = core->finished; }
    if (done) ret = _jit_isolate_extract(core);
  }
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}

inline JitValue _jit_isolate_drop(JitClosure*, JitValue self, int64_t, JitValue*) {
  long id = _jit_isolate_self_id(self);
  auto core = jit_isolate_lookup(id);
  if (core && !core->joined && core->thread.joinable()) {
    core->interrupt.store(true, std::memory_order_relaxed);
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
  return {TAG_NIL, 0};
}

// A captureless native method closure (reads its state from `self`). Same
// shape as Proc's _jit_make_handle_method; kept local so this header depends
// only on jit.h (and can be included before stdlib_jit.h's helpers).
inline JitClosure* _jit_native_method(
    JitValue (*fn)(JitClosure*, JitValue, int64_t, JitValue*)) {
  auto* c = new JitClosure();
  c->refcount = 1;
  c->fn_ptr = reinterpret_cast<void*>(fn);
  c->n_captures = 0;
  c->captures = nullptr;
  c->arity = 0;
  _gc_register(c, GC_TAG_FUNC);
  return c;
}

inline JitValue _jit_build_isolate_handle(long id) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("_core_id", JitValue{TAG_LONG, id}, true);
  h->set_or_append("__nonsendable__", JitValue{TAG_BOOL, 1}, false);
  auto m = [&](const char* name,
               JitValue (*f)(JitClosure*, JitValue, int64_t, JitValue*)) {
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
  sendable::SendNode sclo = jit_serialize({fn_tag, fn_data}, sc);
  std::vector<sendable::SendNode> sargs;
  sargs.reserve(n_args);
  for (int64_t i = 0; i < n_args; i++) sargs.push_back(jit_serialize(args[i], sc));

  auto core = std::make_shared<IsolateCore>();
  long id = jit_isolate_next_id().fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(jit_isolate_reg_mutex());
    jit_isolate_reg()[id] = core;
  }
  bool threaded =
      g_live_isolates().fetch_add(1, std::memory_order_relaxed) < isolate_cap();
  if (!threaded) g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
  core->thread = std::thread([core, sclo = std::move(sclo),
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

inline long _jit_self_long(JitValue self, const char* key) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t i = h->find_slot(key);
  return i == static_cast<size_t>(-1) ? 0 : h->slots[i].value.data;
}

inline JitValue _jit_chan_send(JitClosure*, JitValue self, int64_t n,
                               JitValue* args) {
  long id = _jit_self_long(self, "__channel_id__");
  if (n >= 1) {
    JitSerCtx sc;
    channel_send_node(id, jit_serialize(args[0], sc));  // Sendable check here
  }
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_NIL, 0};
}

inline JitValue _jit_chan_recv(JitClosure*, JitValue self, int64_t, JitValue*) {
  long id = _jit_self_long(self, "__channel_id__");
  auto node = chan_pop_blocking(id);
  JitValue ret{TAG_NIL, 0};
  if (node) {
    JitDeCtx dc;
    ret = jit_deserialize(*node, dc);
    release_inflight_channels(*node);
  }
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}

inline JitValue _jit_chan_drop(JitClosure*, JitValue self, int64_t, JitValue*) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t di = h->find_slot("_dropped");
  if (di != static_cast<size_t>(-1) && h->slots[di].value.tag == TAG_BOOL &&
      h->slots[di].value.data) {
    return {TAG_NIL, 0};  // already dropped (explicit + GC are idempotent)
  }
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 1}, true);
  chan_drop(_jit_self_long(self, "__channel_id__"),
            static_cast<int>(_jit_self_long(self, "__channel_role__")));
  return {TAG_NIL, 0};
}

inline JitValue _jit_chan_clone(JitClosure*, JitValue self, int64_t, JitValue*) {
  long id = _jit_self_long(self, "__channel_id__");
  int role = static_cast<int>(_jit_self_long(self, "__channel_role__"));
  chan_bump(id, role, +1);
  JitValue ret = _jit_make_channel_endpoint(id, role);
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}

// rx for-in iterator: has_next pulls one value (blocking, ends on close), next
// hands over the cached lookahead. State lives in the iterator's own slots.
inline JitValue _jit_chan_iter_self(JitClosure*, JitValue self, int64_t,
                                    JitValue*) {
  culebra_runtime_value_retain(self.tag, self.data);
  return self;
}
inline JitValue _jit_chan_iter_has_next(JitClosure*, JitValue self, int64_t,
                                        JitValue*) {
  auto* it = reinterpret_cast<JitObject*>(self.data);
  long st = _jit_self_long(self, "_st");
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
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}
inline JitValue _jit_chan_iter_next(JitClosure*, JitValue self, int64_t,
                                    JitValue*) {
  auto* it = reinterpret_cast<JitObject*>(self.data);
  JitValue ret{TAG_NIL, 0};
  if (_jit_self_long(self, "_st") == 1) {
    ret = it->slots[it->find_slot("_lv")].value;  // transfer slot's +1 to caller
    it->set_or_append("_lv", JitValue{TAG_NIL, 0}, true);  // overwrite, no release
    it->set_or_append("_st", JitValue{TAG_LONG, 0}, true);
  }
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}
inline JitValue _jit_chan_iter(JitClosure*, JitValue self, int64_t, JitValue*) {
  long id = _jit_self_long(self, "__channel_id__");
  auto* it = culebra_runtime_object_new();
  it->set_or_append("_cid", JitValue{TAG_LONG, id}, true);
  it->set_or_append("_st", JitValue{TAG_LONG, 0}, true);
  it->set_or_append("_lv", JitValue{TAG_NIL, 0}, true);
  auto meth = [&](const char* nm,
                  JitValue (*f)(JitClosure*, JitValue, int64_t, JitValue*)) {
    it->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("iter", _jit_chan_iter_self);
  meth("has_next", _jit_chan_iter_has_next);
  meth("next", _jit_chan_iter_next);
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(it)};
}

inline JitValue _jit_make_channel_endpoint(long id, int role) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__channel_endpoint__", JitValue{TAG_BOOL, 1}, false);
  h->set_or_append("__channel_id__", JitValue{TAG_LONG, id}, false);
  h->set_or_append("__channel_role__", JitValue{TAG_LONG, role}, false);
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 0}, true);
  auto meth = [&](const char* nm,
                  JitValue (*f)(JitClosure*, JitValue, int64_t, JitValue*)) {
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

// JIT merged rx (Channel.fan_in). Same JitObject endpoint shape as a channel rx,
// but reaches several sources via chan_select_recv (the backend-neutral core).
// The iterator's iter_self/next slots are generic, so only has_next differs.
inline JitValue _jit_merged_recv(JitClosure*, JitValue self, int64_t, JitValue*) {
  long mid = _jit_self_long(self, "__merged_id__");
  auto node = chan_select_recv(mid);
  JitValue ret{TAG_NIL, 0};
  if (node) {
    JitDeCtx dc;
    ret = jit_deserialize(*node, dc);
    release_inflight_channels(*node);
  }
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}
inline JitValue _jit_merged_drop(JitClosure*, JitValue self, int64_t, JitValue*) {
  auto* h = reinterpret_cast<JitObject*>(self.data);
  size_t di = h->find_slot("_dropped");
  if (di != static_cast<size_t>(-1) && h->slots[di].value.tag == TAG_BOOL &&
      h->slots[di].value.data) {
    return {TAG_NIL, 0};
  }
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 1}, true);
  chan_merge_drop(_jit_self_long(self, "__merged_id__"));  // idempotent anyway
  return {TAG_NIL, 0};
}
inline JitValue _jit_merged_join(JitClosure*, JitValue self, int64_t, JitValue*) {
  long mid = _jit_self_long(self, "__merged_id__");
  culebra_runtime_value_release(self.tag, self.data);
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
  return {TAG_NIL, 0};
}
inline JitValue _jit_merged_iter_has_next(JitClosure*, JitValue self, int64_t,
                                          JitValue*) {
  auto* it = reinterpret_cast<JitObject*>(self.data);
  long st = _jit_self_long(self, "_st");
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
  culebra_runtime_value_release(self.tag, self.data);
  return ret;
}
inline JitValue _jit_merged_iter(JitClosure*, JitValue self, int64_t, JitValue*) {
  long mid = _jit_self_long(self, "__merged_id__");
  auto* it = culebra_runtime_object_new();
  it->set_or_append("_mid", JitValue{TAG_LONG, mid}, true);
  it->set_or_append("_st", JitValue{TAG_LONG, 0}, true);
  it->set_or_append("_lv", JitValue{TAG_NIL, 0}, true);
  auto meth = [&](const char* nm,
                  JitValue (*f)(JitClosure*, JitValue, int64_t, JitValue*)) {
    it->set_or_append(nm,
        JitValue{TAG_FUNC, reinterpret_cast<int64_t>(_jit_native_method(f))},
        false);
  };
  meth("iter", _jit_chan_iter_self);          // reuse: returns self
  meth("has_next", _jit_merged_iter_has_next);
  meth("next", _jit_chan_iter_next);          // reuse: hands over _lv
  culebra_runtime_value_release(self.tag, self.data);
  return {TAG_OBJECT, reinterpret_cast<int64_t>(it)};
}
inline JitValue _jit_make_merged_rx_endpoint(long mid) {
  auto* h = culebra_runtime_object_new();
  h->set_or_append("__merged_rx__", JitValue{TAG_BOOL, 1}, false);
  h->set_or_append("__merged_id__", JitValue{TAG_LONG, mid}, false);
  h->set_or_append("_dropped", JitValue{TAG_BOOL, 0}, true);
  auto meth = [&](const char* nm,
                  JitValue (*f)(JitClosure*, JitValue, int64_t, JitValue*)) {
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
    std::vector<long> ids;
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
      long id = o->slots[o->find_slot("__channel_id__")].value.data;
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
  std::vector<long> ids;
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
    pcore->thread = std::thread(
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
// build Value; these build JitValue). `r` is owned and released here.
inline void jit_parallel_record_success(ParallelState& st, size_t i, JitValue r) {
  switch (st.mode) {
    case culebra::PMode::Map: {
      JitSerCtx sc;
      st.results[i] = jit_serialize(r, sc);
      break;
    }
    case culebra::PMode::MapSettled: {
      JitValue o = _jit_settled_result(true, r, nullptr);  // owns r
      JitSerCtx sc;
      st.results[i] = jit_serialize(o, sc);
      culebra_runtime_value_release(o.tag, o.data);  // frees o and r
      return;
    }
    case culebra::PMode::Each:
      break;
    case culebra::PMode::Race:
      if (!st.race_won.exchange(true)) {
        std::lock_guard<std::mutex> lk(st.err_m);
        JitSerCtx sc;
        st.race_result = jit_serialize(r, sc);
        st.interrupt.store(true, std::memory_order_relaxed);
      }
      break;
  }
  culebra_runtime_value_release(r.tag, r.data);
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
    JitValue fn = jit_deserialize(st->fn, dc);
    auto* cls = reinterpret_cast<JitClosure*>(fn.data);
    while (!st->interrupt.load(std::memory_order_relaxed)) {
      size_t i = st->next.fetch_add(1, std::memory_order_relaxed);
      if (i >= st->items.size()) break;
      try {
        JitDeCtx idc;
        JitValue item = jit_deserialize(st->items[i], idc);
        JitValue r = _culebra_invoke1(cls, item);
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

inline JitValue jit_parallel_run(JitValue items_v, JitValue fn_v, long limit,
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
  std::vector<std::thread> threads;
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
    std::thread t([st] { jit_parallel_worker(st); });
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
  long cap = (n >= 1 && args[0].tag == TAG_LONG) ? args[0].data : 1;
  if (cap < 0) {
    throw culebra::CulebraError("ValueError",
        "Channel.new: capacity must be >= 0", line, col);
  }
  auto core = std::make_shared<ChannelCore>(static_cast<size_t>(cap));
  long id = channel_next_id().fetch_add(1, std::memory_order_relaxed);
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
