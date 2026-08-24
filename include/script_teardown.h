#pragma once

// The teardown a top-level run owes before static destruction: join every
// still-outstanding isolate (whichever engine registered them — each
// engine's walk is installed into its own hook and is a no-op for the
// other's empty registry) and close the FS.watch handles. Engine-neutral;
// the tree-walker's interpret_modules, the REPL loop, the doctest runner
// and the VM script lane all scope one of these around a run. (Split out
// of interpreter.h in Phase 4 B7-b.)

#include <fswatcher.h>
#include <shared.h>  // the teardown join hooks

namespace culebra {

struct ScriptTeardownGuard {
  ~ScriptTeardownGuard() {
    if (auto& fn = interp_isolate_teardown_join_hook()) fn();
    if (auto& fn = isolate_teardown_join_hook()) fn();
    fswatch::fs_watch_close_all();
  }
};

}  // namespace culebra
