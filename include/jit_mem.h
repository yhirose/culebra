#pragma once

#ifdef CULEBRA_JIT_ENABLED

// Reference counting impl helpers, deterministic-drop scope-exit
// resolution, the rt namespace, and the Windows JIT memory managers
// (NearImageMapper / WinSEHMemoryManager).
//
// Runtime-layer fragment of jit.h, split out for readability. These
// fragments rely on jit.h's #include block and are included by jit.h in a
// fixed sequence (see jit.h); they are not standalone headers.

// --- Reference counting (C++ impl helpers, not extern "C") ---
//
// Only heap-allocated refcounted types have refcount headers:
//   TAG_FUNC (closure), TAG_ARRAY, TAG_OBJECT, and JitCell (internal).
// Strings (TAG_STRING) are NOT refcounted in this implementation and leak.
// Nil/Bool/Long are value types and have no refcount.

inline void _culebra_cell_release(JitCell* c);

// RAII drop: if `o` has a 0-arg `drop` Function property, invoke it
// with `self` bound to `o` before any child values are released.
// Called from the normal refcount-0 path and from the cycle collector.
// The contract (drop must be a 0-arg Function) is validated at
// assignment time, so a mis-shaped drop is silently skipped here as a
// belt-and-braces check.
//
// Refcount trick (matches interpreter's shared_ptr + no-op deleter):
// bump high enough that the drop body's function-frame release of its
// owned `self` slot — plus any retain/release pairs in the body —
// can't drive refcount back to 0 and re-enter destruction. The frame
// release subtracts 1, so 2 is the minimum safe baseline. After drop
// returns the entry count is restored (see below) — any NET retain or
// release of `o` the body performed is absorbed by the pin; so on the
// release-to-zero path a body that resurrected `o` by storing it
// somewhere leaves the caller a dangling reference (matches interp's
// documented warning).
inline void _culebra_call_drop_if_present(JitObject* o) {
  if (!o || !o->has_drop) return;
  if (o->dropped) return;  // already ran (explicit or backstop) — at most once
  if (_jit_drop_suppressed()) return;  // cycle-held resource — no finalizer
  // Walks proto so class-sugar instances find their inherited `drop`.
  auto* entry = _find_property(o, "drop");
  if (!entry) return;
  const auto& v = entry->value;
  if (v.tag != GC_TAG_FUNC) return;
  auto* cls = reinterpret_cast<JitClosure*>(v.data);
  if (!cls || cls->arity != 0) return;

  o->dropped = true;  // set before running: re-entrancy-safe, at-most-once

  // Pin the object across its own drop so a re-entrant release inside the
  // drop body can't free it mid-call. The pin must absorb not just the
  // frame's `self` release but ANY number of releases the body performs
  // (e.g. `self.me = nil` breaking its own cycle edge), so use a value
  // no real refcount can reach. RESTORE the entry count afterwards: on
  // the release-to-zero path that's the 0 the caller's teardown expects,
  // but an explicit `obj.drop()` (or a scope-exit / finalize firing)
  // arrives with live references — parking those at 0 would let the
  // next retain/release pair around the (legal, ClosedError-raising)
  // dropped object free it from under its remaining holders.
  const int64_t entry_rc = o->refcount;
  o->refcount = int64_t{1} << 40;
  JitValue self_val{GC_TAG_OBJECT, reinterpret_cast<int64_t>(o)};
  try {
    auto r = _jit_invoke(cls, self_val, 0, nullptr);
    _culebra_value_release_impl(r.tag, r.data);
  } catch (const std::exception& e) {
    std::cerr << "drop: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "drop: unknown error" << std::endl;
  }
  o->refcount = entry_rc;
}

// Explicit `obj.drop()` from JIT-compiled code: route through the at-most-once
// guard so an explicit drop suppresses the later auto-drop (mirrors the interp
// eval_call path). A no-op (the call site yields nil) on a non-Object or a
// drop-less receiver. Does not consume the receiver reference — the caller
// releases it per the method-call ownership convention.
extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_explicit_drop(
    int8_t tag, int64_t data) {
  if (tag == TAG_OBJECT)
    _culebra_call_drop_if_present(reinterpret_cast<JitObject*>(data));
}

// --- Deterministic drop: scope-exit resolution ---

// GC backstop finalize (exactly-once backstop, PEP 442
// style): runs once per collection, before any sweep, over the intact
// dead set. Pin every dead struct's refcount first — a drop body that
// breaks its own cycle would otherwise free a sibling ahead of its
// finalize/sweep — then fire each pending `drop`. The pins need no
// undo: sweep reclaims unmarked objects regardless of refcount, and
// the `dropped` flag keeps the union with every other drop path
// exactly-once. Finalization order within one collection is
// unspecified (matches PEP 442). Suppressed-drop windows (top-level
// exit, multifn body replacement) are honored inside
// _culebra_call_drop_if_present.
inline void _jit_gc_finalize_dead(const std::vector<void*>& dead) {
  auto& heap = _gc_heap();
  // All refcounted heap types share the i64 refcount first field; a traced
  // String has no refcount (its first bytes are content, and for short
  // strings offset 0 may even reach the trailing NUL) and a traced view's
  // offset 0 is its borrowed `ptr`, so never touch either.
  for (void* p : dead) {
    auto* h = heap.header(p);
    if (h && _jit_gc_is_traced_only(h->type_tag)) continue;
    (*reinterpret_cast<int64_t*>(p))++;
  }
  for (void* p : dead) {
    auto* h = heap.header(p);
    if (!h || h->type_tag != GC_TAG_OBJECT) continue;
    // has_drop / dropped / suppressed gating lives in
    // _culebra_call_drop_if_present, which also saves/restores the
    // entry refcount — the pass consumes none of the dead set's pins.
    _culebra_call_drop_if_present(reinterpret_cast<JitObject*>(p));
  }
}

