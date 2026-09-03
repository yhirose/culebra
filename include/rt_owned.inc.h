#pragma once

// Deterministic drop: the owned-resource stack (design section 14.3).
//
// Runtime-layer fragment of rt.h, split out for readability. These
// fragments rely on rt.h's #include block and are included by rt.h in a
// fixed sequence (see rt.h); they are not standalone headers.

// --- Deterministic drop: the owned-resource stack ---
//
// The owned stack: deterministic scope-exit drop. Drop-having
// objects are registered the moment `drop` is bound (instance
// construction / property write); codegen emits a scope-exit call —
// after the scope's slot release — that resolves the entries above the
// scope's mark: tombstoned or already-dropped entries are discarded,
// the rest are escaped or cyclic and a localized trial deletion
// decides (see culebra_runtime_owned_scope_exit). Entries are
// non-owning raw pointers; the ordinary refcount-0 release stays the
// primary drop path and tombstones its entry through `owned_idx`.
// Marks are monotonic registration ids, so pruning never invalidates a
// live scope's mark.
struct JitOwnedStack {
  struct Entry {
    JitObject* obj;  // null = tombstone (object died via release/sweep)
    uint64_t id;
  };
  // The two hot fields sit first at fixed offsets: compiled code reads
  // them directly through the pointer culebra_runtime_owned_hot()
  // returns, so scope entry (load next_id) and the empty-region exit
  // check (top_stamp <= mark) cost no runtime call. See
  // emit_owned_scope_exit in the JIT class.
  uint64_t next_id = 0;
  uint64_t top_stamp = 0;  // entries.back().id + 1, or 0 when empty
  std::vector<Entry> entries;

  void refresh_top() {
    top_stamp = entries.empty() ? 0 : entries.back().id + 1;
  }

  // Tombstones accumulate in regions that never exit (the top level
  // inherits every survivor); prune periodically. Re-indexing keeps
  // every live object's owned_idx back-pointer accurate.
  void maybe_prune() {
    if ((next_id & 1023) != 0) return;
    std::erase_if(entries, [](const Entry& e) { return e.obj == nullptr; });
    for (size_t i = 0; i < entries.size(); i++)
      entries[i].obj->owned_idx = static_cast<int64_t>(i);
    refresh_top();
  }
};
static_assert(offsetof(JitOwnedStack, next_id) == 0 &&
                  offsetof(JitOwnedStack, top_stamp) == 8,
              "compiled code GEPs these two fields directly");

inline JitOwnedStack& _jit_owned_stack() {
  return culebra::runtime_substate<JitOwnedStack>(
      culebra::kSlotJitOwnedStack);
}

inline void _jit_owned_register(JitObject* o) {
  if (!o || o->owned_idx >= 0) return;
  auto& st = _jit_owned_stack();
  o->owned_idx = static_cast<int64_t>(st.entries.size());
  st.entries.push_back({o, st.next_id++});
  st.refresh_top();
  st.maybe_prune();
}

// Stable per-Runtime address of the owned stack's hot fields (the
// substate object is heap-allocated once and never moves). Compiled
// functions fetch it once in their prologue.
extern "C" CULEBRA_RT_KEEP CULEBRA_RT_INLINE int64_t
culebra_runtime_owned_hot() {
  return reinterpret_cast<int64_t>(&_jit_owned_stack());
}

// Tombstone (release/sweep death, explicit de-registration). Safe on
// any object: a never-registered one has owned_idx == -1.
inline void _jit_owned_unregister(JitObject* o) {
  if (!o || o->owned_idx < 0) return;
  auto& st = _jit_owned_stack();
  if (static_cast<size_t>(o->owned_idx) < st.entries.size())
    st.entries[o->owned_idx].obj = nullptr;
  o->owned_idx = -1;
}

// Bind/unbind-`drop` chokepoints: every path that makes an object
// drop-having (or stops it being one) must come through these, so the
// invariant "drop-having ⇔ registered" can't drift as entry points are
// added.
inline void _jit_owned_bind_drop(JitObject* o) {
  o->has_drop = true;
  _jit_owned_register(o);
}
inline void _jit_owned_unbind_drop(JitObject* o) {
  o->has_drop = false;
  _jit_owned_unregister(o);
}

// Register a `new`d object with the collector. The object owns its own memory
// (refcount + RC release-to-zero `delete`); registration only records it in
// the heap's address→metadata registry (metadata external, so offset 0 stays
// `refcount`). `_gc_note_free` is the de-registration the release path runs
// just before `delete`.
template <class T>
inline void _gc_register(T* obj, int8_t tag) {
  _gc_heap().adopt(obj, sizeof(T), tag);
}
// De-register `obj` from the registry just before the release path `delete`s
// it (the conservative heap owns no memory — RC's `delete` frees it).
// Doubles as the owned-stack tombstone chokepoint for objects.
inline void _gc_note_free(void* obj, int8_t tag) {
  if (tag == GC_TAG_OBJECT)
    _jit_owned_unregister(static_cast<JitObject*>(obj));
  _gc_heap().forget(obj);
}

