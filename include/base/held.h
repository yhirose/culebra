#pragma once

// A native frame's hold on an object some table owns (an IdRegistry slot, a
// wrapped instance's foreign-table entry), for as long as the frame keeps it:
// a frame that calls program code with the object in hand keeps that code
// from closing or dropping it, since the owner refuses while any Hold counts
// on the object. Not a GC pin (Heap::pin): it keeps the object from being
// closed, not from being collected. Header-only and value-neutral, like
// id_registry.h.

#include <cstdint>
#include <type_traits>
#include <utility>

namespace culebra {

// One count on an object's use counter, given back when the Hold goes. The
// counter's address has to stay put while any Hold points at it.
class Hold {
 public:
  Hold() = default;
  explicit Hold(uint32_t* users) : users_(users) {
    if (users_) ++*users_;
  }
  Hold(Hold&& o) noexcept : users_(std::exchange(o.users_, nullptr)) {}
  Hold(const Hold&) = delete;
  Hold& operator=(const Hold&) = delete;
  Hold& operator=(Hold&&) = delete;
  ~Hold() {
    if (users_) --*users_;
  }

 private:
  uint32_t* users_ = nullptr;
};

// An object and the Hold that keeps it: the form the tables hand a native
// frame, so no frame can reach an object without holding it. Deliberately not
// convertible to T*: a pointer kept past the Held would be a use the Hold no
// longer covers.
template <class T>
class Held {
 public:
  Held() = default;
  Held(T* p, Hold h) : p_(p), hold_(std::move(h)) {}
  template <class U>
    requires std::is_convertible_v<U*, T*>
  Held(Held<U>&& o) : p_(o.get()), hold_(std::move(o).hold()) {}
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  T& operator*() const { return *p_; }
  explicit operator bool() const { return p_ != nullptr; }
  Hold&& hold() && { return std::move(hold_); }

 private:
  T* p_ = nullptr;
  Hold hold_;
};

}  // namespace culebra
