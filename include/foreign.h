#pragma once
// Foreign instances — wrapped C++ objects (design §9 / §10, Phase 3).
//
// A wrapped C++ object lives in a per-Runtime, per-type id table; the
// script-visible handle is an ordinary drop-having Object carrying the
// id, so the whole Phase 1/2 lifetime machinery (owned stack,
// deterministic scope-exit drop, cycles, GC backstop, exactly-once)
// applies to it unmodified. The two-layer split of §9 falls out of the
// table representation directly:
//
//   drop event   = erase the table entry -> ~T() runs NOW (or the
//                  shared_ptr's count drops). Idempotent: a second
//                  erase of the same id is a no-op.
//   memory event = the handle Object itself is reclaimed by the GC
//                  whenever its references end — "dropped but still
//                  addressable" is a legal state (§7).
//   closed check = a method finding no entry for its id raises
//                  ClosedError instead of touching freed memory: the
//                  single user-visible safety check of §7.
//
// Ownership shapes a binding maps return values through (§10.3):
//   T (by value)   -> adopt(make_unique<T>(std::move(v)))  — culebra owns
//   unique_ptr<T>  -> adopt(std::move(p))                  — culebra owns
//   shared_ptr<T>  -> adopt_shared(std::move(p))           — one share held
//
// The table is keyed per C++ type via a type-erased registry in the
// Runtime (kSlotForeignTables), so isolates get independent instances
// and teardown happens with the Runtime.

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <typeindex>

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

  // The drop event: destroy (or release) the instance. Idempotent.
  void erase(int64_t id) { entries_.erase(id); }

  size_t size() const { return entries_.size(); }

 private:
  struct Entry {
    std::unique_ptr<T> owned;
    std::shared_ptr<T> shared;
  };
  std::map<int64_t, Entry> entries_;
  int64_t next_id_ = 1;
};

// Type-erased per-Runtime registry: one ForeignTable<T> per wrapped T.
struct ForeignTables {
  std::map<std::type_index, std::shared_ptr<void>> tables;
};

template <class T>
inline ForeignTable<T>& table() {
  auto& reg = runtime_substate<ForeignTables>(kSlotForeignTables);
  auto& slot = reg.tables[std::type_index(typeid(T))];
  if (!slot) slot = std::make_shared<ForeignTable<T>>();
  return *static_cast<ForeignTable<T>*>(slot.get());
}

// Method-entry guard: the live instance, or the §7 safety error. The
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

}  // namespace culebra::foreign
