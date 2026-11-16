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

// SourceIsolateRun (sendable.h).
inline void run_source_isolate(IsolateCore& core, std::string source,
                               std::string name,
                               std::vector<sendable::SendNode> argv,
                               bool counted) {
  culebra::Runtime rt(&core.interrupt);
  culebra::RuntimeScope scope(rt);
  {
    Session units;
    // Whether the programs are still there to ask: once they go, any function
    // the executor runs counts as the source's.
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto origin = std::make_shared<SourceOrigin>();
    origin->argv = std::move(argv);
    origin->owns = [&units, alive](const JitClosure* c) {
      if (c->fn_ptr != reinterpret_cast<void*>(&Exec::trampoline))
        return false;
      if (!alive->load(std::memory_order_acquire)) return true;
      const VmFnDesc* d = Exec::closure_desc(const_cast<JitClosure*>(c));
      return d && units.retains(d->prog);
    };
    set_source_origin(rt, origin);
    {
      // The source's top-level bindings. They go before the programs, since
      // releasing one can run the source's code (a `drop`, an isolate handle
      // joining one the source spawned).
      ReplSessionSwap cells;
      _jit_isolate_settle_child(core, origin.get(), [&] {
        return JitOwnedVal(_run_source_unit(units, std::move(source), name));
      });
    }
    alive->store(false, std::memory_order_release);
  }
  if (counted) g_live_isolates().fetch_sub(1, std::memory_order_relaxed);
}

// Lets this process run Isolate.spawn_source. Each host that carries the
// compiler calls it where it sets up the stdlib (the CLI's main, Embed, an
// AOT binary through culebra_rt_source); the parse the source goes through
// expands `let {*} = NS`, so the namespace members come with it.
inline void install_source_isolate() {
  install_namespace_members();
  source_isolate_run() = &run_source_isolate;
}

}  // namespace culebra::vm
