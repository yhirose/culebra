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

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <thread>

#include "sendable.h"

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

  Value inline_result;                          // ran_inline: parent-heap result
  sendable::SendNode result;                    // threaded: neutral result
  std::optional<culebra::CulebraError> error;   // structured error crossed back
  std::optional<sendable::SendNode> thrown;     // threaded user `throw` value
  std::optional<Value> inline_thrown;           // inline user `throw` value

  // Safety net: a joinable std::thread destroyed without join() calls
  // std::terminate. `join`/`drop` normally join first; this covers the rest.
  ~IsolateCore() {
    if (thread.joinable()) thread.join();
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
  auto child = std::make_shared<Interpreter>();
  auto base = culebra::environment({});
  try {
    sendable::DeCtx dc;
    Value fn = sendable::deserialize(sclosure, *child, base, dc);
    std::vector<Value> args;
    args.reserve(sargs.size());
    for (const auto& sa : sargs)
      args.push_back(sendable::deserialize(sa, *child, base, dc));
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
      sendable::contains_closure(n) ? culebra::environment({}) : nullptr;
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
  if (core.thrown) throw _isolate_deser_parent(*core.thrown);
  if (core.ran_inline) return core.inline_result;
  return _isolate_deser_parent(core.result);
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
              // No interrupt flag yet (next cycle): wait for completion, then
              // join so no thread is left detached.
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
