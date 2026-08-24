#pragma once

// Isolate: CPU-parallel execution units, each on its own OS thread with its
// own GC heap (milestone C2-a, interpreter only).
//
//   let h = Isolate.spawn(|| fib(40))      # run a closure on a new thread
//   let h = Isolate.spawn(|n| fib(n), 40)  # with positional args
//   let r = h.join()                       # collect the (Sendable-copied) result
//
// A spawned closure and its arguments must be Sendable (sendable.h); a
// violation throws SendError at the spawn site (never a silent copy). Values
// cross the thread boundary by serialization — the parent and child heaps
// never share a pointer.
//
// Parallelism is capped (default: hardware_concurrency). Spawns beyond the cap
// run INLINE on the current thread (a synchronous fallback that prevents thread
// explosion under recursive parallelism). A Sendable closure run sequentially
// needs no heap isolation, so the inline path runs the original closure
// directly on the parent heap — equivalent and cheaper than a copy round-trip.
//
// Not in this cycle (next): Channel(tx/rx), an interrupt flag (so `drop` can
// cancel a still-running isolate — for now `drop` joins best-effort), Parallel
// .map, JIT support, stdout line-locking.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "packable.h"  // SharedBufferCore lifecycle (shared_buffer_drop)
#include "sendable.h"
#include "sharedval.h"
#include "isolate_core.h"  // the shared isolate/channel core (B7-b)

