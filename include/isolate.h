#pragma once

// Isolate: CPU-parallel execution units, each on its own OS thread with its
// own GC heap ([[project-concurrency-c2]], milestone C2-a, interpreter only).
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
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include "sendable.h"

namespace culebra {

// Defined in the Channel section below. Releases the in-flight channel refs a
// serialized SendNode holds (the bump `channel_extract_hook` took), once the
// node has been deserialized for the last time. Declared early because the
// isolate child / inline paths consume nodes before that section.
inline void release_inflight_channels(const sendable::SendNode& n);
// Deserialize a transferred node on the current heap and release its in-flight
// channel refs (defined in the Channel section). Used by join() and recv().
inline Value chan_take(const sendable::SendNode& n);

// Process-wide parallelism cap and live-isolate counter. Raw spawns and (later)
// Parallel.map share ONE counter so recursive parallelism can't exceed the cap.
inline int isolate_cap() {
  static const int cap = [] {
    if (const char* e = std::getenv("CULEBRA_ISOLATE_LIMIT")) {
      int v = std::atoi(e);
      if (v > 0) return v;
    }
    unsigned hc = std::thread::hardware_concurrency();
    return static_cast<int>(hc ? hc : 1);
  }();
  return cap;
}

inline std::atomic<int>& g_live_isolates() {
  static std::atomic<int> n{0};
  return n;
}

// Shared, GC-external result slot for one spawned isolate. Holds the OS thread
// plus its completion state; the result crosses the boundary as a neutral
// SendNode (threaded) or, for the inline fallback, as a parent-heap Value.
struct IsolateCore {
  std::thread thread;
  std::mutex m;
  std::condition_variable cv;
  bool finished = false;
  bool joined = false;
  bool ran_inline = false;
  std::atomic<bool> interrupt{false};           // set by drop() to cancel

  Value inline_result;                          // ran_inline: parent-heap result
  sendable::SendNode result;                    // threaded: neutral result
  std::optional<culebra::CulebraError> error;   // structured error crossed back
  std::optional<sendable::SendNode> thrown;     // threaded user `throw` value
  std::optional<Value> inline_thrown;           // inline user `throw` value

  // Safety net: a joinable std::thread destroyed without join() calls
  // std::terminate. `join`/`drop` normally join first; this covers the rest.
  ~IsolateCore() {
    if (thread.joinable()) {
      interrupt.store(true, std::memory_order_relaxed);
      thread.join();
    }
  }
};

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
  if (core.inline_thrown) throw *core.inline_thrown;
  if (core.thrown) throw chan_take(*core.thrown);
  if (core.ran_inline) return core.inline_result;
  return chan_take(core.result);
}

// ===========================================================================
// Channel (tx / rx) — bounded message passing between isolates.
// ===========================================================================
//
// A channel is a GC-external core (mutex + bounded queue of serialized values)
// living in a process-wide registry keyed by id. Endpoints are script Objects
// carrying that id + a role (tx/rx); they are the one Sendable native handle
// (serialize ships the id, not a copy), so a closure can capture an endpoint
// and send it into a spawned isolate. When the last tx endpoint is dropped the
// channel auto-closes, ending any `for x in rx` — the deadlock-prevention core.

struct ChannelCore {
  std::mutex m;
  std::condition_variable cv;
  std::deque<sendable::SendNode> q;  // serialized (neutral) values
  size_t cap;
  int tx_count = 0;  // active senders (guarded by m)
  int rx_count = 0;  // active receivers
  bool closed = false;
  explicit ChannelCore(size_t c) : cap(c) {}
};

inline std::mutex& channel_registry_mutex() {
  static std::mutex m;
  return m;
}
inline std::unordered_map<long, std::shared_ptr<ChannelCore>>&
channel_registry() {
  static std::unordered_map<long, std::shared_ptr<ChannelCore>> r;
  return r;
}
inline std::atomic<long>& channel_next_id() {
  static std::atomic<long> n{1};
  return n;
}
inline std::shared_ptr<ChannelCore> chan_lookup(long id) {
  std::lock_guard<std::mutex> lk(channel_registry_mutex());
  auto it = channel_registry().find(id);
  return it == channel_registry().end() ? nullptr : it->second;
}

inline Value make_channel_endpoint(long id, int role);  // fwd (clone recurses)

// Adjust an endpoint's active count (+1 on clone / in-flight send, -1 on drop).
inline void chan_bump(long id, int role, int delta) {
  auto core = chan_lookup(id);
  if (!core) return;
  std::lock_guard<std::mutex> lk(core->m);
  if (role == 0) core->tx_count += delta;
  else           core->rx_count += delta;
}

