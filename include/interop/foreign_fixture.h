#pragma once
// The Foreign-binding PoC fixture: a deliberately plain,
// header-only C++ class with everything Phase 3's hand-written binding
// must exercise end to end — an observable constructor/destructor pair,
// const and mutating methods over primitive and string types, a body
// that throws, and the three return-value ownership shapes: by value,
// unique_ptr, shared_ptr. It stands in for "the user's own C++ asset";
// the binding that wraps it (foreign_binding.h, `__Foreign`
// namespace) is the shape Phase 4's codegen will emit.
//
// `live()` counts constructed-but-not-destroyed instances, so scripts
// can assert exactly when ~Counter ran — the drop/`closed` semantics
// oracle. Thread-local: isolates each get their own count, and the
// difftest harness can compare it across backends byte-for-byte.

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>

namespace culebra::foreign_fixture {

class Counter {
 public:
  explicit Counter(int64_t start) : value_(start) { ++live_; }
  Counter(const Counter& o) : value_(o.value_) { ++live_; }
  ~Counter() { --live_; }

  int64_t value() const { return value_; }
  void add(int64_t n) { value_ += n; }
  void divide(int64_t n) {  // the owning shape of a throwing body
    if (n == 0) throw std::invalid_argument("divide by zero");
    value_ /= n;
  }
  std::string label() const { return "Counter(" + std::to_string(value_) + ")"; }

  // The three return-ownership shapes.
  Counter clone() const { return Counter(value_); }            // by value
  std::unique_ptr<Counter> fork() const {                      // unique_ptr
    return std::make_unique<Counter>(value_);
  }
  std::shared_ptr<Counter> share() {                           // shared_ptr
    if (!shared_self_) shared_self_ = std::make_shared<Counter>(value_);
    return shared_self_;
  }

  static int64_t live() { return live_; }

 private:
  int64_t value_;
  std::shared_ptr<Counter> shared_self_;
  static inline thread_local int64_t live_ = 0;
};

// Phase 5 fixture: a class whose methods BORROW — `inner()`
// returns a reference into self, so the wrap layer must produce a
// borrowing handle (parent `closed` + generation checks) rather than an
// owning copy. `reset` is the non-const mutation that bumps the
// generation; `peek` is const (borrows survive it); `touch` is
// non-const but declared preserves_borrows at the binding; `slot` is the
// borrowing shape of a body that throws.
class Box {
 public:
  explicit Box(int64_t start) : inner_(start) {}

  Counter& inner() { return inner_; }
  const Counter& read_inner() const { return inner_; }
  Counter& slot(int64_t i) {
    if (i != 0) throw std::out_of_range("no such slot");
    return inner_;
  }
  int64_t peek() const { return inner_.value(); }
  void reset(int64_t v) { inner_ = Counter(v); }
  void touch() {}  // non-const, but harmless — preserves_borrows

 private:
  Counter inner_;
};

}  // namespace culebra::foreign_fixture