namespace culebra {

// Deserialize a transferred node on the current heap and release its
// in-flight channel refs (defined below with the interp channel adapters).
// Declared early: the isolate child / inline paths consume nodes first.
inline Value chan_take(const sendable::SendNode& n);


// Child-thread entry point: fresh Runtime (→ own GC heap) + fresh stdlib env,
// rebuild the closure, run it, serialize the result back. Runs every value
// through the heap-free SendNode form so the two heaps never share a pointer.
inline void run_isolate_child(std::shared_ptr<IsolateCore> core,
                              const sendable::SendNode& sclosure,
                              const std::vector<sendable::SendNode>& sargs) {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  rt.interrupt_flag = &core->interrupt;  // for the blocking channel waits
  auto child = std::make_shared<Interpreter>();
  child->interrupt_flag_ = &core->interrupt;  // for the statement-dispatch poll
  auto base = sendable::isolate_base_env();
  try {
    sendable::DeCtx dc;
    Value fn = sendable::deserialize(sclosure, *child, base, dc);
    std::vector<Value> args;
    args.reserve(sargs.size());
    for (const auto& sa : sargs)
      args.push_back(sendable::deserialize(sa, *child, base, dc));
    // The rebuilt endpoints now hold their own refs; release the in-flight ones.
    release_inflight_channels(sclosure);
    for (const auto& sa : sargs) release_inflight_channels(sa);
    Value r = child->call_closure(fn, base, std::move(args));
    sendable::SerCtx sc;
    sendable::SendNode out = sendable::serialize(r, sc);
    {
      std::lock_guard<std::mutex> lk(core->m);
      core->result = std::move(out);
      core->finished = true;
    }
  } catch (Value& thrown) {  // user `throw`
    std::lock_guard<std::mutex> lk(core->m);
    try {
      sendable::SerCtx sc;
      core->thrown = sendable::serialize(thrown, sc);
    } catch (...) {
      core->error = culebra::CulebraError(
          "RuntimeError",
          "uncaught throw in isolate (value not Sendable): " + thrown.str());
    }
    core->finished = true;
  } catch (culebra::CulebraError& e) {
    std::lock_guard<std::mutex> lk(core->m);
    core->error = e;
    core->finished = true;
  } catch (std::exception& e) {
    std::lock_guard<std::mutex> lk(core->m);
    core->error = culebra::CulebraError("RuntimeError", e.what());
    core->finished = true;
  }
  core->cv.notify_all();
  g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
  // base / child / scope / rt tear down here (on the child thread): the child
  // heap is collected after the result has already been serialized out.
}

// Reconstruct a neutral value on the PARENT heap. Builds a stdlib base env only
// when the payload carries a closure (so its body's builtins resolve).
inline Value _isolate_deser_parent(const sendable::SendNode& n) {
  // make_shared (not a stack Interpreter): deserialize may rebuild a closure
  // via make_function_value → shared_from_this(), which throws bad_weak_ptr on
  // a non-shared_ptr-owned Interpreter (e.g. an isolate returning a closure).
  auto ti = std::make_shared<Interpreter>();
  std::shared_ptr<Environment> base =
      sendable::contains_closure(n) ? sendable::isolate_base_env() : nullptr;
  sendable::DeCtx dc;
  return sendable::deserialize(n, *ti, base, dc);
}


// Join the thread (once) and surface the outcome: re-throw a crossed error /
// user throw, or return the result. Precondition: core.finished is true.
inline Value _isolate_extract(IsolateCore& core) {
  if (!core.joined && core.thread.joinable()) {
    core.thread.join();
    core.joined = true;
  }
  if (core.error) throw *core.error;
  if (core.inline_thrown)
    throw *std::static_pointer_cast<Value>(core.inline_thrown);
  if (core.thrown) throw chan_take(*core.thrown);
  if (core.ran_inline) {
    return core.inline_result
               ? *std::static_pointer_cast<Value>(core.inline_result)
               : Value();
  }
  return chan_take(core.result);
}


struct IsolateCore;  // fwd (defined below; held by a merge entry for fan_in(fn))
inline Value _isolate_extract(IsolateCore& core);  // fwd


inline void chan_merge_join(int64_t mid) {
  std::vector<std::shared_ptr<IsolateCore>> producers = chan_merge_producers(mid);
  std::optional<culebra::CulebraError> first_err;
  std::optional<Value> first_thrown;
  for (auto& core : producers) {
    try {
      _isolate_extract(*core);  // joins; rethrows the producer's error
    } catch (culebra::CulebraError& e) {
      if (!first_err && !first_thrown) first_err = e;
    } catch (Value& v) {
      if (!first_err && !first_thrown) first_thrown = v;
    }
  }
  if (first_thrown) throw *first_thrown;
  if (first_err) throw *first_err;
}

// --- Interp teardown safety net -------------------------------------------
//
// A standalone Isolate.spawn handle is only reachable through the script's
// own Value (its join/poll/drop closures each capture a shared_ptr<IsolateCore>)
// plus the spawned thread's own capture — nothing process-wide keeps it
// reachable. Normally that's fine: h.join()/h.drop() (or the handle simply
// going out of scope, reaped by IsolateCore's destructor safety net) joins
// the OS thread promptly. But a top-level uncaught throw unwinds straight
// past an unreached join(), and by the time the handle's refcount actually
// drops to zero the process may already be past interpret_modules, into
// main()'s static-destruction teardown — including process-wide statics an
// isolate's own send/recv still touches (channel_registry, merge_registry).
// A thread racing that teardown is a use-after-free, not merely a hang.
//
// This registry exists purely so _interp_isolate_teardown_join_all (called
// once, right after a top-level script run ends — interpreter.h) can find
// and join whatever standalone spawn is still outstanding at that point.
// It holds weak_ptrs, not owning refs: unlike jit_isolate_reg (sendable_jit.h,
// the sole owner behind a captureless JIT handle's integer id), an interp
// handle already owns the strong ref, so a weak entry here just rides along
// without changing when a handle that IS reaped promptly (the common case)
// actually frees its isolate — it only gives teardown a way to reach the
// stragglers that outlive their handle.
inline std::mutex& interp_isolate_reg_mutex() { static std::mutex m; return m; }
inline std::vector<std::weak_ptr<IsolateCore>>& interp_isolate_reg() {
  static std::vector<std::weak_ptr<IsolateCore>> r;
  return r;
}
// Spawning is already the heavy operation (a new OS thread), so prune expired
// entries here rather than only at teardown — otherwise a long-lived process
// that never reaches teardown (a REPL session, or a single script's batch
// loop spawning many isolates before interpret_modules returns) grows this
// vector by one entry per spawn for the life of the process, even though
// every one of them is promptly joined and freed elsewhere.
inline void interp_isolate_reg_add(const std::shared_ptr<IsolateCore>& core) {
  std::lock_guard<std::mutex> lk(interp_isolate_reg_mutex());
  auto& reg = interp_isolate_reg();
  reg.erase(std::remove_if(reg.begin(), reg.end(),
                            [](const std::weak_ptr<IsolateCore>& w) {
                              return w.expired();
                            }),
            reg.end());
  reg.push_back(core);
}


// Cancel + join every interp isolate still outstanding: a standalone
// Isolate.spawn (interp_isolate_reg, weak — collect the survivors) or a
// Channel.fan_in producer (collect_live_merge_producers above). A no-op once
// everything has already been joined (h.join()/h.drop() ran, or the handle's
// own scope exit reaped it), which is the common case. Installed into
// interp_isolate_teardown_join_hook (shared.h) below; interpreter.h calls the
// hook once a top-level script run ends, mirroring jit.h's teardown guard
// around JIT::exec.
inline void _interp_isolate_teardown_join_all() {
  std::vector<std::shared_ptr<IsolateCore>> live;
  {
    std::lock_guard<std::mutex> lk(interp_isolate_reg_mutex());
    auto& reg = interp_isolate_reg();
    for (auto& w : reg)
      if (auto core = w.lock())
        if (!core->joined) live.push_back(std::move(core));
    reg.clear();  // prune both the expired and the ones we're about to join
  }
  collect_live_merge_producers(live);
  cancel_and_join_isolates(std::move(live));
}
inline bool _install_interp_isolate_teardown_hook() {
  interp_isolate_teardown_join_hook() = _interp_isolate_teardown_join_all;
  return true;
}
inline const bool _interp_isolate_teardown_hook_installed =
    _install_interp_isolate_teardown_hook();

inline Value make_channel_endpoint(int64_t id, int role);  // fwd (clone recurses)




inline void channel_send(int64_t id, const Value& v) {
  sendable::SerCtx sc;
  channel_send_node(id, sendable::serialize(v, sc));  // Sendable check here
}


// Deserialize a value popped from a channel and release its in-flight channel
// refs (a value carrying an endpoint was extract-bumped at send time).
inline Value chan_take(const sendable::SendNode& node) {
  Value v = _isolate_deser_parent(node);
  release_inflight_channels(node);
  return v;
}

// Block for one value. Returns nil when the channel is closed and drained
// (for a clean end-of-stream, prefer `for x in rx`, which ends instead).
inline Value channel_recv(int64_t id) {
  auto node = chan_pop_blocking(id);
  return node ? chan_take(*node) : Value();
}


// `try_recv` answers with a ChannelResult variant: `Value(v)`, `Empty`, or
// `Closed`. The instance is hand-tagged with the same `class` / `__enum`
// pair eval_enum_decl writes, so `match` / `inspect` treat it exactly like a
// declared enum's variant — no binding for the name exists (nor is one
// needed: a pattern compares the tag, never resolving the name).
//
// Built fresh per call: a cached singleton would be unreachable from any
// environment root, so a collection could clear it out from under a later
// caller. The allocation is noise next to the mutex already taken above.
inline ObjectValue _make_channel_result(ChanTryPopStatus status) {
  ObjectValue inst;
  inst.properties->emplace(
      "class", Symbol{Value(std::string(chan_status_variant(status))), true});
  inst.properties->emplace("__enum",
                           Symbol{Value(std::string("ChannelResult")), true});
  return inst;
}

inline Value channel_try_recv(int64_t id) {
  auto r = chan_pop_nonblocking(id);
  ObjectValue inst = _make_channel_result(r.status);
  if (r.status == ChanTryPopStatus::Value) {
    inst.properties->emplace(std::string(positional_field_name(0)),
                             Symbol{chan_take(*r.node), true});
  }
  return Value(std::move(inst));
}


inline size_t chan_drain_max(const Value& v, int64_t line, int64_t col) {
  bool is_count = v.type == Value::Long;
  return chan_drain_max(v.type == Value::Nil ? DrainMaxKind::Nil
                        : is_count           ? DrainMaxKind::Count
                                             : DrainMaxKind::Other,
                        is_count ? v.to_long() : 0, line, col);
}

// Take what is queued right now, without blocking — at most `max` of it.
inline Value channel_drain(int64_t id, size_t max) {
  ArrayValue out;
  if (auto core = chan_lookup(id)) {
    auto taken = chan_take_queued(*core, max);
    out.values->reserve(taken.size());
    for (const auto& node : taken) out.values->push_back(chan_take(node));
  }
  return Value(std::move(out));
}


inline Value channel_iter(int64_t id) {
  return _make_iterator(
      [id](std::shared_ptr<Environment>) -> std::optional<Value> {
        auto node = chan_pop_blocking(id);
        return node ? _iter_step_value(chan_take(*node))
                    : _iter_step_done();  // closed + empty → for-in ends
      });
}

// A shared-resource handle (channel endpoint / SharedBuffer) can be dropped
// both explicitly (`x.drop()`) and again by the GC at heap teardown, so its
// refcount must move exactly once. Returns true if `fn` was already dropped;
// otherwise marks it dropped (via the `_dropped` flag on the shared property
// map both call paths see) and returns false.
inline bool _handle_drop_consumed(const Value& self) {
  auto& props = *self.to_object().properties;
  if (auto it = props.find("_dropped");
      it != props.end() && it->second.val.type == Value::Bool &&
      it->second.val.to_bool()) {
    return true;
  }
  props.insert_or_assign("_dropped", Symbol{Value(true), true});
  return false;
}

inline Value _chan_drop_once(const Value& self, int64_t id, int role) {
  if (!_handle_drop_consumed(self)) chan_drop(id, role);
  return Value();
}

inline Value make_channel_endpoint(int64_t id, int role) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("__channel_endpoint__", Value(true), false);
  h.initialize("__channel_id__", Value(id), false);
  h.initialize("__channel_role__", Value(static_cast<int64_t>(role)), false);
  h.initialize("_dropped", Value(false), true);
  h.initialize("drop",
      Value(FunctionValue({}, [id, role](std::shared_ptr<Environment> env) -> Value {
        return _chan_drop_once(env->get("self"), id, role);
      }, "Nil"sv)), false);
  h.initialize("clone",
      Value(FunctionValue({}, [id, role](std::shared_ptr<Environment>) -> Value {
        chan_bump(id, role, +1);
        return make_channel_endpoint(id, role);
      }, ""sv)), false);
  if (role == 0) {  // tx
    h.initialize("send",
        Value(FunctionValue({{"v", false, ""sv}},
            [id](std::shared_ptr<Environment> env) -> Value {
              channel_send(id, env->get("v"));
              return Value();
            }, "Nil"sv)), false);
  } else {  // rx
    h.initialize("recv",
        Value(FunctionValue({}, [id](std::shared_ptr<Environment>) -> Value {
          return channel_recv(id);
        }, ""sv)), false);
    h.initialize("iter",
        Value(FunctionValue({}, [id](std::shared_ptr<Environment>) -> Value {
          return channel_iter(id);
        }, ""sv)), false);
    h.initialize("try_recv",
        Value(FunctionValue({}, [id](std::shared_ptr<Environment>) -> Value {
          return channel_try_recv(id);
        }, ""sv)), false);
    h.initialize("drain",
        Value(FunctionValue({{"max", false, ""sv, nullptr, kw_default_nil()}},
            [id](std::shared_ptr<Environment> env) -> Value {
              int64_t line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
              int64_t col = env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
              return channel_drain(id, chan_drain_max(env->get("max"), line, col));
            }, "Array"sv)), false);
  }
  return Value(std::move(h));
}

