#pragma once

// The Shared.new frozen-tree core: the process-wide registry of frozen
// SendNode trees, its refcounting (bump/drop), the flattened index, and
// freeze_shared_val. Engine-neutral (SendNode only); the interp's view
// handles stay in sharedval.h, the compiled lanes' live in jit_runtime /
// sendable_jit. Moved verbatim from sharedval.h (Phase 4 B7-b).

#include <conc/send_node.h>
#include <base/shared.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace culebra {

using sendable::SendNode;


// Whether a frozen key is made of nothing but scalars, Strings and Tuples of
// those: the keys the tree can hash and compare as nodes. One that is not (an
// enum variant) is matched as the value it is, by the reader that holds one.
inline bool shared_key_plain(const SendNode& k) {
  using K = SendNode::K;
  switch (k.kind) {
    case K::Nil: case K::Bool: case K::Long: case K::Float: case K::Str:
      return true;
    case K::Tuple:
      return std::ranges::all_of(k.elems, shared_key_plain);
    default:
      return false;
  }
}

// The key relation an Object has (JitValueEq), over plain keys: never across
// types, a Tuple by its elements, NaN one key. A String may be asked about by
// its bytes alone, which is what a lookup by name has in hand.
struct SharedKeyEq {
  using is_transparent = void;
  bool operator()(const SendNode* a, const SendNode* b) const {
    using K = SendNode::K;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
      case K::Nil: return true;
      case K::Bool: return a->b == b->b;
      case K::Long: return a->i == b->i;
      case K::Float: return same_float_key(a->d, b->d);
      case K::Str: return a->s == b->s;
      default:
        return std::ranges::equal(
            a->elems, b->elems,
            [&](const SendNode& x, const SendNode& y) { return (*this)(&x, &y); });
    }
  }
  bool operator()(std::string_view a, const SendNode* b) const {
    return b->kind == SendNode::K::Str && a == b->s;
  }
  bool operator()(const SendNode* a, std::string_view b) const {
    return (*this)(b, a);
  }
};
struct SharedKeyHash {
  using is_transparent = void;
  size_t operator()(std::string_view s) const {
    return std::hash<std::string_view>{}(s);
  }
  size_t operator()(const SendNode* k) const {
    using K = SendNode::K;
    switch (k->kind) {
      case K::Nil: return 0;
      case K::Bool: return hash_long(k->b ? 1 : 0);
      case K::Long: return hash_long(k->i);
      case K::Float: return hash_double(k->d);
      case K::Str: return (*this)(std::string_view(k->s));
      default: {
        size_t h = k->elems.size();
        for (const auto& e : k->elems) h = h * 31 + (*this)(&e);
        return h;
      }
    }
  }
};

struct SharedValCore {
  SendNode root;
  // Pre-order node table; a view's `__sharedval_node__` is an index here.
  std::vector<const SendNode*> nodes;
  std::unordered_map<const SendNode*, size_t> ids;  // reverse (sub-view mint)
  // What one Object node's keys are found through: the entry each plain key
  // is at, and the entries under a key of any other kind.
  struct KeyIndex {
    std::unordered_map<const SendNode*, size_t, SharedKeyHash, SharedKeyEq>
        plain;
    std::vector<size_t> other;
  };
  std::unordered_map<size_t, KeyIndex> obj_index;
  // Live handle count (every handle and sub-view holds one). Guarded by
  // shared_val_mutex() on the cold bump/drop paths.
  int64_t refcount = 0;
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
// Per-thread id->core cache. The feature's whole point is N isolates
// reading one frozen tree concurrently; a per-read registry mutex
// serialized them (measured: 8 worker threads ran at ~1-thread
// throughput, vs ~4x with this cache). Each isolate reads a small set of
// ids, so it resolves each once under the lock then hits its thread-local
// cache lock-free. weak_ptr (not shared_ptr) so a cache entry never keeps
// a dropped tree alive — the referent expires when the last live view
// drops. The map grows by distinct-id (not read count); a stale hit
// erases its own entry, so the cache tracks the thread's live id set.
inline std::shared_ptr<SharedValCore> lookup_shared_val(int64_t id) {
  static thread_local std::unordered_map<long, std::weak_ptr<SharedValCore>>
      cache;
  if (auto it = cache.find(id); it != cache.end()) {
    if (auto sp = it->second.lock()) return sp;  // lock-free fast path
    cache.erase(it);  // referent dropped — don't keep the dead entry
  }
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  auto it = shared_val_registry().find(id);
  if (it == shared_val_registry().end()) return nullptr;
  cache[id] = it->second;
  return it->second;
}
inline void shared_val_bump(int64_t id) {
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  auto it = shared_val_registry().find(id);
  if (it != shared_val_registry().end()) it->second->refcount++;
}
inline void shared_val_drop(int64_t id) {
  // Erase under the lock, release the shared_ptr outside it (a concurrent
  // lookup may hold a copy) — mirrors shared_buffer_drop.
  std::shared_ptr<SharedValCore> doomed;
  {
    std::lock_guard<std::mutex> lock(shared_val_mutex());
    auto it = shared_val_registry().find(id);
    if (it == shared_val_registry().end()) return;
    if (--it->second->refcount > 0) return;
    doomed = std::move(it->second);
    shared_val_registry().erase(it);
  }
}

// Pre-order DFS: number every node, build the Object key indexes.
inline void _shared_val_index(SharedValCore& core, const SendNode& n) {
  size_t id = core.nodes.size();
  core.nodes.push_back(&n);
  core.ids.emplace(&n, id);
  if (n.kind == SendNode::K::Object) {
    auto& idx = core.obj_index[id];
    for (size_t e = 0; e < n.entries.size(); e++) {
      const SendNode& key = n.entries[e].first;
      if (shared_key_plain(key)) idx.plain.emplace(&key, e);
      else idx.other.push_back(e);
    }
    for (const auto& [k, v] : n.entries) {
      _shared_val_index(core, k);
      _shared_val_index(core, v);
    }
  }
  for (const auto& e : n.elems) _shared_val_index(core, e);
}

// Data-only check, run before freeze. The serializer accepted the value
// (it is Sendable), but Shared narrows further: no functions (data only)
// and no handle references (a frozen tree must not own channel /
// SharedBuffer / Shared refcounts).
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

// Freeze a serialized tree into the registry. Returns the new id; the
// creator's handle owns the initial refcount.
inline int64_t freeze_shared_val(SendNode&& root) {
  auto core = std::make_shared<SharedValCore>();
  core->root = std::move(root);
  _shared_val_index(*core, core->root);
  core->refcount = 1;
  int64_t id = shared_val_next_id()++;
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  shared_val_registry().emplace(id, std::move(core));
  return id;
}

}  // namespace culebra
