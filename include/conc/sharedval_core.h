#pragma once

// The Shared.new frozen-tree core: the process-wide registry of frozen
// SendNode trees, its refcounting, the flattened index, and
// freeze_shared_val. Engine-neutral (SendNode only); the interp's view
// handles stay in sharedval.h, the compiled lanes' live in jit_runtime /
// sendable_jit. Moved verbatim from sharedval.h (Phase 4 B7-b).

#include <conc/send_node.h>
#include <base/shared.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
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
  // What a keyed node's keys are found through (an Object's keys, a Set's
  // members; see shared_key_at): the position each plain key is at, and the
  // positions of the keys of any other kind.
  struct KeyIndex {
    std::unordered_map<const SendNode*, size_t, SharedKeyHash, SharedKeyEq>
        plain;
    std::vector<size_t> other;
  };
  // Pre-order node table; a view's `__sharedval_node__` is an index here, and
  // each node carries its own index back (SendNode::shared_index). A keyed
  // node's entry has its key index beside it.
  struct Entry {
    const SendNode* node;
    const KeyIndex* keys = nullptr;
  };
  std::vector<Entry> nodes;
  std::deque<KeyIndex> key_indexes;  // what Entry::keys points into
  // Every class meta the tree writes out, by the ref id the instances after
  // the first name it by: what rebuilding one node of the tree resolves a
  // backref through.
  std::unordered_map<int, const SendNode*> metas;
  // Live count: every view holds one, and a message in flight one per view it
  // carries. Whoever adds to it holds a count already, so it is never added
  // to at zero and needs no lock; the registry's mutex covers the map alone.
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
// One more count on a tree the caller holds a count on already. N isolates
// reading one tree mint views of it side by side, and this is all the tree
// itself hears of that (a registry mutex here once held 8 reader threads to
// one thread's throughput).
inline void shared_val_retain(const SharedValCore& core) {
  core.refcount.fetch_add(1, std::memory_order_relaxed);
}
// Give a count back, by the tree (a view's). The last one takes the tree out
// of the registry, releasing it outside the lock (mirrors
// shared_buffer_drop); a count taken by id in between leaves it there.
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
// The same, by id: a message's in-flight count, once it has been rebuilt.
inline void shared_val_drop(int64_t id) {
  std::shared_ptr<SharedValCore> core;
  {
    std::lock_guard<std::mutex> lock(shared_val_mutex());
    auto it = shared_val_registry().find(id);
    if (it == shared_val_registry().end()) return;
    core = it->second;
  }
  shared_val_release(*core, id);
}

// A keyed node's keys, by position: an Object's entry keys, a Set's members.
inline bool shared_keyed(const SendNode& n) {
  return n.kind == SendNode::K::Object || n.kind == SendNode::K::Set;
}
// How many values a node yields: an Object's entries, or the elements of any
// other container. For a keyed node that is its key count.
inline size_t shared_node_count(const SendNode& n) {
  return n.kind == SendNode::K::Object ? n.entries.size() : n.elems.size();
}
inline const SendNode& shared_key_at(const SendNode& n, size_t at) {
  return n.kind == SendNode::K::Object ? n.entries[at].first : n.elems[at];
}

// Pre-order DFS: number every node, build the key indexes.
inline void _shared_val_index(SharedValCore& core, SendNode& n) {
  size_t id = core.nodes.size();
  core.nodes.push_back({&n});
  n.shared_index = static_cast<int32_t>(id);
  if (n.kind == SendNode::K::Object && !n.is_backref && n.ref_id >= 0)
    core.metas.emplace(n.ref_id, &n);
  if (shared_keyed(n)) {
    auto& idx = core.key_indexes.emplace_back();
    core.nodes[id].keys = &idx;
    idx.plain.reserve(shared_node_count(n));
    for (size_t at = 0; at < shared_node_count(n); at++) {
      const SendNode& key = shared_key_at(n, at);
      if (shared_key_plain(key)) idx.plain.emplace(&key, at);
      else idx.other.push_back(at);
    }
  }
  if (n.kind == SendNode::K::Object) {
    for (auto& [k, v] : n.entries) {
      _shared_val_index(core, k);
      _shared_val_index(core, v);
    }
  }
  for (auto& e : n.elems) _shared_val_index(core, e);
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

// Freeze a serialized tree into the registry. Returns the new id and the
// tree; the creator's handle owns the initial count.
inline std::pair<int64_t, SharedValCore*> freeze_shared_val(SendNode&& root) {
  auto core = std::make_shared<SharedValCore>();
  core->root = std::move(root);
  _shared_val_index(*core, core->root);
  core->refcount = 1;
  int64_t id = shared_val_next_id()++;
  auto* tree = core.get();
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  shared_val_registry().emplace(id, std::move(core));
  return {id, tree};
}

}  // namespace culebra