// A read-only endpoint that merges several source rx channels (Channel.fan_in).
// recv/iter pull from whichever source is ready (chan_select_recv); it ends once
// every source is closed. fan_in took one rx clone per source, dropped by
// chan_merge_drop (idempotent, so explicit drop + GC teardown both run cleanly).
inline Value make_merged_rx_endpoint(int64_t mid) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("__merged_rx__", Value(true), false);
  h.initialize("__merged_id__", Value(mid), false);
  h.initialize("recv",
      Value(FunctionValue({}, [mid](std::shared_ptr<Environment>) -> Value {
        auto node = chan_select_recv(mid);
        return node ? chan_take(*node) : Value();
      }, ""sv)), false);
  h.initialize("iter",
      Value(FunctionValue({}, [mid](std::shared_ptr<Environment>) -> Value {
        return _make_iterator(
            [mid](std::shared_ptr<Environment>) -> std::optional<Value> {
              auto node = chan_select_recv(mid);
              return node ? _iter_step_value(chan_take(*node))
                          : _iter_step_done();
            });
      }, ""sv)), false);
  h.initialize("drop",
      Value(FunctionValue({}, [mid](std::shared_ptr<Environment>) -> Value {
        chan_merge_drop(mid);  // idempotent (registry erase)
        return Value();
      }, "Nil"sv)), false);
  // For the fan_in(items, fn) form: join the spawned producers and surface the
  // first error. A no-op for fan_in([rx]) (no producers).
  h.initialize("join",
      Value(FunctionValue({}, [mid](std::shared_ptr<Environment>) -> Value {
        chan_merge_join(mid);
        return Value();
      }, "Nil"sv)), false);
  return Value(std::move(h));
}

