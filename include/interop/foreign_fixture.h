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

#include <algorithm>  // std::min
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <interop/search_splitter.h>

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

  // Wrap.h Gap A/B fixture: a handle argument borrows (merge/steal/
  // add_maybe never take ownership of `o`), and bump_by/scale exercise a
  // trailing optional.
  void merge(const Counter& o) { value_ += o.value_; }        // const U& — no bump on o
  void steal(Counter& o) {                                    // U& — bumps o
    value_ += o.value_;
    o.value_ = 0;
  }
  void add_maybe(Counter* o) {                                // U* — nil accepted
    if (o) value_ += o->value_;
  }
  void bump_by(int64_t n) { value_ += n; }                    // trailing default
  void scale(int64_t k, int64_t off) { value_ = value_ * k + off; }

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

  // Gap A on a second wrapped class: `other` is a handle argument of a
  // DIFFERENT wrapped type than the receiver, so its bump is independent of
  // whatever the receiver's own const-ness does.
  void poke(Box& other) { other.reset(0); }         // Box& — bumps other
  void peek_at(const Box&) const {}                 // const Box& — no bump

 private:
  Counter inner_;
};

// A splitter for Search (interop/search_splitter.h): cuts on ASCII whitespace
// and emits a crude stem -- lowercase, a trailing -ing or -s removed -- while
// every range stays on the surface word. With `overlap`, each word is followed
// by two more terms: "stacked" over its first byte, which starts where the
// word started and so is an alternative at the word's position, and "behind"
// over the byte before it, which starts before the word -- a contract
// violation the engine has to drop rather than index. Deliberately nothing of
// culebra's beyond the contract header, since that is what a user's class
// includes.
class Splitter : public search::ISplitter {
 public:
  explicit Splitter(bool overlap) : overlap_(overlap) {}

  size_t split(std::string_view text, size_t offset,
               const search::SplitEmit& emit) const override {
    constexpr std::string_view ws = " \n\t\r";
    for (size_t i = text.find_first_not_of(ws, offset);
         i != std::string_view::npos; i = text.find_first_not_of(ws, i)) {
      size_t end = std::min(text.find_first_of(ws, i), text.size());
      emit(stem(text.substr(i, end - i)), i, end - i);
      if (overlap_) {
        emit("stacked", i, 1);
        if (i > 0) emit("behind", i - 1, 1);
      }
      i = end;
    }
    return text.size() - offset;
  }

 private:
  static std::string stem(std::string_view word) {
    std::string s(word);
    for (auto& c : s) {
      if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (s.size() > 5 && s.ends_with("ing")) {
      s.resize(s.size() - 3);
    } else if (s.size() > 3 && s.ends_with("s")) {
      s.pop_back();
    }
    return s;
  }

  bool overlap_;
};

}  // namespace culebra::foreign_fixture