// Resolve the owned-stack region registered above `mark` (the scope's
// entry snapshot of next_id, loaded inline). Codegen reaches here only
// when the inline top_stamp check saw a non-empty region, and only
// AFTER the scope's slot release, so anything held only by the
// scope's bindings is already gone (its entry tombstoned by the
// ordinary refcount-0 release — that path stays the primary one and
// keeps its timing). What remains is escaped or cyclic; a localized
// trial deletion (Bacon-Rajan style, mirroring the interp's
// _owned_resolve_ambiguous without the binding credit) decides:
// unreachable from outside the subgraph → drop (newest first — reverse
// creation order, cycle members included); reachable → survives,
// pushed back in original order for the parent scope's region to
// inherit. Closures are walked precisely (captures → cells → values,
// dispatcher → table-held bodies), so a cycle through a resource's own
// closure slot resolves here too; the cycle's memory is still left to
// the backstop collector, which never re-runs a `dropped` body. (The
// interp force-pins a self-captured env instead — whole-env capture
// can't be walked per-variable — so these shapes fire one collect
// later there; the drop count stays symmetric, the timing is not.)
extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE void
culebra_runtime_owned_scope_exit(int64_t mark_arg) {
  const uint64_t mark = static_cast<uint64_t>(mark_arg);
  auto& stack = _jit_owned_stack();
  auto& st = stack.entries;
  // Common case: nothing registered under this scope. (Compiled code
  // already checked top_stamp <= mark inline; other callers land here.)
  if (st.empty() || st.back().id < mark) return;

  std::vector<JitOwnedStack::Entry> pending;  // newest first
  while (!st.empty() && st.back().id >= mark) {
    auto e = st.back();
    st.pop_back();
    if (!e.obj) continue;          // tombstone: died via release/sweep
    e.obj->owned_idx = -1;         // detached while pending
    if (e.obj->dropped) continue;  // explicit drop already ran
    // Analysis pin (mirrors the interp's OwnedPending::sp lock): an
    // earlier candidate's drop body may break the cycle that keeps a
    // later one alive — without the pin its memory would be freed
    // before we fire (or skip) it. Credited via the seed's explained++.
    e.obj->refcount++;
    pending.push_back(e);
  }
  stack.refresh_top();
  if (pending.empty()) return;

  // Localized trial deletion over the candidates' subgraph. Containers
  // (Object/Array/Tuple/Set) become nodes; every reference occurrence
  // from within the subgraph is "explained". A node with references
  // beyond that is pinned from outside; whatever a pinned node reaches
  // is externally reachable. No user code runs during the analysis, so
  // the refcounts it reads are stable.
  struct Node {
    int64_t use_count = 0;
    long explained = 0;
    std::vector<size_t> out;
  };
  std::vector<Node> nodes;
  std::unordered_map<const void*, size_t> index;
  auto add_node = [&](const void* p, int64_t use_count, bool& fresh) {
    auto [it, inserted] = index.try_emplace(p, nodes.size());
    fresh = inserted;
    if (inserted) nodes.push_back({use_count, 0, {}});
    return it->second;
  };
  constexpr size_t npos = static_cast<size_t>(-1);
  // Runaway guard (mirrors the interp's kNodeBudget): resource graphs
  // are small; if discovery exceeds the budget, bail out and let every
  // candidate survive (safe direction — the backstop covers them).
  constexpr size_t kNodeBudget = 4096;
  bool overflow = false;
  auto walk_value = [&](const JitValue& v, size_t from, auto&& self) -> void {
    if (overflow || nodes.size() > kNodeBudget) {
      overflow = true;
      return;
    }
    switch (v.tag) {
      case TAG_OBJECT:
      case TAG_ARRAY:
      case TAG_TUPLE:
      case TAG_SET:
      case TAG_FUNC:
        break;
      default:
        return;  // primitives / Tensor (no script references inside)
    }
    if (!v.data) return;
    auto* p = reinterpret_cast<void*>(v.data);
    bool fresh;
    // All refcounted heap types share the i64 refcount first field.
    size_t id = add_node(p, *reinterpret_cast<int64_t*>(p), fresh);
    nodes[id].explained++;
    if (from != npos) nodes[from].out.push_back(id);
    if (!fresh) return;
    switch (v.tag) {
      case TAG_OBJECT: {
        auto* o = reinterpret_cast<JitObject*>(p);
        for (auto& entry : o->slots) self(entry.value, id, self);
        if (o->key_order && o->non_string_props) {
          for (const auto& k : *o->key_order) {
            self(k, id, self);
            if (k.tag == TAG_STRING) continue;
            auto it = o->non_string_props->find(k);
            if (it != o->non_string_props->end()) {
              // A refcounted key holds two refs: key_order's (walked
              // above) and the map's stored alias — explain both.
              self(it->first, id, self);
              self(it->second.value, id, self);
            }
          }
        }
        // The proto edge keeps the class meta pinned (its class binding
        // is outside the subgraph, so the meta stays externally
        // reachable and everything it owns survives with it) — following
        // it keeps the counts exact for instance-only cycles.
        if (o->proto)
          self(JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(o->proto)}, id,
               self);
        break;
      }
      case TAG_ARRAY:
      case TAG_TUPLE: {
        auto* a = reinterpret_cast<JitArray*>(p);
        for (size_t i = 0; i < a->size; i++) self(a->items[i], id, self);
        break;
      }
      case TAG_SET: {
        auto* s = reinterpret_cast<JitSet*>(p);
        for (auto& m : s->members) self(m, id, self);
        break;
      }
      case TAG_FUNC: {
        // A closure holds one ref per capture cell; the cell holds one
        // ref to its value. Walking both makes a self-capture cycle
        // (`res.me = fn () { res.id }`: res → closure → cell → res)
        // fully explained, so it drops at scope exit instead of parking
        // for the backstop. Cells share the i64-refcount-first layout.
        auto* c = reinterpret_cast<JitClosure*>(p);
        for (size_t i = 0; i < c->n_captures; i++) {
          auto* cell = c->captures[i];
          if (!cell) continue;
          bool cell_fresh;
          size_t cid = add_node(cell, cell->refcount, cell_fresh);
          nodes[cid].explained++;
          nodes[id].out.push_back(cid);
          if (cell_fresh) self(cell->value, cid, self);
        }
        // A multifn dispatcher owns its overload bodies through the
        // thread-local table (+1 each, invisible to slot walks) — the
        // same edges the mark phase uses.
        if (_jit_is_multifn_dispatcher(c)) {
          std::vector<void*> bodies;
          _jit_multifn_push_bodies(c, bodies);
          for (void* b : bodies)
            self(JitValue{TAG_FUNC, reinterpret_cast<int64_t>(b)}, id, self);
        }
        break;
      }
      default:
        break;
    }
  };

  // Seed: each candidate. The generic walk's explained++ accounts for
  // exactly one reference — our analysis pin above.
  std::vector<size_t> cand_ids(pending.size());
  for (size_t i = 0; i < pending.size(); i++) {
    walk_value(
        JitValue{TAG_OBJECT, reinterpret_cast<int64_t>(pending[i].obj)}, npos,
        walk_value);
    cand_ids[i] = index.find(pending[i].obj)->second;
  }

  std::vector<char> reachable(nodes.size(), 0);
  std::vector<size_t> q;
  for (size_t i = 0; i < nodes.size(); i++) {
    if (overflow || nodes[i].use_count > nodes[i].explained) {
      reachable[i] = 1;
      q.push_back(i);
    }
  }
  while (!q.empty()) {
    size_t i = q.back();
    q.pop_back();
    for (size_t t : nodes[i].out) {
      if (!reachable[t]) {
        reachable[t] = 1;
        q.push_back(t);
      }
    }
  }

  // Survivors return first, in original (creation) order, inherited by
  // the parent scope's region — BEFORE any drop fires: a drop body may
  // register new (higher-id) entries, and appending older survivor ids
  // after those would break the stack's id ordering. Then drops fire
  // newest-first on the pre-drop snapshot decisions, and finally the
  // analysis pins come off (the at-most-once `dropped` flag makes any
  // resulting refcount-0 re-entry into drop a no-op).
  for (size_t i = pending.size(); i-- > 0;) {
    if (reachable[cand_ids[i]]) {
      pending[i].obj->owned_idx =
          static_cast<int64_t>(stack.entries.size());
      stack.entries.push_back(pending[i]);
    }
  }
  stack.refresh_top();
  for (size_t i = 0; i < pending.size(); i++) {
    // The chokepoint saves/restores the entry refcount: firing consumes
    // none of the cycle member's remaining references.
    if (!reachable[cand_ids[i]])
      _culebra_call_drop_if_present(pending[i].obj);
  }
  for (auto& p : pending) {
    _culebra_value_release_impl(GC_TAG_OBJECT,
                                reinterpret_cast<int64_t>(p.obj));
  }
}