// --- SharedBuffer handle lifecycle (mirrors the channel endpoint) ---------
inline Value _shared_buffer_drop_once(const Value& self, int64_t id) {
  if (!_handle_drop_consumed(self)) culebra::shared_buffer_drop(id);
  return Value();
}

// Build a SharedBuffer handle: hidden id/count markers plus a GC-driven `drop`
// that releases the buffer's ref (freeing it at the last handle).
inline Value make_shared_buffer_handle(int64_t id, int64_t count) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("__sharedbuffer_id__", Value(id), false);
  h.initialize("__sharedbuffer_count__", Value(count), false);
  h.initialize("_dropped", Value(false), true);
  h.initialize("drop",
      Value(FunctionValue({}, [id](std::shared_ptr<Environment> env) -> Value {
        return _shared_buffer_drop_once(env->get("self"), id);
      }, "Nil"sv)), false);
  // `with_lock(fn)` runs `fn()` while holding the buffer's lock (a std::mutex
  // for heap, a PROCESS_SHARED pthread mutex in the mapped header for
  // file/shared), then returns its value. The lock is released on any exit,
  // including a throw. The escape hatch for multi-field consistent updates;
  // disjoint writes need no lock.
  h.initialize("with_lock",
      Value(FunctionValue({{"fn", false, ""sv}},
          [id](std::shared_ptr<Environment> env) -> Value {
            Value fnv = env->get("fn");
            if (fnv.type != Value::Function) {
              throw culebra::CulebraError(
                  "TypeError", "SharedBuffer.with_lock expects a function");
            }
            auto core = culebra::lookup_shared_buffer(id);
            if (!core) {
              throw culebra::CulebraError("ValueError",
                  "SharedBuffer has been dropped");
            }
            culebra::SharedBufferLockGuard guard(std::move(core));
            return _invoke_callback(fnv);
          }, ""sv)), false);
  // File-backed buffers get `flush()` (msync dirty pages to disk). Heap/Shared
  // have no such method — the data interface is otherwise identical.
  auto core = culebra::lookup_shared_buffer(id);
  if (core && core->storage == culebra::SharedBufferCore::Storage::File) {
    h.initialize("flush",
        Value(FunctionValue({}, [id](std::shared_ptr<Environment>) -> Value {
          culebra::shared_buffer_flush(id);
          return Value();
        }, "Nil"sv)), false);
  }
  return Value(std::move(h));
}

// Register the SharedBuffer Sendable hooks (handles ship by id) once at load.
inline bool _install_shared_buffer_hooks() {
  sendable::sharedbuffer_extract_hook() = [](const Value& v) -> int64_t {
    int64_t id = v.to_object().get("__sharedbuffer_id__").to_long();
    if (!sendable::sharedval_freezing())
      culebra::shared_buffer_bump(id, +1);  // in-flight ref (skip during freeze)
    return id;
  };
  sendable::sharedbuffer_rebuild_hook() = [](int64_t id) -> Value {
    culebra::shared_buffer_bump(id, +1);  // the rebuilt handle's own ref
    auto core = culebra::lookup_shared_buffer(id);
    return make_shared_buffer_handle(
        id, core ? static_cast<int64_t>(core->count) : 0);
  };
  return true;
}
inline const bool _shared_buffer_hooks_installed = _install_shared_buffer_hooks();

// Register the Embed.dir Sendable hook once at load. The handle shares no
// state, so only the rebuild side exists: the receiving isolate builds a fresh
// handle over the same directory name (baked table or live disk, same choice).
inline bool _install_embed_dir_hook() {
  sendable::embed_dir_rebuild_hook() = [](const std::string& name) -> Value {
    return make_embed_dir_handle(name);
  };
  return true;
}
inline const bool _embed_dir_hook_installed = _install_embed_dir_hook();

// Register the Sendable hooks (endpoints ship by id + role) once at load.
inline bool _install_channel_hooks() {
  sendable::channel_extract_hook() =
      [](const Value& v) -> sendable::ChannelRef {
    const auto& o = v.to_object();
    int64_t id = o.get("__channel_id__").to_long();
    int role = static_cast<int>(o.get("__channel_role__").to_long());
    // In-flight ref: keeps the channel open across the async gap between
    // serialize and rebuild (so a sender dropping its endpoint right after
    // spawn can't auto-close before the child rebuilds). Released exactly once
    // by release_inflight_channels after the node is consumed.
    if (!sendable::sharedval_freezing())
      chan_bump(id, role, +1);  // skip during a Shared.new freeze
    return {id, role};
  };
  sendable::channel_rebuild_hook() =
      [](const sendable::ChannelRef& r) -> Value {
    chan_bump(r.id, r.role, +1);  // this rebuilt endpoint's own ref (its drop -1)
    return make_channel_endpoint(r.id, r.role);
  };
  return true;
}
inline const bool _channel_hooks_installed = _install_channel_hooks();

