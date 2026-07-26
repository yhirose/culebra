#pragma once
// Foreign instances — wrapped C++ objects (Phase 3).
//
// A wrapped C++ object lives in a per-Runtime, per-type id table; the
// script-visible handle is an ordinary drop-having Object carrying the
// id, so the whole Phase 1/2 lifetime machinery (owned stack,
// deterministic scope-exit drop, cycles, GC backstop, exactly-once)
// applies to it unmodified. The two-layer split falls out of the
// table representation directly:
//
//   drop event   = erase the table entry -> ~T() runs NOW (or the
//                  shared_ptr's count drops). Idempotent: a second
//                  erase of the same id is a no-op.
//   memory event = the handle Object itself is reclaimed by the GC
//                  whenever its references end — "dropped but still
//                  addressable" is a legal state.
//   closed check = a method finding no entry for its id raises
//                  ClosedError instead of touching freed memory: the
//                  single user-visible safety check.
//
// Ownership shapes a binding maps return values through:
//   T (by value)   -> adopt(make_unique<T>(std::move(v)))  — culebra owns
//   unique_ptr<T>  -> adopt(std::move(p))                  — culebra owns
//   shared_ptr<T>  -> adopt_shared(std::move(p))           — one share held
//
// The table is keyed per C++ type via a type-erased registry in the
// Runtime (kSlotForeignTables), so isolates get independent instances
// and teardown happens with the Runtime.

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <shared.h>

namespace culebra::foreign {

template <class T>
class ForeignTable {
 public:
  // Adopt sole ownership (by-value and unique_ptr returns).
  int64_t adopt(std::unique_ptr<T> p) {
    int64_t id = next_id_++;
    entries_.emplace(id, Entry{std::move(p), nullptr});
    return id;
  }
  // Hold one share (shared_ptr returns).
  int64_t adopt_shared(std::shared_ptr<T> p) {
    int64_t id = next_id_++;
    entries_.emplace(id, Entry{nullptr, std::move(p)});
    return id;
  }

  // Live instance for `id`, or nullptr once dropped (the ClosedError
  // trigger — never a dangling pointer).
  T* get(int64_t id) {
    auto it = entries_.find(id);
    if (it == entries_.end()) return nullptr;
    return it->second.owned ? it->second.owned.get()
                            : it->second.shared.get();
  }

  // Generation counter: non-const method dispatch bumps it;
  // borrows snapshot it at creation and go stale on mismatch. -1 means
  // the entry is closed — a snapshot is never negative, so one compare
  // covers both "parent dropped" and "parent mutated".
  int64_t gen_of(int64_t id) {
    auto it = entries_.find(id);
    return it == entries_.end() ? -1 : it->second.gen;
  }
  void bump_gen(int64_t id) {
    auto it = entries_.find(id);
    if (it != entries_.end()) it->second.gen++;
  }

  // The drop event: destroy (or release) the instance. Idempotent.
  void erase(int64_t id) { entries_.erase(id); }

  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    std::unique_ptr<T> owned;
    std::shared_ptr<T> shared;
    int64_t gen = 0;
  };
  std::unordered_map<int64_t, Entry> entries_;
  int64_t next_id_ = 1;
};

// Type-erased per-Runtime registry: one ForeignTable<T> per wrapped T.
struct ForeignTables {
  std::map<std::type_index, std::shared_ptr<void>> tables;
  // Unique per registry instance, so the per-T thread_local memo in
  // table() can never alias across RuntimeScope swaps, Runtime address
  // reuse, or the lazy revival of an empty registry during ~Runtime
  // teardown.
  const uint64_t gen = next_gen();
  static uint64_t next_gen() {
    static std::atomic<uint64_t> g{1};
    return g.fetch_add(1, std::memory_order_relaxed);
  }
};

template <class T>
inline ForeignTable<T>& table() {
  auto& reg = runtime_substate<ForeignTables>(kSlotForeignTables);
  // Every method call resolves its table; memoize the type-index walk
  // behind the registry's generation stamp (File gets the same effect
  // from its dedicated RuntimeSlot).
  static thread_local uint64_t cached_gen = 0;
  static thread_local ForeignTable<T>* cached = nullptr;
  if (cached_gen != reg.gen) {
    auto& slot = reg.tables[std::type_index(typeid(T))];
    if (!slot) slot = std::make_shared<ForeignTable<T>>();
    cached = static_cast<ForeignTable<T>*>(slot.get());
    cached_gen = reg.gen;
  }
  return *cached;
}

// Method-entry guard: the live instance, or the safety error. The
// wording is shared by both backends so messages stay symmetric.
template <class T>
inline T* get_or_throw(int64_t id, std::string_view type_name, long line,
                       long col) {
  T* p = table<T>().get(id);
  if (!p) {
    throw CulebraError(
        "ClosedError",
        std::string(type_name) + " instance was dropped", line, col);
  }
  return p;
}