inline void chan_drop(long id, int role);  // fwd (release uses it)

// Recursively release the in-flight refs (the extract-time bumps) a serialized
// node holds for any channel endpoints it carries. Called exactly once per
// serialized node, after it has been fully consumed (deserialized), so the
// "endpoint = one count" invariant holds across both 1:1 (spawn / recv) and
// 1:N (Parallel) transfers. Plain values are a no-op.
inline void release_inflight_channels(const sendable::SendNode& n) {
  using K = sendable::SendNode::K;
  if (n.kind == K::Channel) { chan_drop(n.i, n.b ? 1 : 0); return; }
  for (const auto& e : n.elems) release_inflight_channels(e);
  for (const auto& e : n.entries) {
    release_inflight_channels(e.first);
    release_inflight_channels(e.second);
  }
  for (const auto& c : n.captures) release_inflight_channels(c.second);
}

// Drop one endpoint: when the last tx is gone the channel auto-closes (waking
// receivers); when no endpoint of either kind remains the core is reclaimed.
inline void chan_drop(long id, int role) {
  auto core = chan_lookup(id);
  if (!core) return;
  bool reclaim = false;
  {
    std::lock_guard<std::mutex> lk(core->m);
    if (role == 0) {
      if (core->tx_count > 0) core->tx_count--;
      if (core->tx_count == 0) core->closed = true;
    } else {
      if (core->rx_count > 0) core->rx_count--;
    }
    reclaim = core->tx_count == 0 && core->rx_count == 0;
  }
  core->cv.notify_all();
  if (reclaim) {
    std::lock_guard<std::mutex> lk(channel_registry_mutex());
    channel_registry().erase(id);
  }
}

// Backend-neutral enqueue: the caller has already serialized the value (interp
// or JIT) into a neutral node. Blocks while the buffer is full; interrupt- and
// close-aware.
inline void channel_send_node(long id, sendable::SendNode node) {
  auto core = chan_lookup(id);
  if (!core) throw culebra::CulebraError("ChannelError", "send on a closed channel");
  std::unique_lock<std::mutex> lk(core->m);
  while (!core->closed && core->q.size() >= core->cap) {
    if (culebra::interrupt_requested())
      throw culebra::CulebraError("Interrupted", "isolate cancelled");
    core->cv.wait_for(lk, std::chrono::milliseconds(50));
  }
  if (core->closed)
    throw culebra::CulebraError("ChannelError", "send on a closed channel");
  core->q.push_back(std::move(node));
  lk.unlock();
  core->cv.notify_all();
}

inline void channel_send(long id, const Value& v) {
  sendable::SerCtx sc;
  channel_send_node(id, sendable::serialize(v, sc));  // Sendable check here
}