// `Channel.new(cap = 1) -> (tx, rx)`. Bounded; `cap = 0` is rendezvous.
inline Value make_channel_namespace() {
  using namespace std::literals;
  static const auto cap_default = std::make_shared<Value>(Value(int64_t{1}));
  ObjectValue ns;
  ns.initialize("new",
      Value(FunctionValue({{"cap", false, ""sv, nullptr, cap_default}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t cap = env->get("cap").to_long();
            int64_t line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col = env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
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
            std::vector<Value> pair;
            pair.push_back(make_channel_endpoint(id, 0));  // tx
            pair.push_back(make_channel_endpoint(id, 1));  // rx
            return Value(TupleValue(std::move(pair)));
          }, "Tuple"sv)), false);
  // `Channel.fan_in(sources: [rx]) -> rx`. Merge several receivers into one. The
  // result yields values from whichever source is ready, ending once all sources
  // close — so N producers can each own their own channel (no shared `tx.clone()`
  // and no drop bookkeeping). Takes one rx clone per source; reading the original
  // receivers afterward races the merge, so hand them over and don't reuse them.
  ns.initialize("fan_in",
      Value(FunctionValue(
          {{"a", false, ""sv}, {"fn", false, ""sv, nullptr, kw_default_nil()}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col = env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            Value a = env->get("a");
            const Value& fnv = env->get("fn");
            if (a.type != Value::Array) {
              throw culebra::CulebraError("TypeError",
                  "Channel.fan_in: first argument must be an Array", line, col);
            }

            if (fnv.type != Value::Function) {
              // fan_in([rx, ...]): merge existing receivers.
              std::vector<int64_t> ids;
              for (const auto& s : *a.to_array().values) {
                if (s.type != Value::Object ||
                    !s.to_object().has("__channel_endpoint__") ||
                    s.to_object().get("__channel_role__").to_long() != 1) {
                  throw culebra::CulebraError("TypeError",
                      "Channel.fan_in: every source must be a receiver (rx)",
                      line, col);
                }
                int64_t id = s.to_object().get("__channel_id__").to_long();
                chan_bump(id, /*role=*/1, +1);  // the merged endpoint's own rx
                ids.push_back(id);
              }
              return make_merged_rx_endpoint(chan_merge_register(std::move(ids)));
            }

            // fan_in(items, fn): spawn one producer per item and merge their
            // streams. `fn(item, tx)` sends to its own channel; fan_in drops the
            // parent's tx and the producer's auto-drops on exit, so the consumer
            // sees no tx and no drop bookkeeping at all. Producers run threaded
            // (a streaming producer blocks on send, so the inline-over-cap
            // fallback would deadlock).
            const auto& items = *a.to_array().values;
            sendable::SerCtx fsc;
            sendable::SendNode sfn;
            try {
              sfn = sendable::serialize(fnv, fsc);  // Sendable check (once)
            } catch (culebra::CulebraError& e) {
              if (e.kind == "SendError" && e.line == 0)
                throw culebra::CulebraError(
                    "SendError", std::string("Channel.fan_in: ") + e.what(),
                    line, col);
              throw;
            }
            std::vector<int64_t> ids;
            std::vector<std::shared_ptr<IsolateCore>> producers;
            for (const auto& item : items) {
              auto chcore = std::make_shared<ChannelCore>(1);
              long cid = channel_next_id().fetch_add(1, std::memory_order_relaxed);
              {
                std::lock_guard<std::mutex> lk(channel_registry_mutex());
                channel_registry()[cid] = chcore;
              }
              chcore->tx_count = 0;  // no parent tx (only the producer holds one)
              chcore->rx_count = 1;  // the merged endpoint's rx
              // args = [item, tx]. The tx crosses as an in-flight Channel node;
              // the child rebuilds it (+1) and run_isolate_child releases the
              // in-flight ref, leaving the producer as the sole tx.
              sendable::SerCtx asc;
              std::vector<sendable::SendNode> sargs;
              try {
                sargs.push_back(sendable::serialize(item, asc));
              } catch (culebra::CulebraError& e) {
                if (e.kind == "SendError" && e.line == 0)
                  throw culebra::CulebraError(
                      "SendError", std::string("Channel.fan_in: ") + e.what(),
                      line, col);
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
                    run_isolate_child(pcore, sfn, sargs);
                  });
              producers.push_back(std::move(pcore));
              ids.push_back(cid);
            }
            return make_merged_rx_endpoint(
                chan_merge_register(std::move(ids), std::move(producers)));
          }, ""sv)), false);
  return Value(std::move(ns));
}

// `Signal.notify(tx)` / `Signal.reset()` — Go's signal.Notify graceful-shutdown
// model. notify registers a channel tx endpoint to receive "SIGINT" on Ctrl+C
// (suppressing the throw); reset restores default behavior. The machinery is in
// signal_notify_register / signal_notify_reset above.
inline Value make_signal_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize("notify",
      Value(FunctionValue({{"tx", false, ""sv}},
          [](std::shared_ptr<Environment> env) -> Value {
            int64_t line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col = env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            const Value& tx = env->get("tx");
            if (tx.type != Value::Object ||
                !tx.to_object().has("__channel_endpoint__") ||
                tx.to_object().get("__channel_role__").to_long() != 0) {
              throw culebra::CulebraError("TypeError",
                  "Signal.notify: argument must be a channel tx endpoint", line,
                  col);
            }
            signal_notify_register(tx.to_object().get("__channel_id__").to_long());
            return Value();
          }, "Nil"sv)), false);
  ns.initialize("reset",
      Value(FunctionValue({}, [](std::shared_ptr<Environment>) -> Value {
            signal_notify_reset();
            return Value();
          }, "Nil"sv)), false);
  return Value(std::move(ns));
}


// A settled Result Object: {ok: Bool, value: Any, error: String?}. Mirrors the
// Proc.all allSettled shape so `r.ok ? r.value : r.error` reads the same way.
inline Value _settled_ok(Value v) {
  ObjectValue o;
  o.initialize("ok", Value(true), false);
  o.initialize("value", std::move(v), false);
  o.initialize("error", Value(), false);
  return Value(std::move(o));
}
inline Value _settled_err(std::string msg) {
  ObjectValue o;
  o.initialize("ok", Value(false), false);
  o.initialize("value", Value(), false);
  o.initialize("error", Value(std::move(msg)), false);
  return Value(std::move(o));
}


