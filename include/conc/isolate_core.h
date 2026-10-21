#pragma once

// The isolate/channel core both engines share: IsolateCore (the GC-external
// result slot), the channel core and its send/recv/drop protocol, the
// fan_in merge machinery, Signal.notify delivery, the Parallel worker-pool
// state, and the teardown join helpers. Everything here speaks SendNode and
// plain C++ — no Value / Environment / JitValue — so the compiled lanes
// (conc/sendable.h) include this alone. Moved verbatim from isolate.h
// (Phase 4 B7-b), whose tree-walker half retired with the engine (B7-f).

#include <conc/send_node.h>      // SendNode
#include <base/shared.h>         // SizedThread, the cancel/wake flags

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace culebra {

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

// One spawned isolate: its OS thread, and what the thread hands back (the
// result as a neutral SendNode). The thread only borrows the core
// (run_isolate_child_jit), so the core owns the thread's whole life: cancel()
// asks it to stop, join() waits for it to end, and whoever lets the core go
// last does both. Either may be called again, and join() from any thread.
struct IsolateCore {
  culebra::SizedThread thread;
  std::mutex m;  // guards `finished` and what the child hands back
  bool finished = false;
  sendable::SendNode result;                    // neutral result
  std::optional<culebra::CulebraError> error;   // structured error crossed back
  std::optional<sendable::SendNode> thrown;     // threaded user `throw` value
  // A cancel that reached the child, kept apart from `error`: an Interrupted
  // is not a CulebraError, and join() must not re-raise it as one.
  std::optional<std::string> interrupted;
  std::atomic<bool> interrupt{false};       // set by cancel()
  std::atomic<bool> cancel_counted{false};  // contributed to g_cancel_pending

  // Sets the child's sticky `interrupt` AND the global JIT wake (so a JIT
  // tight loop branches to its safepoint). Counted once per core, so the wake
  // stays set until every cancelled isolate is joined.
  void cancel() {
    interrupt.store(true, std::memory_order_relaxed);
    if (!cancel_counted.exchange(true)) culebra_g_cancel_pending.fetch_add(1);
    culebra_g_wake.store(true);
  }
  // Locked: at exit the teardown walk and a parent isolate's own teardown may
  // both reach one grandchild at once, and a thread is not safe to join from two.
  void join() {
    std::lock_guard<std::mutex> lk(join_m_);
    if (thread.joinable()) thread.join();
    if (cancel_counted.exchange(false) && culebra_g_cancel_pending.fetch_sub(1) == 1)
      _clear_wake_if_idle();
  }

  // What the child handed back stays here for as long as the handle may be
  // asked for it (`join` after `poll` reads it again), so the refs its nodes
  // hold in flight (a channel end, a SharedBuffer, a Shared view) go back
  // with it and not at a read.
  ~IsolateCore() {
    if (thread.joinable()) cancel();
    join();
  }

 private:
  std::mutex join_m_;
};

// Whether the isolate has finished (normally or by throwing). Read under the
// core lock; used by the fan_in merge to treat a finished producer's drained
// channel as done even if its `tx` wasn't dropped.
inline bool _isolate_finished(IsolateCore& core) {
  std::lock_guard<std::mutex> lk(core.m);
  return core.finished;
}

// ===========================================================================
// Channel (tx / rx) — bounded message passing between isolates.
// ===========================================================================
//
// A channel is a GC-external core (mutex + bounded queue of serialized values).
// Endpoints are handle Objects holding the core itself and a role (tx/rx);
// they are the one Sendable native handle (serialize ships the core, not a
// copy), so a closure can capture an endpoint and send it into a spawned
// isolate. When the last tx endpoint is dropped the channel auto-closes,
// ending any `for x in rx` — the deadlock-prevention core.
//
// The core lives exactly as long as its counts. Whatever holds a pointer to
// one holds one count of its own, tx or rx: an endpoint, a message carrying an
// endpoint (counted when it is serialized, given back when the message goes), a
// fan_in merge's sources, the Signal.notify table. The last count given back
// frees it, so nothing looks a channel up: a send or a recv touches no lock but
// the channel's own.

// A consumer waiting on SEVERAL channels at once (Channel.fan_in, the basis for
// a future select) registers one MergeWaiter on each source's `selectors` list.
// Any send/close on a source signals every registered waiter. `ready` is a
// LEVEL-triggered flag (set true, stays true until the waiter rescans): a signal
// that races ahead of the wait is not lost — the next wait sees `ready` already
// set. Mirrors Go's `sudog` / crossbeam's `Waker` registered on all cases.
struct MergeWaiter {
  std::mutex m;
  std::condition_variable cv;
  bool ready = false;
  void signal() {
    std::lock_guard<std::mutex> lk(m);
    ready = true;
    cv.notify_all();
  }
};

struct ChannelCore {
  std::mutex m;
  std::condition_variable cv;
  std::deque<sendable::SendNode> q;  // serialized (neutral) values
  size_t cap;
  int tx_count = 1;  // the counts held on each end (guarded by m), one
  int rx_count = 1;  // each at birth, for its creator to hand on
  bool closed = false;
  std::vector<MergeWaiter*> selectors;  // fan_in waiters (guarded by m)
  explicit ChannelCore(size_t c) : cap(c) {}
  // Signal every registered fan_in waiter. Caller MUST hold `m` so a waiter
  // can't be unregistered + destroyed between the read and the signal.
  void signal_selectors() {
    for (auto* w : selectors) w->signal();
  }
};

// A core as a message carries it, in SendNode::i, and back.
inline int64_t chan_word(ChannelCore* core) {
  return static_cast<int64_t>(reinterpret_cast<intptr_t>(core));
}
inline ChannelCore* chan_core(int64_t word) {
  return reinterpret_cast<ChannelCore*>(static_cast<intptr_t>(word));
}

inline void chan_drop(ChannelCore* core, int role);  // fwd

// Cancels them all before joining any, so they wind down together rather than
// one after another.
inline void cancel_and_join_isolates(
    const std::vector<std::shared_ptr<IsolateCore>>& cores) {
  for (const auto& core : cores) core->cancel();
  for (const auto& core : cores) core->join();
}

// A Channel.fan_in merge: the channels it reads, each held by an rx count of
// the merge's own, and for `fan_in(items, fn)` the producer isolates it
// spawned, one per source. Whatever reads through it shares it: the merged rx
// and each of its for-in cursors. The last of them to go stops the producers
// still running and joins them all, then gives the counts back.
struct ChannelMerge {
  std::vector<ChannelCore*> sources;
  std::vector<std::shared_ptr<IsolateCore>> producers;  // empty for fan_in([rx])
  ChannelMerge() = default;
  ChannelMerge(const ChannelMerge&) = delete;
  ChannelMerge& operator=(const ChannelMerge&) = delete;
  ~ChannelMerge() {
    cancel_and_join_isolates(producers);
    for (auto* core : sources) chan_drop(core, /*role=*/1);
  }
};

// Every isolate an Isolate.spawn or a fan_in(items, fn) has started. A handle
// that goes cancels and joins its own; this is for the ones nothing lets go
// of by the end of the run (a handle held in a cycle a top-level binding
// reaches), which join_started_isolates stops before the code they run is
// freed. Weak: what keeps one is its handle or its merge.
struct StartedIsolates {
  std::mutex m;
  std::vector<std::weak_ptr<IsolateCore>> cores;
  size_t prune_at = 64;  // doubles with the live ones, so a start is O(1) amortized
};
inline StartedIsolates& started_isolates() {
  static StartedIsolates s;
  return s;
}
inline void join_started_isolates() {
  std::vector<std::shared_ptr<IsolateCore>> live;
  {
    auto& s = started_isolates();
    std::lock_guard<std::mutex> lk(s.m);
    for (const auto& w : s.cores)
      if (auto core = w.lock()) live.push_back(std::move(core));
  }
  cancel_and_join_isolates(live);
}
// Once it has a thread to join.
inline void started_isolates_add(const std::shared_ptr<IsolateCore>& core) {
  isolate_teardown_join_hook.store(&join_started_isolates,
                                   std::memory_order_relaxed);
  auto& s = started_isolates();
  std::lock_guard<std::mutex> lk(s.m);
  if (s.cores.size() >= s.prune_at) {
    std::erase_if(s.cores, [](const auto& w) { return w.expired(); });
    s.prune_at = std::max<size_t>(64, s.cores.size() * 2);
  }
  s.cores.push_back(core);
}

// Take one more count on an end: the caller holds one already (an endpoint, a
// message), so the core cannot go while this runs.
inline void chan_bump(ChannelCore* core, int role) {
  std::lock_guard<std::mutex> lk(core->m);
  if (role == 0) core->tx_count++;
  else           core->rx_count++;
}

// A channel end as a message carries it: the core, the role, and the count the
// caller already took for it (SendNode::held), given back when the last copy
// of the message goes. A dropped end (null core) holds nothing.
inline sendable::SendNode chan_node(ChannelCore* core, int role) {
  sendable::SendNode n;
  n.kind = sendable::SendNode::K::Channel;
  n.i = chan_word(core);
  n.b = (role == 1);
  if (core)
    n.held = {core, [role](void* p) {
                chan_drop(static_cast<ChannelCore*>(p), role);
              }};
  return n;
}

// Does a serialized value carry a channel endpoint (captured by a closure, held
// in a collection, or passed as an arg)? Such a value makes its isolate an
// inter-isolate primitive that may block on send/recv whose only unblocker is
// ANOTHER isolate running concurrently — so it must run on a real thread, never
// the inline-over-cap fallback (which would deadlock: a streaming producer fills
// a bounded channel with no consumer draining it yet).
template <class Leaf>
inline bool node_any(const sendable::SendNode& n, Leaf leaf) {
  if (leaf(n.kind)) return true;
  for (const auto& e : n.elems)
    if (node_any(e, leaf)) return true;
  for (const auto& e : n.entries)
    if (node_any(e.first, leaf) || node_any(e.second, leaf)) return true;
  for (const auto& c : n.captures)
    if (node_any(c.second, leaf)) return true;
  return false;
}

inline bool node_carries_channel(const sendable::SendNode& n) {
  using K = sendable::SendNode::K;
  return node_any(n, [](K k) { return k == K::Channel; });
}

// Does a serialized value carry a handle by reference, live or dropped?
// (node_carries_channel asks a narrower question: only a channel endpoint can
// block its isolate.)
inline bool node_carries_inflight(const sendable::SendNode& n) {
  using K = sendable::SendNode::K;
  return node_any(n, [](K k) {
    return k == K::Channel || k == K::SharedBuffer || k == K::SharedVal;
  });
}

// Give one count back: when the last tx is gone the channel auto-closes
// (waking receivers); when no count of either kind remains the core is freed.
inline void chan_drop(ChannelCore* core, int role) {
  bool reclaim = false;
  {
    std::lock_guard<std::mutex> lk(core->m);
    (role == 0 ? core->tx_count : core->rx_count)--;
    assert(core->tx_count >= 0 && core->rx_count >= 0);
    reclaim = core->tx_count == 0 && core->rx_count == 0;
    // A waiter waits on `closed` or the queue, never on a count, so only the
    // last tx going wakes anyone. Notified under the lock: once it is let go,
    // the core may be freed.
    if (role == 0 && core->tx_count == 0) {
      core->closed = true;
      core->signal_selectors();
      core->cv.notify_all();
    }
  }
  // No count is left, so nothing else can reach the core. An undelivered
  // node goes with it, and gives back what it holds in flight.
  if (reclaim) delete core;
}

// Non-blocking enqueue: push iff there is room, else drop and return false.
// Used by Signal.notify delivery, which (like Go's signal relay) must never
// block — a full buffer means the program hasn't drained, so the extra signal
// is dropped rather than wedging the delivery thread.
inline bool channel_try_send_node(ChannelCore& core, sendable::SendNode node) {
  std::lock_guard<std::mutex> lk(core.m);
  if (core.closed) return false;
  size_t room = core.cap == 0 ? 1 : core.cap;
  if (core.q.size() >= room) return false;  // full → drop
  core.q.push_back(std::move(node));
  core.cv.notify_all();
  core.signal_selectors();
  return true;
}

// --- Signal.notify (Go signal.Notify model) -------------------------------
//
// `Signal.notify(tx)` registers a channel's tx endpoint to receive a "SIGINT"
// string on each Ctrl+C, in place of the cooperative Interrupted throw — so a
// program drives its own graceful shutdown (block on `rx.recv()`, then clean
// up) instead of unwinding. The SIGINT handler (shared.h) can't touch a channel
// (not async-signal-safe), so it only sets `culebra_g_signal_pending`; a
// detached delivery thread polls that flag and relays to every registered
// channel with a non-blocking send (drop if full, as Go does). `Signal.reset()`
// restores default (throw) behavior. notify retains a tx on each channel so it
// stays open while registered even if the user drops their own endpoint.
inline std::mutex& signal_registry_mutex() { static std::mutex m; return m; }
// Each holds a tx count of the table's own.
inline std::vector<ChannelCore*>& signal_channels() {
  static std::vector<ChannelCore*> v;
  return v;
}

inline void _signal_delivery_loop() {
  for (;;) {
    if (culebra_g_signal_pending.exchange(false, std::memory_order_relaxed)) {
      // Under the table's lock, which is what keeps its counts — and so the
      // channels — from going mid-delivery; the sends never block.
      std::lock_guard<std::mutex> lk(signal_registry_mutex());
      for (auto* core : signal_channels()) {
        sendable::SendNode n;
        n.kind = sendable::SendNode::K::Str;
        n.s = "SIGINT";
        channel_try_send_node(*core, std::move(n));
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

inline void _ensure_signal_delivery_thread() {
  static std::once_flag once;
  std::call_once(once, [] { std::thread(_signal_delivery_loop).detach(); });
}

// `core` is null for a tx already dropped: nothing is delivered to it, and
// Ctrl+C is routed away from the throw all the same.
inline void signal_notify_register(ChannelCore* core) {
  if (core) {
    chan_bump(core, /*role=*/0);  // a sender of its own → channel stays open
    std::lock_guard<std::mutex> lk(signal_registry_mutex());
    signal_channels().push_back(core);
  }
  culebra_g_signal_notify.store(true, std::memory_order_relaxed);
  _ensure_signal_delivery_thread();
}

inline void signal_notify_reset() {
  culebra_g_signal_notify.store(false, std::memory_order_relaxed);
  std::vector<ChannelCore*> cores;
  {
    std::lock_guard<std::mutex> lk(signal_registry_mutex());
    cores.swap(signal_channels());
  }
  for (auto* core : cores) chan_drop(core, /*role=*/0);  // release the senders
  culebra_g_signal_pending.store(false, std::memory_order_relaxed);
}

// Backend-neutral enqueue: the caller has already serialized the value into a
// neutral node. Blocks while the buffer is full; interrupt- and close-aware. A
// null core is an endpoint already dropped, which is a closed channel. A node
// that never makes it into the queue goes, and gives back what it holds.
inline void channel_send_node(ChannelCore* core, sendable::SendNode node) {
  if (!core) throw culebra::CulebraError("ChannelError", "send on a closed channel");
  std::unique_lock<std::mutex> lk(core->m);
  // A rendezvous channel (cap 0) holds at most one in-transit value: wait for an
  // empty slot, enqueue, then block again until a receiver takes it (synchronous
  // handoff). A bounded channel just waits for room.
  size_t room = core->cap == 0 ? 1 : core->cap;
  while (!core->closed && core->q.size() >= room) {
    if (culebra::interrupt_requested())
      throw culebra::Interrupted("isolate cancelled");
    core->cv.wait_for(lk, std::chrono::milliseconds(50));
  }
  if (core->closed)
    throw culebra::CulebraError("ChannelError", "send on a closed channel");
  core->q.push_back(std::move(node));
  core->cv.notify_all();
  core->signal_selectors();  // wake any fan_in waiter (under lk = no dangling)
  if (core->cap == 0) {
    while (!core->closed && !core->q.empty()) {
      if (culebra::interrupt_requested())
        throw culebra::Interrupted("isolate cancelled");
      core->cv.wait_for(lk, std::chrono::milliseconds(50));
    }
  }
}

// Block until a value is available or the channel is closed+drained. Returns
// the dequeued (neutral) node, or nullopt when closed and empty.
inline std::optional<sendable::SendNode> chan_pop_blocking(ChannelCore& core) {
  std::unique_lock<std::mutex> lk(core.m);
  while (core.q.empty() && !core.closed) {
    if (culebra::interrupt_requested())
      throw culebra::Interrupted("isolate cancelled");
    core.cv.wait_for(lk, std::chrono::milliseconds(50));
  }
  if (core.q.empty()) return std::nullopt;  // closed + empty
  sendable::SendNode node = std::move(core.q.front());
  core.q.pop_front();
  lk.unlock();
  core.cv.notify_all();
  return node;
}

// The three outcomes of a non-blocking pop. Blocking recv collapses the last
// two into nullopt (it only cares "a value, or never one"); a caller that
// can't block must tell "nothing yet" from "nothing ever" apart.
enum class ChanTryPopStatus { Value, Empty, Closed };
struct ChanTryPopResult {
  ChanTryPopStatus status;
  std::optional<sendable::SendNode> node;  // set iff status == Value
};

// The variant name each outcome answers with — one table, so the backends
// can't drift on the spelling.
inline std::string_view chan_status_variant(ChanTryPopStatus s) {
  switch (s) {
    case ChanTryPopStatus::Value: return "Value";
    case ChanTryPopStatus::Empty: return "Empty";
    case ChanTryPopStatus::Closed: return "Closed";
  }
  return "Closed";
}

// chan_pop_blocking's non-blocking twin: same lock / pop / notify, no wait.
// Both backends call this one function, so neither can observe a different
// queue-vs-close race than the other.
inline ChanTryPopResult chan_pop_nonblocking(ChannelCore& core) {
  std::unique_lock<std::mutex> lk(core.m);
  if (!core.q.empty()) {
    sendable::SendNode node = std::move(core.q.front());
    core.q.pop_front();
    lk.unlock();
    core.cv.notify_all();  // a sender waiting for room (bounded / rendezvous)
    return {ChanTryPopStatus::Value, std::move(node)};
  }
  return {core.closed ? ChanTryPopStatus::Closed : ChanTryPopStatus::Empty,
          std::nullopt};
}

// Take up to `max` of what is queued at this instant, in one lock. Popping
// one at a time races the producer instead: each pop wakes a blocked sender,
// which refills before the loop re-checks, so a cap-4 channel can hand back
// hundreds of values.
inline std::deque<sendable::SendNode> chan_take_queued(ChannelCore& core,
                                                      size_t max) {
  std::deque<sendable::SendNode> taken;
  {
    std::lock_guard<std::mutex> lk(core.m);
    if (max >= core.q.size()) {
      taken.swap(core.q);
    } else {
      auto end = core.q.begin() + static_cast<std::ptrdiff_t>(max);
      taken.assign(std::make_move_iterator(core.q.begin()),
                   std::make_move_iterator(end));
      core.q.erase(core.q.begin(), end);
    }
  }
  if (!taken.empty()) core.cv.notify_all();  // room for every waiting sender
  return taken;
}

// How `rx.drain(max = nil)`'s argument arrived. Each backend classifies its
// own value representation, then shares the checks below, so a bad `max`
// fails identically in both.
enum class DrainMaxKind { Nil, Count, Other };

inline size_t chan_drain_max(DrainMaxKind kind, int64_t n, int64_t line,
                             int64_t col) {
  if (kind == DrainMaxKind::Nil) return std::numeric_limits<size_t>::max();
  if (kind != DrainMaxKind::Count) {
    throw culebra::CulebraError("TypeError",
        "rx.drain: max must be a Long or nil", line, col);
  }
  if (n < 0) {
    throw culebra::CulebraError("ValueError", "rx.drain: max must be >= 0",
                                line, col);
  }
  return static_cast<size_t>(n);
}

// Wait until one of a merge's sources has a value and pop it; return nullopt
// once EVERY source is drained AND done. This is the fan_in / select core:
// register one waiter on each source, then block on it — event-driven, no
// polling (the 50ms is only the interrupt heartbeat, as in chan_pop_blocking).
// Mirrors Go's `select` (sudog on every case) and core.async's `alts!` (its
// `merge` is just a loop over this).
//
// A source is "done" when it is empty and either closed OR its producer isolate
// (for the fan_in(items, fn) form) has finished — the latter is the safety net
// for a producer that ends while something still holds a `tx` for its channel,
// so the channel never closed itself.
//
// The caller holds the merge for the call, which holds the sources.
inline std::optional<sendable::SendNode> chan_select_recv(
    const ChannelMerge& merge) {
  const auto& cores = merge.sources;
  MergeWaiter w;
  for (auto* c : cores) {
    std::lock_guard<std::mutex> lk(c->m);
    c->selectors.push_back(&w);  // register before the first scan: no lost wakeup
  }
  // Unregister on every exit (value / all done / interrupt throw).
  struct Unreg {
    const std::vector<ChannelCore*>& cores;
    MergeWaiter* w;
    ~Unreg() {
      for (auto* c : cores) {
        std::lock_guard<std::mutex> lk(c->m);
        auto& s = c->selectors;
        s.erase(std::remove(s.begin(), s.end(), w), s.end());
      }
    }
  } unreg{cores, &w};

  while (true) {
    bool all_done = true;
    for (size_t i = 0; i < cores.size(); i++) {
      auto* c = cores[i];
      std::unique_lock<std::mutex> lk(c->m);
      if (!c->q.empty()) {
        sendable::SendNode node = std::move(c->q.front());
        c->q.pop_front();
        lk.unlock();
        c->cv.notify_all();  // a sender waiting for room (bounded / rendezvous)
        return node;
      }
      // The source's producer, if the merge spawned one.
      auto* prod =
          i < merge.producers.size() ? merge.producers[i].get() : nullptr;
      bool done = c->closed || (prod && _isolate_finished(*prod));
      if (!done) all_done = false;
    }
    if (all_done) return std::nullopt;
    if (culebra::interrupt_requested())
      throw culebra::Interrupted("isolate cancelled");
    std::unique_lock<std::mutex> lk(w.m);
    w.cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return w.ready; });
    w.ready = false;
  }
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

// The four Parallel combinators differ only in how a worker records its
// per-element outcome; the pool, queue, and cancellation machinery are shared.
//   Map        — collect values by index, fail-fast on the first error
//   Each       — run for side effects, collect nothing, fail-fast
//   MapSettled — collect a {ok,value,error} Result by index, never fail-fast
//   Race       — first success wins (cancels the rest); all-fail throws
enum class PMode { Map, Each, MapSettled, Race };

struct ParallelState {
  std::vector<sendable::SendNode> items;    // serialized inputs
  sendable::SendNode fn;                     // serialized closure
  // The parent's default table, shared by the workers.
  std::shared_ptr<const sendable::TraitDefaults> trait_defaults;
  PMode mode = PMode::Map;
  std::vector<sendable::SendNode> results;   // serialized outputs, by index
  std::atomic<size_t> next{0};               // next element to claim
  std::atomic<size_t> done{0};               // elements finished (on_progress)
  std::atomic<bool> interrupt{false};        // fail-fast / cancellation
  std::mutex err_m;
  bool failed = false;
  size_t err_index = 0;
  std::optional<culebra::CulebraError> err;
  // Race: the first worker to succeed claims the winner slot (CAS) and stores
  // its serialized result; later finishers are discarded.
  std::atomic<bool> race_won{false};
  sendable::SendNode race_result;
  bool collects() const { return mode == PMode::Map || mode == PMode::MapSettled; }
};

inline void parallel_record_error(ParallelState& st, size_t i,
                                  culebra::CulebraError e) {
  st.interrupt.store(true, std::memory_order_relaxed);  // stop the others
  std::lock_guard<std::mutex> lk(st.err_m);
  if (!st.failed) {
    st.failed = true;
    st.err_index = i;
    st.err = std::move(e);
  }
}

inline const char* parallel_mode_name(PMode m) {
  switch (m) {
    case PMode::Map:        return "map";
    case PMode::Each:       return "each";
    case PMode::MapSettled: return "map_settled";
    case PMode::Race:       return "race";
  }
  return "map";
}

}  // namespace culebra
