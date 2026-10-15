#pragma once

// The Shared.new frozen-tree core: the process-wide registry of frozen trees
// and its refcounting. Engine-neutral (the tree is plain arrays, frozen_tree.h);
// what makes one of a value, and reads it back, is the runtime's
// (conc/sendable.h).

#include <conc/frozen_tree.h>
#include <conc/send_node.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>

namespace culebra {

using sendable::SendNode;

struct SharedValCore {
  FrozenTree tree;
  // Live count: one per Runtime's hold on the tree (conc/sendable.h's
  // JitSvHold, which the views of that Runtime count on in turn), and one per
  // view a message in flight carries. Whoever adds to it holds a count
  // already, so it is never added to at zero and needs no lock; the
  // registry's mutex covers the map alone.
  mutable std::atomic<int64_t> refcount{0};
};

inline std::mutex& shared_val_mutex() {
  static std::mutex m;
  return m;
}
inline std::map<long, std::shared_ptr<SharedValCore>>& shared_val_registry() {
  static std::map<long, std::shared_ptr<SharedValCore>> r;
  return r;
}
inline std::atomic<long>& shared_val_next_id() {
  static std::atomic<long> n{1};
  return n;
}
// A count on the tree `id` names, taken by id: what a message being rebuilt
// takes for the view it makes (the sender's in-flight count keeps the tree
// there meanwhile). Null when the tree is gone.
inline SharedValCore* shared_val_acquire(int64_t id) {
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  auto it = shared_val_registry().find(id);
  if (it == shared_val_registry().end()) return nullptr;
  it->second->refcount.fetch_add(1, std::memory_order_relaxed);
  return it->second.get();
}
// Give a count back, by the tree (a Runtime's hold). The last one takes the
// tree out of the registry, releasing it outside the lock; a count taken by
// id in between leaves it there.
inline void shared_val_release(const SharedValCore& core, int64_t id) {
  if (core.refcount.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  std::shared_ptr<SharedValCore> doomed;
  {
    std::lock_guard<std::mutex> lock(shared_val_mutex());
    auto it = shared_val_registry().find(id);
    if (it == shared_val_registry().end()) return;
    if (it->second->refcount.load(std::memory_order_acquire) > 0) return;
    doomed = std::move(it->second);
    shared_val_registry().erase(it);
  }
}
// One more count on a tree the caller holds a count on already, for a message
// to carry (SendNode::held): given back when the message goes.
inline std::shared_ptr<void> shared_val_held(const SharedValCore& core,
                                             int64_t id) {
  core.refcount.fetch_add(1, std::memory_order_relaxed);
  return {const_cast<SharedValCore*>(&core), [id](void* p) {
            shared_val_release(*static_cast<SharedValCore*>(p), id);
          }};
}

// Why a value that can be sent still cannot be frozen, or null: Shared
// narrows what the serializer accepts to data alone. No functions, and no
// handle references (a frozen tree must not own channel / SharedBuffer /
// Shared refcounts).
inline const char* _shared_val_reject_reason(const SendNode& n) {
  switch (n.kind) {
    case SendNode::K::Closure:
      return "functions cannot be shared (Shared values are data only)";
    case SendNode::K::Channel:
    case SendNode::K::SharedBuffer:
    case SendNode::K::SharedVal:
      return "handles cannot be nested in a Shared value";
    default:
      break;
  }
  for (const auto& e : n.elems) {
    if (auto* r = _shared_val_reject_reason(e)) return r;
  }
  for (const auto& [k, v] : n.entries) {
    if (auto* r = _shared_val_reject_reason(k)) return r;
    if (auto* r = _shared_val_reject_reason(v)) return r;
  }
  return nullptr;
}

// Put a frozen tree in the registry. Returns its id and its core; the
// creator's handle owns the initial count.
inline std::pair<int64_t, SharedValCore*> shared_val_register(FrozenTree&& tree) {
  auto core = std::make_shared<SharedValCore>();
  core->tree = std::move(tree);
  core->refcount = 1;
  int64_t id = shared_val_next_id()++;
  auto* made = core.get();
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  shared_val_registry().emplace(id, std::move(core));
  return {id, made};
}

}  // namespace culebra