// Record one element's success, by mode. Race claims the winner slot via CAS
// and cancels the rest; the loser's result is discarded.
inline void parallel_record_success(ParallelState& st, size_t i, Value r) {
  switch (st.mode) {
    case PMode::Map: {
      sendable::SerCtx sc;
      st.results[i] = sendable::serialize(r, sc);
      break;
    }
    case PMode::MapSettled: {
      sendable::SerCtx sc;
      st.results[i] = sendable::serialize(_settled_ok(std::move(r)), sc);
      break;
    }
    case PMode::Each:
      break;  // side effects only
    case PMode::Race:
      if (!st.race_won.exchange(true)) {
        std::lock_guard<std::mutex> lk(st.err_m);
        sendable::SerCtx sc;
        st.race_result = sendable::serialize(r, sc);
        st.interrupt.store(true, std::memory_order_relaxed);  // cancel the rest
      }
      break;
  }
}

// Record one element's failure, by mode. Settled stores a Result and keeps
// going; Race remembers the first error for the all-failed message (no
// cancellation — a sibling may still win); Map/Each fail-fast.
inline void parallel_record_element_error(ParallelState& st, size_t i,
                                          culebra::CulebraError e) {
  switch (st.mode) {
    case PMode::MapSettled: {
      sendable::SerCtx sc;
      st.results[i] = sendable::serialize(_settled_err(e.what()), sc);
      break;
    }
    case PMode::Race: {
      std::lock_guard<std::mutex> lk(st.err_m);
      if (!st.failed) { st.failed = true; st.err_index = i; st.err = std::move(e); }
      break;
    }
    default:  // Map / Each
      parallel_record_error(st, i, std::move(e));
  }
}

// One worker: its own Runtime/heap, deserialize `fn` once, then drain elements
// until the queue is empty or a sibling fails. Runs on a spawned thread or
// inline on the parent (the parent always works one, so progress is guaranteed
// even with zero free thread slots).
inline void parallel_worker(std::shared_ptr<ParallelState> st) {
  culebra::Runtime rt;
  culebra::RuntimeScope scope(rt);
  rt.interrupt_flag = &st->interrupt;
  auto interp = std::make_shared<Interpreter>();
  interp->interrupt_flag_ = &st->interrupt;
  auto base = sendable::isolate_base_env();
  try {
    sendable::DeCtx dc;
    Value fn = sendable::deserialize(st->fn, *interp, base, dc);
    while (!st->interrupt.load(std::memory_order_relaxed)) {
      size_t i = st->next.fetch_add(1, std::memory_order_relaxed);
      if (i >= st->items.size()) break;
      try {
        sendable::DeCtx idc;
        Value item = sendable::deserialize(st->items[i], *interp, base, idc);
        Value r = interp->call_closure(fn, base,
                                       std::vector<Value>{std::move(item)});
        parallel_record_success(*st, i, std::move(r));
      } catch (culebra::CulebraError& e) {
        parallel_record_element_error(*st, i, e);
      } catch (Value& thrown) {
        parallel_record_element_error(*st, i,
            culebra::CulebraError("RuntimeError", thrown.str()));
      } catch (std::exception& e) {
        parallel_record_element_error(*st, i,
            culebra::CulebraError("RuntimeError", e.what()));
      }
      st->done.fetch_add(1, std::memory_order_relaxed);  // for on_progress
    }
  } catch (culebra::CulebraError& e) {
    parallel_record_error(*st, 0, e);  // fn itself wasn't Sendable / rebuildable
  } catch (std::exception& e) {
    // No exception may escape an inline worker — the parent's not-yet-joined
    // threads would std::terminate. Mirror run_isolate_child's final arm.
    parallel_record_error(*st, 0, culebra::CulebraError("RuntimeError", e.what()));
  }
}


// Runs on the parent thread while thread workers drain the queue: polls the
// shared `done` counter and calls `on_progress(done, total)` on the parent heap
// each time it advances. Polling (not a CV) keeps the workers' hot path free of
// a notify; 10 ms granularity is ample for a progress bar. A throwing callback
// cancels the run (recorded in cb_err, re-raised by parallel_run after join).
inline void parallel_progress_coordinator(
    std::shared_ptr<ParallelState> st, const Value& on_progress,
    const std::shared_ptr<Environment>& env, std::exception_ptr& cb_err) {
  auto cb_interp = std::make_shared<Interpreter>();
  const size_t total = st->items.size();
  size_t reported = 0;
  while (reported < total && !st->interrupt.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    size_t d = std::min(st->done.load(std::memory_order_relaxed), total);
    if (d == reported) continue;
    reported = d;
    try {
      cb_interp->call_closure(on_progress, env,
          {Value(static_cast<int64_t>(reported)), Value(static_cast<int64_t>(total))});
    } catch (...) {
      // Stash the throw (a CulebraError or a raw `throw <value>` Value) and stop
      // the workers; parallel_run rethrows it once the threads are joined.
      cb_err = std::current_exception();
      st->interrupt.store(true, std::memory_order_relaxed);
      return;
    }
  }
}

