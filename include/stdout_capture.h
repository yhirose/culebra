#pragma once

// Where a running program's output goes, and the two ways of reading it back.
//
// `program_out()` is the stream `IO.print` / `println` / `inspect` write to:
// std::cout, unless the thread running them is inside an `IO.capture`, in
// which case its innermost buffer. Per thread, because the answer is a value
// the program reads: two threads capturing at once must not take each other's
// output, and the process-wide `std::cout.rdbuf()` swap below cannot say that
// — the two restores race, and the loser leaves std::cout pointing at a
// destroyed buffer.
//
// The runners keep the swap: a report of what a test printed wants everything
// the process wrote, an isolate's share of it included. The two compose —
// output no capture claims reaches std::cout, whose buffer is the runner's.

#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

namespace culebra {

// This thread's capture stack. Function-local so that no "TLS init function"
// symbol lands in the runtime archives (tools/check_rt_archive_tls.sh).
inline std::vector<std::ostream*>& capture_stack() {
  static thread_local std::vector<std::ostream*> stack;
  return stack;
}

inline std::ostream& program_out() {
  auto& s = capture_stack();
  return s.empty() ? std::cout : *s.back();
}

// RAII: everything this thread prints lands in `take()`'s string until the
// capture ends — which it does even if the program throws out of it. `take()`
// is idempotent; a second call returns "".
class ProgramOutCapture {
  std::ostringstream buf_;
  bool active_ = true;

 public:
  ProgramOutCapture() { capture_stack().push_back(&buf_); }
  ~ProgramOutCapture() { end(); }
  ProgramOutCapture(const ProgramOutCapture&) = delete;
  ProgramOutCapture& operator=(const ProgramOutCapture&) = delete;

  std::string take() {
    if (!active_) return {};
    end();
    return buf_.str();
  }

 private:
  // Nesting on one thread is a stack, so this is always the top entry.
  void end() {
    if (!active_) return;
    capture_stack().pop_back();
    active_ = false;
  }
};

// RAII redirect of `std::cout` into an internal stringstream. The dtor
// restores the original rdbuf even if the consumer never calls `take()`
// (e.g. an unexpected exception type escaped the catch). `take()` is
// idempotent — repeated calls return "" after the first. The runners' own,
// held around one test at a time on the thread that runs the suite.
class StdoutCapture {
  std::stringstream buf_;
  std::streambuf* old_ = nullptr;

 public:
  explicit StdoutCapture(bool active) {
    if (active) old_ = std::cout.rdbuf(buf_.rdbuf());
  }
  ~StdoutCapture() {
    if (old_) std::cout.rdbuf(old_);
  }
  StdoutCapture(const StdoutCapture&) = delete;
  StdoutCapture& operator=(const StdoutCapture&) = delete;

  std::string take() {
    if (!old_) return {};
    std::cout.rdbuf(old_);
    old_ = nullptr;
    return buf_.str();
  }
};

}  // namespace culebra