// Block until a value is available or the channel is closed+drained. Returns
// the dequeued (neutral) node, or nullopt when closed and empty.
inline std::optional<sendable::SendNode> chan_pop_blocking(long id) {
  auto core = chan_lookup(id);
  if (!core) return std::nullopt;
  std::unique_lock<std::mutex> lk(core->m);
  while (core->q.empty() && !core->closed) {
    if (culebra::interrupt_requested())
      throw culebra::CulebraError("Interrupted", "isolate cancelled");
    core->cv.wait_for(lk, std::chrono::milliseconds(50));
  }
  if (core->q.empty()) return std::nullopt;  // closed + empty
  sendable::SendNode node = std::move(core->q.front());
  core->q.pop_front();
  lk.unlock();
  core->cv.notify_all();
  return node;
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
inline Value channel_recv(long id) {
  auto node = chan_pop_blocking(id);
  return node ? chan_take(*node) : Value();
}

inline Value channel_iter(long id) {
  return _make_iterator(
      [id](std::shared_ptr<Environment>) -> std::optional<Value> {
        auto node = chan_pop_blocking(id);
        return node ? _iter_step_value(chan_take(*node))
                    : _iter_step_done();  // closed + empty → for-in ends
      });
}

// Idempotent endpoint drop: an endpoint can be dropped both explicitly
// (`tx.drop()`) and again by the GC when its isolate's heap tears down, so the
// count must move exactly once per endpoint. The `_dropped` flag (on the same
// shared property map both call paths see) guards the second call.
inline Value _chan_drop_once(const Value& self, long id, int role) {
  auto& props = *self.to_object().properties;
  if (auto it = props.find("_dropped");
      it != props.end() && it->second.val.type == Value::Bool &&
      it->second.val.to_bool()) {
    return Value();
  }
  props.insert_or_assign("_dropped", Symbol{Value(true), true});
  chan_drop(id, role);
  return Value();
}

inline Value make_channel_endpoint(long id, int role) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("__channel_endpoint__", Value(true), false);
  h.initialize("__channel_id__", Value(id), false);
  h.initialize("__channel_role__", Value(static_cast<long>(role)), false);
  h.initialize("_dropped", Value(false), true);
  h.initialize("drop",
      Value(FunctionValue({}, [id, role](std::shared_ptr<Environment> env) -> Value {
        return _chan_drop_once(env->get("this"), id, role);
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
  }
  return Value(std::move(h));
}

// Register the Sendable hooks (endpoints ship by id + role) once at load.
inline bool _install_channel_hooks() {
  sendable::channel_extract_hook() =
      [](const Value& v) -> sendable::ChannelRef {
    const auto& o = v.to_object();
    long id = o.get("__channel_id__").to_long();
    int role = static_cast<int>(o.get("__channel_role__").to_long());
    // In-flight ref: keeps the channel open across the async gap between
    // serialize and rebuild (so a sender dropping its endpoint right after
    // spawn can't auto-close before the child rebuilds). Released exactly once
    // by release_inflight_channels after the node is consumed.
    chan_bump(id, role, +1);
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

// `Channel.new(cap = 1) -> (tx, rx)`. Bounded; capacity must be >= 1.
inline Value make_channel_namespace() {
  using namespace std::literals;
  static const auto cap_default = std::make_shared<Value>(Value(1L));
  ObjectValue ns;
  ns.initialize("new",
      Value(FunctionValue({{"cap", false, ""sv, nullptr, cap_default}},
          [](std::shared_ptr<Environment> env) -> Value {
            long cap = env->get("cap").to_long();
            long line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            long col = env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
            if (cap < 1) {
              throw culebra::CulebraError("ValueError",
                  "Channel.new: capacity must be >= 1 (rendezvous channels are "
                  "not yet supported)", line, col);
            }
            auto core = std::make_shared<ChannelCore>(static_cast<size_t>(cap));
            long id = channel_next_id().fetch_add(1, std::memory_order_relaxed);
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
  return Value(std::move(ns));
}

// ===========================================================================
// Parallel.map / Parallel.each — a worker pool over a fixed isolate count.
// ===========================================================================
//
// The high-level API that dogfoods the Sendable path: `fn` and every element
// cross the boundary, so a Sendable hole shows up here first. `limit` workers
// (default = the parallelism cap) pull elements from one shared queue — N→N
// with at most `limit` live isolates, not one isolate per element. Fail-fast:
// the first error stops the rest (via the shared interrupt flag) and is
// re-raised as `ParallelError` with the element index. Results come back in
// input order. on_progress / map_settled / map_reduce are later increments.

struct ParallelState {
  std::vector<sendable::SendNode> items;    // serialized inputs
  sendable::SendNode fn;                     // serialized closure
  bool collect = true;                       // map (true) vs each (false)
  std::vector<sendable::SendNode> results;   // serialized outputs, by index
  std::atomic<size_t> next{0};               // next element to claim
  std::atomic<bool> interrupt{false};        // fail-fast / cancellation
  std::mutex err_m;
  bool failed = false;
  size_t err_index = 0;
  std::optional<culebra::CulebraError> err;
};

inline void parallel_record_error(ParallelState& st, size_t i,
                                  culebra::CulebraError e) {
  st.interrupt.store(true, std::memory_order_relaxed);  // stop the others
  if (e.kind == "Interrupted") return;  // a fail-fast consequence, not the cause
  std::lock_guard<std::mutex> lk(st.err_m);
  if (!st.failed) {
    st.failed = true;
    st.err_index = i;
    st.err = std::move(e);
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
        if (st->collect) {
          sendable::SerCtx sc;
          st->results[i] = sendable::serialize(r, sc);
        }
      } catch (culebra::CulebraError& e) {
        parallel_record_error(*st, i, e);
      } catch (Value& thrown) {
        parallel_record_error(*st, i,
            culebra::CulebraError("RuntimeError", thrown.str()));
      } catch (std::exception& e) {
        parallel_record_error(*st, i,
            culebra::CulebraError("RuntimeError", e.what()));
      }
    }
  } catch (culebra::CulebraError& e) {
    parallel_record_error(*st, 0, e);  // fn itself wasn't Sendable / rebuildable
  } catch (std::exception& e) {
    // No exception may escape an inline worker — the parent's not-yet-joined
    // threads would std::terminate. Mirror run_isolate_child's final arm.
    parallel_record_error(*st, 0, culebra::CulebraError("RuntimeError", e.what()));
  }
}

inline Value parallel_run(const Value& items_v, const Value& fn_v, long limit,
                          bool collect, long line, long col) {
  if (items_v.type != Value::Array) {
    throw culebra::CulebraError("TypeError",
        "Parallel.map/each: first argument must be an Array", line, col);
  }
  if (fn_v.type != Value::Function) {
    throw culebra::CulebraError("TypeError",
        "Parallel.map/each: second argument must be a function", line, col);
  }
  const auto& items = *items_v.to_array().values;
  auto st = std::make_shared<ParallelState>();
  st->collect = collect;
  // Serialize fn + every element up front — Sendable violations throw here.
  { sendable::SerCtx sc; st->fn = sendable::serialize(fn_v, sc); }
  st->items.reserve(items.size());
  for (const auto& it : items) {
    sendable::SerCtx sc;
    st->items.push_back(sendable::serialize(it, sc));
  }
  if (items.empty()) {
    release_inflight_channels(st->fn);  // no workers will consume it
    if (!collect) return Value();
    ArrayValue empty;
    return Value(std::move(empty));
  }
  if (collect) st->results.resize(items.size());

  if (limit < 1) limit = isolate_cap();
  size_t target = std::min(static_cast<size_t>(limit), items.size());
  // Spawn up to target-1 thread workers (cap-limited); the parent works one
  // inline, so the pool drains even when no thread slots are free.
  std::vector<std::thread> threads;
  for (size_t w = 0; w + 1 < target; w++) {
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
  parallel_worker(st);
  for (auto& t : threads) t.join();

  // Each worker rebuilt fn (and its claimed item), so the in-flight refs the
  // parent's serialize took are now redundant — release them once.
  release_inflight_channels(st->fn);
  for (const auto& it : st->items) release_inflight_channels(it);

  if (st->failed) {
    // Results are abandoned on the fail-fast path; release any in-flight refs
    // their (unconsumed) nodes hold. Unfilled slots are K::Nil = no-op.
    for (const auto& rn : st->results) release_inflight_channels(rn);
    throw culebra::CulebraError("ParallelError",
        std::format("Parallel.{}: element[{}] failed: {}",
                    collect ? "map" : "each", st->err_index, st->err->what()),
        line, col);
  }
  if (!collect) return Value();
  ArrayValue out;
  out.values->reserve(st->results.size());
  for (const auto& rn : st->results) {
    out.values->push_back(chan_take(rn));  // deserialize + release in-flight
  }
  return Value(std::move(out));
}

// `Parallel.map(items, fn, limit = <cap>) -> Array` (N→N, fail-fast) and
// `Parallel.each(items, fn, limit = <cap>) -> Nil` (side effects).
inline Value make_parallel_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  auto make_entry = [](bool collect, std::string_view rt) {
    return Value(FunctionValue(
        {{"items", false, ""sv},
         {"fn", false, ""sv},
         {"limit", false, ""sv, nullptr, kw_default_zero()}},
        [collect](std::shared_ptr<Environment> env) -> Value {
          long line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
          long col = env->has("__COLUMN__") ? env->get("__COLUMN__").to_long() : 0;
          return parallel_run(env->get("items"), env->get("fn"),
                              env->get("limit").to_long(), collect, line, col);
        },
        rt));
  };
  ns.initialize("map", make_entry(true, "Array"sv), false);
  ns.initialize("each", make_entry(false, "Nil"sv), false);
  return Value(std::move(ns));
}

// Build the live handle Object: `join` / `poll` / `drop` methods closing over
// the shared core, plus a `__nonsendable__` marker (a handle can't be sent).
inline Value make_isolate_handle(std::shared_ptr<IsolateCore> core) {
  using namespace std::literals;
  ObjectValue h;
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
              // boundary / blocking channel op), then wait + join so no thread
              // is left detached.
              core->interrupt.store(true, std::memory_order_relaxed);
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
            long line = env->has("__LINE__") ? env->get("__LINE__").to_long() : 0;
            long col =
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
            bool threaded =
                g_live_isolates().fetch_add(1, std::memory_order_relaxed) <
                isolate_cap();
            if (!threaded) g_live_isolates().fetch_sub(1, std::memory_order_relaxed);

            if (threaded) {
              core->thread = std::thread(
                  [core, sclosure = std::move(sclosure),
                   sargs = std::move(sargs)]() mutable {
                    run_isolate_child(core, sclosure, sargs);
                  });
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
                core->inline_result =
                    inl->call_closure(fn, root, std::move(iargs));
              } catch (Value& thrown) {
                core->inline_thrown = thrown;
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