inline Value parallel_run(const Value& items_v, const Value& fn_v, int64_t limit,
                          PMode mode, int64_t line, int64_t col,
                          const Value& on_progress = Value(),
                          std::shared_ptr<Environment> parent_env = nullptr) {
  const char* who = parallel_mode_name(mode);
  if (items_v.type != Value::Array) {
    throw culebra::CulebraError("TypeError",
        std::format("Parallel.{}: first argument must be an Array", who),
        line, col);
  }
  if (fn_v.type != Value::Function) {
    throw culebra::CulebraError("TypeError",
        std::format("Parallel.{}: second argument must be a function", who),
        line, col);
  }
  const auto& items = *items_v.to_array().values;
  auto st = std::make_shared<ParallelState>();
  st->mode = mode;
  // Serialize fn + every element up front — Sendable violations throw here.
  { sendable::SerCtx sc; st->fn = sendable::serialize(fn_v, sc); }
  st->items.reserve(items.size());
  for (const auto& it : items) {
    sendable::SerCtx sc;
    st->items.push_back(sendable::serialize(it, sc));
  }
  if (items.empty()) {
    release_inflight_channels(st->fn);  // no workers will consume it
    if (mode == PMode::Race) {
      throw culebra::CulebraError("ParallelError",
          "Parallel.race: empty array has no result", line, col);
    }
    if (!st->collects()) return Value();
    return Value(ArrayValue{});
  }
  if (st->collects()) st->results.resize(items.size());

  if (limit < 1) limit = isolate_cap();
  size_t target = std::min(static_cast<size_t>(limit), items.size());
  // With on_progress the parent can't also be a worker (the callback runs on
  // the parent heap, an inline worker on a separate one) — it coordinates while
  // up to `target` workers run on threads. Without it, the parent works one
  // inline, so the pool drains even when no thread slots are free.
  bool has_progress = on_progress.type == Value::Function;
  size_t to_spawn = has_progress ? target : target - 1;
  std::vector<culebra::SizedThread> threads;
  for (size_t w = 0; w < to_spawn; w++) {
    if (g_live_isolates().fetch_add(1, std::memory_order_relaxed) <
        isolate_cap()) {
      threads.emplace_back([st] {
        parallel_worker(st);
        g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
      });
    } else {
      g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
      break;
    }
  }
  std::exception_ptr cb_err;
  if (has_progress && !threads.empty()) {
    parallel_progress_coordinator(st, on_progress, parent_env, cb_err);
  } else {
    parallel_worker(st);  // no free slot for progress → run inline, no callbacks
  }
  for (auto& t : threads) t.join();
  if (cb_err) std::rethrow_exception(cb_err);

  // Each worker rebuilt fn (and its claimed item), so the in-flight refs the
  // parent's serialize took are now redundant — release them once.
  release_inflight_channels(st->fn);
  for (const auto& it : st->items) release_inflight_channels(it);

  // Race: the winner (if any) is the single result; all-fail throws.
  if (mode == PMode::Race) {
    if (st->race_won.load()) return chan_take(st->race_result);
    throw culebra::CulebraError("ParallelError",
        std::format("Parallel.race: all {} elements failed: {}", items.size(),
                    st->err ? st->err->what() : "unknown"),
        line, col);
  }
  // Map / Each fail-fast (MapSettled never sets failed).
  if (st->failed) {
    for (const auto& rn : st->results) release_inflight_channels(rn);
    throw culebra::CulebraError("ParallelError",
        std::format("Parallel.{}: element[{}] failed: {}", who, st->err_index,
                    st->err->what()),
        line, col);
  }
  if (!st->collects()) return Value();
  ArrayValue out;
  out.values->reserve(st->results.size());
  for (const auto& rn : st->results) {
    out.values->push_back(chan_take(rn));  // deserialize + release in-flight
  }
  return Value(std::move(out));
}

// Parallel combinators over an Array, each running `fn(element)` across a
// worker pool (limit = parallelism cap, 0 = machine default):
//   map(items, fn)        -> Array     N→N in input order, fail-fast
//   each(items, fn)       -> Nil       side effects, fail-fast
//   map_settled(items,fn) -> [Result]  per-element {ok,value,error}, never fail-fast
//   race(items, fn)       -> Any       first success wins (cancels the rest)
inline Value make_parallel_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  auto make_entry = [](PMode mode, std::string_view rt) {
    return Value(FunctionValue(
        {{"items", false, ""sv},
         {"fn", false, ""sv},
         {"limit", false, ""sv, nullptr, kw_default_zero()},
         {"on_progress", false, ""sv, nullptr, kw_default_nil()}},
        [mode](std::shared_ptr<Environment> env) -> Value {
          int64_t line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
          int64_t col = env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
          return parallel_run(env->get("items"), env->get("fn"),
                              env->get("limit").to_long(), mode, line, col,
                              env->get("on_progress"), env);
        },
        rt));
  };
  ns.initialize("map", make_entry(PMode::Map, "Array"sv), false);
  ns.initialize("each", make_entry(PMode::Each, "Nil"sv), false);
  ns.initialize("map_settled", make_entry(PMode::MapSettled, "Array"sv), false);
  ns.initialize("race", make_entry(PMode::Race, "Any"sv), false);
  return Value(std::move(ns));
}

// Build the live handle Object: `join` / `poll` / `drop` methods closing over
// the shared core, plus a `__nonsendable__` marker (a handle can't be sent).
// The id the handle carries is the same counter the compiled engines'
// registry hands out (they need it to find the core again through a
// captureless native method; this walker closes over `core` directly). It is
// on the handle for the same reason a Channel endpoint carries
// `__channel_id__` on every engine — a handle's identity is observable, and
// all three must show the same one.