inline void _culebra_value_release_impl(int8_t tag, int64_t data) {
  if (data == 0) return;
  switch (tag) {
    case GC_TAG_FUNC: {
      auto* c = reinterpret_cast<JitClosure*>(data);
      if (--c->refcount == 0) {
        // A dispatcher reaching refcount 0 is non-recursive (a recursive one
        // sits in a dispatcher↔body cycle and is reclaimed by sweep instead),
        // so releasing its bodies' table-held +1 cannot re-enter this closure.
        if (_jit_is_multifn_dispatcher(c))
          _jit_multifn_forget(c, /*release_bodies=*/true);
        for (size_t i = 0; i < c->n_captures; i++) {
          _culebra_cell_release(c->captures[i]);
        }
        std::free(c->captures);
        _gc_note_free(c, GC_TAG_FUNC);
        delete c;
      }
      break;
    }
    case GC_TAG_ARRAY:
    case GC_TAG_TUPLE: {
      auto* a = reinterpret_cast<JitArray*>(data);
      if (--a->refcount == 0) {
        for (size_t i = 0; i < a->size; i++) {
          _culebra_value_release_impl(a->items[i].tag, a->items[i].data);
        }
        delete[] a->items;
        _gc_note_free(a, tag);
        delete a;
      }
      break;
    }
    case GC_TAG_OBJECT: {
      auto* o = reinterpret_cast<JitObject*>(data);
      if (--o->refcount == 0) {
        _culebra_call_drop_if_present(o);
        if (o->proto) {
          auto* proto = o->proto;
          o->proto = nullptr;
          _culebra_value_release_impl(GC_TAG_OBJECT,
                                       reinterpret_cast<int64_t>(proto));
        }
        for (auto& entry : o->slots) {
          _culebra_value_release_impl(entry.value.tag, entry.value.data);
        }
        // Sidecar teardown. Both `key_order` and the AnyKeyMap hold
        // a +1 ref on each refcounted (Tuple) key, plus the map holds
        // a +1 on each entry value. Each loop drops its own ref so the
        // refcount lands at zero for keys that go fully unreachable.
        if (o->key_order) {
          for (auto& k : *o->key_order) {
            if (_is_refcounted_value_tag(k.tag)) {
              _culebra_value_release_impl(k.tag, k.data);
            }
          }
          delete o->key_order;
          o->key_order = nullptr;
        }
        if (o->non_string_props) {
          // Release both the entry's value AND the entry's stored key.
          // The key release drops the +1 transferred from the original
          // `object_set_any` insert; `key_order` separately released
          // its own +1 above, so the two stored refs are balanced.
          for (auto& [k, entry] : *o->non_string_props) {
            _culebra_value_release_impl(entry.value.tag, entry.value.data);
            _culebra_value_release_impl(k.tag, k.data);
          }
          delete o->non_string_props;
          o->non_string_props = nullptr;
        }
        _gc_note_free(o, GC_TAG_OBJECT);
        delete o;
      }
      break;
    }
    case GC_TAG_TENSOR: {
      auto* t = reinterpret_cast<JitTensor*>(data);
      if (--t->refcount == 0) {
        _gc_note_free(t, GC_TAG_TENSOR);
        delete t;  // ~JitTensor releases the shared_ptr<TensorImpl>
      }
      break;
    }
    case GC_TAG_SET: {
      auto* s = reinterpret_cast<JitSet*>(data);
      if (--s->refcount == 0) {
        for (auto& m : s->members) {
          _culebra_value_release_impl(m.tag, m.data);
        }
        delete s->index;
        _gc_note_free(s, GC_TAG_SET);
        delete s;
      }
      break;
    }
    default:
      break;  // TAG_NIL/BOOL/LONG/STRING: no-op
  }
}

inline void _culebra_cell_release(JitCell* c) {
  if (!c) return;
  if (--c->refcount == 0) {
    _culebra_value_release_impl(c->value.tag, c->value.data);
    _gc_note_free(c, GC_TAG_CELL);
    delete c;
  }
}

