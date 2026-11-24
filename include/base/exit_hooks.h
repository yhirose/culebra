#pragma once

// How the process ends. What it puts away first (a window, an audio device, a
// compile thread, the terminal's mode) is armed here once it exists and run by
// run_exit_hooks(), which every way the process ends calls: Sys.exit through
// end_process(), and the driver's main and the AOT bootstrap before they
// return. A hook runs while every static it reaches is still alive.
//
// Sys.exit then ends the process without destroying statics, as Java's
// System.exit runs its shutdown hooks and halts: another thread may still be
// running the program (an isolate that has not reached its own Sys.exit), and
// std::exit would destroy what that thread is using under it. std::atexit is
// not used for the same reason: its handlers run among the destructors of
// statics, in the reverse of the order each was registered or built
// (tools/checks/check_exit_hook_door.sh).

#include <base/stdout_capture.h>  // flush_own_output

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#ifdef CULEBRA_COVERAGE
extern "C" void __gcov_dump();
#endif

namespace culebra {

namespace _exit_hooks {
struct List {
  std::mutex mu;
  std::vector<void (*)()> armed;
  std::thread::id runner;  // set once the hooks start
};
inline List& list() {
  static List l;
  return l;
}
}  // namespace _exit_hooks

// Arms `fn` once, however often it is asked. The last armed runs first, as
// what comes up later may rest on what came up before it.
inline void arm_exit_hook(void (*fn)()) {
  auto& l = _exit_hooks::list();
  std::lock_guard lock(l.mu);
  if (l.runner != std::thread::id()) return;
  for (auto* f : l.armed)
    if (f == fn) return;
  l.armed.push_back(fn);
}

// Runs the armed hooks, once per process. A second thread ending the process
// meanwhile (an isolate's Sys.exit beside the main thread's) waits here for
// the first to end it, as a second System.exit blocks in Java, rather than
// tear down beside it; a hook that ends the process itself returns.
inline void run_exit_hooks() {
  auto& l = _exit_hooks::list();
  std::unique_lock lock(l.mu);
  if (l.runner != std::thread::id()) {
    if (l.runner == std::this_thread::get_id()) return;
    lock.unlock();
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
  }
  l.runner = std::this_thread::get_id();
  while (!l.armed.empty()) {
    auto* fn = l.armed.back();
    l.armed.pop_back();
    lock.unlock();
    fn();
    lock.lock();
  }
}

// Sys.exit's end: the hooks, then what is buffered for the standard streams,
// then the process, with every static left as it is.
[[noreturn]] inline void end_process(int code) {
  run_exit_hooks();
  flush_own_output();
#ifdef CULEBRA_COVERAGE
  __gcov_dump();
#endif
  std::_Exit(code);
}

}  // namespace culebra