// Type-erased per-T state read for the borrow validation chain:
// the current generation, or -1 when the entry is closed.
template <class T>
inline int64_t entry_state(int64_t id) {
  return table<T>().gen_of(id);
}

// Registry of the entry_state<T> functions, indexed by a small id. An
// owning handle carries the INDEX (not the raw pointer) so a borrow can
// read its parent's generation without knowing the parent's C++ type —
// and a forged index is a bounds miss, never a called pointer. (Storing
// the raw function pointer in a script-writable slot would let a forged
// value be called: the function-pointer twin of the _ptr hole.)
inline std::vector<int64_t (*)(int64_t)>& state_fns() {
  static std::vector<int64_t (*)(int64_t)> v;
  return v;
}
template <class T>
inline int64_t state_fn_id() {
  static const int64_t id = [] {
    state_fns().push_back(&entry_state<T>);
    return static_cast<int64_t>(state_fns().size() - 1);
  }();
  return id;
}
inline int64_t owner_gen_via(int64_t state_id, int64_t owner_id) {
  auto& v = state_fns();
  if (state_id < 0 || static_cast<size_t>(state_id) >= v.size()) return -1;
  return v[state_id](owner_id);
}

// The single user-visible borrow failure (closed, one check): both
// "parent dropped" and "parent mutated since the borrow" land here, on
// both backends, byte-identically.
[[noreturn]] inline void throw_borrow_invalid(std::string_view type_name,
                                              long line, long col) {
  throw CulebraError(
      "ClosedError",
      std::string(type_name) +
          " borrow is no longer valid (its owner was dropped or mutated)",
      line, col);
}

// Borrow registry. A borrowing handle stores only an opaque id;
// the raw pointer and the parent link live HERE, in C++ memory a script
// can't forge — so validation and dereference never trust a
// script-writable slot, the same invariant owning handles get from the
// per-T foreign table. An entry is erased when its handle is reclaimed
// (the handle's internal drop), so the table tracks live borrows only.
struct BorrowEntry {
  void* ptr;
  // Exactly one parent link resolves the parent's current generation:
  // an owning parent through its state-fn id + table id, or a borrowing
  // parent (chained borrow) through its own id.
  int64_t parent_state_id;  // -1 ⇒ parent is itself a borrow
  int64_t parent_owner_id;
  int64_t parent_bid;       // valid when parent_state_id < 0
  int64_t pgen;             // parent's generation snapshotted at creation
  int64_t gen = 0;          // this borrow's own generation (stales children)
};
struct BorrowTable {
  std::unordered_map<int64_t, BorrowEntry> entries;
  int64_t next_id = 1;
};
inline BorrowTable& borrow_table() {
  return runtime_substate<BorrowTable>(kSlotBorrowTable);
}

inline int64_t borrow_gen(int64_t bid);  // fwd (recursion through chains)

// True iff the borrow `bid` is still valid: its entry exists and its
// parent is still valid AND unmutated since the snapshot. A snapshot is
// never negative, so the one compare covers a dropped parent too.
inline bool borrow_valid(int64_t bid) {
  auto& t = borrow_table();
  auto it = t.entries.find(bid);
  if (it == t.entries.end()) return false;
  const auto& e = it->second;
  int64_t parent_gen =
      e.parent_state_id >= 0 ? owner_gen_via(e.parent_state_id, e.parent_owner_id)
                             : borrow_gen(e.parent_bid);
  return parent_gen >= 0 && parent_gen == e.pgen;
}
inline int64_t borrow_gen(int64_t bid) {
  auto& t = borrow_table();
  auto it = t.entries.find(bid);
  if (it == t.entries.end() || !borrow_valid(bid)) return -1;
  return it->second.gen;
}
inline int64_t borrow_adopt(void* ptr, int64_t parent_state_id,
                            int64_t parent_owner_id, int64_t parent_bid,
                            int64_t pgen) {
  auto& t = borrow_table();
  int64_t bid = t.next_id++;
  t.entries.emplace(bid, BorrowEntry{ptr, parent_state_id, parent_owner_id,
                                     parent_bid, pgen, 0});
  return bid;
}
inline void* borrow_ptr(int64_t bid) {
  auto& t = borrow_table();
  auto it = t.entries.find(bid);
  return it == t.entries.end() ? nullptr : it->second.ptr;
}
inline void borrow_bump(int64_t bid) {
  auto& t = borrow_table();
  auto it = t.entries.find(bid);
  if (it != t.entries.end()) it->second.gen++;
}
inline void borrow_erase(int64_t bid) { borrow_table().entries.erase(bid); }

}  // namespace culebra::foreign
