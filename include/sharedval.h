#pragma once

// Shared.new — variable-length IMMUTABLE data shared by reference across
// isolates (concurrency C4; the read-only lane next to SharedBuffer's
// fixed-layout read+write lane and the channel's copy lane).
//
// The value is frozen ONCE through the Sendable serializer (the same walk
// that ships values to isolates — so "what can be shared" and "what can
// cross a thread" stay one definition), and the resulting SendNode tree is
// owned by a process-wide registry entry outside every GC heap. Isolates
// read the same tree through view handles: a leaf materializes into the
// reader's heap on access, a container comes back as a sub-view, and
// nothing is copied per worker. Functions are rejected at freeze (Shared
// values are data only — sharing code is free already, the AST is
// process-lifetime), as are native handles; this keeps the tree
// trivially immutable and cycle-free.
//
// Views address tree nodes by INDEX into a freeze-time node table, never
// by raw pointer — a script-visible slot must not be dereferenceable as a
// pointer (the wrap.h borrow-table invariant). The tree is append-only
// at freeze and never mutated after, so the table's pointers are stable.
//
// Included from isolate.h after sendable.h: SendNode, the interp Value
// types, and serialize() are all visible here (same TU pattern as
// sendable.h itself).

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace culebra {

using sendable::SendNode;

// Defined in isolate.h (which includes this header first).
inline void release_inflight_channels(const sendable::SendNode& n);

struct SharedValCore {
  SendNode root;
  // Pre-order node table; a view's `__sharedval_node__` is an index here.
  std::vector<const SendNode*> nodes;
  std::unordered_map<const SendNode*, size_t> ids;  // reverse (sub-view mint)
  // Per-Object-node key index (String keys) for O(1) dictionary lookup.
  std::unordered_map<size_t, std::unordered_map<std::string_view, size_t>>
      obj_index;
  // Live handle count (every handle and sub-view holds one). Guarded by
  // shared_val_mutex() on the cold bump/drop paths.
  long refcount = 0;
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
inline std::shared_ptr<SharedValCore> lookup_shared_val(long id) {
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
inline void shared_val_bump(long id) {
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  auto it = shared_val_registry().find(id);
  if (it != shared_val_registry().end()) it->second->refcount++;
}
inline void shared_val_drop(long id) {
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
      if (n.entries[e].first.kind == SendNode::K::Str) {
        idx.emplace(n.entries[e].first.s, e);
      }
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
inline long freeze_shared_val(SendNode&& root) {
  auto core = std::make_shared<SharedValCore>();
  core->root = std::move(root);
  _shared_val_index(*core, core->root);
  core->refcount = 1;
  long id = shared_val_next_id()++;
  std::lock_guard<std::mutex> lock(shared_val_mutex());
  shared_val_registry().emplace(id, std::move(core));
  return id;
}

// ---------------------------------------------------------------------------
// Interp-side views. A view handle is an ObjectValue carrying
// `__sharedval_id__` + `__sharedval_node__` (index, root = 0) + `_dropped`
// + the reader methods as own props. Data reads are intercepted in
// eval_property / eval_array_reference (declared there, defined below).
// ---------------------------------------------------------------------------

inline Value make_shared_val_view(long id, long node_idx);

struct ResolvedNode {
  std::shared_ptr<SharedValCore> core;
  const SendNode* node;
  long id;
  size_t idx;  // node table index (== obj_index key for an Object node)
};

inline ResolvedNode _shared_val_node_of(const Value& view) {
  const auto& o = view.to_object();
  if (o.has("_dropped") && o.get("_dropped").to_bool()) {
    throw CulebraError("ClosedError", "Shared value has been dropped");
  }
  long id = o.get("__sharedval_id__").to_long();
  auto core = lookup_shared_val(id);
  if (!core) {
    throw CulebraError("ClosedError", "Shared value has been dropped");
  }
  long node = o.get("__sharedval_node__").to_long();
  if (node < 0 || node >= static_cast<long>(core->nodes.size())) {
    throw CulebraError("ValueError", "corrupt Shared view");
  }
  // Resolve the node pointer BEFORE moving `core` — aggregate init runs
  // member-by-member in order, so reading core->nodes after move-init
  // would deref a moved-from pointer.
  const SendNode* n = core->nodes[static_cast<size_t>(node)];
  return {std::move(core), n, id, static_cast<size_t>(node)};
}

// One node, one Value: a leaf materializes into the local heap (strings
// copy — a StringView into the tree would need owner-lifetime rules; a
// later optimization), a container mints a sub-view (+1 on the registry).
inline Value _shared_val_read(long id, const SharedValCore& core,
                              const SendNode& n) {
  switch (n.kind) {
    case SendNode::K::Nil:   return Value();
    case SendNode::K::Bool:  return Value(n.b);
    case SendNode::K::Long:  return Value(n.i);
    case SendNode::K::Float: return Value(n.d);
    case SendNode::K::Str:   return Value(std::string(n.s));
    case SendNode::K::Array:
    case SendNode::K::Object:
    case SendNode::K::Set:
    case SendNode::K::Tuple: {
      shared_val_bump(id);
      return make_shared_val_view(id,
                                  static_cast<long>(core.ids.at(&n)));
    }
    default:
      throw CulebraError("ValueError", "corrupt Shared view");
  }
}

// Deep materialization into the local heap (`view.copy()` — the escape
// hatch back to ordinary mutable values). Data-only by construction.
inline Value _shared_val_materialize(const SendNode& n) {
  switch (n.kind) {
    case SendNode::K::Nil:   return Value();
    case SendNode::K::Bool:  return Value(n.b);
    case SendNode::K::Long:  return Value(n.i);
    case SendNode::K::Float: return Value(n.d);
    case SendNode::K::Str:   return Value(std::string(n.s));
    case SendNode::K::Array: {
      ArrayValue arr;
      for (const auto& e : n.elems) {
        arr.values->push_back(_shared_val_materialize(e));
      }
      return Value(std::move(arr));
    }
    case SendNode::K::Tuple: {
      TupleValue t;
      for (const auto& e : n.elems) {
        t.elements->push_back(_shared_val_materialize(e));
      }
      return Value(std::move(t));
    }
    case SendNode::K::Set: {
      SetValue s;
      for (const auto& e : n.elems) s.add(_shared_val_materialize(e));
      return Value(std::move(s));
    }
    case SendNode::K::Object: {
      ObjectValue o;
      for (size_t e = 0; e < n.entries.size(); e++) {
        const auto& [k, v] = n.entries[e];
        bool is_mut = e < n.entry_mut.size() && n.entry_mut[e];
        if (k.kind == SendNode::K::Str) {
          o.initialize(k.s, _shared_val_materialize(v), is_mut);
        } else {
          o.initialize(_shared_val_materialize(k),
                       _shared_val_materialize(v), is_mut);
        }
      }
      return Value(std::move(o));
    }
    default:
      throw CulebraError("ValueError", "corrupt Shared view");
  }
}

inline bool _shared_val_key_eq(const SendNode& k, const Value& key) {
  switch (k.kind) {
    case SendNode::K::Str:
      return (key.type == Value::String || key.type == Value::StringView) &&
             key.to_string_view() == k.s;
    case SendNode::K::Long:
      return key.type == Value::Long && key.to_long() == k.i;
    case SendNode::K::Bool:
      return key.type == Value::Bool && key.to_bool() == k.b;
    case SendNode::K::Float:
      return key.type == Value::Float && key.get<double>() == k.d;
    default:
      return false;
  }
}

// Throw the interp's TypeError for an Object-only reader on a non-Object
// node (has / keys / values), single-sourced.
inline void _shared_val_require_object(const SendNode* n, const char* m) {
  if (n->kind != SendNode::K::Object) {
    throw CulebraError("TypeError",
        std::string(m) + "() requires an Object Shared view");
  }
}

// `view.name` — Object-node field read. A miss reads as nil, mirroring a
// plain Object's missing property.
inline Value shared_val_get_prop(const Value& view, std::string_view name) {
  auto [core, n, id, node_id] = _shared_val_node_of(view);
  if (n->kind != SendNode::K::Object) return Value();
  auto idx_it = core->obj_index.find(node_id);
  if (idx_it != core->obj_index.end()) {
    auto e = idx_it->second.find(name);
    if (e != idx_it->second.end()) {
      return _shared_val_read(id, *core, n->entries[e->second].second);
    }
  }
  return Value();
}

// `view[key]` — Object key lookup (KeyError on miss, like a local dict)
// or Array/Tuple positional read (IndexError out of range).
inline Value shared_val_get_index(const Value& view, const Value& key) {
  auto [core, n, id, node_id] = _shared_val_node_of(view);
  switch (n->kind) {
    case SendNode::K::Object: {
      if (key.type == Value::String || key.type == Value::StringView) {
        auto idx_it = core->obj_index.find(node_id);
        if (idx_it != core->obj_index.end()) {
          auto e = idx_it->second.find(key.to_string_view());
          if (e != idx_it->second.end()) {
            return _shared_val_read(id, *core, n->entries[e->second].second);
          }
        }
        throw CulebraError("KeyError", "key not present");
      }
      for (const auto& [k, v] : n->entries) {
        if (_shared_val_key_eq(k, key)) {
          return _shared_val_read(id, *core, v);
        }
      }
      throw CulebraError("KeyError", "key not present");
    }
    case SendNode::K::Array:
    case SendNode::K::Tuple: {
      long len = static_cast<long>(n->elems.size());
      long idx = key.to_long();
      if (idx < 0) idx += len;
      if (idx < 0 || idx >= len) {
        throw CulebraError("IndexError", "index out of range");
      }
      return _shared_val_read(id, *core, n->elems[static_cast<size_t>(idx)]);
    }
    default:
      throw CulebraError("TypeError",
                         "this Shared value is not indexable");
  }
}

// for-in over a view: Object yields (key, value) pairs, Array/Tuple/Set
// yield elements — mirroring the local collection protocols. The node is
// immutable, so a plain cursor is a correct snapshot.
inline Value shared_val_make_iter(const Value& view) {
  using namespace std::literals;
  auto [core, n, id, node_id] = _shared_val_node_of(view);
  (void)node_id;  // core (shared_ptr) keeps the tree alive for the iterator
  auto cursor = std::make_shared<size_t>(0);
  const SendNode* node = n;
  size_t total = node->kind == SendNode::K::Object ? node->entries.size()
                                                   : node->elems.size();
  ObjectValue it;
  it.initialize("has_next",
      Value(FunctionValue({}, [cursor, total](std::shared_ptr<Environment>) {
        return Value(*cursor < total);
      }, "Bool"sv)), false);
  it.initialize("next",
      Value(FunctionValue({}, [cursor, total, core, node,
                               id](std::shared_ptr<Environment>) -> Value {
        if (*cursor >= total) return Value();
        size_t i = (*cursor)++;
        if (node->kind == SendNode::K::Object) {
          const auto& [k, v] = node->entries[i];
          TupleValue pair;
          pair.elements->push_back(_shared_val_materialize(k));
          pair.elements->push_back(_shared_val_read(id, *core, v));
          return Value(std::move(pair));
        }
        return _shared_val_read(id, *core, node->elems[i]);
      }, ""sv)), false);
  return Value(std::move(it));
}

inline Value make_shared_val_view(long id, long node_idx) {
  using namespace std::literals;
  ObjectValue h;
  h.initialize("__sharedval_id__", Value(id), false);
  h.initialize("__sharedval_node__", Value(node_idx), false);
  h.initialize("_dropped", Value(false), true);
  auto self_node = [](std::shared_ptr<Environment>& env) {
    return _shared_val_node_of(env->get("this"));
  };
  h.initialize("size",
      Value(FunctionValue({}, [self_node](std::shared_ptr<Environment> env) {
        auto [core, n, id, idx] = self_node(env);
        long s = n->kind == SendNode::K::Object
                     ? static_cast<long>(n->entries.size())
                     : static_cast<long>(n->elems.size());
        return Value(s);
      }, "Long"sv)), false);
  h.initialize("has",
      Value(FunctionValue({{"key", false, ""sv}},
          [self_node](std::shared_ptr<Environment> env) {
        auto [core, n, id, idx] = self_node(env);
        _shared_val_require_object(n, "has");
        auto key = env->get("key");
        for (const auto& [k, v] : n->entries) {
          if (_shared_val_key_eq(k, key)) return Value(true);
        }
        return Value(false);
      }, "Bool"sv)), false);
  h.initialize("keys",
      Value(FunctionValue({}, [self_node](std::shared_ptr<Environment> env) {
        auto [core, n, id, idx] = self_node(env);
        _shared_val_require_object(n, "keys");
        ArrayValue arr;
        for (const auto& [k, v] : n->entries) {
          arr.values->push_back(_shared_val_materialize(k));
        }
        return Value(std::move(arr));
      }, "Array"sv)), false);
  h.initialize("values",
      Value(FunctionValue({}, [self_node](std::shared_ptr<Environment> env) {
        auto [core, n, id, idx] = self_node(env);
        _shared_val_require_object(n, "values");
        ArrayValue arr;
        for (const auto& [k, v] : n->entries) {
          arr.values->push_back(_shared_val_read(id, *core, v));
        }
        return Value(std::move(arr));
      }, "Array"sv)), false);
  h.initialize("copy",
      Value(FunctionValue({}, [self_node](std::shared_ptr<Environment> env) {
        auto [core, n, id, idx] = self_node(env);
        return _shared_val_materialize(*n);
      }, ""sv)), false);
  h.initialize("iter",
      Value(FunctionValue({}, [](std::shared_ptr<Environment> env) {
        return shared_val_make_iter(env->get("this"));
      }, ""sv)), false);
  h.initialize("drop",
      Value(FunctionValue({}, [id](std::shared_ptr<Environment> env) -> Value {
        // Idempotent via the shared `_dropped` flag (explicit drop and
        // the GC/scope-exit drop path both land here; later calls no-op).
        auto self = env->get("this");
        auto& o = self.to_object();
        if (o.get("_dropped").to_bool()) return Value();
        o.assign("_dropped", Value(true));
        shared_val_drop(id);
        return Value();
      }, "Nil"sv)), false);
  h.is_shared_val = true;
  return Value(std::move(h));
}

// `Shared.new(v)` — freeze v (serialize = the freeze) and hand back the
// root view. Rejections release any in-flight refcounts the serializer
// took (channel/SharedBuffer/Shared nodes bump on extract).
inline Value make_shared_namespace() {
  using namespace std::literals;
  ObjectValue ns;
  ns.initialize("new",
      Value(FunctionValue({{"value", false, ""sv}},
          [](std::shared_ptr<Environment> env) -> Value {
        auto v = env->get("value");
        long line = env->get("__LINE__").to_long();
        long col = env->get("__COLUMN__").to_long();
        sendable::SendNode root;
        try {
          // Freeze mode: extract hooks skip the in-flight bump (a freeze
          // has no async window and Shared rejects every handle anyway),
          // so a serialize that throws past an already-walked handle
          // leaks nothing.
          sendable::FreezeGuard _fz;
          sendable::SerCtx ctx;
          root = sendable::serialize(v, ctx);
        } catch (const CulebraError& e) {
          throw CulebraError(e.kind,
                             std::format("Shared.new: {}", e.what()),
                             line, col);
        }
        if (auto* why = _shared_val_reject_reason(root)) {
          throw CulebraError("SendError",
                             std::format("Shared.new: {}", why), line, col);
        }
        long id = freeze_shared_val(std::move(root));
        return make_shared_val_view(id, 0);
      }, ""sv)), false);
  return Value(std::move(ns));
}

// Register the Sendable hooks once at load: a view ships by (id, node)
// reference. extract bumps for the in-flight window (released by the
// consumer's release_inflight_channels once the node is rebuilt);
// rebuild bumps again for the receiver handle's own ref (released by
// that handle's drop). Mirrors the SharedBuffer hook contract.
inline const bool _sharedval_hooks_installed = [] {
  sendable::sharedval_extract_hook() =
      [](const Value& v) -> sendable::SharedValRef {
    const auto& o = v.to_object();
    if (o.get("_dropped").to_bool()) {
      throw CulebraError("ClosedError", "Shared value has been dropped");
    }
    long id = o.get("__sharedval_id__").to_long();
    if (!sendable::sharedval_freezing()) shared_val_bump(id);
    return {id, o.get("__sharedval_node__").to_long()};
  };
  sendable::sharedval_rebuild_hook() =
      [](const sendable::SharedValRef& r) -> Value {
    shared_val_bump(r.id);
    return make_shared_val_view(r.id, r.node);
  };
  return true;
}();

}  // namespace culebra