inline Value make_isolate_handle(std::shared_ptr<IsolateCore> core) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("_core_id",
               Value(static_cast<int64_t>(isolate_next_id().fetch_add(
                   1, std::memory_order_relaxed))),
               true);
  h.initialize("__nonsendable__", Value(true), false);

  h.initialize(
      "join",
      Value(FunctionValue(
          {},
          [core](std::shared_ptr<Environment>) -> Value {
            if (!core->ran_inline) {
              std::unique_lock<std::mutex> lk(core->m);
              core->cv.wait(lk, [&] { return core->finished; });
            }
            return _isolate_extract(*core);
          },
          ""sv)),
      false);

  h.initialize(
      "poll",
      Value(FunctionValue(
          {},
          [core](std::shared_ptr<Environment>) -> Value {
            if (!core->ran_inline) {
              std::lock_guard<std::mutex> lk(core->m);
              if (!core->finished) return Value();  // nil: still running
            }
            return _isolate_extract(*core);
          },
          ""sv)),
      false);

  h.initialize(
      "drop",
      Value(FunctionValue(
          {},
          [core](std::shared_ptr<Environment>) -> Value {
            if (core->joined) return Value();
            if (core->thread.joinable()) {
              // Ask the isolate to stop (it unwinds at its next statement
              // boundary / loop safepoint / blocking channel op), then wait +
              // join so no thread is left detached.
              mark_isolate_cancelled(*core);
              {
                std::unique_lock<std::mutex> lk(core->m);
                core->cv.wait(lk, [&] { return core->finished; });
              }
              core->thread.join();
              core->joined = true;
            }
            return Value();
          },
          "Nil"sv)),
      false);

  return Value(std::move(h));
}

// `Isolate.spawn(fn, *args) -> handle`. Validates Sendability of the closure
// and args up front (throws SendError on violation), then either spawns a
// thread or, beyond the parallelism cap, runs inline on the current thread.
inline Value make_isolate_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize(
      "spawn",
      Value(FunctionValue(
          {{"fn", false, ""sv},
           FunctionValue::Parameter::make_args_rest("args")},
          [](std::shared_ptr<Environment> env) -> Value {
            const Value& fnv = env->get("fn");
            int64_t line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            int64_t col =
                env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            if (fnv.type != Value::Function) {
              throw culebra::CulebraError(
                  "TypeError",
                  "Isolate.spawn: first argument must be a function", line, col);
            }
            std::vector<Value> extra;
            if (env->has("args")) {
              if (const Value& restv = env->get("args");
                  restv.type == Value::Array) {
                extra = *restv.to_array().values;
              }
            }

            // Validate Sendable + build neutral forms (SendError surfaces here).
            sendable::SerCtx sc;
            sendable::SendNode sclosure;
            std::vector<sendable::SendNode> sargs;
            try {
              sclosure = sendable::serialize(fnv, sc);
              sargs.reserve(extra.size());
              for (const auto& a : extra) sargs.push_back(sendable::serialize(a, sc));
            } catch (culebra::CulebraError& e) {
              if (e.kind == "SendError" && e.line == 0) {
                throw culebra::CulebraError(
                    "SendError", std::string("Isolate.spawn: ") + e.what(),
                    line, col);
              }
              throw;
            }

            auto core = std::make_shared<IsolateCore>();
            // A closure (or arg) carrying a channel endpoint may block on a peer
            // isolate, so it MUST run on its own thread — the inline-over-cap
            // fallback runs synchronously on the spawning thread and would
            // deadlock (e.g. a streaming producer fills a bounded channel before
            // any consumer starts). This mirrors fan_in(items, fn), which always
            // threads its producers. CPU-bound closures still honor the cap and
            // may run inline. The counter is always bumped so run_isolate_child's
            // matching fetch_sub balances; a forced thread may exceed the cap.
            bool must_thread = node_carries_channel(sclosure);
            for (const auto& a : sargs)
              if (!must_thread && node_carries_channel(a)) must_thread = true;
            bool under_cap =
                g_live_isolates().fetch_add(1, std::memory_order_relaxed) <
                isolate_cap();
            bool threaded = must_thread || under_cap;
            if (!threaded) g_live_isolates().fetch_sub(1, std::memory_order_relaxed);

            if (threaded) {
              core->thread = culebra::SizedThread(
                  [core, sclosure = std::move(sclosure),
                   sargs = std::move(sargs)]() mutable {
                    run_isolate_child(core, sclosure, sargs);
                  });
              // So a top-level teardown can still reach this isolate if the
              // script's own handle unwinds past an unreached join()/drop()
              // (see interp_isolate_teardown_join_all above).
              interp_isolate_reg_add(core);
            } else {
              // Synchronous fallback (over the parallelism cap): run on the
              // current thread, but through the SAME copy round-trip as the
              // threaded path — deserialize rebuilds the closure with COPIED
              // captures, so a closure that mutates a captured collection can't
              // leak into the parent (the inline/threaded split stays
              // observationally identical). Sequential ⇒ no heap isolation
              // needed, so it runs on the parent heap. `join()` returns at once.
              core->ran_inline = true;
              auto inl = std::make_shared<Interpreter>();
              std::shared_ptr<Environment> root = env;  // parent base (builtins)
              while (root->outer) root = root->outer;
              try {
                sendable::DeCtx dc;
                Value fn = sendable::deserialize(sclosure, *inl, root, dc);
                std::vector<Value> iargs;
                iargs.reserve(sargs.size());
                for (const auto& sa : sargs)
                  iargs.push_back(sendable::deserialize(sa, *inl, root, dc));
                release_inflight_channels(sclosure);
                for (const auto& sa : sargs) release_inflight_channels(sa);
                core->inline_result = std::make_shared<Value>(
                    inl->call_closure(fn, root, std::move(iargs)));
              } catch (Value& thrown) {
                core->inline_thrown = std::make_shared<Value>(thrown);
              } catch (culebra::CulebraError& e) {
                core->error = e;
              }
              core->finished = true;
            }
            return make_isolate_handle(core);
          },
          "Object"sv)),
      false);
  return Value(std::move(ns));
}

}  // namespace culebra