extern "C" {

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_value_retain(int8_t tag,
                                                               int64_t data) {
  if (data == 0) return;
  switch (tag) {
    case TAG_FUNC:
      reinterpret_cast<JitClosure*>(data)->refcount++;
      break;
    case TAG_ARRAY:
    case TAG_TUPLE:
      reinterpret_cast<JitArray*>(data)->refcount++;
      break;
    case TAG_OBJECT:
      reinterpret_cast<JitObject*>(data)->refcount++;
      break;
    case TAG_TENSOR:
      reinterpret_cast<JitTensor*>(data)->refcount++;
      break;
    case TAG_SET:
      reinterpret_cast<JitSet*>(data)->refcount++;
      break;
  }
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_value_release(int8_t tag,
                                                                int64_t data) {
  _culebra_value_release_impl(tag, data);
}

// Fused retain(rhs) + release(lhs); halves runtime call overhead in
// the postfix loop's per-step ownership swap.
CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_value_swap_owned(
    int8_t lt, int64_t ld, int8_t rt, int64_t rd) {
  culebra_runtime_value_retain(rt, rd);
  _culebra_value_release_impl(lt, ld);
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_cell_retain(JitCell* c) {
  if (c) c->refcount++;
}

CULEBRA_RT_KEEP CULEBRA_RT_INLINE void culebra_runtime_cell_release(JitCell* c) {
  _culebra_cell_release(c);
}

}  // extern "C"

namespace culebra {

// ---------------------------------------------------------------------------
// Runtime function names
//
// Centralized so: a typo fails to compile (unknown identifier) instead
// of silently producing an unresolved LLVM declaration; renames happen
// once; the full runtime ABI is scannable in one place.
// ---------------------------------------------------------------------------
namespace rt {
inline constexpr auto array_all           = "culebra_runtime_array_all";
inline constexpr auto array_any           = "culebra_runtime_array_any";
inline constexpr auto array_contains      = "culebra_runtime_array_contains";
inline constexpr auto array_insert        = "culebra_runtime_array_insert";
inline constexpr auto array_remove_at     = "culebra_runtime_array_remove_at";
inline constexpr auto array_min_by        = "culebra_runtime_array_min_by";
inline constexpr auto array_max_by        = "culebra_runtime_array_max_by";
inline constexpr auto array_to_set        = "culebra_runtime_array_to_set";
inline constexpr auto array_to_object     = "culebra_runtime_array_to_object";
inline constexpr auto array_group_by      = "culebra_runtime_array_group_by";
inline constexpr auto array_partition     = "culebra_runtime_array_partition";
inline constexpr auto array_filter        = "culebra_runtime_array_filter";
inline constexpr auto array_find          = "culebra_runtime_array_find";
inline constexpr auto array_flat_map      = "culebra_runtime_array_flat_map";
inline constexpr auto array_for_each      = "culebra_runtime_array_for_each";
inline constexpr auto array_get           = "culebra_runtime_array_get";
inline constexpr auto array_index_of      = "culebra_runtime_array_index_of";
inline constexpr auto array_join          = "culebra_runtime_array_join";
inline constexpr auto array_map           = "culebra_runtime_array_map";
inline constexpr auto array_new           = "culebra_runtime_array_new";
inline constexpr auto array_new_reserved  = "culebra_runtime_array_new_reserved";
inline constexpr auto array_pop           = "culebra_runtime_array_pop";
inline constexpr auto array_push          = "culebra_runtime_array_push";
inline constexpr auto array_max           = "culebra_runtime_array_max";
inline constexpr auto array_min           = "culebra_runtime_array_min";
inline constexpr auto array_product       = "culebra_runtime_array_product";
inline constexpr auto array_reduce        = "culebra_runtime_array_reduce";
inline constexpr auto array_sum           = "culebra_runtime_array_sum";
inline constexpr auto array_resize        = "culebra_runtime_array_resize";
inline constexpr auto array_reverse       = "culebra_runtime_array_reverse";
inline constexpr auto array_set           = "culebra_runtime_array_set";
inline constexpr auto array_set_or_push   = "culebra_runtime_array_set_or_push";
inline constexpr auto array_extend        = "culebra_runtime_array_extend";
inline constexpr auto object_merge        = "culebra_runtime_object_merge";
inline constexpr auto array_size          = "culebra_runtime_array_size";
inline constexpr auto tensor_zeros        = "culebra_runtime_tensor_zeros";
inline constexpr auto tensor_ones         = "culebra_runtime_tensor_ones";
inline constexpr auto tensor_randn        = "culebra_runtime_tensor_randn";
inline constexpr auto tensor_from         = "culebra_runtime_tensor_from";
inline constexpr auto tensor_concat       = "culebra_runtime_tensor_concat";
inline constexpr auto tensor_shape        = "culebra_runtime_tensor_shape";
inline constexpr auto tensor_binop        = "culebra_runtime_tensor_binop";
inline constexpr auto tensor_eval_one     = "culebra_runtime_tensor_eval_one";
inline constexpr auto tensor_transpose    = "culebra_runtime_tensor_transpose";
inline constexpr auto tensor_clone        = "culebra_runtime_tensor_clone";
inline constexpr auto tensor_slice        = "culebra_runtime_tensor_slice";
inline constexpr auto tensor_reshape      = "culebra_runtime_tensor_reshape";
inline constexpr auto tensor_reduce_axis  = "culebra_runtime_tensor_reduce_axis";
inline constexpr auto tensor_reduce_all   = "culebra_runtime_tensor_reduce_all";
inline constexpr auto tensor_to_array     = "culebra_runtime_tensor_to_array";
inline constexpr auto tensor_item         = "culebra_runtime_tensor_item";
inline constexpr auto tensor_no_grad      = "culebra_runtime_tensor_no_grad";
inline constexpr auto tensor_dot          = "culebra_runtime_tensor_dot";
inline constexpr auto tensor_from_csv     = "culebra_runtime_tensor_from_csv";
inline constexpr auto tensor_unary        = "culebra_runtime_tensor_unary";
inline constexpr auto tensor_linear_sigmoid = "culebra_runtime_tensor_linear_sigmoid";
inline constexpr auto tensor_requires_grad = "culebra_runtime_tensor_requires_grad";
inline constexpr auto tensor_grad         = "culebra_runtime_tensor_grad";
inline constexpr auto tensor_backward     = "culebra_runtime_tensor_backward";
inline constexpr auto tensor_zero_grad    = "culebra_runtime_tensor_zero_grad";
inline constexpr auto tensor_detach       = "culebra_runtime_tensor_detach";
inline constexpr auto array_slice         = "culebra_runtime_array_slice";
inline constexpr auto slice               = "culebra_runtime_slice";
inline constexpr auto make_range          = "culebra_runtime_make_range";
inline constexpr auto is_range            = "culebra_runtime_is_range";
inline constexpr auto range_iter          = "culebra_runtime_range_iter";
inline constexpr auto range_step_check    = "culebra_runtime_range_step_check";
inline constexpr auto array_slice2        = "culebra_runtime_array_slice2";
inline constexpr auto array_sort_by       = "culebra_runtime_array_sort_by";
inline constexpr auto array_sorted_by     = "culebra_runtime_array_sorted_by";
inline constexpr auto array_sort          = "culebra_runtime_array_sort";
inline constexpr auto array_sorted        = "culebra_runtime_array_sorted";
// Tuple allocates a JitArray and returns it tagged TAG_TUPLE; storage
// is shared with Array but the tag forbids mutation everywhere
// downstream.
inline constexpr auto tuple_new           = "culebra_runtime_tuple_new";
inline constexpr auto tuple_push          = "culebra_runtime_tuple_push";
inline constexpr auto set_new             = "culebra_runtime_set_new";
inline constexpr auto set_add             = "culebra_runtime_set_add";
inline constexpr auto set_contains        = "culebra_runtime_set_contains";
inline constexpr auto set_size            = "culebra_runtime_set_size";
inline constexpr auto set_union           = "culebra_runtime_set_union";
inline constexpr auto set_intersect       = "culebra_runtime_set_intersect";
inline constexpr auto set_diff            = "culebra_runtime_set_diff";
inline constexpr auto set_sym_diff        = "culebra_runtime_set_sym_diff";
inline constexpr auto set_subset          = "culebra_runtime_set_subset";
inline constexpr auto set_superset        = "culebra_runtime_set_superset";
inline constexpr auto set_add_method      = "culebra_runtime_set_add_method";
inline constexpr auto set_remove          = "culebra_runtime_set_remove";
inline constexpr auto set_to_array        = "culebra_runtime_set_to_array";
inline constexpr auto tuple_to_array      = "culebra_runtime_tuple_to_array";
inline constexpr auto tuple_contains      = "culebra_runtime_tuple_contains";
inline constexpr auto json_stringify      = "culebra_runtime_json_stringify";
inline constexpr auto json_parse          = "culebra_runtime_json_parse";
inline constexpr auto proc_run_kw         = "culebra_runtime_proc_run_kw";
inline constexpr auto ns_method_call_kw   = "culebra_runtime_ns_method_call_kw";
inline constexpr auto proc_all_kw         = "culebra_runtime_proc_all_kw";
inline constexpr auto proc_spawn_kw       = "culebra_runtime_proc_spawn_kw";
inline constexpr auto proc_race           = "culebra_runtime_proc_race";
inline constexpr auto proc_race_kw        = "culebra_runtime_proc_race_kw";
inline constexpr auto set_call_site       = "culebra_runtime_set_call_site";
inline constexpr auto set_op_pos          = "culebra_runtime_set_op_pos";
inline constexpr auto set_callback_arg_site = "culebra_runtime_set_callback_arg_site";
inline constexpr auto set_arg_pos         = "culebra_runtime_set_arg_pos";
inline constexpr auto set_call_positions  = "culebra_runtime_set_call_positions";
inline constexpr auto register_native_fn  = "culebra_runtime_register_native_fn";
inline constexpr auto register_getter     = "culebra_runtime_register_getter";
inline constexpr auto getter_or_value     = "culebra_runtime_getter_or_value";
inline constexpr auto param_pos           = "culebra_runtime_param_pos";
inline constexpr auto type_check_param    = "culebra_runtime_type_check_param";
// Trailing underscore on `throw_` dodges C++ keyword collision.
inline constexpr auto cell_new            = "culebra_runtime_cell_new";
inline constexpr auto fn_handle           = "culebra_runtime_fn_handle";
inline constexpr auto self_merge          = "culebra_runtime_self_merge";
inline constexpr auto cell_release        = "culebra_runtime_cell_release";
inline constexpr auto cell_retain         = "culebra_runtime_cell_retain";
inline constexpr auto closure_new         = "culebra_runtime_closure_new";
inline constexpr auto register_param_meta = "culebra_runtime_register_param_meta";
inline constexpr auto register_mut_captures = "culebra_runtime_register_mut_captures";
inline constexpr auto fn_introspect_get    = "culebra_runtime_fn_introspect_get";
inline constexpr auto call_with_kwargs    = "culebra_runtime_call_with_kwargs";
inline constexpr auto debugger_break      = "culebra_runtime_debugger_break";
inline constexpr auto defer_mark          = "culebra_runtime_defer_mark";
inline constexpr auto defer_push          = "culebra_runtime_defer_push";
inline constexpr auto defer_run_to        = "culebra_runtime_defer_run_to";
inline constexpr auto set_drop_suppressed = "culebra_runtime_set_drop_suppressed";
inline constexpr auto div_zero            = "culebra_runtime_div_zero";
inline constexpr auto input               = "culebra_runtime_input";
inline constexpr auto math_pow            = "culebra_runtime_math_pow";
inline constexpr auto math_wrap           = "culebra_runtime_math_wrap";
inline constexpr auto math_log            = "culebra_runtime_math_log";
inline constexpr auto math_exp            = "culebra_runtime_math_exp";
inline constexpr auto math_sqrt           = "culebra_runtime_math_sqrt";
inline constexpr auto math_sin            = "culebra_runtime_math_sin";
inline constexpr auto math_cos            = "culebra_runtime_math_cos";
inline constexpr auto math_tan            = "culebra_runtime_math_tan";
inline constexpr auto math_asin           = "culebra_runtime_math_asin";
inline constexpr auto math_acos           = "culebra_runtime_math_acos";
inline constexpr auto math_atan           = "culebra_runtime_math_atan";
inline constexpr auto math_atan2          = "culebra_runtime_math_atan2";
inline constexpr auto math_floor          = "culebra_runtime_math_floor";
inline constexpr auto math_ceil           = "culebra_runtime_math_ceil";
inline constexpr auto math_round          = "culebra_runtime_math_round";
inline constexpr auto math_abs            = "culebra_runtime_math_abs";
inline constexpr auto math_min            = "culebra_runtime_math_min";
inline constexpr auto math_max            = "culebra_runtime_math_max";
inline constexpr auto num_add             = "culebra_runtime_num_add";
inline constexpr auto num_sub             = "culebra_runtime_num_sub";
inline constexpr auto num_mul             = "culebra_runtime_num_mul";
inline constexpr auto num_matmul          = "culebra_runtime_num_matmul";
inline constexpr auto num_div             = "culebra_runtime_num_div";
inline constexpr auto num_mod             = "culebra_runtime_num_mod";
inline constexpr auto num_pow             = "culebra_runtime_num_pow";
inline constexpr auto num_neg             = "culebra_runtime_num_neg";
// In-place variants for compound assignment (`t += x`). Tensor lhs
// mutates in place and returns the same Tensor; non-Tensor lhs is
// equivalent to the plain num_OP helper.
inline constexpr auto num_inplace_add     = "culebra_runtime_num_inplace_add";
inline constexpr auto num_inplace_sub     = "culebra_runtime_num_inplace_sub";
inline constexpr auto num_inplace_mul     = "culebra_runtime_num_inplace_mul";
inline constexpr auto num_inplace_div     = "culebra_runtime_num_inplace_div";
inline constexpr auto num_inplace_pow     = "culebra_runtime_num_inplace_pow";
inline constexpr auto sys_argv            = "culebra_runtime_sys_argv";
inline constexpr auto sys_env             = "culebra_runtime_sys_env";
inline constexpr auto sys_exit            = "culebra_runtime_sys_exit";
inline constexpr auto sys_time            = "culebra_runtime_sys_time";
inline constexpr auto sys_getcwd          = "culebra_runtime_sys_getcwd";
inline constexpr auto sys_chdir           = "culebra_runtime_sys_chdir";
inline constexpr auto sys_set_env         = "culebra_runtime_sys_set_env";
inline constexpr auto random_seed         = "culebra_runtime_random_seed";
inline constexpr auto random_int          = "culebra_runtime_random_int";
inline constexpr auto random_uniform      = "culebra_runtime_random_uniform";
inline constexpr auto random_gauss        = "culebra_runtime_random_gauss";
inline constexpr auto random_shuffle      = "culebra_runtime_random_shuffle";
inline constexpr auto random_weighted_choice =
    "culebra_runtime_random_weighted_choice";
inline constexpr auto io_exists           = "culebra_runtime_io_exists";
inline constexpr auto hash_any            = "culebra_runtime_hash_any";
inline constexpr auto fs_join             = "culebra_runtime_fs_join";
inline constexpr auto fs_basename         = "culebra_runtime_fs_basename";
inline constexpr auto fs_dirname          = "culebra_runtime_fs_dirname";
inline constexpr auto fs_extension        = "culebra_runtime_fs_extension";
inline constexpr auto fs_stem             = "culebra_runtime_fs_stem";
inline constexpr auto fs_is_file          = "culebra_runtime_fs_is_file";
inline constexpr auto fs_is_dir           = "culebra_runtime_fs_is_dir";
inline constexpr auto fs_size             = "culebra_runtime_fs_size";
inline constexpr auto fs_list_dir         = "culebra_runtime_fs_list_dir";
inline constexpr auto fs_mkdir            = "culebra_runtime_fs_mkdir";
inline constexpr auto fs_remove           = "culebra_runtime_fs_remove";
inline constexpr auto fs_remove_all        = "culebra_runtime_fs_remove_all";
inline constexpr auto fs_stat             = "culebra_runtime_fs_stat";
inline constexpr auto fs_rename           = "culebra_runtime_fs_rename";
inline constexpr auto fs_copy             = "culebra_runtime_fs_copy";
inline constexpr auto fs_normpath         = "culebra_runtime_fs_normpath";
inline constexpr auto fs_is_abs           = "culebra_runtime_fs_is_abs";
inline constexpr auto fs_abspath          = "culebra_runtime_fs_abspath";
inline constexpr auto fs_realpath         = "culebra_runtime_fs_realpath";
inline constexpr auto fs_is_symlink       = "culebra_runtime_fs_is_symlink";
inline constexpr auto fs_symlink          = "culebra_runtime_fs_symlink";
inline constexpr auto fs_readlink         = "culebra_runtime_fs_readlink";
inline constexpr auto fs_walk             = "culebra_runtime_fs_walk";
inline constexpr auto fs_glob             = "culebra_runtime_fs_glob";
inline constexpr auto time_monotonic       = "culebra_runtime_time_monotonic";
inline constexpr auto time_sleep           = "culebra_runtime_time_sleep";
inline constexpr auto time_now_nanos       = "culebra_runtime_time_now_nanos";
inline constexpr auto time_from_iso_nanos  = "culebra_runtime_time_from_iso_nanos";
inline constexpr auto time_parse_nanos     = "culebra_runtime_time_parse_nanos";
inline constexpr auto time_iso_nanos       = "culebra_runtime_time_iso_nanos";
inline constexpr auto time_format_nanos    = "culebra_runtime_time_format_nanos";
inline constexpr auto time_weekday_nanos   = "culebra_runtime_time_weekday_nanos";
inline constexpr auto time_parts_nanos     = "culebra_runtime_time_parts_nanos";
inline constexpr auto time_from_parts_nanos = "culebra_runtime_time_from_parts_nanos";
inline constexpr auto time_add_nanos       = "culebra_runtime_time_add_nanos";
inline constexpr auto time_start_of_nanos  = "culebra_runtime_time_start_of_nanos";
inline constexpr auto term_cols            = "culebra_runtime_term_cols";
inline constexpr auto term_rows            = "culebra_runtime_term_rows";
inline constexpr auto term_raw_on          = "culebra_runtime_term_raw_on";
inline constexpr auto term_raw_off         = "culebra_runtime_term_raw_off";
inline constexpr auto term_flush           = "culebra_runtime_term_flush";
inline constexpr auto term_resized         = "culebra_runtime_term_resized";
inline constexpr auto term_width           = "culebra_runtime_term_width";
inline constexpr auto term_color_level     = "culebra_runtime_term_color_level";
inline constexpr auto term_read_key        = "culebra_runtime_term_read_key";
inline constexpr auto term_attach_tty      = "culebra_runtime_term_attach_tty";
inline constexpr auto canvas_coord         = "culebra_runtime_canvas_coord";
inline constexpr auto canvas_init          = "culebra_runtime_canvas_init";
inline constexpr auto canvas_clear         = "culebra_runtime_canvas_clear";
inline constexpr auto canvas_set_pixel     = "culebra_runtime_canvas_set_pixel";
inline constexpr auto canvas_get_pixel     = "culebra_runtime_canvas_get_pixel";
inline constexpr auto canvas_rect          = "culebra_runtime_canvas_rect";
inline constexpr auto canvas_line          = "culebra_runtime_canvas_line";
inline constexpr auto canvas_ellipse       = "culebra_runtime_canvas_ellipse";
inline constexpr auto canvas_font_load     = "culebra_runtime_canvas_font_load";
inline constexpr auto canvas_glyph         = "culebra_runtime_canvas_glyph";
inline constexpr auto canvas_sprite_load   = "culebra_runtime_canvas_sprite_load";
inline constexpr auto canvas_sprite_from_png = "culebra_runtime_canvas_sprite_from_png";
inline constexpr auto canvas_sprite_to_png = "culebra_runtime_canvas_sprite_to_png";
inline constexpr auto canvas_sprite_blank  = "culebra_runtime_canvas_sprite_blank";
inline constexpr auto canvas_sprite_free   = "culebra_runtime_canvas_sprite_free";
inline constexpr auto canvas_target        = "culebra_runtime_canvas_target";
inline constexpr auto canvas_sprite_width  = "culebra_runtime_canvas_sprite_width";
inline constexpr auto canvas_sprite_height = "culebra_runtime_canvas_sprite_height";
inline constexpr auto canvas_triangle      = "culebra_runtime_canvas_triangle";
inline constexpr auto canvas_polygon       = "culebra_runtime_canvas_polygon";
inline constexpr auto canvas_blit          = "culebra_runtime_canvas_blit";
inline constexpr auto canvas_blit_scaled   = "culebra_runtime_canvas_blit_scaled";
inline constexpr auto canvas_present       = "culebra_runtime_canvas_present";
inline constexpr auto canvas_buttons       = "culebra_runtime_canvas_buttons";
inline constexpr auto canvas_mouse_x       = "culebra_runtime_canvas_mouse_x";
inline constexpr auto canvas_mouse_y       = "culebra_runtime_canvas_mouse_y";
inline constexpr auto canvas_mouse_buttons = "culebra_runtime_canvas_mouse_buttons";
inline constexpr auto canvas_key           = "culebra_runtime_canvas_key";
inline constexpr auto canvas_title         = "culebra_runtime_canvas_title";
inline constexpr auto canvas_key_pop       = "culebra_runtime_canvas_key_pop";
inline constexpr auto canvas_char_pop      = "culebra_runtime_canvas_char_pop";
inline constexpr auto canvas_closing       = "culebra_runtime_canvas_closing";
inline constexpr auto canvas_windowed      = "culebra_runtime_canvas_windowed";
inline constexpr auto canvas_tone          = "culebra_runtime_canvas_tone";
inline constexpr auto canvas_music_play    = "culebra_runtime_canvas_music_play";
inline constexpr auto canvas_music_stop    = "culebra_runtime_canvas_music_stop";
inline constexpr auto canvas_music_pause   = "culebra_runtime_canvas_music_pause";
inline constexpr auto canvas_music_resume  = "culebra_runtime_canvas_music_resume";
inline constexpr auto canvas_music_volume  = "culebra_runtime_canvas_music_volume";
inline constexpr auto canvas_music_seek    = "culebra_runtime_canvas_music_seek";
inline constexpr auto canvas_music_playing = "culebra_runtime_canvas_music_playing";
inline constexpr auto canvas_sound_load    = "culebra_runtime_canvas_sound_load";
inline constexpr auto canvas_sound_play    = "culebra_runtime_canvas_sound_play";
inline constexpr auto canvas_sound_stop    = "culebra_runtime_canvas_sound_stop";
inline constexpr auto canvas_sound_playing = "culebra_runtime_canvas_sound_playing";
inline constexpr auto canvas_sound_free    = "culebra_runtime_canvas_sound_free";
inline constexpr auto canvas_width         = "culebra_runtime_canvas_width";
inline constexpr auto canvas_height        = "culebra_runtime_canvas_height";
inline constexpr auto object_get          = "culebra_runtime_object_get";
inline constexpr auto object_get_default  = "culebra_runtime_object_get_default";
inline constexpr auto object_get_or_put   = "culebra_runtime_object_get_or_put";
inline constexpr auto object_get_ic       = "culebra_runtime_object_get_ic";
inline constexpr auto prop_get            = "culebra_runtime_prop_get";
inline constexpr auto bind_method_value   = "culebra_runtime_bind_method_value";
inline constexpr auto call_receiver       = "culebra_runtime_call_receiver";
inline constexpr auto to_bool             = "culebra_runtime_to_bool";
inline constexpr auto class_call_method   = "culebra_runtime_class_call_method";
inline constexpr auto class_new_method    = "culebra_runtime_class_new_method";
inline constexpr auto mark_class          = "culebra_runtime_mark_class";
inline constexpr auto explicit_drop       = "culebra_runtime_explicit_drop";
inline constexpr auto owned_hot           = "culebra_runtime_owned_hot";
inline constexpr auto owned_scope_exit    = "culebra_runtime_owned_scope_exit";
inline constexpr auto object_has          = "culebra_runtime_object_has";
inline constexpr auto object_has_own_field
    = "culebra_runtime_object_has_own_field";
inline constexpr auto bare_builtin_reject
    = "culebra_runtime_bare_builtin_reject";
inline constexpr auto object_class_matches
    = "culebra_runtime_object_class_matches";
inline constexpr auto object_has_value    = "culebra_runtime_object_has_value";
inline constexpr auto object_keys         = "culebra_runtime_object_keys";
inline constexpr auto object_values       = "culebra_runtime_object_values";
inline constexpr auto object_new          = "culebra_runtime_object_new";
inline constexpr auto object_remove       = "culebra_runtime_object_remove";
inline constexpr auto is_shared_val       = "culebra_runtime_is_shared_val";
inline constexpr auto nc_receiver_kind    = "culebra_runtime_nc_receiver_kind";
inline constexpr auto is_namespace        = "culebra_runtime_is_namespace";
inline constexpr auto object_remove_any   = "culebra_runtime_object_remove_any";
inline constexpr auto build_class_instance
    = "culebra_runtime_build_class_instance";
inline constexpr auto run_field_init
    = "culebra_runtime_run_field_init";
inline constexpr auto build_variant       = "culebra_runtime_build_variant";
inline constexpr auto make_variant_ctor   = "culebra_runtime_make_variant_ctor";
inline constexpr auto make_derived_method
    = "culebra_runtime_make_derived_method";
inline constexpr auto build_class_meta
    = "culebra_runtime_build_class_meta";
inline constexpr auto object_set          = "culebra_runtime_object_set";
inline constexpr auto object_bind_static  = "culebra_runtime_object_bind_static";
inline constexpr auto wk_contract_error   = "culebra_runtime_wk_contract_error";
inline constexpr auto object_set_fast     = "culebra_runtime_object_set_fast";
inline constexpr auto object_set_ic       = "culebra_runtime_object_set_ic";
inline constexpr auto object_set_any      = "culebra_runtime_object_set_any";
inline constexpr auto object_get_any      = "culebra_runtime_object_get_any";
inline constexpr auto object_get_for_coalesce
    = "culebra_runtime_object_get_for_coalesce";
inline constexpr auto object_has_any      = "culebra_runtime_object_has_any";
inline constexpr auto register_packable   = "culebra_runtime_register_packable";
inline constexpr auto register_packable_enum =
    "culebra_runtime_register_packable_enum";
inline constexpr auto object_size         = "culebra_runtime_object_size";
inline constexpr auto print               = "culebra_runtime_print";
inline constexpr auto println             = "culebra_runtime_println";
inline constexpr auto inspect             = "culebra_runtime_inspect";
inline constexpr auto iota                = "culebra_runtime_iota";
inline constexpr auto math_range           = "culebra_runtime_math_range";
inline constexpr auto check_pos_count      = "culebra_runtime_check_pos_count";
inline constexpr auto iter_collect         = "culebra_runtime_iter_collect";
inline constexpr auto iter_join            = "culebra_runtime_iter_join";
inline constexpr auto iter_count           = "culebra_runtime_iter_count";
inline constexpr auto iter_for_each        = "culebra_runtime_iter_for_each";
inline constexpr auto iter_reduce          = "culebra_runtime_iter_reduce";
inline constexpr auto iter_find            = "culebra_runtime_iter_find";
inline constexpr auto iter_position        = "culebra_runtime_iter_position";
inline constexpr auto iter_contains        = "culebra_runtime_iter_contains";
inline constexpr auto iter_first           = "culebra_runtime_iter_first";
inline constexpr auto iter_last            = "culebra_runtime_iter_last";
inline constexpr auto iter_nth             = "culebra_runtime_iter_nth";
inline constexpr auto iter_any             = "culebra_runtime_iter_any";
inline constexpr auto iter_all             = "culebra_runtime_iter_all";
inline constexpr auto iter_max             = "culebra_runtime_iter_max";
inline constexpr auto iter_min_by          = "culebra_runtime_iter_min_by";
inline constexpr auto iter_max_by          = "culebra_runtime_iter_max_by";
inline constexpr auto iter_to_set          = "culebra_runtime_iter_to_set";
inline constexpr auto iter_to_object       = "culebra_runtime_iter_to_object";
inline constexpr auto iter_group_by        = "culebra_runtime_iter_group_by";
inline constexpr auto iter_partition       = "culebra_runtime_iter_partition";
inline constexpr auto iter_min             = "culebra_runtime_iter_min";
inline constexpr auto iter_product         = "culebra_runtime_iter_product";
inline constexpr auto iter_sum             = "culebra_runtime_iter_sum";
inline constexpr auto iter_map             = "culebra_runtime_iter_map";
inline constexpr auto iter_filter          = "culebra_runtime_iter_filter";
inline constexpr auto iter_take            = "culebra_runtime_iter_take";
inline constexpr auto iter_skip            = "culebra_runtime_iter_skip";
inline constexpr auto iter_take_while      = "culebra_runtime_iter_take_while";
inline constexpr auto iter_skip_while      = "culebra_runtime_iter_skip_while";
inline constexpr auto iter_flatten         = "culebra_runtime_iter_flatten";
inline constexpr auto iter_scan            = "culebra_runtime_iter_scan";
inline constexpr auto iter_distinct        = "culebra_runtime_iter_distinct";
inline constexpr auto iter_tap             = "culebra_runtime_iter_tap";
inline constexpr auto iter_step_by         = "culebra_runtime_iter_step_by";
inline constexpr auto iter_chunk_by        = "culebra_runtime_iter_chunk_by";
inline constexpr auto iter_chunks          = "culebra_runtime_iter_chunks";
inline constexpr auto iter_windows         = "culebra_runtime_iter_windows";
inline constexpr auto iter_chain           = "culebra_runtime_iter_chain";
inline constexpr auto iter_zip             = "culebra_runtime_iter_zip";
inline constexpr auto iter_enumerate       = "culebra_runtime_iter_enumerate";
inline constexpr auto enumerate_any        = "culebra_runtime_enumerate_any";
inline constexpr auto iter_flat_map        = "culebra_runtime_iter_flat_map";
inline constexpr auto iter_advance         = "culebra_runtime_iter_advance";
inline constexpr auto iter_protocol_open   = "culebra_runtime_iter_protocol_open";
inline constexpr auto str_code_points      = "culebra_runtime_str_code_points";
inline constexpr auto str_scalars          = "culebra_runtime_str_scalars";
inline constexpr auto str_scalar_view      = "culebra_runtime_str_scalar_view";
inline constexpr auto str_graphemes        = "culebra_runtime_str_graphemes";
inline constexpr auto str_bytes            = "culebra_runtime_str_bytes";
inline constexpr auto array_iter           = "culebra_runtime_array_iter";
inline constexpr auto object_iter          = "culebra_runtime_object_iter";
inline constexpr auto read_file           = "culebra_runtime_read_file";
inline constexpr auto rethrow             = "culebra_runtime_rethrow";
inline constexpr auto str_cmp             = "culebra_runtime_str_cmp";
inline constexpr auto str_concat          = "culebra_runtime_str_concat";
inline constexpr auto str_eq              = "culebra_runtime_str_eq";
inline constexpr auto str_lower           = "culebra_runtime_str_lower";
inline constexpr auto str_scalar_at       = "culebra_runtime_str_scalar_at";
inline constexpr auto str_size            = "culebra_runtime_str_size";
inline constexpr auto utf8_scalar_len     = "culebra_runtime_utf8_scalar_len";
inline constexpr auto str_slice           = "culebra_runtime_str_slice";
inline constexpr auto str_split           = "culebra_runtime_str_split";
inline constexpr auto strlike_view        = "culebra_runtime_strlike_view";
inline constexpr auto strlike_slice_view  = "culebra_runtime_strlike_slice_view";
inline constexpr auto strlike_to_cstr     = "culebra_runtime_strlike_to_cstr";
inline constexpr auto str_contains        = "culebra_runtime_str_contains";
inline constexpr auto str_starts_with     = "culebra_runtime_str_starts_with";
inline constexpr auto str_ends_with       = "culebra_runtime_str_ends_with";
inline constexpr auto str_trim            = "culebra_runtime_str_trim";
inline constexpr auto str_tr              = "culebra_runtime_str_tr";
inline constexpr auto str_trim_start      = "culebra_runtime_str_trim_start";
inline constexpr auto str_trim_end        = "culebra_runtime_str_trim_end";
inline constexpr auto str_upper           = "culebra_runtime_str_upper";
inline constexpr auto str_capitalize      = "culebra_runtime_str_capitalize";
inline constexpr auto str_repeat          = "culebra_runtime_str_repeat";
inline constexpr auto str_truncate        = "culebra_runtime_str_truncate";
inline constexpr auto str_lines           = "culebra_runtime_str_lines";
inline constexpr auto str_count           = "culebra_runtime_str_count";
inline constexpr auto safepoint           = "culebra_runtime_safepoint";
inline constexpr auto throw_              = "culebra_runtime_throw";
inline constexpr auto to_long             = "culebra_runtime_to_long";
inline constexpr auto to_long_any         = "culebra_runtime_to_long_any";
inline constexpr auto to_float_any        = "culebra_runtime_to_float_any";
inline constexpr auto type_check          = "culebra_runtime_type_check";
inline constexpr auto type_matches        = "culebra_runtime_type_matches";
inline constexpr auto register_trait_default
    = "culebra_runtime_register_trait_default";
inline constexpr auto register_trait_method
    = "culebra_runtime_register_trait_method";
inline constexpr auto register_trait_super
    = "culebra_runtime_register_trait_super";
inline constexpr auto type_error          = "culebra_runtime_type_error";
inline constexpr auto destructure_mismatch
    = "culebra_runtime_destructure_mismatch";
inline constexpr auto compound_missing_property
    = "culebra_runtime_compound_missing_property";
inline constexpr auto immutable_assign
    = "culebra_runtime_immutable_assign";
inline constexpr auto module_register
    = "culebra_runtime_module_register";
inline constexpr auto module_get
    = "culebra_runtime_module_get";
inline constexpr auto namespace_get
    = "culebra_runtime_namespace_get";
inline constexpr auto lazy_ns_register
    = "culebra_runtime_lazy_ns_register";
inline constexpr auto unknown_kwarg
    = "culebra_runtime_unknown_kwarg";
inline constexpr auto missing_required_arg
    = "culebra_runtime_missing_required_arg";
inline constexpr auto throw_error
    = "culebra_runtime_throw_error";
inline constexpr auto type_error_typed    = "culebra_runtime_type_error_typed";
inline constexpr auto class_parameters_walk =
    "culebra_runtime_class_parameters_walk";
inline constexpr auto multifn_register_and_install =
    "culebra_runtime_multifn_register_and_install";
inline constexpr auto arity_error         = "culebra_runtime_arity_error";
inline constexpr auto arity_missing       = "culebra_runtime_arity_missing";
inline constexpr auto args_slice_to_array =
    "culebra_runtime_args_slice_to_array";
inline constexpr auto release_overflow_args =
    "culebra_runtime_release_overflow_args";
inline constexpr auto type_of             = "culebra_runtime_type_of";
inline constexpr auto value_equal         = "culebra_runtime_value_equal";
inline constexpr auto value_less          = "culebra_runtime_value_less";
inline constexpr auto value_leq           = "culebra_runtime_value_leq";
inline constexpr auto value_greater       = "culebra_runtime_value_greater";
inline constexpr auto value_geq           = "culebra_runtime_value_geq";
inline constexpr auto value_release       = "culebra_runtime_value_release";
inline constexpr auto value_retain        = "culebra_runtime_value_retain";
inline constexpr auto value_swap_owned    = "culebra_runtime_value_swap_owned";
inline constexpr auto value_to_display    = "culebra_runtime_value_to_display";
inline constexpr auto format_value        = "culebra_runtime_format_value";
inline constexpr auto write_file          = "culebra_runtime_write_file";
}  // namespace rt

#ifdef _WIN32
// RTDyld memory manager that registers Win64 unwind tables for JIT'd code, so a
// throw can unwind *through* a JIT frame in-process (reaching its defers and an
// outer catch). Without this the seh0 personality is emitted correctly but the
// OS SEH unwinder never learns the JIT'd frames exist: LLVM's RTDyld collects
// each object's `.pdata` (an array of RUNTIME_FUNCTION) and calls
// registerEHFrames(), but its in-process body is a Win64 no-op (llvm#24607). We
// override it to call RtlAddFunctionTable against the object's image base — the
// lowest section address, which all the `.pdata`/`.xdata` RVAs are relative to.
// This is the mingw/Itanium half of the old clang-interpreter Win64 example;
// culebra throws via __cxa_throw + __gxx_personality_seh0 (not MSVC
// _CxxThrowException), so none of that example's throw-record image-base fixup
// is needed.
//
// Reachability: the `.xdata` handler field and RtlAddFunctionTable's base are
// both 32-bit image-relative, so the whole thing only works when the JIT slab
// lands within 4 GB of the runtime's __gxx_personality_seh0. By default
// VirtualAlloc puts the slab in the low address space, gigabytes below the
// ASLR'd main image — the personality RVA can't reach it and the OS unwinder
// jumps into the slab instead. NearImageMapper (below) forces every page just
// under the main image so the RVA fits.
//
// Maps LLVM's page requests (SectionMemoryManager's default source) into a
// 2 GB window immediately below the main module, via VirtualAlloc2 with an
// address requirement. That keeps personality - slab_base positive and well
// under 4 GB, so RTDyld writes a correct ADDR32NB RVA and the OS computes the
// handler as slab_base + RVA == the real personality. Resolved dynamically so a
// mingw import lib without VirtualAlloc2, or a failed reservation, falls back to
// the default allocator (JIT still runs; that object just won't unwind).
class NearImageMapper : public llvm::SectionMemoryManager::MemoryMapper {
  using Purpose = llvm::SectionMemoryManager::AllocationPurpose;
  using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG,
                                         MEM_EXTENDED_PARAMETER*, ULONG);

 public:
  llvm::sys::MemoryBlock allocateMappedMemory(
      Purpose /*P*/, size_t NumBytes,
      const llvm::sys::MemoryBlock* const NearBlock, unsigned Flags,
      std::error_code& EC) override {
    static VirtualAlloc2Fn va2 = reinterpret_cast<VirtualAlloc2Fn>(
        ::GetProcAddress(::GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
    if (va2) {
      if (!low_) compute_window();
      const size_t gran = 0x10000;  // Windows allocation granularity
      size_t sz = (NumBytes + gran - 1) & ~(gran - 1);
      MEM_ADDRESS_REQUIREMENTS req{};
      req.LowestStartingAddress = reinterpret_cast<PVOID>(low_);
      req.HighestEndingAddress = reinterpret_cast<PVOID>(high_);
      req.Alignment = 0;
      MEM_EXTENDED_PARAMETER ep{};
      ep.Type = MemExtendedParameterAddressRequirements;
      ep.Pointer = &req;
      void* p = va2(nullptr, nullptr, sz, MEM_RESERVE | MEM_COMMIT,
                    to_win_prot(Flags), &ep, 1);
      if (p) {
        EC = std::error_code();
        return llvm::sys::MemoryBlock(p, sz);
      }
    }
    // No VirtualAlloc2, or the window is full: fall back to the OS default.
    return llvm::sys::Memory::allocateMappedMemory(NumBytes, NearBlock, Flags, EC);
  }
  std::error_code protectMappedMemory(const llvm::sys::MemoryBlock& Block,
                                      unsigned Flags) override {
    return llvm::sys::Memory::protectMappedMemory(Block, Flags);
  }
  std::error_code releaseMappedMemory(llvm::sys::MemoryBlock& M) override {
    return llvm::sys::Memory::releaseMappedMemory(M);
  }

 private:
  void compute_window() {
    auto base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    // Reserve strictly below the image base so every personality RVA is
    // positive; a 2 GB window keeps it comfortably under the 4 GB ADDR32NB cap.
    high_ = base - 1;  // module base is 64K-aligned, so this ends in 0xFFFF
    uintptr_t span = 0x80000000ull;
    low_ = base > span ? base - span : 0x10000;
  }
  static DWORD to_win_prot(unsigned f) {
    using M = llvm::sys::Memory;
    bool w = f & M::MF_WRITE, x = f & M::MF_EXEC, r = f & M::MF_READ;
    if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (w) return PAGE_READWRITE;
    if (r) return PAGE_READONLY;
    return PAGE_NOACCESS;
  }
  uintptr_t low_ = 0, high_ = 0;
};

inline NearImageMapper& near_image_mapper() {
  static NearImageMapper m;
  return m;
}

class WinSEHMemoryManager : public llvm::SectionMemoryManager {
 public:
  WinSEHMemoryManager() : llvm::SectionMemoryManager(&near_image_mapper()) {}

  uint8_t* allocateCodeSection(uintptr_t Size, unsigned Align, unsigned ID,
                               llvm::StringRef Name) override {
    uint8_t* p = SectionMemoryManager::allocateCodeSection(Size, Align, ID, Name);
    note_base(p);
    return p;
  }
  uint8_t* allocateDataSection(uintptr_t Size, unsigned Align, unsigned ID,
                               llvm::StringRef Name, bool RO) override {
    uint8_t* p =
        SectionMemoryManager::allocateDataSection(Size, Align, ID, Name, RO);
    note_base(p);
    return p;
  }
  // Addr points at the loaded `.pdata` section = a RUNTIME_FUNCTION array. We do
  // not chain to the base implementation: its in-process path feeds the block to
  // the DWARF __register_frame machinery, which would misread these COFF unwind
  // records. We keep our own list for teardown instead.
  void registerEHFrames(uint8_t* Addr, uint64_t /*LoadAddr*/,
                        size_t Size) override {
    auto* fns = reinterpret_cast<PRUNTIME_FUNCTION>(Addr);
    DWORD n = static_cast<DWORD>(Size / sizeof(RUNTIME_FUNCTION));
    if (n && image_base_ && ::RtlAddFunctionTable(fns, n, image_base_)) {
      registered_.push_back(fns);
      fixup_personality(fns, n);
    }
  }
  void deregisterEHFrames() override {
    for (auto* fns : registered_) ::RtlDeleteFunctionTable(fns);
    registered_.clear();
  }

 private:
  // RTDyld loads .xdata (with setProcessAllSections) but does not relocate the
  // exception-handler RVA inside each UNWIND_INFO — it keeps the object-file
  // placeholder, so the OS unwinder dispatches into the middle of .xdata and
  // crashes. Point every EH/UH handler at the real __gxx_personality_seh0. The
  // near-image mapper guarantees personality - image_base fits in the 32-bit
  // RVA. See the Win64 UNWIND_INFO layout: byte0 = Version:3|Flags:5, byte2 =
  // CountOfCodes, then that many 2-byte codes (padded to an even count),
  // immediately followed by the handler RVA when UNW_FLAG_[EU]HANDLER is set.
  void fixup_personality(PRUNTIME_FUNCTION fns, DWORD n) {
    const uintptr_t base = image_base_;
    const uint32_t rva = static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(&__gxx_personality_seh0) - base);
    for (DWORD i = 0; i < n; ++i) {
      uint32_t unwind = reinterpret_cast<const uint32_t*>(&fns[i])[2];
      auto* ui = reinterpret_cast<uint8_t*>(base + unwind);
      if (!((ui[0] >> 3) & 0x3)) continue;  // no EH/UH handler on this frame
      uint8_t codes = ui[2];
      auto* handler =
          reinterpret_cast<uint32_t*>(ui + 4 + ((codes + 1) & ~1) * 2);
      DWORD old;
      if (::VirtualProtect(handler, sizeof(uint32_t), PAGE_READWRITE, &old)) {
        *handler = rva;
        DWORD tmp;
        ::VirtualProtect(handler, sizeof(uint32_t), old, &tmp);
      }
    }
  }
  void note_base(uint8_t* p) {
    if (!p) return;
    auto a = reinterpret_cast<uintptr_t>(p);
    if (!image_base_ || a < image_base_) image_base_ = a;
  }
  uintptr_t image_base_ = 0;
  std::vector<PRUNTIME_FUNCTION> registered_;
};
#endif  // _WIN32

}  // namespace culebra

#endif  // CULEBRA_JIT_ENABLED
