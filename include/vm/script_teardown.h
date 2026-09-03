#pragma once

// The teardown a top-level run owes before static destruction: join every
// still-outstanding isolate and close the FS.watch handles. The script
// lanes, the REPL loop and the doctest runner all scope one of these
// around a run.

#include <stdlib/fswatcher.h>
#include <base/shared.h>  // the teardown join hook

namespace culebra {

struct ScriptTeardownGuard {
  ~ScriptTeardownGuard() {
    if (auto& fn = isolate_teardown_join_hook()) fn();
    fswatch::fs_watch_close_all();
  }
};

}  // namespace culebra
