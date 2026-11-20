#pragma once

// Isolate.spawn_source's thread. The source compiles on the bytecode compiler
// and runs on the executor whichever lane spawned it: an AOT binary carries
// no LLVM, so the one engine every lane can run it on is what makes them
// answer alike. The session's programs are the source's code, and go when the
// isolate ends; SourceOrigin::owns keeps a value that carries them from
// leaving (sendable.h).

#include <conc/sendable.h>
#include <frontend/effects_transform.h>  // apply_transforms
#include <frontend/parser.h>             // parse, ParseFailure
#include <stdlib/preamble.h>             // install_namespace_members
#include <vm/session.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace culebra::vm {

// The value the source ends with, owned; what it raises or throws goes on as
// raised or thrown, to _jit_isolate_settle_child.
inline JitValue _run_source_unit(Session& units, std::string source,
                                 const std::string& name) {
  std::vector<std::string> msgs;
  if (!units.run_builtin_traits(msgs)) {
    // The traits preamble is baked into the binary; failing to run it is a
    // build bug, not something the source did.
    throw CulebraError("InternalError",
                       "built-in traits failed: " + msgs.front());
  }
  // The session owns the text the AST's tokens view into.
  auto owned = std::make_shared<std::string>(std::move(source));
  std::vector<ParseFailure> failures;
  auto ast = parse(name, *owned, failures);
  if (!ast) {
    const auto& f = failures.front();
    throw CulebraError("SyntaxError", f.message, static_cast<int64_t>(f.line),
                       static_cast<int64_t>(f.col));
  }
  ast = apply_transforms(ast, name, *owned);
  if (!units.run_stdlib_delta(*ast, msgs)) {
    if (auto e = units.take_last_error()) throw *e;
    throw CulebraError("InternalError", msgs.front());
  }
  return units.run_input_carrying(ast, std::move(owned));
}

// The source's Sys.argv for as long as it runs: the arguments rebuilt once,
// as Isolate.spawn's child rebuilds its own, so that the message lets go of
// what it held (a channel's counts) and only the values do. Each read is a
// fresh Array of them, as the process's Sys.argv is.
class SourceArgv {
 public:
  explicit SourceArgv(std::vector<sendable::SendNode> argv) {
    auto* r = culebra_runtime_array_new_reserved(
        static_cast<int64_t>(argv.size()));
    held_ = JitOwnedVal(JitValue{TAG_ARRAY, reinterpret_cast<int64_t>(r)});
    // One message, as the spawn serialized it: a class the second argument
    // shares with the first arrives as a reference back to it.
    JitDeCtx dc;
    for (const auto& a : argv) {
      JitValue v = jit_deserialize(a, dc).consume();
      culebra_runtime_array_push(r, v.tag, v.data);  // absorbs the +1
    }
    // Reached from a std::function from here on, which no collection scans.
    _gc_heap().pin(r);
    current_source_slot().argv = [r] {
      return culebra_runtime_array_slice(r, 0, static_cast<int64_t>(r->size));
    };
  }
  ~SourceArgv() {
    current_source_slot().argv = nullptr;
    _gc_heap().unpin(reinterpret_cast<void*>(held_.borrow().data));
  }
  SourceArgv(const SourceArgv&) = delete;
  SourceArgv& operator=(const SourceArgv&) = delete;

 private:
  JitOwnedVal held_{TAG_NIL, 0};
};

// SourceIsolateRun (sendable.h).
inline void run_source_isolate(IsolateCore& core, std::string source,
                               std::string name,
                               std::vector<sendable::SendNode> argv,
                               bool counted) {
  culebra::Runtime rt(&core.interrupt, core.random_seed);
  culebra::RuntimeScope scope(rt);
  {
    // Owned by the origin, which every Runtime that can run the source's
    // code holds: the programs outlive this thread when an isolate the source
    // spawned does.
    auto units = std::make_shared<Session>();
    auto origin = std::make_shared<SourceOrigin>();
    origin->code = units;
    // Raw: the origin owns the session, and only a holder of the origin asks.
    origin->owns = [units = units.get()](const JitClosure* c) {
      const VmFnDesc* d = Exec::closure_desc(const_cast<JitClosure*>(c));
      return d && units->retains(d->prog);
    };
    origin->hooks.is_builtin_var = &JitExtension::is_builtin_var;
    set_source_origin(rt, origin);
    // The source's top-level bindings, released on this thread while its
    // Runtime is current: releasing one can run the source's code (a `drop`).
    ReplSessionSwap cells;
    _jit_isolate_settle_child(core, origin.get(), [&] {
      SourceArgv args(std::move(argv));
      return JitOwnedVal(_run_source_unit(*units, std::move(source), name));
    });
  }
  if (counted) g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
}

// Lets this process run Isolate.spawn_source. Each host that carries the
// compiler calls it where it sets up the stdlib (the CLI's main, Embed, an
// AOT binary through culebra_rt_source). What compiling a source needs comes
// with it, since a built binary compiles nothing else: the namespace members
// the parse expands `let {*} = NS` over (the builtin names ride the origin).
inline void install_source_isolate() {
  install_namespace_members();
  source_isolate_run() = &run_source_isolate;
}

}  // namespace culebra::vm
