#pragma once
// The Foreign-binding PoC fixture (design §14.5): a deliberately plain,
// header-only C++ class with everything Phase 3's hand-written binding
// must exercise end to end — an observable constructor/destructor pair,
// const and mutating methods over primitive and string types, and the
// three return-value ownership shapes (§10.3): by value, unique_ptr,
// shared_ptr. It stands in for "the user's own C++ asset"; the binding
// that wraps it (stdlib_interp.h / stdlib_jit.h, `__Foreign` namespace)
// is the shape Phase 4's codegen will emit.
//
// `live()` counts constructed-but-not-destroyed instances, so scripts
// can assert exactly when ~Counter ran — the drop/`closed` semantics
// oracle. Thread-local: isolates each get their own count, and the
// difftest harness can compare it across backends byte-for-byte.

#include <memory>
#include <string>

namespace culebra::foreign_fixture {

class Counter {
 public:
  explicit Counter(long start) : value_(start) { ++live_; }
  Counter(const Counter& o) : value_(o.value_) { ++live_; }
  ~Counter() { --live_; }

  long value() const { return value_; }
  void add(long n) { value_ += n; }
  std::string label() const { return "Counter(" + std::to_string(value_) + ")"; }

  // The three §10.3 return-ownership shapes.
  Counter clone() const { return Counter(value_); }            // by value
  std::unique_ptr<Counter> fork() const {                      // unique_ptr
    return std::make_unique<Counter>(value_);
  }
  std::shared_ptr<Counter> share() {                           // shared_ptr
    if (!shared_self_) shared_self_ = std::make_shared<Counter>(value_);
    return shared_self_;
  }

  static long live() { return live_; }

 private:
  long value_;
  std::shared_ptr<Counter> shared_self_;
  static inline thread_local long live_ = 0;
};

}  // namespace culebra::foreign_fixture
